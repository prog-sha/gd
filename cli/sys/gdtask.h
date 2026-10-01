/**************************************************************************/
/*  gdtask.h                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Keep the result of a started operation until the script drops it.
// A started operation reports through a Signal, which forgets its result when nobody is listening yet.
// The task listens from the start, so a later await, all() or connect() still receives the result.

#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

class GDTask : public RefCounted {
	GDCLASS(GDTask, RefCounted);

	Signal source; // Completion signal of the operation this task follows.
	Vector<Variant> args; // Completion arguments recorded when the operation finishes.
	bool done = false; // Whether the operation has finished.

	Variant rang(const Variant **p_args, int p_count, Callable::CallError &r_err); // Record the operation's completion.
	void settle(const Vector<Variant> &p_args); // Keep the completion and notify listeners.
	void check_lost(); // Finish with an error when the operation vanished without completing.

protected:
	static void _bind_methods();

public:
	// Follow a started operation's completion signal.
	static Ref<GDTask> follow(const Signal &p_signal);
	// Return a signal that delivers this item's completion, whether it is a Signal or a task.
	static Signal signal_of(const Variant &p_item, bool p_pair = false);
	// Turn a task returned by a script into its completion signal, leaving other values unchanged.
	static Variant as_signal(const Variant &p_value);

	// Report whether the operation has finished; a finished task answers await at once.
	bool is_done();
	// Ask the operation to stop; its completion, usually an interruption, still arrives through this task.
	void cancel();
	// Return the signal a waiter connects to: one or two values as the operation reports, or always value and error.
	Signal signal(bool p_pair = false) { return Signal(this, p_pair ? "_finished_pair" : "finished"); }
	Variant packed() const; // Result as a single await sees it: one value, or [value, error].
	Variant value() const; // First completion argument.
	Variant error() const; // Second completion argument, or null.
};
