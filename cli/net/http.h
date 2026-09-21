/**************************************************************************/
/*  http.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once
#include "cli/net/serve_conn.h"
#include "cli/net/h2_core.h"
#include <map>

// Run HTTP listeners with independent multiplexed streams and native transport ownership.
//
// Avoid per-byte script calls and their slicing, splitting, and dictionary costs.
// Parse requests and assemble responses in the transport layer.
// Scripts advance processing, inspect routes, and submit responses.

#include "cli/sys/file_source.h"

#include "cli/net/stream.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rb_set.h"

// Return status reason text, also used outside transport framing by routes.
const char *http_reason(int p_status);

constexpr int64_t HTTP_HEADER_LIMIT_MAX = INT_MAX - 4096; // Request-header boundary leaving read-ahead room within int-indexed storage.

class GDWebServer : public RefCounted {
	GDCLASS(GDWebServer, RefCounted);
	friend class GDWebBodyCall;

	static constexpr int READ_CHUNK = 4096; // Receive-buffer width for incremental header parsing.
	static constexpr int BODY_READ_WAIT = 0; // More body data has not arrived yet.
	static constexpr int BODY_READ_DATA = 1; // One body chunk was returned.
	static constexpr int BODY_READ_EOF = 2; // The body has been fully consumed.
	static constexpr int BODY_READ_LIMIT = 3; // The explicit body limit was exceeded.
	static constexpr int BODY_READ_BAD = 4; // Malformed body framing or a lost connection.
	static constexpr int DRAIN_MAX = 256 * 1024; // Unread-body threshold for deciding connection reuse.
	static constexpr int DEFAULT_MAX_HEAD = 1 << 20; // Default total request-header limit.
	static constexpr int DEFAULT_MAX_FIELDS = INT_MAX; // Default field count leaves the header-byte limit as the effective bound.
	static constexpr int HEAD_SLOP = 4096; // Extra read-ahead capacity for finding the header terminator.
	static constexpr int TRAILER_MAX = 4096; // Request-trailer limit matching the regular buffer width.
	static constexpr int CHUNK_LINE_MAX = 4096; // Maximum chunk-size line length.
	static constexpr int CHUNK_HEX_MAX = 16; // Hexadecimal digits available in a uint64 chunk length.
	static constexpr int CHUNK_EXCESS_MAX = 16 * 1024; // Maximum accumulated excess chunk framing.
	static constexpr int64_t BODY_LIMIT_MAX = INT64_MAX; // Body boundary representable by HTTP Content-Length.
	static constexpr int64_t DEFAULT_MAX_BODY = BODY_LIMIT_MAX; // Do not narrow the body length when no limit is specified.
	static constexpr uint64_t DEFAULT_HEAD_MS = 0; // No header deadline by default.
	static constexpr uint64_t DEFAULT_BODY_MS = 0; // No body deadline by default.
	static constexpr int SERVER_LEN = 12; // Length of the cached Server: gd header including CRLF.

	// Distinguish simultaneous deadlines by connection ID.
	struct ConnTime {
		uint64_t due = 0; // Deadline in milliseconds.
		int id = 0; // Connection ID.

		bool operator<(const ConnTime &p_other) const {
			return due == p_other.due ? id < p_other.id : due < p_other.due;
		}
	};

	// Keep multiplexed request metadata separate from physical transport and compression ownership.
	struct H2Request {
		int parent = 0; // Physical connection owning compression and socket readiness.
		uint32_t stream = 0; // Multiplexed wire identifier, separate from the application request ID.
		String method, target, path, query; // Decoded request metadata, never reparsed as an HTTP/1 message.
		std::vector<GDH2::Field> fields; // Ordered initial fields, preserving repeated values.
		std::deque<std::vector<uint8_t>> body; // Flow-controlled unread DATA chunks.
		size_t at = 0; // Consumed prefix of the first DATA chunk.
		int64_t written = 0; // Current response batch bytes accepted by the physical transport.
		bool remote_end = false, dead = false, writing = false, sent = false; // Stream lifecycle and producer scheduling.
	};
	struct H2Session {
		GDH2::Connection connection; // Compression, framing, and per-stream flow ownership.
		std::map<uint32_t,int> requests; // Map wire stream IDs to stable application request IDs.
		std::deque<int> writers; // Round-robin logical streams with sendable output or ready producers.
		std::vector<uint8_t> output; // Exactly one output unit retained across socket backpressure.
		size_t at = 0; // Sent prefix of the current output unit.
		bool ending = false; // Stop accepting streams while draining existing responses.
		explicit H2Session(const GDH2::Config &config) : connection(config) {} // Construct direction-local protocol ownership.
	};
	// Store physical connections and logical requests under stable application identifiers.
	struct Conn {
		Ref<GDServeConn> sock;
		std::unique_ptr<H2Session> h2; // Physical multiplexed connection, absent on logical requests.
		std::unique_ptr<H2Request> request; // Logical stream state, absent on physical and HTTP/1 connections.
		String ip; // Actual peer address, not a forwarded header value.
		LocalVector<uint8_t> buf; // Partially received input.
		int scan = 0; // Header-terminator scan position.
		int head_end = -1; // Header-end position, or -1 while incomplete.
		int64_t need = 0; // Total fixed body length, or completed chunked-body length.
		int64_t body_got = 0; // Body bytes delivered to the handler or discarded.
		int64_t body_limit = DEFAULT_MAX_BODY; // Explicit read limit for this request.
		int64_t drained = 0; // Bytes discarded after handler completion to permit reuse.
		bool body_done = false; // Whether the body terminator has been consumed.
		bool body_reading = false; // Whether an asynchronous body read awaits more input.
		bool body_limited = false; // Preserve the same failure for reads after the body limit is exceeded.
		bool draining = false; // Whether a small body left unread by the handler is being drained.
		bool expect_continue = false; // Send 100 Continue on the first body read.
		bool continue_sent = false; // Whether 100 Continue has already been sent.
		int continue_at = 0; // Partial 100 Continue write position.
		bool chunked = false; // Whether Transfer-Encoding is chunked.
		int chunk_at = 0; // Next wire position to decode.
		int64_t chunk_left = 0; // Data bytes remaining in the current chunk.
		int chunk_state = 0; // Chunk state: 0 size, 1 data, 2 trailing CRLF, 3 trailer.
		int chunk_fields = 0; // Number of trailer fields.
		int chunk_trailer = 0; // Trailer bytes consumed.
		int chunk_excess = 0; // Excess framing bytes relative to actual data.
		int bad = 0; // Rejection status for malformed requests, or zero for an accepted request.
		bool head_only = false; // HEAD request; do not transmit a response body.
		bool http10 = false; // Unknown response lengths require connection-close framing.
		uint64_t began = 0; // First-byte arrival time used for the header deadline.
		bool keep = true; // Whether to retain the connection after the response.
		bool ready = false; // Whether one request is ready.
		bool ready_queued = false; // Whether kernel readiness has enqueued this connection.
		bool half = false; // Peer send side is closed; no more requests will arrive.
		bool shut = false; // Close after flushing output.
		bool reused = false; // Keepalive connection that has handled at least one request.
		uint64_t last = 0; // Most recent receive activity.
		uint64_t due = 0; // Deadline registered in the ordered index.
		int id = -1; // Table ID used to find the connection when flushing.
		// Coalesce response headers and framing for contiguous socket writes.
		LocalVector<uint8_t> out_buf;
		BodyChunk out_body; // Share the response body without a full transport copy.
		int64_t out_body_at = 0; // Response-body write position.
		Ref<GDBodySource> out_file; // Incremental body delivered as the socket consumes chunks.
		bool out_chunked = false; // Emit length framing around each produced chunk.
		bool out_suppressed = false; // Keep only source completion until bodyless response headers drain.
		int out_tail = 0; // Unsent framing bytes following the current chunk.
		bool out_taken = false; // Retain even an empty source batch until its framing has drained.
		bool out_final = false; // Include terminal framing with the final body range.
		int64_t reply_status = 0; // Response status retained until unread-body draining finishes.
		PackedByteArray reply_body; // Response body waiting for draining.
		Ref<GDBodySource> reply_file; // Incremental body to send after draining.
		String reply_type; // Response content type waiting for draining.
		Dictionary reply_headers; // Additional response headers waiting for draining.
		bool reply_waiting = false; // Whether to send the retained response after draining.
		int m_at = 0, m_len = 0; // Method position and length, such as GET.
		int t_at = 0, t_len = 0; // Request-target position and length.
		int p_len = 0; // Target length before the query delimiter.
		int h_at = 0, h_len = 0; // Header-section position excluding the request line.
		Callable body_ready; // Internal continuation awaiting more body data.
	};

	LocalVector<int> readyq; // FIFO containing only connections reported ready by the kernel.
	uint32_t ready_at = 0; // Next ready connection to advance.
	LocalVector<int> ended_ids; // Removed multiplexed requests whose suspended handlers need cancellation.
	Callable ready_call; // Application serve task.
	bool accept_ready = false; // Whether the listener is ready to accept.
	bool accept_turn = true; // Alternate a saturated listener with already-runnable connections.
	bool ready_posted = false; // Whether the serve task is already in the runtime queue.
	bool polling = false; // Defer reentrant polling to the next runtime turn.
	String type_memo; // Most recently transmitted content type.
	CharString type_memo_utf; // Cached UTF-8 form of that content type.
	uint64_t drop_n = 0; // Cumulative number of omitted headers.
	int64_t max_body = DEFAULT_MAX_BODY; // Body-reader limit applied to all requests.
	int max_head = DEFAULT_MAX_HEAD; // Per-request header limit; exceeding it produces 431.
	int max_fields = DEFAULT_MAX_FIELDS; // Separate request-header and trailer field-count limits.
	uint64_t head_ms = DEFAULT_HEAD_MS; // Deadline for receiving complete headers; zero is unlimited.
	uint64_t body_ms = DEFAULT_BODY_MS; // Body deadline for closing stalled incremental senders.
	uint64_t due = 0; // Earliest receive deadline registered with the kernel wait.
	RBSet<ConnTime> conn_times; // Time-ordered index of header and body receive deadlines.
	void flush_conn(Conn *p_c, bool p_producer = false); // Drain output without recursively notifying a running producer.
	void flush_source(int p_id); // Attempt an immediate producer write on its live connection.
	Conn *h2_parent(Conn *p_c) const; // Resolve a logical request's still-live physical owner.
	void h2_schedule(Conn *p_c); // Enqueue one logical producer and wake its physical socket task.
	void h2_poll(Conn *p_c, PackedInt32Array &r_ready); // Advance multiplexed I/O and publish logical request readiness.
	bool h2_flush(Conn *p_c); // Flush one queued transport unit without blocking receive progress.
	void h2_write(Conn *p_c); // Give ready response producers one fair flow-controlled turn each.
	int h2_body(Conn *p_c, int64_t p_max, PackedByteArray &r_data, bool p_limit); // Consume DATA and return window credit through the physical connection.
	void h2_reply(Conn *p_c, int64_t p_status, const PackedByteArray &p_body, const String &p_type, const Dictionary *p_extra, const Ref<GDBodySource> &p_file); // Submit a multiplexed response using existing body producers.
	void h2_done(Conn *p_c); // Retire completed streams or cancel unread bodies after their response is sent.
	bool has_output(const Conn *p_c) const; // Report pending response headers or body bytes.
	bool background_read(Conn *p_c, int p_end, bool p_write_open = false); // Retain one pipelined byte or detect a suspended producer's disconnect.
	void abort_files(Conn *p_c); // Cancel file reads when the connection disappears.
	void finish_body(Conn *p_c); // Release a successful producer after its framing reaches the transport.
	uint64_t deadline_of(const Conn *p_c) const; // Choose a receive deadline from current connection state.
	void refresh_deadline(Conn *p_c); // Update one connection's deadline index entry.
	void rebuild_deadlines(); // Rebuild the deadline index when timeout settings change.
	void arm_deadline(); // Pass the earliest receive deadline to the event loop.
	void listener_ready(); // Mark the listener runnable.
	void socket_ready(int p_id); // Mark only the ready connection runnable.
	void deadline_ready(); // Mark connections that need deadline checks runnable.
	void queue_ready(Conn *p_c); // Enqueue a connection once in the ready queue.
	void post_ready(); // Enqueue the application's serve task in the runtime queue.
	void dispatch_ready(); // Wake the application from the runtime queue.
	void forget(int p_id); // Safely release the connection and its wait resources.
	bool reap_closed(Conn *p_c); // Remove a connection that closed after output was flushed.

	Ref<GDStream> srv;
	Ref<GDTLSIdentity> identity; // Shared immutable server identity, absent for plaintext.
	// Connection IDs are unique and never reused.
	// Handlers may await before responding; reusing an ID could send their response to another peer.
	HashMap<int, Conn *> conns;
	int next_id = 1;

	int64_t date_at = -1; // Second for which the cached date header was generated.
	char date_txt[80] = { 0 }; // Cached server identification and date headers.

	// Find the header terminator, returning its position or -1.
	static int find_head_end(const uint8_t *p_buf, int p_size, int &r_scan);
	// Find a header case-insensitively and return the value's offset and length.
	static bool find_header(const uint8_t *p_buf, int p_at, int p_len, const String &p_name, int &r_at, int &r_len);
	// Return current date headers, rebuilding at most once per second.
	const char *now();
	// Advance one connection, returning true when a request is ready.
	void drop(Conn *p_c);
	// Close after a complete response without resetting it away.
	void close_sent(Conn *p_c);
	bool advance(Conn *p_c, uint64_t p_now_ms); // Parse input using the current poll timestamp.
	// Read one body chunk at the handler's request.
	int read_body(Conn *p_c, int64_t p_max, PackedByteArray &r_data, bool p_limit);
	// Drain only small unread bodies and decide whether the connection is reusable.
	void drain_body(Conn *p_c);
	// Send 100 Continue only when body reading starts.
	void send_continue(Conn *p_c);
	// Drain unread input if necessary before sending the response.
	void queue_reply(Conn *p_c, int64_t p_status, const PackedByteArray &p_body, const String &p_type, const Dictionary *p_extra);
	// Drain unread input before sending a file response.
	void queue_file_reply(Conn *p_c, int64_t p_status, const Ref<GDBodySource> &p_file, const String &p_type, const Dictionary *p_extra);
	// Discard the completed request.
	void consume(Conn *p_c);
	// Assemble response headers and pass a shared body reference to transmission.
	void build_out(Conn *p_c, int64_t p_status, const PackedByteArray &p_body, const String &p_type, const Dictionary *p_extra, bool p_consume = true, const Ref<GDBodySource> &p_file = Ref<GDBodySource>());

protected:
	static void _bind_methods();

public:
	bool has_request(int p_id) const; // Ignore stale readiness IDs after cancellation or response completion.
	// Set the body-reader limit for all requests.
	void set_body_limit(int64_t p_bytes);
	// Set request-header byte and field-count limits.
	void set_header_limits(int64_t p_bytes, int64_t p_values);
	// Set complete-header and body deadlines; zero is unlimited.
	void set_header_timeout(double p_seconds);
	void set_body_timeout(double p_seconds);
	// Set the application task that processes ready connections.
	void set_ready_callback(const Callable &p_call);
	Ref<R> listen(int64_t p_port, const String &p_host);
	void set_identity(const Ref<GDTLSIdentity> &p_identity) { identity = p_identity; } // Configure encryption before accepting connections.
	// Return the actual listen port, including kernel-assigned ports.
	int get_port() const;
	// Stop accepting new connections while allowing active requests to respond.
	void begin_shutdown();
	// Enable shared-port listeners for multiprocess serving.
	static bool share_port;
	void stop();
	bool is_listening() const;

	// Advance connections and return ready request IDs, valid until the next poll.
	PackedInt32Array poll();
	// Check that the connection remains alive before asynchronous completion.
	bool has_conn(int p_id) const { return conns.has(p_id); }
	// Detect peer disconnect after the body terminator.
	bool request_alive(int p_id, const Ref<GDAsyncContext> &p_context = Ref<GDAsyncContext>());

	// Return a malformed request's rejection status, or zero to proceed.
	int bad_of(int p_id) const;
	String get_method(int p_id) const;
	bool is_head(int p_id) const; // Inspect the method without decoding a temporary string.
	String get_path(int p_id) const;
	String get_query(int p_id) const;
	String get_ip(int p_id) const;
	String get_header(int p_id, const String &p_name) const;
	Dictionary get_headers(int p_id) const;
	// Read up to p_max body bytes, returning a BODY_READ_* state.
	int read_body(int p_id, int64_t p_max, PackedByteArray &r_data);
	// Set the internal continuation for the next body arrival.
	bool wait_body(int p_id, const Callable &p_call);
	void clear_body_wait(int p_id);
	// Read one body chunk through the low-level API, returning state and data.
	Dictionary body_part(int p_id, int64_t p_max);
	// Set a byte-reader limit for this request only.
	bool set_request_body_limit(int p_id, int64_t p_bytes);
	// Return Content-Length, or -1 for chunked input.
	int64_t get_body_size(int p_id) const;

	// Submit the common response form with just content and type.
	void respond(int p_id, int64_t p_status, const PackedByteArray &p_body, const String &p_type);
	// Submit an assembled body directly, avoiding an intermediate copy.
	void respond_bytes(int p_id, int p_status, const uint8_t *p_body, int p_len, const String &p_type);
	// Submit a response with additional headers.
	void respond_with(int p_id, int64_t p_status, const Dictionary &p_headers, const PackedByteArray &p_body);
	// Stream worker-delivered file chunks as socket capacity becomes available.
	void respond_file(int p_id, int64_t p_status, const Dictionary &p_headers, const Ref<GDBodySource> &p_file, const String &p_type);

	int connection_count() const;
	uint64_t dropped_headers() const { return drop_n; } // Cumulative number of omitted headers.

	GDWebServer() {}
	~GDWebServer();
};
