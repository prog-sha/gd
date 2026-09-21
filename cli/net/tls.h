// Provide TLS and certificate verification over native TCP independently of engine wrappers.
#pragma once

#include "cli/net/stream.h"
#include "cli/sys/std.h"
#include <memory>
#include <vector>
namespace GDCrypto { class Cert; class CertStore; class TLSIdentity; }
class GDTrust;

// Share a validated server identity without reading files on the event loop.
class GDTLSIdentity : public RefCounted {
	GDCLASS(GDTLSIdentity, RefCounted);
	friend class GDTLS;
	std::shared_ptr<const GDCrypto::TLSIdentity> config; // Immutable independent credentials retained by each accepted connection.
	Ref<GDTrust> clients; // Trust roots for client-certificate authentication.
	int client_auth = 0; // Explicit certificate-presence and verification policy.
protected:
	static void _bind_methods() {}
public:
	static Ref<R> load(const String &p_cert, const String &p_key, const Dictionary &p_opts = Dictionary()); // Read and validate credentials and optional client-auth policy on a worker.
	~GDTLSIdentity(); // Release the shared configuration after its last connection.
};

// Own trust anchors and verify peers using OS trust or explicit CA certificates.
class GDTrust : public RefCounted {
	GDCLASS(GDTrust, RefCounted);
	std::unique_ptr<GDCrypto::CertStore> roots; // Independently parsed anchors loaded on a worker and shared immutably across connections.
	bool system = true; // Delegate trust decisions to the OS on macOS and Windows.

protected:
	static void _bind_methods() {} // Keep trust stores outside the script API.

public:
	GDTrust(); // Initialize an empty trust store.
	~GDTrust(); // Release native certificate storage.
	static Ref<R> load(const String &p_path); // Prepare explicit CA certificates or system trust on a worker.
	static void shutdown(); // Release shared trust after workers finish.
	Ref<R> verify(const std::vector<std::shared_ptr<const GDCrypto::Cert>> &p_chain, const String &p_host, bool p_server = true) const; // Verify owned peer certificates for the endpoint purpose and any required server name.
	std::vector<std::vector<uint8_t>> names() const; // Copy configured authority names for a client-certificate request.
};

// Own one cryptographic worker at a time and publish only completed I/O to the runtime.
class GDTLS : public RefCounted {
public:
	enum State { HANDSHAKE, READY, CLOSED, BROKEN }; // Keep unverified connections unavailable to application I/O.
private:
	struct Context;
	struct Work;
	Context *ctx = nullptr; // Cryptographic state accessed only by the active worker.
	std::shared_ptr<Work> job; // Immutable submission and worker-owned result until completion.
	Ref<GDTLS> self_hold; // Retain through signal delivery after the worker closure is released.
	Ref<GDStream> tcp; // Descriptor whose lifetime is pinned across worker execution.
	String why; // Terminal failure published by the owning runtime.
	Ref<Err> failure; // Original worker, trust, or transport failure after publication.
	String protocol; // Application protocol published after verified handshake completion.
	int version = 0; // Negotiated protocol version published by the worker.
	bool multiplex_ok = false; // Completed negotiation satisfies multiplexed HTTP record protection requirements.
	State state = CLOSED; // Runtime-visible connection state.
	Callable callback; // Application continuation on the owning runtime.
	PackedByteArray input, output; // Completed plaintext and stable pending write.
	List<PackedByteArray> ciphertext; // Ordered encrypted output awaiting native socket capacity.
	int64_t cipher_at = 0; // Sent bytes within the first encrypted output slice.
	int input_at = 0, sent = 0; // Consumed plaintext and acknowledged pending write count.
	bool busy = false, eof = false, write_failed = false; // Worker ownership and terminal directions.
	bool can_read = true, can_write = true, can_handshake = true; // Retry tokens replenished by kernel readiness or completed progress.
	bool reading = false, writing = false, read_requested = false; // Caller demand retained while a worker is running.
	void interests(); // Disable kernel wakeups while a worker owns the connection.
	void flush(); // Send already-encrypted bytes on the runtime without cryptographic work.
	void socket_ready(); // Replenish retry tokens only on actual kernel notification.
	void dispatch(bool p_close = false); // Submit required operations with an exclusive context and descriptor lease.
	void launch(); // Queue the selected job using an already-acquired descriptor lease.
	void completed(const Ref<R> &p_result); // Publish worker results after releasing the descriptor lease.
	Error fail(const String &p_reason, const Ref<Err> &p_error = Ref<Err>()); // Preserve failure and close logically before releasing pending ownership.
public:
	GDTLS(); // Allocate inert context storage without cryptographic work.
	~GDTLS(); // Release state only after all jobs release their reference.
	Error start(const Ref<GDStream> &p_tcp, const String &p_host, const Ref<GDTrust> &p_trust, bool p_insecure, const Ref<GDTLSIdentity> &p_identity = Ref<GDTLSIdentity>(), const PackedStringArray &p_protocols = PackedStringArray()); // Schedule client setup and optional credentials without cryptographic work on the runtime.
	Error accept(const Ref<GDStream> &p_tcp, const Ref<GDTLSIdentity> &p_identity); // Retain an immutable server identity for worker setup.
	void poll(); // Advance handshakes or requested record input when readiness permits.
	void watch(bool p_read, bool p_write, const Callable &p_callback); // Own kernel callbacks so crypto completion never fabricates socket readiness.
	State status() const { return state; } // Return the last published state.
	const String &error() const { return why; } // Return the terminal reason.
	Ref<Err> get_error() const { return failure; } // Return only the published cause, never worker-owned state.
	const String &negotiated_protocol() const { return protocol; } // Return completed ALPN selection without reading active worker state.
	int negotiated_version() const { return version; } // Return the published handshake version.
	bool multiplex_compatible() const { return multiplex_ok; } // Inspect verified record policy without accessing worker-owned state.
	bool read_eof() const { return eof && available() == 0; } // Deliver buffered plaintext before EOF.
	int available() const { return input.size() - input_at; } // Return only already-decrypted bytes.
	Error read(uint8_t *p_data, int p_size, int &r_got); // Consume ready plaintext or suspend the caller for worker completion.
	Error write(const uint8_t *p_data, int p_size, int &r_sent); // Preserve the common partial-write interface.
	Error write_parts(GDWrites p_parts, int64_t &r_sent); // Retain one gathered record across worker and kernel waits.
	void abort_write(); // Discard unsent ciphertext and keep canceled record writes permanently unusable.
	void close(); // Cancel application access immediately without waiting for cryptographic work.
};
