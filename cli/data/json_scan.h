// Copy ordinary JSON string spans without crossing their input or output bounds.
// Block classification removes repeated escape branches on ordinary text; exceptions
// return to the shared scalar validator instead of introducing a permissive parser.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace GDJson {

// Detect an equal 32-bit lane; cross-lane borrow cannot hide the first match.
inline bool same32(uint64_t p_word, uint32_t p_char) {
	const uint64_t word = p_word ^ (uint64_t(p_char) * 0x100000001ULL);
	return ((word - 0x100000001ULL) & ~word & 0x8000000080000000ULL) != 0;
}

// Narrow a safe ASCII prefix, leaving escapes and Unicode to the scalar encoder.
inline size_t ascii(const char32_t *p_src, uint8_t *p_dst, size_t p_size, bool p_html) {
	size_t at = 0;
#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
	for (; p_size - at >= 4; at += 4) {
		const __m128i chars = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p_src + at));
		__m128i bad = _mm_or_si128(_mm_cmplt_epi32(chars, _mm_set1_epi32(0x20)), _mm_cmpgt_epi32(chars, _mm_set1_epi32(0x7f)));
		bad = _mm_or_si128(bad, _mm_or_si128(_mm_cmpeq_epi32(chars, _mm_set1_epi32('"')), _mm_cmpeq_epi32(chars, _mm_set1_epi32('\\'))));
		if (p_html) {
			bad = _mm_or_si128(bad, _mm_or_si128(_mm_cmpeq_epi32(chars, _mm_set1_epi32('<')), _mm_cmpeq_epi32(chars, _mm_set1_epi32('>'))));
			bad = _mm_or_si128(bad, _mm_cmpeq_epi32(chars, _mm_set1_epi32('&')));
		}
		if (_mm_movemask_epi8(bad)) break;
		const __m128i halves = _mm_packs_epi32(chars, chars);
		const uint32_t bytes = _mm_cvtsi128_si32(_mm_packus_epi16(halves, halves));
		memcpy(p_dst + at, &bytes, sizeof(bytes));
	}
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
	// Validate full code points before narrowing; high bits must never disappear.
	for (; p_size - at >= 4; at += 4) {
		uint32x4_t chars;
		memcpy(&chars, p_src + at, sizeof(chars));
		uint32x4_t bad = vorrq_u32(vcltq_u32(chars, vdupq_n_u32(0x20)), vcgtq_u32(chars, vdupq_n_u32(0x7f)));
		bad = vorrq_u32(bad, vorrq_u32(vceqq_u32(chars, vdupq_n_u32('"')), vceqq_u32(chars, vdupq_n_u32('\\'))));
		if (p_html) {
			bad = vorrq_u32(bad, vorrq_u32(vceqq_u32(chars, vdupq_n_u32('<')), vceqq_u32(chars, vdupq_n_u32('>'))));
			bad = vorrq_u32(bad, vceqq_u32(chars, vdupq_n_u32('&')));
		}
		if (vmaxvq_u32(bad)) break;
		const uint16x4_t halves = vmovn_u32(chars);
		const uint8x8_t bytes = vmovn_u16(vcombine_u16(halves, halves));
		// Copy only four lanes; the following output bytes may not exist.
		uint8_t packed[8];
		vst1_u8(packed, bytes);
		memcpy(p_dst + at, packed, 4);
	}
#endif
	// Paired lanes also work without SIMD; memcpy permits unaligned, alias-safe loads.
	for (; p_size - at >= 2; at += 2) {
		uint64_t word;
		memcpy(&word, p_src + at, sizeof(word));
		if ((word & 0xffffff80ffffff80ULL) || ((word - 0x2000000020ULL) & 0x8000000080000000ULL) || same32(word, '"') || same32(word, '\\')) break;
		if (p_html && (same32(word, '<') || same32(word, '>') || same32(word, '&'))) break;
		p_dst[at] = uint8_t(p_src[at]);
		p_dst[at + 1] = uint8_t(p_src[at + 1]);
	}
	// Finish a partial block exactly at its first exceptional character.
	for (; at < p_size; at++) {
		const char32_t c = p_src[at];
		if (c < 0x20 || c > 0x7f || c == '"' || c == '\\' || (p_html && (c == '<' || c == '>' || c == '&'))) break;
		p_dst[at] = uint8_t(c);
	}
	return at;
}

