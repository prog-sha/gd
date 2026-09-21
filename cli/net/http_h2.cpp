// Schedule multiplexed requests independently while one physical task owns transport and compression state.
#include "modules/gdscript/gdscript_function.h"
#include "cli/net/http.h"
#include "cli/net/http_fields.h"
#include "cli/api/text.h"
#include "cli/sys/clock.h"
#include "cli/sys/sched.h"
#include "core/object/callable_mp.h"
#include <algorithm>
#include <cstring>

namespace {
// Project split cookie fields and the authoritative host consistently into application header APIs.
void request_fields(std::vector<GDH2::Field> &fields) {
	const bool authority = std::any_of(fields.begin(), fields.end(), [](const GDH2::Field &field) { return field.name == ":authority"; });
	std::vector<GDH2::Field> normalized;
	normalized.reserve(fields.size());
	size_t cookie = SIZE_MAX;
	for (GDH2::Field &field : fields) {
		if (authority && field.name == "host") continue;
		if (field.name == "cookie") {
			if (cookie != SIZE_MAX) { normalized[cookie].value += "; " + field.value; continue; }
			cookie = normalized.size();
		}
		normalized.push_back(std::move(field));
	}
	fields.swap(normalized);
}
}

// Resolve a logical stream without retaining a pointer across application callbacks.
GDWebServer::Conn *GDWebServer::h2_parent(Conn *c) const {
	if (!c || !c->request) return nullptr;
	Conn *const *found = conns.getptr(c->request->parent);
	return found && (*found)->h2 ? *found : nullptr;
}

// Distinguish a dispatchable request from a stale cancellation notification.
bool GDWebServer::has_request(int id) const {
	Conn *const *found = conns.getptr(id);
	return found && (*found)->ready && (!(*found)->request || !(*found)->request->dead);
}

// Coalesce producer notifications while preserving stream round-robin order.
void GDWebServer::h2_schedule(Conn *c) {
	Conn *parent = h2_parent(c);
	if (!parent || c->request->dead) return;
	if (!c->request->writing && !c->ready && !c->request->sent) {
		c->request->writing = true;
		parent->h2->writers.push_back(c->id);
	}
	queue_ready(parent);
	post_ready();
}

// Keep one frame stable until the encrypted transport accepts all of it.
bool GDWebServer::h2_flush(Conn *c) {
	H2Session &session = *c->h2;
	if (session.output.empty() && !session.connection.output(session.output)) {
		c->sock->write_wait(false);
		return true;
	}
	int sent = 0;
	const int size = int(std::min<size_t>(GDStream::MAX_WRITE, session.output.size() - session.at));
	const Error error = c->sock->write(session.output.data() + session.at, size, sent);
	if (error == ERR_BUSY || (error == OK && !sent)) {
		c->sock->write_wait(true);
		return false;
	}
	if (error != OK) { c->sock->close(); return false; }
	// Count only accepted DATA payload, excluding framing and output still queued behind another stream.
	if (session.output.size() >= 9 && session.output[3] == 0) {
		const uint8_t *head = session.output.data();
		const uint32_t stream = (uint32_t(head[5]) << 24) | (uint32_t(head[6]) << 16) | (uint32_t(head[7]) << 8) | head[8];
		auto request = session.requests.find(stream);
		Conn **found = request == session.requests.end() ? nullptr : conns.getptr(request->second);
		if (found && !(*found)->request->dead) {
			(*found)->request->written += std::max<size_t>(session.at + sent, 9) - std::max<size_t>(session.at, 9);
		}
	}
	session.at += sent;
	if (session.at == session.output.size()) { session.connection.sent(session.output); session.output.clear(); session.at = 0; }
	return true;
}

