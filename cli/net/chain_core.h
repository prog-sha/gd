// Build authenticated certificate paths from explicit anchors and untrusted issuer candidates.
#pragma once
#include "cert_store.h"

namespace GDCrypto {
// Supply explicit verification context without consulting ambient clocks or network services.
struct ChainOptions {
	int64_t now = 0; // Unix seconds at which every selected certificate must be valid.
	std::string_view hostname; // Optional endpoint identity; empty for client-certificate identity validation.
	std::vector<Bytes> usages, policies; // Accepted extended usages and initial policies; empty usages selects server authentication.
};

// Return only a fully verified path, with diagnostic progress retained on failure.
struct ChainResult {
	enum Error { NONE, UNTRUSTED, INVALID, HOSTNAME, USAGE, POLICY, LIMIT }; // Distinguish trust, schema-policy, identity, purpose, and work-budget failures.
	Error error = UNTRUSTED; // Verification remains unsuccessful until every path check finishes.
	size_t attempts = 0; // Issuer candidates considered, including cycles and unsupported keys.
	std::vector<Cert::Ptr> path; // Verified leaf-to-anchor path, empty on every failure.
};

bool verify_chain(Cert::Ptr leaf, const CertStore &roots, const CertStore &intermediates, const ChainOptions &options, ChainResult &output); // Authenticate one acceptable path without promoting untrusted certificates to anchors.
}
