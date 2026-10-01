/**************************************************************************/
/*  mv_secret.cpp                                                         */
/**************************************************************************/

// Server-only Node type not delivered to Clients. Only the Online side holds its contents.

#include "mv_secret.h"

#include "mv_client_script.h"
#include "mv_gate.h"

#include "core/io/resource_loader.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/resources/packed_scene.h"

const char *Secret::KIND = "Secret";
const char *Secret::HOST_DEFAULT = "127.0.0.1";
const char *Secret::NAME_DEFAULT = "localhost";
const char *Secret::CA_DEFAULT = "res://online-ca.pem";

// This table is every setting authors touch. Adding one row shows it in the inspector, stores it in the scene,
// and makes it readable from config_of_project(). Empty means "not written"; the reader picks the default.
// client means "read by the Client". false ones are dropped from exports,
// so keys or storage passwords written here never reach players.
// room_max stays so rooms hosted by a player fill up at the same size
struct Entry {
	const char32_t *group; // Inspector heading. Same as the previous row means continued
	const char *prefix; // Prefix stripped from the name under the heading
	const char *name;
	bool client;
	Variant::Type type;
	PropertyHint hint;
	const char32_t *hint_text;
};
static const Entry SETTINGS[] = {
	{ U"", "", "room_max", true, Variant::INT, PROPERTY_HINT_RANGE, U"0,256,1" },
	{ U"", "", "player_scene", false, Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, U"PackedScene" },
	{ U"", "", "keep_sec", false, Variant::INT, PROPERTY_HINT_RANGE, U"0,600,1,suffix:s" },
	{ U"", "", "secret_name", true, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"localhost" },
	{ U"", "", "ca", true, Variant::STRING, PROPERTY_HINT_FILE, U"*.pem" },
	{ U"Display", "", "teleport_px", true, Variant::INT, PROPERTY_HINT_RANGE, U"0,100000,1,suffix:px" },
	{ U"Client Address", "room_", "room_host", true, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"127.0.0.1" },
	{ U"Client Address", "room_", "room_port", true, Variant::INT, PROPERTY_HINT_RANGE, U"0,65535,1" },
	{ U"Server Listen", "listen_", "listen_host", false, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"0.0.0.0" },
	{ U"Server Listen", "listen_", "listen_port", false, Variant::INT, PROPERTY_HINT_RANGE, U"0,65535,1" },
	{ U"Matchmaking", "match_", "match_url", true, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"" },
	{ U"Matchmaking", "match_", "match_key", false, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"" },
	{ U"Matchmaking", "match_", "match_room", false, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"" },
	{ U"Save", "redis_", "redis_host", false, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"" },
	{ U"Save", "redis_", "redis_port", false, Variant::INT, PROPERTY_HINT_RANGE, U"0,65535,1" },
	{ U"Save", "redis_", "redis_password", false, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"" },
	{ U"Save", "redis_", "redis_prefix", false, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"online" },
	{ U"Save", "", "connect_key", false, Variant::STRING, PROPERTY_HINT_PLACEHOLDER_TEXT, U"" },
	{ U"Accounts", "", "account_providers", false, Variant::DICTIONARY, PROPERTY_HINT_NONE, U"" }, // Server-only authorization endpoints and credentials.
};

// Drop settings the Client does not read. The shell goes into the PCK, so unless dropped here
// keys and storage passwords would reach the player's machine
void Secret::drop_server_settings() {
	for (const Entry &one : SETTINGS) {
		if (!one.client) {
			wrote.erase(StringName(one.name));
		}
	}
}

// Only names in the table are stored here. Others belong to the Node itself
static const Entry *entry_of(const StringName &p_name) {
	for (const Entry &one : SETTINGS) {
		if (p_name == StringName(one.name)) {
			return &one;
		}
	}
	return nullptr;
}

bool Secret::_set(const StringName &p_name, const Variant &p_value) {
	const Entry *one = entry_of(p_name);
	if (one == nullptr) {
		return false;
	}
	wrote[p_name] = p_value;
	return true;
}

bool Secret::_get(const StringName &p_name, Variant &r_ret) const {
	const Entry *one = entry_of(p_name);
	if (one == nullptr) {
		return false;
	}
	// Unwritten settings are the type's empty value. The reader picks the default
	Callable::CallError error;
	Variant blank;
	Variant::construct(one->type, blank, nullptr, 0, error);
	r_ret = wrote.get(p_name, blank);
	return true;
}

