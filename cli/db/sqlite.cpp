/**************************************************************************/
/*  sqlite.cpp                                                            */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement embedded SQL connections, binding, row conversion, and resource settings declared in sqlite.h.

#include "cli/db/sqlite.h"
#include "cli/sys/clock.h"
#include "cli/data/utf8.h"
#include "cli/db/rows.h"

#include "cli/sys/mount.h"
#include "cli/sys/source_error.h"
#include "cli/sys/perm.h"
#include "thirdparty/sqlite/sqlite3.h"

#ifdef UNIX_ENABLED
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/object/class_db.h"
#include "core/variant/typed_array.h"

#include <cstring>

namespace {

constexpr int ROW_DEFAULT = 0; // Apply row limits only when explicitly configured.
constexpr int BYTES_DEFAULT = 0; // Apply byte limits only when explicitly configured.
constexpr int BUSY_DEFAULT = 5000; // Default lock-wait timeout in milliseconds.
constexpr int TIME_DEFAULT = 0; // Allow embedded SQL to finish unless a deadline is explicitly configured.
constexpr int STMT_MAX = 128; // Prepared statements reused per connection.
constexpr int64_t STMT_BYTES_MAX = 4LL * 1024 * 1024; // Maximum prepared-statement cache bytes per connection.
constexpr int SQLITE_COLUMN_MAX = 2000; // Default maximum columns in one embedded SQL result.
constexpr int SQLITE_ATTACHED_MAX = 0; // Prevent attaching databases outside the mount.
constexpr int SQLITE_WORKER_MAX = 0; // Keep thread creation under standard-module management.
constexpr int SQLITE_CACHE_KIB = 4096; // Approximate page-cache KiB per connection.

#ifdef UNIX_ENABLED
constexpr const char *PINNED_VFS_NAME = "gd-pinned"; // VFS name for pinned descriptor paths.
constexpr const char *PINNED_PATH_PREFIX = "/gd-sqlite/"; // Virtual path prefix identifying pinned main databases.
sqlite3_vfs pinned_vfs = {}; // Registered VFS overlay for pinned-name handling.
sqlite3_vfs *pinned_base = nullptr; // Default VFS performing actual file operations.
Mutex pinned_mutex; // Protect pinned files across embedded SQL callbacks and connection closure.
uint64_t pinned_next = 1; // Process-local identifier assigned to virtual paths.

// Pin a connection's parent directory and main database until its default-VFS path is no longer needed.
struct PinnedDb {
	int dir_fd = -1; // Directory descriptor used to inspect sidecars.
	int file_fd = -1; // Descriptor pinning the main database inode.
	CharString leaf; // Main database name relative to its parent.
	Vector<char> path; // Double-NUL-terminated /dev/fd path passed to the default VFS.

