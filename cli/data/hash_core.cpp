// Compute message transforms with circular schedules and explicit unsigned byte operations.
#include "hash_core.h"
#include "crypto_core.h"
#include <algorithm>
#include <cstring>
#include "hash_hw.h"

namespace {
constexpr uint32_t SHA_K[64] = { // Round constants defined by the digest algorithm.
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};
constexpr uint32_t MD5_K[64] = { // Additive constants of the legacy protocol digest.
	0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
	0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
	0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
	0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
	0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
	0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
	0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
	0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
constexpr unsigned MD5_ROT[16] = {7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21}; // Per-round rotation distances.

// Rotate a word right by a nonzero sub-word distance.
uint32_t ror(uint32_t value, unsigned bits) { return (value >> bits) | (value << (32 - bits)); }

// Decode a block without alignment or host-endian assumptions.
std::array<uint32_t, 16> decode(const uint8_t *bytes, bool little) {
	std::array<uint32_t, 16> words{};
	for (unsigned index = 0; index != 64; ++index) {
		const unsigned shift = 8 * (little ? index % 4 : 3 - index % 4);
		words[index / 4] |= uint32_t(bytes[index]) << shift;
	}
	return words;
}

// Apply four mixing rounds to sixteen little-endian message words.
void md5_block(std::array<uint32_t, 8> &chain, const uint8_t *bytes) {
	const auto words = decode(bytes, true);
	std::array<uint32_t, 4> work = {chain[0], chain[1], chain[2], chain[3]};
	for (unsigned round = 0; round != 4; ++round) {
		for (unsigned column = 0; column != 16; ++column) {
			const unsigned step = round * 16 + column;
			const unsigned slot = (4 - column % 4) % 4;
			const uint32_t b = work[(slot + 1) % 4], c = work[(slot + 2) % 4], d = work[(slot + 3) % 4];
			uint32_t mixed = 0;
			unsigned selected = 0;
			switch (round) {
				case 0: mixed = d ^ (b & (c ^ d)); selected = column; break;
				case 1: mixed = c ^ (d & (b ^ c)); selected = (5 * column + 1) % 16; break;
				case 2: mixed = b ^ c ^ d; selected = (3 * column + 5) % 16; break;
				case 3: mixed = c ^ (b | ~d); selected = (7 * column) % 16; break;
			}
			const uint32_t total = work[slot] + mixed + words[selected] + MD5_K[step];
			work[slot] = b + ror(total, 32 - MD5_ROT[round * 4 + column % 4]);
		}
	}
	for (unsigned index = 0; index != 4; ++index) chain[index] += work[index];
}

#if defined(GD_SHA_ARM)
// Apply SHA-256 rounds and message expansion using optional vector instructions.
GD_SHA_TARGET void sha_hw(std::array<uint32_t, 8> &chain, const uint8_t *bytes) {
	auto words = decode(bytes, false);
	uint32x4_t schedule[4]; // Four rotating message vectors.
	for (unsigned index = 0; index != 4; ++index) schedule[index] = vld1q_u32(words.data() + index * 4);
	uint32x4_t left = vld1q_u32(chain.data()), right = vld1q_u32(chain.data() + 4);
	for (unsigned group = 0; group != 16; ++group) {
		const unsigned slot = group % 4;
		const uint32x4_t add = vaddq_u32(schedule[slot], vld1q_u32(SHA_K + group * 4));
		const uint32x4_t saved = left;
		left = vsha256hq_u32(left, right, add);
		right = vsha256h2q_u32(right, saved, add);
		if (group < 12) schedule[slot] = vsha256su1q_u32(vsha256su0q_u32(schedule[slot], schedule[(slot + 1) % 4]), schedule[(slot + 2) % 4], schedule[(slot + 3) % 4]);
	}
	vst1q_u32(chain.data(), vaddq_u32(left, vld1q_u32(chain.data())));
	vst1q_u32(chain.data() + 4, vaddq_u32(right, vld1q_u32(chain.data() + 4)));
}
#elif defined(GD_SHA_X86)
// Keep chaining words in the instruction's ABEF and CDGH lane order.
GD_SHA_TARGET void sha_hw(std::array<uint32_t, 8> &chain, const uint8_t *bytes) {
	const __m128i flip = _mm_set_epi8(12,13,14,15,8,9,10,11,4,5,6,7,0,1,2,3);
	__m128i schedule[4]; // Four rotating groups of big-endian message words.
	for (unsigned i = 0; i != 4; ++i) schedule[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes + i * 16)), flip);
	__m128i left = _mm_set_epi32(chain[0], chain[1], chain[4], chain[5]);
	__m128i right = _mm_set_epi32(chain[2], chain[3], chain[6], chain[7]);
	const __m128i saved_left = left, saved_right = right;
	for (unsigned group = 0; group != 16; ++group) {
		const unsigned slot = group % 4;
		__m128i add = _mm_add_epi32(schedule[slot], _mm_loadu_si128(reinterpret_cast<const __m128i *>(SHA_K + group * 4)));
		right = _mm_sha256rnds2_epu32(right, left, add);
		left = _mm_sha256rnds2_epu32(left, right, _mm_shuffle_epi32(add, 0x0e));
		if (group < 12) {
			__m128i next = _mm_sha256msg1_epu32(schedule[slot], schedule[(slot + 1) % 4]);
			next = _mm_add_epi32(next, _mm_alignr_epi8(schedule[(slot + 3) % 4], schedule[(slot + 2) % 4], 4));
			schedule[slot] = _mm_sha256msg2_epu32(next, schedule[(slot + 3) % 4]);
		}
	}
	uint32_t abef[4], cdgh[4]; // Native lane order is the reverse of the chaining notation.
	_mm_storeu_si128(reinterpret_cast<__m128i *>(abef), _mm_add_epi32(left, saved_left));
	_mm_storeu_si128(reinterpret_cast<__m128i *>(cdgh), _mm_add_epi32(right, saved_right));
	chain = {abef[3], abef[2], cdgh[3], cdgh[2], abef[1], abef[0], cdgh[1], cdgh[0]};
}
#endif

