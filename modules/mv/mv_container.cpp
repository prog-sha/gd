/**************************************************************************/
/*  mv_container.cpp                                                      */
/**************************************************************************/

// Editor menu actions that export the server and run it in a local docker container.

#include "mv_container.h"

#ifdef TOOLS_ENABLED

#include "mv_export.h"
#include "mv_secret.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/export/editor_export.h"
#include "editor/file_system/editor_file_system.h"

bool MVContainer::busy = false;

void MVContainer::install() {
	EditorNode::get_singleton()->add_tool_menu_item(TTR("Online: Start the local test container"), callable_mp_static(&MVContainer::start));
	EditorNode::get_singleton()->add_tool_menu_item(TTR("Online: Stop the local test container"), callable_mp_static(&MVContainer::stop));
}

String MVContainer::work_dir() {
	return ProjectSettings::get_singleton()->globalize_path("res://.godot/online_container");
}

// Name the local stack after the project path so different projects never share its data.
String MVContainer::project_name() {
	return "gd-online-" + ProjectSettings::get_singleton()->get_resource_path().md5_text().substr(0, 12);
}

// Run docker compose in the export directory. Callers use a worker thread so the editor does not block
bool MVContainer::compose(const char *p_verb, const String &p_flag) {
	List<String> args;
	args.push_back("compose");
	args.push_back("--project-name");
	args.push_back(project_name());
	args.push_back("--project-directory");
	args.push_back(work_dir());
	args.push_back(p_verb);
	if (!p_flag.is_empty()) {
		args.push_back(p_flag);
	}
	if (String(p_verb) == "up") {
		args.push_back("--build");
	}
	String output;
	int code = -1;
	const Error err = OS::get_singleton()->execute("docker", args, &output, &code, true);
	if (!output.strip_edges().is_empty()) {
		print_line(output.strip_edges());
	}
	if (err != OK) {
		ERR_PRINT(U"Online: cannot run docker. Install Docker and make sure `docker compose` works in a terminal");
		return false;
	}
	if (code != 0) {
		ERR_PRINT(vformat(U"Online: docker compose %s failed (%d)", String(p_verb), code));
		return false;
	}
	return true;
}

// Export the server preset and bring the container up. The export hook writes Dockerfile and compose.yaml next to it
void MVContainer::start() {
	if (busy) {
		print_line(U"Online: the container is still starting or stopping");
		return;
	}
	Ref<EditorExportPreset> preset;
	for (int i = 0; i < EditorExport::get_singleton()->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> one = EditorExport::get_singleton()->get_export_preset(i);
		if (MVExport::server_preset(one, false)) {
			preset = one;
			break;
		}
	}
	if (preset.is_null()) {
		ERR_PRINT(U"Online: no server export preset. Add a Linux preset whose release template is gd-godot built with extra_suffix=online_server");
		return;
	}
	const String dir = work_dir();
	Ref<DirAccess> d = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (d.is_null() || d->make_dir_recursive(dir) != OK) {
		ERR_PRINT(vformat(U"Online: cannot create %s", dir));
		return;
	}
	const String pack = dir.path_join("game.pck");
	if (preset->get_platform()->export_pack(preset, false, pack) != OK || !FileAccess::exists(dir.path_join("compose.yaml"))) {
		ERR_PRINT(vformat(U"Online: the server export to %s failed. See the errors above", pack));
		return;
	}
	busy = true;
	WorkerThreadPool::get_singleton()->add_native_task(run_start, nullptr, true, "Online container");
}

void MVContainer::run_start(void *) {
	const String dir = work_dir();
	if (compose("up", "-d")) {
		// Copy only the public certificate from the persistent server volume.
		const String made = dir.path_join("online_certificate.pem");
		bool copied = false;
		for (int i = 0; i < 100 && !copied; i++) {
			List<String> args;
			args.push_back("compose");
			args.push_back("--project-name");
			args.push_back(project_name());
			args.push_back("--project-directory");
			args.push_back(dir);
			args.push_back("cp");
			args.push_back("online:/online_data/online_secret/certificate.pem");
			args.push_back(made);
			String output;
			int code = -1;
			copied = OS::get_singleton()->execute("docker", args, &output, &code, true) == OK && code == 0;
			if (copied) {
				break;
			}
			OS::get_singleton()->delay_usec(100000);
		}
		const String ca = ProjectSettings::get_singleton()->globalize_path(Secret::config_of_project().get("ca", "res://online-ca.pem"));
		Ref<DirAccess> d = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (!copied || !FileAccess::exists(made)) {
			ERR_PRINT(vformat(U"Online: the container is up but its certificate could not be copied. Check `docker compose logs` in %s", dir));
		} else if (FileAccess::exists(ca)) {
			// Do not silently overwrite the author's certificate (it may belong to the production server)
			if (FileAccess::get_file_as_bytes(ca) == FileAccess::get_file_as_bytes(made)) {
				print_line(vformat(U"Online: the local test container is up on 127.0.0.1:%d. %s already is its certificate", MVExport::listen_port(), ca));
			} else {
				print_line(vformat(U"Online: the local test container is up on 127.0.0.1:%d. %s is kept as it is. To connect to the container, replace it with %s", MVExport::listen_port(), ca, made));
			}
		} else if (d.is_null() || d->copy(made, ca) != OK) {
			ERR_PRINT(vformat(U"Online: cannot copy the container's certificate to %s", ca));
		} else {
			print_line(vformat(U"Online: the local test container is up on 127.0.0.1:%d. Its certificate is now %s, so Run Project connects to it", MVExport::listen_port(), ca));
			callable_mp(EditorFileSystem::get_singleton(), &EditorFileSystem::scan).call_deferred();
		}
	}
	busy = false;
}

void MVContainer::stop() {
	if (busy) {
		print_line(U"Online: the container is still starting or stopping");
		return;
	}
	if (!FileAccess::exists(work_dir().path_join("compose.yaml"))) {
		print_line(U"Online: no local test container has been started from this project");
		return;
	}
	busy = true;
	WorkerThreadPool::get_singleton()->add_native_task(run_stop, nullptr, true, "Online container");
}

void MVContainer::run_stop(void *) {
	if (compose("down", String())) {
		print_line(U"Online: the local test container is stopped");
	}
	busy = false;
}

#endif // TOOLS_ENABLED
