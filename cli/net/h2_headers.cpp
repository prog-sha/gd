// Validate field-block semantics and publish request streams without an intermediate text protocol.
#include "h2_state.h"
#include <algorithm>
#include <set>

namespace GDH2 {
namespace {
// Accept only lowercase wire tokens, leaving pseudo-field syntax to its separate schema.
bool field_name(const std::string &name) {
	if (name.empty()) return false;
	for (unsigned char ch : name) {
		if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) continue;
		if (!ch || std::string_view("!#$%&'*+-.^_`|~").find(char(ch)) == std::string_view::npos) return false;
	}
	return true;
}
// Exclude line delimiters and control octets before passing fields to application APIs.
bool field_value(const std::string &value) {
	if (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.back() == ' ' || value.back() == '\t')) return false;
	for (unsigned char ch : value) if ((ch < 32 && ch != '\t') || ch == 127) return false;
	return true;
}
// Recognize connection-specific fields that cannot be forwarded on a multiplexed stream.
bool connection_field(const Field &field) {
	const std::string &name = field.name;
	return name == "connection" || name == "proxy-connection" || name == "keep-alive" ||
		name == "upgrade" || name == "transfer-encoding" || (name == "te" && field.value != "trailers");
}
// Parse declared lengths without accepting signs, whitespace, or signed-range overflow.
bool body_length(const std::string &text, int64_t &out) {
	if (text.empty()) return false;
	uint64_t value = 0;
	for (unsigned char ch : text) {
		if (ch < '0' || ch > '9' || value > (uint64_t(INT64_MAX)-(ch-'0'))/10) return false;
		value = value*10+(ch-'0');
	}
	out = int64_t(value);
	return true;
}
// Resolve exactly one pseudo field while detecting duplicates and unknown names.
bool pseudos(const std::vector<Field> &fields, std::map<std::string,std::string> &values) {
	for (const auto &field : fields) {
		if (field.name.empty() || field.name[0] != ':') continue;
		if (!values.emplace(field.name,field.value).second) return false;
	}
	return true;
}
}
// Account decoded fields and stop retaining invalid or oversized lists without corrupting table history.
void Connection::State::field(Field &&value) {
	if (++block_count > config.header_count || value.size() > head_left) {
		truncated = true; head_left = 0; decoder.emitting_fields(false); return;
	}
	const bool pseudo = !value.name.empty() && value.name[0] == ':';
	if (!field_value(value.value) || (pseudo && regular) || (!pseudo && !field_name(value.name))) {
		block_error = PROTOCOL_ERROR; decoder.emitting_fields(false); return;
	}
	if (!pseudo) regular = true;
	head_left -= value.size();
	fields.push_back(std::move(value));
}
// Decode one contiguous fragment and enforce uninterrupted continuation ownership.
bool Connection::State::headers() {
	size_t offset = 0, length = payload.size();
	if (frame_type == 1) {
		block_id = frame_stream; block_end = frame_flags & 1; block_error = 0; block_count = 0;
		regular = false; truncated = false; fields.clear(); head_left = config.header_size;
		decoder.begin([this](Field &&value) { field(std::move(value)); },config.header_size);
		if (frame_flags & 8) {
			if (!length || payload[0] >= length) return fail(PROTOCOL_ERROR);
			offset = 1; length -= size_t(payload[0])+1;
		}
		if (frame_flags & 0x20) {
			if (length < 5) return fail(FRAME_SIZE_ERROR);
			const uint32_t parent = ((uint32_t(payload[offset]) << 24) | (uint32_t(payload[offset+1]) << 16) |
				(uint32_t(payload[offset+2]) << 8) | payload[offset+3]) & INT32_MAX;
			if (parent == block_id) block_error = PROTOCOL_ERROR;
			offset += 5; length -= 5;
		}
	}
	if (uint64_t(length) > 2*head_left || (frame_type == 9 && block_error)) return fail(PROTOCOL_ERROR);
	if (!decoder.feed(length ? payload.data()+offset : nullptr,length)) return fail(COMPRESSION_ERROR);
	continuation = frame_flags & 4 ? 0 : block_id;
	if (continuation) return true;
	if (!decoder.finish()) return fail(COMPRESSION_ERROR);
	return complete_headers();
}
// Resolve initial metadata, trailers, and informational responses under one stream lifecycle.
bool Connection::State::complete_headers() {
	const uint32_t id = block_id;
	if (!(id & 1)) return fail(PROTOCOL_ERROR);
	auto found = streams.find(id);
	if (config.server && found == streams.end()) {
		if (id <= highest) return fail(PROTOCOL_ERROR);
		highest = id;
		if (stopping) { number(3,id,REFUSED_STREAM); return !broken; }
		if (active >= config.concurrent) {
			number(3,id,unacked ? REFUSED_STREAM : PROTOCOL_ERROR); return !broken;
		}
		Stream stream; stream.send = peer_window; stream.receive.available = config.stream_window;
		stream.wire_known = true;
		found = streams.emplace(id,std::move(stream)).first;
		++active;
	} else if (found == streams.end()) return fail(PROTOCOL_ERROR);
	Stream &stream = found->second;
	if (stream.remote_end) { reset(id,STREAM_CLOSED,true); return !broken; }
	if (block_error) { reset(id,block_error,true); return !broken; }
	std::map<std::string,std::string> pseudo;
	if (!pseudos(fields,pseudo)) { reset(id,PROTOCOL_ERROR,true); return !broken; }
	const bool trailers = stream.incoming;
	bool informational = false;
	uint32_t rejection = truncated ? 431 : 0;
	if (trailers) {
		if (!pseudo.empty() || !block_end || truncated) { reset(id,PROTOCOL_ERROR,true); return !broken; }
	} else if (config.server && !truncated) {
		const std::string method = pseudo[":method"], path = pseudo[":path"], scheme = pseudo[":scheme"];
		bool valid = !method.empty();
		for (unsigned char ch : method) {
			if (ch <= 32 || ch >= 127 || std::string_view("()<>@,;:\\\"/[]?={}").find(char(ch)) != std::string_view::npos) valid = false;
		}
		if (method == "CONNECT") valid = valid && !pseudo[":authority"].empty() && path.empty() && scheme.empty();
		else valid = valid && (scheme == "http" || scheme == "https") && !path.empty() && (path[0] == '/' || path == "*");
		if (method != "CONNECT" && pseudo[":authority"].find('@') != std::string::npos) valid = false;
		// Require complete percent escapes in the path; query decoding belongs to the application.
		for (size_t i = 0; i < path.size() && path[i] != '?'; ++i) {
			if (path[i] != '%') continue;
			if (i+2 >= path.size() || std::string_view("0123456789abcdefABCDEF").find(path[i+1]) == std::string_view::npos ||
				std::string_view("0123456789abcdefABCDEF").find(path[i+2]) == std::string_view::npos) { valid = false; break; }
			i += 2;
		}
		for (const auto &value : pseudo) if (value.first != ":method" && value.first != ":path" && value.first != ":scheme" && value.first != ":authority") valid = false;
		if (!valid) { reset(id,PROTOCOL_ERROR,true); return !broken; }
	} else if (!config.server) {
		if (truncated || pseudo.size() != 1 || !pseudo.count(":status")) { reset(id,PROTOCOL_ERROR,true); return !broken; }
		const std::string &status = pseudo.at(":status");
		if (status.size() != 3 || status[0] < '1' || status[0] > '9' || status[1] < '0' || status[1] > '9' ||
			status[2] < '0' || status[2] > '9' || status == "101") { reset(id,PROTOCOL_ERROR,true); return !broken; }
		informational = status[0] == '1';
		if (!informational) stream.no_body = status == "204" || status == "304";
		if (informational && block_end) { reset(id,PROTOCOL_ERROR,true); return !broken; }
	}
	int64_t length = -1;
	for (const Field &value : fields) {
		if (connection_field(value)) {
			if (!config.server || trailers) { reset(id,PROTOCOL_ERROR,true); return !broken; }
			rejection = 400;
		}
		if (value.name == "content-length") {
			int64_t count;
			if (trailers || !body_length(value.value,count) || (length >= 0 && length != count)) { reset(id,PROTOCOL_ERROR,true); return !broken; }
			length = count;
		}
	}
	if (!trailers && !informational) { stream.incoming = true; stream.length = length; }
	Event event; event.kind = Event::HEADERS; event.stream = id; event.code = rejection;
	event.trailers = trailers; event.fields.swap(fields); events.push_back(std::move(event));
	return block_end ? end(id) : !broken;
}
// Encode one uninterrupted header block without imposing an unrelated body or frame-count bound.
bool Connection::State::send_headers(uint32_t id, const std::vector<Field> &values, bool end) {
	auto found = streams.find(id);
	if (broken || found == streams.end() || found->second.local_end) return false;
	Stream &stream = found->second;
	uint64_t size = 0;
	bool informational = false;
	for (const Field &value : values) {
		size += value.size();
		if (size > peer_headers || !field_value(value.value)) return false;
		if (value.name.empty() || (value.name[0] != ':' && !field_name(value.name)) || connection_field(value)) return false;
		if (stream.outgoing && value.name[0] == ':') return false;
		if (value.name == ":status" && !value.value.empty() && value.value[0] == '1') informational = true;
	}
	if ((stream.outgoing && !end) || (informational && end)) return false;
	Packet header; header.header = true; header.stream = id; header.end = end; header.fields = values;
	packets.push_back(std::move(header));
	if (!informational) stream.outgoing = true;
	if (end) { stream.local_end = true; settle(stream); }
	return !broken;
}
// Open requests only while the peer's concurrency allowance and stream identifier space remain available.
uint32_t Connection::open(const std::vector<Field> &fields, bool end) {
	State &s = *state;
	if (s.config.server || s.broken || s.stopping || s.active >= s.peer_streams || s.next > INT32_MAX) return 0;
	const uint32_t id = s.next;
	State::Stream stream; stream.send = s.peer_window; stream.receive.available = s.config.stream_window;
	for (const auto &field : fields) if (field.name == ":method" && field.value == "HEAD") stream.head = true;
	s.streams.emplace(id,std::move(stream));
	++s.active;
	if (!s.send_headers(id,fields,end)) { s.streams.erase(id); --s.active; return 0; }
	s.next += 2;
	return id;
}
// Queue response or trailer fields for an existing logical stream.
bool Connection::headers(uint32_t stream, const std::vector<Field> &fields, bool end) {
	return state->send_headers(stream,fields,end);
}
}
