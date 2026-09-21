// Protect records with an arithmetic stream cipher and a one-time polynomial authenticator.
#pragma once
#include "crypto_core.h"
#include <array>

namespace GDCrypto {
// Own a 256-bit key; require a unique 12-byte nonce under each key.
class ChaChaPoly {
	std::array<uint8_t, 32> key{}; // Stream cipher key material.
	bool ready = false; // Initialization status.
	void crypt(const uint8_t *nonce, const uint8_t *input, size_t size, uint8_t *output) const; // Encrypt with counter blocks starting at one.
	void tag(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, uint8_t *output) const; // Authenticate padded ciphertext and associated data.
public:
	~ChaChaPoly(); // Erase the stream cipher key.
	bool reset(const uint8_t *input, size_t size); // Accept exactly one 32-byte key.
	static bool valid_size(uint64_t size); // Reject messages that exhaust the block counter.
	bool seal(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, uint8_t *output, uint8_t *auth) const; // Encrypt with a detached 16-byte tag and exact or disjoint input/output.
	bool open(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, const uint8_t *auth, uint8_t *output) const; // Release plaintext only after verifying the whole tag.
};
}
