/**************************************************************************/
/*  proc.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement asynchronous child-process calls declared in proc.h.

#include "cli/sys/proc.h"
#include "cli/sys/clock.h"

#include "cli/data/bytes.h"
#include "cli/sys/limit.h"
#include "cli/sys/perm.h"
#include "cli/sys/source_error.h"
#include "cli/sys/task.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

#ifdef UNIX_ENABLED
#include "drivers/unix/file_access_unix_pipe.h"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr uint64_t WAIT_MS = 0; // Default timeout; zero is unlimited.
constexpr int READ_BYTES_MAX = 256 * 1024; // Maximum bytes drained from one pipe per step.

// Create an absolute deadline without overflow; zero is unlimited.
uint64_t deadline_after(uint64_t p_wait) {
	const uint64_t now = GDClock::msec();
	return p_wait == 0 ? 0 : (p_wait > UINT64_MAX - now ? UINT64_MAX : now + p_wait);
}

// Check whether all writers have closed, meaningful only after buffered data is drained.
// A descendant may retain a writer after the direct child exits.
bool writer_gone(const Ref<FileAccess> &p_pipe, bool p_waited) {
#ifdef UNIX_ENABLED
	// The wrapper reports the same read error for EOF and EAGAIN; inspect POLLHUP instead.
	const Ref<FileAccessUnixPipe> up = p_pipe;
	if (up.is_null()) {
		return true;
	}
	const int fd = up->get_read_fd();
	if (fd < 0) {
		return true;
	}
	pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (::poll(&pfd, 1, 0) <= 0) {
		return false; // A failed query is inconclusive; check again later.
	}
	return (pfd.revents & POLLHUP) != 0;
#elif defined(WINDOWS_ENABLED)
	return p_pipe->eof_reached(); // PeekNamedPipe distinguishes idle writers from closed writers.
#else
	return p_waited; // Without direct pipe inspection, wait for child completion.
#endif
}

// Check whether the target can be launched before starting it.
// Return a reason on failure or empty when launch is possible.
//
// The underlying fork adapter converts execvp failure to exit code 127.
// Its parent therefore sees only a child exit status.
// Preflight checks help distinguish launch failure from a child deliberately returning 127.
String cannot_run(const String &p_path) {
#ifdef UNIX_ENABLED
	// A name without separators is resolved through PATH, so skip direct-path inspection.
	if (!p_path.contains("/")) {
		return String();
	}
	const CharString raw = p_path.utf8();
	struct stat st = {};
	if (::stat(raw.get_data(), &st) != 0) {
		SourceError::posix(errno);
		return vformat("cannot start %s", p_path);
	}
	if (S_ISDIR(st.st_mode)) {
		SourceError::posix(EISDIR);
		return vformat("cannot start %s", p_path);
	}
	if (::access(raw.get_data(), X_OK) != 0) {
		SourceError::posix(errno);
		return vformat("cannot start %s", p_path);
	}
#endif
	return String();
}

} //namespace

// Wait for child exit on a worker while draining output.
void ProcWaitJob::run() {
	if (call.is_null()) {
		return;
	}
	call->launch_on_worker();
	if (call->pid == 0) {
		return;
	}
	OS *os = OS::get_singleton();
	// Poll process liveness and pipes at short intervals through the available adapter.
	// Drain without sleeping while output remains, including small Windows pipe buffers.
	for (;;) {
		if (!call->killed.is_set() && call->deadline > 0 && GDClock::msec() > call->deadline) {
			call->killed.set();
			call->reap_now();
		}
		bool done = false;
		{
			MutexLock lock(call->reap_mutex);
			if (call->reaped) {
				done = true; // Cancellation or deadline handling already reaped the child.
			} else if (!os->is_process_running(call->pid)) {
				// Reap under the shared lock to avoid racing cancellation's kill.
				// Both paths invoke waitpid, so concurrent calls could produce ECHILD.
				call->exit_code = os->get_process_exit_code(call->pid);
				os->release_process(call->pid);
				call->reaped = true;
				done = true;
			}
		}
		const bool moved = call->pull_output(done);
		if (call->output_failed) {
			call->reap_now();
			break;
		}
		const bool output_done = !call->want_output ||
				((call->out_pipe.is_null() || call->out_eof) && (call->err_pipe.is_null() || call->err_eof));
		if (done && (call->killed.is_set() || output_done)) {
			break;
		}
		if (!moved) {
			os->delay_usec(2000);
		}
	}
}

// Deliver final output and exit state on the main thread before releasing references.
void ProcWaitJob::finish() {
	if (call.is_valid()) {
		call->emit_now();
	}
	call.unref();
}

// Terminate unlimited children during pool shutdown to release worker waits.
void ProcWaitJob::cancel_on_shutdown() {
	if (call.is_valid()) {
		call->cancel();
	}
}

// Start the child process.
void GDProcCall::begin(const String &p_path, const PackedStringArray &p_args, const Dictionary &p_opts) {
	self_hold = Ref<GDProcCall>(this);
	Async::watch(callable_mp(this, &GDProcCall::finish_now), true);

	// Authorize executable names and paths using the same permission rules.
	start_path = p_path;
	if (!Perm::check(Perm::RUN, p_path)) {
		reject_kind = Err::PERMISSION_DENIED;
		why = vformat("requires run access to \"%s\", run again with the --allow-run flag.", p_path);
		return;
	}
	const double timeout = p_opts.get("timeout", double(WAIT_MS) / 1000.0);
	uint64_t wait = 0;
	if (!Limit::seconds_ms(timeout, wait)) {
		reject_kind = Err::INVALID_DATA;
		why = "timeout must be zero or a positive number of seconds";
		return;
	}
	deadline = deadline_after(wait);
	// Capture output only when requested; otherwise inherit the parent's standard streams.
	want_output = p_opts.get("output", true);
	start_args = p_args;
	// Run creation, exit waiting, and pipe draining on the same worker.
	Ref<ProcWaitJob> job;
	job.instantiate();
	job->call = Ref<GDProcCall>(this);
	job->submit();
	Async::watch(callable_mp(this, &GDProcCall::finish_now), false); // PoolJob owns completion delivery.
}

// Launch on a worker and retain its pipes on that same thread.
void GDProcCall::launch_on_worker() {
	if (killed.is_set()) {
		return;
	}
	SourceError::clear();
	const String cannot = cannot_run(start_path);
	if (!cannot.is_empty()) {
		start_error = SourceError::path(start_path, "exec", ERR_CANT_CREATE, cannot);
		return;
	}
	List<String> args;
	for (const String &a : start_args) {
		args.push_back(a);
	}
	OS *os = OS::get_singleton();
	ProcessID made_pid = 0;
	Ref<FileAccess> made_out;
	Ref<FileAccess> made_err;
	if (!want_output) {
		if (os->create_process(start_path, args, &made_pid) != OK) {
			start_error = SourceError::path(start_path, "exec", ERR_CANT_CREATE, vformat("cannot start %s", start_path));
			return;
		}
	} else {
		const Dictionary got = os->execute_with_pipe(start_path, args, false);
		if (got.is_empty()) {
			start_error = SourceError::path(start_path, "exec", ERR_CANT_CREATE, vformat("cannot start %s", start_path));
			return;
		}
		made_pid = (ProcessID)(int64_t)got.get("pid", 0);
		made_out = got.get("stdio", Variant());
		made_err = got.get("stderr", Variant());
	}
	if (made_pid == 0) {
		start_error = SourceError::path(start_path, "exec", ERR_CANT_CREATE, vformat("cannot start %s", start_path));
		return;
	}
	{
		MutexLock lock(reap_mutex);
		pid = made_pid;
		out_pipe = made_out;
		err_pipe = made_err;
	}
	if (killed.is_set()) {
		reap_now();
	}
}

// Drain worker-side buffered output, returning true when bytes were read.
// Use drained reads to determine EOF; FIONREAD returning zero is not sufficient.
// Zero available bytes can mean either no arrival yet or a closed writer.
bool GDProcCall::pull_output(bool p_done) {
	Ref<FileAccess> pipes[2] = { out_pipe, err_pipe };
	bool *eofs[2] = { &out_eof, &err_eof };
	bool moved = false;
	for (int i = 0; i < 2; i++) {
		const Ref<FileAccess> &p = pipes[i];
		if (p.is_null() || !p->is_open() || *eofs[i]) {
			continue;
		}
		int64_t took = 0;
		while (took < READ_BYTES_MAX) {
			// Read only the amount reported available.
			// This avoids empty nonblocking reads returning EAGAIN.
			const int64_t avail = (int64_t)p->get_length();
			if (avail <= 0) {
				// Mark EOF only after all writers close.
				// A reaped child is insufficient if a grandchild still owns a writer.
				if (writer_gone(p, p_done)) {
					*eofs[i] = true; // Pipe EOF.
				}
				break;
			}
			const int64_t want = MIN(avail, READ_BYTES_MAX - took);
			PackedByteArray chunk;
			if (chunk.resize(want) != OK) {
				output_failed = true;
				return moved;
			}
			const uint64_t got = p->get_buffer(chunk.ptrw(), want);
			if (got == 0) {
				break; // No bytes read; retry on a later step.
			}
			chunk.resize(got);
			took += got;
			moved = true;
			// Retain all requested combined output and propagate allocation failure.
			const int64_t at = out.size();
			if (chunk.size() > INT_MAX - 1 - at || out.resize(at + chunk.size()) != OK) {
				output_failed = true;
				return moved;
			}
			memcpy(out.ptrw() + at, chunk.ptr(), chunk.size());
		}
	}
	return moved;
}

// Report pre-launch failures on a turn after returning the completion signal.
void GDProcCall::finish_now() {
	if (self_hold.is_null()) {
		return;
	}
	// Deliver launch failure on the next event-loop turn.
	if (!why.is_empty()) {
		emit_now();
		return;
	}
}

// Emit the result exactly once and clean up.
void GDProcCall::emit_now() {
	// Retain the call through cleanup; do not access it after releasing the final reference.
	Ref<GDProcCall> keep(this);
	Async::watch(callable_mp(this, &GDProcCall::finish_now), false);
	for (const Ref<FileAccess> &p : { out_pipe, err_pipe }) {
		if (p.is_valid() && p->is_open()) {
			p->close();
		}
	}
	out_pipe.unref();
	err_pipe.unref();

	Ref<R> result;
	if (start_error.is_valid()) {
		result = start_error;
	} else if (output_failed) {
		result = R::err("cannot allocate process output buffer", Err::LIMITED);
	} else if (!why.is_empty()) {
		// Distinguish permission denial from launch failure and attach the path and original cause.
		result = reject_kind != Err::NONE ? R::err(why, reject_kind)
						: SourceError::path(start_path, "exec", ERR_CANT_CREATE, why);
	} else if (canceled.is_set()) {
		result = R::err("process was canceled", Err::INTERRUPTED);
	} else if (killed.is_set()) {
		result = R::err("process did not finish in time", Err::TIMED_OUT);
	} else if (exit_code != 0) {
		Dictionary info;
		info["path"] = start_path;
		info["operation"] = "exec";
		info["code"] = exit_code;
		result = R::err(Err::make(vformat("process exited with status %d", exit_code), Err::NONE, info));
	}
	// Keep exit status and all captured output even when the operation fails.
	Dictionary d;
	d["code"] = exit_code;
	d["output"] = out.is_empty() ? String() : String::utf8((const char *)out.ptr(), out.size());
	result = result.is_valid() ? R::err(result->get_e(), Err::NONE, d) : R::ok(d);
	emit_signal("finished", result);
	start_error.unref();
	start_args.clear();
	self_hold.unref(); // Release self-ownership; the call may be destroyed here.
}

// Report cancellation as a completed result when the caller stops waiting.
// Without notification, awaiting callers would remain suspended forever.
void GDProcCall::cancel() {
	if (self_hold.is_null()) {
		return; // The call already completed.
	}
	canceled.set();
	killed.set();
	reap_now();
}

// Terminate and reap the child unless it has already been reaped.
// Use the worker's reaping lock because kill also invokes waitpid.
void GDProcCall::reap_now() {
	MutexLock lock(reap_mutex);
	if (reaped || pid == 0) {
		return; // Already reaped; the PID may now identify another process.
	}
	reaped = true; // Mark reaped before termination to exclude concurrent worker reaping.
	OS::get_singleton()->kill(pid); // The termination adapter also performs waitpid.
}

// Register completion and cancellation.
void GDProcCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDProcCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Start a child and return its exit-completion signal.
Signal Proc::run(const String &p_path, const PackedStringArray &p_args, const Dictionary &p_opts) {
	Ref<GDProcCall> call;
	call.instantiate();
	// Create the signal before any completion can destroy its source.
	// A child can exit immediately, so no network round-trip delay can protect signal setup.
	const Signal signal(call.ptr(), "finished");
	call->begin(p_path, p_args, p_opts);
	return signal;
}