// Allow each ready producer one DATA frame before revisiting any producer.
void GDWebServer::h2_write(Conn *parent) {
	H2Session &session = *parent->h2;
	const size_t count = session.writers.size();
	for (size_t i = 0; i < count; ++i) {
		const int id = session.writers.front();
		session.writers.pop_front();
		Conn **found = conns.getptr(id);
		if (!found || !(*found)->request) continue;
		Conn *c = *found;
		H2Request &request = *c->request;
		request.writing = false;
		if (request.dead || request.sent || c->ready) continue;
		if (c->out_body_at == c->out_body.size()) {
			c->out_body.clear();
			c->out_body_at = 0;
			request.written = 0;
			if (c->out_file.is_valid()) {
				if (!c->out_file->take(c->out_body)) {
					if (!c->out_file->error().is_empty()) {
						session.connection.reset(request.stream, GDH2::INTERNAL_ERROR);
						request.dead = true;
						queue_ready(c);
						continue;
					}
					if (!c->out_file->done()) continue;
				}
			}
		}
		if (c->out_body.is_empty() && (c->out_file.is_null() || c->out_file->done())) {
			request.sent = session.connection.finish(request.stream);
			if (!request.sent) { request.dead = true; queue_ready(c); }
			continue;
		}
		if (c->out_body.is_empty()) { h2_schedule(c); continue; }
		size_t size = c->out_body.size() - c->out_body_at;
		std::vector<uint8_t> *frame = session.connection.reserve(request.stream, size, false);
		if (!frame) continue;
		// Fill only this stream's permitted frame before another connection operation can change its storage.
		for (uint32_t part = c->out_body.find(c->out_body_at); size; ++part) {
			const size_t take = std::min<size_t>(size, c->out_body.part(part).end - c->out_body_at);
			const uint8_t *data = c->out_body.ptr(part, c->out_body_at);
			frame->insert(frame->end(), data, data + take);
			c->out_body_at += take;
			size -= take;
		}
		h2_schedule(c);
	}
}

// Release completed stream storage only after its queued response reached the transport.
void GDWebServer::h2_done(Conn *parent) {
	H2Session &session = *parent->h2;
	if (!session.output.empty() || session.connection.pending()) return;
	LocalVector<int> finished;
	for (const auto &entry : session.requests) {
		Conn **found = conns.getptr(entry.second);
		if (!found || !(*found)->request->sent) continue;
		Conn *c = *found;
		finish_body(c);
		if (c->request->remote_end) {
			size_t unread = 0;
			for (const auto &part : c->request->body) unread += part.size();
			unread -= c->request->at;
			session.connection.consume(entry.first, unread);
			session.connection.retire(entry.first);
		} else session.connection.reset(entry.first, GDH2::NO_ERROR);
		finished.push_back(c->id);
	}
	for (const int id : finished) forget(id);
}

