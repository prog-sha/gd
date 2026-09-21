// Compress wide digest blocks with unsigned rotations and explicit network byte order.
#include "hash64_core.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr uint64_t CONSTANTS[80] = { // Fractional cube-root words defined by the transform.
	0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
	0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
	0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
	0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

// Rotate by a public nonzero distance less than one word.
uint64_t rotate(uint64_t word, unsigned bits) { return (word >> bits) | (word << (64 - bits)); }

// Add one expanded message block into the chaining state.
void block(std::array<uint64_t, 8> &chain, const uint8_t *bytes) {
	std::array<uint64_t, 16> schedule{};
	for (unsigned at = 0; at != 128; ++at) schedule[at / 8] |= uint64_t(bytes[at]) << (56 - (at % 8) * 8);
	auto work = chain;
	for (unsigned step = 0; step != 80; ++step) {
		const unsigned word = step % 16, slot = (8 - step % 8) % 8;
		if (step >= 16) {
			const uint64_t x = schedule[(word + 1) % 16], y = schedule[(word + 14) % 16];
			schedule[word] += (rotate(x, 1) ^ rotate(x, 8) ^ (x >> 7)) + schedule[(word + 9) % 16] + (rotate(y, 19) ^ rotate(y, 61) ^ (y >> 6));
		}
		const uint64_t a = work[slot], b = work[(slot + 1) % 8], c = work[(slot + 2) % 8];
		const uint64_t e = work[(slot + 4) % 8], f = work[(slot + 5) % 8], g = work[(slot + 6) % 8];
		const uint64_t total = work[(slot + 7) % 8] + (rotate(e, 14) ^ rotate(e, 18) ^ rotate(e, 41)) + (g ^ (e & (f ^ g))) + CONSTANTS[step] + schedule[word];
		work[(slot + 3) % 8] += total;
		work[(slot + 7) % 8] = total + (rotate(a, 28) ^ rotate(a, 34) ^ rotate(a, 39)) + ((a & b) | (c & (a | b)));
	}
	for (unsigned at = 0; at != 8; ++at) chain[at] += work[at];
}
}

// Initialize wide chaining words and both byte-count halves.
void GDCrypto::Hash64::reset(Kind p_kind) {
	kind = p_kind;
	low = high = 0;
	state = kind == SHA384 ? std::array<uint64_t, 8>{0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
		0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL} :
		std::array<uint64_t, 8>{0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
		0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
}

// Consume complete blocks directly and retain only unfinished bytes.
void GDCrypto::Hash64::write(const void *data, size_t size) {
	size_t offset = low % pending.size();
	const uint64_t before = low;
	low += size;
	high += low < before;
	const auto *bytes = static_cast<const uint8_t *>(data);
	while (size) {
		if (!offset && size >= pending.size()) {
			block(state, bytes);
			bytes += pending.size();
			size -= pending.size();
		} else {
			const size_t count = std::min(size, pending.size() - offset);
			std::memcpy(pending.data() + offset, bytes, count);
			offset += count;
			bytes += count;
			size -= count;
			if (offset == pending.size()) { block(state, pending.data()); offset = 0; }
		}
	}
}

// Finalize a private state with a 128-bit bit count and one or two padding blocks.
void GDCrypto::Hash64::sum(uint8_t *output) const {
	std::array<uint8_t, 256> padding{};
	const size_t used = low % pending.size(), total = used + 17 > 128 ? 256 : 128;
	std::copy_n(pending.data(), used, padding.data());
	padding[used] = 0x80;
	const uint64_t upper = (high << 3) | (low >> 61), lower = low << 3;
	for (unsigned at = 0; at != 8; ++at) {
		padding[total - 16 + at] = uint8_t(upper >> (56 - at * 8));
		padding[total - 8 + at] = uint8_t(lower >> (56 - at * 8));
	}
	auto final = state;
	for (size_t at = 0; at != total; at += 128) block(final, padding.data() + at);
	for (size_t at = 0; at != size(); ++at) output[at] = uint8_t(final[at / 8] >> (56 - (at % 8) * 8));
}