// Detect an equal byte without relying on alignment or byte order.
inline bool same8(uint64_t p_word, uint8_t p_char) {
	const uint64_t word = p_word ^ (uint64_t(p_char) * 0x0101010101010101ULL);
	return ((word - 0x0101010101010101ULL) & ~word & 0x8080808080808080ULL) != 0;
}

// Widen a validated ASCII prefix; quotes, escapes and malformed bytes remain unread.
inline size_t ascii(const uint8_t *p_src, char32_t *p_dst, size_t p_size) {
	size_t at = 0;
#if !defined(GD_UTF8_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
	const __m128i zero = _mm_setzero_si128();
	for (; p_size - at >= 16; at += 16) {
		const __m128i chars = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p_src + at));
		const __m128i bad = _mm_or_si128(_mm_cmplt_epi8(chars, _mm_set1_epi8(0x20)), _mm_or_si128(_mm_cmpeq_epi8(chars, _mm_set1_epi8('"')), _mm_cmpeq_epi8(chars, _mm_set1_epi8('\\'))));
		if (_mm_movemask_epi8(bad)) break;
		const __m128i lo = _mm_unpacklo_epi8(chars, zero);
		const __m128i hi = _mm_unpackhi_epi8(chars, zero);
		_mm_storeu_si128(reinterpret_cast<__m128i *>(p_dst + at), _mm_unpacklo_epi16(lo, zero));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(p_dst + at + 4), _mm_unpackhi_epi16(lo, zero));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(p_dst + at + 8), _mm_unpacklo_epi16(hi, zero));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(p_dst + at + 12), _mm_unpackhi_epi16(hi, zero));
	}
#elif !defined(GD_UTF8_SCALAR) && defined(__aarch64__)
	for (; p_size - at >= 16; at += 16) {
		const uint8x16_t chars = vld1q_u8(p_src + at);
		uint8x16_t bad = vorrq_u8(vcltq_u8(chars, vdupq_n_u8(0x20)), vcgtq_u8(chars, vdupq_n_u8(0x7f)));
		bad = vorrq_u8(bad, vorrq_u8(vceqq_u8(chars, vdupq_n_u8('"')), vceqq_u8(chars, vdupq_n_u8('\\'))));
		if (vmaxvq_u8(bad)) break;
		const uint16x8_t lo = vmovl_u8(vget_low_u8(chars));
		const uint16x8_t hi = vmovl_u8(vget_high_u8(chars));
		const uint32x4_t words[] = {vmovl_u16(vget_low_u16(lo)), vmovl_u16(vget_high_u16(lo)), vmovl_u16(vget_low_u16(hi)), vmovl_u16(vget_high_u16(hi))};
		memcpy(p_dst + at, words, sizeof(words));
	}
#endif
	for (; p_size - at >= 8; at += 8) {
		uint64_t word;
		memcpy(&word, p_src + at, sizeof(word));
		if ((word & 0x8080808080808080ULL) || ((word - 0x2020202020202020ULL) & 0x8080808080808080ULL) || same8(word, '"') || same8(word, '\\')) break;
		for (size_t i = 0; i < 8; i++) p_dst[at + i] = p_src[at + i];
	}
	for (; at < p_size; at++) {
		const uint8_t c = p_src[at];
		if (c < 0x20 || c > 0x7f || c == '"' || c == '\\') break;
		p_dst[at] = c;
	}
	return at;
}

} // namespace GDJson
