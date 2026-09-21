// Advance authenticated TLS connections on a cryptographic worker without owning sockets or event-loop callbacks.
#pragma once
#include "identity_core.h"
#include <functional>
#include <string>

namespace GDCrypto {
// Keep role, authentication policy, and application negotiation explicit for each connection.
struct TLSOptions {
	enum ClientAuth { NONE, REQUEST, REQUIRE, VERIFY_IF_GIVEN, REQUIRE_AND_VERIFY }; // Independent certificate-presence and trust requirements for client peers.
	bool server = false, insecure = false; // Endpoint role and explicit server-trust bypass for test connections.
	uint16_t minimum = 0x0303, maximum = 0x0304; // Supported protocol range, excluding obsolete generations.
	std::string hostname; // Original peer name used by trust verification and server-name negotiation.
	std::vector<std::string> protocols; // Ordered application-protocol preferences.
	std::vector<uint16_t> groups; // Named-group preferences, defaulted when empty.
	std::vector<uint16_t> suites; // Explicit supported cipher policy, defaulted when empty.
	std::shared_ptr<const TLSIdentity> identity; // Optional client identity or mandatory server identity.
	ClientAuth client_auth = NONE; // Server policy for requesting and authenticating client credentials.
	std::vector<std::vector<uint8_t>> authorities; // Encoded client-certificate authority names, with an empty list accepting any issuer selection.
	std::function<bool(const std::vector<Cert::Ptr> &,bool)> verify; // Worker-side trust decision; the boolean identifies a server peer.
};

// Serialize directional key epochs and handshake state independently of transport readiness.
class TLSConnection {
	struct Context;
	std::unique_ptr<Context> context; // Exclusive per-connection cryptographic state.
public:
	TLSConnection(); // Allocate inert state without starting a handshake or reading entropy.
	~TLSConnection(); // Erase private state after the final worker releases the connection.
	TLSConnection(const TLSConnection &) = delete; // Never duplicate record sequence numbers or traffic secrets.
	TLSConnection &operator=(const TLSConnection &) = delete; // Preserve exclusive ownership of key epochs.
	bool start(TLSOptions options); // Begin one configured handshake and queue any initial records.
	bool record_size(Bytes header, size_t &size); // Validate a complete header and publish its payload size before transport allocation.
	bool receive(Bytes record, std::vector<uint8_t> &application); // Authenticate one complete record and publish application bytes only after peer authentication.
	bool write(Bytes application, size_t &written); // Protect one plaintext fragment and report its exact consumed length.
	std::vector<uint8_t> drain(); // Transfer queued encrypted records to the transport's ordered write queue.
	bool ready() const; // Require both the finished exchange and the configured trust decision.
	bool closed() const; // Expose received closure independently of previously published plaintext.
	uint8_t alert() const; // Report a sticky local or peer protocol failure.
	uint16_t version() const; // Return the negotiated version, or zero before negotiation.
	uint16_t cipher() const; // Return the negotiated record-suite identifier without exposing secret state.
	const std::string &protocol() const; // Inspect the negotiated application protocol after hello processing.
	const std::vector<Cert::Ptr> &peer() const; // Borrow retained peer certificate encodings without asserting readiness.
	void close(); // Queue an authenticated close notification only for a completed, usable connection.
};
}
