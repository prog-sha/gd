/**************************************************************************/
/*  cli.h                                                                 */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Provide command-line, logging, and networking utilities.
// Parse flags, record diagnostics, and inspect network settings.

#pragma once

#include "cli/sys/std.h"

#include <functional>

class GDLogAPI;

// Preserve log submission order across CPU formatting and report completed writes.
class GDLogCall : public RefCounted {
	GDCLASS(GDLogCall, RefCounted);

	Ref<GDLogCall> self_hold; // Retain the call until writing completes.
	Ref<R> value; // CPU-formatted line or formatting failure.
	std::function<Ref<R>(const Ref<R> &)> writer; // Writer executed on the ordered I/O queue.

	void prepared(const Ref<R> &p_value); // Return formatted results to submission order.
	void written(const Ref<R> &p_result); // Deliver the write result and release ownership.
	static void drain(); // Send consecutive formatted entries to ordered I/O.

protected:
	static void _bind_methods();

public:
	// Reserve order before CPU formatting; an empty formatter acts as a barrier for prior logs.
	static Signal start(std::function<Ref<R>()> p_format, std::function<Ref<R>(const Ref<R> &)> p_write);
};

// Accept declared command-line flags and reject unknown ones.
class GDCLIFlags : public RefCounted {
	GDCLASS(GDCLIFlags, RefCounted);

	// Declaration for one flag.
	struct Def {
		Variant::Type type = Variant::STRING;
		Variant fallback;
		String help;
	};

	HashMap<String, Def> defs;
	Dictionary values; // Values actually supplied.
	PackedStringArray rest; // Non-flag arguments.
	String name = "gd"; // Command name shown in usage.

protected:
	static void _bind_methods();

public:
	void set_name(const String &p_name);
	void flag_bool(const String &p_name, bool p_fallback, const String &p_help);
	void flag_str(const String &p_name, const String &p_fallback, const String &p_help);
	void flag_int(const String &p_name, int64_t p_fallback, const String &p_help);

	// Accept both --name=value and --name value.
	Ref<R> parse(const Array &p_args);

	bool get_bool(const String &p_name) const;
	String get_str(const String &p_name) const;
	int64_t get_int(const String &p_name) const;
	PackedStringArray get_rest() const { return rest; }
	String usage() const; // Return usage guidance.
};

// Logger with severity, destination, and format; default to one terminal line.
class LogState {
public:
	// Severity increases with the numeric level.
	enum Level {
		DEBUG = 10,
		INFO = 20,
		WARN = 30,
		ERROR = 40,
	};

	static Level level_of(const String &p_name); // Parse a configured severity name.
	static String flat(const String &p_s); // Safely flatten external values into one log line.

	// Write at a selected severity for named loggers.
	Signal write(Level p_at, const String &p_msg, const Variant &p_extra);
	static Signal flush(); // Wait for earlier log writes, including lines still being formatted.

private:
	friend class GDLogAPI; // Only the global logging API changes shared configuration.

	Level level = INFO; // Discard messages below this severity.
	String name; // Prefix identifying the logger.
	String to_file; // Output file, or empty for terminal-only logging.
	bool with_time = true;
	bool with_color = true;

	Signal emit(Level p_at, const String &p_msg, const Variant &p_extra);

	void setup(const String &p_name, Level p_level);
	void set_file(const String &p_path);
	void set_time(bool p_on) { with_time = p_on; }
	void set_color(bool p_on) { with_color = p_on; }

	Signal debug(const String &p_msg, const Variant &p_extra);
	Signal info(const String &p_msg, const Variant &p_extra);
	Signal warn(const String &p_msg, const Variant &p_extra);
	Signal error(const String &p_msg, const Variant &p_extra);
	// Log results directly, using ERROR for failures.
	Signal result(const Ref<R> &p_r, const String &p_msg);
	String format(Level p_at, const String &p_msg, const Variant &p_extra) const;
	String _line(Level p_at, const String &p_msg, const String &p_extra, bool p_color, int64_t p_time) const;
};

// Network utilities used before starting listeners.
class Net {
public:
	static bool is_free(int64_t p_port, const String &p_host); // Check availability by binding and immediately closing.
	// Find a free port, using kernel selection when zero is requested.
	static Ref<R> free_port(int64_t p_from, const String &p_host);
	static Ref<R> local_addresses(); // Return local OS addresses or a failure with its original cause.
	static bool is_ip(const String &p_text);
	// Split host:port, using the default when the port is absent.
	static Dictionary split_host(const String &p_text, int64_t p_default_port);
};
