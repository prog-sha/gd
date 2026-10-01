/**************************************************************************/
/*  mv_stage.cpp                                                          */
/**************************************************************************/

// Builds the fixed world tree under the runtime and strips or records Secret and author scripts.

#include "mv_stage.h"

#include "mv_client_script.h"
#include "mv_frame.h"
#include "mv_gate.h"
#include "mv_online.h"
#include "mv_secret.h"

#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "scene/resources/packed_scene.h"

// Whether a Secret exists anywhere in the tree. Checks by type, not text,
// so none are missed whether instanced, placed at the root, or nested
bool MVStage::finds_secret(Node *p_node) {
	if (Object::cast_to<Secret>(p_node) != nullptr) {
		return true;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (finds_secret(p_node->get_child(i))) {
			return true;
		}
	}
	return false;
}

// Only the Secret entry remains on the Client. Its script, values and child Nodes are not handed over.
// The Node itself stays, so it points at the same place when it becomes the server in a host relay
void MVStage::hollow_secrets(Node *p_node) {
	if (Secret *here = Object::cast_to<Secret>(p_node)) {
		here->drop_server_settings();
		p_node->set_script(Variant());
		for (int i = p_node->get_child_count() - 1; i >= 0; i--) {
			Node *inside = p_node->get_child(i);
			p_node->remove_child(inside);
			memdelete(inside);
		}
		return;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		hollow_secrets(p_node->get_child(i));
	}
}

// On the Online side, record only the script path and detach it. Unless detached before entering the tree,
// the scene's original script runs _ready before it is replaced with the Online-side script
void MVStage::hold_secrets(Node *p_node) {
	if (Object::cast_to<Secret>(p_node) != nullptr) {
		const Ref<Script> attached = p_node->get_script();
		if (attached.is_valid()) {
			// Built-in scripts cannot be stripped from exports or rebuilt into Online-side scripts.
			// Instead of silently running the raw script, tell the author to save it to a file.
			// A script written inside a scene has a path like `…tscn::id`; emptiness alone cannot tell
			if (attached->get_path().is_empty() || attached->get_path().contains("::")) {
				ERR_PRINT(vformat(U"Online: the Secret script of %s is a built-in script. Save it as a .gd file so it can be kept from clients", String(p_node->get_name())));
			} else {
				p_node->set_meta(SNAME("secret_src"), attached->get_path());
			}
			p_node->set_script(Variant());
		}
		p_node->set_meta(SNAME("kind"), Secret::KIND);
	}
	// Secrets nested inside a Secret get the same treatment. Stopping here would leave the inner script
	for (int i = 0; i < p_node->get_child_count(); i++) {
		hold_secrets(p_node->get_child(i));
	}
}

// Record only the script's values that can be sent over the network. __-prefixed ones are runtime internals
void MVStage::script_values(Node *p_node, Dictionary &r_keep) {
	ScriptInstance *instance = p_node->get_script_instance();
	if (instance == nullptr) {
		return;
	}
	List<PropertyInfo> props;
	instance->get_property_list(&props);
	for (const PropertyInfo &prop : props) {
		if ((prop.usage & PROPERTY_USAGE_SCRIPT_VARIABLE) && !String(prop.name).begins_with("__")) {
			bool valid = false;
			const Variant value = p_node->get(prop.name, &valid);
			if (valid && MVFrame::plain(value)) {
				r_keep[prop.name] = value;
			}
		}
	}
}

// Detach the script before entering the tree and keep only its values. Otherwise, before it is replaced with
// the script with the shared base added, the scene's original _ready runs without the base's tools
void MVStage::hold_body(Node *p_node) {
	const Ref<Script> attached = p_node->get_script();
	if (attached.is_null() || attached->get_path().is_empty()) {
		return;
	}
	// Values edited in the scene inspector vanish with the script, so record them first
	Dictionary keep;
	script_values(p_node, keep);
	p_node->set_meta(SNAME("held_values"), keep);
	p_node->set_script(Variant());
}

// Hand recorded values to the script reload. Clear the marker so they are not used twice
void MVStage::take_held(Node *p_node, Dictionary &r_keep) {
	if (!p_node->has_meta(SNAME("held_values"))) {
		return;
	}
	const Dictionary held = p_node->get_meta(SNAME("held_values"));
	for (const Variant &name : held.get_key_list()) {
		r_keep[name] = held[name];
	}
	p_node->remove_meta(SNAME("held_values"));
}

// Give Nodes with author scripts under Online a serial number and type that match on both sides.
// Both sides walk the same scene in the same order, so numbers match naturally. Secret contents are outside distribution and skipped
static void mark_fixed(Node *p_server, Node *p_node, int64_t &r_uid) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		if (Object::cast_to<Secret>(child) != nullptr) {
			continue;
		}
		const Ref<Script> body = child->get_script();
		if (body.is_valid() && !body->get_path().is_empty()) {
			const String kind = String(MVGate::SERVER) + "/" + String(p_server->get_path_to(child));
			child->set_meta(SNAME("uid"), ++r_uid);
			child->set_meta(SNAME("kind"), kind);
			child->set_meta(SNAME("own"), String());
			MVClientScript::add_fixed(kind, body->get_path(), child->get_class());
			// Detach first so the scene's original script does not run _ready before the shared base is added
			MVStage::hold_body(child);
		}
		mark_fixed(p_server, child, r_uid);
	}
}

// Put the author's Online directly under the runtime. Both sides get the same shape
Online *MVStage::build(Node *p_parent, bool p_authority) {
	ERR_FAIL_NULL_V(p_parent, nullptr);
	// If the author placed res://online/Online.tscn, it becomes the world itself.
	// That lets it have children such as score displays and connect signals from the inspector
	Online *server = nullptr;
	const String scene = MVClientScript::world_scene();
	if (ResourceLoader::exists(scene, "PackedScene")) {
		Ref<PackedScene> packed = MVClientScript::quiet_scene(scene);
		if (packed.is_valid()) {
			server = Object::cast_to<Online>(packed->instantiate());
			if (server == nullptr) {
				ERR_PRINT(vformat(U"Online: the root of %s must be Online", scene));
			}
		}
	}
	if (server == nullptr) {
		server = memnew(Online);
	}
	// In the Client world, strip script, children and Online-only settings from Secrets.
	// The Node itself stays so it points at the same place when this side becomes the host
	if (p_authority) {
		hold_secrets(server);
	} else {
		hollow_secrets(server);
	}
	// Detach the world's own script before entering the tree too. Detaching after entering
	// runs the original _ready without the shared base one extra time
	hold_body(server);
	server->set_name(MVGate::SERVER);
	server->set_meta(SNAME("uid"), MVGate::UID_SERVER);
	server->set_meta(SNAME("kind"), Online::KIND);
	// Author scripts under Online sync values just like spawned things.
	// No spawn notice is sent, since these fixed Nodes exist in both worlds from the start
	int64_t fixed = MVGate::UID_PLAYERS;
	mark_fixed(server, server, fixed);
	MVGate::seed_uid(fixed);
	p_parent->add_child(server);
	return server;
}

// Check that what follows Player is a positive integer
bool MVStage::is_who(const String &p_name) {
	if (!p_name.begins_with("Player")) {
		return false;
	}
	const String suffix = p_name.substr(6);
	return !suffix.is_empty() && suffix.is_valid_int() && suffix.to_int() > 0;
}

// Used only from C++. Nothing is exposed to GDScript
void MVStage::_bind_methods() {
}
