// Match endpoint names with explicit numeric-address handling and ASCII-only DNS folding.
#include "hostname_core.h"
#include "ip.h"
#include <array>
#include <cstring>

namespace GDCrypto {
namespace {
// Fold domain case without changing non-ASCII bytes or locale state.
char lowercase(char value) { return value >= 'A' && value <= 'Z' ? value+32 : value; }

// Compare complete domain spellings, preserving invalid names for exact-only matching.
bool same(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) return false;
	for (size_t at = 0; at != a.size(); ++at) if (lowercase(a[at]) != lowercase(b[at])) return false;
	return true;
}

// Validate label grammar before enabling wildcard or terminal-dot normalization.
bool wildcard_grammar(std::string_view value, bool pattern) {
	if (!pattern && !value.empty() && value.back() == '.') value.remove_suffix(1);
	if (value.empty() || value == "*" || value.back() == '.') return false;
	size_t begin = 0;
	for (size_t at = 0; at <= value.size(); ++at) {
		if (at != value.size() && value[at] != '.') continue;
		if (at == begin) return false;
		const auto label = value.substr(begin,at-begin);
		if (!(pattern && !begin && label == "*")) for (size_t pos = 0; pos != label.size(); ++pos) {
			const char ch = lowercase(label[pos]);
			if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || (ch == '-' && pos))) return false;
		}
		begin = at+1;
	}
	return true;
}

// Treat a mapped address and its four-byte representation as the same numeric endpoint.
Bytes unmapped(Bytes value) {
	constexpr uint8_t prefix[] = {0,0,0,0,0,0,0,0,0,0,255,255}; // IPv4-mapped address prefix.
	if (value.size == 16 && !std::memcmp(value.data,prefix,sizeof(prefix))) return {value.data+12,4};
	return value;
}
}

// Match a requested endpoint exclusively against the corresponding alternative-name family.
bool certificate_hostname(const CertConstraints &certificate, std::string_view host) {
	std::string_view numeric = host;
	if (numeric.size() >= 3 && numeric.front() == '[' && numeric.back() == ']') {numeric.remove_prefix(1); numeric.remove_suffix(1);}
	const auto address = GDIP::parse(numeric); std::array<uint8_t,16> encoded{};
	const size_t count = address.pack(encoded.data());
	if (count) {
		const Bytes expected = unmapped({encoded.data(),count});
		for (const auto &name : certificate.names) if (name.tag == 0x87) {
			const Bytes candidate = unmapped(name.value);
			if (candidate.size == expected.size && !std::memcmp(candidate.data,expected.data,expected.size)) return true;
		}
		return false;
	}
	const bool valid = wildcard_grammar(host,false);
	for (const auto &name : certificate.names) if (name.tag == 0x82) {
		const std::string_view pattern(reinterpret_cast<const char *>(name.value.data),name.value.size);
		if (pattern.empty() || host.empty() || pattern == "." || host == ".") continue;
		if (!valid || !wildcard_grammar(pattern,true)) {if (same(pattern,host)) return true; continue;}
		auto candidate = host; if (candidate.back() == '.') candidate.remove_suffix(1);
		if (pattern.substr(0,2) == "*.") {const size_t dot = candidate.find('.'); if (dot != candidate.npos && same(pattern.substr(1),candidate.substr(dot))) return true;}
		else if (same(pattern,candidate)) return true;
	}
	return false;
}
}
