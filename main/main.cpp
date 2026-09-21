/**************************************************************************/
/*  main.cpp                                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// Initialize the runtime and interpret arguments; subcommands are implemented in cli/main/cmd.
// Parse flags, establish mounts, then load the entry script.

#include "main.h"

#include "cli/api/reg.h"
#include "cli/data/bytes.h"
#include "cli/main/cmd.h"
#include "cli/main/help.h"
#include "cli/net/http.h"
#include "cli/run/entry.h"
#include "cli/run/loop.h"
#include "cli/run/guard.h"
#include "cli/sys/jail.h"
#include "cli/sys/mount.h"
#include "cli/sys/perm.h"
#include "cli/sys/pkgscope.h"
#include "cli/sys/pool.h"
#include "cli/sys/task.h"
#include "cli/sys/wait.h"
#include "cli/tool/compile.h"
#include "cli/tool/fmt.h"
#include "cli/tool/pkg.gen.h"
#include "cli/tool/pkgmap.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/core_globals.h"
#include "core/crypto/crypto.h"
#include "core/debugger/engine_debugger.h"
#include "core/extension/extension_api_dump.h"
#include "core/extension/gdextension_manager.h"
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/io/file_access.h"
#include "core/io/file_access_pack.h"
#include "core/io/file_access_zip.h"
#include "core/io/image_loader.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/object/message_queue.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/profiling/profiling.h"
#include "core/register_core_types.h"
#include "core/string/translation_server.h"
#include "core/variant/variant_parser.h"
#include "core/version.h"
#include "drivers/register_driver_types.h"
#include "main/main_timer_sync.h"
#include "main/performance.h"
#include "scene/main/scene_tree.h"
#include "scene/property_list_helper.h"
#include "scene/register_scene_types.h"
#include "scene/resources/packed_scene.h"
#include "servers/register_server_types.h"

#include "modules/register_module_types.h"
#include "platform/register_platform_apis.h"

#include <climits>

// 2D

#ifndef PHYSICS_2D_DISABLED
#include "servers/physics_2d/physics_server_2d.h"
#include "servers/physics_2d/physics_server_2d_dummy.h"
#endif // PHYSICS_2D_DISABLED

// 3D

#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/physics_3d/physics_server_3d_dummy.h"
#endif // PHYSICS_3D_DISABLED

#if defined(STEAMAPI_ENABLED)
#include "main/steam_tracker.h"
#endif

#include "modules/modules_enabled.gen.h" // For mono.

#if defined(MODULE_MONO_ENABLED) && defined(TOOLS_ENABLED)
#include "modules/mono/editor/bindings_generator.h"
#endif

#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_function.h"
#endif // MODULE_GDSCRIPT_ENABLED

/* Static members */

// Singletons

// Initialized in setup()
static Engine *engine = nullptr;
static ProjectSettings *globals = nullptr;
static Input *input = nullptr;
static InputMap *input_map = nullptr;
static TranslationServer *translation_server = nullptr;
static Performance *performance = nullptr;
static PackedData *packed_data = nullptr;
#ifdef MINIZIP_ENABLED
static ZipArchive *zip_packed_data = nullptr;
#endif
static MessageQueue *message_queue = nullptr;

#if defined(STEAMAPI_ENABLED)
static SteamTracker *steam_tracker = nullptr;
#endif

// Initialized in setup2()
#ifndef PHYSICS_2D_DISABLED
static PhysicsServer2DManager *physics_server_2d_manager = nullptr;
static PhysicsServer2D *physics_server_2d = nullptr;
#endif // PHYSICS_2D_DISABLED
#ifndef PHYSICS_3D_DISABLED
static PhysicsServer3DManager *physics_server_3d_manager = nullptr;
static PhysicsServer3D *physics_server_3d = nullptr;
#endif // PHYSICS_3D_DISABLED
// We error out if setup2() doesn't turn this true
static bool _start_success = false;
static bool show_help = false;
static bool dump_extension_api = false; // Write extension_api.json after ClassDB is complete.

static Object *gd_entry = nullptr; // Entry object for the running script.

// Keep display configuration in the optional game layer.

// Debug

static int frame_delay = 0;
bool profile_gpu = false;

/* Helper methods */

// Decode command-line escapes into argument values.
static String unescape_cmdline(const String &p_str) {
	return p_str.replace("%20", " ");
}

// Return the complete runtime version string.
static String get_full_version_string() {
	String hash = String(GODOT_VERSION_HASH);
	if (!hash.is_empty()) {
		hash = "." + hash.left(9);
	}
	return String(GODOT_VERSION_FULL_BUILD) + hash;
}

// Display the runtime version together with its engine compatibility baseline.
static String get_cli_version_string() {
	return String(GD_CLI_VERSION) + " (Godot " + get_full_version_string() + ")";
}

// FIXME: Could maybe be moved to have less code in main.cpp.
// Initialize the empty physics servers.
void initialize_physics() {
#ifndef PHYSICS_3D_DISABLED
	/// 3D Physics Server
	physics_server_3d = PhysicsServer3DManager::get_singleton()->new_server(
			GLOBAL_GET(PhysicsServer3DManager::setting_property_name));
	if (!physics_server_3d) {
		// Physics server not found, Use the default physics
		physics_server_3d = PhysicsServer3DManager::get_singleton()->new_default_server();
	}

	// Fall back to dummy if no default server has been registered.
	if (!physics_server_3d) {
		// Use the default dummy physics server silently in headless builds.
		physics_server_3d = memnew(PhysicsServer3DDummy);
	}

	// Should be impossible, but make sure it's not null.
	ERR_FAIL_NULL_MSG(physics_server_3d, "Failed to initialize PhysicsServer3D.");
	physics_server_3d->init();
#endif // PHYSICS_3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
	// 2D Physics server
	physics_server_2d = PhysicsServer2DManager::get_singleton()->new_server(
			GLOBAL_GET(PhysicsServer2DManager::get_singleton()->setting_property_name));
	if (!physics_server_2d) {
		// Physics server not found, Use the default physics
		physics_server_2d = PhysicsServer2DManager::get_singleton()->new_default_server();
	}

	// Fall back to dummy if no default server has been registered.
	if (!physics_server_2d) {
		// Use the default dummy physics server silently in headless builds.
		physics_server_2d = memnew(PhysicsServer2DDummy);
	}

	// Should be impossible, but make sure it's not null.
	ERR_FAIL_NULL_MSG(physics_server_2d, "Failed to initialize PhysicsServer2D.");
	physics_server_2d->init();
#endif // PHYSICS_2D_DISABLED
}

// Shut down the physics servers.
void finalize_physics() {
#ifndef PHYSICS_3D_DISABLED
	physics_server_3d->finish();
	memdelete(physics_server_3d);
#endif // PHYSICS_3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
	physics_server_2d->finish();
	memdelete(physics_server_2d);
#endif // PHYSICS_2D_DISABLED
}

// Keep display and visual features in the optional game layer.
void finalize_display() {}

//#define DEBUG_INIT
#ifdef DEBUG_INIT
#define MAIN_PRINT(m_txt) print_line(m_txt)
#else
#define MAIN_PRINT(m_txt)
#endif

// Display startup version information.
void Main::print_header() {
	String head = String(GODOT_VERSION_NAME) + " v" + get_cli_version_string();
	if (GODOT_VERSION_TIMESTAMP > 0) {
		head += " (" + Time::get_singleton()->get_datetime_string_from_unix_time(GODOT_VERSION_TIMESTAMP, true) + " UTC)";
	}
	Engine::get_singleton()->print_header(head + " - " + String(GODOT_VERSION_WEBSITE));
}

// Initialize in three stages, called in order by each platform's main().
//
// setup() parses flags, prepares core singletons and types, and establishes mounts.
// It continues into setup2() when p_second_phase is true.
// setup2() registers higher-level types and modules, then starts the scripting language.
// start() loads the entry script and creates the main loop; subcommands execute and finish here.

