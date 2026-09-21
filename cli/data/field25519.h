// Share bounded prime-field arithmetic between agreement and signature operations.
#pragma once
#include "crypto_core.h"
#include <array>
#include <algorithm>

namespace GDCrypto::Field25519 {
constexpr uint64_t MASK = (uint64_t(1) << 51) - 1; // Five radix words encode the 255-bit field.
using Field = std::array<uint64_t, 5>;

// Accumulate products without requiring a compiler-specific integer representation.
struct Wide {
	uint64_t low = 0, high = 0; // Little-endian halves of an unsigned double-width word.
	// Add one product with carry into the upper half.
	void add(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__) && !defined(GD_CRYPTO_PORTABLE)
		const unsigned __int128 value = static_cast<unsigned __int128>(a) * b;
		const uint64_t before = low;
		low += uint64_t(value);
		high += uint64_t(value >> 64) + (low < before);
#else
		const uint64_t products[] = {uint64_t(uint32_t(a)) * uint32_t(b), uint64_t(uint32_t(a)) * (b >> 32), (a >> 32) * uint32_t(b), (a >> 32) * (b >> 32)};
		const uint64_t middle = (products[0] >> 32) + uint32_t(products[1]) + uint32_t(products[2]);
		const uint64_t bottom = (middle << 32) | uint32_t(products[0]), before = low;
		low += bottom;
		high += products[3] + (products[1] >> 32) + (products[2] >> 32) + (middle >> 32) + (low < before);
#endif
	}
	// Propagate a bounded carry without invoking signed arithmetic.
	void carry(uint64_t value) { const uint64_t before = low; low += value; high += low < before; }
	uint64_t upper() const { return (low >> 51) | (high << 13); } // Remove the low radix word.
};

// Reduce excess radix bits and select the unique representative below the prime.
inline Field reduce(Field value) {
	for (unsigned pass = 0; pass != 3; ++pass) {
		for (unsigned at = 0; at != 4; ++at) { value[at + 1] += value[at] >> 51; value[at] &= MASK; }
		value[0] += (value[4] >> 51) * 19;
		value[4] &= MASK;
	}
	Field subtracted = value;
	uint64_t carry = 19;
	for (unsigned at = 0; at != 5; ++at) {
		carry += value[at];
		subtracted[at] = carry & MASK;
		carry >>= 51;
	}
	const uint64_t select = uint64_t(0) - carry;
	for (unsigned at = 0; at != 5; ++at) value[at] ^= (value[at] ^ subtracted[at]) & select;
	return value;
}

// Multiply and fold high powers with the prime's defining relation.
inline Field multiply(const Field &a, const Field &b) {
	std::array<Wide, 5> sum{};
	for (unsigned left = 0; left != 5; ++left) for (unsigned right = 0; right != 5; ++right) {
		const unsigned power = left + right;
		sum[power % 5].add(a[left] * (power >= 5 ? 19 : 1), b[right]);
	}
	Field result{};
	for (unsigned at = 0; at != 4; ++at) { result[at] = sum[at].low & MASK; sum[at + 1].carry(sum[at].upper()); }
	result[4] = sum[4].low & MASK;
	result[0] += sum[4].upper() * 19;
	return reduce(result);
}

// Multiply by a public single-word coefficient without expanding zero limbs.
inline Field scale(const Field &value, uint32_t coefficient) {
	Field result{};
	uint64_t carry = 0;
	for (unsigned at = 0; at != 5; ++at) {
		Wide product;
		product.add(value[at], coefficient); product.carry(carry);
		result[at] = product.low & MASK; carry = product.upper();
	}
	result[0] += carry * 19;
	return reduce(result);
}

// Add canonical field elements and restore the radix bounds.
inline Field add(const Field &a, const Field &b) {
	Field out{};
	for (unsigned at = 0; at != 5; ++at) out[at] = a[at] + b[at];
	return reduce(out);
}

// Subtract with a positive multiple of the modulus, avoiding unsigned underflow.
inline Field subtract(const Field &a, const Field &b) {
	Field out{};
	for (unsigned at = 0; at != 5; ++at) out[at] = a[at] + 2 * (MASK - (at == 0 ? 18 : 0)) - b[at];
	return reduce(out);
}

// Raise to a public power of two using repeated squaring.
inline Field squares(Field value, unsigned count) {
	for (unsigned at = 0; at != count; ++at) value = multiply(value, value);
	return value;
}

// Build the shared long run of one-bits used by inverse and square-root exponents.
inline Field run250(const Field &value) {
	const Field power2 = squares(value, 1), power3 = multiply(power2, value);
	const Field run5 = multiply(squares(multiply(squares(power3, 2), power3), 1), value);
	const Field run10 = multiply(squares(run5, 5), run5), run20 = multiply(squares(run10, 10), run10);
	const Field run40 = multiply(squares(run20, 20), run20), run50 = multiply(squares(run40, 10), run10);
	const Field run100 = multiply(squares(run50, 50), run50), run200 = multiply(squares(run100, 100), run100);
	return multiply(squares(run200, 50), run50);
}

// Compute the inverse with a fixed addition chain for the public exponent p-2.
inline Field inverse(const Field &value) {
	const Field power2 = squares(value, 1), power3 = multiply(power2, value);
	return multiply(squares(run250(value), 5), multiply(squares(power2, 2), power3));
}

// Raise to the fixed exponent used for decoding square roots of coordinate ratios.
inline Field root_power(const Field &value) {
	return multiply(squares(run250(value), 2), value);
}

// Decode the coordinate modulo the prime while masking the reserved top bit.
inline Field decode(const uint8_t *bytes) {
	Field out{};
	for (unsigned bit = 0; bit != 255; ++bit) out[bit / 51] |= uint64_t((bytes[bit / 8] >> (bit % 8)) & 1) << (bit % 51);
	return reduce(out);
}

// Encode a canonical coordinate as an unaligned little-endian byte string.
inline std::array<uint8_t, 32> encode(const Field &value) {
	std::array<uint8_t, 32> out{};
	const Field canonical = reduce(value);
	for (unsigned bit = 0; bit != 255; ++bit) out[bit / 8] |= uint8_t(((canonical[bit / 51] >> (bit % 51)) & 1) << (bit % 8));
	return out;
}

// Exchange coordinates using a full-width mask, without secret-dependent addresses.
inline void exchange(Field &left, Field &right, uint64_t bit) {
	const uint64_t mask = uint64_t(0) - bit;
	for (unsigned at = 0; at != 5; ++at) {
		const uint64_t difference = (left[at] ^ right[at]) & mask;
		left[at] ^= difference; right[at] ^= difference;
	}
}
}
