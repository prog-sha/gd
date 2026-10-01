/**************************************************************************/
/*  redis.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement GDRedisClient connections declared in redis.h.

#include "cli/db/redis.h"
#include "cli/db/options.h"
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
Ref<Err> redis_nil(bool p_transaction = false) {
	Dictionary info;
	info["code"] = p_transaction ? "tx_failed" : "nil";
	return Err::make(p_transaction ? "transaction aborted" : "value not found",
			p_transaction ? Err::INTERRUPTED : Err::NOT_FOUND, info);
}

// Build a native success without exposing an intermediate result object.
VariantPair redis_ok(const Variant &p_value = Variant()) {
	return { p_value, Variant() };
}

// Keep the completed value on its error while returning two native slots.
VariantPair redis_fail(const Variant &p_reason, Err::Kind p_kind = Err::NONE, const Variant &p_value = Variant()) {
	const Ref<Err> error = Err::from(p_reason, p_kind);
	return { p_value, error->with_partial(p_value) };
}

// Read the failure slot of an internal result.
Ref<Err> redis_error(const VariantPair &p_result) {
	return p_result.error;
}

// Create a deadline without overflow; zero means no deadline.
uint64_t deadline_after(uint64_t p_wait) {
	const uint64_t now = GDClock::msec();
	return p_wait == 0 ? 0 : (p_wait > UINT64_MAX - now ? UINT64_MAX : now + p_wait);
}

// Read phase-specific timeouts with a shared explicit override.
bool redis_timeouts(const Dictionary &p_opts, uint64_t &r_dial, uint64_t &r_read, uint64_t &r_write) {
	uint64_t common = 5000;
	if (!DbOption::seconds(p_opts, "timeout", 5.0, common)) return false;
	const double fallback = double(common) / 1000.0;
	return DbOption::seconds(p_opts, "dial_timeout", fallback, r_dial) &&
			DbOption::seconds(p_opts, "read_timeout", fallback, r_read) &&
			DbOption::seconds(p_opts, "write_timeout", fallback, r_write);
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
void GDRedisCallInternal::fail_later(const VariantPair &p_out) {
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
	pending_ready = true;
	schedule();
}

// Advance the asynchronous operation by one state.
void GDRedisCallInternal::step() {
	posted = false;
	Ref<GDRedisCallInternal> keep(this); // Remain alive while connection cleanup releases self-references.
	if (pending_ready) {
		const VariantPair out = pending;
		pending = VariantPair();
		pending_ready = false;
		if (dropped) {
			// Notify cancellation but retain queue position because the reply is still expected.
			if (authenticating && db.is_valid()) {
				pending = out;
				pending_ready = true;
				db->fail_connection(out); // The authentication callback delivers cancellation after closing the socket.
				return;
			}
			if (!notified) {
				notified = true;
				const VariantPair reply = shape == VALUE ? out : result(out);
				emit_result(reply.value, redis_error(reply));
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
		const VariantPair why = redis_fail("redis did not answer in time", Err::TIMED_OUT);
		if ((mode == READING || mode == WRITING) && db.is_valid()) {
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
	if (mode == RESOLVING || mode == WRITING) {
		return; // Wait for worker-based resolution or socket write readiness.
	}
	if (mode == OPENING) {
		db->sock.poll();
		const Wire::State st = db->sock.state();
		if (st == Wire::FAILED || st == Wire::CLOSED) {
			// Propagate the transport cause without reclassifying encrypted connections.
			const VariantPair why = redis_fail(sock_error(db->sock, "connection closed"));
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
				const VariantPair why = redis_fail(sock_error(db->sock, "TLS setup failed", Err::NONE));
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
			done(redis_ok());
			return;
		}
		// Send authentication and wait for its reply before reporting success.
		// Let the ordinary connection pump read AUTH once setup-specific handling ends.
		mode = READING;
		authenticating = true;
		set_due(0); // The authentication command owns its write and read deadlines.
		if (db->opening.ptr() == this) {
			db->opening.unref();
		}
		Array args;
		args.push_back(password);
		Ref<GDRedisCallInternal> auth = db->start("AUTH", args);
		auth->connect("finished", callable_mp(this, &GDRedisCallInternal::on_auth), Object::CONNECT_ONE_SHOT);
		return;
	}

	const Ref<Err> got = db->fill();
	if (got.is_valid()) {
		done(redis_fail(got));
		return;
	}
	int at = db->buf_at;
	Variant value;
	bool ready = false;
	const Ref<Err> read = db->take_reply(at, value, ready, GDClock::usec() + GD_SCHED_SLICE_USEC);
	db->buf_at = at; // Do not rewind a parsed prefix when a reply remains incomplete.
	if (read.is_valid()) {
		done(redis_fail(read));
		return;
	}
	if (!ready) {
		return; // Resume on the next turn.
	}
	db->buf_at = at;
	done(redis_ok(value));
}

// Receive worker-resolved addresses and proceed to socket connection.
void GDRedisClient::resolved(const Variant &p_value, const Ref<Err> &p_error, const Ref<GDRedisCallInternal> &p_call) {
	if (p_call.is_null() || opening.ptr() != p_call.ptr() || p_call->self_hold.is_null() || p_call->db.ptr() != this || p_call->mode != GDRedisCallInternal::RESOLVING) {
		return;
	}
	if (Pool::is_stopping()) {
		p_call->fail_later(redis_fail("worker pool stopped", Err::INTERRUPTED));
		return;
	}
	if (p_error.is_valid()) {
		p_call->fail_later(redis_fail(p_error));
		return;
	}
	const Dictionary prepared = p_value;
	const String addr = prepared.get("address", "");
	ca = prepared.get("ca", Variant());
	if (addr.is_empty()) {
		p_call->fail_later(redis_fail(vformat("cannot resolve \"%s\"", host), Err::NOT_FOUND));
		return;
	}
	p_call->mode = GDRedisCallInternal::OPENING;
	if (sock.open(addr, port, p_call->due) != OK) {
		p_call->fail_later(redis_fail(sock_error(sock, vformat("cannot reach %s:%d", host, port))));
	} else {
		p_call->schedule();
	}
}

// Handle the key-value authentication result.
void GDRedisCallInternal::on_auth(const Variant &p_value, const Variant &p_error) {
	authenticating = false;
	const Ref<Err> error = p_error;
	if (error.is_valid()) {
		const VariantPair why = redis_fail(error->note("auth failed"), Err::NONE, p_value);
		if (db.is_valid()) {
			db->close(); // Do not reuse an unauthenticated connection for ordinary queries.
		}
		done(why);
		return;
	}
	done(redis_ok());
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
	pending = VariantPair();
	pending_ready = false;
	password = String();
	want_replies = 1;
	collected = Array();
	shape = VALUE;
	dropped = false;
	authenticating = false;
	notified = false;
}

// Preserve complete replies and mark unknown command results when the operation fails.
VariantPair GDRedisCallInternal::result(const VariantPair &p_error) const {
	if (shape == VALUE) {
		if (redis_error(p_error).is_valid()) return p_error;
		const Variant value = collected[0];
		const Ref<Err> one = value;
		return one.is_valid() ? redis_fail(one) : redis_ok(value);
	}
	Ref<Err> error = redis_error(p_error);
	Array replies;
	const int count = shape == EXEC ? MAX(0, want_replies - 2) : want_replies;
	if (shape == BATCH) replies = collected;
	else if (collected.size() == want_replies) {
		const Variant value = collected[collected.size() - 1];
		const Ref<Err> one = value;
		if (one.is_valid()) error = one;
		else if (value.get_type() == Variant::ARRAY) replies = value;
	} else if (error.is_valid() && db.is_valid() && !db->inflight.is_empty() &&
			db->inflight.front()->get().ptr() == this && collected.size() == want_replies - 1 &&
			!db->reply_stack.is_empty() && db->reply_stack[0].kind == '*') {
		replies = db->reply_stack[0].items; // Completed EXEC elements precede any unfinished nested value.
	}
	if (redis_error(p_error).is_valid()) replies = replies.duplicate(); // Cancellation leaves parser storage live.
	const int completed = replies.size();
	// Failure placeholders preserve command positions without claiming execution did not occur.
	if (error.is_valid()) {
		while (replies.size() < count) replies.push_back(error);
	}
	Array failures;
	if (error.is_valid()) failures.push_back(error);
	for (int i = 0; i < replies.size(); i++) {
		if (replies[i].get_type() == Variant::NIL) replies[i] = redis_nil();
		const Ref<Err> one = replies[i];
		if (i < completed && one.is_valid() && one.ptr() != error.ptr()) failures.push_back(one->note(vformat("command %d", i)));
	}
	if (failures.size() == 1) error = failures[0];
	else if (failures.size() > 1) error = Err::join(failures);
	return error.is_valid() ? redis_fail(error, Err::NONE, replies) : redis_ok(replies);
}

// Emit both result slots while keeping completed work on a failure reason.
void GDRedisCallInternal::emit_result(const Variant &p_value, const Ref<Err> &p_error) {
	Async::finish(this, SNAME("finished"), p_value, p_error.is_valid() ? Variant(p_error->with_partial(p_value)) : Variant());
}

// Finish the operation and deliver the result to its waiter.
void GDRedisCallInternal::done(const VariantPair &p_out) {
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
		const VariantPair out = redis_error(p_out).is_valid() && shape != VALUE && p_out.value.get_type() != Variant::ARRAY ? result(p_out) : p_out;
		emit_result(out.value, redis_error(out));
	} else if (!notified && pending_ready) {
		// Deliver any pending cancellation before returning the operation for reuse.
		// A reply may arrive before the scheduled cancellation turn and recycle the operation,
		// otherwise removing its notification and leaving the waiter suspended forever.
		const VariantPair late = pending;
		pending = VariantPair();
		pending_ready = false;
		notified = true;
		const VariantPair reply = shape == VALUE ? late : result(late);
		emit_result(reply.value, redis_error(reply));
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
	pending = redis_fail("cancelled", Err::INTERRUPTED);
	pending_ready = true;
	schedule();
}

// Release the transport immediately after its script owner disappears.
void GDRedisCallInternal::abort() {
	if (db.is_valid()) db->close();
	else cancel();
}

// Register public methods and properties with script.
void GDRedisCallInternal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDRedisCallInternal::cancel);
	ClassDB::bind_method(D_METHOD("abort"), &GDRedisCallInternal::abort);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
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
	fail_connection(redis_fail("connection closed", Err::INTERRUPTED));
}

// Fail all waiters after timeout or malformed framing makes reply order unusable.
void GDRedisClient::fail_connection(const VariantPair &p_why) {
	// Snapshot the parser's completed root elements before releasing its unfinished frames.
	const Ref<GDRedisCallInternal> head = inflight.is_empty() ? Ref<GDRedisCallInternal>() : inflight.front()->get();
	const VariantPair partial = head.is_valid() ? head->result(p_why) : VariantPair();
	wire_generation++;
	out_buf.clear();
	next_buf.clear();
	writing.unref();
	next_writing.unref();
	writing_generation = 0;
	next_writing_generation = 0;
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
Ref<Err> GDRedisClient::fill() {
	const int old_at = buf_at;
	const Ref<Err> got = sock_fill(sock, buf, buf_at, REDIS_READ_CHUNK, REDIS_RECV_MAX);
	if (old_at > 0 && buf_at == 0) {
		line_at = MAX(0, line_at - old_at);
	}
	return got;
}

// Parse one RESP reply for the caller.
Ref<Err> GDRedisClient::take_reply(int &r_at, Variant &r_value, bool &r_ready, uint64_t p_due) {
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
	auto accept = [&](Variant p_value, Ref<Err> p_result, bool &r_limited) {
		while (!reply_stack.is_empty()) {
			ParseFrame &frame = reply_stack[reply_stack.size() - 1];
			const Variant stored = p_result.is_valid() ? Variant(p_result) : p_value;
			const int64_t slot_bytes = frame.kind == '%' ? MAP_VALUE_BYTES : ARRAY_VALUE_BYTES;
			if (!keep_memory(slot_bytes)) {
				r_limited = true;
				return Err::from("reply exceeds the memory limit", Err::LIMITED);
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
				return Ref<Err>();
			}
			p_value = frame.kind == '%' ? Variant(frame.map) : Variant(frame.items);
			p_result = Ref<Err>();
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
				return Ref<Err>();
			}
			if (buf[r_at + bulk_len] != '\r' || buf[r_at + bulk_len + 1] != '\n') {
				return Err::from("bulk reply has no CRLF", Err::INVALID_DATA);
			}
			// String::append_utf8 allocates one char32_t per input byte before conversion.
			if (!keep_memory((int64_t(bulk_len) + 1) * sizeof(char32_t))) {
				return Err::from("reply exceeds the memory limit", Err::LIMITED);
			}
			const VariantPair decoded = utf8_value(buf.ptr() + r_at, bulk_len);
			const Ref<Err> text_error = decoded.error;
			if (text_error.is_valid()) return Err::from(text_error);
			const Variant value = decoded.value;
			r_at += bulk_len + 2;
			bulk_len = -1;
			bool memory_limited = false;
			Ref<Err> done = accept(value, Ref<Err>(), memory_limited);
			if (memory_limited) {
				return done;
			}
			if (r_ready) {
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
			return Ref<Err>();
		}
		const String line = String::utf8((const char *)buf.ptr() + r_at, end - r_at);
		r_at = end + 2;
		line_at = r_at;
		if (line.is_empty()) {
			return Err::from("empty reply", Err::INVALID_DATA);
		}
		const char32_t kind = line[0];
		const String rest = line.substr(1);
		Variant value;
		Ref<Err> result = Ref<Err>();
		int64_t value_memory = 0;
		switch (kind) {
			case '+':
				value = rest;
				value_memory = (int64_t(rest.length()) + 1) * sizeof(char32_t);
				break;
			case '-':
				result = Err::from(rest, Err::INVALID_DATA);
				value_memory = (int64_t(rest.length()) + 1) * sizeof(char32_t);
				break;
			case ':': {
				int64_t n = 0;
				if (!resp_int(rest, n)) {
					return Err::from("invalid integer reply", Err::INVALID_DATA);
				}
				value = n;
			} break;
			case ',':
				if (!rest.is_valid_float()) {
					return Err::from("invalid float reply", Err::INVALID_DATA);
				}
				value = rest.to_float();
				break;
			case '#':
				if (rest != "t" && rest != "f") {
					return Err::from("invalid boolean reply", Err::INVALID_DATA);
				}
				value = rest == "t";
				break;
			case '_':
				if (!rest.is_empty()) {
					return Err::from("invalid null reply", Err::INVALID_DATA);
				}
				break;
			case '$': {
				int64_t n = 0;
				if (!resp_int(rest, n) || n < -1 || n > REPLY_BULK_MAX) {
					return Err::from("invalid bulk length", Err::INVALID_DATA);
				}
				if (n >= 0) {
					const int64_t text_bytes = (n + 1) * sizeof(char32_t);
					if (text_bytes > REPLY_MEMORY_MAX - reply_memory) {
						return Err::from("reply exceeds the memory limit", Err::LIMITED);
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
					return Err::from(kind == '%' ? "invalid map count" : "invalid aggregate count", Err::INVALID_DATA);
				}
				if (count < 0) {
					break;
				}
				const int values = kind == '%' ? (int)count * 2 : (int)count;
				if (values > 0) {
					if (reply_stack.size() >= Variant::MAX_RECURSION_DEPTH) {
						return Err::from("reply exceeds Variant nesting capacity", Err::INVALID_DATA);
					}
					const int64_t slot_bytes = kind == '%' ? MAP_VALUE_BYTES : ARRAY_VALUE_BYTES;
					if (int64_t(values) > (REPLY_MEMORY_MAX - reply_memory) / slot_bytes || !keep_memory(sizeof(ParseFrame))) {
						return Err::from("reply exceeds the memory limit", Err::LIMITED);
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
				return Err::from(vformat("unknown reply type '%s'", String::chr(kind)), Err::INVALID_DATA);
		}
		if (value_memory > 0 && !keep_memory(value_memory)) {
			return Err::from("reply exceeds the memory limit", Err::LIMITED);
		}
		bool memory_limited = false;
		Ref<Err> done = accept(value, result, memory_limited);
		if (memory_limited) {
			return done;
		}
		if (r_ready) {
			reply_memory = 0;
			return done;
		}
	}
	reply_yielded = true;
	return Ref<Err>();
}

// Open the destination and prepare it for use.
Signal GDRedisClient::open(const String &p_host, int64_t p_port, const Dictionary &p_opts) {
	uint64_t dial_wait = 0, read_wait = 0, write_wait = 0;
	if (!redis_timeouts(p_opts, dial_wait, read_wait, write_wait)) {
		Ref<GDRedisCallInternal> bad;
		bad.instantiate();
		bad->self_hold = bad;
		bad->fail_later(redis_fail("redis timeouts must be zero or positive seconds", Err::INVALID_DATA));
		return Signal(bad.ptr(), "finished");
	}
	if (p_host.is_empty() || p_port < Limit::PORT_MIN || p_port > Limit::PORT_MAX) {
		Ref<GDRedisCallInternal> bad;
		bad.instantiate();
		bad->self_hold = bad;
		bad->fail_later(redis_fail("redis address is invalid", Err::INVALID_DATA));
		return Signal(bad.ptr(), "finished");
	}
	Wire::Guard parsed_guard;
	// Select verify-full for explicit true or external-host defaults, and disable for default loopback.
	if (!Wire::guard_of(p_opts.get("tls", Wire::default_guard(p_host)), parsed_guard)) {
		Ref<GDRedisCallInternal> bad;
		bad.instantiate();
		bad->self_hold = bad;
		bad->fail_later(redis_fail("tls must be one of disable / require / verify-full", Err::INVALID_DATA));
		return Signal(bad.ptr(), "finished");
	}
	String ca_path, password;
	if (!DbOption::text(p_opts, "ca", "", ca_path) || !DbOption::text(p_opts, "password", "", password)) {
		Ref<GDRedisCallInternal> bad;
		bad.instantiate();
		bad->self_hold = bad;
		bad->fail_later(redis_fail("redis credentials must be text", Err::INVALID_DATA));
		return Signal(bad.ptr(), "finished");
	}

	Ref<GDRedisCallInternal> call;
	call.instantiate();
	call->db = Ref<GDRedisClient>(this);
	call->self_hold = call;
	call->mode = GDRedisCallInternal::OPENING;
	call->password = password;
	call->set_due(dial_wait);

	// Accept hostnames as well as numeric addresses for container and production endpoints.
	// Check network permission before resolving names, since a later connection check
	// would still allow the DNS query to leave the process.
	if (!Perm::check(Perm::NET, vformat("%s:%d", p_host, p_port))) {
		call->fail_later(redis_fail(vformat("net access to \"%s\" is not allowed", p_host), Err::PERMISSION_DENIED));
		return Signal(call.ptr(), "finished");
	}
	if (opening.is_valid() || sock.is_valid() || !inflight.is_empty() || !packing.is_empty()) {
		const uint64_t previous = wire_generation;
		fail_connection(redis_fail("connection replaced", Err::INTERRUPTED));
		if (wire_generation != previous + 1 || opening.is_valid() || sock.is_valid()) {
			call->fail_later(redis_fail("connection replaced during notification", Err::INTERRUPTED));
			return Signal(call.ptr(), "finished");
		}
	}
	// Commit settings only after old waiters have finished, preserving a reentrant open.
	host = p_host;
	port = int(p_port);
	password = call->password;
	read_ms = read_wait;
	write_ms = write_wait;
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
		GDPairCall::start([lookup_host, ca_path]() { return Wire::prepare(lookup_host, ca_path); }, false).connect(
				callable_mp(this, &GDRedisClient::resolved).bind(call), Object::CONNECT_ONE_SHOT);
		return Signal(call.ptr(), "finished");
	}
	if (sock.open(host, port, call->due) != OK) {
		call->fail_later(redis_fail(sock_error(sock, vformat("cannot reach %s:%d", host, port))));
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
	call->mode = GDRedisCallInternal::WRITING;
	call->set_due(write_ms);
	call->want_replies = MAX(1, p_channels.size()); // Each subscription produces one acknowledgment.

	if (!is_open()) {
		call->fail_later(redis_fail("not connected", Err::NOT_FOUND));
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
void GDRedisPoolCall::received(const Variant &p_value, const Variant &p_error) {
	if (done) {
		return;
	}
	inner = Signal();
	if (opening) {
		opening = false;
		pool->opening--;
		pool->schedule();
		if (p_error.get_type() == Variant::NIL) {
			start(conn, false);
			return;
		}
	}
	finish({ p_value, p_error });
}

// Detach wait and inner-signal state, then schedule delivery after returning the connection.
void GDRedisPoolCall::finish(const VariantPair &p_out, bool p_close) {
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
	outcome = p_out;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDRedisPoolCall::deliver));
	self_hold.unref(); // The ready queue retains this object, avoiding an undelivered self-cycle at shutdown.
}

// Deliver completion exactly once and release the retained result.
void GDRedisPoolCall::deliver() {
	Ref<GDRedisPoolCall> keep(this);
	const VariantPair out = outcome;
	outcome = VariantPair();
	Async::finish(this, SNAME("finished"), out.value, out.error);
}

// Remove only calls whose connection-acquisition deadline expired.
void GDRedisPoolCall::expired() {
	if (!done && due > 0 && GDClock::msec() >= due) {
		finish({ Variant(), Err::make("redis pool wait timed out", Err::TIMED_OUT) }, true);
	}
}

// Close only this call's exclusively held connection during setup or execution.
void GDRedisPoolCall::cancel() {
	finish({ Variant(), Err::make("cancelled", Err::INTERRUPTED) }, true);
}

// Register result notification and cancellation.
void GDRedisPoolCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDRedisPoolCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
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
		return Async::ready_pair({ Variant(), Err::make("pool size must be between 0 and 2147483647", Err::INVALID_DATA) });
	}
	if (p_host.is_empty() || p_port < Limit::PORT_MIN || p_port > Limit::PORT_MAX) {
		return Async::ready_pair({ Variant(), Err::make("redis address is invalid", Err::INVALID_DATA) });
	}
	const int64_t cpus = MAX(1, GDSystem::cpus());
	const int standard_size = int(MIN(int64_t(INT_MAX), cpus * 10));
	const int want = p_size > 0 ? int(p_size) : (default_size > 0 ? default_size : standard_size);
	if (default_size < 0) {
		return Async::ready_pair({ Variant(), Err::make("pool size must be between 0 and 2147483647", Err::INVALID_DATA) });
	}
	uint64_t dial = 0, read = 0, write = 0;
	uint64_t wait = 0, max_idle = 0;
	if (!redis_timeouts(p_opts, dial, read, write) ||
			!DbOption::seconds(p_opts, "pool_timeout", read > 0 ? double(read) / 1000.0 + 1.0 : 30.0, wait) ||
			!DbOption::seconds(p_opts, "conn_max_idle_time", 1800.0, max_idle)) {
		return Async::ready_pair({ Variant(), Err::make("timeouts must be zero or positive seconds", Err::INVALID_DATA) });
	}
	Wire::Guard guard;
	if (!Wire::guard_of(p_opts.get("tls", Wire::default_guard(p_host)), guard)) {
		return Async::ready_pair({ Variant(), Err::make("redis pool connection options are invalid", Err::INVALID_DATA) });
	}
	String credential;
	if (!DbOption::text(p_opts, "ca", "", credential) || !DbOption::text(p_opts, "password", "", credential)) {
		return Async::ready_pair({ Variant(), Err::make("redis credentials must be text", Err::INVALID_DATA) });
	}
	close();
	host = p_host;
	port = int(p_port);
	opts = p_opts.duplicate(true);
	max_open = want;
	dial_limit = MIN(standard_size, max_open); // Bound simultaneous dials within the selected pool capacity.
	wait_ms = wait;
	idle_ms = max_idle;
	configured = true;
	return Async::ready_pair({ Variant(), Variant() });
}

// Queue a query in FIFO order with a cancelable, deadline-aware acquisition wait.
Signal GDRedisPool::query(const String &p_cmd, const Array &p_args) {
	if (!configured) {
		return Async::ready_pair({ Variant(), Err::make("pool is not open", Err::NOT_FOUND) });
	}
	// Commands that change connection state require a dedicated client.
	const String name = p_cmd.to_upper();
	if (name == "SELECT" || name == "MULTI" || name == "EXEC" || name == "DISCARD" ||
			name == "WATCH" || name == "UNWATCH" || name == "SUBSCRIBE" || name == "PSUBSCRIBE" ||
			name == "SSUBSCRIBE" || name == "UNSUBSCRIBE" || name == "PUNSUBSCRIBE" ||
			name == "SUNSUBSCRIBE" || name == "MONITOR" || name == "AUTH" ||
			name == "HELLO" || name == "QUIT" || name == "RESET" || name == "CLIENT" ||
			name == "READONLY" || name == "READWRITE" || name == "ASKING" ||
			(name == "SCRIPT" && !p_args.is_empty() && String(p_args[0]).to_upper() == "DEBUG")) {
		return Async::ready_pair({ Variant(), Err::make("redis command needs a dedicated connection", Err::INVALID_DATA) });
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
			const Idle item = idle[idle.size() - 1];
			conn = item.conn;
			idle.resize(idle.size() - 1);
			if (idle_ms > 0 && GDClock::msec() - item.since >= idle_ms) {
				conns.erase(conn);
				conn->close();
				conn.unref();
				continue;
			}
			// Check a retained socket before a waiting command can use it.
			conn->sock.poll();
			if (conn->is_open() && !conn->is_subscribed()) {
				break;
			}
			conns.erase(conn);
			conn->close();
			conn.unref();
		}
		const bool create = conn.is_null();
		if (create) {
			if (opening >= dial_limit || conns.size() >= max_open) {
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
			Idle item;
			item.conn = p_conn;
			item.since = GDClock::msec();
			idle.push_back(item);
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
		call->finish({ Variant(), Err::make("redis pool closed", Err::INTERRUPTED) }, true);
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
	ADD_AWAIT("open", "Pair:Variant");
	ADD_AWAIT("query", "Pair:Variant");
	ADD_AWAIT("open_async", "Pair:Variant");
	ADD_AWAIT("query_async", "Pair:Variant");
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

// Encode RESP on a worker.
void GDRedisPackJob::run() {
	if (!batch) {
		if (!GDRedisClient::pack_cmd(packed, cmd, args)) {
			error = Err::make("redis send limit exceeded", Err::LIMITED);
		}
		return;
	}
	for (int i = 0; i < cmds.size(); i++) {
		if (cmds[i].get_type() != Variant::ARRAY) {
			error = Err::make(vformat("command %d is not an Array", i), Err::INVALID_DATA);
			return;
		}
		const Array one = cmds[i];
		if (one.is_empty()) {
			error = Err::make(vformat("command %d is empty", i), Err::INVALID_DATA);
			return;
		}
		if (!GDRedisClient::pack_cmd(packed, Pool::text(one[0]), one.slice(1))) {
			error = Err::make("redis pipeline exceeds the send limit", Err::LIMITED);
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
		job->call->done(redis_fail("worker pool stopped", Err::INTERRUPTED));
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
			job->call->done(redis_fail("cancelled", Err::INTERRUPTED));
		} else if (job->error.is_valid()) {
			job->call->fail_later(redis_fail(job->error));
		} else if (!is_open()) {
			job->call->fail_later(redis_fail("not connected", Err::NOT_FOUND));
		} else if (job->call->due > 0 && GDClock::msec() >= job->call->due) {
			job->call->fail_later(redis_fail("redis command encoding timed out", Err::TIMED_OUT));
		} else {
			if (out_buf.is_empty() && next_buf.is_empty()) {
				out_buf = static_cast<ByteBuf &&>(job->packed);
				writing = job->call;
				writing_generation = job->call->generation;
			} else {
				next_buf = static_cast<ByteBuf &&>(job->packed);
				next_writing = job->call;
				next_writing_generation = job->call->generation;
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
		fail_connection(redis_fail(sock_error(sock, "connection lost")));
		return;
	}
	if (!sock_flush(sock, out_buf)) {
		fail_connection(redis_fail(sock_error(sock, "send failed")));
		return;
	}
	if (out_buf.is_empty()) {
		const Ref<GDRedisCallInternal> sent = writing;
		const uint64_t sent_generation = writing_generation;
		writing.unref();
		writing_generation = 0;
		if (sent.is_valid() && sent->self_hold.is_valid() && sent->generation == sent_generation && sent->mode == GDRedisCallInternal::WRITING) {
			sent->mode = GDRedisCallInternal::READING;
			sent->set_due(read_ms);
		}
	}
	if (out_buf.is_empty() && !next_buf.is_empty()) {
		out_buf = static_cast<ByteBuf &&>(next_buf); // Move to the next command without copying the whole buffer.
		writing = next_writing;
		writing_generation = next_writing_generation;
		next_writing.unref();
		next_writing_generation = 0;
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
	call->mode = GDRedisCallInternal::WRITING;
	call->set_due(write_ms);

	if (!is_open()) {
		call->fail_later(redis_fail("not connected", Err::NOT_FOUND));
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
	call->mode = GDRedisCallInternal::WRITING;
	call->set_due(write_ms);
	call->want_replies = p_cmds.size();
	call->shape = GDRedisCallInternal::BATCH;

	if (p_cmds.is_empty()) {
		call->fail_later(redis_ok(Array()));
		return call;
	}
	if (!is_open()) {
		call->fail_later(redis_fail("not connected", Err::NOT_FOUND));
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
			job->call->pending = redis_fail("redis command encoding timed out", Err::TIMED_OUT);
			job->call->pending_ready = true;
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
	const Ref<Err> live = fill();
	// Finish buffered replies across parser yields before failing the remaining calls.

	// Deliver replies until the first waiter is satisfied, yielding when input is incomplete.
	// A reply without a waiter is an unsolicited message.
	while (!inflight.is_empty() || subscribed) {
		if (inflight.is_empty()) {
			int at2 = buf_at;
			Variant pushed;
			bool ready2 = false;
			const Ref<Err> got2 = take_reply(at2, pushed, ready2, slice_due);
			buf_at = at2;
			if (got2.is_valid() && !ready2) {
				// Close subscriptions too when malformed pushes destroy framing boundaries.
				fail_connection(redis_fail(got2));
				return;
			}
			if (!ready2) {
				break;
			}
			if (got2.is_null()) {
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
		const Ref<Err> got = take_reply(at, value, ready, slice_due);
		buf_at = at;
		if (got.is_valid() && !ready) {
			// Propagate transport or framing failures to every operation on the connection.
			fail_connection(redis_fail(got));
			return;
		}
		if (!ready) {
			break; // Wait for the incomplete reply.
		}
		// Always advance the receive cursor after consuming a complete reply.
		// Otherwise an error reply could be mistaken for the next command's result.
		if (got.is_valid()) {
			// Keep per-command errors local to that command within the pipeline.
			head->collected.push_back(got);
		} else {
			const bool aborted = head->shape == GDRedisCallInternal::EXEC && head->collected.size() + 1 == head->want_replies;
			head->collected.push_back(value.get_type() == Variant::NIL ? Variant(redis_nil(aborted)) : value);
		}
		if (head->collected.size() >= head->want_replies) {
			const VariantPair result = head->result();
			inflight.pop_front(); // Remove the operation before notifying its waiter.
			head->done(result);
			if (active_generation != wire_generation) {
				return; // Leave a connection reopened by the completion handler to its new pump.
			}
		}
	}
	if (live.is_valid() && !reply_yielded) {
		fail_connection(redis_fail(live));
		return;
	}
	if (reply_yielded || sock.available() > 0) {
		post_pump();
	}

	if (!inflight.is_empty()) {
		Ref<GDRedisCallInternal> head = inflight.front()->get();
		if (head->due > 0 && GDClock::msec() >= head->due) {
			fail_connection(redis_fail("redis did not answer in time", Err::TIMED_OUT));
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
	ADD_AWAIT("open", "Pair:Variant");
	ADD_AWAIT("query", "Pair:Variant");
	ClassDB::bind_method(D_METHOD("pipeline", "cmds"), &GDRedisClient::pipeline);
	ClassDB::bind_method(D_METHOD("transaction", "cmds"), &GDRedisClient::transaction);
	ClassDB::bind_method(D_METHOD("subscribe", "channels"), &GDRedisClient::subscribe);
	ClassDB::bind_method(D_METHOD("open_async", "host", "port", "opts"), &GDRedisClient::open, DEFVAL("127.0.0.1"), DEFVAL(6379), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("query_async", "cmd", "args"), &GDRedisClient::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("pipeline_async", "cmds"), &GDRedisClient::pipeline);
	ClassDB::bind_method(D_METHOD("transaction_async", "cmds"), &GDRedisClient::transaction);
	ClassDB::bind_method(D_METHOD("subscribe_async", "channels"), &GDRedisClient::subscribe);
	ADD_AWAIT("pipeline", "Pair:Array");
	ADD_AWAIT("transaction", "Pair:Array");
	ADD_AWAIT("subscribe", "Pair:Variant");
	ADD_AWAIT("open_async", "Pair:Variant");
	ADD_AWAIT("query_async", "Pair:Variant");
	ADD_AWAIT("pipeline_async", "Pair:Array");
	ADD_AWAIT("transaction_async", "Pair:Array");
	ADD_AWAIT("subscribe_async", "Pair:Variant");
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
