// Share secret-storage and buffer-layout operations between independent cryptographic primitives.
#pragma once
#include <cstddef>
#include <cstdint>

namespace GDCrypto {
void erase(void *data, size_t size); // Clear secret storage through observable byte writes.
bool equal(const void *left, const void *right, size_t size); // Compare fixed public-length byte sequences without early exits.
bool overlap(const void *left, size_t left_size, const void *right, size_t right_size); // Detect intersecting buffers without out-of-range pointer arithmetic.
bool aead_layout(const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, const uint8_t *nonce, const uint8_t *auth, uint8_t *output); // Validate disjoint or exact record buffers with a 12-byte nonce and 16-byte tag.
uint32_t opaque_mask(uint32_t value); // Preserve a mask while preventing secret-selected load substitution.
}
