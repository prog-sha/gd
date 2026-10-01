/**************************************************************************/
/*  body.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Decode announcement bodies.
// Decoding is lenient where clients may differ (unknown fields, name case, trailing data)
// and strict where values are typed, so every server instance accepts the same announcements.

#include "cli/online/match.h"

#include <cstring>

namespace OnlineWire {

// Decode one UTF-8 sequence; invalid bytes become U+FFFD and consume one byte.
int rune(const uint8_t *p_s, int p_n, char32_t &r_rune) {
	const uint8_t c = p_s[0];
	// Sequence length from the lead byte; zero when it cannot start one.
	int size = 0;
	if (c < 0x80) {
		size = 1;
	} else if ((c & 0xE0) == 0xC0) {
		size = 2;
	} else if ((c & 0xF0) == 0xE0) {
		size = 3;
	} else if ((c & 0xF8) == 0xF0) {
		size = 4;
	}
	r_rune = 0xFFFD;
	if (size == 0 || size > p_n) {
		return 1;
	}
	if (size == 1) {
		r_rune = c;
		return 1;
	}
	char32_t v = c & (0x7F >> size);
	for (int i = 1; i < size; i++) {
		if ((p_s[i] & 0xC0) != 0x80) {
			return 1;
		}
		v = (v << 6) | (p_s[i] & 0x3F);
	}
	// Reject overlong forms, surrogates, and values beyond Unicode.
	static const char32_t least[] = { 0, 0, 0x80, 0x800, 0x10000 }; // Smallest value for each length.
	if (v < least[size] || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
		return 1;
	}
	r_rune = v;
	return size;
}

} // namespace OnlineWire

namespace {

// Cursor over one request body.
struct Scan {
	const uint8_t *s = nullptr; // Body bytes.
	int n = 0; // Body length.
	int at = 0; // Next unread byte.

	// Skip JSON whitespace.
	void space() {
		while (at < n && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) {
			at++;
		}
	}

	// Consume an exact keyword.
	bool word(const char *p_word) {
		const int len = strlen(p_word);
		if (at + len > n || memcmp(s + at, p_word, len) != 0) {
			return false;
		}
		at += len;
		return true;
	}

	// Read four hex digits of a \u escape.
	bool hex4(char32_t &r_value) {
		if (at + 4 > n) {
			return false;
		}
		r_value = 0;
		for (int i = 0; i < 4; i++) {
			const uint8_t c = s[at++];
			const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
					: c >= 'A' && c <= 'F'										? c - 'A' + 10
																				: -1;
			if (d < 0) {
				return false;
			}
			r_value = (r_value << 4) | d;
		}
		return true;
	}

	// Append one character as UTF-8.
	static void put(PackedByteArray &r_out, char32_t p_c) {
		const int size = p_c < 0x80 ? 1 : p_c < 0x800 ? 2
				: p_c < 0x10000						  ? 3
													  : 4;
		static const uint8_t lead[] = { 0, 0, 0xC0, 0xE0, 0xF0 }; // Lead-byte marks by sequence length.
		r_out.push_back(size == 1 ? uint8_t(p_c) : uint8_t(lead[size] | (p_c >> (6 * (size - 1)))));
		for (int i = size - 2; i >= 0; i--) {
			r_out.push_back(uint8_t(0x80 | ((p_c >> (6 * i)) & 0x3F)));
		}
	}

	// Read one string as UTF-8 bytes, keeping NUL; lone surrogates and invalid UTF-8 become U+FFFD.
	bool str(PackedByteArray &r_out) {
		if (at >= n || s[at] != '"') {
			return false;
		}
		at++;
		r_out.clear();
		while (at < n) {
			const uint8_t c = s[at];
			if (c == '"') {
				at++;
				return true;
			}
			if (c < 0x20) {
				return false;
			}
			if (c != '\\') {
				char32_t rune;
				at += OnlineWire::rune(s + at, n - at, rune);
				put(r_out, rune);
				continue;
			}
			if (++at >= n) {
				return false;
			}
			const uint8_t e = s[at++];
			static const char *const from = "\"\\/bfnrt"; // Single-character escapes.
			static const char *const to = "\"\\/\b\f\n\r\t"; // Their decoded characters.
			const char *hit = e ? strchr(from, e) : nullptr;
			if (hit) {
				put(r_out, char32_t(to[hit - from]));
				continue;
			}
			char32_t u;
			if (e != 'u' || !hex4(u)) {
				return false;
			}
			if (u >= 0xD800 && u < 0xDC00 && at + 6 <= n && s[at] == '\\' && s[at + 1] == 'u') {
				// Join a surrogate pair only when the low half follows directly.
				const int back = at;
				at += 2;
				char32_t low;
				if (!hex4(low)) {
					return false;
				}
				if (low >= 0xDC00 && low <= 0xDFFF) {
					put(r_out, char32_t(0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00)));
					continue;
				}
				at = back;
			}
			put(r_out, (u >= 0xD800 && u <= 0xDFFF) ? char32_t(0xFFFD) : u);
		}
		return false;
	}

	// Read one number literal without converting it.
	bool num(int &r_from) {
		r_from = at;
		if (at < n && s[at] == '-') {
			at++;
		}
		if (at >= n || s[at] < '0' || s[at] > '9') {
			return false;
		}
		if (s[at++] != '0') {
			while (at < n && s[at] >= '0' && s[at] <= '9') {
				at++;
			}
		}
		// Fraction and exponent must each carry digits.
		auto digits = [&]() -> bool {
			const int from = at;
			while (at < n && s[at] >= '0' && s[at] <= '9') {
				at++;
			}
			return at > from;
		};
		if (at < n && s[at] == '.' && (++at, !digits())) {
			return false;
		}
		if (at < n && (s[at] == 'e' || s[at] == 'E')) {
			at++;
			if (at < n && (s[at] == '+' || s[at] == '-')) {
				at++;
			}
			if (!digits()) {
				return false;
			}
		}
		return true;
	}

	// Read an integer that fits 64 bits; fractions and exponents are refused.
	bool integer(int64_t &r_value) {
		int from;
		if (!num(from)) {
			return false;
		}
		const bool neg = s[from] == '-';
		const uint64_t limit = uint64_t(INT64_MAX) + (neg ? 1 : 0); // Largest magnitude for the sign.
		uint64_t v = 0;
		for (int i = from + (neg ? 1 : 0); i < at; i++) {
			if (s[i] < '0' || s[i] > '9') {
				return false;
			}
			const uint64_t d = s[i] - '0';
			if (v > (limit - d) / 10) {
				return false;
			}
			v = v * 10 + d;
		}
		r_value = neg ? int64_t(0 - v) : int64_t(v);
		return true;
	}

	// Read the members of an object or the items of an array after its opening byte.
	// Each object member hands its name to p_each with the cursor at its value.
	template <typename F>
	bool items(uint8_t p_close, F p_each) {
		space();
		if (at < n && s[at] == p_close) {
			return ++at, true;
		}
		while (true) {
			PackedByteArray name;
			space();
			if (p_close == '}' && (!str(name) || (space(), at >= n || s[at++] != ':'))) {
				return false;
			}
			if (!p_each(name)) {
				return false;
			}
			space();
			if (at >= n) {
				return false;
			}
			if (s[at] == p_close) {
				return ++at, true;
			}
			if (s[at++] != ',') {
				return false;
			}
		}
	}

	// Validate and skip any value of an unknown field.
	bool skip() {
		space();
		if (at >= n) {
			return false;
		}
		PackedByteArray unused;
		int from;
		switch (s[at]) {
			case '"':
				return str(unused);
			case 't':
				return word("true");
			case 'f':
				return word("false");
			case 'n':
				return word("null");
			case '{':
			case '[': {
				const uint8_t close = s[at++] == '{' ? '}' : ']';
				return items(close, [&](const PackedByteArray &) { return skip(); });
			}
			default:
				return num(from);
		}
	}

	// Match a field name the way case-insensitive decoders do:
	// ASCII letters ignore case, and the long s (U+017F) equals "s".
	static bool same(const PackedByteArray &p_name, const char *p_want) {
		int at = 0;
		for (const char *w = p_want; *w; w++) {
			if (at >= p_name.size()) {
				return false;
			}
			char32_t c;
			at += OnlineWire::rune(p_name.ptr() + at, p_name.size() - at, c);
			c = c == 0x17F ? U's' : (c >= 'A' && c <= 'Z') ? c + 32
														   : c;
			if (c != char32_t(*w)) {
				return false;
			}
		}
		return at == p_name.size();
	}

	// Read one field into the room; null leaves the field unchanged.
	bool field(const PackedByteArray &p_name, OnlineRoom &r_room) {
		static const char *const names[] = { "room", "host", "port", "players", "max", "secret", "free" }; // Wire names in field order.
		int which = -1;
		for (int i = 0; i < 7 && which < 0; i++) {
			if (same(p_name, names[i])) {
				which = i;
			}
		}
		space();
		if (which < 0) {
			return skip();
		}
		if (word("null")) {
			return true;
		}
		switch (which) {
			case 0:
				return str(r_room.id);
			case 1:
				return str(r_room.host);
			case 5:
				if (word("true")) {
					return r_room.secret = true, true;
				}
				return word("false") && (r_room.secret = false, true);
		}
		int64_t *const ints[] = { nullptr, nullptr, &r_room.port, &r_room.players, &r_room.max, nullptr, &r_room.free };
		return at < n && (s[at] == '-' || (s[at] >= '0' && s[at] <= '9')) && integer(*ints[which]);
	}
};

} // namespace

namespace OnlineWire {

// Decode the first JSON value of the body into a room.
// Only null or an object is accepted; bytes after the value are not examined.
// p_more reports body bytes beyond p_body, which cannot complete a value cut at the limit.
bool decode(const PackedByteArray &p_body, bool p_more, OnlineRoom &r_room) {
	Scan in;
	in.s = p_body.ptr();
	in.n = p_body.size();
	r_room = OnlineRoom();
	in.space();
	// A top-level literal ends at the next byte, whatever it is, so only a cut body can fail.
	if (in.word("null")) {
		return in.at < in.n || !p_more;
	}
	if (in.at >= in.n || in.s[in.at++] != '{') {
		return false;
	}
	return in.items('}', [&](const PackedByteArray &p_name) { return in.field(p_name, r_room); });
}

} // namespace OnlineWire
