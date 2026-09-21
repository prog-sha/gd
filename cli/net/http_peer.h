// Read HTTP responses incrementally while keeping logical requests separate from transport ownership.
#pragma once

#include "cli/net/http_link.h"

class GDHTTPPeer : public RefCounted {
	GDCLASS(GDHTTPPeer, RefCounted);

public:
	enum Status { STATUS_DISCONNECTED, STATUS_CONNECTING, STATUS_CONNECTED, STATUS_REQUESTING, STATUS_BODY, STATUS_CONNECTION_ERROR }; // HTTP connection and body states.

private:
	Ref<GDHTTPLink> link; // Physical connection shared by multiplexed request peers.
	uint32_t stream = 0; // Logical request stream, zero before reservation becomes a wire request.
	std::vector<GDH2::Field> fields; // Request metadata waiting for an available stream.
	std::deque<std::vector<uint8_t>> chunks; // Received DATA bounded by stream flow credit.
	size_t chunk_at = 0; // Consumed prefix of the first body chunk.
	bool stream_end = false, upload_queued = false; // Receive completion and coalesced upload readiness.
	bool read_on = true; // Per-request delivery interest, independent of physical socket reads.
	bool replay = false; // Peer confirmed that the request was not processed, allowing a retained body to be resent.
	friend class GDHTTPLink;
	Status status = STATUS_DISCONNECTED; // Public connection state.
	String host; // Target for the Host header and TLS verification.
	int port = 0; // Remote port.
	bool secure = false; // Whether this is an HTTPS connection.
	String method; // Request method used for body rules such as HEAD.
	PackedByteArray request_head; // Request headers being transmitted.
	PackedByteArray request_body; // Body shared with the caller.
	int64_t head_at = 0; // Partial header-write position.
	int64_t body_at = 0; // Partial body-write position.
	PackedByteArray input; // Received bytes not yet consumed by parsing.
	int at = 0; // Consumed-input position.
	int scan = 0; // Position from which to resume the line-ending search.
	int code = 0; // Response status code.
	bool ready = false; // Whether final response headers have arrived.
	bool old = false; // Whether the response uses HTTP/1.0.
	bool chunked = false; // Whether to decode chunk boundaries.
	bool close_body = false; // Whether the body continues until TCP EOF.
	bool eof = false; // Normal EOF from the underlying stream.
	int64_t length = -1; // Content-Length, or -1 when absent.
	uint64_t left = 0; // Bytes remaining in the fixed-length body or current chunk.
	int chunk_state = 0; // Chunk state: 0 length, 1 data, 2 CRLF, 3 trailer.
	int64_t excess = 0; // Accumulated excess chunk framing.
	int64_t header_bytes = 0; // Total header bytes, including informational responses.
	int trailer_bytes = 0; // Trailer bytes within the input-buffer capacity.
	List<String> headers; // Final response headers with duplicates preserved.
	Callable callback; // Target for returning buffered continuations to the ready queue.

	bool fill(int p_size = 4096); // Append required input using the default receive-buffer width.
	bool line(String &r_line, int p_limit, bool p_crlf = false); // Extract a line without rescanning consumed input.
	bool parse_headers(); // Validate the response line and headers.
	void fail(); // Prevent reuse after framing is lost.
	void again(); // Enqueue continuations that need no kernel notification.

protected:
	static void _bind_methods() {} // Keep internal HTTP transport operations outside the script API.

public:
	bool can_replay() const { return replay && !ready; } // Exclude responses already exposed to the caller.
	Ref<Err> error() const { return link.is_valid() ? link->wire.error() : Ref<Err>(); } // Expose the original transport failure after framing stops.
	Ref<GDHTTPLink> multiplex_link() const { return link.is_valid() && link->h2 ? link : Ref<GDHTTPLink>(); } // Share only authenticated multiplexed links.
	static Ref<GDHTTPPeer> reserve(const Ref<GDHTTPLink> &p_link, const String &p_host, int p_port); // Create an independent request peer on a reusable encrypted connection.
	static bool token(const String &p_text); // Validate method and header-name tokens.
	Error open(const String &p_host, int p_port, bool p_secure, uint64_t p_due); // Begin connecting.
	Error request(const String &p_method, const String &p_target, const PackedStringArray &p_headers, const PackedByteArray &p_body); // Transmit the request incrementally.
	void poll(); // Advance connection, transmission, and header parsing.
	void set_callback(const Callable &p_callback); // Replace the target for native notifications and buffered continuations.
	void read_wait(bool p_on); // Pause this request's delivery without blocking other streams.
	int available() const; // Detect unread response bytes without treating multiplexed control frames as dirty HTTP/1 input.
	Status get_status() const { return status; } // Return the HTTP state.
	bool has_response() const { return ready; } // Report whether final headers are complete.
	int get_response_code() const { return code; } // Return the response status code.
	int64_t get_response_body_length() const { return length; } // Return the declared body length.
	bool is_response_chunked() const { return chunked; } // Report a chunked response.
	bool is_response_http_10() const { return old; } // Report an HTTP/1.0 response.
	bool request_sent() const { return head_at == request_head.size() && body_at == request_body.size(); } // Prevent reuse when an early response leaves request bytes unsent.
	void get_response_headers(List<String> *p_headers) const { *p_headers = headers; } // Return headers with duplicates preserved.
	PackedByteArray read_response_body_chunk(); // Return body bytes incrementally using the copy-buffer width.
	void close(); // Cancel pending dialing and close the owned connection.
	~GDHTTPPeer(); // Release resources that were not reused.
};