Error Main::setup(const char *execpath, int argc, char *argv[], bool p_second_phase) {
	GodotProfileZone("setup");
	Thread::make_main_thread();
	set_current_thread_safe_for_nodes(true);

	OS::get_singleton()->initialize();

#ifdef WINDOWS_ENABLED
	IdleWait::init(); // Construct the platform reactor before worker-backed services can publish completions.
#endif

	CoreGlobals::print_ready = true;

	// Benchmark tracking must be done after `OS::get_singleton()->initialize()` as on some
	// platforms, it's used to set up the time utilities.
	OS::get_singleton()->benchmark_begin_measure("Startup", "Main::Setup");

	engine = memnew(Engine);

	MAIN_PRINT("Main: Initialize CORE");

	register_core_types();
	register_core_driver_types();

	MAIN_PRINT("Main: Initialize Globals");

	input_map = memnew(InputMap);
	globals = memnew(ProjectSettings);

	register_core_settings(); //here globals are present

	translation_server = memnew(TranslationServer);
	performance = memnew(Performance);
	GDREGISTER_CLASS(Performance);
	engine->add_singleton(Engine::Singleton("Performance", performance));

	// Only flush stdout in debug builds by default, as spamming `print()` will
	// decrease performance if this is enabled.
	GLOBAL_DEF_RST("application/run/flush_stdout_on_print", false);
	GLOBAL_DEF_RST("application/run/flush_stdout_on_print.debug", true);

	MAIN_PRINT("Main: Parse CMDLine");

	/* argument parsing and main creation */
	List<String> args;
	List<String> main_args;
	List<String> user_args;
	bool adding_user_args = false;
	List<String> platform_args = OS::get_singleton()->get_cmdline_platform_args();

	// Add command line arguments.
	for (int i = 0; i < argc; i++) {
		args.push_back(String::utf8(argv[i]));
	}

	// Add arguments received from macOS LaunchService (URL schemas, file associations).
	for (const String &arg : platform_args) {
		args.push_back(arg);
	}

	List<String>::Element *I = args.front();

	while (I) {
		I->get() = unescape_cmdline(I->get().strip_edges());
		I = I->next();
	}

	String project_path = "."; // Resource root at the invocation directory.
	bool quiet_stdout = false;
	int separate_thread_render = -1; // Tri-state: -1 = not set, 0 = false, 1 = true.
	bool load_shell_env = false;

	packed_data = PackedData::get_singleton();
	if (!packed_data) {
		packed_data = memnew(PackedData);
	}

#ifdef MINIZIP_ENABLED

	//XXX: always get_singleton() == 0x0
	zip_packed_data = ZipArchive::get_singleton();
	//TODO: remove this temporary fix
	if (!zip_packed_data) {
		zip_packed_data = memnew(ZipArchive);
	}

	packed_data->add_pack_source(zip_packed_data);
#endif

	// Exit error code used in the `goto error` conditions.
	// It's returned as the program exit code. ERR_HELP is special cased and handled as success (0).
	Error exit_err = ERR_INVALID_PARAMETER;

	// Keep original arguments for restart, removing only the restarting feature's own flag.
	for (const String &arg : args) {
		Cmd::raw.push_back(arg);
	}
	I = args.front();
	while (I) {
		List<String>::Element *N = I->next();

		const String &arg = I->get();

#ifdef MACOS_ENABLED
		// Ignore the process serial number argument passed by macOS Gatekeeper.
		// Otherwise, the first start would try to open a nonexistent project and abort.
		if (arg.begins_with("-psn_")) {
			I = N;
			continue;
		}
#endif

		// Reject unknown flags before they reach the engine argument parser.
		// Otherwise undocumented flags could remain active outside --help.
		// Leave arguments after -- or ++ untouched for the script.
		if (!adding_user_args && arg.begins_with("-") && !Cmd::takes_flag(arg)) {
			// Suggest a supported spelling when an unrecognized flag has a known alternative.
			const String advice = Perm::flag_advice(arg);
			if (!advice.is_empty()) {
				OS::get_singleton()->printerr("%s\n", advice.utf8().get_data());
			} else {
				OS::get_singleton()->printerr("Unknown option: %s\nRun with --help to see what gd takes.\n", arg.utf8().get_data());
			}
			goto error;
		}

		if (adding_user_args) {
			Cmd::args.push_back(arg);
			user_args.push_back(arg);
		} else if (arg == "-h" || arg == "--help") { // display help

			show_help = true;
			exit_err = ERR_HELP; // Hack to force an early exit in `main()` with a success code.
			goto error;

		} else if (arg == "--version") {
			print_line(get_cli_version_string());
			exit_err = ERR_HELP; // Hack to force an early exit in `main()` with a success code.
			goto error;

		} else if (arg == "--dump-extension-api") {
			dump_extension_api = true; // Need registered classes; dump after setup2.

		} else if (arg == "-v" || arg == "--verbose") { // verbose output

			OS::get_singleton()->_verbose_stdout = true;
		} else if (arg == "-q" || arg == "--quiet") { // quieter output

			quiet_stdout = true;

		} else if (arg == "--no-header") {
			Engine::get_singleton()->_print_header = false;

		} else if (arg == "--watch") {
			Cmd::watch = true; // Watch for changes and restart execution.
		} else if (arg == "--no-scene-tree") {
			RunGuard::no_tree = true; // Detect tree creation during both script loading and execution.
			Cmd::flags.push_back(arg);
		} else if (arg.begins_with("--workers=")) {
			// Start a listener group, using the core count for auto.
			const String v = arg.substr(10);
			if (v == "auto") {
				Cmd::workers = OS::get_singleton()->get_processor_count();
			} else {
				const int64_t count = v.to_int();
				if (!v.is_valid_int() || count < 1 || count > INT_MAX) {
					OS::get_singleton()->printerr("--workers needs a positive integer or auto.\n");
					goto error;
				}
				Cmd::workers = int(count);
			}
		} else if (arg == "--output" || arg == "-o") {
			// Read the compile output filename from -o.
			// Do not let the option become a script argument and silently leave the default output name.
			if (N) {
				Cmd::output = N->get();
				N = N->next();
			}
		} else if (arg == "--header") {
			Engine::get_singleton()->_print_header = true; // Enable version-header display.
		} else if (arg == "--strict") {
			// Enable stricter file, I/O, and Variant checks together.
			Mount::set_strict();
			Perm::set_strict();
			Cmd::strict = true;
			Cmd::flags.push_back(arg);
		} else if (arg == "--mount" && N) {
			// Parse mounts as name=path:access, accepting a separate option value.
			// Reject malformed specifications rather than running without the intended mount.
			const String one = "--mount=" + String(N->get());
			if (Mount::parse_flag(one) == Mount::BAD) {
				goto error;
			}
			Cmd::flags.push_back(one);
			N = N->next();
		} else if (arg.begins_with("--mount")) {
			if (Mount::parse_flag(arg) == Mount::BAD) {
				goto error;
			}
			Cmd::flags.push_back(arg);
		} else if (Perm::parse_flag(arg)) {
			// Preserve permission flags for child processes.
			Cmd::flags.push_back(arg);
		} else if (!Cmd::pkg.is_empty() && (arg == "--latest" || arg == "--frozen" || arg == "--cached-only" || arg == "--dry-run")) {
			Cmd::pkg_args.push_back(arg); // Collect package-command options.
		} else if (arg == "--" || arg == "++") {
			adding_user_args = true;
		} else if (!arg.begins_with("-") && Cmd::name.is_empty() && Cmd::script.is_empty() && Cmd::is_subcommand(arg)) {
			// Use the first positional argument as a subcommand when its name matches.
			Cmd::name = arg;
			if (Cmd::is_pkg_cmd(arg)) {
				Cmd::pkg = arg; // Dispatch package commands through the embedded script.
			}
		} else if (!arg.begins_with("-") && !Cmd::pkg.is_empty()) {
			Cmd::pkg_args.push_back(arg); // Pass package arguments unchanged to the embedded script.
		} else if (!arg.begins_with("-") && Cmd::name == "eval" && Cmd::eval_src.is_empty()) {
			Cmd::eval_src = arg;
		} else if (!arg.begins_with("-") && Cmd::script.is_empty() && Cmd::name != "eval" && !GDCompile::has_embedded()) {
			// Use the positional script path as the execution target.
			// For embedded packages the entry is fixed, so pass every argument to that entry.
			Cmd::script = arg;
		} else if (!arg.begins_with("-") && (!Cmd::script.is_empty() || GDCompile::has_embedded())) {
			Cmd::args.push_back(arg); // Collect script arguments.
		} else {
			// Unknown flags were rejected at the start of the loop; remaining values are
			// recognized flags handled later, such as --check, or positional arguments.
			main_args.push_back(arg);
		}

		I = N;
	}

	if (!Cmd::pkg.is_empty()) {
		Mount::set_pkg_mode(); // Allow package-storage writes only for the package command.
	}
	// Establish mounts after flags and before configuration loading.
	// Both res:// and user:// must resolve against these roots before settings are read.
	if (!Mount::setup()) {
		goto error;
	}
	// Allow a script entry without a project configuration file.
	globals->setup(project_path, String(), false, false);

	// Initialize WorkerThreadPool.
	{
#ifdef THREADS_ENABLED
		{
			int worker_threads = GLOBAL_GET("threading/worker_pool/max_threads");
			float low_priority_ratio = GLOBAL_GET("threading/worker_pool/low_priority_thread_ratio");
			WorkerThreadPool::get_singleton()->init(worker_threads, low_priority_ratio);
		}
#else
		WorkerThreadPool::get_singleton()->init(0, 0);
#endif
	}

	OS::get_singleton()->set_cmdline(execpath, main_args, user_args);

	Engine::get_singleton()->set_physics_ticks_per_second(GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "physics/common/physics_ticks_per_second", PROPERTY_HINT_RANGE, "1,1000,1"), 60));
	Engine::get_singleton()->set_max_physics_steps_per_frame(GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "physics/common/max_physics_steps_per_frame", PROPERTY_HINT_RANGE, "1,100,1"), 8));
	Engine::get_singleton()->set_physics_jitter_fix(GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "physics/common/physics_jitter_fix", PROPERTY_HINT_RANGE, "0,2,0.001,or_greater"), 0.5));
	Engine::get_singleton()->set_max_fps(GLOBAL_DEF(PropertyInfo(Variant::INT, "application/run/max_fps", PROPERTY_HINT_RANGE, "0,1000,1"), 0));

	// Initialize user data dir.
	OS::get_singleton()->ensure_user_data_dir();

	OS::get_singleton()->set_low_processor_usage_mode(GLOBAL_DEF("application/run/low_processor_mode", false));
	OS::get_singleton()->set_low_processor_usage_mode_sleep_usec(
			GLOBAL_DEF(PropertyInfo(Variant::INT, "application/run/low_processor_mode_sleep_usec", PROPERTY_HINT_RANGE, "0,33200,1,or_greater"), 6900)); // Roughly 144 FPS

	OS::get_singleton()->set_delta_smoothing(GLOBAL_DEF("application/run/delta_smoothing", true));

	GLOBAL_DEF("debug/settings/stdout/print_fps", false);
	GLOBAL_DEF("debug/settings/stdout/print_gpu_profile", false);
	GLOBAL_DEF("debug/settings/stdout/verbose_stdout", false);
	GLOBAL_DEF("debug/settings/physics_interpolation/enable_warnings", true);
	if (!OS::get_singleton()->_verbose_stdout) { // Not manually overridden.
		OS::get_singleton()->_verbose_stdout = GLOBAL_GET("debug/settings/stdout/verbose_stdout");
	}

	register_early_core_singletons();
	initialize_modules(MODULE_INITIALIZATION_LEVEL_CORE);
	// Choose warning severity before reading the script.
	Cmd::apply_types();

	// Resolve pkg:// before any script or extension list is read, fetching what gd.json names but
	// the machine lacks. Package commands manage the same files themselves.
	if (Cmd::pkg.is_empty() && Cmd::name != "init" && Cmd::name != "doc" && Cmd::name != "completions" &&
			!(Cmd::script.is_empty() && Cmd::eval_src.is_empty() && Cmd::name.is_empty() && GDCompile::embedded_entry().is_empty()) &&
			!GDPkgMap::prepare()) {
		goto error;
	}

	// Materialize embedded native extensions as files before passing them to the extension loader.
	ERR_FAIL_COND_V_MSG(GDCompile::prepare_extensions() != OK, ERR_CANT_OPEN, "Cannot prepare embedded extensions.");
	{
		const Perm::Trusted trust; // Treat embedded extensions as executable components.
		register_core_extensions(); // core extensions must be registered after globals setup and before display
	}

	{
		ResourceUID::get_singleton()->enable_reverse_cache();
	}
	ResourceUID::get_singleton()->load_from_cache(true); // Load UUIDs from cache.
	ProjectSettings::get_singleton()->fix_autoload_paths(); // Handles autoloads saved as UID.

	GLOBAL_DEF(PropertyInfo(Variant::INT, "network/limits/debugger/max_chars_per_second", PROPERTY_HINT_RANGE, "256,4096,1,or_greater"), 32768);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "network/limits/debugger/max_queued_messages", PROPERTY_HINT_RANGE, "128,8192,1,or_greater"), 2048);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "network/limits/debugger/max_errors_per_second", PROPERTY_HINT_RANGE, "1,200,1,or_greater"), 400);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "network/limits/debugger/max_warnings_per_second", PROPERTY_HINT_RANGE, "1,200,1,or_greater"), 400);

	EngineDebugger::initialize(String(), false, false, Vector<String>(), []() {});

	GLOBAL_DEF("debug/file_logging/enable_file_logging", false);
	// Only file logging by default on desktop platforms as logs can't be
	// accessed easily on mobile/Web platforms (if at all).
	GLOBAL_DEF("debug/file_logging/enable_file_logging.pc", true);
	GLOBAL_DEF("debug/file_logging/log_path", "user://logs/godot.log");
	GLOBAL_DEF(PropertyInfo(Variant::INT, "debug/file_logging/max_log_files", PROPERTY_HINT_RANGE, "0,20,1,or_greater"), 5);

	if (FileAccess::get_create_func(FileAccess::ACCESS_USERDATA) && GLOBAL_GET("debug/file_logging/enable_file_logging")) {
		const String base_path = GLOBAL_GET("debug/file_logging/log_path");
		const int max_files = GLOBAL_GET("debug/file_logging/max_log_files");
		OS::get_singleton()->add_logger(memnew(RotatedFileLogger(base_path, max_files)));
	}

	// Print usage and exit when no execution target is selected.
	// A project.godot file alone does not select a scene entry or keep this process waiting.
	if (!dump_extension_api && Cmd::script.is_empty() && Cmd::eval_src.is_empty() && Cmd::name.is_empty() && GDCompile::embedded_entry().is_empty()) {
		Help::show(execpath);
		goto error;
	}

	input_map->load_from_project_settings();

	if (bool(GLOBAL_GET("application/run/disable_stdout"))) {
		quiet_stdout = true;
	}
	if (bool(GLOBAL_GET("application/run/disable_stderr"))) {
		CoreGlobals::print_error_enabled = false;
	}
	if (!bool(GLOBAL_GET("application/run/print_header"))) {
		// --no-header option for project settings.
		Engine::get_singleton()->_print_header = false;
	}

	if (quiet_stdout) {
		CoreGlobals::print_line_enabled = false;
	}

	Logger::set_flush_stdout_on_print(GLOBAL_GET("application/run/flush_stdout_on_print"));

	// Keep rendering-driver configuration in the optional game layer.

	// Keep renderer selection and window positioning in the optional game layer.
	OS::get_singleton()->_allow_hidpi = GLOBAL_DEF("display/window/dpi/allow_hidpi", true);
	OS::get_singleton()->_allow_layered = GLOBAL_DEF_RST("display/window/per_pixel_transparency/allowed", false);

	load_shell_env = GLOBAL_DEF("application/run/load_shell_environment", false);

	if (load_shell_env) {
		OS::get_singleton()->load_shell_environment();
	}

	if (separate_thread_render == -1) {
		separate_thread_render = (int)GLOBAL_DEF("rendering/driver/threads/thread_model", OS::RENDER_THREAD_SAFE) == OS::RENDER_SEPARATE_THREAD;
	}

