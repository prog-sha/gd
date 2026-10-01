/**************************************************************************/
/*  mv_classes.cpp                                                        */
/**************************************************************************/

// Manages in C++ the scene, sync marks and size of each auto-detected Node type.

#include "mv_classes.h"

#include "modules/gdscript/gdscript_online.h"

#include "scene/main/node.h"

#include "core/variant/dictionary.h"

HashMap<String, String> MVClasses::scenes;
HashMap<String, Vector2i> MVClasses::shapes;
HashSet<String> MVClasses::bases;
HashMap<String, String> MVClasses::kinds;

// Replace the scene path table. Takes the GDScript-side list as-is
void MVClasses::set_scenes(const Dictionary &p_scenes) {
	scenes.clear();
	for (const Variant &k : p_scenes.get_key_list()) {
		const String name = k;
		if (!bases.has(name)) {
			scenes.insert(name, p_scenes[k]);
		}
	}
}

// Remember per type the Node count and depth checked before instancing
void MVClasses::set_shapes(const Dictionary &p_shapes) {
	shapes.clear();
	for (const Variant &k : p_shapes.get_key_list()) {
		const Variant value = p_shapes[k];
		if (value.get_type() == Variant::VECTOR2I) {
			shapes.insert(k, value);
		}
	}
}

// Replace the fixed base types that are never spawned
void MVClasses::set_bases(const PackedStringArray &p_bases) {
	bases.clear();
	for (const String &base : p_bases) {
		bases.insert(base);
	}
}

// Build the table mapping scenes the author preloads to types
void MVClasses::set_kinds(const Dictionary &p_scenes) {
	kinds.clear();
	for (const Variant &k : p_scenes.get_key_list()) {
		kinds.insert(p_scenes[k], k);
	}
}

// Add a scene passed to spawn as a type on the spot
void MVClasses::add_scene(const String &p_kind, const String &p_scene, const Vector2i &p_shape, const String &p_author) {
	if (bases.has(p_kind)) {
		return;
	}
	scenes.insert(p_kind, p_scene);
	shapes.insert(p_kind, p_shape);
	kinds.insert(p_author, p_kind);
}

const char *MVClasses::OFF = "auto_off";

// Node2D and Node3D in a script with at least one @online share position and rotation without writing anything.
// Authors do not choose which to share. For CharacterBody, floor and velocity go along with position.
// Only things not to share get @online("auto_off"). Scripts without @online (plain engine Nodes) are left alone
Dictionary MVClasses::with_auto(Node *p_node, const Dictionary &p_marks, bool p_marked) {
	Dictionary out = p_marks.duplicate();
	const bool off = out.has(OFF);
	out.erase(OFF);
	const bool placed = p_node->is_class("Node2D") || p_node->is_class("Node3D");
	if (off && !placed) {
		ERR_PRINT(vformat(U"Online: @online(\"auto_off\") does nothing on %s. Only Node2D and Node3D share their position without a mark", p_node->get_name()));
	}
	if (off || !placed || !p_marked) {
		return out;
	}
	// Position and rotation. Unchanging things stop after the first send, so worlds with many static things do not grow
	for (const String &name : { String("position"), String("rotation") }) {
		if (!out.has(name)) {
			out[name] = GDScriptOnline::F_ONLINE | GDScriptOnline::F_AUTO;
		}
	}
	return out;
}

// Type of a scene passed by the author. Empty if it cannot be placed in the world
String MVClasses::kind_of(const String &p_scene) {
	const HashMap<String, String>::ConstIterator e = kinds.find(p_scene);
	return e ? e->value : String();
}

String MVClasses::scene_of(const String &p_name) {
	const HashMap<String, String>::ConstIterator e = scenes.find(p_name);
	return e ? e->value : String();
}

// Return the Node count and max depth with external scenes expanded
Vector2i MVClasses::shape_of(const String &p_name) {
	const HashMap<String, Vector2i>::ConstIterator e = shapes.find(p_name);
	return e ? e->value : Vector2i();
}

// Used only from C++. Nothing is exposed to GDScript
void MVClasses::_bind_methods() {
}
