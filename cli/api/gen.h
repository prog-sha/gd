/**************************************************************************/
/*  gen.h                                                                 */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Generate time-dependent values, sortable identifiers, and random values.
// Keep state-dependent results separate from pure value transformations.
// Expose generation through GD.gen and hide implementation names.

#pragma once

#include "cli/sys/std.h"


// Date and time operations.
class Datetime {
public:
	static int64_t now();
	static Dictionary to_parts(int64_t p_unix); // UTC
	static int64_t from_parts(const Dictionary &p_parts);
	static String format(int64_t p_unix, const String &p_pattern);
	static String to_iso(int64_t p_unix);
	static String to_http(int64_t p_unix);
	static Ref<R> parse_iso(const String &p_text);
	// Supported units: second, minute, hour, day, and week.
	static int64_t add(int64_t p_unix, int64_t p_amount, const String &p_unit);
	static int64_t diff(int64_t p_a, int64_t p_b, const String &p_unit);
	static int64_t start_of_day(int64_t p_unix);
	static int weekday(int64_t p_unix); // Return weekday with Sunday as zero.
	static bool is_leap(int p_year);
	static int days_in_month(int p_year, int p_month);
	static String ago(int64_t p_unix, int64_t p_base); // Describe elapsed time in human-readable form.
};

// Sortable identifiers.
class Ulid {
	int64_t last_ms = -1; // Timestamp of the previous generated value.
	uint8_t last_rand[16] = { 0 }; // Random digits of the previous value.
	bool has_last = false;

public:
	String make(int64_t p_ms);
	static bool is_valid(const String &p_text);
	static Ref<R> time_of(const String &p_text);
};

// UUID。
class Uuid {
public:
	static String v4();
	static bool is_valid(const String &p_text);
	static Ref<R> to_bytes(const String &p_text);
	// Derive a deterministic identifier from namespace and name.
	static Ref<R> v5(const String &p_space, const String &p_name);
	static int version_of(const String &p_text); // Return zero when the UUID cannot be parsed.

	// Standard namespace values specified by RFC 4122.
	static String nil_id();
	static String ns_dns();
	static String ns_url();
	static String ns_oid();
};
