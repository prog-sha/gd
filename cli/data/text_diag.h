// Collect opt-in text phase timings without logging from worker or transport callbacks.
#pragma once
#include "cli/sys/clock.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace TextDiag {
enum Metric { OPERATIONS, ADVANCES, PROBE_US, MEASURE_US, ALLOCATE_US, ENCODE_US, MAIN_WAIT_US,
	QUEUE_WAIT_US, WORKER_US, COMPLETION_WAIT_US, OFFLOADS, REQUEUES, CANCELS, MAX_OUTPUT_BYTES, COUNT };

// Emit one aggregate record at process exit, after runtime workers have been joined.
struct Stats {
	bool enabled = false; // Diagnostics must be explicitly enabled and measured separately.
	std::atomic<uint64_t> values[COUNT]{};
	Stats() {
		const char *flag = std::getenv("GD_TEXT_DIAGNOSTICS");
		enabled = flag && flag[0] == '1' && flag[1] == '\0';
	}
	~Stats() {
		if (!enabled) return;
		const char *names[] = { "operations", "advances", "probe_us", "measure_us", "allocate_us", "encode_us",
			"main_wait_us", "worker_queue_wait_us", "worker_run_us", "completion_wait_us", "offloads", "requeues", "cancels", "max_output_bytes" };
		std::fprintf(stderr, "GD_TEXT_DIAGNOSTICS {\"clock\":\"monotonic_elapsed_us\"");
		for (int i = 0; i < COUNT; ++i) std::fprintf(stderr, ",\"%s\":%llu", names[i], (unsigned long long)values[i].load(std::memory_order_relaxed));
		std::fprintf(stderr, "}\n");
	}
};
inline Stats &stats() { static Stats value; return value; }
inline bool enabled() { return stats().enabled; }
inline void add(Metric p_metric, uint64_t p_value = 1) { stats().values[p_metric].fetch_add(p_value, std::memory_order_relaxed); }
inline void peak(uint64_t p_bytes) {
	auto &value = stats().values[MAX_OUTPUT_BYTES];
	uint64_t old = value.load(std::memory_order_relaxed);
	while (old < p_bytes && !value.compare_exchange_weak(old, p_bytes, std::memory_order_relaxed)) {}
}

// Charge only enabled conversion work to its current phase, including early return paths.
struct Phase {
	bool active = enabled();
	Metric metric = PROBE_US;
	uint64_t at = active ? GDClock::usec() : 0;
	void next(Metric p_metric) {
		if (!active) return;
		const uint64_t now = GDClock::usec();
		add(metric, now - at);
		at = now;
		metric = p_metric;
	}
	~Phase() { if (active) add(metric, GDClock::usec() - at); }
};
}
