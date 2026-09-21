/**************************************************************************/
/*  pool.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement worker pools declared in pool.h.
//
// Use separate queues for I/O waits and CPU jobs.
// Grow I/O workers independently while limiting active CPU jobs to processor capacity.

#include "cli/sys/pool.h"
#include "cli/sys/system.h"

#include "cli/sys/task.h"
#include "cli/sys/wait.h"

#include "core/object/callable_mp.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/string/string_builder.h"
#include "core/templates/list.h"
#include "modules/gdscript/gdscript_function.h"

namespace {

SafeFlag shutdown_flag; // Prevent any worker queue from restarting during process shutdown.

// Protect pool state shared by the main thread and workers with a mutex.
struct PoolState {
	Mutex mutex;
	Semaphore ready; // Notify workers when jobs arrive.
	Semaphore slots; // Slots for concurrently executing CPU jobs.
	List<Ref<PoolJob>> jobs; // Jobs in submission order.
	List<Ref<PoolJob>> all; // All jobs whose finish callback remains pending.
	LocalVector<Thread *> threads; // Running worker threads.
	int active = 0; // Jobs currently executing on workers.
	int blocked = 0; // Jobs that yielded CPU capacity while waiting for main-thread callbacks.
	int capacity = 0; // CPU capacity fixed on first use.
	bool stopping = false; // Prevent growth during shutdown.
};

thread_local PoolState *running = nullptr; // Worker queue owning the current native job.
Mutex text_mutex; // Protect worker requests for Object string conversion.
SafeFlag text_ready; // Publish whether new text callbacks await main-thread transfer.

// Convert required objects on main while retaining the native worker stack.
class TextCall : public RefCounted {
public:
	Variant value; // Input retained until callback completion.
	String text; // Text produced by main and returned to the worker.
	Semaphore ready; // Wait for main-thread completion or shutdown cancellation.
	List<Ref<TextCall>>::Element *entry = nullptr; // Position in the pending request registry.
	Ref<GDScriptFunctionState> continuation; // VM continuation while a formatting callback awaits I/O.
	void dispatch(); // Evaluate once on main and wake the worker.
	void complete(const Variant &p_text); // Receive the final callback value and wake the worker.
	void aborted(); // Complete requests whose owning script discarded their continuation.
	void disconnect(); // Disconnect VM continuation notifications.
	void cancel(); // Detach VM callbacks and release the worker during shutdown.
};

List<Ref<TextCall>> text_calls; // Requests awaiting main-thread dispatch or completion.
List<Ref<TextCall>> new_text_calls; // Requests transferred to the ready queue by the next drain.

// Evaluate on main, release retained state, and resume native work.
void TextCall::dispatch() {
	{
		MutexLock lock(text_mutex);
		if (!entry) {
			return; // Shutdown already cancelled the request.
		}
	}
	// Invoke script formatting directly without returning suspended state to native String conversion.
	Object *object = value.get_type() == Variant::OBJECT ? value.get_validated_object() : nullptr;
	ScriptInstance *script = object ? object->get_script_instance() : nullptr;
	if (script && script->has_method(SNAME("_to_string"))) {
		Callable::CallError error;
		GDScriptFunction::SuspendableCall suspendable;
		const Variant result = script->callp(SNAME("_to_string"), nullptr, 0, error);
		if (error.error == Callable::CallError::CALL_OK) {
			continuation = result;
			if (continuation.is_valid()) {
				continuation->connect(SNAME("completed"), callable_mp(this, &TextCall::complete), Object::CONNECT_ONE_SHOT);
				continuation->connect(SNAME("cancelled"), callable_mp(this, &TextCall::aborted), Object::CONNECT_ONE_SHOT);
				return;
			}
			complete(result);
			return;
		}
		ERR_PRINT("Cannot call _to_string.");
		complete(String());
		return;
	}
	complete(String(value));
}

// Return only the final VM result after I/O waits finish to the native stack.
void TextCall::complete(const Variant &p_text) {
	Ref<TextCall> keep(this);
	{
		MutexLock lock(text_mutex);
		if (!entry) {
			return;
		}
		text_calls.erase(entry);
		entry = nullptr;
	}
	if (p_text.get_type() == Variant::STRING) {
		text = p_text;
	} else {
		ERR_PRINT("Wrong type for _to_string, must be a String.");
	}
	disconnect();
	value = Variant();
	ready.post();
}

// Release native waits even when the VM discards the continuation.
void TextCall::aborted() {
	complete(String("<Freed Object>"));
}

// Disconnect success and abort notifications to prevent repeated completion.
void TextCall::disconnect() {
	if (continuation.is_valid()) {
		const Callable done = callable_mp(this, &TextCall::complete);
		const Callable stop = callable_mp(this, &TextCall::aborted);
		if (continuation->is_connected(SNAME("completed"), done)) {
			continuation->disconnect(SNAME("completed"), done);
		}
		if (continuation->is_connected(SNAME("cancelled"), stop)) {
			continuation->disconnect(SNAME("cancelled"), stop);
		}
	}
	continuation.unref();
}

// Detach completion callbacks and leave no waiting worker at shutdown.
void TextCall::cancel() {
	entry = nullptr;
	disconnect();
	value = Variant();
	ready.post();
}

Mutex completed_mutex; // Protect the shared completion queue across all worker kinds.
List<Ref<PoolJob>> completed_jobs; // FIFO ordered by actual completion, not worker kind.
SafeFlag completed_ready; // Publish whether completed jobs await main-thread transfer.

// Return process-local I/O, CPU, or ordered-I/O queue state.
PoolState &state(bool p_cpu, bool p_serial = false) {
	static PoolState io;
	static PoolState cpu;
	static PoolState serial;
	return p_serial ? serial : (p_cpu ? cpu : io);
}

// Use processor capacity only for CPU jobs, independently of blocking I/O.
int max_threads(bool p_cpu, bool p_serial) {
	if (p_serial) {
		return 1;
	}
	if (!p_cpu) {
		return INT_MAX; // The common thread-creation entry enforces the process-wide safety budget.
	}
	const int cpus = GDSystem::cpus();
	return MAX(cpus, 1);
}

} //namespace

// Take one job at a time and sleep when none is queued.
void Pool::worker_loop(void *p_data) {
	PoolState &s = *static_cast<PoolState *>(p_data);
	for (;;) {
		s.ready.wait();
		Ref<PoolJob> job;
		{
			MutexLock lock(s.mutex);
			if (!s.jobs.is_empty()) {
				job = s.jobs.front()->get();
				s.jobs.pop_front();
				s.active++;
			}
			if (job.is_null()) {
				if (s.stopping) {
					return; // Stop the worker after its queue drains.
				}
				continue; // Another worker took the job after the wakeup.
			}
		}
		job->run_on_worker();
	}
}

// Run one worker job, enqueue completion, and wake the event loop.
void PoolJob::run_on_worker() {
	PoolState &s = state(cpu, serial);
	if (cpu && !serial) {
		s.slots.wait();
	}
	running = &s;
	started();
	const bool done = cpu && !serial ? run_slice() : (run(), true);
	running = nullptr;
	if (cpu && !serial) {
		s.slots.post();
	}
	{
		MutexLock lock(s.mutex);
		s.active--;
		if (!done) {
			queued();
			s.jobs.push_back(Ref<PoolJob>(this)); // Keep the existing registry entry and single completion.
		}
	}
	if (!done) {
		s.ready.post();
		return; // Another worker may now own the saved state; do not touch it again.
	}
	completed();
	{
		MutexLock lock(completed_mutex);
		completed_jobs.push_back(Ref<PoolJob>(this));
		completed_ready.set();
	}
	IdleWait::wake();
}

// Distinguish active CPU jobs from callback waiters and add only needed worker capacity.
void Pool::grow(void *p_state, bool p_cpu, bool p_serial) {
	PoolState &s = *static_cast<PoolState *>(p_state);
	if (!s.threads.is_empty() && int64_t(s.threads.size()) >= int64_t(s.active) + s.jobs.size()) {
		return; // Existing workers cover all work; capacity discovery cannot require another thread.
	}
	const int max = max_threads(p_cpu, p_serial);
	if (p_cpu && !p_serial && s.capacity == 0) {
		s.capacity = max;
		s.slots.post(max);
	}
	const int64_t limit = int64_t(max) + (p_cpu && !p_serial ? s.blocked : 0);
	const int64_t needed = MIN(limit, int64_t(s.active) + s.jobs.size());
	if (int64_t(s.threads.size()) < needed && !s.stopping) {
		Thread *t = memnew(Thread);
		s.threads.push_back(t);
		t->start(&Pool::worker_loop, &s);
	}
}

// Release CPU capacity during a main-thread callback wait and reacquire it on return.
bool Pool::stringify(const Variant &p_value, String &r_text) {
	if (!running) {
		return false;
	}
	Ref<TextCall> call;
	call.instantiate();
	call->value = p_value;
	{
		MutexLock lock(text_mutex);
		if (shutdown_flag.is_set()) {
			return true; // Do not start another script callback during shutdown.
		}
		call->entry = text_calls.push_back(call);
		new_text_calls.push_back(call);
		text_ready.set();
	}
	PoolState &s = *running;
	const bool cpu = &s == &state(true);
	if (cpu) {
		MutexLock lock(s.mutex);
		s.blocked++;
		s.slots.post();
		grow(&s, true, false);
	}
	IdleWait::wake();
	call->ready.wait();
	if (cpu) {
		s.slots.wait();
		MutexLock lock(s.mutex);
		s.blocked--;
	}
	r_text = call->text;
	return true;
}

// Preserve container display syntax without changing the underlying value types.
String Pool::text(const Variant &p_value, int p_depth) {
	if (!running) {
		return p_value.stringify(p_depth);
	}
	const Variant::Type type = p_value.get_type();
	if (type == Variant::OBJECT || type == Variant::CALLABLE || type == Variant::SIGNAL) {
		String out;
		stringify(p_value, out);
		return out;
	}
	if (type != Variant::ARRAY && type != Variant::DICTIONARY) {
		return p_value.stringify(p_depth);
	}
	ERR_FAIL_COND_V_MSG(p_depth > MAX_RECURSION, type == Variant::ARRAY ? "[...]" : "{ ... }", "Maximum container recursion reached!");
	// Quote string-like elements so nested values remain distinguishable from syntax.
	auto element = [&](const Variant &p_item) -> String {
		String out = text(p_item, p_depth + 1);
		switch (p_item.get_type()) {
			case Variant::STRING: return out.c_escape().quote();
			case Variant::STRING_NAME: return "&" + out.c_escape().quote();
			case Variant::NODE_PATH: return "^" + out.c_escape().quote();
			default: return out;
		}
	};
	StringBuilder out;
	if (type == Variant::ARRAY) {
		const Array items = p_value;
		out += "[";
		for (int i = 0; i < items.size(); i++) {
			if (i) out += ", ";
			out += element(items[i]);
		}
		out += "]";
	} else {
		const Dictionary items = p_value;
		out += "{ ";
		bool first = true;
		for (const KeyValue<Variant, Variant> &item : items) {
			if (!first) out += ", ";
			first = false;
			out += element(item.key);
			out += ": ";
			out += element(item.value);
		}
		out += " }";
	}
	return out.as_string();
}

// Submit work, growing idle capacity within the queue's budget when needed.
bool PoolJob::submit(bool p_cpu, bool p_serial) {
	if (shutdown_flag.is_set()) {
		return false; // Do not create workers while any queue is shutting down.
	}
	self_hold = Ref<PoolJob>(this);
	cpu = p_cpu;
	serial = p_serial;
	PoolState &s = state(cpu, serial);
	{
		MutexLock lock(s.mutex);
		if (s.stopping) {
			self_hold.unref();
			return false;
		}
		queued();
		s.jobs.push_back(Ref<PoolJob>(this));
		entry = s.all.push_back(Ref<PoolJob>(this));
		Pool::grow(&s, cpu, serial);
	}
	s.ready.post();
	return true;
}

// Remove a completed job and deliver its result on main.
void PoolJob::finish_on_main() {
	if (!entry) {
		return; // Do not complete a job already delivered during shutdown.
	}
	// Retain the job before releasing its self-reference.
	Ref<PoolJob> keep(this);
	{
		PoolState &s = state(cpu, serial);
		MutexLock lock(s.mutex);
		s.all.erase(entry);
		entry = nullptr;
	}
	finish();
	self_hold.unref();
}

// Deliver completions from all queues in actual completion order.
void Pool::drain(bool p_now) {
	if (!completed_ready.is_set()) {
		return;
	}
	List<Ref<PoolJob>> completed;
	{
		MutexLock lock(completed_mutex);
		completed = std::move(completed_jobs);
		completed_ready.clear();
	}
	LocalVector<Async::Post> posts;
	if (!p_now) {
		posts.reserve(completed.size());
	}
	for (Ref<PoolJob> &job : completed) {
		if (p_now) {
			job->finish_on_main();
		} else {
			posts.push_back(Async::Post{ job, callable_mp(job.ptr(), &PoolJob::finish_on_main) });
		}
	}
	Async::post_many(std::move(posts));
}

// Move completed worker jobs into the shared ready queue.
void Pool::drain() {
	List<Ref<TextCall>> calls;
	if (text_ready.is_set()) {
		MutexLock lock(text_mutex);
		calls = std::move(new_text_calls);
		text_ready.clear();
	}
	LocalVector<Async::Post> posts;
	posts.reserve(calls.size());
	for (const Ref<TextCall> &call : calls) {
		posts.push_back(Async::Post{ call, callable_mp(call.ptr(), &TextCall::dispatch) });
	}
	Async::post_many(std::move(posts));
	drain(false);
}

// Report whether any queue retains jobs awaiting completion delivery.
bool Pool::has_work() {
	for (PoolState *s : { &state(false, false), &state(true, false), &state(false, true) }) {
		MutexLock lock(s->mutex);
		if (!s->all.is_empty()) {
			return true;
		}
	}
	return false;
}

// Report whether a queue has stopped accepting new work.
bool Pool::is_stopping(bool p_cpu, bool p_serial) {
	if (shutdown_flag.is_set()) {
		return true;
	}
	PoolState &s = state(p_cpu, p_serial);
	MutexLock lock(s.mutex);
	return s.stopping;
}

// Shut down one worker queue.
void Pool::shutdown_one(void *p_data) {
	PoolState &s = *static_cast<PoolState *>(p_data);
	LocalVector<Thread *> taken;
	List<Ref<PoolJob>> jobs;
	int wake = 0;
	{
		MutexLock lock(s.mutex);
		if (s.stopping) {
			return;
		}
		s.stopping = true;
		taken = s.threads;
		s.threads.clear();
		wake = taken.size();
		jobs = s.all;
	}
	// Interrupt indefinitely waiting jobs first and let short jobs complete normally.
	for (Ref<PoolJob> &job : jobs) {
		job->cancel_on_shutdown();
	}
	for (int i = 0; i < wake; i++) {
		s.ready.post(); // Wake every sleeping worker so it can terminate.
	}
	for (Thread *t : taken) {
		t->wait_to_finish();
		memdelete(t);
	}
	// Deliver finished worker results directly because the event loop has stopped.
	drain(true);
	// Also deliver completions already moved to the ready queue while registry entries remain alive.
	for (;;) {
		Ref<PoolJob> job;
		{
			MutexLock lock(s.mutex);
			if (s.all.is_empty()) {
				break;
			}
			job = s.all.front()->get();
		}
		job->finish_on_main();
	}
	MutexLock lock(s.mutex);
	s.jobs.clear();
	s.all.clear();
	s.active = 0;
	s.stopping = false;
}

// Shut down all I/O and CPU worker threads.
void Pool::shutdown() {
	shutdown_flag.set();
	{
		MutexLock lock(text_mutex);
		for (Ref<TextCall> &call : text_calls) {
			call->cancel();
		}
		text_calls.clear();
		new_text_calls.clear();
		text_ready.clear();
	}
	shutdown_one(&state(false, false));
	shutdown_one(&state(true, false));
	shutdown_one(&state(false, true));
}
