/**************************************************************************/
/*  gdscript_online.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

// Holds @online analysis data inside GDScript and the boundary that lets only Online-side diffs write Client copies.

#pragma once

#include "core/os/mutex.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/object/object_id.h"
#include "core/templates/hash_set.h"
#include "core/variant/dictionary.h"

class Node;
class Object;

class GDScriptOnline {
public:
	static HashMap<String, bool> world_bases; // Path -> whether the script extends Online/Secret (cache for scripts without marks).

	static HashMap<String, Dictionary> fields; // Sync marks per script.
	static HashMap<String, Dictionary> bindings; // Display targets per script.
	static thread_local int trusted_writes; // Nesting depth of Online-side diff writes.
	static thread_local int field_inits; // Nesting depth of declaration initializer runs.
	static Mutex tables; // Guards the tables, since script parsing also runs on ResourceLoader threads.
	static HashSet<ObjectID> dirty; // Objects whose marked variables were written since the last collection; the collector skips others.

public:
	enum Field {
		F_ONLINE = 1 << 0, // Broadcast to everyone.
		F_MY = 1 << 1, // Send to the owner only.
		F_SAVE = 1 << 2, // Persist.
		F_SIGNAL = 1 << 3, // Signal declaration.
		F_METHOD = 1 << 4, // Function executed on the server.
		F_BIND = 1 << 5, // Direct binding to a Node property.
		F_SECRET = 1 << 6, // Function callable from outside a Secret.
		F_AUTO = 1 << 7, // Position and rotation synced without marks; companion values (floor, velocity) travel with them.
		F_INPUT = 1 << 22, // Value decided on the owner's client and sent to the Online side; not broadcast (bits 8-21 hold rate, smoothing, frames).
	};
	static bool (*owner_check)(Object *p_node); // Whether the object belongs to the local player; set by the world owner (modules/mv).
	static bool is_mine(Object *p_node) { return owner_check != nullptr && owner_check(p_node); }

	// Client display smoothing, chosen by the author as in @online(position, 20, "arc").
	enum Smooth {
		S_AUTO = 0, // Decide automatically from the Node and property.
		S_SNAP, // No smoothing; apply received values as is.
		S_SMOOTH, // Interpolate between received values; no prediction.
		S_PREDICT, // Interpolate, and extrapolate at constant velocity while values are missing.
		S_ARC, // Interpolate, and extrapolate with acceleration including gravity.
	};

	enum {
		RATE_SHIFT = 8, // Bit offset of the sync rate.
		RATE_MAX = 255, // Maximum syncs per second.
		SMOOTH_SHIFT = 16, // Bit offset of the smoothing mode.
		SMOOTH_MAX = 7, // Maximum smoothing mode id.
		FRAMES_SHIFT = 19, // Bit offset of the past frame count used to estimate speed.
		FRAMES_MAX = 7, // Maximum past frame count.
		CALL_ARGS_MAX = 8, // Maximum arguments of an @online function.
	};

	// Decide from source text alone whether the base is Secret. The single check shared by the exporter
	// and the tokenizer; if they disagreed, one could treat a script as non-Secret
	// and move it to .gdc while the other misses stripping it, leaking the source into the build.
	static bool secret_source(const String &p_source, const String &p_path);

	// Return the script's base from its text; used to keep type and location when the body is dropped.
	static String extends_of(const String &p_source);

	static void initialize(); // Clear the analysis tables.
	static void shutdown(); // Discard the analysis tables.
	static bool author(const String &p_path);
	static void touch(const String &p_path); // Record that the script has @online (from function marks kept out of the table).
	static void touched(Object *p_owner); // A marked variable was written on the Online side; flag the object for the next collection.
	static void take_dirty(HashSet<ObjectID> &r_out); // Hand over the written-object flags and clear them.
	static bool marked(const String &p_path); // Whether the author script has at least one @online.
	static bool author_name(const StringName &p_name); // Whether the name is provided by the automatic base.
	static void begin_trusted_write(); // Open the Online-side diff write boundary.
	static void end_trusted_write(); // Close the Online-side diff write boundary.
	static bool is_trusted_write(); // Whether an Online-side diff write is in progress.
	static void begin_field_init(); // Open the declaration initializer boundary.
	static void end_field_init(); // Close the declaration initializer boundary.
	static bool is_field_init(); // Whether declaration initializers are running.

	_FORCE_INLINE_ static int rate_of(int p_flags) { return (p_flags >> RATE_SHIFT) & RATE_MAX; }
	_FORCE_INLINE_ static int smooth_of(int p_flags) { return (p_flags >> SMOOTH_SHIFT) & SMOOTH_MAX; }
	_FORCE_INLINE_ static int frames_of(int p_flags) { return (p_flags >> FRAMES_SHIFT) & FRAMES_MAX; }
	static int smooth_id(const String &p_name); // Smoothing name to id; -1 if unknown.
	static String smooth_names(); // List of smoothing names shown to the author.
	_FORCE_INLINE_ static bool is_dynamic(const String &p_path) {
		return p_path.is_empty() || p_path.begins_with("gdscript://") || p_path.begins_with("online-authority://") || p_path.begins_with("online-client://");
	}

	static void mark(const String &p_path, const StringName &p_name, int p_flags); // Record a sync mark.
	static void bind_field(const String &p_path, const StringName &p_name, const StringName &p_target); // Record a display target.
	static Dictionary fields_of(const String &p_path); // Return the script's sync marks.
	static Dictionary bindings_of(const String &p_path); // Return the script's display targets.
	static void ready(Node *p_node); // Run ready for a script attached later.
	static Vector<StringName> callback_names(); // Callbacks the engine calls while walking the tree.
	static String callback_args(const StringName &p_name); // Arguments that callback receives.
	static bool is_callback(const StringName &p_name); // Whether the name is a Node callback called by the engine.

};
