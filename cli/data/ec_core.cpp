// Compute prime-curve agreement and signature verification with checked point encodings and masked arithmetic.
#include "ec_core.h"
#include "hash_core.h"
#include "nonce_core.h"
#include <cstring>

namespace {
// Describe the mathematical domain parameters for supported prime curves.
struct Parameters {
	unsigned bits; // Field size identifying the named curve.
	const char *prime, *order, *b, *x, *y; // Canonical hexadecimal domain values; a null prime denotes the Mersenne field.
};
constexpr Parameters PARAMETERS[] = { // Standard domain values, not implementation-specific operating limits.
	{224,
		"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF000000000000000000000001",
		"FFFFFFFFFFFFFFFFFFFFFFFFFFFF16A2E0B8F03E13DD29455C5C2A3D",
		"B4050A850C04B3ABF54132565044B0B7D7BFD8BA270B39432355FFB4",
		"B70E0CBD6BB4BF7F321390B94A03C1D356C21122343280D6115C1D21",
		"BD376388B5F723FB4C22DFE6CD4375A05A07476444D5819985007E34"},
	{256,
		"FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF",
		"FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551",
		"5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B",
		"6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296",
		"4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5"},
	{384,
		"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFFFF0000000000000000FFFFFFFF",
		"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF581A0DB248B0A77AECEC196ACCC52973",
		"B3312FA7E23EE7E4988E056BE3F82D19181D9C6EFE8141120314088F5013875AC656398D8A2ED19D2A85C8EDD3EC2AEF",
		"AA87CA22BE8B05378EB1C71EF320AD746E1D3B628BA79B9859F741E082542A385502F25DBF55296C3A545E3872760AB7",
		"3617DE4A96262C6F5D9E98BF9292DC29F8F41DBD289A147CE9DA3113B5F0B8C00A60B1CE1D7E819D7A431D7C90EA0E5F"},
	{521, nullptr,
		"01FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFA51868783BF2F966B7FCC0148F709A5D03BB5C9B8899C47AEBB6FB71E91386409",
		"0051953EB9618E1C9A1F929A21A0B68540EEA2DA725B99B315F3B8B489918EF109E156193951EC7E937B1652C0BD3BB1BF073573DF883D2C34F1EF451FD46B503F00",
		"00C6858E06B70404E9CD9E3ECB662395B4429C648139053FB521F828AF606B4D3DBAA14B5E77EFE75928FE1DC127A2FFA8DE3348B3C1856A429BF97E7E31C2E5BD66",
		"011839296A789A3BC0045C8A5FB42C7D1BD998F54449579B446817AFBD17273E662C97EE72995EF42640C550B9013FAD0761353C7086A272C24088BE94769FD16650"}
};

// Decode a complete mathematical constant with an exact expected byte width.
bool constant(const char *text, uint8_t *output, size_t size) {
	if (!text || std::strlen(text) != size * 2) return false;
	for (size_t at = 0; at != size; ++at) {
		const unsigned left = text[at * 2] <= '9' ? unsigned(text[at * 2] - '0') : unsigned(text[at * 2] - 'A' + 10);
		const unsigned right = text[at * 2 + 1] <= '9' ? unsigned(text[at * 2 + 1] - '0') : unsigned(text[at * 2 + 1] - 'A' + 10);
		if (left > 15 || right > 15) return false;
		output[at] = uint8_t(left * 16 + right);
	}
	return true;
}

// Detect the zero field element without an early exit.
uint32_t zero(const GDCrypto::Mod<17>::Value &value) {
	uint32_t combined = 0;
	for (uint32_t word : value) combined |= word;
	return uint32_t(combined == 0);
}

// Encode one public signature integer with only its necessary sign-padding byte.
size_t integer(const uint8_t *bytes, size_t size, uint8_t *output) {
	while (size > 1 && !bytes[0]) { ++bytes; --size; }
	const size_t padding = (bytes[0] >> 7) & 1;
	output[0] = 2; output[1] = uint8_t(size + padding);
	if (padding) output[2] = 0;
	std::copy_n(bytes, size, output + 2 + padding);
	return size + padding + 2;
}
}