// Advance socket readiness without running application continuations inside frame decoding.
void GDWebServer::h2_poll(Conn *c, PackedInt32Array &ready) {
	H2Session &session = *c->h2;
	const uint64_t outer = GDScriptFunction::native_time_slice_deadline();
	const uint64_t until = outer ? outer : GDClock::usec() + GD_SCHED_SLICE_USEC;
	// A borrowed turn may already be exhausted before the first transport attempt.
	if (outer && GDClock::usec() >= outer) {
		queue_ready(c);
		post_ready();
		return;
	}
	bool reading = true, writing = true;
	while (c->sock->is_open()) {
		if (reading && !session.connection.failed()) {
			uint8_t bytes[READ_CHUNK]; // Incremental transport slice, not a request-size bound.
			int got = 0;
			const Error error = c->sock->read(bytes, sizeof(bytes), got);
			if (error == ERR_BUSY) reading = false;
			else if (error != OK || !got) { c->sock->close(); break; }
			else {
				c->last = GDClock::msec();
				session.connection.feed(bytes, got);
			}
		}
		GDH2::Event event;
		while (session.connection.event(event)) {
			auto it = session.requests.find(event.stream);
			Conn *request = it == session.requests.end() ? nullptr : conns.get(it->second);
			if (event.kind == GDH2::Event::HEADERS && !event.trailers && !request) {
				request = memnew(Conn);
				request->request = std::make_unique<H2Request>();
				H2Request &body = *request->request;
				body.parent = c->id;
				body.stream = event.stream;
				body.fields = std::move(event.fields);
				request_fields(body.fields);
				request->id = next_id++;
				request->sock = c->sock;
				request->ip = c->ip;
				request->need = -1;
				request->began = request->last = GDClock::msec();
				request->ready = true;
				request->bad = event.code;
				request->body_limit = max_body;
				for (const GDH2::Field &field : body.fields) {
					if (field.name == ":path") {
						const size_t end = field.value.find('?');
						const size_t size = end == std::string::npos ? field.value.size() : end;
						if (!Url::decode_check(reinterpret_cast<const uint8_t *>(field.value.data()), size, false, nullptr)) { request->bad = 400; continue; }
					}
					const String value = String::utf8(field.value.data(), field.value.size());
					if (field.name == ":method") body.method = value;
					else if (field.name == ":path") body.target = value;
					else if (field.name == "content-length") request->need = value.to_int();
					else if (field.name == "expect" && value.nocasecmp_to("100-continue") == 0) request->expect_continue = true;
				}
				if (body.method == "CONNECT") {
					for (const GDH2::Field &field : body.fields) if (field.name == ":authority") body.target = String::utf8(field.value.data(), field.value.size());
				}
				const int query = body.target.find("?");
				body.path = query < 0 ? body.target : body.target.substr(0, query);
				if (query >= 0) body.query = body.target.substr(query + 1);
				request->head_only = body.method == "HEAD";
				session.requests.emplace(event.stream, request->id);
				conns.insert(request->id, request);
				c->head_end = 0;
				ready.push_back(request->id);
			} else if (event.kind == GDH2::Event::WRITABLE) {
				for (const auto &entry : session.requests) {
					if (!event.stream || event.stream == entry.first) h2_schedule(conns.get(entry.second));
				}
			} else if (event.kind == GDH2::Event::GOAWAY) session.ending = true;
			if (!request) continue;
			if (event.kind == GDH2::Event::DATA) request->request->body.push_back(std::move(event.data));
			else if (event.kind == GDH2::Event::END) request->request->remote_end = true;
			else if (event.kind == GDH2::Event::RESET) { request->request->dead = true; abort_files(request); }
			queue_ready(request);
		}
		if (writing) {
			if (session.output.empty() && !session.connection.pending()) { h2_done(c); h2_write(c); }
			if (!session.output.empty() || session.connection.pending()) writing = h2_flush(c);
			else writing = false;
		}
		if (GDClock::usec() >= until) {
			if (reading || writing || !session.writers.empty()) queue_ready(c);
			break;
		}
		if (!reading && !writing) break;
	}
	h2_done(c);
	if (head_ms && c->head_end < 0 && GDClock::msec() - c->began >= head_ms) c->sock->close();
	const bool empty = session.output.empty() && !session.connection.pending();
	if (empty && (session.connection.failed() || (session.ending && session.requests.empty()))) c->sock->close();
	if (!c->sock->is_open()) { forget(c->id); return; }
	c->sock->read_wait(!session.connection.failed());
	if (empty) c->sock->write_wait(false);
	if (writing && (!session.writers.empty() || (!session.connection.failed() && !empty))) queue_ready(c);
	refresh_deadline(c);
}

