/**************************************************************************/
/*  mv_client_script.cpp                                                  */
/**************************************************************************/

// Builds, in C++, only the scene display and @online send stubs the Client needs.

#include "mv_client_script.h"

#include "mv_check.h"
#include "mv_classes.h"
#include "mv_frame.h"
#include "mv_gate.h"
#include "mv_online.h"
#include "mv_secret.h"
#include "mv_stage.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_online.h"
#include "modules/regex/regex.h"

HashMap<String, String> MVClientScript::sources;
HashMap<String, String> MVClientScript::natives;
HashMap<String, String> MVClientScript::views;
HashMap<String, String> MVClientScript::authority_scenes;
HashMap<String, Dictionary> MVClientScript::scene_values;
HashMap<String, ObjectID> MVClientScript::scripts;
HashMap<String, Ref<PackedScene>> MVClientScript::quiet;
HashMap<String, bool> MVClientScript::secret_roots;

// Signals and helpers every online Node has
static const char *SUPPORT = R"ONLINE(
signal disconnected
signal online_changed(field, value, previous)
func __my(): return __Online.__my(self)
)ONLINE";

// World-touching helpers only the author's Nodes have
static const char *NODE_SUPPORT = R"ONLINE(
func spawn(scene, a = null, b = null): return __Online.__spawn(self, scene, a, b)
)ONLINE";

// The world advances at 20Hz. The Online side calls this itself at that step, so the engine must not.
// Which side runs is decided by marks; this is about the world's clock
const char *MVClientScript::STEP = "__online_world_step";

// Builds "a0, a1" from the argument count
static String arg_list(const Dictionary &p_spec) {
	PackedStringArray args;
	for (int i = 0; i < CLAMP(int(p_spec.get("count", 0)), 0, int(GDScriptOnline::CALL_ARGS_MAX)); i++) {
		args.push_back("a" + itos(i));
	}
	return String(", ").join(args);
}

// Stub asking the Online side from the local side. Only the player's own Nodes may ask; the Online side decides the rest.
// Returns the answer signal as is. The author's await waits on it; a call without await only sends.
// If the stub itself were a coroutine, Online-side bodies that directly use another @online func's answer
// would stop in analysis on the local copy with "await required"
static String ask_stub(const String &p_name, const String &p_list) {
	return "\nfunc " + p_name + "(" + p_list + "):\n\tif not __my(): return null\n\treturn __Online.__ask(self, \"" + p_name + "\", [" + p_list + "])\n";
}

// Builds the head pattern of a func declaration. One pattern covers leading annotations or static, and annotations on the same line.
// Missing one here would silently skip loading the whole body, so accepted forms are kept broad
static String func_head(const String &p_name) {
	return "(?m)^([ \\t]*(?:@[A-Za-z_][A-Za-z_0-9]*(?:\\([^\\n)]*\\))?[ \\t]+)*(?:static[ \\t]+)?)func[ \\t]+" +
			p_name + "[ \\t]*\\(";
}

// Whether an owner-only copy (__online_my<name>) exists
static bool has_my_callback(const String &p_source, const String &p_name) {
	Ref<RegEx> regex;
	regex.instantiate();
	return regex->compile(func_head("__online_my" + p_name)) == OK && regex->search(p_source).is_valid();
}

// @online_my func bodies run only on the owner's device. Removes the mark and puts "return unless owner" at the top of the body.
// Both sides use the same form. On the Online side it runs only when the host device owns the Thing;
// a dedicated server has no owner, so it does nothing. After the mark is removed it is treated like an unmarked body
static String local_my_funcs(const String &p_source) {
	PackedStringArray lines = p_source.split("\n");
	Ref<RegEx> regex;
	regex.instantiate();
	if (regex->compile("^([ \\t]*)@online_my[ \\t]*(?:\\(\\))?[ \\t]*$|^([ \\t]*)@online_my[ \\t]*(?:\\(\\))?[ \\t]+(func[ \\t].*)$") != OK) {
		return String();
	}
	for (int i = 0; i < lines.size(); i++) {
		const Ref<RegExMatch> hit = regex->search(lines[i]);
		if (hit.is_null()) {
			continue;
		}
		int at = i;
		if (hit->get_string(3).is_empty()) {
			// A mark-only line. If the next declaration is not a func, it marks a variable or signal, so leave it
			int next = i + 1;
			while (next < lines.size() && lines[next].strip_edges().is_empty()) {
				next++;
			}
			if (next >= lines.size() || !lines[next].strip_edges().begins_with("func ")) {
				continue;
			}
			lines.set(i, "");
			at = next;
		} else {
			lines.set(i, hit->get_string(2) + hit->get_string(3));
		}
		String head = lines[at];
		if (!head.strip_edges().ends_with(":")) {
			continue; // Multi-line parameter lists are not accepted. It stays as an unmarked body, so the failure shows immediately
		}
		// Engine-called bodies (such as _ready) can sit beside an unmarked one of the same name.
		// The owner's one moves to another name, and an entry under the engine-called name calls both in order
		Ref<RegEx> callback;
		callback.instantiate();
		if (callback->compile("^([ \\t]*func[ \\t]+)(_[A-Za-z_0-9]*)([ \\t]*\\()") == OK) {
			const Ref<RegExMatch> which = callback->search(head);
			if (which.is_valid() && GDScriptOnline::is_callback(which->get_string(2))) {
				head = callback->sub(head, "${1}__online_my${2}${3}");
				lines.set(at, head);
			}
		}
		int body = at + 1;
		while (body < lines.size() && lines[body].strip_edges().is_empty()) {
			body++;
		}
		const String indent = body < lines.size() ? lines[body].substr(0, lines[body].length() - lines[body].strip_edges(true, false).length()) : String("\t");
		const bool typed = head.contains("->") && !head.contains("-> void");
		lines.insert(at + 1, indent + "if not __my(): return" + (typed ? " null" : ""));
		i = at + 1;
	}
	return String("\n").join(lines);
}

// Renames only the found func declarations. Returns empty to tell the caller when none is found
static String rename_func(const String &p_source, const String &p_name, const String &p_to, bool &r_found) {
	Ref<RegEx> regex;
	regex.instantiate();
	if (regex->compile(func_head(p_name)) != OK) {
		r_found = false;
		return String();
	}
	const String out = regex->sub(p_source, "${1}func " + p_to + "(", true);
	r_found = out != p_source;
	return out;
}

