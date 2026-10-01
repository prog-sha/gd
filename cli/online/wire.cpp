/**************************************************************************/
/*  wire.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Produce and check the matchmaking wire format: game paths, announcements, grants, and replies.

#include "cli/data/codec.h"
#include "cli/data/hash_core.h"
#include "cli/online/match.h"

namespace OnlineWire {

// Take the game name from the path; allowed characters keep keys and log lines unambiguous.
bool game_of(const String &p_path, String &r_game) {
	int from = 0, to = p_path.length();
	while (from < to && p_path[from] == '/') {
		from++;
	}
	while (to > from && p_path[to - 1] == '/') {
		to--;
	}
	r_game = p_path.substr(from, to - from);
	if (r_game.length() > 64) {
		return false;
	}
	for (int i = 0; i < r_game.length(); i++) {
		const char32_t c = r_game[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '-' && c != '.' && c != '/') {
			return false;
		}
	}
	return true;
}

// Check announcement fields; the identity gateway carries only an address and a free count.
bool valid(const OnlineRoom &p_room) {
	if (p_room.port < 1 || p_room.port > 65535 || p_room.host.size() > 255) {
		return false;
	}
	if (p_room.secret) {
		return p_room.free >= 0;
	}
	if (p_room.id.is_empty() || p_room.id.size() > 64) {
		return false;
	}
	if (p_room.max < 1 || p_room.max > 256 || p_room.players < 0 || p_room.players > p_room.max) {
		return false;
	}
	for (const uint8_t c : p_room.id) {
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '-') {
			return false;
		}
	}
	return true;
}

// Sign a grant payload with the shared key.
String sign(const String &p_key, const String &p_payload) {
	const PackedByteArray mac = Hash::hmac_sha256(p_key.to_utf8_buffer(), p_payload.to_utf8_buffer());
	return String::hex_encode_buffer(mac.ptr(), mac.size());
}

// Mint a short-lived hosting grant so only the chosen client can open a room.
String grant(const String &p_key, int64_t p_now) {
	uint8_t nonce[8]; // Random part keeping grants distinct within one second.
	if (p_key.is_empty() || !GDCrypto::random_fill(nonce, sizeof(nonce))) {
		return String();
	}
	const String payload = itos(p_now + GRANT_LIFE) + "." + String::hex_encode_buffer(nonce, sizeof(nonce));
	return payload + "." + sign(p_key, payload);
}

// Append a JSON string from UTF-8 bytes, escaping HTML-sensitive and line-separator characters.
// Invalid UTF-8 becomes U+FFFD.
static void quote(String &r_out, const PackedByteArray &p_text) {
	static const char *const hex = "0123456789abcdef"; // Lowercase escape digits.
	r_out += "\"";
	for (int i = 0; i < p_text.size();) {
		char32_t c;
		const int size = rune(p_text.ptr() + i, p_text.size() - i, c);
		const bool bad = c == 0xFFFD && !(size == 3 && p_text[i] == 0xEF); // Replacement for invalid input.
		i += size;
		static const char *const shorts = "\"\\\b\f\n\r\t"; // Characters with short escapes.
		static const char *const brief[] = { "\\\"", "\\\\", "\\b", "\\f", "\\n", "\\r", "\\t" }; // Their escapes.
		const char *hit = c > 0 && c < 0x80 ? strchr(shorts, int(c)) : nullptr;
		if (hit) {
			r_out += brief[hit - shorts];
		} else if (c < 0x20 || c == '<' || c == '>' || c == '&') {
			r_out += "\\u00";
			r_out += char32_t(hex[c >> 4]);
			r_out += char32_t(hex[c & 15]);
		} else if (c == 0x2028 || c == 0x2029 || bad) {
			r_out += c == 0x2028 ? "\\u2028" : c == 0x2029 ? "\\u2029"
														   : "\\ufffd";
		} else {
			r_out += c;
		}
	}
	r_out += "\"";
}

// Encode a destination; the hosting flag and grant appear only when set.
PackedByteArray place(const Variant &p_room, const Variant &p_host, int64_t p_port, const String &p_grant, bool p_host_here) {
	String out = "{\"room\":";
	quote(out, bytes(p_room));
	out += ",\"host\":";
	quote(out, bytes(p_host));
	out += ",\"port\":" + itos(p_port);
	if (p_host_here) {
		out += ",\"you_host\":true";
	}
	if (!p_grant.is_empty()) {
		out += ",\"grant\":";
		quote(out, p_grant.to_utf8_buffer());
	}
	return (out + "}\n").to_utf8_buffer();
}

// Read stored text as UTF-8 bytes; other types give nothing.
PackedByteArray bytes(const Variant &p_value) {
	if (p_value.get_type() == Variant::STRING) {
		return String(p_value).to_utf8_buffer();
	}
	return p_value.get_type() == Variant::PACKED_BYTE_ARRAY ? PackedByteArray(p_value) : PackedByteArray();
}

// Read a stored count; anything but plain decimal digits is zero, and huge values wrap like 64-bit integers.
int64_t number(const Variant &p_value) {
	if (p_value.get_type() == Variant::INT) {
		return p_value;
	}
	uint64_t total = 0;
	for (const uint8_t c : bytes(p_value)) {
		if (c < '0' || c > '9') {
			return 0;
		}
		total = total * 10 + (c - '0');
	}
	return int64_t(total);
}

} // namespace OnlineWire