// Consume only requested DATA and return credit when the application releases those bytes.
int GDWebServer::h2_body(Conn *c, int64_t maximum, PackedByteArray &data, bool limited) {
	data.clear();
	Conn *parent = h2_parent(c);
	if (!parent || c->request->dead || !c->sock->is_open()) return BODY_READ_BAD;
	if (limited && c->body_limited) return BODY_READ_LIMIT;
	H2Request &request = *c->request;
	if (request.body.empty() && request.remote_end) { c->body_done = true; c->body_reading = false; return BODY_READ_EOF; }
	c->body_reading = true;
	if (c->expect_continue && !c->continue_sent) {
		c->continue_sent = parent->h2->connection.headers(request.stream, {{":status", "100"}}, false);
		queue_ready(parent);
		post_ready();
	}
	if (request.body.empty()) return BODY_READ_WAIT;
	const auto &part = request.body.front();
	size_t size = std::min<uint64_t>(part.size() - request.at, maximum);
	if (limited) {
		const int64_t left = std::max<int64_t>(0, c->body_limit - c->body_got);
		if (!left && size) {
			c->body_limited = true;
			c->body_reading = false;
			return BODY_READ_LIMIT;
		}
		size = std::min<uint64_t>(size, left);
	}
	if (data.resize_uninitialized(size) != OK) {
		c->bad = 500;
		c->body_reading = false;
		return BODY_READ_BAD;
	}
	std::memcpy(data.ptrw(), part.data() + request.at, size);
	request.at += size;
	c->body_got += size;
	if (request.at == part.size()) { request.body.pop_front(); request.at = 0; }
	parent->h2->connection.consume(request.stream, size);
	c->body_reading = false;
	queue_ready(parent);
	post_ready();
	return BODY_READ_DATA;
}

// Encode response metadata directly and reuse native streaming body producers.
void GDWebServer::h2_reply(Conn *c, int64_t code, const PackedByteArray &body, const String &type, const Dictionary *extra, const Ref<GDBodySource> &file) {
	Conn *parent = h2_parent(c);
	if (!parent || c->request->dead || !c->ready) { if (file.is_valid()) file->abort(); return; }
	const int status = code >= 100 && code <= 999 && code != 101 ? int(code) : 200;
	std::vector<GDH2::Field> fields = {{":status", std::to_string(status)}};
	bool has_type = false, has_server = false, has_date = false;
	http_response_fields(extra, drop_n, [&](const CharString &, const String &name, const CharString &value) {
		if (name == "keep-alive" || name == "proxy-connection" || name == "upgrade") { ++drop_n; return; }
		const CharString key = name.utf8();
		std::string text(value.get_data(), value.length());
		const size_t first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
		fields.push_back({std::string(key.get_data(), key.length()), first == std::string::npos ? std::string() : text.substr(first, last - first + 1)});
		has_type |= name == "content-type";
		has_server |= name == "server";
		has_date |= name == "date";
	});
	if (status < 200) {
		parent->h2->connection.headers(c->request->stream, fields, false);
		h2_reply(c, 200, body, type, extra, file);
		return;
	}
	const bool no_length = status == 204 || status == 205 || status == 304;
	const bool no_body = no_length || c->head_only;
	const int64_t length = file.is_valid() ? file->size() : body.size();
	if (!no_length && length >= 0) fields.push_back({"content-length", std::to_string(length)});
	if (!no_length && !has_type && !type.is_empty()) {
		const CharString value = type.strip_edges().utf8();
		if (http_field_value(value.get_data(), value.length())) fields.push_back({"content-type", std::string(value.get_data(), value.length())});
		else ++drop_n;
	}
	if (!has_server) fields.push_back({"server", "gd"});
	if (!has_date) {
		const char *date = now() + SERVER_LEN + 6;
		fields.push_back({"date", std::string(date, std::strlen(date) - 2)});
	}
	const bool end = no_body || (length == 0 && file.is_null());
	if (!parent->h2->connection.headers(c->request->stream, fields, end)) {
		if (file.is_valid()) file->abort();
		parent->h2->connection.reset(c->request->stream, GDH2::INTERNAL_ERROR);
		c->request->dead = true;
		queue_ready(c);
	} else {
		c->ready = false;
		c->body_reading = false;
		c->request->sent = end;
		if (file.is_valid()) {
			c->out_file = file;
			if (no_body) file->suppress();
			else file->set_ready_callback(callable_mp(this, &GDWebServer::socket_ready).bind(c->id));
		} else if (!no_body) c->out_body = body;
	}
	refresh_deadline(c);
	h2_schedule(c);
	queue_ready(parent);
	post_ready();
}
