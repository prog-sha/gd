/**************************************************************************/
/*  cli.cpp                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement command-line, logging, and network utilities declared in cli.h.

#include "cli/api/cli.h"
#include "cli/net/address.h"

#include "cli/sys/file_job.h"

#include "cli/api/gen.h"
#include "cli/api/text.h"
#include "cli/data/json.h"
#include "cli/net/lookup.h"
#include "cli/net/socket.h"
#include "cli/sys/os.h"
#include "cli/sys/limit.h"
#include "cli/sys/perm.h"
#include "cli/sys/task.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/string/string_builder.h"
#include "core/templates/list.h"

#include <cstdio>

namespace {

List<Ref<GDLogCall>> log_calls; // Main-thread-owned log queue in submission order.

// Write one line to the terminal and optional file from the ordered worker.
Ref<R> write_log_lines(bool p_err, const String &p_console, const String &p_path, const String &p_body) {
	const CharString raw = p_console.utf8();
	FILE *stream = p_err ? stderr : stdout;
	const size_t wrote = fwrite(raw.get_data(), 1, raw.length(), stream);
	if (wrote != size_t(raw.length()) || fflush(stream) != 0) {
		return R::err("log output failed");
	}
	return p_path.is_empty() ? R::ok() : Os::append_text(p_path, p_body);
}

// Convert only supported textual forms to booleans.
bool flag_bool_of(const String &p_text, bool &r_value) {
	if (p_text == "1" || p_text == "t" || p_text == "T" || p_text == "TRUE" || p_text == "true" || p_text == "True") {
		r_value = true;
		return true;
	}
	if (p_text == "0" || p_text == "f" || p_text == "F" || p_text == "FALSE" || p_text == "false" || p_text == "False") {
		r_value = false;
		return true;
	}
	return false;
}

// Return an ASCII digit valid in the selected radix.
int flag_digit(char32_t p_char) {
	if (p_char >= '0' && p_char <= '9') {
		return p_char - '0';
	}
	if (p_char >= 'a' && p_char <= 'f') {
		return p_char - 'a' + 10;
	}
	if (p_char >= 'A' && p_char <= 'F') {
		return p_char - 'A' + 10;
	}
	return -1;
}

// Parse prefixed integers with separators within the signed 64-bit range.
bool flag_int_of(const String &p_text, int64_t &r_value) {
	if (p_text.is_empty()) {
		return false;
	}
	int at = 0;
	bool negative = false;
	if (p_text[at] == '+' || p_text[at] == '-') {
		negative = p_text[at] == '-';
		if (++at >= p_text.length()) {
			return false;
		}
	}
	int base = 10;
	bool prefix = false;
	if (p_text[at] == '0' && at + 1 < p_text.length()) {
		const char32_t mark = p_text[at + 1];
		if (mark == 'x' || mark == 'X') {
			base = 16;
			at += 2;
			prefix = true;
		} else if (mark == 'b' || mark == 'B') {
			base = 2;
			at += 2;
			prefix = true;
		} else if (mark == 'o' || mark == 'O') {
			base = 8;
			at += 2;
			prefix = true;
		} else {
			base = 8;
			at++;
			prefix = true;
		}
	}
	uint64_t value = 0;
	bool digit_seen = false;
	bool underscore = false;
	const uint64_t max = negative ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX); // Largest magnitude permitted for each sign.
	for (; at < p_text.length(); at++) {
		const char32_t c = p_text[at];
		if (c == '_') {
			if (underscore || (!digit_seen && !prefix) || at + 1 >= p_text.length()) {
				return false;
			}
			underscore = true;
			continue;
		}
		const int digit = flag_digit(c);
		if (digit < 0 || digit >= base || (underscore && !digit_seen && !prefix)) {
			return false;
		}
		if (value > (max - digit) / base) {
			return false;
		}
		value = value * base + digit;
		digit_seen = true;
		underscore = false;
		prefix = false;
	}
	if (!digit_seen || underscore) {
		return false;
	}
	r_value = negative ? value == uint64_t(INT64_MAX) + 1 ? INT64_MIN : -(int64_t)value : (int64_t)value;
	return true;
}

} // namespace

// ---------------- Command-line flags ----------------

void GDCLIFlags::set_name(const String &p_name) {
	name = p_name;
}

// Declare a boolean command-line flag.
void GDCLIFlags::flag_bool(const String &p_name, bool p_fallback, const String &p_help) {
	Def d;
	d.type = Variant::BOOL;
	d.fallback = p_fallback;
	d.help = p_help;
	defs.insert(p_name, d);
}

// Declare a string command-line flag.
void GDCLIFlags::flag_str(const String &p_name, const String &p_fallback, const String &p_help) {
	Def d;
	d.type = Variant::STRING;
	d.fallback = p_fallback;
	d.help = p_help;
	defs.insert(p_name, d);
}

// Declare an integer command-line flag.
void GDCLIFlags::flag_int(const String &p_name, int64_t p_fallback, const String &p_help) {
	Def d;
	d.type = Variant::INT;
	d.fallback = p_fallback;
	d.help = p_help;
	defs.insert(p_name, d);
}

// Parse supplied arguments against declared flags.
Ref<R> GDCLIFlags::parse(const Array &p_args) {
	values = Dictionary();
	rest = PackedStringArray();
	int i = 0;
	while (i < p_args.size()) {
		const String a = p_args[i];
		if (!a.begins_with("-")) {
			rest.push_back(a);
			i++;
			continue;
		}
		if (a == "--") { // Treat remaining arguments as positional values.
			for (int j = i + 1; j < p_args.size(); j++) {
				rest.push_back(p_args[j]);
			}
			break;
		}

		const String body = a.trim_prefix("--").trim_prefix("-");
		String key = body;
		String inline_val;
		bool has_inline = false;
		const int eq = body.find_char('=');
		if (eq >= 0) {
			key = body.substr(0, eq);
			inline_val = body.substr(eq + 1);
			has_inline = true;
		}

		HashMap<String, Def>::ConstIterator found = defs.find(key);
		if (!found) {
			return R::err(vformat("unknown flag \"--%s\"", key), Err::INVALID_DATA);
		}
		if (found->value.type == Variant::BOOL) {
			bool value = true;
			if (has_inline && !flag_bool_of(inline_val, value)) {
				return R::err(vformat("invalid boolean value \"%s\" for flag \"--%s\"", inline_val, key), Err::INVALID_DATA);
			}
			values[key] = value;
			i++;
			continue;
		}

		String raw = inline_val;
		if (!has_inline) {
			if (i + 1 >= p_args.size()) {
				return R::err(vformat("flag \"--%s\" needs a value", key), Err::INVALID_DATA);
			}
			raw = p_args[i + 1];
			i++;
		}
		i++;
		if (found->value.type == Variant::INT) {
			int64_t value = 0;
			if (!flag_int_of(raw, value)) {
				return R::err(vformat("invalid value \"%s\" for flag \"--%s\"", raw, key), Err::INVALID_DATA);
			}
			values[key] = value;
		} else {
			values[key] = raw;
		}
	}
	return R::ok();
}

// Return a boolean flag's current value.
bool GDCLIFlags::get_bool(const String &p_name) const {
	HashMap<String, Def>::ConstIterator found = defs.find(p_name);
	return values.get(p_name, found ? found->value.fallback : Variant(false));
}

// Return a string flag's current value.
String GDCLIFlags::get_str(const String &p_name) const {
	HashMap<String, Def>::ConstIterator found = defs.find(p_name);
	return values.get(p_name, found ? found->value.fallback : Variant(""));
}

// Return an integer flag's current value.
int64_t GDCLIFlags::get_int(const String &p_name) const {
	HashMap<String, Def>::ConstIterator found = defs.find(p_name);
	return values.get(p_name, found ? found->value.fallback : Variant(0));
}

// Build usage text for registered flags.
String GDCLIFlags::usage() const {
	PackedStringArray names;
	for (const KeyValue<String, Def> &kv : defs) {
		names.push_back(kv.key);
	}
	names.sort();
	String out = vformat("usage: %s [flags] [args]\n", name);
	for (const String &n : names) {
		const Def &d = defs[n];
		out += vformat(String::utf8("  --%-14s %s (既定 %s)\n"), n, d.help, d.fallback);
	}
	return out;
}

// Register public script methods and properties.
void GDCLIFlags::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_name", "name"), &GDCLIFlags::set_name);
	ClassDB::bind_method(D_METHOD("flag_bool", "name", "fallback", "help"), &GDCLIFlags::flag_bool, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("flag_str", "name", "fallback", "help"), &GDCLIFlags::flag_str, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("flag_int", "name", "fallback", "help"), &GDCLIFlags::flag_int, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("parse", "args"), &GDCLIFlags::parse);
	ClassDB::bind_method(D_METHOD("get_bool", "name"), &GDCLIFlags::get_bool);
	ClassDB::bind_method(D_METHOD("get_str", "name"), &GDCLIFlags::get_str);
	ClassDB::bind_method(D_METHOD("get_int", "name"), &GDCLIFlags::get_int);
	ClassDB::bind_method(D_METHOD("get_rest"), &GDCLIFlags::get_rest);
	ClassDB::bind_method(D_METHOD("usage"), &GDCLIFlags::usage);
}

// ---------------- Logging ----------------

// Reserve submission order before scheduling CPU formatting.
Signal GDLogCall::start(std::function<Ref<R>()> p_format, std::function<Ref<R>(const Ref<R> &)> p_write) {
	if (Pool::is_stopping(true) || Pool::is_stopping(false, true)) {
		return Async::ready(R::err("worker pool stopped", Err::INTERRUPTED));
	}
	Ref<GDLogCall> call;
	call.instantiate();
	call->self_hold = call;
	call->writer = std::move(p_write);
	log_calls.push_back(call);
	const Signal signal(call.ptr(), "finished");
	// Represent flush barriers as worker jobs so shutdown also delivers their completion.
	if (!p_format) {
		p_format = []() { return R::ok(); };
	}
	GDFileCall::start(std::move(p_format), true).connect(callable_mp(call.ptr(), &GDLogCall::prepared), Object::CONNECT_ONE_SHOT);
	return signal;
}

// Retain results to restore submission order after out-of-order CPU completion.
void GDLogCall::prepared(const Ref<R> &p_value) {
	value = p_value;
	drain();
}

// Transfer only a contiguous formatted prefix to the write queue.
void GDLogCall::drain() {
	while (!log_calls.is_empty() && log_calls.front()->get()->value.is_valid()) {
		Ref<GDLogCall> call = log_calls.front()->get();
		log_calls.pop_front();
		if (Pool::is_stopping(false, true)) {
			call->written(R::err("worker pool stopped", Err::INTERRUPTED));
			continue;
		}
		const Ref<R> value = call->value;
		auto writer = std::move(call->writer);
		GDFileCall::start([value, writer]() {
			return value->get_ok() && writer ? writer(value) : value;
		}, false, true).connect(callable_mp(call.ptr(), &GDLogCall::written), Object::CONNECT_ONE_SHOT);
	}
}

// Deliver the write result once and release the line.
void GDLogCall::written(const Ref<R> &p_result) {
	Ref<GDLogCall> keep(this);
	value.unref();
	writer = nullptr;
	self_hold.unref();
	emit_signal("finished", p_result);
}

// Register the signal carrying the caller's write result.
void GDLogCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Wait through the current submission boundary, including formatting in progress.
Signal LogState::flush() {
	return GDLogCall::start(nullptr, nullptr);
}

// Parse severity names, treating unknown names as INFO.
LogState::Level LogState::level_of(const String &p_name) {
	const String up = p_name.to_upper();
	if (up == "DEBUG") {
		return DEBUG;
	}
	if (up == "WARN") {
		return WARN;
	}
	if (up == "ERROR") {
		return ERROR;
	}
	return INFO;
}

// Initialize the logger's name and severity.
void LogState::setup(const String &p_name, Level p_level) {
	name = p_name;
	level = p_level;
}

// Set the log output file.
void LogState::set_file(const String &p_path) {
	to_file = p_path;
}

// Keep each record on one line by escaping newlines and control characters.
// Otherwise external values could inject convincing records with forged severity or labels.
String LogState::flat(const String &p_s) {
	StringBuilder out;
	int start = 0;
	for (int i = 0; i < p_s.length(); i++) {
		const char32_t c = p_s[i];
		String escaped;
		if (c == '\n') {
			escaped = "\\n";
		} else if (c == '\r') {
			escaped = "\\r";
		} else if (c == '\\') {
			escaped = "\\\\";
		} else if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) {
			escaped = vformat("\\x%02x", (int)c);
		} else if (c == 0x2028 || c == 0x2029) {
			escaped = vformat("\\u%04x", (int)c);
		} else {
			continue;
		}
		out += p_s.substr(start, i - start);
		out += escaped;
		start = i + 1;
	}
	if (start == 0) {
		return p_s;
	}
	out += p_s.substr(start);
	return out.as_string();
}

// Format values for the script logging API using terminal-display conventions.
String LogState::format(Level p_at, const String &p_msg, const Variant &p_extra) const {
	const int64_t time = with_time ? Datetime::now() : 0;
	String extra;
	if (p_extra.get_type() != Variant::NIL) {
		const Ref<R> encoded = JsonData::encode(p_extra);
		if (encoded->get_ok()) {
			const PackedByteArray raw = encoded->get_v();
			extra = String::utf8((const char *)raw.ptr(), raw.size());
		} else {
			extra = String(p_extra);
		}
	}
	return _line(p_at, p_msg, extra, with_color, time);
}

// Build one log line without terminal color escapes in file output.
// Do not reintroduce control characters after sanitizing caller-provided values.
String LogState::_line(Level p_at, const String &p_msg, const String &p_extra, bool p_color, int64_t p_time) const {
	String tag;
	switch (p_at) {
		case DEBUG:
			tag = "DEBUG";
			break;
		case WARN:
			tag = "WARN";
			break;
		case ERROR:
			tag = "ERROR";
			break;
		default:
			tag = "INFO";
			break;
	}
	if (p_color) {
		if (p_at >= ERROR) {
			tag = Text::paint(tag, 31, true);
		} else if (p_at >= WARN) {
			tag = Text::paint(tag, 33, true);
		} else if (p_at <= DEBUG) {
			tag = Text::paint(tag, 90, true);
		}
	}
	PackedStringArray parts;
	if (with_time) {
		parts.push_back(Datetime::format(p_time, "HH:mm:ss"));
	}
	parts.push_back(tag);
	if (!name.is_empty()) {
		parts.push_back("[" + flat(name) + "]");
	}
	parts.push_back(flat(p_msg));
	if (!p_extra.is_empty()) {
		parts.push_back(flat(p_extra));
	}
	return String(" ").join(parts);
}

// Output one log record at the selected severity.
Signal LogState::emit(Level p_at, const String &p_msg, const Variant &p_extra) {
	if (p_at < level) {
		return Async::ready(R::ok());
	}
	const bool err = p_at >= WARN;
	const bool color = with_color && (err ? Text::stderr_tty() : Text::stdout_tty());
	const String path = to_file;
	const LogState state = *this;
	const int64_t time = with_time ? Datetime::now() : 0;
	// Capture time and settings at submission, then separate CPU formatting from I/O writing.
	return GDLogCall::start([state, p_at, p_msg, p_extra, path, time, color]() {
		String extra;
		if (p_extra.get_type() != Variant::NIL) {
			const Ref<R> encoded = JsonData::encode(p_extra);
			if (!encoded->get_ok()) {
				return encoded;
			}
			const PackedByteArray raw = encoded->get_v();
			extra = String::utf8((const char *)raw.ptr(), raw.size());
		}
		const String console = state._line(p_at, p_msg, extra, color, time) + "\n";
		const String body = path.is_empty() ? String() : color ? state._line(p_at, p_msg, extra, false, time) + "\n" : console;
		return R::ok(PackedStringArray{ console, body });
	}, [err, path](const Ref<R> &p_value) {
		const PackedStringArray lines = p_value->get_v();
		return write_log_lines(err, lines[0], path, lines[1]);
	});
}

// Write a debug-level record.
Signal LogState::debug(const String &p_msg, const Variant &p_extra) {
	return emit(DEBUG, p_msg, p_extra);
}

// Write an info-level record.
Signal LogState::info(const String &p_msg, const Variant &p_extra) {
	return emit(INFO, p_msg, p_extra);
}

// Write a warning-level record.
Signal LogState::warn(const String &p_msg, const Variant &p_extra) {
	return emit(WARN, p_msg, p_extra);
}

// Write an error-level record.
Signal LogState::error(const String &p_msg, const Variant &p_extra) {
	return emit(ERROR, p_msg, p_extra);
}

// Choose log severity from the result's success or failure.
Signal LogState::result(const Ref<R> &p_r, const String &p_msg) {
	if (p_r.is_valid() && p_r->get_e().is_valid()) {
		return emit(ERROR, vformat("%s: %s", p_msg, p_r->get_e()->text()), Variant());
	}
	return Async::ready(R::ok());
}

// Write through a named logger at the selected severity.
Signal LogState::write(Level p_at, const String &p_msg, const Variant &p_extra) {
	return emit(p_at, p_msg, p_extra);
}

// ---------------- Network utilities ----------------

bool Net::is_free(int64_t p_port, const String &p_host) {
	if (p_port < Limit::PORT_MIN || p_port > Limit::PORT_MAX) {
		return false;
	}
	const Ref<R> opened = GDTCPListener::listen(p_host, p_port);
	if (!opened->get_ok()) {
		return false;
	}
	const Ref<GDTCPListener> srv = opened->get_v();
	srv->close();
	return true;
}

// Find an available TCP port.
Ref<R> Net::free_port(int64_t p_from, const String &p_host) {
	if (p_from < 0 || p_from > Limit::PORT_MAX) {
		return R::err(vformat("invalid port %d", p_from), Err::INVALID_DATA);
	}
	if (p_from == 0) {
		// Pass port zero to let the kernel choose an unused port.
		const Ref<R> opened = GDTCPListener::listen(p_host, 0);
		if (!opened->get_ok()) {
			return opened;
		}
		const Ref<GDTCPListener> srv = opened->get_v();
		const int port = srv->addr().get("port", 0);
		srv->close();
		return R::ok(port);
	}
	for (int at = int(p_from); at <= Limit::PORT_MAX; at++) {
		if (is_free(at, p_host)) {
			return R::ok(at);
		}
	}
	return R::err(vformat("no free port from %d", p_from), Err::NOT_FOUND);
}

// Return local network addresses.
Ref<R> Net::local_addresses() {
	// Require system permission because interface addresses reveal host metadata.
	GD_PERM_FAIL_V(SYS, "networkInterfaces", R::err("local address access is denied", Err::PERMISSION_DENIED));
	return GDAddress::local();
}

// Check whether a string is an IP literal.
//
// Reject leading-zero decimal octets such as 0177.0.0.1.
// Some resolvers interpret them as octal, producing different destinations from the same spelling.
bool Net::is_ip(const String &p_text) {
	if (!p_text.is_valid_ip_address()) {
		return false;
	}
	// Embedded IPv4 in IPv6 also ends with a dotted decimal quad after the final colon.
	const int colon = p_text.rfind_char(':');
	const String quad = colon < 0 ? p_text : p_text.substr(colon + 1);
	if (!quad.contains(".")) {
		return true;
	}
	for (const String &part : quad.split(".")) {
		if (part.length() > 1 && part[0] == '0') {
			return false;
		}
	}
	return true;
}

// Split host and port safely.
Dictionary Net::split_host(const String &p_text, int64_t p_default_port) {
	// Delegate to Url::split_host so parsing rules cannot diverge.
	return Url::split_host(p_text, p_default_port);
}
