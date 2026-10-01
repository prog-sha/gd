/**************************************************************************/
/*  args.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Validate and normalize public arguments before any command changes process state.
#include "cli/main/cmd.h"
#include "cli/main/spec.h"
#include "cli/sys/perm.h"

#include "core/os/os.h"
#include "core/templates/vector.h"

namespace {

// Find an option by either spelling, without interpreting its value.
const CLI::Option *option(const String &p_word) {
	return CLI::option(p_word.get_slice("=", 0).utf8().get_data());
}

// Print a parse error while command execution is still impossible.
bool fail(const String &p_message) {
	OS::get_singleton()->printerr("error: %s\n", p_message.utf8().get_data());
	return false;
}

} // namespace

// Parse all syntax and option relationships, then leave only normalized options for setup.
bool Cmd::parse(List<String> &r_args, bool p_embedded, String &r_help, bool &r_wants_help) {
	const CLI::Command *target = nullptr;
	List<String> normalized;
	Vector<String> operands;
	uint64_t seen = 0;
	int split = -1;
	bool named = false;
	for (auto *item = r_args.front(); item; item = item->next()) {
		const String word = item->get();
		if (split >= 0) {
			operands.push_back(word);
			continue;
		}
#ifdef MACOS_ENABLED
		// Ignore the launch identifier added by the desktop before parsing public arguments.
		if (word.begins_with("-psn_")) continue;
#endif
		if (word == "--") {
			split = operands.size();
			continue;
		}
		if (word == "++") return fail("unexpected argument: ++; use -- before script arguments");
		if (!word.begins_with("-")) {
			if (!target) {
				target = CLI::command(word.utf8().get_data());
				if (target) {
					named = true;
					continue;
				}
				target = CLI::command("run");
			}
			operands.push_back(word);
			continue;
		}
		const CLI::Option *opt = option(word);
		if (!opt) {
			const String advice = Perm::flag_advice(word);
			return fail(advice.is_empty() ? "unknown option: " + word : advice);
		}
		seen |= opt->flag;
		const bool equal = word.contains("=");
		if (equal && opt->value == CLI::NONE) return fail(String(opt->name) + " does not take a value");
		String value;
		if (equal) {
			value = word.substr(word.find("=") + 1);
		} else if (opt->value == CLI::REQUIRED) {
			if (!item->next() || item->next()->get().begins_with("-")) return fail(String(opt->name) + " needs " + opt->arg);
			item = item->next();
			value = item->get();
		}
		if (opt->value == CLI::REQUIRED && value.is_empty()) return fail(String(opt->name) + " needs " + opt->arg);
		// Keep scoped permissions and listener counts together; setup reads other values separately.
		if ((opt->value == CLI::OPTIONAL_VALUE && equal) || opt->flag == CLI::WORKERS || opt->flag == CLI::MOUNT) {
			normalized.push_back(String(opt->name) + "=" + value);
		} else {
			normalized.push_back(opt->name);
			if (opt->value == CLI::REQUIRED) normalized.push_back(value);
		}
	}
	if (!target) target = CLI::command("run");
	name = named ? String(target->name) : String();
	tooling = (target->kind & CLI::TOOL) != 0;
	pkg = target->kind & CLI::PACKAGE ? name : String();

	// Answer help after any command operand, but never consume a script's delimited arguments.
	r_wants_help = (seen & CLI::HELP) || name == "help";
	if (r_wants_help) {
		if (name == "help") {
			if (operands.size() > 1) return fail("gd help accepts one command");
			r_help = operands.is_empty() ? String() : operands[0];
		} else {
			r_help = name;
		}
		return true;
	}
	for (const CLI::Option &opt : CLI::OPTIONS) {
		if ((seen & opt.flag) && !((CLI::COMMON | target->flags) & opt.flag)) {
			return fail(vformat("%s is not supported by gd %s", opt.name, target->name));
		}
	}
	const bool global = (seen & CLI::GLOBAL) != 0;
	const bool install = name == "install";
	if (global && (seen & CLI::GODOT)) return fail("--godot manages project packages and cannot be combined with --global");
	const uint64_t needs_global = CLI::NAME | CLI::ROOT | (install ? uint64_t(CLI::FORCE) : 0);
	if ((seen & needs_global) && !global) return fail("--name, --root, and install --force require --global");
	if ((seen & CLI::RESTORE) && (global || !operands.is_empty())) return fail("restore options require gd install without package arguments");
	if ((seen & CLI::WATCH) && (seen & CLI::WORKERS)) return fail("--watch and --workers cannot be combined");
	if ((target->kind & CLI::DISPLAY) && (seen & CLI::PATH) && !operands.is_empty() && split != 0) return fail("choose one project path, as an argument or with --path");

	// Delimited arguments belong to a global command or display program after its own operands.
	const bool forwarded = (install && global) || (target->kind & CLI::DISPLAY);
	const int count = forwarded && split >= 0 ? split : operands.size();
	const int low = install && global ? 1 : p_embedded && (target->kind & CLI::ARGS) ? 0 : target->min;
	const int high = install && global ? -1 : target->max;
	const bool empty_invocation = !named && operands.is_empty();
	if (low > 0 && count > 0 && operands[0].is_empty()) return fail(vformat("gd %s needs %s", target->name, target->usage));
	if (!(seen & CLI::VERSION) && !empty_invocation && (count < low || (high >= 0 && count > high))) {
		return fail(vformat("usage: gd %s %s", target->name, target->usage));
	}
	if (split >= 0 && forwarded && !(target->kind & CLI::ARGS) && !global && operands.size() > split) return fail("gd editor does not accept trailing arguments");

	// Assign operands once so setup cannot reinterpret values as commands or options.
	if (!pkg.is_empty()) {
		for (int i = 0; i < count; i++) {
			// Package operands cannot become management options when the script receives them.
			if (operands[i].begins_with("-")) return fail("unexpected package argument: " + operands[i] + "; use ./ for local paths starting with -");
			pkg_args.push_back(operands[i]);
		}
		if (forwarded && split >= 0) {
			for (int i = split; i < operands.size(); i++) args.push_back(operands[i]);
		}
	} else {
		int first = 0;
		if (!operands.is_empty() && !(p_embedded && (target->kind & CLI::ARGS) && name != "eval")) {
			if (name == "eval") eval_src = operands[0];
			else script = operands[0];
			first = 1;
		}
		for (int i = first; i < operands.size(); i++) args.push_back(operands[i]);
	}
	r_args = normalized;
	return true;
}

// Identify saved runtime authority without allowing command or formatting options.
bool Cmd::runtime_flag(const String &p_arg) {
	const CLI::Option *opt = option(p_arg);
	return opt && (opt->flag & CLI::AUTHORITY) && (opt->value != CLI::REQUIRED || p_arg.contains("="));
}

// Share display-command selection across startup and package dispatch.
bool Cmd::display() {
	const CLI::Command *entry = CLI::command(name.utf8().get_data());
	return entry && (entry->kind & CLI::DISPLAY);
}
