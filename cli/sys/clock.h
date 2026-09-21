// Separate wall time from monotonic time so clock corrections cannot shift deadlines.
#pragma once
#include "core/variant/variant.h"

namespace GDClock {
struct Civil {
	int64_t year; // Gregorian calendar year.
	int month, day, hour, minute, second, weekday; // UTC calendar, clock, and weekday fields.
};
uint64_t usec(); // Return monotonic microseconds.
inline uint64_t msec() { return usec() / 1000; } // Return milliseconds from the same monotonic clock.
double unix_time(); // Return wall-clock seconds since the Unix epoch.
Dictionary parts(int64_t p_time); // Convert Unix seconds to a UTC calendar dictionary.
Civil civil(int64_t p_time); // Convert to UTC calendar fields without allocation.
int64_t from_parts(const Dictionary &p_parts); // Convert UTC calendar fields to Unix seconds.
}
