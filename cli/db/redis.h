/**************************************************************************/
/*  redis.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Provide a RESP connection for GDRedisClient.
// Send array-form commands and read both RESP2 and RESP3 replies.
//
// Scripts can call `await db.query("GET", ["k"])`.
// The result is an R value.

#include "cli/data/bytes.h"
#include "cli/sys/pool.h"
#include "cli/sys/std.h"

#include "core/templates/list.h"
#include "core/templates/local_vector.h"

class GDRedisClient;
class GDRedisPool;
class GDRedisCallInternal;
class GDRedisPoolCall;

// Encode one key-value command as RESP on a CPU worker.
class GDRedisPackJob : public PoolJob {
	GDCLASS(GDRedisPackJob, PoolJob);

	Ref<GDRedisClient> db; // Retain the connection until completion.
	Ref<GDRedisCallInternal> call; // Query operation receiving the result.
	String cmd; // Single-command name.
	Array args; // Single-command arguments.
	Array cmds; // Pipeline commands.
	ByteBuf packed; // Outbound bytes produced by the worker.
	Ref<R> error; // Encoding failure.
	int64_t queued_bytes = 0; // Conservative byte footprint in the connection queue.
	bool batch = false; // Whether to encode a pipeline.
	bool subscription = false; // Whether success switches to passive subscription reads.

	friend class GDRedisClient;

protected:
	static void _bind_methods() {}
	void run() override;
	void finish() override;
};

// Represent one query and emit finished when its result is complete.
class GDRedisCallInternal : public RefCounted {
	GDCLASS(GDRedisCallInternal, RefCounted);

	// Track the pending phase.
	enum Mode {
		RESOLVING, // Resolve the hostname on a worker.
		OPENING, // Wait for connection establishment.
		READING, // Wait for a complete reply.
	};

	Ref<GDRedisClient> db;
	Ref<GDRedisCallInternal> self_hold; // Remain alive until completion even without a script reference.
	Mode mode = READING;
	uint64_t due = 0; // Operation deadline.
	Ref<R> pending; // Result delivered on the next scheduler turn.
	String password; // Password sent immediately after connecting, or empty if absent.
	int want_replies = 1; // Expected reply count, increased for pipelines.
	Array collected; // Replies collected from a pipeline.
	enum Shape { VALUE, BATCH, EXEC }; // Public result shape independent of reply count.
	Shape shape = VALUE; // Single command, pipeline, or transaction result.
	bool dropped = false; // Discard canceled replies while retaining queue position.
	bool notified = false; // Whether the waiter has already received a result.
	bool posted = false; // Prevent duplicate runtime ready-queue entries.
	uint64_t generation = 1; // Generation identifying stale notifications after reuse.

	void reset(); // Reset state for reuse.
	void step(); // Advance from the runtime ready queue.
	void schedule(); // Schedule once for the next event-loop turn.
	void dispatch(uint64_t p_generation); // Dispatch only notifications for the current generation.
	void done(const Ref<R> &p_out);
	// Defer immediate failures until the caller can begin waiting.
	void fail_later(const Ref<R> &p_out);
	void set_due(uint64_t p_wait); // Register the deadline with the kernel wait layer.
	Ref<R> result(const Ref<R> &p_error = Ref<R>()) const; // Preserve command positions and failure causes.
	void on_auth(const Ref<R> &p_out); // Receive the authentication result.

	friend class GDRedisClient;
	friend class GDRedisPool;

public:
	void cancel(); // Close the connection when the waiter cancels.

protected:
	static void _bind_methods();
};

class GDRedisClient : public RefCounted {
	GDCLASS(GDRedisClient, RefCounted);

	Wire sock; // Plain or TLS-protected connection.
	Wire::Guard guard = Wire::NONE; // Protection mode selected from TLS options.
	Ref<GDTrust> ca; // Optional trust roots loaded on a worker.
	bool wrapped = false; // Whether TLS setup has completed.
	// Batch commands queued in the same turn into one outbound buffer
	// to avoid one socket write per command.
	ByteBuf out_buf;
	ByteBuf next_buf; // Next bytes completed by a worker during the previous send.
	bool flush_queued = false; // Whether a flush is scheduled for the next turn.
	PackedByteArray buf; // Partially consumed receive buffer.
	int buf_at = 0; // Read cursor advanced without deleting the prefix.
	int line_at = 0; // Position at which newline scanning resumes.
	int bulk_len = -1; // Bulk length whose header is complete but body is pending.
	int64_t reply_memory = 0; // Estimated bytes of strings and containers created from the current reply.
	// Retain unfinished arrays, sets, pushes, and maps for the next receive.
	struct ParseFrame {
		char kind = 0;
		int left = 0;
		Array items;
		Dictionary map;
		Variant key;
		bool has_key = false;
	};
	LocalVector<ParseFrame> reply_stack;
	String host;
	int port = 6379;
	String password;
	uint64_t wait_ms = 10000; // Connection and response timeout; zero waits until cancellation.

	// Complete queued replies in send order for pipelined operations.
	List<Ref<GDRedisCallInternal>> inflight;
	List<Ref<GDRedisPackJob>> packing; // Encoding queue preserving per-connection arrival order.
	int64_t packing_bytes = 0; // Maximum wire bytes retained by pending encoding jobs.
	Ref<GDRedisCallInternal> opening; // Operation performing DNS, connection setup, or authentication.
	bool watching = false; // Whether socket notifications need processing.
	bool pump_posted = false; // Prevent duplicate runtime ready-queue entries.
	bool reply_yielded = false; // Whether RESP parsing exhausted its time slice.
	uint64_t wire_generation = 1; // Connection generation rejecting stale pumps after reopening.

	// Receive unsolicited messages after subscribing.
	bool subscribed = false;
	LocalVector<Ref<GDRedisCallInternal>> spare; // Reusable operation objects.

	// Resume parsing one reply; retain incomplete state and return ok(false).
	Ref<R> take_reply(int &r_at, Variant &r_value, bool &r_ready, uint64_t p_due);
	Ref<R> fill(); // Read all currently available socket bytes.
	// Borrow and return operation objects to avoid repeated registration.
	Ref<GDRedisCallInternal> lend();
	void give_back(GDRedisCallInternal *p_call);
	// Start an operation for native continuation chaining.
	Ref<GDRedisCallInternal> start(const String &p_cmd, const Array &p_args);
	// Start one pipeline operation waiting for all expected replies.
	Ref<GDRedisCallInternal> start_batch(const Array &p_cmds);
	// Encode one command directly into bytes.
	static bool pack_cmd(ByteBuf &r_out, const String &p_cmd, const Array &p_args);
	bool write_cmd(const String &p_cmd, const Array &p_args); // Buffer a command when it fits the configured limits.
	void queue_pack(const Ref<GDRedisPackJob> &p_job); // Append to the connection's encoding queue.
	void start_pack(); // Submit only the first encoding job to the CPU pool.
	void packed(GDRedisPackJob *p_job); // Move worker-produced bytes into the send queue in arrival order.
	void queue_flush(); // Schedule a single flush for the next turn.
	void flush_out(); // Send the buffered commands together.
	void pump(); // Distribute received replies in send order.
	void socket_ready(); // Run only when the kernel reports connection readiness.
	void post_pump(); // Schedule buffered continuation or fair yielding for the next turn.
	void dispatch_pump(); // Pump from the ready queue only when needed.
	void emit_push(const Variant &p_reply); // Deliver an unsolicited message.
	void watch(bool p_on);
	void fail_connection(const Ref<R> &p_why); // Close the connection and all waiters after losing response framing.
	// Continue connection setup after worker-based hostname resolution.
	void resolved(const Ref<R> &p_result, const Ref<GDRedisCallInternal> &p_call);

	friend class GDRedisCallInternal;
	friend class GDRedisPackJob;
	friend class GDRedisPool;

protected:
	static void _bind_methods();

public:
	GDRedisClient();
	// Detach socket delivery targets on destruction.
	~GDRedisClient();
	// Close all live connections at process shutdown.
	static void shutdown_all();
	bool is_open() const;
	void close();
	// Connect and immediately authenticate when a password is supplied.
	// Return a signal; scripts receive R with `await db.open(...)`.
	Signal open(const String &p_host, int64_t p_port, const Dictionary &p_opts);
	// Send one command and read one reply; additional commands may queue without waiting.
	Signal query(const String &p_cmd, const Array &p_args);
	// Send a pipeline and collect replies as an array in one round trip.
	// Each command has the form ["SET", "k", "v"].
	Signal pipeline(const Array &p_cmds);
	// Wrap commands in MULTI and EXEC for transactional execution.
	Signal transaction(const Array &p_cmds);
	// Begin subscription reads and emit message for each received item.
	Signal subscribe(const PackedStringArray &p_channels);
	int in_flight() const { return inflight.size() + packing.size(); }
	bool is_subscribed() const { return subscribed; }
};

// Own connection acquisition through return while honoring cancellation and deadlines during waits.
class GDRedisPoolCall : public RefCounted {
	GDCLASS(GDRedisPoolCall, RefCounted);
	Ref<GDRedisPoolCall> self_hold; // Retain this operation until result delivery.
	Ref<GDRedisPool> pool; // Pool lending the connection.
	Ref<GDRedisClient> conn; // Exclusively borrowed connection.
	Signal inner; // Connection or query completion signal.
	String cmd; // Command name to send.
	Array args; // Arguments retained at call time.
	Ref<R> outcome; // Result delivered on the next ready-queue turn.
	List<Ref<GDRedisPoolCall>>::Element *entry = nullptr; // Entry in the pool's complete operation list.
	List<Ref<GDRedisPoolCall>>::Element *waiting = nullptr; // Position in the connection wait queue.
	uint64_t due = 0; // Connection-wait deadline; zero waits until cancellation.
	bool opening = false; // Whether a physical connection is being established.
	bool done = false; // Prevent duplicate completion or cancellation.
	void start(const Ref<GDRedisClient> &p_conn, bool p_open); // Borrow a connection and start the operation.
	void received(const Ref<R> &p_out); // Receive connection or query results.
	void finish(const Ref<R> &p_out, bool p_close = false); // Return the connection and release the wait entry.
	void deliver(); // Deliver the result exactly once.
	void expired(); // Check the connection-wait deadline.
	friend class GDRedisPool;

protected:
	static void _bind_methods();

public:
	void cancel(); // Cancel only this operation.
};

// Create connections on demand and lend each exclusively until returned.
class GDRedisPool : public RefCounted {
	GDCLASS(GDRedisPool, RefCounted);

	Vector<Ref<GDRedisClient>> conns;
	Vector<Ref<GDRedisClient>> idle; // Reuse returned connections in LIFO order.
	List<Ref<GDRedisPoolCall>> calls; // All incomplete operations.
	List<Ref<GDRedisPoolCall>> waits; // Operations waiting for a connection in arrival order.
	String host; // Configured destination.
	Dictionary opts; // Options passed to each physical connection.
	int port = 6379; // Default key-value server port.
	int default_size = 0; // Factory maximum connection count; zero leaves it unspecified.
	int max_open = 0; // Configured maximum connection count; zero is unlimited.
	int dial_limit = 1; // CPU-derived concurrent connection-creation limit.
	int opening = 0; // Connections currently being established.
	uint64_t wait_ms = 0; // Connection-wait timeout.
	bool configured = false; // Whether a usable destination is configured.
	bool posted = false; // Prevent duplicate scheduling of connection-wait resumptions.
	void schedule(); // Schedule connection-wait continuation on the ready queue.
	void pump(); // Lend an idle or new connection to a waiter.
	void release(const Ref<GDRedisClient> &p_conn); // Return or discard a connection.
	friend class GDRedisPoolCall;

protected:
	static void _bind_methods();

public:
	GDRedisPool();
	~GDRedisPool();
	void set_default_size(int64_t p_size) { default_size = (p_size >= 0 && p_size <= INT_MAX) ? int(p_size) : -1; }
	Signal open(const String &p_host, int64_t p_port, const Dictionary &p_opts, int64_t p_size);
	// Borrow a connection exclusively, waiting for return when the pool is at capacity.
	Signal query(const String &p_cmd, const Array &p_args);
	void close();
	int size() const { return conns.size(); }
	int in_flight() const;
};
