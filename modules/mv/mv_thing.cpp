/**************************************************************************/
/*  mv_thing.cpp                                                          */
/**************************************************************************/

// Relays common @online Node operations only to the isolated world's public entry points.

#include "mv_thing.h"

#include "mv_link.h"
#include "mv_online.h"
#include "mv_rep.h"
#include "mv_runtime.h"

#include "core/config/engine.h"
#include "core/os/time.h"
#include "scene/2d/camera_2d.h"
#include "scene/3d/camera_3d.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "scene/main/node.h"

#include "modules/gdscript/gdscript_online.h"

ObjectID MVThing::latest_owner;
uint64_t MVThing::latest_frame = 0;
HashMap<ObjectID, ObjectID> MVThing::tree_allowed;

// Return the runtime of the world holding this Node. On a device acting as host, the Online-side world and
// its own local world share one process, so remembering a single destination would mix them up.
// Nodes of both worlds sit under a runtime, so walking up the tree always reaches the right world.
// A Node detached from the tree belongs to no world
MVRuntime *MVThing::runtime(Node *p_node) {
	for (Node *at = p_node; at != nullptr; at = at->get_parent()) {
		if (MVRuntime *found = Object::cast_to<MVRuntime>(at)) {
			// A world that is shutting down is not returned. Otherwise the engine cannot finish freeing children and the tree cannot close
			return found->is_closing() ? nullptr : found;
		}
	}
	return nullptr;
}

// Whether the Node belongs to the authenticated local user
bool MVThing::is_my(Node *p_node) {
	MVRuntime *rt = runtime(p_node);
	return rt != nullptr && rt->is_my(p_node);
}

// Pass only queue_free of synced Nodes to the Runtime's Online-side decision
bool MVThing::guard_queue_free(Node *p_node) {
	if (GDScriptOnline::is_trusted_write()) {
		return false;
	}
	MVRuntime *rt = runtime(p_node);
	return rt != nullptr && rt->queue_node(p_node);
}

// free() also goes through the gate. On the Online side the gate only detaches it from the tree
// and the release continues as usual. To the author it is the same as a normal free():
// the Node is gone right after the call. The local side refuses
bool MVThing::guard_free(Node *p_node) {
	if (GDScriptOnline::is_trusted_write()) {
		return false;
	}
	MVRuntime *rt = runtime(p_node);
	return rt != nullptr && rt->free_node(p_node);
}

// Let the read-only Runtime decide only parent/child changes in the Client's public world
bool MVThing::guard_tree_change(Node *p_parent, Node *p_child) {
	if (p_parent != nullptr && p_child != nullptr) {
		const HashMap<ObjectID, ObjectID>::Iterator found = tree_allowed.find(p_child->get_instance_id());
		if (found && found->value == p_parent->get_instance_id()) {
			tree_allowed.remove(found);
			return false;
		}
	}
	if (GDScriptOnline::is_trusted_write()) {
		return false;
	}
	// A Node already marked for deletion leaving the tree is the engine cleaning up. Do not refuse
	if (p_child != nullptr && p_child->is_queued_for_deletion()) {
		return false;
	}
	// If either parent or child is inside a world, show it to that world's runtime
	MVRuntime *rt = runtime(p_parent);
	if (rt == nullptr) {
		rt = runtime(p_child);
	}
	return rt != nullptr && rt->tree_node(p_parent, p_child);
}

// Let a parent/child pair from a checked C++ operation through once, the next time that pair arrives.
// Tree insertion may be deferred, so any number of pending pairs are not mixed up
void MVThing::allow_tree_change(Node *p_parent, Node *p_child) {
	if (p_parent == nullptr || p_child == nullptr) {
		return;
	}
	tree_allowed[p_child->get_instance_id()] = p_parent->get_instance_id();
}

// Route the place-in-world entry point from any Node's script to Online.
// Placement is fixed under Online, so the author does not choose the parent
Node *MVThing::spawn(Node *p_node, const Ref<PackedScene> &p_scene, const Variant &p_a, const Variant &p_b) {
	MVRuntime *rt = runtime(p_node);
	Online *host = rt != nullptr ? rt->server_node() : nullptr;
	return host != nullptr ? host->spawn(p_scene, p_a, p_b) : nullptr;
}

