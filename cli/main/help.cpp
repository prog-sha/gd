/**************************************************************************/
/*  help.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement command-line help declared in help.h.

#include "cli/main/help.h"

#include "core/os/os.h"

namespace {

// Print a section heading with terminal color.
void title(const char *p_text) {
	OS::get_singleton()->print("\n\u001b[1;93m%s:\u001b[0m\n", p_text);
}

// Print an option name on the left and its description on the right.
void line(const char *p_name, const char *p_text) {
	OS::get_singleton()->print("  \u001b[92m%-34s\u001b[0m %s\n", p_name, p_text); // Width of the option-name column.
}

} // namespace

// Return standard API purpose descriptions.
void Help::show(const char *p_binary) {
	title("Usage");
	OS::get_singleton()->print("  %s \u001b[96m[command] [options] <script.gd> [args...]\u001b[0m\n", p_binary);

	title("Commands");
	line("run <script.gd>", "Run a script. The default when a path is given.");
	line("serve <script.gd>", "Run and keep running after main() returns.");
	line("test [path]", "Collect *_test.gd and run them.");
	line("bench [path]", "Run *_bench.gd five times and report the time.");
	line("check [path]", "Type-check one file or every .gd below a path.");
	line("fmt [path]", "Format. Use --check to report without writing.");
	line("eval '<code>'", "Run one line.");
	line("repl", "Interactive. Values carry to the next line.");
	line("doc [name|manual|all]", "Read the manual or inspect an API.");
	line("init", "Write gd.json and main.gd.");
	line("task [name]", "Run a task from gd.json. No name lists them.");
	line("compile <script.gd>", "Build one standalone executable.");
	line("completions [zsh]", "Print the shell completion definition.");
	line("add [name] <@scope/name|url|path>", "Add a dependency from the registry, a URL, or a local directory.");
	line("install", "Fetch everything in gd.json.");
	line("remove <name>", "Drop a dependency.");
	line("outdated", "List packages with a newer version.");
	line("update [name]", "Move versions up within their range.");
	line("search <words...>", "Look packages up in the registry catalog.");
	line("publish", "Push to the registry.");
	line("info", "List what is being imported.");

	title("Package options");
	line("--latest", "For update: move the range up too.");
	line("--frozen", "Fail instead of changing gd.lock.");
	line("--cached-only", "Use what is already fetched. No network.");
	line("--dry-run", "Report what would change. Writes nothing.");

	title("Files");
	line("--mount <name>=<path>:<r|rw>", "Let the script reach <path> as <name>://. Not on Windows.");
	line("--strict", "Restrict permissions and warn about dynamic types.");

	title("Other permissions");
	line("--allow-net[=host[:port],...]", "Connect and listen.");
	line("--allow-env[=name,...]", "Environment variables.");
	line("--allow-run[=command,...]", "Child processes.");
	line("--allow-ext[=path,...]", "Native extensions. Trusts their code.");
	line("--allow-sys[=item,...]", "Machine and environment facts.");
	line("--deny-<kind>[=...]", "Cancel the above. Beats an allow.");
	line("-A, --allow-all", "Allow everything except files.");

	title("Options");
	line("--watch", "Re-run when a .gd changes.");
	line("--no-scene-tree", "Fail immediately if a SceneTree is constructed (development guard).");
	line("--workers=<n|auto>", "Run n listeners; default 1. Sessions and rate limits are not shared.");
	line("-o, --output <name>", "Where compile writes. Default: the input file name.");
	line("--check", "For fmt: report differences without writing.");
	line("-v, --verbose", "Verbose stdout.");
	line("-q, --quiet", "Silence stdout. Errors still show.");
	line("--header", "Print the version header on startup.");
	line("--no-header", "Suppress the version header.");
	line("--, ++", "Everything after this goes to the script.");
	line("--dump-extension-api", "Write extension_api.json for gd-cpp.");
	line("--version", "Print the version string.");
	line("-h, --help", "This message.");
	OS::get_singleton()->print("\n");
}
