/**************************************************************************/
/*  mv_online.cpp                                                         */
/**************************************************************************/

// Fixed Node representing the world itself, and the internal singleton used only by generated scripts.

#include "mv_online.h"

#include "mv_classes.h"
#include "mv_client_script.h"
#include "mv_client.h"
#include "mv_runtime.h"
#include "mv_secret.h"
#include "mv_stage.h"
#include "mv_thing.h"
#ifdef TOOLS_ENABLED
#include "mv_tr.h"
#endif

#include "core/object/class_db.h"
#include "scene/main/node.h"

#include "modules/gdscript/gdscript_online.h"

const char *Online::KIND = "Online";

// In the editor, tell what this world still lacks.
// Not a scolding; it only says what to place next
PackedStringArray Online::get_configuration_warnings() const {
	PackedStringArray warnings = Node::get_configuration_warnings();
#ifdef TOOLS_ENABLED
	if (!MVStage::finds_secret(const_cast<Online *>(this))) {
		warnings.push_back(RTR(MVTr::SECRET_HINT));
	}
#endif
	return warnings;
}

// When children are added or removed, ask for the ⚠ to be refreshed.
// It must vanish the moment a Secret is placed, or the author cannot tell it worked
void Online::_notification(int p_what) {
	if (p_what == NOTIFICATION_CHILD_ORDER_CHANGED || p_what == NOTIFICATION_ENTER_TREE) {
		update_configuration_warnings();
	}
}

// Switch permission to the world side only while calling a world method
void Online::begin_online_call() {
	if (MVRuntime *rt = MVThing::runtime(this)) {
		rt->begin_server_call();
	}
}

// Restore the calling Node's permission
void Online::end_online_call() {
	if (MVRuntime *rt = MVThing::runtime(this)) {
		rt->end_server_call();
	}
}

// Create an auto-named child directly under the world and return the ready-to-use Node
Node *Online::spawn(const Ref<PackedScene> &p_scene, const Variant &p_a, const Variant &p_b) {
	MVRuntime *rt = MVThing::runtime(this);
	// Trailing args are read by type, not order. A dictionary is initial values; anything else (my or an owned thing) is the owner
	Dictionary props;
	Variant owner;
	for (const Variant &one : { p_a, p_b }) {
		if (one.get_type() == Variant::DICTIONARY) {
			props = one;
		} else if (one.get_type() != Variant::NIL) {
			owner = one;
		}
	}
	// All author scripts running on the Online side are equally trusted, so any Node's @online func may place things.
	// Calls from the local side are warned about and refused by the runtime
	if (rt == nullptr) {
		return nullptr;
	}
	// The scene may live anywhere. It is registered as a type the first time it is passed
	const String kind = p_scene.is_valid() ? MVClientScript::ensure_scene(p_scene->get_path()) : String();
	if (kind.is_empty()) {
		ERR_PRINT(vformat(U"Online: spawn() takes a scene whose root is a Node: %s", p_scene.is_valid() ? p_scene->get_path() : String("(not a scene)")));
		return nullptr;
	}
	return rt->spawn_node(this, kind, MY::own_of(owner), props);
}

// Log in with an external account. Called from Client scripts (buttons, etc.).
// The result arrives as logged_in firing again. my.guest becomes false and my.name holds the account name
// Unmarked scripts also run on the Online side, so it silently does nothing when not a Client
void Online::login_with(const String &p_provider) {
	if (MVClient::current != nullptr) {
		MVClient::current->login_with(p_provider);
	}
}

// Unlink this device from the account. The local side stops, and the next launch is a new guest.
// An exit on shared devices so the next person does not end up in this account
void Online::logout() {
	if (MVClient::current != nullptr) {
		MVClient::current->logout();
	}
}

// Passing my or a Node that person owns both point to the same person
String MY::own_of(const Variant &p_owner) {
	Object *one = p_owner;
	if (MY *mine = Object::cast_to<MY>(one)) {
		return mine->get_who();
	}
	if (Node *node = Object::cast_to<Node>(one)) {
		return node->get_meta(SNAME("own"), String());
	}
	return String();
}

bool MVOnline::is_my(Node *p_node) const {
	return MVThing::is_my(p_node);
}

Node *MVOnline::spawn(Node *p_node, const Ref<PackedScene> &p_scene, const Variant &p_a, const Variant &p_b) const {
	return MVThing::spawn(p_node, p_scene, p_a, p_b);
}

// Whether it is an engine-native property. The Secret entry refuses only author contents and lets name etc. through for tree-walking scripts
bool MVOnline::native_property(Object *p_object, const StringName &p_name) const {
	return p_object != nullptr && ClassDB::has_property(p_object->get_class_name(), p_name);
}

