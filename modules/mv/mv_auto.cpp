/**************************************************************************/
/*  mv_auto.cpp                                                           */
/**************************************************************************/

// Decides from @online annotation marks, without author settings, whether networking is needed.

#include "mv_auto.h"

#include "mv_client.h"
#include "mv_client_script.h"
#ifdef MV_SECRET_ENABLED
#include "mv_authority.h"
#endif

#include "core/config/engine.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/os/os.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#include "modules/gdscript/gdscript_online.h"

// Skip hidden folders and folders holding .gdignore, as the engine does
static bool ignored_folder(const String &p_path) {
	return p_path.get_file().begins_with(".") || FileAccess::exists(p_path.path_join(".gdignore"));
}

// Register the internal detector, which has no public methods
void MVAuto::_bind_methods() {
}

// Decide whether a loaded script has any @online mark
bool MVAuto::script_uses_online(const String &p_path) {
	Error error = OK;
	Ref<Resource> script = ResourceLoader::load(p_path, "Script", ResourceFormatLoader::CACHE_MODE_REUSE, &error);
	if (error != OK || script.is_null()) {
		return false;
	}
	Dictionary marks = GDScriptOnline::fields_of(p_path);
	if (marks.is_empty() && script->get_path() != p_path) {
		marks = GDScriptOnline::fields_of(script->get_path());
	}
	for (const Variant &name : marks.get_key_list()) {
		if (int(marks[name]) & GDScriptOnline::F_ONLINE) {
			return true;
		}
	}
	return false;
}

// Walk the folder tree and stop at the first @online mark
bool MVAuto::dir_uses_online(const String &p_root, const String &p_skip) {
	Vector<String> dirs;
	dirs.push_back(p_root == "res://" ? p_root : p_root.trim_suffix("/"));
	while (!dirs.is_empty()) {
		const String dir = dirs[dirs.size() - 1];
		dirs.resize(dirs.size() - 1);
		for (const String &name : ResourceLoader::list_directory(dir)) {
			const bool folder = name.ends_with("/");
			const String path = dir.path_join(folder ? name.trim_suffix("/") : name);
			if (folder) {
				if (path != p_skip && !ignored_folder(path)) {
					dirs.push_back(path);
				}
			} else if ((name.ends_with(".gd") || name.ends_with(".gdc")) && script_uses_online(path)) {
				return true;
			}
		}
	}
	return false;
}

// Find @online marks wherever they are written in the project
bool MVAuto::project_uses_online() {
	return dir_uses_online("res://", String());
}

// Launch arguments. The same flag counts before or after "--"
static bool flagged(const String &p_flag) {
	return OS::get_singleton()->get_cmdline_args().find(p_flag) != nullptr ||
			OS::get_singleton()->get_cmdline_user_args().find(p_flag) != nullptr;
}

// Decide on the first frame after scene load and start the Client only when needed
void MVAuto::_notification(int p_what) {
	if (p_what != NOTIFICATION_PROCESS) {
		return;
	}
	set_process(false);
	if (project_uses_online()) {
#ifdef MV_SECRET_ENABLED
		// --online-secret holds no world. It handles only identity and Secret bodies;
		// the host chosen by matchmaking runs the world
		const bool secret_only = flagged("--online-secret");
		if (secret_only || flagged("--online-server")) {
			MVAuthority *authority = memnew(MVAuthority);
			authority->set_name("_OnlineServer");
			get_tree()->get_root()->add_child(authority, false, Node::INTERNAL_MODE_BACK);
			const String reason = authority->start(secret_only);
			if (!reason.is_empty()) {
				ERR_PRINT(String(U"Online Server: ") + reason);
				authority->queue_free();
			}
			queue_free();
			return;
		}
#else
		// Silently running as a Client after the flag was given would make the author think a server started
		if (flagged("--online-secret") || flagged("--online-server")) {
			ERR_PRINT(U"Online: this is a client build. --online-server and --online-secret need the editor or the server build");
		}
#endif
		MVClient *client = memnew(MVClient);
		client->set_name("_OnlineEntry");
		get_tree()->get_root()->add_child(client, false, Node::INTERNAL_MODE_BACK);
	}
	queue_free();
}

// Add only the lightweight one-time check, except in the editor and script runs
void MVAuto::install(SceneTree *p_tree) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const List<String> args = OS::get_singleton()->get_cmdline_args();
	if (args.find("--script") != nullptr || args.find("-s") != nullptr || args.find("--test") != nullptr) {
		return;
	}
	MVAuto *detector = memnew(MVAuto);
	detector->set_name("_OnlineDetect");
	detector->set_process(true);
	p_tree->get_root()->add_child(detector, false, Node::INTERNAL_MODE_BACK);
}
