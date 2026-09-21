// Read sequential or multiplexed HTTP responses with strict framing and per-request cancellation.
#include "cli/net/http_peer.h"
#include "cli/sys/clock.h"
#include "cli/sys/task.h"
#include "cli/sys/sched.h"
#include <cstring>

namespace {
constexpr int HEADER_MAX = 10 << 20; // Response-header byte limit including informational responses.
constexpr int CHUNK_LINE = 4096; // Maximum chunk-size line length.
constexpr int TRAILER_MAX = 4096; // Trailer capacity matching the input buffer width.
constexpr int COPY_SIZE = 32 * 1024; // Incremental copy-buffer size, not a total body limit.

// Parse decimal Content-Length without signs or integer overflow.
bool decimal(const String &p_value, int64_t &r_value) {
	if (p_value.is_empty()) return false;
	r_value = 0;
	for (int i = 0; i < p_value.length(); i++) {
		const char32_t c = p_value[i];
		if (c < '0' || c > '9' || r_value > (INT64_MAX - (c - '0')) / 10) return false;
		r_value = r_value * 10 + c - '0';
	}
	return true;
}

// Detect control characters forbidden in header values.
bool value_ok(const String &p_value) {
	for (int i = 0; i < p_value.length(); i++) {
		const char32_t c = p_value[i];
		if ((c < 32 && c != '\t') || c == 127) return false;
	}
	return true;
}
}

// Validate ASCII HTTP tokens while accepting unfamiliar valid methods.
bool GDHTTPPeer::token(const String &p_text) {
	if (p_text.is_empty()) return false;
	const String marks = "!#$%&'*+-.^_`|~";
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || marks.contains(String::chr(c)))) return false;
	}
	return true;
}

// Close on failure instead of carrying unconsumed bytes into another request.
void GDHTTPPeer::fail() {
	if (link.is_valid()) { if (link->h2) link->detach(this); else link->wire.close(); }
	status = STATUS_CONNECTION_ERROR;
}

// Schedule buffered work without requiring another kernel notification.
void GDHTTPPeer::again() { if (callback.is_valid()) Async::post(Ref<RefCounted>(this), callback); }

// Use one delivery target for network readiness and internal continuations.
void GDHTTPPeer::set_callback(const Callable &p_callback) {
	callback = p_callback;
	if (link.is_valid() && !link->h2) link->wire.set_wait_callback(callback);
}

// Delegate DNS and TCP to the shared dialer, starting TLS only after TCP connects.
Error GDHTTPPeer::open(const String &p_host, int p_port, bool p_secure, uint64_t p_due) {
	close();
	link.instantiate();
	link->wire.set_wait_callback(callback);
	host = p_host;
	port = p_port;
	secure = p_secure;
	status = STATUS_CONNECTING;
	return link->wire.open(host, port, p_due);
}

// Store the request line and headers in a small buffer while sharing the body.
Error GDHTTPPeer::request(const String &p_method, const String &p_target, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (status != STATUS_CONNECTED || !token(p_method)) return ERR_INVALID_PARAMETER;
	for (int i = 0; i < p_target.length(); i++) if (p_target[i] <= 32 || p_target[i] == 127) return ERR_INVALID_PARAMETER;
	if (p_target.is_empty()) return ERR_INVALID_PARAMETER;
	method = p_method;
	const String name = host.contains(":") && !host.begins_with("[") ? "[" + host + "]" : host;
	if (link->h2) {
		fields.clear();
		auto add = [&](const char *key, const String &value) {
			const CharString bytes = value.utf8();
			fields.push_back({key, std::string(bytes.get_data(), bytes.length())});
		};
		add(":method", method);
		add(":authority", method == "CONNECT" ? p_target : name + (port == 443 ? String() : ":" + itos(port)));
		if (method != "CONNECT") { add(":scheme", "https"); add(":path", p_target); }
		bool has_length = false;
		for (const String &field : p_headers) {
			const int colon = field.find(":");
			if (colon <= 0 || !token(field.substr(0, colon)) || !value_ok(field.substr(colon+1))) return ERR_INVALID_PARAMETER;
			const String key = field.substr(0,colon).to_lower(), value = field.substr(colon+1).strip_edges();
			if (key == "connection" || key == "proxy-connection" || key == "keep-alive" || key == "upgrade" || key == "transfer-encoding") continue;
			if (key == "te" && value != "trailers") return ERR_INVALID_PARAMETER;
			has_length |= key == "content-length";
			add(key.utf8().get_data(), value);
		}
		if (!has_length && (p_body.size() || method == "POST" || method == "PUT" || method == "PATCH")) add("content-length", itos(p_body.size()));
		request_head.clear();
		request_body = p_body;
		head_at = body_at = 0;
		ready = stream_end = false;
		code = 0;
		length = -1;
		headers.clear();
		status = STATUS_REQUESTING;
		link->kick();
		return OK;
	}
	String head = method + " " + p_target + " HTTP/1.1\r\nHost: " + name;
	if (port != (secure ? 443 : 80)) head += ":" + itos(port);
	head += "\r\n";
	bool has_length = false;
	for (const String &field : p_headers) {
		const int colon = field.find(":");
		if (colon <= 0 || !token(field.substr(0, colon)) || !value_ok(field.substr(colon + 1))) return ERR_INVALID_PARAMETER;
		has_length = has_length || field.substr(0, colon).to_lower() == "content-length";
		head += field + "\r\n";
	}
	if (!has_length && (p_body.size() || method == "POST" || method == "PUT" || method == "PATCH")) head += "Content-Length: " + itos(p_body.size()) + "\r\n";
	request_head = (head + "\r\n").to_utf8_buffer();
	request_body = p_body;
	head_at = body_at = 0;
	ready = false;
	code = 0;
	headers.clear();
	header_bytes = 0;
	length = -1;
	chunked = close_body = eof = false;
	chunk_state = 0;
	excess = trailer_bytes = 0;
	status = STATUS_REQUESTING;
	link->wire.read_wait(true);
	link->wire.write_wait(true);
	again();
	return OK;
}