// Renames only marked declarations, or only unmarked ones.
// These two opposite renames let one name have separate local and Online-side bodies side by side
static String rename_side(const String &p_source, const String &p_name, const String &p_to, bool p_marked, bool &r_found) {
	PackedStringArray lines = p_source.split("\n");
	Ref<RegEx> regex;
	regex.instantiate();
	// Only declarations at line start. Ignores func inside docs or lambdas
	if (regex->compile(func_head(p_name)) != OK) {
		r_found = false;
		return String();
	}
	r_found = false;
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i];
		if (regex->search(line).is_null()) {
			continue;
		}
		// The mark may be on the same line or on the line just above. Both forms read the same
		bool marked = line.strip_edges().begins_with("@");
		int from = i;
		for (int back = i - 1; !marked && back >= 0; back--) {
			const String above = lines[back].strip_edges();
			if (above.is_empty() || above.begins_with("#")) {
				continue;
			}
			// Checks for a mark-only line. `@online var something` is a declaration, not a mark
			if (!above.begins_with("@") || above.contains(" ") || above.contains("\t")) {
				break;
			}
			marked = above.begins_with("@online");
			from = back;
		}
		if (marked != p_marked) {
			continue;
		}
		// The new name is no longer engine-called. The mark is removed with it.
		// Otherwise the mark alone would remain and take another meaning (a network stub)
		String moved = line;
		if (p_marked) {
			const int head = line.find("func");
			moved = line.substr(0, line.length() - line.strip_edges(true, false).length()) + line.substr(head);
			for (int back = from; back < i; back++) {
				lines.set(back, "");
			}
		}
		lines.set(i, regex->sub(moved, "${1}func " + p_to + "("));
		r_found = true;
	}
	return String("\n").join(lines);
}

// The world is "the scene whose root is Online". Location and name are up to the author; finds exactly one in the project.
// With two it cannot decide, so it uses neither and reports it
static String found_world;
static bool searched_world = false;

static bool online_root_scene(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || file->get_length() > MVCheck::SRC_MAX) {
		return false;
	}
	const String body = file->get_as_text();
	if (!body.begins_with("[gd_scene")) {
		return false; // The world is read as text (exports keep it as text too)
	}
	for (const String &raw : body.split("\n")) {
		const String line = raw.strip_edges();
		if (line.begins_with("[node ")) {
			return MVClientScript::attribute(line, "type") == String(Online::KIND);
		}
	}
	return false;
}

static void find_worlds(const String &p_dir, Vector<String> &r_hits) {
	for (const String &name : ResourceLoader::list_directory(p_dir)) {
		const String path = p_dir.path_join(name.trim_suffix("/"));
		if (name.ends_with("/")) {
			if (!name.begins_with(".") && !FileAccess::exists(path.path_join(".gdignore"))) {
				find_worlds(path, r_hits);
			}
		} else if (name.ends_with(".tscn") && online_root_scene(path)) {
			r_hits.push_back(path);
		}
	}
}

String MVClientScript::world_scene() {
	if (!searched_world) {
		searched_world = true;
		Vector<String> hits;
		find_worlds("res://", hits);
		if (hits.size() > 1) {
			ERR_PRINT(vformat(U"Online: %d scenes have an Online root (%s, %s ...). Keep one", hits.size(), hits[0], hits[1]));
		} else if (hits.size() == 1) {
			found_world = hits[0];
		}
	}
	return found_world;
}

// Script attached to the world root. Read from the world scene's text
String MVClientScript::world_script() {
	const String scene = world_scene();
	Ref<FileAccess> file = scene.is_empty() ? Ref<FileAccess>() : FileAccess::open(scene, FileAccess::READ);
	if (file.is_null()) {
		return String();
	}
	HashMap<String, String> resources;
	bool root = false;
	for (const String &raw : file->get_as_text().split("\n")) {
		const String line = raw.strip_edges();
		if (line.begins_with("[ext_resource ")) {
			resources[attribute(line, "id")] = attribute(line, "path");
		} else if (line.begins_with("[node ")) {
			if (root) {
				break; // End of the root section
			}
			root = true;
		} else if (root && line.begins_with("script = ")) {
			const HashMap<String, String>::ConstIterator found = resources.find(external_id(line, "ExtResource("));
			return found ? found->value : String();
		}
	}
	return String();
}

// Spawnable scenes are registered when passed in. There is no rule on location.
// The kind name is the root name; another scene with the same name is not allowed (saved values and display names depend on it)
static bool authority_side = false;

String MVClientScript::ensure_scene(const String &p_path) {
	if (!p_path.begins_with("res://")) {
		return String();
	}
	const String known = MVClasses::kind_of(p_path);
	if (!known.is_empty()) {
		return known;
	}
	Dictionary scenes;
	Dictionary shapes;
	if (add_scene(p_path, scenes, shapes) != OK || scenes.is_empty()) {
		return String();
	}
	const String kind = scenes.keys()[0];
	const HashMap<String, String>::ConstIterator other = authority_scenes.find(kind);
	if (MVClasses::has(kind) && (!other || other->value != p_path)) {
		ERR_PRINT(vformat(U"Online: %s cannot be used: another scene already has the root name %s", p_path, kind));
		return String();
	}
	views[kind] = scenes[kind];
	MVClasses::add_scene(kind, authority_side ? p_path : String(scenes[kind]), shapes[kind], p_path);
	MVClasses::add_base(natives[kind]); // The script's extends uses this native type
	return kind;
}

// Finds and registers a scene by kind name (root name). Used by processes that are not given scenes (identity-only processes)
// to check saved-value fields. Root names of project scenes are collected only once
static HashMap<String, String> roots_by_name;
static bool searched_roots = false;

