// Perform prime-field ladder arithmetic with bounded limbs and secret-independent control flow.
#include "x25519_core.h"
#include "field25519.h"
#include <array>
#include <algorithm>

using namespace GDCrypto::Field25519;

// Advance a fixed-length differential ladder and reject an all-zero shared coordinate.
bool GDCrypto::x25519(const uint8_t *secret, const uint8_t *peer, uint8_t *output) {
	if (!secret || !peer || !output) return false;
	std::array<uint8_t, 32> scalar{};
	std::copy_n(secret, scalar.size(), scalar.data());
	scalar[0] &= 248; scalar[31] = (scalar[31] & 127) | 64;
	const Field base = decode(peer);
	Field x0 = {1, 0, 0, 0, 0}, z0{}, x1 = base, z1 = {1, 0, 0, 0, 0};
	for (int bit = 254; bit >= 0; --bit) {
		const uint64_t selected = (scalar[unsigned(bit) / 8] >> (unsigned(bit) % 8)) & 1;
		exchange(x0, x1, selected); exchange(z0, z1, selected);
		const Field plus = add(x0, z0), minus = subtract(x0, z0), plus2 = squares(plus, 1), minus2 = squares(minus, 1);
		const Field cross0 = multiply(subtract(x1, z1), plus), cross1 = multiply(add(x1, z1), minus), difference = subtract(plus2, minus2);
		x1 = squares(add(cross0, cross1), 1); z1 = multiply(base, squares(subtract(cross0, cross1), 1));
		x0 = multiply(plus2, minus2); z0 = multiply(difference, add(plus2, scale(difference, 121665)));
		exchange(x0, x1, selected); exchange(z0, z1, selected);
	}
	auto shared = encode(multiply(x0, inverse(z0)));
	const std::array<uint8_t, 32> zero{};
	const bool valid = !equal(shared.data(), zero.data(), shared.size());
	if (valid) std::copy(shared.begin(), shared.end(), output);
	erase(scalar.data(), scalar.size()); erase(shared.data(), shared.size());
	erase(x0.data(), sizeof(x0)); erase(z0.data(), sizeof(z0)); erase(x1.data(), sizeof(x1)); erase(z1.data(), sizeof(z1));
	return valid;
}

// Derive the public coordinate by multiplying the fixed base point.
bool GDCrypto::x25519_public(const uint8_t *secret, uint8_t *output) {
	const std::array<uint8_t, 32> base = {9};
	return x25519(secret, base.data(), output);
}
