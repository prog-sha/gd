// Enforce complete key-schema consumption while preserving borrowed integer and point encodings.
#include "key_core.h"
#include "crypto_core.h"
#include <array>

namespace {
using namespace GDCrypto;
constexpr uint8_t RSA_ID[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01}; // RSA public and private key algorithm.
constexpr uint8_t EC_ID[] = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01}; // Prime-curve public key algorithm.
constexpr uint8_t ED_ID[] = {0x2b,0x65,0x70}; // Compact Edwards signature key algorithm.

// Match a complete object identifier without interpreting its arcs as bounded integers.
template <size_t N> bool matches(Bytes bytes, const uint8_t (&expected)[N]) { return bytes.size == N && equal(bytes.data, expected, N); }

// Unwrap one sequence and reject bytes outside its declared extent.
bool sequence(Bytes input, Bytes &content) {
	DER reader(input);
	return reader.take(0x30, content) && reader.empty();
}

// Read a canonical positive integer without imposing a cryptographic strength policy.
bool positive(DER &reader, Bytes &value) {
	if (!reader.integer(value)) return false;
	uint8_t combined = 0;
	for (size_t at = 0; at != value.size; ++at) combined |= value.data[at];
	return combined != 0;
}

// Require the explicit null parameters used by RSA key identifiers.
bool null_parameters(Bytes bytes) {
	const uint8_t encoded[] = {5, 0}; // Canonical null field.
	return matches(bytes, encoded);
}

// Resolve a domain identifier from a complete parameter field.
unsigned parameter_curve(Bytes parameters) {
	DER reader(parameters); Bytes oid;
	return reader.oid(oid) && reader.empty() ? named_curve(oid) : 0;
}

// Read an RSA public pair while preserving the full modulus width for its consumer.
bool public_pair(DER &reader, PublicKeyInfo &output) {
	uint64_t exponent = 0;
	if (!positive(reader, output.modulus) || !reader.number(exponent) || exponent < 2 || exponent > INT32_MAX) return false;
	output.kind = KeyKind::RSA; output.exponent = uint32_t(exponent);
	return true;
}

// Check nested attribute values with an explicit cursor stack bounded by their encoded input.
bool encoded_values(Bytes input) {
	std::vector<DER> readers{DER(input)};
	while (!readers.empty()) {
		if (readers.back().empty()) { readers.pop_back(); continue; }
		uint8_t tag; Bytes value;
		if (!readers.back().any(tag, value) || !tag) return false;
		if (tag & 0x20) readers.emplace_back(value);
	}
	return true;
}

// Validate ignored attributes as complete identifier-and-value-set structures.
bool attributes(Bytes input) {
	DER reader(input); Bytes content, oid, values;
	while (!reader.empty()) {
		if (!reader.take(0x30, content)) return false;
		DER attribute(content);
		if (!attribute.oid(oid) || !attribute.take(0x31, values) || !values.size || !attribute.empty() || !encoded_values(values)) return false;
	}
	return true;
}
}

// Parse exact algorithm syntax without accepting hidden trailing parameter fields.
bool GDCrypto::parse_algorithm(Bytes input, Algorithm &output) {
	Bytes content;
	if (!sequence(input, content)) return false;
	DER reader(content); Algorithm result;
	if (!reader.oid(result.oid)) return false;
	if (!reader.empty()) { uint8_t tag; Bytes value; if (!reader.any(tag, value, &result.parameters)) return false; }
	if (!reader.empty()) return false;
	output = result;
	return true;
}

// Select standard domain sizes without copying mathematical curve parameters into the parser.
unsigned GDCrypto::named_curve(Bytes oid) {
	constexpr uint8_t p256[] = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07}; // 256-bit prime domain.
	constexpr uint8_t prefix[] = {0x2b,0x81,0x04,0x00}; // Named-domain identifier prefix.
	if (matches(oid, p256)) return 256;
	if (oid.size != 5 || !equal(oid.data, prefix, sizeof(prefix))) return 0;
	return oid.data[4] == 0x21 ? 224 : oid.data[4] == 0x22 ? 384 : oid.data[4] == 0x23 ? 521 : 0;
}