static void collect_roots(const String &p_dir) {
	for (const String &name : ResourceLoader::list_directory(p_dir)) {
		const String path = p_dir.path_join(name.trim_suffix("/"));
		if (name.ends_with("/")) {
			if (!name.begins_with(".") && !FileAccess::exists(path.path_join(".gdignore"))) {
				collect_roots(path);
			}
			continue;
		}
		if (name.ends_with(".tscn.remap") || name.ends_with(".scn")) {
			// A scene exported as binary. Opens it to read the root name
			Ref<PackedScene> packed = ResourceLoader::load(path.trim_suffix(".remap"), "PackedScene");
			Ref<SceneState> state = packed.is_valid() ? packed->get_state() : Ref<SceneState>();
			if (state.is_valid() && state->get_node_count() > 0) {
				const String root = state->get_node_name(0);
				if (!roots_by_name.has(root)) {
					roots_by_name[root] = path.trim_suffix(".remap");
				}
			}
			continue;
		}
		if (!name.ends_with(".tscn")) {
			continue;
		}
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
		if (file.is_null() || file->get_length() > MVCheck::SRC_MAX) {
			continue;
		}
		for (const String &raw : file->get_as_text().split("\n")) {
			const String line = raw.strip_edges();
			if (line.begins_with("[node ")) {
				const String root = MVClientScript::attribute(line, "name");
				if (!roots_by_name.has(root)) {
					roots_by_name[root] = path;
				}
				break;
			}
		}
	}
}

String MVClientScript::ensure_kind(const String &p_kind) {
	if (!searched_roots) {
		searched_roots = true;
		collect_roots("res://");
	}
	const HashMap<String, String>::ConstIterator found = roots_by_name.find(p_kind);
	return found ? ensure_scene(found->value) : String();
}

// Location of the Online-side scene. Sent in the spawn notice so the local side builds a display copy from the same scene
String MVClientScript::author_scene_of(const String &p_kind) {
	const HashMap<String, String>::ConstIterator found = authority_scenes.find(p_kind);
	return found ? found->value : String();
}

// Returns only the quoted value from `name="value"`
String MVClientScript::attribute(const String &p_line, const String &p_name) {
	const String head = p_name + "=\"";
	const int start = p_line.find(head);
	if (start < 0) {
		return String();
	}
	const int value_at = start + head.length();
	const int end = p_line.find_char('"', value_at);
	return end >= value_at ? p_line.substr(value_at, end - value_at) : String();
}

// Returns the quoted ID from the given ExtResource syntax
String MVClientScript::external_id(const String &p_line, const String &p_key) {
	const int key_at = p_line.find(p_key);
	if (key_at < 0) {
		return String();
	}
	const int quote = p_line.find_char('"', key_at + p_key.length());
	const int end = quote < 0 ? -1 : p_line.find_char('"', quote + 1);
	return quote >= 0 && end > quote ? p_line.substr(quote + 1, end - quote - 1) : String();
}

// Answers whether the given scene's root is Secret, remembering each opened scene.
// Used so a Secret placed as an instance is recognized the same way from the text side
bool MVClientScript::secret_root_scene(const String &p_path) {
	// ext_resource lists scripts and images too. Does not open non-scenes
	const String kind = p_path.get_extension().to_lower();
	if (p_path.is_empty() || (kind != "tscn" && kind != "scn" && kind != "res")) {
		return false;
	}
	const HashMap<String, bool>::ConstIterator known = secret_roots.find(p_path);
	if (known) {
		return known->value;
	}
	bool found = false;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_valid() && file->get_length() <= MVCheck::SRC_MAX) {
		const String body = file->get_as_text();
		if (body.begins_with("[gd_scene")) {
			for (const String &raw : body.split("\n")) {
				const String line = raw.strip_edges();
				if (line.begins_with("[node ")) {
					found = attribute(line, "type") == String(Secret::KIND);
					break;
				}
			}
		} else {
			// Binary scenes are checked by type. Without text, opening is the only way to check
			Ref<PackedScene> packed = ResourceLoader::load(p_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
			Node *root = packed.is_valid() ? packed->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE) : nullptr;
			if (root != nullptr) {
				found = Object::cast_to<Secret>(root) != nullptr;
				memdelete(root);
			}
		}
	}
	secret_roots[p_path] = found;
	return found;
}

// Strips Secret contents from a text scene. Keeps only the entry Node and removes
// its script, values, child Nodes, connections pointing at them, and ext_resources no longer used.
// Both Client display scenes and exported PCKs pass through here, so the boundary is in one place
String MVClientScript::hollow_text(const String &p_source) {
	PackedStringArray out;
	Vector<String> dropped; // Paths of Secrets whose contents were stripped
	String inside; // Name of the Secret being stripped. Empty when not stripping
	bool at_secret = false; // Whether the current line belongs to the Secret node itself
	// A Secret placed as an instance has no type=. Records IDs of scenes whose root is Secret first,
	// so differences in text form do not let one slip through
	HashSet<String> secret_ids;
	for (const String &raw : p_source.split("\n")) {
		const String line = raw.strip_edges();
		if (line.begins_with("[ext_resource ") && secret_root_scene(attribute(line, "path"))) {
			secret_ids.insert(attribute(line, "id"));
		}
	}
	for (const String &raw : p_source.split("\n")) {
		const String line = raw.strip_edges();
		// Entering a non-Node section such as [connection] clears the stripping state
		if (line.begins_with("[") && !line.begins_with("[node ")) {
			inside = String();
			at_secret = false;
		}
		// Connections pointing at stripped Nodes cannot remain
		if (line.begins_with("[connection ") && !dropped.is_empty()) {
			bool cut = false;
			for (const String &gone : dropped) {
				for (const String &side : { attribute(line, "from"), attribute(line, "to") }) {
					cut = cut || side == gone || side.begins_with(gone + "/");
				}
			}
			if (cut) {
				continue;
			}
		}
		if (line.begins_with("[node ")) {
			const String parent = attribute(line, "parent");
			const String placed = external_id(line, "instance");
			at_secret = attribute(line, "type") == String(Secret::KIND) ||
					(!placed.is_empty() && secret_ids.has(placed));
			if (at_secret) {
				inside = parent == "." ? String(attribute(line, "name")) : parent + "/" + attribute(line, "name");
				dropped.push_back(inside);
				// Checks up to the boundary so siblings sharing a name prefix are not caught
			} else if (!inside.is_empty() && parent != inside && !parent.begins_with(inside + "/")) {
				inside = String();
			}
			// Children of a Secret are dropped whole
			if (!at_secret && !inside.is_empty()) {
				continue;
			}
		} else if (!inside.is_empty() && !at_secret) {
			continue;
		}
		// Only the header of the Secret node is kept. Its script and values are dropped here
		if (at_secret && !line.begins_with("[node ")) {
			continue;
		}
		out.push_back(raw);
	}
	if (dropped.is_empty()) {
		return p_source;
	}
	// Removes ext_resources nothing points to anymore. This removes Secret scripts from the PCK
	PackedStringArray body;
	for (const String &raw : out) {
		if (!raw.strip_edges().begins_with("[ext_resource ")) {
			body.push_back(raw);
		}
	}
	const String rest = String("\n").join(body);
	PackedStringArray kept;
	for (const String &raw : out) {
		const String line = raw.strip_edges();
		const String id = line.begins_with("[ext_resource ") ? attribute(line, "id") : String();
		if (!id.is_empty() && !rest.contains("ExtResource(\"" + id + "\")") &&
				!rest.contains("ExtResource(" + id + ")")) {
			continue;
		}
		kept.push_back(raw);
	}
	return String("\n").join(kept);
}