// Compact consumed input only when necessary and retain the unfinished-line scan position.
bool GDHTTPPeer::fill(int p_size) {
	if (eof || status == STATUS_CONNECTION_ERROR) return false;
	if (at > 0) {
		const int rest = input.size() - at;
		if (rest) memmove(input.ptrw(), input.ptr() + at, rest);
		input.resize(rest);
		scan = MAX(0, scan - at);
		at = 0;
	}
	link->wire.poll();
	if (link->wire.state() == Wire::CLOSED) { eof = true; return false; }
	if (!link->wire.is_ready()) { fail(); return false; }
	const int start = input.size();
	if (input.resize(start + p_size) != OK) { fail(); return false; }
	int got = 0;
	const Error error = link->wire.read(input.ptrw() + start, p_size, got);
	input.resize(start + got);
	if (error == ERR_FILE_EOF) eof = true;
	else if (error != OK && error != ERR_BUSY) fail();
	return got > 0;
}

// Accept LF for headers but require CRLF for chunk-size lines.
bool GDHTTPPeer::line(String &r_line, int p_limit, bool p_crlf) {
	for (;;) {
		for (int i = MAX(at, scan); i < input.size(); i++) {
			if (input[i] != '\n') continue;
			const bool cr = i > at && input[i - 1] == '\r';
			const int end = i - (cr ? 1 : 0);
			if (end - at >= p_limit || (p_crlf && !cr)) { fail(); return false; }
			for (int j = at; j < end; j++) if (input[j] == '\r') { fail(); return false; }
			r_line = String::utf8(reinterpret_cast<const char *>(input.ptr()) + at, end - at);
			at = scan = i + 1;
			return true;
		}
		scan = input.size();
		if (input.size() - at >= p_limit) { fail(); return false; }
		if (!fill()) return false;
	}
}

