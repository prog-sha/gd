/**************************************************************************/
/*  fetch.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement client-side HTTP requests declared in fetch.h.

#include "cli/net/fetch.h"
#include "cli/sys/clock.h"

#include "cli/api/text.h"
#include "cli/data/bytes.h"
#include "cli/data/codec.h"
#include "cli/sys/limit.h"
#include "cli/sys/sched.h"
#include "cli/sys/file_job.h"
#include "cli/sys/os.h"
#include "cli/sys/task.h"
#include "cli/sys/wait.h" // IdleWait schedules body continuations.

#include "cli/data/digest.h"
#include "cli/data/hash_core.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"

namespace {

HashSet<GDHTTPCall *> http_calls; // Unfinished requests to cancel at shutdown.

constexpr uint64_t WAIT_MS = 30000; // Default request timeout in milliseconds.
constexpr int64_t SINK_PENDING_MAX = 4 * 1024 * 1024; // Pending writer bytes allowed before applying backpressure.
constexpr int IDLE_MAX = 100; // Maximum idle connections across all origins.
constexpr int IDLE_HOST_MAX = 2; // Maximum idle connections per origin.
constexpr uint64_t IDLE_MS = 90000; // Idle connection retention in milliseconds.
constexpr unsigned H2_RETRIES = 7; // Maximum replay attempts for unprocessed multiplexed requests.

// Construct a deadline without overflow; zero means no deadline.
uint64_t deadline_after(uint64_t p_wait) {
	const uint64_t now = GDClock::msec();
	return p_wait == 0 ? 0 : (p_wait > UINT64_MAX - now ? UINT64_MAX : now + p_wait);
}

// Validate header characters to prevent CR, LF, or NUL from splitting requests.
bool head_field_ok(const String &p_s) {
	for (int i = 0; i < p_s.length(); i++) {
		const char32_t c = p_s[i];
		if ((c < 0x20 && c != '\t') || c == 0x7f) {
			return false;
		}
	}
	return true;
}

// Require token header names and prevent callers from overriding framing headers.
bool head_name_ok(const String &p_name) {
	if (p_name.is_empty()) {
		return false;
	}
	const String separators = "()<>@,;:\\\"/[]?={} \t";
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if (c <= 0x20 || c >= 0x7f || separators.contains(String::chr(c))) {
			return false;
		}
	}
	const String low = p_name.to_lower();
	return low != "host" && low != "content-length" && low != "transfer-encoding";
}

// Check for a hexadecimal SHA-256 digest.
bool sha256_ok(const String &p_value) {
	if (p_value.length() != 64) {
		return false;
	}
	for (int i = 0; i < p_value.length(); i++) {
		const char32_t c = p_value[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	return true;
}

// Parse the destination, returning an empty dictionary on failure.
Dictionary split_url(const String &p_raw) {
	// Delegate URL parsing to Url::parse to share authority, user-info, and IPv6 interpretation.
	// A separate interpretation could connect to a host other than the one validated.
	const Ref<R> got = Url::parse(p_raw);
	if (got->get_e().is_valid()) {
		return Dictionary();
	}
	const Dictionary u = got->get_v();
	Dictionary out;
	out["scheme"] = u["scheme"];
	out["host"] = u["host"];
	out["port"] = u["port"];
	out["target"] = Url::request_target(u); // Do not transmit URL fragments.
	return out;
}

} // namespace

// Monitor disconnects only while idle connections exist.
void GDHTTPTransport::watch(bool p_on) {
	watching = p_on;
	for (const Idle &slot : idle) slot.client->set_callback(p_on ? callable_mp(this, &GDHTTPTransport::step) : Callable());
}

// Close expired or disconnected idle connections.
void GDHTTPTransport::prune() {
	const uint64_t now = GDClock::msec();
	for (auto *item = shared.front(); item;) {
		auto *next = item->next();
		const Ref<GDHTTPLink> link = item->get().link;
		if (!link->alive() || (!link->busy() && link->idle_at && now-link->idle_at >= IDLE_MS)) { link->idle_call = Callable(); shared.erase(item); }
		item = next;
	}
	for (List<Idle>::Element *item = idle.front(); item;) {
		List<Idle>::Element *next = item->next();
		const Idle &slot = item->get();
		if (slot.client.is_valid()) {
			slot.client->poll();
		}
		const bool connected = slot.client.is_valid() && slot.client->get_status() == GDHTTPPeer::STATUS_CONNECTED;
		const bool dirty = connected && slot.client->available() > 0;
		if (slot.expires <= now || !connected || dirty) {
			if (slot.client.is_valid()) {
				slot.client->close();
			}
			idle.erase(item);
		}
		item = next;
	}
}

// Register only the earliest idle-connection expiry.
void GDHTTPTransport::arm() {
	if (expiry.is_valid()) {
		expiry->abandon();
		expiry.unref();
	}
	const uint64_t now = GDClock::msec();
	uint64_t due = idle.is_empty() ? 0 : idle.front()->get().expires;
	for (const Shared &slot : shared) {
		if (!slot.link->busy() && slot.link->idle_at) {
			const uint64_t end = slot.link->idle_at + IDLE_MS;
			if (!due || end < due) due = end;
		}
	}
	if (!due) { watch(!idle.is_empty()); return; }
	const double sec = due > now ? double(due - now) / 1000.0 : 0.0;
	expiry = Async::start_sleep(sec);
	Signal(expiry.ptr(), "finished").connect(callable_mp(this, &GDHTTPTransport::expire).unbind(1), Object::CONNECT_ONE_SHOT);
	watch(true);
}

// Close expired connections on timer delivery and arm the next expiry.
void GDHTTPTransport::expire() {
	expiry.unref();
	prune();
	arm();
}

// Reflect socket activity and disconnects in GDHTTPPeer.
void GDHTTPTransport::step() {
	prune();
	if (idle.is_empty()) {
		arm();
	}
}

// Take the oldest idle connection for the same origin.
Ref<GDHTTPPeer> GDHTTPTransport::take(const String &p_origin, const String &p_host, int p_port) {
	prune();
	for (const Shared &slot : shared) {
		if (slot.origin != p_origin) continue;
		Ref<GDHTTPPeer> peer = GDHTTPPeer::reserve(slot.link, p_host, p_port);
		if (peer.is_valid()) { arm(); return peer; }
	}
	for (List<Idle>::Element *item = idle.front(); item; item = item->next()) {
		if (item->get().origin == p_origin) {
			const Ref<GDHTTPPeer> out = item->get().client;
			idle.erase(item);
			out->set_callback(Callable());
			arm();
			return out;
		}
	}
	return Ref<GDHTTPPeer>();
}

// Publish a physical multiplexed link once while individual request peers retain their own callbacks.
void GDHTTPTransport::share(const String &origin, const Ref<GDHTTPLink> &link) {
	if (link.is_null() || !link->alive()) return;
	for (const Shared &slot : shared) if (slot.link == link) return;
	link->idle_call = callable_mp(this, &GDHTTPTransport::expire);
	shared.push_back(Shared{origin, link});
}

// Pool fully consumed connections within the idle limits.
void GDHTTPTransport::give(const String &p_origin, const Ref<GDHTTPPeer> &p_client) {
	if (p_client.is_valid() && p_client->multiplex_link().is_valid()) {
		const Ref<GDHTTPLink> link = p_client->multiplex_link();
		p_client->close();
		prune();
		if (link->alive()) share(p_origin, link);
		arm();
		return;
	}
	if (p_client.is_null() || p_client->get_status() != GDHTTPPeer::STATUS_CONNECTED) {
		if (p_client.is_valid()) {
			p_client->close();
		}
		return;
	}
	prune();
	int same = 0;
	for (const Idle &slot : idle) {
		if (slot.origin == p_origin) {
			same++;
		}
	}
	if (idle.size() >= IDLE_MAX || same >= IDLE_HOST_MAX) {
		p_client->close();
		return;
	}
	Idle slot;
	slot.origin = p_origin;
	slot.client = p_client;
	slot.expires = deadline_after(IDLE_MS);
	idle.push_back(slot);
	p_client->read_wait(true); // Restore read notifications paused for storage so idle disconnects can be detected.
	arm();
}

// Close every retained connection.
void GDHTTPTransport::clear() {
	for (const Shared &slot : shared) slot.link->idle_call = Callable();
	shared.clear();
	if (expiry.is_valid()) {
		expiry->abandon();
		expiry.unref();
	}
	watch(false);
	for (const Idle &slot : idle) {
		if (slot.client.is_valid()) {
			slot.client->close();
		}
	}
	idle.clear();
}

// Leave no idle connections during singleton shutdown.
GDHTTPTransport::~GDHTTPTransport() {
	clear();
}

// ---------------- Responses ----------------

// Decode body bytes as text, rejecting only conversions beyond the basic String representation.
String GDHTTPResponse::text() const {
	ERR_FAIL_COND_V_MSG(body.size() > INT_MAX - 1, String(), "HTTP body exceeds the native string capacity");
	return String::utf8((const char *)body.ptr(), int(body.size()));
}

// Decode the response body as JSON.
Ref<R> GDHTTPResponse::json() const {
	return json_of(body);
}

// Register public script methods and properties.
void GDHTTPResponse::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status"), &GDHTTPResponse::get_status);
	ClassDB::bind_method(D_METHOD("get_headers"), &GDHTTPResponse::get_headers);
	ClassDB::bind_method(D_METHOD("get_body"), &GDHTTPResponse::get_body);
	ClassDB::bind_method(D_METHOD("ok"), &GDHTTPResponse::ok);
	ClassDB::bind_method(D_METHOD("text"), &GDHTTPResponse::text);
	ClassDB::bind_method(D_METHOD("json"), &GDHTTPResponse::json);
	ADD_RESULT("json", "Variant");

	ADD_PROPERTY(PropertyInfo(Variant::INT, "status"), "", "get_status");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "headers"), "", "get_headers");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "body"), "", "get_body");
}

// ---------------- Requests ----------------

void GDHTTPCall::watch(bool p_on) {
	if (client.is_valid()) client->set_callback(p_on ? callable_mp(this, &GDHTTPCall::step) : Callable());
	if (p_on) Async::post(Ref<RefCounted>(this), callable_mp(this, &GDHTTPCall::step));
}

// Validate the URL and options, then start an HTTP request.
void GDHTTPCall::begin(const String &p_url, const Dictionary &p_opts, const Ref<GDHTTPTransport> &p_transport) {
	res.instantiate();
	self_hold = Ref<GDHTTPCall>(this);
	http_calls.insert(this);
	transport = p_transport;
	const double timeout = p_opts.get("timeout", double(WAIT_MS) / 1000.0);
	uint64_t wait = 0;
	if (!Limit::seconds_ms(timeout, wait)) {
		error = Err::make("timeout must be zero or a positive number of seconds", Err::INVALID_DATA);
		stage = DONE;
		watch(true);
		return;
	}
	set_due(wait);
	watch(true);

	const Dictionary parts = split_url(p_url);
	if (parts.is_empty()) {
		error = Err::make(vformat("no scheme or host in \"%s\"", p_url), Err::INVALID_DATA);
		stage = DONE;
		return;
	}
	host = parts["host"];
	target = parts["target"];
	port = parts["port"];
	tls_enabled = String(parts["scheme"]) == "https";
	origin = vformat("%s://%s:%d", String(parts["scheme"]).to_lower(), host.to_lower(), port);
	body_max = int64_t(p_opts.get("max_body", body_max));
	if (body_max < 0) {
		error = Err::make("max_body must not be negative", Err::INVALID_DATA);
		stage = DONE;
		return;
	}
	save_path = p_opts.get("save", "");
	want_sha = String(p_opts.get("sha256", "")).to_lower();
	if ((!want_sha.is_empty() && !sha256_ok(want_sha)) || (!want_sha.is_empty() && save_path.is_empty())) {
		error = Err::make("sha256 requires save and 64 hexadecimal characters", Err::INVALID_DATA);
		stage = DONE;
		return;
	}
	const String wanted = p_opts.get("method", "GET");
	if (!GDHTTPPeer::token(wanted)) {
		error = Err::make("invalid HTTP method", Err::INVALID_DATA);
		stage = DONE;
		return;
	}
	method = wanted;
	if (method == "CONNECT") {
		target = p_opts.get("authority", vformat("%s:%d", host, (int)parts["port"]));
	}
	// Reject line breaks in the request target to prevent request splitting.
	if (!head_field_ok(target) || !head_field_ok(host)) {
		error = Err::make(vformat("bad characters in \"%s\"", p_url), Err::INVALID_DATA);
		stage = DONE;
		return;
	}

	// Reject malformed headers before sending any part of the request.
	const Dictionary given = p_opts.get("headers", Dictionary());
	for (const Variant &k : given.keys()) {
		const String name = String(k);
		const String value = String(given[k]);
		if (!head_name_ok(name) || !head_field_ok(value)) {
			error = Err::make("invalid HTTP request header", Err::INVALID_DATA);
			stage = DONE;
			return;
		}
		if (name.to_lower() == "connection") {
			for (const String &token : value.to_lower().split(",")) {
				peer_close = peer_close || token.strip_edges() == "close";
			}
		}
		head.push_back(vformat("%s: %s", name, value));
	}
	// Accept text and binary bodies without stringifying raw bytes.
	// A byte array must remain binary rather than becoming text such as [7, 0, 255, ...].
	const Variant body_in = p_opts.get("body", Variant());
	if (body_in.get_type() == Variant::PACKED_BYTE_ARRAY) {
		send_body = body_in;
	} else if (body_in.get_type() != Variant::NIL) {
		send_body = String(body_in).to_utf8_buffer();
	}
	if (!send_body.is_empty()) {
		head.push_back(vformat("Content-Length: %d", send_body.size()));
	}

	client = transport.is_valid() ? transport->take(origin, host, port) : Ref<GDHTTPPeer>();
	reused = client.is_valid();
	if (reused) client->set_callback(callable_mp(this, &GDHTTPCall::step));
	if (client.is_null() && !open_client()) {
		stage = DONE;
	}
}

// Begin opening a new HTTP connection to the destination.
bool GDHTTPCall::open_client() {
	client.instantiate();
	client->set_callback(callable_mp(this, &GDHTTPCall::step));
	const Error opened = client->open(host, port, tls_enabled, deadline);
	if (opened == OK) return true;
	error = client->error().is_valid() ? client->error() : Err::make(vformat("cannot reach %s", host), Err::of(opened));
	return false;
}

// Register the request deadline; zero waits until caller cancellation.
void GDHTTPCall::set_due(uint64_t p_wait) {
	Async::drop_deadline(this, deadline);
	deadline = deadline_after(p_wait);
	Async::track_deadline(this, deadline, callable_mp(this, &GDHTTPCall::step));
}

// Replay retained bodies only after peer-confirmed rejection or a safe stale-connection failure.
bool GDHTTPCall::retry() {
	if (client->can_replay() && res->status_code == 0 && received == 0) {
		if (retries >= H2_RETRIES) return false;
		client->close();
		reused = false;
		stage = RETRYING;
		const unsigned attempt = retries++;
		if (!attempt) retry_ready();
		else {
			uint32_t noise = 0; // Independent jitter to avoid synchronized reconnects.
			if (!GDCrypto::random_fill(reinterpret_cast<uint8_t *>(&noise), sizeof(noise))) return false;
			const double base = double(uint64_t(1) << (attempt-1));
			const double seconds = uint64_t(base + base * 0.1 * (double(noise) / 4294967296.0));
			retry_wait = Async::start_sleep(seconds);
			Signal(retry_wait.ptr(), "finished").connect(callable_mp(this, &GDHTTPCall::retry_ready).unbind(1), Object::CONNECT_ONE_SHOT);
		}
		return true;
	}
	if (client->multiplex_link().is_valid()) return false;
	const bool safe = method == "GET" || method == "HEAD" || method == "PUT" || method == "DELETE" || method == "OPTIONS" || method == "TRACE";
	if (!reused || !safe || res->status_code != 0 || received != 0) {
		return false;
	}
	client->close();
	reused = false;
	stage = CONNECTING;
	return open_client();
}

// Resume transport creation without blocking other requests during backoff.
void GDHTTPCall::retry_ready() {
	retry_wait.unref();
	if (stage != RETRYING) return;
	stage = CONNECTING;
	if (!open_client()) done(error);
}

// Advance the connection, header, and body-storage state machine.
void GDHTTPCall::step() {
	if (self_hold.is_null()) return;
	Ref<GDHTTPCall> held(this); // Retain state across callbacks that may close the request.
	if (stage == DONE) {
		done(error);
		return;
	}
	if (deadline > 0 && GDClock::msec() >= deadline) {
		done(vformat("%s did not answer in time", host), Err::TIMED_OUT);
		return;
	}
	if (stage == RETRYING) return;
	client->poll();

	switch (stage) {
		case RETRYING: break;
		case CONNECTING: {
			const GDHTTPPeer::Status st = client->get_status();
			if (st == GDHTTPPeer::STATUS_DISCONNECTED || st == GDHTTPPeer::STATUS_CONNECTION_ERROR) {
				if (retry()) {
						return; // Replace only a stale idle connection, once.
				}
				done(error.is_valid() ? error : client->error().is_valid() ? client->error() : Err::make(vformat("connection failed to %s", host), Err::NONE));
				return;
			}
			if (st != GDHTTPPeer::STATUS_CONNECTED) {
				return; // Connection is not ready yet.
			}
			if (transport.is_valid() && client->multiplex_link().is_valid()) transport->share(origin, client->multiplex_link());
			const Error requested = client->request(method, target, head, send_body);
			if (requested != OK) {
				if (retry()) {
						return; // Replace keepalive closed by the peer with a new connection.
				}
				done(error.is_valid() ? error : client->error().is_valid() ? client->error() : Err::make("request failed", Err::of(requested)));
				return;
			}
			stage = WAITING;
		} break;

		case WAITING: {
			const GDHTTPPeer::Status st = client->get_status();
			if (st == GDHTTPPeer::STATUS_CONNECTION_ERROR) {
				if (retry()) {
					return; // Replay once after an idle connection closes immediately after transmission.
				}
				done(error.is_valid() ? error : client->error().is_valid() ? client->error() : Err::make("connection lost", Err::NONE));
				return;
			}
			if (!client->has_response()) {
				return; // Response headers have not arrived yet.
			}
			res->status_code = client->get_response_code();
			const bool no_body = method == "HEAD" || res->status_code == 204 || res->status_code == 304;
			body_len = no_body ? 0 : client->get_response_body_length();
			chunked = client->is_response_chunked();
			const int64_t capacity = INT64_MAX; // PackedByteArray and Content-Length representation boundary.
			const int64_t body_limit = body_max > 0 ? MIN(body_max, capacity) : capacity;
			List<String> lines;
			client->get_response_headers(&lines);
			for (const String &line : lines) {
				const int colon = line.find_char(':');
				if (colon <= 0) {
					continue;
				}
				const String name = line.substr(0, colon).strip_edges().to_lower();
				const String val = line.substr(colon + 1).strip_edges();
				if (name == "connection") {
					for (const String &token : val.to_lower().split(",")) {
						if (token.strip_edges() == "close") {
							peer_close = true;
						}
					}
				}
				// Preserve repeated headers such as multiple Set-Cookie fields.
				// Promote the second occurrence to a sequence instead of overwriting the first.
				if (!res->head.has(name)) {
					res->head[name] = val;
					continue;
				}
				const Variant had = res->head[name];
				if (had.get_type() == Variant::ARRAY) {
					Array vals = had;
					vals.push_back(val);
				} else {
					Array vals;
					vals.push_back(had);
					vals.push_back(val);
					res->head[name] = vals;
				}
			}
			if (body_len > body_limit) {
				done(vformat("%s sent more than %d bytes", host, body_limit), Err::LIMITED);
				return;
			}
			if (!save_path.is_empty()) {
				const PackedByteArray random = GDDigest::random(16);
				if (random.size() != 16) {
					done("cannot create a temporary file name");
					return;
				}
				save_next = vformat("%s.%s.next", save_path, Encoding::hex_encode(random));
				save_sink.instantiate();
				save_sink->set_ready_callback(callable_mp(this, &GDHTTPCall::step));
				// Delegate file open and writes to a worker, keeping file I/O off the receiving event loop.
				save_sink->open(save_next, true);
			}
			stage = READING;
		} // Read body bytes received with the headers in the same turn.
		[[fallthrough]];

		case READING: {
			client->read_wait(true);
			// A bodyless response is complete as soon as its headers are parsed.
			// Yield to other connections at the shared scheduler's time-slice boundary.
			const uint64_t until = GDClock::usec() + GD_SCHED_SLICE_USEC;
			while (client->get_status() == GDHTTPPeer::STATUS_BODY) {
				// Apply writer backpressure to receiving so asynchronous I/O stays bounded.
				if (save_sink.is_valid() && save_sink->pending_bytes() >= SINK_PENDING_MAX) {
					client->read_wait(false);
					return; // Writer completion schedules the next turn.
				}
				if (GDClock::usec() >= until) {
					Async::post(held, callable_mp(this, &GDHTTPCall::step)); // Return buffered continuation work to the ready queue.
					return;
				}
				const PackedByteArray chunk = client->read_response_body_chunk();
				if (chunk.is_empty()) {
					if (client->get_status() != GDHTTPPeer::STATUS_BODY) {
						break; // Complete empty bodies and terminal chunks without another socket notification.
					}
					return; // Resume on a later notification.
				}
				// Enforce the explicit body limit and the total-byte representation boundary.
				const int64_t capacity = INT64_MAX; // Byte storage has a capacity independent of text conversion.
				const int64_t body_limit = body_max > 0 ? MIN(body_max, capacity) : capacity;
				if (received > body_limit - chunk.size()) {
					done(vformat("%s sent more than %d bytes", host, body_limit), Err::LIMITED);
					return;
				}
				received += chunk.size();
				if (save_sink.is_valid()) {
					if (save_sink->error().is_valid()) {
						done(save_sink->error());
						return;
					}
					save_sink->push(chunk); // Workers write chunks in submission order.
				} else {
					const int64_t offset = res->body.size();
					if (res->body.resize(received) != OK) {
						done("cannot allocate HTTP response body", Err::LIMITED);
						return;
					}
					memcpy(res->body.ptrw() + offset, chunk.ptr(), chunk.size());
				}
			}
			const GDHTTPPeer::Status st = client->get_status();
			if (st == GDHTTPPeer::STATUS_CONNECTION_ERROR || (body_len >= 0 && received != body_len)) {
				done(client->error().is_valid() ? client->error() : Err::make("response body ended early", Err::INVALID_DATA));
				return;
			}
			if (chunked && st == GDHTTPPeer::STATUS_BODY) {
				return; // Wait until GDHTTPPeer confirms the terminal chunk.
			}
			reusable = !peer_close && method != "CONNECT" && res->status_code != 101 && !client->is_response_http_10() && client->request_sent();
			if (save_sink.is_valid()) {
				save_sink->close(); // Flush remaining bytes before closing the writer.
				stage = FLUSHING;
				client->read_wait(false);
				if (save_sink->done()) Async::post(held, callable_mp(this, &GDHTTPCall::step));
				return; // A failed open may already have delivered its worker notification.
			}
			done(String());
		} break;

		case FLUSHING: {
			// Reception is complete; wait for the worker to finish writing.
			if (!save_sink->done()) {
				return;
			}
			if (save_sink->error().is_valid()) {
				done(save_sink->error());
				return;
			}
			const String got = save_sink->digest();
			if (res->status_code < 200 || res->status_code >= 300) {
				done(String());
				return;
			}
			if (!want_sha.is_empty() && got != want_sha) {
				done("response body SHA-256 does not match", Err::INVALID_DATA);
				return;
			}
			save_sink.unref();
			const String from = save_next;
			const String to = save_path;
			Signal signal = GDFileCall::start([from, to]() { return Os::rename(from, to); });
			signal.connect(callable_mp(this, &GDHTTPCall::moved), Object::CONNECT_ONE_SHOT);
			stage = MOVING;
		} break;

		case MOVING:
			break; // The rename completion signal delivers the result.

		default:
			break;
	}
}

// Commit the saved file after receiving the worker's rename result.
void GDHTTPCall::moved(const Ref<R> &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	if (p_result.is_null() || !p_result->get_ok()) {
		done(p_result.is_valid() && p_result->get_e().is_valid() ? p_result->get_e() : Err::make(vformat("cannot move %s", save_path), Err::NONE));
		return;
	}
	save_next = "";
	done(String());
}

// Release request resources and deliver the final result to its waiter.
void GDHTTPCall::done(const String &p_why, Err::Kind p_kind) {
	done(p_why.is_empty() ? Ref<Err>() : Err::make(p_why, p_kind));
}

// Release request resources while keeping the error and partial response together.
void GDHTTPCall::done(const Ref<Err> &p_error) {
	if (self_hold.is_null()) {
		return;
	}
	// Retain this call until cleanup finishes; releasing the final reference invalidates further access.
	Ref<GDHTTPCall> keep(this);
	watch(false);
	Async::drop_deadline(this, deadline);
	deadline = 0;
	stage = DONE;
	if (retry_wait.is_valid()) { retry_wait->abandon(); retry_wait.unref(); }
	if (p_error.is_valid()) error = p_error;
	if (save_sink.is_valid()) {
		// Leave partial-file cleanup to the writer so deletion cannot race an unfinished open.
		// This also avoids deleting a file while Windows still holds it open.
		save_sink->abort(save_next);
		save_sink.unref();
		save_next = "";
	} else if (!save_next.is_empty()) {
		const String drop = save_next;
		GDFileCall::start([drop]() { return Os::remove(drop); });
		save_next = "";
	}
	if (client.is_valid()) {
		if (error.is_null() && reusable && transport.is_valid()) {
			transport->give(origin, client);
		} else {
			client->close();
		}
		client.unref();
	}
	transport.unref();
	http_calls.erase(this);
	self_hold.unref(); // Make cancellation reentry from completion callbacks harmless.
	emit_signal("finished", error.is_valid() ? R::err(error, Err::NONE, res) : R::ok(res));
}

// Cancel every event-loop request at process shutdown.
void GDHTTPCall::shutdown_all() {
	LocalVector<Ref<GDHTTPCall>> calls;
	for (GDHTTPCall *call : http_calls) {
		calls.push_back(Ref<GDHTTPCall>(call));
	}
	for (const Ref<GDHTTPCall> &call : calls) {
		call->done("process is shutting down", Err::INTERRUPTED);
	}
}

// Complete cancellation as a result when the caller stops waiting.
// Without completion notification, awaiting callers would remain suspended forever.
void GDHTTPCall::cancel() {
	if (self_hold.is_null()) {
		return; // The request is already complete.
	}
	done("cancelled", Err::INTERRUPTED);
}

// Register public script methods and properties.
void GDHTTPCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDHTTPCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "res", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}
