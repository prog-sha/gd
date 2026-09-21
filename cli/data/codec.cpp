/**************************************************************************/
/*  codec.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement byte encoding, packing, and hashing declared in codec.h.

#include "cli/data/codec.h"
#include "cli/data/hex_scan.h"
#include "cli/data/utf8.h"
#include "cli/sys/pool.h"

#include "cli/api/text.h"
#include "cli/sys/mount.h"
#include "cli/sys/os.h"
#include "cli/sys/source_error.h"

#include "cli/sys/native_file.h"
#include "core/templates/hash_set.h"

#include <thirdparty/mbedtls/include/mbedtls/hkdf.h>
#include <thirdparty/mbedtls/include/mbedtls/md.h>
#include <thirdparty/mbedtls/include/mbedtls/pkcs5.h>

#ifdef MACOS_ENABLED
#include <CoreFoundation/CoreFoundation.h>
#elif defined(WINDOWS_ENABLED)
#include <windows.h>
#include <winternl.h>
#endif

namespace {

const char *B32_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"; // RFC 4648

// Decode one base64 character using the caller-selected alphabet.
int b64_val(char32_t p_c, char32_t p_c62, char32_t p_c63) {
	if (p_c >= 'A' && p_c <= 'Z') {
		return p_c - 'A';
	}
	if (p_c >= 'a' && p_c <= 'z') {
		return p_c - 'a' + 26;
	}
	if (p_c >= '0' && p_c <= '9') {
		return p_c - '0' + 52;
	}
	return p_c == p_c62 ? 62 : 63;
}

// Decode complete quanta while retaining the prefix preceding malformed input.
// Identity-sensitive decoding forbids padding, whitespace, and nonzero spare bits.
Ref<R> decode_b64(const String &p_text, bool p_url, bool p_raw, bool p_identity = false) {
	const char32_t c62 = p_url ? '-' : '+'; // Alphabet value 62.
	const char32_t c63 = p_url ? '_' : '/'; // Alphabet value 63.
	PackedByteArray out;
	if (out.resize((int64_t(p_text.length()) + 3) / 4 * 3) != OK) return R::err("cannot allocate base64 output", Err::LIMITED);
	uint8_t *w = out.ptrw();
	int at = 0, count = 0;
	uint32_t bits = 0;
	// Commit bytes only when the current quantum is complete.
	auto put = [&]() {
		for (int n = count * 6 - 8; n >= 0; n -= 8) w[at++] = uint8_t(bits >> n);
	};
	auto fail = [&](int pos) {
		out.resize(at);
		return R::err(vformat("invalid base64 at %d", pos), Err::INVALID_DATA, out);
	};
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		if (!p_identity && (c == '\r' || c == '\n')) continue;
		if (c == '=') {
			if (p_raw || count < 2) return fail(i);
			int need = 3 - count; // Remaining padding after this character.
			while (need && ++i < p_text.length()) {
				if (p_text[i] == '\r' || p_text[i] == '\n') continue;
				if (p_text[i] != '=') return fail(i);
				need--;
			}
			if (need) return fail(i);
			put();
			while (++i < p_text.length()) {
				if (p_text[i] != '\r' && p_text[i] != '\n') return fail(i);
			}
			out.resize(at);
			return R::ok(out);
		}
		if (c != c62 && c != c63 && !is_ascii_alphanumeric_char(c)) return fail(i);
		bits = (bits << 6) | b64_val(c, c62, c63);
		if (++count == 4) { put(); count = 0; bits = 0; }
	}
	if (count == 1 || (!p_raw && count) || (p_identity && count && (bits & ((1U << (count * 6 % 8)) - 1)))) return fail(p_text.length());
	put();
	out.resize(at);
	return R::ok(out);
}

// Append an integer most-significant byte first.
void put_be(LocalVector<uint8_t> &out, uint64_t n, int width) {
	for (int i = width - 1; i >= 0; i--) {
		out.push_back((uint8_t)((n >> (i * 8)) & 0xff));
	}
}

// Convert an ordinary array to bytes.
PackedByteArray to_packed(const LocalVector<uint8_t> &v) {
	PackedByteArray out;
	out.resize(v.size());
	if (v.size() > 0) {
		memcpy(out.ptrw(), v.ptr(), v.size());
	}
	return out;
}

} // namespace

// ---------------- Encodings ----------------

// Encode directly into native text storage without a temporary byte string.
String Encoding::hex_encode(const PackedByteArray &p_data) {
	const int64_t n = p_data.size();
	String out;
	// Native text counts include the terminator and must fit the signed int API.
	if (n > (INT_MAX - 1) / 2 || out.resize_uninitialized(n * 2 + 1) != OK) return String();
	char32_t *w = out.ptrw();
	GDHex::encode(p_data.ptr(), w, n);
	w[n * 2] = 0;
	return out;
}

// Decode hexadecimal text into bytes.
Ref<R> Encoding::hex_decode(const String &p_text) {
	const int n = p_text.length();
	PackedByteArray out;
	if (out.resize(n / 2) != OK) return R::err("cannot allocate hex output", Err::LIMITED);
	const size_t at = GDHex::decode(p_text.ptr(), out.ptrw(), n - n % 2);
	if (at != size_t(n - n % 2) || n % 2) {
		out.resize(at / 2);
		return R::err(vformat("invalid hex at %d", at), Err::INVALID_DATA, out);
	}
	return R::ok(out);
}

// Encode bytes as standard base64.
String Encoding::base64_encode(const PackedByteArray &p_data) {
	const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; // Standard RFC 4648 alphabet.
	CharString out;
	const int64_t size = ((int64_t(p_data.size()) + 2) / 3) * 4;
	if (size > INT_MAX - 1 || out.resize_uninitialized(size + 1) != OK) return String();
	char *dst = out.ptrw();
	for (int64_t i = 0, at = 0; i < p_data.size(); i += 3) {
		const uint32_t value = uint32_t(p_data[i]) << 16 | (i + 1 < p_data.size() ? uint32_t(p_data[i + 1]) << 8 : 0) |
				(i + 2 < p_data.size() ? p_data[i + 2] : 0);
		dst[at++] = table[value >> 18]; dst[at++] = table[(value >> 12) & 63];
		dst[at++] = i + 1 < p_data.size() ? table[(value >> 6) & 63] : '=';
		dst[at++] = i + 2 < p_data.size() ? table[value & 63] : '=';
	}
	dst[size] = 0;
	return String::ascii(Span<char>(dst, size));
}

// Decode standard base64 text.
Ref<R> Encoding::base64_decode(const String &p_text, bool p_raw) {
	return decode_b64(p_text, false, p_raw);
}

// Encode bytes as URL-safe base64.
String Encoding::base64url_encode(const PackedByteArray &p_data) {
	return base64_encode(p_data).replace("+", "-").replace("/", "_").rstrip("=");
}

// Decode URL-safe base64 text.
Ref<R> Encoding::base64url_decode(const String &p_text, bool p_raw) {
	return decode_b64(p_text, true, p_raw);
}

// Decode canonical unpadded base64url.
// Text used as an identity key, such as a JWT, must not accept multiple equivalent spellings.
// Alternative spellings would bypass comparisons or revocation records.
Ref<R> Encoding::base64url_raw_decode(const String &p_text) {
	return decode_b64(p_text, true, true, true);
}

// Encode bytes as base32.
String Encoding::base32_encode(const PackedByteArray &p_data) {
	String out;
	uint32_t acc = 0; // Accumulated input bits.
	int bits = 0; // Number of accumulated bits.
	const int n = p_data.size();
	const uint8_t *r = p_data.ptr();
	LocalVector<char32_t> buf;
	for (int i = 0; i < n; i++) {
		acc = (acc << 8) | r[i];
		bits += 8;
		while (bits >= 5) {
			bits -= 5;
			buf.push_back(B32_TABLE[(acc >> bits) & 31]);
		}
	}
	if (bits > 0) {
		buf.push_back(B32_TABLE[(acc << (5 - bits)) & 31]);
	}
	while (buf.size() % 8 != 0) {
		buf.push_back('=');
	}
	out.resize_uninitialized(buf.size() + 1);
	char32_t *w = out.ptrw();
	for (uint32_t i = 0; i < buf.size(); i++) {
		w[i] = buf[i];
	}
	w[buf.size()] = 0;
	return out;
}

// Decode base32 text into bytes.
Ref<R> Encoding::base32_decode(const String &p_text, bool p_raw) {
	PackedByteArray out;
	if (out.resize((int64_t(p_text.length()) + 7) / 8 * 5) != OK) return R::err("cannot allocate base32 output", Err::LIMITED);
	uint8_t *w = out.ptrw();
	uint64_t bits = 0;
	int count = 0, at = 0;
	// Preserve completed quanta and publish the last one only after padding validation.
	auto put = [&]() {
		for (int n = count * 5 - 8; n >= 0; n -= 8) w[at++] = uint8_t(bits >> n);
	};
	auto valid = [&]() { return count == 0 || count == 2 || count == 4 || count == 5 || count == 7; };
	auto fail = [&](int pos) {
		out.resize(at);
		return R::err(vformat("invalid base32 at %d", pos), Err::INVALID_DATA, out);
	};
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (c == '\r' || c == '\n') continue;
		if (c == '=') {
			if (p_raw || !count || !valid()) return fail(i);
			// Padding belongs to the terminal quantum only; count encoded bytes without line breaks.
			int tail = 0;
			for (int j = i + 1; j < p_text.length() && tail < 8; j++) {
				if (p_text[j] != '\r' && p_text[j] != '\n') tail += GDUtf8::width(p_text[j]);
			}
			if (tail >= 8) return fail(i);
			int need = 7 - count; // Remaining padding after this character.
			while (need && ++i < p_text.length()) {
				if (p_text[i] == '\r' || p_text[i] == '\n') continue;
				if (p_text[i] != '=') return fail(i);
				need--;
			}
			if (need) return fail(i);
			put();
			out.resize(at);
			return R::ok(out);
		}
		if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return fail(i);
		bits = (bits << 5) | (c >= 'A' ? c - 'A' : c - '2' + 26);
		if (++count == 8) { put(); count = 0; bits = 0; }
	}
	if (!p_raw && count) return fail(p_text.length());
	if (valid()) put();
	out.resize(at);
	return R::ok(out);
}

// Encode an integer as a varint.
PackedByteArray Encoding::varint_encode(int64_t p_n) {
	LocalVector<uint8_t> out;
	uint64_t v = (uint64_t)p_n;
	while (true) {
		const uint8_t b = (uint8_t)(v & 127);
		v >>= 7;
		if (v != 0) {
			out.push_back(b | 128);
		} else {
			out.push_back(b);
			break;
		}
	}
	return to_packed(out);
}

// Decode a varint at the supplied byte offset.
Ref<R> Encoding::varint_decode(const PackedByteArray &p_data, int p_at) {
	if (p_at < 0 || p_at > p_data.size()) return R::err("varint offset is outside the input", Err::INVALID_DATA);
	uint64_t v = 0;
	int shift = 0;
	int i = p_at;
	const int n = p_data.size();
	const uint8_t *r = p_data.ptr();
	while (i < n) {
		const uint8_t b = r[i];
		if (shift == 63 && b > 1) return R::err("varint overflows 64 bits", Err::INVALID_DATA);
		v |= (uint64_t)(b & 127) << shift;
		i++;
		if ((b & 128) == 0) {
			Dictionary box;
			box["value"] = (int64_t)v;
			box["next"] = i;
			return R::ok(box);
		}
		shift += 7;
		if (shift > 63) {
			return R::err("varint too long", Err::INVALID_DATA);
		}
	}
	return R::err("varint truncated", Err::INVALID_DATA);
}

// ---------------- Byte sequences ----------------

PackedByteArray Bytes::concat(const Array &p_parts) {
	// Accumulate total length in 64 bits before allocating.
	// An overflowing int total could allocate too little and allow an out-of-bounds copy.
	int64_t total = 0;
	for (int i = 0; i < p_parts.size(); i++) {
		const PackedByteArray b = p_parts[i];
		total += b.size();
	}
	PackedByteArray out;
	if (out.resize(total) != OK) {
		return PackedByteArray(); // Do not access storage after allocation failure.
	}
	uint8_t *w = out.ptrw();
	int64_t at = 0;
	for (int i = 0; i < p_parts.size(); i++) {
		const PackedByteArray b = p_parts[i];
		if (b.size() > 0) {
			memcpy(w + at, b.ptr(), b.size());
			at += b.size();
		}
	}
	return out;
}

// Find the first occurrence of a byte subsequence.
int Bytes::index_of(const PackedByteArray &p_hay, const PackedByteArray &p_needle, int p_from) {
	const int hn = p_hay.size();
	const int nn = p_needle.size();
	if (nn == 0) {
		return p_from;
	}
	if (nn > hn) {
		return -1;
	}
	const uint8_t *h = p_hay.ptr();
	const uint8_t *nd = p_needle.ptr();
	for (int at = MAX(p_from, 0); at <= hn - nn; at++) {
		const uint8_t *hit = (const uint8_t *)memchr(h + at, nd[0], hn - nn - at + 1);
		if (!hit) {
			return -1;
		}
		at = (int)(hit - h);
		if (memcmp(h + at, nd, nn) == 0) {
			return at;
		}
	}
	return -1;
}

// Find the last occurrence of a byte subsequence.
int Bytes::last_index_of(const PackedByteArray &p_hay, const PackedByteArray &p_needle) {
	const int hn = p_hay.size();
	const int nn = p_needle.size();
	if (nn > hn) {
		return -1;
	}
	if (nn == 0) {
		return hn; // An empty pattern matches at the end without accessing storage.
	}
	const uint8_t *h = p_hay.ptr();
	const uint8_t *nd = p_needle.ptr();
	for (int at = hn - nn; at >= 0; at--) {
		if (memcmp(h + at, nd, nn) == 0) {
			return at;
		}
	}
	return -1;
}

// Check whether bytes begin with the selected prefix.
bool Bytes::starts_with(const PackedByteArray &p_hay, const PackedByteArray &p_prefix) {
	if (p_prefix.size() > p_hay.size()) {
		return false;
	}
	if (p_prefix.is_empty()) {
		return true; // An empty prefix matches without dereferencing its null storage.
	}
	return memcmp(p_hay.ptr(), p_prefix.ptr(), p_prefix.size()) == 0;
}

// Check whether bytes end with the selected suffix.
bool Bytes::ends_with(const PackedByteArray &p_hay, const PackedByteArray &p_suffix) {
	if (p_suffix.size() > p_hay.size()) {
		return false;
	}
	if (p_suffix.is_empty()) {
		return true;
	}
	return memcmp(p_hay.ptr() + (p_hay.size() - p_suffix.size()), p_suffix.ptr(), p_suffix.size()) == 0;
}

// Repeat a byte sequence the requested number of times.
PackedByteArray Bytes::repeat(const PackedByteArray &p_src, int p_times) {
	const int64_t t = MAX(p_times, 0);
	PackedByteArray out;
	// Check allocation success before obtaining a pointer.
	if (out.resize(p_src.size() * t) != OK) {
		return PackedByteArray();
	}
	uint8_t *w = out.ptrw();
	for (int64_t i = 0; i < t; i++) {
		memcpy(w + i * p_src.size(), p_src.ptr(), p_src.size());
	}
	return out;
}

// Truncate bytes to a length or extend with zeros.
PackedByteArray Bytes::fit(const PackedByteArray &p_src, int p_size) {
	PackedByteArray out;
	out.resize(MAX(p_size, 0));
	uint8_t *w = out.ptrw();
	memset(w, 0, out.size());
	const int n = MIN(out.size(), p_src.size());
	if (n > 0) {
		memcpy(w, p_src.ptr(), n);
	}
	return out;
}

// Split bytes at each delimiter sequence.
Array Bytes::split(const PackedByteArray &p_src, const PackedByteArray &p_sep) {
	Array out;
	if (p_sep.is_empty()) {
		out.push_back(p_src);
		return out;
	}
	int at = 0;
	while (true) {
		const int hit = index_of(p_src, p_sep, at);
		if (hit < 0) {
			out.push_back(p_src.slice(at));
			break;
		}
		out.push_back(p_src.slice(at, hit));
		at = hit + p_sep.size();
	}
	return out;
}

// Compare two byte sequences for equality.
bool Bytes::equals(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	return p_a == p_b;
}

// Check whether bytes contain a subsequence.
bool Bytes::includes(const PackedByteArray &p_hay, const PackedByteArray &p_needle) {
	return index_of(p_hay, p_needle, 0) >= 0;
}

// ---------------- MessagePack ----------------

namespace {

// Choose a length marker for short, 16-bit, or 32-bit sizes.
void mp_head(LocalVector<uint8_t> &out, int n, uint8_t fix, uint8_t m16, uint8_t m32) {
	if (n < 16) {
		out.push_back(fix | (uint8_t)n);
	} else if (n < 65536) {
		out.push_back(m16);
		put_be(out, n, 2);
	} else {
		out.push_back(m32);
		put_be(out, n, 4);
	}
}

// Encode a MessagePack integer.
void mp_int(LocalVector<uint8_t> &out, int64_t n) {
	if (n >= 0) {
		if (n < 128) {
			out.push_back((uint8_t)n);
		} else if (n < 256) {
			out.push_back(0xcc);
			out.push_back((uint8_t)n);
		} else if (n < 65536) {
			out.push_back(0xcd);
			put_be(out, n, 2);
		} else if (n < 4294967296LL) {
			out.push_back(0xce);
			put_be(out, n, 4);
		} else {
			out.push_back(0xcf);
			put_be(out, n, 8);
		}
		return;
	}
	if (n >= -32) {
		out.push_back((uint8_t)(0xe0 | (n + 32)));
	} else if (n >= -128) {
		out.push_back(0xd0);
		out.push_back((uint8_t)(n & 255));
	} else if (n >= -32768) {
		out.push_back(0xd1);
		put_be(out, (uint64_t)(n & 0xffff), 2);
	} else if (n >= -2147483648LL) {
		out.push_back(0xd2);
		put_be(out, (uint64_t)(n & 0xffffffffLL), 4);
	} else {
		out.push_back(0xd3);
		put_be(out, (uint64_t)n, 8);
	}
}

// Encode a MessagePack string.
void mp_str(LocalVector<uint8_t> &out, const String &s) {
	const CharString b = s.utf8();
	const int n = b.length();
	if (n < 32) {
		out.push_back((uint8_t)(0xa0 | n));
	} else if (n < 256) {
		out.push_back(0xd9);
		out.push_back((uint8_t)n);
	} else if (n < 65536) {
		out.push_back(0xda);
		put_be(out, n, 2);
	} else {
		out.push_back(0xdb);
		put_be(out, n, 4);
	}
	const int at = out.size();
	out.resize(at + n);
	memcpy(out.ptr() + at, b.get_data(), n);
}

// Encode one MessagePack value with cycle and depth checks.
bool mp_put(LocalVector<uint8_t> &out, const Variant &v, int depth, HashSet<const void *> &active) {
	if (depth > Variant::MAX_RECURSION_DEPTH) {
		return false;
	}
	switch (v.get_type()) {
		case Variant::OBJECT:
		case Variant::CALLABLE:
		case Variant::SIGNAL:
			return false; // Runtime behavior has no data representation in this format.
		case Variant::NIL:
			out.push_back(0xc0);
			return true;
		case Variant::BOOL:
			out.push_back(((bool)v) ? 0xc3 : 0xc2);
			return true;
		case Variant::INT:
			mp_int(out, (int64_t)v);
			return true;
		case Variant::FLOAT: {
			out.push_back(0xcb);
			const double d = (double)v;
			uint64_t bits;
			memcpy(&bits, &d, 8);
			put_be(out, bits, 8);
			return true;
		}
		case Variant::PACKED_BYTE_ARRAY: {
			const PackedByteArray b = v;
			const int n = b.size();
			if (n < 256) {
				out.push_back(0xc4);
				out.push_back((uint8_t)n);
			} else if (n < 65536) {
				out.push_back(0xc5);
				put_be(out, n, 2);
			} else {
				out.push_back(0xc6);
				put_be(out, n, 4);
			}
			const int at = out.size();
			out.resize(at + n);
			if (n > 0) {
				memcpy(out.ptr() + at, b.ptr(), n);
			}
			return true;
		}
		case Variant::ARRAY: {
			const Array a = v;
			if (active.has(a.id())) {
				return false;
			}
			active.insert(a.id());
			mp_head(out, a.size(), 0x90, 0xdc, 0xdd);
			for (int i = 0; i < a.size(); i++) {
				if (!mp_put(out, a[i], depth + 1, active)) {
					active.erase(a.id());
					return false;
				}
			}
			active.erase(a.id());
			return true;
		}
		case Variant::DICTIONARY: {
			const Dictionary d = v;
			if (active.has(d.id())) {
				return false;
			}
			active.insert(d.id());
			mp_head(out, d.size(), 0x80, 0xde, 0xdf);
			for (const KeyValue<Variant, Variant> &kv : d) {
				if (!mp_put(out, kv.key, depth + 1, active) || !mp_put(out, kv.value, depth + 1, active)) {
					active.erase(d.id());
					return false;
				}
			}
			active.erase(d.id());
			return true;
		}
		default:
			mp_str(out, Pool::text(v));
			return true;
	}
}

// Byte reader retaining its current position.
struct Reader {
	const uint8_t *b = nullptr;
	int n = 0;
	int at = 0;
	bool bad = false;
	String why;

	uint64_t be(int width) {
		uint64_t v = 0;
		for (int i = 0; i < width; i++) {
			if (at >= n) {
				bad = true;
				why = "truncated";
				return v;
			}
			v = (v << 8) | b[at++];
		}
		return v;
	}

	// Read the declared length and record truncation as malformed input.
	// Silently returning fewer bytes would accept a truncated container as valid.
	// Compare a 64-bit length directly against remaining input.
	// Adding length to an int position could overflow on str32 or bin32 and bypass validation.
	PackedByteArray take(int64_t len) {
		PackedByteArray out;
		if (len < 0 || len > (int64_t)n - at) {
			bad = true;
			why = "truncated";
			return out;
		}
		if (len > 0) {
			if (out.resize(len) != OK) {
				bad = true;
				why = "cannot allocate decoded bytes";
				return out;
			}
			memcpy(out.ptrw(), b + at, len);
		}
		at += (int)len;
		return out;
	}

	// Preserve arbitrary text bytes and apply format-specific UTF-8 validation.
	Variant text(int64_t p_size, bool p_strict = false) {
		const PackedByteArray bytes = take(p_size);
		if (bad) return Variant();
		const Variant value = utf8_value(bytes.ptr(), bytes.size(), p_strict);
		const Ref<R> error = value;
		if (error.is_valid()) {
			bad = true;
			why = error->get_e()->text();
			return Variant();
		}
		return value;
	}
};

// Restore the specified signed width from an unsigned value.
int64_t signed_of(uint64_t v, int bits) {
	const uint64_t top = (uint64_t)1 << (bits - 1);
	return v >= top ? (int64_t)(v - (top << 1)) : (int64_t)v;
}

Variant mp_get(Reader &r, int depth);

// Decode a MessagePack array.
Variant mp_arr(Reader &r, int n, int depth) {
	Array out;
	if (n < 0 || n > r.n - r.at) {
		r.bad = true;
		r.why = "array count exceeds input";
		return out;
	}
	for (int i = 0; i < n && !r.bad; i++) {
		out.push_back(mp_get(r, depth));
	}
	return out;
}

// Decode a MessagePack map.
Variant mp_map(Reader &r, int n, int depth) {
	Dictionary out;
	if (n < 0 || n > (r.n - r.at) / 2) {
		r.bad = true;
		r.why = "map count exceeds input";
		return out;
	}
	for (int i = 0; i < n && !r.bad; i++) {
		const Variant k = mp_get(r, depth);
		const Variant v = mp_get(r, depth);
		out[k] = v;
	}
	return out;
}

// Decode one MessagePack value.
Variant mp_get(Reader &r, int depth) {
	// Bound recursive nesting to prevent stack exhaustion.
	if (depth > Variant::MAX_RECURSION_DEPTH) {
		r.bad = true;
		r.why = "too deep";
		return Variant();
	}
	if (r.at >= r.n) {
		r.bad = true;
		r.why = "truncated";
		return Variant();
	}
	const uint8_t b = r.b[r.at++];
	if (b < 0x80) {
		return (int64_t)b;
	}
	if (b >= 0xe0) {
		return (int64_t)b - 256;
	}
	if ((b & 0xf0) == 0x80) {
		return mp_map(r, b & 15, depth + 1);
	}
	if ((b & 0xf0) == 0x90) {
		return mp_arr(r, b & 15, depth + 1);
	}
	if ((b & 0xe0) == 0xa0) {
		return r.text(b & 31);
	}
	switch (b) {
		case 0xc0:
			return Variant();
		case 0xc2:
			return false;
		case 0xc3:
			return true;
		case 0xc4:
			return r.take(r.be(1));
		case 0xc5:
			return r.take(r.be(2));
		case 0xc6:
			return r.take(r.be(4));
		case 0xca: {
			const uint32_t bits = (uint32_t)r.be(4);
			float f;
			memcpy(&f, &bits, 4);
			return (double)f;
		}
		case 0xcb: {
			const uint64_t bits = r.be(8);
			double d;
			memcpy(&d, &bits, 8);
			return d;
		}
		case 0xcc:
			return (int64_t)r.be(1);
		case 0xcd:
			return (int64_t)r.be(2);
		case 0xce:
			return (int64_t)r.be(4);
		case 0xcf: {
			const uint64_t n = r.be(8);
			if (n > INT64_MAX) {
				r.bad = true;
				r.why = "uint64 exceeds Variant integer";
				return Variant();
			}
			return (int64_t)n;
		}
		case 0xd0:
			return signed_of(r.be(1), 8);
		case 0xd1:
			return signed_of(r.be(2), 16);
		case 0xd2:
			return signed_of(r.be(4), 32);
		case 0xd3:
			return (int64_t)r.be(8);
		case 0xd9:
			return r.text(r.be(1));
		case 0xda:
			return r.text(r.be(2));
		case 0xdb:
			return r.text(r.be(4));
		case 0xdc:
			return mp_arr(r, (int)r.be(2), depth + 1);
		case 0xdd: {
			const uint64_t n = r.be(4);
			return mp_arr(r, n > INT_MAX ? -1 : (int)n, depth + 1);
		}
		case 0xde:
			return mp_map(r, (int)r.be(2), depth + 1);
		case 0xdf: {
			const uint64_t n = r.be(4);
			return mp_map(r, n > INT_MAX ? -1 : (int)n, depth + 1);
		}
	}
	r.bad = true;
	r.why = vformat("unknown tag 0x%02x", b);
	return Variant();
}

} // namespace

// Encode a value as MessagePack bytes.
Ref<R> Msgpack::encode(const Variant &p_v) {
	LocalVector<uint8_t> out;
	HashSet<const void *> active;
	if (!mp_put(out, p_v, 0, active)) {
		return R::err("unsupported value, cyclic data, or invalid nesting depth", Err::INVALID_DATA);
	}
	return R::ok(to_packed(out));
}

// Decode MessagePack bytes into a value.
Ref<R> Msgpack::decode(const PackedByteArray &p_data) {
	Reader r;
	r.b = p_data.ptr();
	r.n = p_data.size();
	const Variant v = mp_get(r, 0);
	if (r.bad || r.at != r.n) {
		if (!r.bad) {
			r.why = "trailing bytes";
		}
		return R::err(r.why, r.why.begins_with("unknown") ? Err::UNSUPPORTED : Err::INVALID_DATA);
	}
	return R::ok(v);
}

// ---------------- CBOR ----------------

namespace {

// CBOR major type stored in the high three bits.
enum CborKind {
	C_UINT = 0,
	C_NEGINT = 1,
	C_BYTES = 2,
	C_TEXT = 3,
	C_ARRAY = 4,
	C_MAP = 5,
	C_SIMPLE = 7,
};

// Encode a CBOR major-type and length header.
void cb_head(LocalVector<uint8_t> &out, int kind, uint64_t n) {
	const uint8_t top = (uint8_t)(kind << 5);
	if (n < 24) {
		out.push_back(top | (uint8_t)n);
	} else if (n < 256) {
		out.push_back(top | 24);
		out.push_back((uint8_t)n);
	} else if (n < 65536) {
		out.push_back(top | 25);
		put_be(out, n, 2);
	} else if (n < 4294967296ULL) {
		out.push_back(top | 26);
		put_be(out, n, 4);
	} else {
		out.push_back(top | 27);
		put_be(out, n, 8);
	}
}

// Encode one CBOR value with cycle and depth checks.
bool cb_put(LocalVector<uint8_t> &out, const Variant &v, int depth, HashSet<const void *> &active) {
	if (depth > Variant::MAX_RECURSION_DEPTH) {
		return false;
	}
	switch (v.get_type()) {
		case Variant::OBJECT:
		case Variant::CALLABLE:
		case Variant::SIGNAL:
			return false; // Runtime behavior has no data representation in this format.
		case Variant::NIL:
			out.push_back(0xf6);
			return true;
		case Variant::BOOL:
			out.push_back(((bool)v) ? 0xf5 : 0xf4);
			return true;
		case Variant::INT: {
			const int64_t n = (int64_t)v;
			if (n >= 0) {
				cb_head(out, C_UINT, (uint64_t)n);
			} else {
				cb_head(out, C_NEGINT, (uint64_t)(-1 - n)); // Encode negative integers as -1-n.
			}
			return true;
		}
		case Variant::FLOAT: {
			out.push_back(0xfb);
			const double d = (double)v;
			uint64_t bits;
			memcpy(&bits, &d, 8);
			put_be(out, bits, 8);
			return true;
		}
		case Variant::PACKED_BYTE_ARRAY: {
			const PackedByteArray b = v;
			cb_head(out, C_BYTES, b.size());
			const int at = out.size();
			out.resize(at + b.size());
			if (b.size() > 0) {
				memcpy(out.ptr() + at, b.ptr(), b.size());
			}
			return true;
		}
		case Variant::ARRAY: {
			const Array a = v;
			if (active.has(a.id())) {
				return false;
			}
			active.insert(a.id());
			cb_head(out, C_ARRAY, a.size());
			for (int i = 0; i < a.size(); i++) {
				if (!cb_put(out, a[i], depth + 1, active)) {
					active.erase(a.id());
					return false;
				}
			}
			active.erase(a.id());
			return true;
		}
		case Variant::DICTIONARY: {
			const Dictionary d = v;
			if (active.has(d.id())) {
				return false;
			}
			active.insert(d.id());
			cb_head(out, C_MAP, d.size());
			for (const KeyValue<Variant, Variant> &kv : d) {
				if (!cb_put(out, kv.key, depth + 1, active) || !cb_put(out, kv.value, depth + 1, active)) {
					active.erase(d.id());
					return false;
				}
			}
			active.erase(d.id());
			return true;
		}
		default: {
			const CharString b = Pool::text(v).utf8();
			cb_head(out, C_TEXT, b.length());
			const int at = out.size();
			out.resize(at + b.length());
			memcpy(out.ptr() + at, b.get_data(), b.length());
			return true;
		}
	}
}

Variant cb_get(Reader &r, int depth);

// Decode a CBOR length, using values below 24 directly.
uint64_t cb_len(Reader &r, int extra) {
	if (extra < 24) {
		return (uint64_t)extra;
	}
	switch (extra) {
		case 24:
			return r.be(1);
		case 25:
			return r.be(2);
		case 26:
			return r.be(4);
		case 27:
			return r.be(8);
	}
	r.bad = true;
	r.why = vformat("unsupported length %d", extra);
	return 0;
}

// Decode one CBOR value.
Variant cb_get(Reader &r, int depth) {
	// Bound recursive nesting to prevent stack exhaustion.
	if (depth > Variant::MAX_RECURSION_DEPTH) {
		r.bad = true;
		r.why = "too deep";
		return Variant();
	}
	if (r.at >= r.n) {
		r.bad = true;
		r.why = "truncated";
		return Variant();
	}
	const uint8_t b = r.b[r.at++];
	const int kind = b >> 5;
	const int extra = b & 31;

	if (kind == C_SIMPLE) {
		switch (extra) {
			case 20:
				return false;
			case 21:
				return true;
			case 22:
			case 23:
				return Variant();
			case 27: {
				const uint64_t bits = r.be(8);
				double d;
				memcpy(&d, &bits, 8);
				return d;
			}
		}
		r.bad = true;
		r.why = vformat("unsupported simple %d", extra);
		return Variant();
	}

	const uint64_t n = cb_len(r, extra);
	if (r.bad) {
		return Variant();
	}
	switch (kind) {
		case C_UINT:
			if (n > INT64_MAX) {
				r.bad = true;
				r.why = "uint64 exceeds Variant integer";
				return Variant();
			}
			return (int64_t)n;
		case C_NEGINT:
			if (n > INT64_MAX) {
				r.bad = true;
				r.why = "negative integer exceeds Variant integer";
				return Variant();
			}
			return (int64_t)(-1 - (int64_t)n);
		case C_BYTES:
			return r.take(n);
		case C_TEXT:
			return r.text(n, true);
		case C_ARRAY: {
			Array out;
			if (n > (uint64_t)(r.n - r.at)) {
				r.bad = true;
				r.why = "array count exceeds input";
				return out;
			}
			for (uint64_t i = 0; i < n && !r.bad; i++) {
				out.push_back(cb_get(r, depth + 1));
			}
			return out;
		}
		case C_MAP: {
			Dictionary out;
			if (n > (uint64_t)(r.n - r.at) / 2) {
				r.bad = true;
				r.why = "map count exceeds input";
				return out;
			}
			for (uint64_t i = 0; i < n && !r.bad; i++) {
				const Variant k = cb_get(r, depth + 1);
				const Variant v = cb_get(r, depth + 1);
				out[k] = v;
			}
			return out;
		}
	}
	r.bad = true;
	r.why = vformat("unsupported kind %d", kind);
	return Variant();
}

} // namespace

// Encode a value as CBOR bytes.
Ref<R> Cbor::encode(const Variant &p_v) {
	LocalVector<uint8_t> out;
	HashSet<const void *> active;
	if (!cb_put(out, p_v, 0, active)) {
		return R::err("unsupported value, cyclic data, or invalid nesting depth", Err::INVALID_DATA);
	}
	return R::ok(to_packed(out));
}

// Decode CBOR bytes into a value.
Ref<R> Cbor::decode(const PackedByteArray &p_data) {
	Reader r;
	r.b = p_data.ptr();
	r.n = p_data.size();
	const Variant v = cb_get(r, 0);
	if (r.bad || r.at != r.n) {
		if (!r.bad) {
			r.why = "trailing bytes";
		}
		return R::err(r.why, r.why.begins_with("unsupported") ? Err::UNSUPPORTED : Err::INVALID_DATA);
	}
	return R::ok(v);
}

// ---------------- tar ----------------

namespace {

constexpr int TAR_BLOCK = 512; // USTAR header and data-padding block size.
constexpr int TAR_NAME = 100; // USTAR name-field width.
constexpr int TAR_PREFIX = 155; // USTAR prefix-field width.

// Write one bounded TAR header field.
bool tar_put(uint8_t *h, int at, int width, const String &s) {
	const CharString b = s.utf8();
	if (b.length() > width) {
		return false;
	}
	for (int i = 0; i < b.length(); i++) {
		h[at + i] = (uint8_t)b[i];
	}
	return true;
}

// Fit UTF-8 into USTAR name and prefix fields without splitting characters.
bool tar_put_name(uint8_t *p_header, const String &p_name) {
	if (p_name.utf8().length() <= TAR_NAME) {
		return tar_put(p_header, 0, TAR_NAME, p_name);
	}
	int slash = p_name.rfind_char('/');
	while (slash > 0) {
		const String prefix = p_name.left(slash);
		const String name = p_name.substr(slash + 1);
		if (!name.is_empty() && prefix.utf8().length() <= TAR_PREFIX && name.utf8().length() <= TAR_NAME) {
			return tar_put(p_header, 0, TAR_NAME, name) && tar_put(p_header, 345, TAR_PREFIX, prefix);
		}
		slash = p_name.rfind_char('/', slash - 1);
	}
	return false;
}

// Validate TAR text as UTF-8 convertible without replacement.
bool tar_utf8_ok(const uint8_t *p_data, int p_len) {
	int i = 0;
	while (i < p_len) {
		const uint8_t b = p_data[i++];
		if (b < 0x80) {
			continue;
		}
		int extra = 0; // Remaining continuation bytes.
		uint32_t ord = 0; // Accumulated code point.
		uint32_t least = 0; // Minimum scalar value for the shortest encoding.
		if ((b & 0xe0) == 0xc0) {
			extra = 1;
			ord = b & 0x1f;
			least = 0x80;
		} else if ((b & 0xf0) == 0xe0) {
			extra = 2;
			ord = b & 0x0f;
			least = 0x800;
		} else if ((b & 0xf8) == 0xf0) {
			extra = 3;
			ord = b & 0x07;
			least = 0x10000;
		} else {
			return false;
		}
		for (int k = 0; k < extra; k++) {
			if (i >= p_len || (p_data[i] & 0xc0) != 0x80) {
				return false;
			}
			ord = (ord << 6) | (p_data[i++] & 0x3f);
		}
		if (ord < least || ord > 0x10ffff || (ord >= 0xd800 && ord <= 0xdfff)) {
			return false;
		}
	}
	return true;
}

// Read a NUL-terminated UTF-8 field, rejecting invalid names instead of normalizing aliases.
bool tar_get(const uint8_t *d, int64_t at, int len, int64_t total, String &r_out) {
	int64_t end = at;
	while (end < at + len && end < total && d[end] != 0) {
		end++;
	}
	const int size = (int)(end - at);
	if (!tar_utf8_ok(d + at, size)) {
		return false;
	}
	// Prefix a marker during conversion to preserve a leading BOM, then remove only the marker.
	CharString guarded;
	guarded.resize_uninitialized(size + 2);
	guarded.ptrw()[0] = 'x';
	memcpy(guarded.ptrw() + 1, d + at, size);
	guarded.ptrw()[size + 1] = 0;
	r_out = String::utf8(guarded.get_data(), guarded.length()).substr(1);
	return true;
}

// Parse TAR numbers as optional leading spaces, octal digits, then termination.
bool tar_octal(const uint8_t *p_data, int64_t p_at, int p_len, int64_t &r_value) {
	r_value = 0;
	bool digit = false;
	bool ended = false;
	for (int i = 0; i < p_len; i++) {
		const uint8_t c = p_data[p_at + i];
		if (c == 0) {
			ended = true; // Do not accept digits hidden after NUL.
			continue;
		}
		if (c == ' ') {
			ended = ended || digit;
			continue;
		}
		if (ended || c < '0' || c > '7' || r_value > (INT64_MAX - 7) / 8) {
			return false;
		}
		digit = true;
		r_value = r_value * 8 + (c - '0');
	}
	return digit;
}

// Validate archive names against relative-path rules and native destination semantics.
bool tar_extract_name_ok(const String &p_name, bool p_dir) {
	String name = p_name;
	if (p_dir && name.ends_with("/")) {
		name = name.trim_suffix("/");
	}
	// Treat backslashes as separators because public file paths do so on every OS.
	if (name.is_empty() || name.begins_with("/") || name.contains("\\")) {
		return false;
	}
	for (const String &part : name.split("/", true)) {
		if (part.is_empty() || part == "." || part == "..") {
			return false;
		}
	#ifdef WINDOWS_ENABLED
		if (part.contains(":") || part.ends_with(".") || part.ends_with(" ")) {
			return false;
		}
		for (int i = 0; i < part.length(); i++) {
			if (part[i] < 0x20 || part[i] == 0x7f || (part[i] < 0x80 && strchr("<>\"|?*", (char)part[i]) != nullptr)) {
				return false;
			}
		}
		String upper = part.get_slice(".", 0).to_upper();
		while (upper.ends_with(" ")) {
			upper = upper.trim_suffix(" "); // Windows ignores trailing spaces before a reserved name's dot.
		}
		const bool basic = upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" || upper == "CONIN$" || upper == "CONOUT$";
		const bool numbered = upper.length() == 4 && (upper.begins_with("COM") || upper.begins_with("LPT")) && upper[3] >= '1' && upper[3] <= '9';
		const String last = upper.length() == 4 ? upper.substr(3) : String();
		const bool superscript = (upper.begins_with("COM") || upper.begins_with("LPT")) && (last == "²" || last == "³" || last == "¹");
		if (basic || numbered || superscript) {
			return false;
		}
	#endif
	}
	return true;
}

// Join validated archive names to the extraction root using archive slashes only.
String tar_output_path(const String &p_root, const String &p_name) {
	if (p_root == "/" || p_root.ends_with("://")) {
		return p_root + p_name;
	}
	return p_root.rstrip("/") + "/" + p_name;
}

// Match filesystem case-insensitive behavior when checking collisions.
String tar_part_key(const String &p_path, bool p_case_sensitive) {
#ifdef MACOS_ENABLED
	const CharString raw = p_path.utf8();
	CFStringRef source = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)raw.get_data(), raw.length(), kCFStringEncodingUTF8, false);
	if (source == nullptr) {
		return p_case_sensitive ? p_path : p_path.to_lower();
	}
	CFMutableStringRef folded = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source);
	CFRelease(source);
	if (folded == nullptr) {
		return p_case_sensitive ? p_path : p_path.to_lower();
	}
	CFStringNormalize(folded, kCFStringNormalizationFormD);
	if (!p_case_sensitive) {
		CFStringFold(folded, kCFCompareCaseInsensitive, nullptr);
	}
	const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(folded), kCFStringEncodingUTF8) + 1;
	CharString out;
	if (out.resize_uninitialized(size) != OK || !CFStringGetCString(folded, out.ptrw(), size, kCFStringEncodingUTF8)) {
		CFRelease(folded);
		return p_case_sensitive ? p_path : p_path.to_lower();
	}
	CFRelease(folded);
	return String::utf8(out.get_data());
#elif defined(WINDOWS_ENABLED)
	// Fold Unicode aliases that compare equal under Windows case-insensitive naming before writing.
	return p_case_sensitive ? p_path : p_path.to_upper();
#else
	return p_case_sensitive ? p_path : p_path.to_upper();
#endif
}

// Obtain filesystem comparison rules from the nearest existing directory for new destinations.
bool tar_case_sensitive(const String &p_root, const Ref<GDDir> &p_fs) {
	if (p_fs.is_null()) {
		return true;
	}
	String probe = p_root;
	while (!GDDir::dir_exists_absolute(probe)) {
		const String parent = probe.get_base_dir();
		if (parent == probe || parent.is_empty()) {
			return true;
		}
		probe = parent;
	}
	return p_fs->is_case_sensitive(probe);
}

// Build collision keys using each directory's own case rules.
String tar_name_key(const String &p_root, const String &p_name, const Ref<GDDir> &p_fs) {
	String dir = p_root;
	String key;
	for (const String &part : p_name.split("/", true)) {
		key += "/" + tar_part_key(part, tar_case_sensitive(dir, p_fs));
		dir = tar_output_path(dir, part);
	}
	return key;
}

#ifdef LINUXBSD_ENABLED
// Use kernel-resolved identities in Linux casefold directories to reject Unicode aliases in constant time.
bool tar_linux_alias(const String &p_path, const Ref<GDDir> &p_fs, HashSet<String> &r_names, HashSet<String> &r_ids) {
	if (r_names.has(p_path)) {
		return false;
	}
	r_names.insert(p_path);
	if (!tar_case_sensitive(p_path.get_base_dir(), p_fs)) {
		const String id = p_fs->get_identity(p_path);
		if (!id.is_empty()) {
			if (r_ids.has(id)) {
				return true;
			}
			r_ids.insert(id);
		}
	}
	return false;
}

// Record newly written file identities for later alias checks.
void tar_linux_remember(const String &p_path, const Ref<GDDir> &p_fs, HashSet<String> &r_ids) {
	if (!tar_case_sensitive(p_path.get_base_dir(), p_fs)) {
		const String id = p_fs->get_identity(p_path);
		if (!id.is_empty()) {
			r_ids.insert(id);
		}
	}
}
#endif

#ifdef WINDOWS_ENABLED
// Open one component relative to an NT parent handle, atomically rejecting junctions and symlinks.
bool tar_win_open(HANDLE p_parent, const String &p_name, bool p_dir, HANDLE &r_handle) {
	SourceError::clear();
	Char16String raw = p_name.utf16();
	UNICODE_STRING name = {};
	name.Buffer = (PWSTR)raw.ptrw();
	name.Length = raw.length() * sizeof(char16_t);
	name.MaximumLength = name.Length;
	OBJECT_ATTRIBUTES attr;
	InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, p_parent, nullptr);
	IO_STATUS_BLOCK status = {};
	const ULONG options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT | (p_dir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
	const ACCESS_MASK access = p_dir ? FILE_GENERIC_READ : FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES;
	const NTSTATUS code = NtCreateFile(&r_handle, access, &attr, &status, nullptr, FILE_ATTRIBUTE_NORMAL,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN_IF, options, nullptr, 0);
	if (code < 0) {
		SourceError::win32(RtlNtStatusToDosError(code));
		return false;
	}
	BY_HANDLE_FILE_INFORMATION info = {};
	if (!GetFileInformationByHandle(r_handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		const DWORD why = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? ERROR_CANT_ACCESS_FILE : GetLastError();
		SourceError::win32(why);
		CloseHandle(r_handle);
		r_handle = INVALID_HANDLE_VALUE;
		return false;
	}
	return true;
}

// Normalize final Windows path separators for comparison.
String tar_win_final(const String &p_path) {
	String path = p_path.replace("\\", "/");
	if (path.begins_with("//?/UNC/")) {
		return "//" + path.substr(8);
	}
	return path.trim_prefix("//?/");
}

// Pin the extraction root and verify that reparsing did not redirect it from the validated path.
HANDLE tar_win_root(const String &p_root, Ref<R> &r_error) {
	const Ref<R> made = Os::ensure_dir(p_root);
	if (made->get_e().is_valid()) {
		r_error = made;
		return INVALID_HANDLE_VALUE;
	}
	SourceError::clear();
	String why;
	const String real = Mount::resolve(p_root, true, why);
	if (real.is_empty()) {
		const String msg = why.is_empty() ? vformat("cannot resolve tar root %s", p_root) : why;
		r_error = SourceError::path(p_root, "open", ERR_FILE_BAD_PATH, msg);
		return INVALID_HANDLE_VALUE;
	}
	HANDLE root = CreateFileW((LPCWSTR)real.utf16().get_data(), FILE_GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (root == INVALID_HANDLE_VALUE) {
		SourceError::win32(GetLastError());
		r_error = SourceError::path(p_root, "open", ERR_CANT_OPEN, vformat("cannot open tar root %s", p_root));
		return root;
	}
	BY_HANDLE_FILE_INFORMATION info = {};
	if (!GetFileInformationByHandle(root, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		DWORD code = GetLastError();
		if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
			code = ERROR_CANT_ACCESS_FILE;
		} else if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			code = ERROR_DIRECTORY;
		}
		SourceError::win32(code);
		CloseHandle(root);
		const Error fallback = ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) ? ERR_FILE_BAD_PATH : ERR_CANT_OPEN;
		r_error = SourceError::path(p_root, "open", fallback, vformat("cannot open tar root %s", p_root));
		return INVALID_HANDLE_VALUE;
	}
	DWORD size = GetFinalPathNameByHandleW(root, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	const DWORD size_error = size == 0 ? GetLastError() : ERROR_SUCCESS;
	Char16String final;
	const Error alloc = final.resize_uninitialized(size + 1);
	const DWORD got = size > 0 && alloc == OK ? GetFinalPathNameByHandleW(root, (LPWSTR)final.ptrw(), size + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
	const DWORD got_error = got == 0 ? GetLastError() : ERROR_SUCCESS;
	const bool short_buffer = got >= size + 1;
	const bool moved = got > 0 && !short_buffer && tar_win_final(String::utf16(final.get_data(), got)).nocasecmp_to(tar_win_final(real)) != 0;
	if (size == 0 || alloc != OK || got == 0 || short_buffer || moved) {
		DWORD code = size == 0 ? size_error : got_error;
		if (short_buffer) {
			code = ERROR_INSUFFICIENT_BUFFER;
		} else if (moved) {
			code = ERROR_CANT_ACCESS_FILE;
		}
		if (alloc == OK) {
			SourceError::win32(code);
		}
		CloseHandle(root);
		const Error fallback = alloc != OK ? ERR_OUT_OF_MEMORY : moved ? ERR_FILE_BAD_PATH : ERR_CANT_OPEN;
		r_error = SourceError::path(p_root, "open", fallback, vformat("cannot verify tar root %s", p_root));
		return INVALID_HANDLE_VALUE;
	}
	return root;
}

// Traverse Windows extraction paths by parent handles so archive names cannot escape through reparse points.
Ref<R> tar_unpack_windows(const Array &p_items, const LocalVector<String> &p_names, const String &p_root) {
	for (int i = 0; i < p_items.size(); i++) {
		Ref<R> root_error;
		HANDLE parent = tar_win_root(p_root, root_error);
		if (parent == INVALID_HANDLE_VALUE) {
			return root_error;
		}
		const Vector<String> parts = p_names[i].split("/", true);
		const bool is_dir = (bool)((Dictionary)p_items[i]).get("is_dir", false);
		const int dirs = is_dir ? parts.size() : parts.size() - 1;
		bool ok = true;
		int failed_part = -1;
		for (int k = 0; k < dirs; k++) {
			HANDLE next = INVALID_HANDLE_VALUE;
			ok = tar_win_open(parent, parts[k], true, next);
			CloseHandle(parent);
			parent = next;
			if (!ok) {
				failed_part = k;
				break;
			}
		}
		if (!ok) {
			String failed_name = parts[0];
			for (int k = 1; k <= failed_part; k++) {
				failed_name += "/" + parts[k];
			}
			const String path = tar_output_path(p_root, failed_name);
			return SourceError::path(path, "openat", ERR_FILE_BAD_PATH, vformat("cannot open tar directory %s", failed_name));
		}
		if (is_dir) {
			CloseHandle(parent);
			continue;
		}
		HANDLE file = INVALID_HANDLE_VALUE;
		ok = tar_win_open(parent, parts[parts.size() - 1], false, file);
		CloseHandle(parent);
		const PackedByteArray body = ((Dictionary)p_items[i]).get("body", PackedByteArray());
		LARGE_INTEGER zero = {};
		if (!ok || !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
			if (ok) {
				SourceError::win32(GetLastError());
			}
			if (file != INVALID_HANDLE_VALUE) {
				CloseHandle(file);
			}
			const String path = tar_output_path(p_root, p_names[i]);
			return SourceError::path(path, "write", ERR_FILE_CANT_WRITE, vformat("cannot write tar file %s", p_names[i]));
		}
		int64_t at = 0;
		while (at < body.size()) {
			DWORD wrote = 0;
			const DWORD want = (DWORD)MIN(body.size() - at, (int64_t)UINT32_MAX);
			const bool done = WriteFile(file, body.ptr() + at, want, &wrote, nullptr);
			if (!done || wrote == 0) {
				SourceError::win32(done ? ERROR_WRITE_FAULT : GetLastError());
				CloseHandle(file);
				const String path = tar_output_path(p_root, p_names[i]);
				return SourceError::path(path, "write", ERR_FILE_CANT_WRITE, vformat("cannot write tar file %s", p_names[i]));
			}
			at += wrote;
		}
		CloseHandle(file);
	}
	return R::ok(p_items.size());
}
#endif

} // namespace

// Encode entries as a TAR archive.
Ref<R> Tar::pack(const Array &p_entries) {
	PackedByteArray out;
	for (int i = 0; i < p_entries.size(); i++) {
		if (p_entries[i].get_type() != Variant::DICTIONARY) return R::err("tar entry must be a dictionary", Err::INVALID_DATA);
		const Dictionary e = p_entries[i];
		// Validate dynamic fields before native conversions can discard their values.
		if ((e.get("name", "").get_type() != Variant::STRING && e.get("name", "").get_type() != Variant::STRING_NAME) ||
				e.get("body", PackedByteArray()).get_type() != Variant::PACKED_BYTE_ARRAY ||
				e.get("is_dir", false).get_type() != Variant::BOOL || e.get("mode", 420).get_type() != Variant::INT ||
				e.get("mtime", 0).get_type() != Variant::INT) {
			return R::err(vformat("tar entry %d has invalid field types", i), Err::INVALID_DATA);
		}
		const String name = Pool::text(e.get("name", ""));
		const bool is_dir = e.get("is_dir", false);
		// Reject unnamed entries that cannot be addressed by extractors.
		if (name.is_empty()) {
			return R::err(vformat("tar entry %d has no name", i), Err::INVALID_DATA);
		}
		const PackedByteArray body = e.get("body", PackedByteArray());
		const int64_t mode = e.get("mode", 420);
		const int64_t mtime = e.get("mtime", 0);
		uint8_t h[TAR_BLOCK];
		memset(h, 0, TAR_BLOCK);
		const bool fields_ok = tar_put_name(h, name) &&
				tar_put(h, 100, 8, vformat("%07o", mode)) &&
				tar_put(h, 108, 8, "0000000") && // Owner ID.
				tar_put(h, 116, 8, "0000000") && // Group ID.
				tar_put(h, 124, 12, vformat("%011o", is_dir ? 0 : body.size())) &&
				tar_put(h, 136, 12, vformat("%011o", mtime)) &&
				tar_put(h, 148, 8, "        ") && // Fill the checksum field with spaces before computing the sum.
				tar_put(h, 156, 1, is_dir ? "5" : "0") &&
				tar_put(h, 257, 6, "ustar") && tar_put(h, 263, 2, "00");
		if (!fields_ok) {
			return R::err(vformat("tar entry %d cannot be represented by ustar", i), Err::INVALID_DATA);
		}

		// Sum every header byte for the checksum.
		int sum = 0;
		for (int k = 0; k < TAR_BLOCK; k++) {
			sum += h[k];
		}
		tar_put(h, 148, 6, vformat("%06o", sum));
		h[154] = 0;
		h[155] = 32;

		const int64_t at = out.size();
		if (out.resize(at + TAR_BLOCK) != OK) {
			return R::err("cannot allocate tar output", Err::LIMITED);
		}
		memcpy(out.ptrw() + at, h, TAR_BLOCK);

		if (!is_dir) {
			const int64_t bat = out.size();
			const int64_t pad = (TAR_BLOCK - body.size() % TAR_BLOCK) % TAR_BLOCK;
			if (out.resize(bat + body.size() + pad) != OK) {
				return R::err("cannot allocate tar output", Err::LIMITED);
			}
			if (body.size() > 0) {
				memcpy(out.ptrw() + bat, body.ptr(), body.size());
			}
			if (pad > 0) {
				memset(out.ptrw() + bat + body.size(), 0, pad);
			}
		}
	}
	// Terminate the archive with two empty blocks.
	const int64_t at = out.size();
	if (out.resize(at + TAR_BLOCK * 2) != OK) {
		return R::err("cannot allocate tar output", Err::LIMITED);
	}
	memset(out.ptrw() + at, 0, TAR_BLOCK * 2);
	return R::ok(out);
}

// Validate TAR structure and return entry dictionaries.
Ref<R> Tar::unpack(const PackedByteArray &p_data) {
	Array out;
	const uint8_t *d = p_data.ptr();
	const int64_t total = p_data.size();
	// Reject archives not aligned to complete blocks.
	// Returning empty would misreport a truncated archive as successful with no content.
	if (total == 0 || total % TAR_BLOCK != 0) {
		return R::err(vformat("tar size %d is not a multiple of %d", total, TAR_BLOCK), Err::INVALID_DATA);
	}
	int64_t at = 0;
	bool ended = false;
	while (at + TAR_BLOCK <= total) {
		String name;
		if (!tar_get(d, at, TAR_NAME, total, name)) {
			return R::err("tar name is not valid UTF-8", Err::INVALID_DATA);
		}
		if (name.is_empty()) {
			if (at + TAR_BLOCK * 2 > total) {
				return R::err("tar end marker is incomplete", Err::INVALID_DATA);
			}
			// Reject additional archives hidden after the terminator.
			for (int64_t i = at; i < total; i++) {
				if (d[i] != 0) {
					return R::err("tar has data after its end", Err::INVALID_DATA);
				}
			}
			ended = true;
			break;
		}
		String prefix;
		if (!tar_get(d, at + 345, 155, total, prefix)) {
			return R::err("tar prefix is not valid UTF-8", Err::INVALID_DATA);
		}
		if (!prefix.is_empty()) {
			name = prefix + "/" + name;
		}
		int64_t size = 0, mode = 0, mtime = 0, stored_sum = 0;
		if (!tar_octal(d, at + 124, 12, size) || !tar_octal(d, at + 100, 8, mode) ||
				!tar_octal(d, at + 136, 12, mtime) || !tar_octal(d, at + 148, 8, stored_sum)) {
			return R::err(vformat("invalid tar number at \"%s\"", name), Err::INVALID_DATA);
		}
		int64_t sum = 0;
		for (int i = 0; i < TAR_BLOCK; i++) {
			sum += (i >= 148 && i < 156) ? 32 : d[at + i];
		}
		if (sum != stored_sum) {
			return R::err(vformat("tar checksum mismatch at \"%s\"", name), Err::INVALID_DATA);
		}
		String kind;
		if (!tar_get(d, at + 156, 1, total, kind)) {
			return R::err(vformat("tar kind is not valid UTF-8 at \"%s\"", name), Err::INVALID_DATA);
		}
		if (kind == "5" && size != 0) {
			return R::err(vformat("tar directory has data at \"%s\"", name), Err::INVALID_DATA);
		}
		Dictionary e;
		e["name"] = name;
		e["mode"] = mode;
		e["mtime"] = mtime;
		e["is_dir"] = kind == "5";
		at += TAR_BLOCK;
		PackedByteArray body;
		if (kind != "5" && size > 0) {
			if (size > total - at) {
				return R::err(vformat("truncated tar at \"%s\"", name), Err::INVALID_DATA);
			}
			if (body.resize(size) != OK) {
				return R::err(vformat("cannot allocate tar body at \"%s\"", name), Err::LIMITED);
			}
			memcpy(body.ptrw(), d + at, size);
			at += size + (TAR_BLOCK - size % TAR_BLOCK) % TAR_BLOCK;
		}
		e["body"] = body;
		out.push_back(e);
	}
	if (!ended) {
		return R::err("tar has no end marker", Err::INVALID_DATA);
	}
	return R::ok(out);
}

// Archive a directory tree.
Ref<R> Tar::pack_dir(const String &p_root) {
	const Ref<R> listed = Os::walk(p_root, false, false);
	if (listed->get_e().is_valid()) {
		return listed;
	}
	const PackedStringArray files = listed->get_v();
	Array entries;
	for (const String &f : files) {
		const Ref<R> got = Os::read_bytes(f);
		if (got->get_e().is_valid()) {
			return got;
		}
		Dictionary e;
		e["name"] = f.trim_prefix(p_root).trim_prefix("/");
		e["body"] = got->get_v();
		e["mode"] = 420; // Octal 0644.
		e["mtime"] = 0;
		e["is_dir"] = false;
		entries.push_back(e);
	}
	return pack(entries);
}

// Extract an archive into the selected directory.
Ref<R> Tar::unpack_to(const PackedByteArray &p_data, const String &p_root) {
	const Ref<R> got = unpack(p_data);
	if (got->get_e().is_valid()) {
		return got;
	}
	const Array items = got->get_v();
	HashSet<String> destinations; // Reject archive names normalizing to the same destination.
	HashSet<String> files; // File destinations that cannot be parents of other entries.
	LocalVector<String> names;
	names.reserve(items.size());
	const String root = Path::normalize(p_root);
	const Ref<GDDir> fs = GDDir::create();
	// Validate every name before writing to catch collisions without partial extraction.
	for (int i = 0; i < items.size(); i++) {
		const Dictionary e = items[i];
		const String name = Pool::text(e.get("name", ""));
		const bool is_dir = e.get("is_dir", false);
		if (!tar_extract_name_ok(name, is_dir)) {
			return R::err(vformat("tar name is not portable \"%s\"", name), Err::INVALID_DATA);
		}
		const String local = is_dir && name.ends_with("/") ? name.trim_suffix("/") : name;
		// Join validated components using only archive slash separators.
		const String dst = tar_output_path(root, local);
		const String key = tar_name_key(root, local, fs);
		if (destinations.has(key)) {
			return R::err(vformat("tar names share output \"%s\"", dst), Err::INVALID_DATA);
		}
		destinations.insert(key);
		if (!is_dir) {
			files.insert(key);
		}
		names.push_back(local);
	}
	// Reject entries nested beneath files before writing, independently of archive order.
	for (const String &name : names) {
		int slash = name.rfind("/");
		while (slash >= 0) {
			const String parent = tar_output_path(root, name.substr(0, slash));
			if (files.has(tar_name_key(root, name.substr(0, slash), fs))) {
				return R::err(vformat("tar file is also a parent \"%s\"", parent), Err::INVALID_DATA);
			}
			slash = name.rfind("/", slash - 1);
		}
	}
#ifdef WINDOWS_ENABLED
	return tar_unpack_windows(items, names, root);
#endif
	#ifdef LINUXBSD_ENABLED
	HashSet<String> written_names; // Paths created while extracting this archive.
	HashSet<String> written_ids; // Kernel-resolved device and inode identities.
	#endif
	for (int i = 0; i < items.size(); i++) {
		const Dictionary e = items[i];
		const String dst = tar_output_path(root, names[i]);
		if ((bool)e.get("is_dir", false)) {
			const Ref<R> made = Os::ensure_dir(dst);
			if (made->get_e().is_valid()) {
				return made;
			}
		#ifdef LINUXBSD_ENABLED
			String part = root;
			for (const String &name : names[i].split("/", true)) {
				part = tar_output_path(part, name);
				if (tar_linux_alias(part, fs, written_names, written_ids)) {
					return R::err(vformat("tar names share output \"%s\"", part), Err::INVALID_DATA);
				}
			}
		#endif
			continue;
		}
		const Ref<R> parent = Os::ensure_dir(dst.get_base_dir());
		if (parent->get_e().is_valid()) {
			return parent;
		}
	#ifdef LINUXBSD_ENABLED
		String part = root;
		const Vector<String> bits = names[i].split("/", true);
		for (int k = 0; k < bits.size(); k++) {
			part = tar_output_path(part, bits[k]);
			if (tar_linux_alias(part, fs, written_names, written_ids)) {
				return R::err(vformat("tar names share output \"%s\"", part), Err::INVALID_DATA);
			}
		}
	#endif
		const Ref<R> wrote = Os::write_bytes(dst, e.get("body", PackedByteArray()));
		if (wrote->get_e().is_valid()) {
			return wrote;
		}
	#ifdef LINUXBSD_ENABLED
		tar_linux_remember(dst, fs, written_ids);
	#endif
	}
	return R::ok(items.size());
}

// ---------------- Key derivation ----------------

namespace {

constexpr int SHA256_LEN = 32; // SHA-256 output width.

// Map public hash names strictly to backend algorithms.
const mbedtls_md_info_t *hash_info(const String &p_name) {
	mbedtls_md_type_t type = MBEDTLS_MD_NONE;
	if (p_name == "sha1") {
		type = MBEDTLS_MD_SHA1;
	} else if (p_name == "sha224") {
		type = MBEDTLS_MD_SHA224;
	} else if (p_name == "sha256") {
		type = MBEDTLS_MD_SHA256;
	} else if (p_name == "sha384") {
		type = MBEDTLS_MD_SHA384;
	} else if (p_name == "sha512") {
		type = MBEDTLS_MD_SHA512;
	} else if (p_name == "sha3-224") {
		type = MBEDTLS_MD_SHA3_224;
	} else if (p_name == "sha3-256") {
		type = MBEDTLS_MD_SHA3_256;
	} else if (p_name == "sha3-384") {
		type = MBEDTLS_MD_SHA3_384;
	} else if (p_name == "sha3-512") {
		type = MBEDTLS_MD_SHA3_512;
	}
	return mbedtls_md_info_from_type(type);
}

// Compute one fixed-size digest with the selected algorithm.
PackedByteArray digest_of(const mbedtls_md_type_t p_type, const PackedByteArray &p_msg) {
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(p_type);
	PackedByteArray out;
	if (!info) {
		return out;
	}
	if (out.resize(mbedtls_md_get_size(info)) != OK) {
		return out;
	}
	if (mbedtls_md(info, p_msg.ptr(), p_msg.size(), out.ptrw()) != 0) {
		out.clear();
	}
	return out;
}

// Write HMAC into raw storage, preventing use of its contents after failure.
bool hmac_into(const mbedtls_md_info_t *p_info, const PackedByteArray &p_key, const uint8_t *p_msg, int p_len, uint8_t *r_out) {
	return p_info && mbedtls_md_hmac(p_info, p_key.ptr(), p_key.size(), p_msg, p_len, r_out) == 0;
}

// Expand a pseudorandom key with context and a counter to the required length.
bool hkdf_expand_into(const mbedtls_md_info_t *p_md, const PackedByteArray &p_key, const PackedByteArray &p_info, int p_size, PackedByteArray &r_out) {
	if (r_out.resize(p_size) != OK) {
		return false;
	}
	if (p_size == 0) {
		return true;
	}
	mbedtls_md_context_t ctx;
	mbedtls_md_init(&ctx);
	if (mbedtls_md_setup(&ctx, p_md, 1) != 0) {
		mbedtls_md_free(&ctx);
		return false;
	}
	uint8_t previous[MBEDTLS_MD_MAX_SIZE] = {};
	const int hash_size = mbedtls_md_get_size(p_md);
	int previous_size = 0;
	int at = 0;
	for (uint8_t block = 1; at < p_size; block++) {
		const bool failed = mbedtls_md_hmac_starts(&ctx, p_key.ptr(), p_key.size()) != 0 ||
				mbedtls_md_hmac_update(&ctx, previous, previous_size) != 0 ||
				mbedtls_md_hmac_update(&ctx, p_info.ptr(), p_info.size()) != 0 ||
				mbedtls_md_hmac_update(&ctx, &block, 1) != 0 ||
				mbedtls_md_hmac_finish(&ctx, previous) != 0;
		if (failed) {
			mbedtls_md_free(&ctx);
			mbedtls_platform_zeroize(previous, sizeof(previous));
			r_out.fill(0);
			r_out.clear();
			return false;
		}
		previous_size = hash_size;
		const int take = MIN(hash_size, p_size - at);
		memcpy(r_out.ptrw() + at, previous, take);
		at += take;
	}
	mbedtls_md_free(&ctx);
	mbedtls_platform_zeroize(previous, sizeof(previous));
	return true;
}

} // namespace

// Derive a key using PBKDF2-HMAC-SHA256.
Ref<R> Hash::pbkdf2_sha256(const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds) {
	return pbkdf2("sha256", p_pass, p_salt, p_rounds, SHA256_LEN);
}

// Derive a PBKDF2 key with the selected hash and output length.
Ref<R> Hash::pbkdf2(const String &p_hash, const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds, int64_t p_size) {
	const mbedtls_md_info_t *info = hash_info(p_hash);
	if (!info) {
		return R::err("unsupported hash", Err::UNSUPPORTED);
	}
	if (p_rounds > UINT_MAX || p_size < 1 || p_size > INT_MAX) {
		return R::err("invalid PBKDF2 size or rounds", Err::INVALID_DATA);
	}
	PackedByteArray out;
	if (out.resize(int(p_size)) != OK) {
		return R::err("cannot allocate PBKDF2 key", Err::LIMITED);
	}
	const int e = mbedtls_pkcs5_pbkdf2_hmac_ext(mbedtls_md_get_type(info), p_pass.ptr(), p_pass.size(), p_salt.ptr(), p_salt.size(), uint32_t(MAX(p_rounds, int64_t(1))), uint32_t(p_size), out.ptrw());
	return e == 0 ? R::ok(out) : R::err("cannot derive PBKDF2 key");
}

// Derive an HKDF key from secret, salt, and context.
Ref<R> Hash::hkdf(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt, const PackedByteArray &p_info, int64_t p_size) {
	const mbedtls_md_info_t *info = hash_info(p_hash);
	if (!info) {
		return R::err("unsupported hash", Err::UNSUPPORTED);
	}
	if (p_size < 0 || p_size > 255LL * mbedtls_md_get_size(info) || p_size > INT_MAX) {
		return R::err("invalid HKDF size", Err::INVALID_DATA);
	}
	PackedByteArray key;
	if (key.resize(mbedtls_md_get_size(info)) != OK) {
		return R::err("cannot allocate HKDF key", Err::LIMITED);
	}
	if (mbedtls_hkdf_extract(info, p_salt.ptr(), p_salt.size(), p_secret.ptr(), p_secret.size(), key.ptrw()) != 0) {
		key.fill(0);
		return R::err("cannot extract HKDF key");
	}
	PackedByteArray out;
	const bool made = hkdf_expand_into(info, key, p_info, int(p_size), out);
	key.fill(0);
	return made ? R::ok(out) : R::err("cannot derive HKDF key");
}

// Extract an HKDF pseudorandom key from secret and salt.
Ref<R> Hash::hkdf_extract(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt) {
	const mbedtls_md_info_t *info = hash_info(p_hash);
	if (!info) {
		return R::err("unsupported hash", Err::UNSUPPORTED);
	}
	PackedByteArray out;
	if (out.resize(mbedtls_md_get_size(info)) != OK) {
		return R::err("cannot allocate HKDF key", Err::LIMITED);
	}
	const int e = mbedtls_hkdf_extract(info, p_salt.ptr(), p_salt.size(), p_secret.ptr(), p_secret.size(), out.ptrw());
	return e == 0 ? R::ok(out) : R::err("cannot extract HKDF key");
}

// Expand an HKDF pseudorandom key with context.
Ref<R> Hash::hkdf_expand(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_info, int64_t p_size) {
	const mbedtls_md_info_t *info = hash_info(p_hash);
	if (!info) {
		return R::err("unsupported hash", Err::UNSUPPORTED);
	}
	if (p_size < 0 || p_size > 255LL * mbedtls_md_get_size(info) || p_size > INT_MAX) {
		return R::err("invalid HKDF size", Err::INVALID_DATA);
	}
	PackedByteArray out;
	return hkdf_expand_into(info, p_key, p_info, int(p_size), out) ? R::ok(out) : R::err("cannot expand HKDF key");
}

// Compute SHA-224.
PackedByteArray Hash::sha224(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA224, p_msg);
}

// Compute SHA-256.
PackedByteArray Hash::sha256(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA256, p_msg);
}

// Compute SHA-384.
PackedByteArray Hash::sha384(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA384, p_msg);
}

// Compute SHA-512.
PackedByteArray Hash::sha512(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA512, p_msg);
}

// Compute SHA3-224.
PackedByteArray Hash::sha3_224(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA3_224, p_msg);
}

// Compute SHA3-256.
PackedByteArray Hash::sha3_256(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA3_256, p_msg);
}

// Compute SHA3-384.
PackedByteArray Hash::sha3_384(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA3_384, p_msg);
}

// Compute SHA3-512.
PackedByteArray Hash::sha3_512(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA3_512, p_msg);
}

// Compute SHA-1.
PackedByteArray Hash::sha1(const PackedByteArray &p_msg) {
	return digest_of(MBEDTLS_MD_SHA1, p_msg);
}

// Compute HMAC using the selected hash.
Ref<R> Hash::hmac(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_msg) {
	const mbedtls_md_info_t *info = hash_info(p_hash);
	if (!info) {
		return R::err("unsupported hash", Err::UNSUPPORTED);
	}
	PackedByteArray out;
	if (out.resize(mbedtls_md_get_size(info)) != OK) {
		return R::err("cannot allocate HMAC", Err::LIMITED);
	}
	return hmac_into(info, p_key, p_msg.ptr(), p_msg.size(), out.ptrw()) ? R::ok(out) : R::err("cannot compute HMAC");
}

// Compute HMAC-SHA256.
PackedByteArray Hash::hmac_sha256(const PackedByteArray &p_key, const PackedByteArray &p_msg) {
	PackedByteArray out;
	out.resize(SHA256_LEN);
	if (!hmac_into(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), p_key, p_msg.ptr(), p_msg.size(), out.ptrw())) {
		return PackedByteArray(); // Do not return zero-filled bytes as a successfully computed authentication value.
	}
	return out;
}

// Combine two byte sequences with XOR.
PackedByteArray Hash::xor_bytes(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	const int n = MIN(p_a.size(), p_b.size());
	PackedByteArray out;
	out.resize(n);
	uint8_t *w = out.ptrw();
	const uint8_t *a = p_a.ptr();
	const uint8_t *b = p_b.ptr();
	for (int i = 0; i < n; i++) {
		w[i] = a[i] ^ b[i];
	}
	return out;
}

// Compare byte contents without mismatch-dependent early exit.
bool Hash::equal_ct(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	// Length is not secret here, so unequal lengths may return immediately.
	if (p_a.size() != p_b.size()) {
		return false;
	}
	const uint8_t *a = p_a.ptr();
	const uint8_t *b = p_b.ptr();
	uint8_t diff = 0;
	for (int64_t i = 0; i < p_a.size(); i++) {
		diff |= (uint8_t)(a[i] ^ b[i]); // Accumulate differences without early exit.
	}
	return diff == 0;
}
