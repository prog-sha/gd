/**************************************************************************/
/*  sched.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"

#include <cstdint>

// Time slice for rotating runnable work.
constexpr uint64_t GD_SCHED_SLICE_USEC = 10 * 1000;

// Compact a consumed FIFO prefix only when it is at least as large as the live suffix.
template <typename T>
inline void gd_ready_compact(LocalVector<T> &p_queue, uint32_t &r_at) {
	if (r_at >= p_queue.size()) {
		p_queue.clear();
		r_at = 0;
		return;
	}
	const uint32_t live = p_queue.size() - r_at;
	if (r_at < live) {
		return;
	}
	for (uint32_t i = 0; i < live; i++) {
		p_queue[i] = static_cast<T &&>(p_queue[r_at + i]);
	}
	p_queue.resize(live);
	r_at = 0;
}
