// Extract certificate extension constraints without conflating schema validity with path trust.
#pragma once
#include "cert_core.h"
#include <utility>

namespace GDCrypto {
// Retain a general name with its encoded family for later identity and constraint validation.
struct CertName {
	uint8_t tag = 0; // Context-specific family including its primitive or constructed encoding.
	Bytes value; // Borrowed name or address contents.
};

// Preserve subtree distances until supported-name semantics are applied by the path verifier.
struct CertSubtree {
	CertName name; // Base name or address-and-mask pair.
	int64_t minimum = 0, maximum = -1; // Encoded distances, with an absent maximum distinguished.
};

// Collect extension inputs whose ownership remains with the encoded certificate.
struct CertConstraints {
	bool basic = false, ca = false, san = false, constrained = false; // Presence and authority flags required by path validation.
	uint16_t usage = 0; // Nine standardized key-use bits in their public numbering.
	int64_t path_length = -1, explicit_policy = -1, mapping = -1, any_policy = -1; // Nonnegative path counters, or absent values.
	Bytes subject_id, authority_id; // Key identifiers used to prioritize candidate issuers.
	std::vector<Bytes> extended, policies, unhandled; // Exact usage, policy, and unsupported critical identifiers.
	std::vector<std::pair<Bytes,Bytes>> mappings; // Issuer-to-subject policy identifier mappings.
	std::vector<CertName> names; // Subject alternative names, including unknown families.
	std::vector<CertSubtree> permitted, excluded; // Name constraints evaluated against subordinate certificates.
};

bool parse_extensions(const CertificateInfo &certificate, CertConstraints &output); // Extract all supported schemas transactionally; name and policy semantics remain mandatory.
}
