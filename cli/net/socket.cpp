/**************************************************************************/
/*  socket.cpp                                                            */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement standard TCP streams, listeners, and UDP endpoints declared in socket.h.

#include "cli/net/socket.h"
#include "cli/sys/clock.h"
#include "cli/net/lookup.h"

#include "cli/api/cli.h"
#include "cli/data/bytes.h"
#include "cli/sys/file_job.h"
#include "cli/sys/perm.h"
#include "cli/sys/task.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "cli/sys/sched.h"
#include "core/templates/hash_set.h"

namespace {

HashSet<GDTCPDialCall *> tcp_dials; // Pending TCP dials cancelled at shutdown.
HashSet<GDTLSDialCall *> tls_dials; // Pending TLS handshakes cancelled at shutdown.

constexpr int STREAM_CHUNK = GDStream::MAX_WRITE; // Maximum byte count passed to one stream system call.
constexpr int PACKET_READ_BYTES = 65536; // Default receive-buffer width, not a retained-packet count limit.

// Convert seconds from now to a deadline: zero is unlimited, UINT64_MAX marks invalid input.
uint64_t due_after(double p_seconds) {
	if (!Math::is_finite(p_seconds) || p_seconds < 0.0) {
		return UINT64_MAX;
	}
	if (p_seconds == 0.0) {
		return 0;
	}
	const uint64_t now = GDClock::msec();
	const double ms = p_seconds * 1000.0;
	return ms >= double(UINT64_MAX - now) ? UINT64_MAX - 1 : now + (uint64_t)ms;
}

// Check whether a deadline has expired.
bool expired(uint64_t p_due) {
	return p_due != 0 && p_due != UINT64_MAX && GDClock::msec() >= p_due;
}

// Build the shared address dictionary.
Dictionary address_of(const String &p_host, int p_port, const String &p_network) {
	Dictionary out;
	out["network"] = p_network;
	out["host"] = p_host;
	out["port"] = p_port;
	return out;
}

// Convert an engine error to a standard result.
Ref<R> socket_error(const String &p_message, Error p_error) {
	return R::err(p_message, Err::of(p_error));
}

} // namespace

// ---------------- TCP connections ----------------

// Complete a TCP operation and deliver its result.
void GDTCPCall::done(const Ref<R> &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDTCPCall> keep(this);
	owner = nullptr;
	self_hold.unref(); // Commit completion before external callbacks to make reentrant cancel harmless.
	emit_signal("finished", p_result);
}

// Cancel a TCP operation from its waiter.
void GDTCPCall::cancel() {
	if (owner) {
		owner->cancel_call(this);
	}
}

// Register the TCP operation's completion signal.
void GDTCPCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDTCPCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Wrap an already-connected stream as a standard connection.
Ref<GDTCPConn> GDTCPConn::take(const Ref<GDStream> &p_peer) {
	Ref<GDTCPConn> out;
	out.instantiate();
	out->native = p_peer;
	out->watch(false);
	return out;
}

// Wrap a handshaken TLS stream as a standard connection.
Ref<GDTCPConn> GDTCPConn::take_tls(const Ref<GDStream> &p_peer, const Ref<GDTLS> &p_tls, const String &p_name) {
	Ref<GDTCPConn> out;
	out.instantiate();
	out->native = p_peer;
	out->tls = p_tls;
	out->tls_name = p_name;
	out->watch(false);
	return out;
}

// Attach to the event loop only while operations remain pending.
void GDTCPConn::watch(bool p_on) {
	const Callable call = p_on ? callable_mp(this, &GDTCPConn::step) : Callable();
	if (tls.is_valid()) tls->watch(p_on && !reads.is_empty(), p_on && !writes.is_empty(), call);
	else if (native.is_valid()) native->watch(p_on && !reads.is_empty(), p_on && !writes.is_empty(), call);
	if (p_on && !watching) {
		Async::post(Ref<RefCounted>(this), call);
	}
	watching = p_on;
}

// Register the earliest TCP operation deadline with the kernel wait.
void GDTCPConn::arm_deadline() {
	uint64_t nearest = 0;
	for (const Ref<GDTCPCall> &call : reads) {
		if (call->due > 0 && (nearest == 0 || call->due < nearest)) {
			nearest = call->due;
		}
	}
	for (const Ref<GDTCPCall> &call : writes) {
		if (call->due > 0 && (nearest == 0 || call->due < nearest)) {
			nearest = call->due;
		}
	}
	if (nearest == due) {
		return;
	}
	Async::drop_deadline(this, due);
	due = nearest;
	Async::track_deadline(this, due, callable_mp(this, &GDTCPConn::step));
}

// Start reading, suspending only this caller until bytes arrive.
Signal GDTCPConn::read(int64_t p_max) {
	if (p_max < 0) {
		return Async::ready(R::err("read size must not be negative", Err::INVALID_DATA));
	}
	if (!is_open()) {
		return Async::ready(R::err("connection closed", Err::INTERRUPTED));
	}
	if (p_max == 0) {
		return Async::ready(R::ok(PackedByteArray()));
	}
	if ((tls.is_valid() ? tls->read_eof() : native->read_eof())) {
		return Async::ready(expired(read_due) ? R::err("read deadline exceeded", Err::TIMED_OUT) : R::ok(PackedByteArray()));
	}
	Ref<GDTCPCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = this;
	call->max = (int)MIN(p_max, int64_t(STREAM_CHUNK));
	call->due = read_due;
	const Signal out(call.ptr(), "finished");
	reads.push_back(call);
	arm_deadline();
	watch(true);
	return out;
}

// Start writing, suspending only this caller until all bytes reach the kernel.
Signal GDTCPConn::write(const PackedByteArray &p_data) {
	if (!is_open()) {
		return Async::ready(R::err("connection closed", Err::INTERRUPTED));
	}
	if (p_data.is_empty()) {
		return Async::ready(R::ok(0));
	}
	Ref<GDTCPCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = this;
	call->data = p_data;
	call->writing = true;
	call->due = write_due;
	const Signal out(call.ptr(), "finished");
	writes.push_back(call);
	arm_deadline();
	watch(true);
	return out;
}