// Read informational responses through the final response, checking duplicate lengths and transfer-encoding precedence.
bool GDHTTPPeer::parse_headers() {
	String value;
	while (line(value, int(HEADER_MAX - header_bytes))) {
		header_bytes += value.utf8().length() + 2;
		if (header_bytes > HEADER_MAX) { fail(); return false; }
		if (code == 0) {
			if (value.length() < 12 || (!value.begins_with("HTTP/1.1 ") && !value.begins_with("HTTP/1.0 ")) ||
					(value.length() > 12 && value[12] != ' ')) { fail(); return false; }
			int64_t number = 0;
			if (!decimal(value.substr(9, 3), number) || number < 100) { fail(); return false; }
			code = int(number);
			old = value[7] == '0';
			continue;
		}
		if (value.is_empty()) {
			String declared;
			for (auto *field = headers.front(); field;) {
				auto *next = field->next();
				const String text = field->get();
				const int colon = text.find(":");
				const String name = text.substr(0, colon).to_lower();
				const String val = text.substr(colon + 1).strip_edges();
				if (name == "content-length") {
					int64_t n = 0;
					if (!decimal(val, n) || (!declared.is_empty() && declared != val)) { fail(); return false; }
					if (!declared.is_empty()) headers.erase(field);
					declared = val;
					length = n;
				} else if (name == "transfer-encoding" && !old) {
					if (chunked || val.to_lower() != "chunked") { fail(); return false; }
					chunked = true;
				}
				field = next;
			}
			if (code < 200 && code != 101) { code = 0; headers.clear(); length = -1; chunked = false; continue; }
			const bool empty = method == "HEAD" || code / 100 == 1 || code == 204 || code == 304;
			if (chunked) {
				for (auto *field = headers.front(); field;) {
					auto *next = field->next();
					if (field->get().substr(0, field->get().find(":")).to_lower() == "content-length") headers.erase(field);
					field = next;
				}
				length = -1;
			}
			if (empty) { length = 0; chunked = false; }
			close_body = !chunked && length < 0;
			left = length >= 0 ? uint64_t(length) : 0;
			status = !chunked && length == 0 ? STATUS_CONNECTED : STATUS_BODY;
			ready = true;
			return true;
		}
		if (value.begins_with(" ") || value.begins_with("\t")) {
			if (headers.is_empty() || !value_ok(value)) { fail(); return false; }
			headers.back()->get() += " " + value.strip_edges();
			continue;
		}
		const int colon = value.find(":");
		if (colon <= 0 || !token(value.substr(0, colon)) || !value_ok(value.substr(colon + 1))) { fail(); return false; }
		headers.push_back(value);
	}
	if (eof) fail();
	return false;
}

// Advance connection, partial request writes, and response headers within event-loop turns.
void GDHTTPPeer::poll() {
	if (status == STATUS_DISCONNECTED || status == STATUS_CONNECTION_ERROR) return;
	if (link->h2) { link->kick(); return; }
	link->wire.poll();
	if (status == STATUS_CONNECTING) {
		if (link->wire.state() == Wire::LINKING) return;
		if (!link->wire.is_ready()) { fail(); return; }
		if (secure && !link->wire.is_wrapped()) {
			PackedStringArray protocols; protocols.push_back("h2"); protocols.push_back("http/1.1");
			if (link->wire.wrap(host, Wire::VERIFY, Ref<GDTrust>(), protocols) != OK) fail();
			return;
		}
		if (link->wire.protocol() == "h2") {
			if (!link->wire.multiplex_compatible()) { fail(); return; }
			link->enable();
			link->attach(this);
		}
		status = STATUS_CONNECTED;
		return;
	}
	if (status == STATUS_CONNECTED) {
		if (link->wire.state() == Wire::CLOSED) status = STATUS_DISCONNECTED;
		else if (link->wire.state() == Wire::FAILED) fail();
		return;
	}
	if (status != STATUS_REQUESTING) return;
	// Read early responses independently of body writes so a peer returning 413 need not keep receiving.
	if (input.size() > at || link->wire.available() > 0) {
		parse_headers();
		if (ready || status == STATUS_CONNECTION_ERROR) { link->wire.write_wait(false); return; }
	}
	const uint64_t until = GDClock::usec() + GD_SCHED_SLICE_USEC;
	while (head_at < request_head.size() || body_at < request_body.size()) {
		const bool head = head_at < request_head.size();
		const PackedByteArray &data = head ? request_head : request_body;
		int64_t &offset = head ? head_at : body_at;
		int sent = 0;
		if (link->wire.send(data.ptr() + offset, int(MIN(data.size() - offset, int64_t(1) << 30)), sent) != OK) { fail(); return; }
		offset += sent;
		if (!sent) { parse_headers(); if (ready) link->wire.write_wait(false); return; }
		if (GDClock::usec() >= until) { again(); return; }
	}
	link->wire.write_wait(false);
	parse_headers();
}

