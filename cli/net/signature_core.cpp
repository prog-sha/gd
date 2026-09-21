// Validate signature parameter encodings and authenticate certificate bytes with immutable native keys.
#include "signature_core.h"
#include <cstring>

namespace GDCrypto {
namespace {
constexpr uint8_t rsa_prefix[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01}; // Identifier stem for RSA signature constructions.
constexpr uint8_t ec_prefix[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03}; // Identifier stem for elliptic signatures with SHA-2 digests.
constexpr uint8_t hash_prefix[] = {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02}; // Identifier stem for the supported SHA-2 digests.

// Match a complete identifier family and return its final single-octet arc.
template <size_t Size> unsigned identifier(Bytes oid, const uint8_t (&prefix)[Size]) {
	return oid.size == Size + 1 && oid.data && equal(oid.data,prefix,Size) ? oid.data[Size] : 0;
}

// Accept absent or canonical NULL digest parameters where the signature grammar permits either.
bool digest_parameters(Bytes input) { return !input.size || (input.size == 2 && input.data && input.data[0] == 5 && input.data[1] == 0); }

// Require a complete nonnegative integer inside an explicitly tagged parameter.
bool explicit_number(DER &reader, uint8_t tag, uint64_t &output) {
	Bytes content; if (!reader.take(tag,content)) return false;
	DER integer(content); return integer.number(output) && integer.empty();
}

// Validate matching message and mask digests, digest-width salt, and the standard trailer.
bool pss_parameters(Bytes input, unsigned &hash) {
	Bytes content, encoded; DER envelope(input);
	if (!envelope.take(0x30,content) || !envelope.empty()) return false;
	DER fields(content); Algorithm message, mask, masked; uint64_t salt, trailer = 1;
	if (!fields.take(0xa0,encoded) || !parse_algorithm(encoded,message) || !fields.take(0xa1,encoded) || !parse_algorithm(encoded,mask) || !explicit_number(fields,0xa2,salt)) return false;
	if (fields.peek(0xa3) && !explicit_number(fields,0xa3,trailer)) return false;
	if (!fields.empty() || trailer != 1 || !digest_parameters(message.parameters) || identifier(mask.oid,rsa_prefix) != 8 || !parse_algorithm(mask.parameters,masked) || !digest_parameters(masked.parameters)) return false;
	const unsigned selected = identifier(message.oid,hash_prefix);
	if (!selected || selected > 3 || identifier(masked.oid,hash_prefix) != selected || salt != (selected == 1 ? 32 : selected == 2 ? 48 : 64)) return false;
	hash = selected; return true;
}

// Dispatch digest-bound public recovery without re-parsing or re-expanding the RSA key.
template <size_t Words> bool rsa_signature(const RSAPublic<Words> &key, SignatureInfo algorithm, Bytes digest, Bytes signature) {
	if (algorithm.mode == SignatureInfo::RSA) return key.verify_digest(algorithm.hash,digest,signature);
	if (algorithm.hash == 1) return key.template verify_pss<Hash32>(Hash32::SHA256,digest,signature,digest.size);
	return key.template verify_pss<Hash64>(algorithm.hash == 2 ? Hash64::SHA384 : Hash64::SHA512,digest,signature,digest.size);
}
}

// Recognize strong algorithm families while keeping parameter-bearing constructions explicit.
bool SignatureInfo::parse(const Algorithm &algorithm, SignatureInfo &output) {
	SignatureInfo value;
	const unsigned rsa = identifier(algorithm.oid,rsa_prefix), ec = identifier(algorithm.oid,ec_prefix);
	if (rsa >= 11 && rsa <= 13) value = {RSA,rsa-10};
	else if (rsa == 10) {value.mode = PSS; if (!pss_parameters(algorithm.parameters,value.hash)) return false;}
	else if (ec >= 2 && ec <= 4) value = {EC,ec-1};
	else {
		constexpr uint8_t id[] = {0x2b,0x65,0x70}; // Compact message-signature identifier.
		if (algorithm.oid.size != sizeof(id) || !algorithm.oid.data || !equal(algorithm.oid.data,id,sizeof(id)) || algorithm.parameters.size) return false;
		value = {ED25519,0};
	}
	output = value; return true;
}

// Replace a public key atomically with an inert state on every parsing or mathematical failure.
bool SignatureKey::reset(Bytes spki) {
	family = KeyKind::NONE; curve = 0; point_size = 0; key.emplace<std::monostate>(); point.fill(0);
	PublicKeyInfo parsed; if (!parse_public_key(spki,parsed)) return false;
	if (parsed.kind == KeyKind::RSA) {
		const bool valid = parsed.modulus.size <= 256 ? key.emplace<RSAPublic<64>>().reset(parsed.modulus,parsed.exponent) : key.emplace<RSAPublic<0>>().reset(parsed.modulus,parsed.exponent);
		if (!valid) return false;
	} else {
		if (parsed.kind == KeyKind::EC) {
			auto &domain = key.emplace<PrimeCurve>();
			if (!domain.reset(parsed.curve) || !domain.valid_public(parsed.point)) return false;
			curve = parsed.curve;
		}
		point_size = parsed.point.size; std::memcpy(point.data(),parsed.point.data,point_size);
	}
	family = parsed.kind; return true;
}

// Report width without conflating an inline allocation choice with a peer key-strength policy.
size_t SignatureKey::bits() const {
	if (family == KeyKind::EC) return curve;
	if (family == KeyKind::ED25519) return 256;
	if (family != KeyKind::RSA) return 0;
	if (const auto *fixed = std::get_if<RSAPublic<64>>(&key)) return fixed->bits();
	return std::get<RSAPublic<0>>(key).bits();
}

// Hash the message with the declared strong digest before key-family-specific verification.
bool SignatureKey::verify(SignatureInfo algorithm, Bytes message, Bytes signature) const {
	if ((message.size && !message.data) || (signature.size && !signature.data)) return false;
	if (algorithm.mode == SignatureInfo::ED25519) return family == KeyKind::ED25519 && !algorithm.hash && ed25519_verify(point.data(),message,signature);
	if (algorithm.hash < 1 || algorithm.hash > 3 || (algorithm.mode == SignatureInfo::EC ? family != KeyKind::EC : ((algorithm.mode != SignatureInfo::RSA && algorithm.mode != SignatureInfo::PSS) || family != KeyKind::RSA))) return false;
	std::array<uint8_t,64> digest{}; const size_t width = algorithm.hash == 1 ? 32 : algorithm.hash == 2 ? 48 : 64;
	if (algorithm.hash == 1) {Hash32 hash; hash.write(message.data,message.size); hash.sum(digest.data());}
	else {Hash64 hash(algorithm.hash == 2 ? Hash64::SHA384 : Hash64::SHA512); hash.write(message.data,message.size); hash.sum(digest.data());}
	if (family == KeyKind::EC) return std::get<PrimeCurve>(key).verify({point.data(),point_size},{digest.data(),width},signature);
	if (const auto *fixed = std::get_if<RSAPublic<64>>(&key)) return rsa_signature(*fixed,algorithm,{digest.data(),width},signature);
	return rsa_signature(std::get<RSAPublic<0>>(key),algorithm,{digest.data(),width},signature);
}

// Normalize non-octet-aligned signature bits only when required by their encoded representation.
bool verify_certificate_signature(const CertificateInfo &certificate, const SignatureKey &issuer) {
	SignatureInfo algorithm; if (!SignatureInfo::parse(certificate.algorithm,algorithm) || certificate.signature_unused > 7) return false;
	if (!certificate.signature_unused) return issuer.verify(algorithm,certificate.signed_data,certificate.signature);
	if (!certificate.signature.data) return false;
	std::vector<uint8_t> aligned(certificate.signature.size);
	for (size_t at = 0; at != aligned.size(); ++at) aligned[at] = uint8_t(certificate.signature.data[at] >> certificate.signature_unused) | (at ? uint8_t(certificate.signature.data[at-1] << (8-certificate.signature_unused)) : 0);
	return issuer.verify(algorithm,certificate.signed_data,{aligned.data(),aligned.size()});
}
}