void Secret::_get_property_list(List<PropertyInfo> *p_list) const {
	String group;
	for (const Entry &one : SETTINGS) {
		if (String(one.group) != group) {
			group = one.group;
			p_list->push_back(PropertyInfo(Variant::NIL, group, PROPERTY_HINT_NONE, one.prefix, PROPERTY_USAGE_GROUP));
		}
		p_list->push_back(PropertyInfo(one.type, one.name, one.hint, one.hint_text));
	}
}

// Walk the world tree top-down, counting every Secret, and return the first
static Secret *first_secret(Node *p_node, int &r_found) {
	Secret *here = Object::cast_to<Secret>(p_node);
	if (here != nullptr) {
		r_found++;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Secret *deeper = first_secret(p_node->get_child(i), r_found);
		here = here != nullptr ? here : deeper;
	}
	return here;
}

// Return the one Secret that applies to the world. Later ones have no effect, so report once instead of silently picking
static Secret *only_secret(Node *p_world) {
	int found = 0;
	Secret *one = p_world != nullptr ? first_secret(p_world, found) : nullptr;
	if (found > 1) {
		static bool told = false;
		if (!told) {
			told = true;
			ERR_PRINT(vformat(U"Online: %d Secret nodes found. Only the first one's settings apply", found));
		}
	}
	return one;
}

// Read the settings of the Secret anywhere in the world. Default if none.
// Secret need not be a direct child, so the first found sets the world's player count
int Secret::room_max_of(Node *p_world) {
	Secret *one = only_secret(p_world);
	const int found = one != nullptr ? int(one->wrote.get("room_max", 0)) : 0;
	return found > 0 ? MIN(found, (int)ROOM_LIMIT) : (int)ROOM_DEFAULT;
}

// Scene placed once per joining person. If set, the engine spawns it before player_joining
Ref<PackedScene> Secret::player_scene_of(Node *p_world) {
	Secret *one = only_secret(p_world);
	return one != nullptr ? Ref<PackedScene>(one->wrote.get("player_scene", Variant())) : Ref<PackedScene>();
}

// Read how many seconds a disconnected person's Player is kept. Returning within it resumes play
int Secret::keep_sec_of(Node *p_world) {
	Secret *one = only_secret(p_world);
	const int found = one != nullptr ? int(one->wrote.get("keep_sec", 0)) : 0;
	return found > 0 ? MIN(found, (int)KEEP_LIMIT) : (int)KEEP_DEFAULT;
}

// The Client needs the destination before connecting, but no world exists yet.
// Read only the record without building the scene. No author script runs.
// Inspector and launch arguments share the spelling. Only transport turns `_` into `-`
Dictionary Secret::config_of_project() {
	Dictionary out;
	const String scene = MVClientScript::world_scene();
	if (!ResourceLoader::exists(scene, "PackedScene")) {
		return out;
	}
	Ref<PackedScene> packed = ResourceLoader::load(scene, "PackedScene");
	Ref<SceneState> state = packed.is_valid() ? packed->get_state() : Ref<SceneState>();
	if (state.is_null()) {
		return out;
	}
	// Checked by type, so a Secret with an inherited script attached is not missed.
	// The record holds only author-written settings, so copy them all
	for (int i = 0; i < state->get_node_count(); i++) {
		if (!ClassDB::is_parent_class(state->get_node_type(i), KIND)) {
			continue;
		}
		for (int p = 0; p < state->get_node_property_count(i); p++) {
			const String name = state->get_node_property_name(i, p);
			if (entry_of(name) != nullptr) {
				out[name.replace("_", "-")] = state->get_node_property_value(i, p);
			}
		}
		break;
	}
	// Settings the Client does not read can also come from environment variables ONLINE_<UPPERCASE NAME>.
	// Keys or passwords written in the scene stay in the repo. For authors who want to pass them from the deployment
	for (const Entry &one : SETTINGS) {
		if (one.client) {
			continue;
		}
		const String value = OS::get_singleton()->get_environment("ONLINE_" + String(one.name).to_upper());
		if (value.is_empty()) {
			continue;
		}
		if (one.type == Variant::DICTIONARY) {
			const Variant parsed = JSON::parse_string(value);
			if (parsed.get_type() == Variant::DICTIONARY) {
				out[String(one.name).replace("_", "-")] = parsed;
			} else {
				ERR_PRINT(vformat(U"Online: %s must contain a JSON object", one.name));
			}
		} else {
			out[String(one.name).replace("_", "-")] = one.type == Variant::INT ? Variant(value.to_int()) : Variant(value);
		}
	}
	return out;
}
