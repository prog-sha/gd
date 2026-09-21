// Authenticate counter-encrypted records using fixed arithmetic without secret-indexed tables.
#include "chacha_core.h"
#include "chacha_simd.h"
#include <algorithm>

namespace {
using GDCrypto::erase;

// Decode a little-endian word without requiring aligned storage.
uint32_t load(const uint8_t *bytes) {
	return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

// Rotate an unsigned word by a public distance.
uint32_t rotate(uint32_t word, unsigned bits) { return (word << bits) | (word >> (32 - bits)); }

// Mix one four-word column or diagonal with alternating additions and rotations.
void quarter(uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d) {
	a += b; d = rotate(d ^ a, 16); c += d; b = rotate(b ^ c, 12);
	a += b; d = rotate(d ^ a, 8); c += d; b = rotate(b ^ c, 7);
}

// Decode the shared initial state without alignment or byte-order assumptions.
std::array<uint32_t, 16> seed(const std::array<uint8_t, 32> &key, const uint8_t *nonce, uint32_t counter) {
	std::array<uint32_t, 16> initial = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574}; // Encoded domain-separation words.
	for (unsigned at = 0; at != 8; ++at) initial[4 + at] = load(key.data() + at * 4);
	initial[12] = counter;
	for (unsigned at = 0; at != 3; ++at) initial[13 + at] = load(nonce + at * 4);
	return initial;
}

// Generate a complete keystream block from the key, nonce, and explicit counter.
std::array<uint8_t, 64> block(const std::array<uint8_t, 32> &key, const uint8_t *nonce, uint32_t counter) {
	auto initial = seed(key, nonce, counter);
	auto work = initial;
	for (unsigned round = 0; round != 20; ++round) {
		const unsigned diagonal = round % 2;
		for (unsigned col = 0; col != 4; ++col) quarter(work[col], work[4 + (col + diagonal) % 4], work[8 + (col + 2 * diagonal) % 4], work[12 + (col + 3 * diagonal) % 4]);
	}
	std::array<uint8_t, 64> out{};
	for (unsigned at = 0; at != 16; ++at) {
		const uint32_t word = work[at] + initial[at];
		for (unsigned byte = 0; byte != 4; ++byte) out[at * 4 + byte] = uint8_t(word >> (byte * 8));
	}
	erase(work.data(), sizeof(work)); erase(initial.data(), sizeof(initial));
	return out;
}

// Accumulate full authenticator blocks in five bounded radix words.
class Polynomial {
	static constexpr uint64_t MASK = (uint64_t(1) << 26) - 1; // Radix for the 130-bit authentication field.
	std::array<uint64_t, 5> key{}, state{}; // Clamped multiplier and running accumulator.
	std::array<uint8_t, 16> pad{}; // Additive half of the one-time key.
	// Decode the low 128 bits into radix words.
	static std::array<uint64_t, 5> decode(const uint8_t *bytes) {
		return {load(bytes) & MASK, (load(bytes + 3) >> 2) & MASK, (load(bytes + 6) >> 4) & MASK,
			(load(bytes + 9) >> 6) & MASK, load(bytes + 12) >> 8};
	}
	// Fold powers above bit 130 and remove any remaining multiple of the modulus.
	void reduce() {
		for (unsigned pass = 0; pass != 3; ++pass) {
			for (unsigned at = 0; at != 4; ++at) { state[at + 1] += state[at] >> 26; state[at] &= MASK; }
			state[0] += (state[4] >> 26) * 5; state[4] &= MASK;
		}
		auto reduced = state;
		uint64_t carry = 5;
		for (unsigned at = 0; at != 5; ++at) { carry += state[at]; reduced[at] = carry & MASK; carry >>= 26; }
		const uint64_t selected = uint64_t(0) - carry;
		for (unsigned at = 0; at != 5; ++at) state[at] ^= (state[at] ^ reduced[at]) & selected;
	}
public:
	// Separate the clamped multiplier and final additive pad.
	explicit Polynomial(const uint8_t *secret) {
		std::array<uint8_t, 16> clamped{};
		std::copy_n(secret, 16, clamped.data());
		for (unsigned at : {3U, 7U, 11U, 15U}) clamped[at] &= 15;
		for (unsigned at : {4U, 8U, 12U}) clamped[at] &= 252;
		key = decode(clamped.data()); std::copy_n(secret + 16, 16, pad.data());
		erase(clamped.data(), clamped.size());
	}
	// Erase the one-time multiplier, accumulator, and additive pad.
	~Polynomial() { erase(key.data(), sizeof(key)); erase(state.data(), sizeof(state)); erase(pad.data(), pad.size()); }
	// Add a complete padded block and its terminating field bit, then multiply.
	void write(const uint8_t *bytes) {
		auto input = decode(bytes);
		input[4] |= uint64_t(1) << 24;
		for (unsigned at = 0; at != 5; ++at) state[at] += input[at];
		std::array<uint64_t, 5> sums{};
		for (unsigned left = 0; left != 5; ++left) for (unsigned right = 0; right != 5; ++right) {
			const unsigned power = left + right;
			sums[power % 5] += state[left] * key[right] * (power >= 5 ? 5 : 1);
		}
		state = sums; reduce();
		erase(sums.data(), sizeof(sums));
	}
	// Authenticate a field with independent zero padding to a full block boundary.
	void absorb(const uint8_t *bytes, size_t size) {
		while (size >= 16) { write(bytes); bytes += 16; size -= 16; }
		if (size) { std::array<uint8_t, 16> tail{}; std::copy_n(bytes, size, tail.data()); write(tail.data()); }
	}
	// Add the one-time pad modulo 128 bits to form the transmitted authenticator.
	void sum(uint8_t *output) {
		reduce();
		std::array<uint8_t, 16> raw{};
		for (unsigned at = 0; at != 16; ++at) {
			const unsigned word = at * 8 / 26, shift = at * 8 % 26;
			raw[at] = uint8_t(state[word] >> shift);
			if (shift > 18 && word < 4) raw[at] |= uint8_t(state[word + 1] << (26 - shift));
		}
		unsigned carry = 0;
		for (unsigned at = 0; at != 16; ++at) { carry += unsigned(raw[at]) + pad[at]; output[at] = uint8_t(carry); carry >>= 8; }
		erase(raw.data(), raw.size());
	}
};
}

