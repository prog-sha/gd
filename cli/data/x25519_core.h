// Compute fixed-width key agreement while rejecting non-contributory public inputs.
#pragma once
#include <cstdint>

namespace GDCrypto {
bool x25519(const uint8_t *secret, const uint8_t *peer, uint8_t *output); // Consume 32-byte inputs, return a canonical shared value, and leave output unchanged on failure.
bool x25519_public(const uint8_t *secret, uint8_t *output); // Derive a public key from a 32-byte private seed.
}
