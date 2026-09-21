/**************************************************************************/
/*  task.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement asynchronous waits and cancellation declared in task.h.

#include "cli/sys/task.h"
#include "cli/run/loop.h"
#include "cli/sys/clock.h"

#include "cli/sys/sched.h"
#include "cli/sys/wait.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/rb_set.h"
#include "modules/gdscript/gdscript_function.h"

namespace {

// Order timers by deadline, then by registration so equal deadlines complete first-in first-out.
struct TimedWait {
	uint64_t due = 0; // Wakeup time in milliseconds.
	uint64_t seq = 0; // Registration order among equal deadlines.
	const void *owner = nullptr; // Wait owner the deadline belongs to.

	bool operator<(const TimedWait &p_other) const {
		return due == p_other.due ? seq < p_other.seq : due < p_other.due;
	}
};

uint64_t timer_seq = 0; // Next registration number.
HashMap<const void *, uint64_t> timer_seqs; // Registration number of each owner's timer, for removal.

RBSet<TimedWait> timed_waits; // Pending timers contributing to kernel wait deadlines.
HashSet<GDWait *> live_waits; // Pending waits cleaned up at shutdown.
HashMap<const void *, Callable> deadline_calls; // Callbacks waking only the deadline's target.

LocalVector<Async::Post> posted_calls; // Contiguous FIFO delivered on subsequent event-loop turns.
uint32_t posted_at = 0; // Next FIFO delivery position.
Mutex posted_mutex; // Protect worker enqueue against main-thread delivery.
SafeFlag posted_ready; // Publish whether the FIFO can be read without first taking its mutex.
HashMap<Callable, uint64_t> observers; // Observers for backends without per-operation callbacks.
uint64_t observer_seq = 0; // Generation distinguishing removal and re-registration during delivery.
SafeFlag notified; // Backend state-change notification.

// Wake only when the publishing thread can race the runtime's final queue check.
void wake_posted() {
	if (!Thread::is_main_thread() || !Object::cast_to<GDLoop>(OS::get_singleton()->get_main_loop())) {
		IdleWait::wake(false);
	}
}

// Convert public seconds into deadlines without undefined conversion or overflow.
uint64_t deadline_after(double p_sec) {
	const uint64_t now = GDClock::msec();
	if (!Math::is_finite(p_sec) || p_sec <= 0.0) {
		return now;
	}
	const double wait = p_sec * 1000.0;
	return wait >= double(UINT64_MAX - now) ? UINT64_MAX : now + (uint64_t)wait;
}

} // namespace

// ---------------- Individual waits ----------------

// Retain the wait until completion and register it for shutdown cleanup.
void GDWait::retain() {
	self_hold = Ref<GDWait>(this);
	live_waits.insert(this);
}

// Deliver retained values on the next event-loop turn.
void GDWait::post() {
	if (posted || self_hold.is_null()) {
		return;
	}
	posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWait::step));
}

// Add or remove an asynchronous timer from the deadline registry.
void GDWait::time(bool p_on) {
	if (p_on == timed) {
		return;
	}
	timed = p_on;
	if (p_on) {
		Async::track_deadline(this, due, callable_mp(this, &GDWait::step));
	} else {
		Async::drop_deadline(this, due);
	}
}

// Resolve immediate and suspended Callable results through the same completion path.
void GDWait::spawn_result(const Variant &p_value) {
	Signal signal;
	Object *obj = p_value.get_type() == Variant::OBJECT ? p_value.get_validated_object() : nullptr;
	spawn_resume = obj && obj->has_signal("completed");
	if (p_value.get_type() == Variant::SIGNAL) {
		signal = p_value;
	} else if (spawn_resume) {
		signal = Signal(obj, "completed");
	} else {
		done(p_value);
		return;
	}
	mode = ONE;
	hold_obj = Ref<RefCounted>(Object::cast_to<RefCounted>(signal.get_object()));
	if (signal.get_object() && signal.connect(Callable(this, "rang").bind(0), Object::CONNECT_ONE_SHOT) == OK) return;
	hold_obj.unref();
	done(R::err("callable returned an unavailable signal", Err::INVALID_DATA));
}

// Advance the asynchronous wait state.
void GDWait::step() {
	posted = false;
	if (self_hold.is_null()) {
		return;
	}
	if (mode == SPAWN) {
		Variant ret;
		Callable::CallError err;
		const Callable fn = spawn_fn;
		spawn_fn = Callable();
		const bool sliced = GDScriptFunction::begin_time_slice();
		{
			GDScriptFunction::SuspendableCall suspendable;
			fn.callp(nullptr, 0, ret, err);
		}
		if (sliced) {
			GDScriptFunction::end_time_slice();
		}
		spawn_result(err.error == Callable::CallError::CALL_OK ? ret : Variant(R::err("cannot start the callable", Err::INVALID_DATA)));
		return;
	}
	if (cancel_pending) {
		cancel_pending = false;
		cancel_target();
		return;
	}
	if (pending) {
		// With nothing to await, deliver the aggregate or already-ready value.
		done(has_ready_value ? ready_value : Variant(values));
		return;
	}
	if (GDClock::msec() < due) {
		return;
	}
	// Treat expiry during a race as the timer winning.
	done(mode == TIMER ? Variant() : Variant(due_at));
}

// Pack signal arguments consistently across asynchronous consumers.
Variant Async::signal_value(const Variant **p_args, int p_count) {
	if (p_count == 1) {
		return *p_args[0];
	}
	if (p_count > 1) {
		Array values;
		for (int i = 0; i < p_count; i++) {
			values.push_back(*p_args[i]);
		}
		return values;
	}
	return Variant();
}

// Receive completion from an input signal.
Variant GDWait::rang(const Variant **p_args, int p_count, Callable::CallError &r_err) {
	r_err.error = Callable::CallError::CALL_OK;
	// The last bound argument is the index; preceding arguments are the signal's values.
	const int p_at = p_count > 0 ? (int)*p_args[p_count - 1] : 0;
	Variant p_value = Async::signal_value(p_args, p_count - 1);
	if (mode == RACE) {
		if (hit < 0) {
			hit = p_at;
			done(p_at);
		}
		return Variant();
	}
	if (mode == ONE) {
		// An operation stopped by the context reports the context's reason, which names why it was stopped.
		Ref<R> failed = p_value;
		Ref<GDAsyncContext> ctx = cancel_context;
		if (failed.is_valid() && !failed->get_ok() && ctx.is_valid() && ctx->is_done() && ctx->get_reason().is_valid()) {
			p_value = R::err(ctx->get_reason(), Err::NONE, failed->get_v());
		}
		// Defer synchronous context cancellation until the caller can connect on the next turn.
		hold_obj.unref();
		if (spawn_resume) {
			spawn_result(p_value);
		} else if (defer_one) {
			ready_value = p_value;
			has_ready_value = true;
			pending = true;
			post();
		} else {
			done(p_value);
		}
		return Variant();
	}
	// For ALL, retain input order while waiting for every value.
	if (p_at < values.size()) {
		values[p_at] = p_value;
	}
	left--;
	if (left <= 0) {
		for (const KeyValue<int, int> &e : shared) {
			values[e.key] = values[e.value];
		}
		done(values);
	}
	return Variant();
}

// Forward context cancellation to the retained operation.
void GDWait::cancel_target() {
	Ref<GDWait> keep(this);
	if (hold_obj.is_valid() && hold_obj->has_method("cancel")) {
		hold_obj->call("cancel");
	}
}

// Stop owned GDWait inputs without modifying arbitrary external signal sources.
void GDWait::drop_rivals() {
	for (int i = 0; i < rivals.size(); i++) {
		const Signal s = rivals[i];
		Object *obj = s.get_object();
		if (!obj) {
			continue;
		}
		GDWait *other = Object::cast_to<GDWait>(obj);
		if (other) {
			if (other != this && other->self_hold.is_valid()) {
				// Retain the target before clearing its self-reference.
				// Otherwise destruction could release the same target again while its count is already zero.
				// The temporary reference defers destruction until after the member is cleared.
				Ref<GDWait> hold(other);
				other->time(false);
				live_waits.erase(other);
				other->self_hold.unref();
			}
			continue;
		}
		// Notify cancellable sources when their result is no longer awaited.
		// This releases their resources before their own deadlines; extensions may implement the same cancel contract.
		if (obj->has_method("cancel")) {
			obj->call("cancel");
		}
	}
	rivals.clear();
}

// Complete the wait and deliver its result.
void GDWait::done(const Variant &p_value) {
	if (self_hold.is_null()) {
		return; // Repeated completion does nothing.
	}
	// Retain the wait before clearing its self-reference to avoid recursive release during destruction.
	// The temporary reference keeps it alive until this function returns.
	Ref<GDWait> keep(this);
	time(false);
	if (cancel_context.is_valid()) {
		const Callable stop = callable_mp(this, &GDWait::cancel_target);
		if (cancel_context->is_connected("canceled", stop)) {
			cancel_context->disconnect("canceled", stop);
		}
		cancel_context.unref();
	}
	// Choose the winner before cancelling losing inputs.
	// Cancellation may emit results synchronously and must not overwrite the winner.
	if (mode == RACE && hit < 0) {
		hit = p_value.get_type() == Variant::INT ? (int)p_value : 0;
	}
	drop_rivals();
	live_waits.erase(this);
	emit_signal("finished", p_value);
	self_hold.unref(); // Release self-ownership.
}

// Disconnect monitoring and release retained resources without delivering a result.
void GDWait::abandon() {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDWait> keep(this);
	time(false);
	if (cancel_context.is_valid()) {
		const Callable stop = callable_mp(this, &GDWait::cancel_target);
		if (cancel_context->is_connected("canceled", stop)) {
			cancel_context->disconnect("canceled", stop);
		}
		cancel_context.unref();
	}
	drop_rivals();
	live_waits.erase(this);
	self_hold.unref();
}

// Silently release all event-loop waits at process shutdown.
void GDWait::shutdown_all() {
	LocalVector<Ref<GDWait>> waits;
	for (GDWait *wait : live_waits) {
		waits.push_back(Ref<GDWait>(wait));
	}
	for (const Ref<GDWait> &wait : waits) {
		wait->abandon();
	}
}

// Register public script methods and properties.
void GDWait::_bind_methods() {
	// Accept variable argument counts because signal signatures differ.
	{
		MethodInfo mi;
		mi.name = "rang";
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "rang", &GDWait::rang, mi);
	}
	ClassDB::bind_method(D_METHOD("cancel"), &GDWait::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)));
}

// Stop waiting on behalf of a cancelled context and report the interruption to the awaiting caller.
void GDWait::cancel() {
	if (self_hold.is_null()) {
		return;
	}
	drop_rivals();
	done(R::err("wait was canceled", Err::INTERRUPTED));
}

// ---------------- Wait construction ----------------

// Retain completion work and enqueue it for a main-loop turn.
void Async::post(const Ref<RefCounted> &p_hold, const Callable &p_call) {
	if (!p_call.is_valid()) {
		return;
	}
	{
		MutexLock lock(posted_mutex);
		posted_calls.push_back(Async::Post{ p_hold, p_call });
		posted_ready.set();
	}
	// The task loop checks this queue before sleeping; its own thread cannot race that check.
	// Other threads still wake the backend after publishing work under the queue mutex.
	wake_posted();
}

// Publish a collected batch without taking the FIFO mutex for every descriptor.
void Async::post_many(const LocalVector<Callable> &p_calls) {
	if (p_calls.is_empty()) return;
	{
		MutexLock lock(posted_mutex);
		for (const Callable &call : p_calls) {
			posted_calls.push_back(Async::Post{ Ref<RefCounted>(), call });
		}
		posted_ready.set();
	}
	wake_posted();
}

// Move retained callbacks into the shared FIFO with one publication and wakeup.
void Async::post_many(LocalVector<Post> &&p_posts) {
	if (p_posts.is_empty()) return;
	{
		MutexLock lock(posted_mutex);
		for (Post &post : p_posts) {
			posted_calls.push_back(std::move(post));
		}
		posted_ready.set();
	}
	wake_posted();
}

// Notify main of worker or callback-free backend state changes.
void Async::notify() {
	notified.set();
}

// Observe callback-free backends through notifications rather than periodic polling.
void Async::watch(const Callable &p_call, bool p_on) {
	if (p_on) {
		if (!observers.has(p_call)) {
			observers.insert(p_call, ++observer_seq);
			post(Ref<RefCounted>(), p_call);
		}
	} else {
		observers.erase(p_call);
	}
}

// Keep caller monitoring state consistent with observer registration.
void Async::watch(const Callable &p_call, bool p_on, bool &r_on) {
	watch(p_call, p_on);
	r_on = p_on;
}

// Deliver deadlines and ready work in FIFO turns, returning promptly to I/O.
void Async::drain() {
	if (notified.clear_if_set()) {
		HashMap<Callable, uint64_t> pending;
		for (const KeyValue<Callable, uint64_t> &entry : observers) {
			pending.insert(entry.key, entry.value);
		}
		for (const KeyValue<Callable, uint64_t> &entry : pending) {
			const uint64_t *seq = observers.getptr(entry.key);
			if (seq && *seq == entry.value && entry.key.is_valid()) {
				entry.key.call();
			}
		}
	}
	// Deliver every timer due now, but not one registered by a delivery: that one waits a turn, after deferred calls.
	const uint64_t now = timed_waits.is_empty() ? 0 : GDClock::msec(); // Read time only when a deadline needs comparison.
	const uint64_t registered = timer_seq;
	while (!timed_waits.is_empty() && timed_waits.front()->get().due <= now && timed_waits.front()->get().seq < registered) {
		const TimedWait wait = timed_waits.front()->get();
		const Callable *found = deadline_calls.getptr(wait.owner);
		const Callable call = found ? *found : Callable();
		timed_waits.erase(wait);
		deadline_calls.erase(wait.owner);
		timer_seqs.erase(wait.owner);
		if (call.is_valid()) {
			call.call();
		}
	}
	Post post;
	if (posted_ready.is_set()) {
		MutexLock lock(posted_mutex);
		if (posted_at < posted_calls.size()) {
			post = posted_calls[posted_at];
			posted_calls[posted_at++] = Post();
		}
		gd_ready_compact(posted_calls, posted_at);
		posted_ready.set_to(posted_at < posted_calls.size());
	}
	if (post.call.is_valid()) {
		post.call.call();
	}
}

// Report undelivered work in the runtime ready queue.
bool Async::has_ready() {
	return notified.is_set() || posted_ready.is_set() || (!timed_waits.is_empty() && timed_waits.front()->get().due <= GDClock::msec());
}

// Release undelivered targets during process shutdown.
void Async::shutdown() {
	observers.clear();
	notified.clear();
	posted_calls.clear();
	posted_at = 0;
	posted_ready.clear();
	deadline_calls.clear();
	timed_waits.clear();
	timer_seqs.clear();
}

// Return a signal completing after the selected duration.
Signal Async::sleep(double p_sec) {
	return Signal(start_sleep(p_sec).ptr(), "finished");
}

// Start a wait that completes after the selected duration.
Ref<GDWait> Async::start_sleep(double p_sec) {
	Ref<GDWait> w;
	w.instantiate();
	w->retain();
	w->mode = GDWait::TIMER;
	w->due = deadline_after(p_sec);
	w->time(true);
	return w;
}

// Deliver an already-ready value after the caller has connected await.
Signal Async::ready(const Variant &p_value) {
	Ref<GDWait> w;
	w.instantiate();
	w->retain();
	w->mode = GDWait::ALL;
	w->pending = true;
	w->ready_value = p_value;
	w->has_ready_value = true;
	w->post();
	return Signal(w.ptr(), "finished");
}

// Start a Callable and return its completion signal.
// Retain coroutine state and forward its completed result.
// For immediate completion, deliver the returned value on the next turn.
// Keep concurrent work observable instead of discarding its completion.
Signal Async::spawn(const Callable &p_fn) {
	Ref<GDWait> w;
	w.instantiate();
	w->retain();
	w->mode = GDWait::SPAWN;
	w->spawn_fn = p_fn;
	w->post();
	return Signal(w.ptr(), "finished");
}

// Return the first asynchronous completion.
Signal Async::race(const Array &p_signals) {
	Ref<GDWait> w = start_race(p_signals);
	if (p_signals.is_empty()) {
		w->ready_value = -1;
		w->has_ready_value = true;
		w->pending = true;
		w->post(); // Even an empty race completes only after the caller can begin awaiting.
	}
	return Signal(w.ptr(), "finished");
}

// Start a race among asynchronous operations.
Ref<GDWait> Async::start_race(const Array &p_signals) {
	Ref<GDWait> w;
	w.instantiate();
	w->retain();
	w->mode = GDWait::RACE;
	w->rivals = p_signals;
	for (int i = 0; i < p_signals.size(); i++) {
		// Bind the input index while accepting any number of signal values.
		Signal s = p_signals[i];
		s.connect(Callable(w.ptr(), "rang").bind(i), Object::CONNECT_ONE_SHOT);
	}
	return w;
}

// Register all completions before starting supplied Callables, preserving input order.
Signal Async::all(const Array &p_signals) {
	Ref<GDWait> w;
	w.instantiate();
	w->retain();
	w->mode = GDWait::ALL;
	w->left = p_signals.size();
	w->values.resize(p_signals.size());
	for (int i = 0; i < p_signals.size(); i++) {
		const Variant item = p_signals[i];
		Signal s = item.get_type() == Variant::CALLABLE ? spawn(item) : (item.get_type() == Variant::SIGNAL ? Signal(item) : Signal());
		int earlier = -1;
		for (int j = 0; j < i && earlier < 0; j++) {
			if (p_signals[j].get_type() == Variant::SIGNAL && Signal(p_signals[j]) == s) {
				earlier = j;
			}
		}
		if (earlier >= 0) {
			w->shared.insert(i, earlier);
			w->left--;
			continue;
		}
		if (!s.get_object() || s.connect(Callable(w.ptr(), "rang").bind(i), Object::CONNECT_ONE_SHOT) != OK) {
			w->values[i] = R::err("all needs a live Signal or Callable", Err::INVALID_DATA);
			w->left--;
		}
	}
	if (w->left == 0) {
		w->pending = true; // Even an empty set completes only after the caller can begin awaiting.
		w->post();
	}
	return Signal(w.ptr(), "finished");
}

// Race an operation against a deadline.
Signal Async::with_timeout(const Signal &p_signal, double p_sec) {
	// Return zero when the signal wins or one when time expires.
	Array one;
	one.push_back(p_signal);
	Ref<GDWait> w = start_race(one);
	w->due = deadline_after(p_sec);
	w->due_at = 1;
	w->time(true);
	return Signal(w.ptr(), "finished");
}

// Forward context cancellation to a cancellable asynchronous operation.
Signal Async::with_context(const Ref<GDAsyncContext> &p_context, const Signal &p_signal) {
	ERR_FAIL_COND_V_MSG(p_context.is_null(), Signal(), "with_context needs a GDAsyncContext.");
	Object *target = p_signal.get_object();
	ERR_FAIL_NULL_V_MSG(target, Signal(), "with_context needs a live Signal.");
	ERR_FAIL_COND_V_MSG(!target->has_method("cancel"), Signal(), "with_context needs a cancelable operation.");
	Ref<RefCounted> held = Ref<RefCounted>(Object::cast_to<RefCounted>(target));
	ERR_FAIL_COND_V_MSG(held.is_null(), Signal(), "with_context needs a reference-counted operation.");
	Ref<GDWait> w;
	w.instantiate();
	w->retain();
	w->mode = GDWait::ONE;
	w->defer_one = true;
	w->hold_obj = held;
	w->cancel_context = p_context;
	Signal operation = p_signal;
	operation.connect(Callable(w.ptr(), "rang").bind(0), Object::CONNECT_ONE_SHOT);
	const Callable stop = callable_mp(w.ptr(), &GDWait::cancel_target);
	if (p_context->is_done()) {
		w->cancel_pending = true;
		w->post(); // Return cancellation only after the caller can connect to the signal.
	} else {
		p_context->connect("canceled", stop, Object::CONNECT_ONE_SHOT);
	}
	return Signal(w.ptr(), "finished");
}

// Return seconds until the nearest asynchronous timer.
double Async::time_to_next_timer() {
	if (timed_waits.is_empty()) {
		return -1.0;
	}
	const uint64_t now = GDClock::msec();
	const uint64_t nearest = timed_waits.front()->get().due;
	return nearest <= now ? 0.0 : double(nearest - now) / 1000.0;
}

// Register external I/O deadlines alongside asynchronous timers.
void Async::track_deadline(const void *p_owner, uint64_t p_due, const Callable &p_call) {
	if (!p_owner || p_due == 0) {
		return;
	}
	if (timer_seqs.has(p_owner)) {
		return;
	}
	const TimedWait wait{ p_due, timer_seq++, p_owner };
	const bool earlier = timed_waits.is_empty() || p_due < timed_waits.front()->get().due;
	timed_waits.insert(wait);
	timer_seqs.insert(p_owner, wait.seq);
	if (p_call.is_valid()) {
		deadline_calls.insert(p_owner, p_call);
	}
	if (earlier) {
		IdleWait::wake(); // Recompute the active kernel wait when an earlier deadline is added.
	}
}

// Remove the deadline of completed external I/O.
void Async::drop_deadline(const void *p_owner, uint64_t p_due) {
	const uint64_t *seq = p_owner && p_due > 0 ? timer_seqs.getptr(p_owner) : nullptr;
	if (seq) {
		timed_waits.erase(TimedWait{ p_due, *seq, p_owner });
		timer_seqs.erase(p_owner);
		deadline_calls.erase(p_owner);
	}
}

// Create a cancellable context.
Ref<GDAsyncContext> Async::ctx() {
	Ref<GDAsyncContext> c;
	c.instantiate();
	return c;
}

// ---------------- Cancellation contexts ----------------

void GDAsyncContext::cancel(const String &p_msg, Err::Kind p_kind) {
	if (done) {
		return;
	}
	done = true;
	reason = Err::make(p_msg, p_kind == Err::NONE ? Err::INTERRUPTED : p_kind);
	detach();
	emit_signal("canceled");
}

// Report context deadline expiry.
void GDAsyncContext::on_deadline() {
	cancel("deadline exceeded", Err::TIMED_OUT);
}

// Forward the parent's first cancellation reason unchanged.
void GDAsyncContext::on_parent() {
	if (parent.is_valid() && parent->reason.is_valid()) {
		cancel(parent->reason->get_msg(), parent->reason->get_kind());
	} else {
		cancel("canceled", Err::INTERRUPTED);
	}
}

// Detach parent and timer references so completed contexts are not retained.
void GDAsyncContext::detach() {
	if (deadline.is_valid()) {
		const Callable call = callable_mp(this, &GDAsyncContext::on_deadline).unbind(1);
		if (deadline->is_connected("finished", call)) {
			deadline->disconnect("finished", call);
		}
		deadline->abandon();
		deadline.unref();
	}
	if (parent.is_valid()) {
		const Callable call = callable_mp(this, &GDAsyncContext::on_parent);
		if (parent->is_connected("canceled", call)) {
			parent->disconnect("canceled", call);
		}
		parent.unref();
	}
}

// Create a child context with a deadline.
Ref<GDAsyncContext> GDAsyncContext::with_timeout(double p_sec) {
	Ref<GDAsyncContext> child = with_cancel();
	if (child->done) {
		return child;
	}
	child->deadline = Async::start_sleep(p_sec);
	child->deadline->connect("finished", callable_mp(child.ptr(), &GDAsyncContext::on_deadline).unbind(1), Object::CONNECT_ONE_SHOT);
	return child;
}

// Create an independently cancellable child context.
Ref<GDAsyncContext> GDAsyncContext::with_cancel() {
	Ref<GDAsyncContext> child;
	child.instantiate();
	child->parent = Ref<GDAsyncContext>(this);
	if (done) {
		child->on_parent();
		return child;
	}
	connect("canceled", callable_mp(child.ptr(), &GDAsyncContext::on_parent), Object::CONNECT_ONE_SHOT);
	return child;
}

GDAsyncContext::~GDAsyncContext() {
	detach();
}

// Register public script methods and properties.
void GDAsyncContext::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel", "msg", "kind"), &GDAsyncContext::cancel, DEFVAL("canceled"), DEFVAL(Err::NONE));
	ClassDB::bind_method(D_METHOD("with_cancel"), &GDAsyncContext::with_cancel);
	ClassDB::bind_method(D_METHOD("with_timeout", "sec"), &GDAsyncContext::with_timeout);
	ClassDB::bind_method(D_METHOD("is_done"), &GDAsyncContext::is_done);
	ClassDB::bind_method(D_METHOD("get_reason"), &GDAsyncContext::get_reason);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "reason", PROPERTY_HINT_RESOURCE_TYPE, "Err"), "", "get_reason");
	ADD_SIGNAL(MethodInfo("canceled"));
}
