// Track policy reachability with one graph node per certificate policy and indexed parent expectations.
#include "policy_core.h"
#include <algorithm>
#include <map>
#include <set>
#include <string_view>

namespace GDCrypto {
namespace {
constexpr char any_bytes[] = {0x55,0x1d,0x20,0}; // Encoded identifier for the universal certificate policy.
const std::string_view any_policy(any_bytes,sizeof(any_bytes)); // Stable borrowed key for the synthetic initial policy.

// Borrow canonical policy identifiers and names without copying certificate-owned storage.
std::string_view view(Bytes value) { return {reinterpret_cast<const char *>(value.data),value.size}; }

// Keep one shared policy node with parent links and its next-certificate expectations.
struct PolicyNode {
	std::string_view oid; // Policy identifier asserted at this depth.
	std::vector<size_t> parents; // Stable indexes of prior-depth parent nodes.
	std::vector<std::string_view> expected; // Distinct policy identifiers accepted from the next certificate.
};

// Restrict a remaining-depth counter only when an extension supplies a smaller nonnegative value.
void restrict_counter(size_t &counter, int64_t constraint) { if (constraint >= 0 && uint64_t(constraint) < counter) counter = size_t(constraint); }
}

// Propagate policy expectations through a signed path while preserving mandatory explicit-policy failures.
bool certificate_policies(const std::vector<Cert::Ptr> &path, const std::vector<Bytes> &requested) {
	if (path.empty()) return false;
	for (const auto &cert : path) if (!cert) return false;
	if (path.size() == 1) return true;
	std::vector<PolicyNode> nodes{{any_policy,{}, {any_policy}}};
	std::map<std::string_view,size_t> frontier{{any_policy,0}};
	size_t explicit_left = path.size(), mapping_left = path.size(), any_left = path.size();
	bool available = true;
	for (size_t depth = path.size()-1; depth != 0;) {
		--depth; const auto &cert = *path[depth]; const auto &rules = cert.constraints();
		const bool self_issued = view(cert.info().issuer) == view(cert.info().subject);
		if (rules.policies.empty()) {available = false; frontier.clear();}
		if (!explicit_left && !available) return false;
		if (available) {
			std::map<std::string_view,std::vector<size_t>> expected;
			for (const auto &entry : frontier) for (auto policy : nodes[entry.second].expected) expected[policy].push_back(entry.second);
			std::map<std::string_view,size_t> next;
			const auto universal = frontier.find(any_policy);
			// Reuse a shared parent list when multiple predecessor policies converge on one identifier.
			auto append = [&](std::string_view oid, const std::vector<size_t> &parents) {
				const size_t index = nodes.size(); nodes.push_back({oid,parents,{oid}}); next.emplace(oid,index);
			};
			bool has_any = false;
			for (Bytes encoded : rules.policies) {
				const auto policy = view(encoded);
				if (policy == any_policy) {has_any = true; continue;}
				const auto parents = expected.find(policy);
				if (parents != expected.end()) append(policy,parents->second);
				else if (universal != frontier.end()) append(policy,{universal->second});
			}
			if (has_any && (any_left || (depth && self_issued))) for (const auto &entry : expected) if (!next.count(entry.first)) append(entry.first,entry.second);
			if (depth) {
				std::map<std::string_view,std::vector<std::string_view>> mappings;
				for (const auto &mapping : rules.mappings) {
					const auto issuer = view(mapping.first), subject = view(mapping.second);
					if (issuer == any_policy || subject == any_policy) return false;
					if (mapping_left) mappings[issuer].push_back(subject); else next.erase(issuer);
				}
				for (auto &mapping : mappings) {
					auto found = next.find(mapping.first);
					if (found == next.end() && next.count(any_policy) && universal != frontier.end()) {append(mapping.first,{universal->second}); found = next.find(mapping.first);}
					if (found == next.end()) continue;
					auto &values = mapping.second; std::sort(values.begin(),values.end()); values.erase(std::unique(values.begin(),values.end()),values.end());
					nodes[found->second].expected = std::move(values);
				}
			}
			frontier = std::move(next);
		}
		if (depth) {
			if (!self_issued) {if (explicit_left) --explicit_left; if (mapping_left) --mapping_left; if (any_left) --any_left;}
			restrict_counter(explicit_left,rules.explicit_policy); restrict_counter(mapping_left,rules.mapping); restrict_counter(any_left,rules.any_policy);
		}
	}
	if (explicit_left) --explicit_left;
	if (path.front()->constraints().explicit_policy == 0) explicit_left = 0;
	if (explicit_left) return true;
	if (!available || frontier.empty()) return false;
	std::set<std::string_view> wanted; for (Bytes value : requested) wanted.insert(view(value));
	const bool any_requested = wanted.empty() || (wanted.size() == 1 && wanted.count(any_policy));
	if (frontier.count(any_policy)) return true;
	// Traverse only surviving ancestry so pruned paths cannot contribute an accepted policy.
	std::vector<uint8_t> live(nodes.size()); std::vector<size_t> pending;
	for (const auto &entry : frontier) pending.push_back(entry.second);
	while (!pending.empty()) {
		const size_t index = pending.back(); pending.pop_back(); if (live[index]) continue; live[index] = 1;
		const auto &node = nodes[index];
		if (node.oid != any_policy && node.parents.size() == 1 && nodes[node.parents.front()].oid == any_policy && (any_requested || wanted.count(node.oid))) return true;
		pending.insert(pending.end(),node.parents.begin(),node.parents.end());
	}
	return false;
}
}
