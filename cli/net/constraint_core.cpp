// Use sorted suffixes and disjoint address intervals to avoid quadratic certificate-name comparisons.
#include "constraint_core.h"
#include "name_core.h"
#include "ip.h"
#include <algorithm>
#include <cstring>

namespace GDCrypto {
namespace {
// Fold only ASCII domain case while preserving mailbox local-part spelling.
char folded(char ch) { return ch >= 'A' && ch <= 'Z' ? ch+32 : ch; }

// Order domains by reversed labels with the label separator before every printable character.
int domain_order(std::string_view left, std::string_view right) {
	const size_t common = std::min(left.size(),right.size());
	for (size_t at = 0; at != common; ++at) {
		const char a = left[left.size()-1-at], b = right[right.size()-1-at];
		const int x = a == '.' ? 0 : uint8_t(folded(a)), y = b == '.' ? 0 : uint8_t(folded(b));
		if (x != y) return x < y ? -1 : 1;
	}
	return left.size() == right.size() ? 0 : left.size() < right.size() ? -1 : 1;
}

// Match a complete host or label suffix without allocating normalized domain copies.
bool covered(const CertDomains::Rule &rule, std::string_view value) {
	if (rule.name.size() > value.size() || (rule.exact && rule.name.size() != value.size())) return false;
	if (domain_order(rule.name,value.substr(value.size()-rule.name.size()))) return false;
	return rule.name.empty() || rule.name.size() == value.size() || rule.name.front() == '.' || value[value.size()-rule.name.size()-1] == '.';
}

// Normalize case only for identities stored in the exact mailbox index.
std::string lowercase(std::string_view value) { std::string output(value); for (auto &ch : output) ch = folded(ch); return output; }

// Borrow name bytes without introducing a null-terminated string requirement.
std::string_view text(Bytes value) { return {reinterpret_cast<const char *>(value.data),value.size}; }

// Merge sorted overlapping address ranges into a compact lookup table.
void ranges(std::vector<CertRange> &values) {
	std::sort(values.begin(),values.end(),[](const auto &a,const auto &b) {return a.first == b.first ? a.last > b.last : a.first < b.first;});
	size_t count = 0;
	for (size_t at = 0; at != values.size(); ++at) {
		if (count && values[at].first <= values[count-1].last) values[count-1].last = std::max(values[count-1].last,values[at].last);
		else values[count++] = values[at];
	}
	values.resize(count);
}

// Locate the interval whose start immediately precedes the queried address.
bool address_contains(const std::vector<CertRange> &values, Bytes input) {
	std::array<uint8_t,16> key{}; if (input.size != 4 && input.size != 16) return false;
	std::memcpy(key.data(),input.data,input.size);
	auto found = std::upper_bound(values.begin(),values.end(),key,[](const auto &a,const auto &b) {return a < b.first;});
	return found != values.begin() && key <= (--found)->last;
}
}

// Sort suffixes, discard fully covered entries, and index wildcard parent intersections.
void CertDomains::prepare(bool wildcard) {
	std::sort(rules.begin(),rules.end(),[](const auto &a,const auto &b) {const int order = domain_order(a.name,b.name); return order ? order < 0 : a.exact < b.exact;});
	size_t count = 0;
	for (size_t at = 0; at != rules.size(); ++at) {
		if (rules[at].name.empty()) all = true;
		if (!count || !covered(rules[count-1],rules[at].name)) rules[count++] = rules[at];
	}
	rules.resize(count);
	if (!wildcard) return;
	for (const auto &rule : rules) {const size_t dot = rule.name.find('.'); if (dot != rule.name.npos) parents.push_back(rule.name.substr(dot));}
	std::sort(parents.begin(),parents.end(),[](auto a,auto b) {return domain_order(a,b) < 0;});
	parents.erase(std::unique(parents.begin(),parents.end(),[](auto a,auto b) {return !domain_order(a,b);}),parents.end());
}

// Query the nearest suffix and optionally test excluded wildcard overlap against indexed parents.
bool CertDomains::contains(std::string_view name, bool wildcard) const {
	if (all) return true;
	auto found = std::upper_bound(rules.begin(),rules.end(),name,[](auto value,const auto &rule) {return domain_order(value,rule.name) < 0;});
	if (found != rules.begin() && covered(*--found,name)) return true;
	if (!wildcard || name.empty() || name.front() != '*') return false;
	const size_t dot = name.find('.'); if (dot == name.npos) return false;
	return std::binary_search(parents.begin(),parents.end(),name.substr(dot),[](auto a,auto b) {return domain_order(a,b) < 0;});
}

// Build per-family indexes without applying an arbitrary count or comparison limit.
bool NameConstraints::reset(const CertConstraints &issuer) {
	ready = false; active = issuer.constrained; clauses = {};
	for (unsigned side = 0; side != 2; ++side) {
		auto &clause = clauses[side];
		for (const auto &subtree : side ? issuer.excluded : issuer.permitted) {
			if (subtree.minimum || subtree.maximum != -1) return false;
			const auto &name = subtree.name; const unsigned family = name.tag == 0x82 ? 0 : name.tag == 0x86 ? 1 : name.tag == 0x81 ? 2 : 3;
			if (name.tag != 0x81 && name.tag != 0x82 && name.tag != 0x86 && name.tag != 0x87) continue;
			clause.present[family] = true;
			if (family == 3) {
				if (name.value.size != 8 && name.value.size != 32) return false;
				const size_t width = name.value.size/2; CertRange range;
				for (size_t at = 0; at != width; ++at) {const uint8_t mask = name.value.data[width+at]; range.first[at] = name.value.data[at]&mask; range.last[at] = range.first[at]|uint8_t(~mask);}
				clause.addresses[width == 16].push_back(range);
			} else {
				const auto value = text(name.value);
				if (family == 2 && value.find('@') != value.npos) {
					std::string local; std::string_view domain; if (!certificate_mailbox(value,local,domain)) return false;
					clause.mailboxes.emplace_back(std::move(local),lowercase(domain));
				} else clause.domains[family].rules.push_back({value,family == 2 && !value.empty() && value.front() != '.'});
			}
		}
		for (unsigned family = 0; family != 3; ++family) clause.domains[family].prepare(side && family != 2);
		for (auto &values : clause.addresses) ranges(values);
		std::sort(clause.mailboxes.begin(),clause.mailboxes.end());
		clause.mailboxes.erase(std::unique(clause.mailboxes.begin(),clause.mailboxes.end()),clause.mailboxes.end());
	}
	ready = true; return true;
}

// Prepare each subordinate name once before applying any ancestor's constraint indexes.
bool CertNames::reset(const CertConstraints &subject) {
	ready = false; entries.clear();
	for (const auto &name : subject.names) {
		if (name.tag != 0x81 && name.tag != 0x82 && name.tag != 0x86 && name.tag != 0x87) continue;
		Entry entry; const unsigned family = entry.family = name.tag == 0x82 ? 0 : name.tag == 0x86 ? 1 : name.tag == 0x81 ? 2 : 3;
		entry.raw = name.value; std::string_view domain = family == 3 ? std::string_view{} : text(name.value);
		if (family == 1) {
			auto &owned = entry.host; if (!certificate_uri(domain,owned) || owned.empty()) return false;
			if (owned.front() == '[') return false;
			const size_t colon = owned.find(':');
			if (colon != owned.npos) {if (owned.find(':',colon+1) != owned.npos || owned.find_first_of("[]") != owned.npos) return false; owned.resize(colon);}
			if (GDIP::parse(owned).bit_len()) return false;
			domain = owned;
		} else if (family == 2) {if (!certificate_mailbox(domain,entry.mailbox.first,domain)) return false; entry.mailbox.second = lowercase(domain);}
		if (family != 3 && !certificate_domain(domain,false)) return false;
		if (family == 3 && name.value.size != 4 && name.value.size != 16) return false;
		entries.push_back(std::move(entry));
	}
	ready = true; return true;
}

// Validate identities only when this issuer actually has name constraints.
bool NameConstraints::check(const CertConstraints &subject) const {
	if (!ready) return false;
	if (!active) return true;
	CertNames names; return names.reset(subject) && check(names);
}

// Apply immutable indexes without reparsing URI or mailbox identities for each ancestor.
bool NameConstraints::check(const CertNames &subject) const {
	if (!ready || !subject.ready) return false;
	if (!active) return true;
	for (const auto &name : subject.entries) {
		const unsigned family = name.family;
		const std::string_view domain = family == 0 ? text(name.raw) : family == 1 ? std::string_view(name.host) : std::string_view(name.mailbox.second);
		for (unsigned side = 0; side != 2; ++side) {
			const auto &clause = clauses[side]; if (!clause.present[family]) continue;
			bool match;
			if (family == 3) match = address_contains(clause.addresses[name.raw.size == 16],name.raw);
			else match = clause.domains[family].contains(domain,side && family != 2) || (family == 2 && std::binary_search(clause.mailboxes.begin(),clause.mailboxes.end(),name.mailbox));
			if (side ? match : !match) return false;
		}
	}
	return true;
}
}
