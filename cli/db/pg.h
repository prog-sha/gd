/**************************************************************************/
/*  pg.h                                                                  */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Provide a remote SQL connection using frontend/backend protocol version 3.
// Support parameter binding, statement reuse, pipelining, and connection pooling.
//
// Send values separately with extended queries using Parse, Bind, and Execute.
// Keep values out of SQL text to prevent injection through interpolation.
// Retain named prepared statements to avoid parsing identical SQL repeatedly.
// Pipeline queries without awaiting each reply to reduce round trips.
// Lend pooled connections through GDPostgresPool.
//
// Support cleartext, MD5, and SCRAM-SHA-256 authentication.
//
// Scripts can call `await db.query("select $1", [1])`.
// Return R containing a dictionary with columns, rows, and tag.

#include "cli/data/bytes.h"
#include "cli/sys/pool.h"
#include "cli/sys/std.h"

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/rb_set.h"

class GDPostgresClient;
class GDPostgresCallInternal;
class GDPostgresPoolCall;
class GDDatabaseRows;

// Encode remote SQL queries into wire bytes on CPU workers.
// Preserve order within each connection while encoding across connections concurrently.
class GDPostgresPackJob : public PoolJob {
	GDCLASS(GDPostgresPackJob, PoolJob);

	Ref<GDPostgresClient> db; // Retain the connection until completion.
	Ref<GDPostgresCallInternal> call; // Query operation receiving the result.
	String sql; // SQL text placed in the Parse message.
	String stmt; // Server-side statement name.
	Array rows; // Argument snapshot read only by the worker.
	LocalVector<uint8_t> fmts; // Snapshot of per-column result formats.
	ByteBuf packed; // Outbound bytes produced by the worker.
	Ref<R> error; // Validation or encoding failure.
	int64_t queued_bytes = 0; // Conservative byte footprint in the connection queue.
	int64_t work_bytes = 0; // Estimated encoding bytes used to select main-thread processing.
	uint64_t due = 0; // Encoding deadline.
	bool is_new = false; // Whether to include a Parse message.
	bool ask_desc = false; // Whether to request a row description.
	bool check_only = false; // Whether to encode a syntax-only check.
	bool started = false; // Prevent resubmitting the same first job during reentrancy.
	bool flush_now = false; // Whether to attempt sending immediately after acquisition.

	friend class GDPostgresClient;

protected:
	static void _bind_methods() {}
	void run() override;
	void finish() override;
};

// Represent one query and emit finished when its result is complete.
class GDPostgresCallInternal : public RefCounted {
	GDCLASS(GDPostgresCallInternal, RefCounted);

	// Track the pending phase.
	enum Mode {
		RESOLVING, // Resolve the hostname on a worker.
		CONNECTING, // Wait for connection establishment and optional TLS setup.
		HANDSHAKE, // Perform authentication and initialize connection state.
		QUERYING, // Wait for complete query results.
	};

	Ref<GDPostgresClient> db;
	Ref<GDPostgresCallInternal> self_hold; // Remain alive until completion even without a script reference.
	Mode mode = QUERYING;
	uint64_t due = 0; // Operation deadline.
	Ref<R> pending; // Result delivered on the next scheduler turn.
	Ref<R> outcome; // Complete result emitted after removal from the queue.
	Dictionary opts; // Username, database name, and password.

