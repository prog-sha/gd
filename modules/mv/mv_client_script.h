/**************************************************************************/
/*  mv_client_script.h                                                    */
/**************************************************************************/

// Finds Client scenes and generates @online call stubs without shipping GDScript.

#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "scene/resources/packed_scene.h"

class GDScript;
class Node;

class MVClientScript {
	static HashMap<String, String> sources; // Kind to author script
	static HashMap<String, String> natives; // Kind to the scene root's native type
	static HashMap<String, String> views; // Kind to display scene
	static HashMap<String, String> authority_scenes; // Kind to Online-side scene
	static HashMap<String, Dictionary> scene_values; // Kind to script values written in the scene
	static HashMap<String, ObjectID> scripts; // Live Client scripts sharing the same API
	static HashMap<String, Ref<PackedScene>> quiet; // Cached scenes with connections removed, used for building
	static HashMap<String, bool> secret_roots; // Cache mapping a scene path to whether its root is Secret

	static bool secret_root_scene(const String &p_path); // Whether the scene's root is Secret

	static Error add_scene(const String &p_path, Dictionary &r_scenes, Dictionary &r_shapes); // Builds the display copy of one scene
	static String bind_source(const String &p_source, const String &p_native, bool p_server = false); // Binds an author script to the shared base
	static String client_source(const String &p_source, const Dictionary &p_fns); // Turns @online script bodies into send stubs
	static String base_path(const String &p_native, bool p_server = false); // Prepares the shared base for each native type

public:
	static String attribute(const String &p_line, const String &p_name); // Reads an attribute of a tscn header
	static String external_id(const String &p_line, const String &p_key); // Reads an ExtResource ID
	enum {
		ARGS_SANE = 64, // Max trusted argument count for a signal arriving over the wire
	};

	static const char *STEP; // Internal name of the physics body the Online side calls at 20Hz

	static String hollow_text(const String &p_source); // Strips Secret contents from a text scene
	static String world_scene(); // Scene whose root is Online. Finds exactly one in the whole project
	static String world_script(); // Script attached to the world root
	static String ensure_scene(const String &p_path); // Registers a spawnable scene and returns its kind name. Empty on failure
	static String author_scene_of(const String &p_kind); // Location of the author's scene for a kind
	static String ensure_kind(const String &p_kind); // Finds and registers a scene by kind name (root name). Empty if none
	static bool has_view(const String &p_kind) { return views.has(p_kind); } // Whether a local display scene exists
	static String configure(); // Auto-detects the world scene and sets up the Client display table
	static String configure_authority(); // Switches the same detection result to the Online-side scene table
	static void add_fixed(const String &p_kind, const String &p_source, const String &p_native); // Records a fixed Node under Online as a kind
	static String door_source(const Dictionary &p_doors); // Builds the script for Secret's entries
	static String source_of(const String &p_kind); // Returns the author script of a kind
	static constexpr const char *IMPLEMENTATION = "_online_server_implementation_"; // Name prefix for where @online func bodies are moved on the Online side
	static String native_of(const String &p_kind); // Returns the Node native type of a kind
	static String authority_source(const String &p_source, const String &p_native, bool p_server, const Dictionary &p_marks); // Binds an Online-side script to the shared base
	static Ref<PackedScene> quiet_scene(const String &p_path); // Returns the scene with connections removed
	static void wire_signals(Node *p_node, const String &p_kind, bool p_authority); // Reconnects scene connections to the script's signals
	static void split_signals(Node *p_node, const Dictionary &p_marks, bool p_authority); // Keeps scene-connected signals only on the emitting side
	static Node *make(const String &p_kind, const String &p_name, const String &p_own, int64_t p_uid); // Creates a Client display scene from the fixed table
	static Object *dress(Node *p_node, const Dictionary &p_api); // Loads send stubs onto a public Node
	static void clear(); // Releases generated Script references before exit
};
