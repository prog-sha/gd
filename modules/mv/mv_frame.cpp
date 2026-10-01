/**************************************************************************/
/*  mv_frame.cpp                                                          */
/**************************************************************************/

// Packs and opens one message per line. Uses its own base64 step so the data is copied only once.

#include "mv_frame.h"

#include "core/crypto/crypto_core.h"
#include "core/io/marshalls.h"

static const char *MV_MAGIC = "MVF1"; // Line prefix. Tells frames apart from startup messages printed by the engine

// Turns one message into one text line, keeping types such as Vector3, Color and integers.
// Builds the result in one pass; an intermediate string would copy the body again
String MVFrame::pack(int p_type, const Dictionary &p_body) {
	if (!plain_at(p_body, 0)) {
		return String();
	}
	int len = 0;
	if (encode_variant(p_body, nullptr, len, false) != OK || len <= 0 ||
			((int64_t(len) + 2) / 3) * 4 + 16 > BUF_MAX) {
		return String();
	}
	LocalVector<uint8_t> raw;
	raw.resize(len);
	if (encode_variant(p_body, raw.ptr(), len, false) != OK) {
		return String();
	}
	LocalVector<uint8_t> b64;
	b64.resize((size_t)len / 3 * 4 + 8);
	size_t b64n = 0;
	if (CryptoCore::b64_encode(b64.ptr(), b64.size(), &b64n, raw.ptr(), len) != OK) {
		return String();
	}
	const String head = String(MV_MAGIC) + " " + itos(p_type) + " ";
	const int hn = head.length();
	String out;
	out.resize_uninitialized(hn + (int)b64n + 2); // Room for the newline and the terminator
	char32_t *w = out.ptrw();
	const char32_t *h = head.ptr();
	for (int i = 0; i < hn; i++) {
		w[i] = h[i];
	}
	for (size_t i = 0; i < b64n; i++) {
		w[hn + i] = b64[i];
	}
	w[hn + b64n] = '\n';
	w[hn + b64n + 1] = 0;
	return out;
}

// Opens one line into type and body. A malformed line is treated as engine output and yields empty so it is dropped.
// Splits prefix, type and body by position only, without cutting substrings
Dictionary MVFrame::open(const String &p_line) {
	const char32_t *s = p_line.ptr();
	int n = p_line.length();
	int a = 0;
	while (a < n && s[a] <= ' ') {
		a++;
	}
	while (n > a && s[n - 1] <= ' ') {
		n--;
	}
	const int mn = 4; // Prefix length
	if (n - a < mn + 2 || s[a] != 'M' || s[a + 1] != 'V' || s[a + 2] != 'F' || s[a + 3] != '1' || s[a + mn] != ' ') {
		return Dictionary();
	}
	int i = a + mn + 1;
	bool neg = false;
	// Once the prefix and separator are verified, the sign position always holds one character
	if (s[i] == '-' || s[i] == '+') {
		neg = s[i] == '-';
		i++;
	}
	int type = 0;
	const int num_at = i;
	while (i < n && s[i] >= '0' && s[i] <= '9') {
		// Limits digits. The type uses only one byte, so a long number is already malformed
		if (i - num_at >= 5) {
			return Dictionary();
		}
		type = type * 10 + (s[i] - '0');
		i++;
	}
	if (i == num_at || i >= n || s[i] != ' ') {
		return Dictionary();
	}
	i++;
	// The body is base64 characters only; shapes containing whitespace are rejected.
	// The previous char is the separator and trailing whitespace is trimmed, so at least one char remains
	const int bn = n - i;
	LocalVector<uint8_t> b64;
	b64.resize(bn);
	for (int k = 0; k < bn; k++) {
		const char32_t c = s[i + k];
		if (c <= ' ' || c > 0x7F) {
			return Dictionary();
		}
		b64[k] = (uint8_t)c;
	}
	LocalVector<uint8_t> raw;
	raw.resize((size_t)bn / 4 * 3 + 1);
	size_t rawn = 0;
	if (CryptoCore::b64_decode(raw.ptr(), raw.size(), &rawn, b64.ptr(), bn) != OK) {
		return Dictionary();
	}
	Variant body;
	if (decode_variant(body, raw.ptr(), (int)rawn, nullptr, false) != OK) {
		return Dictionary();
	}
	if (body.get_type() != Variant::DICTIONARY || !plain_at(body, 0)) {
		return Dictionary();
	}
	Dictionary out;
	out["type"] = neg ? -type : type;
	out["body"] = body;
	return out;
}