// The script's world. Points to the world itself (Online) at any depth
Node *MVOnline::world(Node *p_node) const {
	MVRuntime *rt = MVThing::runtime(p_node);
	return rt != nullptr ? rt->server_node() : nullptr;
}

// Send a generated script's @online command to the Online side
Signal MVOnline::ask(Node *p_node, const String &p_name, const Array &p_args) const {
	return MVThing::ask(p_node, p_name, p_args);
}

// Call the real function from the Secret entry. The answer is received with await
Signal MVOnline::secret(Node *p_door, const String &p_name, const Array &p_args) const {
	return MVThing::secret(p_door, p_name, p_args);
}

void MVOnline::begin_local(Node *p_node) const {
	MVThing::begin_local(p_node);
}

void MVOnline::end_local(Node *p_node) const {
	MVThing::end_local(p_node);
}

bool MVOnline::in_local(Node *p_node) const {
	return MVThing::in_local(p_node);
}

void MVOnline::begin_latest(Node *p_node) const {
	MVThing::begin_latest(p_node);
}

void MVOnline::end_latest(Node *p_node) const {
	MVThing::end_latest(p_node);
}

// Return @online analysis marks to language tests
Dictionary MVOnline::fields(const String &p_path) const {
	return GDScriptOnline::fields_of(p_path);
}

// Return @online binding targets to language tests
Dictionary MVOnline::bindings(const String &p_path) const {
	return GDScriptOnline::bindings_of(p_path);
}

// Register internal entry points called only by generated scripts. No author-facing names are exposed
void Online::_bind_methods() {
	ClassDB::bind_method(D_METHOD("__begin_online_call"), &Online::begin_online_call);
	ClassDB::bind_method(D_METHOD("__end_online_call"), &Online::end_online_call);
	ClassDB::bind_method(D_METHOD("spawn", "scene", "owner", "properties"), &Online::spawn, DEFVAL(Variant()), DEFVAL(Variant()));
	ClassDB::bind_static_method("Online", D_METHOD("login_with", "provider"), &Online::login_with);
	ClassDB::bind_static_method("Online", D_METHOD("logout"), &Online::logout);
	// Authors connect these from the Online Node's inspector. Commands from the server gather here
	ADD_SIGNAL(MethodInfo("player_joining", PropertyInfo(Variant::OBJECT, "my", PROPERTY_HINT_RESOURCE_TYPE, "MY")));
	ADD_SIGNAL(MethodInfo("logged_in", PropertyInfo(Variant::OBJECT, "my", PROPERTY_HINT_RESOURCE_TYPE, "MY")));
	ADD_SIGNAL(MethodInfo("login_failed", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("spawned", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE, "Node")));
	ADD_SIGNAL(MethodInfo("player_left", PropertyInfo(Variant::OBJECT, "my", PROPERTY_HINT_RESOURCE_TYPE, "MY")));
}

// Register entry points of the internal singleton used only by generated scripts
void MVOnline::_bind_methods() {
	ClassDB::bind_method(D_METHOD("__my", "node"), &MVOnline::is_my);
	ClassDB::bind_method(D_METHOD("__spawn", "node", "scene", "a", "b"), &MVOnline::spawn);
	ClassDB::bind_method(D_METHOD("__native_property", "object", "name"), &MVOnline::native_property);
	ClassDB::bind_method(D_METHOD("__world", "node"), &MVOnline::world);
	ClassDB::bind_method(D_METHOD("__ask", "node", "name", "args"), &MVOnline::ask);
	ClassDB::bind_method(D_METHOD("__secret", "door", "name", "args"), &MVOnline::secret);
	ClassDB::bind_method(D_METHOD("__begin_local", "node"), &MVOnline::begin_local);
	ClassDB::bind_method(D_METHOD("__end_local", "node"), &MVOnline::end_local);
	ClassDB::bind_method(D_METHOD("__local", "node"), &MVOnline::in_local);
	ClassDB::bind_method(D_METHOD("__begin_latest", "node"), &MVOnline::begin_latest);
	ClassDB::bind_method(D_METHOD("__end_latest", "node"), &MVOnline::end_latest);
	ClassDB::bind_method(D_METHOD("__fields", "path"), &MVOnline::fields);
	ClassDB::bind_method(D_METHOD("__bindings", "path"), &MVOnline::bindings);
}

// Expose the user's context read-only. Not for authors to rewrite
void MY::_bind_methods() {
	ClassDB::bind_method(D_METHOD("__id"), &MY::get_id);
	ClassDB::bind_method(D_METHOD("__session"), &MY::get_session);
	ClassDB::bind_method(D_METHOD("__guest"), &MY::get_guest);
	ClassDB::bind_method(D_METHOD("__name"), &MY::get_display_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "id"), "", "__id");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "guest"), "", "__guest");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "", "__name");
}