#if !defined(THREADS_ENABLED)
	separate_thread_render = 0;
#endif
	OS::get_singleton()->_separate_thread_render = separate_thread_render;

	// Keep display and audio driver selection in the optional game layer.
	if (frame_delay == 0) {
		frame_delay = GLOBAL_DEF(PropertyInfo(Variant::INT, "application/run/frame_delay_msec", PROPERTY_HINT_RANGE, "0,100,1,or_greater"), 0);
		if (Engine::get_singleton()->is_editor_hint()) {
			frame_delay = 0;
		}
	}

	GLOBAL_DEF("display/window/ios/allow_high_refresh_rate", true);
	GLOBAL_DEF("display/window/ios/hide_home_indicator", true);
	GLOBAL_DEF("display/window/ios/hide_status_bar", true);
	GLOBAL_DEF("display/window/ios/suppress_ui_gesture", true);

#ifndef _3D_DISABLED
	// XR project settings.
	GLOBAL_DEF_RST_BASIC("xr/openxr/enabled", false);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "xr/openxr/target_api_version"), "");
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::STRING, "xr/openxr/default_action_map", PROPERTY_HINT_FILE, "*.tres"), "res://openxr_action_map.tres");
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/form_factor", PROPERTY_HINT_ENUM, "Head Mounted,Handheld"), "0");
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/view_configuration", PROPERTY_HINT_ENUM, "Mono,Stereo"), "1"); // "Mono,Stereo,Quad,Observer"
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/reference_space", PROPERTY_HINT_ENUM, "Local,Stage,Local Floor"), "1");
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/environment_blend_mode", PROPERTY_HINT_ENUM, "Opaque,Additive,Alpha"), "0");
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/foveation_level", PROPERTY_HINT_ENUM, "Off,Low,Medium,High"), "0");
	GLOBAL_DEF_BASIC("xr/openxr/foveation_dynamic", false);
	GLOBAL_DEF_BASIC("xr/openxr/foveation_eye_tracked", true);
	GLOBAL_DEF_BASIC("xr/openxr/foveation_with_subsampled_images", true);

	GLOBAL_DEF_BASIC("xr/openxr/submit_depth_buffer", false);
	GLOBAL_DEF_BASIC("xr/openxr/startup_alert", true);

	// OpenXR project extensions settings.
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/extensions/debug_utils", PROPERTY_HINT_ENUM, "Disabled,Error,Warning,Info,Verbose"), "0");
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/extensions/debug_message_types", PROPERTY_HINT_FLAGS, "General,Validation,Performance,Conformance"), "15");
	GLOBAL_DEF_BASIC("xr/openxr/extensions/frame_synthesis", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/hand_tracking", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/hand_tracking_unobstructed_data_source", false); // XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT
	GLOBAL_DEF_BASIC("xr/openxr/extensions/hand_tracking_controller_data_source", false); // XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT
	GLOBAL_DEF_RST_BASIC("xr/openxr/extensions/hand_interaction_profile", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/spatial_entity/enabled", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/spatial_entity/enable_spatial_anchors", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/spatial_entity/enable_persistent_anchors", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/spatial_entity/enable_builtin_anchor_detection", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/spatial_entity/enable_plane_tracking", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/spatial_entity/enable_builtin_plane_detection", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/spatial_entity/enable_marker_tracking", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/spatial_entity/enable_builtin_marker_tracking", false);
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/extensions/spatial_entity/aruco_dict", PROPERTY_HINT_ENUM, "4x4 50 IDs,4x4 100 IDs,4x4 250 IDs,4x4 1000 IDs,5x5 50 IDs,5x5 100 IDs,5x5 250 IDs,5x5 1000 IDs,6x6 50 IDs,6x6 100 IDs,6x6 250 IDs,6x6 1000 IDs,7x7 50 IDs,7x7 100 IDs,7x7 250 IDs,7x7 1000 IDs"), "15");
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "xr/openxr/extensions/spatial_entity/april_tag_dict", PROPERTY_HINT_ENUM, "4x4H5,5x5H9,6x6H10,6x6H11"), "3");
	GLOBAL_DEF_RST_BASIC("xr/openxr/extensions/eye_gaze_interaction", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/render_model", false);
	GLOBAL_DEF_BASIC("xr/openxr/extensions/user_presence", false);

	// OpenXR Binding modifier settings
	GLOBAL_DEF_BASIC("xr/openxr/binding_modifiers/analog_threshold", false);
	GLOBAL_DEF_RST_BASIC("xr/openxr/binding_modifiers/dpad_binding", false);

#endif // _3D_DISABLED

	Engine::get_singleton()->set_frame_delay(frame_delay);

	message_queue = memnew(MessageQueue);

	OS::get_singleton()->benchmark_end_measure("Startup", "Main::Setup");

	if (p_second_phase) {
		exit_err = setup2();
		if (exit_err != OK) {
			goto error;
		}
	}

	return OK;

error:

	Engine::get_singleton()->set_write_movie_path(String());
	project_path = "";

	args.clear();
	main_args.clear();

	if (show_help) {
		Help::show(execpath);
	}

	EngineDebugger::deinitialize();

	if (performance) {
		memdelete(performance);
	}
	if (input_map) {
		memdelete(input_map);
	}
	if (translation_server) {
		memdelete(translation_server);
	}
	if (globals) {
		memdelete(globals);
	}
	if (packed_data) {
		memdelete(packed_data);
	}

	unregister_core_driver_types();
	unregister_core_extensions();

	unregister_cli_types(); // Unregister global runtime services while Engine is still alive.
	if (engine) {
		memdelete(engine);
	}

	unregister_core_types();

	OS::get_singleton()->_cmdline.clear();
	OS::get_singleton()->_user_args.clear();

	if (message_queue) {
		memdelete(message_queue);
	}

	OS::get_singleton()->benchmark_end_measure("Startup", "Main::Setup");

#if defined(STEAMAPI_ENABLED)
	if (steam_tracker) {
		memdelete(steam_tracker);
	}
#endif

	OS::get_singleton()->finalize_core();

	Thread::release_main_thread();

	return exit_err;
}

