/**************************************************************************/
/*  mv_export.cpp                                                         */
/**************************************************************************/

// Strips Secret scripts from the PCK. An empty shell remains afterwards,
// so the scene's ext_resource still resolves and dependencies do not break.

#include "mv_export.h"

#ifdef TOOLS_ENABLED

#include "mv_client_script.h"
#include "mv_secret.h"
#include "mv_stage.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/io/resource_uid.h"
#include "core/object/script_language.h"
#include "core/version.h"
#include "scene/resources/packed_scene.h"

#include "modules/gdscript/gdscript_online.h"

// Only presets with the online_server marker may export with Secret scripts
const char *MVExport::SERVER_FEATURE = "online_server";
// Use the online_server suffix of the server template name as the marker, so authors need not add preset settings by hand.
// Only the template used for this export is checked. Checking the other one too
// would leak scripts in a release export by someone who set the server template as debug
bool MVExport::server_preset(const Ref<EditorExportPreset> &p_preset, bool p_debug) {
	if (p_preset.is_null()) {
		return false;
	}
	if (p_preset->get_custom_features().split(",", false).has(SERVER_FEATURE)) {
		return true;
	}
	const String key = p_debug ? "custom_template/debug" : "custom_template/release";
	// Check the file name, not the whole path, so an output path that happens to contain it does not match
	return p_preset->has(key) && String(p_preset->get(key)).get_file().contains(SERVER_FEATURE);
}

// Keep all private server state at the root of its persistent container volume.
String MVExport::container_userdata() {
	return "/online_data";
}

// Decide from the marker and template name whether this preset is a server-side export
void MVExport::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	debug_build = p_debug;
	export_path = p_path;
	keeps_secret = p_features.has(SERVER_FEATURE) || server_preset(get_export_preset(), p_debug);
	if (!keeps_secret) {
		find_secret_scripts();
	}
}

// Put a ready-to-run container definition next to the server export
void MVExport::_export_end() {
	if (keeps_secret) {
		write_container();
	}
}

// Port the Online side listens on. The setting exists only in the Secret inspector
int MVExport::listen_port() {
	const Dictionary wrote = Secret::config_of_project();
	const int listen = int(wrote.get("listen-port", 0));
	return listen > 0 ? listen : int(wrote.get("room-port", (int)Secret::PORT_DEFAULT));
}

// Dockerfile and compose.yaml, rebuilt on every export. The binary and PCK are COPYed under their output names,
// so the author just runs docker compose up -d --build in the output directory to run it locally with Redis
void MVExport::write_container() {
	const String dir = export_path.get_base_dir();
	const String name = export_path.get_file();
	const bool pack_only = name.get_extension().to_lower() == "pck" || name.get_extension().to_lower() == "zip";
	String binary = name;
	String pack = pack_only ? name : name.get_basename() + ".pck";
	if (pack_only) {
		// A PCK-only export has no binary. Copy the server template the preset points to next to it
		const Ref<EditorExportPreset> preset = get_export_preset();
		const String key = debug_build ? "custom_template/debug" : "custom_template/release";
		const String from = preset.is_valid() && preset->has(key) ? String(preset->get(key)) : String();
		binary = from.get_file();
		if (from.is_empty() || !FileAccess::exists(from)) {
			ERR_PRINT(vformat(U"Online: the server template %s is missing. The container cannot be built without it", from));
		} else if (from != dir.path_join(binary)) {
			Ref<DirAccess> d = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
			if (d.is_null() || d->copy(from, dir.path_join(binary), 0755) != OK) {
				ERR_PRINT(vformat(U"Online: cannot copy the server template next to %s", export_path));
			}
		}
	} else if (!FileAccess::exists(dir.path_join(pack))) {
		pack = String(); // Export with the PCK embedded in the binary. --main-pack is not needed
	}
	const int port = listen_port();
	String docker = String("# Online server, rewritten on every export. Start it with: docker compose up -d --build\n") +
					"FROM ubuntu:24.04\n"
					"WORKDIR /online\n"
					"COPY " + binary + (pack.is_empty() ? String() : " " + pack) + " ./\n"
					"ENV ONLINE_LISTEN_HOST=0.0.0.0\n"
					"EXPOSE " + itos(port) + "/tcp " + itos(port) + "/udp\n"
					"CMD [\"./" + binary + "\", \"--headless\"" + (pack.is_empty() ? String() : ", \"--main-pack\", \"" + pack + "\"") + ", \"--\", \"--online-server\"]\n";
	String compose = String("# Runs locally with Redis. Private data stays in the online_data volume.\n") +
					 "services:\n"
					 "  online:\n"
					 "    build: .\n"
					 "    ports:\n"
					 "      - \"" + itos(port) + ":" + itos(port) + "/tcp\"\n"
					 "      - \"" + itos(port) + ":" + itos(port) + "/udp\"\n"
					 "    environment:\n"
					 "      ONLINE_REDIS_HOST: redis\n"
					 "      GD_USER_HOME: " + container_userdata() + "\n" +
					 "    volumes:\n"
					 "      - \"online_data:" + container_userdata() + "\"\n"
					 "    depends_on:\n"
					 "      - redis\n"
					 "  redis:\n"
					 "    image: redis:7-alpine\n"
					 "    command: [\"redis-server\", \"--appendonly\", \"yes\"]\n"
					 "    volumes:\n"
					 "      - \"redis_data:/data\"\n"
					 "volumes:\n"
					 "  online_data:\n"
					 "  redis_data:\n";
	const String files[2][2] = { { "Dockerfile", docker }, { "compose.yaml", compose } };
	for (const String(&one)[2] : files) {
		Ref<FileAccess> f = FileAccess::open(dir.path_join(one[0]), FileAccess::WRITE);
		if (f.is_null()) {
			ERR_PRINT(vformat(U"Online: cannot write %s next to %s", one[0], export_path));
			return;
		}
		f->store_string(one[1]);
	}
	print_line(vformat(U"Online: wrote Dockerfile and compose.yaml to %s. Run: docker compose up -d --build", dir));
}

