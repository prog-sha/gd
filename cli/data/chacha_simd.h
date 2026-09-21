// Process four independent stream blocks using baseline 128-bit vector arithmetic.
#pragma once
#include "crypto_core.h"

#if !defined(GD_CRYPTO_PORTABLE) && (defined(__x86_64__) || defined(_M_X64))
#define GD_CHACHA_SIMD
#define GD_CHACHA_SSE
#include <emmintrin.h>
#elif !defined(GD_CRYPTO_PORTABLE) && defined(__aarch64__)
#define GD_CHACHA_SIMD
#include <arm_neon.h>
#endif

#ifdef GD_CHACHA_SIMD
namespace ChaChaSIMD {
#ifdef GD_CHACHA_SSE
using Word = __m128i; // Four independent unsigned 32-bit lanes.
inline Word repeat(uint32_t x) { return _mm_set1_epi32(x); } // Broadcast a state word.
inline Word add(Word a, Word b) { return _mm_add_epi32(a, b); } // Add modulo the lane width.
inline Word bits(Word a, Word b) { return _mm_xor_si128(a, b); } // Combine state lanes.
template <int N> inline Word rotate(Word a) { return _mm_or_si128(_mm_slli_epi32(a, N), _mm_srli_epi32(a, 32 - N)); } // Rotate each lane.
inline Word counters() { return _mm_set_epi32(3, 2, 1, 0); } // Consecutive block offsets.
inline void store(uint32_t *out, Word a) { _mm_storeu_si128(reinterpret_cast<Word *>(out), a); } // Allow unaligned destinations.
#else
using Word = uint32x4_t; // Four independent unsigned 32-bit lanes.
inline Word repeat(uint32_t x) { return vdupq_n_u32(x); } // Broadcast a state word.
inline Word add(Word a, Word b) { return vaddq_u32(a, b); } // Add modulo the lane width.
inline Word bits(Word a, Word b) { return veorq_u32(a, b); } // Combine state lanes.
template <int N> inline Word rotate(Word a) { return vorrq_u32(vshlq_n_u32(a, N), vshrq_n_u32(a, 32 - N)); } // Rotate each lane.
inline Word counters() { const uint32_t offsets[] = {0, 1, 2, 3}; return vld1q_u32(offsets); } // Consecutive block offsets.
inline void store(uint32_t *out, Word a) { vst1q_u32(out, a); } // Allow unaligned destinations.
#endif

// Apply the same quarter round independently to all four block states.
inline void quarter(Word &a, Word &b, Word &c, Word &d) {
	a = add(a, b); d = rotate<16>(bits(d, a)); c = add(c, d); b = rotate<12>(bits(b, c));
	a = add(a, b); d = rotate<8>(bits(d, a)); c = add(c, d); b = rotate<7>(bits(b, c));
}

// XOR four full blocks after the caller has verified counter capacity and buffer layout.
inline void crypt(const uint32_t *initial, const uint8_t *input, uint8_t *output) {
	Word work[16]; // Each word position spans four independent blocks.
	for (unsigned i = 0; i != 16; ++i) work[i] = repeat(initial[i]);
	work[12] = add(work[12], counters());
	for (unsigned round = 0; round != 10; ++round) {
		quarter(work[0], work[4], work[8], work[12]);
		quarter(work[1], work[5], work[9], work[13]);
		quarter(work[2], work[6], work[10], work[14]);
		quarter(work[3], work[7], work[11], work[15]);
		quarter(work[0], work[5], work[10], work[15]);
		quarter(work[1], work[6], work[11], work[12]);
		quarter(work[2], work[7], work[8], work[13]);
		quarter(work[3], work[4], work[9], work[14]);
	}
	uint32_t lanes[4]; // Transpose one word at a time without an intermediate keystream buffer.
	for (unsigned word = 0; word != 16; ++word) {
		Word seed = repeat(initial[word]);
		if (word == 12) seed = add(seed, counters());
		store(lanes, add(work[word], seed));
		for (unsigned lane = 0; lane != 4; ++lane) {
			for (unsigned byte = 0; byte != 4; ++byte) {
				const unsigned at = lane * 64 + word * 4 + byte;
				output[at] = input[at] ^ uint8_t(lanes[lane] >> (byte * 8));
			}
		}
	}
	GDCrypto::erase(lanes, sizeof(lanes));
	GDCrypto::erase(work, sizeof(work));
}
}
#endif
