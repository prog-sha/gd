/**************************************************************************/
/*  proc.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Start and await child processes through GD.proc.run.
//
// Delegate process-exit waits and output draining to workers.
// Drain pipes independently of event-loop cadence so a stalled child cannot block unrelated work.

#pragma once

#include "cli/sys/pool.h"
#include "cli/sys/std.h"

#include "core/io/file_access.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

class GDProcCall;

// Worker job awaiting a child process.
class ProcWaitJob : public PoolJob {
	GDCLASS(ProcWaitJob, PoolJob);

	Ref<GDProcCall> call; // Retain the process call while the job runs.

	friend class GDProcCall;

protected:
	static void _bind_methods() {}
	virtual void run() override;
	virtual void finish() override;
	virtual void cancel_on_shutdown() override;
};

// One child process returning exit state through finished.
class GDProcCall : public RefCounted {
	GDCLASS(GDProcCall, RefCounted);

	Ref<GDProcCall> self_hold; // Retain the call until completion even when scripts release it.
	ProcessID pid = 0; // Running child ID, or zero when absent.
	int exit_code = -1; // Child exit code.
	PackedByteArray out; // Combined output retained as bytes across character boundaries.
	String why; // Launch failure detail, empty after successful launch.
	Ref<R> start_error; // Worker-observed launch failure details.
	String start_path; // Attempted executable for launch diagnostics.
	PackedStringArray start_args; // Arguments passed to the worker.
	Err::Kind reject_kind = Err::NONE; // Preflight failure category.
	uint64_t deadline = 0; // Termination deadline, or zero for unlimited.
	SafeFlag killed; // Atomic flag read by both deadline handling and the worker.
	SafeFlag canceled; // The caller stopped waiting, as opposed to the deadline passing.
	bool output_failed = false; // Worker output-allocation failure read by main only after join.
	bool want_output = false; // Capture output when true; otherwise connect directly to the parent.
	// Serialize reaping and termination because both can invoke waitpid.
	// Otherwise one may see ECHILD or terminate a different process after PID reuse.
	Mutex reap_mutex;
	bool reaped = false; // Reaped state accessed only under reap_mutex.
	Ref<FileAccess> out_pipe; // Child stdout pipe, retained only when capturing output.
	Ref<FileAccess> err_pipe; // Child stderr pipe, retained only when capturing output.
	bool out_eof = false; // Whether stdout reached EOF.
	bool err_eof = false; // Whether stderr reached EOF.

	friend class ProcWaitJob;

	void finish_now(); // Report pre-launch failure on a later event-loop turn.
	bool pull_output(bool p_done); // Drain buffered output on the worker.
	void emit_now(); // Emit finished and release resources.
	void reap_now(); // Terminate and reap unless already reaped.
	void launch_on_worker(); // Perform blocking process creation on a worker.

protected:
	static void _bind_methods();

public:
	// Start the child and signal even launch failures asynchronously.
	void begin(const String &p_path, const PackedStringArray &p_args, const Dictionary &p_opts);
	void cancel(); // Terminate the child when the caller cancels.
};

class Proc {
public:
	// Start a child and return its exit-completion signal.
	static Signal run(const String &p_path, const PackedStringArray &p_args, const Dictionary &p_opts);
};
