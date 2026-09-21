/**************************************************************************/
/*  wire.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement the shared plain and encrypted transport declared in wire.h.

#include "cli/net/wire.h"
#include "cli/sys/clock.h"
#include "cli/net/lookup.h"
#include "cli/net/socket.h"
#include "cli/sys/task.h"
#include "core/object/callable_mp.h"

#include "cli/sys/perm.h"

#include "cli/net/address.h"
#include "cli/net/datagram.h"

// Convert user TLS verification settings into internal guard modes.
bool Wire::guard_of(const Variant &p_value, Guard &r_out) {
	// Accept booleans, with true selecting certificate verification.
	if (p_value.get_type() == Variant::BOOL) {
		r_out = (bool)p_value ? VERIFY : NONE;
		return true;
	}
	if (p_value.get_type() == Variant::NIL) {
		r_out = NONE;
		return true;
	}
	const String s = String(p_value).to_lower();
	// Reject an empty mode instead of silently treating an omitted setting as plaintext.
	if (s == "disable") {
		r_out = NONE;
		return true;
	}
	if (s == "require") {
		r_out = ENCRYPT;
		return true;
	}
	if (s == "verify-full") {
		r_out = VERIFY;
		return true;
	}
	return false;
}

// Default to plaintext only on loopback and full verification for external connections.
Variant Wire::default_guard(const String &p_host) {
	const String host = p_host.to_lower().trim_suffix(".").trim_prefix("[").trim_suffix("]");
	bool local = host == "localhost";
	local = local || host == "::1" || (GDDatagram::is_ip(host) && host.begins_with("127."));
	return local ? Variant(false) : Variant("verify-full");
}

// Prepare connection resolution settings and CA-file loading as one I/O job.
Ref<R> Wire::prepare(const String &p_host, const String &p_ca_path) {
	const Ref<R> loaded = GDTrust::load(p_ca_path);
	if (!loaded->get_ok()) return loaded;
	const Ref<GDTrust> ca = loaded->get_v();
	Dictionary out;
	out["address"] = p_host; // Delegate DNS aggregation and candidate racing to the shared dialer.
	out["ca"] = ca;
	return R::ok(out);
}

// Apply TCP and TLS readiness directions to the shared poller.
void Wire::watch() {
	if (tcp.is_null()) return;
	bool reading = read_on;
	bool writing = write_on;
	if (tcp->status() == GDStream::CONNECTING) {
		reading = false;
		writing = true;
	} else if (tls.is_valid()) {
		tls->watch(reading, writing, wait_callback);
		return;
	}
	tcp->watch(reading, writing, wait_callback);
}

// Retain the resume target and apply it to the current descriptor.
void Wire::set_wait_callback(const Callable &p_callback) {
	wait_callback = p_callback;
	watch();
}

// Retain the first transport cause while keeping textual diagnostics available.
void Wire::failed(const Ref<Err> &p_error) const {
	if (fail_error.is_valid() || p_error.is_null()) return;
	fail_error = p_error;
	fail_why = p_error->text();
}

// Begin a nonblocking connection to a numeric address.
Error Wire::open(const String &p_addr, int p_port, uint64_t p_due) {
	close();
	fail_why = String();
	fail_error.unref();
	Dictionary opts;
	const uint64_t now = GDClock::msec();
	opts["timeout"] = p_due ? double(p_due > now ? p_due - now : 1) / 1000.0 : 0.0;
	dial.instantiate();
	dial->owner = this;
	dial->pending = GDTCPDialCall::start(p_addr, p_port, opts);
	const Error error = dial->pending.connect(callable_mp(dial.ptr(), &GDWireDial::connected), Object::CONNECT_ONE_SHOT);
	if (error != OK) {
		failed(Err::make("cannot await TCP connection", Err::of(error)));
		close();
		return error;
	}
	return OK;
}

// Transfer a connection to an uncancelled Wire and resume its database operation.
void GDWireDial::connected(const Ref<R> &p_result) {
	Ref<GDWireDial> keep(this);
	pending = Signal();
	if (!owner) return;
	Wire *wire = owner;
	owner = nullptr;
	wire->dial.unref();
	if (p_result.is_valid() && p_result->get_ok()) {
		const Ref<GDTCPConn> conn = p_result->get_v();
		wire->tcp = conn->native;
		conn->native.unref();
		wire->read_on = true;
		wire->watch();
	} else {
		wire->failed(p_result.is_valid() ? p_result->get_e() : Err::make("invalid TCP connection result", Err::INVALID_DATA));
	}
	if (wire->wait_callback.is_valid()) Async::post(keep, wire->wait_callback);
}

// Wrap connected TCP in TLS without leaving a plaintext fallback on failure.
Error Wire::wrap(const String &p_host, Guard p_guard, const Ref<GDTrust> &p_ca, const PackedStringArray &p_protocols) {
	if (p_guard == NONE) return OK;
	if (tcp.is_null()) {
		failed(Err::make("no connection to wrap", Err::of(ERR_UNCONFIGURED)));
		return ERR_UNCONFIGURED;
	}
	tls.instantiate();
	const Error error = tls->start(tcp, p_host, p_ca, p_guard == ENCRYPT, Ref<GDTLSIdentity>(), p_protocols);
	if (error != OK) {
		failed(tls->get_error().is_valid() ? tls->get_error() : Err::make(tls->error(), Err::of(error)));
		close();
		return error;
	}
	poll();
	return OK;
}

// Advance connection, TLS, or receive-EOF handling and update readiness interests.
void Wire::poll() {
	if (tcp.is_null()) return;
	if (tls.is_valid()) {
		tls->poll();
	} else {
		tcp->poll();
		if (read_on) tcp->probe();
	}
	watch();
}

// Keep unverified TLS out of READY and distinguish EOF from connection failure.
Wire::State Wire::state() const {
	if (dial.is_valid()) return LINKING;
	if (tcp.is_null()) return fail_why.is_empty() ? CLOSED : FAILED;
	if (tcp->wait_error().is_valid()) {
		failed(tcp->wait_error()->get_e());
		return FAILED;
	}
	if (tls.is_valid()) {
		switch (tls->status()) {
			case GDTLS::HANDSHAKE: return LINKING;
			case GDTLS::READY: return tls->read_eof() && !tls->available() ? CLOSED : READY;
			case GDTLS::BROKEN:
				if (fail_error.is_null()) failed(tls->get_error().is_valid() ? tls->get_error() : Err::make(tls->error(), Err::NONE));
				return FAILED;
			default: return CLOSED;
		}
	}
	switch (tcp->status()) {
		case GDStream::CONNECTING: return LINKING;
		case GDStream::CONNECTED: return tcp->read_eof() ? CLOSED : READY;
		case GDStream::BROKEN:
			if (fail_error.is_null()) failed(Err::make("connection failed", Err::NONE));
			return FAILED;
		default: return CLOSED;
	}
}

// Report whether protocol I/O can proceed.
bool Wire::is_ready() const { return state() == READY; }

// Return decrypted bytes or bytes available in the kernel.
int Wire::available() const {
	return tls.is_valid() ? tls->available() : (tcp.is_valid() ? tcp->available() : 0);
}

// Read directly into caller storage without blocking the event loop for missing bytes.
Error Wire::read(uint8_t *p_dst, int p_len, int &r_got) {
	r_got = 0;
	if (tcp.is_null()) return ERR_UNCONFIGURED;
	const Error error = tls.is_valid() ? tls->read(p_dst, p_len, r_got) : tcp->read(p_dst, p_len, r_got);
	if (error != OK && error != ERR_BUSY && error != ERR_FILE_EOF) {
		failed(tls.is_valid() && tls->get_error().is_valid() ? tls->get_error() : Err::make("connection read failed", Err::of(error)));
	}
	watch();
	return error;
}

// Send available bytes and represent EAGAIN as zero progress for the caller's wait.
Error Wire::send(const uint8_t *p_src, int p_len, int &r_sent) {
	r_sent = 0;
	if (tcp.is_null()) return ERR_UNCONFIGURED;
	const Error error = tls.is_valid() ? tls->write(p_src, p_len, r_sent) : tcp->write(p_src, p_len, r_sent);
	if (error != OK && error != ERR_BUSY) {
		failed(tls.is_valid() && tls->get_error().is_valid() ? tls->get_error() : Err::make("connection write failed", Err::of(error)));
	}
	watch();
	return error == ERR_BUSY ? OK : error;
}

// Subscribe to writable readiness only while output remains pending.
void Wire::write_wait(bool p_on) {
	write_on = p_on;
	watch();
}

// Pause kernel read notifications until the caller resumes reading.
void Wire::read_wait(bool p_on) {
	read_on = p_on;
	watch();
}

// Close TLS and TCP without leaving a descriptor or verification callback active.
void Wire::close() {
	if (dial.is_valid()) {
		Ref<GDWireDial> held = dial;
		dial.unref();
		held->owner = nullptr;
		if (Object *call = held->pending.get_object()) call->call("cancel");
	}
	read_on = false;
	write_on = false;
	if (tls.is_valid()) tls->close();
	tls.unref();
	if (tcp.is_valid()) tcp->close();
	tcp.unref();
}

// Cancel connection waits and release native resources without retaining an owner.
Wire::~Wire() { close(); }