// Advance TCP I/O within a fair scheduling turn.
void GDTCPConn::step() {
	Ref<GDTCPConn> keep(this); // Retain the connection even if a completion callback releases its last external owner.
	if (native.is_null()) {
		fail_all(R::err("connection closed", Err::INTERRUPTED));
		return;
	}
	if (native->wait_error().is_valid()) { fail_all(native->wait_error()); return; }
	if (tls.is_valid()) {
		tls->poll();
	} else {
		native->poll();
	}
	if (!is_open()) {
		fail_all(R::err("connection closed", Err::INTERRUPTED));
		return;
	}

	// Advance only the FIFO head to preserve stream read order.
	while (!reads.is_empty()) {
		Ref<GDTCPCall> call = reads.front()->get();
		if (expired(call->due)) {
			reads.pop_front();
			call->done(R::err("read deadline exceeded", Err::TIMED_OUT));
			if (native.is_null()) {
				return;
			}
			continue;
		}
		const int available = tls.is_valid() ? tls->available() : native->available();
		const int wanted = MIN(call->max, MAX(1, available)); // Use recv to distinguish plaintext readiness waits from EOF.
		PackedByteArray &data = call->data; // Retain caller-owned storage across EAGAIN waits.
		if (data.size() != wanted && data.resize(wanted) != OK) {
			reads.pop_front();
			call->done(R::err("read buffer allocation failed", Err::LIMITED));
			if (native.is_null()) {
				return;
			}
			continue;
		}
		int got = 0;
		const Error err = tls.is_valid() ? tls->read(data.ptrw(), wanted, got) : native->read(data.ptrw(), wanted, got);
		if (err == ERR_BUSY) break;
		if (err == ERR_FILE_EOF) {
			data.resize(MAX(0, got));
			reads.pop_front();
			call->done(R::ok(data));
			if (!reads.is_empty()) {
				Async::post(Ref<RefCounted>(this), callable_mp(this, &GDTCPConn::step)); // Deliver the continuation waiting for EOF.
			}
			break;
		}
		if (err != OK) {
			reads.pop_front();
			call->done(socket_error("read failed", err));
			if (native.is_null()) {
				return;
			}
			continue;
		}
		data.resize(MAX(0, got));
		if (got <= 0) {
			break;
		}
		reads.pop_front();
		call->done(R::ok(data));
		break; // Yield execution to other connections.
	}
	// A completion callback may close the connection.
	if (native.is_null()) {
		return;
	}

	const uint64_t until = GDClock::usec() + GD_SCHED_SLICE_USEC;
	while (!writes.is_empty() && GDClock::usec() < until) {
		Ref<GDTCPCall> call = writes.front()->get();
		if (expired(call->due)) {
			writes.pop_front();
			if (tls.is_valid()) tls->abort_write();
			call->done(R::err("write deadline exceeded", Err::TIMED_OUT));
			if (native.is_null()) {
				return;
			}
			continue;
		}
		const int wanted = (int)MIN(int64_t(STREAM_CHUNK), call->data.size() - call->at);
		int sent = 0;
		const Error err = tls.is_valid() ? tls->write(call->data.ptr() + call->at, wanted, sent) : native->write(call->data.ptr() + call->at, wanted, sent);
		if (err == ERR_BUSY) break;
		if (err != OK) {
			writes.pop_front();
			call->done(socket_error("write failed", err));
			if (native.is_null()) {
				return;
			}
			continue;
		}
		call->at += MAX(0, sent);
		if (call->at >= call->data.size()) {
			const int64_t total = call->data.size();
			writes.pop_front();
			call->done(R::ok(total));
			if (native.is_null()) {
				return; // The completion callback closed the connection.
			}
			continue;
		}
		if (sent <= 0) {
			break;
		}
	}
	arm_deadline();
	watch(!reads.is_empty() || !writes.is_empty());
	if (!reads.is_empty() && tls.is_valid() && tls->available() > 0) {
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDTCPConn::step));
	}
}

// Remove the selected TCP operation from its queue and return cancellation.
void GDTCPConn::cancel_call(GDTCPCall *p_call) {
	List<Ref<GDTCPCall>> &queue = p_call->writing ? writes : reads;
	for (List<Ref<GDTCPCall>>::Element *e = queue.front(); e; e = e->next()) {
		if (e->get().ptr() == p_call) {
			Ref<GDTCPCall> keep = e->get();
			if (p_call->writing && e == writes.front() && tls.is_valid()) tls->abort_write();
			queue.erase(e);
			keep->done(R::err("operation canceled", Err::INTERRUPTED));
			break;
		}
	}
	arm_deadline();
	watch(!reads.is_empty() || !writes.is_empty());
}

// Deliver the same closure reason to all TCP operations.
void GDTCPConn::fail_all(const Ref<R> &p_result) {
	while (!reads.is_empty()) {
		Ref<GDTCPCall> call = reads.front()->get();
		reads.pop_front();
		call->done(p_result);
	}
	while (!writes.is_empty()) {
		Ref<GDTCPCall> call = writes.front()->get();
		writes.pop_front();
		call->done(p_result);
	}
	arm_deadline();
	watch(false);
}

// Set both I/O deadlines in seconds from now.
Ref<R> GDTCPConn::set_deadline(double p_seconds) {
	Ref<R> result = set_read_deadline(p_seconds);
	return result->get_ok() ? set_write_deadline(p_seconds) : result;
}

// Set the read deadline, including reads already waiting.
Ref<R> GDTCPConn::set_read_deadline(double p_seconds) {
	const uint64_t next = due_after(p_seconds);
	if (next == UINT64_MAX) {
		return R::err("read deadline must be a non-negative number of seconds", Err::INVALID_DATA);
	}
	read_due = next;
	for (Ref<GDTCPCall> &call : reads) {
		call->due = read_due;
	}
	arm_deadline();
	return R::ok();
}

// Set the write deadline, including writes already waiting.
Ref<R> GDTCPConn::set_write_deadline(double p_seconds) {
	const uint64_t next = due_after(p_seconds);
	if (next == UINT64_MAX) {
		return R::err("write deadline must be a non-negative number of seconds", Err::INVALID_DATA);
	}
	write_due = next;
	for (Ref<GDTCPCall> &call : writes) {
		call->due = write_due;
	}
	arm_deadline();
	return R::ok();
}

// Return the local TCP address.
Dictionary GDTCPConn::local_addr() const {
	return native.is_valid() ? native->addr() : Dictionary();
}

// Return the remote TCP address.
Dictionary GDTCPConn::remote_addr() const {
	return native.is_valid() ? native->addr(true) : Dictionary();
}

