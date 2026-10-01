/**************************************************************************/
/*  mv_gate.h                                                             */
/**************************************************************************/

// Gate that checks each tree-changing request one by one and applies only accepted ones to the real tree.
//
// The tree is the engine's Nodes themselves. Things may be added or removed only under Server.
// Owner and serial number are set at creation. After that the thing is referred to by its serial number.

#pragma once

#include "core/object/ref_counted.h"
#include "scene/main/node.h"

class MVGate : public RefCounted {
	GDCLASS(MVGate, RefCounted);

public:
	enum {
		PER_TICK = 200, // Max requests accepted per time step
		NODES_MAX = 4096, // Total things that can be in the tree
		DEPTH_MAX = 32, // Max depth from the root
		NAME_LEN = 64, // Max name length
		UID_SERVER = 1, // Serial number of Server. Same on sending and receiving sides
		UID_PLAYERS = 2, // Lower bound of issued serial numbers. At or below are fixed Nodes present on both sides from the start
	};

private:
	Array rejects; // Rejected requests and reasons
	int nodes = 0; // Node count in the current world
	int requests = 0; // Requests received in the current time step
	ObjectID root_id; // Root of the world currently counted

	static int64_t last_uid; // Last issued serial number
	static int64_t fixed_uid; // Max serial number of fixed Nodes present on both sides from the start

	void ng(const Variant &p_op, const String &p_reason); // Record a rejected request with its reason
	static bool shape(const String &p_kind, int &r_nodes, int &r_depth); // Read the scene size before creation
	static int depth(Node *p_node); // Recheck max depth after instancing

protected:
	static void _bind_methods();

public:
	static const char *SERVER; // Name of the server-authoritative node

	static bool from_server(const String &p_path); // Whether it is Server or under it
	static bool safe_name(const String &p_name); // Whether it can be used as a name
	static int count(Node *p_node); // Count of itself and descendants
	static void quiet(Node *p_node); // Stop playing Audio in a tree being discarded
	static int64_t next_uid(); // A serial number not yet used
	static void seed_uid(int64_t p_last); // Advance serial numbers past the fixed Nodes
	static int64_t fixed_max() { return fixed_uid; } // At or below this exist on both sides from the start

	// Normalize a path string by walking the tree.
	// All checks use this form so `..` or absolute paths cannot bypass the gate.
	// Empty if it points outside the tree
	static String real_path(Node *p_root, const String &p_path);

	// Create one from a type: instance the scene, attach owner and serial number, and return it. nullptr on failure.
	// Passing 0 as serial number takes a new one. Receivers use the delivered number as-is
	static Node *make(const String &p_kind, const String &p_name, const String &p_own, int64_t p_uid);

	void begin(Node *p_root); // When the world changes, count its total; reset the per-step request count
	Node *prepare(Node *p_root, Node *p_parent, const String &p_kind, const String &p_own); // Create a checked child without adding it to the tree
	bool attach(Node *p_root, Node *p_parent, Node *p_node); // Add a checked child to the tree exactly once
	bool removable(Node *p_root, Node *p_from, Node *p_node, bool p_counted); // Answer that only things under Server may be removed, and subtract the Node count
	bool destroy(Node *p_root, Node *p_from, Node *p_node);
	bool detach(Node *p_root, Node *p_from, Node *p_node); // Detach from the tree and subtract the count only. Freeing is left to the caller

	// Clears on read, so repeated refusals in one time step are not recounted
	Array take_rejects() {
		Array out = rejects;
		rejects = Array();
		return out;
	}
};
