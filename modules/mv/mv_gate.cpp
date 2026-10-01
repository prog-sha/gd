/**************************************************************************/
/*  mv_gate.cpp                                                           */
/**************************************************************************/

// Centrally manages creation and removal under Server with ownership and size limits.

#include "mv_gate.h"

#include "mv_classes.h"
#include "mv_client_script.h"
#include "mv_thing.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "scene/2d/audio_stream_player_2d.h"
#include "scene/3d/audio_stream_player_3d.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/resources/packed_scene.h"
#include "servers/physics_2d/physics_server_2d.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "modules/gdscript/gdscript_online.h"

const char *MVGate::SERVER = "Online";
int64_t MVGate::last_uid = MVGate::UID_PLAYERS;
int64_t MVGate::fixed_uid = MVGate::UID_PLAYERS;

// Build the name in one place. It follows automatically if SERVER changes
static const String SERVER_IN = String(MVGate::SERVER) + "/";

// Whether it is Server or under it. Only requests from here may change the shape
bool MVGate::from_server(const String &p_path) {
	return p_path == SERVER || p_path.begins_with(SERVER_IN);
}

int64_t MVGate::next_uid() {
	return ++last_uid;
}

// Advance issued serial numbers past the fixed Nodes known when the world is built.
// Both sides walk the same scene in the same order, so fixed Node numbers match naturally
void MVGate::seed_uid(int64_t p_last) {
	fixed_uid = MAX(fixed_uid, p_last);
	last_uid = MAX(last_uid, p_last);
}

// Normalize a path string by walking the tree. Empty if it points outside the tree
String MVGate::real_path(Node *p_root, const String &p_path) {
	Node *n = p_root->get_node_or_null(NodePath(p_path));
	if (n == nullptr || n == p_root || !p_root->is_ancestor_of(n)) {
		return String();
	}
	return String(p_root->get_path_to(n));
}

// Whether it can be used as a name. Rejects chars that could turn it into a path
bool MVGate::safe_name(const String &p_name) {
	if (p_name.is_empty() || p_name.length() > NAME_LEN) {
		return false;
	}
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok) {
			return false;
		}
	}
	return true;
}

int MVGate::count(Node *p_node) {
	int n = 1;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		n += count(p_node->get_child(i));
	}
	return n;
}

// Stop descendant Audio before detaching and release playback resources at the same time
void MVGate::quiet(Node *p_node) {
	if (AudioStreamPlayer *player = Object::cast_to<AudioStreamPlayer>(p_node)) {
		player->stop();
		player->set_stream(Ref<AudioStream>());
	} else if (AudioStreamPlayer2D *player = Object::cast_to<AudioStreamPlayer2D>(p_node)) {
		player->stop();
		player->set_stream(Ref<AudioStream>());
	} else if (AudioStreamPlayer3D *player = Object::cast_to<AudioStreamPlayer3D>(p_node)) {
		player->stop();
		player->set_stream(Ref<AudioStream>());
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		quiet(p_node->get_child(i));
	}
}

// Return max depth after instancing expands external scenes
int MVGate::depth(Node *p_node) {
	int result = 1;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		result = MAX(result, 1 + depth(p_node->get_child(i)));
	}
	return result;
}

void MVGate::ng(const Variant &p_op, const String &p_reason) {
	Dictionary d;
	d["op"] = p_op;
	d["reason"] = p_reason;
	rejects.push_back(d);
}

// Read Node count and max depth before instancing the scene, so rejection costs no creation
bool MVGate::shape(const String &p_kind, int &r_nodes, int &r_depth) {
	const Vector2i known = MVClasses::shape_of(p_kind);
	if (known.x > 0 && known.y > 0) {
		r_nodes = known.x;
		r_depth = known.y;
		return r_nodes <= NODES_MAX && r_depth <= DEPTH_MAX;
	}
	Ref<PackedScene> sc = ResourceLoader::load(MVClasses::scene_of(p_kind), "PackedScene");
	if (sc.is_null()) {
		return false;
	}
	Ref<SceneState> state = sc->get_state();
	if (state.is_null() || state->get_node_count() > NODES_MAX) {
		return false;
	}
	r_nodes = state->get_node_count();
	r_depth = 1;
	for (int i = 0; i < state->get_node_count(); i++) {
		r_depth = MAX(r_depth, state->get_node_path(i).get_name_count() + 1);
	}
	return r_depth <= DEPTH_MAX;
}

Node *MVGate::make(const String &p_kind, const String &p_name, const String &p_own, int64_t p_uid) {
	Ref<PackedScene> sc = MVClientScript::quiet_scene(MVClasses::scene_of(p_kind));
	if (sc.is_null()) {
		return nullptr;
	}
	// Any Node that is a PackedScene root can be placed in the world, regardless of type
	Node *n = sc->instantiate();
	if (n == nullptr) {
		return nullptr;
	}
	n->set_name(p_name);
	n->set_meta(SNAME("kind"), p_kind);
	n->set_meta(SNAME("own"), p_own);
	n->set_meta(SNAME("uid"), p_uid > 0 ? p_uid : next_uid());
	n->set_meta(SNAME("mark"), Dictionary()); // Marks are decided when the script is attached
	return n;
}