// Expose TLS handshake state using standard ConnectionState names.
Dictionary GDTCPConn::connection_state() const {
	Dictionary out;
	out["handshake_complete"] = tls.is_valid() && tls->status() == GDTLS::READY;
	out["server_name"] = tls_name;
	out["negotiated_protocol"] = tls.is_valid() ? tls->negotiated_protocol() : String();
	out["version"] = tls.is_valid() ? tls->negotiated_version() : 0;
	return out;
}

// Check whether the TCP stream permits I/O.
bool GDTCPConn::is_open() const {
	if (tls.is_valid()) {
		return native.is_valid() && tls->status() == GDTLS::READY;
	}
	return native.is_valid() && native->status() == GDStream::CONNECTED;
}

// Close the TCP stream and wake every pending operation.
void GDTCPConn::close() {
	if (tls.is_valid()) tls->close();
	if (native.is_valid()) {
		native->close();
		native.unref();
	}
	tls.unref();
	fail_all(R::err("connection closed", Err::INTERRUPTED));
}

// Leave no pending waits when destroying a TCP stream.
GDTCPConn::~GDTCPConn() {
	close();
}

// Register public TCP stream methods.
void GDTCPConn::_bind_methods() {
	ClassDB::bind_method(D_METHOD("read", "max"), &GDTCPConn::read, DEFVAL(65536));
	ClassDB::bind_method(D_METHOD("read_async", "max"), &GDTCPConn::read_async, DEFVAL(65536));
	ClassDB::bind_method(D_METHOD("write", "data"), &GDTCPConn::write);
	ClassDB::bind_method(D_METHOD("write_async", "data"), &GDTCPConn::write_async);
	ClassDB::bind_method(D_METHOD("set_deadline", "seconds"), &GDTCPConn::set_deadline);
	ClassDB::bind_method(D_METHOD("set_read_deadline", "seconds"), &GDTCPConn::set_read_deadline);
	ClassDB::bind_method(D_METHOD("set_write_deadline", "seconds"), &GDTCPConn::set_write_deadline);
	ClassDB::bind_method(D_METHOD("local_addr"), &GDTCPConn::local_addr);
	ClassDB::bind_method(D_METHOD("remote_addr"), &GDTCPConn::remote_addr);
	ClassDB::bind_method(D_METHOD("connection_state"), &GDTCPConn::connection_state);
	ClassDB::bind_method(D_METHOD("is_open"), &GDTCPConn::is_open);
	ClassDB::bind_method(D_METHOD("close"), &GDTCPConn::close);
	ADD_AWAIT("read", "R:PackedByteArray");
	ADD_AWAIT("read_async", "R:PackedByteArray");
	ADD_AWAIT("write", "R:int");
	ADD_AWAIT("write_async", "R:int");
	ADD_AUTO_WAIT("read");
	ADD_AUTO_WAIT("write");
	ADD_RESULT("set_deadline", "Variant");
	ADD_RESULT("set_read_deadline", "Variant");
	ADD_RESULT("set_write_deadline", "Variant");
}

// ---------------- TCP dialing ----------------

// Deliver both address-family lanes to one completion handler.
void GDTCPDialCall::watch(bool p_on) {
	const Callable call = p_on ? callable_mp(this, &GDTCPDialCall::step) : Callable();
	for (Lane &lane : lanes) {
		if (lane.peer.is_valid()) lane.peer->watch(false, p_on, call);
	}
	if (p_on && !watching) Async::post(Ref<RefCounted>(this), call);
	watching = p_on;
}

// Register only the earliest overall, candidate, or fallback deadline.
void GDTCPDialCall::arm_deadline() {
	uint64_t nearest = due;
	for (uint64_t value : { fallback, lanes[0].due, lanes[1].due }) {
		if (value && (!nearest || value < nearest)) nearest = value;
	}
	if (nearest == wake_due) return;
	Async::drop_deadline(this, wake_due);
	wake_due = nearest;
	Async::track_deadline(this, wake_due, callable_mp(this, &GDTCPDialCall::step));
}

// Use the first candidate's family as primary and the other family as fallback.
void GDTCPDialCall::resolved(const Ref<R> &p_result) {
	lookup = Signal();
	if (self_hold.is_null()) return;
	if (Pool::is_stopping()) {
		done(R::err("worker pool stopped", Err::INTERRUPTED));
		return;
	}
	if (p_result.is_null() || !p_result->get_ok()) {
		done(p_result.is_valid() ? p_result : R::err("name resolution failed", Err::NOT_FOUND));
		return;
	}
	const PackedStringArray addresses = p_result->get_v();
	if (addresses.is_empty()) {
		done(R::err("no address to connect", Err::NOT_FOUND));
		return;
	}
	const bool first_v6 = addresses[0].contains(":");
	for (const String &address : addresses) {
		lanes[address.contains(":") == first_v6 ? 0 : 1].addresses.push_back(address);
	}
	prepared = true;
	if (!lanes[1].addresses.is_empty()) {
		fallback = GDClock::msec() + 300; // Default delay before starting the fallback family.
	}
	step();
}

// Try candidates in family order and close every losing connection when the call completes.
bool GDTCPDialCall::advance(Lane &p_lane) {
	for (;;) {
		if (p_lane.peer.is_valid()) {
			if (p_lane.peer->wait_error().is_valid() && p_lane.error.is_null()) p_lane.error = p_lane.peer->wait_error();
			p_lane.peer->poll();
			if (!expired(p_lane.due) && p_lane.retries < 2 && p_lane.peer->retryable()) {
				p_lane.retries++; // Allow two retries with a newly selected ephemeral port.
				p_lane.peer->dial(p_lane.addresses[p_lane.next - 1], port);
				continue; // Recheck without extending this candidate's deadline.
			}
			if (!expired(p_lane.due) && p_lane.peer->status() == GDStream::CONNECTED) {
				Ref<GDStream> winner = p_lane.peer;
				p_lane.peer.unref();
				done(R::ok(GDTCPConn::take(winner)));
				return true;
			}
			if (!expired(p_lane.due) && p_lane.peer->status() == GDStream::CONNECTING) return false;
			if (p_lane.error.is_null()) {
				p_lane.error = expired(p_lane.due) ? R::err("address connection timed out", Err::TIMED_OUT) :
						R::err(vformat("cannot connect to %s:%d", host, port), Err::NOT_FOUND);
			}
			p_lane.peer->close();
			p_lane.peer.unref();
			p_lane.due = 0;
		}
		if (p_lane.next >= p_lane.addresses.size()) return false;
		const uint64_t now = GDClock::msec();
		if (due && now >= due) return false;
		if (due) {
			const uint64_t left = due - now;
			const uint64_t share = left / uint64_t(p_lane.addresses.size() - p_lane.next);
			p_lane.due = now + MIN(left, MAX(share, uint64_t(2000))); // Allocate at least two seconds when available, without extending the overall deadline.
		}
		const String address = p_lane.addresses[p_lane.next++];
		p_lane.retries = 0;
		p_lane.peer.instantiate();
		const Error error = p_lane.peer->dial(address, port);
		if (error != OK && p_lane.error.is_null()) {
			p_lane.error = socket_error(vformat("cannot connect to %s:%d", address, port), error);
		}
	}
}

