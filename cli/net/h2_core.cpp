// Drive multiplexed framing and flow accounting without blocking on network or application progress.
#include "h2_state.h"
#include <algorithm>
#include <cstring>

namespace GDH2 {
namespace {
constexpr char PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"; // Connection identification octets.
constexpr uint32_t MAX_WINDOW = INT32_MAX, CONTROL_LIMIT = 10000; // Wire credit width and queued control-frame flood bound.
// Read a network-order field whose width is established by its frame schema.
uint32_t read32(const uint8_t *data) {
	return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}
// Append a network-order settings value or stream identifier.
void append32(std::vector<uint8_t> &out, uint32_t value) {
	for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
}
}
// Accumulate consumed credit without flooding the peer with tiny updates.
uint32_t Connection::State::Window::give(uint32_t size) {
	returned += size;
	if (returned < 4096 && returned < available) return 0;
	const uint32_t update = uint32_t(returned);
	available += returned;
	returned = 0;
	return update;
}
// Start from wire defaults and announce configured receive capacities.
Connection::State::State(Config value) : config(value) {
	if (!config.server) peer_streams = 100; // Initial client reservations before peer settings arrive.
	if (!config.concurrent || config.concurrent > INT32_MAX) config.concurrent = 250;
	if (config.connection_window < 65535 || config.connection_window > MAX_WINDOW) config.connection_window = config.server ? 1 << 20 : 1 << 30;
	if (!config.stream_window || config.stream_window > MAX_WINDOW) config.stream_window = config.server ? 1 << 20 : 4 << 20;
	if (config.frame_size < 16384 || config.frame_size > 0xffffff) config.frame_size = 1 << 20;
	if (!config.table_size || config.table_size > INT32_MAX) config.table_size = 4096;
	if (!config.server) packets.push_back({std::vector<uint8_t>(PREFACE,PREFACE+24),false});
	std::vector<uint8_t> settings;
	auto setting = [&](uint16_t id, uint32_t number) {
		settings.push_back(uint8_t(id >> 8)); settings.push_back(uint8_t(id)); append32(settings,number);
	};
	setting(1,config.table_size);
	if (!config.server) setting(2,0);
	setting(3,config.concurrent);
	setting(4,config.stream_window);
	setting(5,config.frame_size);
	setting(6,config.header_size);
	packet(4,0,0,settings.data(),settings.size());
	decoder.allow(std::max<uint32_t>(4096,config.table_size));
	if (config.connection_window > 65535) number(8,0,config.connection_window-65535);
	receive.available = config.connection_window;
}
// Construct one output unit without publishing it to the transport queue.
Connection::State::Packet Connection::State::encode(uint8_t type, uint8_t flags, uint32_t id, const uint8_t *data, size_t size) {
	Packet packet;
	packet.control = type != 0 && type != 1 && type != 9;
	packet.bytes.reserve(9+size);
	packet.bytes.push_back(uint8_t(size >> 16)); packet.bytes.push_back(uint8_t(size >> 8)); packet.bytes.push_back(uint8_t(size));
	packet.bytes.push_back(type); packet.bytes.push_back(flags);
	append32(packet.bytes,id);
	if (data && size) packet.bytes.insert(packet.bytes.end(),data,data+size);
	return packet;
}
// Frame one queued output unit and enforce the control-only flood policy.
void Connection::State::packet(uint8_t type, uint8_t flags, uint32_t id, const uint8_t *data, size_t size) {
	if (config.server && type != 0 && type != 1 && type != 9 && type != 7 && control_count >= CONTROL_LIMIT) { fail(ENHANCE_YOUR_CALM); return; }
	Packet packet = encode(type,flags,id,data,size);
	if (packet.control) ++control_count;
	packets.push_back(std::move(packet));
}
// Encode one fixed-width control value.
void Connection::State::number(uint8_t type, uint32_t id, uint32_t value) {
	std::vector<uint8_t> data;
	append32(data,value);
	packet(type,0,id,data.data(),data.size());
}
// Preserve the accepted-stream boundary when refusing future work.
void Connection::State::goaway(uint32_t code) {
	std::vector<uint8_t> data;
	append32(data,highest); append32(data,code);
	packet(7,0,0,data.data(),data.size());
	stopping = true;
}
// Publish one fatal error without recursively enqueueing additional failures.
bool Connection::State::fail(uint32_t code) {
	if (!broken) {
		broken = true;
		goaway(code);
		Event event; event.kind = Event::GOAWAY; event.code = code; events.push_back(std::move(event));
	}
	return false;
}
// Determine historical stream state without retaining every closed stream.
bool Connection::State::idle(uint32_t id) const {
	return !id || (id & 1) == 0 || (config.server ? id > highest : id >= next);
}
// Release unread body credit and wake a cancelled stream's application exactly once.
void Connection::State::reset(uint32_t id, uint32_t code, bool send_reset) {
	// Discard unstarted header blocks before they affect compression history, and refund unsent DATA credit.
	for (auto packet = packets.begin(); packet != packets.end();) {
		const bool body = !packet->header && packet->bytes.size() >= 9 && packet->bytes[3] == 0 && read32(packet->bytes.data()+5) == id;
		if ((packet->header && packet->stream == id) || body) {
			if (body) send += packet->bytes.size()-9;
			packet = packets.erase(packet);
		} else ++packet;
	}
	auto found = streams.find(id);
	if (found != streams.end()) {
		send_reset = send_reset && found->second.wire_known;
		const uint32_t count = uint32_t(found->second.unread);
		if (found->second.counted) --active;
		streams.erase(found);
		credit(0,count);
		Event event; event.kind = Event::RESET; event.stream = id; event.code = code; events.push_back(std::move(event));
	}
	if (send_reset) number(3,id,code);
}
// Restore connection credit even when the stream was reset or has completed.
void Connection::State::credit(uint32_t id, uint32_t size) {
	if (uint32_t update = receive.give(size)) number(8,0,update);
	auto found = streams.find(id);
	if (found != streams.end() && !found->second.remote_end) {
		if (uint32_t update = found->second.receive.give(size)) number(8,id,update);
	}
}
// Validate frame placement and fixed sizes before reserving payload memory.
bool Connection::State::frame_header() {
	frame_length = (uint32_t(frame_head[0]) << 16) | (uint32_t(frame_head[1]) << 8) | frame_head[2];
	frame_type = frame_head[3]; frame_flags = frame_head[4]; frame_stream = read32(frame_head.data()+5) & MAX_WINDOW;
	if (frame_length > config.frame_size) return fail(FRAME_SIZE_ERROR);
	if (continuation && (frame_type != 9 || frame_stream != continuation)) return fail(PROTOCOL_ERROR);
	if (!continuation && frame_type == 9) return fail(PROTOCOL_ERROR);
	if (!started && (frame_type != 4 || (frame_flags & 1))) return fail(PROTOCOL_ERROR);
	if ((frame_type == 0 || frame_type == 1 || frame_type == 2 || frame_type == 3 || frame_type == 5 || frame_type == 9) && !frame_stream) return fail(PROTOCOL_ERROR);
	if ((frame_type == 4 || frame_type == 6 || frame_type == 7) && frame_stream) return fail(PROTOCOL_ERROR);
	if ((frame_type == 2 && frame_length != 5) || (frame_type == 3 && frame_length != 4) ||
		(frame_type == 6 && frame_length != 8) || (frame_type == 7 && frame_length < 8) ||
		(frame_type == 8 && frame_length != 4) || (frame_type == 4 && (frame_length % 6 || ((frame_flags & 1) && frame_length)))) return fail(FRAME_SIZE_ERROR);
	if (frame_type == 5) return fail(PROTOCOL_ERROR);
	payload.resize(frame_type <= 9 ? frame_length : 0);
	body_at = 0;
	return true;
}
// Apply acknowledged decoder limits and atomically validate peer settings.
bool Connection::State::settings() {
	if (frame_flags & 1) {
		if (!unacked) return fail(PROTOCOL_ERROR);
		--unacked;
		decoder.allow(config.table_size);
		return true;
	}
	if (config.server && payload.size()/6 > 100) return fail(PROTOCOL_ERROR);
	std::vector<std::pair<uint16_t,uint32_t>> values;
	std::map<uint16_t,bool> seen;
	for (size_t i = 0; i < payload.size(); i += 6) {
		const uint16_t id = (uint16_t(payload[i]) << 8) | payload[i+1];
		const uint32_t value = read32(payload.data()+i+2);
		if (config.server && !seen.emplace(id,true).second) return fail(PROTOCOL_ERROR);
		values.emplace_back(id,value);
		if ((id == 2 || id == 8 || id == 9) && value > 1) return fail(PROTOCOL_ERROR);
		if (id == 2 && !config.server && value) return fail(PROTOCOL_ERROR);
		if (id == 4 && value > MAX_WINDOW) return fail(FLOW_CONTROL_ERROR);
		if (id == 5 && (value < 16384 || value > 0xffffff)) return fail(PROTOCOL_ERROR);
	}
	if (!config.server && !peer_settings) peer_streams = 1000; // Client fallback when the initial settings omit concurrency.
	peer_settings = true;
	for (const auto &entry : values) {
		const uint32_t value = entry.second;
		switch (entry.first) {
			case 1: encoder.resize(value); break;
			case 3: peer_streams = value; break;
			case 4: {
				const int64_t delta = int64_t(value)-peer_window;
				for (auto &stream : streams) if (stream.second.send+delta > MAX_WINDOW) return fail(FLOW_CONTROL_ERROR);
				for (auto &stream : streams) stream.second.send += delta;
				peer_window = value;
				break;
			}
			case 5: peer_frame = value; break;
			case 6: peer_headers = value; break;
		}
	}
	packet(4,1,0,nullptr,0);
	Event event; event.kind = Event::WRITABLE; events.push_back(std::move(event));
	return !broken;
}
// Validate complete DATA records and transfer only their unpadded payload.
bool Connection::State::data() {
	if (idle(frame_stream)) return fail(PROTOCOL_ERROR);
	size_t offset = 0, length = payload.size();
	if (frame_flags & 8) {
		if (!length || payload[0] >= length) return fail(PROTOCOL_ERROR);
		offset = 1; length -= size_t(payload[0])+1;
	}
	if (frame_length > receive.available) return fail(FLOW_CONTROL_ERROR);
	receive.available -= frame_length;
	auto found = streams.find(frame_stream);
	if (found == streams.end() || found->second.remote_end) {
		credit(0,frame_length); reset(frame_stream,STREAM_CLOSED,true); return !broken;
	}
	Stream &stream = found->second;
	if (frame_length > stream.receive.available) {
		credit(0,frame_length); reset(frame_stream,FLOW_CONTROL_ERROR,true); return !broken;
	}
	stream.receive.available -= frame_length;
	if (!stream.incoming || ((stream.head || stream.no_body) && length) || (stream.length >= 0 && uint64_t(length) > uint64_t(stream.length)-stream.received)) {
		credit(0,frame_length); reset(frame_stream,PROTOCOL_ERROR,true); return !broken;
	}
	stream.received += length; stream.unread += length;
	credit(frame_stream,frame_length-uint32_t(length));
	if (length) {
		Event event; event.kind = Event::DATA; event.stream = frame_stream;
		if (!offset && length == payload.size()) event.data.swap(payload);
		else event.data.assign(payload.begin()+offset,payload.begin()+offset+length);
		events.push_back(std::move(event));
	}
	return (frame_flags & 1) ? end(frame_stream) : !broken;
}
// Close the receive side and distinguish truncated declared bodies from a clean EOF.
bool Connection::State::end(uint32_t id) {
	auto found = streams.find(id);
	if (found == streams.end()) return !broken;
	Stream &stream = found->second;
	if (stream.length >= 0 && !stream.head && !stream.no_body && stream.received != uint64_t(stream.length)) {
		reset(id,PROTOCOL_ERROR,true); return !broken;
	}
	stream.remote_end = true;
	settle(stream);
	Event event; event.kind = Event::END; event.stream = id; events.push_back(std::move(event));
	return !broken;
}
// Separate protocol concurrency from retained body ownership after both END_STREAM transitions.
void Connection::State::settle(Stream &stream) {
	if (stream.counted && stream.remote_end && stream.local_sent) { stream.counted = false; --active; }
}
// Dispatch completed control frames without depending on body-consumer progress.
bool Connection::State::frame() {
	started = true;
	switch (frame_type) {
		case 0: return data();
		case 1: case 9: return headers();
		case 2:
			if ((read32(payload.data()) & MAX_WINDOW) == frame_stream) reset(frame_stream,PROTOCOL_ERROR,true);
			return !broken;
		case 3:
			if (idle(frame_stream)) return fail(PROTOCOL_ERROR);
			reset(frame_stream,read32(payload.data()),false); return !broken;
		case 4: return settings();
		case 6:
			if (!(frame_flags & 1)) packet(6,1,0,payload.data(),payload.size());
			return !broken;
		case 7: {
			const uint32_t last = read32(payload.data()) & MAX_WINDOW;
			if (last > peer_last) return fail(PROTOCOL_ERROR);
			peer_last = last; stopping = true;
			if (!config.server) {
				std::vector<uint32_t> refused;
				for (const auto &stream : streams) if (stream.first > last) refused.push_back(stream.first);
				for (uint32_t id : refused) reset(id,REFUSED_STREAM,false);
			}
			Event event; event.kind = Event::GOAWAY; event.stream = last; event.code = read32(payload.data()+4); events.push_back(std::move(event));
			return true;
		}
		case 8: {
			const uint32_t increment = read32(payload.data()) & MAX_WINDOW;
			if (frame_stream && idle(frame_stream)) return fail(PROTOCOL_ERROR);
			if (!increment) { if (!frame_stream) return fail(PROTOCOL_ERROR); reset(frame_stream,PROTOCOL_ERROR,true); return !broken; }
			auto stream = streams.find(frame_stream);
			if (frame_stream && stream == streams.end()) return true;
			int64_t &window = frame_stream ? stream->second.send : send;
			if (window+increment > MAX_WINDOW) {
				if (!frame_stream) return fail(FLOW_CONTROL_ERROR);
				reset(frame_stream,FLOW_CONTROL_ERROR,true); return !broken;
			}
			window += increment;
			Event event; event.kind = Event::WRITABLE; event.stream = frame_stream; events.push_back(std::move(event));
			return true;
		}
	}
	return true;
}
// Create protocol state without attaching any network or runtime objects.
Connection::Connection(Config config) : state(std::make_unique<State>(config)) {}
// Release directional compression and all retained stream data.
Connection::~Connection() = default;
// Parse each incoming byte at most once and bound allocation by the validated frame header.
bool Connection::feed(const uint8_t *data, size_t size) {
	State &s = *state;
	if (s.broken || (!data && size)) return false;
	size_t at = 0;
	while (at < size) {
		if (s.config.server && s.preface_at < 24) {
			if (data[at++] != uint8_t(PREFACE[s.preface_at++])) return s.fail(PROTOCOL_ERROR);
			continue;
		}
		if (s.head_at < 9) {
			s.frame_head[s.head_at++] = data[at++];
			if (s.head_at < 9) continue;
			if (!s.frame_header()) return false;
		}
		const size_t take = std::min<size_t>(size-at,s.frame_length-s.body_at);
		if (take && !s.payload.empty()) std::memcpy(s.payload.data()+s.body_at,data+at,take);
		s.body_at += take; at += take;
		if (s.body_at == s.frame_length) {
			if (!s.frame()) return false;
			s.head_at = 0;
		}
	}
	return !s.broken;
}
// Transfer queued semantic events in wire order.
bool Connection::event(Event &out) {
	if (state->events.empty()) return false;
	out = std::move(state->events.front()); state->events.pop_front(); return true;
}
// Transfer one output unit so transport backpressure remains visible to the caller.
bool Connection::output(std::vector<uint8_t> &out) {
	if (state->packets.empty()) return false;
	if (state->packets.front().header) {
		State::Packet header = std::move(state->packets.front()); state->packets.pop_front();
		auto stream = state->streams.find(header.stream);
		if (stream != state->streams.end()) stream->second.wire_known = true;
		std::vector<uint8_t> block;
		state->encoder.begin(block);
		for (const Field &field : header.fields) state->encoder.write(field,block);
		// Retain started continuations as an indivisible protocol sequence, even if the stream is later cancelled.
		if (block.empty()) state->packets.push_front(State::encode(1,uint8_t(4 | (header.end ? 1 : 0)),header.stream,nullptr,0));
		else for (size_t end = block.size(); end;) {
			const size_t start = ((end-1)/state->peer_frame)*state->peer_frame;
			const uint8_t flags = uint8_t((end == block.size() ? 4 : 0) | (!start && header.end ? 1 : 0));
			state->packets.push_front(State::encode(start ? 9 : 1,flags,header.stream,block.data()+start,end-start));
			end = start;
		}
	}
	if (state->packets.front().control) --state->control_count;
	out = std::move(state->packets.front().bytes); state->packets.pop_front(); return true;
}
// Report output waiting for the socket writer.
bool Connection::pending() const { return !state->packets.empty(); }
// Commit a terminal DATA or complete header block in actual transport order.
void Connection::sent(const std::vector<uint8_t> &frame) {
	if (frame.size() < 9 || frame.size() != 9 + (size_t(frame[0]) << 16) + (size_t(frame[1]) << 8) + frame[2]) return;
	const uint8_t type = frame[3], flags = frame[4];
	uint32_t id = read32(frame.data()+5) & INT32_MAX;
	if (type == 1 && (flags & 1) && !(flags & 4)) { state->sent_header_end = id; return; }
	if (type == 9 && (flags & 4) && id == state->sent_header_end) state->sent_header_end = 0;
	else if ((type != 0 && type != 1) || !(flags & 1)) return;
	auto found = state->streams.find(id);
	if (found == state->streams.end()) return;
	found->second.local_sent = true;
	state->settle(found->second);
	if (found->second.retiring && !found->second.counted && !found->second.unread) state->streams.erase(found);
}
// Report a terminal protocol failure after its GOAWAY has been queued.
bool Connection::failed() const { return state->broken; }
// Exclude draining, exhausted, and terminal connections from new client reservations.
uint32_t Connection::capacity() const {
	const State &s = *state;
	return s.config.server || s.broken || s.stopping || s.next > INT32_MAX || s.active >= s.peer_streams ? 0 : std::min(s.peer_streams-s.active, (uint32_t(INT32_MAX)-s.next)/2+1);
}
// Compute one sendable DATA fragment without modifying either credit window.
size_t Connection::writable(uint32_t id) const {
	auto stream = state->streams.find(id);
	if (state->broken || stream == state->streams.end() || stream->second.local_end || !stream->second.outgoing) return 0;
	return size_t(std::max<int64_t>(0,std::min<int64_t>({state->send,stream->second.send,state->peer_frame})));
}
// Reserve a flow-controlled frame for immediate payload assembly without intermediate storage.
std::vector<uint8_t> *Connection::reserve(uint32_t id, size_t &size, bool end) {
	const size_t take = std::min(size,writable(id));
	const bool final = end && take == size;
	size = take;
	if (!take) return nullptr;
	auto &stream = state->streams.at(id);
	state->packet(0,final ? 1 : 0,id,nullptr,take);
	state->send -= take; stream.send -= take;
	if (final) { stream.local_end = true; state->settle(stream); }
	return &state->packets.back().bytes;
}
// Copy a contiguous payload into the same flow-controlled frame assembly path.
size_t Connection::write(uint32_t id, const uint8_t *data, size_t size, bool end) {
	if (!data || !size) return 0;
	std::vector<uint8_t> *frame = reserve(id,size,end);
	if (frame) frame->insert(frame->end(),data,data+size);
	return size;
}
// Complete a body without requiring positive flow-control credit.
bool Connection::finish(uint32_t id) {
	auto stream = state->streams.find(id);
	if (state->broken || stream == state->streams.end() || stream->second.local_end || !stream->second.outgoing) return false;
	state->packet(0,1,id,nullptr,0); stream->second.local_end = true; state->settle(stream->second); return true;
}
// Return only application-owned unread bytes to receive credit.
bool Connection::consume(uint32_t id, size_t size) {
	auto stream = state->streams.find(id);
	if (stream == state->streams.end() || size > stream->second.unread) return false;
	stream->second.unread -= size; state->credit(id,uint32_t(size)); return true;
}
// Cancel one live stream without disturbing unrelated requests.
void Connection::reset(uint32_t id, uint32_t code) {
	if (state->streams.count(id)) state->reset(id,code,true);
}
// Remove completed stream bookkeeping only when all received bytes were consumed.
void Connection::retire(uint32_t id) {
	auto stream = state->streams.find(id);
	if (stream != state->streams.end() && stream->second.local_end && stream->second.remote_end && !stream->second.unread) {
		if (stream->second.local_sent) state->streams.erase(stream);
		else stream->second.retiring = true;
	}
}
// Send the last accepted stream identifier while preserving existing streams.
void Connection::shutdown(uint32_t code) {
	if (!state->stopping) state->goaway(code);
}
}
