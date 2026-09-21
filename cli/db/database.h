/**************************************************************************/
/*  database.h                                                            */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Provide shared remote SQL and embedded SQL CRUD operations through one client API.

#include "cli/db/pg.h"
#include "cli/db/redis.h"
#include "cli/db/rows.h"
#include "cli/db/sqlite.h"
#include "cli/sys/task.h"

#include "core/object/object.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/safe_refcount.h"

class GDDatabaseClient;
class GDDatabaseTxCall;

// Create remote SQL connections and pools through one entry point.
class GDPostgresAPI : public Object {
	GDCLASS(GDPostgresAPI, Object);

protected:
	// Expose remote SQL APIs to script.
	static void _bind_methods();

public:
	// Create a remote SQL client.
	Ref<GDPostgresClient> client() const;
	// Create a remote SQL connection pool.
	Ref<GDPostgresPool> pool(int64_t p_size) const;
};

// Create key-value connections and pools through one entry point.
class GDRedisAPI : public Object {
	GDCLASS(GDRedisAPI, Object);

protected:
	// Expose key-value APIs to script.
	static void _bind_methods();

public:
	// Create a key-value client.
	Ref<GDRedisClient> client() const;
	// Create a key-value connection pool.
	Ref<GDRedisPool> pool(int64_t p_size) const;
};

// Open synchronous embedded databases through one backend entry point.
class GDSQLiteAPI : public Object {
	GDCLASS(GDSQLiteAPI, Object);

protected:
	// Expose embedded database APIs to script.
	static void _bind_methods();

public:
	// Open a mounted embedded database.
	Ref<R> open(const String &p_path, const Dictionary &p_opts) const { return GDSQLiteDB::open(p_path, p_opts); }
};

// Query the same connection only within its transaction.
class GDDatabaseTx : public RefCounted {
	GDCLASS(GDDatabaseTx, RefCounted);

	Ref<GDDatabaseTxCall> call; // Operation retaining transaction state and connection ownership.

	friend class GDDatabaseClient;
	friend class GDDatabaseTxCall;

protected:
	// Expose methods permitted inside a transaction to script.
	static void _bind_methods();

public:
	// Execute SQL only while the transaction is valid.
	Signal query(const String &p_sql, const Array &p_args);
	// Return only the first row within a transaction.
	Signal query_row(const String &p_sql, const Array &p_args);
	// Return sequential Rows within a transaction.
	Signal query_rows(const String &p_sql, const Array &p_args);
	// Return the completion signal without waiting, for composition.
	Signal query_async(const String &p_sql, const Array &p_args) { return query(p_sql, p_args); }
	// Return query_row's completion signal without waiting, for composition.
	Signal query_row_async(const String &p_sql, const Array &p_args) { return query_row(p_sql, p_args); }
	// Return query_rows' completion signal without waiting, for composition.
	Signal query_rows_async(const String &p_sql, const Array &p_args) { return query_rows(p_sql, p_args); }
	// Check whether the transaction remains usable.
	bool is_active() const;
};

// Advance asynchronously from BEGIN through COMMIT or ROLLBACK.
class GDDatabaseTxCall : public RefCounted {
	GDCLASS(GDDatabaseTxCall, RefCounted);

	enum Mode {
		ACTION, // Execute a callback.
		MIGRATION, // Execute statements sequentially.
	};

	Ref<GDDatabaseTxCall> self_hold; // Keep this operation alive until completion is delivered.
	Ref<GDDatabaseClient> owner; // Client owning the transaction's physical connection.
	Ref<GDDatabaseTx> tx; // Dedicated client passed to the callback.
	Callable action; // Callback executed inside the transaction.
	Array statements; // SQL statements applied sequentially by a migration.
	Ref<R> outcome; // Callback result or failed-statement result.
	Ref<R> pending; // Closure or cancellation result delivered on the next runtime turn.
	uint64_t generation = 0; // Generation distinguishing this transaction from others.
	int at = 0; // Index of the next statement.
	Mode mode = ACTION;
	bool finished = false; // Prevent duplicate completion.
	bool ending = false; // Reject public SQL once transaction termination begins.
	bool committing = false; // Whether COMMIT was sent last.
	bool close_after_end = false; // Close the connection after receiving the COMMIT result.

