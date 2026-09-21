/**************************************************************************/
/*  cmd.cpp                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement subcommands declared in cmd.h.

#include "cli/main/cmd.h"
#include "cli/sys/system.h"
#include "cli/sys/clock.h"
#include "cli/main/briefs.gen.h"
#include "cli/main/manual.gen.h"
#include "cli/sys/os.h"
#include "cli/sys/pkgscope.h"
#include "cli/tool/compile.h"
#include "cli/tool/fmt.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/core_globals.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/hashfuncs.h"
#include "core/version.h"

#include "modules/gdscript/gdscript.h"

#include <csignal>

#include <cstdio>

namespace {

volatile std::sig_atomic_t run_signal = 0; // Termination signal received by a parent managing child processes.
constexpr int DOC_LINE_MAX = 76; // Maximum list line width for gd doc all.

// Check whether a task flag expands its strict parent's permissions.
bool task_grants_permission(const String &p_arg) {
	return p_arg == "-A" || p_arg == "--allow-all" || p_arg == "--mount" ||
			p_arg.begins_with("--allow-") || p_arg.begins_with("--mount=");
}

// Perform only a signal-safe integer assignment in the handler.
void stop_run(int p_signal) {
	run_signal = p_signal;
}

// Handle termination signals while children exist and restore previous handlers afterward.
class RunSignals {
	using Handler = void (*)(int);
	Handler old_int = SIG_DFL;
	Handler old_term = SIG_DFL;

public:
	RunSignals() {
		run_signal = 0;
		old_int = std::signal(SIGINT, stop_run);
		old_term = std::signal(SIGTERM, stop_run);
	}

	~RunSignals() {
		if (old_int != SIG_ERR) {
			std::signal(SIGINT, old_int);
		}
		if (old_term != SIG_ERR) {
			std::signal(SIGTERM, old_term);
		}
	}

	int caught() const { return run_signal; }
};

// Cache watched-file metadata to avoid rereading unchanged files.
struct WatchFile {
	uint64_t modified = 0; // Last observed modification time in seconds.
	int64_t size = 0; // Last observed byte size.
	uint64_t body = 0; // Content fingerprint detecting saves within the same second.
	uint64_t seen = 0; // Last scan generation used to detect deletion.
	bool verify = false; // Whether to verify contents after the modification second ends.
};

HashMap<String, WatchFile> watch_files; // File fingerprints retained only during watch mode.
uint64_t watch_round = 0; // Scan generation used to detect removed files.

// Walk script files beneath a directory and include contents in their fingerprints.
uint64_t watch_tree(const String &p_dir, uint64_t p_round, uint64_t p_now) {
	uint64_t sum = 0;
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null() || dir->list_dir_begin() != OK) {
		return 0;
	}
	String name = dir->get_next();
	while (!name.is_empty()) {
		if (!name.begins_with(".") && name != "bin") {
			const String full = p_dir.path_join(name);
			if (dir->current_is_dir()) {
				sum ^= watch_tree(full, p_round, p_now);
			} else if (name.ends_with(".gd")) {
				const uint64_t modified = FileAccess::get_modified_time(full);
				const int64_t size = FileAccess::get_size(full);
				WatchFile *file = watch_files.getptr(full);
				if (!file) {
					watch_files.insert(full, WatchFile());
					file = watch_files.getptr(full);
				}
				const bool changed = file->modified != modified || file->size != size;
				// Watch contents during the modification second, then verify once at its boundary and cache.
				if (changed || modified >= p_now) {
					file->body = FileAccess::get_md5(full).hash64();
					file->verify = true;
				} else if (file->verify) {
					file->body = FileAccess::get_md5(full).hash64();
					file->verify = false;
				}
				file->modified = modified;
				file->size = size;
				file->seen = p_round;
				uint64_t one = hash_djb2_one_64(full.hash64());
				one = hash_djb2_one_64(modified, one);
				one = hash_djb2_one_64((uint64_t)size, one);
				one = hash_djb2_one_64(file->body, one);
				sum ^= one;
			}
		}
		name = dir->get_next();
	}
	dir->list_dir_end();
	return sum;
}

// Select Japanese documentation only when the locale begins with ja; otherwise use English.
bool doc_japanese() {
	String locale = GDSystem::env("LC_ALL");
	if (locale.is_empty()) {
		locale = GDSystem::env("LANG");
	}
	return locale.begins_with("ja");
}

// Look up API purpose descriptions embedded from docs/api.json and docs/api.en.json.
const char *doc_brief(const String &p_name) {
	for (const GDBrief &brief : gd_briefs) {
		if (p_name == brief.name) {
			return doc_japanese() ? brief.ja : brief.en;
		}
	}
	return nullptr;
}

// Extract object classes referenced by properties or return values from ClassDB metadata.
StringName doc_class(const PropertyInfo &p_info) {
	if (p_info.class_name != StringName()) {
		return p_info.class_name;
	}
	if (p_info.type == Variant::OBJECT && ClassDB::class_exists(p_info.hint_string)) {
		return p_info.hint_string;
	}
	return StringName();
}

// Format ClassDB types as public script type names.
String doc_type(const PropertyInfo &p_info, bool p_return = false) {
	const StringName object = doc_class(p_info);
	if (object != StringName()) {
		return object;
	}
	if (p_info.type == Variant::NIL) {
		const bool variant = p_info.usage & PROPERTY_USAGE_NIL_IS_VARIANT;
		return p_return && !variant ? "void" : "Variant";
	}
	return Variant::get_type_name(p_info.type);
}

// Check whether an implementation class name needs a public API path.
bool doc_internal(const StringName &p_type) {
	const String name = p_type;
	return name.ends_with("API") || name.ends_with("Internal");
}

// Copy registered purpose metadata into terminal documentation.
String doc_group(const StringName &p_type, const String &p_name) {
	return ClassDB::get_method_group(p_type, p_name);
}

// Format API defaults as concise source-compatible expressions.
String doc_value(const Variant &p_value) {
	if (p_value.get_type() == Variant::CALLABLE && !Callable(p_value).is_valid()) {
		return "null";
	}
	const String json = JSON::stringify(p_value);
	return json.is_empty() ? vformat("%s", p_value) : json;
}

// Remove the R metadata marker and return only traversable object types.
StringName doc_meta_class(const StringName &p_raw) {
	String name = p_raw;
	if (name.begins_with("R:")) {
		name = name.substr(2);
	}
	return ClassDB::class_exists(name) ? StringName(name) : StringName();
}

// Combine a method's actual success type and waiting behavior into its public signature.
String doc_flow(const StringName &p_type, const String &p_path, const MethodInfo &p_method) {
	String flow = ClassDB::get_await_class(p_type, p_method.name);
	const bool waits = !flow.is_empty() && !ClassDB::is_auto_wait(p_type, p_method.name);
	if (flow.is_empty()) {
		flow = ClassDB::get_result_class(p_type, p_method.name);
	}
	if (flow.is_empty()) {
		const StringName object = doc_class(p_method.return_val);
		return doc_internal(object) ? p_path + "." + String(p_method.name) + "()" : doc_type(p_method.return_val, true);
	}
	const bool result = flow.begins_with("R:") || !ClassDB::get_result_class(p_type, p_method.name).is_empty();
	flow = flow.trim_prefix("R:");
	return (waits ? "await " : "") + flow + (result ? ", Err" : "");
}

// Find the object type reached after paths such as GD.file or GD.web.app.
StringName doc_child(const StringName &p_type, const String &p_name) {
	List<PropertyInfo> properties;
	ClassDB::get_property_list(p_type, &properties, true);
	for (const PropertyInfo &property : properties) {
		if (property.name == p_name) {
			return doc_class(property);
		}
	}

	List<MethodInfo> methods;
	ClassDB::get_method_list(p_type, &methods, true);
	for (const MethodInfo &method : methods) {
		if (method.name == p_name) {
			StringName flow = ClassDB::get_await_class(p_type, method.name);
			if (flow == StringName()) {
				flow = ClassDB::get_result_class(p_type, method.name);
			}
			return flow != StringName() ? doc_meta_class(flow) : doc_class(method.return_val);
		}
	}
	return StringName();
}

// Find a named public method for selected-method documentation.
bool doc_method(const StringName &p_type, const StringName &p_name, MethodInfo &r_method) {
	List<MethodInfo> methods;
	ClassDB::get_method_list(p_type, &methods, true);
	for (const MethodInfo &method : methods) {
		if (method.name == p_name && !String(p_name).begins_with("_")) {
			r_method = method;
			return true;
		}
	}
	return false;
}

// Format a method with arguments, defaults, and success type on one line.
String doc_method_line(const StringName &p_type, const String &p_path, const MethodInfo &p_method) {
	String args;
	const int first_default = p_method.arguments.size() - p_method.default_arguments.size();
	for (int i = 0; i < p_method.arguments.size(); i++) {
		const PropertyInfo &argument = p_method.arguments[i];
		if (!args.is_empty()) {
			args += ", ";
		}
		args += vformat("%s: %s", argument.name, doc_type(argument));
		if (i >= first_default) {
			args += " = " + doc_value(p_method.default_arguments[i - first_default]);
		}
	}
	return vformat("%s(%s) -> %s", String(p_method.name), args, doc_flow(p_type, p_path, p_method));
}

} // namespace

// Wrap eval source in a main function for immediate execution.
String Cmd::wrap_eval(const String &p_src) {
	String body;
	for (const String &line : p_src.split("\n")) {
		body += "\t" + line + "\n";
	}
	return "func main() -> int:\n" + body + "\treturn 0\n";
}

// Build script from source text.
Ref<Script> Cmd::script_from_source(const String &p_src) {
	for (int i = 0; i < ScriptServer::get_language_count(); i++) {
		ScriptLanguage *lang = ScriptServer::get_language(i);
		if (lang->get_extension() == "gd") {
			Ref<Script> res = Ref<Script>(Object::cast_to<Script>(ClassDB::instantiate(lang->get_type())));
			ERR_FAIL_COND_V(res.is_null(), Ref<Script>());
			res->set_source_code(p_src);
			res->reload();
			return res;
		}
	}
	return Ref<Script>();
}

// List available types and their members.
// Hide internal execution types and expose only script-accessible types.
int Cmd::doc(const String &p_name) {
	// Select locale-specific documentation and resolve links against the runtime's upstream documentation.
	String manual = doc_japanese() ? String::utf8((const char *)gd_manual_ja, sizeof(gd_manual_ja)) : String::utf8((const char *)gd_manual_en, sizeof(gd_manual_en));
	manual = manual.replace(GD_DOCS_NEUTRAL_URL, GODOT_VERSION_DOCS_URL);
	if (p_name.is_empty()) {
		print_line("API entries:");
		print_line("  GD");
		print_line("  GD.web  GD.database");
		print_line("  advanced: GD.database.postgres  GD.database.redis");
		print_line("  results: Err  R");
		print_line("  gd doc GD.file     inspect a gd API");
		print_line("  gd doc SceneTree   inspect a Godot class");
		print_line("  gd doc manual      read the full manual");
		print_line("  gd doc all         list every public class\n");
		// Keep the introduction and first runnable example independent of installation details.
		const int intro = manual.find("\n## ");
		const int next = intro < 0 ? -1 : manual.find("\n## ", intro + 1);
		print_line(next < 0 ? manual : manual.substr(0, next));
		const int quick = manual.find(String::utf8(doc_japanese() ? "\n## クイックスタート\n" : "\n## Quick start\n"));
		if (quick >= 0) {
			const int end = manual.find("\n## ", quick + 1);
			print_line(manual.substr(quick, end < 0 ? -1 : end - quick));
		}
		print_line(String(GODOT_VERSION_DOCS_URL) + "/classes/");
		return EXIT_SUCCESS;
	}
	if (p_name == "manual") {
		print_line(manual);
		return EXIT_SUCCESS;
	}

	if (p_name == "all") {
		LocalVector<StringName> names;
		ClassDB::get_class_list(names);
		for (uint32_t i = 0; i < names.size();) {
			if (!ClassDB::is_class_exposed(names[i])) {
				names.remove_at_unordered(i);
				continue;
			}
			i++;
		}
		names.sort_custom<StringName::AlphCompare>();
		print_line(vformat("%d classes:", names.size()));
		String line;
		for (const StringName &n : names) {
			if (line.length() + String(n).length() > DOC_LINE_MAX) {
				print_line("  " + line);
				line = String();
			}
			line += String(n) + " ";
		}
		if (!line.is_empty()) {
			print_line("  " + line);
		}

		List<Engine::Singleton> singletons;
		Engine::get_singleton()->get_singletons(&singletons);
		LocalVector<StringName> singleton_names;
		for (const Engine::Singleton &singleton : singletons) {
			singleton_names.push_back(singleton.name);
		}
		singleton_names.sort_custom<StringName::AlphCompare>();
		print_line(vformat("%d singletons:", singleton_names.size()));
		line = String();
		for (const StringName &name : singleton_names) {
			if (line.length() + String(name).length() > DOC_LINE_MAX) {
				print_line("  " + line);
				line = String();
			}
			line += String(name) + " ";
		}
		if (!line.is_empty()) {
			print_line("  " + line);
		}
		return EXIT_SUCCESS;
	}

	const PackedStringArray path = p_name.split(".", false);
	const bool is_singleton = path.size() == 1 && Engine::get_singleton()->has_singleton(p_name);
	const bool is_class = path.size() == 1 && ClassDB::class_exists(p_name) && ClassDB::is_class_exposed(p_name);
	const bool root_singleton = !path.is_empty() && Engine::get_singleton()->has_singleton(path[0]);
	const bool root_class = !path.is_empty() && ClassDB::class_exists(path[0]) && ClassDB::is_class_exposed(path[0]);
	StringName type;
	bool is_path = path.size() > 1 && (root_singleton || root_class);
	bool selected_method = false;
	bool selected_has_child = false;
	StringName selected_owner;
	MethodInfo selected;
	if (is_path) {
		type = root_singleton ? Engine::get_singleton()->get_singleton_object(path[0])->get_class_name() : StringName(path[0]);
		for (int i = 1; i < path.size(); i++) {
			const String part = path[i].trim_suffix("()");
			const StringName child = doc_child(type, part);
			MethodInfo method;
			const bool is_method = doc_method(type, part, method);
			if (i == path.size() - 1 && is_method) {
				selected_method = true;
				selected_has_child = child != StringName();
				selected_owner = type;
				selected = method;
				if (selected_has_child) {
					type = child;
				}
				continue;
			}
			type = child;
			if (child == StringName()) {
				is_path = false;
				break;
			}
		}
	}
	if (!is_singleton && !is_class && !is_path) {
		print_error(vformat("\nerror: no such class: %s\n", p_name));
		return EXIT_FAILURE;
	}

	if (!is_path) {
		type = is_singleton ? Engine::get_singleton()->get_singleton_object(p_name)->get_class_name() : StringName(p_name);
	}
	print_line(vformat("%s %s", is_path ? "api" : (is_singleton ? "singleton" : "class"), p_name));
	// Show the authoritative purpose description before the signature.
	const char *brief = doc_brief(p_name);
	if (brief != nullptr) {
		print_line("  " + String::utf8(brief));
	}
	if (selected_method) {
		const int dot = p_name.rfind(".");
		const String owner_path = dot >= 0 ? p_name.substr(0, dot) : p_name;
		print_line("  " + doc_method_line(selected_owner, owner_path, selected));
		if (!selected_has_child) {
			return EXIT_SUCCESS;
		}
		print_line(vformat("  returns %s:", String(type)));
	}
	const StringName parent = ClassDB::get_parent_class(type);
	if (parent != StringName()) {
		print_line(vformat("  extends %s", String(parent)));
	}

	List<PropertyInfo> properties;
	ClassDB::get_property_list(type, &properties, true);
	if (!properties.is_empty()) {
		print_line("  properties:");
		for (const PropertyInfo &property : properties) {
			if (!String(property.name).begins_with("_")) {
				const StringName object = doc_class(property);
				const String shown = doc_internal(object) ? p_name + "." + String(property.name) : doc_type(property);
				print_line(vformat("    %s: %s", property.name, shown));
			}
		}
	}

	List<MethodInfo> methods;
	ClassDB::get_method_list(type, &methods, true);
	if (!methods.is_empty()) {
		print_line("  methods:");
		HashSet<StringName> getters;
		for (const PropertyInfo &property : properties) {
			const StringName getter = ClassDB::get_property_getter(type, property.name);
			if (getter != StringName()) {
				getters.insert(getter);
			}
		}
		LocalVector<String> plain;
		LocalVector<String> group_order;
		HashMap<String, LocalVector<String>> grouped;
		for (const MethodInfo &m : methods) {
			if (String(m.name).begins_with("_") || getters.has(m.name)) {
				continue; // Hide internal members.
			}
			const String line = doc_method_line(type, p_name, m);
			const String group = doc_group(type, m.name);
			if (group.is_empty()) {
				plain.push_back(line);
				continue;
			}
			if (!grouped.has(group)) {
				grouped[group] = LocalVector<String>();
				group_order.push_back(group);
			}
			grouped[group].push_back(line);
		}
		for (const String &line : plain) {
			print_line("    " + line);
		}
		for (const String &group : group_order) {
			print_line("    [" + group + "]");
			for (const String &line : grouped[group]) {
				print_line("      " + line);
			}
		}
	}

	List<String> constants;
	ClassDB::get_integer_constant_list(type, &constants, true);
	if (!constants.is_empty()) {
		print_line("  constants:");
		for (const String &c : constants) {
			print_line(vformat("    %s = %d", c, ClassDB::get_integer_constant(type, c)));
		}
	}
	return EXIT_SUCCESS;
}

// Execute interactive input immediately.
// Rebuild accumulated lines so variable declarations remain available to later input.
int Cmd::repl() {
	print_line("gd repl - .exit to quit, .clear to forget, .show to list");
	List<String> kept; // Previously accepted lines.

	while (true) {
		OS::get_singleton()->print("gd> ");
		const String line = OS::get_singleton()->get_stdin_string().strip_edges();
		// A closed input ends the session like .exit does, instead of prompting forever.
		if (line == ".exit" || line == ".quit" || (line.is_empty() && feof(stdin))) {
			break;
		}
		if (line == ".clear") {
			kept.clear();
			print_line("cleared");
			continue;
		}
		if (line.is_empty()) {
			continue;
		}
		if (line == ".show") {
			for (const String &k : kept) {
				print_line("  " + k);
			}
			continue;
		}

		// Build the accumulated source with the current input line.
		// Wrap expression-like input in print to show its value.
		const bool is_statement = line.begins_with("var ") || line.begins_with("const ") ||
				line.contains("=") || line.begins_with("print(") || line.begins_with("if ") ||
				line.begins_with("for ") || line.begins_with("while ") || line.begins_with("@");
		String body;
		for (const String &k : kept) {
			body += "\t" + k + "\n";
		}
		body += is_statement ? ("\t" + line + "\n") : ("\tprint(" + line + ")\n");

		const String src = "extends RefCounted\n\n\nfunc main() -> int:\n" + body + "\treturn 0\n";
		Ref<Script> res = Cmd::script_from_source(src);
		if (res.is_null() || !res->is_valid()) {
			continue; // The error has already been reported with its location.
		}

		Object *obj = ClassDB::instantiate(res->get_instance_base_type());
		if (!obj) {
			continue;
		}
		Ref<RefCounted> holder = Ref<RefCounted>(Object::cast_to<RefCounted>(obj));
		obj->set_script(res);
		Callable::CallError cerr;
		obj->callp(SNAME("main"), nullptr, 0, cerr);
		if (holder.is_null()) {
			memdelete(obj);
		}
		if (is_statement) {
			kept.push_back(line); // Retain only state-defining lines.
		}
	}
	return EXIT_SUCCESS;
}

// Combine watched paths, times, sizes, and contents into one fingerprint.
uint64_t Cmd::watch_stamp(const String &p_dir) {
	const uint64_t round = ++watch_round;
	const uint64_t now = (uint64_t)GDClock::unix_time();
	const uint64_t sum = watch_tree(p_dir, round, now);
	List<String> gone;
	for (const KeyValue<String, WatchFile> &one : watch_files) {
		if (one.value.seen != round) {
			gone.push_back(one.key);
		}
	}
	for (const String &path : gone) {
		watch_files.erase(path);
	}
	return sum;
}

// Restart execution after each detected change.
int Cmd::watch_loop(const List<String> &p_args, const String &p_dir) {
	OS *os = OS::get_singleton();
	const String self = os->get_executable_path();
	uint64_t last = 0;
	ProcessID child = 0;
	bool first = true;
	RunSignals signals;
	print_line("watching for changes... (Ctrl-C to stop)");
	while (!signals.caught()) {
		const uint64_t now = Cmd::watch_stamp(p_dir);
		if (first || now != last) {
			// Stop the previous child before launching a replacement with the same arguments.
			if (child != 0 && os->is_process_running(child)) {
				os->kill(child);
			}
			child = 0;
			first = false;
			last = now;
			print_line("--- restarting ---");
			if (os->create_process(self, p_args, &child) != OK) {
				ERR_PRINT("cannot start watched process");
				return EXIT_FAILURE;
			}
		} else if (child != 0 && !os->is_process_running(child)) {
			print_line(vformat("child exited with code %d", os->get_process_exit_code(child)));
			child = 0;
		}
		os->delay_usec(300000); // Check every 0.3 seconds.
	}
	if (child != 0 && os->is_process_running(child)) {
		os->kill(child);
	}
	os->set_exit_code(128 + signals.caught());
	return EXIT_SUCCESS;
}

// Start listener workers sharing one port, with connection distribution handled by the kernel.
// The parent supervises children and stops all remaining workers when any exits.
int Cmd::workers_loop(const List<String> &p_args, int p_count) {
	const String self = OS::get_singleton()->get_executable_path();
	RunSignals signals;
	OS::get_singleton()->set_environment("GD_WORKER", "1"); // Prevent child processes from creating another worker group.
	LocalVector<ProcessID> kids;
	for (int i = 0; i < p_count; i++) {
		ProcessID pid = 0;
		if (OS::get_singleton()->create_process(self, p_args, &pid) != OK) {
			break;
		}
		kids.push_back(pid);
	}
	if (kids.size() != (uint32_t)p_count) {
		for (const ProcessID &pid : kids) {
			OS::get_singleton()->kill(pid);
		}
		ERR_PRINT("cannot start workers");
		return EXIT_FAILURE;
	}
#ifdef MACOS_ENABLED
	// Darwin SO_REUSEPORT directs connections to the last listener rather than distributing them.
	// Warn because additional workers would retain memory and connections without serving traffic.
	WARN_PRINT("macOS では同じ港を分け合っても繋ぎが割り振られない。1つの処理しか捌かないため --workers は効かない");
#endif
	WARN_PRINT("GD.web.sessions and GD.web.rate keep process-local state; --workers does not share sessions or rate limits.");
	print_line(vformat("%d workers", (int)kids.size()));
	while (!signals.caught()) {
		for (uint32_t i = 0; i < kids.size(); i++) {
			if (!OS::get_singleton()->is_process_running(kids[i])) {
				const int code = OS::get_singleton()->get_process_exit_code(kids[i]);
				for (uint32_t k = 0; k < kids.size(); k++) {
					if (k != i) {
						OS::get_singleton()->kill(kids[k]);
					}
				}
				if (code < 0) {
					return EXIT_FAILURE;
				}
				OS::get_singleton()->set_exit_code(code);
				return EXIT_SUCCESS;
			}
		}
		OS::get_singleton()->delay_usec(200000); // Check every 0.2 seconds.
	}
	for (const ProcessID &pid : kids) {
		if (OS::get_singleton()->is_process_running(pid)) {
			OS::get_singleton()->kill(pid);
		}
	}
	OS::get_singleton()->set_exit_code(128 + signals.caught());
	return EXIT_SUCCESS;
}

// Print shell-completion definitions for bash or zsh.
int Cmd::completions(const String &p_shell) {
	// Keep command and flag names aligned with is_subcommand and takes_flag.
	const String cmds = "run serve test check fmt eval init task add install remove outdated update search publish info compile completions bench repl doc";
	const String flags = "--mount --strict --allow-net --allow-env --allow-run --allow-ext --allow-sys "
						 "--deny-net --deny-env --deny-run --deny-ext --deny-sys -A --allow-all "
						 "--watch --workers= --no-scene-tree -o --output --check "
						 "-v --verbose -q --quiet --header --no-header "
						 "--latest --frozen --cached-only --dry-run --dump-extension-api --version --help";
	if (p_shell == "zsh") {
		print_line(vformat(
				"#compdef gd\n"
				"_gd() {\n"
				"  local -a cmds flags\n"
				"  cmds=(%s)\n"
				"  flags=(%s)\n"
				"  if (( CURRENT == 2 )); then\n"
				"    _describe 'command' cmds\n"
				"  else\n"
				"    _alternative 'flags:flag:($flags)' 'files:file:_files -g \"*.gd\"'\n"
				"  fi\n"
				"}\n"
				"compdef _gd gd",
				cmds, flags));
		return EXIT_SUCCESS;
	}
	// Default to bash.
	print_line(vformat(
			"_gd() {\n"
			"  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
			"  if [ $COMP_CWORD -eq 1 ]; then\n"
			"    COMPREPLY=( $(compgen -W \"%s\" -- \"$cur\") )\n"
			"  elif [[ \"$cur\" == -* ]]; then\n"
			"    COMPREPLY=( $(compgen -W \"%s\" -- \"$cur\") )\n"
			"  else\n"
			"    COMPREPLY=( $(compgen -f -X '!*.gd' -- \"$cur\") $(compgen -d -- \"$cur\") )\n"
			"  fi\n"
			"}\n"
			"complete -F _gd gd",
			cmds, flags));
	return EXIT_SUCCESS;
}

// Share self-invocation argument construction between watch and worker modes.
// Remove their triggering flags so children do not recursively invoke the same mode.
List<String> Cmd::child_args(const String &p_drop) {
	List<String> child;
	bool user = false;
	for (const String &a : Cmd::raw) {
		if (user || !a.begins_with(p_drop)) {
			child.push_back(a);
		}
		if (a == "--" || a == "++") {
			user = true;
		}
	}
	return child;
}

// Read gd.json only at the jail root; parent traversal is not permitted.
// The embedded package script uses the same location.
String Cmd::find_config() {
	const String at = "res://gd.json";
	return FileAccess::exists(at) ? at : String();
}

// Read gd.json, returning an empty dictionary when absent.
Dictionary Cmd::load_config() {
	const String path = Cmd::find_config();
	if (path.is_empty()) {
		return Dictionary();
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		return Dictionary();
	}
	JSON json;
	if (json.parse(f->get_as_text()) != OK) {
		ERR_PRINT(vformat("gd.json is not valid JSON: %s", json.get_error_message()));
		return Dictionary();
	}
	const Variant data = json.get_data();
	return data.get_type() == Variant::DICTIONARY ? Dictionary(data) : Dictionary();
}

// Locate package copies beside gd.json.
String Cmd::pkg_dir() {
	const String cfg = Cmd::find_config();
	const String base = cfg.is_empty() ? String(".") : cfg.get_base_dir();
	return base.path_join("pkg");
}

// Map strictness settings to warning levels.
// Keep Variant diagnostics as warnings and reject discarded failures in strict mode.
void Cmd::apply_types() {
	const char *types[] = {
		"untyped_declaration", "unsafe_property_access", "unsafe_method_access",
		"unsafe_cast", "unsafe_call_argument", "inference_on_variant", nullptr
	};
	const char *discard[] = { "error_value_discarded", nullptr };
	// Suppress diagnostics that do not apply to this execution model.
	// integer_division permits intentional integer arithmetic.
	// inferred_declaration accepts inferred typed declarations such as var x := 1.
	const char *quiet[] = { "integer_division", "inferred_declaration", nullptr };
	// Warn about standalone asynchronous calls because they may intentionally run fire-and-forget.
	const char *safety[] = { "missing_await", nullptr };

	// Warning levels: 0 ignores, 1 warns, and 2 rejects.
	const int for_types = strict ? 1 : 0;
	const int for_discard = strict ? 2 : 1;
	const int for_safety = 1;

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return;
	}
	for (int i = 0; types[i]; i++) {
		ps->set_setting(String("debug/gdscript/warnings/") + types[i], for_types);
	}
	for (int i = 0; quiet[i]; i++) {
		ps->set_setting(String("debug/gdscript/warnings/") + quiet[i], 0);
	}
	for (int i = 0; discard[i]; i++) {
		ps->set_setting(String("debug/gdscript/warnings/") + discard[i], for_discard);
	}
	for (int i = 0; safety[i]; i++) {
		ps->set_setting(String("debug/gdscript/warnings/") + safety[i], for_safety);
	}
}

// Check whether a subcommand belongs to the embedded dependency-management script.
bool Cmd::is_pkg_cmd(const String &p_cmd) {
	static const char *cmds[] = { "install", "add", "remove", "outdated", "update", "search", "publish", nullptr };
	for (int i = 0; cmds[i]; i++) {
		if (p_cmd == cmds[i]) {
			return true;
		}
	}
	return false;
}

// List installed dependencies.
int Cmd::info() {
	const Dictionary cfg = Cmd::load_config();
	const String path = Cmd::find_config();
	print_line(vformat("config: %s", path.is_empty() ? String("(none)") : path));
	if (cfg.has("name")) {
		print_line(vformat("name:   %s", String(cfg["name"])));
	}
	const Dictionary imports = cfg.has("imports") ? Dictionary(cfg["imports"]) : Dictionary();
	print_line(vformat("imports: %d", imports.size()));
	for (const Variant &k : imports.keys()) {
		// Probe through the alias so cached, copied, and local packages report alike.
		const bool present = DirAccess::exists("pkg://" + String(k));
		print_line(vformat("  %s <- %s %s", String(k), String(imports[k]), present ? "[ok]" : "[missing]"));
	}
	// List every pinned registry package, including those only other packages need.
	Ref<FileAccess> lock = FileAccess::open("res://gd.lock", FileAccess::READ);
	JSON json;
	if (lock.is_valid() && json.parse(lock->get_as_text()) == OK && json.get_data().get_type() == Variant::DICTIONARY) {
		const Dictionary data = json.get_data();
		const Dictionary packages = data.has("packages") ? Dictionary(data["packages"]) : Dictionary();
		print_line(vformat("packages: %d", packages.size()));
		for (const Variant &k : packages.keys()) {
			const String id = String(k);
			if (!PkgScope::is_id(id)) {
				continue;
			}
			const Dictionary entry = packages[k];
			const Dictionary deps = entry.has("imports") ? Dictionary(entry["imports"]) : Dictionary();
			String needs;
			for (const Variant &a : deps.keys()) {
				needs += (needs.is_empty() ? String(" needs ") : String(", ")) + String(a) + "=" + String(deps[a]);
			}
			print_line(vformat("  %s %s%s", id, DirAccess::exists("pkg://" + id) ? "[ok]" : "[missing]", needs));
		}
	}
	return EXIT_SUCCESS;
}

// Write one file unless it exists already.
static void _seed(const String &p_path, const String &p_text) {
	if (FileAccess::exists(p_path)) {
		return;
	}
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(p_text);
	}
}

// Start a project, or a package when given @scope/name: a package is a project of its own,
// with mod.gd as its entry, so it is written, tested, and published from here.
// An editor project reads only res://, so it keeps packages under pkg/ and commits them.
int Cmd::init_project(const String &p_name) {
	if (FileAccess::exists("gd.json")) {
		print_line("gd.json already exists");
		return EXIT_FAILURE;
	}
	const bool package = p_name.begins_with("@");
	if (package && (p_name.get_slice_count("/") != 2 || p_name.get_slicec('/', 1).is_empty())) {
		print_error("a package name is @scope/name");
		return EXIT_FAILURE;
	}
	const bool godot = FileAccess::exists("project.godot");
	Ref<FileAccess> f = FileAccess::open("gd.json", FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), EXIT_FAILURE, "Cannot write gd.json.");
	if (package) {
		f->store_string(vformat(R"({
  "name": "%s",
  "version": "0.1.0",
  "description": "",
  "main": "mod.gd",
  "tasks": {
    "test": "test tests"
  },
  "imports": {}
}
)",
				p_name));
	} else {
		f->store_string(vformat(R"({
  "name": "%s",
  "version": "0.1.0",
  "tasks": {
    "run": "main.gd",
    "test": "test tests"
  },%s
  "imports": {}
}
)",
				p_name.is_empty() ? String("my-tool") : p_name, godot ? String("\n  \"place\": \"project\",") : String()));
	}
	f->close();

	if (!godot) {
		// Copies under pkg/ are rebuilt from gd.lock; the lock is what a checkout needs.
		// An editor team commits pkg/ instead, so every member can open the project.
		_seed(".gitignore", "pkg/\n");
	}
	if (package) {
		const String leaf = p_name.get_slicec('/', 1).replace("-", "_");
		_seed("mod.gd", String::utf8("# Package entry: what consumers preload through their alias for this package.\n# res:// means this package's own root, here and after installation.\nextends RefCounted\n\n\nstatic func hello(who: String) -> String:\n\treturn \"hello, %s\" % who\n"));
		DirAccess::make_dir_recursive_absolute("tests");
		_seed("tests/" + leaf + "_test.gd", String::utf8("extends RefCounted\n\nconst Mod = preload(\"../mod.gd\")\n\n\nfunc main() -> int:\n\tassert(Mod.hello(\"gd\") == \"hello, gd\")\n\tprint(\"ok\")\n\treturn 0\n"));
		print_line(vformat("created package %s; publish with gd publish", p_name));
	} else {
		_seed("main.gd", String::utf8("# Script entry point with required type annotations.\nextends RefCounted\n\n\nfunc main() -> int:\n\tprint(\"hello\")\n\treturn 0\n"));
		print_line("created gd.json");
	}
	return EXIT_SUCCESS;
}

// Run a task defined in gd.json.
int Cmd::run_task(const String &p_name) {
	const Dictionary cfg = Cmd::load_config();
	if (!cfg.has("tasks")) {
		print_error("no tasks in gd.json");
		return EXIT_FAILURE;
	}
	const Dictionary tasks = cfg["tasks"];
	if (p_name.is_empty()) {
		print_line("tasks:");
		for (const Variant &k : tasks.keys()) {
			print_line(vformat("  %s: %s", String(k), String(tasks[k])));
		}
		return EXIT_SUCCESS;
	}
	if (!tasks.has(p_name)) {
		print_error(vformat("no such task \"%s\"", p_name));
		return EXIT_FAILURE;
	}

	List<String> args;
	// Forward strict, mount, allow, and deny flags without changing the parent's permission boundary.
	for (const String &flag : Cmd::flags) {
		args.push_back(flag);
	}
	// Forward quiet mode while preserving standard error output.
	if (!CoreGlobals::print_line_enabled) {
		args.push_back("-q");
	}
	bool user_args = false;
	for (const String &part : String(tasks[p_name]).split(" ", false)) {
		// Only the invoking caller may grant strict permissions; project configuration cannot expand them.
		if (!user_args && Cmd::strict && task_grants_permission(part)) {
			print_error("a task cannot grant permissions under --strict; pass them to gd explicitly");
			return EXIT_FAILURE;
		}
		args.push_back(part);
		user_args = user_args || part == "--" || part == "++";
	}
	// Connect all three standard streams directly to the child for streaming I/O.
	int code = EXIT_FAILURE;
	const Error err = OS::get_singleton()->execute(OS::get_singleton()->get_executable_path(), args, nullptr, &code, false, nullptr, false, true);
	return (err == OK && code == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Collect .gd files matching a suffix, recursively traversing directories.
void Cmd::collect(const String &p_path, List<String> &r_files, const String &p_suffix) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		// Collect only existing files and reject nonexistent paths.
		if (p_path.ends_with(".gd") && FileAccess::exists(p_path)) {
			r_files.push_back(p_path);
		}
		return;
	}
	Error err = dir->list_dir_begin();
	if (err != OK) {
		return;
	}
	String name = dir->get_next();
	while (!name.is_empty()) {
		if (!name.begins_with(".")) {
			const String full = p_path.path_join(name);
			if (dir->current_is_dir()) {
				Cmd::collect(full, r_files, p_suffix);
			} else if (name.ends_with(p_suffix)) {
				r_files.push_back(full);
			}
		}
		name = dir->get_next();
	}
	dir->list_dir_end();
	return;
}

// Collect *_bench.gd files, run repeated samples, and report their durations.
int Cmd::run_bench(const String &p_path, const List<String> &p_flags, int p_runs) {
	List<String> files;
	if (!Cmd::collect_or_fail(p_path, "_bench.gd", files)) {
		return EXIT_FAILURE;
	}

	const String self = OS::get_singleton()->get_executable_path();
	for (const String &file : files) {
		List<String> args;
		for (const String &f : p_flags) {
			args.push_back(f);
		}
		args.push_back(file);

		uint64_t best = UINT64_MAX;
		uint64_t total = 0;
		int failed = 0;
		for (int i = 0; i < p_runs; i++) {
			String output;
			int code = 0;
			const uint64_t t0 = GDClock::usec();
			const Error err = OS::get_singleton()->execute(self, args, &output, &code, true);
			const uint64_t dt = GDClock::usec() - t0;
			if (err != OK || code != 0) {
				failed++;
				continue;
			}
			best = MIN(best, dt);
			total += dt;
		}
		if (failed == p_runs) {
			print_line(vformat("FAIL  %s", file));
			continue;
		}
		const int ok_runs = p_runs - failed;
		print_line(vformat("%-40s best %7.1f ms  avg %7.1f ms  (%d runs)",
				file, best / 1000.0, (total / (double)ok_runs) / 1000.0, ok_runs));
	}
	return EXIT_SUCCESS;
}

// Collect and sort files, reporting an error when none match.
bool Cmd::collect_or_fail(const String &p_path, const String &p_suffix, List<String> &r_files) {
	Cmd::collect(p_path.is_empty() ? String(".") : p_path, r_files, p_suffix);
	r_files.sort();
	if (r_files.is_empty()) {
		print_error(vformat("no *%s found", p_suffix));
		return false;
	}
	return true;
}

// Run collected scripts in separate child processes and aggregate results.
// Share execution and checking infrastructure, varying only file selection,
// subcommand insertion, successful-file output, and final counts.
int Cmd::run_each(const String &p_path, const List<String> &p_flags,
		const char *p_suffix, const char *p_extra_cmd, bool p_print_ok, const char *p_tally) {
	List<String> files;
	if (!Cmd::collect_or_fail(p_path, p_suffix, files)) {
		return EXIT_FAILURE;
	}

	const String self = OS::get_singleton()->get_executable_path();
	int passed = 0;
	int failed = 0;
	for (const String &file : files) {
		List<String> args;
		for (const String &f : p_flags) {
			args.push_back(f);
		}
		if (p_extra_cmd != nullptr) {
			args.push_back(p_extra_cmd);
		}
		args.push_back(file);

		String output;
		int code = 0;
		const Error err = OS::get_singleton()->execute(self, args, &output, &code, true);
		if (err == OK && code == 0) {
			if (p_print_ok) {
				print_line(vformat("ok    %s", file));
			}
			passed++;
		} else {
			print_line(vformat("FAIL  %s", file));
			print_line(output.strip_edges());
			failed++;
		}
	}
	if (p_print_ok) {
		print_line("---");
		print_line(vformat("pass=%d fail=%d", passed, failed));
	} else {
		print_line(vformat("%s=%d fail=%d", p_tally, passed + failed, failed));
	}
	return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Run discovered tests.
int Cmd::run_tests(const String &p_path, const List<String> &p_flags) {
	return Cmd::run_each(p_path, p_flags, "_test.gd", nullptr, true, nullptr);
}

// Check each .gd file beneath a path and fail when none pass.
int Cmd::run_checks(const String &p_path, const List<String> &p_flags) {
	return Cmd::run_each(p_path, p_flags, ".gd", "check", false, "checked");
}

// Check whether a positional argument names a subcommand.
bool Cmd::is_subcommand(const String &p_arg) {
	static const char *cmds[] = { "run", "check", "eval", "serve", "test", "init", "task", "install", "add", "remove", "outdated", "update", "search", "publish", "info", "fmt", "compile", "completions", "bench", "repl", "doc", nullptr };
	for (int i = 0; cmds[i]; i++) {
		if (p_arg == cmds[i]) {
			return true;
		}
	}
	return false;
}

// Define the single supported-flag list and keep help output aligned with it.
// Reject unlisted runtime flags rather than silently forwarding undocumented options.
bool Cmd::takes_flag(const String &p_arg) {
	static const char *exact[] = {
		"-h", "--help", "--version", "--header", "--no-header", "-v", "--verbose", "-q", "--quiet",
		"--dump-extension-api",
		"--mount", "--strict", "-A", "--allow-all",
		"--watch", "--no-scene-tree", "-o", "--output", "--check",
		"--latest", "--frozen", "--cached-only", "--dry-run", "--sync", // Options consumed only by package management.
		"--", "++", // Following arguments belong to the script.
		nullptr
	};
	for (int i = 0; exact[i]; i++) {
		if (p_arg == exact[i]) {
			return true;
		}
	}
	static const char *heads[] = { "--mount=", "--workers=", nullptr }; // Flags whose values follow an equals sign.
	for (int i = 0; heads[i]; i++) {
		if (p_arg.begins_with(heads[i])) {
			return true;
		}
	}
	// Accept allow and deny flags with optional resource scopes.
	static const char *kinds[] = { "net", "env", "run", "ext", "sys", nullptr };
	for (int i = 0; kinds[i]; i++) {
		for (int deny = 0; deny < 2; deny++) {
			const String head = (deny ? "--deny-" : "--allow-") + String(kinds[i]);
			if (p_arg == head || p_arg.begins_with(head + "=")) {
				return true;
			}
		}
	}
	return false;
}
