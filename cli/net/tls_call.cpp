// Complete encrypted listener startup without blocking the event loop on files.
#include "cli/net/tls_call.h"
#include "core/object/class_db.h"
#include "cli/sys/task.h"

// Read credentials with the submitting operation's permissions.
void GDWebTLSCall::run() {
	result = GDTLSIdentity::load(cert, key, options);
}

// Reject stale startup completions rather than reopening a stopped application.
void GDWebTLSCall::finish() {
	if (app->opening != token) result = { Variant(), Err::make("TLS listen cancelled", Err::INTERRUPTED) };
	else {
		app->opening.unref();
		if (Ref<Err>(result.error).is_null()) result = app->listen_at(port, host, result.value);
	}
	app.unref();
	Async::finish(this, SNAME("finished"), result.value, result.error);
}

// Register the asynchronous startup result.
void GDWebTLSCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}

// Return an awaitable startup operation without binding before identity validation succeeds.
Signal GDWebApp::listen_tls(int64_t p_port, const String &p_cert, const String &p_key, const String &p_host, const Dictionary &p_opts) {
	if (opening.is_valid() || srv.is_valid() || shutting) return Async::ready_pair({ Variant(), Err::make("server already started", Err::ALREADY_EXISTS) });
	Ref<GDWebTLSCall> call;
	call.instantiate();
	opening.instantiate();
	call->token = opening;
	call->app = Ref<GDWebApp>(this);
	call->port = p_port;
	call->host = p_host;
	call->cert = p_cert;
	call->key = p_key;
	call->options = p_opts.duplicate(true);
	const Signal signal(call.ptr(), "finished");
	if (!call->submit()) {
		opening.unref();
		return Async::ready_pair({ Variant(), Err::make("worker pool stopped", Err::INTERRUPTED) });
	}
	return signal;
}