// Start fallback immediately after primary failure and return only the winning connection.
void GDTCPDialCall::step() {
	if (self_hold.is_null()) return;
	if (expired(due)) {
		done(R::err(vformat("connection to %s:%d timed out", host, port), Err::TIMED_OUT));
		return;
	}
	if (prepared) {
		if (advance(lanes[0])) return;
		const bool primary_done = lanes[0].peer.is_null() && lanes[0].next >= lanes[0].addresses.size();
		if (fallback && (primary_done || expired(fallback))) fallback = 0;
		if (!fallback && advance(lanes[1])) return;
		if (primary_done && lanes[1].peer.is_null() && lanes[1].next >= lanes[1].addresses.size()) {
			done(lanes[0].error.is_valid() ? lanes[0].error : R::err("no address to connect", Err::NOT_FOUND));
			return;
		}
	}
	arm_deadline();
	watch(true);
}

// Close losing connections and invalidate late DNS completion.
void GDTCPDialCall::done(const Ref<R> &p_result) {
	if (self_hold.is_null()) return;
	Ref<GDTCPDialCall> keep(this);
	self_hold.unref(); // Commit completion before resolver cancellation can invoke callbacks.
	if (Object *pending = lookup.get_object()) pending->call("cancel");
	lookup = Signal();
	watch(false);
	Async::drop_deadline(this, wake_due);
	for (Lane &lane : lanes) {
		if (lane.peer.is_valid()) lane.peer->close();
		lane.peer.unref();
	}
	tcp_dials.erase(this);
	emit_signal("finished", p_result);
}

// Cancel all pending connections at shutdown, including those with completed DNS.
void GDTCPDialCall::shutdown_all() {
	LocalVector<Ref<GDTCPDialCall>> calls;
	for (GDTCPDialCall *call : tcp_dials) {
		calls.push_back(Ref<GDTCPDialCall>(call));
	}
	for (const Ref<GDTCPDialCall> &call : calls) {
		call->done(R::err("worker pool stopped", Err::INTERRUPTED));
	}
}

// Start a TCP connection and return its completion signal.
Signal GDTCPDialCall::start(const String &p_host, int64_t p_port, const Dictionary &p_opts) {
	const double timeout = p_opts.get("timeout", 0.0);
	const uint64_t due = due_after(timeout);
	if (p_host.is_empty() || p_port < 1 || p_port > 65535 || due == UINT64_MAX) {
		return Async::ready(R::err("dial requires a host, a valid port, and a non-negative timeout", Err::INVALID_DATA));
	}
	Ref<GDTCPDialCall> call;
	call.instantiate();
	tcp_dials.insert(call.ptr());
	call->self_hold = call;
	const Signal out(call.ptr(), "finished");
	call->host = p_host;
	call->port = (int)p_port;
	call->due = due;
	call->arm_deadline();
	call->watch(call->due > 0); // Monitor an explicit deadline even while resolving the name.
	call->lookup = GDLookupCall::start(p_host, false, int(p_port));
	call->lookup.connect(callable_mp(call.ptr(), &GDTCPDialCall::resolved), Object::CONNECT_ONE_SHOT);
	return out;
}

// Cancel a pending TCP connection.
void GDTCPDialCall::cancel() {
	done(R::err("connection canceled", Err::INTERRUPTED));
}

// Register the TCP dial completion signal.
void GDTCPDialCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDTCPDialCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// ---------------- TLS dialing ----------------

// Wait only for readiness required by the current lookup, handshake, or verification phase.
void GDTLSDialCall::watch(bool p_on) {
	const Callable call = p_on ? callable_mp(this, &GDTLSDialCall::step) : Callable();
	if (tls.is_valid()) tls->watch(p_on, false, call);
	else if (peer.is_valid()) peer->watch(false, false, call);
	if (p_on && !watching) Async::post(Ref<RefCounted>(this), call);
	watching = p_on;
}

// Retain worker-loaded trust settings and dial all TCP candidates within the remaining deadline.
void GDTLSDialCall::prepared(const Ref<R> &p_result) {
	if (self_hold.is_null()) return;
	if (Pool::is_stopping()) { done(R::err("worker pool stopped", Err::INTERRUPTED)); return; }
	if (p_result.is_null() || !p_result->get_ok()) {
		done(p_result.is_valid() ? p_result : R::err("TLS preparation failed", Err::INVALID_DATA));
		return;
	}
	if (expired(due)) { step(); return; }
	const Array prepared = p_result->get_v();
	ca = prepared[0]; identity = prepared[1];
	Dictionary opts;
	opts["timeout"] = due ? double(due - GDClock::msec()) / 1000.0 : 0.0;
	pending = GDTCPDialCall::start(host, port, opts);
	pending.connect(callable_mp(this, &GDTLSDialCall::connected), Object::CONNECT_ONE_SHOT);
}

// Transfer the connected descriptor exclusively to the verified TLS setup.
void GDTLSDialCall::connected(const Ref<R> &p_result) {
	pending = Signal();
	if (self_hold.is_null()) return;
	if (p_result.is_null() || !p_result->get_ok()) {
		done(p_result.is_valid() ? p_result : R::err("TCP connection failed", Err::NOT_FOUND));
		return;
	}
	const Ref<GDTCPConn> conn = p_result->get_v();
	peer = conn->native;
	conn->native.unref(); // This connection has no application read or write waiters yet.
	tls.instantiate();
	if (tls->start(peer, server_name, ca, insecure, identity, protocols) != OK) {
		done(R::err(tls->error(), Err::UNAUTHENTICATED));
		return;
	}
	step();
}

