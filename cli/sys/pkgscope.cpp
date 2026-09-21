/**************************************************************************/
/*  pkgscope.cpp                                                          */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement package scoping declared in pkgscope.h.

#include "cli/sys/pkgscope.h"

#include "cli/sys/mount.h"

#include "core/extension/gdextension.h"
#include "core/extension/gdextension_manager.h"
#include "core/io/config_file.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"

HashMap<String, HashMap<String, String>> PkgScope::tables;
HashMap<String, String> PkgScope::dirs;
HashMap<String, String> PkgScope::class_owners;
bool PkgScope::classes_scanned = false;
Mutex PkgScope::class_mutex;

static thread_local const String *active_path = nullptr; // Lexical script running on this thread.

// Enter a lexical package scope while retaining the preceding script path.
PkgScope::Frame::Frame(const String *p_path) : previous(active_path) {
	active_path = p_path;
}

// Restore the lexical package scope that surrounded this frame.
PkgScope::Frame::~Frame() {
	active_path = previous;
}

static const char *PKG_SCHEME = "pkg://"; // Scheme used for package identity and loading.
static const char *PKG_COPIES = "res://pkg/"; // Project directory holding placed package copies.

// Register one alias in the importing package's dependency table.
void PkgScope::set_alias(const String &p_owner, const String &p_alias, const String &p_id) {
	tables[p_owner][p_alias] = p_id;
}

// Associate a placed package directory with its canonical identity.
void PkgScope::set_dir(const String &p_dir, const String &p_id) {
	dirs[p_dir] = p_id;
}

// Reset package ownership before loading a new project graph.
void PkgScope::clear() {
	tables.clear();
	dirs.clear();
	MutexLock lock(class_mutex);
	class_owners.clear();
	classes_scanned = false;
}

// A canonical id is @scope/name@version: it starts with @ and carries a second @ after the slash.
bool PkgScope::is_id(const String &p_key) {
	if (!p_key.begins_with("@")) {
		return false;
	}
	const int slash = p_key.find_char('/');
	return slash > 1 && p_key.find_char('@', slash) > slash + 1;
}

// Split the segment after the scheme; a canonical id spans two segments because of its scope.
static String _lead(const String &p_tail, String *r_rest) {
	int cut = p_tail.find_char('/');
	if (p_tail.begins_with("@") && cut >= 0) {
		cut = p_tail.find_char('/', cut + 1);
	}
	if (cut < 0) {
		if (r_rest) {
			*r_rest = String();
		}
		return p_tail;
	}
	if (r_rest) {
		*r_rest = p_tail.substr(cut + 1);
	}
	return p_tail.substr(0, cut);
}

// Split a package path into its alias or identity and optional remaining path.
String PkgScope::key_of(const String &p_path, String *r_rest) {
	if (!p_path.begins_with(PKG_SCHEME)) {
		if (r_rest) {
			*r_rest = String();
		}
		return String();
	}
	return _lead(p_path.trim_prefix(PKG_SCHEME), r_rest);
}

// Choose the smallest project alias independently of manifest insertion order.
String PkgScope::dir_of(const String &p_id) {
	const HashMap<String, String> *app = tables.getptr(String());
	String alias;
	if (app) {
		for (const KeyValue<String, String> &e : *app) {
			if (e.value == p_id && (alias.is_empty() || e.key < alias)) {
				alias = e.key;
			}
		}
	}
	return alias.is_empty() ? p_id : alias;
}

// Scripts named on the command line arrive as bare relative paths; read them as res:// like the loader does.
static String _resource(const String &p_path) {
	if (p_path.contains("://") || p_path.is_absolute_path()) {
		return p_path;
	}
	return ("res://" + p_path).simplify_path();
}

// Return the package identity that owns one script or extension path.
String PkgScope::owner_of(const String &p_raw) {
	const String p_script_path = _resource(p_raw);
	if (p_script_path.begins_with(PKG_SCHEME)) {
		const String key = key_of(p_script_path);
		if (is_id(key)) {
			return key;
		}
		const String id = resolve(String(), key);
		// A local package owns its scripts even when it imports nothing, so project aliases never leak into it.
		const String *copy = dirs.getptr(key);
		return id.is_empty() && copy && copy->begins_with("local:") ? *copy : id;
	}
	if (p_script_path.begins_with(PKG_COPIES)) {
		const String dir = _lead(p_script_path.trim_prefix(PKG_COPIES), nullptr);
		const String *id = dirs.getptr(dir);
		return id ? *id : String();
	}
	return String();
}

