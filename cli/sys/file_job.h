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

// One file operation returning its result through finished.
class GDFileCall : public PoolJob {
	GDCLASS(GDFileCall, PoolJob);

	std::function<Ref<R>()> work; // Os operation executed on the worker thread.
	Ref<R> outcome; // Worker-produced result delivered on the main thread.

protected:
	static void _bind_methods();

	// Run on the worker, accessing only this job's owned state.
	virtual void run() override;
	// Deliver the result through a signal on the main thread.
	virtual void finish() override;

public:
	// Submit work and return its completion signal.
	// Set p_cpu for parsing work to separate it from the I/O queue.
	static Signal start(std::function<Ref<R>()> p_work, bool p_cpu = false, bool p_serial = false);
};

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

	std::function<Ref<R>(const Ref<R> &)> parse; // Parser executed by a CPU worker.
	Ref<R> input; // Content returned by the I/O worker.
	Ref<R> outcome; // Result returned by the CPU worker.
	Ref<GDFormatCall> self_hold; // Retain this operation through result delivery.

	friend class GDFormatJob;

	void loaded(const Ref<R> &p_input); // Receive file-read results.
	void parse_on_worker(); // Parse the format on a CPU worker.
	void parsed(); // Deliver the final result on the main thread.

protected:
	static void _bind_methods();

public:
	// Run reading and parsing sequentially on separate worker queues.
	static Signal start(std::function<Ref<R>()> p_read, std::function<Ref<R>(const Ref<R> &)> p_parse);
};