// Advance the handshake and trust verification within the TCP connection's overall deadline.
void GDTLSDialCall::step() {
	if (self_hold.is_null()) return;
	if (peer.is_valid() && peer->wait_error().is_valid()) { done(peer->wait_error()); return; }
	if (expired(due)) {
		done(R::err(vformat("TLS connection to %s:%d timed out", host, port), Err::TIMED_OUT));
		return;
	}
	if (tls.is_null()) return;
	tls->poll();
	if (tls->status() == GDTLS::READY) {
		done(R::ok(GDTCPConn::take_tls(peer, tls, server_name)));
		return;
	}
	if (tls->status() == GDTLS::BROKEN || tls->status() == GDTLS::CLOSED) {
		done(R::err(tls->error(), Err::UNAUTHENTICATED));
		return;
	}
	watch(true);
}

// Commit completion before external notification and cancel any TCP setup still in progress.
void GDTLSDialCall::done(const Ref<R> &p_result) {
	if (self_hold.is_null()) return;
	Ref<GDTLSDialCall> keep(this);
	self_hold.unref();
	watch(false);
	Async::drop_deadline(this, due);
	if (Object *call = pending.get_object()) call->call("cancel");
	pending = Signal();
	const bool ok = p_result.is_valid() && p_result->get_ok();
	if (!ok && tls.is_valid()) tls->close();
	if (!ok && peer.is_valid()) peer->close();
	tls.unref();
	peer.unref();
	ca.unref();
	identity.unref();
	tls_dials.erase(this);
	emit_signal("finished", p_result);
}

// Cancel all pending TLS connections at shutdown, including those with completed DNS.
void GDTLSDialCall::shutdown_all() {
	LocalVector<Ref<GDTLSDialCall>> calls;
	for (GDTLSDialCall *call : tls_dials) {
		calls.push_back(Ref<GDTLSDialCall>(call));
	}
	for (const Ref<GDTLSDialCall> &call : calls) {
		call->done(R::err("worker pool stopped", Err::INTERRUPTED));
	}
}

// Validate TLS settings and start resolution and CA loading on a worker.
Signal GDTLSDialCall::start(const String &p_host, int64_t p_port, const Dictionary &p_opts) {
	static const char *const known[] = { "timeout", "server_name", "insecure_skip_verify", "ca_file", "cert_file", "key_file", "next_protos", nullptr };
	for (const KeyValue<Variant, Variant> &kv : p_opts) {
		bool found = false;
		for (int i = 0; known[i]; i++) {
			found = found || String(kv.key) == known[i];
		}
		if (!found) {
			return Async::ready(R::err(vformat("unknown TLS option: %s", kv.key), Err::INVALID_DATA));
		}
	}
	const Variant timeout_value = p_opts.get("timeout", 0.0);
	const Variant name_value = p_opts.get("server_name", p_host);
	const Variant insecure_value = p_opts.get("insecure_skip_verify", false);
	const Variant ca_value = p_opts.get("ca_file", "");
	const Variant cert_value = p_opts.get("cert_file", ""), key_value = p_opts.get("key_file", ""), protocols_value = p_opts.get("next_protos",PackedStringArray());
	if (p_host.is_empty() || p_port < 1 || p_port > 65535 ||
			(timeout_value.get_type() != Variant::INT && timeout_value.get_type() != Variant::FLOAT) ||
			name_value.get_type() != Variant::STRING || insecure_value.get_type() != Variant::BOOL || ca_value.get_type() != Variant::STRING ||
			cert_value.get_type() != Variant::STRING || key_value.get_type() != Variant::STRING || (protocols_value.get_type() != Variant::ARRAY && protocols_value.get_type() != Variant::PACKED_STRING_ARRAY)) {
		return Async::ready(R::err("dial_tls requires a host, valid port, and typed options", Err::INVALID_DATA));
	}
	const String server_name = name_value;
	const String cert_file = cert_value, key_file = key_value;
	if (cert_file.is_empty() != key_file.is_empty()) return Async::ready(R::err("TLS cert_file and key_file must be provided together",Err::INVALID_DATA));
	PackedStringArray protocols;
	const Array names = protocols_value;
	int64_t encoded_size = 0;
	for (const Variant &value : names) {
		if (value.get_type() != Variant::STRING) return Async::ready(R::err("TLS next_protos requires strings",Err::INVALID_DATA));
		const String name = value; const int64_t size = name.utf8().length();
		if (size < 1 || size > 255 || encoded_size > 65535-size-1) return Async::ready(R::err("TLS next_protos exceeds encoded protocol-name widths",Err::INVALID_DATA));
		encoded_size += size+1; protocols.push_back(name);
	}
	const uint64_t due = due_after((double)timeout_value);
	if (server_name.is_empty() || due == UINT64_MAX) {
		return Async::ready(R::err("TLS server_name must not be empty and timeout must be non-negative", Err::INVALID_DATA));
	}
	Ref<GDTLSDialCall> call;
	call.instantiate();
	tls_dials.insert(call.ptr());
	call->self_hold = call;
	const Signal out(call.ptr(), "finished");
	call->host = p_host;
	call->port = (int)p_port;
	call->server_name = server_name;
	call->insecure = insecure_value;
	call->ca_file = ca_value;
	call->protocols = protocols;
	call->due = due;
	Async::track_deadline(call.ptr(), call->due, callable_mp(call.ptr(), &GDTLSDialCall::step));
	call->watch(call->due > 0);
	const String host = call->host;
	const int port = call->port;
	const String ca_file = call->ca_file;
	Signal prepared = GDFileCall::start([host, port, ca_file, cert_file, key_file]() -> Ref<R> {
		const String target = vformat("%s:%d", host, port);
		if (!Perm::check(Perm::NET, target)) {
			return R::err(vformat("net access to %s is not allowed", target), Err::PERMISSION_DENIED);
		}
		const Ref<R> roots = GDTrust::load(ca_file); if (!roots->get_ok()) return roots;
		Ref<GDTLSIdentity> identity;
		if (!cert_file.is_empty()) {const Ref<R> credentials = GDTLSIdentity::load(cert_file,key_file); if (!credentials->get_ok()) return credentials; identity = credentials->get_v();}
		Array values; values.push_back(roots->get_v()); values.push_back(identity); return R::ok(values);
	});
	prepared.connect(callable_mp(call.ptr(), &GDTLSDialCall::prepared), Object::CONNECT_ONE_SHOT);
	return out;
}

