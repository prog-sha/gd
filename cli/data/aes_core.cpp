// Protect records with arithmetic substitution, counter encryption, and polynomial authentication.
#include "aes_core.h"
#include <algorithm>
#include <cstring>
#include "aes_hw.h"

namespace {
using Block = std::array<uint8_t, 16>;
using GDCrypto::overlap;

// Multiply by the field generator with branch-free polynomial reduction.
uint8_t twice(uint8_t x) { return uint8_t((unsigned(x) << 1) ^ (0x1bU & (0U - (x >> 7)))); }

// Multiply two field elements using a fixed number of additions.
uint8_t product(uint8_t x, uint8_t y) {
	uint8_t result = 0;
	for (unsigned bit = 0; bit != 8; ++bit) {
		result ^= x & uint8_t(0U - (y & 1));
		x = twice(x);
		y >>= 1;
	}
	return result;
}

// Invert one byte in the cipher's finite field, mapping zero to zero.
uint8_t invert(uint8_t x) {
	const uint8_t x2 = product(x, x), x3 = product(x2, x), x6 = product(x3, x3);
	const uint8_t x12 = product(x6, x6), x15 = product(x12, x3), x30 = product(x15, x15);
	const uint8_t x60 = product(x30, x30), x120 = product(x60, x60), x240 = product(x120, x120);
	return product(product(x240, x12), x2);
}

// Apply the forward affine map to an inverted field element.
uint8_t substitute(uint8_t x) {
	const uint8_t inverse = invert(x);
	const unsigned word = unsigned(inverse) | (unsigned(inverse) << 8);
	return uint8_t(word ^ (word >> 7) ^ (word >> 6) ^ (word >> 5) ^ (word >> 4) ^ 0x63);
}

// Undo the affine substitution before applying field inversion.
[[maybe_unused]] uint8_t unsubstitute(uint8_t x) {
	const unsigned word = unsigned(x) | (unsigned(x) << 8);
	return invert(uint8_t((word >> 7) ^ (word >> 5) ^ (word >> 2) ^ 0x05));
}

// Mix columns in either direction without secret-dependent table indices.
void columns(Block &block, bool inverse) {
	for (unsigned col = 0; col != 16; col += 4) {
		if (inverse) {
			const uint8_t a = twice(twice(block[col] ^ block[col + 2])), b = twice(twice(block[col + 1] ^ block[col + 3]));
			block[col] ^= a; block[col + 2] ^= a; block[col + 1] ^= b; block[col + 3] ^= b;
		}
		const Block before = block;
		const uint8_t total = before[col] ^ before[col + 1] ^ before[col + 2] ^ before[col + 3];
		for (unsigned row = 0; row != 4; ++row) block[col + row] ^= total ^ twice(before[col + row] ^ before[col + (row + 1) % 4]);
	}
}

// Apply row permutation and column mixing without data-dependent memory access.
[[maybe_unused]] void round_block(Block &block, bool last) {
	Block shifted{};
	for (unsigned col = 0; col != 4; ++col) {
		for (unsigned row = 0; row != 4; ++row) shifted[col * 4 + row] = substitute(block[((col + row) % 4) * 4 + row]);
	}
	block = shifted;
	if (!last) columns(block, false);
}

// Decode a network-order field word without alignment requirements.
uint64_t load(const uint8_t *bytes) {
	uint64_t word = 0;
	for (unsigned at = 0; at != 8; ++at) word = (word << 8) | bytes[at];
	return word;
}

// Encode one network-order field word.
void save(uint8_t *bytes, uint64_t word) {
	for (unsigned at = 0; at != 8; ++at) bytes[at] = uint8_t(word >> (56 - 8 * at));
}

#if defined(GD_AES_X86) || defined(GD_AES_ARM)
// Convert between reflected authentication bits and polynomial instruction operands.
uint64_t reverse(uint64_t word) {
	word = ((word & 0x5555555555555555ULL) << 1) | ((word >> 1) & 0x5555555555555555ULL);
	word = ((word & 0x3333333333333333ULL) << 2) | ((word >> 2) & 0x3333333333333333ULL);
	word = ((word & 0x0f0f0f0f0f0f0f0fULL) << 4) | ((word >> 4) & 0x0f0f0f0f0f0f0f0fULL);
	word = ((word & 0x00ff00ff00ff00ffULL) << 8) | ((word >> 8) & 0x00ff00ff00ff00ffULL);
	word = ((word & 0x0000ffff0000ffffULL) << 16) | ((word >> 16) & 0x0000ffff0000ffffULL);
	return (word << 32) | (word >> 32);
}
#endif

// Multiply authentication polynomials with fixed control flow and no secret tables.
void multiply(Block &value, const Block &key) {
#if defined(GD_AES_X86) || defined(GD_AES_ARM)
	if (AESHW::features().mul) {
		const uint64_t a = reverse(load(value.data())), b = reverse(load(value.data() + 8));
		const uint64_t c = reverse(load(key.data())), d = reverse(load(key.data() + 8));
		uint64_t x, y, h, j;
		AESHW::product(a, b, c, d, x, y, h, j);

		x ^= h ^ (h << 1) ^ (h << 2) ^ (h << 7);
		y ^= j ^ (j << 1) ^ (j << 2) ^ (j << 7) ^ (h >> 63) ^ (h >> 62) ^ (h >> 57);
		const uint64_t carry = (j >> 63) ^ (j >> 62) ^ (j >> 57);
		x ^= carry ^ (carry << 1) ^ (carry << 2) ^ (carry << 7);
		save(value.data(), reverse(x));
		save(value.data() + 8, reverse(y));

		return;
	}
#endif
	uint64_t x = load(value.data()), y = load(value.data() + 8);
	uint64_t a = load(key.data()), b = load(key.data() + 8), left = 0, right = 0;
	for (unsigned bit = 0; bit != 128; ++bit) {
		const uint64_t mask = uint64_t(0) - (x >> 63);
		left ^= a & mask;
		right ^= b & mask;
		x = (x << 1) | (y >> 63);
		y <<= 1;
		const uint64_t reduction = 0xe100000000000000ULL & (uint64_t(0) - (b & 1));
		b = (b >> 1) | (a << 63);
		a = (a >> 1) ^ reduction;
	}
	save(value.data(), left);
	save(value.data() + 8, right);
}

// Authenticate full and trailing blocks with independent zero padding for each input field.
void absorb(Block &state, const Block &key, const uint8_t *bytes, size_t size) {
	while (size) {
		const size_t count = std::min(size, state.size());
		for (size_t at = 0; at != count; ++at) state[at] ^= bytes[at];
		multiply(state, key);
		bytes += count;
		size -= count;
	}
}

}

