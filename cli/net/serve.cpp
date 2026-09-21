/**************************************************************************/
/*  serve.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement routed HTTP applications declared in serve.h.

#include "cli/net/serve.h"
#include "cli/sys/pool.h"
#include "cli/sys/clock.h"

#include "cli/sys/limit.h"
#include "cli/sys/file_job.h"
#include "cli/sys/mount.h"
#include "cli/sys/os.h"
#include "cli/sys/perm.h"
#include "cli/sys/sched.h"

#include "cli/api/cli.h"
#include "cli/api/text.h"
#include "cli/data/bytes.h"
#include "cli/data/codec.h"
#include "cli/data/json.h"
#include "cli/data/utf8.h"
#include "cli/net/body_source.h"
#include "cli/net/mw.h"

#include "cli/sys/native_file.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/main_loop.h"
#include "core/os/os.h"

#include "modules/gdscript/gdscript_function.h"

namespace {

// Default content types when a handler supplies none.
const char *TEXT_TYPE = "text/plain; charset=utf-8"; // Default for plain text responses.
const char *HTML_TYPE = "text/html; charset=utf-8";
const char *JSON_TYPE = "application/json";
const char *BYTES_TYPE = "application/octet-stream"; // Default for raw byte responses.
constexpr int COPY_BUFFER = 32 * 1024; // Buffer width for incremental copying.

// Supply a measured HEAD length to the shared response framing without retaining body bytes.
class HeadBody : public GDBodySource {
	int64_t length; // Encoded byte length reported by the corresponding GET response.
public:
	explicit HeadBody(int64_t p_length) : length(p_length) {} // Retain only the measured length.
	bool take(BodyChunk &r_chunk) override { r_chunk.clear(); return false; } // Never produce omitted body data.
	int64_t size() override { return length; } // Expose the representation length to response framing.
	bool done() override { return true; } // No body production remains pending.
	String error() override { return String(); } // Measurement completed before this source was created.
	void abort() override {} // No asynchronous work or buffers remain to cancel.
	void set_ready_callback(const Callable &p_call) override {} // An omitted body never needs a readiness notification.
};

// Preserve encoding failures and wrap only a completed response.
Ref<R> json_reply(const Variant &p_encoded, int64_t p_status) {
	if (p_encoded.get_type() == Variant::PACKED_BYTE_ARRAY) return R::ok(Http::bytes_out(p_encoded, JSON_TYPE, p_status));
	const Ref<R> result = p_encoded;
	return result.is_valid() ? result : R::err("invalid JSON encoder result");
}

// Preserve file errors while constructing the body needed by postprocessors.
Ref<R> file_reply(const String &p_path) {
	const Ref<R> read = Os::read_bytes(p_path);
	if (!read->get_ok()) return read;
	return R::ok(Http::bytes_out(read->get_v(), Media::by_path(p_path), 200));
}

// Convert dynamic callback failures into ordinary handler errors while preserving suspension.
template <typename... Args>
Variant web_call(const Callable &p_call, Args... p_args) {
	const Variant values[2] = { Variant(p_args)... };
	const Variant *args[] = { (&values[0]) + 0, (&values[0]) + 1 };
	Variant out;
	Callable::CallError error;
	GDScriptFunction::SuspendableCall suspendable;
	p_call.callp(args, sizeof...(p_args), out, error);
	return error.error == Callable::CallError::CALL_OK ? out : Variant(R::err("web callback could not be invoked", Err::INVALID_DATA));
}

// Choose the compact response path when Content-Type is the only header.
const Variant *only_type(const Dictionary &p_headers) {
	return p_headers.size() == 1 ? p_headers.getptr(SNAME("Content-Type")) : nullptr;
}

// Recognize same-origin relative references, treating schemes and network paths as external.
bool local_location(const String &p_raw) {
	// Caller values have already been decoded as URL components.
	// Decoding again could turn a safe reference such as %252f into a network path.
	if (p_raw.strip_edges() != p_raw) {
		return false; // Reject values that become external after header whitespace trimming.
	}
	const String s = p_raw.replace("\\", "/");
	if (s.begins_with("//")) {
		return false;
	}
	for (int i = 0; i < s.length(); i++) {
		const char32_t c = s[i];
		if (c == '/' || c == '?' || c == '#') {
			return true;
		}
		if (c == ':') {
			return i == 0; // A leading colon cannot form a scheme.
		}
		const bool first = i == 0 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
		const bool rest = i > 0 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.');
		if (!first && !rest) {
			return true; // A relative path that does not follow scheme syntax.
		}
	}
	return true;
}

// Normalize status codes to the public range before passing them to the transport.
int safe_status(int64_t p_status) {
	return p_status >= Limit::RESPONSE_STATUS_MIN && p_status <= Limit::RESPONSE_STATUS_MAX ? int(p_status) : Limit::RESPONSE_STATUS_FALLBACK;
}

// Decode URL segments once into the public path, matching key, and matching segments.
// Re-escape decoded segments for the key so encoded slashes cannot become segment separators.
bool path_values(const String &p_raw, String &r_path, String &r_key, PackedStringArray &r_parts, bool p_parts = true) {
	// Skip decoding and re-encoding ordinary ASCII paths on the exact-route fast path.
	bool plain = true;
	for (int i = 0; i < p_raw.length(); i++) {
		const char32_t c = p_raw[i];
		if (c != '/' && c != '.' && c != '-' && c != '~' && c != '_' && !is_ascii_alphanumeric_char(c)) {
			plain = false;
			break;
		}
	}
	if (plain) {
		r_path = p_raw;
		r_key = p_raw;
		r_parts = p_parts ? p_raw.split("/", true) : PackedStringArray();
		return true;
	}
	r_path = String();
	r_key = String();
	r_parts = PackedStringArray();
	const PackedStringArray raw = p_raw.split("/", true);
	for (int i = 0; i < raw.size(); i++) {
		String part;
		if (!Url::decode_part(raw[i], false, part)) {
			return false;
		}
		if (i > 0) {
			r_path += "/";
			r_key += "/";
		}
		r_path += part;
		r_key += part.uri_encode();
		r_parts.push_back(part); // Retain encoded-path segments to distinguish decoded slashes from original boundaries.
	}
	return true;
}

// Validate web-limit option names and explain invalid settings.
String limit_name_error(const Dictionary &p_opts) {
	static const PackedStringArray names = { "jobs", "job_timeout", "header_timeout", "body_timeout", "header_bytes", "header_values" };
	for (const Variant &raw : p_opts.keys()) {
		if (raw.get_type() != Variant::STRING && raw.get_type() != Variant::STRING_NAME) {
			return "web limit name must be a String";
		}
		const String name = raw;
		if (!names.has(name)) {
			return vformat("unknown web limit \"%s\"", name);
		}
	}
	return String();
}

} // namespace

// ---------------- Asynchronous request-body reads ----------------

// Enqueue body processing once in the runtime ready queue.
void GDWebBodyCall::schedule() {
	if (scheduled || self_hold.is_null()) {
		return;
	}
	scheduled = true;
	Async::post(self_hold, callable_mp(this, &GDWebBodyCall::step));
}

// Consume ready input in the current turn, or explicitly defer completion.
// Ordinary calls must try the shared state machine before entering the ready queue:
// posting already-ready work captures a VM stack and creates a job, context, deadline and
// signal connections only to undo them on the next turn. Inline completion also lets
// the application reuse the request once its handler returns. This is not a separate
// small-body parser: unavailable input, exhausted turns and conversion work retain
// the same continuation. Explicit async calls defer so listeners can attach first.
// The raw-completion probes in tests/net/encode cover both policies without auto-wait.
Variant GDWebBodyCall::start(const Ref<GDWebRequest> &p_req, const Ref<GDWebServer> &p_srv, int p_id, int p_mode, int64_t p_want, const String &p_path, bool p_deferred) {
	self_hold = Ref<GDWebBodyCall>(this);
	req = p_req;
	srv = p_srv;
	id = p_id;
	mode = p_mode;
	want = p_want;
	path = p_path;
	if (sink.is_valid()) {
		sink->set_ready_callback(callable_mp(this, &GDWebBodyCall::schedule));
	}
	if (p_deferred) {
		schedule();
	} else {
		starting = true;
		step();
		starting = false;
		if (ready.is_valid()) return ready;
	}
	return Signal(this, "finished");
}

// Deliver a result exactly once and release retained resources.
void GDWebBodyCall::finish(const Ref<R> &p_value) {
	if (self_hold.is_null()) return;
	// Wait for an in-flight write before reporting its final partial count.
	if (mode == SAVE && sink.is_valid() && !sink->done() && !p_value->get_ok()) {
		if (failure.is_null()) failure = p_value->get_e();
		flushing = true;
		sink->abort();
		return;
	}
	Ref<R> p_result = p_value;
	if (p_result.is_valid() && !p_result->get_ok() && p_result->get_v().get_type() == Variant::NIL) {
		if (mode == SAVE && sink.is_valid()) p_result = R::err(p_result->get_e(), Err::NONE, sink->count());
		else if (mode == READ || mode == BYTES) p_result = R::err(p_result->get_e(), Err::NONE, data);
	}

	if (self_hold.is_null()) {
		return;
	}
	Ref<GDWebBodyCall> keep(this);
	self_hold.unref(); // Mark completion before callbacks can cancel the same operation again.
	scheduled = false;
	if (srv.is_valid()) {
		srv->clear_body_wait(id);
	}
	if (req.is_valid()) {
		req->body_busy = false;
		if (req->body_active.ptr() == this) {
			req->body_active.unref();
		}
	}
	// Inline completion has no signal listener yet; return its value without suspending
	// the caller. Deferred completion must still notify exactly once on the main loop.
	if (starting) {
		ready = p_result;
	} else {
		emit_signal("finished", p_result);
	}
	if (sink.is_valid()) {
		sink->set_ready_callback(Callable());
		sink->abort(); // Stop writing without deleting a caller-selected partial destination.
		sink.unref();
	}
	data.clear();
	req.unref();
	srv.unref();
}

// Consume ready body data within one shared turn, yielding only for time or downstream waits.
// Reuse the caller's deadline rather than granting every body operation a fresh turn.
// A scheduling quantum limits uninterrupted work, never accepted body size.
void GDWebBodyCall::step() {
	scheduled = false;
	if (self_hold.is_null() || converting) return;
	Ref<GDWebBodyCall> keep(this);
	// Partial reads complete once; file writes retain their sink-driven pacing.
	if (mode == READ || mode == SAVE) {
		if (advance()) schedule();
		return;
	}
	const bool sliced = GDScriptFunction::begin_time_slice();
	const uint64_t until = GDScriptFunction::native_time_slice_deadline();
	bool more;
	do {
		more = advance();
	} while (more && GDClock::usec() < until);
	if (more) schedule();
	if (sliced) GDScriptFunction::end_time_slice();
}

// Advance one body chunk and distinguish runnable work from completion or backpressure.
bool GDWebBodyCall::advance() {
	if (self_hold.is_null() || converting) {
		return false;
	}
	// Reception is complete; wait for the worker to finish writing.
	if (flushing) {
		if (!sink->done()) {
			return false;
		}
		const Ref<Err> why = failure.is_valid() ? failure : sink->error();
		const int64_t written = sink->count();
		sink.unref();
		finish(why.is_null() ? R::ok(written) : R::err(why, Err::NONE, written));
		return false;
	}
	// Do not read another chunk until the writer catches up.
	if (mode == SAVE && sink.is_valid() && sink->pending_bytes() > 0) {
		return false;
	}
	if (srv.is_null() || !srv->has_conn(id)) {
		finish(R::err("request body is no longer available", Err::INTERRUPTED));
		return false;
	}
	PackedByteArray part;
	const int state = srv->read_body(id, mode == READ ? want : COPY_BUFFER, part);
	if (state == GDWebServer::BODY_READ_WAIT) {
		if (!srv->wait_body(id, callable_mp(this, &GDWebBodyCall::schedule))) {
			finish(R::err("request body is no longer available", Err::INTERRUPTED));
		}
		return false;
	}
	srv->clear_body_wait(id);
	if (state == GDWebServer::BODY_READ_LIMIT) {
		finish(R::err("http: request body too large", Err::LIMITED));
		return false;
	}
	if (state == GDWebServer::BODY_READ_BAD) {
		finish(R::err("request body ended before its boundary", Err::INVALID_DATA));
		return false;
	}
	if (!part.is_empty()) {
		total += part.size();
		if (mode == SAVE) {
			if (sink.is_null() || sink->error().is_valid()) {
				finish(sink.is_valid() ? R::err(sink->error(), Err::NONE, sink->count()) : R::err("body writer unavailable", Err::INTERRUPTED, total));
				return false;
			}
			sink->push(part);
		} else {
			const int64_t at = data.size();
			if (at > INT64_MAX - part.size()) {
				finish(R::err("request body is too large for an in-memory value", Err::LIMITED));
				return false;
			}
			if (at == 0) {
				data = part;
			} else {
				if (data.resize_uninitialized(at + part.size()) != OK) {
					finish(R::err("cannot allocate request body", Err::LIMITED));
					return false;
				}
				memcpy(data.ptrw() + at, part.ptr(), part.size());
			}
		}
	}
	if (mode == READ && (!part.is_empty() || state == GDWebServer::BODY_READ_EOF)) {
		finish(R::ok(data));
		return false;
	}
	if (state != GDWebServer::BODY_READ_EOF) return true;
	if (mode == SAVE) {
		sink->close();
		flushing = true;
		return true; // Closure may already have completed before this turn.
	} else {
		if (mode == BYTES) {
			finish(R::ok(data));
			return false;
		}
		// Decode ready JSON locally while retaining the shared turn and resumable worker fallback.
		if (mode == JSON) {
			const Variant result = JsonData::decode_reply(data, GDScriptFunction::native_time_slice_deadline());
			if (result.get_type() == Variant::SIGNAL) {
				converting = true;
				Signal(result).connect(callable_mp(this, &GDWebBodyCall::converted), Object::CONNECT_ONE_SHOT);
			} else {
				converted(result);
			}
			return false;
		}
		// Move whole-body native conversions off the serve loop because they cannot suspend midway.
		const PackedByteArray body = data;
		const Dictionary checked_rule = rule;
		converting = true;
		Signal converted_signal = GDValueCall::start([body, checked_rule, mode = mode]() -> Variant {
			if (mode == TEXT) {
				if (body.size() >= INT_MAX) return R::err("request body exceeds text conversion size", Err::LIMITED);
				return R::ok(String::utf8((const char *)body.ptr(), body.size()));
			}
			const Ref<R> decoded = json_of(body);
			if (!decoded->get_ok()) {
				return decoded;
			}
			return GDWebValid::check(decoded->get_v(), checked_rule);
		});
		converted_signal.connect(callable_mp(this, &GDWebBodyCall::converted), Object::CONNECT_ONE_SHOT);
	}
	return false;
}

// Return worker-converted body data to main-thread request state.
void GDWebBodyCall::converted(const Variant &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	converting = false;
	const Ref<R> result = p_result;
	if (result.is_null()) {
		finish(R::err("request body conversion failed", Err::INVALID_DATA));
		return;
	}
	if (mode == VALIDATE && result->get_ok() && req.is_valid()) {
		req->keep("valid:" + keep_name, result->get_v());
		finish(R::ok());
		return;
	}
	finish(result);
}

// Cancel a body operation whose result is no longer awaited.
void GDWebBodyCall::cancel() {
	finish(R::err("request body read canceled", Err::INTERRUPTED));
}

// Register the body-operation completion signal.
void GDWebBodyCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDWebBodyCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}

// ---------------- Requests ----------------

String GDWebRequest::get_method() const {
	return srv->get_method(id);
}

// Return the current peer IP.
String GDWebRequest::get_ip() const {
	return srv->get_ip(id);
}

// Decode once and expose the first value of each valid query parameter.
Dictionary GDWebRequest::get_query() {
	if (!query_done) {
		query_done = true;
		query_txt = srv->get_query(id);
		if (query_txt.is_empty()) {
			if (!query_map.is_empty()) query_map = Dictionary();
			if (!query_values.is_empty()) query_values = Dictionary();
			return query_map;
		}
		query_map = Dictionary();
		query_values = Url::decode_query(query_txt)->get_v();
		for (const Variant &key : query_values.keys()) {
			const Array values = query_values[key];
			if (!values.is_empty()) query_map[key] = values[0];
		}
	}
	return query_map;
}

// Return the complete parsed query without flattening duplicate values.
Dictionary GDWebRequest::get_query_all() {
	get_query();
	return query_values;
}

// Return the current request target.
String GDWebRequest::get_target() {
	get_query(); // Retrieve the original query text here.
	return query_txt.is_empty() ? raw_path_txt : raw_path_txt + "?" + query_txt;
}

// Initialize a request object for reuse.
void GDWebRequest::reset(int p_id, const String &p_path, const Ref<GDAsyncContext> &p_parent) {
	finish_context("request reused");
	ctx.unref();
	ctx_reason.unref();
	ctx_parent = p_parent;
	ctx_end = nullptr;
	id = p_id;
	raw_path_txt = p_path;
	const bool valid_path = path_values(p_path, path_txt, route_txt, path_parts, false);
	ERR_FAIL_COND_MSG(!valid_path, "validated HTTP path could not be decoded");
	query_txt = String();
	query_done = false;
	// Allocate dictionaries only for requests that use them.
	// Reuse empty dictionaries and replace only populated ones.
	// Leave query_map to get_query, which always refreshes it.
	if (!params_map.is_empty()) {
		params_map = Dictionary();
	}
	if (!store.is_empty()) {
		store = Dictionary();
	}
	body_busy = false;
}

// Create cancellation monitoring on first use, including observation after completion.
Ref<GDAsyncContext> GDWebRequest::get_context() {
	if (ctx.is_null()) {
		ctx = ctx_parent.is_valid() ? ctx_parent->with_cancel() : Async::ctx();
		ctx_parent.unref();
		if (ctx_end) {
			ctx->cancel(ctx_reason.is_valid() ? ctx_reason->get_msg() : String(ctx_end), ctx_reason.is_valid() ? ctx_reason->get_kind() : Err::INTERRUPTED);
		}
	}
	return ctx;
}

// Propagate completion to observed contexts and retain the first reason for late observers.
void GDWebRequest::finish_context(const char *p_reason) {
	if (ctx_end) {
		return;
	}
	// Preserve an earlier parent cancellation before releasing its unobserved context.
	if (ctx.is_null() && ctx_parent.is_valid() && ctx_parent->is_done()) {
		get_context();
	}
	ctx_end = p_reason;
	ctx_parent.unref();
	if (body_active.is_valid()) {
		body_active->cancel();
	}
	if (ctx.is_valid() && !ctx->is_done()) {
		ctx->cancel(p_reason, Err::INTERRUPTED);
	}
	if (ctx.is_valid()) {
		ctx_reason = ctx->get_reason();
		ctx.unref(); // Break request/context/callback ownership cycles after notification.
	}
}

// Store a request-local value for later stages.
void GDWebRequest::keep(const String &p_name, const Variant &p_value) {
	store[p_name] = p_value;
}

// Return a request-local stored value.
Variant GDWebRequest::kept(const String &p_name, const Variant &p_fallback) const {
	return store.get(p_name, p_fallback);
}

// Return a validated request value.
Variant GDWebRequest::valid(const String &p_name, const Variant &p_fallback) const {
	return store.get("valid:" + p_name, p_fallback);
}

// Return the named HTTP header.
String GDWebRequest::header(const String &p_name) const {
	return srv->get_header(id, p_name);
}

// Return HTTP headers as a dictionary.
Dictionary GDWebRequest::headers() const {
	return srv->get_headers(id);
}

// Start one body operation, preventing concurrent access to the same stream.
Variant GDWebRequest::body_call(int p_mode, int64_t p_want, bool p_deferred) {
	if (body_busy || srv.is_null() || !srv->has_conn(id)) {
		const Ref<R> result = R::err(body_busy ? "request body is already being read" : "request body is no longer available", body_busy ? Err::ALREADY_EXISTS : Err::INTERRUPTED);
		return p_deferred ? Variant(Async::ready(result)) : Variant(result);
	}
	Ref<GDWebBodyCall> call;
	call.instantiate();
	body_busy = true;
	body_active = call;
	return call->start(Ref<GDWebRequest>(this), srv, id, p_mode, p_want, String(), p_deferred);
}

// Read one body chunk from the stream.
Variant GDWebRequest::read(int64_t p_bytes) {
	if (p_bytes < 1) {
		return R::err("request body read size must be positive", Err::INVALID_DATA);
	}
	return body_call(GDWebBodyCall::READ, p_bytes);
}

// Read all remaining body bytes.
Variant GDWebRequest::bytes() {
	return body_call(GDWebBodyCall::BYTES);
}

// Defer a partial read so listeners can attach before completion.
Signal GDWebRequest::read_async(int64_t p_bytes) {
	if (p_bytes < 1) return Async::ready(R::err("request body read size must be positive", Err::INVALID_DATA));
	return body_call(GDWebBodyCall::READ, p_bytes, true);
}

// Defer collecting the remaining body bytes.
Signal GDWebRequest::bytes_async() {
	return body_call(GDWebBodyCall::BYTES, 0, true);
}

// Return the body length without reading a large body into memory.
int64_t GDWebRequest::body_size() const {
	return srv->get_body_size(id);
}

// Set this request's body-reader limit.
Ref<R> GDWebRequest::limit(int64_t p_bytes) {
	if (p_bytes < 0) return R::err("request body limit must be zero or greater", Err::INVALID_DATA);
	if (body_busy) return R::err("request body limit must be set before reading", Err::INVALID_DATA);
	if (srv.is_null() || !srv->set_request_body_limit(id, p_bytes)) return R::err("request body is no longer available", Err::INTERRUPTED);
	return R::ok();
}

// Save the body incrementally inside a mount.
Signal GDWebRequest::save(const String &p_path) {
	GD_PERM_FAIL_V(WRITE, p_path, Async::ready(R::err(vformat("cannot write %s", p_path), Err::PERMISSION_DENIED)));
	String why;
	const String real = Mount::resolve(p_path, true, why);
	if (real.is_empty()) {
		return Async::ready(R::err(why.is_empty() ? vformat("cannot write %s", p_path) : why, Err::PERMISSION_DENIED));
	}
	if (body_busy || srv.is_null() || !srv->has_conn(id)) {
		return Async::ready(R::err(body_busy ? "request body is already being read" : "request body is no longer available", body_busy ? Err::ALREADY_EXISTS : Err::INTERRUPTED));
	}
	Ref<GDWebBodyCall> call;
	call.instantiate();
	call->sink.instantiate();
	// Delegate file open and writes to workers, keeping file I/O off the handler event loop.
	call->sink->open(p_path, false);
	body_busy = true;
	body_active = call;
	return call->start(Ref<GDWebRequest>(this), srv, id, GDWebBodyCall::SAVE, 0, p_path);
}

// Decode the remaining body as UTF-8 text.
Variant GDWebRequest::text() {
	return body_call(GDWebBodyCall::TEXT);
}

// Decode the remaining body as JSON.
Variant GDWebRequest::json() {
	return body_call(GDWebBodyCall::JSON);
}

// Defer decoding the remaining body as text.
Signal GDWebRequest::text_async() {
	return body_call(GDWebBodyCall::TEXT, 0, true);
}

// Defer decoding the remaining body as JSON.
Signal GDWebRequest::json_async() {
	return body_call(GDWebBodyCall::JSON, 0, true);
}

// Read JSON and apply middleware validation rules.
Signal GDWebRequest::json_valid(const Dictionary &p_rule, const String &p_name) {
	if (body_busy || srv.is_null() || !srv->has_conn(id)) {
		return Async::ready(R::err(body_busy ? "request body is already being read" : "request body is no longer available", body_busy ? Err::ALREADY_EXISTS : Err::INTERRUPTED));
	}
	Ref<GDWebBodyCall> call;
	call.instantiate();
	call->rule = p_rule;
	call->keep_name = p_name;
	body_busy = true;
	body_active = call;
	return call->start(Ref<GDWebRequest>(this), srv, id, GDWebBodyCall::VALIDATE, 0, String());
}

// Register public script methods and properties.
void GDWebRequest::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_method"), &GDWebRequest::get_method);
	ClassDB::bind_method(D_METHOD("get_path"), &GDWebRequest::get_path);
	ClassDB::bind_method(D_METHOD("get_ip"), &GDWebRequest::get_ip);
	ClassDB::bind_method(D_METHOD("get_query"), &GDWebRequest::get_query);
	ClassDB::bind_method(D_METHOD("get_query_all"), &GDWebRequest::get_query_all);
	ClassDB::bind_method(D_METHOD("get_params"), &GDWebRequest::get_params);
	ClassDB::bind_method(D_METHOD("get_target"), &GDWebRequest::get_target);
	ClassDB::bind_method(D_METHOD("get_context"), &GDWebRequest::get_context);
	ClassDB::bind_method(D_METHOD("header", "name"), &GDWebRequest::header);
	ClassDB::bind_method(D_METHOD("headers"), &GDWebRequest::headers);
	ClassDB::bind_method(D_METHOD("read", "bytes"), &GDWebRequest::read, DEFVAL(32768));
	ClassDB::bind_method(D_METHOD("bytes"), &GDWebRequest::bytes);
	ClassDB::bind_method(D_METHOD("body_size"), &GDWebRequest::body_size);
	ClassDB::bind_method(D_METHOD("limit", "bytes"), &GDWebRequest::limit);
	ADD_RESULT("limit", "Variant");
	ClassDB::bind_method(D_METHOD("save", "path"), &GDWebRequest::save);
	ClassDB::bind_method(D_METHOD("text"), &GDWebRequest::text);
	ClassDB::bind_method(D_METHOD("json"), &GDWebRequest::json);
	ClassDB::bind_method(D_METHOD("read_async", "bytes"), &GDWebRequest::read_async, DEFVAL(32768));
	ClassDB::bind_method(D_METHOD("bytes_async"), &GDWebRequest::bytes_async);
	ClassDB::bind_method(D_METHOD("save_async", "path"), &GDWebRequest::save);
	ClassDB::bind_method(D_METHOD("text_async"), &GDWebRequest::text_async);
	ClassDB::bind_method(D_METHOD("json_async"), &GDWebRequest::json_async);
	ADD_AWAIT("read", "R:PackedByteArray");
	ADD_AWAIT("bytes", "R:PackedByteArray");
	ADD_AWAIT("save", "R:int");
	ADD_AWAIT("text", "R:String");
	ADD_AWAIT("json", "R:Variant");
	ADD_AWAIT("read_async", "R:PackedByteArray");
	ADD_AWAIT("bytes_async", "R:PackedByteArray");
	ADD_AWAIT("save_async", "R:int");
	ADD_AWAIT("text_async", "R:String");
	ADD_AWAIT("json_async", "R:Variant");
	ADD_AUTO_WAIT("read");
	ADD_AUTO_WAIT("bytes");
	ADD_AUTO_WAIT("save");
	ADD_AUTO_WAIT("text");
	ADD_AUTO_WAIT("json");
	ClassDB::bind_method(D_METHOD("keep", "name", "value"), &GDWebRequest::keep);
	ClassDB::bind_method(D_METHOD("kept", "name", "fallback"), &GDWebRequest::kept, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("valid", "name", "fallback"), &GDWebRequest::valid, DEFVAL(Variant()));
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "context", PROPERTY_HINT_RESOURCE_TYPE, "GDAsyncContext"), "", "get_context");

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "method"), "", "get_method");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "path"), "", "get_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "ip"), "", "get_ip");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "query"), "", "get_query");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "query_all"), "", "get_query_all");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "params"), "", "get_params");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "target"), "", "get_target");
}

// ---------------- Template responses ----------------

// Start template processing and return a signal carrying the completed reply.
Signal GDWebViewCall::start(const String &p_path, const Dictionary &p_data, int64_t p_status, const Callable &p_renderer) {
	Ref<GDWebViewCall> call;
	call.instantiate();
	call->self_hold = call;
	call->path = p_path;
	call->data = p_data;
	call->status = p_status;
	call->renderer = p_renderer;
	const Signal signal(call.ptr(), "finished");
	GDFileCall::start([p_path]() { return Os::read_bytes(p_path); }).connect(callable_mp(call.ptr(), &GDWebViewCall::loaded), Object::CONNECT_ONE_SHOT);
	return signal;
}

// Pass a loaded template to the default CPU renderer or a custom renderer.
void GDWebViewCall::loaded(const Variant &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	const Ref<R> result = p_result;
	if (result.is_null() || !result->get_ok()) {
		finish(result.is_valid() ? result : R::err("invalid template read result"));
		return;
	}
	const PackedByteArray bytes = result->get_v();
	if (!renderer.is_valid()) {
		if (!build) {
			build = std::make_shared<HtmlBuild>();
		}
		GDValueCall::start([build = build, bytes, data = data, status = status]() -> Variant {
			Error err = build->step(String::utf8((const char *)bytes.ptr(), bytes.size()));
			if (err == ERR_BUSY) {
				return build->needed();
			}
			String body;
			if (err == OK) err = build->render(data, body);
			if (err != OK) return build->failure(err);
			return R::ok(Http::html(body, status));
		}).connect(callable_mp(this, &GDWebViewCall::prepared), Object::CONNECT_ONE_SHOT);
		return;
	}
	GDValueCall::start([bytes]() -> Variant { return String::utf8((const char *)bytes.ptr(), bytes.size()); }).connect(callable_mp(this, &GDWebViewCall::custom_ready), Object::CONNECT_ONE_SHOT);
}

// Queue only missing partials for I/O, retaining paused parsing state for CPU work.
void GDWebViewCall::prepared(const Variant &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	if (p_result.get_type() != Variant::STRING) {
		const Ref<R> failed = p_result;
		finish(failed.is_valid() ? failed : R::err("invalid template build result"));
		return;
	}
	part = p_result;
	const String root = path.get_base_dir();
	const String name = part;
	GDFileCall::start([root, name]() -> Ref<R> {
		if (!Html::partial_ok(name)) {
			return R::err("invalid partial name", Err::INVALID_DATA);
		}
		const String file = Path::under(root.path_join("partials"), name + ".html");
		return file.is_empty() ? R::err("invalid partial path", Err::INVALID_DATA) : Os::read_bytes(file);
	}).connect(callable_mp(this, &GDWebViewCall::loaded), Object::CONNECT_ONE_SHOT);
}

// After source conversion, run only custom rendering on the script scheduler.
void GDWebViewCall::custom_ready(const Variant &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDWebViewCall> keep(this);
	if (p_result.get_type() != Variant::STRING) {
		finish(Ref<R>(p_result).is_valid() ? Ref<R>(p_result) : R::err("invalid template source result"));
		return;
	}
	Variant args[3] = { p_result, data, path };
	const Variant *argv[3] = { &args[0], &args[1], &args[2] };
	Variant out;
	Callable::CallError err;
	const bool sliced = GDScriptFunction::begin_time_slice();
	const Callable render = renderer;
	{
		GDScriptFunction::SuspendableCall suspendable;
		render.callp(argv, 3, out, err);
	}
	if (sliced) {
		GDScriptFunction::end_time_slice();
	}
	// Do not retain returned await state after synchronous cancellation inside the renderer.
	if (self_hold.is_null()) {
		GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(out.get_validated_object());
		if (state) {
			state->_clear_connections();
		}
		return;
	}
	if (err.error != Callable::CallError::CALL_OK) {
		finish(R::err("template renderer failed", Err::INVALID_DATA));
		return;
	}
	GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(out.get_validated_object());
	if (state) {
		continuation = Ref<RefCounted>(state);
		state->connect("completed", callable_mp(this, &GDWebViewCall::rendered), Object::CONNECT_ONE_SHOT);
		return;
	}
	rendered(out);
}

// Convert rendered text into an HTML reply.
void GDWebViewCall::rendered(const Variant &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	continuation.unref();
	Variant value = p_result;
	const Ref<R> result = value;
	if (result.is_valid()) {
		if (!result->get_ok()) { finish(result); return; }
		value = result->get_v();
	}
	const Ref<Err> why = value;
	if (why.is_valid()) { finish(R::err(why)); return; }
	if (value.get_type() != Variant::STRING) {
		finish(R::err("template renderer must return text", Err::INVALID_DATA));
		return;
	}
	finish(R::ok(Http::html(value, status)));
}

// Return the template reply exactly once and release retained references.
void GDWebViewCall::finish(const Ref<R> &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDWebViewCall> keep(this);
	renderer = Callable();
	data = Dictionary();
	build.reset();
	self_hold.unref();
	emit_signal("finished", p_result);
}

// Stop a custom renderer continuation when its request disappears.
void GDWebViewCall::cancel() {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDWebViewCall> keep(this);
	GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(continuation.ptr());
	if (state) {
		state->_clear_connections();
	}
	continuation.unref();
	finish(R::err("template rendering canceled", Err::INTERRUPTED));
}

// Register completion and cancellation.
void GDWebViewCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDWebViewCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "reply", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)));
}

// ---------------- Routed HTTP applications ----------------

// Preserve the first configuration failure until the caller handles listen's result.
bool GDWebApp::invalid(bool p_bad, const String &p_message) const {
	if (p_bad && config_error.is_null()) config_error = Err::make(p_message, Err::INVALID_DATA);
	return p_bad;
}

// Normalize Callables and objects exposing handle(req) to one invocation shape.
GDWebApp::Mid GDWebApp::mid_of(const Variant &p_mid) const {
	Mid mid;
	if (p_mid.get_type() == Variant::CALLABLE) {
		mid.fn = p_mid;
		return mid;
	}
	if (p_mid.get_type() == Variant::OBJECT) {
		Object *obj = p_mid;
		if (obj && obj->has_method("handle")) {
			mid.hold = p_mid;
			mid.fn = Callable(obj, "handle");
		}
	}
	return mid;
}

// Normalize route middleware, returning empty if any entry is invalid.
LocalVector<GDWebApp::Mid> GDWebApp::mids_of(const Array &p_mids) const {
	LocalVector<Mid> out;
	for (const Variant &item : p_mids) {
		Mid mid = mid_of(item);
		if (invalid(!mid.fn.is_valid(), "middleware must be Callable or have handle(req)")) return LocalVector<Mid>();
		out.push_back(mid);
	}
	return out;
}

// Validate and register a route and handler.
void GDWebApp::add_route(const String &p_method, const String &p_pattern, const Callable &p_handler, const Array &p_mids, int p_group) {
	if (invalid(!p_handler.is_valid(), "route handler is invalid")) return;
	LocalVector<Mid> mids = mids_of(p_mids);
	if (invalid(!p_mids.is_empty() && mids.is_empty(), "route was not registered because middleware is invalid")) return;
	route_mids.push_back(std::move(mids));
	const int mid_id = (int)route_mids.size() - 1;
	String decoded;
	String key;
	PackedStringArray parts;
	if (invalid(!path_values(p_pattern, decoded, key, parts), "route pattern contains an invalid URL path")) return;
	if (!p_pattern.contains(":")) {
		Slot slot;
		slot.method = p_method.to_upper();
		slot.handler = p_handler;
		slot.mids = mid_id;
		slot.group = p_group;
		exact[key].push_back(slot);
		return;
	}
	Route r;
	r.method = p_method.to_upper();
	r.parts = parts;
	r.handler = p_handler;
	r.mids = mid_id;
	r.group = p_group;
	routes.push_back(r);
}

// Bind an HTTP method and path to a handler.
void GDWebApp::route(const String &p_method, const String &p_pattern, const Callable &p_handler, const Array &p_mids) {
	add_route(p_method, p_pattern, p_handler, p_mids, -1);
}

// Register middleware before route selection.
void GDWebApp::pre(const Variant &p_mid) {
	Mid mid = mid_of(p_mid);
	if (invalid(!mid.fn.is_valid(), "middleware must be Callable or have handle(req)")) return;
	pres.push_back(mid);
}

// Register middleware for every route.
void GDWebApp::use(const Variant &p_mid) {
	Mid mid = mid_of(p_mid);
	if (invalid(!mid.fn.is_valid(), "middleware must be Callable or have handle(req)")) return;
	uses.push_back(mid);
}

// Register response postprocessing.
void GDWebApp::after(const Callable &p_after) {
	if (invalid(!p_after.is_valid(), "postprocessor is invalid")) return;
	afters.push_back(p_after);
}

// Register the error handler.
void GDWebApp::on_error(const Callable &p_handler) {
	on_fail = p_handler;
}

// Hide failure details by default because they may contain internal statements or destinations.
// Enable detailed response bodies explicitly for local development.
void GDWebApp::show_errors(bool p_on) {
	tell_why = p_on;
}

// Set an explicit body-reader limit for all requests.
void GDWebApp::body_limit(int64_t p_bytes) {
	if (invalid(p_bytes < 0, "body limit must be zero or greater")) return;
	max_body = p_bytes;
}

// Configure pending-input counts and deadlines before listening.
void GDWebApp::limits(const Dictionary &p_opts) {
	const String name_error = limit_name_error(p_opts);
	if (invalid(!name_error.is_empty(), name_error)) return;
	const int64_t jobs_raw = p_opts.get("jobs", job_max);
	const double job_raw = p_opts.get("job_timeout", (double)job_ms / 1000.0);
	const double head_raw = p_opts.get("header_timeout", head_seconds);
	const double body_raw = p_opts.get("body_timeout", body_seconds);
	const int64_t header_bytes_raw = p_opts.get("header_bytes", header_bytes);
	const int64_t header_values_raw = p_opts.get("header_values", header_values);
	uint64_t parsed_job = 0;
	uint64_t parsed_head = 0;
	uint64_t parsed_body = 0;
	if (invalid(jobs_raw < 0 || jobs_raw > INT_MAX, "jobs must be between 0 and 2147483647")) return;
	if (invalid(!Limit::seconds_ms(job_raw, parsed_job), "job timeout must be zero or a positive number of seconds")) return;
	if (invalid(!Limit::seconds_ms(head_raw, parsed_head), "header timeout must be zero or a positive number of seconds")) return;
	if (invalid(!Limit::seconds_ms(body_raw, parsed_body), "body timeout must be zero or a positive number of seconds")) return;
	if (invalid(header_bytes_raw < 1 || header_bytes_raw > HTTP_HEADER_LIMIT_MAX, "header limit must be between 1 and 2147479551")) return;
	if (invalid(header_values_raw < 1 || header_values_raw > INT_MAX, "header values must be between 1 and 2147483647")) return;
	job_max = (int)jobs_raw;
	job_ms = parsed_job;
	job_times.clear();
	for (KeyValue<int, Job> &kv : jobs) {
		kv.value.due = job_ms == 0 ? 0 : (kv.value.made > UINT64_MAX - job_ms ? UINT64_MAX : kv.value.made + job_ms);
		if (kv.value.due > 0) {
			job_times.insert(JobTime{ kv.value.due, kv.key });
		}
	}
	arm_jobs();
	head_seconds = head_raw;
	body_seconds = body_raw;
	header_bytes = (int)header_bytes_raw;
	header_values = (int)header_values_raw;
	if (srv.is_valid()) {
		srv->set_header_limits(header_bytes, header_values);
		srv->set_header_timeout(head_seconds);
		srv->set_body_timeout(body_seconds);
	}
}

// Return the count of unsafe response headers discarded.
uint64_t GDWebApp::dropped_headers() const {
	return srv.is_valid() ? srv->dropped_headers() : 0;
}

// Create routes sharing a prefix and middleware.
Ref<GDWebRouteGroup> GDWebApp::group(const String &p_prefix, const Array &p_mids) {
	Band band;
	band.prefix = p_prefix.trim_suffix("/");
	band.mids = mids_of(p_mids);
	if (invalid(!p_mids.is_empty() && band.mids.is_empty(), "route group middleware is invalid")) return Ref<GDWebRouteGroup>();
	bands.push_back(band);

	Ref<GDWebRouteGroup> g;
	g.instantiate();
	g->app = Ref<GDWebApp>(this);
	g->id = (int)bands.size() - 1;
	return g;
}

// Map a URL prefix to a static-file directory.
void GDWebApp::static_dir(const String &p_prefix, const String &p_dir) {
	String decoded;
	String key;
	PackedStringArray parts;
	if (invalid(!path_values(p_prefix.trim_suffix("/"), decoded, key, parts), "static prefix contains an invalid URL path")) return;
	statics.push_back(Pair<String, String>(key, p_dir));
}

// Register the unmatched-route handler.
void GDWebApp::otherwise(const Callable &p_handler) {
	fallback = p_handler;
}

// Start listening at the selected address.
Ref<R> GDWebApp::listen(int64_t p_port, const String &p_host) {
	if (opening.is_valid() || srv.is_valid()) return R::err("server already started", Err::ALREADY_EXISTS);
	return listen_at(p_port, p_host, Ref<GDTLSIdentity>());
}

// Apply identical HTTP limits and scheduling to plain and encrypted listeners.
Ref<R> GDWebApp::listen_at(int64_t p_port, const String &p_host, const Ref<GDTLSIdentity> &p_identity) {
	if (config_error.is_valid()) return R::err(config_error);
	if (shutting) {
		return R::err("server is shutting down", Err::ALREADY_EXISTS);
	}
	srv.instantiate();
	srv->set_identity(p_identity);
	srv->set_body_limit(max_body);
	srv->set_header_limits(header_bytes, header_values);
	srv->set_header_timeout(head_seconds);
	srv->set_body_timeout(body_seconds);
	const Ref<R> opened = srv->listen(p_port, p_host);
	if (!opened->get_ok()) {
		srv.unref();
		return opened;
	}
	root_ctx = Async::ctx();
	// Dispatch only kernel-ready connections to the runtime serve task.
	srv->set_ready_callback(callable_mp(this, &GDWebApp::poll));
	return R::ok();
}

// Return the application's actual listen port.
int GDWebApp::port() const {
	return srv.is_valid() ? srv->get_port() : 0;
}

// Stop new accepts and keepalive reuse while retaining active requests.
Signal GDWebApp::shutdown(const Ref<GDAsyncContext> &p_ctx) {
	if (p_ctx.is_null()) {
		return Async::ready(R::err("shutdown needs a context", Err::INVALID_DATA));
	}
	if (shutdown_wait.is_valid()) {
		return Async::ready(R::err("shutdown is already waiting", Err::ALREADY_EXISTS));
	}
	if (srv.is_null()) {
		opening.unref();
		return Async::ready(R::ok());
	}
	shutting = true;
	shutdown_ctx = p_ctx;
	shutdown_wait.instantiate();
	shutdown_wait->self_hold = shutdown_wait;
	p_ctx->connect("canceled", callable_mp(this, &GDWebApp::poll), Object::CONNECT_ONE_SHOT);
	srv->begin_shutdown();
	post_poll(); // With only idle connections, finish immediately on the next runtime task.
	return Signal(shutdown_wait.ptr(), "finished");
}

// Deliver one graceful-shutdown result and release its deadline.
void GDWebApp::finish_shutdown(const Ref<R> &p_result) {
	if (shutdown_wait.is_valid()) {
		Ref<GDWait> wait = shutdown_wait;
		shutdown_wait.unref();
		shutdown_ctx.unref();
		wait->done(p_result);
	}
}

// Succeed when no active requests remain, or return the earlier deadline failure.
void GDWebApp::check_shutdown() {
	if (!shutting || srv.is_null()) {
		return;
	}
	if (srv->connection_count() == 0) {
		shutting = false;
		if (root_ctx.is_valid() && !root_ctx->is_done()) {
			root_ctx->cancel("server shutdown", Err::INTERRUPTED);
		}
		root_ctx.unref();
		srv->set_ready_callback(Callable());
		srv.unref();
		spare.unref();
		finish_shutdown(R::ok());
		return;
	}
	if (shutdown_wait.is_valid() && shutdown_ctx.is_valid() && shutdown_ctx->is_done()) {
		finish_shutdown(R::err(shutdown_ctx->get_reason()));
	}
}

// Honor deferred stopping at a work boundary.
void GDWebApp::leave() {
	busy--;
	if (busy == 0 && stop_wanted && (stop_after < 0 || !jobs.has(stop_after))) {
		stop_wanted = false;
		stop_after = -1;
		stop();
	}
}

// Stop processing and listening, then release resources.
void GDWebApp::stop() {
	opening.unref();
	// Handlers and middleware may request stopping while application work is still active.
	// Defer destruction to a work boundary so continuations cannot access a released server.
	if (busy > 0) {
		stop_wanted = true;
		stop_after = current_id;
		return;
	}
	Async::drop_deadline(this, job_due);
	job_due = 0;
	// Detach every job before cancellation can reenter through another job's signal.
	busy++;
	shutting = true;
	HashMap<int, Job> stopped = std::move(jobs);
	awaiting = 0;
	job_times.clear();
	for (KeyValue<int, Job> &entry : stopped) {
		cancel_job(entry.value);
	}
	if (srv.is_valid()) {
		srv->set_ready_callback(Callable());
		srv->stop();
		srv.unref();
	}
	if (root_ctx.is_valid() && !root_ctx->is_done()) {
		root_ctx->cancel("server stopped", Err::INTERRUPTED);
	}
	root_ctx.unref();
	shutting = false;
	ready_ids.clear();
	ready_at = 0;
	poll_posted = false;
	stop_after = -1;
	stop_wanted = false;
	busy--;
	// Discard a reusable request's reference to the old server before restarting.
	spare.unref();
	finish_shutdown(R::err("server stopped", Err::INTERRUPTED));
}

// Report whether the application is listening.
bool GDWebApp::is_listening() const {
	return srv.is_valid() && srv->is_listening();
}

// Advance only ready I/O.
void GDWebApp::poll() {
	poll_posted = false;
	if (srv.is_null()) {
		return;
	}
	busy++; // Defer stopping while this work is active.
	trim_jobs(); // Release disconnected or expired work first.
	const bool sliced = GDScriptFunction::begin_time_slice();
	const bool read_first = ready_ids.is_empty() || !dispatch_first;
	dispatch_first = read_first;
	const auto read = [&]() {
		const PackedInt32Array ids = srv->poll();
		for (const int id : ids) ready_ids.push_back(id);
	};
	if (read_first) read();
	const uint64_t until = GDScriptFunction::native_time_slice_deadline();
	for (; ready_at < ready_ids.size(); ready_at++) {
		if (GDClock::usec() >= until) break;
		const int id = ready_ids[ready_at];
		HashMap<int, Job>::Iterator waiting = jobs.find(id);
		if (waiting) {
			// Notify active handlers of FIN without discarding a response that can still be written.
			if (waiting->value.stage != ENCODE && !srv->request_alive(id, waiting->value.req->get_context())) {
				Job dead = waiting->value;
				drop_job_time(id, dead);
				jobs.erase(id);
				cancel_job(dead);
				arm_jobs();
			}
			continue;
		}
		if (!srv->has_request(id)) continue;
		// Do not pass malformed requests to handlers.
			// Reject them without route selection or middleware because their framing is untrusted.
			const int bad = srv->bad_of(id);
			if (bad != 0) {
				srv->respond(id, bad, PackedByteArray(), TEXT_TYPE);
				continue;
			}
			Job job;
			// Reuse request objects to avoid allocating an Object for every response.
			if (spare.is_valid()) {
				job.req = spare;
				spare.unref();
			} else {
				job.req.instantiate();
				job.req->srv = srv;
			}
			job.req->reset(id, srv->get_path(id), root_ctx);
			current_id = id;
			run(id, job, Variant(), false, until);
			current_id = -1;
	}
	// Do not let continuously ready sockets consume every turn before retained handlers run.
	if (!read_first) {
		if (GDClock::usec() < until) read();
		else post_poll(); // Return to I/O even when the handler backlog was just exhausted.
	}
	gd_ready_compact(ready_ids, ready_at);
	if (!ready_ids.is_empty()) {
		post_poll();
	}
	if (sliced) GDScriptFunction::end_time_slice();
	leave();
	check_shutdown();
}

// Append remaining runnable requests to the runtime FIFO.
void GDWebApp::post_poll() {
	if (poll_posted) {
		return;
	}
	poll_posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWebApp::poll));
}

// Retain an incomplete asynchronous result until it can resume.
bool GDWebApp::park(const Variant &p_ret, int p_id, Job &p_job) {
	const bool signal_wait = p_ret.get_type() == Variant::SIGNAL;
	Object *obj = signal_wait ? nullptr : p_ret.get_type() == Variant::OBJECT ? p_ret.get_validated_object() : nullptr;
	if (!signal_wait && (!obj || !obj->is_class("GDScriptFunctionState"))) {
		return false;
	}
	if (p_job.stage != ENCODE && job_max > 0 && awaiting >= job_max && !jobs.has(p_id)) {
		// Reject additional suspension when the configured pending-work capacity is exhausted.
		if (signal_wait) {
			const Signal signal = p_ret;
			Object *owner = signal.get_object();
			if (owner && owner->has_method("cancel")) {
				owner->call("cancel");
			}
		} else {
			GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(obj);
			if (state) {
				state->_clear_connections();
			}
		}
		if (p_job.file.is_valid()) {
			p_job.file->abort();
			p_job.file.unref();
		}
		p_job.req->finish_context("request job limit reached");
		srv->respond(p_id, 503, String("service unavailable").to_utf8_buffer(), TEXT_TYPE);
		return true;
	}
	if (signal_wait) {
		p_job.wait_signal = p_ret;
		p_job.hold = Ref<RefCounted>(Object::cast_to<RefCounted>(p_job.wait_signal.get_object()));
		if (p_job.stage == ENCODE) utf8_watch(p_ret, callable_mp(srv.ptr(), &GDWebServer::has_request).bind(p_id));
	} else {
		p_job.hold = Ref<RefCounted>(Object::cast_to<RefCounted>(obj));
		p_job.wait_signal = Signal(obj, "completed");
	}
	p_job.wait_call = Callable(this, "_resume_signal").bind(p_id);
	if (p_job.made == 0) {
		p_job.made = GDClock::msec();
	}
	p_job.due = job_ms == 0 ? 0 : (p_job.made > UINT64_MAX - job_ms ? UINT64_MAX : p_job.made + job_ms);
	if (p_job.stage != ENCODE && !jobs.has(p_id)) {
		awaiting++;
	}
	jobs.insert(p_id, p_job);
	if (p_job.due > 0) {
		job_times.insert(JobTime{ p_job.due, p_id });
	}
	arm_jobs();
	Object *owner = p_job.wait_signal.get_object();
	if (!owner || !owner->has_signal(p_job.wait_signal.get_name()) || p_job.wait_signal.connect(p_job.wait_call, Object::CONNECT_ONE_SHOT) != OK) {
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWebApp::resumed).bind(R::err("handler returned an unavailable signal"), p_id));
		return true;
	}
	// Cancel context-aware waits after FIN while keeping ordinary response work writable.
	if (p_job.stage != ENCODE && !srv->request_alive(p_id, p_job.req->get_context())) {
		Job dead = jobs[p_id];
		drop_job_time(p_id, dead);
		jobs.erase(p_id);
		cancel_job(dead);
		arm_jobs();
	}
	return true;
}

// Disconnect the awaited signal's retained state so its suspended stack can be released.
void GDWebApp::cancel_job(Job &p_job) {
	// Detach before cancellation can synchronously emit or leave a persistent signal behind.
	if (p_job.wait_signal.get_object() && p_job.wait_signal.is_connected(p_job.wait_call)) {
		p_job.wait_signal.disconnect(p_job.wait_call);
	}
	p_job.wait_call = Callable();
	GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(p_job.hold.ptr());
	if (state) {
		state->_clear_connections();
	}
	p_job.req->finish_context("request canceled");
	if (!p_job.wait_signal.is_null()) {
		Object *owner = p_job.wait_signal.get_object();
		if (owner && owner->has_method("cancel")) {
			owner->call("cancel");
		}
		p_job.wait_signal = Signal();
	}
	if (p_job.file.is_valid()) {
		p_job.file->abort();
		p_job.file.unref();
	}
	p_job.hold.unref();
}

// Release expired suspended work in deadline order.
void GDWebApp::trim_jobs() {
	const uint64_t now = GDClock::msec();
	if (!job_times.is_empty() && job_times.front()->get().due <= now) {
		const JobTime timed = job_times.front()->get();
		job_times.erase(timed);
		const int id = timed.id;
		HashMap<int, Job>::Iterator it = jobs.find(id);
		if (it) {
			Job job = it->value;
			drop_job_time(id, job);
			jobs.erase(id); // Remove map references before cancellation signals can reenter synchronously.
			if (srv->request_alive(id)) {
				srv->respond(id, 504, String("gateway timeout").to_utf8_buffer(), TEXT_TYPE);
			}
			cancel_job(job);
		}
	}
	arm_jobs();
}

// Remove completed or cancelled jobs from counts and the deadline index.
void GDWebApp::drop_job_time(int p_id, const Job &p_job) {
	if (p_job.stage != ENCODE) {
		awaiting--;
	}
	if (p_job.due > 0) {
		job_times.erase(JobTime{ p_job.due, p_id });
	}
}

// Register only the earliest handler deadline with the runtime timer.
void GDWebApp::arm_jobs() {
	Async::drop_deadline(this, job_due);
	job_due = 0;
	if (job_times.is_empty()) {
		return;
	}
	job_due = job_times.front()->get().due;
	Async::track_deadline(this, job_due, callable_mp(this, &GDWebApp::poll));
}

// Normalize signal values while keeping the bound request identifier separate.
Variant GDWebApp::resume_signal(const Variant **p_args, int p_count, Callable::CallError &r_err) {
	r_err.error = Callable::CallError::CALL_OK;
	if (p_count > 0) {
		resumed(Async::signal_value(p_args, p_count - 1), int(*p_args[p_count - 1]));
	}
	return Variant();
}

// Resume request processing with an asynchronous result.
void GDWebApp::resumed(const Variant &p_value, int p_id) {
	HashMap<int, Job>::Iterator it = jobs.find(p_id);
	if (!it) {
		return;
	}
	Job job = it->value;
	drop_job_time(p_id, job);
	jobs.erase(p_id);
	arm_jobs();
	job.hold.unref();
	job.wait_signal = Signal();
	job.wait_call = Callable();
	if (job.stage == ENCODE && (srv.is_null() || !srv->has_request(p_id))) {
		cancel_job(job);
		if (shutting) post_poll();
		return;
	}
	busy++;
	current_id = p_id;
	// A completed coroutine may return another operation that must finish first.
	if (!park(p_value, p_id, job)) {
		run(p_id, job, p_value, true, GDClock::usec() + GD_SCHED_SLICE_USEC);
	}
	current_id = -1;
	leave();
	if (shutting) {
		post_poll(); // Reclaim closed connections after the final asynchronous handler completes.
	}
}

// Execute request middleware and handlers in order.
void GDWebApp::run(int p_id, Job &p_job, const Variant &p_back, bool p_resumed, uint64_t p_until) {
	const bool sliced = GDScriptFunction::begin_time_slice(p_until);
	const uint64_t outer = GDScriptFunction::native_time_slice_deadline();
	if (outer) p_until = p_until ? MIN(p_until, outer) : outer;
	if (p_resumed) {
		step(p_job, p_back);
	}

	// Advance the phase position after each call and return immediately on suspension.
	while (p_job.stage != DONE) {
		Variant ret;
		bool called = false;

		switch (p_job.stage) {
			case PRE: {
				if (p_job.at < (int)pres.size()) {
					ret = web_call(pres[p_job.at].fn, p_job.req);
					called = true;
				} else {
					pick(p_job);
				}
			} break;

			case STATIC: {
				ret = pick_static(p_job);
				called = ret.get_type() == Variant::SIGNAL;
			} break;

			case USE: {
				if (p_job.at < (int)uses.size()) {
					ret = web_call(uses[p_job.at].fn, p_job.req);
					called = true;
				} else if (p_job.ready) {
					p_job.stage = AFTER;
					p_job.at = 0;
				} else {
					p_job.stage = p_job.band >= 0 ? BAND : ROUTE;
					p_job.at = 0;
				}
			} break;

			case BAND: {
				const LocalVector<Mid> &mid = bands[p_job.band].mids;
				if (p_job.at < (int)mid.size()) {
					ret = web_call(mid[p_job.at].fn, p_job.req);
					called = true;
				} else {
					p_job.stage = ROUTE;
					p_job.at = 0;
				}
			} break;

			case ROUTE: {
				if (p_job.mids >= 0 && p_job.at < (int)route_mids[p_job.mids].size()) {
					ret = web_call(route_mids[p_job.mids][p_job.at].fn, p_job.req);
					called = true;
				} else {
					p_job.stage = HANDLE;
					p_job.at = 0;
				}
			} break;

			case HANDLE: {
				if (p_job.handler.is_valid()) {
					ret = web_call(p_job.handler, p_job.req);
					p_job.handler = Callable();
					called = true;
				} else {
					p_job.stage = FAIL;
					p_job.at = 0;
				}
			} break;

			case FAIL: {
				R *res = p_job.at == 0 ? Object::cast_to<R>(p_job.out) : nullptr;
				// Treat a directly returned error reason like an R failure.
				Ref<Err> why;
				if (res != nullptr) {
					why = res->get_e();
				} else if (p_job.at == 0) {
					why = Ref<Err>(p_job.out);
				}
				if (why.is_valid()) {
					if (on_fail.is_valid()) {
						ret = web_call(on_fail, p_job.req, why);
						called = true;
						break;
					}
					// Keep detailed failure text in logs rather than exposing statements or destinations.
					// External responses normally contain only the status reason phrase.
					// Include details in the body only when show_errors(true) is enabled.
					const int code = Http::status_of(why);
					if (tell_why) {
						p_job.out = Http::text(why->text(), code);
					} else {
						// Log only 5xx errors; 4xx failures originate from the requester.
						// Logging every client failure would permit external log amplification.
						if (code >= 500) {
							ERR_PRINT(vformat("handler failed: %s", LogState::flat(why->text())));
						}
						p_job.out = Http::text(http_reason(code), code);
					}
				} else if (res != nullptr) {
					p_job.out = res->get_v(); // Use the successful result's value directly as the response.
				}
				p_job.stage = AFTER;
				p_job.at = p_job.after_at;
			} break;

			case AFTER: {
				if (p_job.at < (int)afters.size()) {
					ret = web_call(afters[p_job.at], p_job.req, p_job.out);
					called = true;
				} else {
					p_job.stage = ENCODE;
				}
			} break;

			case ENCODE: {
				// Reject unsent response failures before selecting an encoding path.
				const Ref<Err> error = R::unpack(p_job.out);
				if (error.is_valid()) { failed(p_job, error); break; }
				if (p_job.out.get_type() == Variant::OBJECT) {
					failed(p_job, Err::make("response must be data or an envelope", Err::INVALID_DATA));
					break;
				}
				// Preserve shared envelopes unless a wrapped body needs replacing.
				if (p_job.out.get_type() == Variant::DICTIONARY) {
					Dictionary response = p_job.out;
					const Variant *body = response.getptr(SNAME("body"));
					if (body && body->get_type() == Variant::OBJECT) {
						Variant value = *body;
						const Ref<Err> error = R::unpack(value);
						if (error.is_valid()) { failed(p_job, error); break; }
						if (value.get_type() == Variant::OBJECT && Ref<GDBodySource>(value).is_null()) {
							failed(p_job, Err::make("response body must be data or a byte source", Err::INVALID_DATA));
							break;
						}
						if (Ref<R>(*body).is_valid()) {
							response = response.duplicate();
							response["body"] = value;
							p_job.out = response;
						}
					}
				}
				const Variant value = p_job.out;
				// Borrow body storage only while the retained envelope is unchanged in this turn.
				const Variant *content = nullptr;
				if (value.get_type() == Variant::DICTIONARY) {
					const Dictionary response = value;
					content = response.getptr(SNAME("body"));
				}
				// Measure omitted text only after middleware has finalized its representation.
				if (srv->is_head(p_id)) {
					const Variant &text = content ? *content : value;
					if (text.get_type() == Variant::STRING || text.get_type() == Variant::STRING_NAME) {
						p_job.head_text = true;
						ret = utf8_size(text, p_until);
						called = true;
						break;
					}
				}
				// Scalar text does not need dictionary allocation or response-envelope lookups.
				if (value.get_type() == Variant::STRING || value.get_type() == Variant::STRING_NAME) {
					p_job.encoded_type = TEXT_TYPE;
					ret = utf8_reply(value, p_until, [](const PackedByteArray &p_bytes) -> Variant { return p_bytes; });
					called = true;
					break;
				}
				const bool raw_json = value.get_type() == Variant::DICTIONARY && !content;
				const bool text_body = content && content->get_type() != Variant::PACKED_BYTE_ARRAY && content->get_type() != Variant::OBJECT;
				const bool plain_value = value.get_type() != Variant::DICTIONARY && value.get_type() != Variant::PACKED_BYTE_ARRAY && value.get_type() != Variant::NIL;
				if (!raw_json && !text_body && !plain_value) {
					p_job.stage = DONE;
					break;
				}
				// Keep resumable conversion local until its time slice requires a worker continuation.
				if (raw_json) {
					p_job.encoded_type = JSON_TYPE;
					ret = JsonData::encode_reply(value, p_until);
					called = true;
					break;
				}
				// Encode envelope text with the same resumable path as scalar text.
				if (text_body && (content->get_type() == Variant::STRING || content->get_type() == Variant::STRING_NAME)) {
					p_job.body_encoded = true;
					ret = utf8_reply(*content, p_until, [](const PackedByteArray &p_bytes) -> Variant { return p_bytes; });
					called = true;
					break;
				}
				// User callbacks and non-resumable formatting retain their worker execution context.
				p_job.body_encoded = text_body;
				p_job.encoded_type = TEXT_TYPE;
				const Variant body = text_body ? *content : value;
				ret = GDValueCall::start([body]() -> Variant { return Pool::text(body).to_utf8_buffer(); });
				called = true;
			} break;
		}

		if (!called) {
			continue;
		}
		if (park(ret, p_id, p_job)) {
			if (sliced) {
				GDScriptFunction::end_time_slice();
			}
			return;
		}
		step(p_job, ret);
	}
	if (sliced) {
		GDScriptFunction::end_time_slice();
	}
	finish(p_id, p_job);
}

// Route an unsent failure once, retaining postprocessing progress and clearing stale body state.
void GDWebApp::failed(Job &p_job, const Ref<Err> &p_error) {
	if (p_job.file.is_valid()) { p_job.file->abort(); p_job.file.unref(); }
	p_job.body = Variant();
	p_job.body_encoded = false;
	p_job.head_text = false;
	p_job.encoded_type = nullptr;
	if (p_job.failing) {
		ERR_PRINT(vformat("error response failed: %s", LogState::flat(p_error->text())));
		p_job.out = Http::bytes_out(String("Internal Server Error").to_utf8_buffer(), TEXT_TYPE, 500);
		p_job.stage = DONE;
		return;
	}
	p_job.after_at = p_job.stage == AFTER ? p_job.at + 1 : p_job.stage == ENCODE ? afters.size() : 0;
	p_job.failing = true;
	p_job.out = p_error;
	p_job.stage = FAIL;
	p_job.at = 0;
}

// Advance the asynchronous processing state by one phase.
void GDWebApp::step(Job &p_job, const Variant &p_value) {
	Variant p_ret = p_value;
	if (p_job.stage != STATIC) {
		const Ref<Err> error = R::unpack(p_ret);
		if (error.is_valid()) { failed(p_job, error); return; }
	}
	switch (p_job.stage) {
		case PRE:
		case USE:
		case BAND:
		case ROUTE: {
			Variant out = p_ret;
			// Stop ordinary processing when a value is returned, but still run postprocessing.
			if (out.get_type() != Variant::NIL) {
				p_job.out = out;
				p_job.stage = AFTER;
				p_job.at = 0;
				return;
			}
			p_job.at++;
		} break;

		case STATIC: {
			const Ref<R> opened = p_ret;
			if (opened.is_null() || !opened->get_ok()) {
				if (p_job.file.is_valid()) {
					p_job.file->abort();
					p_job.file.unref();
				}
				const Ref<Err> error = opened.is_valid() ? opened->get_e() : Err::make("invalid static file result", Err::NONE);
				if (!error->is(Err::NOT_FOUND)) failed(p_job, error);
				return; // Missing files may be supplied by another static root.
			}
			if (p_job.file.is_null()) {
				const Dictionary served = opened->get_v();
				if (served.is_empty()) {
					return; // Inspect the next static root.
				}
				p_job.out = served;
				p_job.ready = true;
				p_job.stage = USE;
				p_job.at = 0;
				break;
			}
			Dictionary headers;
			headers["Content-Type"] = p_job.file_type;
			Dictionary out;
			out["status"] = 200;
			out["headers"] = headers;
			out["body"] = p_job.file;
			p_job.out = out;
			p_job.ready = true;
			p_job.stage = USE;
			p_job.at = 0;
		} break;

		case HANDLE: {
			p_job.out = p_ret;
			p_job.stage = FAIL;
			p_job.at = 0;
		} break;

		case FAIL: {
			// Response constructed by the error handler.
			p_job.out = p_ret;
			p_job.at = 1;
		} break;

		case AFTER: {
			// Leave the response unchanged when postprocessing returns nothing.
			if (p_ret.get_type() != Variant::NIL) {
				p_job.out = p_ret;
			}
			p_job.at++;
		} break;

		case ENCODE: {
			if (p_job.head_text && p_ret.get_type() == Variant::INT) {
				p_job.body = Ref<HeadBody>(memnew(HeadBody(int64_t(p_ret))));
				p_job.body_encoded = true;
				p_job.stage = DONE;
				break;
			}
			if (p_job.body_encoded && p_ret.get_type() == Variant::PACKED_BYTE_ARRAY) {
				p_job.body = p_ret;
				p_job.stage = DONE;
				break;
			}
			p_job.out = p_ret;
			p_job.stage = DONE;
		} break;

		default:
			break;
	}
}

// Select a route and handler matching the request.
void GDWebApp::pick(Job &p_job) {
	GDWebRequest *req = p_job.req.ptr();
	String method = req->get_method();
	// Dispatch HEAD through GET routes with the same headers but no body.
	// The transport suppresses the body; separate routing would incorrectly return 404.
	const bool head_only = (method == "HEAD");
	if (head_only) {
		method = "GET";
	}

	// Resolve exact routes with one dictionary lookup on the common path.
	HashMap<String, LocalVector<Slot>>::ConstIterator hit = exact.find(req->route_txt);
	if (hit) {
		for (const Slot &slot : hit->value) {
			if (slot.method != method) {
				continue;
			}
			p_job.handler = slot.handler;
			p_job.band = slot.group;
			p_job.mids = slot.mids;
			p_job.stage = USE;
			p_job.at = 0;
			return;
		}
	}

	// Match routes with named captures.
	if (!routes.is_empty() && req->path_parts.is_empty()) {
		req->path_parts = req->path_txt.split("/", true); // Split only when a named-capture route can consume the segments.
	}
	const PackedStringArray &target = req->path_parts;
	for (const Route &r : routes) {
		if (r.method != method || !match(r, target, req)) {
			continue;
		}
		p_job.handler = r.handler;
		p_job.band = r.group;
		p_job.mids = r.mids;
		p_job.stage = USE;
		p_job.at = 0;
		return;
	}

	// If no route matches, inspect static files on a worker.
	p_job.static_at = 0;
	p_job.stage = STATIC;
}

// Inspect the next static root and return its file-open completion signal.
Variant GDWebApp::pick_static(Job &p_job) {
	GDWebRequest *req = p_job.req.ptr();
	const String method = req->get_method();
	for (; p_job.static_at < (int)statics.size(); p_job.static_at++) {
		const Pair<String, String> &st = statics[p_job.static_at];
		if (method != "GET") {
			if (method != "HEAD") {
				break;
			}
		}
		if (!req->route_txt.begins_with(st.first) || (req->route_txt.length() > st.first.length() && req->route_txt[st.first.length()] != '/')) {
			continue;
		}
		// Decode percent escapes at the URL-to-filesystem boundary, supporting names with spaces.
		// Check containment after decoding so escaped traversal such as %2e%2e cannot escape the root.
		String rel;
		if (!Url::decode_part(req->route_txt.substr(st.first.length()).trim_prefix("/"), false, rel)) {
			continue;
		}
		const String full = Path::under(st.second, rel);
		if (full.is_empty()) {
			continue;
		}
		p_job.static_at++;
		if (!afters.is_empty()) {
			// Build the reply dictionary expected by postprocessors while offloading file I/O.
			return GDFileCall::start([full]() { return file_reply(full); });
		}
		p_job.file.instantiate();
		p_job.file_type = Media::by_path(full);
		return p_job.file->open(full, method != "HEAD");
	}

	if (fallback.is_valid()) {
		p_job.handler = fallback;
		p_job.band = -1;
		p_job.stage = USE;
		p_job.at = 0;
		return Variant();
	}
	p_job.out = Http::not_found("not found");
	p_job.ready = true;
	p_job.stage = USE;
	p_job.at = 0;
	return Variant();
}

// Check whether a route pattern matches the request path.
bool GDWebApp::match(const Route &p_route, const PackedStringArray &p_target, GDWebRequest *p_req) const {
	if (p_route.parts.size() != p_target.size()) {
		return false;
	}
	Dictionary got;
	for (int i = 0; i < p_route.parts.size(); i++) {
		const String part = p_route.parts[i];
		if (part.begins_with(":")) {
			// Pass once-decoded request segments directly into route captures.
			got[part.substr(1)] = p_target[i];
		} else if (part != p_target[i]) {
			return false;
		}
	}
	p_req->params_map = got;
	return true;
}

// Read a file on the I/O queue and preserve its original result.
Signal GDWebApp::file_at(const String &p_path) const {
	return GDFileCall::start([p_path]() { return file_reply(p_path); });
}

// Complete processing and release request resources.
void GDWebApp::finish(int p_id, Job &p_job) {
	// Do not stringify unsupported objects into public responses; retain details only in logs.
	if (p_job.out.get_type() == Variant::OBJECT) {
		const Ref<R> res = p_job.out;
		const Ref<Err> why = res.is_valid() ? res->get_e() : Ref<Err>(p_job.out);
		const String detail = why.is_valid() ? why->text() : String("returned an object");
		ERR_PRINT(vformat("handler failed: %s", LogState::flat(detail)));
		if (p_job.file.is_valid()) {
			p_job.file->abort();
			p_job.file.unref();
		}
		srv->respond(p_id, 500, String("internal error").to_utf8_buffer(), TEXT_TYPE);
		p_job.req->finish_context("request finished");
		if (p_job.req->get_reference_count() == 1) {
			spare = p_job.req;
		}
		return;
	}
	const Variant &p_out = p_job.out;
	bool streaming = false;
	if (p_out.get_type() == Variant::DICTIONARY) {
		const Dictionary out = p_out;
		const Variant *content = out.getptr(SNAME("body"));
		bool sent = false;
		if (content) {
			// Read final metadata after encoding, preserving shared-envelope updates during await.
			const Variant *status_value = out.getptr(SNAME("status"));
			const Variant *header_value = out.getptr(SNAME("headers"));
			const int status = status_value ? safe_status(*status_value) : 200;
			const Variant body = p_job.body_encoded ? p_job.body : *content;
			const Ref<GDBodySource> source = body;
			if (header_value) {
				const Dictionary headers = *header_value;
				const Variant *type = only_type(headers);
				if (source.is_valid()) {
					streaming = source->watch_disconnect();
					if (streaming) source->set_context(p_job.req->get_context());
					if (p_job.head_text && type) {
						srv->respond_file(p_id, status, Dictionary(), source, *type);
					} else {
						srv->respond_file(p_id, status, headers, source, String());
					}
					p_job.file.unref();
					sent = true;
				} else if (body.get_type() == Variant::PACKED_BYTE_ARRAY) {
					if (type) {
						srv->respond(p_id, status, body, *type);
					} else {
						srv->respond_with(p_id, status, headers, body);
					}
					sent = true;
				}
			} else if (body.get_type() == Variant::PACKED_BYTE_ARRAY) {
				srv->respond(p_id, status, body, TEXT_TYPE);
				sent = true;
			} else if (p_job.head_text && source.is_valid()) {
				srv->respond_file(p_id, status, Dictionary(), source, TEXT_TYPE);
				sent = true;
			}
		}
		if (!sent) {
			srv->respond(p_id, 500, String("invalid response").to_utf8_buffer(), TEXT_TYPE);
		}
	} else if (p_job.head_text && p_job.body_encoded) {
		srv->respond_file(p_id, 200, Dictionary(), p_job.body, TEXT_TYPE);
	} else if (p_out.get_type() == Variant::PACKED_BYTE_ARRAY) {
		srv->respond(p_id, 200, p_out, p_job.encoded_type ? p_job.encoded_type : BYTES_TYPE);
	} else if (p_out.get_type() == Variant::NIL) {
		srv->respond(p_id, 204, PackedByteArray(), TEXT_TYPE); // No response body.
	} else {
		srv->respond(p_id, 500, String("invalid response").to_utf8_buffer(), TEXT_TYPE);
	}
	if (p_job.file.is_valid()) {
		p_job.file->abort(); // Middleware replaced the static body.
		p_job.file.unref();
	}
	if (!streaming) p_job.req->finish_context("request finished");
	// Reuse the request object if the handler has not retained it.
	if (!streaming && p_job.req->get_reference_count() == 1) {
		spare = p_job.req;
	}
}

GDWebApp::~GDWebApp() {
	stop();
}

// ---------------- Route groups ----------------

void GDWebRouteGroup::use(const Variant &p_mid) {
	GDWebApp::Mid mid = app->mid_of(p_mid);
	if (app->invalid(!mid.fn.is_valid(), "middleware must be Callable or have handle(req)")) return;
	app->bands[id].mids.push_back(mid);
}

// Register a route with its group's prefix and middleware.
void GDWebRouteGroup::route(const String &p_method, const String &p_pattern, const Callable &p_handler, const Array &p_mids) {
	app->add_route(p_method, app->bands[id].prefix + p_pattern, p_handler, p_mids, id);
}

// Register public script methods and properties.
void GDWebRouteGroup::_bind_methods() {
	ClassDB::bind_method(D_METHOD("use", "middleware"), &GDWebRouteGroup::use);
	ClassDB::bind_method(D_METHOD("route", "method", "pattern", "handler", "middleware"), &GDWebRouteGroup::route, DEFVAL(Array()));
}

// Register public script methods and properties.
void GDWebApp::_bind_methods() {
	MethodInfo resume;
	resume.name = "_resume_signal";
	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, resume.name, &GDWebApp::resume_signal, resume);
	ClassDB::bind_method(D_METHOD("route", "method", "pattern", "handler", "middleware"), &GDWebApp::route, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("pre", "middleware"), &GDWebApp::pre);
	ClassDB::bind_method(D_METHOD("use", "middleware"), &GDWebApp::use);
	ClassDB::bind_method(D_METHOD("after", "handler"), &GDWebApp::after);
	ClassDB::bind_method(D_METHOD("on_error", "handler"), &GDWebApp::on_error);
	ClassDB::bind_method(D_METHOD("show_errors", "on"), &GDWebApp::show_errors);
	ClassDB::bind_method(D_METHOD("body_limit", "bytes"), &GDWebApp::body_limit);
	ClassDB::bind_method(D_METHOD("limits", "opts"), &GDWebApp::limits);
	ClassDB::bind_method(D_METHOD("dropped_headers"), &GDWebApp::dropped_headers);
	ClassDB::bind_method(D_METHOD("group", "prefix", "middleware"), &GDWebApp::group, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("static", "prefix", "dir"), &GDWebApp::static_dir);
	ClassDB::bind_method(D_METHOD("fallback", "handler"), &GDWebApp::otherwise);
	ClassDB::bind_method(D_METHOD("file_at", "path"), &GDWebApp::file_at);
	ClassDB::bind_method(D_METHOD("file_at_async", "path"), &GDWebApp::file_at);
	ADD_AWAIT("file_at_async", "R:Dictionary");
	ADD_AWAIT("file_at", "R:Dictionary");
	ADD_AUTO_WAIT("file_at");
	ClassDB::bind_method(D_METHOD("listen", "port", "host"), &GDWebApp::listen, DEFVAL("127.0.0.1"));
	ClassDB::bind_method(D_METHOD("listen_tls", "port", "cert", "key", "host", "opts"), &GDWebApp::listen_tls, DEFVAL("127.0.0.1"), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("listen_tls_async", "port", "cert", "key", "host", "opts"), &GDWebApp::listen_tls, DEFVAL("127.0.0.1"), DEFVAL(Dictionary()));
	ADD_AWAIT("listen_tls", "R:Variant");
	ADD_AWAIT("listen_tls_async", "R:Variant");
	ADD_AUTO_WAIT("listen_tls");
	ADD_RESULT("listen", "Variant");
	ClassDB::bind_method(D_METHOD("port"), &GDWebApp::port);
	ClassDB::bind_method(D_METHOD("shutdown", "context"), &GDWebApp::shutdown);
	ClassDB::bind_method(D_METHOD("shutdown_async", "context"), &GDWebApp::shutdown);
	ADD_AWAIT("shutdown", "R:Variant");
	ADD_AWAIT("shutdown_async", "R:Variant");
	ADD_AUTO_WAIT("shutdown");
	ClassDB::bind_method(D_METHOD("stop"), &GDWebApp::stop);
	ClassDB::bind_method(D_METHOD("is_listening"), &GDWebApp::is_listening);
	ClassDB::bind_method(D_METHOD("poll"), &GDWebApp::poll);
}

// ---------------- Response constructors ----------------

namespace {

// Normalize handler return values into HTTP responses.
Dictionary reply(int64_t p_status, const String &p_type, const Variant &p_body) {
	Dictionary headers;
	headers["Content-Type"] = p_type;
	Dictionary out;
	out["status"] = safe_status(p_status);
	out["headers"] = headers;
	out["body"] = p_body;
	return out;
}

} // namespace

// Send an HTTP request and return an asynchronous response.
Signal Http::fetch(const String &p_url, const Dictionary &p_opts, const Ref<GDHTTPTransport> &p_transport) {
	Ref<GDHTTPCall> call;
	call.instantiate();
	call->begin(p_url, p_opts, p_transport);
	return Signal(call.ptr(), "finished");
}

// Return retained content as text.
Dictionary Http::text(const String &p_body, int64_t p_status) {
	return reply(p_status, TEXT_TYPE, p_body);
}

// Create an HTML response.
Dictionary Http::html(const String &p_body, int64_t p_status) {
	return reply(p_status, HTML_TYPE, p_body);
}

// Create the response on the encoding path that completes its body.
Variant Http::json_out(const Variant &p_data, int64_t p_status, uint64_t p_until) {
	return JsonData::encode_reply(p_data, p_until, [p_status](const Variant &p_encoded) -> Variant {
		return json_reply(p_encoded, p_status);
	});
}

// Return assembled content without an intermediate text conversion.
Dictionary Http::bytes_out(const PackedByteArray &p_body, const String &p_type, int64_t p_status) {
	return reply(p_status, p_type, p_body);
}

// Add protective headers and return the same reply dictionary for chaining.
Dictionary Http::guard(const Dictionary &p_reply) {
	// Add security defaults without overwriting caller-supplied values.
	// Cover content interpretation, framing, referrer leakage, and cross-origin isolation.
	// Also restrict browser features and require secure transport after an HTTPS response.
	static const char *pairs[][2] = {
		{ "X-Content-Type-Options", "nosniff" }, // Disable content-type sniffing.
		{ "X-Frame-Options", "DENY" }, // Disallow embedding in frames.
		{ "Referrer-Policy", "no-referrer" }, // Do not expose the referring URL.
		// Default to same-origin content sources to reduce the impact of injected content.
		{ "Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'" },
		// Require HTTPS for subsequent visits; browsers ignore this header over plaintext HTTP.
		// This applies when deployed behind TLS termination and lasts 180 days.
		{ "Strict-Transport-Security", "max-age=15552000; includeSubDomains" },
		// Disable powerful browser features unless the page explicitly enables them.
		{ "Permissions-Policy", "geolocation=(), microphone=(), camera=()" },
		{ "Cross-Origin-Opener-Policy", "same-origin" }, // Do not share opener context across origins.
		{ "Cross-Origin-Resource-Policy", "same-origin" }, // Disallow cross-origin resource access.
		{ "X-Permitted-Cross-Domain-Policies", "none" }, // Disable legacy cross-domain policy files.
	};
	Dictionary out = p_reply;
	Dictionary headers = out.get("headers", Dictionary());
	for (const char *const *one : pairs) {
		if (!headers.has(one[0])) {
			headers[one[0]] = one[1];
		}
	}
	out["headers"] = headers;
	return out;
}

// Default redirects to the same origin to avoid trusting external input as a destination.
// An unchecked external destination can disguise a phishing redirect behind a trusted origin.
// Callers must explicitly set away to permit external redirects.
Dictionary Http::redirect(const String &p_to, int64_t p_status, bool p_away) {
	String to = p_to;
	if (!p_away) {
		// Reject external schemes and network paths while preserving same-origin relative references.
		if (!local_location(to)) {
			to = "/"; // Replace an untrusted target with the local root.
		}
	}
	Dictionary headers;
	headers["Location"] = to;
	Dictionary out;
	out["status"] = safe_status(p_status);
	out["headers"] = headers;
	out["body"] = "";
	return out;
}

// Create a 404 response.
Dictionary Http::not_found(const String &p_msg) {
	return reply(404, TEXT_TYPE, p_msg);
}

// Replace a response header.
Dictionary Http::head(const Dictionary &p_reply, const String &p_name, const Variant &p_value) {
	Dictionary headers = p_reply.get("headers", Dictionary());
	headers[p_name] = p_value; // Sequence values produce multiple fields with the same name.
	Dictionary out = p_reply;
	out["headers"] = headers;
	return out;
}

// Append without removing existing values; dictionaries retain one value per key.
// Promote the second occurrence to a sequence.
Dictionary Http::add_head(const Dictionary &p_reply, const String &p_name, const Variant &p_value) {
	Dictionary headers = p_reply.get("headers", Dictionary());
	if (!headers.has(p_name)) {
		return head(p_reply, p_name, p_value);
	}
	const Variant had = headers[p_name];
	Array vals;
	if (had.get_type() == Variant::ARRAY) {
		vals = had;
		vals = vals.duplicate(); // Do not mutate the caller's sequence.
	} else {
		vals.push_back(had);
	}
	vals.push_back(p_value);
	return head(p_reply, p_name, vals);
}

// Return the HTTP status corresponding to an Err.
int Http::status_of(const Ref<Err> &p_err) {
	if (p_err.is_null()) {
		return 500;
	}
	switch (p_err->get_kind()) {
		case Err::NOT_FOUND:
			return 404;
		case Err::PERMISSION_DENIED:
			return 403;
		case Err::INVALID_DATA:
			return 400;
		case Err::TIMED_OUT:
			return 504;
		case Err::LIMITED:
			return 429;
		case Err::UNSUPPORTED:
			return 501;
		case Err::UNAUTHENTICATED:
			return 401;
		default:
			return 500;
	}
}
