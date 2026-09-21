// Authenticate compact signatures using complete extended-coordinate group operations.
#include "ed25519_core.h"
#include "field25519.h"
#include "hash64_core.h"
#include "mod_core.h"

namespace {
using namespace GDCrypto::Field25519;
using GDCrypto::Bytes;
using GDCrypto::Hash64;
using Scalar = GDCrypto::Mod<8>;

// Keep the extended-coordinate identity valid even before the first group operation.
struct Point { Field x{}, y{1}, z{1}, t{}; };

// Share immutable mathematical constants and subgroup reduction precomputation.
struct Curve {
	Field d{}, sqrt_i{}; // Curve coefficient and square root of minus one.
	Point base; // Standard generator with positive even X coordinate.
	Scalar order; // Prime subgroup modulus; signature scalars use little-endian encoding.
	Curve(); // Derive field constants and initialize the standard generator.
	bool decode(const uint8_t *input, Point &output) const; // Recover an on-curve point from its compact encoding.
	Point plus(const Point &left, const Point &right) const; // Add any pair of curve points with complete formulas.
	Point times(const Point &point, const uint8_t *scalar) const; // Process every bit of a fixed-width scalar with masked selection.
};

// Derive public domain constants from their defining field relations.
Curve::Curve() {
	d = subtract({}, multiply(Field{121665}, inverse(Field{121666})));
	sqrt_i = multiply(squares(root_power(Field{2}), 1), Field{2});
	std::array<uint8_t, 32> encoded{}; encoded.fill(0x66); encoded[0] = 0x58;
	decode(encoded.data(), base);
	constexpr uint8_t subgroup[] = {0x10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x14,0xde,0xf9,0xde,0xa2,0xf7,0x9c,0xd6,0x58,0x12,0x63,0x1a,0x5c,0xf5,0xd3,0xed}; // Prime order of the standard generator.
	order.reset({subgroup, sizeof(subgroup)});
}

// Validate the public curve equation while accepting equivalent encoded public coordinates.
bool Curve::decode(const uint8_t *input, Point &output) const {
	Point point; point.y = GDCrypto::Field25519::decode(input);
	const Field y2 = squares(point.y, 1), u = subtract(y2, Field{1}), v = add(multiply(d, y2), Field{1});
	const Field v3 = multiply(squares(v, 1), v), v7 = multiply(squares(v3, 1), v);
	point.x = multiply(multiply(u, v3), root_power(multiply(u, v7)));
	const Field check = multiply(v, squares(point.x, 1));
	if (check != u) {
		if (check != subtract({}, u)) return false;
		point.x = multiply(point.x, sqrt_i);
	}
	if ((point.x[0] & 1) != (input[31] >> 7)) point.x = subtract({}, point.x);
	point.t = multiply(point.x, point.y);
	output = point;
	return true;
}

// Add extended points without exceptional branches or affine inversions.
Point Curve::plus(const Point &left, const Point &right) const {
	const Field a = multiply(subtract(left.y, left.x), subtract(right.y, right.x));
	const Field b = multiply(GDCrypto::Field25519::add(left.y, left.x), GDCrypto::Field25519::add(right.y, right.x));
	const Field c = scale(multiply(multiply(left.t, right.t), d), 2), d2 = scale(multiply(left.z, right.z), 2);
	const Field e = subtract(b, a), f = subtract(d2, c), g = GDCrypto::Field25519::add(d2, c), h = GDCrypto::Field25519::add(b, a);
	return {multiply(e, f), multiply(g, h), multiply(f, g), multiply(e, h)};
}

// Double an extended point with four squares and no exceptional-case handling.
Point twice(const Point &point) {
	const Field a = squares(point.x, 1), b = squares(point.y, 1), c = scale(squares(point.z, 1), 2), d = subtract({}, a);
	const Field e = subtract(subtract(squares(add(point.x, point.y), 1), a), b), g = add(d, b), f = subtract(g, c), h = subtract(d, b);
	return {multiply(e, f), multiply(g, h), multiply(f, g), multiply(e, h)};
}

// Select complete point coordinates with a compiler-opaque secret bit.
Point choose(Point left, const Point &right, uint32_t bit) {
	const uint64_t mask = uint64_t(0) - GDCrypto::opaque_mask(bit);
	for (auto pair : {std::pair<Field *, const Field *>{&left.x, &right.x}, {&left.y, &right.y}, {&left.z, &right.z}, {&left.t, &right.t}}) {
		for (size_t at = 0; at != left.x.size(); ++at) (*pair.first)[at] ^= ((*pair.first)[at] ^ (*pair.second)[at]) & mask;
	}
	return left;
}

// Use one doubling and one complete addition for each encoded secret bit.
Point Curve::times(const Point &point, const uint8_t *scalar) const {
	Point result;
	for (unsigned bit = 256; bit != 0; --bit) {
		result = twice(result);
		result = choose(result, plus(result, point), (scalar[(bit - 1) / 8] >> ((bit - 1) % 8)) & 1);
	}
	return result;
}

// Encode a point canonically with its affine X parity in the reserved Y bit.
std::array<uint8_t, 32> encode(const Point &point) {
	const Field inverted = inverse(point.z), x = multiply(point.x, inverted);
	auto encoded = GDCrypto::Field25519::encode(multiply(point.y, inverted));
	encoded[31] |= uint8_t((x[0] & 1) << 7);
	return encoded;
}

// Initialize immutable domain parameters once, without using application-global runtime objects.
const Curve &curve() { static const Curve parameters; return parameters; }

// Reduce a little-endian byte string into a canonical subgroup scalar.
Scalar::Value reduce_scalar(const uint8_t *input, size_t size) {
	std::array<uint8_t, 64> reversed{};
	std::reverse_copy(input, input + size, reversed.data());
	auto value = curve().order.remainder({reversed.data(), size});
	GDCrypto::erase(reversed.data(), reversed.size());
	return value;
}

// Serialize a canonical scalar with the signature format's fixed little-endian width.
std::array<uint8_t, 32> encode_scalar(const Scalar::Value &value) {
	std::array<uint8_t, 32> output{};
	curve().order.write(value, output.data(), output.size()); std::reverse(output.begin(), output.end());
	return output;
}

// Expand a seed into its clamped private scalar and per-message prefix.
std::array<uint8_t, 64> expand_seed(const uint8_t *seed) {
	std::array<uint8_t, 64> expanded{};
	Hash64 hash(Hash64::SHA512); hash.write(seed, 32); hash.sum(expanded.data());
	expanded[0] &= 248; expanded[31] = (expanded[31] & 63) | 64;
	GDCrypto::erase(&hash, sizeof(hash));
	return expanded;
}
}

