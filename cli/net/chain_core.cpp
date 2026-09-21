// Authenticate issuer links before applying path-wide identity, usage, name, and policy constraints.
#include "chain_core.h"
#include "hostname_core.h"
#include "policy_core.h"
#include <functional>
#include <set>

namespace GDCrypto {
namespace {
constexpr size_t issuer_attempts = 100; // Candidate-attempt budget includes invalid keys and cycles, stopping before attempt 101 is verified.
constexpr uint8_t server_usage[] = {0x2b,6,1,5,5,7,3,1}; // Server-authentication extended usage selected by default.
constexpr uint8_t any_usage[] = {0x55,0x1d,37,0}; // Extended usage permitting every requested purpose.

// Compare canonical encoded fields without allocating temporary identities.
std::string_view view(Bytes value) { return {reinterpret_cast<const char *>(value.data),value.size}; }

// Retain exact alternative-name encodings when identifying already visited certificates.
Bytes alternative_names(const Cert &certificate) {
	for (const auto &extension : certificate.info().extensions) if (extension.oid.size == 3 && extension.oid.data[0] == 0x55 && extension.oid.data[1] == 0x1d && extension.oid.data[2] == 17) return extension.value;
	return {};
}

// Detect repeated subject-and-key identities while preserving certificates with distinct alternative names.
bool visited(const Cert &candidate, const std::vector<Cert::Ptr> &path) {
	for (const auto &item : path) if (view(item->info().subject) == view(candidate.info().subject) && view(item->info().public_key) == view(candidate.info().public_key) && view(alternative_names(*item)) == view(alternative_names(candidate))) return true;
	return false;
}

// Require recognized critical extensions and inclusive validity endpoints for every selected certificate.
bool current(const Cert &certificate, int64_t now) { return certificate.constraints().unhandled.empty() && now >= certificate.info().not_before && now <= certificate.info().not_after; }

// Prefer matching key identifiers, then missing identifiers, before explicit mismatches.
unsigned priority(const Cert &child, const Cert &parent) {
	const auto authority = child.constraints().authority_id, subject = parent.constraints().subject_id;
	return view(authority) == view(subject) ? 0 : !authority.size || !subject.size ? 1 : 2;
}

// Intersect accepted purposes across the complete path, including the explicit anchor.
bool purposes(const std::vector<Cert::Ptr> &path, const std::vector<Bytes> &requested) {
	const auto any = view({any_usage,sizeof(any_usage)}); std::set<std::string_view> wanted;
	if (requested.empty()) wanted.insert(view({server_usage,sizeof(server_usage)})); else for (Bytes oid : requested) wanted.insert(view(oid));
	if (wanted.count(any)) return true;
	for (const auto &certificate : path) {
		const auto &values = certificate->constraints().extended; if (values.empty()) continue;
		std::set<std::string_view> allowed; for (Bytes oid : values) allowed.insert(view(oid));
		if (allowed.count(any)) continue;
		for (auto found = wanted.begin(); found != wanted.end();) {if (!allowed.count(*found)) found = wanted.erase(found); else ++found;}
		if (wanted.empty()) return false;
	}
	return true;
}

// Apply path-wide constraints only to a path whose links reach an explicit trust anchor.
ChainResult::Error complete(const std::vector<Cert::Ptr> &path, const ChainOptions &options) {
	if (!purposes(path,options.usages)) return ChainResult::USAGE;
	std::vector<std::unique_ptr<CertNames>> names(path.size());
	for (size_t issuer = 1; issuer != path.size(); ++issuer) {
		if (!path[issuer]->constraints().constrained) continue;
		for (size_t child = 0; child != issuer; ++child) {
			if (!names[child]) {names[child] = std::make_unique<CertNames>(); if (!names[child]->reset(path[child]->constraints())) return ChainResult::INVALID;}
			if (!path[issuer]->permits(*names[child])) return ChainResult::INVALID;
		}
	}
	return certificate_policies(path,options.policies) ? ChainResult::NONE : ChainResult::POLICY;
}
}

// Explore issuer candidates under one shared attempt budget and publish only a fully verified path.
bool verify_chain(Cert::Ptr leaf, const CertStore &roots, const CertStore &intermediates, const ChainOptions &options, ChainResult &output) {
	ChainResult result; std::vector<Cert::Ptr> path;
	if (!leaf || !current(*leaf,options.now) || !leaf->key()) {result.error = ChainResult::INVALID; output = std::move(result); return false;}
	if (!options.hostname.empty() && !certificate_hostname(leaf->constraints(),options.hostname)) {result.error = ChainResult::HOSTNAME; output = std::move(result); return false;}
	path.push_back(std::move(leaf));
	std::function<bool()> search = [&]() {
		const auto &child = *path.back();
		if (roots.contains(child)) {
			result.error = complete(path,options); if (result.error == ChainResult::NONE) {result.path = path; return true;} return false;
		}
		for (const CertStore *store : {&roots,&intermediates}) {
			const auto *group = store->issuers(child); if (!group) continue;
			for (unsigned rank = 0; rank != 3; ++rank) for (const auto &parent : *group) {
				if (priority(child,*parent) != rank) continue;
				if (++result.attempts > issuer_attempts) {result.error = ChainResult::LIMIT; return false;}
				if (visited(*parent,path)) continue;
				const auto &info = parent->info(); const auto &rules = parent->constraints();
				if (!current(*parent,options.now) || (info.version == 2 && !rules.basic) || (rules.basic && !rules.ca) || (rules.usage && !(rules.usage&32))) continue;
				if (store == &intermediates && (!rules.basic || !rules.ca)) continue;
				if (rules.basic && rules.path_length >= 0 && path.size()-1 > uint64_t(rules.path_length)) continue;
				const auto *key = parent->key(); if (!key || !verify_certificate_signature(child.info(),*key)) continue;
				path.push_back(parent); const bool valid = search(); path.pop_back();
				if (valid) return true;
				if (result.error == ChainResult::LIMIT) return false;
			}
		}
		return false;
	};
	const bool valid = search(); output = std::move(result); return valid;
}
}
