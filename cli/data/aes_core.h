// Encrypt blocks and authenticate records without retaining complete messages.
#pragma once
#include "crypto_core.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace GDCrypto {
// Own an expanded encryption key with no secret-indexed lookup tables.
class AES {
	std::array<uint8_t, 240> keys{}; // Expanded words for the largest supported key.
	unsigned rounds = 0; // Zero denotes an uninitialized key.
public:
	static bool accelerated(); // Report runtime support for both block rounds and polynomial multiplication.

	~AES(); // Erase expanded secret material.
	bool reset(const uint8_t *key, size_t size); // Accept a 128, 192, or 256-bit key.
	bool encrypt(const uint8_t *input, uint8_t *output) const; // Encrypt one block, allowing exact overlap.
	bool decrypt(const uint8_t *input, uint8_t *output) const; // Decrypt one block using the same expanded key and overlap contract.
};

// Authenticate records with a 12-byte nonce, unique under each key, and a 16-byte tag.
class AESGCM {
	AES aes; // Expanded block cipher key.
	std::array<uint8_t, 16> subkey{}; // Authentication field multiplier.
	bool ready = false; // Reject use before successful initialization.
	void tag(const uint8_t *nonce, const uint8_t *data, size_t size, const uint8_t *aad, size_t aad_size, uint8_t *out) const; // Bind ciphertext and associated data to a nonce.
	void crypt(const uint8_t *nonce, const uint8_t *input, size_t size, uint8_t *output) const; // Apply the record counter stream.
public:
	~AESGCM(); // Erase the authentication subkey.
	bool reset(const uint8_t *key, size_t size); // Replace the record protection key.
	static bool valid_size(uint64_t size, uint64_t aad_size); // Enforce counter and encoded bit-length bounds before accessing input.
	bool seal(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, uint8_t *output, uint8_t *auth) const; // Encrypt into disjoint or exactly overlapping storage and return a 16-byte tag.
	bool open(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, const uint8_t *auth, uint8_t *output) const; // Leave output unchanged if authentication fails.
};
}