// Resolve an alias only through the dependency table of its lexical owner.
String PkgScope::resolve(const String &p_owner, const String &p_alias) {
	const HashMap<String, String> *table = tables.getptr(p_owner);
	if (!table) {
		return String();
	}
	const String *id = table->getptr(p_alias);
	return id ? *id : String();
}

// Expand a package alias through the importing script's declared dependencies.
String PkgScope::expand(const String &p_path, const String &p_from, String &r_why) {
	String rest;
	const String key = key_of(p_path, &rest);
	if (key.is_empty() || is_id(key)) {
		return p_path;
	}
	const String owner = owner_of(p_from);
	const String id = resolve(owner, key);
	if (id.is_empty()) {
		if (owner == "local:" + key) {
			return p_path; // A local package reaching its own scripts through the alias the project gave it.
		}
		if (!owner.is_empty()) {
			r_why = vformat("%s does not import \"%s\". Packages see only the imports of their own gd.json.", owner, key);
			return String();
		}
		return p_path; // A local or url alias, or an unknown one that the mount reports.
	}
	return rest.is_empty() ? String(PKG_SCHEME) + id : String(PKG_SCHEME) + id + "/" + rest;
}

// Name a copy under pkg/ by its pkg:// path, the identity every other spelling resolves to.
String PkgScope::of_copy(const String &p_path) {
	if (!p_path.begins_with(PKG_COPIES)) {
		return p_path;
	}
	String rest;
	const String dir = _lead(p_path.trim_prefix(PKG_COPIES), &rest);
	const String *id = dirs.getptr(dir);
	if (!id) {
		return p_path;
	}
	if (!id->begins_with("local:")) {
		return rest.is_empty() ? String(PKG_SCHEME) + *id : String(PKG_SCHEME) + *id + "/" + rest;
	}
	// A local package has no canonical id; the project reads it by its alias, rooted at the directory of its main script.
	const String root = Mount::pkg_root(dir);
	if (root.is_empty() || (p_path != root && !p_path.begins_with(root + "/"))) {
		return p_path;
	}
	const String below = p_path.substr(root.length()).trim_prefix("/");
	return below.is_empty() ? String(PKG_SCHEME) + dir : String(PKG_SCHEME) + dir + "/" + below;
}

// Convert placed and aliased package paths to their canonical package identity.
String PkgScope::canonical(const String &p_path) {
	if (p_path.begins_with(PKG_COPIES)) {
		return of_copy(p_path);
	}
	String why;
	const String out = expand(p_path, active_path ? *active_path : String(), why);
	if (out.is_empty()) {
		ERR_PRINT(why);
	}
	return out;
}

// Attribute every class to the extension manifest that registered it, and that manifest to its package.
void PkgScope::_scan_classes() {
	classes_scanned = true;
	GDExtensionManager *manager = GDExtensionManager::get_singleton();
	if (!manager) {
		return;
	}
	for (const String &path : manager->get_loaded_extensions()) {
		Ref<GDExtension> ext = manager->get_extension(path);
		if (ext.is_null()) {
			continue;
		}
		const String owner = owner_of(path);
		List<StringName> names;
		ext->get_class_names(&names);
		for (const StringName &name : names) {
			class_owners[name] = owner;
		}
	}
}

// Allow extension classes only when the lexical package declares their owner.
bool PkgScope::can_see(const StringName &p_class, const String &p_from, String &r_why) {
	if (!ClassDB::class_exists(p_class) || ClassDB::get_api_type(p_class) != ClassDB::API_EXTENSION) {
		return true;
	}
	// Scan once and copy the owner under the lock, so a concurrent analysis never reads a map being filled.
	String ext;
	{
		MutexLock lock(class_mutex);
		if (!classes_scanned) {
			_scan_classes();
		}
		const String *found = class_owners.getptr(p_class);
		ext = found ? *found : String();
	}
	const String from = owner_of(p_from);
	if (ext.is_empty()) {
		// Extensions outside the registry belong to the project; packages cannot declare them.
		if (from.is_empty()) {
			return true;
		}
		r_why = vformat("\"%s\" comes from an extension outside the registry, which %s cannot import.", p_class, from);
		return false;
	}
	const HashMap<String, String> *table = tables.getptr(from);
	if (table) {
		for (const KeyValue<String, String> &e : *table) {
			if (e.value == ext) {
				return true;
			}
		}
	}
	r_why = vformat("\"%s\" comes from %s, which %s does not import. Add ext:%s to imports in gd.json.",
			p_class, ext, from.is_empty() ? String("this project") : from, ext.substr(0, ext.rfind("@")));
	return false;
}

