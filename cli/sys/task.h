/**************************************************************************/
/*  task.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Compose asynchronous waits and cancellation.
// Combine completion aggregation with cancellation propagation.
// Use signals as the completion primitive for thin wait adapters.
//
// Scripts can await GD.async.sleep(0.5).

#include "cli/sys/std.h"

// One wait that emits finished when its condition is met.
class GDWait : public RefCounted {
	GDCLASS(GDWait, RefCounted);

	// Wait kind determining how completion is selected.
	enum Mode {
		TIMER, // Wait for a deadline.
		RACE, // Wait for the first signal.
		ALL, // Wait for every signal.
		ONE, // Forward one signal's result.
		SPAWN, // Start a Callable from the ready queue.
	};

	Ref<GDWait> self_hold; // Retain the wait until completion even when scripts release it.
	Mode mode = TIMER;
	uint64_t due = 0; // Deadline in milliseconds.
	int due_at = -1; // Index returned if the timer wins a race.
	int hit = -1; // Index of the first completed input.
	int left = 0; // Number of incomplete inputs.
	HashMap<int, int> shared; // Input index to the earlier index it repeats, for ALL over one signal given twice.
	Array values; // Values collected for ALL.
	Variant ready_value; // Value to forward when no suspension is needed.
	bool has_ready_value = false; // Distinguish null from an empty aggregate result.
	// Retain the continuation source of spawned work.
	// Without ownership, it could disappear before resumption.
	Ref<RefCounted> hold_obj;
	Callable spawn_fn; // Work started on the next runtime turn.
	Ref<RefCounted> cancel_context; // Retain the cancellation source until completion.
	Array rivals; // Signals participating in the race.
	bool pending = false; // No input remains to await; delivery is deferred to the next turn.
	bool cancel_pending = false; // Defer cancellation from an already-completed context.
	bool defer_one = false; // Retain synchronous ONE completion until the next turn.
	bool spawn_resume = false; // Resolve a spawned coroutine's eventual return value.
	bool timed = false; // Whether a deadline is registered with the event loop.
	bool posted = false; // Prevent duplicate insertion into the completion queue.

	void step(); // Advance the wait on event-loop notification.
	void spawn_result(const Variant &p_value); // Await returned operations or deliver a spawned value.
	void post(); // Deliver retained completion on the next event-loop turn.
	// Cancel losing timers rather than retaining them until expiry.
	void drop_rivals();
	// Receive an input signal with any argument count.
	Variant rang(const Variant **p_args, int p_count, Callable::CallError &r_err);
	void cancel_target(); // Forward context cancellation to the awaited operation.
	void done(const Variant &p_value);
	void abandon();
	void cancel(); // Release a wait without delivering a result.
	void retain(); // Retain through completion and register shutdown cleanup.
	void time(bool p_on); // Register or remove the deadline from kernel waiting.

	friend class Async;
	friend class GDAsyncContext;
	friend class GDWebApp;
	friend class GDHTTPTransport;
	friend class GDHTTPCall;

protected:
	static void _bind_methods();

public:
	static void shutdown_all(); // Cancel pending waits at process shutdown.
};

// Context propagating cancellation.
class GDAsyncContext : public RefCounted {
	GDCLASS(GDAsyncContext, RefCounted);

	Ref<Err> reason; // Cancellation reason.
	bool done = false;
	Ref<GDWait> deadline; // Wait implementing an optional deadline.
	Ref<GDAsyncContext> parent; // Parent cancellation source.

	void on_deadline();
	void on_parent();
	void detach();

protected:
	static void _bind_methods();

public:
	// Cancel once; repeated calls do nothing.
	void cancel(const String &p_msg, Err::Kind p_kind);
	// Create a child inheriting parent cancellation.
	Ref<GDAsyncContext> with_cancel();
	// Set a deadline that cancels the context automatically.
	Ref<GDAsyncContext> with_timeout(double p_sec);
	bool is_done() const { return done; }
	Ref<Err> get_reason() const { return reason; }
	~GDAsyncContext();
};

// Internal wait composition exposed through GD.async.
class Async {
public:
	// One retained callback published to the main-thread ready queue.
	struct Post {
		Ref<RefCounted> hold; // Target ownership kept through callback delivery.
		Callable call; // Callback delivered on its own FIFO turn.
	};

	static void notify(); // Notify state changes from callback-free backends, including workers.
	static void watch(const Callable &p_call, bool p_on); // Register an observer of backend notifications.
	static void watch(const Callable &p_call, bool p_on, bool &r_on); // Update observer registration and return its state to the caller.
	// Queue one main-thread call, retaining only objects needed through delivery.
	static void post(const Ref<RefCounted> &p_hold, const Callable &p_call);
	// Append collected callbacks as one FIFO batch; validate each target at delivery.
	static void post_many(const LocalVector<Callable> &p_calls);
	// Move retained callbacks into the FIFO under one queue lock.
	static void post_many(LocalVector<Post> &&p_posts);
	// Deliver queued completion work on the main thread.
	static void drain();
	static bool has_ready(); // Report immediately deliverable work.
	static void shutdown(); // Release undelivered references during shutdown.
	// Create an internal wait whose continuation is connected by native code.
	static Ref<GDWait> start_sleep(double p_sec);
	static Ref<GDWait> start_race(const Array &p_signals);
	// Deliver an already-ready value on the next turn.
	static Signal ready(const Variant &p_value);
	// Pack signal arguments into null, one value, or an array of values.
	static Variant signal_value(const Variant **p_args, int p_count);

	// Return signals suitable for await, such as GD.async.sleep(0.5).
	static Signal sleep(double p_sec);
	// Start a Callable and return its completion signal.
	// Support concurrent script coroutines without discarding their completion.
	static Signal spawn(const Callable &p_fn);
	// Start supplied Callables, await their results and supplied Signals, and preserve input order.
	static Signal all(const Array &p_signals);
	// Wait for the first signal and return its winning index.
	static Signal race(const Array &p_signals);
	// Return zero if completion beats the deadline, otherwise one.
	static Signal with_timeout(const Signal &p_signal, double p_sec);
	// Propagate context cancellation and return the operation's completion value.
	static Signal with_context(const Ref<GDAsyncContext> &p_context, const Signal &p_signal);
	// Return seconds until the nearest asynchronous timer.
	static double time_to_next_timer();
	// Add an external I/O deadline to kernel waiting and remove it on completion.
	static void track_deadline(const void *p_owner, uint64_t p_due, const Callable &p_call = Callable());
	static void drop_deadline(const void *p_owner, uint64_t p_due);
	static Ref<GDAsyncContext> ctx();
};
