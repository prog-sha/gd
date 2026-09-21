// Convert hexadecimal blocks directly between bytes and native code points.
// Complete blocks use baseline vectors; tails and invalid blocks retain exact scalar errors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace GDHex {

// Decode a full code point, rejecting non-ASCII values before any narrowing.
inline int digit(char32_t c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Write exactly two lowercase code points per byte into caller-reserved storage.
inline void encode(const uint8_t *src, char32_t *dst, size_t size) {
	size_t at = 0;
#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
	const __m128i zero = _mm_setzero_si128();
	for (; size - at >= 8; at += 8) {
		const __m128i bytes = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(src + at));
		const __m128i lo = _mm_and_si128(bytes, _mm_set1_epi8(15));
		const __m128i hi = _mm_and_si128(_mm_srli_epi16(bytes, 4), _mm_set1_epi8(15));
		const __m128i n = _mm_unpacklo_epi8(hi, lo);
		const __m128i chars = _mm_add_epi8(_mm_add_epi8(n, _mm_set1_epi8('0')), _mm_and_si128(_mm_cmpgt_epi8(n, _mm_set1_epi8(9)), _mm_set1_epi8('a' - '0' - 10)));
		const __m128i a = _mm_unpacklo_epi8(chars, zero), b = _mm_unpackhi_epi8(chars, zero);
		_mm_storeu_si128(reinterpret_cast<__m128i *>(dst + at * 2), _mm_unpacklo_epi16(a, zero));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(dst + at * 2 + 4), _mm_unpackhi_epi16(a, zero));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(dst + at * 2 + 8), _mm_unpacklo_epi16(b, zero));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(dst + at * 2 + 12), _mm_unpackhi_epi16(b, zero));
	}
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
	for (; size - at >= 8; at += 8) {
		const uint8x8_t bytes = vld1_u8(src + at);
		const uint8x8x2_t pair = vzip_u8(vshr_n_u8(bytes, 4), vand_u8(bytes, vdup_n_u8(15)));
		const uint8x16_t n = vcombine_u8(pair.val[0], pair.val[1]);
		const uint8x16_t chars = vaddq_u8(vaddq_u8(n, vdupq_n_u8('0')), vandq_u8(vcgtq_u8(n, vdupq_n_u8(9)), vdupq_n_u8('a' - '0' - 10)));
		const uint16x8_t a = vmovl_u8(vget_low_u8(chars)), b = vmovl_u8(vget_high_u8(chars));
		const uint32x4_t words[] = {vmovl_u16(vget_low_u16(a)), vmovl_u16(vget_high_u16(a)), vmovl_u16(vget_low_u16(b)), vmovl_u16(vget_high_u16(b))};
		memcpy(dst + at * 2, words, sizeof(words));
	}
#endif
	// Short inputs avoid vector setup and never require a padded source buffer.
	constexpr char table[] = "0123456789abcdef"; // Canonical lowercase alphabet.
	for (; at < size; at++) {
		dst[at * 2] = table[src[at] >> 4];
		dst[at * 2 + 1] = table[src[at] & 15];
	}
}

#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
// Validate four full code points and select their numeric values without truncation.
inline bool digits(__m128i c, __m128i &out) {
	const __m128i lower = _mm_or_si128(c, _mm_set1_epi32(0x20));
	const __m128i num = _mm_and_si128(_mm_cmpgt_epi32(c, _mm_set1_epi32('0' - 1)), _mm_cmpgt_epi32(_mm_set1_epi32('9' + 1), c));
	const __m128i alpha = _mm_and_si128(_mm_cmpgt_epi32(lower, _mm_set1_epi32('a' - 1)), _mm_cmpgt_epi32(_mm_set1_epi32('f' + 1), lower));
	if (_mm_movemask_epi8(_mm_or_si128(num, alpha)) != 0xffff) return false;
	out = _mm_or_si128(_mm_and_si128(num, _mm_sub_epi32(c, _mm_set1_epi32('0'))), _mm_and_si128(alpha, _mm_sub_epi32(lower, _mm_set1_epi32('a' - 10))));
	return true;
}
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
// Validate four full code points and select their numeric values without truncation.
inline bool digits(uint32x4_t c, uint32x4_t &out) {
	const uint32x4_t lower = vorrq_u32(c, vdupq_n_u32(0x20));
	const uint32x4_t num = vandq_u32(vcgeq_u32(c, vdupq_n_u32('0')), vcleq_u32(c, vdupq_n_u32('9')));
	const uint32x4_t alpha = vandq_u32(vcgeq_u32(lower, vdupq_n_u32('a')), vcleq_u32(lower, vdupq_n_u32('f')));
	if (vminvq_u32(vorrq_u32(num, alpha)) != UINT32_MAX) return false;
	out = vbslq_u32(num, vsubq_u32(c, vdupq_n_u32('0')), vsubq_u32(lower, vdupq_n_u32('a' - 10)));
	return true;
}
#endif

// Decode an even character count, returning the first invalid pair or the full count.
inline size_t decode(const char32_t *src, uint8_t *dst, size_t size) {
	size_t at = 0;
#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
	for (; size - at >= 8; at += 8) {
		__m128i a, b;
		if (!digits(_mm_loadu_si128(reinterpret_cast<const __m128i *>(src + at)), a) || !digits(_mm_loadu_si128(reinterpret_cast<const __m128i *>(src + at + 4)), b)) break;
		const __m128i pairs = _mm_madd_epi16(_mm_packs_epi32(a, b), _mm_set1_epi32(0x00010010));
		const __m128i halves = _mm_packs_epi32(pairs, pairs);
		const uint32_t bytes = _mm_cvtsi128_si32(_mm_packus_epi16(halves, halves));
		memcpy(dst + at / 2, &bytes, sizeof(bytes));
	}
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
	for (; size - at >= 8; at += 8) {
		uint32x4_t a, b;
		memcpy(&a, src + at, sizeof(a));
		memcpy(&b, src + at + 4, sizeof(b));
		if (!digits(a, a) || !digits(b, b)) break;
		const uint32x4_t pairs = vorrq_u32(vshlq_n_u32(vuzp1q_u32(a, b), 4), vuzp2q_u32(a, b));
		const uint16x4_t halves = vmovn_u32(pairs);
		uint8_t bytes[8];
		vst1_u8(bytes, vmovn_u16(vcombine_u16(halves, halves)));
		memcpy(dst + at / 2, bytes, 4);
	}
#endif
	// Revisit only an invalid block scalarly to preserve pair-aligned error positions.
	for (; size - at >= 2; at += 2) {
		const int hi = digit(src[at]), lo = digit(src[at + 1]);
		if (hi < 0 || lo < 0) break;
		dst[at / 2] = uint8_t(hi * 16 + lo);
	}
	return at;
}

} // namespace GDHex