// Prefer hardware record protection only when both independent CPU features are present.
bool GDCrypto::AES::accelerated() { return AESHW::features().aes && AESHW::features().mul; }

// Destroy expanded encryption key material.
GDCrypto::AES::~AES() { erase(keys.data(), keys.size()); }

// Expand a key using arithmetic byte substitution and public key-length branching.
bool GDCrypto::AES::reset(const uint8_t *key, size_t size) {
	erase(keys.data(), keys.size());
	rounds = 0;
	if (!key || (size != 16 && size != 24 && size != 32)) return false;
	rounds = unsigned(size / 4) + 6;
	std::copy_n(key, size, keys.data());
	uint8_t constant = 1;
	for (size_t at = size; at != (rounds + 1) * 16; at += 4) {
		std::array<uint8_t, 4> word{};
		std::copy_n(keys.data() + at - 4, 4, word.data());
		if (at % size == 0) {
			std::rotate(word.begin(), word.begin() + 1, word.end());
			for (auto &byte : word) byte = substitute(byte);
			word[0] ^= constant;
			constant = twice(constant);
		} else if (size == 32 && at % size == 16) {
			for (auto &byte : word) byte = substitute(byte);
		}
		for (unsigned byte = 0; byte != 4; ++byte) keys[at + byte] = keys[at - size + byte] ^ word[byte];
		erase(word.data(), word.size());
	}
	return true;
}

// Encrypt a block with hardware rounds where available and arithmetic rounds otherwise.
bool GDCrypto::AES::encrypt(const uint8_t *input, uint8_t *output) const {
	if (!rounds || !input || !output) return false;
#if defined(GD_AES_X86) || defined(GD_AES_ARM)
	if (AESHW::features().aes) { AESHW::encrypt(keys.data(), rounds, input, output); return true; }
#endif
	Block block{};
	for (unsigned byte = 0; byte != 16; ++byte) block[byte] = input[byte] ^ keys[byte];
	for (unsigned round = 1; round <= rounds; ++round) {
		round_block(block, round == rounds);
		for (unsigned byte = 0; byte != 16; ++byte) block[byte] ^= keys[round * 16 + byte];
	}
	std::copy(block.begin(), block.end(), output);
	erase(block.data(), block.size());
	return true;
}

