// Drive independent client streams over one authenticated connection and shared flow-control state.
#include "cli/net/http_link.h"
#include "cli/net/http_peer.h"
#include "cli/sys/clock.h"
#include "cli/sys/sched.h"
#include "cli/sys/task.h"
#include "core/object/callable_mp.h"
#include <algorithm>

// Install direction-specific receive capacities and a connection-owned readiness callback.
void GDHTTPLink::enable() {
	GDH2::Config config;
	config.server = false;
	config.connection_window = (1 << 30) + 65535;
	config.stream_window = 4 << 20;
	config.header_size = 10 << 20;
	h2 = std::make_unique<GDH2::Connection>(config);
	wire.set_wait_callback(callable_mp(this, &GDHTTPLink::step));
	wire.read_wait(true);
	kick();
}

// Distinguish live draining connections from connections available for new requests.
bool GDHTTPLink::alive() const { return h2 && !closed && !h2->failed() && wire.is_ready(); }

// Account reservations not yet assigned a stream against the peer's remaining capacity.
bool GDHTTPLink::available() const {
	if (!alive()) return false;
	size_t reserved = 0;
	for (const GDHTTPPeer *peer : owners) if (!peer->stream) ++reserved;
	return reserved < h2->capacity();
}

// Retain only a nonowning destination; the peer owns the link and detaches before destruction.
void GDHTTPLink::attach(GDHTTPPeer *peer) { owners.insert(peer); idle_at = 0; }

// Release consumed streams or cancel one unfinished operation without closing its neighbors.
void GDHTTPLink::detach(GDHTTPPeer *peer) {
	if (!owners.erase(peer)) return;
	if (peer->stream) {
		streams.erase(peer->stream);
		if (peer->stream_end && peer->chunks.empty() && peer->request_sent()) h2->retire(peer->stream);
		else h2->reset(peer->stream);
		peer->stream = 0;
	}
	if (owners.empty()) {
		idle_at = GDClock::msec();
		if (idle_call.is_valid()) idle_call.call();
	}
	kick();
}

// Coalesce native continuations and keep their connection alive through delivery.
void GDHTTPLink::kick() {
	if (posted || !h2 || !wire.is_valid()) return;
	posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDHTTPLink::step));
}

// Fail physical transport ownership once and wake all affected logical requests.
void GDHTTPLink::fail() {
	closed = true;
	wire.close();
	for (GDHTTPPeer *peer : owners) {
		if (!peer->stream && !h2->failed()) peer->replay = true;
		if (!peer->stream_end) peer->status = GDHTTPPeer::STATUS_CONNECTION_ERROR;
		peer->again();
	}
}

// Deliver only the affected stream while preserving unread bodies across unrelated readiness.
void GDHTTPLink::events() {
	GDH2::Event event;
	while (h2->event(event)) {
		if (event.kind == GDH2::Event::GOAWAY) {
			closed = true;
			drain_error = drain_error || event.code != GDH2::NO_ERROR;
			for (GDHTTPPeer *peer : owners) {
				if (!peer->stream) {
					peer->replay = !h2->failed();
					peer->status = GDHTTPPeer::STATUS_CONNECTION_ERROR;
					peer->again();
				} else if (peer->stream == 1 && drain_error) peer->replay = false;
			}
			continue;
		}
		if (event.kind == GDH2::Event::WRITABLE) {
			kick();
			for (const auto &entry : streams) {
				GDHTTPPeer *peer = entry.second;
				if ((!event.stream || entry.first == event.stream) && !peer->upload_queued && peer->body_at < peer->request_body.size()) {
					peer->upload_queued = true; writers.push_back(entry.first);
				}
			}
			continue;
		}
		auto found = streams.find(event.stream);
		if (found == streams.end()) continue;
		GDHTTPPeer *peer = found->second;
		if (peer->status == GDHTTPPeer::STATUS_CONNECTION_ERROR) continue;
		if (event.kind == GDH2::Event::HEADERS && !event.trailers) {
			int code = 0;
			for (const auto &field : event.fields) if (field.name == ":status") code = (field.value[0]-'0')*100+(field.value[1]-'0')*10+field.value[2]-'0';
			if (code < 200) {
				for (const auto &field : event.fields) peer->header_bytes += field.name.size() + field.value.size() + 32;
				if (peer->header_bytes > (10 << 20)) {
					h2->reset(peer->stream, GDH2::CANCEL);
					peer->status = GDHTTPPeer::STATUS_CONNECTION_ERROR;
					peer->again();
				}
				continue;
			}
			peer->code = code; peer->ready = true; peer->status = GDHTTPPeer::STATUS_BODY;
			for (const auto &field : event.fields) {
				if (field.name[0] == ':') continue;
				const String value = String::utf8(field.value.data(), field.value.size());
				peer->headers.push_back(String::utf8(field.name.data(), field.name.size()) + ": " + value);
				if (field.name == "content-length") peer->length = value.to_int();
			}
		} else if (event.kind == GDH2::Event::DATA) peer->chunks.push_back(std::move(event.data));
		else if (event.kind == GDH2::Event::END) {
			peer->stream_end = true;
			if (peer->chunks.empty()) peer->status = GDHTTPPeer::STATUS_CONNECTED;
		} else if (event.kind == GDH2::Event::RESET) {
			peer->replay = event.code == GDH2::REFUSED_STREAM;
			peer->status = GDHTTPPeer::STATUS_CONNECTION_ERROR;
		}
		if (peer->read_on || event.kind == GDH2::Event::RESET) peer->again();
	}
}