// Cancel a pending TLS connection.
void GDTLSDialCall::cancel() {
	done(R::err("TLS connection canceled", Err::INTERRUPTED));
}

// Register the TLS dial completion signal.
void GDTLSDialCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDTLSDialCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// ---------------- TCP listeners ----------------

// Complete an accept operation.
void GDTCPAcceptCall::done(const Ref<R> &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDTCPAcceptCall> keep(this);
	owner = nullptr;
	self_hold.unref(); // Commit completion before external callbacks to make reentrant cancel harmless.
	emit_signal("finished", p_result);
}

// Cancel a pending accept.
void GDTCPAcceptCall::cancel() {
	if (owner) {
		owner->cancel_call(this);
	}
}

// Register the accept completion signal.
void GDTCPAcceptCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDTCPAcceptCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Open a TCP listener.
Ref<R> GDTCPListener::listen(const String &p_host, int64_t p_port) {
	if (p_host.is_empty() || p_port < 0 || p_port > 65535) {
		return R::err("listen_tcp requires a host and a valid port", Err::INVALID_DATA);
	}
	if (p_port == 0 ? !Perm::check_net_any_port(p_host) : !Perm::check(Perm::NET, vformat("%s:%d", p_host, p_port))) {
		return R::err("TCP listen address is not allowed", Err::PERMISSION_DENIED);
	}
	Ref<GDTCPListener> out;
	out.instantiate();
	out->server.instantiate();
	out->host = p_host;
	const Error err = out->server->listen(p_host, (int)p_port);
	if (err != OK) {
		out->server.unref();
		return socket_error(vformat("cannot listen on %s:%d", p_host, p_port), err);
	}
	out->watch(false);
	if (p_port == 0 && !Perm::check(Perm::NET, vformat("%s:%d", p_host, int(out->server->addr().get("port", 0))))) {
		out->server->close();
		out->server.unref();
		return R::err("TCP listen address is not allowed", Err::PERMISSION_DENIED);
	}
	return R::ok(out);
}

// Attach to the event loop only while accepts are pending.
void GDTCPListener::watch(bool p_on) {
	const Callable call = p_on ? callable_mp(this, &GDTCPListener::step) : Callable();
	if (server.is_valid()) {
		server->watch(p_on, false, call);
	}
	if (p_on && !watching) {
		Async::post(Ref<RefCounted>(this), call);
	}
	watching = p_on;
}

// Register the earliest accept deadline with the kernel wait.
void GDTCPListener::arm_deadline() {
	uint64_t nearest = 0;
	for (const Ref<GDTCPAcceptCall> &call : accepts) {
		if (call->due > 0 && (nearest == 0 || call->due < nearest)) {
			nearest = call->due;
		}
	}
	if (nearest == due) {
		return;
	}
	Async::drop_deadline(this, due);
	due = nearest;
	Async::track_deadline(this, due, callable_mp(this, &GDTCPListener::step));
}

// Start accepting the next TCP connection.
Signal GDTCPListener::accept() {
	if (!is_open()) {
		return Async::ready(R::err("listener closed", Err::INTERRUPTED));
	}
	Ref<GDTCPAcceptCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = this;
	call->due = accept_due;
	const Signal out(call.ptr(), "finished");
	accepts.push_back(call);
	arm_deadline();
	watch(true);
	return out;
}

// Deliver arrived connections to waiting accept operations in order.
void GDTCPListener::step() {
	Ref<GDTCPListener> keep(this); // Retain the listener throughout accept completion callbacks.
	if (!is_open()) {
		close();
		return;
	}
	while (!accepts.is_empty()) {
		Ref<GDTCPAcceptCall> call = accepts.front()->get();
		if (expired(call->due)) {
			accepts.pop_front();
			call->done(R::err("accept deadline exceeded", Err::TIMED_OUT));
			if (server.is_null()) {
				return;
			}
			continue;
		}
		Ref<GDStream> peer;
		const Error error = server->accept(peer);
		if (error == ERR_BUSY) break;
		if (error != OK) {
			accepts.pop_front();
			call->done(socket_error("accept failed", error));
			if (server.is_null()) return;
			continue;
		}
		accepts.pop_front();
		call->done(R::ok(GDTCPConn::take(peer)));
		if (server.is_null()) {
			return; // The completion callback closed the listener.
		}
	}
	arm_deadline();
	watch(!accepts.is_empty());
}

// Remove the selected pending accept from its queue.
void GDTCPListener::cancel_call(GDTCPAcceptCall *p_call) {
	for (List<Ref<GDTCPAcceptCall>>::Element *e = accepts.front(); e; e = e->next()) {
		if (e->get().ptr() == p_call) {
			Ref<GDTCPAcceptCall> keep = e->get();
			accepts.erase(e);
			keep->done(R::err("accept canceled", Err::INTERRUPTED));
			break;
		}
	}
	arm_deadline();
	watch(!accepts.is_empty());
}

// Set the accept deadline in seconds from now.
Ref<R> GDTCPListener::set_deadline(double p_seconds) {
	const uint64_t next = due_after(p_seconds);
	if (next == UINT64_MAX) {
		return R::err("accept deadline must be a non-negative number of seconds", Err::INVALID_DATA);
	}
	accept_due = next;
	for (Ref<GDTCPAcceptCall> &call : accepts) {
		call->due = accept_due;
	}
	arm_deadline();
	return R::ok();
}

// Return the listen address.
Dictionary GDTCPListener::addr() const {
	return is_open() ? server->addr() : Dictionary();
}

// Check whether the listener is open.
bool GDTCPListener::is_open() const {
	return server.is_valid() && server->status() == GDStream::LISTENING;
}

// Close the listener and wake all pending accepts.
void GDTCPListener::close() {
	const Ref<R> failed = server.is_valid() ? server->wait_error() : Ref<R>();
	if (server.is_valid()) {
		server->close();
		server.unref();
	}
	while (!accepts.is_empty()) {
		Ref<GDTCPAcceptCall> call = accepts.front()->get();
		accepts.pop_front();
		call->done(failed.is_valid() ? failed : R::err("listener closed", Err::INTERRUPTED));
	}
	arm_deadline();
	watch(false);
}