// Parse a resource path into an empty validation resource.
Error _parse_resource_dummy(void *p_data, VariantParser::Stream *p_stream, Ref<Resource> &r_res, int &line, String &r_err_str) {
	VariantParser::Token token;
	VariantParser::get_token(p_stream, token, line, r_err_str);
	if (token.type != VariantParser::TK_NUMBER && token.type != VariantParser::TK_STRING) {
		r_err_str = "Expected number (old style sub-resource index) or String (ext-resource ID)";
		return ERR_PARSE_ERROR;
	}

	r_res.unref();

	VariantParser::get_token(p_stream, token, line, r_err_str);
	if (token.type != VariantParser::TK_PARENTHESIS_CLOSE) {
		r_err_str = "Expected ')'";
		return ERR_PARSE_ERROR;
	}

	return OK;
}

// Finish initialization before starting the main loop.
Error Main::setup2(bool p_show_boot_logo) {
	GodotProfileZone("setup2");
	OS::get_singleton()->benchmark_begin_measure("Startup", "Main::Setup2");

	Thread::make_main_thread(); // Make whatever thread call this the main thread.
	set_current_thread_safe_for_nodes(true);

	// Don't use rich formatting to prevent ANSI escape codes from being written to log files.
	print_header();

	OS::get_singleton()->benchmark_begin_measure("Startup", "Servers");

	// Keep text layout in the optional game layer.

#ifndef PHYSICS_3D_DISABLED
	physics_server_3d_manager = memnew(PhysicsServer3DManager);
#endif // PHYSICS_3D_DISABLED
#ifndef PHYSICS_2D_DISABLED
	physics_server_2d_manager = memnew(PhysicsServer2DManager);
#endif // PHYSICS_2D_DISABLED

	// Keep navigation servers in the optional game layer.

	register_server_types();
	{
		OS::get_singleton()->benchmark_begin_measure("Servers", "Modules and Extensions");

		initialize_modules(MODULE_INITIALIZATION_LEVEL_SERVERS);
		GDExtensionManager::get_singleton()->initialize_extensions(GDExtension::INITIALIZATION_LEVEL_SERVERS);

		OS::get_singleton()->benchmark_end_measure("Servers", "Modules and Extensions");
	}

	/* Initialize Input */

	{
		OS::get_singleton()->benchmark_begin_measure("Servers", "Input");

		input = memnew(Input);
		OS::get_singleton()->initialize_joypads();

		OS::get_singleton()->benchmark_end_measure("Servers", "Input");
	}

	// Keep display, input, rendering, audio, and XR initialization in the optional game layer.
	register_core_singletons();
	// Keep window, splash, and text-layout initialization in the optional game layer.

	MAIN_PRINT("Main: Load Scene Types");

	OS::get_singleton()->benchmark_begin_measure("Startup", "Scene");

	// Keep ThemeDB and navigation in the optional game layer.
	register_scene_types();
	register_driver_types();

	register_scene_singletons();

	{
		OS::get_singleton()->benchmark_begin_measure("Scene", "Modules and Extensions");

		initialize_modules(MODULE_INITIALIZATION_LEVEL_SCENE);
		GDExtensionManager::get_singleton()->initialize_extensions(GDExtension::INITIALIZATION_LEVEL_SCENE);

		OS::get_singleton()->benchmark_end_measure("Scene", "Modules and Extensions");

		// A package extension may register only what its manifest declared, since that is what the registry reserved.
		String why;
		ERR_FAIL_COND_V_MSG(!PkgScope::verify_declared(why), ERR_INVALID_DATA, why);
	}

	PackedStringArray extensions;
	extensions.push_back("gd");
	if (ClassDB::class_exists("CSharpScript")) {
		extensions.push_back("cs");
	}
	extensions.push_back("gdshader");
	GLOBAL_DEF_NOVAL(PropertyInfo(Variant::PACKED_STRING_ARRAY, "editor/script/search_in_file_extensions"), extensions); // Note: should be defined after Scene level modules init to see .NET.

	OS::get_singleton()->benchmark_end_measure("Startup", "Scene");

	MAIN_PRINT("Main: Load Platforms");

	OS::get_singleton()->benchmark_begin_measure("Startup", "Platforms");

	register_platform_apis();

	OS::get_singleton()->benchmark_end_measure("Startup", "Platforms");

	// Keep cursor support in the optional game layer.

	OS::get_singleton()->benchmark_begin_measure("Startup", "Finalize Setup");

	// Keep camera support in the optional game layer.

	MAIN_PRINT("Main: Load Physics");

	initialize_physics();

	register_server_singletons();

	// This loads global classes, so it must happen before custom loaders and savers are registered
	ScriptServer::init_languages();

	// Keep visual and audio features in the optional game layer.

#if defined(MODULE_MONO_ENABLED) && defined(TOOLS_ENABLED)
	// Hacky to have it here, but we don't have good facility yet to let modules
	// register command line options to call at the right time. This needs to happen
	// after init'ing the ScriptServer, but also after init'ing the ThemeDB,
	// for the C# docs generation in the bindings.
	List<String> cmdline_args = OS::get_singleton()->get_cmdline_args();
	BindingsGenerator::handle_cmdline_args(cmdline_args);
#endif

	// Keep shader variables in the optional game layer.

	OS::get_singleton()->benchmark_end_measure("Startup", "Finalize Setup");

	_start_success = true;

	ClassDB::set_current_api(ClassDB::API_NONE); //no more APIs are registered at this point

	print_verbose("CORE API HASH: " + uitos(ClassDB::get_api_hash(ClassDB::API_CORE)));
	print_verbose("EDITOR API HASH: " + uitos(ClassDB::get_api_hash(ClassDB::API_EDITOR)));
	MAIN_PRINT("Main: Done");

	OS::get_singleton()->benchmark_end_measure("Startup", "Main::Setup2");

	return OK;
}