	// Accumulate the current result.
	PackedStringArray columns;
	LocalVector<StringName> keys; // Prehashed column names used in row dictionaries.
	LocalVector<int32_t> oids; // Per-column type identifiers.
	LocalVector<uint8_t> fmts; // Per-column result format; one denotes binary.
	String sql; // SQL key identifying cached column metadata.
	String stmt; // Server-side statement name used by this query.
	bool dropped = false; // Discard canceled replies while retaining queue position.
	bool shared = false; // Preserve unrelated pooled queries when this caller expires.
	bool notified = false; // Whether the waiter has already received a result.
	Array rows;
	String tag;
	int max_rows = 0; // Nonzero returned-row limit.
	int64_t max_bytes = 0; // Nonzero returned-body byte limit.
	int64_t result_bytes = 0; // Bytes of rows received so far.
	int result_rows = 0; // Rows received so far.
	// Accumulate batch results separately for each query.
	Array batches;
	int want_n = 0; // Expected query count; zero means one query.
	int done_n = 0; // Completed query count in the batch.
	int sync_n = 0; // Queries drained through their individual Sync boundaries.
	bool discard_many = false; // Return only the completion count without building individual results.
	bool flat_many = false; // Flatten rows from all batch results into one array.
	bool values_only = false; // Return each row as a column-ordered array.
	bool flat_values = false; // Flatten every row's values into one array.
	bool first_only = false; // Retain and return only the first row.
	GDDatabaseRows *stream = nullptr; // Sequential Rows receiving only the current row.
	bool stream_mode = false; // Deliver rows individually without accumulating the full result.
	bool stream_paused = false; // Pause receive parsing until the caller requests the next row.
	bool cancel_deferred = false; // Whether cancellation delivery is scheduled for the next turn.
	bool cancel_finished = false; // Whether the underlying query finished before cancellation delivery.
	bool pending_posted = false; // Prevent duplicate pending-result completion entries.
	bool step_posted = false; // Prevent duplicate connection-processing ready entries.
	uint64_t generation = 1; // Generation identifying stale notifications after reuse.
	GDPostgresPoolCall *pool_call = nullptr; // Pool operation receiving results directly without an intermediate signal.
	Array values; // Values accumulated for flat output.
	Ref<Err> failed;

	void step();
	void schedule(); // Append connection processing to the ready queue.
	void dispatch(uint64_t p_generation); // Dispatch only notifications for the current generation.
	void deliver_pending(uint64_t p_generation); // Deliver pending results only to the matching operation generation.
	void deliver_cancel(uint64_t p_generation); // Deliver cancellation once after the waiter attaches.
	void notify(const Ref<R> &p_out); // Deliver the result once to either the direct waiter or pool operation.
	void interrupt(const Ref<R> &p_out); // Notify cancellation while retaining the protocol position.
	void done(const Ref<R> &p_out);
	void reset(); // Reset state for reuse.
	void finish(); // Emit the result after leaving the queue.
	// Defer immediate failures until the caller can begin waiting.
	void fail_later(const Ref<R> &p_out);
	void set_due(uint64_t p_wait); // Register the deadline with the kernel wait layer.
	// Process one message and return true on completion.
	bool take(char p_kind, const uint8_t *p_body, int p_len);
	// Resume receive parsing when Rows requests its next row.
	void resume_stream();
	// Drain to the protocol boundary without building values after early Rows closure.
	void close_stream();

	friend class GDPostgresClient;
	friend class GDPostgresPool;
	friend class GDPostgresPoolCall;
	friend class GDDatabaseRows;

public:
	void cancel(); // Report waiter cancellation and retain the operation until protocol synchronization.

protected:
	static void _bind_methods();
};

class GDPostgresPool;
class GDPostgresPoolCall;
class GDDatabaseClient;
class GDDatabaseTxCall;

// Order pool-wait deadlines by time.
struct GDPostgresPoolDeadline {
	uint64_t due = 0; // Absolute query deadline.
	GDPostgresPoolCall *call = nullptr; // Waiter identity distinguishing equal deadlines.

	bool operator<(const GDPostgresPoolDeadline &p_other) const {
		return due == p_other.due ? uintptr_t(call) < uintptr_t(p_other.call) : due < p_other.due;
	}
};

// Derive SCRAM keys on a worker thread.
// Keep peer-selected iteration work off the main thread so listeners remain responsive.
class ScramKeyJob : public PoolJob {
	GDCLASS(ScramKeyJob, PoolJob);

	Ref<GDPostgresClient> db; // Connection receiving the authentication continuation.
	PackedByteArray password; // Key-derivation input.
	PackedByteArray salt;
	int iters = 0; // Peer-selected iteration count.
	Ref<R> salted; // Derived key or its original failure.
	String combined; // Peer nonce used by the continuation.
	String server_first; // Peer's initial message used by the continuation.

	friend class GDPostgresClient;

protected:
	static void _bind_methods() {}
	virtual void run() override;
	virtual void finish() override;
};

class GDPostgresClient : public RefCounted {
	GDCLASS(GDPostgresClient, RefCounted);

