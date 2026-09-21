// Frame multiplexed HTTP messages independently of sockets, runtime objects, and body storage.
#pragma once
#include "hpack_core.h"
#include <memory>

namespace GDH2 {
enum Error : uint32_t {
	NO_ERROR = 0, PROTOCOL_ERROR = 1, INTERNAL_ERROR = 2, FLOW_CONTROL_ERROR = 3,
	SETTINGS_TIMEOUT = 4, STREAM_CLOSED = 5, FRAME_SIZE_ERROR = 6, REFUSED_STREAM = 7,
	CANCEL = 8, COMPRESSION_ERROR = 9, CONNECT_ERROR = 10, ENHANCE_YOUR_CALM = 11,
	INADEQUATE_SECURITY = 12, HTTP_1_1_REQUIRED = 13,
}; // Standard connection and stream error identifiers.
struct Config {
	bool server = true; // Accept requests instead of opening local request streams.
	uint32_t concurrent = 250; // Advertised maximum simultaneously active peer streams.
	uint32_t connection_window = 1 << 20, stream_window = 1 << 20; // Receive capacities, independent of total body length.
	uint32_t frame_size = 1 << 20; // Advertised maximum received frame payload.
	uint32_t table_size = 4096; // Advertised header decoder table capacity.
	uint32_t header_size = (1 << 20) + 320; // Field-list octets including per-field overhead.
	uint32_t header_count = INT32_MAX; // Optional field-count bound after octet accounting.
};
struct Event {
	enum Kind { HEADERS, DATA, END, RESET, WRITABLE, GOAWAY } kind = WRITABLE;
	uint32_t stream = 0, code = 0; // Logical stream and error or rejection status.
	bool trailers = false; // Distinguish final fields from initial or informational headers.
	std::vector<Field> fields;
	std::vector<uint8_t> data; // Body ownership transferred to the application.
};
// Own a single connection's protocol state while callers schedule ready streams and transport writes.
class Connection {
	struct State;
	std::unique_ptr<State> state;
public:
	explicit Connection(Config config = {}); // Queue the connection preface and initial settings.
	~Connection();
	bool feed(const uint8_t *data, size_t size); // Incrementally validate incoming frames and queue semantic events.
	bool event(Event &out); // Transfer the next event without invoking application callbacks inside parsing.
	bool output(std::vector<uint8_t> &out); // Transfer one complete queued frame or connection preface.
	void sent(const std::vector<uint8_t> &frame); // Commit stream closure only after this complete output unit reaches the transport.
	bool pending() const; // Report buffered transport output.
	bool failed() const; // Report a terminal connection error.
	uint32_t capacity() const; // Report reservable client streams after accounting active wire streams.
	uint32_t open(const std::vector<Field> &fields, bool end); // Open an available client stream, or return zero while unavailable.
	bool headers(uint32_t stream, const std::vector<Field> &fields, bool end); // Queue a response or trailer without HTTP/1 transcoding.
	size_t writable(uint32_t stream) const; // Report the next DATA payload allowed by both flow windows and frame size.
	std::vector<uint8_t> *reserve(uint32_t stream, size_t &size, bool end); // Reserve one DATA frame; append exactly size bytes before any other connection call.
	size_t write(uint32_t stream, const uint8_t *data, size_t size, bool end); // Queue one flow-controlled DATA fragment.
	bool finish(uint32_t stream); // Queue an empty terminal DATA frame when no body bytes remain.
	bool consume(uint32_t stream, size_t size); // Return receive credit only for bytes consumed by the application.
	void reset(uint32_t stream, uint32_t code = CANCEL); // Cancel one stream and return its unread connection credit.
	void retire(uint32_t stream); // Release a fully consumed closed stream without resetting healthy peers.
	void shutdown(uint32_t code = NO_ERROR); // Stop new streams and drain already accepted work.
};
}
