// Bind certificate signature identifiers to independently validated public-key verification.
#pragma once
#include "cert_core.h"
#include "cli/data/ec_core.h"
#include "cli/data/ed25519_core.h"
#include "cli/data/rsa_core.h"
#include <variant>

namespace GDCrypto {
// Select a strong signature construction independently of certificate trust policy.
struct SignatureInfo {
	enum Mode { NONE, RSA, PSS, EC, ED25519 }; // Supported signature encodings without legacy digest fallback.
	Mode mode = NONE; // Construction selected from a recognized identifier.
	unsigned hash = 0; // SHA-256/384/512 identifiers, or zero for message-based compact signatures.
	static bool parse(const Algorithm &algorithm, SignatureInfo &output); // Validate algorithm parameters and preserve output on failure.
};

// Retain immutable public-key precomputation for repeated concurrent signature checks.
class SignatureKey {
	std::variant<std::monostate, RSAPublic<64>, RSAPublic<0>, PrimeCurve> key; // Inline common RSA width with expandable fallback; no runtime width cap.
	std::array<uint8_t,133> point{}; // Largest supported uncompressed prime point or compact public key.
	KeyKind family = KeyKind::NONE; // Successfully validated key family.
	unsigned curve = 0; // Named curve width needed by handshake signature negotiation.
	size_t point_size = 0; // Actual encoded public-point length.
public:
	bool reset(Bytes spki); // Parse and validate public-key mathematics without assigning certificate trust.
	KeyKind kind() const { return family; } // Expose the key family for protocol negotiation.
	size_t bits() const; // Expose the mathematical width so the consuming protocol can apply its own peer policy.
	bool verify(SignatureInfo algorithm, Bytes message, Bytes signature) const; // Verify an encoded signature without changing public-key state or accepting weak digests.
};

bool verify_certificate_signature(const CertificateInfo &certificate, const SignatureKey &issuer); // Check only the exact signed bytes; names, time, constraints, and anchors remain mandatory trust checks.
}
