// Share only in-flight name resolution while completing each waiter independently.
#include "cli/net/lookup.h"
#include "cli/net/datagram.h"
#include "cli/sys/perm.h"
#include "cli/sys/task.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"

namespace {
HashMap<String, Ref<GDLookupJob>> groups; // Main-thread-owned registry, not a cache of completed DNS results.
bool stopping = false; // Prevent shutdown callbacks from starting new lookups.
}

// Run the OS resolver on a worker and retain its result until delivery.
void GDLookupJob::run() { result = GDLookup::all(host); }

// Remove the shared entry before callbacks can start a new lookup.
void GDLookupJob::finish() {
	const Ref<GDLookupJob> *active = groups.getptr(host);
	if (active && active->ptr() == this) groups.erase(host);
	while (!calls.is_empty()) {
		Ref<GDLookupCall> call = calls.front()->get();
		calls.pop_front();
		call->entry = nullptr;
		call->job.unref();
		call->done(result);
	}
	result.unref();
}

// Check each caller's permissions before joining an in-flight lookup for the same host.
Signal GDLookupCall::start(const String &p_host, bool p_single, int p_port) {
	const String host = p_host.contains(":") && !p_host.begins_with("[") ? "[" + p_host + "]" : p_host;
	const String target = p_port >= 0 ? vformat("%s:%d", host, p_port) : host;
	if (!Perm::check(Perm::NET, target)) return Async::ready(R::err("name lookup is not allowed", Err::PERMISSION_DENIED));
	if (stopping || Pool::is_stopping()) return Async::ready(R::err("worker pool stopped", Err::INTERRUPTED));
	Ref<GDLookupCall> call;
	call.instantiate();
	call->self_hold = call;
	call->single = p_single;
	call->port = p_port;
	const Signal out(call.ptr(), "finished");
	if (GDDatagram::is_ip(p_host)) {
		Async::post(call, callable_mp(call.ptr(), &GDLookupCall::done).bind(GDLookup::all(p_host))); // Numeric IP addresses do not need a resolver worker.
		return out;
	}
	Ref<GDLookupJob> job;
	bool fresh = false;
	if (const Ref<GDLookupJob> *active = groups.getptr(p_host)) job = *active;
	else {
		job.instantiate();
		job->host = p_host;
		groups.insert(p_host, job);
		fresh = true;
	}
	call->job = job;
	call->entry = job->calls.push_back(call);
	if (fresh && !job->submit()) {
		call->cancel();
		return Async::ready(R::err("worker pool stopped", Err::INTERRUPTED));
	}
	return out;
}

// Filter results with each caller's IP permissions rather than sharing authorization.
void GDLookupCall::done(const Ref<R> &p_result) {
	if (self_hold.is_null()) return;
	Ref<GDLookupCall> keep(this);
	self_hold.unref();
	Ref<R> out = p_result;
	if (out.is_valid() && out->get_ok()) {
		const PackedStringArray addresses = out->get_v();
		PackedStringArray allowed;
		for (const String &address : addresses) {
			if (port < 0 || Perm::check_net_ip(vformat("%s:%d", address, port))) allowed.push_back(address);
		}
		out = allowed.is_empty() ? R::err("no permitted address to connect", Err::PERMISSION_DENIED) :
				(single ? R::ok(allowed[0]) : R::ok(allowed));
	}
	emit_signal("finished", out.is_valid() ? out : R::err("name lookup failed", Err::NOT_FOUND));
}

// Remove a job from the shared registry when its last waiter leaves; the worker retains the OS call.
void GDLookupCall::cancel() {
	if (job.is_valid()) {
		if (entry) job->calls.erase(entry);
		entry = nullptr;
		const Ref<GDLookupJob> *active = groups.getptr(job->host);
		if (job->calls.is_empty() && active && active->ptr() == job.ptr()) groups.erase(job->host);
		job.unref();
	}
	done(R::err("name lookup canceled", Err::INTERRUPTED));
}

// Cancel all calls at shutdown without starting new resolver jobs.
void GDLookupCall::shutdown_all() {
	stopping = true;
	List<Ref<GDLookupCall>> calls;
	for (const KeyValue<String, Ref<GDLookupJob>> &group : groups) {
		for (const Ref<GDLookupCall> &call : group.value->calls) calls.push_back(call);
	}
	for (const Ref<GDLookupCall> &call : calls) call->cancel();
}

// Register lookup completion and individual cancellation.
void GDLookupCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDLookupCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}
