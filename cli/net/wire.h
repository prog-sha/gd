/**************************************************************************/
/*  wire.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Expose plain and TLS-wrapped connections through a shared I/O interface.
//
// Support TLS from the first byte for connections such as GDRedisClient.
// For negotiated TLS, request encryption over plaintext and wait for the server's S response.
// Then start the handshake on that connection; only the placement of wrap() differs.
// Subsequent reads and writes use the same interface.
//
// Implementation is in wire.cpp.

#include "cli/net/tls.h"
#include "cli/sys/std.h"

class GDWireDial;

class Wire {
public:
	// Connection state, with LINKING also covering the TLS handshake.
	enum State {
		CLOSED, // Not connected.
		LINKING, // Connecting or handshaking.
		READY, // Ready for I/O.
		FAILED, // Failed; why() provides the reason.
	};

	// Three transport-security modes with explicit verification behavior.
	// NONE: plaintext.
	// ENCRYPT: encryption without peer verification; an on-path attacker can impersonate the peer.
	// VERIFY: encryption with chain and hostname verification.
	enum Guard {
		NONE,
		ENCRYPT,
		VERIFY,
	};

	// Convert script options to Guard; true selects VERIFY and false selects NONE.
	// Accept disable, require, or verify-full; return false for an invalid mode.
	static bool guard_of(const Variant &p_value, Guard &r_out);
	// Default to plaintext locally and certificate plus hostname verification externally.
	static Variant default_guard(const String &p_host);
	// Prepare name resolution and optional CA-file loading for an I/O worker.
	static Ref<R> prepare(const String &p_host, const String &p_ca_path);

private:
	Ref<GDStream> tcp;
	Ref<GDTLS> tls; // Encrypted transport used for I/O after wrapping.
	Ref<GDWireDial> dial; // Shared TCP dialer handling DNS and candidate races.
	Callable wait_callback; // Main-thread callback for socket readiness.
	bool read_on = false; // Whether the caller can advance receiving.
	bool write_on = false; // Whether output remains pending.
	mutable String fail_why; // Failure detail, also updated when querying status.
	mutable Ref<Err> fail_error; // First typed failure retained after transport cleanup.
	void failed(const Ref<Err> &p_error) const; // Preserve the original failure and its textual diagnostic.

	void watch(); // Apply TCP/TLS readiness directions to the poller.
	friend class GDWireDial;

public:
	void set_wait_callback(const Callable &p_callback); // Set the delivery target for the next socket.
	Error open(const String &p_addr, int p_port, uint64_t p_due = 0); // Pass the overall deadline through to per-candidate time allocation.
	// Wrap the plain connection, using p_host for certificate hostname checks.
	// The handshake is initially incomplete; call poll() until READY.
	Error wrap(const String &p_host, Guard p_guard, const Ref<GDTrust> &p_ca = Ref<GDTrust>(), const PackedStringArray &p_protocols = PackedStringArray());
	String protocol() const { return tls.is_valid() ? tls->negotiated_protocol() : String(); } // Read the application protocol published by authentication.
	bool multiplex_compatible() const { return tls.is_null() || tls->multiplex_compatible(); } // Reject prohibited ciphers before enabling multiplexed HTTP.

	void poll();
	State state() const;
	bool is_ready() const;
	bool is_wrapped() const { return tls.is_valid(); }
	const String &why() const { return fail_why; }
	Ref<Err> error() const { return fail_error; } // Retain error identity for callers that propagate results.

	int available() const;
	Error read(uint8_t *p_dst, int p_len, int &r_got); // Return the partial read count to the caller.
	// Return bytes sent in r_sent without waiting for a full peer buffer.
	// The caller retains the remainder for a later iteration.
	Error send(const uint8_t *p_src, int p_len, int &r_sent);
	// Wait for writable readiness until the remaining data can be sent.
	void write_wait(bool p_on);
	// Pause kernel readability notifications until the caller reads again.
	void read_wait(bool p_on);
	void close();
	bool is_valid() const { return tcp.is_valid() || dial.is_valid(); }
	~Wire(); // Cancel pending connections when the owner is destroyed.
};

// Relay connection completion safely even if the owning Wire is destroyed.
class GDWireDial : public RefCounted {
	GDCLASS(GDWireDial, RefCounted);
	Wire *owner = nullptr; // Detached first by Wire::close.
	Signal pending; // Cancellation target for this shared-dial call.
	friend class Wire;
	void connected(const Ref<R> &p_result); // Transfer the connection to Wire and wake its caller.

protected:
	static void _bind_methods() {} // Keep the relay outside the script API.
};
