/**************************************************************************/
/*  file_job.cpp                                                          */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement asynchronous file-operation jobs declared in file_job.h.

#include "cli/sys/file_job.h"
#include "cli/sys/task.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

// Run a generic computation on a worker.
void GDValueCall::run() {
	outcome = work ? work() : Variant();
}

// Emit the computation result on the main thread.
void GDValueCall::finish() {
	Variant out = outcome;
	outcome = Variant();
	work = nullptr;
	Async::finish(this, SNAME("finished"), out);
}

// Submit a generic computation to a worker.
Signal GDValueCall::start(std::function<Variant()> p_work, bool p_cpu) {
	Ref<GDValueCall> call;
	call.instantiate();
	call->work = std::move(p_work);
	const Signal signal(call.ptr(), "finished");
	if (!call->submit(p_cpu)) {
		return Async::ready(Err::make("worker pool stopped", Err::INTERRUPTED));
	}
	return signal;
}

// Register completion carrying an arbitrary result type.
void GDValueCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)));
}

// Compute both result slots on a worker thread.
void GDPairCall::run() {
	outcome = work();
}

// Emit both result slots after returning to the main thread.
void GDPairCall::finish() {
	work = nullptr;
	Async::finish(this, SNAME("finished"), outcome.value, outcome.error);
	outcome = VariantPair();
}

// Submit a computation whose completion carries value and Err separately.
Signal GDPairCall::start(std::function<VariantPair()> p_work, bool p_cpu, bool p_serial) {
	Ref<GDPairCall> call;
	call.instantiate();
	call->work = std::move(p_work);
	const Signal signal(call.ptr(), "finished");
	if (!call->submit(p_cpu, p_serial)) {
		call->outcome = { Variant(), Err::make("worker pool stopped", Err::INTERRUPTED) };
		Async::post(Ref<RefCounted>(call.ptr()), callable_mp(call.ptr(), &GDPairCall::finish));
	}
	return signal;
}

// Declare the two completion slots for awaiters.
void GDPairCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}

// Parse the format on a CPU worker.
void GDFormatJob::run() {
	if (call.is_valid()) {
		call->parse_on_worker();
	}
}

// Deliver the parsing result on the main thread.
void GDFormatJob::finish() {
	if (call.is_valid()) {
		call->parsed();
	}
	call.unref();
}

// Receive I/O results and start CPU parsing only after a successful read.
void GDFormatCall::loaded(const Variant &p_value, const Variant &p_error) {
	input = { p_value, p_error };
	if (Ref<Err>(p_error).is_valid()) {
		outcome = input;
		parsed();
		return;
	}
	Ref<GDFormatJob> job;
	job.instantiate();
	job->call = Ref<GDFormatCall>(this);
	if (!job->submit(true)) {
		outcome = { Variant(), Err::make("worker pool stopped", Err::INTERRUPTED) };
		parsed(); // Release the self-reference without starting another worker during shutdown.
	}
}

// Decode received content on a CPU worker.
void GDFormatCall::parse_on_worker() {
	outcome = parse ? parse(input.value) : VariantPair{ Variant(), Err::make("no format parser was given", Err::INVALID_DATA) };
}

// Emit the final result and release retained input.
void GDFormatCall::parsed() {
	Ref<GDFormatCall> keep(this);
	const VariantPair result = outcome;
	input = VariantPair();
	outcome = VariantPair();
	parse = nullptr;
	Async::finish(this, SNAME("finished"), result.value, result.error);
	self_hold.unref();
}

// Queue file reading for I/O and send only successful results to the CPU queue.
Signal GDFormatCall::start(std::function<VariantPair()> p_read, std::function<VariantPair(const Variant &)> p_parse) {
	Ref<GDFormatCall> call;
	call.instantiate();
	call->self_hold = call;
	call->parse = std::move(p_parse);
	const Signal signal(call.ptr(), "finished");
	Signal loaded = GDPairCall::start(std::move(p_read), false);
	loaded.connect(callable_mp(call.ptr(), &GDFormatCall::loaded), Object::CONNECT_ONE_SHOT);
	return signal;
}

// Register the completion signal.
void GDFormatCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}
