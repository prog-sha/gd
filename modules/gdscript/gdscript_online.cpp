/**************************************************************************/
/*  gdscript_online.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

// Implements the @online analysis tables and the Online-side diff write boundary.

#include "gdscript_online.h"

#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/io/resource_uid.h"
#include "core/object/script_language.h"
#include "scene/main/node.h"

HashMap<String, bool> GDScriptOnline::world_bases;

HashMap<String, Dictionary> GDScriptOnline::fields;
Mutex GDScriptOnline::tables;
HashSet<ObjectID> GDScriptOnline::dirty;

// Flag an object whose marked variable was written; the collector reads only flagged objects, not every variable each tick.
// Built-in position and rotation change via physics or interpolation without passing here, so those are read every tick.
void GDScriptOnline::touched(Object *p_owner) {
	MutexLock lock(tables);
	dirty.insert(p_owner->get_instance_id());
}

void GDScriptOnline::take_dirty(HashSet<ObjectID> &r_out) {
	MutexLock lock(tables);
	r_out.clear();
	SWAP(r_out, dirty);
}
HashMap<String, Dictionary> GDScriptOnline::bindings;
thread_local int GDScriptOnline::trusted_writes = 0;
bool (*GDScriptOnline::owner_check)(Object *) = nullptr;
thread_local int GDScriptOnline::field_inits = 0;

// Extract only the script's base from its text; empty if not found.
// Trailing comments are stripped first; a leading BOM is accepted.
String GDScriptOnline::extends_of(const String &p_source) {
	for (const String &raw : p_source.split("\n")) {
		const String line = raw.trim_prefix(String::utf8("\xef\xbb\xbf")).get_slicec('#', 0).strip_edges();
		if (line.is_empty() || line.begins_with("@")) {
			continue;
		}
		if (line.begins_with("class_name")) {
			// class_name and extends on one line; otherwise check the next line.
			const int at = line.find(" extends ");
			if (at < 0) {
				continue;
			}
			return line.substr(at + 9).strip_edges();
		}
		// Require a delimiter after the keyword so names like extendsX aren't mistaken for inheritance.
		if (line.begins_with("extends") && line.length() > 7 && !is_ascii_identifier_char(line[7])) {
			return line.substr(7).strip_edges();
		}
		return String();
	}
	return String();
}

// Decide from source text whether the base is Secret.
// A miss would leak the source into the build, so skipped lines and extends forms are accepted broadly.
bool GDScriptOnline::secret_source(const String &p_source, const String &p_path) {
	String source = p_source;
	String current = p_path.simplify_path();
	HashSet<String> seen;
	seen.insert(current);
	while (true) {
		const String base = extends_of(source);
		if (base.is_empty()) {
			return false;
		}
		if (base == "Secret") {
			return true;
		}
		String path;
		if (base.begins_with("\"") || base.begins_with("'")) {
			path = base.substr(1, base.length() - 2); // Read the quoted base path.
		} else if (base.find_char('.') < 0 && base.find_char('(') < 0 && ScriptServer::is_global_class(base)) {
			path = ScriptServer::get_global_class_path(base); // Read the registered base path.
		}
		if (path.begins_with("uid://")) {
			path = ResourceUID::ensure_path(path);
		} else if (!path.is_empty() && !path.begins_with("res://") && !path.begins_with("user://") && !path.is_absolute_path()) {
			path = current.get_base_dir().path_join(path); // Resolve a relative base beside its script.
		}
		if (!path.is_empty()) {
			path = path.simplify_path();
		}
		if (path.is_empty() || path.get_extension().to_lower() != "gd") {
			return false;
		}
		if (seen.has(path)) {
			return true; // A cycle cannot prove that the source is public.
		}
		seen.insert(path);
		Error err = OK;
		source = FileAccess::get_file_as_string(path, &err);
		if (err != OK) {
			return true; // An unreadable base cannot prove that the source is public.
		}
		current = path;
	}
}

// Smoothing names, in order from S_SNAP. Auto has no name:
// omitting it means auto, so a spelling for auto would duplicate that.
static const char *SMOOTH_NAMES[] = { "snap", "ease", "predict", "arc" };

// Smoothing name to id; -1 if unknown.
int GDScriptOnline::smooth_id(const String &p_name) {
	for (int i = 0; i < (int)(sizeof(SMOOTH_NAMES) / sizeof(SMOOTH_NAMES[0])); i++) {
		if (p_name == SMOOTH_NAMES[i]) {
			return i + GDScriptOnline::S_SNAP;
		}
	}
	return -1;
}

// List of smoothing names shown to the author.
String GDScriptOnline::smooth_names() {
	String out;
	for (int i = 0; i < (int)(sizeof(SMOOTH_NAMES) / sizeof(SMOOTH_NAMES[0])); i++) {
		if (i > 0) {
			out += ", ";
		}
		out += "\"" + String(SMOOTH_NAMES[i]) + "\"";
	}
	return out;
}

void GDScriptOnline::initialize() {
	shutdown();
}

// Discard the analysis tables.
void GDScriptOnline::shutdown() {
	MutexLock lock(tables);
	fields.clear();
	bindings.clear();
	trusted_writes = 0;
	field_inits = 0;
}

// Whether a regular script (before automatic conversion) is an online author script.
bool GDScriptOnline::author(const String &p_path) {
	// Without the online runtime no script is world code, even one naming its class Online.
	if (p_path.is_empty() || !ClassDB::class_exists(SNAME("Online"))) {
		return false;
	}
	MutexLock lock(tables);
	if (fields.has(p_path)) {
		return true;
	}
	// A script extending Online or Secret is world code even without marks; it may use spawn and my.
	const HashMap<String, bool>::ConstIterator known = world_bases.find(p_path);
	if (known) {
		return known->value;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	const String base = file.is_valid() ? extends_of(file->get_as_text()) : String();
	const bool world = base == "Online" || base == "Secret";
	world_bases.insert(p_path, world);
	return world;
}

// Whether the name is added to author scripts by the automatic base.
bool GDScriptOnline::author_name(const StringName &p_name) {
	static const HashSet<StringName> names = {
		SNAME("spawn"), SNAME("disconnected"), SNAME("online_changed"),
		// `my` written by the author is renamed to this during analysis.
		// The base adds it, so it looks undefined while only the author script is visible.
		SNAME("__my__"), SNAME("__world__")
	};
	return names.has(p_name);
}

// Open the boundary inside which only Online-side diffs may write Client copies.
void GDScriptOnline::begin_trusted_write() {
	trusted_writes++;
}

// Close one level of the Online-side diff write boundary.
void GDScriptOnline::end_trusted_write() {
	if (trusted_writes > 0) {
		trusted_writes--;
	}
}

// Decide whether a write to a Client copy comes from an Online-side diff.
bool GDScriptOnline::is_trusted_write() {
	return trusted_writes > 0;
}

// Open the boundary for initializers like `@online var angle := $body.rotation`.
void GDScriptOnline::begin_field_init() {
	field_inits++;
}

// Close one level of the declaration initializer boundary.
void GDScriptOnline::end_field_init() {
	if (field_inits > 0) {
		field_inits--;
	}
}

// Decide whether the current write is a declaration's own initializer.
bool GDScriptOnline::is_field_init() {
	return field_inits > 0;
}

// Record that the script has @online; functions kept out of the table (engine callbacks, _my) still make it world code.
void GDScriptOnline::touch(const String &p_path) {
	MutexLock lock(tables);
	fields[p_path.simplify_path()]; // The analyzer looks up simplified paths; this also rewrites the // in online-authority://.
}

// Whether the script has any @online; only its Node2D/Node3D sync position and rotation without marks.
bool GDScriptOnline::marked(const String &p_path) {
	MutexLock lock(tables);
	return fields.has(p_path.simplify_path());
}

// Merge sync marks stacked on the same name.
void GDScriptOnline::mark(const String &p_path, const StringName &p_name, int p_flags) {
	MutexLock lock(tables);
	Dictionary &values = fields[p_path];
	const String key = p_name;
	values[key] = int(values.get(key, 0)) | p_flags;
}

// Link an Online-side variable to its Client display target.
void GDScriptOnline::bind_field(const String &p_path, const StringName &p_name, const StringName &p_target) {
	MutexLock lock(tables);
	bindings[p_path][String(p_name)] = String(p_target);
}

// Return the script's sync marks.
Dictionary GDScriptOnline::fields_of(const String &p_path) {
	MutexLock lock(tables);
	const HashMap<String, Dictionary>::ConstIterator found = fields.find(p_path);
	// Return a copy; sharing would let the receiver's writes alter the tables and other Nodes.
	return found ? found->value.duplicate(true) : Dictionary();
}

// Return the script's Client display targets.
Dictionary GDScriptOnline::bindings_of(const String &p_path) {
	MutexLock lock(tables);
	const HashMap<String, Dictionary>::ConstIterator found = bindings.find(p_path);
	return found ? found->value.duplicate(true) : Dictionary();
}

// Callbacks the engine calls while walking the tree, with their arguments; marks decide which side runs them.
static const char *CALLBACKS[][2] = {
	{ "_enter_tree", "" }, { "_ready", "" }, { "_exit_tree", "" }, { "_draw", "" },
	{ "_notification", "what" }, { "_process", "delta" }, { "_physics_process", "delta" },
	{ "_input", "event" }, { "_shortcut_input", "event" }, { "_unhandled_input", "event" },
	{ "_unhandled_key_input", "event" }, { nullptr, nullptr }
};

Vector<StringName> GDScriptOnline::callback_names() {
	Vector<StringName> out;
	for (int i = 0; CALLBACKS[i][0] != nullptr; i++) {
		out.push_back(StringName(CALLBACKS[i][0]));
	}
	return out;
}

String GDScriptOnline::callback_args(const StringName &p_name) {
	for (int i = 0; CALLBACKS[i][0] != nullptr; i++) {
		if (p_name == StringName(CALLBACKS[i][0])) {
			return CALLBACKS[i][1];
		}
	}
	return String();
}

bool GDScriptOnline::is_callback(const StringName &p_name) {
	for (int i = 0; CALLBACKS[i][0] != nullptr; i++) {
		if (p_name == StringName(CALLBACKS[i][0])) {
			return true;
		}
	}
	return false;
}


// Run _ready for a script attached later; the Node is already in the tree, so the engine won't call it again.
void GDScriptOnline::ready(Node *p_node) {
	ScriptInstance *instance = p_node->get_script_instance();
	if (instance == nullptr) {
		return;
	}
	Callable::CallError error;
	instance->callp(SNAME("_ready"), nullptr, 0, error);
}
