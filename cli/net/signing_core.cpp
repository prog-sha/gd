// Validate private-key relationships and sign messages without an external cryptographic backend.
#include "signing_core.h"
#include <cstring>

namespace GDCrypto {
namespace {
// Compare exact unsigned public values without encoding-dependent aliases.
bool same(Bytes a, Bytes b) { return a.size == b.size && (!a.size || equal(a.data,b.data,a.size)); }

// Multiply a growing factor product exactly within the announced public modulus width.
bool factor_product(CryptoStorage<uint8_t,0> &value, Bytes factor) {
	CryptoStorage<uint8_t,0> result(value.size()); uint32_t overflow = 0;
	for (size_t a = 0; a != value.size(); ++a) {
		uint32_t carry = 0;
		for (size_t b = 0; b != factor.size; ++b) {
			const size_t at = a+b;
			carry += uint32_t(value[value.size()-1-a])*factor.data[factor.size-1-b];
			if (at < result.size()) {carry += result[result.size()-1-at]; result[result.size()-1-at] = uint8_t(carry);}
			else overflow |= carry;
			carry >>= 8;
		}
		for (size_t at = a+factor.size; at < result.size(); ++at) {carry += result[result.size()-1-at]; result[result.size()-1-at] = uint8_t(carry); carry >>= 8;}
		overflow |= carry;
	}
	if (overflow) return false;
	value = std::move(result); return true;
}

// Require all multi-factor exponent, coprimality, and exact-product relationships before using an unfactored exponent.
bool multiple_factors(const PrivateKeyInfo &info) {
	std::vector<ExtraFactor> factors{{info.first,info.left_exp,{}},{info.second,info.right_exp,info.inverse}};
	factors.insert(factors.end(),info.extra.begin(),info.extra.end());
	CryptoStorage<uint8_t,0> product(info.public_key.modulus.size); product.back() = 1;
	CryptoStorage<uint8_t,0> exponent(info.secret.size+4); uint64_t carry = 0;
	for (size_t at = info.secret.size; at != 0; --at) {carry += uint64_t(info.secret.data[at-1])*info.public_key.exponent; exponent[at+3] = uint8_t(carry); carry >>= 8;}
	for (unsigned at = 4; at != 0; --at) {exponent[at-1] = uint8_t(carry); carry >>= 8;}
	for (size_t at = 0; at != factors.size(); ++at) {
		const auto &part = factors[at]; Mod<0> prime; if (!prime.reset(part.prime)) return false;
		auto residue = prime.previous_remainder({exponent.data(),exponent.size()}); residue[0] ^= 1;
		uint32_t difference = 0; for (uint32_t word : residue) difference |= word;
		if (difference) return false;
		residue = prime.previous_remainder(info.secret); Mod<0>::Value declared;
		if (!prime.read(part.exponent,declared) || !equal(residue.data(),declared.data(),residue.size()*sizeof(uint32_t))) return false;
		if (at) {
			// The first coefficient reverses the factor order; subsequent coefficients invert the preceding product.
			Mod<0> domain; const Bytes base = at == 1 ? info.second : Bytes{product.data(),product.size()};
			if (!domain.reset(at == 1 ? info.first : part.prime) || !domain.read(part.coefficient,declared)) return false;
			residue = domain.leave(domain.multiply(domain.enter(declared),domain.enter(domain.remainder(base)))); residue[0] ^= 1;
			difference = 0; for (uint32_t word : residue) difference |= word;
			if (difference) return false;
		}
		if (!factor_product(product,part.prime)) return false;
	}
	return same({product.data(),product.size()},info.public_key.modulus);
}

// Bind a validated RSA key to the requested digest construction and exact signature width.
template <size_t Words> bool rsa_sign(const RSAPrivate<Words> &key, SignatureInfo algorithm, Bytes digest, std::vector<uint8_t> &out) {
	out.resize(key.size());
	if (algorithm.mode == SignatureInfo::RSA) return key.sign_digest(algorithm.hash,digest,out.data(),out.size());
	if (algorithm.hash == 1) return key.template sign_pss<Hash32>(Hash32::SHA256,digest,out.data(),out.size(),digest.size);
	return key.template sign_pss<Hash64>(algorithm.hash == 2 ? Hash64::SHA384 : Hash64::SHA512,digest,out.data(),out.size(),digest.size);
}
}

// Parse supported private envelopes without relying on their optional claimed public-key fields.
bool SigningKey::initialize(Bytes der) {
	PrivateKeyInfo info;
	if (!parse_private_key(der,info) && !parse_rsa_key(der,info) && !parse_ec_key(der,info)) return false;
	family = info.public_key.kind;
	if (family == KeyKind::RSA) {
		const auto &pub = info.public_key; exponent = pub.exponent;
		const auto load = [&](auto &rsa) {return info.extra.empty() ? rsa.reset(pub.modulus,exponent,info.secret,info.first,info.second,info.left_exp,info.right_exp,info.inverse) : multiple_factors(info) && rsa.reset(pub.modulus,exponent,info.secret);};
		if (!(pub.modulus.size <= 256 ? load(key.emplace<RSAPrivate<64>>()) : load(key.emplace<RSAPrivate<0>>()))) return false;
		public_bytes.assign(pub.modulus.data,pub.modulus.data+pub.modulus.size);
		width = (pub.modulus.size-1)*8; for (uint8_t top = pub.modulus.data[0]; top; top >>= 1) ++width;
	} else {
		Bytes scalar = info.secret;
		if (family == KeyKind::EC) {
			auto &domain = key.emplace<PrimeCurve>(); width = info.public_key.curve;
			if (!domain.reset(unsigned(width))) return false;
			while (scalar.size > domain.size() && !scalar.data[0]) {++scalar.data; --scalar.size;}
			if (scalar.size > domain.size()) return false;
			secret.resize(domain.size()); std::memcpy(secret.data()+secret.size()-scalar.size,scalar.data,scalar.size);
			public_bytes.resize(1+2*domain.size());
			if (!domain.public_key({secret.data(),secret.size()},public_bytes.data(),public_bytes.size())) return false;
		} else if (family == KeyKind::ED25519) {
			width = 256; secret.assign(scalar.data,scalar.data+scalar.size); public_bytes.resize(32);
			if (!ed25519_public(secret.data(),public_bytes.data())) return false;
		} else return false;
	}
	return true;
}

// Keep failed initialization private so callers never observe a partially usable key.
std::unique_ptr<SigningKey> SigningKey::read(Bytes der) {
	std::unique_ptr<SigningKey> result(new SigningKey); return result->initialize(der) ? std::move(result) : nullptr;
}

// Match certificate identity against derived public values rather than optional private-file metadata.
bool SigningKey::matches(Bytes spki) const {
	PublicKeyInfo info; if (!parse_public_key(spki,info) || family != info.kind) return false;
	if (family == KeyKind::RSA) return exponent == info.exponent && same({public_bytes.data(),public_bytes.size()},info.modulus);
	return (family != KeyKind::EC || width == info.curve) && same({public_bytes.data(),public_bytes.size()},info.point);
}

// Perform immutable signing with per-call scratch and publish no partial bytes on failure.
bool SigningKey::sign(SignatureInfo algorithm, Bytes message, std::vector<uint8_t> &output) const {
	if ((message.size && !message.data) || family == KeyKind::NONE) return false;
	std::vector<uint8_t> result;
	if (algorithm.mode == SignatureInfo::ED25519) {
		if (family != KeyKind::ED25519 || algorithm.hash) return false;
		result.resize(64); if (!ed25519_sign(secret.data(),message,result.data())) return false;
	} else {
		if (algorithm.hash < 1 || algorithm.hash > 3 || (algorithm.mode == SignatureInfo::EC ? family != KeyKind::EC : ((algorithm.mode != SignatureInfo::RSA && algorithm.mode != SignatureInfo::PSS) || family != KeyKind::RSA))) return false;
		std::array<uint8_t,64> digest{}; const size_t size = algorithm.hash == 1 ? 32 : algorithm.hash == 2 ? 48 : 64;
		if (algorithm.hash == 1) {Hash32 hash; hash.write(message.data,message.size); hash.sum(digest.data());}
		else {Hash64 hash(algorithm.hash == 2 ? Hash64::SHA384 : Hash64::SHA512); hash.write(message.data,message.size); hash.sum(digest.data());}
		if (family == KeyKind::EC) {
			result.resize(2*secret.size()+9); size_t written = 0;
			if (!std::get<PrimeCurve>(key).sign({secret.data(),secret.size()},{digest.data(),size},result.data(),result.size(),written)) return false;
			result.resize(written);
		} else if (const auto *fixed = std::get_if<RSAPrivate<64>>(&key)) {if (!rsa_sign(*fixed,algorithm,{digest.data(),size},result)) return false;}
		else if (!rsa_sign(std::get<RSAPrivate<0>>(key),algorithm,{digest.data(),size},result)) return false;
	}
	output = std::move(result); return true;
}
}