// Cameras inside owned things live only on the owner's side. Others' things and dedicated servers turn them off.
// Authors need not write "enable only my camera"
void MVThing::own_cameras(Node *p_node, bool p_mine) {
	if (Camera2D *flat = Object::cast_to<Camera2D>(p_node)) {
		flat->set_enabled(p_mine);
	} else if (Camera3D *deep = Object::cast_to<Camera3D>(p_node)) {
		deep->set_current(p_mine);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		own_cameras(p_node->get_child(i), p_mine);
	}
}

// Tell the author about the disconnect. Received like any other signal, connected from the scene
void MVThing::disconnected(Node *p_node) {
	if (p_node->has_signal(SNAME("disconnected"))) {
		p_node->emit_signal(SNAME("disconnected"));
	}
}

// Advance the Online-side world at 20Hz for the elapsed render frames and send that step's diff to everyone.
// The world may close during tick, so the runtime and link are looked up again each time.
// The world clock is the engine physics tick itself. Since it advances inside the physics tick, move_and_slide()
// called from author scripts also advances by 1/20 s. Advancing on the render tick would make move_and_slide() use the render delta
// and disagree with formulas the author wrote with delta (gravity, jumps)
void MVThing::drive_world(ObjectID p_runtime, ObjectID p_state) {
	MVRuntime *rt = Object::cast_to<MVRuntime>(ObjectDB::get_instance(p_runtime));
	MVLink *link = Object::cast_to<MVLink>(ObjectDB::get_instance(p_state));
	if (rt == nullptr || link == nullptr) {
		return;
	}
	const Dictionary frame = rt->tick();
	for (const String &who : link->players()) {
		link->push(who, rt->for_player(frame, who));
	}
}

static int physics_before = 0; // Physics tick rate before holding a world. Used to restore it

void MVThing::world_clock(bool p_on) {
	Engine *engine = Engine::get_singleton();
	if (p_on) {
		if (physics_before == 0) {
			physics_before = engine->get_physics_ticks_per_second();
		}
		engine->set_physics_ticks_per_second(MVRep::TICK_HZ);
	} else if (physics_before > 0) {
		engine->set_physics_ticks_per_second(physics_before);
		physics_before = 0;
	}
}

// Online-side section of script shared by everyone. The marker is set on the runtime of that Node's world
void MVThing::begin_local(Node *p_node) {
	if (MVRuntime *rt = runtime(p_node)) {
		rt->begin_local();
	}
}

void MVThing::end_local(Node *p_node) {
	if (MVRuntime *rt = runtime(p_node)) {
		rt->end_local();
	}
}

bool MVThing::in_local(Node *p_node) {
	MVRuntime *rt = runtime(p_node);
	return rt != nullptr && rt->in_local();
}

// Only during the Client's _process, open a section that does not queue stale inputs of the same function
void MVThing::begin_latest(Node *p_node) {
	latest_owner = p_node != nullptr ? p_node->get_instance_id() : ObjectID();
	latest_frame = Engine::get_singleton()->get_process_frames();
}

// Close the section so latest-wins state does not leak into other callbacks
void MVThing::end_latest(Node *p_node) {
	if (p_node != nullptr && latest_owner == p_node->get_instance_id()) {
		latest_owner = ObjectID();
	}
}

// Send an @online function as latest input during _process, otherwise as a reliable event
Signal MVThing::ask(Node *p_node, const String &p_fn, const Array &p_args) {
	MVRuntime *rt = runtime(p_node);
	if (rt == nullptr) {
		return Signal();
	}
	const bool latest = p_node != nullptr && latest_owner == p_node->get_instance_id() &&
			latest_frame == Engine::get_singleton()->get_process_frames();
	const int64_t uid = p_node != nullptr ? int64_t(p_node->get_meta(SNAME("uid"), 0)) : 0;
	return latest ? rt->ask_latest(uid, p_fn, p_args) : rt->ask(uid, p_fn, p_args);
}

// Call the real function from the Secret entry. Returns a Signal so the answer can be awaited
Signal MVThing::secret(Node *p_door, const String &p_name, const Array &p_args) {
	MVRuntime *rt = runtime(p_door);
	return rt != nullptr ? rt->secret_call(p_door, p_name, p_args) : Signal();
}

// Internal class, so GDScript cannot reach it by name. Only the Online side is exposed to authors
void MVThing::_bind_methods() {
}