// Records root values written in the scene, only those the script owns.
// The Client copy drops the script, so inspector values such as `@export var radius = 34`
// would revert to defaults. Remembered so they can be written back after rebuilding.
// Position and rotation owned by the native type are excluded; they would overwrite values arriving at spawn
static Dictionary root_script_values(const String &p_path, const String &p_native) {
	Dictionary out;
	Ref<PackedScene> packed = ResourceLoader::load(p_path, "PackedScene");
	Ref<SceneState> state = packed.is_valid() ? packed->get_state() : Ref<SceneState>();
	if (state.is_null() || state->get_node_count() < 1) {
		return out;
	}
	for (int i = 0; i < state->get_node_property_count(0); i++) {
		const String name = state->get_node_property_name(0, i);
		if (name == "script" || ClassDB::has_property(p_native, name)) {
			continue;
		}
		const Variant value = state->get_node_property_value(0, i);
		if (MVFrame::plain(value)) {
			out[name] = value;
		}
	}
	return out;
}

// Builds a Client copy that removes only the scene root's script and keeps child visuals and normal scripts
Error MVClientScript::add_scene(const String &p_path, Dictionary &r_scenes, Dictionary &r_shapes) {
	// A scene exported as binary has no text (the .scn behind .remap). ResourceLoader follows the remap
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_valid() && file->get_length() > MVCheck::SRC_MAX) {
		return ERR_FILE_CORRUPT;
	}
	const String source = file.is_valid() ? file->get_as_text() : String();
	// Opens an exported binary scene with ResourceLoader and turns it into a display scene without the root Script
	if (!source.begins_with("[gd_scene")) {
		Ref<PackedScene> packed = quiet_scene(p_path);
		Node *root = packed.is_valid() ? packed->instantiate() : nullptr;
		if (root == nullptr) {
			return ERR_FILE_CORRUPT;
		}
		const String kind = root->get_name();
		const String native = root->get_class();
		const Ref<Script> script = root->get_script();
		const int nodes = MVGate::count(root);
		int depth = 1;
		Vector<Pair<Node *, int>> walk;
		walk.push_back(Pair<Node *, int>(root, 1));
		while (!walk.is_empty()) {
			const Pair<Node *, int> one = walk[walk.size() - 1];
			walk.resize(walk.size() - 1);
			depth = MAX(depth, one.second);
			for (int i = 0; i < one.first->get_child_count(); i++) {
				walk.push_back(Pair<Node *, int>(one.first->get_child(i), one.second + 1));
			}
		}
		// Scenes whose root is Secret are not made spawnable kinds. With a uid,
		// variable and function names would leak to the Client as api. Placing it as an instance
		// in Online.tscn still works
		if (native == String(Secret::KIND)) {
			memdelete(root);
			return OK;
		}
		// Secret may be placed anywhere. The Client display scene strips its contents and keeps only the entry
		MVStage::hollow_secrets(root);
		if (kind.is_empty() || !MVGate::safe_name(kind) || !ClassDB::is_parent_class(native, "Node") ||
				nodes > MVGate::NODES_MAX || depth > MVGate::DEPTH_MAX || r_scenes.has(kind)) {
			memdelete(root);
			return ERR_INVALID_DATA;
		}
		root->set_script(Ref<Script>());
		Ref<PackedScene> view;
		view.instantiate();
		const String dir = "user://online_view";
		const String target = dir.path_join(p_path.sha256_text() + ".scn");
		DirAccess::make_dir_recursive_absolute(dir);
		const Error packed_error = view->pack(root);
		memdelete(root);
		if (packed_error != OK || ResourceSaver::save(view, target) != OK) {
			return ERR_CANT_CREATE;
		}
		r_scenes[kind] = target;
		r_shapes[kind] = Vector2i(nodes, depth);
		authority_scenes[kind] = p_path;
		sources[kind] = script.is_valid() ? script->get_path() : String();
		natives[kind] = native;
		scene_values[kind] = root_script_values(p_path, native);
		return OK;
	}
	HashMap<String, String> resources;
	PackedStringArray out;
	String kind;
	String native;
	String script;
	int nodes = 0;
	int depth = 0;
	bool root = false;
	// Secret may be placed anywhere. Stripping is handled in one place
	for (const String &raw : hollow_text(source).split("\n")) {
		const String line = raw.strip_edges();
		if (line.begins_with("[ext_resource ")) {
			resources[attribute(line, "id")] = attribute(line, "path");
		}
		if (line.begins_with("[node ")) {
			nodes++;
			root = nodes == 1;
			if (root) {
				kind = attribute(line, "name");
				native = attribute(line, "type");
			}
			const String parent = attribute(line, "parent");
			depth = MAX(depth, nodes == 1 ? 1 : 2 + (parent == "." ? 0 : parent.get_slice_count("/")));
		}
		if (root && line.begins_with("script = ")) {
			const String id = external_id(line, "ExtResource(");
			const HashMap<String, String>::ConstIterator found = resources.find(id);
			if (found) {
				script = found->value;
			}
			continue;
		}
		out.push_back(raw);
	}
	// Scenes whose root is Secret are not made spawnable kinds (same reason as the binary path)
	if (native == String(Secret::KIND)) {
		return OK;
	}
	if (kind.is_empty() || !MVGate::safe_name(kind) || native.is_empty() ||
			!ClassDB::is_parent_class(native, "Node") || nodes > MVGate::NODES_MAX || depth > MVGate::DEPTH_MAX ||
			r_scenes.has(kind)) {
		return ERR_INVALID_DATA;
	}
	const String dir = "user://online_view";
	DirAccess::make_dir_recursive_absolute(dir);
	const String target = dir.path_join(p_path.sha256_text() + ".tscn");
	Ref<FileAccess> made = FileAccess::open(target, FileAccess::WRITE);
	if (made.is_null()) {
		return ERR_CANT_CREATE;
	}
	made->store_string(String("\n").join(out));
	r_scenes[kind] = target;
	authority_scenes[kind] = p_path;
	r_shapes[kind] = Vector2i(nodes, depth);
	sources[kind] = script;
	natives[kind] = native;
	scene_values[kind] = root_script_values(p_path, native);
	return OK;
}

