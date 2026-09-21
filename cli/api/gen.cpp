/**************************************************************************/
/*  gen.cpp                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement time-dependent values and identifiers declared in gen.h.

#include "cli/api/gen.h"
#include "cli/sys/clock.h"

#include "cli/data/codec.h"
#include "core/math/random_number_generator.h"

#include "core/object/class_db.h"

#include <stdio.h>

// ---------------- Date and time ----------------

namespace {

const char *DAY_NAMES[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char *MONTH_NAMES[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

} // namespace

// Format Unix seconds as UTC using the selected pattern.
String Datetime::format(int64_t p_unix, const String &p_pattern) {
	const GDClock::Civil c = GDClock::civil(p_unix);
	char buf[8];
	String out = p_pattern;
	// Replace longer tokens first so shorter ones cannot consume them.
	out = out.replace("YYYY", String::num_int64(c.year).pad_zeros(4));
	out = out.replace("MMM", MONTH_NAMES[(c.month - 1) % 12]);
	out = out.replace("ddd", DAY_NAMES[c.weekday % 7]);
	snprintf(buf, sizeof(buf), "%02d", c.month);
	out = out.replace("MM", buf);
	snprintf(buf, sizeof(buf), "%02d", c.day);
	out = out.replace("DD", buf);
	snprintf(buf, sizeof(buf), "%02d", c.hour);
	out = out.replace("HH", buf);
	snprintf(buf, sizeof(buf), "%02d", c.minute);
	out = out.replace("mm", buf);
	snprintf(buf, sizeof(buf), "%02d", c.second);
	out = out.replace("ss", buf);
	return out;
}

namespace {

constexpr int64_t SEC_MIN = 60;
constexpr int64_t SEC_HOUR = 3600;
constexpr int64_t SEC_DAY = 86400;

// Convert a unit to seconds, defaulting unknown names to seconds.
int64_t unit_secs(const String &p_unit) {
	if (p_unit == "minute") {
		return SEC_MIN;
	}
	if (p_unit == "hour") {
		return SEC_HOUR;
	}
	if (p_unit == "day") {
		return SEC_DAY;
	}
	if (p_unit == "week") {
		return SEC_DAY * 7;
	}
	return 1;
}

// Saturate 64-bit addition to prevent timestamp wraparound.
int64_t saturating_add(int64_t p_a, int64_t p_b) {
	if (p_b > 0 && p_a > INT64_MAX - p_b) {
		return INT64_MAX;
	}
	if (p_b < 0 && p_a < INT64_MIN - p_b) {
		return INT64_MIN;
	}
	return p_a + p_b;
}

// Scale positive units and add them to time, saturating intermediate or final overflow.
int64_t saturating_add_scaled(int64_t p_base, int64_t p_value, int64_t p_scale) {
	if (p_value > INT64_MAX / p_scale) {
		return INT64_MAX;
	}
	if (p_value < INT64_MIN / p_scale) {
		return INT64_MIN;
	}
	return saturating_add(p_base, p_value * p_scale);
}

// Saturate 64-bit subtraction at the representable boundaries.
int64_t saturating_sub(int64_t p_a, int64_t p_b) {
	if (p_b < 0 && p_a > INT64_MAX + p_b) {
		return INT64_MAX;
	}
	if (p_b > 0 && p_a < INT64_MIN + p_b) {
		return INT64_MIN;
	}
	return p_a - p_b;
}

// Read a fixed-width decimal RFC 3339 field and check its range.
bool decimal_at(const String &p_text, int p_at, int p_len, int p_min, int p_max, int &r_value) {
	if (p_at < 0 || p_at + p_len > p_text.length()) {
		return false;
	}
	int value = 0;
	for (int i = 0; i < p_len; i++) {
		const char32_t c = p_text[p_at + i];
		if (c < '0' || c > '9') {
			return false;
		}
		value = value * 10 + c - '0';
	}
	if (value < p_min || value > p_max) {
		return false;
	}
	r_value = value;
	return true;
}

} // namespace

// Return current Unix seconds.
int64_t Datetime::now() {
	return (int64_t)GDClock::unix_time();
}

// Split Unix seconds into UTC calendar fields.
Dictionary Datetime::to_parts(int64_t p_unix) {
	return GDClock::parts(p_unix);
}

// Combine UTC calendar fields into Unix seconds.
int64_t Datetime::from_parts(const Dictionary &p_parts) {
	return GDClock::from_parts(p_parts);
}

// Format Unix seconds as UTC ISO 8601.
String Datetime::to_iso(int64_t p_unix) {
	return format(p_unix, "YYYY-MM-DDTHH:mm:ss") + "Z";
}

// Format Unix seconds as HTTP-date.
String Datetime::to_http(int64_t p_unix) {
	return format(p_unix, "ddd, DD MMM YYYY HH:mm:ss") + " GMT";
}

// Validate an RFC 3339 timestamp and convert it to Unix seconds.
Ref<R> Datetime::parse_iso(const String &p_text) {
	// Validate timestamp fields at their fixed RFC 3339 positions.
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	if (p_text.length() < 20 || p_text[4] != '-' || p_text[7] != '-' || p_text[10] != 'T' || p_text[13] != ':' || p_text[16] != ':' ||
			!decimal_at(p_text, 0, 4, 0, 9999, year) || !decimal_at(p_text, 5, 2, 1, 12, month) ||
			!decimal_at(p_text, 8, 2, 1, days_in_month(year, month), day) || !decimal_at(p_text, 11, 2, 0, 23, hour) ||
			!decimal_at(p_text, 14, 2, 0, 59, minute) || !decimal_at(p_text, 17, 2, 0, 59, second)) {
		return R::err(vformat("invalid RFC 3339 datetime \"%s\"", p_text), Err::INVALID_DATA);
	}
	int at = 19;
	if (at < p_text.length() && p_text[at] == '.') {
		at++;
		const int start = at;
		while (at < p_text.length() && p_text[at] >= '0' && p_text[at] <= '9') {
			at++;
		}
		if (at == start) {
			return R::err(vformat("invalid RFC 3339 fraction \"%s\"", p_text), Err::INVALID_DATA);
		}
	}
	int offset = 0;
	if (at < p_text.length() && p_text[at] == 'Z' && at + 1 == p_text.length()) {
		at++;
	} else {
		int zone_hour = 0, zone_minute = 0;
		if (at + 6 != p_text.length() || (p_text[at] != '+' && p_text[at] != '-') || p_text[at + 3] != ':' ||
				!decimal_at(p_text, at + 1, 2, 0, 23, zone_hour) || !decimal_at(p_text, at + 4, 2, 0, 59, zone_minute)) {
			return R::err(vformat("invalid RFC 3339 timezone \"%s\"", p_text), Err::INVALID_DATA);
		}
		offset = (zone_hour * 60 + zone_minute) * 60;
		if (p_text[at] == '-') {
			offset = -offset;
		}
		at += 6;
	}
	Dictionary box;
	box["year"] = year;
	box["month"] = month;
	box["day"] = day;
	box["hour"] = hour;
	box["minute"] = minute;
	box["second"] = second;
	return at == p_text.length() ? R::ok(from_parts(box) - offset) : R::err("data after RFC 3339 datetime", Err::INVALID_DATA);
}

// Add the selected time unit and saturate to the nearest boundary on overflow.
int64_t Datetime::add(int64_t p_unix, int64_t p_amount, const String &p_unit) {
	return saturating_add_scaled(p_unix, p_amount, unit_secs(p_unit));
}

// Return the timestamp difference in the selected unit, saturating overflow.
int64_t Datetime::diff(int64_t p_a, int64_t p_b, const String &p_unit) {
	return saturating_sub(p_b, p_a) / unit_secs(p_unit);
}

// Round Unix seconds down to the start of the same UTC day.
int64_t Datetime::start_of_day(int64_t p_unix) {
	int64_t rest = p_unix % SEC_DAY;
	if (rest < 0) {
		rest += SEC_DAY; // Preserve the day boundary for negative timestamps.
	}
	return saturating_sub(p_unix, rest);
}

// Return the UTC weekday with Sunday as zero.
int Datetime::weekday(int64_t p_unix) {
	return (int)(int64_t)to_parts(p_unix).get("weekday", 0);
}

// Check whether a Gregorian year is a leap year.
bool Datetime::is_leap(int p_year) {
	return (p_year % 4 == 0 && p_year % 100 != 0) || p_year % 400 == 0;
}

// Return the number of days in the selected year and month.
int Datetime::days_in_month(int p_year, int p_month) {
	if (p_month == 2) {
		return is_leap(p_year) ? 29 : 28;
	}
	if (p_month == 4 || p_month == 6 || p_month == 9 || p_month == 11) {
		return 30;
	}
	return 31;
}

// Describe a timestamp difference using the largest human-readable unit.
String Datetime::ago(int64_t p_unix, int64_t p_base) {
	const int64_t at = p_base < 0 ? now() : p_base;
	const int64_t delta = saturating_sub(at, p_unix);
	const bool back = delta < 0;
	const uint64_t d = back ? uint64_t(-(delta + 1)) + 1 : uint64_t(delta);
	String txt;
	if (d < SEC_MIN) {
		txt = vformat(String::utf8("%d 秒"), (int64_t)d);
	} else if (d < SEC_HOUR) {
		txt = vformat(String::utf8("%d 分"), (int64_t)(d / SEC_MIN));
	} else if (d < SEC_DAY) {
		txt = vformat(String::utf8("%d 時間"), (int64_t)(d / SEC_HOUR));
	} else {
		txt = vformat(String::utf8("%d 日"), (int64_t)(d / SEC_DAY));
	}
	return txt + (back ? String::utf8("後") : String::utf8("前"));
}

// ---------------- ULID ----------------

namespace {

const char *ULID_B32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford alphabet excluding I, L, O, and U.
constexpr int ULID_TIME = 10;
constexpr int ULID_RAND = 16;

} // namespace

// Generate a time-sortable ULID from milliseconds and random bytes.
String Ulid::make(int64_t p_ms) {
	const int64_t at = p_ms < 0 ? (int64_t)(GDClock::unix_time() * 1000.0) : p_ms;
	if (at == last_ms && has_last) {
		// Increment the random suffix within the same millisecond, carrying from the end.
		for (int i = ULID_RAND - 1; i >= 0; i--) {
			if (last_rand[i] < 31) {
				last_rand[i]++;
				break;
			}
			last_rand[i] = 0;
		}
	} else {
		last_ms = at;
		has_last = true;
		Ref<RandomNumberGenerator> gen;
		gen.instantiate();
		gen->randomize();
		for (int i = 0; i < ULID_RAND; i++) {
			last_rand[i] = (uint8_t)gen->randi_range(0, 31);
		}
	}

	char out[ULID_TIME + ULID_RAND + 1];
	int64_t v = at;
	for (int i = ULID_TIME - 1; i >= 0; i--) {
		out[i] = ULID_B32[v % 32];
		v /= 32;
	}
	for (int i = 0; i < ULID_RAND; i++) {
		out[ULID_TIME + i] = ULID_B32[last_rand[i]];
	}
	out[ULID_TIME + ULID_RAND] = 0;
	return String::utf8(out, ULID_TIME + ULID_RAND);
}

// Validate ULID length and alphabet.
bool Ulid::is_valid(const String &p_text) {
	if (p_text.length() != ULID_TIME + ULID_RAND) {
		return false;
	}
	for (int i = 0; i < p_text.length(); i++) {
		if (!strchr(ULID_B32, (int)p_text[i]) || p_text[i] == 0) {
			return false;
		}
	}
	return true;
}

// Extract the creation timestamp from a ULID prefix.
Ref<R> Ulid::time_of(const String &p_text) {
	if (!is_valid(p_text)) {
		return R::err(vformat("invalid ulid \"%s\"", p_text), Err::INVALID_DATA);
	}
	int64_t v = 0;
	for (int i = 0; i < ULID_TIME; i++) {
		v = v * 32 + (int64_t)(strchr(ULID_B32, (int)p_text[i]) - ULID_B32);
	}
	return R::ok(v);
}

// ---------------- UUID ----------------

namespace {

const char *HEXL = "0123456789abcdef";

// Set UUID version and variant bits before formatting.
String uuid_stamp(uint8_t *b, int version) {
	b[6] = (uint8_t)((b[6] & 0x0f) | (version << 4)); // Version occupies the high four bits.
	b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); // Variant occupies the high two bits.
	char out[37];
	int at = 0;
	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10) {
			out[at++] = '-';
		}
		out[at++] = HEXL[b[i] >> 4];
		out[at++] = HEXL[b[i] & 15];
	}
	out[at] = 0;
	return String::utf8(out, at);
}

} // namespace

// Generate UUID v4 from cryptographic random bytes.
String Uuid::v4() {
	Ref<RandomNumberGenerator> gen;
	gen.instantiate();
	gen->randomize();
	uint8_t b[16];
	for (int i = 0; i < 16; i++) {
		b[i] = (uint8_t)gen->randi_range(0, 255);
	}
	return uuid_stamp(b, 4);
}

// Validate UUID separators and hexadecimal digits.
bool Uuid::is_valid(const String &p_text) {
	if (p_text.length() != 36) {
		return false;
	}
	for (int i = 0; i < 36; i++) {
		const char32_t c = p_text[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-') {
				return false;
			}
		} else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	return true;
}

// Decode a UUID string into 16 bytes.
Ref<R> Uuid::to_bytes(const String &p_text) {
	if (!is_valid(p_text)) {
		return R::err(vformat("invalid uuid \"%s\"", p_text), Err::INVALID_DATA);
	}
	const String hex = p_text.replace("-", "");
	PackedByteArray out;
	out.resize(16);
	uint8_t *w = out.ptrw();
	for (int i = 0; i < 16; i++) {
		w[i] = (uint8_t)hex.substr(i * 2, 2).hex_to_int();
	}
	return R::ok(out);
}

// Derive UUID v5 deterministically from namespace and name.
Ref<R> Uuid::v5(const String &p_space, const String &p_name) {
	const Ref<R> ns = to_bytes(p_space);
	if (ns->get_e().is_valid()) {
		return ns;
	}
	PackedByteArray raw = ns->get_v();
	raw.append_array(p_name.to_utf8_buffer());
	PackedByteArray digest = Hash::sha1(raw);
	digest.resize(16);
	return R::ok(uuid_stamp(digest.ptrw(), 5));
}

// Return the UUID version, or zero for malformed text.
int Uuid::version_of(const String &p_text) {
	if (!is_valid(p_text)) {
		return 0;
	}
	return p_text.substr(14, 1).hex_to_int();
}

// Return the all-zero UUID.
String Uuid::nil_id() {
	return "00000000-0000-0000-0000-000000000000";
}

// Return the RFC 4122 DNS namespace UUID.
String Uuid::ns_dns() {
	return "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
}

// Return the RFC 4122 URL namespace UUID.
String Uuid::ns_url() {
	return "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
}

// Return the RFC 4122 OID namespace UUID.
String Uuid::ns_oid() {
	return "6ba7b812-9dad-11d1-80b4-00c04fd430c8";
}
