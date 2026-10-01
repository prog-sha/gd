/**************************************************************************/
/*  mv_secret.h                                                           */
/**************************************************************************/

// Server-only Node placed under Online that is never sent to Clients.
// Authors write script here and can keep per-person persistent values with @online_save.
// In the Client binary this type exists only as an empty shell, with no script or child Nodes.
// The shell keeps only settings the Client needs to connect. Listen, keys and storage settings are dropped.
// All Online settings live here. There is no settings file.

#pragma once

#include "core/variant/dictionary.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

class Secret : public Node {
	GDCLASS(Secret, Node);

	Dictionary wrote; // Settings written in the inspector. Only written ones are stored

protected:
	static void _bind_methods() {}
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

public:
	enum {
		ROOM_LIMIT = 256, // Max players per room
		ROOM_DEFAULT = 8, // Player count for worlds that do not set one
		KEEP_LIMIT = 600, // Max seconds to wait for a reconnect
		KEEP_DEFAULT = 10, // Default seconds a disconnected player can return within
		PORT_DEFAULT = 4433, // Default port for listening and connecting
		TELEPORT_DEFAULT = 200, // A single move beyond this many px jumps instead of smoothing
	};

	static const char *KIND; // Type name referring to this type inside the world
	static const char *HOST_DEFAULT; // Connect target when nothing is written
	static const char *NAME_DEFAULT; // Server name when nothing is written
	static const char *CA_DEFAULT; // Certificate path when nothing is written

	void copy_settings(const Secret *p_from) { wrote = p_from != nullptr ? p_from->wrote.duplicate() : Dictionary(); }
	void drop_server_settings(); // Drop settings the Client does not read

	static int room_max_of(Node *p_world); // Read the player count from the Secret under the world
	static int keep_sec_of(Node *p_world); // Read the reconnect wait seconds from the Secret under the world
	static Ref<PackedScene> player_scene_of(Node *p_world); // Scene placed once per joining person. Empty if none
	static Dictionary config_of_project(); // Read settings from the scene record before building the tree
};
