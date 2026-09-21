/**************************************************************************/
/*  perm.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement permission decisions declared in perm.h.
//
// Store whole-kind or destination-scoped authorization separately for each permission kind.
// Check authorization whenever a protected API entry point is called.

#include "cli/sys/perm.h"
#include "cli/sys/system.h"

#include "cli/api/text.h"
#include "cli/net/address.h"
#include "cli/sys/mount.h"

#include "cli/net/datagram.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

Perm::Entry Perm::entries[Perm::KIND_MAX];
bool Perm::strict = false;
bool Perm::allow_all = false;
thread_local int Perm::depth = 0;

// Names used in flags and diagnostics.
const char *Perm::_kind_name(Kind p_kind) {
	switch (p_kind) {
		case NET:
			return "net";
		case ENV:
			return "env";
		case RUN:
			return "run";
		case EXT:
			return "ext";
		case SYS:
			return "sys";
		default:
			return "";
	}
}

// Make relative paths absolute and normalize both scope and target before comparison.
static String _abs(const String &p_path) {
	const String s = p_path.is_absolute_path() ? p_path : GDSystem::cwd().path_join(p_path);
	return s.simplify_path();
}

// Check whether an allowed network name and actual destination identify the same host.
// Include standard loopback IPs so localhost authorization survives name resolution.
static bool _net_same(const String &p_scope, const String &p_target) {
	const int scope_at = p_scope.find("%");
	const int target_at = p_target.find("%");
	const String scope = (scope_at < 0 ? p_scope : p_scope.substr(0, scope_at)).to_lower();
	const String target = (target_at < 0 ? p_target : p_target.substr(0, target_at)).to_lower();
	// Unzoned rules apply to all interfaces; explicit zones match only the same interface.
	uint32_t scope_zone = 0;
	uint32_t target_zone = 0;
	if (scope_at >= 0 && (!scope.contains(":") || !GDDatagram::is_ip(scope) ||
			!GDAddress::zone(p_scope.substr(scope_at + 1), scope_zone))) return false;
	if (target_at >= 0 && (!target.contains(":") || !GDDatagram::is_ip(target) ||
			!GDAddress::zone(p_target.substr(target_at + 1), target_zone))) return false;
	if (scope_at >= 0 && scope_zone != target_zone) return false;
	if (scope == target) {
		return true;
	}
	// Wildcard domain rules cover only descendants of the specified domain.
	if (scope.begins_with("*.") && !GDDatagram::is_ip(target)) {
		// Do not interpret IPv6 zones or literals as domain suffixes.
		if (target.contains("%") || target.contains(":") || target.contains("[") || target.contains("]")) {
			return false;
		}
		const String base = scope.substr(2);
		if (base.is_empty() || base.begins_with(".") || base.ends_with(".") || base.contains("..") || GDDatagram::is_ip(base)) {
			return false;
		}
		const String suffix = "." + base;
		return target.length() > suffix.length() && target.ends_with(suffix);
	}
	const bool scope_ip = GDDatagram::is_ip(scope);
	const bool target_ip = GDDatagram::is_ip(target);
	if (scope_ip && target_ip) {
		return GDAddress::equal(scope, target);
	}
	if (scope != "localhost" || !target_ip) {
		return false;
	}
	return GDAddress::loopback(target);
}

// Match path scopes by component-aware prefix and other scopes by name.
bool Perm::_match(Kind p_kind, const Vector<String> &p_scopes, const String &p_target) {
	for (const String &s : p_scopes) {
		if (p_kind == RUN || p_kind == EXT) {
			// Normalize both executable/extension scopes and targets before prefix comparison.
			// This prevents alternate paths such as /usr/bin/../bin/sh from changing scope interpretation.
			// It also prevents spellings such as /usr/bin/./id from bypassing a deny.
			if (Mount::inside(_abs(s), _abs(p_target))) {
				return true;
			}
			if (p_kind == RUN && p_target == s) {
				return true; // Executable name resolved through PATH.
			}
		} else if (p_kind == NET) {
			// Require matching hosts and either equal ports or an omitted port.
			// A permission without a port permits all ports.
			// A portless lookup target may match a port-scoped permission.
			String s_port;
			String t_port;
			const String s_host = Url::host_port(s, s_port);
			const String t_host = Url::host_port(p_target, t_port);
			if (!_net_same(s_host, t_host)) {
				continue;
			}
			if (s_port.is_empty() || t_port.is_empty() || s_port == t_port) {
				return true;
			}
		} else if (p_target == s) {
			return true;
		}
	}
	return false;
}

// Return replacement syntax for unsupported flags, or empty when none applies.
//
// File access is configured by mounts, not read/write flags.
// Explain the supported syntax instead of leaving callers with only an unknown-flag error.
String Perm::flag_advice(const String &p_arg) {
	for (int k = 0; k < 2; k++) {
		const String name = k == 0 ? "read" : "write";
		for (int deny = 0; deny < 2; deny++) {
			const String head = (deny ? "--deny-" : "--allow-") + name;
			if (p_arg == head || p_arg.begins_with(head + "=")) {
				return vformat("\"%s\" is not a flag. Files are decided by mounts. "
							   "Write --mount=<name>=<path>:%s and use <name>:// in the script.",
						head, k == 0 ? "r" : "rw");
			}
		}
	}
	return String();
}

// Split a command-line flag into name and value.
bool Perm::parse_flag(const String &p_arg) {
	if (p_arg == "-A" || p_arg == "--allow-all") {
		allow_all = true;
		return true;
	}
	// --allow-X / --allow-X=a,b / --deny-X / --deny-X=a,b
	for (int k = 0; k < KIND_MAX; k++) {
		if (k == READ || k == WRITE) {
			continue;
		}
		const String name = _kind_name((Kind)k);
		for (int deny = 0; deny < 2; deny++) {
			const String head = (deny ? "--deny-" : "--allow-") + name;
			if (p_arg != head && !p_arg.begins_with(head + "=")) {
				continue;
			}
			Entry &e = entries[k];
			if (p_arg == head) {
				if (deny) {
					e.deny_all = true;
				} else {
					e.allowed = true;
				}
			} else {
				const String list = p_arg.substr(head.length() + 1);
				for (const String &item : list.split(",", false)) {
					if (deny) {
						e.deny_scopes.push_back(item.strip_edges());
					} else {
						e.allowed = true;
						e.allow_scopes.push_back(item.strip_edges());
					}
				}
			}
			return true;
		}
	}
	return false;
}

// Evaluate flags and mounts, optionally returning the reason for denial.
Perm::Verdict Perm::_verdict(Kind p_kind, const String &p_target, String *r_why) {
	if (depth > 0) {
		return PASS; // Trusted internal operation.
	}

	// File access is controlled by mounts rather than permission flags.
	// Embedded package contents must remain within the res:// mount too.
	// Package entry names may be caller-controlled, so package membership is not authorization.
	// A name pointing outside its mount must not bypass path checks.
	if (p_kind == READ || p_kind == WRITE) {
		String why;
		if (!Mount::resolve(p_target, p_kind == WRITE, why).is_empty()) {
			return PASS;
		}
		if (r_why) {
			*r_why = why;
		}
		return DENIED;
	}

	const Entry &e = entries[p_kind];
	const char *name = _kind_name(p_kind);

	// Explicit denial takes precedence.
	if (e.deny_all || _match(p_kind, e.deny_scopes, p_target)) {
		if (r_why) {
			*r_why = vformat("%s access to \"%s\" is denied by --deny-%s.", name, p_target, name);
		}
		return DENIED;
	}
	if (!strict) {
		return PASS; // Trust the project outside strict mode.
	}
	if (allow_all) {
		return PASS;
	}
	if (e.allowed && (e.allow_scopes.is_empty() || _match(p_kind, e.allow_scopes, p_target))) {
		return PASS;
	}
	if (r_why) {
		*r_why = vformat("requires %s access to \"%s\", run again with the --allow-%s flag.", name, p_target, name);
	}
	return UNSET;
}

// Report whether a permission is granted.
bool Perm::granted(Kind p_kind, const String &p_target) {
	return _verdict(p_kind, p_target) == PASS;
}

// Check whether input satisfies the permission rules.
bool Perm::check(Kind p_kind, const String &p_target) {
	String why;
	const Verdict v = _verdict(p_kind, p_target, &why);
	if (v == PASS) {
		return true;
	}
	ERR_PRINT(vformat("PermissionDenied: %s", why));
	return false;
}

// After authorizing the original host, check only explicit denies on resolved IPs.
bool Perm::check_net_ip(const String &p_target) {
	if (depth > 0) {
		return true;
	}
	const Entry &e = entries[NET];
	if (!e.deny_all && !_match(NET, e.deny_scopes, p_target)) {
		return true;
	}
	ERR_PRINT(vformat("PermissionDenied: net access to \"%s\" is denied by --deny-net.", p_target));
	return false;
}

// Require an unscoped-port host permission before selecting a port in the kernel.
bool Perm::check_net_any_port(const String &p_host) {
	if (depth > 0) {
		return true;
	}
	const Entry &e = entries[NET];
	auto matches_host = [&](const Vector<String> &p_scopes) {
		for (const String &scope : p_scopes) {
			String port;
			const String host = Url::host_port(scope, port);
			if (port.is_empty() && _net_same(host, p_host)) {
				return true;
			}
		}
		return false;
	};
	if (e.deny_all || matches_host(e.deny_scopes)) {
		ERR_PRINT(vformat("PermissionDenied: net access to all ports on \"%s\" is denied by --deny-net.", p_host));
		return false;
	}
	if (!strict || allow_all || (e.allowed && (e.allow_scopes.is_empty() || matches_host(e.allow_scopes)))) {
		return true;
	}
	ERR_PRINT(vformat("PermissionDenied: kernel-selected port requires net access to all ports on \"%s\".", p_host));
	return false;
}
