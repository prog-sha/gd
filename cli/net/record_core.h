// Protect complete TLS records with directional keys and non-repeating sequence numbers.
#pragma once
#include "cli/data/cbc_core.h"
#include "cli/data/chacha_core.h"
#include <variant>

namespace GDCrypto {
// Describe negotiated protection without coupling record storage to handshake messages.
struct TLSCipher {
	enum Mode { GCM, CHACHA, CBC }; // Record authentication and encryption construction.
	Mode mode = GCM; // Selected construction after a successful lookup.
	size_t key = 0, iv = 0, mac = 0; // Key-block field lengths in bytes.
	bool modern = false, sha384 = false; // Protocol generation and handshake digest selection.
	static bool find(uint16_t suite, TLSCipher &output); // Resolve supported wire identifiers without changing output on failure.
};

// Serialize one direction of record protection on its connection's cryptographic worker.
class TLSRecord {
	std::variant<std::monostate, AESGCM, ChaChaPoly, TLSCBC> cipher; // Own exactly one negotiated protection engine.
	TLSCipher config; // Immutable parameters within the current key epoch.
	std::array<uint8_t, 12> iv{}; // Fixed nonce salt, excluding transmitted explicit nonces.
	std::vector<uint8_t> scratch; // Unpublished output reused only after erasure.
	uint64_t sequence = 0; // Next record number within the installed traffic-key epoch.
	uint8_t error = 0; // Sticky protocol alert after a peer or sequence failure.
	bool ready = false; // Reject plaintext fallback before successful key installation.
	void prepare(size_t size); // Erase preceding unpublished plaintext before reusing storage.
	bool fail(uint8_t alert); // Retain the first failure and erase unpublished bytes.
	void nonce(uint8_t *output, const uint8_t *explicit_nonce = nullptr) const; // Combine the traffic salt with the directional record identity.
	void aad(uint8_t type, size_t size, uint8_t *output) const; // Bind legacy records to their sequence, type, version, and plaintext size.
	bool protect(bool encrypt, const uint8_t *nonce, Bytes input, Bytes aad, uint8_t *output, uint8_t *tag); // Dispatch authenticated encryption without changing the selected construction.
public:
	TLSRecord() = default; // Start without keys or an implicit plaintext mode.
	TLSRecord(const TLSRecord &) = delete; // Prevent duplicate nonce streams under copied traffic keys.
	TLSRecord &operator=(const TLSRecord &) = delete; // Keep sequence ownership unique.
	~TLSRecord(); // Erase nonce state and unpublished plaintext before key destruction.
	bool reset(uint16_t suite, Bytes key, Bytes salt, Bytes mac = {}); // Install a new key epoch; callers must never reinstall a previously used traffic key.
	uint8_t alert() const { return error; } // Report the original terminal alert to the handshake layer.
	bool seal(uint8_t type, Bytes plain, std::vector<uint8_t> &output, size_t padding = 0); // Produce one framed record, permitting zero padding only within the encoded plaintext limit.
	bool open(Bytes record, uint8_t &type, std::vector<uint8_t> &output); // Authenticate one complete framed record before publishing its type or plaintext.
};
}
