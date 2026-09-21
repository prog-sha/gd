// Preserve HTTP transport semantics across plain and encrypted native connections.
#pragma once
#include "cli/net/tls.h"

class GDServeConn : public RefCounted {
	Ref<GDStream> tcp; // Native descriptor and readiness ownership.
	Ref<GDTLS> tls; // Per-connection encryption, absent for plaintext.
	Callable callback; // HTTP ready-queue target.
	Callable native_ready; // Plaintext readiness invalidates the last nonblocking read result.
	Callable native_apply; // Reuse the deferred plaintext interest update for this connection.
	bool read_blocked = false; // Retry an exhausted plaintext read only after a socket notification.
	bool reading = false, writing = false; // Logical application interests before TLS direction translation.
	bool posted = false; // Coalesce delivery of already-decrypted input.
	bool pending = false; // Coalesce plaintext interest changes on the runtime queue.
	bool negotiating = false; // Preserve handshake transition until HTTP consumes the completed phase.
	void interests(); // Translate application waits into record-layer readiness directions.
	void apply(); // Apply the latest plaintext interests while preserving closed transport lifetime.
	void buffered(); // Deliver buffered-input readiness on the runtime queue.
	void notified(); // Permit another native read before waking the HTTP task.
public:
	void start(const Ref<GDStream> &p_tcp, const Ref<GDTLSIdentity> &p_identity); // Attach accepted transport without blocking.
	bool handshake(); // Advance an incomplete handshake before parsing HTTP.
	bool handshaking() const { return negotiating; } // Detect the first transition into HTTP processing.
	String protocol() const { return tls.is_valid() ? tls->negotiated_protocol() : String(); } // Dispatch only the application protocol published after authentication.
	bool multiplex_compatible() const { return tls.is_null() || tls->multiplex_compatible(); } // Enforce encrypted HTTP protocol requirements without main-thread cryptographic work.
	Error read(uint8_t *p_data, int p_size, int &r_got); // Read application bytes rather than ciphertext.
	Error write(const uint8_t *p_data, int p_size, int &r_sent); // Retain TLS record retries across kernel waits.
	Error write_parts(GDWrites p_parts, int64_t &r_sent); // Gather framing and body in either transport mode.
	void read_wait(bool p_on) { if (reading != p_on) { reading = p_on; interests(); } } // Set the logical read interest only when it changes.
	void write_wait(bool p_on) { if (writing != p_on) { writing = p_on; interests(); } } // Set the logical write interest only when it changes.
	void set_callback(const Callable &p_call) { callback = p_call; interests(); } // Replace the HTTP readiness target.
	bool is_open() const { return tcp.is_valid() && tcp->is_open(); } // Report native transport lifetime.
	void close(); // Close encryption and the socket without a blocking shutdown wait.
};
