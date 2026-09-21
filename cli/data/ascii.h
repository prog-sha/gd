// Encode ASCII from UTF-32 with portable and baseline SIMD instructions.
#pragma once
#include <cstddef>
#include <cstdint>

#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace GDUtf8 {
// Write one byte per scalar only when every input code point is ASCII.
inline bool ascii(const char32_t *src, uint8_t *dst, size_t length) {
	size_t i = 0;
#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
	const __m128i mask = _mm_set1_epi32(~0x7f); // Bits forbidden in an ASCII code point.
	for (; length - i >= 16; i += 16) {
		const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
		const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i + 4));
		const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i + 8));
		const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i + 12));
		const __m128i bits = _mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, d));
		if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(bits, mask), _mm_setzero_si128())) != 0xffff) return false;
		_mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_packus_epi16(_mm_packs_epi32(a, b), _mm_packs_epi32(c, d)));
	}
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
	for (; length - i >= 16; i += 16) {
		const uint32x4_t a = vld1q_u32(reinterpret_cast<const uint32_t *>(src + i));
		const uint32x4_t b = vld1q_u32(reinterpret_cast<const uint32_t *>(src + i + 4));
		const uint32x4_t c = vld1q_u32(reinterpret_cast<const uint32_t *>(src + i + 8));
		const uint32x4_t d = vld1q_u32(reinterpret_cast<const uint32_t *>(src + i + 12));
		if (vmaxvq_u32(vorrq_u32(vorrq_u32(a, b), vorrq_u32(c, d))) > 0x7f) return false;
		const uint16x8_t lo = vcombine_u16(vmovn_u32(a), vmovn_u32(b));
		const uint16x8_t hi = vcombine_u16(vmovn_u32(c), vmovn_u32(d));
		vst1q_u8(dst + i, vcombine_u8(vmovn_u16(lo), vmovn_u16(hi)));
	}
#endif
	// Preserve short strings and tails without reading beyond the input range.
	for (; i < length; ++i) {
		if (src[i] > 0x7f) return false;
		dst[i] = uint8_t(src[i]);
	}
	return true;
}
} // namespace GDUtf8