// Leave no pending accepts when destroying a listener.
GDTCPListener::~GDTCPListener() {
	close();
}

// Register public TCP listener methods.
void GDTCPListener::_bind_methods() {
	ClassDB::bind_method(D_METHOD("accept"), &GDTCPListener::accept);
	ClassDB::bind_method(D_METHOD("accept_async"), &GDTCPListener::accept_async);
	ClassDB::bind_method(D_METHOD("set_deadline", "seconds"), &GDTCPListener::set_deadline);
	ClassDB::bind_method(D_METHOD("addr"), &GDTCPListener::addr);
	ClassDB::bind_method(D_METHOD("is_open"), &GDTCPListener::is_open);
	ClassDB::bind_method(D_METHOD("close"), &GDTCPListener::close);
	ADD_AWAIT("accept", "R:GDTCPConn");
	ADD_AWAIT("accept_async", "R:GDTCPConn");
	ADD_AUTO_WAIT("accept");
	ADD_RESULT("set_deadline", "Variant");
}

// ---------------- UDP endpoints ----------------

// Complete a UDP operation.
void GDUDPCall::done(const Ref<R> &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDUDPCall> keep(this);
	owner = nullptr;
	self_hold.unref(); // Commit completion before external callbacks to make reentrant cancel harmless.
	emit_signal("finished", p_result);
}

// Cancel a UDP operation.
void GDUDPCall::cancel() {
	if (owner) {
		owner->cancel_call(this);
	}
}

// Register the UDP operation completion signal.
void GDUDPCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDUDPCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Open a UDP endpoint.
Ref<R> GDUDPPacketConn::listen(const String &p_host, int64_t p_port, int64_t p_buffer) {
	if (p_host.is_empty() || p_port < 0 || p_port > 65535 || p_buffer < 0 || p_buffer > INT_MAX) {
		return R::err("listen_udp requires a host, a valid port, and a non-negative buffer", Err::INVALID_DATA);
	}
	if (p_port == 0 ? !Perm::check_net_any_port(p_host) : !Perm::check(Perm::NET, vformat("%s:%d", p_host, p_port))) {
		return R::err("UDP listen address is not allowed", Err::PERMISSION_DENIED);
	}
	Ref<GDUDPPacketConn> out;
	out.instantiate();
	out->peer.instantiate();
	out->host = p_host;
	const Error err = out->peer->open(p_host, (int)p_port, (int)p_buffer);
	if (err != OK) {
		out->peer.unref();
		return socket_error(vformat("cannot listen on %s:%d", p_host, p_port), err);
	}
	out->watch(false);
	if (p_port == 0 && !Perm::check(Perm::NET, vformat("%s:%d", p_host, out->peer->port()))) {
		out->peer->close();
		out->peer.unref();
		return R::err("UDP listen address is not allowed", Err::PERMISSION_DENIED);
	}
	return R::ok(out);
}

// Attach to the event loop only while UDP operations are pending.
void GDUDPPacketConn::watch(bool p_on) {
	const Callable call = p_on ? callable_mp(this, &GDUDPPacketConn::step) : Callable();
	if (peer.is_valid()) {
		peer->watch(!reads.is_empty(), !writes.is_empty(), call);
	}
	if (p_on && !watching) {
		Async::post(Ref<RefCounted>(this), call);
	}
	watching = p_on;
}

// Register the earliest UDP operation deadline with the kernel wait.
void GDUDPPacketConn::arm_deadline() {
	uint64_t nearest = 0;
	for (const Ref<GDUDPCall> &call : reads) {
		if (call->due > 0 && (nearest == 0 || call->due < nearest)) {
			nearest = call->due;
		}
	}
	for (const Ref<GDUDPCall> &call : writes) {
		if (call->due > 0 && (nearest == 0 || call->due < nearest)) {
			nearest = call->due;
		}
	}
	if (nearest == due) {
		return;
	}
	Async::drop_deadline(this, due);
	due = nearest;
	Async::track_deadline(this, due, callable_mp(this, &GDUDPPacketConn::step));
}

// Start receiving the next UDP packet.
Signal GDUDPPacketConn::read_from(int64_t p_max) {
	if (p_max < 0 || p_max > INT_MAX) {
		return Async::ready(R::err("packet read size is outside the native buffer range", Err::INVALID_DATA));
	}
	if (!is_open()) {
		return Async::ready(R::err("packet connection closed", Err::INTERRUPTED));
	}
	Ref<GDUDPCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = this;
	call->max = (int)p_max;
	call->due = read_due;
	const Signal out(call.ptr(), "finished");
	reads.push_back(call);
	arm_deadline();
	watch(true);
	return out;
}

// Start sending a UDP packet to an IP address.
Signal GDUDPPacketConn::write_to(const PackedByteArray &p_data, const String &p_host, int64_t p_port) {
	if (!GDDatagram::is_ip(p_host) || p_port < 1 || p_port > 65535) {
		return Async::ready(R::err("write_to requires a resolved IP address and a valid port", Err::INVALID_DATA));
	}
	if (p_data.size() > INT_MAX) {
		return Async::ready(R::err("packet exceeds the native send size", Err::LIMITED));
	}
	if (!Perm::check(Perm::NET, vformat("%s:%d", p_host, p_port))) {
		return Async::ready(R::err("packet destination is not allowed", Err::PERMISSION_DENIED));
	}
	if (!is_open()) {
		return Async::ready(R::err("packet connection closed", Err::INTERRUPTED));
	}
	Ref<GDUDPCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = this;
	call->data = p_data;
	call->host = p_host;
	call->port = (int)p_port;
	call->due = write_due;
	call->writing = true;
	const Signal out(call.ptr(), "finished");
	writes.push_back(call);
	arm_deadline();
	watch(true);
	return out;
}

