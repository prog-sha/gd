// Sign encoded messages with validated private factors, fixed-work exponents, and public recovery checks.
#pragma once
#include "rsa_core.h"

namespace GDCrypto {
// Retain validated key precomputation without retaining borrowed key bytes.
template <size_t Words> class RSAPrivate {
	using Arithmetic = Mod<Words>;
	using Value = typename Arithmetic::Value;
	using BytesBuffer = CryptoStorage<uint8_t, Words * 4>; // Fixed or key-sized secret byte storage.
	Arithmetic modulus, first, second; // Public modulus and optional private factor moduli.
	BytesBuffer secret{}, left_exp{}, right_exp{}; // Exponents padded to their corresponding modulus widths.
	std::array<uint8_t, 4> exponent{}; // Public recovery exponent.
	Value inverse{}, right_factor{}; // Inverse second factor modulo the first, and second factor modulo the full modulus.
	bool ready = false, crt = false; // Validated-key and factor-acceleration states.
	// Detect nonzero key values without a data-dependent early return.
	static bool nonzero(const Value &value) {
		uint32_t combined = 0;
		for (uint32_t word : value) combined |= word;
		return combined != 0;
	}
	// Clear owned key material before replacement and destruction.
	void clear() {
		ready = crt = false;
		erase(secret.data(), secret.size()); erase(left_exp.data(), left_exp.size()); erase(right_exp.data(), right_exp.size());
		wipe_storage(inverse); wipe_storage(right_factor);
		modulus.reset({}); first.reset({}); second.reset({}); exponent.fill(0);
	}
	// Establish fixed-width private and public exponents without trusting any precomputed factors.
	bool base(Bytes n, uint32_t e, Bytes d) {
		clear();
		if (e < 2 || e > INT32_MAX || !modulus.reset(n) || modulus.bits() < 1024) return false;
		Value decoded{};
		const bool valid = modulus.read(d, decoded) && nonzero(decoded);
		secret = crypto_storage<uint8_t, Words * 4>(modulus.size());
		if (valid) modulus.write(decoded, secret.data(), modulus.size());
		wipe_storage(decoded);
		if (!valid) return false;
		for (unsigned at = 0; at != 4; ++at) exponent[at] = uint8_t(e >> (24 - at * 8));
		return true;
	}
	// Check the private exponent and public inverse relation modulo a factor's multiplicative-group order.
	bool factor(const Arithmetic &prime, Bytes encoded, Bytes d, uint32_t e, uint8_t *output) const {
		Value value{};
		if (!prime.read(encoded, value)) return false;
		const Value reduced = prime.previous_remainder(d);
		if (!equal(value.data(), reduced.data(), value.size() * sizeof(uint32_t))) { wipe_storage(value); return false; }
		prime.write(value, output, prime.size()); wipe_storage(value);
		auto product = crypto_storage<uint8_t, Words ? Words * 4 + 4 : 0>(prime.size() + 4);
		uint64_t carry = 0;
		for (size_t at = prime.size(); at != 0; --at) { carry += uint64_t(output[at - 1]) * e; product[at + 3] = uint8_t(carry); carry >>= 8; }
		for (unsigned at = 4; at != 0; --at) { product[at - 1] = uint8_t(carry); carry >>= 8; }
		Value checked = prime.previous_remainder({product.data(), prime.size() + 4});
		checked[0] ^= 1;
		const bool valid = !nonzero(checked);
		wipe_storage(product); wipe_storage(checked);
		return valid;
	}
	// Recover every private result with the public exponent before making it observable.
	bool apply(Bytes input, uint8_t *output, size_t size) const {
		if (!ready || !output || size != modulus.size() || input.size != size) return false;
		struct Work {
			Value message{}, left{}, right{}, result{}, check{};
			BytesBuffer bytes;
			explicit Work(size_t count) : bytes(crypto_storage<uint8_t, Words * 4>(count)) {} // Match scratch width to the installed public modulus.
			~Work() { wipe_storage(message); wipe_storage(left); wipe_storage(right); wipe_storage(result); wipe_storage(check); wipe_storage(bytes); } // Erase backing allocations without corrupting container lifetimes.
		} work(modulus.size());
		if (!modulus.read(input, work.message)) return false;
		if (crt) {
			work.left = first.power(first.enter(first.remainder(input)), {left_exp.data(), first.size()});
			work.right = second.leave(second.power(second.enter(second.remainder(input)), {right_exp.data(), second.size()}));
			second.write(work.right, work.bytes.data(), second.size());
			work.left = first.leave(first.multiply(inverse, first.subtract(work.left, first.enter(first.remainder({work.bytes.data(), second.size()})))));
			work.result = modulus.add(modulus.multiply(modulus.enter(work.left), right_factor), modulus.enter(work.right));
		} else work.result = modulus.power(modulus.enter(work.message), {secret.data(), modulus.size()});
		work.check = modulus.leave(modulus.public_power(work.result, {exponent.data(), exponent.size()}));
		if (!equal(work.check.data(), work.message.data(), work.message.size() * sizeof(uint32_t))) return false;
		work.result = modulus.leave(work.result);
		return modulus.write(work.result, output, size);
	}
public:
	~RSAPrivate() { clear(); } // Erase key-owned state while its object lifetime remains valid.
	RSAPrivate() = default; // Start without a usable private key.
	RSAPrivate(const RSAPrivate &) = delete; // Keep private state under one lifetime.
	RSAPrivate &operator=(const RSAPrivate &) = delete; // Reject implicit key duplication.
	size_t size() const { return ready ? modulus.size() : 0; } // Return the exact encoded signature width.
	// Load an unfactored exponent for keys whose factor format is handled by the containing parser.
	bool reset(Bytes n, uint32_t e, Bytes d) { ready = base(n, e, d); return ready; }
	// Validate all factor relationships before enabling accelerated private operations.
	bool reset(Bytes n, uint32_t e, Bytes d, Bytes p, Bytes q, Bytes dp, Bytes dq, Bytes coefficient) {
		if (!base(n, e, d) || !first.reset(p) || !second.reset(q)) return false;
		Value left{}, right{}, inv{};
		const bool decoded = modulus.read(p, left) && modulus.read(q, right) && first.read(coefficient, inv);
		if (!decoded) { wipe_storage(left); wipe_storage(right); wipe_storage(inv); return false; }
		right_factor = modulus.enter(right);
		const bool factors = modulus.is_product(left, right);
		wipe_storage(left); wipe_storage(right);
		inverse = first.enter(inv); wipe_storage(inv);
		Value checked = first.leave(first.multiply(inverse, first.enter(first.remainder(q)))); checked[0] ^= 1;
		const bool coefficient_valid = !nonzero(checked); wipe_storage(checked);
		left_exp = crypto_storage<uint8_t, Words * 4>(first.size()); right_exp = crypto_storage<uint8_t, Words * 4>(second.size());
		if (!factors || !coefficient_valid || !factor(first, dp, d, e, left_exp.data()) || !factor(second, dq, d, e, right_exp.data())) { clear(); return false; }
		ready = crt = true;
		return true;
	}
	// Sign an algorithm-bound digest with deterministic canonical padding.
	bool sign_digest(unsigned hash_id, Bytes digest, uint8_t *output, size_t count) const {
		if (!ready || count != size() || !output) return false;
		auto encoded = crypto_storage<uint8_t, Words * 4>(size());
		return rsa_digest(hash_id, digest, encoded.data(), size()) && apply({encoded.data(), size()}, output, count);
	}
	// Sign with an explicit salt length, or the largest salt fitting the encoded message when omitted.
	template <class Hash> bool sign_pss(typename Hash::Kind kind, Bytes digest, uint8_t *output, size_t count, std::optional<size_t> salt_size = std::nullopt) const {
		const size_t width = Hash(kind).size();
		if (!ready || !output || count != size() || !digest.data || digest.size != width) return false;
		const size_t length = (modulus.bits() + 6) / 8;
		if (length < width + 2) return false;
		const size_t salt = salt_size.value_or(length - width - 2);
		if (salt > length - width - 2) return false;
		struct Buffer {
			BytesBuffer bytes;
			explicit Buffer(size_t count) : bytes(crypto_storage<uint8_t, Words * 4>(count)) {} // Retain a fully padded message until private exponentiation completes.
			~Buffer() { wipe_storage(bytes); } // Erase salted encodings on every exit path.
		} encoded(size());
		uint8_t *message = encoded.bytes.data() + size() - length;
		const size_t masked = length - width - 1, marker = masked - salt - 1;
		message[marker] = 1;
		if (salt && !random_fill(message + marker + 1, salt)) return false;
		constexpr std::array<uint8_t, 8> zeros{}; // Fixed padding prefix of the signature's digest input.
		Hash hash(kind); hash.write(zeros.data(), zeros.size()); hash.write(digest.data, digest.size); hash.write(message + marker + 1, salt); hash.sum(message + masked);
		rsa_mask<Hash>(kind, {message + masked, width}, message, masked);
		message[0] &= uint8_t(255U >> (length * 8 - (modulus.bits() - 1)));
		message[length - 1] = 0xbc;
		return apply({encoded.bytes.data(), size()}, output, count);
	}
};
}
