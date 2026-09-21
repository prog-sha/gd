/**************************************************************************/
/*  fetch.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Issue client-side HTTP requests through the fetch API.
// Process HTTP with an incremental transport resumed by kernel notifications.
//
// Scripts can use await GD.http.fetch(url).
// Advance on connection and worker completion, then emit finished.

#pragma once

#include "cli/sys/sink.h"

#include "cli/net/http_peer.h"
#include "core/object/ref_counted.h"
#include "core/templates/list.h"

class R;
class GDWait;

// Internal transport reusing HTTP connections by origin.
class GDHTTPTransport : public RefCounted {
	GDCLASS(GDHTTPTransport, RefCounted);

	// One connection waiting for reuse.
	struct Idle {
		String origin; // Origin comprising scheme, host, and port.
		Ref<GDHTTPPeer> client; // Connection whose response has been fully consumed.
		uint64_t expires = 0; // Time after which reuse is abandoned.
	};

	List<Idle> idle; // Connections ordered oldest first.
	struct Shared {
		String origin; // Exact verified destination, without cross-origin certificate coalescing.
		Ref<GDHTTPLink> link; // Multiplexed connection available even while other requests are active.
	};
	List<Shared> shared; // Authenticated multiplexed connections retained by origin.
	Ref<GDWait> expiry; // Timer for the earliest idle-connection expiry.
	bool watching = false; // Whether idle disconnect notifications are being monitored.

	void prune(); // Remove expired or disconnected connections.
	void arm(); // Arm the timer for the earliest expiry.
	void expire(); // Close idle connections whose expiry has arrived.
	void step(); // Update idle connection state after socket notifications.
	void watch(bool p_on); // Attach to the event loop only while idle connections exist.

protected:
	static void _bind_methods() {}

public:
	Ref<GDHTTPPeer> take(const String &p_origin, const String &p_host, int p_port); // Reserve shared capacity or acquire an idle connection for the same origin.
	void share(const String &p_origin, const Ref<GDHTTPLink> &p_link); // Register an authenticated link without waiting for its first response.
	void give(const String &p_origin, const Ref<GDHTTPPeer> &p_client); // Return a fully consumed connection.
	void clear(); // Close all retained connections.
	~GDHTTPTransport();
};

// HTTP response, including partial headers and body retained on transport failure.
class GDHTTPResponse : public RefCounted {
	GDCLASS(GDHTTPResponse, RefCounted);

	int status_code = 0;
	Dictionary head; // Headers with lowercase names.
	PackedByteArray body;

	friend class GDHTTPCall;

protected:
	static void _bind_methods();

public:
	int get_status() const { return status_code; }
	Dictionary get_headers() const { return head; }
	PackedByteArray get_body() const { return body; }
	bool ok() const { return status_code >= 200 && status_code < 300; }
	String text() const;
	Ref<R> json() const; // Distinguish valid JSON null from parsing failure through R.
};

// One request, delivering its response through finished.
class GDHTTPCall : public RefCounted {
	GDCLASS(GDHTTPCall, RefCounted);

	// Request progress determining the next state-machine step.
	enum Stage {
		RETRYING, // Waiting for a cancellable retry delay after an unprocessed stream rejection.
		CONNECTING, // Waiting for connection.
		WAITING, // Waiting for response headers.
		READING, // Receiving the body.
		FLUSHING, // Reception complete; waiting for the writer to flush.
		MOVING, // Moving a validated temporary file to its final name on a worker.
		DONE,
	};

	Ref<GDHTTPPeer> client;
	Ref<GDHTTPTransport> transport; // Shared transport reusing connections between requests.
	Ref<GDHTTPResponse> res;
	Ref<Err> error; // Failure retained independently of the partial response.
	Ref<GDHTTPCall> self_hold; // Retain the call until completion even if scripts release it.
	Ref<GDWait> retry_wait; // Backoff timer, abandoned when the request finishes or is cancelled.
	unsigned retries = 0; // Completed multiplexed replay decisions before response headers.
	Stage stage = CONNECTING;
	uint64_t deadline = 0; // Time after which the request fails.
	String host;
	String origin; // Scheme, host, and port used to partition pooled connections.
	int port = 0; // Port for a new connection.
	bool tls_enabled = false; // Enable TLS on new HTTPS connections.
	bool reused = false; // Allow one replacement of a stale reused connection.
	bool reusable = false; // Response fully consumed and connection eligible for reuse.
	bool peer_close = false; // Whether the peer requested Connection: close.
	String target; // Path and query.
	String method = "GET";
	int64_t body_max = 0; // Caller-selected body limit, or zero for unlimited.
	int64_t body_len = -1; // Content-Length, or -1 when absent or chunked.
	bool chunked = false; // Use transport state to determine whether the terminal chunk arrived.
	PackedStringArray head; // Outgoing headers.
	PackedByteArray send_body; // Preserve binary bytes so image or recording data is not stringified.
	String save_path; // Destination for saving the body without retaining it in memory.
	String save_next; // Temporary path for an incomplete body.
	String want_sha; // SHA-256 expected before committing the saved file.
	Ref<FileSink> save_sink; // Worker-thread body writer.
	int64_t received = 0; // Total received body bytes.

	void step(); // Advance the request on event-loop notifications.
	void moved(const Ref<R> &p_result); // Receive the worker's rename result.
	void done(const Ref<Err> &p_error);
	void done(const String &p_why, Err::Kind p_kind = Err::NONE); // Clean up and emit finished.
	bool open_client(); // Create a new connection.
	bool retry(); // Replay unprocessed requests or safe requests on stale reused connections.
	void retry_ready(); // Reopen the destination after cancellable backoff.
	void set_due(uint64_t p_wait); // Register the deadline with the kernel wait.
	void watch(bool p_on); // Attach or detach event-loop notifications.

protected:
	static void _bind_methods();

public:
	// Validate the target and options, reporting even startup failures asynchronously through finished.
	void begin(const String &p_url, const Dictionary &p_opts, const Ref<GDHTTPTransport> &p_transport);
	void cancel(); // Close the connection when its caller cancels.
	static void shutdown_all(); // Cancel unfinished requests at process shutdown.
};