// Replaces the author script's extends with the auto-generated safe Client base
String MVClientScript::bind_source(const String &p_source, const String &p_native, bool p_server) {
	PackedStringArray lines = p_source.split("\n");
	const String base = base_path(p_native, p_server);
	if (base.is_empty()) {
		return String();
	}
	bool replaced = false;
	for (int i = 0; i < lines.size(); i++) {
		const String text = lines[i].strip_edges();
		if (text.is_empty() || text.begins_with("#") || text.begins_with("@")) {
			continue;
		}
		lines.set(i, "extends \"" + base + "\"");
		replaced = true;
		break;
	}
	// Returning a script whose extends cannot be replaced would leave it without a base and reach the author
	// as an unrelated "cannot assign to native type" error
	if (!replaced) {
		return String();
	}
	return String("\n").join(lines) + "\nvar __ctx = null\nvar __my__:\n\tget:\n\t\treturn __ctx\nvar __world__:\n\tget:\n\t\treturn __Online.__world(self)\n";
}

// Moves @online bodies to internal names and adds same-named ASK entries only the owner can send
String MVClientScript::client_source(const String &p_source, const Dictionary &p_fns) {
	String out = local_my_funcs(p_source);
	Dictionary callbacks;
	// Bodies marked @online belong to the host. Locally they are renamed and silenced.
	// Unmarked bodies keep their names and the engine calls them as usual
	for (const StringName &key : GDScriptOnline::callback_names()) {
		const String name = key;
		bool found = false;
		String renamed = rename_side(out, name, "__online_authority" + name, true, found);
		if (renamed.is_empty()) {
			return String();
		}
		out = renamed;
		const bool mine = has_my_callback(out, name);
		// Only per-frame bodies are wrapped in a section that does not pile up the same command. Bodies with an owner-only copy also get a rebuilt entry
		if (name != "_process" && name != "_physics_process" && !mine) {
			continue;
		}
		const String implementation = "__online_client_local" + name;
		renamed = rename_side(out, name, implementation, false, found);
		if (renamed.is_empty()) {
			return String();
		}
		out = renamed;
		if (name == "_process" || name == "_physics_process") {
			callbacks[name] = found ? implementation : String();
			if (mine) {
				callbacks[name] = String(callbacks[name]) + (found ? "|" : "") + "__online_my" + name;
			}
			continue;
		}
		// Everyone's body first, then the owner's. On a non-owner device the owner's part returns doing nothing
		const String args = GDScriptOnline::callback_args(key);
		out += "\nfunc " + name + "(" + args + ") -> void:\n";
		if (found) {
			out += "\t" + implementation + "(" + args + ")\n";
		}
		out += "\t__online_my" + name + "(" + args + ")\n";
	}
	for (const Variant &key : p_fns.get_key_list()) {
		const String name = key;
		if (!MVGate::safe_name(name)) {
			return String();
		}
		bool found = false;
		const String renamed = rename_func(out, name, "_online_server_implementation_" + name, found);
		if (!found || renamed.is_empty()) {
			ERR_PRINT(vformat(U"Online: cannot find @online func %s() to rewrite it for the client", name));
			return String();
		}
		out = renamed;
	}
	for (const Variant &key : p_fns.get_key_list()) {
		out += ask_stub(key, arg_list(p_fns[key]));
	}
	// However many times a per-frame body calls the same @online func, only the last one is sent
	for (const String &name : { String("_process"), String("_physics_process") }) {
		if (!String(callbacks[name]).is_empty()) {
			out += "\nfunc " + name + "(delta) -> void:\n\t__Online.__begin_latest(self)\n";
			for (const String &one : String(callbacks[name]).split("|", false)) {
				out += "\t" + one + "(delta)\n";
			}
			out += "\t__Online.__end_latest(self)\n";
		}
	}
	return out;
}

// Generates the shared helpers for each native type once in the user area
String MVClientScript::base_path(const String &p_native, bool p_server) {
	if (p_native.is_empty() || !ClassDB::is_parent_class(p_native, "Node")) {
		return String();
	}
	// The world itself has spawn and players natively, so the base does not redeclare them
	const bool world = p_server || p_native == String(MVGate::SERVER);
	const String dir = "user://online_base";
	const String path = dir.path_join(p_native + (world ? "_world.gd" : ".gd"));
	DirAccess::make_dir_recursive_absolute(dir);
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		return String();
	}
	file->store_string("extends " + p_native + "\n" + String::utf8(SUPPORT) + (world ? String() : String::utf8(NODE_SUPPORT)));
	return path;
}

