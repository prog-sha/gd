// Own immutable certificate encodings and index issuer candidates independently of trust decisions.
#pragma once
#include "constraint_core.h"
#include "signature_core.h"
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace GDCrypto {
// Keep borrowed certificate fields alive and initialize expensive key mathematics only when needed.
class Cert {
	std::vector<uint8_t> encoding; // Stable backing storage for every borrowed parsed field.
	CertificateInfo certificate; // Parsed envelope, validity, names, and signature bytes.
	CertConstraints extensions; // Parsed usages, policies, and extension constraints.
	NameConstraints filter; // Immutable indexes for subordinate-name checks.
	mutable std::once_flag key_once; // Serialize one public-key precomputation across verification workers.
	mutable std::unique_ptr<SignatureKey> public_key; // Cache successful key mathematics without retaining a mutable external key.
	Cert() = default; // Require validated construction through the owning factory.
public:
	using Ptr = std::shared_ptr<const Cert>; // Shared immutable lifetime for stores, paths, and worker jobs.
	Cert(const Cert &) = delete; // Prevent borrowed fields from pointing into another object's encoding.
	Cert &operator=(const Cert &) = delete; // Keep ownership and parsed fields inseparable.
	static Ptr read(Bytes input, size_t rsa_bits = 0); // Parse owned bytes and apply an optional consuming-protocol RSA policy before key precomputation.
	const CertificateInfo &info() const { return certificate; } // Borrow the immutable parsed certificate.
	const CertConstraints &constraints() const { return extensions; } // Borrow validated extension schemas without assigning trust.
	const SignatureKey *key() const; // Initialize and retain supported public-key mathematics on first use.
	bool permits(const Cert &subject) const { return filter.check(subject.extensions); } // Apply this issuer's name constraints to one subordinate certificate.
	bool permits(const CertNames &subject) const { return filter.check(subject); } // Apply this issuer's index to already normalized subordinate identities.
};

// Retain all issuer candidates with exact encoded-subject lookup and duplicate certificate elimination.
class CertStore {
	std::map<std::string_view,std::vector<Cert::Ptr>> subjects; // Borrowed subject encodings backed by retained certificate owners.
	std::set<std::string_view> encodings; // Exact DER identities without collision-sensitive hashing or duplicate ownership.
public:
	bool add(Cert::Ptr certificate); // Retain a new encoding once; callers finish store construction before concurrent reads.
	bool contains(const Cert &certificate) const; // Recognize an explicitly stored certificate, not merely a matching subject or key.
	const std::vector<Cert::Ptr> *issuers(const Cert &certificate) const; // Borrow subject-matched candidates without constructing a quadratic all-store scan.
	size_t size() const { return encodings.size(); } // Report distinct retained certificate encodings.
	std::vector<std::vector<uint8_t>> names() const; // Copy distinct encoded authority names for immutable certificate-request configuration.
};
}