	Wire sock; // Plain or TLS-protected connection.
	String host; // Destination hostname used for certificate identity checks.
	int port = 5432;
	Wire::Guard guard = Wire::NONE; // Protection mode selected from TLS options.
	Ref<GDTrust> ca; // Optional trust roots loaded on a worker.
	uint64_t wait_ms = 0; // Query timeout; zero waits until caller cancellation.
	// Track TLS negotiation from request through the peer's one-byte response to wrapping.
	enum Tls {
		TLS_OFF, // Do not use TLS.
		TLS_ASK, // TLS request has not been sent.
		TLS_WAIT, // Wait for the peer's one-byte TLS response.
		TLS_ON, // TLS is active.
	};
	Tls tls_at = TLS_OFF;
	PackedByteArray buf; // Partially consumed receive buffer.
	int buf_at = 0; // Read cursor advanced without deleting the prefix.
	bool ready = false;
	// Record malformed replies whose declared counts disagree with their contents.
	// Bounds checks prevent out-of-buffer reads, but incomplete shapes must also
	// be reported as failures rather than appearing to be valid responses.
	String torn;
	// Store type OIDs by column position to avoid a dictionary lookup per column.
	// Cache them per statement so repeated queries can omit descriptions.
	LocalVector<int32_t> oids;
	HashMap<String, LocalVector<int32_t>> oid_memo; // SQL mapped to its column type sequence.
	// Choose each column's result format from its type; binary numeric values
	// avoid decimal conversion and use their fixed-width wire representation.
	HashMap<String, LocalVector<uint8_t>> fmt_memo;

	// Retain prepared statements to avoid reparsing identical SQL.
	HashMap<String, String> stmts; // SQL mapped to its server-side statement name.
	List<String> stmt_lru; // Most recently used statements at the front.
	HashMap<String, List<String>::Element *> stmt_lru_pos; // SQL mapped to its LRU position.
	// Cache column names per statement to omit repeated Describe exchanges.
	// This avoids both server-side descriptions and client-side metadata parsing.
	HashMap<String, PackedStringArray> cols;
	HashMap<String, LocalVector<StringName>> key_memo; // SQL mapped to prehashed column names.
	int stmt_seq = 0; // Statement-name sequence number.

	// Complete queued replies in send order for pipelined operations.
	List<Ref<GDPostgresCallInternal>> inflight;
	List<Ref<GDPostgresPackJob>> packing; // Encoding queue preserving per-connection arrival order.
	bool packing_active = false; // Advance inline jobs iteratively across completion reentrancy.
	int64_t packing_bytes = 0; // Maximum wire bytes retained by pending encoding jobs.
	Ref<GDPostgresCallInternal> opening; // Operation performing DNS, connection setup, or authentication.
	bool watching = false; // Whether socket delivery is registered.
	bool pump_posted = false; // Prevent duplicate connection completion-queue entries.
	uint64_t wire_generation = 1; // Connection generation rejecting stale pumps after reopening.
	char tx_status = 'I'; // Transaction state reported by the last synchronization boundary.
	bool ready_state(const uint8_t *p_body, int p_len); // Validate and retain the server's transaction state.

	void pump(); // Distribute received replies in send order.
	void socket_ready(); // Run only when the kernel reports connection readiness.
	void post_pump(); // Schedule buffered continuation or fair yielding for the next turn.
	void dispatch_pump(); // Pump from the completion queue only when needed.
	void watch(bool p_on);
	void drain(const Ref<R> &p_why); // Deliver a failure reason to all waiters.
	// Send argument sets through the shared query and query_many implementation.
	Signal send_rows(const String &p_sql, const Array &p_rows, bool p_many, bool p_discard, bool p_flat = false, int p_max_rows = 0, int64_t p_max_bytes = 0, bool p_values = false, bool p_flat_values = false, bool p_first = false, bool p_flush_now = false);
	// Create a sequential Rows query that prefetches at most its first row.
	Ref<GDDatabaseRows> start_rows(const String &p_sql, const Array &p_args, bool p_flush_now = false);
	void queue_pack(const Ref<GDPostgresPackJob> &p_job); // Append to the connection's encoding queue.
	void start_pack(); // Pass statement-cache state to the first job and start it.
	void packed(GDPostgresPackJob *p_job); // Move completed bytes into the send queue in arrival order.
	// Prepare a statement or return its existing name.
	String prepare(const String &p_sql, bool &r_is_new);
	void close_stmt(const String &p_name); // Close a named server-side statement.
	void forget(const String &p_sql, bool p_close = false); // Discard cached metadata for a statement.
	// Return cached column names, or empty when absent.
	const PackedStringArray *known_cols(const String &p_sql) const;
	const LocalVector<StringName> *known_keys(const String &p_sql) const; // Return cached prehashed column names.
	// Return cached column types, or empty when absent.
	const LocalVector<int32_t> *known_oids(const String &p_sql) const;
	// Return cached result formats, or empty when absent.
	const LocalVector<uint8_t> *known_fmts(const String &p_sql) const;

