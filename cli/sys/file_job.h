/**************************************************************************/
/*  file_job.h                                                            */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Run file operations outside the event loop and deliver results through signals.
//
// Keep regular-file blocking system calls off the main thread.
//
// Execute synchronous Os methods entirely within a worker thread.
// This keeps thread-local permission depth and source errors within one call context.

#pragma once

#include "cli/sys/pool.h"
#include "cli/sys/std.h"

#include <functional>

// One worker computation with an unrestricted result type.
class GDValueCall : public PoolJob {
	GDCLASS(GDValueCall, PoolJob);

	std::function<Variant()> work; // Computation completed within the worker thread.
	Variant outcome; // Worker-produced result delivered on the main thread.

protected:
	static void _bind_methods();
	virtual void run() override;
	virtual void finish() override;

public:
	// Submit a computation and return its completion signal.
	static Signal start(std::function<Variant()> p_work, bool p_cpu = true);
};

// Complete a worker computation through two signal arguments.
class GDPairCall : public PoolJob {
	GDCLASS(GDPairCall, PoolJob);

	std::function<VariantPair()> work; // Computation performed by the worker.
	VariantPair outcome; // Value and error delivered on the main thread.

protected:
	static void _bind_methods();
	virtual void run() override;
	virtual void finish() override;

public:
	static Signal start(std::function<VariantPair()> p_work, bool p_cpu = true, bool p_serial = false);
};

class GDFormatCall;

// One parsing-only CPU job.
class GDFormatJob : public PoolJob {
	GDCLASS(GDFormatJob, PoolJob);

	Ref<GDFormatCall> call; // Operation receiving the parsing result.

	friend class GDFormatCall;

protected:
	static void _bind_methods() {}
	virtual void run() override;
	virtual void finish() override;
};

// Sequence file reading on the I/O queue and parsing on the CPU queue.
class GDFormatCall : public RefCounted {
	GDCLASS(GDFormatCall, RefCounted);

	std::function<VariantPair(const Variant &)> parse; // Parser executed by a CPU worker.
	VariantPair input; // Content returned by the I/O worker.
	VariantPair outcome; // Result returned by the CPU worker.
	Ref<GDFormatCall> self_hold; // Retain this operation through result delivery.

	friend class GDFormatJob;

	void loaded(const Variant &p_value, const Variant &p_error); // Receive file-read results.
	void parse_on_worker(); // Parse the format on a CPU worker.
	void parsed(); // Deliver the final result on the main thread.

protected:
	static void _bind_methods();

public:
	// Run reading and parsing sequentially on separate worker queues.
	static Signal start(std::function<VariantPair()> p_read, std::function<VariantPair(const Variant &)> p_parse);
};