// Collect scripts used only under Secret.
// They are removed from the scene with their children, but the .gd file itself would still go into the export.
// Scripts also used outside Secret do not belong to Secret, so they are kept
void MVExport::collect(Node *p_node, bool p_inside, HashSet<String> &r_inside, HashSet<String> &r_outside) {
	const bool inside = p_inside || Object::cast_to<Secret>(p_node) != nullptr;
	const Ref<Script> attached = p_node->get_script();
	if (attached.is_valid() && !attached->get_path().is_empty()) {
		(inside ? r_inside : r_outside).insert(attached->get_path());
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect(p_node->get_child(i), inside, r_inside, r_outside);
	}
}

// Before exporting, scan every scene once to find the scripts under Secret
void MVExport::find_secret_scripts() {
	secret_scripts.clear();
	plain_scenes.clear();
	HashSet<String> inside;
	HashSet<String> outside;
	List<String> dirs;
	dirs.push_back("res://");
	while (!dirs.is_empty()) {
		const String dir = dirs.front()->get();
		dirs.pop_front();
		for (const String &name : ResourceLoader::list_directory(dir)) {
			const String path = dir.path_join(name.trim_suffix("/"));
			if (name.ends_with("/")) {
				if (!name.begins_with(".")) {
					dirs.push_back(path);
				}
				continue;
			}
			const String kind = path.get_extension().to_lower();
			if (kind != "tscn" && kind != "scn" && kind != "res") {
				continue;
			}
			Ref<PackedScene> packed = ResourceLoader::load(path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
			Node *root = packed.is_valid() ? packed->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE) : nullptr;
			if (root == nullptr) {
				continue; // Unreadable scenes are emptied by _export_file
			}
			collect(root, false, inside, outside);
			// This also tells whether the scene has a Secret. Scenes without one need not be reloaded by _export_file
			if (!MVStage::finds_secret(root)) {
				plain_scenes.insert(path);
			}
			memdelete(root);
		}
	}
	for (const String &one : inside) {
		if (!outside.has(one)) {
			secret_scripts.insert(one);
		}
	}
}

// Put the replaced contents into the PCK in place of the original file
void MVExport::replace(const String &p_path, const String &p_text) {
	replace(p_path, p_text.to_utf8_buffer());
}

void MVExport::replace(const String &p_path, const Vector<uint8_t> &p_bytes) {
	skip();
	add_file(p_path, p_bytes, false);
	replaced = true;
	print_line(vformat(U"Online: stripped Secret contents from %s", p_path));
}

// A scene that could not be stripped is not exported as-is. Broken is better than leaking contents.
// To avoid breaking silently, the reason is left both at export and at load
void MVExport::blank(const String &p_path, const String &p_why) {
	ERR_PRINT(vformat(U"Online: %s %s. Secret cannot be stripped, so only an empty placeholder is exported", p_path, p_why));
	replace(p_path, vformat(U"; Online: this scene %s. Secret could not be stripped, so its contents are not exported\n", p_why));
}