// Prepares the fixed kind table. Spawnable scenes are registered when passed in (ensure_scene)
String MVClientScript::configure() {
	if (natives.has(Online::KIND)) {
		return String();
	}
	clear();
	Dictionary scenes;
	Dictionary shapes;
	// Shares the world itself, Secret, and scene root native types as kinds Online-side scripts can extend.
	// Secret is not shipped to the Client, but scenes cannot load unless the type resolves on both sides
	natives[Online::KIND] = Online::KIND;
	natives[Secret::KIND] = Secret::KIND;
	// Script and scene of the world itself. The local world also loads the author's unmarked bodies (such as _on_logged_in)
	const String world = world_scene();
	if (!world.is_empty()) {
		sources[Online::KIND] = world_script();
		authority_scenes[Online::KIND] = world;
		scene_values[Online::KIND] = root_script_values(world, Online::KIND);
	}
	PackedStringArray bases;
	bases.push_back(Online::KIND);
	bases.push_back(Secret::KIND);
	for (const KeyValue<String, String> &entry : natives) {
		if (!bases.has(entry.value)) {
			bases.push_back(entry.value);
		}
	}
	MVClasses::set_bases(bases);
	MVClasses::set_scenes(scenes);
	MVClasses::set_shapes(shapes);
	// Remembers where the author's scene lives. spawn() receives this scene, so lookups use
	// the original location the author preloads, not the engine-made copy
	Dictionary written;
	for (const KeyValue<String, String> &one : authority_scenes) {
		written[one.key] = one.value;
	}
	MVClasses::set_kinds(written);
	for (const Variant &kind : scenes.get_key_list()) {
		views[String(kind)] = scenes[kind];
	}
	return String();
}

// The Online side places the author's scene itself, not the display copy
String MVClientScript::configure_authority() {
	const String reason = configure(); // configure() clears the flag via clear(), so it is set afterwards
	authority_side = true;
	return reason;
}

// Records a fixed Node under Online as a kind. It has no scene, so
// it is not a spawn() target. Only the script and native type are needed
void MVClientScript::add_fixed(const String &p_kind, const String &p_source, const String &p_native) {
	if (p_kind.is_empty() || p_source.is_empty() || p_native.is_empty()) {
		return;
	}
	sources[p_kind] = p_source;
	natives[p_kind] = p_native;
	MVClasses::add_base(p_native); // The script's extends uses this native type
}

// Builds the script for Secret's entry. Lists only the @online funcs inside Secret,
// and returns a reason on the spot when its inner variables are accessed.
// The contents go over the network, so answers are received with await
String MVClientScript::door_source(const Dictionary &p_doors) {
	String out = "extends ";
	out += Secret::KIND;
	out += String(U"\n\nfunc _get(property):\n\tif __Online.__native_property(self, property): return null\n\tpush_error(\"Online: Secret contents cannot be read from outside. Ask through an @online func inside Secret: \" + str(property))\n\treturn null\n");
	out += String(U"\nfunc _set(property, _value):\n\tpush_error(\"Online: Secret contents cannot be changed from outside. Ask through an @online func inside Secret: \" + str(property))\n\treturn true\n");
	for (const Variant &key : p_doors.get_key_list()) {
		const String name = key;
		if (!MVGate::safe_name(name)) {
			return String();
		}
		const String list = arg_list(p_doors[key]);
		out += "\nfunc " + name + "(" + list + "):\n\treturn await __Online.__secret(self, \"" + name + "\", [" + list + "])\n";
	}
	return out;
}

// Returns the location of the author script for a kind
String MVClientScript::source_of(const String &p_kind) {
	const HashMap<String, String>::ConstIterator found = sources.find(p_kind);
	return found ? found->value : String();
}

// Returns the Node type of the scene root for a kind
String MVClientScript::native_of(const String &p_kind) {
	const HashMap<String, String>::ConstIterator found = natives.find(p_kind);
	return found ? found->value : String();
}

// Only @online-marked bodies run on the Online side. Unmarked bodies belong to the local side,
// so they are renamed and silenced. The @online ones keep their names and the engine calls them.
// Physics bodies are called on the world clock (20Hz), so they move to a name the engine does not call
static String mute_local_callbacks(const String &p_source) {
	String out = p_source;
	for (const StringName &key : GDScriptOnline::callback_names()) {
		const String name = key;
		const String args = GDScriptOnline::callback_args(key);
		bool plain = false;
		bool host = false;
		out = rename_side(out, name, "__online_all" + name, false, plain);
		if (out.is_empty()) {
			return String();
		}
		out = rename_side(out, name, "__online_host" + name, true, host);
		if (out.is_empty()) {
			return String();
		}
		const bool mine = has_my_callback(out, name);
		if (!plain && !host && !mine) {
			continue;
		}
		// Call order: "shared body -> owner-only body -> host body". The host is also a normal player;
		// Online-side values are layered after setup is done.
		// Physics bodies are called on the world clock (20Hz), not the engine frame
		const String entry = name == "_physics_process" ? String(MVClientScript::STEP) : name;
		out += "\nfunc " + entry + "(" + args + ") -> void:\n";
		if (plain) {
			// Sets the mark only during the shared body. @online func calls there affect only the owner's Things, as locally
			out += "\t__Online.__begin_local(self)\n\t__online_all" + name + "(" + args + ")\n\t__Online.__end_local(self)\n";
		}
		if (mine) {
			out += "\t__online_my" + name + "(" + args + ")\n";
		}
		if (host) {
			out += "\t__online_host" + name + "(" + args + ")\n";
		}
	}
	return out;
}

// Puts the same gate as the local side in front of @online funcs. When called from an unmarked body (run on every device),
// it does nothing. The Online-side world has no owner, which matches the local "send only your own" answer.
// Without this, an unmarked _process would run @online funcs of others' or ownerless Things directly every frame
static String guard_online_funcs(const String &p_source, const Dictionary &p_marks) {
	String out = p_source;
	for (const Variant &key : p_marks.get_key_list()) {
		const String name = key;
		if (!(int(p_marks[key]) & GDScriptOnline::F_METHOD) || GDScriptOnline::is_callback(name)) {
			continue;
		}
		Ref<RegEx> regex;
		regex.instantiate();
		// Copies the parameter list and return type. Accepts only one level of parentheses in default values
		if (!MVGate::safe_name(name) || regex->compile(func_head(name) + "((?:[^()]|\\([^()]*\\))*)\\)([^:\\n]*):") != OK) {
			return String();
		}
		const Ref<RegExMatch> head = regex->search(out);
		if (head.is_null()) {
			return String();
		}
		bool found = false;
		out = rename_func(out, name, "_online_server_implementation_" + name, found);
		if (!found) {
			return String();
		}
		PackedStringArray names;
		for (const String &one : head->get_string(2).split(",", false)) {
			names.push_back(one.get_slice(":", 0).get_slice("=", 0).strip_edges());
		}
		// Calls through a Callable. Whether the body awaits or not, the answer returns in the same single form.
		// Calling directly would stop awaiting bodies in analysis with "await required", and await on non-awaiting bodies adds warnings
		out += "\nfunc " + name + "(" + head->get_string(2) + "):\n\tif __Online.__local(self):\n\t\treturn null\n" +
				"\treturn Callable(self, \"_online_server_implementation_" + name + "\").callv([" + String(", ").join(names) + "])\n";
	}
	return out;
}