// Checks that a value is made of plain values only.
// The decoder rejects objects carrying code, but containers wrapping a peer's IDs still pass.
// Received data is used directly as numbers or positions, so non-values and overly deep nesting are dropped here
bool MVFrame::plain_at(const Variant &p_v, int p_depth) {
	if (p_depth > DEPTH_MAX) {
		return false;
	}
	switch (p_v.get_type()) {
		case Variant::OBJECT:
		case Variant::RID:
		case Variant::CALLABLE:
		case Variant::SIGNAL:
			return false;
		// Only these two can contain the types above. Packed arrays need no inspection
		case Variant::ARRAY: {
			const Array a = p_v;
			for (int i = 0; i < a.size(); i++) {
				if (!plain_at(a[i], p_depth + 1)) {
					return false;
				}
			}
		} break;
		case Variant::DICTIONARY: {
			const Dictionary d = p_v;
			for (const KeyValue<Variant, Variant> &kv : d) {
				if (!plain_at(kv.key, p_depth + 1) || !plain_at(kv.value, p_depth + 1)) {
					return false;
				}
			}
		} break;
		default:
			break;
	}
	return true;
}

// Size of a value on the wire. Used to know how much fits before splitting
int MVFrame::size_of(const Variant &p_v) {
	int size = 0;
	return encode_variant(p_v, nullptr, size, false) == OK ? size : 0;
}

// Entry point that checks a value consists only of values sendable over the wire
bool MVFrame::plain(const Variant &p_v) {
	return plain_at(p_v, 0);
}

const char *MVFrame::NODE_KEY = "@node";

// Matches count and types against the author's annotations. Only int->float is silently converted
Dictionary MVFrame::arguments(const Dictionary &p_spec, const Array &p_args) {
	Dictionary result;
	result["ok"] = false;
	const int count = p_spec.get("count", 0);
	const Array types = p_spec.get("types", Array());
	if (p_args.size() != count || types.size() != count || !plain_at(p_args, 0)) {
		return result;
	}
	Array out = p_args.duplicate(true);
	for (int i = 0; i < count; i++) {
		const Variant::Type want = Variant::Type(int(types[i]));
		const Variant::Type got = out[i].get_type();
		if (want == Variant::NIL || want == got) {
			continue;
		}
		if (want == Variant::FLOAT && got == Variant::INT) {
			out[i] = double(int64_t(out[i]));
			continue;
		}
		// Node-typed arguments arrive as serial numbers. Converting back to a Node is the world owner's job
		if (want == Variant::OBJECT && got == Variant::DICTIONARY && Dictionary(out[i]).has(NODE_KEY)) {
			continue;
		}
		return result;
	}
	result["ok"] = true;
	result["args"] = out;
	return result;
}

// Appends arriving fragments and extracts completed lines only.
// Cuts off a stall when too much piles up: keeps completed lines, drops only the partial one
Array MVFrame::feed(const String &p_text) {
	buf += p_text;
	if (buf.length() > BUF_MAX) {
		int last = buf.rfind_char('\n');
		buf = last < 0 ? String() : buf.substr(0, last + 1);
	}
	Array out;
	int start = 0;
	int i = buf.find_char('\n', start);
	while (i >= 0) {
		Dictionary f = open(buf.substr(start, i - start));
		if (!f.is_empty()) {
			out.push_back(f);
		}
		start = i + 1;
		i = buf.find_char('\n', start);
	}
	// Cuts the remaining partial line once; re-cutting per line would grow quadratically with length
	if (start > 0) {
		buf = buf.substr(start);
	}
	return out;
}

// Used from C++ only. Not exposed to GDScript
void MVFrame::_bind_methods() {
}