// Select and validate immutable field, subgroup, and generator parameters.
bool GDCrypto::PrimeCurve::reset(unsigned bits) {
	ready = false;
	const Parameters *selected = nullptr;
	for (const auto &candidate : PARAMETERS) if (candidate.bits == bits) selected = &candidate;
	if (!selected) return false;
	const size_t width = (bits + 7) / 8;
	std::array<uint8_t, 66> prime{}, subgroup{}, buffer{};
	if (selected->prime) { if (!constant(selected->prime, prime.data(), width)) return false; }
	else { prime.fill(255); prime[0] = 1; }
	if (!constant(selected->order, subgroup.data(), width) || !field.reset({prime.data(), width}) || !order.reset({subgroup.data(), width})) return false;
	field_exp = prime; order_exp = subgroup;
	for (auto *exponent : {&field_exp, &order_exp}) {
		unsigned borrow = 2;
		for (size_t at = width; at != 0; --at) { const uint16_t value = uint16_t(unsigned((*exponent)[at - 1]) - borrow); (*exponent)[at - 1] = uint8_t(value); borrow = value >> 15; }
	}
	Field decoded;
	if (!constant(selected->b, buffer.data(), width) || !field.read({buffer.data(), width}, decoded)) return false;
	coefficient = field.enter(decoded);
	if (!constant(selected->x, buffer.data(), width) || !field.read({buffer.data(), width}, decoded)) return false;
	base.x = field.enter(decoded);
	if (!constant(selected->y, buffer.data(), width) || !field.read({buffer.data(), width}, decoded)) return false;
	base.y = field.enter(decoded); base.z = field.one();
	ready = true;
	return true;
}

// Select all coordinate words with an optimization-opaque mask.
GDCrypto::PrimeCurve::Point GDCrypto::PrimeCurve::choose(Point left, const Point &right, uint32_t bit) {
	const uint32_t mask = opaque_mask(0U - bit);
	for (size_t at = 0; at != left.x.size(); ++at) {
		left.x[at] ^= (left.x[at] ^ right.x[at]) & mask;
		left.y[at] ^= (left.y[at] ^ right.y[at]) & mask;
		left.z[at] ^= (left.z[at] ^ right.z[at]) & mask;
	}
	return left;
}

// Double a Jacobian point on a curve with linear coefficient minus three.
GDCrypto::PrimeCurve::Point GDCrypto::PrimeCurve::twice(const Point &point) const {
	const Field delta = field.multiply(point.z, point.z), gamma = field.multiply(point.y, point.y), beta = field.multiply(point.x, gamma);
	Field alpha = field.multiply(field.subtract(point.x, delta), field.add(point.x, delta));
	alpha = field.add(alpha, field.add(alpha, alpha));
	const Field beta4 = field.add(field.add(beta, beta), field.add(beta, beta));
	const Field gamma2 = field.multiply(gamma, gamma), gamma4 = field.add(gamma2, gamma2), gamma8 = field.add(gamma4, gamma4);
	Point out;
	out.x = field.subtract(field.multiply(alpha, alpha), field.add(beta4, beta4));
	out.y = field.subtract(field.multiply(alpha, field.subtract(beta4, out.x)), field.add(gamma8, gamma8));
	const Field sum = field.add(point.y, point.z);
	out.z = field.subtract(field.subtract(field.multiply(sum, sum), gamma), delta);
	return out;
}

// Add arbitrary Jacobian points with fixed arithmetic and mask exceptional cases.
GDCrypto::PrimeCurve::Point GDCrypto::PrimeCurve::add(const Point &left, const Point &right) const {
	const Field zleft = field.multiply(left.z, left.z), zright = field.multiply(right.z, right.z);
	const Field uleft = field.multiply(left.x, zright), uright = field.multiply(right.x, zleft);
	const Field sleft = field.multiply(left.y, field.multiply(zright, right.z)), sright = field.multiply(right.y, field.multiply(zleft, left.z));
	const Field h = field.subtract(uright, uleft), r = field.subtract(sright, sleft), h2 = field.multiply(h, h), h3 = field.multiply(h2, h);
	const Field term = field.multiply(uleft, h2);
	Point out;
	out.x = field.subtract(field.subtract(field.multiply(r, r), h3), field.add(term, term));
	out.y = field.subtract(field.multiply(r, field.subtract(term, out.x)), field.multiply(sleft, h3));
	out.z = field.multiply(h, field.multiply(left.z, right.z));
	out = choose(out, twice(left), zero(h) & zero(r));
	out = choose(out, right, zero(left.z));
	return choose(out, left, zero(right.z));
}