// Accept the segments a bare @import may carry; anything else has to be quoted.
static bool _bare_segment(const String &p_segment) {
	if (p_segment.is_empty()) {
		return false;
	}
	for (int i = 0; i < p_segment.length(); i++) {
		const char32_t c = p_segment[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '-') {
			return false;
		}
	}
	return true;
}

// Tell whether a script path exists, without asking file access about places that would be refused loudly.
static bool _exists(const String &p_path) {
	String tail;
	const String scheme = Mount::scheme_of(p_path, tail);
	if (scheme == "pkg") {
		return !Mount::pkg_root(PkgScope::key_of(p_path)).is_empty() && FileAccess::exists(p_path);
	}
	if (!scheme.is_empty() && scheme != "res" && scheme != "user" && !Mount::has_mount(scheme)) {
		return false;
	}
	return FileAccess::exists(p_path);
}

// Try a spec below one directory as a script or as a directory holding mod.gd.
// A directory with its own gd.json is another package: a bare name never descends through it
// and names it only by an alias after gd add, while a quoted path selects it deliberately.
static void _probe(const String &p_dir, const String &p_spec, bool p_explicit, Vector<String> &r_found, Vector<String> &r_tried) {
	const Vector<String> segments = p_spec.split("/");
	String walk = p_dir;
	for (int i = 0; !p_explicit && i + 1 < segments.size(); i++) {
		walk = walk.path_join(segments[i]);
		if (_exists(walk.path_join("gd.json"))) {
			return;
		}
	}
	const String script = p_dir.path_join(p_spec + ".gd").simplify_path();
	const String module = p_dir.path_join(p_spec).path_join("mod.gd").simplify_path();
	const bool foreign = !p_explicit && _exists(p_dir.path_join(p_spec).path_join("gd.json"));
	for (const String &candidate : { script, module }) {
		if ((candidate == module && foreign) || r_tried.has(candidate)) {
			continue;
		}
		r_tried.push_back(candidate);
		if (_exists(candidate)) {
			r_found.push_back(candidate);
		}
	}
}

