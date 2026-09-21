// Provide native clocks and Gregorian calendar conversion using a 400-year cycle.
#include "cli/sys/clock.h"
#include <chrono>
#include <cstring>
#ifdef MACOS_ENABLED
#include "cli/sys/clock_scale.h"
#include <mach/mach_time.h>
#endif

namespace {
constexpr int64_t DAY = 86400; // Seconds per day.
constexpr int64_t ERA = 146097; // Days per 400-year Gregorian cycle.
constexpr int64_t EPOCH = 719468; // Days from the March-based calendar origin to the Unix epoch.

// Round division toward negative infinity for dates before the epoch.
int64_t floor_div(int64_t p_value, int64_t p_unit) { return p_value / p_unit - (p_value % p_unit < 0); }
// Reinterpret wrapped integer bits without undefined signed overflow.
int64_t signed_bits(uint64_t p_value) { int64_t value; std::memcpy(&value, &p_value, sizeof(value)); return value; }
}

// Read the monotonic clock with one consistent epoch.
uint64_t GDClock::usec() {
#ifdef MACOS_ENABLED
	// Cache the immutable scale instead of rediscovering it for every deadline check.
	static const Scale scale = []() {
		mach_timebase_info_data_t value;
		mach_timebase_info(&value);
		return Scale(value.numer, value.denom);
	}();
	return scale.usec(mach_absolute_time());
#else
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// Return wall-clock seconds since the Unix epoch.
double GDClock::unix_time() {
	return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Derive UTC calendar and clock fields from 400-, 100-, and 4-year cycles.
GDClock::Civil GDClock::civil(int64_t p_time) {
	const int64_t days = floor_div(p_time, DAY);
	const int64_t second = p_time % DAY < 0 ? p_time % DAY + DAY : p_time % DAY;
	const int64_t shifted = days + EPOCH;
	const int64_t era = floor_div(shifted, ERA);
	const int64_t day = shifted - era * ERA;
	const int64_t year = (day - day / 1460 + day / 36524 - day / 146096) / 365;
	const int64_t in_year = day - (365 * year + year / 4 - year / 100);
	const int64_t month = (5 * in_year + 2) / 153;
	Civil out;
	out.year = year + era * 400 + (month >= 10);
	out.month = month + (month < 10 ? 3 : -9);
	out.day = in_year - (153 * month + 2) / 5 + 1;
	out.weekday = (days % 7 + 11) % 7;
	out.hour = second / 3600;
	out.minute = second % 3600 / 60;
	out.second = second % 60;
	return out;
}

// Copy UTC calendar fields to the public dictionary form.
Dictionary GDClock::parts(int64_t p_time) {
	const Civil value = civil(p_time);
	Dictionary out;
	out["year"] = value.year; out["month"] = value.month; out["day"] = value.day;
	out["hour"] = value.hour; out["minute"] = value.minute; out["second"] = value.second; out["weekday"] = value.weekday;
	return out;
}

// Normalize UTC months and days using the 400-year calendar cycle.
int64_t GDClock::from_parts(const Dictionary &p_parts) {
	int64_t year = p_parts.get("year", 1970);
	int64_t month = p_parts.get("month", 1);
	const int64_t day = p_parts.get("day", 1);
	// Normalize months with integer arithmetic without signed overflow on extreme input.
	const int64_t years = floor_div(month, 12);
	month %= 12;
	if (month <= 0) month += 12;
	year = signed_bits(uint64_t(year) + uint64_t(years) - (month == 12) - (month <= 2));
	const int64_t era = floor_div(year, 400);
	const int64_t in_era = uint64_t(year) - uint64_t(era) * 400;
	const int64_t m = month + (month > 2 ? -3 : 9);
	const uint64_t days = uint64_t(era) * ERA + in_era * 365 + in_era / 4 - in_era / 100 + (153 * m + 2) / 5 + uint64_t(day) - 1 - EPOCH;
	const uint64_t seconds = days * DAY + uint64_t(int64_t(p_parts.get("hour", 0))) * 3600 +
			uint64_t(int64_t(p_parts.get("minute", 0))) * 60 + uint64_t(int64_t(p_parts.get("second", 0)));
	return signed_bits(seconds);
}