// Advance UDP receiving and sending by one packet each.
void GDUDPPacketConn::step() {
	Ref<GDUDPPacketConn> keep(this); // Retain the endpoint throughout packet completion callbacks.
	if (peer.is_valid() && peer->wait_error().is_valid()) { fail_all(peer->wait_error()); return; }
	if (!is_open()) {
		fail_all(R::err("packet connection closed", Err::INTERRUPTED));
		return;
	}
	while (!reads.is_empty()) {
		Ref<GDUDPCall> call = reads.front()->get();
		if (expired(call->due)) {
			reads.pop_front();
			call->done(R::err("packet read deadline exceeded", Err::TIMED_OUT));
			if (peer.is_null()) {
				return;
			}
			continue;
		}
		Dictionary out;
		const Error err = peer->read(call->max, call->data, out);
		if (err == ERR_BUSY) {
			break;
		}
		if (err != OK) {
			reads.pop_front();
			call->done(socket_error("packet read failed", err));
			if (peer.is_null()) {
				return;
			}
			continue;
		}
		reads.pop_front();
		call->done(R::ok(out));
		break;
	}
	// A completion callback may close the endpoint.
	if (peer.is_null()) {
		return;
	}

	while (!writes.is_empty()) {
		Ref<GDUDPCall> call = writes.front()->get();
		if (expired(call->due)) {
			writes.pop_front();
			call->done(R::err("packet write deadline exceeded", Err::TIMED_OUT));
			if (peer.is_null()) {
				return;
			}
			continue;
		}
		const Error err = peer->write(call->data, call->host, call->port);
		if (err == ERR_BUSY) {
			break;
		}
		writes.pop_front();
		call->done(err == OK ? R::ok(call->data.size()) : socket_error("packet write failed", err));
		if (peer.is_null()) {
			return; // The completion callback closed the endpoint.
		}
		break;
	}
	arm_deadline();
	watch(!reads.is_empty() || !writes.is_empty());
}

// Remove the selected UDP operation from its queue.
void GDUDPPacketConn::cancel_call(GDUDPCall *p_call) {
	List<Ref<GDUDPCall>> &queue = p_call->writing ? writes : reads;
	for (List<Ref<GDUDPCall>>::Element *e = queue.front(); e; e = e->next()) {
		if (e->get().ptr() == p_call) {
			Ref<GDUDPCall> keep = e->get();
			queue.erase(e);
			keep->done(R::err("packet operation canceled", Err::INTERRUPTED));
			break;
		}
	}
	arm_deadline();
	watch(!reads.is_empty() || !writes.is_empty());
}

// Deliver the same closure reason to all UDP operations.
void GDUDPPacketConn::fail_all(const Ref<R> &p_result) {
	while (!reads.is_empty()) {
		Ref<GDUDPCall> call = reads.front()->get();
		reads.pop_front();
		call->done(p_result);
	}
	while (!writes.is_empty()) {
		Ref<GDUDPCall> call = writes.front()->get();
		writes.pop_front();
		call->done(p_result);
	}
	arm_deadline();
	watch(false);
}

// Set both UDP receive and send deadlines.
Ref<R> GDUDPPacketConn::set_deadline(double p_seconds) {
	Ref<R> result = set_read_deadline(p_seconds);
	return result->get_ok() ? set_write_deadline(p_seconds) : result;
}

// Apply the UDP receive deadline to existing waits too.
Ref<R> GDUDPPacketConn::set_read_deadline(double p_seconds) {
	const uint64_t next = due_after(p_seconds);
	if (next == UINT64_MAX) {
		return R::err("packet read deadline must be a non-negative number of seconds", Err::INVALID_DATA);
	}
	read_due = next;
	for (Ref<GDUDPCall> &call : reads) {
		call->due = read_due;
	}
	arm_deadline();
	return R::ok();
}

// Apply the UDP send deadline to existing waits too.
Ref<R> GDUDPPacketConn::set_write_deadline(double p_seconds) {
	const uint64_t next = due_after(p_seconds);
	if (next == UINT64_MAX) {
		return R::err("packet write deadline must be a non-negative number of seconds", Err::INVALID_DATA);
	}
	write_due = next;
	for (Ref<GDUDPCall> &call : writes) {
		call->due = write_due;
	}
	arm_deadline();
	return R::ok();
}

// Return the UDP endpoint's listen address.
Dictionary GDUDPPacketConn::addr() const {
	return is_open() ? address_of(host, peer->port(), "udp") : Dictionary();
}

// Check whether the UDP endpoint is open.
bool GDUDPPacketConn::is_open() const {
	return peer.is_valid() && peer->is_open();
}

// Close the UDP endpoint and wake all pending operations.
void GDUDPPacketConn::close() {
	Ref<GDDatagram> old = peer;
	peer.unref();
	if (old.is_valid()) {
		old->close();
	}
	fail_all(R::err("packet connection closed", Err::INTERRUPTED));
}

// Leave no pending waits when destroying a UDP endpoint.
GDUDPPacketConn::~GDUDPPacketConn() {
	close();
}

// Register public UDP endpoint methods.
void GDUDPPacketConn::_bind_methods() {
	ClassDB::bind_method(D_METHOD("read_from", "max"), &GDUDPPacketConn::read_from, DEFVAL(PACKET_READ_BYTES));
	ClassDB::bind_method(D_METHOD("read_from_async", "max"), &GDUDPPacketConn::read_from_async, DEFVAL(PACKET_READ_BYTES));
	ClassDB::bind_method(D_METHOD("write_to", "data", "host", "port"), &GDUDPPacketConn::write_to);
	ClassDB::bind_method(D_METHOD("write_to_async", "data", "host", "port"), &GDUDPPacketConn::write_to_async);
	ClassDB::bind_method(D_METHOD("set_deadline", "seconds"), &GDUDPPacketConn::set_deadline);
	ClassDB::bind_method(D_METHOD("set_read_deadline", "seconds"), &GDUDPPacketConn::set_read_deadline);
	ClassDB::bind_method(D_METHOD("set_write_deadline", "seconds"), &GDUDPPacketConn::set_write_deadline);
	ClassDB::bind_method(D_METHOD("addr"), &GDUDPPacketConn::addr);
	ClassDB::bind_method(D_METHOD("is_open"), &GDUDPPacketConn::is_open);
	ClassDB::bind_method(D_METHOD("close"), &GDUDPPacketConn::close);
	ADD_AWAIT("read_from", "R:Dictionary");
	ADD_AWAIT("read_from_async", "R:Dictionary");
	ADD_AWAIT("write_to", "R:int");
	ADD_AWAIT("write_to_async", "R:int");
	ADD_AUTO_WAIT("read_from");
	ADD_AUTO_WAIT("write_to");
	ADD_RESULT("set_deadline", "Variant");
	ADD_RESULT("set_read_deadline", "Variant");
	ADD_RESULT("set_write_deadline", "Variant");
}