// Multiply a finite point with one doubling and one complete addition per scalar bit.
GDCrypto::PrimeCurve::Point GDCrypto::PrimeCurve::multiply(const Point &point, Bytes scalar) const {
	Point result;
	for (size_t at = 0; at != scalar.size; ++at) for (unsigned bit = 8; bit != 0; --bit) {
		result = twice(result);
		result = choose(result, add(result, point), (scalar.data[at] >> (bit - 1)) & 1);
	}
	return result;
}

// Require a finite uncompressed point whose coordinates satisfy the selected curve equation.
bool GDCrypto::PrimeCurve::decode(Bytes encoded, Point &point) const {
	const size_t width = size();
	if (!ready || !encoded.data || encoded.size != 1 + width * 2 || encoded.data[0] != 4) return false;
	Field x, y;
	if (!field.read({encoded.data + 1, width}, x) || !field.read({encoded.data + 1 + width, width}, y)) return false;
	x = field.enter(x); y = field.enter(y);
	const Field cube = field.multiply(field.multiply(x, x), x), triple = field.add(x, field.add(x, x));
	if (field.multiply(y, y) != field.add(field.subtract(cube, triple), coefficient)) return false;
	point = {x, y, field.one()};
	return true;
}

// Normalize coordinates with an inverse computed using the fixed public field exponent.
bool GDCrypto::PrimeCurve::affine(Point &point) const {
	if (zero(point.z)) return false;
	const Field inverse = field.public_power(point.z, {field_exp.data(), size()}), square = field.multiply(inverse, inverse);
	point.x = field.leave(field.multiply(point.x, square));
	point.y = field.leave(field.multiply(point.y, field.multiply(square, inverse)));
	point.z = {};
	return true;
}

// Validate exact-width private scalars before beginning group arithmetic.
bool GDCrypto::PrimeCurve::scalar(Bytes input) const {
	Field value;
	const bool valid = ready && input.size == size() && order.read(input, value) && !zero(value);
	erase(value.data(), sizeof(value));
	return valid;
}

// Publish a complete uncompressed key only after successful scalar multiplication.
bool GDCrypto::PrimeCurve::public_key(Bytes secret, uint8_t *output, size_t count) const {
	if (!output || count != 1 + size() * 2 || !scalar(secret)) return false;
	Point point = multiply(base, secret);
	if (!affine(point)) return false;
	std::array<uint8_t, 133> encoded{};
	encoded[0] = 4;
	field.write(point.x, encoded.data() + 1, size()); field.write(point.y, encoded.data() + 1 + size(), size());
	std::copy_n(encoded.data(), count, output);
	erase(&point, sizeof(point));
	return true;
}

// Validate peer membership and reveal only the final fixed-width shared coordinate.
bool GDCrypto::PrimeCurve::shared(Bytes secret, Bytes peer, uint8_t *output, size_t count) const {
	Point point;
	if (!output || count != size() || !scalar(secret) || !decode(peer, point)) return false;
	point = multiply(point, secret);
	if (!affine(point)) return false;
	field.write(point.x, output, count);
	erase(&point, sizeof(point));
	return true;
}

// Verify a canonical signature pair using a joint multiplication of public scalars.
bool GDCrypto::PrimeCurve::verify(Bytes peer, Bytes digest, Bytes signature) const {
	Point public_point;
	if (!decode(peer, public_point) || (!digest.data && digest.size)) return false;
	DER outer(signature);
	Bytes sequence, rbytes, sbytes;
	if (!outer.take(0x30, sequence) || !outer.empty()) return false;
	DER pair(sequence);
	Field r, s;
	if (!pair.integer(rbytes) || !pair.integer(sbytes) || !pair.empty() || !order.read(rbytes, r) || !order.read(sbytes, s) || zero(r) || zero(s)) return false;
	std::array<uint8_t, 66> left{}, right{};
	const Field inverse = order.public_power(order.enter(s), {order_exp.data(), size()});
	const Field a = order.leave(order.multiply(order.enter(hash_value(digest)), inverse)), b = order.leave(order.multiply(order.enter(r), inverse));
	order.write(a, left.data(), size()); order.write(b, right.data(), size());
	Point result;
	for (size_t at = 0; at != size(); ++at) for (unsigned bit = 8; bit != 0; --bit) {
		result = twice(result);
		if ((left[at] >> (bit - 1)) & 1) result = add(result, base);
		if ((right[at] >> (bit - 1)) & 1) result = add(result, public_point);
	}
	if (!affine(result)) return false;
	field.write(result.x, left.data(), size());
	return order.remainder({left.data(), size()}) == r;
}