// Dispatch only after checking CPU support, preserving a baseline implementation.
void sha_block(std::array<uint32_t, 8> &chain, const uint8_t *bytes) {
#if defined(GD_SHA_ARM) || defined(GD_SHA_X86)
	if (SHAHW::available()) { sha_hw(chain, bytes); return; }
#endif
	auto words = decode(bytes, false);

	auto work = chain;
	for (unsigned step = 0; step != 64; ++step) {
		const unsigned word = step % 16, slot = (8 - step % 8) % 8;
		if (step >= 16) {
			const uint32_t x = words[(word + 1) % 16], y = words[(word + 14) % 16];
			words[word] += (ror(x, 7) ^ ror(x, 18) ^ (x >> 3)) + words[(word + 9) % 16] + (ror(y, 17) ^ ror(y, 19) ^ (y >> 10));
		}
		const uint32_t a = work[slot], b = work[(slot + 1) % 8], c = work[(slot + 2) % 8];
		const uint32_t e = work[(slot + 4) % 8], f = work[(slot + 5) % 8], g = work[(slot + 6) % 8];
		const uint32_t total = work[(slot + 7) % 8] + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + (g ^ (e & (f ^ g))) + SHA_K[step] + words[word];
		work[(slot + 3) % 8] += total;
		work[(slot + 7) % 8] = total + (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) | (c & (a | b)));
	}
	for (unsigned index = 0; index != 8; ++index) chain[index] += work[index];
}

// Apply the legacy secure transform with a circular schedule and five chaining words.
void sha1_block(std::array<uint32_t, 8> &chain, const uint8_t *bytes) {
	auto words = decode(bytes, false);
	uint32_t a = chain[0], b = chain[1], c = chain[2], d = chain[3], e = chain[4];
	constexpr uint32_t constants[] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6}; // Round additions defined by the transform.
	for (unsigned step = 0; step != 80; ++step) {
		const unsigned slot = step % 16;
		if (step >= 16) words[slot] = ror(words[(slot + 13) % 16] ^ words[(slot + 8) % 16] ^ words[(slot + 2) % 16] ^ words[slot], 31);
		const uint32_t mixed = step < 20 ? d ^ (b & (c ^ d)) : (step < 40 || step >= 60 ? b ^ c ^ d : (b & c) | (d & (b | c)));
		const uint32_t next = ror(a, 27) + mixed + e + constants[step / 20] + words[slot];
		e = d; d = c; c = ror(b, 2); b = a; a = next;
	}
	chain[0] += a; chain[1] += b; chain[2] += c; chain[3] += d; chain[4] += e;
}

