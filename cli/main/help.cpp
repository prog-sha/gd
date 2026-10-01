/**************************************************************************/
/*  help.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Render command help directly from the definitions used to validate arguments.
#include "cli/main/help.h"
#include "cli/main/spec.h"

#include "core/os/os.h"

namespace {

// Print a section heading with terminal color.
void title(const char *p_text) {
	OS::get_singleton()->print("\n\u001b[1;93m%s:\u001b[0m\n", p_text);
}

// Print one documented spelling alongside its purpose.
void line(const String &p_name, const char *p_text) {
	OS::get_singleton()->print("  \u001b[92m%-34s\u001b[0m %s\n", p_name.utf8().get_data(), p_text);
}

// Describe accepted options, including aliases and their value syntax.
void options(uint64_t p_flags) {
	for (const CLI::Option &opt : CLI::OPTIONS) {
		if (!(p_flags & opt.flag)) continue;
		String name = opt.alias[0] ? String(opt.alias) + ", " + opt.name : String(opt.name);
		if (opt.value == CLI::REQUIRED) name += " <" + String(opt.arg) + ">";
		else if (opt.value == CLI::OPTIONAL_VALUE) name += "[=" + String(opt.arg) + "]";
		line(name, opt.about);
	}
}

} // namespace

// Print the selected command's syntax and supported options.
bool Help::command(const String &p_name) {
	const CLI::Command *entry = CLI::command(p_name.utf8().get_data());
	if (!entry) return false;
	OS::get_singleton()->print("\u001b[1;93mUsage:\u001b[0m gd %s [OPTIONS] %s\n\n%s\n", entry->name, entry->usage, entry->about);
	title("Options");
	options(CLI::COMMON | entry->flags);
	line("--", "Stop option parsing; following words are literal arguments.");
	OS::get_singleton()->print("\n");
	return true;
}

// Print the public command registry and shared process options.
void Help::show() {
	title("Usage");
	OS::get_singleton()->print("  gd <command> [options] [args...]\n  gd [options] <script.gd> [args...]\n");
	title("Commands");
	for (const CLI::Command &entry : CLI::COMMANDS) {
		line(String(entry.name) + (entry.usage[0] ? " " + String(entry.usage) : String()), entry.about);
	}
	title("Common options");
	options(CLI::COMMON);
	line("--", "Stop option parsing; following words are literal arguments.");
	OS::get_singleton()->print("\nUse gd help <command> for command-specific options.\n");
}
