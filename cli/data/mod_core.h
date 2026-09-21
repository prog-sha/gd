// Compute modular arithmetic with word-oriented reduction and fixed secret-bit control flow.
#pragma once
#include "crypto_core.h"
#include "der_core.h"
#include "crypto_storage.h"
#include <array>
#include <algorithm>

namespace GDCrypto {
// Own modulus-dependent arithmetic state; storage capacity is selected by the consuming protocol.
template <size_t Words> class Mod {
public:
	using Value = CryptoStorage<uint32_t, Words>; // Little-endian words; unused capacity must remain zero.
private:
	using Product = CryptoStorage<uint32_t, Words ? Words * 2 + 2 : 0>; // Full integer products including reduction carry words.
	Value modulus{}, squared{}; // Odd modulus and squared radix residue.
	size_t used = 0, width = 0; // Active public word and byte widths.
	uint32_t inverse = 0; // Negative inverse of the low modulus word modulo one radix word.
	// Multiply full-width integers before any modular reduction, preserving all carry words.
	void product(const Value &left, const Value &right, Product &output) const {
		std::fill(output.begin(), output.end(), uint32_t(0));
		for (size_t row = 0; row != used; ++row) {
			uint64_t carry = 0;
			for (size_t col = 0; col != used; ++col) {
				const uint64_t total = uint64_t(left[row]) * right[col] + output[row + col] + carry;
				output[row + col] = uint32_t(total); carry = total >> 32;
			}
			output[row + used] = uint32_t(carry);
		}
	}
	// Select the reduced candidate from an input known to be below twice the modulus.
	void reduce(const uint32_t *input, uint32_t high, Value &out) const {
		uint64_t borrow = 0;
		for (size_t at = 0; at != used; ++at) {
			const uint64_t word = uint64_t(input[at]) - modulus[at] - borrow;
			out[at] = uint32_t(word); borrow = word >> 63;
		}
		const uint32_t select = opaque_mask(0U - uint32_t((high != 0) | (borrow == 0)));
		for (size_t at = 0; at != used; ++at) out[at] = input[at] ^ ((input[at] ^ out[at]) & select);
	}
	// Add into reusable disjoint scratch before reducing, allowing the result to replace either operand.
	void add_to(const Value &left, const Value &right, Value &output, Value &scratch) const {
		uint64_t carry = 0;
		for (size_t at = 0; at != used; ++at) { carry += uint64_t(left[at]) + right[at]; scratch[at] = uint32_t(carry); carry >>= 32; }
		reduce(scratch.data(), uint32_t(carry), output);
	}
	// Multiply through caller-owned scratch so repeated exponent operations need no new allocations.
	void multiply_to(const Value &left, const Value &right, Value &output, Product &scratch) const {
		product(left, right, scratch);
		for (size_t row = 0; row != used; ++row) {
			const uint32_t factor = scratch[row] * inverse;
			uint64_t carry = 0;
			for (size_t col = 0; col != used; ++col) {
				const uint64_t total = uint64_t(factor) * modulus[col] + scratch[row + col] + carry;
				scratch[row + col] = uint32_t(total); carry = total >> 32;
			}
			for (size_t at = row + used; at <= used * 2; ++at) { carry += scratch[at]; scratch[at] = uint32_t(carry); carry >>= 32; }
		}
		reduce(scratch.data() + used, scratch[used * 2], output);
	}
public:
	// Erase modulus-dependent storage, including private factors when used by a signer.
	~Mod() { wipe_storage(modulus); wipe_storage(squared); inverse = 0; }
	size_t size() const { return width; } // Return the canonical modulus byte width.
	Value zero() const { return crypto_storage<uint32_t, Words>(used); } // Return a zero residue with this modulus's announced storage width.
	// Return the modulus bit width without examining secret exponent values.
	size_t bits() const {
		if (!used) return 0;
		uint32_t top = modulus[used - 1];
		size_t count = (used - 1) * 32;
		while (top) { ++count; top >>= 1; }
		return count;
	}
	const Value &value() const { return modulus; } // Expose the modulus to protocol-level validation.
	// Require exact integer factorization rather than equality only modulo the public modulus.
	bool is_product(const Value &left, const Value &right) const {
		auto full = crypto_storage<uint32_t, Words ? Words * 2 + 2 : 0>(used * 2 + 2);
		product(left, right, full);
		uint32_t difference = 0;
		for (size_t at = 0; at != used; ++at) difference |= full[at] ^ modulus[at];
		for (size_t at = used; at != full.size(); ++at) difference |= full[at];
		wipe_storage(full);
		return difference == 0;
	}
	// Initialize an odd modulus and precompute the radix conversion residue.
	bool reset(Bytes input) {
		wipe_storage(modulus); wipe_storage(squared); used = width = 0; inverse = 0;
		if (!input.data && input.size) return false;
		while (input.size && !input.data[0]) { ++input.data; --input.size; }
		if (!input.size || (Words && input.size > Words * 4) || !(input.data[input.size - 1] & 1) || (input.size == 1 && input.data[0] < 3)) return false;
		width = input.size; used = width / 4 + (width % 4 != 0);
		modulus = zero(); squared = zero();
		for (size_t at = 0; at != input.size; ++at) modulus[at / 4] |= uint32_t(input.data[input.size - 1 - at]) << ((at % 4) * 8);
		uint32_t reciprocal = 1;
		for (unsigned step = 0; step != 5; ++step) reciprocal *= 2U - modulus[0] * reciprocal;
		inverse = 0U - reciprocal;
		squared[0] = 1;
		Value scratch = zero();
		for (size_t bit = 0; bit != used * 64; ++bit) add_to(squared, squared, squared, scratch);
		wipe_storage(scratch);
		return true;
	}
	// Decode an ordinary nonnegative value strictly below the initialized modulus.
	bool read(Bytes input, Value &output) const {
		if (!used || (!input.data && input.size)) return false;
		if (input.size > width) {
			uint8_t extra = 0;
			const size_t count = input.size - width;
			for (size_t at = 0; at != count; ++at) extra |= input.data[at];
			if (extra) return false;
			input.data += count; input.size = width;
		}
		Value out = zero();
		for (size_t at = 0; at != input.size; ++at) out[at / 4] |= uint32_t(input.data[input.size - 1 - at]) << ((at % 4) * 8);
		uint64_t borrow = 0;
		for (size_t at = 0; at != used; ++at) borrow = (uint64_t(out[at]) - modulus[at] - borrow) >> 63;
		if (!borrow) return false;
		output = out;
		return true;
	}
	// Serialize an ordinary canonical value into a caller-provided fixed-width destination.
	bool write(const Value &input, uint8_t *output, size_t size) const {
		if (!used || !output || size < width) return false;
		std::fill_n(output, size, uint8_t(0));
		for (size_t at = 0; at != width; ++at) output[size - 1 - at] = uint8_t(input[at / 4] >> ((at % 4) * 8));
		return true;
	}
	// Add canonical residues with one conditional modulus subtraction.
	Value add(const Value &left, const Value &right) const {
		Value result = zero(), scratch = zero();
		add_to(left, right, result, scratch); wipe_storage(scratch);
		return result;
	}
	// Subtract canonical residues with a masked modulus addition on underflow.
	Value subtract(const Value &left, const Value &right) const {
		Value result = zero();
		uint64_t borrow = 0;
		for (size_t at = 0; at != used; ++at) {
			const uint64_t word = uint64_t(left[at]) - right[at] - borrow;
			result[at] = uint32_t(word); borrow = word >> 63;
		}
		const uint32_t selected = opaque_mask(0U - uint32_t(borrow));
		uint64_t carry = 0;
		for (size_t at = 0; at != used; ++at) { carry += uint64_t(result[at]) + (modulus[at] & selected); result[at] = uint32_t(carry); carry >>= 32; }
		return result;
	}
	// Multiply canonical radix residues and cancel low words without division.
	Value multiply(const Value &left, const Value &right) const {
		Value result = zero();
		auto scratch = crypto_storage<uint32_t, Words ? Words * 2 + 2 : 0>(used * 2 + 2);
		multiply_to(left, right, result, scratch); wipe_storage(scratch);
		return result;
	}
	// Convert an ordinary value to radix form, extending shorter factor residues only by their announced width.
	Value enter(const Value &input) const {
		if constexpr (Words == 0) if (input.size() < used) {
			Value padded = zero(); std::copy(input.begin(), input.end(), padded.begin()); return multiply(padded, squared);
		}
		return multiply(input, squared);
	}
	Value leave(const Value &input) const { Value unit = zero(); if (used) unit[0] = 1; return multiply(input, unit); } // Convert a radix residue to its ordinary value.
	Value one() const { Value unit = zero(); if (used) unit[0] = 1; return enter(unit); } // Return the multiplicative identity in radix form.
	// Reduce an arbitrary-width byte string with fixed work per input bit.
	Value remainder(Bytes input) const {
		Value result = zero(), digit = zero(), scratch = zero();
		if (!used) return result;
		for (size_t at = 0; at != input.size; ++at) for (unsigned bit = 8; bit != 0; --bit) {
			add_to(result, result, result, scratch);
			digit[0] = (input.data[at] >> (bit - 1)) & 1;
			add_to(result, digit, result, scratch);
		}
		wipe_storage(digit); wipe_storage(scratch);
		return result;
	}
	// Reduce a private exponent modulo one less than an odd factor without requiring an odd reduction modulus.
	Value previous_remainder(Bytes input) const {
		if (!used) return zero();
		Mod divisor = *this;
		--divisor.modulus[0];
		return divisor.remainder(input);
	}
	// Raise a radix residue with fixed work and full-table selection for each secret nibble.
	Value power(const Value &base, Bytes exponent) const {
		Value result = one(), candidate = zero();
		auto scratch = crypto_storage<uint32_t, Words ? Words * 2 + 2 : 0>(used * 2 + 2);
		std::array<Value, 15> table{}; // Nonzero powers for one four-bit exponent digit.
		for (auto &value : table) value = zero();
		table[0] = base;
		for (size_t at = 1; at != table.size(); ++at) multiply_to(table[at - 1], base, table[at], scratch);
		for (size_t at = 0; at != exponent.size; ++at) for (unsigned shift : {4U, 0U}) {
			for (unsigned bit = 0; bit != 4; ++bit) multiply_to(result, result, result, scratch);
			const uint32_t digit = (exponent.data[at] >> shift) & 15U;
			// Scan every entry so secret digits never select a memory address.
			for (size_t entry = 0; entry != table.size(); ++entry) {
				const uint32_t difference = digit ^ uint32_t(entry + 1);
				const uint32_t selected = opaque_mask(0U - ((difference - 1U) >> 31));
				for (size_t word = 0; word != used; ++word) candidate[word] ^= (candidate[word] ^ table[entry][word]) & selected;
			}
			multiply_to(result, candidate, candidate, scratch);
			const uint32_t selected = opaque_mask(0U - ((digit + 15U) >> 4));
			for (size_t word = 0; word != used; ++word) result[word] ^= (result[word] ^ candidate[word]) & selected;
		}
		for (auto &value : table) wipe_storage(value);
		wipe_storage(candidate); wipe_storage(scratch);
		return result;
	}
	// Skip multiplication for zero bits only when the caller guarantees a public exponent.
	Value public_power(const Value &base, Bytes exponent) const {
		Value result = one();
		auto scratch = crypto_storage<uint32_t, Words ? Words * 2 + 2 : 0>(used * 2 + 2);
		for (size_t at = 0; at != exponent.size; ++at) for (unsigned bit = 8; bit != 0; --bit) {
			multiply_to(result, result, result, scratch);
			if ((exponent.data[at] >> (bit - 1)) & 1) multiply_to(result, base, result, scratch);
		}
		wipe_storage(scratch);
		return result;
	}
};
}