// Keeps @online function names in Online-side scripts and adds only shared helpers and owner context.
// Server scripts exist only on the Online side, so per-frame processing stays as is
String MVClientScript::authority_source(const String &p_source, const String &p_native, bool p_server, const Dictionary &p_marks) {
	const String body = guard_online_funcs(mute_local_callbacks(local_my_funcs(p_source)), p_marks);
	if (body.is_empty()) {
		return String();
	}
	return bind_source(body, p_native, p_server);
}

// During building the script's signals do not exist yet, so returns a copy without connections
Ref<PackedScene> MVClientScript::quiet_scene(const String &p_path) {
	const HashMap<String, Ref<PackedScene>>::ConstIterator found = quiet.find(p_path);
	if (found) {
		return found->value;
	}
	Ref<PackedScene> loaded = ResourceLoader::load(p_path, "PackedScene");
	Ref<SceneState> state = loaded.is_valid() ? loaded->get_state() : Ref<SceneState>();
	if (state.is_null()) {
		return loaded;
	}
	Dictionary bundle = state->get_bundled_scene();
	bundle["conn_count"] = 0;
	bundle["conns"] = Vector<int>();
	Ref<PackedScene> made;
	made.instantiate();
	made->get_state()->set_bundled_scene(bundle);
	quiet[p_path] = made;
	return made;
}

// Connects scene connections after the script is loaded
void MVClientScript::wire_signals(Node *p_node, const String &p_kind, bool p_authority) {
	const HashMap<String, String> &table = p_authority ? authority_scenes : views;
	const HashMap<String, String>::ConstIterator found = table.find(p_kind);
	// The world itself is not in the table. Connects from the author's scene
	const String scene_path = p_kind == String(Online::KIND) ? world_scene() : (found ? found->value : String());
	Ref<PackedScene> scene;
	if (!scene_path.is_empty()) {
		scene = ResourceLoader::load(scene_path, "PackedScene");
	}
	Ref<SceneState> state = scene.is_valid() ? scene->get_state() : Ref<SceneState>();
	for (int i = 0; state.is_valid() && i < state->get_connection_count(); i++) {
		Node *from = p_node->get_node_or_null(state->get_connection_source(i));
		Node *to = p_node->get_node_or_null(state->get_connection_target(i));
		const StringName signal_name = state->get_connection_signal(i);
		// The engine reports misspellings by name
		if (from == nullptr || to == nullptr) {
			continue;
		}
		Callable target(to, state->get_connection_method(i));
		const Array binds = state->get_connection_binds(i);
		if (!binds.is_empty()) {
			target = target.bindv(binds);
		}
		const int unbinds = state->get_connection_unbinds(i);
		if (unbinds > 0) {
			target = target.unbind(unbinds);
		}
		if (!from->is_connected(signal_name, target)) {
			from->connect(signal_name, target, state->get_connection_flags(i));
		}
	}
	// Signals of this base are received by name, like _ready and _process. Connects _on_<signal> if it exists.
	// If already connected in the inspector, the step above handled it, so it does not fire twice. Engine signals (such as pressed)
	// are connected in the inspector as usual
	for (const StringName &signal_name : { SNAME("player_joining"), SNAME("player_left"), SNAME("logged_in"),
				 SNAME("login_failed"), SNAME("spawned"), SNAME("disconnected"), SNAME("online_changed") }) {
		const StringName method = String("_on_") + String(signal_name);
		if (!p_node->has_signal(signal_name) || !p_node->has_method(method)) {
			continue;
		}
		const Callable target(p_node, method);
		if (!p_node->is_connected(signal_name, target)) {
			p_node->connect(signal_name, target);
		}
	}
}

// Keeps scene connections only on the emitting side. Those connected to @online fire on the Online side, others only locally
void MVClientScript::split_signals(Node *p_node, const Dictionary &p_marks, bool p_authority) {
	List<Object::Connection> list;
	p_node->get_signals_connected_to_this(&list);
	for (const Object::Connection &conn : list) {
		if (conn.callable.get_object() != p_node) {
			continue;
		}
		const bool online = int(p_marks.get(conn.callable.get_method(), 0)) & GDScriptOnline::F_METHOD;
		if (online == p_authority) {
			continue;
		}
		if (Object *from = conn.signal.get_object()) {
			from->disconnect(conn.signal.get_name(), conn.callable);
		}
	}
}

// Builds only verified display scenes, without sharing the Online-side scene table
Node *MVClientScript::make(const String &p_kind, const String &p_name, const String &p_own, int64_t p_uid) {
	const HashMap<String, String>::ConstIterator found = views.find(p_kind);
	if (!found) {
		return nullptr;
	}
	Ref<PackedScene> scene = quiet_scene(found->value);
	Node *node = scene.is_valid() ? scene->instantiate() : nullptr;
	if (node == nullptr) {
		return nullptr;
	}
	node->set_name(p_name);
	node->set_meta(SNAME("kind"), p_kind);
	node->set_meta(SNAME("own"), p_own);
	node->set_meta(SNAME("uid"), p_uid);
	return node;
}