// Keep splash rendering in the optional game layer.
void Main::setup_boot_logo() {}

// everything the main loop needs to know about frame timings
static MainTimerSync main_timer_sync;

// Return value should be EXIT_SUCCESS if we start successfully
// and should move on to `OS::run`, and EXIT_FAILURE otherwise for
// an early exit with that error code.
// Start the selected command or script.
int Main::start() {
	GodotProfileZone("start");
	if (dump_extension_api) {
		GDExtensionAPIDump::generate_extension_json_file("extension_api.json");
		return FileAccess::exists("extension_api.json") ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	// Install mount-restricted file access before scripts can execute.
	// Retain only script and extension resource loaders.
	// Earlier reads belong to startup and are not reachable from scripts.
	install_jail();
	trim_loaders();
	OS::get_singleton()->benchmark_begin_measure("Startup", "Main::Start");

	ERR_FAIL_COND_V(!_start_success, EXIT_FAILURE);

	String script;
	String main_loop_type;
	bool check_only = false;

	main_timer_sync.init(OS::get_singleton()->get_ticks_usec());

	MainLoop *main_loop = nullptr;
	if (main_loop_type.is_empty()) {
		main_loop_type = GLOBAL_GET("application/run/main_loop_type");
	}

	// Spawn a listener group by restarting this executable for each child.
	// Distribute request handling across processes and cores using a shared port,
	// letting the kernel assign incoming connections.
	if (Cmd::workers > 1 && Cmd::watch) {
		ERR_PRINT("--watch and --workers cannot be combined");
		return EXIT_FAILURE;
	}
	if (Cmd::workers > 1 && OS::get_singleton()->get_environment("GD_WORKER").is_empty()) {
		return Cmd::workers_loop(Cmd::child_args("--workers="), Cmd::workers);
	}

	// Run watch mode by restarting this executable after changes.
	if (Cmd::watch) {
		const String cfg = Cmd::find_config();
		return Cmd::watch_loop(Cmd::child_args("--watch"), cfg.is_empty() ? String(".") : cfg.get_base_dir());
	}

	// Apply the selected positional arguments and subcommand.
	if (script.is_empty() && !Cmd::script.is_empty()) {
		script = Cmd::script;
	}
	// Run the packaged entry when an archive is embedded in the executable.
	if (script.is_empty() && (Cmd::name.is_empty() || Cmd::name == "serve") && Cmd::eval_src.is_empty()) {
		script = GDCompile::embedded_entry();
	}
	// Show warnings during checks, not ordinary execution.
	if (Cmd::name == "check") {
		GDScript::show_warnings = true;
	}
	if (Cmd::name == "serve") {
		Cmd::serve = true; // Keep serving even when no work remains.
		GDEntry::serve_mode = true;
	}
	// Share the listener port when running as a grouped child.
	if (!OS::get_singleton()->get_environment("GD_WORKER").is_empty()) {
		GDWebServer::share_port = true;
	}
	if (Cmd::name == "init") {
		return Cmd::init_project(Cmd::script);
	}
	if (Cmd::name == "doc") {
		return Cmd::doc(Cmd::script);
	}
	if (Cmd::name == "repl") {
		return Cmd::repl();
	}
	if (Cmd::name == "bench") {
		return Cmd::run_bench(Cmd::script, Cmd::flags, 5);
	}
	if (Cmd::name == "completions") {
		return Cmd::completions(Cmd::script);
	}
	if (Cmd::name == "compile") {
		return GDCompile::run(Cmd::script, Cmd::output);
	}
	if (Cmd::name == "fmt") {
		// Report formatting differences without writing when --check is set.
		bool check_mode = false;
		for (const String &a : OS::get_singleton()->get_cmdline_args()) {
			if (a == "--check") {
				check_mode = true;
			}
		}
		return GDFmt::run(Cmd::script, check_mode);
	}
	if (Cmd::name == "info") {
		return Cmd::info();
	}
	if (Cmd::name == "task") {
		return Cmd::run_task(Cmd::script);
	}
	if (Cmd::name == "check") {
		// Scan recursively when given a directory or no explicit target.
		if (Cmd::script.is_empty() || !Cmd::script.ends_with(".gd") || DirAccess::open(Cmd::script).is_valid()) {
			return Cmd::run_checks(Cmd::script, Cmd::flags);
		}
		check_only = true;
	}
	if (Cmd::name == "test") {
		return Cmd::run_tests(Cmd::script, Cmd::flags); // Pass permission flags unchanged to test children.
	}

	// Track execution failures so an aborted script cannot exit successfully.
	GDFail::watch();

	// Run package commands through the embedded script, including the subcommand in its arguments.
	if (!Cmd::pkg.is_empty()) {
		Ref<Script> pkg_res = Cmd::script_from_source(String::utf8((const char *)gd_pkg_script, sizeof(gd_pkg_script)));
		ERR_FAIL_COND_V_MSG(pkg_res.is_null(), EXIT_FAILURE, "GDScript language is not available.");
		if (!pkg_res->is_valid()) {
			return EXIT_FAILURE;
		}
		List<String> pkg_argv;
		pkg_argv.push_back(Cmd::pkg);
		for (const String &a : Cmd::pkg_args) {
			pkg_argv.push_back(a);
		}
		SceneTree *pkg_tree = memnew(SceneTree);
		GDEntry *pkg_entry = memnew(GDEntry);
		if (pkg_entry->setup(pkg_res, pkg_argv) != OK) {
			memdelete(pkg_entry);
			memdelete(pkg_tree);
			return EXIT_FAILURE;
		}
		pkg_tree->connect(SNAME("process_frame"), callable_mp(pkg_entry, &GDEntry::start), Object::CONNECT_ONE_SHOT);
		gd_entry = pkg_entry;
		main_loop = pkg_tree;
	}

	// Build and execute an inline script for eval.
	if (!main_loop && !Cmd::eval_src.is_empty()) {
		Ref<Script> eval_res = Cmd::script_from_source(Cmd::wrap_eval(Cmd::eval_src));
		ERR_FAIL_COND_V_MSG(eval_res.is_null(), EXIT_FAILURE, "GDScript language is not available.");
		if (!eval_res->is_valid()) {
			return EXIT_FAILURE;
		}
		SceneTree *eval_tree = memnew(SceneTree);
		GDEntry *eval_entry = memnew(GDEntry);
		if (eval_entry->setup(eval_res, List<String>()) != OK) {
			memdelete(eval_entry);
			memdelete(eval_tree);
			return EXIT_FAILURE;
		}
		eval_tree->connect(SNAME("process_frame"), callable_mp(eval_entry, &GDEntry::start), Object::CONNECT_ONE_SHOT);
		gd_entry = eval_entry;
		main_loop = eval_tree;
	}

	if (!main_loop && !script.is_empty()) {
		// Report a missing input early and concisely.
		if (!script.begins_with("res://") && !FileAccess::exists(script)) {
			print_error(vformat("\nerror: no such script: %s\n", script));
			return EXIT_FAILURE;
		}

		// Check without loading so static variable initializers cannot execute
		// during a command intended only to validate syntax and types.
		if (check_only) {
			Error open_err = OK;
			const String src = FileAccess::get_file_as_string(script, &open_err);
			if (open_err != OK) {
				print_error(vformat("\nerror: cannot read: %s\n", script));
				return EXIT_FAILURE;
			}
			// Analyze under the same res:// identity that imports of this script resolve to, and report the path as typed.
			const bool local = script.is_relative_path() && !script.simplify_path().begins_with("..");
			return GDScript::check_source(local ? "res://" + script.simplify_path() : script, src, script) ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		Ref<Script> script_res = ResourceLoader::load(script);
		if (script_res.is_null()) {
			// Avoid repeating the load failure already reported.
			return EXIT_FAILURE;
		}

		if (script_res->can_instantiate()) {
			StringName instance_type = script_res->get_instance_base_type();
			if (Cmd::serve && ClassDB::is_parent_class(instance_type, SNAME("MainLoop"))) {
				ERR_PRINT("serve uses the GD runtime; run SceneTree/MainLoop scripts without serve.");
				return EXIT_FAILURE;
			}
			Object *obj = ClassDB::instantiate(instance_type);
			MainLoop *script_loop = Object::cast_to<MainLoop>(obj);
			if (!script_loop) {
				// Use SceneTree for ordinary execution and the independent runtime for serve.
				if (obj) {
					memdelete(obj);
				}
				SceneTree *tree = Cmd::serve ? nullptr : memnew(SceneTree);
				GDLoop *loop = Cmd::serve ? memnew(GDLoop) : nullptr;
				GDEntry *entry = memnew(GDEntry);
				if (entry->setup(script_res, Cmd::args) != OK) {
					memdelete(entry);
					if (tree) {
						memdelete(tree);
					}
					if (loop) {
						memdelete(loop);
					}
					return EXIT_FAILURE;
				}
				if (loop) {
					Async::post(Ref<RefCounted>(), callable_mp(entry, &GDEntry::start));
				} else {
					tree->connect(SNAME("process_frame"), callable_mp(entry, &GDEntry::start), Object::CONNECT_ONE_SHOT);
				}
				gd_entry = entry;
				main_loop = loop ? static_cast<MainLoop *>(loop) : tree;
			} else {
				script_loop->set_script(script_res);
				main_loop = script_loop;
			}
		} else {
			return EXIT_FAILURE;
		}
	} else { // Not based on script path.
		if (!ClassDB::class_exists(main_loop_type) && ScriptServer::is_global_class(main_loop_type)) {
			String script_path = ScriptServer::get_global_class_path(main_loop_type);
			Ref<Script> script_res = ResourceLoader::load(script_path);
			if (script_res.is_null()) {
				OS::get_singleton()->alert("Error: Could not load MainLoop script type: " + main_loop_type);
				ERR_FAIL_V_MSG(EXIT_FAILURE, vformat("Could not load global class %s.", main_loop_type));
			}
			StringName script_base = script_res->get_instance_base_type();
			Object *obj = ClassDB::instantiate(script_base);
			MainLoop *script_loop = Object::cast_to<MainLoop>(obj);
			if (!script_loop) {
				if (obj) {
					memdelete(obj);
				}
				OS::get_singleton()->alert("Error: Invalid MainLoop script base type: " + script_base);
				ERR_FAIL_V_MSG(EXIT_FAILURE, vformat("The global class %s does not inherit from SceneTree or MainLoop.", main_loop_type));
			}
			script_loop->set_script(script_res);
			main_loop = script_loop;
		}
	}

	if (!main_loop && main_loop_type.is_empty()) {
		main_loop_type = "SceneTree";
	}

	if (!main_loop) {
		if (!ClassDB::class_exists(main_loop_type)) {
			OS::get_singleton()->alert("Error: MainLoop type doesn't exist: " + main_loop_type);
			return EXIT_FAILURE;
		} else {
			Object *ml = ClassDB::instantiate(main_loop_type);
			ERR_FAIL_NULL_V_MSG(ml, EXIT_FAILURE, "Can't instance MainLoop type.");

			main_loop = Object::cast_to<MainLoop>(ml);
			if (!main_loop) {
				memdelete(ml);
				ERR_FAIL_V_MSG(EXIT_FAILURE, "Invalid MainLoop type.");
			}
		}
	}

	OS::get_singleton()->set_main_loop(main_loop);
	// Initialize resource extensions and TLS trust roots independently of SceneTree.
	ResourceLoader::add_custom_loaders();
	ResourceSaver::add_custom_savers();
	{
		const Perm::Trusted trust; // Use machine-default certificates rather than a script-selected path.
		Crypto::load_default_certificates(GLOBAL_GET("network/tls/certificate_bundle_override"));
	}

	SceneTree *sml = Object::cast_to<SceneTree>(main_loop);
	if (sml) {
		// Keep child-window embedding in the optional game layer.

		{ // game
			if (!script.is_empty()) {
				//autoload
				OS::get_singleton()->benchmark_begin_measure("Startup", "Load Autoloads");
				HashMap<StringName, ProjectSettings::AutoloadInfo> autoloads(ProjectSettings::get_singleton()->get_autoload_list());

				//first pass, add the constants so they exist before any script is loaded
				for (const KeyValue<StringName, ProjectSettings::AutoloadInfo> &E : autoloads) {
					const ProjectSettings::AutoloadInfo &info = E.value;

					if (info.is_singleton) {
						for (int i = 0; i < ScriptServer::get_language_count(); i++) {
							ScriptServer::get_language(i)->add_global_constant(info.name, Variant());
						}
					}
				}

				//second pass, load into global constants
				List<Node *> to_add;
				for (const KeyValue<StringName, ProjectSettings::AutoloadInfo> &E : autoloads) {
					const ProjectSettings::AutoloadInfo &info = E.value;

					Node *n = nullptr;
					if (ResourceLoader::get_resource_type(info.path) == "PackedScene") {
						// Cache the scene reference before loading it (for cyclic references)
						Ref<PackedScene> scn;
						scn.instantiate();
						scn->set_path(ResourceUID::ensure_path(info.path));
						scn->reload_from_file();
						ERR_CONTINUE_MSG(scn.is_null(), vformat("Failed to instantiate an autoload, can't load from path: %s.", info.path));

						if (scn.is_valid()) {
							n = scn->instantiate();
						}
					} else {
						Ref<Resource> res = ResourceLoader::load(info.path);
						ERR_CONTINUE_MSG(res.is_null(), vformat("Failed to instantiate an autoload, can't load from path: %s.", info.path));

						Ref<Script> script_res = res;
						if (script_res.is_valid()) {
							StringName ibt = script_res->get_instance_base_type();
							bool valid_type = ClassDB::is_parent_class(ibt, "Node");
							ERR_CONTINUE_MSG(!valid_type, vformat("Failed to instantiate an autoload, script '%s' does not inherit from 'Node'.", info.path));

							Object *obj = ClassDB::instantiate(ibt);
							ERR_CONTINUE_MSG(!obj, vformat("Failed to instantiate an autoload, cannot instantiate '%s'.", ibt));

							n = Object::cast_to<Node>(obj);
							n->set_script(script_res);
						}
					}

					ERR_CONTINUE_MSG(!n, vformat("Failed to instantiate an autoload, path is not pointing to a scene or a script: %s.", info.path));
					n->set_name(info.name);

					//defer so references are all valid on _ready()
					to_add.push_back(n);

					if (info.is_singleton) {
						for (int i = 0; i < ScriptServer::get_language_count(); i++) {
							ScriptServer::get_language(i)->add_global_constant(info.name, n);
						}
					}
				}

				for (Node *E : to_add) {
					sml->get_root()->add_child(E);
				}
				OS::get_singleton()->benchmark_end_measure("Startup", "Load Autoloads");
			}
		}

		sml->set_auto_accept_quit(GLOBAL_GET("application/config/auto_accept_quit"));
		sml->set_quit_on_go_back(GLOBAL_GET("application/config/quit_on_go_back"));

		// Keep display stretching in the optional game layer.

	}

	// Keep video export in the optional game layer.

	GDExtensionManager::get_singleton()->startup();

	// Reset the time baseline immediately before the main loop starts.
	// Exclude startup loading and parsing time from the first iteration's delta
	// so newly created timers cannot expire immediately from startup latency.
	main_timer_sync.init(OS::get_singleton()->get_ticks_usec());
	last_ticks = OS::get_singleton()->get_ticks_usec();

	OS::get_singleton()->benchmark_end_measure("Startup", "Main::Start");
	OS::get_singleton()->benchmark_dump();

	return EXIT_SUCCESS;
}

/* Main iteration
 *
 * This is the iteration of the engine's game loop, advancing the state of physics,
 * rendering and audio.
 * It's called directly by the platform's OS::run method, where the loop is created
 * and monitored.
 *
 * The OS implementation can impact its draw step with the Main::force_redraw() method.
 */

uint64_t Main::last_ticks = 0;
uint32_t Main::frames = 0;
uint32_t Main::hide_print_fps_attempts = 3;
uint32_t Main::frame = 0;
int Main::iterating = 0;

// Return whether a main-loop iteration is active.
bool Main::is_iterating() {
	return iterating > 0;
}

// For performance metrics.
static uint64_t physics_process_max = 0;
static uint64_t process_max = 0;
static uint64_t navigation_process_max = 0;

// Return false means iterating further, returning true means `OS::run`
// will terminate the program. In case of failure, the OS exit code needs
// to be set explicitly here (defaults to EXIT_SUCCESS).
// Advance the main loop once.
bool Main::iteration() {
	GodotProfileZone("Main::iteration");
	if (GDLoop *loop = Object::cast_to<GDLoop>(OS::get_singleton()->get_main_loop())) {
		iterating++;
		const bool done = loop->iteration();
		iterating--;
		if (done) {
			GDScriptFunctionState::finishing = true;
		}
		return done;
	}
	GodotProfileZoneGroupedFirst(_profile_zone, "prepare");
	iterating++;
	IdleWait::dispatch(); // Dispatch only ready sockets directly to their operations.
	Pool::drain(); // Return worker completions to main without per-Signal monitoring.
#ifdef MODULE_GDSCRIPT_ENABLED
	GDScriptFunctionState::drain_scheduler(); // Advance runnable scripts independently of SceneTree.
#endif
	Async::drain(); // Deliver retained Futures on main after signal receivers are connected.

	const uint64_t ticks = OS::get_singleton()->get_ticks_usec();
	Engine::get_singleton()->_frame_ticks = ticks;
	main_timer_sync.set_cpu_ticks_usec(ticks);

	const uint64_t ticks_elapsed = ticks - last_ticks;

	const int physics_ticks_per_second = Engine::get_singleton()->get_user_physics_ticks_per_second();
	const double physics_step = 1.0 / physics_ticks_per_second;

	const double time_scale = Engine::get_singleton()->get_effective_time_scale();

	MainFrameTime advance = main_timer_sync.advance(physics_step, physics_ticks_per_second);
	double process_step = advance.process_step;

	Engine::get_singleton()->_process_step = process_step;
	Engine::get_singleton()->_physics_interpolation_fraction = advance.interpolation_fraction;

	uint64_t physics_process_ticks = 0;
	uint64_t process_ticks = 0;
#if !defined(NAVIGATION_2D_DISABLED) || !defined(NAVIGATION_3D_DISABLED)
	uint64_t navigation_process_ticks = 0;
#endif // !defined(NAVIGATION_2D_DISABLED) || !defined(NAVIGATION_3D_DISABLED)

	frame += ticks_elapsed;

	last_ticks = ticks;

	const int max_physics_steps = Engine::get_singleton()->get_user_max_physics_steps_per_frame();
	if (advance.physics_steps > max_physics_steps) {
		process_step -= (advance.physics_steps - max_physics_steps) * physics_step;
		advance.physics_steps = max_physics_steps;
	}

	bool exit = false;

	// process all our active interfaces
#ifndef XR_DISABLED
	GodotProfileZoneGrouped(_profile_zone, "xr_server->_process");
	XRServer::get_singleton()->_process();
#endif // XR_DISABLED

	GodotProfileZoneGrouped(_profile_zone, "physics");
	for (int iters = 0; iters < advance.physics_steps; ++iters) {
		GodotProfileZone("Physics Step");
		GodotProfileZoneGroupedFirst(_physics_zone, "setup");
		if (Input::get_singleton()->is_agile_input_event_flushing()) {
			Input::get_singleton()->flush_buffered_events();
		}

		Engine::get_singleton()->_in_physics = true;
		Engine::get_singleton()->_physics_frames++;

		uint64_t physics_begin = OS::get_singleton()->get_ticks_usec();

		// Prepare the fixed timestep interpolated nodes BEFORE they are updated
		// by the physics server, otherwise the current and previous transforms
		// may be the same, and no interpolation takes place.
		GodotProfileZoneGrouped(_physics_zone, "main loop iteration prepare");
		OS::get_singleton()->get_main_loop()->iteration_prepare();

#ifndef PHYSICS_3D_DISABLED
		GodotProfileZoneGrouped(_physics_zone, "PhysicsServer3D::sync");
		PhysicsServer3D::get_singleton()->sync();
		PhysicsServer3D::get_singleton()->flush_queries();
#endif // PHYSICS_3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
		GodotProfileZoneGrouped(_physics_zone, "PhysicsServer2D::sync");
		PhysicsServer2D::get_singleton()->sync();
		PhysicsServer2D::get_singleton()->flush_queries();
#endif // PHYSICS_2D_DISABLED

		GodotProfileZoneGrouped(_physics_zone, "physics_process");
		if (OS::get_singleton()->get_main_loop()->physics_process(physics_step * time_scale)) {
#ifndef PHYSICS_3D_DISABLED
			PhysicsServer3D::get_singleton()->end_sync();
#endif // PHYSICS_3D_DISABLED
#ifndef PHYSICS_2D_DISABLED
			PhysicsServer2D::get_singleton()->end_sync();
#endif // PHYSICS_2D_DISABLED

			Engine::get_singleton()->_in_physics = false;
			exit = true;
			break;
		}

#if !defined(NAVIGATION_2D_DISABLED) || !defined(NAVIGATION_3D_DISABLED)
		uint64_t navigation_begin = OS::get_singleton()->get_ticks_usec();

		navigation_process_ticks = MAX(navigation_process_ticks, OS::get_singleton()->get_ticks_usec() - navigation_begin); // keep the largest one for reference
		navigation_process_max = MAX(OS::get_singleton()->get_ticks_usec() - navigation_begin, navigation_process_max);

		message_queue->flush();
#endif // !defined(NAVIGATION_2D_DISABLED) || !defined(NAVIGATION_3D_DISABLED)

#ifndef PHYSICS_3D_DISABLED
		GodotProfileZoneGrouped(_profile_zone, "3D physics");
		PhysicsServer3D::get_singleton()->end_sync();
		PhysicsServer3D::get_singleton()->step(physics_step * time_scale);
#endif // PHYSICS_3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
		GodotProfileZoneGrouped(_profile_zone, "2D physics");
		PhysicsServer2D::get_singleton()->end_sync();
		PhysicsServer2D::get_singleton()->step(physics_step * time_scale);
#endif // PHYSICS_2D_DISABLED

		message_queue->flush();

		GodotProfileZoneGrouped(_profile_zone, "main loop iteration end");
		OS::get_singleton()->get_main_loop()->iteration_end();

		physics_process_ticks = MAX(physics_process_ticks, OS::get_singleton()->get_ticks_usec() - physics_begin); // keep the largest one for reference
		physics_process_max = MAX(OS::get_singleton()->get_ticks_usec() - physics_begin, physics_process_max);

		Engine::get_singleton()->_in_physics = false;
	}

	if (Input::get_singleton()->is_agile_input_event_flushing()) {
		Input::get_singleton()->flush_buffered_events();
	}

	uint64_t process_begin = OS::get_singleton()->get_ticks_usec();

	GodotProfileZoneGrouped(_profile_zone, "process");
	if (OS::get_singleton()->get_main_loop()->process(process_step * time_scale)) {
		exit = true;
	}
	message_queue->flush();

	// Keep navigation and rendering in the optional game layer.

	process_ticks = OS::get_singleton()->get_ticks_usec() - process_begin;
	process_max = MAX(process_ticks, process_max);
	uint64_t frame_time = OS::get_singleton()->get_ticks_usec() - ticks;

	GodotProfileZoneGrouped(_profile_zone, "GDExtensionManager::frame");
	GDExtensionManager::get_singleton()->frame();

	GodotProfileZoneGrouped(_profile_zone, "ScriptServer::frame");
	for (int i = 0; i < ScriptServer::get_language_count(); i++) {
		ScriptServer::get_language(i)->frame();
	}

	if (EngineDebugger::is_active()) {
		EngineDebugger::get_singleton()->iteration(frame_time, process_ticks, physics_process_ticks, physics_step);
	}

	frames++;
	Engine::get_singleton()->_process_frames++;

	if (frame > 1000000) {
		// Wait a few seconds before printing FPS, as FPS reporting just after the engine has started is inaccurate.
		if (hide_print_fps_attempts == 0) {
			if (GLOBAL_GET("debug/settings/stdout/print_fps")) {
				print_line(vformat("FPS: %d (%s mspf)", frames, rtos(1000.0 / frames).pad_decimals(2)));
			}
		} else {
			hide_print_fps_attempts--;
		}

		Engine::get_singleton()->_fps = frames;
		performance->set_process_time(USEC_TO_SEC(process_max));
		performance->set_physics_process_time(USEC_TO_SEC(physics_process_max));
		performance->set_navigation_process_time(USEC_TO_SEC(navigation_process_max));
		process_max = 0;
		physics_process_max = 0;
		navigation_process_max = 0;

		frame %= 1000000;
		frames = 0;
	}

	iterating--;
	if (exit) {
		GDScriptFunctionState::finishing = true; // Lost frames stop resuming callers before nodes are torn down.
		return true; // Honor shutdown before entering an indefinite wait.
	}

	bool script_scheduled = Async::has_ready();
#ifdef MODULE_GDSCRIPT_ENABLED
	if (GDScriptFunctionState::has_scheduled()) {
		script_scheduled = true;
	}
#endif
	if (script_scheduled) {
		// Advance VM continuations to the next time slice independently of SceneTree frequency.
	} else if (IdleWait::count() > 0 || Pool::has_work()) {
		// Wake promptly on socket or worker completion without adding a fixed frame delay.
		IdleWait::wait(OS::get_singleton()->get_frame_delay(false));
	} else {
		GodotProfileZoneGrouped(_profile_zone, "OS::add_frame_delay");
		OS::get_singleton()->add_frame_delay(false, false);
	}

	return exit;
}

// Consume redraw requests in a headless environment.
void Main::force_redraw() {
}

/* Engine deinitialization
 *
 * Responsible for freeing all the memory allocated by previous setup steps,
 * so that the engine closes cleanly without leaking memory or crashing.
 * The order matters as some of those steps are linked with each other.
 */
// Release system resources before process exit.
void Main::cleanup(bool p_force) {
	Thread::make_main_thread();

	GodotProfileZone("cleanup");
	OS::get_singleton()->benchmark_begin_measure("Shutdown", "Main::Cleanup");
	if (!p_force) {
		ERR_FAIL_COND(!_start_success);
	}

	// Printing in the usual way can become problematic during/after cleanup.
	CoreGlobals::print_ready = false;

#ifdef DEBUG_ENABLED
	if (input) {
		input->flush_frame_parsed_events();
	}
#endif

	GDExtensionManager::get_singleton()->shutdown();

	ResourceLoader::clear_thread_load_tasks();

	ResourceLoader::remove_custom_loaders();
	ResourceSaver::remove_custom_savers();
	PropertyListHelper::clear_base_helpers();

	// Report an interrupted script as failure even when main() returns an empty value
	// that would otherwise leave a zero exit status.
	// Exclude serve requests because an individual request failure does not determine shutdown success.
	if (GDFail::hit && !GDEntry::serve_mode && OS::get_singleton()->get_exit_code() == EXIT_SUCCESS) {
		OS::get_singleton()->set_exit_code(EXIT_FAILURE);
	}
	GDFail::unwatch();

	// Collect worker Object references while the entry, SceneTree, and scripting language remain alive.
	shutdown_cli_runtime();

	// Flush before uninitializing the scene, but delete the MessageQueue as late as possible.
	message_queue->flush();

	if (gd_entry) {
		memdelete(gd_entry); // Free the plain script entry object.
		gd_entry = nullptr;
	}

	OS::get_singleton()->delete_main_loop();

	// Release waits queued by Node shutdown before terminating the scripting language.
	shutdown_cli_runtime();

	OS::get_singleton()->_cmdline.clear();
	OS::get_singleton()->_user_args.clear();
	OS::get_singleton()->_execpath = "";
	OS::get_singleton()->_local_clipboard = "";

	ResourceLoader::clear_translation_remaps();

	WorkerThreadPool::get_singleton()->exit_languages_threads();

	ScriptServer::finish_languages();

	// Keep rendering and XR in the optional game layer.

	ImageLoader::cleanup();

	GDExtensionManager::get_singleton()->deinitialize_extensions(GDExtension::INITIALIZATION_LEVEL_SCENE);
	uninitialize_modules(MODULE_INITIALIZATION_LEVEL_SCENE);

	unregister_platform_apis();
	unregister_driver_types();
	unregister_scene_types();

	finalize_physics();

	GDExtensionManager::get_singleton()->deinitialize_extensions(GDExtension::INITIALIZATION_LEVEL_SERVERS);
	uninitialize_modules(MODULE_INITIALIZATION_LEVEL_SERVERS);
	unregister_server_types();

	EngineDebugger::deinitialize();

	OS::get_singleton()->finalize();

	finalize_display();

	if (input) {
		memdelete(input);
	}

	if (packed_data) {
		memdelete(packed_data);
	}
	if (performance) {
		memdelete(performance);
	}
	if (input_map) {
		memdelete(input_map);
	}
	if (translation_server) {
		memdelete(translation_server);
	}
#ifndef PHYSICS_3D_DISABLED
	if (physics_server_3d_manager) {
		memdelete(physics_server_3d_manager);
	}
#endif // PHYSICS_3D_DISABLED
#ifndef PHYSICS_2D_DISABLED
	if (physics_server_2d_manager) {
		memdelete(physics_server_2d_manager);
	}
#endif // PHYSICS_2D_DISABLED
	if (globals) {
		memdelete(globals);
	}

	// Recheck execution permission when performing a scheduled restart.
	// The restart occurs at shutdown, potentially long after its initial authorization.
	if (OS::get_singleton()->is_restart_on_exit_set() && Perm::check(Perm::RUN, OS::get_singleton()->get_executable_path())) {
		//attempt to restart with arguments
		List<String> args = OS::get_singleton()->get_restart_on_exit_arguments();
		OS::get_singleton()->create_instance(args);
		OS::get_singleton()->set_restart_on_exit(false, List<String>()); //clear list (uses memory)
	}

	// Drain workers while MessageQueue remains alive because their completions can enqueue deferred calls.
	unregister_cli_types();

	// Now should be safe to delete MessageQueue (famous last words).
	message_queue->flush();
	memdelete(message_queue);

#if defined(STEAMAPI_ENABLED)
	if (steam_tracker) {
		memdelete(steam_tracker);
	}
#endif

	unregister_core_driver_types();
	unregister_core_extensions();
	uninitialize_modules(MODULE_INITIALIZATION_LEVEL_CORE);

	if (engine) {
		memdelete(engine);
	}

	unregister_core_types();

	OS::get_singleton()->benchmark_end_measure("Shutdown", "Main::Cleanup");
	OS::get_singleton()->benchmark_dump();

	OS::get_singleton()->finalize_core();

	Thread::release_main_thread();
}