// Decode public-key syntax; group membership and signature policy remain mathematical consumer checks.
bool GDCrypto::parse_public_key(Bytes input, PublicKeyInfo &output) {
	Bytes content, field, encoded, key;
	if (!sequence(input, content)) return false;
	DER reader(content); Algorithm algorithm; PublicKeyInfo result;
	if (!reader.take(0x30, field, &encoded) || !parse_algorithm(encoded, algorithm) || !reader.octets(key) || !reader.empty()) return false;
	if (matches(algorithm.oid, RSA_ID)) {
		if (!null_parameters(algorithm.parameters) || !sequence(key, content)) return false;
		DER pair(content);
		if (!public_pair(pair, result) || !pair.empty()) return false;
	} else if (matches(algorithm.oid, EC_ID)) {
		result.kind = KeyKind::EC; result.curve = parameter_curve(algorithm.parameters); result.point = key;
		if (!result.curve || key.size != 1 + 2 * ((result.curve + 7) / 8) || key.data[0] != 4) return false;
	} else if (matches(algorithm.oid, ED_ID)) {
		if (algorithm.parameters.size || key.size != 32) return false;
		result.kind = KeyKind::ED25519; result.point = key;
	} else return false;
	output = result;
	return true;
}

// Decode all private factor fields transactionally before mathematical key validation.
bool GDCrypto::parse_rsa_key(Bytes input, PrivateKeyInfo &output) {
	Bytes content;
	if (!sequence(input, content)) return false;
	DER reader(content); PrivateKeyInfo result; uint64_t version;
	if (!reader.number(version) || version > 1 || !public_pair(reader, result.public_key)) return false;
	for (Bytes *part : {&result.secret, &result.first, &result.second, &result.left_exp, &result.right_exp, &result.inverse}) if (!positive(reader, *part)) return false;
	if (version == 1) {
		if (!reader.take(0x30, content) || !content.size) return false;
		DER factors(content);
		while (!factors.empty()) {
			if (!factors.take(0x30, content)) return false;
			DER factor(content); ExtraFactor value;
			if (!positive(factor, value.prime) || !positive(factor, value.exponent) || !positive(factor, value.coefficient) || !factor.empty()) return false;
			result.extra.push_back(value);
		}
	}
	if (!reader.empty()) return false;
	output = std::move(result);
	return true;
}

// Resolve an elliptic private key's domain without trusting its optional claimed public point.
bool GDCrypto::parse_ec_key(Bytes input, PrivateKeyInfo &output, unsigned curve) {
	Bytes content;
	if (!sequence(input, content)) return false;
	DER reader(content); PrivateKeyInfo result; uint64_t version;
	if (!reader.number(version) || version != 1 || !reader.take(4, result.secret) || !result.secret.size) return false;
	if (reader.peek(0xa0)) {
		if (!reader.take(0xa0, content)) return false;
		const unsigned named = parameter_curve(content);
		if (!named || (curve && curve != named)) return false;
		curve = named;
	}
	if (!curve) return false;
	if (reader.peek(0xa1)) {
		if (!reader.take(0xa1, content)) return false;
		DER point(content);
		if (!point.octets(result.claimed_public) || !point.empty()) return false;
	}
	if (!reader.empty()) return false;
	result.public_key.kind = KeyKind::EC; result.public_key.curve = curve;
	output = std::move(result);
	return true;
}

// Decode supported unencrypted key envelopes and require consistent algorithm parameters.
bool GDCrypto::parse_private_key(Bytes input, PrivateKeyInfo &output) {
	Bytes content, field, encoded, private_key;
	if (!sequence(input, content)) return false;
	DER reader(content); PrivateKeyInfo result; Algorithm algorithm; uint64_t version;
	if (!reader.number(version) || version != 0 || !reader.take(0x30, field, &encoded) || !parse_algorithm(encoded, algorithm) || !reader.take(4, private_key)) return false;
	if (reader.peek(0xa0) && (!reader.take(0xa0, content) || !attributes(content))) return false;
	if (!reader.empty()) return false;
	if (matches(algorithm.oid, RSA_ID)) {
		if (!null_parameters(algorithm.parameters) || !parse_rsa_key(private_key, result)) return false;
	} else if (matches(algorithm.oid, EC_ID)) {
		const unsigned curve = parameter_curve(algorithm.parameters);
		if (!curve || !parse_ec_key(private_key, result, curve)) return false;
	} else if (matches(algorithm.oid, ED_ID)) {
		DER inner(private_key);
		if (algorithm.parameters.size || !inner.take(4, result.secret) || !inner.empty() || result.secret.size != 32) return false;
		result.public_key.kind = KeyKind::ED25519;
	} else return false;
	output = std::move(result);
	return true;
}
