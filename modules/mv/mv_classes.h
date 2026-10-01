/**************************************************************************/
/*  mv_classes.h                                                          */
/**************************************************************************/

// Holds in one place the types allowed in the world.
// A type is the scene (tscn) itself. The scene holds looks and initial values; this only remembers its path and marks.

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

class Node;

class MVClasses : public RefCounted {
	GDCLASS(MVClasses, RefCounted);

	static HashMap<String, String> scenes; // Type -> scene path
	static HashMap<String, Vector2i> shapes; // Type -> Node count and depth including external scenes
	static HashSet<String> bases; // Fixed types that are never spawned, only inherited by delivered scripts
	static HashMap<String, String> kinds; // Author-written scene path -> type

protected:
	static void _bind_methods();

public:
	static void set_scenes(const Dictionary &p_scenes); // Replace the scene path table
	static void set_shapes(const Dictionary &p_shapes); // Replace the scene size table
	static void set_bases(const PackedStringArray &p_bases); // Replace the fixed base types
	static void set_kinds(const Dictionary &p_scenes); // Build the table that maps author scenes to types
	static void add_scene(const String &p_kind, const String &p_scene, const Vector2i &p_shape, const String &p_author); // Add one scene as a type
	static void add_base(const String &p_native) { bases.insert(p_native); } // Add an engine type scripts may inherit

	// Whether scripts may inherit this scene type or fixed type
	_FORCE_INLINE_ static bool has(const String &p_name) { return scenes.has(p_name) || bases.has(p_name); }
	// Whether this scene type can be spawned under Server
	_FORCE_INLINE_ static bool spawnable(const String &p_name) { return scenes.has(p_name); }

	static String scene_of(const String &p_name); // Scene path of the type. Empty if none
	static Vector2i shape_of(const String &p_name); // Node count and depth including external scenes
	static String kind_of(const String &p_scene); // Type of a scene passed by the author. Empty if none
	static const char *OFF; // Name of the marker written as @online("auto_off")
	static Dictionary with_auto(Node *p_node, const Dictionary &p_marks, bool p_marked); // Add position and rotation of a Node2D/Node3D with @online to the shared properties without writing them
};
