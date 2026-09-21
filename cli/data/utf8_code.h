// Encode native codepoint byte representations without allocation or scheduling.
#pragma once

#include <cstdint>

namespace GDUtf8 {

// Count bytes, including the replacement for values outside the representable range.
inline int width(uint32_t p_c) {
	return p_c <= 0x7f ? 1 : p_c <= 0x7ff ? 2 : p_c <= 0xffff ? 3 : p_c <= 0x1fffff ? 4 : p_c <= 0x3ffffff ? 5 : p_c <= 0x7fffffff ? 6 : 3;
}

// Write one codepoint into storage reserved by width(), returning the byte count.
inline int encode(uint32_t p_c, uint8_t *p_out) {
	if (p_c <= 0x7f) {
		*p_out = p_c;
		return 1;
	}
	if (p_c > 0x7fffffff) p_c = 0xfffd;
	const int n = width(p_c);
	p_out[0] = (0xffu << (8 - n)) | (p_c >> (6 * (n - 1)));
	for (int i = 1; i < n; i++) p_out[i] = 0x80 | ((p_c >> (6 * (n - i - 1))) & 0x3f);
	return n;
}

} // namespace GDUtf8
