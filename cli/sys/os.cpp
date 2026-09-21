/**************************************************************************/
/*  os.cpp                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement environment and filesystem operations declared in os.h.

#include "cli/sys/os.h"

#include "cli/sys/perm.h"
#include "cli/sys/mount.h"
#include "cli/sys/source_error.h"

#include "cli/sys/native_file.h"

#ifdef UNIX_ENABLED
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(WINDOWS_ENABLED)
#include <windows.h>
#endif

namespace {

constexpr int COPY_CHUNK = 32 * 1024; // Default copy-buffer size in bytes.
constexpr int READ_CHUNK = 32 * 1024; // One incremental read buffer, not a total-content limit.
constexpr int REMOVE_BATCH = 1024; // Directory entries read per removal batch.

#ifdef UNIX_ENABLED
// Preserve the first recursive-removal OS error and a display path derived from the raw name.
struct RemoveIssue {
	const char *op = nullptr; // Failed system call.
	String path; // Path returned to the caller.
	int code = 0; // errno
};

// Record only the first failure while continuing other removals.
void keep_remove_issue(RemoveIssue &r_issue, const char *p_op, const String &p_path, int p_code) {
	if (r_issue.code == 0) {
		r_issue.op = p_op;
		r_issue.path = p_path;
		r_issue.code = p_code;
	}
}

// Remove trees using parent descriptors and raw POSIX names without reinterpreting backslashes or invalid UTF-8.
bool remove_all_at(int p_parent, const CharString &p_name, const String &p_path, RemoveIssue &r_issue) {
	if (::unlinkat(p_parent, p_name.get_data(), 0) == 0 || errno == ENOENT) {
		return true;
	}
	const int first = errno;
	if (first != EISDIR && first != EPERM && first != EACCES) {
		keep_remove_issue(r_issue, "unlinkat", p_path, first);
		return false;
	}
	bool finished = false;
	while (!finished) {
		const int fd = ::openat(p_parent, p_name.get_data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) {
			const int code = errno;
			if (code == ENOENT) {
				return true;
			}
			keep_remove_issue(r_issue, code == ENOTDIR ? "unlinkat" : "openat", p_path, code == ENOTDIR ? first : code);
			return false;
		}
		DIR *stream = ::fdopendir(fd);
		if (!stream) {
			const int code = errno;
			::close(fd);
			keep_remove_issue(r_issue, "readdirnames", p_path, code);
			return false;
		}
		bool restart = false;
		while (!restart) {
			LocalVector<CharString> names;
			bool eof = false;
			errno = 0;
			while (names.size() < REMOVE_BATCH) {
				dirent *entry = ::readdir(stream);
				if (!entry) {
					eof = true;
					break;
				}
				if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
					names.push_back(CharString(entry->d_name));
				}
			}
			if (eof && errno != 0) {
				const int code = errno;
				::closedir(stream);
				keep_remove_issue(r_issue, "readdirnames", p_path, code);
				return false;
			}
			bool removed = false;
			for (const CharString &name : names) {
				const String child = p_path + "/" + String::utf8(name.get_data(), name.length());
				removed = remove_all_at(fd, name, child, r_issue) || removed;
			}
			if (removed) {
				restart = true;
			} else if (eof) {
				finished = true;
				restart = true;
			}
		}
		::closedir(stream);
	}
	if (::unlinkat(p_parent, p_name.get_data(), AT_REMOVEDIR) == 0 || errno == ENOENT) {
		return true;
	}
	keep_remove_issue(r_issue, "unlinkat", p_path, errno);
	return false;
}
#endif

// Lock the target itself and use the same file for comparison and writing.
class WriteLock {
#ifdef UNIX_ENABLED
	int fd = -1; // Target descriptor used for flock and I/O.
#elif defined(WINDOWS_ENABLED)
	HANDLE file = INVALID_HANDLE_VALUE; // Target handle used for locking and I/O.
	OVERLAPPED range = {}; // Lock range covering the whole file.
#else
	Ref<GDFile> file; // Update target on platforms without native file locks.
#endif
	SourceError::Value issue; // Original failed OS operation.
	Error issue_error = OK; // Fallback engine error when no OS code is available.
	bool issue_kept = false; // Whether a pre-rollback failure was preserved.

	// Capture errno immediately after a failed POSIX operation.
	void keep_posix(int p_code) {
		if (issue_kept) {
			return;
		}
		issue.source = SourceError::POSIX;
		issue.code = p_code;
		issue_kept = true;
	}

	// Capture the error code immediately after a failed Windows operation.
	void keep_win32(uint32_t p_code) {
		if (issue_kept) {
			return;
		}
		issue.source = SourceError::WINDOWS;
		issue.code = p_code;
		issue_kept = true;
	}

	// Preserve the original mount or OS error together with an engine fallback for unknown codes.
	void keep_source(Error p_fallback) {
		if (issue_kept) {
			return;
		}
		issue = SourceError::take();
		issue_error = p_fallback;
		issue_kept = true;
	}

	// Preserve failures without OS codes before rollback can overwrite them.
	void keep_error(Error p_error) {
		if (issue_kept) {
			return;
		}
		issue_error = p_error;
		issue_kept = true;
	}

public:
	// Restore the original PathError and return an operation-appropriate fallback Error.
	Error error(Error p_fallback) const {
		if (issue.source == SourceError::POSIX) {
			SourceError::posix((int)issue.code);
		} else if (issue.source == SourceError::WINDOWS) {
			SourceError::win32((uint32_t)issue.code);
		} else {
			SourceError::clear();
		}
		return issue_error == OK ? p_fallback : issue_error;
	}

	// Open and lock one file while determining whether it already exists.
	int take(const String &p_path, bool p_create, bool &r_existed) {
		SourceError::clear();
		issue = SourceError::Value();
		issue_error = OK;
		issue_kept = false;
#ifdef UNIX_ENABLED
		Mount::At at;
		String why;
		Error gate = OK;
		if (!Mount::at(p_path, true, at, why, &gate)) {
			keep_source(gate == OK ? ERR_FILE_CANT_OPEN : gate);
			return -1;
		}
		const CharString leaf = at.leaf.utf8();
		fd = ::openat(at.fd, leaf.get_data(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		r_existed = fd >= 0;
		if (fd < 0 && errno == ENOENT && p_create) {
			fd = ::openat(at.fd, leaf.get_data(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (fd < 0 && errno == EEXIST) {
				fd = ::openat(at.fd, leaf.get_data(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
				r_existed = true;
			}
		}
		if (fd < 0) {
			const int code = errno;
			keep_posix(code);
			return code == ENOENT ? 0 : -1;
		}
		int code = 0;
		do {
			code = ::flock(fd, LOCK_EX);
		} while (code != 0 && errno == EINTR);
		if (code != 0) {
			keep_posix(errno);
			return -1;
		}
		return 1;
#elif defined(WINDOWS_ENABLED)
		String why;
		const String real = Mount::resolve(p_path, true, why);
		if (real.is_empty()) {
			keep_source(ERR_FILE_CANT_OPEN);
			return -1;
		}
		file = ::CreateFileW((LPCWSTR)real.utf16().get_data(), GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		r_existed = file != INVALID_HANDLE_VALUE;
		if (file == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_FILE_NOT_FOUND && p_create) {
			file = ::CreateFileW((LPCWSTR)real.utf16().get_data(), GENERIC_READ | GENERIC_WRITE,
					FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_FILE_EXISTS) {
				file = ::CreateFileW((LPCWSTR)real.utf16().get_data(), GENERIC_READ | GENERIC_WRITE,
						FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				r_existed = true;
			}
		}
		if (file == INVALID_HANDLE_VALUE) {
			const DWORD code = ::GetLastError();
			keep_win32(code);
			return code == ERROR_FILE_NOT_FOUND ? 0 : -1;
		}
		if (::LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &range) == 0) {
			keep_win32(::GetLastError());
			return -1;
		}
		return 1;
#else
		r_existed = GDFile::exists(p_path);
		if (!r_existed && !p_create) {
			return 0;
		}
		file = GDFile::open(p_path, r_existed ? GDFile::READ_WRITE : GDFile::WRITE_READ);
		if (file.is_null()) {
			keep_source(GDFile::get_open_error());
			return -1;
		}
		return 1;
#endif
	}

	// Read the locked file for both text comparison and rollback.
	bool read(String &r_text, PackedByteArray &r_raw) {
#ifdef UNIX_ENABLED
		struct stat st;
		if (::fstat(fd, &st) != 0) {
			keep_posix(errno);
			return false;
		}
		// Check that text-decoder length and terminator fit the int representation.
		if (st.st_size < 0 || st.st_size >= INT_MAX) {
			keep_error(ERR_OUT_OF_MEMORY);
			return false;
		}
		if (r_raw.resize((int)st.st_size) != OK) {
			keep_error(ERR_OUT_OF_MEMORY);
			return false;
		}
		int at = 0;
		while (at < r_raw.size()) {
			const ssize_t n = ::pread(fd, r_raw.ptrw() + at, r_raw.size() - at, at);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				if (n < 0) {
					keep_posix(errno);
				} else {
					keep_error(ERR_FILE_CANT_READ);
				}
				return false;
			}
			at += (int)n;
		}
#elif defined(WINDOWS_ENABLED)
		LARGE_INTEGER size;
		if (!::GetFileSizeEx(file, &size)) {
			keep_win32(::GetLastError());
			return false;
		}
		// Check that text-decoder length and terminator fit the int representation.
		if (size.QuadPart < 0 || size.QuadPart >= INT_MAX) {
			keep_error(ERR_OUT_OF_MEMORY);
			return false;
		}
		if (r_raw.resize((int)size.QuadPart) != OK) {
			keep_error(ERR_OUT_OF_MEMORY);
			return false;
		}
		LARGE_INTEGER zero = {};
		if (!::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
			keep_win32(::GetLastError());
			return false;
		}
		int at = 0;
		while (at < r_raw.size()) {
			DWORD n = 0;
			if (!::ReadFile(file, r_raw.ptrw() + at, r_raw.size() - at, &n, nullptr)) {
				keep_win32(::GetLastError());
				return false;
			}
			if (n == 0) {
				keep_error(ERR_FILE_CANT_READ);
				return false;
			}
			at += (int)n;
		}
#else
		const uint64_t size = file->get_length();
		r_raw = file->get_buffer(size);
		if ((uint64_t)r_raw.size() != size) {
			const Error err = file->get_error();
			keep_error(err == OK || err == ERR_FILE_EOF ? ERR_FILE_CANT_READ : err);
			return false;
		}
#endif
		if (r_raw.size() >= INT_MAX) {
			keep_error(ERR_OUT_OF_MEMORY);
			return false;
		}
		r_text = String::utf8((const char *)r_raw.ptr(), r_raw.size());
		return true;
	}

	// Write back through the locked descriptor to avoid replacement races.
	bool write(const PackedByteArray &p_old, const CharString &p_body) {
		const int old_len = p_old.size();
		const int new_len = p_body.length();
#ifdef UNIX_ENABLED
		auto put = [&](const uint8_t *p_data, int p_size, int p_at) {
			int done = 0;
			while (done < p_size) {
				const ssize_t n = ::pwrite(fd, p_data + done, p_size - done, p_at + done);
				if (n < 0 && errno == EINTR) {
					continue;
				}
				if (n <= 0) {
					if (n < 0) {
						keep_posix(errno);
					} else {
						keep_error(ERR_FILE_CANT_WRITE);
					}
					return false;
				}
				done += (int)n;
			}
			return true;
		};
		auto resize = [&](int p_size) {
			if (::ftruncate(fd, p_size) == 0) {
				return true;
			}
			keep_posix(errno);
			return false;
		};
#elif defined(WINDOWS_ENABLED)
		auto put = [&](const uint8_t *p_data, int p_size, int p_at) {
			LARGE_INTEGER at;
			at.QuadPart = p_at;
			if (!::SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) {
				keep_win32(::GetLastError());
				return false;
			}
			int done = 0;
			while (done < p_size) {
				DWORD n = 0;
				if (!::WriteFile(file, p_data + done, p_size - done, &n, nullptr)) {
					keep_win32(::GetLastError());
					return false;
				}
				if (n == 0) {
					keep_error(ERR_FILE_CANT_WRITE);
					return false;
				}
				done += (int)n;
			}
			return true;
		};
		auto resize = [&](int p_size) {
			LARGE_INTEGER at;
			at.QuadPart = p_size;
			if (!::SetFilePointerEx(file, at, nullptr, FILE_BEGIN) || !::SetEndOfFile(file)) {
				keep_win32(::GetLastError());
				return false;
			}
			return true;
		};
#else
		auto put = [&](const uint8_t *p_data, int p_size, int p_at) {
			file->seek(p_at);
			const Error sought = file->get_error();
			if (sought != OK) {
				keep_error(sought);
				return false;
			}
			if (file->store_buffer(p_data, p_size)) {
				return true;
			}
			const Error err = file->get_error();
			keep_error(err == OK ? ERR_FILE_CANT_WRITE : err);
			return false;
		};
		auto resize = [&](int p_size) {
			const Error err = file->resize(p_size);
			if (err == OK) {
				return true;
			}
			keep_error(err);
			return false;
		};
#endif
		const uint8_t *body = (const uint8_t *)p_body.get_data();
		if (new_len > old_len && !put(body + old_len, new_len - old_len, old_len)) {
			resize(old_len);
			return false;
		}
		const int head = MIN(new_len, old_len);
		if ((head > 0 && !put(body, head, 0)) || (new_len < old_len && !resize(new_len))) {
			if (old_len == 0 || put(p_old.ptr(), old_len, 0)) {
				resize(old_len);
			}
			return false;
		}
		return true;
	}

	~WriteLock() {
#ifdef UNIX_ENABLED
		if (fd >= 0) {
			::flock(fd, LOCK_UN);
			::close(fd);
		}
#elif defined(WINDOWS_ENABLED)
		if (file != INVALID_HANDLE_VALUE) {
			::UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &range);
			::CloseHandle(file);
		}
#endif
	}
};

// Serialize in-process updates on platforms without native file locking.
Mutex &write_mutex() {
	static Mutex mutex;
	return mutex;
}

// Return operation failures with machine-readable details.
// Classify only known causes and preserve original errno or Win32 codes.
Ref<R> fail(const String &p_path, const char *p_op, Error p_err, const String &p_msg, const Variant &p_value = Variant()) {
	const Ref<R> result = SourceError::path(p_path, p_op, p_err, p_msg);
	return p_value.get_type() == Variant::NIL ? result : R::err(result->get_e(), Err::NONE, p_value);
}

// Return two-path failures with old and new paths.
Ref<R> link_fail(const String &p_old, const String &p_new, const char *p_op, Error p_err, const String &p_msg) {
	Dictionary info;
	info["op"] = p_op;
	info["old"] = p_old;
	info["new"] = p_new;
	const Error err = SourceError::put(info, p_err);
	return R::err(Err::make(p_msg, Err::of(err), info));
}

// Attach the same operation and target context to failures detected without an OS call.
Ref<R> fail_engine(const String &p_path, const char *p_op, Error p_err, const String &p_msg) {
	SourceError::clear();
	return fail(p_path, p_op, p_err, p_msg);
}

// Keep permission category names distinct from the actual operation name.
Ref<R> denied(const String &p_path, const char *p_access, const char *p_op) {
	SourceError::clear();
	return fail(p_path, p_op, ERR_UNAUTHORIZED,
			vformat("%s access to \"%s\" is not allowed", p_access, p_path));
}

// Preserve old and new paths in rename denial, including an empty destination.
Ref<R> link_denied(const String &p_old, const String &p_new, const String &p_denied) {
	SourceError::clear();
	return link_fail(p_old, p_new, "rename", ERR_UNAUTHORIZED,
			vformat("write access to \"%s\" is not allowed", p_denied));
}

// Read an open failure immediately after the corresponding file open.
// The per-type get_open_error slot belongs only to that open operation.
// Reading it after another operation or from another type would report an unrelated failure.
Ref<R> file_error(const String &p_path, const char *p_access) {
	return fail(p_path, "open", GDFile::get_open_error(),
			vformat("cannot open %s for %s", p_path, p_access));
}

// Report post-open I/O failures in their actual operation direction, not as invalid input.
Error file_io_error(const Ref<GDFile> &p_file, Error p_fallback) {
	const Error err = p_file->get_error();
	return err == OK || err == ERR_FILE_EOF ? p_fallback : err;
}

// Read an opened file through EOF, preserving bytes obtained before an intermediate failure.
Ref<R> read_opened(const String &p_path, const Ref<GDFile> &p_file, uint64_t p_offset, uint64_t p_max) {
	p_file->seek(p_offset);
	const Error sought = p_file->get_error();
	if (sought != OK && sought != ERR_FILE_EOF) {
		return fail(p_path, "seek", file_io_error(p_file, ERR_FILE_CANT_READ), vformat("cannot seek %s", p_path));
	}
	PackedByteArray body;
	// Return at the requested maximum without waiting for EOF.
	while (p_max == 0 || (uint64_t)body.size() < p_max) {
		const int want = p_max == 0 ? READ_CHUNK : (int)MIN((uint64_t)READ_CHUNK, p_max - body.size());
		const int64_t old = body.size();
		if (old > INT64_MAX - want || body.resize(old + want) != OK) {
			SourceError::clear();
			return fail(p_path, "read", ERR_OUT_OF_MEMORY, vformat("cannot read %s", p_path), body);
		}
		const uint64_t n = p_file->get_buffer(body.ptrw() + old, want);
		const Error err = p_file->get_error();
		if (n > (uint64_t)want) {
			body.resize(old);
			return fail(p_path, "read", ERR_FILE_CANT_READ, vformat("cannot read %s", p_path), body);
		}
		body.resize(old + (int64_t)n);
		if (err == ERR_FILE_EOF) {
			break;
		}
		if (err != OK) {
			return fail(p_path, "read", err, vformat("cannot read %s", p_path), body);
		}
		if (n == 0) {
			return fail(p_path, "read", ERR_FILE_CANT_READ, vformat("cannot read %s", p_path), body);
		}
	}
	return R::ok(body);
}

// Capture directory-open failure immediately, without consulting file-open state.
Ref<R> dir_error(const String &p_path, const char *p_access) {
	return fail(p_path, "open", GDDir::get_open_error(),
			vformat("cannot open %s for %s", p_path, p_access));
}

#ifndef UNIX_ENABLED
// Remove one file or a directory that has already been emptied.
Ref<R> drop_one(const String &p_path) {
	// Deletion requires write permission; opening the directory checked only read access.
	GD_PERM_FAIL_V(WRITE, p_path, denied(p_path, "write", "remove"));
	SourceError::clear();
	Ref<GDDir> parent = GDDir::open(p_path.get_base_dir());
	if (parent.is_null()) {
		return dir_error(p_path, "write");
	}
	const Error err = parent->remove(p_path.get_file());
	if (err != OK) {
		return fail(p_path, "remove", err, vformat("cannot remove %s", p_path));
	}
	return R::ok();
}

// Determine whether directory opening failed because the target was a file.
bool not_dir_error(const Ref<R> &p_result) {
	if (p_result.is_null() || p_result->get_e().is_null()) {
		return false;
	}
	const Dictionary info = p_result->get_e()->get_info();
#ifdef WINDOWS_ENABLED
	return info.get("source", "") == "win32" && (int64_t)info.get("source_code", 0) == ERROR_PATH_NOT_FOUND;
#else
	return false;
#endif
}
#endif

// Return the next name, always skipping dot entries and including hidden entries only on request.
String next_of(const Ref<GDDir> &p_dir, bool p_hidden) {
	SourceError::clear();
	String next = p_dir->get_next();
	while (!next.is_empty() && (next == "." || next == ".." || (!p_hidden && p_dir->current_is_hidden()))) {
		if (SourceError::kept()) {
			return String();
		}
		SourceError::clear();
		next = p_dir->get_next();
	}
	return SourceError::kept() ? String() : next;
}

// Create parent directories in order and preserve intermediate OS failure details.
Error make_dirs(const Ref<GDDir> &p_dir, const String &p_path) {
	String full = p_path.is_relative_path() ? p_dir->get_current_dir().path_join(p_path) : p_path;
	full = full.replace_char('\\', '/');
	String base;
	String tail;
	const String scheme = Mount::scheme_of(full, tail);
	if (!scheme.is_empty()) {
		base = scheme + "://";
	} else if (full.is_network_share_path()) {
		int at = full.find_char('/', 2);
		if (at < 0 || (at = full.find_char('/', at + 1)) < 0) {
			SourceError::clear();
			return ERR_INVALID_PARAMETER;
		}
		base = full.substr(0, at + 1);
	} else if (full.begins_with("/")) {
		base = "/";
	} else if (full.contains(":/")) {
		base = full.substr(0, full.find(":/") + 2);
	} else {
		SourceError::clear();
		return ERR_INVALID_PARAMETER;
	}
	const Vector<String> parts = full.replace_first(base, "").simplify_path().split("/");
	String current = base;
	for (const String &part : parts) {
		current = current.path_join(part);
		const Error err = p_dir->make_dir(current);
		if (err != OK && err != ERR_ALREADY_EXISTS) {
			return err;
		}
		SourceError::clear();
	}
	// Since make_dir also returns AlreadyExists for files, verify the final component is a directory.
	const Perm::Trusted trust;
	if (!p_dir->dir_exists(p_path)) {
		SourceError::not_dir();
		return FAILED;
	}
	return OK;
}

// Collect descendants recursively, stopping on failure.
Ref<R> walk_into(const String &p_path, bool p_want_dirs, bool p_hidden, PackedStringArray &r_out) {
	SourceError::clear();
	Ref<GDDir> dir = GDDir::open(p_path);
	if (dir.is_null()) {
		return dir_error(p_path, "read");
	}
	// Use this enumeration call's failure rather than stale directory-open state.
	const Error began = dir->list_dir_begin();
	if (began != OK) {
		return fail(p_path, "readdir", began, vformat("cannot list %s", p_path));
	}
	String name = next_of(dir, p_hidden);
	while (!name.is_empty()) {
		if (p_hidden || !name.begins_with(".")) {
			const String full = p_path.path_join(name);
			if (dir->current_is_dir()) {
				if (p_want_dirs) {
					r_out.push_back(full);
				}
				const Ref<R> sub = walk_into(full, p_want_dirs, p_hidden, r_out);
				if (sub->get_e().is_valid()) {
					dir->list_dir_end();
					return sub;
				}
			} else {
				r_out.push_back(full);
			}
		}
		name = next_of(dir, p_hidden);
	}
	dir->list_dir_end();
	if (SourceError::kept()) {
		return fail(p_path, "readdir", FAILED, vformat("cannot list %s", p_path));
	}
	return R::ok();
}
} // namespace

// Acquire an OS file lock and retain it for the object's lifetime.
Ref<R> GDFileLock::take(const String &p_path) {
	GD_PERM_FAIL_V(READ, p_path, denied(p_path, "read", "lock"));
	GD_PERM_FAIL_V(WRITE, p_path, denied(p_path, "write", "lock"));
	WriteLock *lock = memnew(WriteLock);
	bool existed = false;
	if (lock->take(p_path, true, existed) < 1) {
		const Error err = lock->error(ERR_FILE_CANT_OPEN);
		memdelete(lock);
		return fail(p_path, "lock", err, vformat("cannot lock %s", p_path));
	}
	state = lock;
	return R::ok();
}

// Release the retained OS file lock.
GDFileLock::~GDFileLock() {
	if (state) {
		memdelete(static_cast<WriteLock *>(state));
		state = nullptr;
	}
}

// Walk the directory tree and return the requested files or directories.
Ref<R> Os::walk(const String &p_path, bool p_want_dirs, bool p_hidden) {
	PackedStringArray out;
	const Ref<R> err = walk_into(p_path, p_want_dirs, p_hidden, out);
	if (err->get_e().is_valid()) {
		return R::err(err->get_e(), Err::NONE, out);
	}
	return R::ok(out);
}

// Return file paths matching a pattern.
Ref<R> Os::glob(const String &p_path, const String &p_pattern, bool p_hidden) {
	PackedStringArray all;
	const Ref<R> err = walk_into(p_path, false, p_hidden, all);
	PackedStringArray out;
	for (const String &n : all) {
		if (n.match(p_pattern) || n.get_file().match(p_pattern)) {
			out.push_back(n);
		}
	}
	return err->get_ok() ? R::ok(out) : R::err(err->get_e(), Err::NONE, out);
}

// Remove the specified directory tree recursively.
Ref<R> Os::remove_all(const String &p_path) {
	if (p_path.is_empty()) {
		return R::ok(); // An empty path is a no-op.
	}
	const String slash = p_path.replace("\\", "/");
	if (slash == "." || slash.ends_with("/.")) {
		SourceError::clear();
		return fail(p_path, "RemoveAll", ERR_INVALID_PARAMETER, "cannot remove a dot path");
	}
	if (Mount::is_root(p_path)) {
		SourceError::clear();
		return fail(p_path, "RemoveAll", ERR_INVALID_PARAMETER, "cannot remove a mount root");
	}
	// Check access before probing existence to avoid exposing restricted filesystem information.
	if (!Perm::check(Perm::WRITE, p_path)) {
		return denied(p_path, "write", "RemoveAll");
	}
#ifdef UNIX_ENABLED
	Mount::At at;
	String why;
	Error gate = OK;
	if (!Mount::at(p_path, true, at, why, &gate, true)) {
		if (gate == ERR_FILE_NOT_FOUND) {
			return R::ok();
		}
		return fail(p_path, "RemoveAll", gate, why);
	}
	RemoveIssue issue;
	if (remove_all_at(at.fd, at.leaf.utf8(), p_path, issue)) {
		return R::ok();
	}
	SourceError::posix(issue.code);
	return fail(issue.path, issue.op, SourceError::posix_error(issue.code), vformat("cannot remove %s", issue.path));
#else
	// Try removing one object first; success or NotFound completes the operation.
	// Otherwise inspect it as a directory without treating a failed existence probe as absence.
	const Ref<R> first = drop_one(p_path);
	if (first->get_e().is_null() || first->get_e()->is(Err::NOT_FOUND)) {
		return R::ok();
	}
	if (!first->get_e()->is(Err::ALREADY_EXISTS) && !first->get_e()->is(Err::PERMISSION_DENIED)) {
		return first; // Do not infer a directory from an I/O failure and begin deleting children.
	}
	Ref<R> child_error;
	bool done = false;
	while (!done) {
		SourceError::clear();
		Ref<GDDir> dir = GDDir::open(p_path);
		if (dir.is_null()) {
			const Ref<R> opened = dir_error(p_path, "read");
			if (not_dir_error(opened)) {
				return first; // For a file, return the original removal failure rather than the directory-open error.
			}
			if (opened->get_e()->is(Err::NOT_FOUND)) {
				return R::ok(); // Another process removed the target before recursion began.
			}
			return child_error.is_valid() ? child_error : opened;
		}
		const Error began = dir->list_dir_begin();
		if (began != OK) {
			const Ref<R> listed = fail(p_path, "readdir", began, vformat("cannot list %s", p_path));
			return child_error.is_valid() ? child_error : listed;
		}
		while (true) {
			PackedStringArray kids;
			bool eof = false;
			while (kids.size() < REMOVE_BATCH) {
				const String name = next_of(dir, true);
				if (name.is_empty()) {
					eof = true;
					break;
				}
				kids.push_back(p_path.path_join(name));
			}
			if (SourceError::kept()) {
				dir->list_dir_end();
				const Ref<R> stopped = fail(p_path, "readdir", FAILED, vformat("cannot list %s", p_path));
				return child_error.is_valid() ? child_error : stopped;
			}
			bool removed = false;
			for (const String &kid : kids) {
				const Ref<R> r = remove_all(kid);
				if (r->get_e().is_valid()) {
					if (child_error.is_null()) {
						child_error = r; // Preserve the first failure.
					}
				} else {
					removed = true;
				}
			}
			if (removed || eof) {
				dir->list_dir_end();
				done = eof && !removed;
				break; // Reopen from the beginning after removal so reordered directory entries are not skipped.
			}
			// If the full batch could not be removed, advance on the same enumeration stream.
		}
	}
	const Ref<R> last = drop_one(p_path);
	return child_error.is_valid() ? child_error : last;
#endif
}

// ---------------- Files ----------------

Ref<R> Os::read_text(const String &p_path) {
	SourceError::clear();
	Ref<GDFile> f = GDFile::open(p_path, GDFile::READ);
	if (f.is_null()) {
		return file_error(p_path, "read");
	}
	const Ref<R> read = read_opened(p_path, f, 0, 0);
	if (read->get_v().get_type() != Variant::PACKED_BYTE_ARRAY) {
		return read;
	}
	const PackedByteArray body = read->get_v();
	// Keep the text decoder's int length and terminating character within range before narrowing.
	if (body.size() >= INT_MAX) {
		SourceError::clear();
		return fail(p_path, "decode", ERR_OUT_OF_MEMORY, "file text exceeds String decoder representation; use read_bytes or a stream");
	}
	const String text = String::utf8((const char *)body.ptr(), (int)body.size());
	return read->get_ok() ? R::ok(text) : R::err(read->get_e(), Err::NONE, text);
}

// Read the entire file as bytes.
Ref<R> Os::read_bytes(const String &p_path, int64_t p_offset, int64_t p_max) {
	if (p_offset < 0 || p_max < 0) {
		return fail_engine(p_path, "read", ERR_INVALID_PARAMETER, "offset and max must not be negative");
	}
	SourceError::clear();
	Ref<GDFile> f = GDFile::open(p_path, GDFile::READ);
	if (f.is_null()) {
		return file_error(p_path, "read");
	}
	const uint64_t size = f->get_length();
	if (f->get_error() != OK) return fail(p_path, "stat", f->get_error(), vformat("cannot inspect %s", p_path));
	if ((uint64_t)p_offset > size) {
		return fail_engine(p_path, "read", ERR_INVALID_PARAMETER, vformat("offset is beyond %s", p_path));
	}
	return read_opened(p_path, f, p_offset, p_max);
}

// Replace file content with text.
Ref<R> Os::write_text(const String &p_path, const String &p_body) {
	SourceError::clear();
	Ref<GDFile> f = GDFile::open(p_path, GDFile::WRITE);
	if (f.is_null()) {
		return file_error(p_path, "write");
	}
	// Check the write result so a full device cannot be reported as success.
	if (!f->store_string(p_body)) {
		return fail(p_path, "write", file_io_error(f, ERR_FILE_CANT_WRITE), vformat("cannot write %s", p_path));
	}
	const Error closed = f->close();
	return closed == OK ? R::ok() : fail(p_path, "close", closed, vformat("cannot close %s", p_path));
}

// Replace a text file only if its prior content still matches.
Ref<R> Os::replace_text(const String &p_path, const Variant &p_old, const String &p_body) {
	GD_PERM_FAIL_V(READ, p_path, denied(p_path, "read", "replace"));
	GD_PERM_FAIL_V(WRITE, p_path, denied(p_path, "write", "replace"));
	MutexLock guard(write_mutex());
	WriteLock lock;
	bool existed = false;
	const int taken = lock.take(p_path, p_old.get_type() == Variant::NIL, existed);
	if (taken == 0) {
		return fail_engine(p_path, "replace", ERR_ALREADY_EXISTS, vformat("%s changed while it was being edited", p_path));
	}
	if (taken < 0) {
		return fail(p_path, "lock", lock.error(ERR_FILE_CANT_OPEN), vformat("cannot lock %s", p_path));
	}
	if (p_old.get_type() == Variant::NIL) {
		if (existed) {
			return fail_engine(p_path, "replace", ERR_ALREADY_EXISTS, vformat("%s changed while it was being edited", p_path));
		}
	}
	String current;
	PackedByteArray raw;
	if (!lock.read(current, raw)) {
		return fail(p_path, "read", lock.error(ERR_FILE_CANT_READ), vformat("cannot read %s", p_path));
	}
	if (p_old.get_type() != Variant::NIL && current != String(p_old)) {
		return fail_engine(p_path, "replace", ERR_ALREADY_EXISTS, vformat("%s changed while it was being edited", p_path));
	}
	if (!lock.write(raw, p_body.utf8())) {
		return fail(p_path, "write", lock.error(ERR_FILE_CANT_WRITE), vformat("cannot write %s", p_path));
	}
	return R::ok();
}

// Compare through the same file descriptor and remove only matching text content.
Ref<R> Os::remove_text(const String &p_path, const String &p_old) {
	GD_PERM_FAIL_V(READ, p_path, denied(p_path, "read", "remove"));
	GD_PERM_FAIL_V(WRITE, p_path, denied(p_path, "write", "remove"));
	{
		MutexLock guard(write_mutex());
		WriteLock lock;
		bool existed = false;
		const int taken = lock.take(p_path, false, existed);
		if (taken < 0) {
			return fail(p_path, "lock", lock.error(ERR_FILE_CANT_OPEN), vformat("cannot lock %s", p_path));
		}
		if (taken == 0 || !existed) {
			return fail_engine(p_path, "replace", ERR_ALREADY_EXISTS, vformat("%s changed while it was being edited", p_path));
		}
		String current;
		PackedByteArray raw;
		if (!lock.read(current, raw)) {
			return fail(p_path, "read", lock.error(ERR_FILE_CANT_READ), vformat("cannot read %s", p_path));
		}
		if (current != p_old) {
			return fail_engine(p_path, "replace", ERR_ALREADY_EXISTS, vformat("%s changed while it was being edited", p_path));
		}
	}
	return remove(p_path);
}

// Retain one lock across operations involving multiple files.
Ref<R> Os::lock_file(const String &p_path) {
	Ref<GDFileLock> lock;
	lock.instantiate();
	Ref<R> taken = lock->take(p_path);
	if (taken->get_e().is_valid()) {
		return taken;
	}
	return R::ok(lock);
}

// Replace file content with bytes.
Ref<R> Os::write_bytes(const String &p_path, const PackedByteArray &p_body) {
	SourceError::clear();
	Ref<GDFile> f = GDFile::open(p_path, GDFile::WRITE);
	if (f.is_null()) {
		return file_error(p_path, "write");
	}
	if (!f->store_buffer(p_body)) {
		return fail(p_path, "write", file_io_error(f, ERR_FILE_CANT_WRITE), vformat("cannot write %s", p_path));
	}
	const Error closed = f->close();
	return closed == OK ? R::ok() : fail(p_path, "close", closed, vformat("cannot close %s", p_path));
}

// Append bytes to the file.
Ref<R> Os::append_bytes(const String &p_path, const PackedByteArray &p_body) {
	SourceError::clear();
	Ref<GDFile> f = GDFile::open(p_path, GDFile::APPEND);
	if (f.is_null()) {
		return file_error(p_path, "write");
	}
	if (!f->store_buffer(p_body)) {
		return fail(p_path, "write", file_io_error(f, ERR_FILE_CANT_WRITE), vformat("cannot write %s", p_path));
	}
	const Error closed = f->close();
	return closed == OK ? R::ok() : fail(p_path, "close", closed, vformat("cannot close %s", p_path));
}

// Append text to the file.
Ref<R> Os::append_text(const String &p_path, const String &p_body) {
	SourceError::clear();
	Ref<GDFile> f = GDFile::open(p_path, GDFile::APPEND);
	if (f.is_null()) {
		return file_error(p_path, "write");
	}
	if (!f->store_string(p_body)) {
		return fail(p_path, "write", file_io_error(f, ERR_FILE_CANT_WRITE), vformat("cannot write %s", p_path));
	}
	const Error closed = f->close();
	return closed == OK ? R::ok() : fail(p_path, "close", closed, vformat("cannot close %s", p_path));
}

// Check whether the path exists.
bool Os::exists(const String &p_path) {
	// Require read permission because existence also reveals filesystem information.
	GD_PERM_FAIL_V(READ, p_path, false);
	return GDFile::exists(p_path) || GDDir::dir_exists_absolute(p_path);
}

// Remove the specified object.
Ref<R> Os::remove(const String &p_path) {
	GD_PERM_FAIL_V(WRITE, p_path, denied(p_path, "write", "remove"));
	SourceError::clear();
	Ref<GDDir> d = GDDir::open(p_path.get_base_dir());
	if (d.is_null()) {
		return dir_error(p_path, "write");
	}
	const Error err = d->remove(p_path.get_file());
	if (err != OK) {
		return fail(p_path, "remove", err, vformat("cannot remove %s", p_path));
	}
	return R::ok();
}

// Return the file size in bytes.
Ref<R> Os::size_of(const String &p_path) {
	SourceError::clear();
	Ref<GDFile> f = GDFile::open(p_path, GDFile::READ);
	if (f.is_null()) {
		return file_error(p_path, "read");
	}
	const uint64_t size = f->get_length();
	return f->get_error() == OK ? R::ok((int64_t)size) : fail(p_path, "stat", f->get_error(), vformat("cannot inspect %s", p_path));
}

// Reject copying onto the same file and transfer through EOF with the copy buffer.
Ref<R> Os::copy(const String &p_src, const String &p_dst) {
	SourceError::clear();
	Ref<GDFile> src = GDFile::open(p_src, GDFile::READ);
	if (src.is_null()) return file_error(p_src, "read")->note("copy failed");
	Ref<GDFile> dst = GDFile::open(p_dst, GDFile::CREATE);
	if (dst.is_null()) return file_error(p_dst, "write")->note("copy failed");
	if (src->same(dst)) return fail_engine(p_src, "copy", ERR_INVALID_PARAMETER, "copy source and destination must differ");
	if (src->get_error() != OK) return fail(p_src, "stat", src->get_error(), "cannot inspect copy source");
	if (dst->resize(0) != OK) return fail(p_dst, "truncate", dst->get_error(), "cannot truncate copy destination");
	PackedByteArray chunk;
	if (chunk.resize(COPY_CHUNK) != OK) return fail_engine(p_src, "copy", ERR_OUT_OF_MEMORY, "cannot allocate copy buffer");
	for (;;) {
		const uint64_t got = src->get_buffer(chunk.ptrw(), COPY_CHUNK);
		const Error error = src->get_error();
		if (got && !dst->store_buffer(chunk.ptr(), got)) return fail(p_dst, "write", dst->get_error(), "copy failed while writing");
		if (error == ERR_FILE_EOF) break;
		if (error != OK || !got) return fail(p_src, "read", error == OK ? ERR_FILE_CANT_READ : error, "copy failed while reading");
	}
	const Error closed = dst->close();
	return closed == OK ? R::ok() : fail(p_dst, "close", closed, "cannot close copy destination");
}

// Move a file or directory to the destination.
Ref<R> Os::rename(const String &p_src, const String &p_dst) {
	GD_PERM_FAIL_V(WRITE, p_src, link_denied(p_src, p_dst, p_src));
	GD_PERM_FAIL_V(WRITE, p_dst, link_denied(p_src, p_dst, p_dst));
	// Classify this operation's error rather than consulting open-error state.
	SourceError::clear();
	Ref<GDDir> dir = GDDir::create();
	const Error err = dir.is_valid() ? dir->rename(p_src, p_dst) : ERR_UNAVAILABLE;
	if (err != OK) {
		return link_fail(p_src, p_dst, "rename", err, vformat("cannot rename %s", p_src));
	}
	return R::ok();
}

// ---------------- Directories ----------------

Ref<R> Os::list_dir(const String &p_path) {
	SourceError::clear();
	Ref<GDDir> d = GDDir::open(p_path);
	if (d.is_null()) {
		return dir_error(p_path, "read");
	}
	const Error began = d->list_dir_begin();
	if (began != OK) {
		return fail(p_path, "readdir", began, vformat("cannot list %s", p_path));
	}
	PackedStringArray names;
	String name = next_of(d, false);
	while (!name.is_empty()) {
		if (!name.begins_with(".")) {
			names.push_back(name);
		}
		name = next_of(d, false);
	}
	d->list_dir_end();
	if (SourceError::kept()) {
		return R::err(fail(p_path, "readdir", FAILED, vformat("cannot list %s", p_path))->get_e(), Err::NONE, names);
	}
	return R::ok(names);
}

// Create one directory.
Ref<R> Os::make_dir(const String &p_path) {
	// Check the caller's path spelling; mount resolution determines the native target.
	GD_PERM_FAIL_V(WRITE, p_path, denied(p_path, "write", "mkdir"));
	// Classify this operation's error rather than consulting open-error state.
	SourceError::clear();
	Ref<GDDir> dir = GDDir::create();
	const Error err = dir.is_valid() ? make_dirs(dir, p_path) : ERR_UNAVAILABLE;
	if (err != OK) {
		return fail(p_path, "mkdir", err, vformat("cannot make directory %s", p_path));
	}
	return R::ok();
}

// Create a directory together with missing parents.
Ref<R> Os::ensure_dir(const String &p_path) {
	return GDDir::dir_exists_absolute(p_path) ? R::ok() : make_dir(p_path);
}

// ---------------- Permissions ----------------

bool Os::can_read(const String &p_path) {
	Ref<GDFile> f = GDFile::open(p_path, GDFile::READ);
	if (f.is_null()) {
		return GDFile::get_open_error() != ERR_UNAUTHORIZED;
	}
	return true;
}
