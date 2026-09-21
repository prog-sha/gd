// Decode borrowed key schemas before key-specific mathematical validation and ownership transfer.
#pragma once
#include "der_core.h"
#include <vector>

namespace GDCrypto {
enum class KeyKind { NONE, RSA, EC, ED25519 }; // Mathematical key family selected by its identifier.

// Retain exact algorithm parameters without applying a signature or trust policy.
struct Algorithm {
	Bytes oid, parameters; // Identifier contents and optional complete parameter encoding.
};

// Describe public key bytes whose backing encoding must outlive this view.
struct PublicKeyInfo {
	KeyKind kind = KeyKind::NONE; // Selected key family.
	unsigned curve = 0; // Named prime-curve bit width, when applicable.
	Bytes modulus, point; // RSA integer or encoded curve point.
	uint32_t exponent = 0; // Public RSA exponent after range checking.
};

// Describe one additional factor of a multi-prime private key.
struct ExtraFactor { Bytes prime, exponent, coefficient; };

// Borrow private components for validation without duplicating key material during schema parsing.
struct PrivateKeyInfo {
	PublicKeyInfo public_key; // Declared key family and domain parameters, not yet authenticated.
	Bytes secret, first, second, left_exp, right_exp, inverse; // Private scalar and optional RSA factors and CRT values.
	Bytes claimed_public; // Optional curve key that must not replace deriving the public key from the secret.
	std::vector<ExtraFactor> extra; // Encoded additional factors, bounded only by the input schema.
};

bool parse_algorithm(Bytes input, Algorithm &output); // Require a complete algorithm sequence with at most one parameter field.
unsigned named_curve(Bytes oid); // Recognize supported named prime-curve identifiers.
bool parse_public_key(Bytes input, PublicKeyInfo &output); // Decode a complete subject-public-key structure without assigning trust.
bool parse_rsa_key(Bytes input, PrivateKeyInfo &output); // Decode a complete private factor schema, including additional factors.
bool parse_ec_key(Bytes input, PrivateKeyInfo &output, unsigned curve = 0); // Decode an elliptic private key with explicit or enclosing domain parameters.
bool parse_private_key(Bytes input, PrivateKeyInfo &output); // Decode an unencrypted private-key information envelope.
}
