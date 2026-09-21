// Borrow certificate fields for independent signature, extension, and trust-path verification.
#pragma once
#include "cli/data/key_core.h"

namespace GDCrypto {
// Preserve an extension's exact identifier and content until its semantics are verified.
struct CertExtension {
	Bytes oid, value; // Canonical identifier contents and unwrapped extension octets.
	bool critical = false; // Require semantic handling before a path may be trusted.
};

// Keep encoded certificate ownership separate from schema parsing and trust decisions.
struct CertificateInfo {
	Bytes raw, signed_data, serial, issuer, subject, public_key, signature; // Exact borrowed fields used for identity and signature checks.
	Algorithm algorithm; // Signature identifier whose inner and outer encodings agree.
	int64_t not_before = 0, not_after = 0; // Parsed validity endpoints as Unix seconds.
	unsigned version = 0; // Zero-based encoded certificate version.
	uint8_t signature_unused = 0; // Unused bits to remove when aligning the signature value.
	std::vector<CertExtension> extensions; // Ordered extensions with duplicate identifiers rejected.
};

bool certificate_time(uint8_t tag, Bytes input, int64_t &output); // Validate calendar and canonical time fields without process timezone state.
bool certificate_text(uint8_t tag, Bytes input); // Validate supported distinguished-name encodings without converting their values.
bool certificate_attribute(uint8_t tag, Bytes input); // Validate a distinguished-name value while retaining nontextual identity octets.
bool parse_certificate(Bytes input, CertificateInfo &output); // Parse one complete certificate without treating a successful parse as trusted identity.
}
