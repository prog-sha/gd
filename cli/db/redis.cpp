/**************************************************************************/
/*  redis.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement GDRedisClient connections declared in redis.h.

#include "cli/db/redis.h"
#include "cli/sys/system.h"
#include "cli/sys/clock.h"
#include "cli/data/utf8.h"
#include "cli/sys/limit.h"
#include "cli/sys/file_job.h"

#include "cli/data/bytes.h"
#include "cli/sys/perm.h"
#include "cli/sys/sched.h"
#include "cli/sys/task.h" // Async::all for opening connection groups.

#include "core/io/ip.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"

namespace {

HashSet<GDRedisClient *> redis_clients; // Connections requiring shutdown cleanup.
HashSet<GDRedisPool *> redis_pools; // Pools whose connection waiters require shutdown cleanup.

constexpr int SPARE_MAX = 64; // Reusable operation objects retained in reserve.
constexpr int REPLY_BULK_MAX = INT_MAX - 2; // Boundary keeping UTF-8 lengths and CRLF positions representable as int.
constexpr int REDIS_RECV_MAX = INT_MAX; // Receive-buffer boundary for int cursors.
constexpr int64_t REPLY_MEMORY_MAX = INT64_MAX; // Representation boundary for estimated byte accumulation.
constexpr int64_t ARRAY_VALUE_BYTES = sizeof(Variant) * 2; // Per-array-value storage including growth capacity.
constexpr int64_t MAP_VALUE_BYTES = sizeof(Variant) * 3; // Per-map-value storage including hashing and key/value pairs.
constexpr int SEND_MAX = INT_MAX; // Representation boundary for send lengths and cursors.
constexpr int REDIS_READ_CHUNK = 32 * 1024; // Socket read chunk size.

// Identify a missing command value separately from a watched transaction abort.
Ref<R> redis_nil(bool p_transaction = false) {
	Dictionary info;
	info["code"] = p_transaction ? "tx_failed" : "nil";
	return R::err(Err::make(p_transaction ? "transaction aborted" : "value not found",
			p_transaction ? Err::INTERRUPTED : Err::NOT_FOUND, info));
}

// Create a deadline without overflow; zero means no deadline.
uint64_t deadline_after(uint64_t p_wait) {
	const uint64_t now = GDClock::msec();
	return p_wait == 0 ? 0 : (p_wait > UINT64_MAX - now ? UINT64_MAX : now + p_wait);
}

// Parse a RESP decimal integer without extra characters or overflow.
bool resp_int(const String &p_text, int64_t &r_value) {
	if (p_text.is_empty()) {
		return false;
	}
	int at = 0;
	bool neg = false;
	if (p_text[0] == '-') {
		neg = true;
		at = 1;
	}
	if (at >= p_text.length()) {
		return false;
	}
	uint64_t value = 0;
	const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	for (; at < p_text.length(); at++) {
		const char32_t c = p_text[at];
		if (c < '0' || c > '9' || value > (limit - (c - '0')) / 10) {
			return false;
		}
		value = value * 10 + (c - '0');
	}
	r_value = neg ? (value == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)value) : (int64_t)value;
	return true;
}

} // namespace

// ---------------- Query operations ----------------

void GDRedisCallInternal::schedule() {
	if (posted || self_hold.is_null()) {
		return;
	}
	posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDRedisCallInternal::dispatch).bind(generation));
}

// Reject notifications queued before the operation was reused.
void GDRedisCallInternal::dispatch(uint64_t p_generation) {
	if (p_generation == generation) {
		step();
	}
}

// Register a query deadline with shared timers; zero waits until cancellation.
void GDRedisCallInternal::set_due(uint64_t p_wait) {
	Async::drop_deadline(this, due);
	due = deadline_after(p_wait);
	Async::track_deadline(this, due, callable_mp(this, &GDRedisCallInternal::step));
}

// Deliver an asynchronous failure on the next event-loop turn.
void GDRedisCallInternal::fail_later(const Ref<R> &p_out) {
	if (self_hold.is_null()) {
		return; // Do not reschedule an already completed operation.
	}
	if (Pool::is_stopping(true)) {
		done(p_out); // Release references immediately during shutdown when no event-loop turn remains.
		return;
	}
	if (dropped && notified) {
		done(p_out); // Clean up the deadline and self-reference after a canceled operation leaves the queue.
		return;
	}
	pending = p_out;
	schedule();
}

// Advance the asynchronous operation by one state.
void GDRedisCallInternal::step() {
	posted = false;
	Ref<GDRedisCallInternal> keep(this); // Remain alive while connection cleanup releases self-references.
	if (pending.is_valid()) {
		const Ref<R> out = pending;
		pending.unref();
		if (dropped) {
			// Notify cancellation but retain queue position because the reply is still expected.
			if (!notified) {
				notified = true;
				emit_signal("finished", shape == VALUE ? out : result(out));
			}
			if (mode != READING && db.is_valid()) {
				db->fail_connection(out); // Before connection setup there is no reply order to preserve; release resources immediately.
			}
			return;
		}
		done(out);
		return;
	}
	if (due > 0 && GDClock::msec() >= due) {
		const Ref<R> why = R::err("redis did not answer in time", Err::TIMED_OUT);
		if (mode == READING && db.is_valid()) {
			db->fail_connection(why); // Detach queued operations before their objects can be reused.
			return;
		}
		if (mode == OPENING) {
			if (db->opening.ptr() == this) {
				db->opening.unref();
			}
			db->fail_connection(why); // Release sockets even during an incomplete handshake.
		}
		done(why);
		return;
	}
	if (mode == RESOLVING) {
		return; // Wait for worker-based name resolution.
	}
	if (mode == OPENING) {
		db->sock.poll();
		const Wire::State st = db->sock.state();
		if (st == Wire::FAILED || st == Wire::CLOSED) {
			// Propagate the transport cause without reclassifying encrypted connections.
			const Ref<R> why = sock_error(db->sock, "connection closed");
			if (db->opening.ptr() == this) {
				db->opening.unref();
			}
			db->fail_connection(why);
			done(why);
			return;
		}
		if (st != Wire::READY) {
			return; // Wait for connection establishment or the active handshake.
		}
		// Apply TLS after the plain connection is established when requested.
		// Wait for READY on a subsequent turn before continuing the handshake-dependent path.
		if (db->guard != Wire::NONE && !db->wrapped) {
			db->wrapped = true;
			if (db->sock.wrap(db->host, db->guard, db->ca) != OK) {
				const Ref<R> why = sock_error(db->sock, "TLS setup failed", Err::NONE);
				if (db->opening.ptr() == this) {
					db->opening.unref();
				}
				db->fail_connection(why);
				done(why);
			}
			return;
		}
		db->buf = PackedByteArray();
		db->buf_at = 0;
		if (password.is_empty()) {
			done(R::ok());
			return;
		}
		// Send authentication and wait for its reply before reporting success.
		// Let the ordinary connection pump read AUTH once setup-specific handling ends.
		mode = READING;
		if (db->opening.ptr() == this) {
			db->opening.unref();
		}
		Array args;
		args.push_back(password);
		Ref<GDRedisCallInternal> auth = db->start("AUTH", args);
		auth->connect("finished", callable_mp(this, &GDRedisCallInternal::on_auth), Object::CONNECT_ONE_SHOT);
		return;
	}

	const Ref<R> got = db->fill();
	if (got->get_e().is_valid()) {
		done(got);
		return;
	}
	int at = db->buf_at;
	Variant value;
	bool ready = false;
	const Ref<R> read = db->take_reply(at, value, ready, GDClock::usec() + GD_SCHED_SLICE_USEC);
	db->buf_at = at; // Do not rewind a parsed prefix when a reply remains incomplete.
	if (read->get_e().is_valid()) {
		done(read);
		return;
	}
	if (!ready) {
		return; // Resume on the next turn.
	}
	db->buf_at = at;
	done(R::ok(value));
}

// Receive worker-resolved addresses and proceed to socket connection.
void GDRedisClient::resolved(const Ref<R> &p_result, const Ref<GDRedisCallInternal> &p_call) {
	if (p_call.is_null() || opening.ptr() != p_call.ptr() || p_call->self_hold.is_null() || p_call->db.ptr() != this || p_call->mode != GDRedisCallInternal::RESOLVING) {
		return;
	}
	if (Pool::is_stopping()) {
		p_call->fail_later(R::err("worker pool stopped", Err::INTERRUPTED));
		return;
	}
	if (p_result.is_null() || !p_result->get_ok()) {
		p_call->fail_later(p_result.is_valid() ? p_result : R::err(vformat("cannot prepare \"%s\"", host), Err::NOT_FOUND));
		return;
	}
	const Dictionary prepared = p_result->get_v();
	const String addr = prepared.get("address", "");
	ca = prepared.get("ca", Variant());
	if (addr.is_empty()) {
		p_call->fail_later(R::err(vformat("cannot resolve \"%s\"", host), Err::NOT_FOUND));
		return;
	}
	p_call->mode = GDRedisCallInternal::OPENING;
	if (sock.open(addr, port, p_call->due) != OK) {
		p_call->fail_later(sock_error(sock, vformat("cannot reach %s:%d", host, port)));
	} else {
		p_call->schedule();
	}
}

// Handle the key-value authentication result.
void GDRedisCallInternal::on_auth(const Ref<R> &p_out) {
	if (p_out->get_e().is_valid()) {
		const Ref<R> why = p_out->note("auth failed");
		if (db.is_valid()) {
			db->close(); // Do not reuse an unauthenticated connection for ordinary queries.
		}
		done(why);
		return;
	}
	done(R::ok());
}

// Emit a result after removing its operation from the queue.
// Reset reusable state by replacing result containers rather than clearing them.
// Clearing shared containers would also erase values already retained by callers.
void GDRedisCallInternal::reset() {
	Async::drop_deadline(this, due);
	generation++;
	mode = READING;
	due = 0;
	posted = false;
	pending.unref();
	password = String();
	want_replies = 1;
	collected = Array();
	shape = VALUE;
	dropped = false;
	notified = false;
}

// Preserve complete replies and mark unknown command results when the operation fails.
Ref<R> GDRedisCallInternal::result(const Ref<R> &p_error) const {
	if (shape == VALUE) {
		if (p_error.is_valid()) return p_error;
		const Variant value = collected[0];
		const Ref<R> one = value;
		return one.is_valid() ? one : R::ok(value);
	}
	Ref<Err> error = p_error.is_valid() ? p_error->get_e() : Ref<Err>();
	Array replies;
	const int count = shape == EXEC ? MAX(0, want_replies - 2) : want_replies;
	if (shape == BATCH) replies = collected;
	else if (collected.size() == want_replies) {
		const Variant value = collected[collected.size() - 1];
		const Ref<R> one = value;
		if (one.is_valid()) error = one->get_e();
		else if (value.get_type() == Variant::ARRAY) replies = value;
	} else if (p_error.is_valid() && db.is_valid() && !db->inflight.is_empty() &&
			db->inflight.front()->get().ptr() == this && collected.size() == want_replies - 1 &&
			!db->reply_stack.is_empty() && db->reply_stack[0].kind == '*') {
		replies = db->reply_stack[0].items; // Completed EXEC elements precede any unfinished nested value.
	}
	if (p_error.is_valid()) replies = replies.duplicate(); // Cancellation leaves parser storage live.
	// Failure placeholders preserve command positions without claiming execution did not occur.
	if (error.is_valid()) {
		while (replies.size() < count) replies.push_back(R::err(error));
	}
	for (int i = 0; i < replies.size(); i++) {
		if (replies[i].get_type() == Variant::NIL) replies[i] = redis_nil();
		const Ref<R> one = replies[i];
		if (error.is_null() && one.is_valid() && !one->get_ok()) error = one->get_e()->note(vformat("command %d", i));
	}
	return error.is_valid() ? R::err(error, Err::NONE, replies) : R::ok(replies);
}

// Finish the operation and deliver the result to its waiter.
void GDRedisCallInternal::done(const Ref<R> &p_out) {
	if (self_hold.is_null()) {
		return; // Ignore duplicate completion.
	}
	// Retain this object while releasing self_hold, which may own its final reference.
	Ref<GDRedisCallInternal> keep(this);
	Ref<GDRedisClient> owner = db;
	if (owner.is_valid() && owner->opening.ptr() == this) {
		owner->opening.unref();
	}
	Async::drop_deadline(this, due);
	due = 0;
	if (!notified && !dropped) {
		notified = true;
		const Ref<R> out = !p_out->get_ok() && shape != VALUE && p_out->get_v().get_type() != Variant::ARRAY ? result(p_out) : p_out;
		emit_signal("finished", out);
	} else if (!notified && pending.is_valid()) {
		// Deliver any pending cancellation before returning the operation for reuse.
		// A reply may arrive before the scheduled cancellation turn and recycle the operation,
		// otherwise removing its notification and leaving the waiter suspended forever.
		const Ref<R> late = pending;
		pending.unref();
		notified = true;
		emit_signal("finished", shape == VALUE ? late : result(late));
	}
	self_hold.unref();
	// Return the operation to its owner for reuse by another command.
	if (owner.is_valid()) {
		owner->give_back(this);
	}
}

// Handle waiter cancellation and emit it as a result.
// Completing without notification would leave an awaiting caller suspended forever.
// Keep the operation queued to discard its reply without assigning it to another command.
void GDRedisCallInternal::cancel() {
	if (self_hold.is_null() || dropped) {
		return; // Ignore an already completed operation.
	}
	dropped = true;
	// Notify on the next turn so the caller has time to attach its waiter.
	pending = R::err("cancelled", Err::INTERRUPTED);
	schedule();
}

// Register public methods and properties with script.
void GDRedisCallInternal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDRedisCallInternal::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// ---------------- Connections ----------------

bool GDRedisClient::is_open() const {
	return sock.is_valid() && sock.is_ready();
}

// Register the connection for shutdown cleanup.
GDRedisClient::GDRedisClient() {
	redis_clients.insert(this);
	sock.set_wait_callback(callable_mp(this, &GDRedisClient::socket_ready));
}

GDRedisClient::~GDRedisClient() {
	redis_clients.erase(this);
	sock.set_wait_callback(Callable());
}

// Synchronously close all live connections at process shutdown.
void GDRedisClient::shutdown_all() {
	LocalVector<Ref<GDRedisPool>> pools;
	for (GDRedisPool *pool : redis_pools) {
		pools.push_back(Ref<GDRedisPool>(pool));
	}
	for (const Ref<GDRedisPool> &pool : pools) {
		pool->close();
	}
	LocalVector<Ref<GDRedisClient>> clients;
	for (GDRedisClient *client : redis_clients) {
		clients.push_back(Ref<GDRedisClient>(client));
	}
	for (const Ref<GDRedisClient> &client : clients) {
		client->close();
	}
}

// Close and release retained resources.
void GDRedisClient::close() {
	fail_connection(R::err("connection closed", Err::INTERRUPTED));
}

// Fail all waiters after timeout or malformed framing makes reply order unusable.
void GDRedisClient::fail_connection(const Ref<R> &p_why) {
	// Snapshot the parser's completed root elements before releasing its unfinished frames.
	const Ref<GDRedisCallInternal> head = inflight.is_empty() ? Ref<GDRedisCallInternal>() : inflight.front()->get();
	const Ref<R> partial = head.is_valid() ? head->result(p_why) : Ref<R>();
	wire_generation++;
	out_buf.clear();
	next_buf.clear();
	buf.clear();
	buf_at = 0;
	line_at = 0;
	reply_stack.clear();
	reply_memory = 0;
	bulk_len = -1;
	flush_queued = false;
	subscribed = false; // Stop passive reads so the connection can detach from event delivery.
	watch(false);
	sock.close();
	ca.unref();
	const Ref<GDRedisCallInternal> opened = opening;
	opening.unref();
	LocalVector<Ref<GDRedisCallInternal>> failed;
	while (!packing.is_empty()) {
		const Ref<GDRedisPackJob> job = packing.front()->get();
		packing.pop_front();
		if (job->call.is_valid() && job->call->self_hold.is_valid()) failed.push_back(job->call);
	}
	packing_bytes = 0;
	while (!inflight.is_empty()) {
		failed.push_back(inflight.front()->get());
		inflight.pop_front();
	}
	if (opened.is_valid()) opened->fail_later(p_why);
	for (const Ref<GDRedisCallInternal> &call : failed) call->done(call == head ? partial : p_why);
}

// Append available socket bytes to the receive buffer.
Ref<R> GDRedisClient::fill() {
	const int old_at = buf_at;
	const Ref<R> got = sock_fill(sock, buf, buf_at, REDIS_READ_CHUNK, REDIS_RECV_MAX);
	if (old_at > 0 && buf_at == 0) {
		line_at = MAX(0, line_at - old_at);
	}
	return got;
}

// Parse one RESP reply for the caller.
Ref<R> GDRedisClient::take_reply(int &r_at, Variant &r_value, bool &r_ready, uint64_t p_due) {
	r_ready = false;
	reply_yielded = false;

	// Account for result allocations in stored bytes rather than value count.
	auto keep_memory = [&](int64_t p_bytes) {
		if (p_bytes < 0 || reply_memory > REPLY_MEMORY_MAX - p_bytes) {
			return false;
		}
		reply_memory += p_bytes;
		return true;
	};

	// Append completed values to their parents and fold completed parents up to the root.
	auto accept = [&](Variant p_value, Ref<R> p_result, bool &r_limited) {
		while (!reply_stack.is_empty()) {
			ParseFrame &frame = reply_stack[reply_stack.size() - 1];
			const Variant stored = p_result->get_e().is_valid() ? Variant(p_result) : p_value;
			const int64_t slot_bytes = frame.kind == '%' ? MAP_VALUE_BYTES : ARRAY_VALUE_BYTES;
			if (!keep_memory(slot_bytes)) {
				r_limited = true;
				return R::err("reply exceeds the memory limit", Err::LIMITED);
			}
			if (frame.kind == '%') {
				if (!frame.has_key) {
					frame.key = stored;
					frame.has_key = true;
				} else {
					frame.map[frame.key] = stored;
					frame.key = Variant();
					frame.has_key = false;
				}
			} else {
				frame.items.push_back(stored);
			}
			if (--frame.left > 0) {
				return Ref<R>();
			}
			p_value = frame.kind == '%' ? Variant(frame.map) : Variant(frame.items);
			p_result = R::ok();
			reply_stack.remove_at(reply_stack.size() - 1);
		}
		r_value = p_value;
		r_ready = true;
		return p_result;
	};

	while (GDClock::usec() < p_due) {
		// Discard the parsed bulk header and wait only for its body.
		if (bulk_len >= 0) {
			if ((int64_t)buf.size() < (int64_t)r_at + bulk_len + 2) {
				return R::ok();
			}
			if (buf[r_at + bulk_len] != '\r' || buf[r_at + bulk_len + 1] != '\n') {
				return R::err("bulk reply has no CRLF", Err::INVALID_DATA);
			}
			// String::append_utf8 allocates one char32_t per input byte before conversion.
			if (!keep_memory((int64_t(bulk_len) + 1) * sizeof(char32_t))) {
				return R::err("reply exceeds the memory limit", Err::LIMITED);
			}
			Variant value = utf8_value(buf.ptr() + r_at, bulk_len);
			const Ref<R> text_error = value;
			if (text_error.is_valid()) return text_error;
			r_at += bulk_len + 2;
			bulk_len = -1;
			bool memory_limited = false;
			Ref<R> done = accept(value, R::ok(), memory_limited);
			if (memory_limited) {
				return done;
			}
			if (done.is_valid()) {
				reply_memory = 0;
				return done;
			}
			continue;
		}

		// Resume CRLF scanning at the saved cursor to avoid rescanning fragmented input.
		int end = -1;
		for (int i = MAX(r_at, line_at); i + 1 < buf.size(); i++) {
			if (buf[i] == '\r' && buf[i + 1] == '\n') {
				end = i;
				break;
			}
		}
		if (end < 0) {
			line_at = MAX(r_at, buf.size() - 1);
			return R::ok();
		}
		const String line = String::utf8((const char *)buf.ptr() + r_at, end - r_at);
		r_at = end + 2;
		line_at = r_at;
		if (line.is_empty()) {
			return R::err("empty reply", Err::INVALID_DATA);
		}
		const char32_t kind = line[0];
		const String rest = line.substr(1);
		Variant value;
		Ref<R> result = R::ok();
		int64_t value_memory = 0;
		switch (kind) {
			case '+':
				value = rest;
				value_memory = (int64_t(rest.length()) + 1) * sizeof(char32_t);
				break;
			case '-':
				result = R::err(rest, Err::INVALID_DATA);
				value_memory = (int64_t(rest.length()) + 1) * sizeof(char32_t);
				break;
			case ':': {
				int64_t n = 0;
				if (!resp_int(rest, n)) {
					return R::err("invalid integer reply", Err::INVALID_DATA);
				}
				value = n;
			} break;
			case ',':
				if (!rest.is_valid_float()) {
					return R::err("invalid float reply", Err::INVALID_DATA);
				}
				value = rest.to_float();
				break;
			case '#':
				if (rest != "t" && rest != "f") {
					return R::err("invalid boolean reply", Err::INVALID_DATA);
				}
				value = rest == "t";
				break;
			case '_':
				if (!rest.is_empty()) {
					return R::err("invalid null reply", Err::INVALID_DATA);
				}
				break;
			case '$': {
				int64_t n = 0;
				if (!resp_int(rest, n) || n < -1 || n > REPLY_BULK_MAX) {
					return R::err("invalid bulk length", Err::INVALID_DATA);
				}
				if (n >= 0) {
					const int64_t text_bytes = (n + 1) * sizeof(char32_t);
					if (text_bytes > REPLY_MEMORY_MAX - reply_memory) {
						return R::err("reply exceeds the memory limit", Err::LIMITED);
					}
					bulk_len = (int)n;
					continue;
				}
			} break;
			case '*':
			case '~':
			case '>':
			case '%': {
				int64_t count = 0;
				if (!resp_int(rest, count) || count < (kind == '%' ? 0 : -1) || count > (kind == '%' ? INT_MAX / 2 : INT_MAX)) {
					return R::err(kind == '%' ? "invalid map count" : "invalid aggregate count", Err::INVALID_DATA);
				}
				if (count < 0) {
					break;
				}
				const int values = kind == '%' ? (int)count * 2 : (int)count;
				if (values > 0) {
					if (reply_stack.size() >= Variant::MAX_RECURSION_DEPTH) {
						return R::err("reply exceeds Variant nesting capacity", Err::INVALID_DATA);
					}
					const int64_t slot_bytes = kind == '%' ? MAP_VALUE_BYTES : ARRAY_VALUE_BYTES;
					if (int64_t(values) > (REPLY_MEMORY_MAX - reply_memory) / slot_bytes || !keep_memory(sizeof(ParseFrame))) {
						return R::err("reply exceeds the memory limit", Err::LIMITED);
					}
					ParseFrame frame;
					frame.kind = (char)kind;
					frame.left = values;
					reply_stack.push_back(frame);
					continue;
				}
				value = kind == '%' ? Variant(Dictionary()) : Variant(Array());
			} break;
			default:
				// An unknown type has no recoverable length boundary; fail the connection, not just one command.
				return R::err(vformat("unknown reply type '%s'", String::chr(kind)), Err::INVALID_DATA);
		}
		if (value_memory > 0 && !keep_memory(value_memory)) {
			return R::err("reply exceeds the memory limit", Err::LIMITED);
		}
		bool memory_limited = false;
		Ref<R> done = accept(value, result, memory_limited);
		if (memory_limited) {
			return done;
		}
		if (done.is_valid()) {
			reply_memory = 0;
			return done;
		}
	}
	reply_yielded = true;
	return R::ok();
}

// Open the destination and prepare it for use.
Signal GDRedisClient::open(const String &p_host, int64_t p_port, const Dictionary &p_opts) {
	const double timeout = p_opts.get("timeout", 10.0);
	uint64_t parsed_wait = 0;
	if (!Limit::seconds_ms(timeout, parsed_wait)) {
		Ref<GDRedisCallInternal> bad;
		bad.instantiate();
		bad->self_hold = bad;
		bad->fail_later(R::err("timeout must be zero or a positive number of seconds", Err::INVALID_DATA));
		return Signal(bad.ptr(), "finished");
	}
	if (p_host.is_empty() || p_port < Limit::PORT_MIN || p_port > Limit::PORT_MAX) {
		Ref<GDRedisCallInternal> bad;
		bad.instantiate();
		bad->self_hold = bad;
		bad->fail_later(R::err("redis address is invalid", Err::INVALID_DATA));
		return Signal(bad.ptr(), "finished");
	}
	Wire::Guard parsed_guard;
	// Select verify-full for explicit true or external-host defaults, and disable for default loopback.
	if (!Wire::guard_of(p_opts.get("tls", Wire::default_guard(p_host)), parsed_guard)) {
		Ref<GDRedisCallInternal> bad;
		bad.instantiate();
		bad->self_hold = bad;
		bad->fail_later(R::err("tls must be one of disable / require / verify-full", Err::INVALID_DATA));
		return Signal(bad.ptr(), "finished");
	}
	const String ca_path = p_opts.get("ca", "");

	Ref<GDRedisCallInternal> call;
	call.instantiate();
	call->db = Ref<GDRedisClient>(this);
	call->self_hold = call;
	call->mode = GDRedisCallInternal::OPENING;
	call->password = p_opts.get("password", "");
	call->set_due(parsed_wait);

	// Accept hostnames as well as numeric addresses for container and production endpoints.
	// Check network permission before resolving names, since a later connection check
	// would still allow the DNS query to leave the process.
	if (!Perm::check(Perm::NET, vformat("%s:%d", p_host, p_port))) {
		call->fail_later(R::err(vformat("net access to \"%s\" is not allowed", p_host), Err::PERMISSION_DENIED));
		return Signal(call.ptr(), "finished");
	}
	if (opening.is_valid() || sock.is_valid() || !inflight.is_empty() || !packing.is_empty()) {
		const uint64_t previous = wire_generation;
		fail_connection(R::err("connection replaced", Err::INTERRUPTED));
		if (wire_generation != previous + 1 || opening.is_valid() || sock.is_valid()) {
			call->fail_later(R::err("connection replaced during notification", Err::INTERRUPTED));
			return Signal(call.ptr(), "finished");
		}
	}
	// Commit settings only after old waiters have finished, preserving a reentrant open.
	host = p_host;
	port = int(p_port);
	password = call->password;
	wait_ms = parsed_wait;
	guard = parsed_guard;
	ca.unref();
	wrapped = false;
	reply_stack.clear();
	reply_memory = 0;
	line_at = 0;
	bulk_len = -1;
	opening = call;
	if (!ca_path.is_empty()) {
		call->mode = GDRedisCallInternal::RESOLVING;
		const String lookup_host = host;
		GDFileCall::start([lookup_host, ca_path]() { return Wire::prepare(lookup_host, ca_path); }).connect(
				callable_mp(this, &GDRedisClient::resolved).bind(call), Object::CONNECT_ONE_SHOT);
		return Signal(call.ptr(), "finished");
	}
	if (sock.open(host, port, call->due) != OK) {
		call->fail_later(sock_error(sock, vformat("cannot reach %s:%d", host, port)));
	} else {
		call->schedule();
	}
	return Signal(call.ptr(), "finished");
}

// Execute a command and return its result.
Signal GDRedisClient::query(const String &p_cmd, const Array &p_args) {
	return Signal(start(p_cmd, p_args).ptr(), "finished");
}

// Send a pipeline and return replies in command order.
Signal GDRedisClient::pipeline(const Array &p_cmds) {
	return Signal(start_batch(p_cmds).ptr(), "finished");
}

// Wrap commands in MULTI and EXEC for server-side transactional execution.
// Return only EXEC's array of command results.
// Count and discard intermediate QUEUED acknowledgments.
Signal GDRedisClient::transaction(const Array &p_cmds) {
	if (p_cmds.is_empty()) return Signal(start_batch(p_cmds).ptr(), "finished");
	Array wrapped;
	Array multi;
	multi.push_back("MULTI");
	wrapped.push_back(multi);
	for (int i = 0; i < p_cmds.size(); i++) {
		wrapped.push_back(p_cmds[i]);
	}
	Array exec;
	exec.push_back("EXEC");
	wrapped.push_back(exec);

	Ref<GDRedisCallInternal> call = start_batch(wrapped);
	call->shape = GDRedisCallInternal::EXEC; // Return only command results from EXEC.
	return Signal(call.ptr(), "finished");
}

// Begin subscription reads and emit message for each received item.
Signal GDRedisClient::subscribe(const PackedStringArray &p_channels) {
	Array args;
	for (const String &c : p_channels) {
		args.push_back(c);
	}
	Ref<GDRedisCallInternal> call;
	call.instantiate();
	call->db = Ref<GDRedisClient>(this);
	call->self_hold = call;
	call->set_due(wait_ms);
	call->want_replies = MAX(1, p_channels.size()); // Each subscription produces one acknowledgment.

	if (!is_open()) {
		call->fail_later(R::err("not connected", Err::NOT_FOUND));
		return Signal(call.ptr(), "finished");
	}
	Ref<GDRedisPackJob> job;
	job.instantiate();
	job->db = Ref<GDRedisClient>(this);
	job->call = call;
	job->cmd = "SUBSCRIBE";
	job->args = args.duplicate(true);
	job->subscription = true;
	queue_pack(job);
	return Signal(call.ptr(), "finished");
}

// ---------------- Connection pools ----------------

// End the acquisition wait and establish or reuse the borrowed connection.
void GDRedisPoolCall::start(const Ref<GDRedisClient> &p_conn, bool p_open) {
	Async::drop_deadline(this, due);
	due = 0;
	conn = p_conn;
	opening = p_open;
	if (opening) {
		pool->opening++;
		inner = conn->open(pool->host, pool->port, pool->opts);
	} else {
		inner = conn->query(cmd, args);
	}
	inner.connect(callable_mp(this, &GDRedisPoolCall::received), Object::CONNECT_ONE_SHOT);
}

// Run the command only after successful setup, then return the connection with its result.
void GDRedisPoolCall::received(const Ref<R> &p_out) {
	if (done) {
		return;
	}
	inner = Signal();
	if (opening) {
		opening = false;
		pool->opening--;
		pool->schedule();
		if (p_out.is_valid() && p_out->get_ok()) {
			start(conn, false);
			return;
		}
	}
	finish(p_out);
}

// Detach wait and inner-signal state, then schedule delivery after returning the connection.
void GDRedisPoolCall::finish(const Ref<R> &p_out, bool p_close) {
	if (done) {
		return;
	}
	done = true;
	Ref<GDRedisPoolCall> keep(this);
	Async::drop_deadline(this, due);
	due = 0;
	const Callable callback = callable_mp(this, &GDRedisPoolCall::received);
	if (!inner.is_null() && inner.is_connected(callback)) {
		inner.disconnect(callback);
	}
	inner = Signal();
	if (pool.is_valid()) {
		if (waiting) {
			pool->waits.erase(waiting);
			waiting = nullptr;
		}
		if (entry) {
			pool->calls.erase(entry);
			entry = nullptr;
		}
		if (opening) {
			pool->opening--;
			opening = false;
		}
		if (p_close && conn.is_valid()) {
			conn->close();
		}
		pool->release(conn);
	}
	pool.unref();
	conn.unref();
	args = Array();
	cmd = String();
	outcome = p_out.is_valid() ? p_out : R::err("redis returned no result", Err::INVALID_DATA);
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDRedisPoolCall::deliver));
	self_hold.unref(); // The ready queue retains this object, avoiding an undelivered self-cycle at shutdown.
}

// Deliver completion exactly once and release the retained result.
void GDRedisPoolCall::deliver() {
	Ref<GDRedisPoolCall> keep(this);
	const Ref<R> out = outcome;
	outcome.unref();
	emit_signal("finished", out);
}

// Remove only calls whose connection-acquisition deadline expired.
void GDRedisPoolCall::expired() {
	if (!done && due > 0 && GDClock::msec() >= due) {
		finish(R::err("redis pool wait timed out", Err::TIMED_OUT), true);
	}
}

// Close only this call's exclusively held connection during setup or execution.
void GDRedisPoolCall::cancel() {
	finish(R::err("cancelled", Err::INTERRUPTED), true);
}

// Register result notification and cancellation.
void GDRedisPoolCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDRedisPoolCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Register the pool so shutdown also cleans connection waiters.
GDRedisPool::GDRedisPool() {
	redis_pools.insert(this);
}

// Close idle connections and remove the pool from shutdown tracking.
GDRedisPool::~GDRedisPool() {
	close();
	redis_pools.erase(this);
}

// Configure the destination without creating physical connections until the first query.
Signal GDRedisPool::open(const String &p_host, int64_t p_port, const Dictionary &p_opts, int64_t p_size) {
	if (p_size < 0 || p_size > INT_MAX) {
		return Async::ready(R::err("pool size must be between 0 and 2147483647", Err::INVALID_DATA));
	}
	if (p_host.is_empty() || p_port < Limit::PORT_MIN || p_port > Limit::PORT_MAX) {
		return Async::ready(R::err("redis address is invalid", Err::INVALID_DATA));
	}
	const int want = p_size > 0 ? int(p_size) : default_size;
	if (want < 0) {
		return Async::ready(R::err("pool size must be between 0 and 2147483647", Err::INVALID_DATA));
	}
	uint64_t timeout = 0;
	uint64_t wait = 0;
	if (!Limit::seconds_ms(p_opts.get("timeout", 10.0), timeout) ||
			!Limit::seconds_ms(p_opts.get("pool_timeout", timeout > 0 ? double(timeout) / 1000.0 + 1.0 : 30.0), wait)) {
		return Async::ready(R::err("timeouts must be zero or positive seconds", Err::INVALID_DATA));
	}
	Wire::Guard guard;
	if (!Wire::guard_of(p_opts.get("tls", Wire::default_guard(p_host)), guard)) {
		return Async::ready(R::err("redis pool connection options are invalid", Err::INVALID_DATA));
	}
	close();
	host = p_host;
	port = int(p_port);
	opts = p_opts.duplicate(true);
	max_open = want;
	const int64_t cpus = MAX(1, GDSystem::cpus());
	dial_limit = int(MIN(int64_t(INT_MAX), cpus * 10)); // Derive concurrent dial capacity from the CPU count.
	if (max_open > 0) {
		dial_limit = MIN(dial_limit, max_open);
	}
	wait_ms = wait;
	configured = true;
	return Async::ready(R::ok());
}

// Queue a query in FIFO order with a cancelable, deadline-aware acquisition wait.
Signal GDRedisPool::query(const String &p_cmd, const Array &p_args) {
	if (!configured) {
		return Async::ready(R::err("pool is not open", Err::NOT_FOUND));
	}
	Ref<GDRedisPoolCall> call;
	call.instantiate();
	call->self_hold = call;
	call->pool = Ref<GDRedisPool>(this);
	call->cmd = p_cmd;
	call->args = p_args.duplicate(true);
	call->entry = calls.push_back(call);
	call->waiting = waits.push_back(call);
	call->due = deadline_after(wait_ms);
	Async::track_deadline(call.ptr(), call->due, callable_mp(call.ptr(), &GDRedisPoolCall::expired));
	schedule();
	return Signal(call.ptr(), "finished");
}

// Schedule the wait queue once after connection return or creation completes.
void GDRedisPool::schedule() {
	if (!posted && configured && !waits.is_empty()) {
		posted = true;
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDRedisPool::pump));
	}
}

// Lend idle connections to FIFO waiters and create more within concurrent dial capacity.
void GDRedisPool::pump() {
	posted = false;
	const uint64_t due = GDClock::usec() + GD_SCHED_SLICE_USEC;
	while (configured && !waits.is_empty()) {
		Ref<GDRedisPoolCall> call = waits.front()->get();
		call->expired();
		if (call->done) {
			continue;
		}
		Ref<GDRedisClient> conn;
		while (!idle.is_empty()) {
			conn = idle[idle.size() - 1];
			idle.resize(idle.size() - 1);
			if (conn->is_open() && !conn->is_subscribed()) {
				break;
			}
			conns.erase(conn);
			conn->close();
			conn.unref();
		}
		const bool create = conn.is_null();
		if (create) {
			if (opening >= dial_limit || (max_open > 0 && conns.size() >= max_open)) {
				return; // Resume only after a return, dial completion, or deadline notification.
			}
			conn.instantiate();
			conns.push_back(conn);
		}
		waits.erase(call->waiting);
		call->waiting = nullptr;
		call->start(conn, create);
		if (GDClock::usec() >= due) {
			schedule();
			return;
		}
	}
}

// Return usable connections to idle and remove broken ones from capacity accounting.
void GDRedisPool::release(const Ref<GDRedisClient> &p_conn) {
	if (p_conn.is_valid()) {
		if (configured && p_conn->is_open() && !p_conn->is_subscribed()) {
			idle.push_back(p_conn);
		} else {
			conns.erase(p_conn);
			p_conn->close();
		}
	}
	schedule();
}

// Close waiters and borrowed connections first so reopening cannot inherit stale work.
void GDRedisPool::close() {
	configured = false;
	posted = false;
	while (!calls.is_empty()) {
		const Ref<GDRedisPoolCall> call = calls.front()->get();
		call->finish(R::err("redis pool closed", Err::INTERRUPTED), true);
	}
	for (Ref<GDRedisClient> &c : conns) {
		if (c.is_valid()) {
			c->close();
		}
	}
	conns.clear();
	idle.clear();
	opts.clear();
}

// Return the number of active queries.
int GDRedisPool::in_flight() const {
	return calls.size();
}

// Register public methods and properties with script.
void GDRedisPool::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "host", "port", "opts", "size"), &GDRedisPool::open, DEFVAL("127.0.0.1"), DEFVAL(6379), DEFVAL(Dictionary()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("query", "cmd", "args"), &GDRedisPool::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("open_async", "host", "port", "opts", "size"), &GDRedisPool::open, DEFVAL("127.0.0.1"), DEFVAL(6379), DEFVAL(Dictionary()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("query_async", "cmd", "args"), &GDRedisPool::query, DEFVAL(Array()));
	ADD_AWAIT("open", "R:Variant");
	ADD_AWAIT("query", "R:Variant");
	ADD_AWAIT("open_async", "R:Variant");
	ADD_AWAIT("query_async", "R:Variant");
	ADD_AUTO_WAIT("open");
	ADD_AUTO_WAIT("query");
	ClassDB::bind_method(D_METHOD("close"), &GDRedisPool::close);
	ClassDB::bind_method(D_METHOD("size"), &GDRedisPool::size);
	ClassDB::bind_method(D_METHOD("in_flight"), &GDRedisPool::in_flight);
}

// Append one command element as a bulk string.
// Avoid intermediate text conversion and its separate length and content allocations.
// Append byte-array arguments unchanged to preserve binary contents.
static bool push_bulk(ByteBuf &r_out, const Variant &p_one) {
	if (p_one.get_type() == Variant::PACKED_BYTE_ARRAY) {
		const PackedByteArray raw = p_one;
		if (raw.size() > SEND_MAX - 32 || r_out.size() > SEND_MAX - raw.size() - 32) {
			return false;
		}
		r_out.push_back('$');
		push_digits(r_out, raw.size());
		put_raw(r_out, (const uint8_t *)"\r\n", 2);
		put_raw(r_out, raw.ptr(), raw.size());
		put_raw(r_out, (const uint8_t *)"\r\n", 2);
		return true;
	}
	const Variant::Type type = p_one.get_type();
	if (type != Variant::NIL && type != Variant::BOOL && type != Variant::INT && type != Variant::FLOAT && type != Variant::STRING && type != Variant::STRING_NAME) {
		return false;
	}
	const String text = String(p_one);
	const int64_t room = SEND_MAX - int64_t(r_out.size()) - 32;
	if (int64_t(text.length()) * 6 > room && utf8_bytes(text) > room) {
		return false;
	}
	const CharString utf = text.utf8();
	if (utf.length() > SEND_MAX - 32 || r_out.size() > SEND_MAX - utf.length() - 32) {
		return false;
	}
	r_out.push_back('$');
	push_digits(r_out, utf.length());
	put_raw(r_out, (const uint8_t *)"\r\n", 2);
	put_raw(r_out, (const uint8_t *)utf.get_data(), utf.length());
	put_raw(r_out, (const uint8_t *)"\r\n", 2);
	return true;
}

// Encode one command as an array of bulk strings.
bool GDRedisClient::pack_cmd(ByteBuf &r_out, const String &p_cmd, const Array &p_args) {
	if (r_out.size() > SEND_MAX - 32) {
		return false;
	}
	r_out.push_back('*');
	push_digits(r_out, int64_t(p_args.size()) + 1);
	put_raw(r_out, (const uint8_t *)"\r\n", 2);
	if (!push_bulk(r_out, p_cmd)) {
		return false;
	}
	for (int i = 0; i < p_args.size(); i++) {
		if (!push_bulk(r_out, p_args[i])) {
			return false;
		}
	}
	return true;
}

// Conservatively estimate bytes retained while RESP encoding is queued.
static int64_t bulk_bound(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::PACKED_BYTE_ARRAY:
			return PackedByteArray(p_value).size() + 32;
		case Variant::STRING:
		case Variant::STRING_NAME:
			return int64_t(String(p_value).length()) * 4 + 32;
		default:
			return 64;
	}
}

// Compute the maximum retained bytes of one command or pipeline without overflow.
static int64_t redis_bound(const String &p_cmd, const Array &p_args, const Array &p_cmds) {
	int64_t bytes = int64_t(p_cmd.length()) * 4 + 64;
	auto add = [&](int64_t p_n) {
		bytes = bytes > SEND_MAX || p_n > SEND_MAX - bytes ? int64_t(SEND_MAX) + 1 : bytes + p_n;
	};
	for (const Variant &arg : p_args) {
		add(bulk_bound(arg));
	}
	for (const Variant &value : p_cmds) {
		if (value.get_type() != Variant::ARRAY) {
			add(64);
			continue;
		}
		const Array one = value;
		for (const Variant &part : one) {
			add(bulk_bound(part));
		}
		add(32);
	}
	return bytes;
}

// Buffer one command and flush once on the next turn.
bool GDRedisClient::write_cmd(const String &p_cmd, const Array &p_args) {
	ByteBuf one;
	if (!pack_cmd(one, p_cmd, p_args) || out_buf.size() > SEND_MAX - one.size()) {
		return false;
	}
	put_raw(out_buf, one.ptr(), one.size());
	queue_flush();
	return true;
}

// Encode RESP on a worker.
void GDRedisPackJob::run() {
	if (!batch) {
		if (!GDRedisClient::pack_cmd(packed, cmd, args)) {
			error = R::err("redis send limit exceeded", Err::LIMITED);
		}
		return;
	}
	for (int i = 0; i < cmds.size(); i++) {
		if (cmds[i].get_type() != Variant::ARRAY) {
			error = R::err(vformat("command %d is not an Array", i), Err::INVALID_DATA);
			return;
		}
		const Array one = cmds[i];
		if (one.is_empty()) {
			error = R::err(vformat("command %d is empty", i), Err::INVALID_DATA);
			return;
		}
		if (!GDRedisClient::pack_cmd(packed, Pool::text(one[0]), one.slice(1))) {
			error = R::err("redis pipeline exceeds the send limit", Err::LIMITED);
			return;
		}
	}
}

// Return worker results to the connection's arrival-ordered queue.
void GDRedisPackJob::finish() {
	if (db.is_valid()) {
		if (Pool::is_stopping(true)) {
			db->close(); // Do not send; close other unfinished calls on the same connection too.
		} else {
			db->packed(this);
		}
	}
	db.unref();
	call.unref();
	args.clear();
	cmds.clear();
}

// Run only the first encoding job on a given connection.
void GDRedisClient::queue_pack(const Ref<GDRedisPackJob> &p_job) {
	p_job->args = p_job->args.duplicate(true);
	p_job->cmds = p_job->cmds.duplicate(true);
	packing_bytes += p_job->queued_bytes;
	const bool first = packing.is_empty();
	packing.push_back(p_job);
	watch(true); // Keep receiving deadline wakeups during encoding.
	if (first && next_buf.is_empty()) {
		start_pack();
	}
}

// Encode only the first queued job and clean up all unsubmitted jobs during shutdown.
void GDRedisClient::start_pack() {
	while (!packing.is_empty()) {
		Ref<GDRedisPackJob> job = packing.front()->get();
		if (!job->call.is_valid() || !job->call->self_hold.is_valid()) {
			packing.pop_front();
			packing_bytes = MAX(int64_t(0), packing_bytes - job->queued_bytes);
			continue;
		}
		if (job->submit(true)) {
			return;
		}
		packing.pop_front();
		packing_bytes = MAX(int64_t(0), packing_bytes - job->queued_bytes);
		job->call->done(R::err("worker pool stopped", Err::INTERRUPTED));
	}
}

// Move encoded commands into the send queue in arrival order.
void GDRedisClient::packed(GDRedisPackJob *p_job) {
	if (packing.is_empty() || packing.front()->get().ptr() != p_job) {
		return; // Ignore jobs from a closed or previous connection.
	}
	Ref<GDRedisPackJob> job = packing.front()->get();
	packing.pop_front();
	packing_bytes = MAX(int64_t(0), packing_bytes - job->queued_bytes);
	Ref<GDRedisPackJob> next = packing.is_empty() ? Ref<GDRedisPackJob>() : packing.front()->get();
	if (job->call.is_valid() && job->call->self_hold.is_valid()) {
		if (job->call->dropped) {
			job->call->done(R::err("cancelled", Err::INTERRUPTED));
		} else if (job->error.is_valid()) {
			job->call->fail_later(job->error);
		} else if (!is_open()) {
			job->call->fail_later(R::err("not connected", Err::NOT_FOUND));
		} else if (job->call->due > 0 && GDClock::msec() >= job->call->due) {
			job->call->fail_later(R::err("redis command encoding timed out", Err::TIMED_OUT));
		} else {
			if (out_buf.is_empty() && next_buf.is_empty()) {
				out_buf = static_cast<ByteBuf &&>(job->packed);
			} else {
				next_buf = static_cast<ByteBuf &&>(job->packed);
			}
			queue_flush();
			inflight.push_back(job->call);
			if (job->subscription) {
				subscribed = true;
			}
			watch(true);
		}
	}
	if (next.is_valid() && next_buf.is_empty()) {
		start_pack(); // Other connections' first jobs can run concurrently in the CPU pool.
	}
}

// Schedule one flush regardless of how many commands arrive in the same turn.
void GDRedisClient::queue_flush() {
	if (flush_queued) {
		return;
	}
	flush_queued = true;
	callable_mp(this, &GDRedisClient::flush_out).call_deferred();
}

// Send as much buffered data as possible and retain the remainder for another turn.
void GDRedisClient::flush_out() {
	Ref<GDRedisClient> keep(this); // Remain alive while send failures notify waiters that may release this connection.
	flush_queued = false;
	if (out_buf.is_empty()) {
		sock.write_wait(false);
		return;
	}
	if (!sock.is_valid() || !sock.is_ready()) {
		sock.write_wait(false);
		fail_connection(sock_error(sock, "connection lost"));
		return;
	}
	if (!sock_flush(sock, out_buf)) {
		fail_connection(sock_error(sock, "send failed"));
		return;
	}
	if (out_buf.is_empty() && !next_buf.is_empty()) {
		out_buf = static_cast<ByteBuf &&>(next_buf); // Move to the next command without copying the whole buffer.
		if (!packing.is_empty()) {
			start_pack();
		}
	}
	if (!out_buf.is_empty()) {
		watch(true); // Resume the incomplete send on a later readiness notification.
	}
}

// Borrow a reserved operation, or create one if none is available.
Ref<GDRedisCallInternal> GDRedisClient::lend() {
	if (!spare.is_empty()) {
		Ref<GDRedisCallInternal> got = spare[spare.size() - 1];
		spare.remove_at(spare.size() - 1);
		got->reset();
		return got;
	}
	Ref<GDRedisCallInternal> made;
	made.instantiate();
	return made;
}

// Retain a completed operation for reuse.
void GDRedisClient::give_back(GDRedisCallInternal *p_call) {
	if (spare.size() >= SPARE_MAX) {
		return;
	}
	// Release the owner reference before caching to avoid a reference cycle.
	p_call->db.unref();
	p_call->reset(); // Do not retain the previous result or password.
	spare.push_back(Ref<GDRedisCallInternal>(p_call));
}

// Start a single-command operation.
Ref<GDRedisCallInternal> GDRedisClient::start(const String &p_cmd, const Array &p_args) {
	Ref<GDRedisCallInternal> call = lend();
	call->db = Ref<GDRedisClient>(this);
	call->self_hold = call;
	call->set_due(wait_ms);

	if (!is_open()) {
		call->fail_later(R::err("not connected", Err::NOT_FOUND));
		return call;
	}
	Ref<GDRedisPackJob> job;
	job.instantiate();
	job->db = Ref<GDRedisClient>(this);
	job->call = call;
	job->cmd = p_cmd;
	job->args = p_args;
	job->queued_bytes = redis_bound(p_cmd, p_args, Array());
	queue_pack(job);
	return call;
}

// Start one pipeline operation that waits for every expected reply.
// Batch commands to reduce the exchange to one round trip.
Ref<GDRedisCallInternal> GDRedisClient::start_batch(const Array &p_cmds) {
	Ref<GDRedisCallInternal> call = lend();
	call->db = Ref<GDRedisClient>(this);
	call->self_hold = call;
	call->set_due(wait_ms);
	call->want_replies = p_cmds.size();
	call->shape = GDRedisCallInternal::BATCH;

	if (p_cmds.is_empty()) {
		call->fail_later(R::ok(Array()));
		return call;
	}
	if (!is_open()) {
		call->fail_later(R::err("not connected", Err::NOT_FOUND));
		return call;
	}
	Ref<GDRedisPackJob> job;
	job.instantiate();
	job->db = Ref<GDRedisClient>(this);
	job->call = call;
	job->cmds = p_cmds;
	job->batch = true;
	job->queued_bytes = redis_bound(String(), Array(), p_cmds);
	queue_pack(job);
	return call;
}

// Deliver available replies to the first waiter in send order.
void GDRedisClient::pump() {
	pump_posted = false;
	const uint64_t active_generation = wire_generation;
	const uint64_t slice_due = GDClock::usec() + GD_SCHED_SLICE_USEC;
	// Retain the connection while callbacks run: a waiting caller may release
	// the final external reference during result delivery, but subsequent processing
	// still accesses this object's state before returning.
	Ref<GDRedisClient> keep(this);
	// Flush remaining output first so sending resumes when the peer begins reading.
	if (!out_buf.is_empty()) {
		flush_out();
		if (active_generation != wire_generation) {
			return;
		}
	}
	// Expire the caller without stopping the encoder; do not reuse its operation until encoding completes.
	const uint64_t now = GDClock::msec();
	for (const Ref<GDRedisPackJob> &job : packing) {
		if (job->call.is_null() || job->call->due == 0 || now < job->call->due) {
			continue;
		}
		if (!job->call->dropped) {
			job->call->dropped = true;
			job->call->pending = R::err("redis command encoding timed out", Err::TIMED_OUT);
			job->call->schedule();
		}
		Async::drop_deadline(job->call.ptr(), job->call->due);
		job->call->due = 0;
	}
	// Continue watching the connection during passive subscription reads even without waiters.
	// Unsolicited messages have no corresponding command in the wait queue.
	if (inflight.is_empty() && !subscribed && out_buf.is_empty() && next_buf.is_empty() && packing.is_empty()) {
		watch(false);
		return;
	}
	const Ref<R> live = fill();
	// Finish buffered replies across parser yields before failing the remaining calls.

	// Deliver replies until the first waiter is satisfied, yielding when input is incomplete.
	// A reply without a waiter is an unsolicited message.
	while (!inflight.is_empty() || subscribed) {
		if (inflight.is_empty()) {
			int at2 = buf_at;
			Variant pushed;
			bool ready2 = false;
			const Ref<R> got2 = take_reply(at2, pushed, ready2, slice_due);
			buf_at = at2;
			if (got2->get_e().is_valid() && !ready2) {
				// Close subscriptions too when malformed pushes destroy framing boundaries.
				fail_connection(got2);
				return;
			}
			if (!ready2) {
				break;
			}
			if (got2->get_ok()) {
				emit_push(pushed);
				if (active_generation != wire_generation) {
					return; // Leave a connection reopened by the push handler to its new pump.
				}
			}
			continue;
		}
		Ref<GDRedisCallInternal> head = inflight.front()->get();
		int at = buf_at;
		Variant value;
		bool ready = false;
		const Ref<R> got = take_reply(at, value, ready, slice_due);
		buf_at = at;
		if (got->get_e().is_valid() && !ready) {
			// Propagate transport or framing failures to every operation on the connection.
			fail_connection(got);
			return;
		}
		if (!ready) {
			break; // Wait for the incomplete reply.
		}
		// Always advance the receive cursor after consuming a complete reply.
		// Otherwise an error reply could be mistaken for the next command's result.
		if (got->get_e().is_valid()) {
			// Keep per-command errors local to that command within the pipeline.
			head->collected.push_back(got);
		} else {
			const bool aborted = head->shape == GDRedisCallInternal::EXEC && head->collected.size() + 1 == head->want_replies;
			head->collected.push_back(value.get_type() == Variant::NIL ? Variant(redis_nil(aborted)) : value);
		}
		if (head->collected.size() >= head->want_replies) {
			const Ref<R> result = head->result();
			inflight.pop_front(); // Remove the operation before notifying its waiter.
			head->done(result);
			if (active_generation != wire_generation) {
				return; // Leave a connection reopened by the completion handler to its new pump.
			}
		}
	}
	if (live->get_e().is_valid() && !reply_yielded) {
		fail_connection(live);
		return;
	}
	if (reply_yielded || sock.available() > 0) {
		post_pump();
	}

	if (!inflight.is_empty()) {
		Ref<GDRedisCallInternal> head = inflight.front()->get();
		if (head->due > 0 && GDClock::msec() >= head->due) {
			fail_connection(R::err("redis did not answer in time", Err::TIMED_OUT));
		}
	}
	if (inflight.is_empty() && !subscribed && out_buf.is_empty() && next_buf.is_empty() && packing.is_empty()) {
		watch(false);
	}
}

// Deliver pushed messages shaped as ["message", channel, payload].
// Pattern subscriptions use ["pmessage", pattern, channel, payload].
void GDRedisClient::emit_push(const Variant &p_reply) {
	if (p_reply.get_type() != Variant::ARRAY) {
		return;
	}
	const Array a = p_reply;
	if (a.size() < 3) {
		return; // Do not deliver subscription acknowledgments or other non-message replies.
	}
	const String head = a[0];
	if (head == "message") {
		emit_signal("message", String(a[1]), String(a[2]));
	} else if (head == "pmessage" && a.size() >= 4) {
		emit_signal("message", String(a[2]), String(a[3]));
	}
}

// Enable socket notifications only while they are needed.
void GDRedisClient::watch(bool p_on) {
	watching = p_on;
	if (!p_on) {
		pump_posted = false;
	}
}

// Advance only connections reported ready by the kernel.
void GDRedisClient::socket_ready() {
	pump_posted = false;
	if (opening.is_valid()) {
		opening->step();
		return;
	}
	if (watching) {
		pump();
	}
}

// Schedule buffered continuation even without new socket input.
void GDRedisClient::post_pump() {
	if (!watching || pump_posted) {
		return;
	}
	pump_posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDRedisClient::dispatch_pump));
}

// Ignore a ready-queue entry whose work became unnecessary.
void GDRedisClient::dispatch_pump() {
	if (!pump_posted) {
		return;
	}
	pump_posted = false;
	if (watching) {
		pump();
	}
}

// Register public methods and properties with script.
void GDRedisClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "host", "port", "opts"), &GDRedisClient::open, DEFVAL("127.0.0.1"), DEFVAL(6379), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("query", "cmd", "args"), &GDRedisClient::query, DEFVAL(Array()));
	ADD_AWAIT("open", "R:Variant");
	ADD_AWAIT("query", "R:Variant");
	ClassDB::bind_method(D_METHOD("pipeline", "cmds"), &GDRedisClient::pipeline);
	ClassDB::bind_method(D_METHOD("transaction", "cmds"), &GDRedisClient::transaction);
	ClassDB::bind_method(D_METHOD("subscribe", "channels"), &GDRedisClient::subscribe);
	ClassDB::bind_method(D_METHOD("open_async", "host", "port", "opts"), &GDRedisClient::open, DEFVAL("127.0.0.1"), DEFVAL(6379), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("query_async", "cmd", "args"), &GDRedisClient::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("pipeline_async", "cmds"), &GDRedisClient::pipeline);
	ClassDB::bind_method(D_METHOD("transaction_async", "cmds"), &GDRedisClient::transaction);
	ClassDB::bind_method(D_METHOD("subscribe_async", "channels"), &GDRedisClient::subscribe);
	ADD_AWAIT("pipeline", "R:Array");
	ADD_AWAIT("transaction", "R:Array");
	ADD_AWAIT("subscribe", "R:Variant");
	ADD_AWAIT("open_async", "R:Variant");
	ADD_AWAIT("query_async", "R:Variant");
	ADD_AWAIT("pipeline_async", "R:Array");
	ADD_AWAIT("transaction_async", "R:Array");
	ADD_AWAIT("subscribe_async", "R:Variant");
	ADD_AUTO_WAIT("open");
	ADD_AUTO_WAIT("query");
	ADD_AUTO_WAIT("pipeline");
	ADD_AUTO_WAIT("transaction");
	ADD_AUTO_WAIT("subscribe");
	ADD_SIGNAL(MethodInfo("message", PropertyInfo(Variant::STRING, "channel"), PropertyInfo(Variant::STRING, "payload")));
	ClassDB::bind_method(D_METHOD("is_open"), &GDRedisClient::is_open);
	ClassDB::bind_method(D_METHOD("is_subscribed"), &GDRedisClient::is_subscribed);
	ClassDB::bind_method(D_METHOD("in_flight"), &GDRedisClient::in_flight);
	ClassDB::bind_method(D_METHOD("close"), &GDRedisClient::close);
}
