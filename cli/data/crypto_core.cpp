// Clear secrets, compare authenticators, and validate buffer aliases with unsigned operations.
#include "crypto_core.h"

// Clear storage without permitting dead-store elimination.
void GDCrypto::erase(void *data, size_t size) {
	auto *bytes = static_cast<volatile uint8_t *>(data);
	while (size--) *bytes++ = 0;
}

// Accumulate differences before making a single public result decision.
bool GDCrypto::equal(const void *left, const void *right, size_t size) {
	const auto *a = static_cast<const uint8_t *>(left), *b = static_cast<const uint8_t *>(right);
	uint8_t difference = 0;
	for (size_t at = 0; at != size; ++at) difference |= a[at] ^ b[at];
	return difference == 0;
}

// Determine whether two bounded buffers alias without forming out-of-range pointers.
bool GDCrypto::overlap(const void *left, size_t left_size, const void *right, size_t right_size) {
	if (!left_size || !right_size) return false;
	const auto a = reinterpret_cast<uintptr_t>(left), b = reinterpret_cast<uintptr_t>(right);
	return a <= b ? b - a < left_size : a - b < right_size;
}

// Validate a record destination without accessing any caller-provided bytes.
bool GDCrypto::aead_layout(const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, const uint8_t *nonce, const uint8_t *auth, uint8_t *output) {
	return (!size || (input && output)) && (!aad_size || aad) && nonce && auth &&
			(input == output || !overlap(input, size, output, size)) && !overlap(aad, aad_size, output, size) &&
			!overlap(nonce, 12, output, size) && !overlap(auth, 16, output, size);
}

// Hide mask provenance from optimization without changing its value or memory-access pattern.
uint32_t GDCrypto::opaque_mask(uint32_t value) {
	volatile uint32_t preserved = value;
	return preserved;
}
