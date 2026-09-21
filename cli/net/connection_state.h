// Share private connection state between negotiation, authentication, and directional record processing.
#pragma once
#include "connection_core.h"
#include "handshake_key.h"
#include "handshake_wire.h"
#include "record_core.h"
#include "schedule_core.h"

namespace GDCrypto {
// Borrow an exact contiguous cryptographic buffer without changing its ownership.
template <class Buffer> Bytes tls_bytes(const Buffer &buffer) { return {reinterpret_cast<const uint8_t *>(buffer.data()),buffer.size()}; }

// Borrow a protocol label whose bytes do not include a terminating zero.
inline Bytes tls_text(std::string_view text) { return {reinterpret_cast<const uint8_t *>(text.data()),text.size()}; }

// Keep handshake sequencing and key epochs under one exclusive worker lifetime.
struct TLSConnection::Context {
	using Buffer = std::vector<uint8_t>; // Public encoded messages and encrypted transport output.
	using Secret = CryptoStorage<uint8_t,0>; // Secret material erased across released allocation capacity.
	enum Phase { NEW, CLIENT_HELLO, SERVER_HELLO, ENCRYPTED_EXTENSIONS, SERVER_CERTIFICATE, SERVER_CERT_VERIFY, SERVER_FINISHED, CLIENT_CERTIFICATE, CLIENT_CERT_VERIFY, CLIENT_FINISHED, LEGACY, READY, CLOSED, FAILED }; // Expected peer message or terminal connection state.
	enum Alert : uint8_t { UNEXPECTED=10, BAD_RECORD=20, RECORD_OVERFLOW=22, HANDSHAKE_FAILURE=40, BAD_CERTIFICATE=42, ILLEGAL_PARAMETER=47, UNKNOWN_CA=48, DECODE_ERROR=50, DECRYPT_ERROR=51, PROTOCOL_VERSION=70, INTERNAL_ERROR=80, INAPPROPRIATE_FALLBACK=86, MISSING_EXTENSION=109, UNSUPPORTED_EXTENSION=110, CERTIFICATE_REQUIRED=116, NO_APPLICATION_PROTOCOL=120 }; // Protocol alert descriptions rather than application error numbers.
	TLSOptions options; // Immutable per-connection role and authentication configuration after start.
	Phase phase = NEW; // Exact next handshake expectation.
	uint8_t failure = 0; // First fatal local or peer alert.
	uint16_t negotiated = 0, suite = 0, selected_group = 0; // Negotiated protocol and cryptographic identifiers.
	TLSCipher cipher; // Directional record field widths for the selected suite.
	TLSHash narrow, wide{true}; // Parallel transcript candidates retained until suite negotiation.
	TLSKDF kdf; // Derivation bound to the negotiated transcript digest.
	TLSRecord incoming, outgoing; // Independently sequenced authenticated record directions.
	bool read_keys = false, write_keys = false, retried = false, requested = false; // Key installation, retry, and client-certificate request states.
	bool peer_verified = false, local_certificate = false; // Explicit authentication completion and selected client identity.
	bool read_closed = false; // Peer closure stops input without implicitly canceling the local write direction.
	bool server_name = false; // A valid nonempty client server-name extension was accepted.
	enum LegacyStep { L_CERT, L_KEY, L_REQUEST, L_DONE, L_CLIENT_CERT, L_CLIENT_KEY, L_CLIENT_VERIFY, L_CCS, L_FINISHED }; // Full-handshake expectations for the legacy generation.
	LegacyStep legacy_step = L_CERT; // Expected legacy peer message or directional key activation.
	bool extended_master = false, retain_legacy = false; // Negotiated transcript binding and temporary certificate-proof retention.
	Buffer legacy_transcript, peer_point; // Exact legacy proof input and authenticated ephemeral peer share.
	unsigned idle_records = 0; // Consecutive non-advancing record/message count.
	uint64_t read_epoch = 0; // Detect a key change before accepting trailing bytes decrypted under the previous epoch.
	Buffer output, hand, initial_hello, cookie; // Queued ciphertext, partial handshake, retry reference, and peer cookie.
	std::array<uint8_t,32> client_random{}, server_random{}; // Public handshake randoms, with no wall-clock timestamp encoding.
	Buffer session; // Compatibility session identifier echoed across the hello exchange.
	std::map<uint16_t,std::unique_ptr<TLSShare>> shares; // Fresh client shares or the selected server share.
	std::vector<uint16_t> peer_schemes; // Signature schemes accepted for the next local certificate signature.
	Buffer certificate_types; // Legacy requested certificate families, independent of signature-scheme preferences.
	std::vector<Buffer> authorities; // Requested certificate-authority distinguished names.
	std::vector<Cert::Ptr> certificates; // Authenticated or pending peer certificate ownership.
	std::string application_protocol; // Exact negotiated application protocol.
	Secret master, read_secret, write_secret, pending_read, pending_write; // Current and next directional secrets across handshake/application boundaries.
	bool fail(uint8_t alert, bool reply = true); // Make failure sticky and queue at most one alert under the current outgoing epoch.
	TLSHash &transcript(); // Select the negotiated digest without retaining full handshake bodies.
	bool track(Bytes message); // Accumulate exact encoded handshake bytes in the applicable digest candidates.
	bool queue(uint8_t type, Bytes payload, bool plaintext = false); // Fragment one logical payload into protocol-sized directional records.
	bool emit(uint8_t type, Bytes body); // Hash and queue one complete encoded handshake message.
	bool install(Secret secret, bool read); // Derive and install a fresh directional key epoch.
	bool handshake_keys(Bytes shared); // Derive handshake traffic keys and the master secret from the hello transcript.
	bool application_keys(); // Derive pending application secrets at the server-finished transcript boundary.
	bool start(TLSOptions selected); // Validate configuration and generate the initial client flight when required.
	bool client_hello(); // Encode an initial or requested replacement client hello.
	bool accept_hello(Bytes message); // Select server negotiation parameters or issue one group retry.
	bool accept_server_hello(Bytes message); // Validate the server selection before installing handshake keys.
	bool extensions(Bytes body); // Accept only solicited encrypted server parameters.
	bool request_certificate(Bytes body); // Parse peer signature and authority preferences for client authentication.
	bool certificate(Bytes body); // Retain supported peer certificates and perform the configured trust decision.
	bool certificate_verify(Bytes body); // Authenticate the role-separated transcript using the peer's public identity.
	bool finished(Bytes body); // Authenticate peer finished data and advance directional application epochs.
	bool send_server_flight(); // Emit encrypted parameters, optional client-auth request, and server authentication.
	bool send_certificate(bool client); // Emit an appropriate local identity or an explicit empty client certificate list.
	bool send_certificate_verify(bool client); // Sign the role-separated transcript with a mutually supported scheme.
	bool send_finished(); // Authenticate the current transcript with the outgoing handshake secret.
	bool message(Bytes encoded); // Dispatch exactly one complete handshake message under the current phase.
	bool handshake(Bytes fragment); // Reassemble arbitrary message fragments without preallocating peer-announced excess lengths.
	bool receive(Bytes record, Buffer &application); // Validate record framing and process authenticated content in order.
	bool record_size(Bytes header, size_t &size); // Reject invalid headers with a protocol alert before reading or allocating their announced body.
	bool post_handshake(uint8_t type, Bytes body); // Validate session notices and directional key updates after establishment.
	bool legacy_message(Bytes encoded); // Process version-specific legacy authentication and key-change sequencing.
	bool legacy_ccs(); // Activate negotiated legacy read keys only at the expected change-cipher boundary.
	bool legacy_hello(Bytes body); // Negotiate a full legacy handshake without session resumption or renegotiation.
	bool legacy_client_flight(); // Send optional client proof and activate the outgoing legacy epoch.
	bool legacy_keys(Bytes shared); // Derive a master secret and both pending record-key blocks at client-key exchange.
	bool legacy_install(bool read); // Activate exactly one pending directional legacy key block.
	bool legacy_finish(bool send, Bytes body = {}); // Authenticate the directional legacy finished checkpoint.
};
}