// Expose fixed-length, EOF-delimited, and chunked bodies as incremental byte sequences.
PackedByteArray GDHTTPPeer::read_response_body_chunk() {
	PackedByteArray out;
	if (status != STATUS_BODY) return out;
	if (link->h2) {
		if (chunks.empty()) {
			if (stream_end) status = STATUS_CONNECTED;
			else link->kick();
			return out;
		}
		const auto &chunk = chunks.front();
		const size_t count = std::min<size_t>(COPY_SIZE, chunk.size()-chunk_at);
		out.resize(count);
		memcpy(out.ptrw(), chunk.data()+chunk_at, count);
		chunk_at += count;
		if (chunk_at == chunk.size()) { chunks.pop_front(); chunk_at = 0; }
		link->h2->consume(stream,count);
		link->kick();
		if (chunks.empty() && stream_end) status = STATUS_CONNECTED;
		return out;
	}
	for (;;) {
		if (chunked && (chunk_state == 0 || chunk_state == 3)) {
			String text;
			if (!line(text, chunk_state == 0 ? CHUNK_LINE : TRAILER_MAX, true)) {
				if (eof) fail();
				return out;
			}
			if (chunk_state == 3) {
				trailer_bytes += text.utf8().length() + 2;
				if (trailer_bytes > TRAILER_MAX) { fail(); return out; }
				if (text.is_empty()) { status = STATUS_CONNECTED; return out; }
				const int colon = text.find(":");
				if (colon <= 0 || !token(text.substr(0, colon)) || !value_ok(text.substr(colon + 1))) { fail(); return out; }
				continue;
			}
			const int overhead = text.utf8().length() + 2;
			text = text.strip_edges(false, true).get_slice(";", 0);
			if (text.is_empty() || text.length() > 16) { fail(); return out; }
			left = 0;
			for (int i = 0; i < text.length(); i++) {
				const char32_t c = text[i];
				const int digit = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
				if (digit < 0 || left > (UINT64_MAX - digit) / 16) { fail(); return out; }
				left = left * 16 + digit;
			}
			const int64_t cost = excess + overhead - 16; // Allow 16 bytes of framing overhead per chunk.
			excess = cost <= 0 || left >= uint64_t((cost + 1) / 2) ? 0 : cost - int64_t(2 * left);
			if (excess > 16 * 1024) { fail(); return out; } // Reject excessive accumulated non-data framing overhead.
			chunk_state = left ? 1 : 3;
			continue;
		}
		if (chunked && chunk_state == 2) {
			while (input.size() - at < 2) if (!fill()) { if (eof) fail(); return out; }
			if (input[at] != '\r' || input[at + 1] != '\n') { fail(); return out; }
			at += 2;
			scan = at;
			chunk_state = 0;
			continue;
		}
		if (input.size() == at && !fill(COPY_SIZE)) {
			if (eof) { if (close_body) status = STATUS_DISCONNECTED; else fail(); }
			return out;
		}
		int size = MIN(COPY_SIZE, input.size() - at);
		if (!close_body) size = int(MIN(uint64_t(size), left));
		if (out.resize(size) != OK) { fail(); return PackedByteArray(); }
		memcpy(out.ptrw(), input.ptr() + at, size);
		at += size;
		scan = at;
		if (!close_body) {
			left -= size;
			if (!left) { if (chunked) chunk_state = 2; else status = STATUS_CONNECTED; }
		}
		return out;
	}
}

// Release pending connection work and buffers.
void GDHTTPPeer::close() {
	if (link.is_valid()) { if (link->h2) link->detach(this); else link->wire.close(); }
	link.unref();
	status = STATUS_DISCONNECTED;
	input.clear(); chunks.clear(); fields.clear();
	stream = 0; at = scan = 0; chunk_at = 0; upload_queued = false;
	replay = false;
}

// Reserve a fresh logical result without modifying another request's callback or body cursor.
Ref<GDHTTPPeer> GDHTTPPeer::reserve(const Ref<GDHTTPLink> &connection, const String &name, int number) {
	if (connection.is_null() || !connection->available()) return Ref<GDHTTPPeer>();
	Ref<GDHTTPPeer> peer; peer.instantiate();
	peer->link = connection; peer->host = name; peer->port = number; peer->secure = true;
	peer->status = STATUS_CONNECTED;
	connection->attach(peer.ptr());
	return peer;
}

// Apply storage backpressure only to the current response, leaving connection control reads active.
void GDHTTPPeer::read_wait(bool on) {
	read_on = on;
	if (link.is_null()) return;
	if (!link->h2) link->wire.read_wait(on);
	else if (on && (!chunks.empty() || stream_end)) again();
}

// Keep multiplexed control bytes out of the HTTP/1 idle cleanliness check.
int GDHTTPPeer::available() const {
	return link.is_null() || link->h2 ? 0 : input.size()-at+link->wire.available();
}

// Reclaim resources when the last connection owner leaves.
GDHTTPPeer::~GDHTTPPeer() { close(); }
