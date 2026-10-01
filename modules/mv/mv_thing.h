/**************************************************************************/
/*  mv_thing.h                                                            */
/**************************************************************************/

// Provides world operations used by every @online Node, separated from game scripts.

#pragma once

#include "core/object/object_id.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "scene/resources/packed_scene.h"

class MVRuntime;
class Node;

class MVThing : public RefCounted {
	GDCLASS(MVThing, RefCounted);

	static ObjectID latest_owner; // Node allowed to use latest-wins during _process
	static uint64_t latest_frame; // Render frame in which latest-wins is allowed
	static HashMap<ObjectID, ObjectID> tree_allowed; // Child -> parent pairs the C++ gate may let through

protected:
	static void _bind_methods();

public:
	static MVRuntime *runtime(Node *p_node); // Return the runtime of the world holding this Node
	static bool is_my(Node *p_node); // Return whether it belongs to the local Player
	static bool guard_queue_free(Node *p_node); // Let the current Runtime decide queue_free of a synced Node
	static bool guard_free(Node *p_node); // Let the current Runtime decide immediate free() of a synced Node
	static bool guard_tree_change(Node *p_parent, Node *p_child); // Let the current Runtime decide parent/child changes in the synced world
	static void allow_tree_change(Node *p_parent, Node *p_child); // Allow one parent/child change from the C++ gate
	static Node *spawn(Node *p_node, const Ref<PackedScene> &p_scene, const Variant &p_a, const Variant &p_b); // Place one thing in the world. Trailing args are read by type
	static void own_cameras(Node *p_node, bool p_mine); // Cameras in owned things live only on the owner's side
	static void disconnected(Node *p_node); // Run the scheduled handling and signal on Player disconnect
	static void begin_local(Node *p_node); // On the Online side, open a section of script shared by everyone
	static void end_local(Node *p_node); // On the Online side, close a section of script shared by everyone
	static bool in_local(Node *p_node); // On the Online side, whether inside script shared by everyone
	static void begin_latest(Node *p_node); // Open the latest-wins section inside _process
	static void end_latest(Node *p_node); // Close the latest-wins section of the given Node
	static Signal ask(Node *p_node, const String &p_fn, const Array &p_args = Array()); // Send an @online function request by its use and make the answer awaitable
	static Signal secret(Node *p_door, const String &p_name, const Array &p_args); // Call the Secret's real body
	static void drive_world(ObjectID p_runtime, ObjectID p_state); // Advance the Online-side world one step and send the diff. Called from the physics tick (20Hz)
	static void world_clock(bool p_on); // Align the engine physics tick with the world clock (20Hz). False restores it
};
