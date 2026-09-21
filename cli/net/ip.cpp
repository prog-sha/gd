// Parse strict IP literals, format RFC 5952 addresses, and match prefixes within each address family.
#include "ip.h"

#include <charconv>

namespace {

// Read four decimal octets, rejecting leading zeros, signs, abbreviations, and trailing characters.
bool ipv4(std::string_view p_text, uint32_t &r_ip) {
	r_ip = 0;
	for (int i = 0; i < 4; i++) {
		const size_t dot = p_text.find('.');
		const auto part = p_text.substr(0, dot);
		if (part.empty() || (part.size() > 1 && part[0] == '0') || (dot == p_text.npos) != (i == 3)) {
			return false;
		}
		unsigned value = 0;
		for (char c : part) {
			if (c < '0' || c > '9' || (value = value * 10 + c - '0') > 255) {
				return false;
			}
		}
		r_ip = (r_ip << 8) | value;
		if (i < 3) {
			p_text.remove_prefix(dot + 1);
		}
	}
	return true;
}

// Append a 16-bit group in lowercase hexadecimal or an octet in decimal.
void number(std::string &r_text, unsigned p_value, int p_base) {
	char buf[8]; // Hexadecimal digit capacity for an unsigned 32-bit value.
	const auto result = std::to_chars(buf, buf + sizeof(buf), p_value, p_base);
	r_text.append(buf, result.ptr);
}

} // namespace

// Read eight 16-bit IPv6 groups and fill the range abbreviated by :: with zeros.
GDIP GDIP::parse(std::string_view p_text) {
	GDIP out;
	const size_t first = p_text.find_first_of(".:");
	if (first == p_text.npos) {
		return {};
	}
	if (p_text[first] == '.') {
		uint32_t ip;
		if (!ipv4(p_text, ip)) {
			return {};
		}
		out.lo = ip;
		out.width = 32;
		return out;
	}
	// Preserve the zone verbatim without enumerating interfaces or resolving names.
	std::string_view zone;
	const size_t scope = p_text.find('%');
	if (scope != p_text.npos) {
		if (scope + 1 == p_text.size()) {
			return {};
		}
		zone = p_text.substr(scope + 1);
		p_text = p_text.substr(0, scope);
	}
	uint16_t words[8] = {}; // Eight groups forming a 128-bit IPv6 address.
	int count = 0;
	int gap = -1;
	if (p_text.substr(0, 2) == "::") {
		gap = 0;
		p_text.remove_prefix(2);
	}
	while (!p_text.empty()) {
		if (count == 8) {
			return {};
		}
		const size_t colon = p_text.find(':');
		const auto part = p_text.substr(0, colon);
		// A dotted quad may occupy only the final 32 bits.
		if (part.find('.') != part.npos) {
			uint32_t ip;
			if (count > 6 || !ipv4(p_text, ip)) {
				return {};
			}
			words[count++] = ip >> 16;
			words[count++] = ip & 0xffff;
			break;
		}
		if (part.empty() || part.size() > 4) {
			return {};
		}
		unsigned value = 0;
		for (char c : part) {
			const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (digit < 0) {
				return {};
			}
			value = value * 16 + digit;
		}
		words[count++] = value;
		if (colon == p_text.npos) {
			break;
		}
		p_text.remove_prefix(colon + 1);
		if (p_text.empty()) {
			return {};
		}
		if (p_text[0] == ':') {
			if (gap >= 0) {
				return {};
			}
			gap = count;
			p_text.remove_prefix(1);
		}
	}
	if (gap < 0 ? count != 8 : count == 8) {
		return {};
	}
	if (gap >= 0) {
		const int missing = 8 - count;
		for (int i = count - 1; i >= gap; i--) {
			words[i + missing] = words[i];
		}
		for (int i = gap; i < gap + missing; i++) {
			words[i] = 0;
		}
	}
	for (int i = 0; i < 8; i++) {
		uint64_t &half = i < 4 ? out.hi : out.lo;
		half = (half << 16) | words[i];
	}
	out.width = 128;
	// Allocate zone storage only after the address has been validated.
	out.zone = zone;
	return out;
}

// Preserve IPv4-mapped addresses and choose the first longest run of IPv6 zeros.
std::string GDIP::text() const {
	if (width == 0) {
		return "invalid IP";
	}
	std::string out;
	if (width == 32 || (hi == 0 && lo >> 32 == 0xffff)) {
		if (width == 128) {
			out = "::ffff:";
		}
		for (int i = 3; i >= 0; i--) {
			if (i != 3) {
				out += '.';
			}
			number(out, (lo >> (i * 8)) & 255, 10);
		}
	} else {
		uint16_t words[8]; // 16-bit groups used to select the RFC 5952 abbreviation.
		for (int i = 0; i < 8; i++) {
			words[i] = (i < 4 ? hi : lo) >> ((3 - i % 4) * 16);
		}
		int start = -1, length = 1;
		for (int i = 0; i < 8;) {
			const int begin = i;
			while (i < 8 && words[i] == 0) {
				i++;
			}
			if (i - begin > length) {
				start = begin;
				length = i - begin;
			}
			i++;
		}
		for (int i = 0; i < 8; i++) {
			if (i == start) {
				out += "::";
				i += length - 1;
				continue;
			}
			if (!out.empty() && out.back() != ':') {
				out += ':';
			}
			number(out, words[i], 16);
		}
	}
	if (!zone.empty()) {
		out += '%';
		out += zone;
	}
	return out;
}

// Check family and scope before comparing prefix bits with a 128-bit XOR.
bool GDIP::contains(const GDIP &p_ip, int p_bits) const {
	if (!width || width != p_ip.width || zoned() || p_ip.zoned() || p_bits < 0 || p_bits > width) {
		return false;
	}
	if (p_bits == 0) {
		return true;
	}
	if (width == 32) {
		return ((lo ^ p_ip.lo) >> (32 - p_bits)) == 0;
	}
	if (p_bits <= 64) {
		return ((hi ^ p_ip.hi) >> (64 - p_bits)) == 0;
	}
	return hi == p_ip.hi && ((lo ^ p_ip.lo) >> (128 - p_bits)) == 0;
}

// Export numeric address bytes without reparsing text or including a host-local scope identifier.
size_t GDIP::pack(uint8_t *p_output) const {
	if (!p_output || !width) return 0;
	const size_t count = size_t(width/8);
	for (size_t at = 0; at != count; ++at) {
		const size_t shift = (count-1-at)*8;
		p_output[at] = uint8_t((shift >= 64 ? hi : lo) >> (shift%64));
	}
	return count;
}
