/**************************************************************************/
/*  pkgmap.h                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Point pkg:// at installed packages before any script is parsed.
//
// gd.json names the project's packages by alias; gd.lock pins every registry package,
// direct or transitive, as @scope/name@version with the aliases its own imports resolved to.
// Each pinned package gets its canonical path pkg://@scope/name@version/ and a table of
// its aliases, so a package resolves pkg://alias/ through its own gd.json, not the project's.
//
// Pure script packages resolve into the shared cache; place "project" in gd.json keeps
// copies under pkg/ instead, which tools that only read res:// can use. Native extensions
// always live under pkg/ because their loader needs real files. A package the project
// imports is copied as pkg/<alias>/; one only other packages need is pkg/@scope/name@version/.
// A compiled executable resolves everything under its embedded pkg/.
//
// Fetch what gd.json names but the machine lacks by running gd install as a child,
// so the same permission flags decide whether the network may be used.

#include "cli/main/cmd.h"
#include "cli/sys/mount.h"
#include "cli/sys/perm.h"
#include "cli/sys/pkgscope.h"
#include "cli/sys/pkgsource.h"
#include "cli/sys/system.h"
#include "cli/tool/compile.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

class GDPkgMap {
	static constexpr const char *REGISTRY = "https://gd.progsha.com/pkg"; // Default registry, matching pkg.gd.
	static constexpr const char *LOCAL = "res://pkg"; // Package copies beside gd.json.

	// Read the registry the way pkg.gd does: gd.json, then the environment, then the default.
	static String _registry(const Dictionary &p_cfg) {
		const String from_cfg = p_cfg.has("registry") ? String(p_cfg["registry"]) : String();
		if (!from_cfg.is_empty()) {
			return from_cfg.trim_suffix("/");
		}
		if (Perm::granted(Perm::ENV, "GD_REGISTRY") && !GDSystem::env("GD_REGISTRY").is_empty()) {
			return GDSystem::env("GD_REGISTRY").trim_suffix("/");
		}
		return REGISTRY;
	}

	// Read one JSON object, or nothing when the file is absent or not an object.
	static Dictionary _read(const String &p_path) {
		Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
		if (f.is_null()) {
			return Dictionary();
		}
		JSON json;
		if (json.parse(f->get_as_text()) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
			return Dictionary();
		}
		return json.get_data();
	}

	// Read gd.lock whole: imports pin each alias's version, packages hold every pinned package.
	static Dictionary _lock() {
		return _read("res://gd.lock");
	}

	// Root a copied checkout at the directory of its main script, which is the package root it publishes.
	static String _copy_root(const String &p_alias) {
		const String copy = String(LOCAL).path_join(p_alias);
		const String main = _read(copy.path_join("gd.json")).get("main", "");
		return main.get_base_dir().is_empty() ? copy : copy.path_join(main.get_base_dir());
	}

	// Split @scope/name@range at the final at sign, keeping the package name.
	static String _pkg_of(const String &p_body) {
		const int at = p_body.rfind("@");
		return at > 0 ? p_body.substr(0, at) : p_body;
	}

	// Recognize native and explicit relative checkout paths without accepting URI schemes.
	static bool _is_local(const String &p_spec) {
		return p_spec.begins_with("./") || p_spec.begins_with("../") ||
				(p_spec.is_absolute_path() && !p_spec.contains("://"));
	}

	// Distinguish registry identifiers from URL and checkout sources.
	static bool _is_registry(const String &p_spec) {
		return !p_spec.begins_with("http://") && !p_spec.begins_with("https://") && !_is_local(p_spec);
	}

	// Register an alias for a local checkout or a url package, which have no canonical id.
	// A checkout is copied under pkg/<alias>/ with its scripts relocated; it is stale, and
	// copied again by gd install whenever its source snapshot differs.
	static void _map_plain(const String &p_alias, const String &p_spec, bool p_local, bool p_embedded, bool &r_missing) {
		String root;
		if (_is_local(p_spec)) {
			root = _copy_root(p_alias);
			Mount::set_pkg(p_alias, root);
			// The copy is this package whether or not a lock names it, so both spellings share one identity.
			PkgScope::set_dir(p_alias, "local:" + p_alias);
			if (p_embedded) {
				return;
			}
			const String source = p_spec.is_absolute_path() ? p_spec : GDSystem::cwd().path_join(p_spec).simplify_path();
			const String copy = GDSystem::cwd().path_join("pkg").path_join(p_alias);
			const Perm::Trusted trust; // The checkout gd.json names sits outside the jail.
			const String stamp = PkgSource::stamp(source);
			Ref<FileAccess> saved = FileAccess::open(copy.path_join(".gd-source"), FileAccess::READ);
			r_missing = stamp.is_empty() || saved.is_null() || saved->get_as_text() != stamp;
			return;
		}
		if (p_local) {
			root = String(LOCAL).path_join(p_alias);
		} else {
			root = "cache://pkg/_ready/_url/" + p_spec.sha256_text().substr(0, 32) + "/" + p_alias;
		}
		Mount::set_pkg(p_alias, root);
		r_missing = !FileAccess::exists(root.path_join("mod.gd"));
	}

	// Register every pinned package under its canonical id, with its alias table and copy directory.
	static bool _map_pinned(const Dictionary &p_cfg, const Dictionary &p_packages, bool p_local, HashMap<String, String> &r_roots) {
		const String hash = _registry(p_cfg).sha256_text().substr(0, 32);
		bool missing = false;
		for (const Variant &k : p_packages.keys()) {
			const String id = String(k);
			if (id.begins_with("local:") && p_packages[k].get_type() == Variant::DICTIONARY) {
				PkgScope::set_dir(id.trim_prefix("local:"), id);
				const Dictionary entry = p_packages[k];
				const Dictionary imports = entry.has("imports") ? Dictionary(entry["imports"]) : Dictionary();
				for (const Variant &a : imports.keys()) {
					const String dep = imports[a];
					PkgScope::set_alias(id, String(a), dep.substr(dep.find_char(':') + 1));
				}
				continue;
			}
			if (!PkgScope::is_id(id) || p_packages[k].get_type() != Variant::DICTIONARY) {
				continue; // Url packages are keyed by alias and mapped from gd.json.
			}
			const Dictionary entry = p_packages[k];
			const String leaf = String(entry.has("url") ? entry["url"] : Variant("")).get_file();
			const bool ext = leaf.ends_with(".gdextension");
			const String dir = PkgScope::dir_of(id);
			PkgScope::set_dir(dir, id);
			// Pure packages are read from the relocated form the cache derives from the verified files.
			const String root = ext || p_local
					? String(LOCAL).path_join(dir)
					: "cache://pkg/_ready/_registry/" + hash + "/" + _pkg_of(id) + "/" + id.substr(id.rfind("@") + 1);
			Mount::set_pkg(id, root);
			r_roots[id] = root;
			const Dictionary imports = entry.has("imports") ? Dictionary(entry["imports"]) : Dictionary();
			for (const Variant &a : imports.keys()) {
				// Resolutions carry their kind, gd:@scope/name@version or ext:@scope/name@version.
				const String dep = String(imports[a]);
				PkgScope::set_alias(id, String(a), dep.substr(dep.find_char(':') + 1));
			}
			// A pure package is fetched entry first; if a helper failed, the entry alone must not count.
			// Native libraries are placed with their manifest in one step, and only the platform's own.
			if (ext) {
				missing = missing || !FileAccess::exists(root.path_join(leaf));
				continue;
			}
			const Dictionary files = entry.has("files") ? Dictionary(entry["files"]) : Dictionary();
			bool complete = FileAccess::exists(root.path_join("mod.gd"));
			for (const Variant &f : files.keys()) {
				complete = complete && FileAccess::exists(root.path_join(String(f)));
			}
			missing = missing || !complete;
		}
		return missing;
	}

	// Register every alias, returning whether any package still has to be fetched.
	static bool _map_all(const Dictionary &p_cfg, const Dictionary &p_imports, bool p_local, bool p_embedded) {
		const Dictionary lock = _lock();
		const Dictionary locked = lock.has("imports") ? Dictionary(lock["imports"]) : Dictionary();
		const Dictionary packages = lock.has("packages") ? Dictionary(lock["packages"]) : Dictionary();
		bool missing = !p_embedded && (!lock.has("requests") || lock["requests"].get_type() != Variant::DICTIONARY || Dictionary(lock["requests"]) != p_imports);
		// Name the project's registry packages first so their copies keep the alias directory.
		for (const Variant &k : p_imports.keys()) {
			// Aliases become path segments, so refuse anything that could leave pkg/ or read as an id.
			const String alias = String(k);
			if (alias.is_empty() || alias == "." || alias == ".." || alias.begins_with("@") || !alias.is_valid_filename()) {
				print_error(vformat("gd.json imports has an unusable alias \"%s\"", alias));
				return true;
			}
			const String spec = String(p_imports[k]);
			if (!_is_registry(spec)) {
				continue;
			}
			const String version = locked.has(alias) ? String(locked[alias]) : String();
			if (version.is_empty()) {
				missing = true;
				continue;
			}
			PkgScope::set_alias(String(), alias, _pkg_of(spec.trim_prefix("gd:").trim_prefix("ext:")) + "@" + version);
		}
		HashMap<String, String> roots;
		missing = _map_pinned(p_cfg, packages, p_local, roots) || missing;
		for (const Variant &k : p_imports.keys()) {
			const String alias = String(k);
			const String spec = String(p_imports[k]);
			if (!_is_registry(spec)) {
				bool absent = false;
				_map_plain(alias, spec, p_local, p_embedded, absent);
				missing = missing || absent;
				continue;
			}
			const String id = PkgScope::resolve(String(), alias);
			const String *root = id.is_empty() ? nullptr : roots.getptr(id);
			if (!root) {
				missing = true; // Pinned but not in packages, or not pinned at all.
				continue;
			}
			Mount::set_pkg(alias, *root);
		}
		return missing;
	}

	public:
	// Read where gd.json places packages: the shared cache unless it says project,
	// or unless this is an editor project, whose tools read only res://.
	static String place(const Dictionary &p_cfg) {
		if (p_cfg.has("place")) {
			return String(p_cfg["place"]);
		}
		return FileAccess::exists("res://project.godot") ? String("project") : String("cache");
	}

	// Resolve aliases, fetching absent packages first; return false when scripts cannot start.
	static bool prepare() {
		const Dictionary cfg = Cmd::load_config();
		const Dictionary imports = cfg.has("imports") ? Dictionary(cfg["imports"]) : Dictionary();
		const Dictionary locked_source = _lock();
		if (imports.is_empty() && locked_source.is_empty()) {
			return true;
		}
		if (locked_source.has("registry") && String(locked_source["registry"]) != _registry(cfg)) {
			print_error("registry differs from gd.lock; migrate the lock explicitly");
			return false;
		}
		const String where = place(cfg);
		if (where != "cache" && where != "project") {
			print_error(vformat("gd.json place must be \"cache\" or \"project\", not \"%s\"", where));
			return false;
		}
		const bool embedded = GDCompile::has_embedded();
		const bool local = where == "project" || embedded;
		if (!_map_all(cfg, imports, local, embedded) || embedded) {
			// An executable carries what its entry reached; anything else fails where it is loaded.
			return true;
		}
		// Let the child decide network access from the same flags this run received.
		List<String> args;
		for (const String &flag : Cmd::flags) {
			args.push_back(flag);
		}
		args.push_back("install");
		args.push_back("--sync"); // Fetch what is missing and refresh changed checkouts, without re-verifying the rest.
		int code = EXIT_FAILURE;
		const Error err = OS::get_singleton()->execute(OS::get_singleton()->get_executable_path(), args, nullptr, &code, false, nullptr, false, true);
		if (err != OK || code != 0) {
			print_error("cannot fetch the packages named in gd.json; see gd install above");
			return false;
		}
		PkgScope::clear();
		if (_map_all(cfg, imports, local, embedded)) {
			print_error("packages are still missing after gd install");
			return false;
		}
		return true;
	}
};