// Queue one upload frame per ready stream, retaining memory while peer windows are exhausted.
void GDHTTPLink::upload() {
	const size_t count = writers.size();
	for (size_t i = 0; i < count; ++i) {
		const uint32_t id = writers.front(); writers.pop_front();
		auto found = streams.find(id);
		if (found == streams.end()) continue;
		GDHTTPPeer *peer = found->second;
		peer->upload_queued = false;
		if (peer->status == GDHTTPPeer::STATUS_CONNECTION_ERROR) continue;
		const size_t count = h2->write(id, peer->request_body.ptr()+peer->body_at, peer->request_body.size()-peer->body_at, false);
		peer->body_at += count;
		if (peer->body_at == peer->request_body.size()) h2->finish(id);
		else if (count) { peer->upload_queued = true; writers.push_back(id); }
	}
}

// Advance receive and send independently within the shared scheduler slice.
void GDHTTPLink::step() {
	posted = false;
	if (running || !h2) return;
	Ref<GDHTTPLink> held(this);
	running = true;
	wire.poll();
	if (!wire.is_ready()) { fail(); running = false; return; }
	for (GDHTTPPeer *peer : owners) {
		if (peer->stream || peer->fields.empty() || peer->status != GDHTTPPeer::STATUS_REQUESTING) continue;
		if (!h2->capacity()) break;
		peer->stream = h2->open(peer->fields, peer->request_body.is_empty());
		if (!peer->stream) { peer->status = GDHTTPPeer::STATUS_CONNECTION_ERROR; peer->again(); continue; }
		peer->fields.clear();
		streams.emplace(peer->stream, peer);
		if (!peer->request_body.is_empty()) { peer->upload_queued = true; writers.push_back(peer->stream); }
	}
	const uint64_t until = GDClock::usec() + GD_SCHED_SLICE_USEC;
	bool reading = true, writing = true;
	while (wire.is_ready()) {
		if (reading && !h2->failed()) {
			uint8_t data[16384]; // Incremental receive unit, independent of body length.
			int got = 0;
			const Error error = wire.read(data, sizeof(data), got);
			if (error == ERR_BUSY || (error == OK && !got)) reading = false;
			else if (error != OK) { fail(); break; }
			else { h2->feed(data, got); events(); }
		}
		if (writing) {
			if (output.empty() && !h2->pending()) upload();
			if (output.empty()) h2->output(output);
			if (output.empty()) writing = false;
			else {
				int sent = 0;
				const Error error = wire.send(output.data()+at, int(std::min<size_t>(GDStream::MAX_WRITE, output.size()-at)), sent);
				if (error != OK && error != ERR_BUSY) { fail(); break; }
				if (!sent) { writing = false; wire.write_wait(true); }
				else {
					at += sent;
					if (at == output.size()) { h2->sent(output); output.clear(); at = 0; }
				}
			}
		}
		if (GDClock::usec() >= until) { if (reading || writing) kick(); break; }
		if (!reading && !writing) break;
	}
	if (wire.is_ready()) {
		if (output.empty() && !h2->pending()) wire.write_wait(false);
		if (h2->failed() && output.empty() && !h2->pending()) fail();
		else wire.read_wait(!h2->failed());
	}
	running = false;
}

// Relinquish native callbacks before destroying their target.
GDHTTPLink::~GDHTTPLink() { wire.set_wait_callback(Callable()); wire.close(); }
