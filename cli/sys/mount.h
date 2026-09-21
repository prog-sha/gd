/**************************************************************************/
/*  mount.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Restrict script filesystem access to named mount roots.
//
// Configure mounts at startup as --mount name=path:mode, using name as the path scheme.
// Scripts access mounts through paths such as log://; unmounted locations remain inaccessible.
//
// Paths without a scheme belong to res://; absolute paths use the machine root.
// Strict mode confines absolute paths to res:// and applies its read-only boundary.
// Windows supports res:// and user:// only, not named mounts or absolute paths.
//
// Implementation is in mount.cpp.

#pragma once

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/list.h"

class Mount {
public:
	// Flag parsing result; invalid syntax prevents startup.
	enum Parse {
		NOT_MINE, // Not a --mount flag.
		TAKEN, // Accepted as a mount.
		BAD, // A --mount flag with invalid syntax.
	};

	// One mount's native path and access mode.
	struct Box {
		String root; // Absolute native root without a trailing slash.
		bool can_write = false; // Read-only or read-write access.
		bool own = false; // Whether internal storage ownership must also be checked.
		int fd = -1; // Pinned Unix root, unaffected by path replacement.
	};

	// Parent descriptor and final component for one Unix operation.
	struct At {
		int fd = -1; // Descriptor of the parent containing the final component.
		String leaf; // Final name relative to the parent.
		String full; // Native path used in diagnostics.
		bool root = false; // Whether traversal starts at the machine root.

		At() = default;
		~At();
		At(const At &) = delete;
		At &operator=(const At &) = delete;
	};

private:
	static const char *ROOT; // Internal mount name for ordinary absolute paths.
	static HashMap<String, Box> boxes;
	static HashMap<String, String> pkgs; // Package alias to the scheme path holding its files.
	static bool strict; // Confine implicit access to the read-only startup directory.
	static bool pkg_mode; // Package-management mode permitting cache:// writes.

	static bool _reserved(const String &p_name);
	static String _under(const String &p_name, const String &p_tail);
	static String _root_of(const String &p_path);
	static String _solid(const String &p_path, int p_hops = 0);
	static bool _path(const String &p_path, bool p_write, String &r_name, String &r_tail, String &r_full, String &r_why);
	static bool _own_dir(const String &p_path);
	static bool _add(const char *p_name, const String &p_raw, bool p_write, bool p_own);
	static bool _pkg(const String &p_path, String &r_path, String &r_why);
	static bool _open_roots();

public:
	// Parse --mount name=path:mode.
	static Parse parse_flag(const String &p_arg);
	// Confine implicit access to res:// and disable its writes.
	static void set_strict() { strict = true; }
	// Enable package-command writes to cache://.
	static void set_pkg_mode() { pkg_mode = true; }
	// Mount one package checkout read-only and return its private scheme path.
	static String local(const String &p_path);
	// Point pkg://alias or pkg://@scope/name@version at a directory under another mount, such as cache:// or res://pkg.
	static void set_pkg(const String &p_alias, const String &p_root) { pkgs[p_alias] = p_root; }
	// Rewrite pkg://alias/rest onto its registered root so res:// reaches packed data; return other paths unchanged.
	static String unalias(const String &p_path);
	// List every registered alias and canonical id.
	static void pkg_keys(List<String> *r_keys) {
		for (const KeyValue<String, String> &e : pkgs) {
			r_keys->push_back(e.key);
		}
	}
	// Return the scheme path registered for a key, or empty.
	// Tell whether a scheme names a mount that file access can reach.
	static bool has_mount(const String &p_name) { return boxes.has(p_name); }
	static String pkg_root(const String &p_key) {
		const String *root = pkgs.getptr(p_key);
		return root ? *root : String();
	}

	// Create built-in mounts after ProjectSettings initialization.
	// Return false and stop startup if internal storage is not owned by this user.
	static bool setup();
	// Report whether strict file isolation is enabled.
	static bool strict_mode() { return strict; }

	// Resolve to a native path, returning empty and a reason when access is denied.
	// Treat the request as a write when p_write is true.
	static String resolve(const String &p_path, bool p_write, String &r_why);

	// Resolve a Unix mount to a pinned parent and final name without following intermediate symlinks.
	// Return a machine-readable error for the rejected operation.
	// Callers distinguish permission denial from absence using that error, not text alone.
	// Return a final symlink itself only when explicitly requested for deletion.
	static bool at(const String &p_path, bool p_write, At &r_at, String &r_why, Error *r_err = nullptr, bool p_leaf_link = false);

	// Expand only the scheme when the underlying implementation opens the path.
	// Do not check permissions here; unknown schemes return empty.
	static String real(const String &p_path);

	// Extract the leading scheme, or return empty with the original path in r_tail.
	// Validate scheme syntax here so every path consumer uses the same rules.
	static String scheme_of(const String &p_path, String &r_tail);

	// Check normalized-path containment at the shared filesystem boundary.
	static bool inside(const String &p_root, const String &p_full);

	// Check whether a normalized path identifies the mount root itself.
	static bool is_root(const String &p_path);

	// Return the owning mount name; paths without a scheme belong to res://.
	// Return empty for unknown mounts and absolute paths without a public scheme.
	static String name_of(const String &p_path);

	// Accept lowercase alphanumeric mount names and hyphens only.
	static bool name_ok(const String &p_name);
};