// Convert a digest to a scalar using the subgroup's bit width rather than its byte width.
GDCrypto::PrimeCurve::Field GDCrypto::PrimeCurve::hash_value(Bytes digest) const {
	std::array<uint8_t, 66> truncated{};
	const size_t length = std::min(digest.size, size());
	if (length) std::copy_n(digest.data, length, truncated.data());
	if (length * 8 > order.bits()) {
		const unsigned extra = unsigned(length * 8 - order.bits());
		uint8_t preceding = 0;
		for (size_t at = 0; at != length; ++at) { const uint8_t byte = truncated[at]; truncated[at] = uint8_t((byte >> extra) | (unsigned(preceding) << (8 - extra))); preceding = byte; }
	}
	return order.remainder({truncated.data(), length});
}

// Hedge nonce generation with private message material and publish only a complete signature.
bool GDCrypto::PrimeCurve::sign(Bytes secret, Bytes digest, uint8_t *output, size_t capacity, size_t &written) const {
	if (!output || !scalar(secret) || (!digest.data && digest.size)) return false;
	// Erase owned nonce and scalar state on every exit, including entropy and output failures.
	struct Secrets {
		Field d{}, k{}, inverse{};
		std::array<uint8_t, 66> bytes{};
		~Secrets() { erase(d.data(), sizeof(d)); erase(k.data(), sizeof(k)); erase(inverse.data(), sizeof(inverse)); erase(bytes.data(), bytes.size()); }
	} values;
	order.read(secret, values.d); values.d = order.enter(values.d);
	const Field reduced = hash_value(digest), digest_value = order.enter(reduced);
	std::array<uint8_t, 66> message{};
	order.write(reduced, message.data(), size());
	if (!random_fill(values.bytes.data(), size())) return false;
	HedgedNonce nonce({values.bytes.data(), size()}, secret, {message.data(), size()});
	for (;;) {
		nonce.read(values.bytes.data(), size());
		const unsigned extra = unsigned(size() * 8 - order.bits());
		if (extra) {
			uint8_t previous = 0;
			for (size_t at = 0; at != size(); ++at) { const uint8_t byte = values.bytes[at]; values.bytes[at] = uint8_t((byte >> extra) | (unsigned(previous) << (8 - extra))); previous = byte; }
		}
		if (!order.read({values.bytes.data(), size()}, values.k) || zero(values.k)) continue;
		Point point = multiply(base, {values.bytes.data(), size()});
		if (!affine(point)) return false;
		std::array<uint8_t, 66> rbytes{}, sbytes{};
		field.write(point.x, rbytes.data(), size()); erase(&point, sizeof(point));
		const Field r = order.remainder({rbytes.data(), size()});
		if (zero(r)) continue;
		values.inverse = order.public_power(order.enter(values.k), {order_exp.data(), size()});
		const Field s = order.leave(order.multiply(values.inverse, order.add(digest_value, order.multiply(order.enter(r), values.d))));
		if (zero(s)) continue;
		order.write(r, rbytes.data(), size()); order.write(s, sbytes.data(), size());
		std::array<uint8_t, 141> encoded{}; // Two signed 521-bit integers and their enclosing sequence headers.
		const size_t first = integer(rbytes.data(), size(), encoded.data() + 3);
		const size_t body = first + integer(sbytes.data(), size(), encoded.data() + 3 + first);
		const size_t header = body < 128 ? 2 : 3, total = header + body;
		if (capacity < total) return false;
		output[0] = 0x30;
		if (header == 3) { output[1] = 0x81; output[2] = uint8_t(body); }
		else output[1] = uint8_t(body);
		std::copy_n(encoded.data() + 3, body, output + header);
		written = total;
		return true;
	}
}