// Reverse cipher rounds without expanding another key schedule for authenticated stream-only callers.
bool GDCrypto::AES::decrypt(const uint8_t *input, uint8_t *output) const {
	if (!rounds || !input || !output) return false;
#if defined(GD_AES_X86) || defined(GD_AES_ARM)
	if (AESHW::features().aes) { AESHW::decrypt(keys.data(), rounds, input, output); return true; }
#endif
	Block block{};
	for (unsigned byte = 0; byte != 16; ++byte) block[byte] = input[byte] ^ keys[rounds * 16 + byte];
	for (unsigned round = rounds; round != 0; --round) {
		Block shifted{};
		for (unsigned col = 0; col != 4; ++col) for (unsigned row = 0; row != 4; ++row) shifted[col * 4 + row] = unsubstitute(block[((col + 4 - row) % 4) * 4 + row]);
		block = shifted;
		for (unsigned byte = 0; byte != 16; ++byte) block[byte] ^= keys[(round - 1) * 16 + byte];
		if (round != 1) columns(block, true);
	}
	std::copy(block.begin(), block.end(), output); erase(block.data(), block.size());
	return true;
}

// Destroy the record authentication key.
GDCrypto::AESGCM::~AESGCM() { erase(subkey.data(), subkey.size()); }

// Prepare both encryption and authentication keys together.
bool GDCrypto::AESGCM::reset(const uint8_t *key, size_t size) {
	erase(subkey.data(), subkey.size());
	ready = aes.reset(key, size);
	if (ready) aes.encrypt(subkey.data(), subkey.data());
	return ready;
}

// Reject counter wrap and associated-data lengths that cannot be encoded in bits.
bool GDCrypto::AESGCM::valid_size(uint64_t size, uint64_t aad_size) {
	return size <= ((uint64_t(1) << 32) - 2) * 16 && aad_size <= UINT64_MAX / 8;
}

// Produce a counter keystream without retaining plaintext beyond a single block.
void GDCrypto::AESGCM::crypt(const uint8_t *nonce, const uint8_t *input, size_t size, uint8_t *output) const {
	Block counter{}, stream{};
	std::copy_n(nonce, 12, counter.data());
	uint32_t sequence = 1;
	while (size) {
		++sequence;
		for (unsigned byte = 0; byte != 4; ++byte) counter[12 + byte] = uint8_t(sequence >> (24 - 8 * byte));
		aes.encrypt(counter.data(), stream.data());
		const size_t count = std::min(size, stream.size());
		for (size_t at = 0; at != count; ++at) output[at] = input[at] ^ stream[at];
		input += count;
		output += count;
		size -= count;
	}
	erase(stream.data(), stream.size());
}

// Authenticate ciphertext, associated data, and their exact encoded bit lengths.
void GDCrypto::AESGCM::tag(const uint8_t *nonce, const uint8_t *data, size_t size, const uint8_t *aad, size_t aad_size, uint8_t *out) const {
	Block state{}, length{}, mask{};
	absorb(state, subkey, aad, aad_size);
	absorb(state, subkey, data, size);
	save(length.data(), uint64_t(aad_size) * 8);
	save(length.data() + 8, uint64_t(size) * 8);
	absorb(state, subkey, length.data(), length.size());
	std::copy_n(nonce, 12, mask.data());
	mask[15] = 1;
	aes.encrypt(mask.data(), mask.data());
	for (unsigned at = 0; at != 16; ++at) out[at] = state[at] ^ mask[at];
	erase(mask.data(), mask.size());
	erase(state.data(), state.size());
}

// Encrypt only after validating the entire input and detached-tag destination layout.
bool GDCrypto::AESGCM::seal(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, uint8_t *output, uint8_t *auth) const {
	if (!ready || !valid_size(size, aad_size) || !aead_layout(input, size, aad, aad_size, nonce, auth, output) ||
			overlap(auth, 16, aad, aad_size) || overlap(auth, 16, nonce, 12)) return false;
	crypt(nonce, input, size, output);
	tag(nonce, output, size, aad, aad_size, auth);
	return true;
}

// Verify the full tag before making plaintext visible, including in-place operation.
bool GDCrypto::AESGCM::open(const uint8_t *nonce, const uint8_t *input, size_t size, const uint8_t *aad, size_t aad_size, const uint8_t *auth, uint8_t *output) const {
	if (!ready || !valid_size(size, aad_size) || !aead_layout(input, size, aad, aad_size, nonce, auth, output)) return false;
	Block expected{};
	tag(nonce, input, size, aad, aad_size, expected.data());
	const bool matches = equal(auth, expected.data(), expected.size());
	erase(expected.data(), expected.size());
	if (!matches) return false;
	crypt(nonce, input, size, output);
	return true;
}