// The world scene is read as text at runtime. If export settings convert it to binary it cannot be read,
// so this hook, which owns Online, puts it in as text. The shared export code need not know the world folder
void MVExport::keep_text(const String &p_path) {
	const String kind = p_path.get_extension().to_lower();
	if ((kind != "tscn" && kind != "tres") || p_path != MVClientScript::world_scene()) {
		return;
	}
	const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(p_path);
	if (bytes.is_empty()) {
		return;
	}
	skip();
	add_file(p_path, bytes, false);
}

// Replaced files are already in as text. Only untouched ones are passed through as text
void MVExport::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	replaced = false;
	strip_secret(p_path, p_type);
	if (!replaced) {
		keep_text(p_path);
	}
}

// Except for server exports, keep Secret scripts and Secret contents in scenes out of the PCK
void MVExport::strip_secret(const String &p_path, const String &p_type) {
	if (keeps_secret) {
		return;
	}
	const String kind = p_path.get_extension().to_lower();
	// Scenes are detected by type, not extension, so .scn and .res are not missed
	if (kind != "gd" && p_type != "PackedScene") {
		return;
	}
	const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(p_path);
	if (bytes.is_empty()) {
		return;
	}
	const String source = String::utf8((const char *)bytes.ptr(), bytes.size());
	if (kind == "gd") {
		// Keep the type and path; only the contents are discarded, so references from scenes still resolve
		if (GDScriptOnline::secret_source(source, p_path)) {
			replace(p_path, String("extends ") + Secret::KIND + "\n");
			return;
		}
		// Scripts used only under Secret are treated like Secret contents.
		// They do not extend Secret, so only the type is kept and the contents are discarded
		if (secret_scripts.has(p_path)) {
			const String base = GDScriptOnline::extends_of(source);
			ERR_PRINT(vformat(U"Online: %s is only used under Secret, so its contents are not exported to clients. Place it outside Secret as well if clients need it", p_path));
			replace(p_path, "extends " + (base.is_empty() ? String("Node") : base) + "\n");
		}
		return;
	}
	// Scenes the pre-scan found without a Secret are not opened again.
	// Scenes the pre-scan did not see are opened and checked normally
	if (plain_scenes.has(p_path)) {
		return;
	}
	// Scenes are stripped by Node type, not text. Instanced, placed at the root,
	// built-in scripts or binary scenes are all caught by one check
	Ref<PackedScene> packed = ResourceLoader::load(p_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
	Node *root = packed.is_valid() ? packed->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE) : nullptr;
	if (root == nullptr) {
		// For an unreadable scene even the presence of a Secret is unknown. Break it rather than leak contents
		blank(p_path, U"cannot be loaded");
		return;
	}
	if (!MVStage::finds_secret(root)) {
		memdelete(root);
		return;
	}
	MVStage::hollow_secrets(root);
	Ref<PackedScene> out;
	out.instantiate();
	const Error packed_error = out->pack(root);
	memdelete(root);
	if (packed_error != OK) {
		blank(p_path, U"cannot be repacked");
		return;
	}
	// Write with the original extension. ResourceSaver chooses the format by extension
	const String dir = "user://online_export";
	DirAccess::make_dir_recursive_absolute(dir);
	const String target = dir.path_join(p_path.md5_text() + "." + kind);
	if (ResourceSaver::save(out, target) != OK) {
		blank(p_path, U"cannot be saved");
		return;
	}
	Vector<uint8_t> made = FileAccess::get_file_as_bytes(target);
	if (made.is_empty()) {
		blank(p_path, U"cannot be read back");
		return;
	}
	// Repacking drops the original uid. Left dropped, referencing resources warn every time, so write it back
	const ResourceUID::ID id = ResourceLoader::get_resource_uid(p_path);
	if (id != ResourceUID::INVALID_ID) {
		const String text = String::utf8((const char *)made.ptr(), made.size());
		const int head = text.find_char(']');
		if (text.begins_with("[gd_scene") && head > 0 && !text.substr(0, head).contains("uid=")) {
			made = (text.substr(0, head) + " uid=\"" + ResourceUID::get_singleton()->id_to_text(id) + "\"" + text.substr(head)).to_utf8_buffer();
		}
	}
	replace(p_path, made);
}

#endif // TOOLS_ENABLED