// Clear retained stream key material.
GDCrypto::ChaChaPoly::~ChaChaPoly() { erase(key.data(), key.size()); }

// Replace a key only when the complete required width is provided.
bool GDCrypto::ChaChaPoly::reset(const uint8_t *input, size_t size) {
	erase(key.data(), key.size()); ready = false;
	if (!input || size != key.size()) return false;
	std::copy_n(input, size, key.data()); ready = true;
	return true;
}

// Reserve counter zero for the authenticator key and reject counter wrap.
bool GDCrypto::ChaChaPoly::valid_size(uint64_t size) { return size <= uint64_t(UINT32_MAX) * 64; }

// Apply one keystream block at a time with exact input/output overlap support.
void GDCrypto::ChaChaPoly::crypt(const uint8_t *nonce, const uint8_t *input, size_t size, uint8_t *output) const {
	uint32_t counter = 1;
#ifdef GD_CHACHA_SIMD
	// Batch complete blocks while leaving arbitrary tails to the scalar transform.
	while (size >= 256) {
		auto initial = seed(key, nonce, counter);
		ChaChaSIMD::crypt(initial.data(), input, output);
		erase(initial.data(), sizeof(initial));
		counter += 4;
		input += 256; output += 256; size -= 256;
	}
#endif
	while (size) {
		auto stream = block(key, nonce, counter++);
		const size_t count = std::min(size, stream.size());
		for (size_t at = 0; at != count; ++at) output[at] = input[at] ^ stream[at];
		input += count; output += count; size -= count;
		erase(stream.data(), stream.size());
	}
}

// Bind independently padded fields and their exact little-endian byte lengths.
void GDCrypto::ChaChaPoly::tag(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, uint8_t *output) const {
	auto first = block(key, nonce, 0);
	Polynomial mac(first.data());
	erase(first.data(), first.size());
	mac.absorb(aad, aad_size); mac.absorb(input, size);
	std::array<uint8_t, 16> lengths{};
	for (unsigned at = 0; at != 8; ++at) { lengths[at] = uint8_t(uint64_t(aad_size) >> (8 * at)); lengths[8 + at] = uint8_t(uint64_t(size) >> (8 * at)); }
	mac.write(lengths.data()); mac.sum(output);
}

// Validate the entire buffer layout before encrypting or writing the detached tag.
bool GDCrypto::ChaChaPoly::seal(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, uint8_t *output, uint8_t *auth) const {
	if (!ready || !valid_size(size) || !aead_layout(input, size, aad, aad_size, nonce, auth, output) || overlap(auth, 16, aad, aad_size) || overlap(auth, 16, nonce, 12)) return false;
	crypt(nonce, input, size, output); tag(nonce, output, size, aad, aad_size, auth);
	return true;
}

// Authenticate first so failures cannot expose even partial plaintext.
bool GDCrypto::ChaChaPoly::open(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, const uint8_t *auth, uint8_t *output) const {
	if (!ready || !valid_size(size) || !aead_layout(input, size, aad, aad_size, nonce, auth, output)) return false;
	std::array<uint8_t, 16> expected{};
	tag(nonce, input, size, aad, aad_size, expected.data());
	const bool valid = equal(expected.data(), auth, expected.size());
	erase(expected.data(), expected.size());
	if (!valid) return false;
	crypt(nonce, input, size, output);
	return true;
}
