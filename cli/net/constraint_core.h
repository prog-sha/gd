// Index certificate name constraints for repeated logarithmic membership checks.
#pragma once
#include "extension_core.h"
#include <array>
#include <string>
#include <string_view>

namespace GDCrypto {
// Keep normalized range endpoints without allocating per-address tree nodes.
struct CertRange {
	std::array<uint8_t,16> first{}, last{}; // Inclusive address endpoints, zero-extended for the smaller family.
};

// Borrow sorted suffix constraints and retain exact-host distinctions for mailbox domains.
struct CertDomains {
	struct Rule {
		std::string_view name; // Borrowed domain spelling with an optional leading dot.
		bool exact = false; // Restrict a bare mailbox host to exact equality.
	};
	std::vector<Rule> rules; // Sorted, non-overlapping domain constraints.
	std::vector<std::string_view> parents; // Sorted parents for excluded wildcard overlap checks.
	bool all = false; // Match the explicitly empty domain constraint.
	void prepare(bool wildcard); // Sort and prune covered domains once per issuer.
	bool contains(std::string_view name, bool wildcard) const; // Find a suffix or wildcard overlap by binary search.
};

// Normalize subordinate identities once when several ancestors impose name constraints.
class CertNames {
	struct Entry {
		unsigned family = 0; // DNS, URI, mailbox, or binary address family.
		Bytes raw; // Borrowed DNS or address bytes backed by the subordinate certificate.
		std::string host; // Decoded URI host without its port.
		std::pair<std::string,std::string> mailbox; // Decoded local-part and case-folded domain for exact lookup.
	};
	std::vector<Entry> entries; // Prepared names shared across all ancestor indexes for this verification.
	bool ready = false; // Prevent failed normalization from becoming an empty accepted identity list.
	friend class NameConstraints;
public:
	bool reset(const CertConstraints &subject); // Validate and normalize every recognized name while retaining borrowed byte ownership.
};

// Evaluate one issuer's permitted and excluded names independently of path construction.
class NameConstraints {
	struct Clause {
		std::array<CertDomains,3> domains; // DNS, URI, and mailbox domain constraints.
		std::vector<std::pair<std::string,std::string>> mailboxes; // Exact decoded local-part and folded-domain identities.
		std::array<std::vector<CertRange>,2> addresses; // Sorted non-overlapping IPv4 and IPv6 intervals.
		std::array<bool,4> present{}; // Whether each name family has any constraint.
	};
	std::array<Clause,2> clauses; // Permitted union followed by excluded union.
	bool ready = false, active = false; // Reject use after failed initialization and skip truly absent constraints.
public:
	bool reset(const CertConstraints &issuer); // Prepare immutable indexes while retaining the issuer encoding's lifetime requirement.
	bool check(const CertConstraints &subject) const; // Require every recognized subordinate SAN to satisfy both constraint unions.
	bool check(const CertNames &subject) const; // Reuse normalized names when checking several ancestors.
};
}