// Resolve declared aliases only; quoted relative paths explicitly select local scripts.
bool PkgScope::locate(const String &p_spec, bool p_quoted, const String &p_as, const String &p_from, String &r_path, String &r_name, String &r_why) {
	const String from = _resource(p_from);
	const String dir = from.get_base_dir();
	const String spec = p_spec.strip_edges();
	if (spec.is_empty()) {
		r_why = "@import needs a package, a script, or a path.";
		return false;
	}
	Vector<String> found;
	Vector<String> tried;
	const String extension = spec.get_extension();
	if (!extension.is_empty() && extension != "gd" && spec.get_file().contains(".")) {
		r_why = vformat("@import %s: only .gd scripts can be imported. Write const with preload for other files.", spec);
		return false;
	}
	const bool explicit_path = spec.begins_with("./") || spec.begins_with("../") || spec.contains("://") || spec.is_absolute_path();
	if (explicit_path) {
		if (!p_quoted) {
			r_why = vformat("@import: quote paths that start with . or a scheme, as in @import \"%s\".", spec);
			return false;
		}
		const String raw = (spec.contains("://") || spec.is_absolute_path() ? spec : dir.path_join(spec)).simplify_path();
		// A relative path may not climb out of the package; a scheme path is judged by the declared imports below.
		const String home = from.begins_with(PKG_SCHEME) && !spec.contains("://") ? String(PKG_SCHEME) + key_of(from) + "/" : String();
		if (!home.is_empty() && !raw.begins_with(home)) {
			r_why = vformat("@import %s leaves %s. A package imports only its own scripts and its declared imports.", spec, home);
			return false;
		}
		String why;
		const String base = of_copy(expand(raw, from, why));
		if (base.is_empty()) {
			r_why = why;
			return false;
		}
		String scheme_rest;
		const bool package_root = base.begins_with(PKG_SCHEME) && !key_of(base, &scheme_rest).is_empty() && scheme_rest.is_empty();
		if (spec.ends_with(".gd")) {
			tried.push_back(base);
			if (_exists(base)) {
				found.push_back(base);
			}
		} else if (package_root) {
			// A package named by its scheme alone has exactly one entry script.
			const String entry = base.path_join("mod.gd");
			tried.push_back(entry);
			if (_exists(entry)) {
				found.push_back(entry);
			}
		} else {
			_probe(base.get_base_dir(), base.get_file(), true, found, tried);
		}
	} else {
		const Vector<String> segments = spec.trim_suffix(".gd").split("/");
		for (const String &segment : segments) {
			if (!_bare_segment(segment)) {
				r_why = p_quoted ? vformat("@import \"%s\": write a relative path from ./ or ../, a scheme, or a package name.", spec) : vformat("@import: \"%s\" is not a package or script name. Quote a path with . in it.", spec);
				return false;
			}
		}
		const String owner = owner_of(from);
		const String rest = spec.trim_suffix(".gd");
		// Resolve through the defining package rather than probing unrelated project files.
		const String id = resolve(owner, segments[0]);
		if (!id.is_empty() || (owner.is_empty() && !Mount::pkg_root(segments[0]).is_empty())) {
			const String root = String(PKG_SCHEME) + (id.is_empty() ? segments[0] : id);
			if (segments.size() == 1) {
				const String entry = root.path_join("mod.gd");
				tried.push_back(entry);
				if (_exists(entry)) {
					found.push_back(entry);
				}
			} else if (spec.ends_with(".gd")) {
				// A spelled-out extension names one script, never the module of a directory beside it.
				const String exact = root.path_join(rest.substr(segments[0].length() + 1) + ".gd");
				tried.push_back(exact);
				if (_exists(exact)) {
					found.push_back(exact);
				}
			} else {
				_probe(root, rest.substr(segments[0].length() + 1), false, found, tried);
			}
		}

	}
	if (found.size() > 1) {
		String list;
		for (const String &hit : found) {
			list += (list.is_empty() ? String() : String(", ")) + hit;
		}
		r_why = vformat("@import %s matches more than one script: %s. Write const with preload to pick one.", spec, list);
		return false;
	}
	if (found.is_empty()) {
		String list;
		for (const String &miss : tried) {
			list += (list.is_empty() ? String() : String(", ")) + miss;
		}
		// A bare name that reached no candidate path is an alias missing from the package imports, or a run outside the project.
		if (!list.is_empty()) {
			r_why = vformat("@import %s matches nothing. Looked for %s.", spec, list);
		} else if (!_exists("res://gd.json")) {
			r_why = vformat("@import %s matches nothing. There is no gd.json in the working directory; run gd from the project root, or quote a relative path.", spec);
		} else {
			r_why = vformat("@import %s matches nothing. The gd.json imports declare no such package; add it with gd add, or quote a relative path.", spec);
		}
		return false;
	}
	r_path = found[0];
	String name = p_as;
	if (name.is_empty()) {
		// Bind the last segment as written, without the extension; a mod.gd module binds its directory.
		name = spec.trim_suffix("/").get_file().trim_suffix(".gd");
		if (name == "mod" && spec.contains("/")) {
			name = spec.trim_suffix("/mod.gd").trim_suffix("/mod").get_file();
		}
		if (name == "." || name == "..") {
			// A module reached only through dots takes the name of the directory it resolved to.
			name = r_path.get_base_dir().get_file();
		}
		if (!name.is_valid_unicode_identifier()) {
			r_why = vformat("@import %s: \"%s\" cannot be an identifier. Add as <name>.", spec, name);
			return false;
		}
	}
	r_name = name;
	return true;
}

// Verify loaded extensions register only the class names reserved by their manifests.
bool PkgScope::verify_declared(String &r_why) {
	GDExtensionManager *manager = GDExtensionManager::get_singleton();
	if (!manager) {
		return true;
	}
	for (const String &path : manager->get_loaded_extensions()) {
		if (owner_of(path).is_empty()) {
			continue; // The project's own extensions answer to nobody's reservation.
		}
		Ref<GDExtension> ext = manager->get_extension(path);
		if (ext.is_null()) {
			continue;
		}
		Ref<ConfigFile> manifest;
		manifest.instantiate();
		if (manifest->load(path) != OK) {
			r_why = vformat("cannot read the manifest of %s", path);
			return false;
		}
		const Vector<String> declared = manifest->has_section("classes") ? manifest->get_section_keys("classes") : Vector<String>();
		List<StringName> names;
		ext->get_class_names(&names);
		for (const StringName &name : names) {
			if (!declared.has(String(name))) {
				r_why = vformat("%s registered class \"%s\", which its [classes] does not declare. The registry reserves only declared names.", owner_of(path), name);
				return false;
			}
		}
	}
	return true;
}
