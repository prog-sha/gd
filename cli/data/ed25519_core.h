// Derive and authenticate deterministic compact signatures with independently implemented curve arithmetic.
#pragma once
#include "der_core.h"

namespace GDCrypto {
bool ed25519_public(const uint8_t *seed, uint8_t *output); // Derive a 32-byte public key from a 32-byte seed.
bool ed25519_sign(const uint8_t *seed, Bytes message, uint8_t *output); // Sign a complete message into a 64-byte destination without exposing private intermediate values.
bool ed25519_verify(const uint8_t *peer, Bytes message, Bytes signature); // Authenticate an exact-width signature with a 32-byte peer key.
}
