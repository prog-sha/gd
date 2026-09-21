/**************************************************************************/
/*  sqlite.h                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Expose an embedded SQL connection and permission-aware factory to script.

#include "cli/sys/std.h"

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

struct sqlite3;
struct sqlite3_stmt;
class GDDatabaseClient;
class GDDatabaseRows;
class GDSQLiteStatement;

// Retain a cached prepared statement and its estimated byte footprint.
struct SQLiteCachedStmt {
	sqlite3_stmt *stmt = nullptr; // embedded SQL execution program.
	int64_t bytes = 0; // Estimated bytes of the SQL key and execution program.
};

class GDSQLiteDB : public RefCounted {
	GDCLASS(GDSQLiteDB, RefCounted);

	// Select result shape and write permissions for each API entry point.
	enum RunMode {
		RUN_EXEC, // Return affected rows and inserted ID.
		RUN_QUERY, // Return read-only result rows.
		RUN_GET, // Return only the first read-only row.
		RUN_PORTABLE, // Return the shared columns, rows, and tag result shape.
		RUN_PORTABLE_ONE, // Return only the first row through the shared API.
	};

	sqlite3 *db = nullptr; // Currently open connection.
	int max_rows = 0; // Explicit result-row limit; zero is unlimited.
	int max_bytes = 0; // Explicit result-byte limit; zero is unlimited.
	int max_ms = 0; // Maximum execution milliseconds per operation; zero disables interruption.
	uint64_t due = 0; // Deadline for interrupting the active operation.
	const SafeFlag *stop_flag = nullptr; // Cancellation flag for the shared client's active call.
	HashMap<String, SQLiteCachedStmt> stmts; // Prepared-statement cache avoiding repeated SQL parsing.
	int64_t stmt_bytes = 0; // Total estimated bytes of cached SQL keys and execution programs.
	HashSet<GDSQLiteStatement *> prepared; // Explicitly retained prepared statements.
	mutable Mutex mutex; // Serialize operations and closure on one connection.
	uint64_t pinned_id = 0; // Identifier for the parent directory and main database pinned by the Unix VFS.

	friend class GDDatabaseClient;
	friend class GDDatabaseRows;
	friend class GDSQLiteStatement;

	// Execute SQL using a cached or newly prepared program.
	Ref<R> run(const String &p_sql, const Array &p_params, RunMode p_mode, const SafeFlag *p_stop = nullptr);
	// Bind and execute a prepared program.
	Ref<R> execute(sqlite3_stmt *p_stmt, const Array &p_params, RunMode p_mode);
	// Execute an explicit prepared statement under the connection lock.
	Ref<R> run_prepared(GDSQLiteStatement *p_owner, const Array &p_params, RunMode p_mode);
	// Bind multiple argument sets to the same prepared statement sequentially.
	Ref<R> run_many_prepared(GDSQLiteStatement *p_owner, const Array &p_rows);
	// Detach and release an explicit prepared statement.
	void drop_prepared(GDSQLiteStatement *p_stmt);
	// Check whether an explicit prepared statement is usable on this connection.
	bool has_prepared(const GDSQLiteStatement *p_stmt) const;
	// Interrupt the embedded SQL VM at its deadline.
	static int stop(void *p_self);
	// Release all cached prepared statements.
	void clear_stmts();
	// Interrupt an operation on another thread for closure.
	void interrupt();
	// Prepare and retain a statement for shared Rows.
	Ref<R> open_rows(GDDatabaseRows *p_rows, const String &p_sql, const Array &p_params, const SafeFlag *p_stop);
	// Advance shared Rows by one row; return true at termination.
	bool step_rows(GDDatabaseRows *p_rows);
	// Release a shared Rows statement on its worker.
	void close_rows(GDDatabaseRows *p_rows);

protected:
	// Expose the embedded SQL connection to script.
	static void _bind_methods();

public:
	// Clean up an unclosed connection on destruction.
	~GDSQLiteDB();
	// Open an embedded SQL file through mount and permission checks.
	static Ref<R> open(const String &p_path, const Dictionary &p_opts);
	// Bind values and execute one SQL statement.
	Ref<R> exec(const String &p_sql, const Array &p_params) { return run(p_sql, p_params, RUN_EXEC); }
	// Bind values and return read-only SQL rows.
	Ref<R> query(const String &p_sql, const Array &p_params) { return run(p_sql, p_params, RUN_QUERY); }
	// Create a prepared statement that parses SQL once.
	Ref<R> prepare(const String &p_sql);
	// Bind $N parameters and return the shared database result shape.
	Ref<R> portable_query(const String &p_sql, const Array &p_params, const SafeFlag *p_stop = nullptr) { return run(p_sql, p_params, RUN_PORTABLE, p_stop); }
	// Bind $N parameters and return only the first shared-query row.
	Ref<R> portable_row(const String &p_sql, const Array &p_params, const SafeFlag *p_stop = nullptr) { return run(p_sql, p_params, RUN_PORTABLE_ONE, p_stop); }
	// Close the connection.
	void close();
	// Check whether the connection is usable.
	bool is_open() const;
};

// Execute the same prepared SQL repeatedly without reparsing.
class GDSQLiteStatement : public RefCounted {
	GDCLASS(GDSQLiteStatement, RefCounted);

	Ref<GDSQLiteDB> owner; // Retain the originating connection throughout statement lifetime.
	sqlite3_stmt *stmt = nullptr; // Reusable embedded SQL execution program.

	friend class GDSQLiteDB;

protected:
	// Expose prepared-statement methods to script.
	static void _bind_methods();

public:
	// Release the embedded SQL execution program on destruction.
	~GDSQLiteStatement();
	// Execute a write statement and return affected rows.
	Ref<R> run(const Array &p_params);
	// Execute a write statement for multiple argument sets.
	Ref<R> run_many(const Array &p_rows);
	// Return the first read-only query row.
	Ref<R> one(const Array &p_params);
	// Return all read-only query rows.
	Ref<R> all(const Array &p_params);
	// Release the prepared statement explicitly.
	void close();
	// Check whether both statement and connection are usable.
	bool is_valid() const;
};