	~PinnedDb() {
		if (file_fd >= 0) {
			::close(file_fd);
		}
		if (dir_fd >= 0) {
			::close(dir_fd);
		}
	}
};

HashMap<uint64_t, PinnedDb *> pinned_dbs; // Map virtual-path identifiers to pinned files.

// Split a virtual path into a connection identifier and sidecar suffix.
bool pinned_parts(const char *p_path, uint64_t &r_id, const char *&r_suffix) {
	if (!p_path || strncmp(p_path, PINNED_PATH_PREFIX, strlen(PINNED_PATH_PREFIX)) != 0) {
		return false;
	}
	const char *at = p_path + strlen(PINNED_PATH_PREFIX);
	if (*at < '0' || *at > '9') {
		return false;
	}
	uint64_t id = 0;
	while (*at >= '0' && *at <= '9') {
		const uint64_t digit = uint64_t(*at - '0');
		if (id > (UINT64_MAX - digit) / 10) {
			return false;
		}
		id = id * 10 + digit;
		at++;
	}
	r_id = id;
	r_suffix = at;
	return true;
}

// Pass the main database to the default VFS without changing file operations or locking.
int pinned_open(sqlite3_vfs *, sqlite3_filename p_name, sqlite3_file *p_file, int p_flags, int *r_flags) {
	uint64_t id = 0;
	const char *suffix = nullptr;
	if (!pinned_parts(p_name, id, suffix)) {
		return pinned_base->xOpen(pinned_base, p_name, p_file, p_flags, r_flags);
	}
	p_file->pMethods = nullptr;
	MutexLock lock(pinned_mutex);
	const HashMap<uint64_t, PinnedDb *>::ConstIterator found = pinned_dbs.find(id);
	if (!found || *suffix != '\0') {
		return SQLITE_CANTOPEN;
	}
	return pinned_base->xOpen(pinned_base, found->value->path.ptr(), p_file, p_flags, r_flags);
}

// Check main-database or sidecar existence relative to the pinned parent.
int pinned_access(sqlite3_vfs *, const char *p_name, int p_flags, int *r_found) {
	uint64_t id = 0;
	const char *suffix = nullptr;
	if (!pinned_parts(p_name, id, suffix)) {
		return pinned_base->xAccess(pinned_base, p_name, p_flags, r_found);
	}
	MutexLock lock(pinned_mutex);
	const HashMap<uint64_t, PinnedDb *>::ConstIterator found = pinned_dbs.find(id);
	if (!found || (strcmp(suffix, "") != 0 && strcmp(suffix, "-journal") != 0 && strcmp(suffix, "-wal") != 0 && strcmp(suffix, "-shm") != 0)) {
		return SQLITE_CANTOPEN;
	}
	const String name = String::utf8(found->value->leaf.get_data()) + suffix;
	struct stat st = {};
	if (::fstatat(found->value->dir_fd, name.utf8().get_data(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
		*r_found = 1;
		return SQLITE_OK;
	}
	*r_found = 0;
	return errno == ENOENT ? SQLITE_OK : SQLITE_IOERR_ACCESS;
}

// Reject virtual-path deletion without introducing a parent-path deletion fallback.
int pinned_delete(sqlite3_vfs *, const char *p_name, int p_sync_dir) {
	uint64_t id = 0;
	const char *suffix = nullptr;
	if (!pinned_parts(p_name, id, suffix)) {
		return pinned_base->xDelete(pinned_base, p_name, p_sync_dir);
	}
	return SQLITE_IOERR_DELETE;
}

// Preserve pinned names without resolving symlinks; delegate other paths to the default VFS.
int pinned_full_path(sqlite3_vfs *, const char *p_name, int p_size, char *r_path) {
	uint64_t id = 0;
	const char *suffix = nullptr;
	if (!pinned_parts(p_name, id, suffix)) {
		return pinned_base->xFullPathname(pinned_base, p_name, p_size, r_path);
	}
	const size_t size = strlen(p_name) + 1;
	if (size > size_t(p_size)) {
		return SQLITE_CANTOPEN;
	}
	memcpy(r_path, p_name, size);
	return SQLITE_OK;
}

// Register the pinned-name overlay on the default VFS once.
const char *pinned_vfs_name() {
	static const bool ready = []() {
		pinned_base = sqlite3_vfs_find(nullptr);
		if (!pinned_base) {
			return false;
		}
		pinned_vfs = *pinned_base;
		pinned_vfs.pNext = nullptr;
		pinned_vfs.zName = PINNED_VFS_NAME;
		pinned_vfs.xOpen = pinned_open;
		pinned_vfs.xDelete = pinned_delete;
		pinned_vfs.xAccess = pinned_access;
		pinned_vfs.xFullPathname = pinned_full_path;
		return sqlite3_vfs_register(&pinned_vfs, 0) == SQLITE_OK;
	}();
	return ready ? PINNED_VFS_NAME : nullptr;
}

// Reject existing sidecars and WAL format, then register the main database under a pinned virtual path.
uint64_t hold_database(int p_dir_fd, const String &p_leaf, String &r_bad, Error &r_err) {
	SourceError::clear();
	r_err = OK;
	PinnedDb *held = memnew(PinnedDb);
	held->dir_fd = ::dup(p_dir_fd);
	held->leaf = p_leaf.utf8();
	if (held->dir_fd < 0) {
		const int code = errno;
		memdelete(held);
		SourceError::posix(code);
		r_err = FAILED;
		r_bad = "cannot hold sqlite database directory";
		return 0;
	}
	for (const char *suffix : { "-journal", "-wal", "-shm" }) {
		const String name = p_leaf + suffix;
		struct stat st = {};
		const int found = ::fstatat(held->dir_fd, name.utf8().get_data(), &st, AT_SYMLINK_NOFOLLOW);
		if (found == 0) {
			memdelete(held);
			SourceError::clear();
			r_err = ERR_INVALID_DATA;
			r_bad = "sqlite database has a sidecar; recover it before opening inside a mount";
			return 0;
		}
		if (errno != ENOENT) {
			const int code = errno;
			memdelete(held);
			SourceError::posix(code);
			r_err = FAILED;
			r_bad = "cannot inspect sqlite database sidecars";
			return 0;
		}
	}
	held->file_fd = ::openat(held->dir_fd, held->leaf.get_data(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (held->file_fd < 0) {
		const int code = errno;
		memdelete(held);
		SourceError::posix(code);
		r_err = FAILED;
		r_bad = "cannot hold sqlite database file";
		return 0;
	}
	uint8_t header[20] = {};
	const ssize_t header_size = ::pread(held->file_fd, header, sizeof(header), 0);
	if (header_size < 0) {
		const int code = errno;
		memdelete(held);
		SourceError::posix(code);
		r_err = FAILED;
		r_bad = "cannot inspect sqlite database header";
		return 0;
	}
	if (header_size == sizeof(header) && (header[18] == 2 || header[19] == 2)) {
		memdelete(held);
		SourceError::clear();
		r_err = ERR_INVALID_DATA;
		r_bad = "sqlite WAL databases must be recovered before opening inside a mount";
		return 0;
	}
	// Select a descriptor path that supports O_NOFOLLOW in the current environment.
	CharString fd_path;
	int reopen_code = 0;
	for (const String &candidate : { vformat("/proc/self/fd/%d/", held->dir_fd) + p_leaf, vformat("/dev/fd/%d", held->file_fd) }) {
		const CharString raw = candidate.utf8();
		const int probe = ::open(raw.get_data(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		if (probe >= 0) {
			::close(probe);
			fd_path = raw;
			break;
		}
		if (reopen_code == 0) {
			reopen_code = errno;
		}
	}
	if (fd_path.length() == 0) {
		memdelete(held);
		SourceError::posix(reopen_code);
		r_err = FAILED;
		r_bad = "this Unix environment cannot reopen a held sqlite database";
		return 0;
	}
	held->path.resize(fd_path.length() + 2);
	memcpy(held->path.ptrw(), fd_path.get_data(), fd_path.length() + 1);
	held->path.write[fd_path.length() + 1] = '\0';
	MutexLock lock(pinned_mutex);
	const uint64_t id = pinned_next++;
	pinned_dbs.insert(id, held);
	return id;
}

// Release the pinned main database and parent after connection closure.
void release_database(uint64_t p_id) {
	if (p_id == 0) {
		return;
	}
	MutexLock lock(pinned_mutex);
	const HashMap<uint64_t, PinnedDb *>::Iterator found = pinned_dbs.find(p_id);
	if (found) {
		memdelete(found->value);
		pinned_dbs.erase(p_id);
	}
}
#endif

// Release or reset prepared statements on every exit path.
class Stmt {
	sqlite3_stmt *stmt = nullptr; // Statement requiring cleanup.
	HashMap<String, SQLiteCachedStmt> *cache = nullptr; // Owning cache when the statement will be reused.
	int64_t *cache_bytes = nullptr; // Estimated bytes retained by the entire cache.
	String key; // SQL key used to remove a damaged cached statement.

public:
	~Stmt() {
		if (!stmt) {
			return;
		}
		if (cache) {
			const int code = sqlite3_reset(stmt);
			sqlite3_clear_bindings(stmt);
			const HashMap<String, SQLiteCachedStmt>::Iterator found = cache->find(key);
			if (found) {
				const int64_t now = int64_t(key.length()) * sizeof(char32_t) + sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_MEMUSED, 0);
				*cache_bytes += now - found->value.bytes;
				found->value.bytes = now;
			}
			if (code == SQLITE_OK && *cache_bytes <= STMT_BYTES_MAX) {
				return;
			}
			if (found) {
				*cache_bytes -= found->value.bytes;
			}
			cache->erase(key);
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_stmt **out() { return &stmt; }
	sqlite3_stmt *get() const { return stmt; }
	// Transfer ownership to the caller.
	sqlite3_stmt *release() {
		sqlite3_stmt *out = stmt;
		stmt = nullptr;
		return out;
	}
	// Borrow a cache-owned statement for this execution.
	void reuse(sqlite3_stmt *p_stmt, HashMap<String, SQLiteCachedStmt> &p_cache, int64_t &p_cache_bytes, const String &p_key) {
		stmt = p_stmt;
		cache = &p_cache;
		cache_bytes = &p_cache_bytes;
		key = p_key;
	}
};

// Reset an explicit prepared statement for its next execution.
class ResetStmt {
	sqlite3_stmt *stmt = nullptr; // Execution program to reset.

public:
	ResetStmt(sqlite3_stmt *p_stmt) : stmt(p_stmt) {}
	~ResetStmt() {
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}
};

// Map embedded SQL return codes to public error categories.
Err::Kind kind_of(int p_code) {
	if (p_code == SQLITE_CONSTRAINT_UNIQUE || p_code == SQLITE_CONSTRAINT_PRIMARYKEY) {
		return Err::ALREADY_EXISTS;
	}
	switch (p_code & 0xff) {
		case SQLITE_AUTH:
		case SQLITE_PERM:
		case SQLITE_READONLY:
			return Err::PERMISSION_DENIED;
		case SQLITE_BUSY:
		case SQLITE_LOCKED:
			return Err::TIMED_OUT;
		case SQLITE_CANTOPEN:
		case SQLITE_NOTFOUND:
			return Err::NONE; // Preserve the embedded SQL code because this does not necessarily mean a missing file.
		case SQLITE_FULL:
		case SQLITE_NOMEM:
		case SQLITE_TOOBIG:
			return Err::LIMITED;
		case SQLITE_INTERRUPT:
			return Err::TIMED_OUT;
		default:
			return Err::INVALID_DATA;
	}
}

// Extract table and column names from embedded SQL constraint messages.
PackedStringArray sqlite_columns(const String &p_msg, const String &p_prefix, String &r_table) {
	PackedStringArray out;
	if (!p_msg.begins_with(p_prefix)) {
		return out;
	}
	const PackedStringArray names = p_msg.substr(p_prefix.length()).split(", ", false);
	for (const String &name : names) {
		const int dot = name.rfind(".");
		if (dot <= 0 || dot + 1 >= name.length()) {
			return PackedStringArray();
		}
		const String table = name.substr(0, dot);
		if (!r_table.is_empty() && r_table != table) {
			return PackedStringArray();
		}
		r_table = table;
		out.push_back(name.substr(dot + 1));
	}
	return out;
}

// Convert embedded SQL extended codes and messages into shared constraint details.
Dictionary sqlite_info(int p_code, const String &p_msg) {
	Dictionary info;
	info["source"] = "sqlite";
	info["source_code"] = p_code;
	String violation;
	String code;
	String prefix;
	if (p_code == SQLITE_CONSTRAINT_UNIQUE || p_code == SQLITE_CONSTRAINT_PRIMARYKEY) {
		violation = "duplicate";
		code = p_code == SQLITE_CONSTRAINT_UNIQUE ? "SQLITE_CONSTRAINT_UNIQUE" : "SQLITE_CONSTRAINT_PRIMARYKEY";
		prefix = "UNIQUE constraint failed: ";
	} else if (p_code == SQLITE_CONSTRAINT_NOTNULL) {
		violation = "not_null";
		code = "SQLITE_CONSTRAINT_NOTNULL";
		prefix = "NOT NULL constraint failed: ";
	} else if (p_code == SQLITE_CONSTRAINT_FOREIGNKEY) {
		violation = "foreign_key";
		code = "SQLITE_CONSTRAINT_FOREIGNKEY";
	}
	if (violation.is_empty()) {
		return info;
	}
	info["code"] = code;
	info["violation"] = violation;
	String table;
	const PackedStringArray columns = sqlite_columns(p_msg, prefix, table);
	info["columns"] = columns;
	if (!table.is_empty()) {
		info["table"] = table;
	}
	return info;
}

// Wrap the latest embedded SQL error in R.
Ref<R> db_error(sqlite3 *p_db, int p_code, const String &p_prefix = String()) {
	if (p_db && sqlite3_errcode(p_db) != SQLITE_OK) {
		p_code = sqlite3_extended_errcode(p_db);
	}
	const char *raw = p_db ? sqlite3_errmsg(p_db) : sqlite3_errstr(p_code);
	const String msg = raw ? String::utf8(raw) : String("sqlite error");
	return R::err(Err::make(p_prefix.is_empty() ? msg : p_prefix + ": " + msg, kind_of(p_code), sqlite_info(p_code, msg)));
}

// Reject SQL that expands resource access to other files or the entire process.
int authorize(void *, int p_action, const char *p_name, const char *p_value, const char *, const char *) {
	if (p_action == SQLITE_ATTACH || p_action == SQLITE_DETACH) {
		return SQLITE_DENY;
	}
	if (p_action != SQLITE_PRAGMA || !p_value || !p_name) {
		return SQLITE_OK; // Allow read-only PRAGMAs and ordinary SQL.
	}
	static const char *fixed[] = { // Connection settings scripts cannot override.
		"busy_timeout", "cache_size", "data_store_directory", "hard_heap_limit",
		"journal_mode", "mmap_size", "soft_heap_limit", "temp_store", "temp_store_directory",
		"threads", "trusted_schema", "writable_schema", nullptr
	};
	for (int i = 0; fixed[i]; i++) {
		if (sqlite3_stricmp(p_name, fixed[i]) == 0) {
			return SQLITE_DENY;
		}
	}
	return SQLITE_OK;
}

// Bind a Variant safely to an embedded SQL positional parameter.
int bind_one(sqlite3_stmt *p_stmt, int p_at, const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::NIL:
			return sqlite3_bind_null(p_stmt, p_at);
		case Variant::BOOL:
		case Variant::INT:
			return sqlite3_bind_int64(p_stmt, p_at, int64_t(p_value));
		case Variant::FLOAT:
			return sqlite3_bind_double(p_stmt, p_at, double(p_value));
		case Variant::STRING: {
			if (utf8_bytes(String(p_value)) > sqlite3_limit(sqlite3_db_handle(p_stmt), SQLITE_LIMIT_LENGTH, -1)) {
				return SQLITE_TOOBIG;
			}
			const CharString text = String(p_value).utf8();
			return sqlite3_bind_text64(p_stmt, p_at, text.get_data(), text.length(), SQLITE_TRANSIENT, SQLITE_UTF8);
		}
		case Variant::PACKED_BYTE_ARRAY: {
			const PackedByteArray bytes = p_value;
			static const uint8_t empty = 0; // Non-null address distinguishing an empty BLOB from NULL.
			return sqlite3_bind_blob64(p_stmt, p_at, bytes.is_empty() ? &empty : bytes.ptr(), bytes.size(), SQLITE_TRANSIENT);
		}
		default:
			return SQLITE_MISMATCH;
	}
}

// Conservatively estimate bytes occupied by bind values.
int64_t value_bytes(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
			return 8;
		case Variant::STRING:
			return int64_t(String(p_value).length()) * 4;
		case Variant::PACKED_BYTE_ARRAY:
			return PackedByteArray(p_value).size();
		default:
			return -1;
	}
}

// Bind validated argument sets to one program and execute updates sequentially.
Ref<R> execute_many(sqlite3 *p_db, sqlite3_stmt *p_stmt, const Array &p_rows, uint64_t p_due, int64_t &r_changes) {
	r_changes = 0;
	for (int row_at = 0; row_at < p_rows.size(); row_at++) {
		const Array params = p_rows[row_at];
		for (int i = 0; i < params.size(); i++) {
			const int code = bind_one(p_stmt, i + 1, params[i]);
			if (code != SQLITE_OK) {
				return db_error(p_db, code, "cannot bind sqlite batch parameter");
			}
		}
		int code = SQLITE_OK;
		while ((code = sqlite3_step(p_stmt)) == SQLITE_ROW) {
		}
		if (code != SQLITE_DONE) {
			return db_error(p_db, code, "cannot execute sqlite batch");
		}
		r_changes += sqlite3_changes64(p_db);
		if (p_due > 0 && (row_at & 255) == 255 && GDClock::msec() > p_due) {
			return R::err("sqlite batch execution timed out", Err::TIMED_OUT);
		}
		if (row_at + 1 < p_rows.size()) {
			const int reset_code = sqlite3_reset(p_stmt);
			if (reset_code != SQLITE_OK) {
				return db_error(p_db, reset_code, "cannot reset sqlite batch");
			}
		}
	}
	return Ref<R>();
}

// Convert one embedded SQL column into the corresponding Variant.
Variant column_of(sqlite3_stmt *p_stmt, int p_at) {
	switch (sqlite3_column_type(p_stmt, p_at)) {
		case SQLITE_INTEGER:
			return int64_t(sqlite3_column_int64(p_stmt, p_at));
		case SQLITE_FLOAT:
			return sqlite3_column_double(p_stmt, p_at);
		case SQLITE_TEXT: {
			const char *text = reinterpret_cast<const char *>(sqlite3_column_text(p_stmt, p_at));
			return String::utf8(text, sqlite3_column_bytes(p_stmt, p_at));
		}
		case SQLITE_BLOB: {
			const int size = sqlite3_column_bytes(p_stmt, p_at);
			PackedByteArray out;
			out.resize(size);
			if (size > 0) {
				memcpy(out.ptrw(), sqlite3_column_blob(p_stmt, p_at), size);
			}
			return out;
		}
		default:
			return Variant();
	}
}

// Check for another SQL statement after the first one.
int check_tail(sqlite3 *p_db, const char *p_tail) {
	const char *at = p_tail;
	while (at && *at) {
		Stmt extra;
		const char *next = nullptr;
		const int code = sqlite3_prepare_v3(p_db, at, -1, 0, extra.out(), &next);
		if (code != SQLITE_OK) {
			return code;
		}
		if (extra.get()) {
			return SQLITE_MISUSE;
		}
		if (!next || next <= at) {
			break;
		}
		at = next;
	}
	return SQLITE_OK;
}

// Validate SQL and prepare exactly one statement.
Ref<R> prepare_stmt(sqlite3 *p_db, const String &p_sql, Stmt &r_stmt) {
	if (p_sql.is_empty() || utf8_bytes(p_sql) > sqlite3_limit(p_db, SQLITE_LIMIT_SQL_LENGTH, -1)) {
		return R::err("sqlite SQL length is outside the limit", Err::LIMITED);
	}
	const CharString sql = p_sql.utf8();
	for (int i = 0; i < sql.length(); i++) {
		if (sql[i] == 0) {
			return R::err("sqlite SQL contains a zero byte", Err::INVALID_DATA);
		}
	}
	const char *tail = nullptr;
	const int code = sqlite3_prepare_v3(p_db, sql.get_data(), sql.length(), SQLITE_PREPARE_PERSISTENT, r_stmt.out(), &tail);
	if (code != SQLITE_OK) {
		return db_error(p_db, code, "cannot prepare sqlite SQL");
	}
	if (!r_stmt.get()) {
		return R::err("sqlite SQL has no statement", Err::INVALID_DATA);
	}
	const int tail_code = check_tail(p_db, tail);
	if (tail_code != SQLITE_OK) {
		return tail_code == SQLITE_MISUSE ? R::err("sqlite accepts one statement at a time", Err::INVALID_DATA) : db_error(p_db, tail_code, "cannot parse sqlite SQL tail");
	}
	return Ref<R>();
}

} // namespace

GDSQLiteDB::~GDSQLiteDB() {
	close();
}

// Interrupt the embedded SQL VM at its deadline.
int GDSQLiteDB::stop(void *p_self) {
	const GDSQLiteDB *self = static_cast<GDSQLiteDB *>(p_self);
	return (self->stop_flag && self->stop_flag->is_set()) || (self->due > 0 && GDClock::msec() > self->due);
}

// Release all cached prepared statements.
void GDSQLiteDB::clear_stmts() {
	for (const KeyValue<String, SQLiteCachedStmt> &entry : stmts) {
		sqlite3_finalize(entry.value.stmt);
	}
	stmts.clear();
	stmt_bytes = 0;
}

// Validate mount permissions and options, then create one connection.
Ref<R> GDSQLiteDB::open(const String &p_path, const Dictionary &p_opts) {
	if (p_path.is_empty()) {
		return R::err("sqlite path is empty", Err::INVALID_DATA);
	}
	const bool memory = p_path == ":memory:";
	String path = p_path;
	if (!memory) {
		if (Mount::name_of(p_path) != "user") {
			return R::err("persistent sqlite databases must use user://", Err::PERMISSION_DENIED);
		}
		if (!Perm::check(Perm::READ, p_path) || !Perm::check(Perm::WRITE, p_path)) {
			return R::err(vformat("cannot open sqlite database %s", p_path), Err::PERMISSION_DENIED);
		}
		String why;
		path = Mount::resolve(p_path, true, why);
		if (path.is_empty()) {
			return R::err(why, Err::PERMISSION_DENIED);
		}
	}

	const int64_t busy_raw = p_opts.get("busy_ms", BUSY_DEFAULT);
	const int64_t rows_raw = p_opts.get("max_rows", ROW_DEFAULT);
	const int64_t bytes_raw = p_opts.get("max_bytes", BYTES_DEFAULT);
	const int64_t time_raw = p_opts.get("max_ms", TIME_DEFAULT);
	if (busy_raw < 0 || busy_raw > INT_MAX || rows_raw < 0 || rows_raw > INT_MAX || bytes_raw < 0 || bytes_raw > INT_MAX || time_raw < 0 || time_raw > INT_MAX) {
		return R::err("sqlite limits must be between 0 and 2147483647", Err::INVALID_DATA);
	}
	const int busy_ms = (int)busy_raw;
	const int rows = (int)rows_raw;
	const int bytes = (int)bytes_raw;
	const int time_ms = (int)time_raw;
	String open_path = path;
#ifdef UNIX_ENABLED
	uint64_t pinned = 0;
	Mount::At at;
	if (!memory) {
		String at_why;
		Error at_err = OK;
		// Preserve the mount's error category to distinguish missing parents from denied access.
		if (!Mount::at(p_path, true, at, at_why, &at_err)) {
			Dictionary info;
			info["op"] = "open";
			info["path"] = p_path;
			at_err = SourceError::put(info, at_err);
			return R::err(Err::make(at_why, Err::of(at_err), info));
		}
		// Open embedded SQL through the pinned descriptor without resolving the parent path again.
		Error hold_err = OK;
		pinned = hold_database(at.fd, at.leaf, at_why, hold_err);
		if (pinned == 0) {
			Dictionary info;
			info["op"] = "open";
			info["path"] = p_path;
			hold_err = SourceError::put(info, hold_err);
			return R::err(Err::make(at_why, Err::of(hold_err), info));
		}
		open_path = vformat("%s%d", PINNED_PATH_PREFIX, pinned);
	}
#endif
	const CharString raw = open_path.utf8();
	sqlite3 *handle = nullptr;
	int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	const char *vfs = nullptr;
#ifndef UNIX_ENABLED
	flags |= SQLITE_OPEN_NOFOLLOW;
#else
	if (!memory) {
		vfs = pinned_vfs_name();
		if (!vfs) {
			release_database(pinned);
			return R::err("cannot register sqlite file holder", Err::UNSUPPORTED);
		}
	}
#endif
	const int code = sqlite3_open_v2(raw.get_data(), &handle, flags, vfs);
	if (code != SQLITE_OK) {
		const Ref<R> err = db_error(handle, code, "cannot open sqlite database");
		if (handle) {
			sqlite3_close_v2(handle);
		}
#ifdef UNIX_ENABLED
		release_database(pinned);
#endif
		return err;
	}
	sqlite3_extended_result_codes(handle, 1);
	sqlite3_busy_timeout(handle, busy_ms);
	sqlite3_limit(handle, SQLITE_LIMIT_COLUMN, SQLITE_COLUMN_MAX);
	sqlite3_limit(handle, SQLITE_LIMIT_ATTACHED, SQLITE_ATTACHED_MAX);
	sqlite3_limit(handle, SQLITE_LIMIT_WORKER_THREADS, SQLITE_WORKER_MAX);
	sqlite3_db_config(handle, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
	sqlite3_db_config(handle, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
	// Keep updates within the pinned database and memory without opening sidecars through the original path.
	const String setup = vformat("PRAGMA journal_mode=MEMORY; PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-%d", SQLITE_CACHE_KIB);
	const int setup_code = sqlite3_exec(handle, setup.utf8().get_data(), nullptr, nullptr, nullptr);
	if (setup_code != SQLITE_OK) {
		const Ref<R> err = db_error(handle, setup_code, "cannot configure sqlite database");
		sqlite3_close_v2(handle);
#ifdef UNIX_ENABLED
		release_database(pinned);
#endif
		return err;
	}
	sqlite3_set_authorizer(handle, authorize, nullptr);

	Ref<GDSQLiteDB> out;
	out.instantiate();
	out->db = handle;
	out->max_rows = rows;
	out->max_bytes = bytes;
	out->max_ms = time_ms;
#ifdef UNIX_ENABLED
	out->pinned_id = pinned;
#endif
	sqlite3_progress_handler(handle, 10000, GDSQLiteDB::stop, out.ptr());
	return R::ok(out);
}

// Prepare SQL and select binding and result shape for the API entry point.
Ref<R> GDSQLiteDB::run(const String &p_sql, const Array &p_params, RunMode p_mode, const SafeFlag *p_stop) {
	MutexLock lock(mutex);
	// Expose cancellation only during this call so the next operation does not inherit it.
	struct StopScope {
		const SafeFlag *&flag;
		~StopScope() { flag = nullptr; }
	};
	stop_flag = p_stop;
	StopScope stop_scope{ stop_flag };
	if (!db) {
		return R::err("sqlite database is closed", Err::INVALID_DATA);
	}
	if (p_sql.is_empty() || utf8_bytes(p_sql) > sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, -1)) {
		return R::err("sqlite SQL length is outside the limit", Err::LIMITED);
	}
	const CharString sql = p_sql.utf8();
	if (p_params.size() > sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1)) {
		return R::err("sqlite parameter count exceeds the limit", Err::LIMITED);
	}
	due = max_ms > 0 ? GDClock::msec() + max_ms : 0;

	Stmt stmt;
	const HashMap<String, SQLiteCachedStmt>::Iterator found = stmts.find(p_sql);
	if (found) {
		stmt.reuse(found->value.stmt, stmts, stmt_bytes, p_sql);
	} else {
		const Ref<R> failed = prepare_stmt(db, p_sql, stmt);
		if (failed.is_valid()) {
			return failed;
		}
		const int64_t bytes = int64_t(p_sql.length()) * sizeof(char32_t) + sqlite3_stmt_status(stmt.get(), SQLITE_STMTSTATUS_MEMUSED, 0);
		if (stmts.size() >= STMT_MAX || stmt_bytes + bytes > STMT_BYTES_MAX) {
			clear_stmts();
		}
		if (bytes <= STMT_BYTES_MAX) {
			stmts.insert(p_sql, { stmt.get(), bytes });
			stmt_bytes += bytes;
			stmt.reuse(stmt.get(), stmts, stmt_bytes, p_sql);
		}
	}
	return execute(stmt.get(), p_params, p_mode);
}

// Bind and execute a prepared program.
Ref<R> GDSQLiteDB::execute(sqlite3_stmt *p_stmt, const Array &p_params, RunMode p_mode) {
	if (p_params.size() > sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1)) {
		return R::err("sqlite parameter count exceeds the limit", Err::LIMITED);
	}
	if (sqlite3_bind_parameter_count(p_stmt) != p_params.size()) {
		return R::err("sqlite parameter count does not match", Err::INVALID_DATA);
	}
	int code = SQLITE_OK;
	for (int i = 0; i < p_params.size(); i++) {
		int at = i + 1;
		if (p_mode == RUN_PORTABLE || p_mode == RUN_PORTABLE_ONE) {
			const CharString name = (String("$") + String::num_int64(i + 1)).utf8();
			at = sqlite3_bind_parameter_index(p_stmt, name.get_data());
			if (at == 0) {
				return R::err(vformat("portable SQL parameter $%d is missing", i + 1), Err::INVALID_DATA);
			}
		}
		code = bind_one(p_stmt, at, p_params[i]);
		if (code == SQLITE_MISMATCH) {
			return R::err(vformat("sqlite parameter %d has an unsupported type", i + 1), Err::INVALID_DATA);
		}
		if (code != SQLITE_OK) {
			return db_error(db, code, "cannot bind sqlite parameter");
		}
	}
	if ((p_mode == RUN_QUERY || p_mode == RUN_GET) && !sqlite3_stmt_readonly(p_stmt)) {
		return R::err("sqlite query accepts read-only SQL", Err::PERMISSION_DENIED);
	}

	Array rows;
	PackedStringArray columns;
	if (p_mode == RUN_PORTABLE) {
		const int count = sqlite3_column_count(p_stmt);
		for (int i = 0; i < count; i++) {
			columns.push_back(String::utf8(sqlite3_column_name(p_stmt, i)));
		}
	}
	int64_t result_bytes = 0;
	while ((code = sqlite3_step(p_stmt)) == SQLITE_ROW) {
		if (p_mode == RUN_EXEC) {
			continue;
		}
		if (max_rows > 0 && rows.size() >= max_rows) {
			return R::err("sqlite result row count exceeds the limit", Err::LIMITED);
		}
		Dictionary row;
		const int count = sqlite3_column_count(p_stmt);
		for (int i = 0; i < count; i++) {
			const int type = sqlite3_column_type(p_stmt, i);
			const int64_t cell_bytes = ((type == SQLITE_TEXT || type == SQLITE_BLOB) ? sqlite3_column_bytes(p_stmt, i) : 8) + strlen(sqlite3_column_name(p_stmt, i)) + 16;
			result_bytes += cell_bytes;
			if (max_bytes > 0 && result_bytes > max_bytes) {
				return R::err("sqlite result byte count exceeds the limit", Err::LIMITED);
			}
			row[String::utf8(sqlite3_column_name(p_stmt, i))] = column_of(p_stmt, i);
		}
		if (p_mode == RUN_GET || p_mode == RUN_PORTABLE_ONE) {
			// Finish the statement before reporting a row whose write may still fail to commit.
			const int ended = sqlite3_reset(p_stmt);
			return ended == SQLITE_OK ? R::ok(row) : db_error(db, ended, "cannot finish sqlite SQL");
		}
		rows.push_back(row);
	}
	if (code != SQLITE_DONE) {
		return db_error(db, code, "cannot execute sqlite SQL");
	}
	if (p_mode == RUN_QUERY) {
		return R::ok(rows);
	}
	if (p_mode == RUN_GET || p_mode == RUN_PORTABLE_ONE) {
		return R::err("database query returned no rows", Err::NOT_FOUND);
	}
	if (p_mode == RUN_PORTABLE) {
		Dictionary out;
		out["columns"] = columns;
		out["rows"] = rows;
		out["tag"] = sqlite3_stmt_readonly(p_stmt) ? vformat("SELECT %d", rows.size()) : vformat("CHANGE %d", sqlite3_changes64(db));
		return R::ok(out);
	}
	Dictionary out;
	out["changes"] = int64_t(sqlite3_changes64(db));
	out["last_id"] = int64_t(sqlite3_last_insert_rowid(db));
	return R::ok(out);
}

// Prepare a shared Rows statement owned by the embedded SQL worker.
Ref<R> GDSQLiteDB::open_rows(GDDatabaseRows *p_rows, const String &p_sql, const Array &p_params, const SafeFlag *p_stop) {
	MutexLock lock(mutex);
	struct StopScope {
		const SafeFlag *&flag;
		~StopScope() { flag = nullptr; }
	};
	stop_flag = p_stop;
	StopScope stop_scope{ stop_flag };
	if (!db || !p_rows) {
		return R::err("sqlite database is closed", Err::INTERRUPTED);
	}
	if (p_params.size() > sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1)) {
		return R::err("sqlite parameter count exceeds the limit", Err::LIMITED);
	}
	due = max_ms > 0 ? GDClock::msec() + max_ms : 0;
	Stmt prepared_stmt;
	const Ref<R> prepared_result = prepare_stmt(db, p_sql, prepared_stmt);
	if (prepared_result.is_valid()) {
		return prepared_result;
	}
	if (sqlite3_bind_parameter_count(prepared_stmt.get()) != p_params.size()) {
		return R::err("sqlite parameter count does not match", Err::INVALID_DATA);
	}
	for (int i = 0; i < p_params.size(); i++) {
		const CharString name = (String("$") + String::num_int64(i + 1)).utf8();
		const int at = sqlite3_bind_parameter_index(prepared_stmt.get(), name.get_data());
		if (at == 0) {
			return R::err(vformat("portable SQL parameter $%d is missing", i + 1), Err::INVALID_DATA);
		}
		const int code = bind_one(prepared_stmt.get(), at, p_params[i]);
		if (code == SQLITE_MISMATCH) {
			return R::err(vformat("sqlite parameter %d has an unsupported type", i + 1), Err::INVALID_DATA);
		}
		if (code != SQLITE_OK) {
			return db_error(db, code, "cannot bind sqlite parameter");
		}
	}
	PackedStringArray columns;
	const int count = sqlite3_column_count(prepared_stmt.get());
	for (int i = 0; i < count; i++) {
		columns.push_back(String::utf8(sqlite3_column_name(prepared_stmt.get(), i)));
	}
	p_rows->sqlite_opened(prepared_stmt.release(), columns);
	return R::ok();
}

// Advance a shared Rows statement by one row with sqlite3_step.
bool GDSQLiteDB::step_rows(GDDatabaseRows *p_rows) {
	MutexLock lock(mutex);
	if (!p_rows) {
		return true;
	}
	sqlite3_stmt *active = nullptr;
	{
		MutexLock rows_lock(p_rows->mutex);
		active = p_rows->stmt;
	}
	if (!db || !active) {
		p_rows->sqlite_result(Array(), true, Err::make("sqlite Rows is closed", Err::INTERRUPTED), String());
		return true;
	}
	struct StopScope {
		const SafeFlag *&flag;
		~StopScope() { flag = nullptr; }
	};
	stop_flag = &p_rows->stopped;
	StopScope stop_scope{ stop_flag };
	const int code = sqlite3_step(active);
	if (code == SQLITE_ROW) {
		Array values;
		const int count = sqlite3_column_count(active);
		values.resize(count);
		for (int i = 0; i < count; i++) {
			values[i] = column_of(active, i);
		}
		p_rows->sqlite_result(values, false, Ref<Err>(), String());
		return false;
	}
	String complete_tag;
	Ref<Err> error;
	if (code == SQLITE_DONE) {
		int64_t count = 0;
		{
			MutexLock rows_lock(p_rows->mutex);
			count = p_rows->row_count;
		}
		complete_tag = sqlite3_stmt_readonly(active) ? vformat("SELECT %d", count) : vformat("CHANGE %d", sqlite3_changes64(db));
	} else if (p_rows->stopped.is_set()) {
		error = Err::make("database Rows was closed", Err::INTERRUPTED);
	} else {
		error = db_error(db, code, "cannot advance sqlite Rows")->get_e();
	}
	sqlite3_finalize(active);
	{
		MutexLock rows_lock(p_rows->mutex);
		if (p_rows->stmt == active) {
			p_rows->stmt = nullptr;
		}
	}
	p_rows->sqlite_result(Array(), true, error, complete_tag);
	return true;
}

// Release a shared Rows statement on the same embedded SQL worker.
void GDSQLiteDB::close_rows(GDDatabaseRows *p_rows) {
	MutexLock lock(mutex);
	if (!p_rows) {
		return;
	}
	sqlite3_stmt *active = nullptr;
	{
		MutexLock rows_lock(p_rows->mutex);
		active = p_rows->stmt;
		p_rows->stmt = nullptr;
	}
	if (active) {
		const int code = sqlite3_finalize(active);
		if (code != SQLITE_OK) {
			const Ref<Err> error = db_error(db, code, "cannot close sqlite Rows")->get_e();
			MutexLock rows_lock(p_rows->mutex);
			if (p_rows->failed.is_null()) {
				p_rows->failed = error;
			}
		}
	}
}

// Create a prepared statement that parses SQL once.
Ref<R> GDSQLiteDB::prepare(const String &p_sql) {
	MutexLock lock(mutex);
	if (!db) {
		return R::err("sqlite database is closed", Err::INVALID_DATA);
	}
	due = max_ms > 0 ? GDClock::msec() + max_ms : 0;
	Stmt stmt;
	const Ref<R> failed = prepare_stmt(db, p_sql, stmt);
	if (failed.is_valid()) {
		return failed;
	}
	if (sqlite3_bind_parameter_count(stmt.get()) > sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, -1)) {
		return R::err("sqlite parameter count exceeds the limit", Err::LIMITED);
	}
	Ref<GDSQLiteStatement> out;
	out.instantiate();
	out->owner = Ref<GDSQLiteDB>(this);
	out->stmt = stmt.release();
	prepared.insert(out.ptr());
	return R::ok(out);
}

// Execute an explicit prepared statement under the connection lock.
Ref<R> GDSQLiteDB::run_prepared(GDSQLiteStatement *p_owner, const Array &p_params, RunMode p_mode) {
	MutexLock lock(mutex);
	if (!db || !prepared.has(p_owner) || !p_owner->stmt) {
		return R::err("sqlite prepared statement is closed", Err::INVALID_DATA);
	}
	due = max_ms > 0 ? GDClock::msec() + max_ms : 0;
	Ref<R> out;
	{
		ResetStmt reset(p_owner->stmt);
		out = execute(p_owner->stmt, p_params, p_mode);
	}
	return out;
}

// Execute a write statement for multiple argument sets in one native call.
Ref<R> GDSQLiteDB::run_many_prepared(GDSQLiteStatement *p_owner, const Array &p_rows) {
	MutexLock lock(mutex);
	if (!db || !prepared.has(p_owner) || !p_owner->stmt) {
		return R::err("sqlite prepared statement is closed", Err::INVALID_DATA);
	}
	due = max_ms > 0 ? GDClock::msec() + max_ms : 0;
	sqlite3_stmt *stmt = p_owner->stmt;
	if (sqlite3_stmt_readonly(stmt)) {
		return R::err("sqlite run_many accepts update SQL", Err::PERMISSION_DENIED);
	}
	if (p_rows.is_empty()) {
		return R::err("sqlite batch size is outside the limit", Err::LIMITED);
	}

	const int expected = sqlite3_bind_parameter_count(stmt);
	uint64_t input_count = 0;
	for (int row_at = 0; row_at < p_rows.size(); row_at++) {
		if (due > 0 && (row_at & 255) == 255 && GDClock::msec() > due) {
			return R::err("sqlite batch validation timed out", Err::TIMED_OUT);
		}
		if (p_rows[row_at].get_type() != Variant::ARRAY) {
			return R::err(vformat("sqlite batch row %d is not an Array", row_at + 1), Err::INVALID_DATA);
		}
		const Array params = p_rows[row_at];
		if (params.size() != expected) {
			return R::err(vformat("sqlite batch row %d parameter count does not match", row_at + 1), Err::INVALID_DATA);
		}
		for (int i = 0; i < params.size(); i++) {
			input_count++;
			const int64_t bytes = value_bytes(params[i]);
			if (bytes < 0) {
				return R::err(vformat("sqlite batch row %d parameter %d has an unsupported type", row_at + 1, i + 1), Err::INVALID_DATA);
			}
			if (due > 0 && (input_count & 1023) == 0 && GDClock::msec() > due) {
				return R::err("sqlite batch validation timed out", Err::TIMED_OUT);
			}
		}
	}

	Ref<R> failed;
	int64_t changes = 0;
	{
		ResetStmt reset(stmt);
		failed = execute_many(db, stmt, p_rows, due, changes);
	}
	if (failed.is_valid()) {
		return failed;
	}
	Dictionary out;
	out["changes"] = changes;
	out["last_id"] = int64_t(sqlite3_last_insert_rowid(db));
	return R::ok(out);
}

// Detach and release an explicit prepared statement.
void GDSQLiteDB::drop_prepared(GDSQLiteStatement *p_stmt) {
	MutexLock lock(mutex);
	if (!p_stmt->stmt) {
		return;
	}
	prepared.erase(p_stmt);
	sqlite3_finalize(p_stmt->stmt);
	p_stmt->stmt = nullptr;
}

// Check whether an explicit prepared statement is usable on this connection.
bool GDSQLiteDB::has_prepared(const GDSQLiteStatement *p_stmt) const {
	MutexLock lock(mutex);
	return db && prepared.has(const_cast<GDSQLiteStatement *>(p_stmt)) && p_stmt->stmt;
}

// Release the embedded SQL execution program on destruction.
GDSQLiteStatement::~GDSQLiteStatement() {
	close();
	owner.unref();
}

// Execute a write statement and return affected rows.
Ref<R> GDSQLiteStatement::run(const Array &p_params) {
	return owner.is_valid() ? owner->run_prepared(this, p_params, GDSQLiteDB::RUN_EXEC) : R::err("sqlite prepared statement is closed", Err::INVALID_DATA);
}

// Execute a write statement for multiple argument sets.
Ref<R> GDSQLiteStatement::run_many(const Array &p_rows) {
	return owner.is_valid() ? owner->run_many_prepared(this, p_rows) : R::err("sqlite prepared statement is closed", Err::INVALID_DATA);
}

// Return the first read-only query row.
Ref<R> GDSQLiteStatement::one(const Array &p_params) {
	return owner.is_valid() ? owner->run_prepared(this, p_params, GDSQLiteDB::RUN_GET) : R::err("sqlite prepared statement is closed", Err::INVALID_DATA);
}

// Return all read-only query rows.
Ref<R> GDSQLiteStatement::all(const Array &p_params) {
	return owner.is_valid() ? owner->run_prepared(this, p_params, GDSQLiteDB::RUN_QUERY) : R::err("sqlite prepared statement is closed", Err::INVALID_DATA);
}

// Release the prepared statement explicitly.
void GDSQLiteStatement::close() {
	if (owner.is_valid()) {
		owner->drop_prepared(this);
	}
}

// Check whether both statement and connection are usable.
bool GDSQLiteStatement::is_valid() const {
	return owner.is_valid() && owner->has_prepared(this);
}

// Expose prepared-statement methods to script.
void GDSQLiteStatement::_bind_methods() {
	ClassDB::bind_method(D_METHOD("run", "params"), &GDSQLiteStatement::run, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("run_many", "rows"), &GDSQLiteStatement::run_many);
	ClassDB::bind_method(D_METHOD("one", "params"), &GDSQLiteStatement::one, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("all", "params"), &GDSQLiteStatement::all, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("close"), &GDSQLiteStatement::close);
	ClassDB::bind_method(D_METHOD("is_valid"), &GDSQLiteStatement::is_valid);
	ADD_RESULT("run", "Dictionary");
	ADD_RESULT("run_many", "Dictionary");
	ADD_RESULT("one", "Variant");
	ADD_RESULT("all", "Array");
}

// Interrupt an operation running on another thread.
void GDSQLiteDB::interrupt() {
	if (db) {
		sqlite3_interrupt(db);
	}
}

// Close the embedded SQL connection.
void GDSQLiteDB::close() {
	MutexLock lock(mutex);
	if (db) {
		sqlite3_progress_handler(db, 0, nullptr, nullptr);
		clear_stmts();
		for (GDSQLiteStatement *stmt : prepared) {
			sqlite3_finalize(stmt->stmt);
			stmt->stmt = nullptr;
		}
		prepared.clear();
		sqlite3_close_v2(db);
		db = nullptr;
	}
#ifdef UNIX_ENABLED
	release_database(pinned_id);
	pinned_id = 0;
#endif
}

// Check whether the embedded SQL connection is open.
bool GDSQLiteDB::is_open() const {
	MutexLock lock(mutex);
	return db != nullptr;
}

// Expose embedded SQL connection methods to script.
void GDSQLiteDB::_bind_methods() {
	ClassDB::bind_method(D_METHOD("exec", "sql", "params"), &GDSQLiteDB::exec, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query", "sql", "params"), &GDSQLiteDB::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("prepare", "sql"), &GDSQLiteDB::prepare);
	ClassDB::bind_method(D_METHOD("close"), &GDSQLiteDB::close);
	ClassDB::bind_method(D_METHOD("is_open"), &GDSQLiteDB::is_open);
	ADD_RESULT("exec", "Dictionary");
	ADD_RESULT("query", "Array");
	ADD_RESULT("prepare", "GDSQLiteStatement");
}
