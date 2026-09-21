// Prevent OS resource exhaustion from runtime thread growth.
#include "cli/sys/thread_limit.h"

#include "core/error/error_macros.h"

#include <mutex>

namespace {
std::mutex thread_mutex; // Serialize thread reservations and limit changes.
int64_t thread_count = 1; // Runtime-managed thread count including the main thread.
int64_t thread_max = 10000; // Default process-wide OS thread limit.
}

// Reserve a thread slot and fail fatally if the process-wide budget is exceeded.
void GDThreadLimit::acquire() {
	std::lock_guard<std::mutex> lock(thread_mutex);
	CRASH_COND_MSG(thread_count >= thread_max, "runtime thread limit exceeded");
	thread_count++;
}

// Remove a finished thread from the active count.
void GDThreadLimit::release() {
	std::lock_guard<std::mutex> lock(thread_mutex);
	thread_count--;
}

// Check the new limit against current usage atomically and return the previous limit.
int64_t GDThreadLimit::set(int64_t p_max) {
	std::lock_guard<std::mutex> lock(thread_mutex);
	CRASH_COND_MSG(p_max < thread_count, "runtime thread limit exceeded");
	const int64_t previous = thread_max;
	thread_max = p_max;
	return previous;
}