// Select the block transform without an allocated polymorphic context.
void transform(GDCrypto::Hash32::Kind kind, std::array<uint32_t, 8> &chain, const uint8_t *bytes) {
	if (kind == GDCrypto::Hash32::MD5) md5_block(chain, bytes);
	else if (kind == GDCrypto::Hash32::SHA1) sha1_block(chain, bytes);
	else sha_block(chain, bytes);
}
}

// Share compression with callers that own their padding and SHA-224 initial state.
void GDCrypto::sha256_block(uint32_t *p_state, const uint8_t *p_data) {
	std::array<uint32_t, 8> chain;
	std::copy_n(p_state, chain.size(), chain.begin());
	sha_block(chain, p_data);
	std::copy(chain.begin(), chain.end(), p_state);
}

// Initialize the chaining words for the selected message transform.
void GDCrypto::Hash32::reset(Kind p_kind) {
	kind = p_kind;
	count = 0;
	state = kind == MD5 ? std::array<uint32_t, 8>{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, 0, 0, 0} :
			std::array<uint32_t, 8>{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	if (kind == SHA1) state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0, 0, 0, 0};
}

// Stream complete blocks directly and copy only input straddling a block boundary.
void GDCrypto::Hash32::write(const void *p_data, size_t p_size) {
	size_t offset = count % pending.size();
	count += p_size;
	const auto *bytes = static_cast<const uint8_t *>(p_data);
	while (p_size) {
		if (!offset && p_size >= pending.size()) {
			transform(kind, state, bytes);
			bytes += pending.size();
			p_size -= pending.size();
		} else {
			const size_t copied = std::min(p_size, pending.size() - offset);
			std::memcpy(pending.data() + offset, bytes, copied);
			bytes += copied;
			p_size -= copied;
			offset += copied;
			if (offset == pending.size()) { transform(kind, state, pending.data()); offset = 0; }
		}
	}
}

// Append the bit marker and encoded length to a private terminal block sequence.
void GDCrypto::Hash32::sum(uint8_t *p_out) const {
	std::array<uint8_t, 128> ending{}; // Padding may span two algorithm blocks.
	const size_t used = count % pending.size(), size = used + 9 > 64 ? 128 : 64;
	std::copy_n(pending.data(), used, ending.data());
	ending[used] = 0x80;
	for (unsigned byte = 0; byte != 8; ++byte) ending[size - 8 + byte] = uint8_t((count * 8) >> (8 * (kind == MD5 ? byte : 7 - byte)));
	auto final = state;
	for (size_t offset = 0; offset < size; offset += 64) transform(kind, final, ending.data() + offset);
	for (unsigned byte = 0; byte != this->size(); ++byte) p_out[byte] = uint8_t(final[byte / 4] >> (8 * (kind == MD5 ? byte % 4 : 3 - byte % 4)));
}

// Mask the tail and encoded length into fixed padding blocks before selecting the appropriate final state.
void GDCrypto::Hash32::constant_sum(uint8_t *p_out) const {
	std::array<uint8_t, 128> ending{};
	const size_t used = count % pending.size();
	const uint32_t first = opaque_mask(0U - uint32_t(used < 56));
	for (unsigned byte = 0; byte != 64; ++byte) {
		const uint8_t data_mask = uint8_t(opaque_mask(0U - uint32_t(byte < used)));
		const uint8_t marker = uint8_t(opaque_mask(0U - uint32_t(byte == used)));
		ending[byte] = (pending[byte] & data_mask) | (0x80 & marker);
	}
	for (unsigned byte = 0; byte != 8; ++byte) {
		const uint8_t length = uint8_t((count * 8) >> (8 * (kind == MD5 ? byte : 7 - byte)));
		ending[56 + byte] = (ending[56 + byte] & uint8_t(~first)) | (length & uint8_t(first));
		ending[120 + byte] = length;
	}
	auto once = state;
	transform(kind, once, ending.data());
	auto twice = once;
	transform(kind, twice, ending.data() + 64);
	for (size_t word = 0; word != twice.size(); ++word) twice[word] ^= (twice[word] ^ once[word]) & first;
	for (unsigned byte = 0; byte != size(); ++byte) p_out[byte] = uint8_t(twice[byte / 4] >> (8 * (kind == MD5 ? byte % 4 : 3 - byte % 4)));
	erase(ending.data(), ending.size()); erase(once.data(), sizeof(once)); erase(twice.data(), sizeof(twice));
}

// Make one chaining word observable through the fixed-address optimization barrier.
void GDCrypto::Hash32::keep() const { opaque_mask(state[0]); }
