// Select optional block and polynomial instructions without raising the executable's CPU baseline.
#pragma once
#include <cstdint>
#include <cstddef>

#if !defined(GD_CRYPTO_PORTABLE) && (defined(__x86_64__) || defined(_M_X64))
#define GD_AES_X86
#include <wmmintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#define GD_AES_TARGET
#define GD_MUL_TARGET
#else
#include <cpuid.h>
#define GD_AES_TARGET __attribute__((target("aes,sse2")))
#define GD_MUL_TARGET __attribute__((target("pclmul,sse2")))
#endif
#elif !defined(GD_CRYPTO_PORTABLE) && defined(__aarch64__)
#define GD_AES_ARM
#include <arm_neon.h>
#if defined(__clang__)
#define GD_AES_TARGET __attribute__((target("aes")))
#else
#define GD_AES_TARGET __attribute__((target("+crypto")))
#endif
#define GD_MUL_TARGET GD_AES_TARGET
#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif

namespace AESHW {
struct Features {
	bool aes = false; // Block round instructions are usable on this CPU.
	bool mul = false; // Carry-less multiplication is usable independently of block rounds.
};

// Cache public CPU capabilities once; unknown platforms retain the arithmetic implementation.
inline const Features &features() {
	static const Features value = [] {
		Features found;
#ifdef GD_AES_X86
#ifdef _MSC_VER
		int words[4];
		__cpuid(words, 1);
		const unsigned caps = unsigned(words[2]);
#else
		unsigned a, b, caps, d;
		if (!__get_cpuid(1, &a, &b, &caps, &d)) return found;
#endif
		found.aes = (caps & (1U << 25)) != 0;
		found.mul = (caps & (1U << 1)) != 0;
#elif defined(GD_AES_ARM)
#if defined(__linux__)
		const unsigned long caps = getauxval(AT_HWCAP);
		found.aes = (caps & HWCAP_AES) != 0;
		found.mul = (caps & HWCAP_PMULL) != 0;
#elif defined(__APPLE__)
		int aes = 0, mul = 0;
		size_t size = sizeof(int);
		found.aes = sysctlbyname("hw.optional.arm.FEAT_AES", &aes, &size, nullptr, 0) == 0 && aes;
		size = sizeof(int);
		found.mul = sysctlbyname("hw.optional.arm.FEAT_PMULL", &mul, &size, nullptr, 0) == 0 && mul;
#elif defined(_WIN32)
		found.aes = found.mul = IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE);
#endif
#endif
		return found;
	}();
	return value;
}

#if defined(GD_AES_X86) || defined(GD_AES_ARM)
// Encrypt one block from the common byte-oriented encryption schedule.
GD_AES_TARGET inline void encrypt(const uint8_t *keys, unsigned rounds, const uint8_t *input, uint8_t *output) {
#ifdef GD_AES_X86
	__m128i block = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(input)), _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys)));
	for (unsigned round = 1; round < rounds; ++round) block = _mm_aesenc_si128(block, _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys + round * 16)));
	block = _mm_aesenclast_si128(block, _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys + rounds * 16)));
	_mm_storeu_si128(reinterpret_cast<__m128i *>(output), block);
#else
	uint8x16_t block = vld1q_u8(input);
	for (unsigned round = 0; round != rounds - 1; ++round) block = vaesmcq_u8(vaeseq_u8(block, vld1q_u8(keys + round * 16)));
	block = vaeseq_u8(block, vld1q_u8(keys + (rounds - 1) * 16));
	vst1q_u8(output, veorq_u8(block, vld1q_u8(keys + rounds * 16)));
#endif
}

// Decrypt with transformed intermediate keys without retaining a second secret schedule.
GD_AES_TARGET inline void decrypt(const uint8_t *keys, unsigned rounds, const uint8_t *input, uint8_t *output) {
#ifdef GD_AES_X86
	__m128i block = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(input)), _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys + rounds * 16)));
	for (unsigned round = rounds - 1; round != 0; --round) block = _mm_aesdec_si128(block, _mm_aesimc_si128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(keys + round * 16))));
	block = _mm_aesdeclast_si128(block, _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys)));
	_mm_storeu_si128(reinterpret_cast<__m128i *>(output), block);
#else
	const uint8x16_t zero = vdupq_n_u8(0);
	uint8x16_t block = veorq_u8(vld1q_u8(input), vld1q_u8(keys + rounds * 16));
	for (unsigned round = rounds - 1; round != 0; --round) {
		block = veorq_u8(vaesdq_u8(block, zero), vld1q_u8(keys + round * 16));
		block = vaesimcq_u8(block);
	}
	vst1q_u8(output, veorq_u8(vaesdq_u8(block, zero), vld1q_u8(keys)));
#endif
}

// Return the four limbs of a polynomial product using three carry-less multiplications.
GD_MUL_TARGET inline void product(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t &x, uint64_t &y, uint64_t &h, uint64_t &j) {
#ifdef GD_AES_X86
	const __m128i left = _mm_set_epi64x(b, a), right = _mm_set_epi64x(d, c);
	const __m128i low = _mm_clmulepi64_si128(left, right, 0x00), high = _mm_clmulepi64_si128(left, right, 0x11);
	const __m128i middle = _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(_mm_set_epi64x(0, a ^ b), _mm_set_epi64x(0, c ^ d), 0), low), high);
	uint64_t lo[2], hi[2], mid[2];
	_mm_storeu_si128(reinterpret_cast<__m128i *>(lo), low);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(hi), high);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(mid), middle);
	x = lo[0]; y = lo[1] ^ mid[0]; h = hi[0] ^ mid[1]; j = hi[1];
#else
	const uint64x2_t low = vreinterpretq_u64_p128(vmull_p64(a, c)), high = vreinterpretq_u64_p128(vmull_p64(b, d));
	const uint64x2_t middle = veorq_u64(veorq_u64(vreinterpretq_u64_p128(vmull_p64(a ^ b, c ^ d)), low), high);
	x = vgetq_lane_u64(low, 0); y = vgetq_lane_u64(low, 1) ^ vgetq_lane_u64(middle, 0);
	h = vgetq_lane_u64(high, 0) ^ vgetq_lane_u64(middle, 1); j = vgetq_lane_u64(high, 1);
#endif
}
#endif
}
