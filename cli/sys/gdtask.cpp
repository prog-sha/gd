/**************************************************************************/
/*  gdtask.cpp                                                            */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Keep a started operation's result so it can be awaited after other waits.
#include "cli/sys/gdtask.h"
#include "cli/sys/std.h"
#include "cli/sys/task.h"
#include "core/object/class_db.h"

// Follow a started operation's completion signal, or report a failure when it cannot be followed.
Ref<GDTask> GDTask::follow(const Signal &p_signal) {
	Ref<GDTask> task;
	task.instantiate();
	task->source = p_signal;
	Signal s = p_signal;
	// The bound reference keeps the task alive until the operation finishes, even when the script only connects to it.
	if (!s.get_object() || s.connect(Callable(task.ptr(), "_rang").bind(task), Object::CONNECT_ONE_SHOT) != OK) {
		task->args.push_back(Variant());
		task->args.push_back(Err::make("the operation's completion signal is unavailable", Err::INVALID_DATA));
		task->done = true;
	}
	return task;
}

// Return a signal that delivers the item's completion, replaying a finished task on the next turn.
Signal GDTask::signal_of(const Variant &p_item, bool p_pair) {
	if (p_item.get_type() == Variant::SIGNAL) {
		return p_item;
	}
	GDTask *task = Object::cast_to<GDTask>(p_item.get_validated_object());
	if (!task) {
		return Signal();
	}
	task->check_lost();
	if (!task->done) {
		return task->source; // The running operation keeps itself alive and declares its own result shape.
	}
	return p_pair || task->args.size() > 1 ? Async::ready_pair({ task->value(), task->error() }) : Async::ready(task->packed());
}

// Let native code wait on a task a script returned the same way it waits on a Signal.
Variant GDTask::as_signal(const Variant &p_value) {
	return Object::cast_to<GDTask>(p_value.get_validated_object()) ? Variant(signal_of(p_value)) : p_value;
}

// Record the completion and pass it on unchanged to listeners of this task.
Variant GDTask::rang(const Variant **p_args, int p_count, Callable::CallError &r_err) {
	r_err.error = Callable::CallError::CALL_OK;
	if (done || p_count < 1) {
		return Variant();
	}
	Vector<Variant> values;
	for (int i = 0; i < p_count - 1; i++) { // The last argument is the task's own bound reference.
		values.push_back(*p_args[i]);
	}
	settle(values);
	return Variant();
}

// Keep the completion, then notify listeners in both shapes.
void GDTask::settle(const Vector<Variant> &p_args) {
	done = true;
	args = p_args;
	Ref<GDTask> keep(this); // Listeners may drop the last script reference while being notified.
	LocalVector<const Variant *> ptrs;
	for (const Variant &arg : args) {
		ptrs.push_back(&arg);
	}
	emit_signalp(SNAME("finished"), ptrs.ptr(), ptrs.size());
	emit_signal(SNAME("_finished_pair"), value(), error());
}

// Finish with an error when the operation was freed without reporting, so no waiter hangs.
void GDTask::check_lost() {
	if (!done && !source.get_object()) {
		settle({ Variant(), Err::make("the operation ended without a result", Err::INTERRUPTED) });
	}
}

// Report whether the operation has finished, noticing one that vanished.
bool GDTask::is_done() {
	check_lost();
	return done;
}

// Ask the followed operation to stop; its completion still arrives through this task.
void GDTask::cancel() {
	Object *target = source.get_object();
	if (!done && target && target->has_method("cancel")) {
		target->call("cancel");
	}
}

// Pack the result the way awaiting the original signal would.
Variant GDTask::packed() const {
	if (args.size() == 1) {
		return args[0];
	}
	if (args.size() > 1) {
		Array values;
		for (const Variant &arg : args) {
			values.push_back(arg);
		}
		return values;
	}
	return Variant();
}

// Return the success value of a finished operation.
Variant GDTask::value() const {
	return args.is_empty() ? Variant() : args[0];
}

// Return the error of a finished two-result operation.
Variant GDTask::error() const {
	return args.size() > 1 ? args[1] : Variant();
}

// Expose waiting state, cancellation and the completion signal to scripts.
void GDTask::_bind_methods() {
	{
		MethodInfo mi;
		mi.name = "_rang";
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "_rang", &GDTask::rang, mi);
	}
	ClassDB::bind_method(D_METHOD("is_done"), &GDTask::is_done);
	ClassDB::bind_method(D_METHOD("cancel"), &GDTask::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)));
	ADD_SIGNAL(MethodInfo("_finished_pair", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}
