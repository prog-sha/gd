// Translate encrypted I/O readiness while preserving the plaintext gather-write path.
#include "cli/net/serve_conn.h"
#include "cli/sys/task.h"
#include "core/object/callable_mp.h"

// Attach one accepted socket to optional server-side encryption.
void GDServeConn::start(const Ref<GDStream> &p_tcp, const Ref<GDTLSIdentity> &p_identity) {
	tcp = p_tcp;
	read_blocked = false;
	native_ready = callable_mp(this, &GDServeConn::notified);
	native_apply = callable_mp(this, &GDServeConn::apply);
	if (p_identity.is_valid()) {
		negotiating = true;
		tls.instantiate();
		tls->accept(tcp, p_identity);
	}
}

// Advance only unfinished handshakes; record reads remain driven by HTTP demand.
bool GDServeConn::handshake() {
	if (tls.is_null()) return true;
	if (tls->status() == GDTLS::HANDSHAKE) tls->poll();
	interests();
	if (tls->status() != GDTLS::READY) return false;
	negotiating = false;
	return true;
}

// Avoid waiting on a writable socket when record output actually needs readable input.
void GDServeConn::interests() {
	if (!is_open()) return;
	bool rd = reading, wr = writing;
	if (tls.is_valid()) {
		tls->watch(rd, wr, callback);
	} else if (!pending) {
		pending = true;
		Async::post(Ref<RefCounted>(this), native_apply);
	}
	if (tls.is_valid() && reading && tls->available() > 0 && callback.is_valid() && !posted) {
		posted = true;
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDServeConn::buffered));
	}
}

// Collapse transient direction changes before applying the final requested state.
void GDServeConn::apply() {
	pending = false;
	if (is_open()) tcp->watch(reading, writing, native_ready);
}

// A socket event permits another read, including FIN and readiness registration failure.
void GDServeConn::notified() {
	read_blocked = false;
	if (callback.is_valid()) callback.call();
}

// Recheck ownership and demand because a queued notification can outlive its request.
void GDServeConn::buffered() {
	posted = false;
	if (is_open() && reading && tls.is_valid() && tls->available() > 0 && callback.is_valid()) callback.call();
}

// Translate TLS EOF to the same half-close result used by native HTTP reads.
Error GDServeConn::read(uint8_t *p_data, int p_size, int &r_got) {
	if (tls.is_null() && read_blocked && p_size > 0 && is_open()) { r_got = 0; return ERR_BUSY; }
	const Error err = tls.is_valid() ? tls->read(p_data, p_size, r_got) : tcp->read(p_data, p_size, r_got);
	if (tls.is_valid()) interests();
	else read_blocked = err == ERR_BUSY;
	return err;
}

// Send one application slice and refresh cross-direction TLS waits.
Error GDServeConn::write(const uint8_t *p_data, int p_size, int &r_sent) {
	const Error err = tls.is_valid() ? tls->write(p_data, p_size, r_sent) : tcp->write(p_data, p_size, r_sent);
	if (tls.is_valid()) interests();
	return err;
}

// Keep the native gather operation for plaintext without exposing ciphertext bypasses.
Error GDServeConn::write_parts(GDWrites p_parts, int64_t &r_sent) {
	if (tls.is_null()) return tcp->write_parts(p_parts, r_sent);
	const Error err = tls->write_parts(p_parts, r_sent);
	interests();
	return err;
}

// Release the descriptor and any retained callback without waiting for the peer.
void GDServeConn::close() {
	callback = Callable();
	if (tls.is_valid()) tls->close();
	else if (tcp.is_valid()) tcp->close();
}