// Turns the API table from Secret into a Client GDScript and loads it onto the target Node once
Object *MVClientScript::dress(Node *p_node, const Dictionary &p_api) {
	if (p_node == nullptr) {
		return nullptr;
	}
	const String kind = p_node->get_meta(SNAME("kind"), String());
	const HashMap<String, String>::ConstIterator native = natives.find(kind);
	if (!native) {
		return nullptr;
	}
	const Dictionary fns = p_api.get("fns", Dictionary());
	const Dictionary marks = p_api.get("marks", Dictionary());
	const Dictionary binds = p_api.get("binds", Dictionary());
	const Dictionary signals = p_api.get("signals", Dictionary());
	const HashMap<String, String>::ConstIterator found_source = sources.find(kind);
	const String source_path = found_source ? found_source->value : String();
	String source;
	if (!source_path.is_empty()) {
		Ref<FileAccess> file = FileAccess::open(source_path, FileAccess::READ);
		if (file.is_null() || file->get_length() > MVCheck::SRC_MAX) {
			return nullptr;
		}
		source = file->get_as_text();
	}
	const String key = kind + ":" + String::num_int64(p_api.hash()) + ":" + source.md5_text();
	if (String(p_node->get_meta(SNAME("online_script"), String())) == key) {
		return p_node->get_script();
	}
	Dictionary keep;
	// Script values written in the scene inspector were dropped when making the copy. Without restoring,
	// values such as `@export var radius = 34` silently become 0 and quietly break the author's math
	const HashMap<String, Dictionary>::ConstIterator wrote = scene_values.find(kind);
	if (wrote) {
		for (const Variant &name : wrote->value.get_key_list()) {
			keep[name] = wrote->value[name];
		}
	}
	// Nodes whose script was removed before entering the tree receive the values recorded then
	MVStage::take_held(p_node, keep);
	Dictionary preserve = p_node->get_meta(SNAME("mark"), Dictionary()).duplicate();
	preserve.merge(marks, true);
	Dictionary preserve_binds = p_node->get_meta(SNAME("bind"), Dictionary()).duplicate();
	preserve_binds.merge(binds, true);
	for (const Variant &entry : preserve.get_key_list()) {
		if (!(int(preserve[entry]) & GDScriptOnline::F_SIGNAL)) {
			bool valid = false;
			const Variant value = p_node->get(entry, &valid);
			if (valid) {
				keep[entry] = value;
			}
		}
	}
	Ref<GDScript> script;
	const HashMap<String, ObjectID>::ConstIterator cached = scripts.find(key);
	if (cached) {
		GDScript *alive = Object::cast_to<GDScript>(ObjectDB::get_instance(cached->value));
		if (alive != nullptr) {
			script = Ref<GDScript>(alive);
		}
	}
	if (script.is_null()) {
		if (source.is_empty()) {
			source = "extends " + native->value + "\n" + String::utf8(SUPPORT) + (kind == Online::KIND ? String() : String::utf8(NODE_SUPPORT));
			// Even for a Server that ships no script, creates receivers for public values and public signals only
			for (const Variant &entry : marks.get_key_list()) {
				const int flags = marks[entry];
				// Does not declare display-only marks or engine-added position and rotation (the Node already has them)
				if (flags & (GDScriptOnline::F_BIND | GDScriptOnline::F_AUTO)) {
					continue;
				}
				const String name = entry;
				// Received names become GDScript source as is. Only well-formed spellings pass
				if (!MVGate::safe_name(name)) {
					return nullptr;
				}
				if (flags & GDScriptOnline::F_SIGNAL) {
					const int count = int(signals.get(entry, 0));
					// Silently truncating the count would mismatch the author's declaration and the stub's shape.
					// Only this signal is dropped; the Node's other stubs are not affected
					if (count < 0 || count > ARGS_SANE) {
						ERR_PRINT(vformat(U"Online: signal %s has %d arguments; at most %d are allowed. This signal is not connected", name, count, (int)ARGS_SANE));
						continue;
					}
					PackedStringArray args;
					for (int i = 0; i < count; i++) {
						args.push_back("a" + itos(i));
					}
					source += "\nsignal " + name + "(" + String(", ").join(args) + ")\n";
				} else {
					source += "\nvar " + name + " = null\n";
				}
			}
			for (const Variant &entry : fns.get_key_list()) {
				const String name = entry;
				if (!MVGate::safe_name(name)) {
					return nullptr;
				}
				source += ask_stub(name, arg_list(fns[entry]));
			}
		} else {
			source = bind_source(client_source(source, fns), native->value);
		}
		if (source.is_empty()) {
			ERR_PRINT(vformat(U"Online: cannot rewrite %s for the client", source_path));
			return nullptr;
		}
		script.instantiate();
		script->set_path("online-client://" + kind + "-" + key.md5_text() + ".gd");
		if (GDScriptOnline::marked(source_path)) {
			GDScriptOnline::touch(script->get_path()); // If the original script is the world's, my and world are replaced in the local copy too
		}
		script->set_source_code(source);
		if (script->reload() != OK) {
			return nullptr;
		}
		scripts[key] = script->get_instance_id();
	}
	p_node->set_script(script);
	wire_signals(p_node, kind, false);
	split_signals(p_node, preserve, false);
	// The Online side adds position and rotation to what it sends. The local side keeps the received table as is
	p_node->set_meta(SNAME("mark"), preserve);
	p_node->set_meta(SNAME("bind"), preserve_binds);
	p_node->set_meta(SNAME("online_script"), key);
	// Author scripts attached later also receive Node callbacks automatically like normal scenes
	p_node->set_process(p_node->has_method(SNAME("_process")));
	// No Client physics body is generated, so only interpolation of Online-side state moves the display.
	p_node->set_physics_process(p_node->has_method(SNAME("_physics_process")));
	p_node->set_process_input(p_node->has_method(SNAME("_input")));
	p_node->set_process_shortcut_input(p_node->has_method(SNAME("_shortcut_input")));
	p_node->set_process_unhandled_input(p_node->has_method(SNAME("_unhandled_input")));
	p_node->set_process_unhandled_key_input(p_node->has_method(SNAME("_unhandled_key_input")));
	// Values carried over on reload are blocked by the read-only guard for @online values.
	// This is where values from the Online side are written back, so it passes as a trusted write
	GDScriptOnline::begin_trusted_write();
	for (const Variant &entry : keep.get_key_list()) {
		p_node->set(entry, keep[entry]);
	}
	GDScriptOnline::end_trusted_write();
	return script.ptr();
}

// Empties generated scripts and the kind table before the GDScript runtime shuts down
void MVClientScript::clear() {
	for (const KeyValue<String, ObjectID> &entry : scripts) {
		GDScript *script = Object::cast_to<GDScript>(ObjectDB::get_instance(entry.value));
		if (script != nullptr) {
			// Removes them from ResourceCache while still valid, regardless of shutdown order
			script->set_path(String());
		}
	}
	scripts.clear();
	quiet.clear();
	sources.clear();
	natives.clear();
	views.clear();
	authority_scenes.clear();
	scene_values.clear();
	authority_side = false;
	secret_roots.clear();
}
