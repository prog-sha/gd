// Complete encrypted listener startup without blocking the event loop on files.
#include "cli/net/tls_call.h"
#include "core/object/class_db.h"

// Read credentials with the submitting operation's permissions.
void GDWebTLSCall::run() { result = GDTLSIdentity::load(cert, key, options); }

// Reject stale startup completions rather than reopening a stopped application.
void GDWebTLSCall::finish() {
	if (app->opening != token) result = R::err("TLS listen cancelled", Err::INTERRUPTED);
	else {
		app->opening.unref();
		if (result->get_ok()) result = app->listen_at(port, host, result->get_v());
	}
	app.unref();
	emit_signal("finished", result);
}

// Register the asynchronous startup result.
void GDWebTLSCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// Return an awaitable startup operation without binding before identity validation succeeds.
Signal GDWebApp::listen_tls(int64_t p_port, const String &p_cert, const String &p_key, const String &p_host, const Dictionary &p_opts) {
	if (opening.is_valid() || srv.is_valid() || shutting) return Async::ready(R::err("server already started", Err::ALREADY_EXISTS));
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
		return Async::ready(R::err("worker pool stopped", Err::INTERRUPTED));
	}
	return signal;
}
