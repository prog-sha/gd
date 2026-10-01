/**************************************************************************/
/*  cmd.h                                                                 */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Collect parsed arguments and command entry points for the executable.
//
// Share state because argument parsing and command execution occur separately.
// Arguments are parsed in main and commands execute from Main::start.
//
// Implementation is in cmd.cpp.

#include "core/object/script_language.h"
#include "core/templates/list.h"
#include "core/variant/dictionary.h"

struct Cmd {
	// State collected from arguments.
	static inline String name;
	static inline String script;
	static inline String eval_src;
	static inline List<String> args; // Arguments passed to the script.
	static inline List<String> raw; // Original startup arguments forwarded to children in order.
	static inline bool serve = false; // Persistent server mode.
	static inline String output; // Compile output filename.
	static inline bool watch = false; // Restart execution when files change.
	static inline int workers = 1; // Listener worker count; one disables worker grouping.
	static inline List<String> flags; // Permission flags inherited by children.
	static inline bool strict = false; // Enable stricter permission and static checks.
	static inline String pkg; // Package subcommand passed to the embedded script; empty disables it.
	static inline List<String> pkg_args; // Package arguments and options on either side of the command.
	static inline String install_root; // Explicit global installation root.
	static inline String global_bin; // Selected directory holding global commands and their private graphs.

	static inline bool tooling = false; // Management commands do not apply strict execution mode.
	// Validate command syntax before applying options or loading project state.
	static bool parse(List<String> &r_args, bool p_embedded, String &r_help, bool &r_wants_help);
	static bool display(); // Select commands using the installed display package.
	static bool for_godot(); // Select project copies intended for the editor.
	static bool runtime_flag(const String &p_arg); // Identify saved authority without accepting command options.
	static bool global_entry(const String &p_path); // Require an installed entry to stay inside its private package.
	// Expand an installed command's saved permissions and arguments before parsing.
	static bool installed_args(List<String> &r_args);

	// Subcommand entry points.
	static String wrap_eval(const String &p_src);
	static Ref<Script> script_from_source(const String &p_src);
	static int doc(const String &p_name);
	static int repl();
	static uint64_t watch_stamp(const String &p_dir);
	static int watch_loop(const List<String> &p_args, const String &p_dir);
	static int workers_loop(const List<String> &p_args, int p_count);
	static int completions(const String &p_shell);
	// Build self-invocation arguments, removing the selected option and its value.
	static List<String> child_args(const String &p_drop);
	static String find_config();
	static Dictionary load_config();
	static String pkg_dir();
	static void apply_types();
	static int info();
	static int init_project(const String &p_name);
	static int run_task(const String &p_name);
	static void collect(const String &p_path, List<String> &r_files, const String &p_suffix);
	// Collect and sort matching files; report a reason and return false when none match.
	// Share selection across test, check, and format so invalid paths behave consistently.
	static bool collect_or_fail(const String &p_path, const String &p_suffix, List<String> &r_files);
	static int run_each(const String &p_path, const List<String> &p_flags,
			const char *p_suffix, const char *p_extra_cmd, bool p_print_ok, const char *p_tally);
	static int run_tests(const String &p_path, const List<String> &p_flags);
	static int run_checks(const String &p_path, const List<String> &p_flags);
};