	// Retain intermediate SCRAM-SHA-256 authentication state.
	String nonce; // Locally generated one-time nonce.
	String first_bare; // Initial client message used in the proof.
	String server_sig; // Expected server signature.
	// Track SCRAM progress to reject authentication success without a server proof.
	// AuthenticationOk is premature while the server signature is still pending.
	bool scram_pending = false;
	bool auth_ok = false; // Require AuthenticationOk before accepting connection readiness.
	bool scram_done = false; // Prevent switching away from SCRAM authentication.
	Ref<ScramKeyJob> scram_job; // Retain the key-derivation job while it runs on a worker.
	bool md5_done = false; // Prevent switching away from MD5 authentication.

	Ref<R> fill(int p_max_bytes); // Read socket bytes up to the requested amount.
	// Extract a complete message by buffer position without copying its body.
	bool next_msg(char &r_kind, int &r_at, int &r_len);
	// Return the next message's full byte count without advancing the cursor.
	int64_t next_msg_size() const;
	bool has_complete_msg() const; // Check whether the next message is complete in the receive buffer.
	Error send(const String &p_kind, const ByteBuf &p_body);
	// Build all messages for a query directly in one buffer for a single send.
	// Separate writes would add a system-call cost for each protocol message.
	// Reuse completed operation objects to avoid repeated ObjectDB registration.
	// Batch queries benefit from retaining these objects across requests.
	LocalVector<Ref<GDPostgresCallInternal>> spare;
	Ref<GDPostgresCallInternal> lend(); // Borrow an operation or create one when none is available.
	void give_back(GDPostgresCallInternal *p_call); // Return a completed operation for reuse.
	// Batch queries from the same turn into one write to reduce socket calls
	// when many clients submit requests concurrently.
	ByteBuf out_buf;
	ByteBuf next_buf; // Next bytes completed by a worker during the previous send.
	bool flush_queued = false;
	void flush_out(); // Send buffered queries.
	// Continue connection setup after worker-based hostname resolution.
	void resolved(const Ref<R> &p_result, const Ref<GDPostgresCallInternal> &p_call);

	Ref<R> sasl_begin(const PackedByteArray &p_data);
	Ref<R> sasl_continue(const PackedByteArray &p_data, const String &p_password);
	// Build the authentication continuation after worker key derivation completes.
	void sasl_derived(const Ref<R> &p_salted, const String &p_combined, const String &p_server_first);
	Ref<R> sasl_final(const PackedByteArray &p_data);

	PackedStringArray row_desc(const uint8_t *p_data, int p_len);
	Variant data_row(const uint8_t *p_data, int p_len, const LocalVector<StringName> &p_keys, const LocalVector<int32_t> &p_oids, const LocalVector<uint8_t> &p_fmts, bool p_values, Array *r_flat = nullptr);

