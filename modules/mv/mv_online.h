/**************************************************************************/
/*  mv_online.h                                                           */
/**************************************************************************/

// Fixed Node representing the world itself. Authors extend it to write the world script.
// Another instance of the same type becomes the internal singleton used only by generated scripts.

#pragma once

#include "core/object/ref_counted.h"
#include "scene/resources/packed_scene.h"
#include "scene/main/node.h"

// Context of one logged-in person. Authors receive it only under the name my.
// Not placed in the tree. When deciding an owner, my is passed as-is
class MY : public RefCounted {
	GDCLASS(MY, RefCounted);

	String who; // Internal name for this person in the world
	Dictionary context; // Context authenticated by the Secret

protected:
	static void _bind_methods();

public:
	void set_context(const String &p_who, const Dictionary &p_context) {
		who = p_who;
		context = p_context;
	}
	String get_who() const { return who; }
	String get_id() const { return context.get("id", String()); }
	String get_session() const { return context.get("session", String()); }
	bool get_guest() const { return context.get("guest", false); }
	String get_display_name() const { return context.get("name", String()); }
	static String own_of(const Variant &p_owner); // Resolve the owner to one person from either my or a Node they own
};

class Online : public Node {
	GDCLASS(Online, Node);

public:
	static const char *KIND; // Type name of the world itself

private:

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// Tools authors use in the world script
	Node *spawn(const Ref<PackedScene> &p_scene, const Variant &p_a = Variant(), const Variant &p_b = Variant()); // Place one thing in the world. Like marks, trailing args are read by type, not order. A dictionary is initial values; my or an owned thing makes it that person's
	static void login_with(const String &p_provider); // Log in on the Client with an external account provider
	static void logout(); // Unlink this device from the account

	PackedStringArray get_configuration_warnings() const override; // Gently tell in the editor what is missing

	void begin_online_call(); // Open the permission boundary from @online into world methods
	void end_online_call(); // Close the permission boundary from @online into world methods
};

// Internal singleton used only by generated scripts. Registered as __Online, a name authors cannot write.
// A separate type from the Online(Node) authors extend, so it does not clash with generated function names
class MVOnline : public Object {
	GDCLASS(MVOnline, Object);

protected:
	static void _bind_methods();

public:
	bool is_my(Node *p_node) const; // Check ownership from generated scripts
	Node *spawn(Node *p_node, const Ref<PackedScene> &p_scene, const Variant &p_a, const Variant &p_b) const; // Place one thing in the world from generated scripts
	bool native_property(Object *p_object, const StringName &p_name) const; // Whether it is an engine-native property (such as name). The Secret entry lets these through silently
	Node *world(Node *p_node) const; // The world (Online) the thing lives in. The script's world resolves here
	Signal ask(Node *p_node, const String &p_name, const Array &p_args) const; // Send a generated-script command and make the answer awaitable
	Signal secret(Node *p_door, const String &p_name, const Array &p_args) const; // Call the Secret's real body
	void begin_local(Node *p_node) const; // On the Online side, open a section of script shared by everyone
	void end_local(Node *p_node) const; // On the Online side, close a section of script shared by everyone
	bool in_local(Node *p_node) const; // On the Online side, whether inside script shared by everyone
	void begin_latest(Node *p_node) const; // Open the local prediction update range
	void end_latest(Node *p_node) const; // Close the local prediction update range
	Dictionary fields(const String &p_path) const; // Return annotation marks to language tests
	Dictionary bindings(const String &p_path) const; // Return binding targets to language tests
};