	// Proceed from BEGIN to the callback or migration.
	void on_begin(const Ref<R> &p_result);
	// Acquire a dedicated pool connection and proceed to BEGIN.
	void on_acquire(const Ref<R> &p_result);
	// Pass the dedicated transaction client as the callback's ordinary argument.
	Variant call_action();
	// Choose COMMIT or ROLLBACK from the callback result.
	void on_action(const Variant &p_result);
	// Send the next migration statement.
	void next_statement();
	// Proceed to the next statement or ROLLBACK from the statement result.
	void on_statement(const Ref<R> &p_result);
	// Send COMMIT or ROLLBACK.
	void end(bool p_commit);
	// Convert transaction termination into the final outcome.
	void on_end(const Ref<R> &p_result);
	// Deliver the final result exactly once.
	void done(const Ref<R> &p_result, bool p_close);
	// Report rollback caused by owner closure as completion.
	void owner_closed();
	// Defer closure until the result is known after sending COMMIT.
	bool defer_close();
	// Deliver closure or cancellation after the waiter connects.
	void step();

	friend class GDDatabaseClient;
	friend class GDDatabaseTx;

protected:
	// Expose cancellation and completion signals to script.
	static void _bind_methods();

public:
	// Close the connection and roll back when the waiter cancels.
	void cancel();
	// Accept SQL from the transaction client.
	Signal query(const String &p_sql, const Array &p_args);
	// Accept a first-row request from the transaction client.
	Signal query_row(const String &p_sql, const Array &p_args);
	// Accept a sequential Rows request from the transaction client.
	Signal query_rows(const String &p_sql, const Array &p_args);
	// Check whether the transaction remains usable.
	bool is_active() const;
};

// Return embedded SQL worker results through main-thread signals.
class GDDatabaseCall : public RefCounted {
	GDCLASS(GDDatabaseCall, RefCounted);

	Ref<GDDatabaseCall> self_hold; // Keep this operation alive until its result is delivered.
	Ref<GDDatabaseClient> owner; // Retain the client used by the worker.
	Ref<GDSQLiteDB> sqlite; // embedded SQL connection used by the worker.
	String sql; // SQL passed to the worker.
	Array args; // SQL bind parameters.
	int64_t queued_bytes = 0; // Estimated bytes occupied in the embedded SQL queue.
	uint64_t queued_at = 0; // Time the connection-worker wait began.
	bool counted_wait = false; // Whether the wait count includes this operation.
	bool wait_recorded = false; // Whether the wait duration has been accumulated.
	bool one = false; // Whether the query returns only the first row.
	bool rows_open = false; // Whether the query prepares sequential embedded SQL Rows.
	Ref<GDDatabaseRows> rows; // Sequential Rows attached to a statement on the worker.
	Ref<R> result; // Result transferred from worker to main thread.
	SafeFlag ready; // Whether the main thread can read result.
	SafeFlag completed; // Accept only the first completion.
	SafeFlag canceled; // Notify the worker of per-operation cancellation.

	// Publish embedded SQL worker results safely.
	void complete(const Ref<R> &p_result);
	// Emit completed results on the main thread.
	void step();
	// Release the self-reference without awaiting a signal at process shutdown.
	void finish_shutdown();

	friend class GDDatabaseClient;

protected:
	// Register the embedded SQL operation completion signal.
	static void _bind_methods();

public:
	// Stop opening or only this embedded SQL operation when its context ends.
	void cancel();
};

// Provide a shared database client whose driver is selected at open time.
class GDDatabaseClient : public RefCounted {
	GDCLASS(GDDatabaseClient, RefCounted);

