// Retain frame parsing, compression, and per-stream accounting without application dependencies.
#pragma once
#include "h2_core.h"
#include <array>
#include <map>

namespace GDH2 {
struct Connection::State {
	struct Window {
		int64_t available = 65535, returned = 0; // Announced receive credit and coalesced consumed octets.
		uint32_t give(uint32_t size); // Batch updates until four KiB or doubling available credit.
	};
	struct Stream {
		Window receive;
		int64_t send = 65535, length = -1; // Peer send credit and declared incoming body length.
		uint64_t received = 0, unread = 0; // Total DATA and application-owned unconsumed octets.
		bool incoming = false, outgoing = false, remote_end = false, local_end = false, head = false;
		bool no_body = false; // Final response status excludes content independently of representation metadata.
		bool counted = true; // Count only open and half-closed streams toward negotiated concurrency.
		bool wire_known = false; // Whether the peer has observed or initiated this stream.
		bool local_sent = false, retiring = false; // Actual send completion and deferred removal of consumed state.
	};
	struct Packet {
		std::vector<uint8_t> bytes;
		bool control = false; // Count only queued control frames toward flood protection.
		std::vector<Field> fields; // Uncompressed headers retained until their ordered transport turn.
		uint32_t stream = 0; // Stream owning an unstarted header block.
		bool header = false, end = false; // Deferred header marker and its stream-end flag.
	};
	Config config;
	HeaderDecoder decoder;
	HeaderEncoder encoder;
	std::map<uint32_t, Stream> streams;
	std::deque<Event> events;
	std::deque<Packet> packets;
	Window receive;
	int64_t send = 65535; // Connection-wide outbound credit.
	uint32_t peer_window = 65535, peer_frame = 16384, peer_streams = UINT32_MAX;
	uint64_t peer_headers = UINT64_MAX; // No field-list restriction until the peer advertises one.
	bool peer_settings = false; // Preserve the initial concurrency fallback until the first non-ACK settings.
	uint32_t highest = 0, next = 1, peer_last = INT32_MAX; // Remote high-water mark and local allocation state.
	uint32_t active = 0; // Streams still open in at least one direction, independent of retained unread bodies.
	uint32_t continuation = 0, block_id = 0, block_error = 0, block_count = 0;
	uint32_t sent_header_end = 0; // Stream awaiting the final continuation of a terminal header block.
	uint64_t head_left = 0; // Remaining decoded field-list octets for the current block.
	std::vector<Field> fields;
	bool block_end = false, regular = false, truncated = false, started = false, stopping = false, broken = false;
	std::array<uint8_t, 9> frame_head{};
	size_t head_at = 0, body_at = 0, preface_at = 0; // Incremental wire cursor positions.
	uint32_t frame_length = 0, frame_stream = 0, control_count = 0, unacked = 1;
	uint8_t frame_type = 0, frame_flags = 0;
	std::vector<uint8_t> payload; // One bounded frame, never an entire compressed block or body.
	explicit State(Config value); // Validate local settings and emit the initial connection control frames.
	bool frame_header(); // Reject impossible frame sizes and interleaving before payload allocation.
	bool frame(); // Dispatch one fully received frame under its stream and connection state.
	bool settings(); // Apply peer settings and acknowledge each frame independently.
	bool headers(); // Decode an initial or continuation fragment while keeping compression state synchronized.
	bool data(); // Debit received payload and transfer body ownership.
	bool end(uint32_t id); // Validate declared body length and publish receive completion.
	void settle(Stream &stream); // Release concurrency when both directions have completed.
	void field(Field &&value); // Validate individual fields and enforce list accounting during decoding.
	bool complete_headers(); // Check pseudo fields and update request or response stream state.
	bool fail(uint32_t code); // Queue one terminal connection error and stop parsing.
	void reset(uint32_t id, uint32_t code, bool send_reset); // Dispose a stream and publish cancellation exactly once.
	void credit(uint32_t id, uint32_t size); // Restore stream and connection receive capacity.
	void packet(uint8_t type, uint8_t flags, uint32_t id, const uint8_t *data, size_t size); // Encode a frame header and exact payload.
	static Packet encode(uint8_t type, uint8_t flags, uint32_t id, const uint8_t *data, size_t size); // Build a frame without changing output order or control accounting.
	void number(uint8_t type, uint32_t id, uint32_t value); // Queue a four-octet control payload.
	void goaway(uint32_t code); // Advertise the last accepted peer stream.
	bool send_headers(uint32_t id, const std::vector<Field> &values, bool end); // Compress and atomically queue a complete field block.
	bool idle(uint32_t id) const; // Distinguish never-opened streams from closed historical streams.
};
}
