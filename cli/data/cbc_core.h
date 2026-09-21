// Authenticate legacy TLS records while keeping padding failures inseparable from message-authentication failures.
#pragma once
#include "aes_core.h"
#include "der_core.h"
#include "kdf_core.h"
#include <optional>
#include <vector>

namespace GDCrypto {
// Retain keyed prefixes and reusable record storage for a serialized cryptographic worker.
class TLSCBC {
	// Keep both inner and outer authentication finalization independent of private tail lengths.
	struct FixedHash : Hash32 {
		using Hash32::Hash32;
		void sum(uint8_t *output) const { constant_sum(output); } // Use fixed-work terminal compression blocks.
	};
	AES aes; // Expanded block key, shared by encryption and decryption.
	std::optional<HMAC<FixedHash>> mac; // Precomputed keyed digest prefixes after successful initialization.
	std::vector<uint8_t> scratch; // Unpublished plaintext or ciphertext owned exclusively by the worker.
	void prepare(size_t size); // Erase preceding scratch contents before reusing its allocation.
	void authenticate(uint64_t sequence, uint8_t type, Bytes plain, uint8_t *output); // Bind record identity and declared plaintext length to its content.
public:
	~TLSCBC(); // Erase unpublished plaintext before releasing its allocation.
	bool reset(Bytes key, Bytes auth); // Install a 128/256-bit block key and a 20-byte authentication key.
	bool seal(uint64_t sequence, uint8_t type, Bytes plain, std::vector<uint8_t> &output); // Produce one explicit-IV record without publishing partial ciphertext.
	bool open(uint64_t sequence, uint8_t type, Bytes record, std::vector<uint8_t> &output); // Verify padding and authentication before publishing any plaintext.
};
}
