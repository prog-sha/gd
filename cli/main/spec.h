/**************************************************************************/
/*  spec.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Define public commands and options for parsing, help, completion, and delegation.
#pragma once

#include <stdint.h>
#include <string.h>

namespace CLI {

// Name each option once so command masks also describe their supported syntax.
enum Flag : uint64_t {
	HELP = 1ULL << 0, VERSION = 1ULL << 1, VERBOSE = 1ULL << 2, QUIET = 1ULL << 3,
	HEADER = 1ULL << 4, NO_HEADER = 1ULL << 5, PATH = 1ULL << 6, STRICT_FLAG = 1ULL << 7,
	MOUNT = 1ULL << 8, ALL = 1ULL << 9, ALLOW_NET = 1ULL << 10, DENY_NET = 1ULL << 11,
	ALLOW_ENV = 1ULL << 12, DENY_ENV = 1ULL << 13, ALLOW_RUN = 1ULL << 14, DENY_RUN = 1ULL << 15,
	ALLOW_EXT = 1ULL << 16, DENY_EXT = 1ULL << 17, ALLOW_SYS = 1ULL << 18, DENY_SYS = 1ULL << 19,
	WATCH = 1ULL << 20, WORKERS = 1ULL << 21, NO_TREE = 1ULL << 22, OUTPUT = 1ULL << 23,
	CHECK = 1ULL << 24, GLOBAL = 1ULL << 25, NAME = 1ULL << 26, ROOT = 1ULL << 27,
	FORCE = 1ULL << 28, FROZEN = 1ULL << 29, CACHED = 1ULL << 30, SYNC = 1ULL << 31,
	LATEST = 1ULL << 32, DRY = 1ULL << 33, GODOT = 1ULL << 34,
};

const uint64_t AUTHORITY = STRICT_FLAG | MOUNT | ALL | ALLOW_NET | DENY_NET | ALLOW_ENV | DENY_ENV | ALLOW_RUN | DENY_RUN | ALLOW_EXT | DENY_EXT | ALLOW_SYS | DENY_SYS; // Saved runtime permissions.
const uint64_t COMMON = HELP | VERSION | VERBOSE | QUIET | HEADER | NO_HEADER | PATH | AUTHORITY; // Shared process settings.
const uint64_t RESTORE = FROZEN | CACHED | SYNC; // Options requiring dependency restoration.

enum Value { NONE, REQUIRED, OPTIONAL_VALUE }; // Optional values use an equals sign.

// Describe an option's spellings, value, and visible purpose.
struct Option {
	uint64_t flag; // Membership bit in command masks.
	const char *name; // Canonical long spelling.
	const char *alias; // Short spelling, or an empty string.
	Value value; // Whether the option carries a value.
	const char *arg; // Value label displayed in help.
	const char *about; // Help description.
};

inline constexpr Option OPTIONS[] = {
	{ HELP, "--help", "-h", NONE, "", "Print command help." },
	{ VERSION, "--version", "", NONE, "", "Print the version." },
	{ VERBOSE, "--verbose", "-v", NONE, "", "Print verbose output." },
	{ QUIET, "--quiet", "-q", NONE, "", "Silence stdout; keep errors." },
	{ HEADER, "--header", "", NONE, "", "Print the startup version header." },
	{ NO_HEADER, "--no-header", "", NONE, "", "Suppress the startup header." },
	{ PATH, "--path", "", REQUIRED, "directory", "Use an absolute directory as res://." },
	{ STRICT_FLAG, "--strict", "", NONE, "", "Restrict script execution; ignored by management commands." },
	{ MOUNT, "--mount", "", REQUIRED, "name=path:r|rw", "Expose a file directory; unavailable on Windows." },
	{ ALL, "--allow-all", "-A", NONE, "", "Allow all capabilities except files." },
	{ ALLOW_NET, "--allow-net", "", OPTIONAL_VALUE, "host[:port],...", "Allow network access." },
	{ DENY_NET, "--deny-net", "", OPTIONAL_VALUE, "host[:port],...", "Deny network access, overriding allows." },
	{ ALLOW_ENV, "--allow-env", "", OPTIONAL_VALUE, "name,...", "Allow environment variables." },
	{ DENY_ENV, "--deny-env", "", OPTIONAL_VALUE, "name,...", "Deny environment variables, overriding allows." },
	{ ALLOW_RUN, "--allow-run", "", OPTIONAL_VALUE, "command,...", "Allow child processes." },
	{ DENY_RUN, "--deny-run", "", OPTIONAL_VALUE, "command,...", "Deny child processes, overriding allows." },
	{ ALLOW_EXT, "--allow-ext", "", OPTIONAL_VALUE, "path,...", "Allow native extensions and trust their code." },
	{ DENY_EXT, "--deny-ext", "", OPTIONAL_VALUE, "path,...", "Deny native extensions, overriding allows." },
	{ ALLOW_SYS, "--allow-sys", "", OPTIONAL_VALUE, "item,...", "Allow system information." },
	{ DENY_SYS, "--deny-sys", "", OPTIONAL_VALUE, "item,...", "Deny system information, overriding allows." },
	{ WATCH, "--watch", "", NONE, "", "Restart when a .gd file changes." },
	{ WORKERS, "--workers", "", REQUIRED, "n|auto", "Run multiple listeners; cannot combine with --watch." },
	{ NO_TREE, "--no-scene-tree", "", NONE, "", "Fail if a SceneTree is constructed." },
	{ OUTPUT, "--output", "-o", REQUIRED, "file", "Output executable; default: the input file name." },
	{ CHECK, "--check", "", NONE, "", "Report formatting differences without writing." },
	{ GLOBAL, "--global", "-g", NONE, "", "Manage a command independently of the project." },
	{ NAME, "--name", "-n", REQUIRED, "name", "With --global: choose the installed command name." },
	{ ROOT, "--root", "", REQUIRED, "directory", "With --global: override GD_INSTALL_ROOT." },
	{ FORCE, "--force", "-f", NONE, "", "Replace an installed global command or reinstall gd during upgrade." },
	{ FROZEN, "--frozen", "", NONE, "", "Restore only: fail instead of changing gd.lock." },
	{ CACHED, "--cached-only", "", NONE, "", "Restore only: use cached packages without network access." },
	{ SYNC, "--sync", "", NONE, "", "Restore only: prune packages no longer requested." },
	{ GODOT, "--godot", "", NONE, "", "Manage project package copies under addons/." },
	{ LATEST, "--latest", "", NONE, "", "Update dependency ranges as well as versions." },
	{ DRY, "--dry-run", "", NONE, "", "Preview publication or upgrade without applying it." },
};

enum Kind { TOOL = 1, PACKAGE = 2, DISPLAY = 4, ARGS = 8, VIEW = 16 }; // Execution and delegation properties.

// Describe positional arguments and command-specific options in one registry.
struct Command {
	const char *name; // Public command word.
	const char *usage; // Positional syntax for help.
	const char *about; // Purpose shown in general and command help.
	uint64_t flags; // Accepted command-specific options in addition to COMMON.
	int kind; // Execution and forwarding properties.
	int min; // Required positional argument count.
	int max; // Maximum positional argument count; -1 permits trailing arguments.
};

inline constexpr Command COMMANDS[] = {
	{ "run", "<script.gd> [args...]", "Run a script; also the default when a path is given.", WATCH | WORKERS | NO_TREE, ARGS, 1, -1 },
	{ "serve", "<script.gd> [args...]", "Run a server and keep running after main() returns.", WATCH | WORKERS | NO_TREE, ARGS, 1, -1 },
	{ "test", "[path]", "Collect and run *_test.gd files.", NO_TREE, 0, 0, 1 },
	{ "check", "[path]", "Check syntax and types without running scripts.", NO_TREE, TOOL, 0, 1 },
	{ "fmt", "[path]", "Format .gd files, or report differences with --check.", CHECK, TOOL, 0, 1 },
	{ "eval", "<code> [args...]", "Execute inline code.", NO_TREE, ARGS, 1, -1 },
	{ "repl", "", "Start an interactive session with persistent values.", 0, 0, 0, 0 },
	{ "doc", "[name|manual|all]", "Read the manual or inspect an API.", 0, TOOL, 0, 1 },
	{ "init", "[@scope/name]", "Create a project, or a package when a scoped name is given.", 0, TOOL, 0, 1 },
	{ "task", "[name]", "Run a task from gd.json, or list available tasks.", NO_TREE, 0, 0, 1 },
	{ "compile", "<script.gd>", "Bundle a script and its runtime in one executable.", OUTPUT, TOOL, 1, 1 },
	{ "dump-extension-api", "", "Write extension_api.json for gd-cpp.", 0, TOOL, 0, 0 },
	{ "completions", "<bash|zsh>", "Print shell completion definitions.", 0, TOOL, 1, 1 },
	{ "add", "[alias] <package|url|path>", "Add and fetch a project dependency or asset.", GODOT, TOOL | PACKAGE | VIEW, 1, 2 },
	{ "install", "[alias] [package|url|file|directory] [-- args...]", "Restore dependencies, add a package, or install a command with -g.", GLOBAL | NAME | ROOT | FORCE | RESTORE | GODOT, TOOL | PACKAGE | VIEW, 0, 2 },
	{ "uninstall", "<name>", "Remove a project dependency, or a global command with -g.", GLOBAL | ROOT | GODOT, TOOL | PACKAGE | VIEW, 1, 1 },
	{ "outdated", "", "List dependencies with newer versions.", 0, TOOL | PACKAGE | VIEW, 0, 0 },
	{ "update", "[name]", "Update dependencies within their requested ranges.", LATEST | GODOT, TOOL | PACKAGE | VIEW, 0, 1 },
	{ "search", "<words...>", "Search package registries and asset catalogs.", 0, TOOL | PACKAGE | VIEW, 1, -1 },
	{ "publish", "[release-url]", "Write release files, or list a released version in the registry.", DRY, TOOL | PACKAGE | VIEW, 0, 1 },
	{ "info", "", "List configured dependencies and their installed locations.", GODOT, TOOL | VIEW, 0, 0 },
	{ "upgrade", "[version]", "Update the running gd executable.", DRY | FORCE, TOOL | PACKAGE | VIEW, 0, 1 },
	{ "editor", "[path]", "Install gd-godot when needed and open the editor.", 0, TOOL | PACKAGE | DISPLAY, 0, 1 },
	{ "run-game", "[path] [-- args...]", "Run a project through the installed gd-godot.", 0, TOOL | PACKAGE | DISPLAY | ARGS, 0, 1 },
	{ "help", "[command]", "List commands or describe one command.", 0, TOOL, 0, 1 },
};

// Resolve either option spelling before applying its value syntax.
inline const Option *option(const char *p_name) {
	for (const Option &entry : OPTIONS) {
		if (strcmp(entry.name, p_name) == 0 || (entry.alias[0] && strcmp(entry.alias, p_name) == 0)) return &entry;
	}
	return nullptr;
}

// Find command properties without initializing the runtime.
inline const Command *command(const char *p_name) {
	for (const Command &entry : COMMANDS) {
		if (strcmp(entry.name, p_name) == 0) return &entry;
	}
	return nullptr;
}

} // namespace CLI