	Ref<GDPostgresClient> postgres; // Connection for the remote SQL driver.
	Ref<GDPostgresPool> postgres_pool; // Pool for multiple remote SQL connections.
	Ref<GDPostgresClient> tx_postgres; // Connection pinned from the pool during a transaction.
	Ref<GDPostgresPoolCall> tx_lease; // Lease retaining the connection until the transaction ends.
	Ref<GDSQLiteDB> sqlite; // Connection for the embedded SQL driver.
	Thread sqlite_thread; // Worker isolating embedded SQL operations from the main thread.
	Mutex sqlite_mutex; // Protect the embedded SQL operation queue.
	Semaphore sqlite_ready; // Notify the worker of new operations.
	Semaphore sqlite_consumed; // Notify the worker that the main thread delivered a result.
	List<Ref<GDDatabaseCall>> sqlite_jobs; // embedded SQL operations in arrival order.
	Ref<GDDatabaseCall> sqlite_active; // embedded SQL operation currently executing on the worker.
	Ref<GDDatabaseRows> sqlite_rows; // Rows retaining the embedded SQL connection until closed.
	Ref<GDDatabaseRows> sqlite_rows_job; // Rows advanced next by the worker.
	bool sqlite_rows_close = false; // Whether the next Rows job closes its statement.
	int64_t sqlite_queue_bytes = 0; // Estimated bytes of queued and active embedded SQL operations.
	SafeFlag sqlite_stop; // Request worker termination.
	SafeFlag sqlite_waiting_result; // Whether the worker awaits main-thread result consumption.
	bool sqlite_closing = false; // Whether an I/O worker is waiting for embedded SQL worker shutdown.
	bool destroying = false; // Force synchronous worker cleanup only during destruction.
	int max_rows = 0; // Explicit result-row limit; zero is unlimited.
	int max_bytes = 0; // Explicit result-byte limit; zero is unlimited.
	bool opening = false; // Whether a remote SQL open result is pending.
	uint64_t open_generation = 0; // Generation used to ignore stale remote SQL open results.
	Ref<GDDatabaseTxCall> tx_call; // Transaction occupying the connection.
	HashSet<GDDatabaseRows *> tx_rows; // Sequential Rows closed before transaction termination.
	uint64_t tx_generation = 0; // Generation used to validate transaction clients.
	bool tx_active = false; // Exclude ordinary queries while a transaction is active.
	int64_t sqlite_wait_count = 0; // Operations that waited for their connection-worker turn.
	int64_t sqlite_wait_ms = 0; // Cumulative connection-worker wait in milliseconds.

	// Execute embedded SQL operations in arrival order.
	void sqlite_loop();
	// Accumulate each operation's wait time once under sqlite_mutex.
	void record_sqlite_wait(GDDatabaseCall *p_call);
	// Enter the embedded SQL worker through the thread's C callback.
	static void sqlite_entry(void *p_self);
	// Receive remote SQL open results and restore reusability on failure.
	void postgres_opened(const Ref<R> &p_result, const Ref<GDDatabaseCall> &p_call,
			const Ref<GDPostgresClient> &p_expected, uint64_t p_generation);
	// Forward remote SQL pool configuration results to the shared open operation.
	void postgres_pool_opened(const Ref<R> &p_result, const Ref<GDDatabaseCall> &p_call,
			const Ref<GDPostgresPool> &p_expected, uint64_t p_generation);
	// Adopt a worker-opened embedded SQL connection and start its query worker.
	void sqlite_opened(const Ref<R> &p_result, const Ref<GDDatabaseCall> &p_call, uint64_t p_generation);
	// Close the embedded SQL worker and connection safely.
	void close_sqlite();
	// Receive completion of the I/O worker's embedded SQL shutdown wait.
	void sqlite_closed(const Ref<R> &p_result);
	// Deliver embedded SQL results on the main thread and let the worker proceed.
	void sqlite_finished(GDDatabaseCall *p_call, const Ref<GDSQLiteDB> &p_sqlite, int64_t p_queued_bytes);
	// Stop opening or embedded SQL operations whose contexts ended.
	void cancel_call(GDDatabaseCall *p_call);
	// Execute shared SQL without checking transaction occupancy.
	Signal query_inner(const String &p_sql, const Array &p_args, bool p_one = false);
	// Open sequential Rows without checking transaction occupancy.
	Signal query_rows_inner(const String &p_sql, const Array &p_args);
	// Send SQL through the transaction's dedicated connection.
	Signal query_tx(uint64_t p_generation, const String &p_sql, const Array &p_args, bool p_one = false);
	// Open sequential Rows through the transaction's dedicated connection.
	Signal query_tx_rows(uint64_t p_generation, const String &p_sql, const Array &p_args);
	// Dispatch embedded SQL Rows Next or Close to the dedicated worker.
	bool request_sqlite_rows(GDDatabaseRows *p_rows, bool p_close);
	// Track transaction Rows for cleanup at transaction end.
	void track_tx_rows(const Ref<GDDatabaseRows> &p_rows);
	// Remove closed Rows from transaction cleanup tracking.
	void untrack_tx_rows(GDDatabaseRows *p_rows, uint64_t p_generation);
	// Close Rows from the same generation before COMMIT or ROLLBACK.
	void close_tx_rows(uint64_t p_generation);
	// Release connection ownership only for the current transaction generation.
	void finish_tx(uint64_t p_generation);
	// Start a callback or statement sequence as a transaction.
	Signal start_tx(const Callable &p_action, const Array &p_statements, bool p_migration);

