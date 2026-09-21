/**************************************************************************/
/*  limit.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Normalize public numeric settings into internal units without undefined conversions.

#include "core/math/math_funcs.h"

#include <cstdint>

namespace Limit {

constexpr int64_t PORT_MIN = 1; // Smallest explicitly selected TCP or UDP port.
constexpr int64_t PORT_MAX = 65535; // Largest TCP or UDP port.
constexpr int64_t RESPONSE_STATUS_MIN = 100; // Minimum supported HTTP status code.
constexpr int64_t RESPONSE_STATUS_MAX = 999; // Maximum supported HTTP status code.
constexpr int RESPONSE_STATUS_FALLBACK = 500; // Safe replacement for an out-of-range status.
constexpr int64_t SEC_MAX = INT64_MAX / 1000000000; // Largest integral seconds value safely representable as nanoseconds.

// Keep zero unlimited and round positive durations up to at least one millisecond.
inline bool seconds_ms(double p_sec, uint64_t &r_ms) {
	if (!Math::is_finite(p_sec) || p_sec < 0.0) {
		return false;
	}
	if (p_sec == 0.0) {
		r_ms = 0;
		return true;
	}
	if (p_sec > (double)INT64_MAX / 1000000000.0) {
		return false;
	}
	const double ms = p_sec * 1000.0;
	r_ms = MAX(uint64_t(1), (uint64_t)ms);
	return true;
}

} // namespace Limit
