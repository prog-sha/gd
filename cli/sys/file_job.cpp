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

// Run on a worker and store the result without emitting signals there.
void GDFileCall::run() {
	outcome = work ? work() : R::err("no file work was given", Err::INVALID_DATA);
}

// Deliver the result to its waiter on the main thread.
void GDFileCall::finish() {
	const Ref<R> out = outcome.is_valid() ? outcome : R::err("file work returned nothing", Err::INVALID_DATA);
	outcome.unref();
	work = nullptr;
	emit_signal("finished", out);
}

// Submit work and return its completion signal.
Signal GDFileCall::start(std::function<Ref<R>()> p_work, bool p_cpu, bool p_serial) {
	Ref<GDFileCall> call;
	call.instantiate();
	call->work = std::move(p_work);
	const Signal signal(call.ptr(), "finished");
	if (!call->submit(p_cpu, p_serial)) {
		return Async::ready(R::err("worker pool stopped", Err::INTERRUPTED));
	}
	return signal;
}

// Register the completion signal.
void GDFileCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Run a generic computation on a worker.
void GDValueCall::run() {
	outcome = work ? work() : Variant();
}

// Emit the computation result on the main thread.
void GDValueCall::finish() {
	Variant out = outcome;
	outcome = Variant();
	work = nullptr;
	emit_signal("finished", out);
}

// Submit a generic computation to a worker.
Signal GDValueCall::start(std::function<Variant()> p_work, bool p_cpu) {
	Ref<GDValueCall> call;
	call.instantiate();
	call->work = std::move(p_work);
	const Signal signal(call.ptr(), "finished");
	if (!call->submit(p_cpu)) {
		return Async::ready(R::err("worker pool stopped", Err::INTERRUPTED));
	}
	return signal;
}

// Register completion carrying an arbitrary result type.
void GDValueCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)));
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
void GDFormatCall::loaded(const Ref<R> &p_input) {
	input = p_input;
	if (input.is_null() || !input->get_ok()) {
		outcome = input;
		parsed();
		return;
	}
	Ref<GDFormatJob> job;
	job.instantiate();
	job->call = Ref<GDFormatCall>(this);
	if (!job->submit(true)) {
		outcome = R::err("worker pool stopped", Err::INTERRUPTED);
		parsed(); // Release the self-reference without starting another worker during shutdown.
	}
}

// Decode received content on a CPU worker.
void GDFormatCall::parse_on_worker() {
	outcome = parse ? parse(input) : R::err("no format parser was given", Err::INVALID_DATA);
}

// Emit the final result and release retained input.
void GDFormatCall::parsed() {
	Ref<GDFormatCall> keep(this);
	const Ref<R> result = outcome.is_valid() ? outcome : R::err("format parser returned nothing", Err::INVALID_DATA);
	input.unref();
	outcome.unref();
	parse = nullptr;
	emit_signal("finished", result);
	self_hold.unref();
}

// Queue file reading for I/O and send only successful results to the CPU queue.
Signal GDFormatCall::start(std::function<Ref<R>()> p_read, std::function<Ref<R>(const Ref<R> &)> p_parse) {
	Ref<GDFormatCall> call;
	call.instantiate();
	call->self_hold = call;
	call->parse = std::move(p_parse);
	const Signal signal(call.ptr(), "finished");
	Signal loaded = GDFileCall::start(std::move(p_read));
	loaded.connect(callable_mp(call.ptr(), &GDFormatCall::loaded), Object::CONNECT_ONE_SHOT);
	return signal;
}

// Register the completion signal.
void GDFormatCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}
