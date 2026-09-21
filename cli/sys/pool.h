/**************************************************************************/
/*  pool.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Run work unsuitable for kernel polling on threads outside the event loop.
// Internal asynchronous APIs submit jobs and receive results on event-loop turns.
//
// Use separate queues for I/O waits and CPU work.
// Blocked I/O does not occupy the CPU-worker budget or stall independent file and process jobs.

#pragma once

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "core/templates/list.h"

// One job implementing run, followed by finish on the main thread.
class PoolJob : public RefCounted {
	GDCLASS(PoolJob, RefCounted);

	Ref<PoolJob> self_hold; // Retain the job while submitted.
	bool cpu = false; // CPU queue membership used again on completion.
	bool serial = false; // Whether the job belongs to the ordered single-worker queue.
	List<Ref<PoolJob>>::Element *entry = nullptr; // Registry entry retained until completion.

	void finish_on_main(); // Deliver a completed result on the main thread.
	// Execute one job on a worker through marking completion.
	void run_on_worker();

	friend class Pool;

protected:
	static void _bind_methods() {}

	// Use only owned values or borrowed values immutable through worker completion.
	virtual void run() = 0;
	// Return false only after saving progress and releasing all borrowed worker state.
	virtual bool run_slice() { run(); return true; }
	// Observe queue entry and actual execution without changing ordinary job scheduling.
	virtual void queued() {}
	virtual void started() {}
	virtual void completed() {}
	// Deliver result signals on the main thread.
	virtual void finish() = 0;
	// Cancel blocking waits only for jobs requiring explicit shutdown interruption.
	virtual void cancel_on_shutdown() {}

public:
	// Submit work, adding I/O workers independently of processor count.
	// Limit CPU jobs to processor capacity; invoke finish exactly once.
	bool submit(bool p_cpu = false, bool p_serial = false);
};

class Pool {
	// Take one job at a time, sleeping when the queue is empty.
	static void worker_loop(void *);
	// Shut down one worker queue.
	static void shutdown_one(void *);
	static void drain(bool p_now); // Deliver directly without the ready queue only during shutdown.
	static void grow(void *p_state, bool p_cpu, bool p_serial); // Lend blocked CPU capacity to other workers.
	friend class PoolJob;

public:
	// Delegate Object, Callable, and Signal formatting to main; return false outside workers.
	static bool stringify(const Variant &p_value, String &r_text);
	// Format nested values while keeping worker Object callbacks on main.
	static String text(const Variant &p_value, int p_depth = 0);
	// Move completed worker jobs into the main-thread ready queue.
	static void drain();
	// Report jobs whose completion has not yet been delivered.
	static bool has_work();
	// Report whether the selected queue is shutting down.
	static bool is_stopping(bool p_cpu = false, bool p_serial = false);
	// Shut down all worker threads.
	static void shutdown();
};