// Derive the public encoding after copying all seed material needed for an in-place destination.
bool GDCrypto::ed25519_public(const uint8_t *seed, uint8_t *output) {
	if (!seed || !output) return false;
	auto expanded = expand_seed(seed);
	Point point = curve().times(curve().base, expanded.data());
	const auto public_key = encode(point); std::copy(public_key.begin(), public_key.end(), output);
	erase(expanded.data(), expanded.size()); erase(&point, sizeof(point));
	return true;
}

// Generate a deterministic signature with fixed-work scalar arithmetic and no external cryptographic calls.
bool GDCrypto::ed25519_sign(const uint8_t *seed, Bytes message, uint8_t *output) {
	if (!seed || !output || (!message.data && message.size)) return false;
	struct Work {
		std::array<uint8_t, 64> expanded{}, digest{};
		std::array<uint8_t, 32> nonce{};
		Scalar::Value r{}, k{}, s{};
		Point point;
		~Work() { erase(this, sizeof(*this)); }
	} work;
	work.expanded = expand_seed(seed);
	work.point = curve().times(curve().base, work.expanded.data());
	const auto public_key = encode(work.point);
	Hash64 hash(Hash64::SHA512); hash.write(work.expanded.data() + 32, 32); hash.write(message.data, message.size); hash.sum(work.digest.data());
	work.r = reduce_scalar(work.digest.data(), work.digest.size()); work.nonce = encode_scalar(work.r);
	work.point = curve().times(curve().base, work.nonce.data()); const auto r_point = encode(work.point);
	hash.reset(Hash64::SHA512); hash.write(r_point.data(), r_point.size()); hash.write(public_key.data(), public_key.size()); hash.write(message.data, message.size); hash.sum(work.digest.data());
	work.k = reduce_scalar(work.digest.data(), work.digest.size()); work.s = reduce_scalar(work.expanded.data(), 32);
	const auto &order = curve().order;
	work.s = order.add(work.r, order.leave(order.multiply(order.enter(work.k), order.enter(work.s))));
	const auto s_bytes = encode_scalar(work.s);
	std::copy(r_point.begin(), r_point.end(), output); std::copy(s_bytes.begin(), s_bytes.end(), output + 32);
	erase(&hash, sizeof(hash));
	return true;
}

// Recompute the canonical commitment from the uncofactored public verification equation.
bool GDCrypto::ed25519_verify(const uint8_t *peer, Bytes message, Bytes signature) {
	if (!peer || !signature.data || signature.size != 64 || (!message.data && message.size)) return false;
	Point public_point;
	if (!curve().decode(peer, public_point)) return false;
	std::array<uint8_t, 32> reversed{}; std::reverse_copy(signature.data + 32, signature.data + 64, reversed.data());
	Scalar::Value s{};
	if (!curve().order.read({reversed.data(), reversed.size()}, s)) return false;
	std::array<uint8_t, 64> digest{};
	Hash64 hash(Hash64::SHA512); hash.write(signature.data, 32); hash.write(peer, 32); hash.write(message.data, message.size); hash.sum(digest.data());
	const auto k = encode_scalar(reduce_scalar(digest.data(), digest.size()));
	public_point.x = subtract({}, public_point.x); public_point.t = subtract({}, public_point.t);
	const Point commitment = curve().plus(curve().times(curve().base, signature.data + 32), curve().times(public_point, k.data()));
	const auto encoded = encode(commitment);
	return equal(encoded.data(), signature.data, encoded.size());
}
