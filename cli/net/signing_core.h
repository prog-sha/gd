// Own validated private signing identities independently of key-file and certificate-buffer lifetimes.
#pragma once
#include "signature_core.h"
#include "cli/data/rsa_private.h"
#include <memory>

namespace GDCrypto {
// Retain secret-bearing arithmetic under one lifetime and share only immutable signing operations.
class SigningKey {
	std::variant<std::monostate,RSAPrivate<64>,RSAPrivate<0>,PrimeCurve> key; // Inline ordinary RSA keys with dynamically sized fallback.
	CryptoStorage<uint8_t,0> secret; // Normalized elliptic scalar or compact signing seed, erased on release.
	std::vector<uint8_t> public_bytes; // Canonical public modulus or derived public point.
	KeyKind family = KeyKind::NONE; // Successfully validated mathematical family.
	size_t width = 0; // Public modulus or named-curve bit width.
	uint32_t exponent = 0; // Public RSA recovery exponent.
	SigningKey() = default; // Require successful mathematical validation before exposing a private identity.
	bool initialize(Bytes der); // Decode a complete private key, validate its mathematics, and derive its public identity.
public:
	static std::unique_ptr<SigningKey> read(Bytes der); // Publish only a fully usable private identity.
	KeyKind kind() const { return family; } // Expose the key family without revealing private state.
	size_t bits() const { return width; } // Report width for signature-scheme negotiation, not an allocation policy.
	bool matches(Bytes spki) const; // Compare the derived public identity with a certificate's encoded public key.
	bool sign(SignatureInfo algorithm, Bytes message, std::vector<uint8_t> &output) const; // Publish a complete signature only after successful private arithmetic and fault checks.
};
}