	friend class GDPostgresCallInternal;
	friend class GDPostgresPool;
	friend class GDPostgresPoolCall;
	friend class GDDatabaseClient;
	friend class GDDatabaseTxCall;
	friend class GDDatabaseRows;
	friend class ScramKeyJob;
	friend class GDPostgresPackJob;

protected:
	static void _bind_methods();

public:
	GDPostgresClient();
	// Detach event delivery on destruction to prevent callbacks into a freed object.
	~GDPostgresClient();
	// Close all live connections at process shutdown.
	static void shutdown_all();
	bool is_open() const;
	void close();
	// Return a signal; scripts receive R with `await db.open(...)`.
	Signal open(const String &p_host, int64_t p_port, const Dictionary &p_opts);
	// Send $1 and $2 values separately without interpolating them into SQL.
	Signal query(const String &p_sql, const Array &p_args);
	// Return column-ordered row arrays for bulk retrieval.
	Signal query_values(const String &p_sql, const Array &p_args);
	// Return all values in one row-major array.
	Signal query_flat(const String &p_sql, const Array &p_args);
	// Send through the shared database API with explicit result row and byte limits.
	Signal query_limited(const String &p_sql, const Array &p_args, int p_max_rows, int64_t p_max_bytes);
	// Return only the first row and drain the rest to the protocol boundary.
	Signal query_row(const String &p_sql, const Array &p_args, int64_t p_max_bytes = 0);
	// Retain the connection's result and return one row per Next call.
	Signal query_rows(const String &p_sql, const Array &p_args);
	// Batch one statement with different arguments into one wait and completion signal.
	// The server still receives and counts each as an individual query.
	Signal query_many(const String &p_sql, const Array &p_rows);
	// Batch one statement and flatten all returned rows.
	Signal fetch_many(const String &p_sql, const Array &p_rows);
	// Flatten rows from repeated statements as column-ordered arrays.
	Signal fetch_values_many(const String &p_sql, const Array &p_rows);
	Signal fetch_flat_many(const String &p_sql, const Array &p_rows); // Return all row values in one array.
	// Batch updates and return only the completion count without individual results.
	Signal exec_many(const String &p_sql, const Array &p_rows);
	// Validate syntax and names without execution by preparing, describing, and discarding.
	// Use the server's parser instead of duplicating SQL syntax locally.
	Signal check(const String &p_sql);
	int in_flight() const { return inflight.size() + packing.size(); } // Count operations awaiting encoding or replies.
	int cached_stmts() const { return stmts.size(); } // Count cached statements.
};

// Share ordinary queries and reserve exclusive connections for transactions or sequential Rows.
// Retain each operation until its protocol boundary, including after caller cancellation.
class GDPostgresPoolCall : public RefCounted {
	GDCLASS(GDPostgresPoolCall, RefCounted);

	Ref<GDPostgresPoolCall> self_hold; // Keep this operation alive until completion.
	Ref<GDPostgresPool> pool; // Pool owning the wait queue and borrowed connection.
	Ref<GDPostgresClient> conn; // Connection occupied during the query.
	Ref<GDPostgresCallInternal> inner; // Underlying query receiving cancellation.
	String sql; // SQL to execute.
	Array args; // Bind arguments or batched argument sets.
	Ref<R> pending; // Result delivered once from the completion queue.
	Ref<GDDatabaseRows> stream; // Sequential Rows retained until delivery to the caller.
	int kind = 0; // Selected query operation kind.
	int max_rows = 0; // Maximum returned row count.
	int64_t max_bytes = 0; // Maximum returned byte count.
	uint64_t queued_at = 0; // Time connection acquisition began.
	uint64_t due = 0; // Absolute query deadline including pool waiting.
	List<Ref<GDPostgresPoolCall>>::Element *wait_entry = nullptr; // Wait-queue position supporting constant-time removal.
	bool notified = false; // Whether the caller has received a result.
	bool done = false; // Prevent duplicate completion.
	bool delivery_posted = false; // Prevent duplicate queued result delivery.

	void start(const Ref<GDPostgresClient> &p_conn); // Start the query on the borrowed connection.
	void answered(const Ref<R> &p_result); // Return connection results through the pool operation.
	void settled(); // Return the connection only after draining protocol replies, even after cancellation.
	void fail_later(const Ref<R> &p_result); // Defer failure until the next turn after the waiter attaches.
	void deliver(); // Deliver a pending failure through its signal.
	void deliver_lease(); // Deliver the connection on the next turn after the waiter attaches.
	void deliver_stream(); // Deliver sequential Rows on the next turn after the waiter attaches.
	void post_result(const Ref<R> &p_result, bool p_release); // Retain the result and schedule its delivery for the next turn.
	void finish(const Ref<R> &p_result); // Clean up the connection and self-reference.
	void release(); // Release the occupied connection and self-reference.

	friend class GDPostgresPool;
	friend class GDPostgresCallInternal;
	friend class GDDatabaseClient;
	friend class GDDatabaseTxCall;

protected:
	static void _bind_methods();

public:
	void cancel(); // Cancel connection acquisition or the active query.
	Ref<GDPostgresClient> connection() const { return conn; } // Physical connection borrowed by a transaction.
	void release_lease() { release(); } // Return the connection after transaction completion.
};

class GDPostgresPool : public RefCounted {
	GDCLASS(GDPostgresPool, RefCounted);

