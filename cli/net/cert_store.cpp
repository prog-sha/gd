// Preserve certificate ownership and defer public-key expansion until a candidate is actually used.
#include "cert_store.h"
#include <utility>

namespace GDCrypto {
// Publish exact authority names without leaking borrowed store lifetime into connection configuration.
std::vector<std::vector<uint8_t>> CertStore::names() const {
	std::vector<std::vector<uint8_t>> output;
	for (const auto &[name,certificates] : subjects) output.emplace_back(name.begin(),name.end());
	return output;
}

namespace {
// Borrow encoded bytes for ordered exact-identity indexes.
std::string_view identity(Bytes value) { return {reinterpret_cast<const char *>(value.data),value.size}; }

// Compare an RSA width with the caller's policy without overflowing a byte-to-bit conversion.
bool within_rsa(Bytes modulus, size_t limit) {
	if (!limit) return true;
	if (!modulus.size || !modulus.data || modulus.size-1 > limit/8) return false;
	unsigned first = 0; for (uint8_t value = modulus.data[0]; value; value >>= 1) ++first;
	return first <= limit-(modulus.size-1)*8;
}
}

// Copy one encoding before parsing so worker-visible views cannot outlive caller storage.
Cert::Ptr Cert::read(Bytes input, size_t rsa_bits) {
	if (!input.data || !input.size) return {};
	std::shared_ptr<Cert> value(new Cert);
	value->encoding.assign(input.data,input.data+input.size);
	const Bytes owned{value->encoding.data(),value->encoding.size()};
	if (!parse_certificate(owned,value->certificate) || !parse_extensions(value->certificate,value->extensions) || !value->filter.reset(value->extensions)) return {};
	PublicKeyInfo key;
	if (!parse_public_key(value->certificate.public_key,key) || (key.kind == KeyKind::RSA && !within_rsa(key.modulus,rsa_bits))) return {};
	return value;
}

// Cache a successful immutable public key once without publishing a partially initialized result.
const SignatureKey *Cert::key() const {
	std::call_once(key_once,[this]() {
		auto value = std::make_unique<SignatureKey>();
		if (value->reset(certificate.public_key)) public_key = std::move(value);
	});
	return public_key.get();
}

// Retain an exact certificate identity before exposing it through the issuer lookup table.
bool CertStore::add(Cert::Ptr certificate) {
	if (!certificate) return false;
	const auto raw = identity(certificate->info().raw), subject = identity(certificate->info().subject);
	if (encodings.find(raw) != encodings.end()) return false;
	auto &group = subjects[subject]; group.push_back(std::move(certificate));
	encodings.insert(raw);
	return true;
}

// Check anchor identity using the full encoding rather than a subject-name or public-key shortcut.
bool CertStore::contains(const Cert &certificate) const { return encodings.find(identity(certificate.info().raw)) != encodings.end(); }

// Return immutable subject-matched candidates for the path builder's priority and policy checks.
const std::vector<Cert::Ptr> *CertStore::issuers(const Cert &certificate) const {
	const auto found = subjects.find(identity(certificate.info().issuer));
	return found == subjects.end() ? nullptr : &found->second;
}
}
