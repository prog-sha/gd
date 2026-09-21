/**************************************************************************/
/*  perm.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Manage script permissions, requiring explicit allows only in strict mode.
// Check existing API entry points without introducing a new public script class.

#include "core/string/ustring.h"
#include "core/templates/vector.h"

class Perm {
public:
	// Permission kinds corresponding directly to allow/deny flags.
	enum Kind {
		READ, // File reads.
		WRITE, // File writes.
		NET, // Network connections.
		ENV, // Environment variables.
		RUN, // Child-process execution.
		EXT, // GDExtension loading.
		SYS, // Platform and environment metadata.
		KIND_MAX,
	};

private:
	// Per-kind allows and denies; an unscoped allow permits the entire kind.
	struct Entry {
		bool allowed = false; // Whether --allow-X was supplied.
		Vector<String> allow_scopes; // Scopes supplied through --allow-X=...
		Vector<String> deny_scopes; // Scopes supplied through --deny-X=...
		bool deny_all = false; // Unscoped --deny-X.
	};

	static Entry entries[KIND_MAX];
	static bool strict; // Enable default-deny behavior.
	static bool allow_all; // -A / --allow-all
	static thread_local int depth; // Trusted-region nesting depth; zero enables permission boundaries.

	// Flag and mount decision, distinguishing unspecified access from explicit denial.
	enum Verdict {
		PASS, // Access permitted.
		DENIED, // Explicit denial by a deny flag or mount escape.
		UNSET, // Neither allowed nor denied explicitly.
	};

	static const char *_kind_name(Kind p_kind);
	static bool _match(Kind p_kind, const Vector<String> &p_scopes, const String &p_target);
	static Verdict _verdict(Kind p_kind, const String &p_target, String *r_why = nullptr);

public:
	// Report trusted internal operations so jailed wrappers can pass operator paths to the base implementation.
	static bool is_trusted() { return depth > 0; }

	// Consume one permission flag, returning true when recognized.
	static bool parse_flag(const String &p_arg);

	// Enable strict default-deny behavior.
	static void set_strict() { strict = true; }

	// Return guidance for an unsupported flag, or empty if none applies.
	static String flag_advice(const String &p_arg);

	// Check permission and explain any denial.
	static bool check(Kind p_kind, const String &p_target);

	// Check explicit IP denies after resolving an already-authorized hostname.
	static bool check_net_ip(const String &p_target);

	// Require host-wide port permission before using a kernel-selected port.
	static bool check_net_any_port(const String &p_host);

	// Inspect existing authorization silently, without diagnostics or prompts.
	// Use this to try one permission kind when either of two kinds can authorize an operation.
	static bool granted(Kind p_kind, const String &p_target);

	// Mark an already-checked region to avoid duplicate checks of the same path.
	//
	// Use trusted regions in two situations.
	// A jailed wrapper delegates after checking the outer path;
	// the base implementation resolves it and invokes virtual methods again.
	// Internal operator-requested work, such as reading an executable and writing compile output.
	//
	// Track depth per thread so another thread's script remains restricted.
	class Trusted {
	public:
		Trusted() { depth++; }
		~Trusted() { depth--; }
	};
};

// Return a permission failure while allowing arbitrary return expressions, including commas.
#define GD_PERM_FAIL_V(m_kind, m_target, ...) \
	if (!Perm::check(Perm::m_kind, m_target)) { \
		return __VA_ARGS__; \
	} else \
		((void)0)

// Permission guard for functions without a return value.
#define GD_PERM_FAIL(m_kind, m_target) \
	if (!Perm::check(Perm::m_kind, m_target)) { \
		return; \
	} else \
		((void)0)