	Vector<Ref<GDPostgresClient>> conns;
	List<Ref<GDPostgresPoolCall>> waits; // Queries waiting for connections in arrival order.
	HashSet<GDPostgresClient *> leased; // Connections reserved for transactions or sequential Rows.
	RBSet<GDPostgresPoolDeadline> deadlines; // Connection-wait deadlines ordered by time.
	String host; // Destination hostname for lazy connection creation.
	Dictionary opts; // Options for lazily created connections.
	int port = 5432; // Destination port for lazy connection creation.
	int default_size = 4; // Maximum connection count selected by the factory.
	int max_open = 4; // Most recently configured maximum connection count.
	int opening = 0; // Connections currently being created.
	uint64_t query_wait_ms = 0; // Query timeout including connection acquisition.
	uint64_t generation = 0; // Generation rejecting connection results from before closure.
	int64_t wait_count = 0; // Cumulative number of queries that waited for a connection.
	int64_t wait_usec = 0; // Cumulative connection-acquisition wait time.
	uint64_t wait_due = 0; // Earliest deadline registered with the kernel wait layer.
	bool configured = false; // Whether queries may create connections on demand.

	Ref<GDPostgresClient> pick(bool p_exclusive) const; // Prefer unused connections and share the least busy when capacity is reached.
	Signal send(int p_kind, const String &p_sql, const Array &p_args, int p_max_rows = 0, int64_t p_max_bytes = 0); // Borrow a connection and send a query.
	void dispatch(); // Assign the first waiter to an available connection.
	void grow(); // Create only the connections needed by waiters, up to configured capacity.
	void opened(const Ref<R> &p_result, const Ref<GDPostgresClient> &p_conn, uint64_t p_generation); // Apply lazy-connection results to waiting operations.
	void trim_closed(); // Remove closed connections from the physical connection count.
	void cancel_wait(GDPostgresPoolCall *p_call); // Remove one waiting operation from the queue.
	void released(const Ref<GDPostgresClient> &p_conn, bool p_exclusive); // Return one operation without releasing another caller's reservation.
	void record_wait(GDPostgresPoolCall *p_call); // Accumulate connection-acquisition time.
	void sync_wait(); // Register only the earliest deadline with the event loop.
	void step(); // Finish all waiters whose deadlines have expired.

	friend class GDPostgresPoolCall;
	friend class GDDatabaseClient;

protected:
	static void _bind_methods();

public:
	GDPostgresPool();
	void set_default_size(int64_t p_size); // Use CPU-derived capacity for zero or the specified positive maximum.
	Signal acquire(); // Acquire a transaction connection through the ordinary query wait queue.
	static Signal no_conn(); // Return a failure when the pool is not open.
	// Configure the pool and create connections lazily on first use.
	Signal open(const String &p_host, int64_t p_port, const Dictionary &p_opts, int64_t p_size);
	// Send through an available connection; scripts use `await pool.query(...)`.
	Signal query(const String &p_sql, const Array &p_args);
	// Send shared database queries through an available connection with explicit result limits.
	Signal query_limited(const String &p_sql, const Array &p_args, int p_max_rows, int64_t p_max_bytes);
	// Return only the first row through an available connection.
	Signal query_row(const String &p_sql, const Array &p_args, int64_t p_max_bytes = 0);
	// Return rows sequentially while retaining the borrowed connection until Rows closes.
	Signal query_rows(const String &p_sql, const Array &p_args);
	Signal query_values(const String &p_sql, const Array &p_args); // Return rows as column-ordered arrays.
	Signal query_flat(const String &p_sql, const Array &p_args); // Return all row values in one array.
	// Batch one statement through one connection with one wait and completion signal.
	Signal query_many(const String &p_sql, const Array &p_rows);
	// Batch one statement and flatten all rows from its queries.
	Signal fetch_many(const String &p_sql, const Array &p_rows);
	Signal fetch_values_many(const String &p_sql, const Array &p_rows); // Flatten column-ordered row arrays.
	Signal fetch_flat_many(const String &p_sql, const Array &p_rows); // Return all row values in one array.
	// Batch updates through one connection and return only the completion count.
	Signal exec_many(const String &p_sql, const Array &p_rows);
	void close();
	int size() const { return conns.size(); }
	int in_flight() const;
	Dictionary stats() const; // Return connection-pool state and cumulative wait statistics.
	bool is_open() const;
};