	friend class GDDatabaseCall;
	friend class GDDatabaseRows;
	friend class GDDatabaseTxCall;

protected:
	// Expose shared client methods to script.
	static void _bind_methods();

public:
	GDDatabaseClient();
	// Close the worker and connection on destruction.
	~GDDatabaseClient();
	// Close all live clients and dedicated workers at process shutdown.
	static void shutdown_all();
	// Select the driver and connection options, then open the required connections.
	Signal open(const Dictionary &p_opts);
	// Return open's completion signal without waiting, for composition.
	Signal open_async(const Dictionary &p_opts) { return open(p_opts); }
	// Execute shared SQL with bound parameters.
	Signal query(const String &p_sql, const Array &p_args);
	// Return only the first shared-query row.
	Signal query_row(const String &p_sql, const Array &p_args);
	// Return sequential Rows that advance one row per Next call.
	Signal query_rows(const String &p_sql, const Array &p_args);
	// Return query's completion signal without waiting, for composition.
	Signal query_async(const String &p_sql, const Array &p_args) { return query(p_sql, p_args); }
	// Return query_row's completion signal without waiting, for composition.
	Signal query_row_async(const String &p_sql, const Array &p_args) { return query_row(p_sql, p_args); }
	// Return query_rows' completion signal without waiting, for composition.
	Signal query_rows_async(const String &p_sql, const Array &p_args) { return query_rows(p_sql, p_args); }
	// Execute a callback inside a transaction on the same connection.
	Signal transaction(const Callable &p_action);
	// Return transaction's completion signal without waiting, for composition.
	Signal transaction_async(const Callable &p_action) { return transaction(p_action); }
	// Apply SQL statements sequentially in one transaction.
	Signal migrate(const Array &p_statements);
	// Return migration's completion signal without waiting, for composition.
	Signal migrate_async(const Array &p_statements) { return migrate(p_statements); }
	// Close the current connection.
	void close();
	// Check whether the selected driver is connected.
	bool is_open() const;
	// Return connection-pool state and cumulative waiting statistics.
	Dictionary stats() const;
};

// Create database clients with selectable drivers.
class GDDatabaseAPI : public Object {
	GDCLASS(GDDatabaseAPI, Object);

	GDPostgresAPI *postgres = nullptr;
	GDRedisAPI *redis = nullptr;
	GDSQLiteAPI *sqlite = nullptr;

protected:
	// Expose the shared database entry point to script.
	static void _bind_methods();

public:
	// Create backend-specific entry points.
	GDDatabaseAPI();
	// Destroy backend-specific entry points.
	~GDDatabaseAPI();
	// Create an unconnected shared client.
	Ref<GDDatabaseClient> client() const;
	// Return the embedded database entry point.
	GDSQLiteAPI *get_sqlite() const { return sqlite; }
	// Return the remote SQL entry point.
	GDPostgresAPI *get_postgres() const { return postgres; }
	// Return the key-value entry point.
	GDRedisAPI *get_redis() const { return redis; }
};