// Count the tree state used in one time step, once
void MVGate::begin(Node *p_root) {
	rejects = Array();
	requests = 0;
	const ObjectID next_root = p_root == nullptr ? ObjectID() : p_root->get_instance_id();
	// The gate counts changes in the same world itself, so the full tree walk is needed only the first time
	if (root_id != next_root) {
		root_id = next_root;
		nodes = p_root == nullptr ? 0 : count(p_root);
	}
}

// Create a child, before the Online-side script is attached, from only type and owner
Node *MVGate::prepare(Node *p_root, Node *p_parent, const String &p_kind, const String &p_own) {
	if (root_id != p_root->get_instance_id()) {
		begin(p_root);
	}
	Dictionary op;
	op["op"] = "spawn";
	op["kind"] = p_kind;
	if (requests++ >= PER_TICK) {
		ng(op, U"over the per-tick limit");
		return nullptr;
	}
	const String parent = real_path(p_root, String(p_root->get_path_to(p_parent)));
	if (parent.is_empty() || !from_server(parent)) {
		ng(op, U"only allowed under Online");
		return nullptr;
	}
	if (!MVClasses::spawnable(p_kind)) {
		ng(op, String(U"unknown kind: ") + p_kind);
		return nullptr;
	}
	int scene_nodes = 0;
	int scene_depth = 0;
	const int parent_depth = NodePath(parent).get_name_count();
	if (!shape(p_kind, scene_nodes, scene_depth) || nodes + scene_nodes > NODES_MAX ||
			parent_depth + scene_depth > DEPTH_MAX) {
		ng(op, nodes + scene_nodes > NODES_MAX ? U"over the node limit" : U"over the depth limit");
		return nullptr;
	}
	const int64_t uid = next_uid();
	const String name = p_kind + "_" + String::num_int64(uid);
	Node *made = make(p_kind, name, p_own, uid);
	if (made == nullptr) {
		ng(op, String(U"cannot instantiate: ") + p_kind);
		return nullptr;
	}
	const int actual_nodes = count(made);
	const int actual_depth = depth(made);
	if (nodes + actual_nodes > NODES_MAX || parent_depth + actual_depth > DEPTH_MAX) {
		memdelete(made);
		ng(op, nodes + actual_nodes > NODES_MAX ? U"over the node limit" : U"over the depth limit");
		return nullptr;
	}
	return made;
}

// Add to the tree only children whose Online-side script is attached
bool MVGate::attach(Node *p_root, Node *p_parent, Node *p_node) {
	if (root_id != p_root->get_instance_id()) {
		begin(p_root);
	}
	const String parent = real_path(p_root, String(p_root->get_path_to(p_parent)));
	const int made_nodes = count(p_node);
	const int made_depth = depth(p_node);
	if (parent.is_empty() || !from_server(parent) || nodes + made_nodes > NODES_MAX ||
			NodePath(parent).get_name_count() + made_depth > DEPTH_MAX) {
		ng("attach", U"cannot add the checked scene under Online");
		return false;
	}
	MVThing::allow_tree_change(p_parent, p_node);
	// Things may be placed from inside a collision signal. Touching the tree while physics is dispatching results
	// makes the engine stop, so only then wait until the end of the frame to add it
	if (PhysicsServer2D::get_singleton()->is_flushing_queries() ||
			PhysicsServer3D::get_singleton()->is_flushing_queries()) {
		p_parent->call_deferred(SNAME("add_child"), p_node);
	} else {
		p_parent->add_child(p_node);
	}
	nodes += made_nodes;
	return true;
}

// Check caller and target as Node references and answer that only things under Server may be removed.
// If removal is allowed, the Node count is subtracted here
bool MVGate::removable(Node *p_root, Node *p_from, Node *p_node, bool p_counted) {
	ERR_FAIL_NULL_V(p_from, false);
	if (root_id != p_root->get_instance_id()) {
		begin(p_root);
	}
	Dictionary op;
	op["op"] = "destroy";
	if (p_counted && requests++ >= PER_TICK) {
		ng(op, U"over the per-tick limit");
		return false;
	}
	const String from = real_path(p_root, String(p_root->get_path_to(p_from)));
	const String path = real_path(p_root, String(p_root->get_path_to(p_node)));
	if (!from_server(from) || path.is_empty() || path == SERVER) {
		ng(op, String(U"cannot remove node: ") + path);
		return false;
	}
	nodes -= count(p_node);
	return true;
}

// Hand a Node that passed the gate to the engine's cleanup
bool MVGate::destroy(Node *p_root, Node *p_from, Node *p_node) {
	if (!removable(p_root, p_from, p_node, true)) {
		return false;
	}
	// Detaching is left to the engine. Even when removed inside a collision signal,
	// the engine cleans up at the end of the frame, so the tree does not change while physics is running.
	// The gate itself decided the removal, so this queue_free is not routed back to the gate
	GDScriptOnline::begin_trusted_write();
	p_node->queue_free();
	GDScriptOnline::end_trusted_write();
	return true;
}

// Detach from the tree and subtract the count only. The caller continues with the release.
// free() must remove it immediately as usual, so no queue_free here
bool MVGate::detach(Node *p_root, Node *p_from, Node *p_node) {
	if (!removable(p_root, p_from, p_node, false)) {
		return false;
	}
	Node *parent = p_node->get_parent();
	MVThing::allow_tree_change(parent, p_node);
	parent->remove_child(p_node);
	return true;
}

// Used only from C++. Nothing is exposed to GDScript
void MVGate::_bind_methods() {
}
