/**************************************************************************/
/*  rows.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Read remote SQL and embedded SQL results one row at a time while retaining the connection.

#include "cli/sys/std.h"

#include "core/os/mutex.h"
#include "core/templates/safe_refcount.h"

struct sqlite3_stmt;
class GDDatabaseClient;
class GDPostgresCallInternal;
class GDSQLiteDB;

// Advance with Next and read only the current row with Scan or Values.
class GDDatabaseRows : public RefCounted {
	GDCLASS(GDDatabaseRows, RefCounted);

	Ref<GDPostgresCallInternal> pg; // Query retained until the remote SQL protocol boundary.
	Ref<GDDatabaseClient> owner; // Client retaining the dedicated embedded SQL worker and connection.
	Ref<GDSQLiteDB> sqlite; // Connection that created the embedded SQL statement.
	sqlite3_stmt *stmt = nullptr; // Statement advanced only by the embedded SQL worker.
	mutable Mutex mutex; // Serialize worker result publication and main-thread reads.
	PackedStringArray names; // Current result column names.
	Array current; // Current-row values in column order.
	Ref<Err> failed; // Error distinguishing failure from EOF.
	String tag; // Backend completion tag.
	SafeFlag stopped; // Notify the embedded SQL progress handler of explicit closure.
	int64_t row_count = 0; // Rows read, used for completion tags and validation.
	uint64_t tx_generation = 0; // Generation distinguishing the owning transaction from other completions.
	bool current_valid = false; // Whether a current row is available for Scan.
	bool row_ready = false; // Whether a prefetched remote SQL row is retained.
	bool next_pending = false; // Whether the single permitted Next operation is pending.
	bool worker_ready = false; // Whether an embedded SQL worker result is ready for main-thread delivery.
	bool worker_value = false; // Next result produced by the embedded SQL worker.
	bool ended = false; // Whether EOF or failure has been reached.
	bool closed = false; // Whether the caller has closed Rows.

	// Deliver a pending Next result through a main-thread signal.
	void deliver(bool p_value);
	// Transfer embedded SQL worker results to the main thread.
	void step();
	// Attach sequential reading to a remote SQL query.
	void attach_postgres(const Ref<GDPostgresCallInternal> &p_call);
	// Receive remote SQL column names.
	void postgres_columns(const PackedStringArray &p_names);
	// Receive one remote SQL row.
	void postgres_row(const Array &p_values);
	// Receive remote SQL EOF or failure.
	void postgres_done(const Ref<Err> &p_error, const String &p_tag);
	// Detach references before reusing a remote SQL query.
	void postgres_detach(GDPostgresCallInternal *p_call);
	// Register an embedded SQL statement on its worker.
	void sqlite_opened(sqlite3_stmt *p_stmt, const PackedStringArray &p_names);
	// Publish one row or termination from the embedded SQL worker.
	void sqlite_result(const Array &p_values, bool p_done, const Ref<Err> &p_error, const String &p_tag);
	// Close embedded SQL Rows when their connection ends.
	void sqlite_shutdown(const Ref<Err> &p_error);
	// Remove Rows from transaction tracking.
	void release_transaction();

	friend class GDDatabaseClient;
	friend class GDPostgresCallInternal;
	friend class GDPostgresClient;
	friend class GDPostgresPoolCall;
	friend class GDSQLiteDB;

protected:
	// Expose Rows methods and signals to script.
	static void _bind_methods();

public:
	// Close remaining backend resources.
	~GDDatabaseRows();
	// Advance one row; return false on EOF or failure.
	Signal next();
	// Copy the current row into a column-name dictionary.
	Ref<R> scan() const;
	// Return current-row values in column order.
	Ref<R> values() const;
	// Return result column names.
	PackedStringArray columns() const;
	// Return the reason Next returned false, or null for EOF.
	Ref<Err> err() const;
	// Return the backend completion tag.
	String command_tag() const;
	// Close Rows without reading the remainder.
	void close();
	// Close Rows while preserving context cancellation as an Err.
	void cancel();
	// Check whether Rows reached EOF, failure, or explicit closure.
	bool is_closed() const;
};
