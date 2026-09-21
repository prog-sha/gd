// Convert native clock ticks to exact microseconds without widening integral ratios.
#pragma once
#include <cstdint>
#include <numeric>

namespace GDClock {
struct Scale {
	uint64_t num, den; // Reduced tick-to-microsecond ratio.

	// Reduce the immutable nanosecond timebase before repeated deadline reads.
	Scale(uint32_t p_num, uint32_t p_den) {
		const uint64_t denominator = uint64_t(p_den) * 1000;
		const uint64_t factor = std::gcd(uint64_t(p_num), denominator);
		num = p_num / factor;
		den = denominator / factor;
	}

	// Preserve floor rounding and full-width products for every valid timebase.
	uint64_t usec(uint64_t p_ticks) const {
		return num == 1 ? p_ticks / den : uint64_t((static_cast<unsigned __int128>(p_ticks) * num) / den);
	}
};
}
