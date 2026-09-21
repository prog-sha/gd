// Parse certificate domains, mailboxes, and URI authorities with bounded-work forward scans.
#include "name_core.h"
#include "ip.h"

namespace GDCrypto {
namespace {
// Recognize ASCII letters without locale-dependent character classification.
bool letter(char value) { return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z'); }

// Recognize decimal digits without interpreting ports or imposing numeric ranges.
bool digit(char value) { return value >= '0' && value <= '9'; }

// Decode a single percent-escape digit, preserving invalid input as a negative result.
int hex_digit(char value) { return digit(value) ? value-'0' : value >= 'A' && value <= 'F' ? value-'A'+10 : value >= 'a' && value <= 'f' ? value-'a'+10 : -1; }

// Validate percent triples without decoding components whose contents are not needed for identity.
bool escapes(std::string_view value) {
	for (size_t at = 0; at != value.size(); ++at) if (value[at] == '%') {
		if (value.size()-at < 3 || hex_digit(value[at+1]) < 0 || hex_digit(value[at+2]) < 0) return false;
		at += 2;
	}
	return true;
}

// Accept authority bytes whose spelling cannot introduce another URI component.
bool host_byte(char value) { return letter(value) || digit(value) || std::string_view("-._~!$&'()*+,;=:\"<>[]").find(value) != std::string_view::npos; }

// Decode host escapes with distinct scoped-address handling and no recursive unescaping.
bool host_decode(std::string_view value, bool scoped, std::string &output) {
	bool zone = false;
	for (size_t at = 0; at != value.size(); ++at) {
		unsigned char ch = value[at];
		if (ch == '%') {
			if (value.size()-at < 3 || hex_digit(value[at+1]) < 0 || hex_digit(value[at+2]) < 0) return false;
			ch = uint8_t((hex_digit(value[at+1])<<4)|hex_digit(value[at+2])); at += 2;
			if (zone ? (ch != '%' && ch != ' ' && !host_byte(char(ch))) : (ch < 128 && ch != '%')) return false;
			zone |= scoped && ch == '%';
		} else if (!host_byte(char(ch))) return false;
		output += char(ch);
	}
	return true;
}

// Check an optional colon-prefixed decimal port without treating its value as an allocation limit.
bool port(std::string_view value) {
	if (value.empty()) return true;
	if (value.front() != ':') return false;
	for (char ch : value.substr(1)) if (!digit(ch)) return false;
	return true;
}

// Parse user information and host separately so escaped separators cannot alter authority boundaries.
bool authority(std::string_view input, std::string_view scheme, std::string &output) {
	const size_t user = input.rfind('@');
	if (user != input.npos) {
		const auto value = input.substr(0,user);
		for (char ch : value) if (!letter(ch) && !digit(ch) && std::string_view("-._~!$&'()*+,;=:%@").find(ch) == value.npos) return false;
		if (!escapes(value)) return false;
		input.remove_prefix(user+1);
	}
	const size_t bracket = input.rfind('[');
	if (bracket != input.npos) {
		if (bracket != 0) return false;
		const size_t end = input.rfind(']'); if (end == input.npos || !port(input.substr(end+1))) return false;
		std::string address; if (!host_decode(input.substr(1,end-1),true,address) || GDIP::parse(address).bit_len() != 128) return false;
		output = '['+address+']'; output += input.substr(end+1);
	} else {
		const size_t colon = scheme == "postgres" || scheme == "postgresql" ? input.rfind(':') : input.find(':');
		if (colon != input.npos && !port(input.substr(colon))) return false;
		if (!host_decode(input,false,output)) return false;
	}
	return certificate_domain(output,false);
}
}

// Validate label boundaries without imposing DNS wire-format limits on certificate strings.
bool certificate_domain(std::string_view value, bool constraint) {
	if (value.empty()) return true;
	if (constraint && value.front() == '.') value.remove_prefix(1);
	if (value.empty() || value.front() == '.' || value.back() == '.') return false;
	char previous = 0;
	for (unsigned char ch : value) {if (ch < 33 || ch > 126 || (ch == '.' && previous == '.')) return false; previous = char(ch);}
	return true;
}

// Decode mailbox quoting with a forward state machine and keep local-part case significant.
bool certificate_mailbox(std::string_view value, std::string &local, std::string_view &domain) {
	if (value.empty()) return false;
	std::string decoded; size_t at = 0; const bool quoted = value.front() == '"'; bool closed = !quoted;
	if (quoted) ++at;
	while (at != value.size()) {
		unsigned char ch = value[at++];
		if (quoted && ch == '"') {closed = true; break;}
		if (!quoted && ch == '@') {--at; break;}
		if (ch == '\\') {
			if (at == value.size()) return false;
			ch = value[at++];
			if (quoted && (!ch || ch == 10 || ch == 13 || ch > 127)) return false;
		} else if (quoted) {
			if (!ch || ch == 9 || ch == 10 || ch == 13 || ch > 127) return false;
		} else if (!letter(char(ch)) && !digit(char(ch)) && std::string_view("!#$%&'*+-/=?^_`{|}~.").find(char(ch)) == value.npos) return false;
		decoded += char(ch);
	}
	if (!closed || at == value.size() || value[at] != '@') return false;
	if (!quoted && (decoded.empty() || decoded.front() == '.' || decoded.back() == '.' || decoded.find("..") != decoded.npos)) return false;
	const auto suffix = value.substr(at+1); if (suffix.find('@') != suffix.npos || !certificate_domain(suffix,false)) return false;
	local = std::move(decoded); domain = suffix; return true;
}

// Separate URI components before validating escaping, preserving opaque and raw-query semantics.
bool certificate_uri(std::string_view value, std::string &host) {
	for (unsigned char ch : value) if (ch > 127) return false;
	const size_t fragment = value.find('#');
	if (fragment != value.npos) {if (!escapes(value.substr(fragment+1))) return false; value = value.substr(0,fragment);}
	for (unsigned char ch : value) if (ch < 32 || ch == 127) return false;
	value = value.substr(0,value.find('?'));
	std::string scheme;
	const size_t colon = value.find(':'), slash = value.find('/');
	if (colon != value.npos && (slash == value.npos || colon < slash)) {
		const auto prefix = value.substr(0,colon);
		if (prefix.empty() || !letter(prefix.front())) return false;
		for (char ch : prefix) {if (!letter(ch) && !digit(ch) && ch != '+' && ch != '-' && ch != '.') return false; scheme += ch >= 'A' && ch <= 'Z' ? ch+32 : ch;}
		value.remove_prefix(colon+1);
	}
	std::string result;
	if (!scheme.empty() && !value.empty() && value.front() != '/') {host.clear(); return true;}
	if (value.substr(0,2) == "//" && (!scheme.empty() || value.substr(0,3) != "///")) {
		value.remove_prefix(2); const size_t end = value.find('/');
		if (!authority(value.substr(0,end),scheme,result)) return false;
		value = end == value.npos ? std::string_view{} : value.substr(end);
	}
	if (!escapes(value)) return false;
	host = std::move(result); return true;
}
}
