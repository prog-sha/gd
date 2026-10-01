/**************************************************************************/
/*  serve.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement routed HTTP applications declared in serve.h.

#include "cli/net/serve.h"
#include "cli/sys/gdtask.h"
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
#include "cli/sys/task.h"

namespace {

// Default content types when a handler supplies none.
const char *TEXT_TYPE = "text/plain; charset=utf-8"; // Default for plain text responses.
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
VariantPair json_reply(const VariantPair &p_encoded, int64_t p_status) {
	if (p_encoded.error.get_type() != Variant::NIL) return { Variant(), p_encoded.error };
	if (p_encoded.value.get_type() != Variant::PACKED_BYTE_ARRAY) return { Variant(), Err::make("invalid JSON encoder result", Err::INVALID_DATA) };
	return { GDWebResponse::make(p_status, HTTP_TYPE_JSON, p_encoded.value), Variant() };
}

// Preserve file errors while constructing the body needed by postprocessors.
VariantPair file_reply(const String &p_path) {
	const VariantPair read = Os::read_bytes(p_path);
	if (Ref<Err>(read.error).is_valid()) return { Variant(), Ref<Err>(read.error)->with_partial(read.value) };
	return { Http::bytes_out(read.value, Media::by_path(p_path), 200), Variant() };
}

// Convert dynamic callback failures into ordinary handler errors while preserving suspension.
template <typename... Args>
VariantPair web_call(bool &r_fault, const Callable &p_call, Args... p_args) {
	const Variant values[2] = { Variant(p_args)... };
	const Variant *args[] = { (&values[0]) + 0, (&values[0]) + 1 };
	Variant out;
	Variant failure;
	Callable::CallError error;
	error.result_error = &failure;
	GDScriptFunction::SuspendableCall suspendable;
	const uint64_t fault_before = GDScriptFunction::runtime_fault_epoch();
	p_call.callp(args, sizeof...(p_args), out, error);
	r_fault = error.runtime_failed || fault_before != GDScriptFunction::runtime_fault_epoch();
	if (r_fault) return {};
	if (error.error != Callable::CallError::CALL_OK) return { Variant(), Err::make("web callback could not be invoked", Err::INVALID_DATA) };
	return { out, failure };
}

// Find a response field regardless of the spelling of its name.
const Variant *find_header(const Dictionary &p_headers, const String &p_name) {
	if (const Variant *exact = p_headers.getptr(p_name)) return exact;
	for (const Variant &key : p_headers.keys()) {
		if ((key.get_type() == Variant::STRING || key.get_type() == Variant::STRING_NAME) && String(key).nocasecmp_to(p_name) == 0) return p_headers.getptr(key);
	}
	return nullptr;
}

// Choose the compact response path when Content-Type is the only header.
const Variant *only_type(const Dictionary &p_headers) {
	if (p_headers.size() != 1) return nullptr;
	// Reuse the interned name before checking alternate header spellings.
	if (const Variant *type = p_headers.getptr(SNAME("Content-Type"))) return type;
	return find_header(p_headers, "Content-Type");
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
		const Variant::Type type = p_opts[raw].get_type();
		const bool duration = name == "job_timeout" || name == "header_timeout" || name == "body_timeout";
		if (duration ? (type != Variant::INT && type != Variant::FLOAT) : type != Variant::INT) {
			return vformat("web limit \"%s\" must be numeric", name);
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
// Raw-completion probes cover both policies without auto-wait.
VariantPair GDWebBodyCall::start(const Ref<GDWebRequest> &p_req, const Ref<GDWebServer> &p_srv, int p_id, int p_mode, int64_t p_want, const String &p_path, bool p_deferred) {
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
		if (ready_done) return ready;
	}
	return { Signal(this, "finished"), Variant() };
}

// Deliver a result exactly once and release retained resources.
void GDWebBodyCall::finish(const VariantPair &p_value) {
	if (self_hold.is_null()) return;
	// Wait for an in-flight write before reporting its final partial count.
	const Ref<Err> why = p_value.error;
	if (mode == SAVE && sink.is_valid() && !sink->done() && why.is_valid()) {
		if (failure.is_null()) failure = why;
		flushing = true;
		sink->abort();
		return;
	}
	VariantPair result = p_value;
	if (why.is_valid() && result.value.get_type() == Variant::NIL) {
		if (mode == SAVE && sink.is_valid()) result.value = sink->count();
		else if (mode == READ || mode == BYTES) result.value = data;
	}
	if (why.is_valid()) {
		result.error = why->with_partial(result.value);
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
		ready = result;
		ready_done = true;
	} else {
		Async::finish(this, SNAME("finished"), result.value, result.error);
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
		Ref<Err> why = failure;
		const Ref<Err> sink_error = sink->error();
		why = Err::join(why, sink_error);
		const int64_t written = sink->count();
		sink.unref();
		finish({ written, why });
		return false;
	}
	// Do not read another chunk until the writer catches up.
	if (mode == SAVE && sink.is_valid() && sink->pending_bytes() > 0) {
		return false;
	}
	if (srv.is_null() || !srv->has_conn(id)) {
		finish({ Variant(), Err::make("request body is no longer available", Err::INTERRUPTED) });
		return false;
	}
	PackedByteArray part;
	const int state = srv->read_body(id, mode == READ ? want : COPY_BUFFER, part);
	if (state == GDWebServer::BODY_READ_WAIT) {
		if (!srv->wait_body(id, callable_mp(this, &GDWebBodyCall::schedule))) {
			finish({ Variant(), Err::make("request body is no longer available", Err::INTERRUPTED) });
		}
		return false;
	}
	srv->clear_body_wait(id);
	if (state == GDWebServer::BODY_READ_LIMIT) {
		Dictionary info;
		info["http_status"] = 413;
		finish({ Variant(), Err::make("http: request body too large", Err::LIMITED, info) });
		return false;
	}
	if (state == GDWebServer::BODY_READ_BAD) {
		finish({ Variant(), Err::make("request body ended before its boundary", Err::INVALID_DATA) });
		return false;
	}
	if (!part.is_empty()) {
		total += part.size();
		if (mode == SAVE) {
			if (sink.is_null() || sink->error().is_valid()) {
				finish({ sink.is_valid() ? Variant(sink->count()) : Variant(total), sink.is_valid() ? Variant(sink->error()) : Variant(Err::make("body writer unavailable", Err::INTERRUPTED)) });
				return false;
			}
			sink->push(part);
		} else {
			const int64_t at = data.size();
			if (at > INT64_MAX - part.size()) {
				finish({ Variant(), Err::make("request body is too large for an in-memory value", Err::LIMITED) });
				return false;
			}
			if (at == 0) {
				data = part;
			} else {
				if (data.resize_uninitialized(at + part.size()) != OK) {
					finish({ Variant(), Err::make("cannot allocate request body", Err::NONE) });
					return false;
				}
				memcpy(data.ptrw() + at, part.ptr(), part.size());
			}
		}
	}
	if (mode == READ && (!part.is_empty() || state == GDWebServer::BODY_READ_EOF)) {
		finish({ data, Variant() });
		return false;
	}
	if (state != GDWebServer::BODY_READ_EOF) return true;
	// Keep the complete body so repeated whole-body reads return the same content.
	if (mode != SAVE && req.is_valid()) {
		req->body_cache = data;
		req->body_cached = true;
	}
	if (mode == SAVE) {
		sink->close();
		flushing = true;
		return true; // Closure may already have completed before this turn.
	} else {
		if (mode == BYTES) {
			finish({ data, Variant() });
			return false;
		}
		// Decode ready JSON locally while retaining the shared turn and resumable worker fallback.
		if (mode == JSON) {
			const VariantPair result = JsonData::decode_reply(data, GDScriptFunction::native_time_slice_deadline());
			if (result.value.get_type() == Variant::SIGNAL) {
				converting = true;
				Signal(result.value).connect(callable_mp(this, &GDWebBodyCall::converted), Object::CONNECT_ONE_SHOT);
			} else {
				converted(result.value, result.error);
			}
			return false;
		}
		// Move whole-body native conversions off the serve loop because they cannot suspend midway.
		const PackedByteArray body = data;
		const Dictionary checked_rule = rule;
		converting = true;
		Signal converted_signal = GDPairCall::start([body, checked_rule, mode = mode]() -> VariantPair {
			if (mode == TEXT) {
				if (body.size() >= INT_MAX) return { Variant(), Err::make("request body exceeds text conversion size", Err::LIMITED) };
				return utf8_text(body.ptr(), body.size());
			}
			const VariantPair decoded = json_of(body);
			const Ref<Err> error = decoded.error;
			if (error.is_valid()) {
				return decoded;
			}
			return GDWebValid::check(decoded.value, checked_rule);
		});
		converted_signal.connect(callable_mp(this, &GDWebBodyCall::converted), Object::CONNECT_ONE_SHOT);
	}
	return false;
}

// Return worker-converted body data to main-thread request state.
void GDWebBodyCall::converted(const Variant &p_result, const Variant &p_error) {
	if (self_hold.is_null()) {
		return;
	}
	converting = false;
	if (mode == VALIDATE && p_error.get_type() == Variant::NIL && req.is_valid()) {
		req->keep("valid:" + keep_name, p_result);
		finish({ Variant(), Variant() });
		return;
	}
	finish({ p_result, p_error });
}

// Cancel a body operation whose result is no longer awaited.
void GDWebBodyCall::cancel() {
	finish({ Variant(), Err::make("request body read canceled", Err::INTERRUPTED) });
}

// Register the body-operation completion signal.
void GDWebBodyCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDWebBodyCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
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
		query_values = Url::decode_query(query_txt).value;
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
	if (!reply_headers.is_empty()) {
		reply_headers = Dictionary();
	}
	body_busy = false;
	body_cache = PackedByteArray();
	body_cached = false;
	body_saved = false;
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

// Retain middleware response metadata until the final response is sent.
void GDWebRequest::set_reply_header(const String &p_name, const String &p_value) {
	reply_headers[p_name] = p_value;
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

// Convert the retained whole body in the requested form.
VariantPair GDWebRequest::cached_body(int p_mode) const {
	if (p_mode == GDWebBodyCall::TEXT) {
		return utf8_text(body_cache.ptr(), body_cache.size());
	}
	if (p_mode == GDWebBodyCall::JSON) {
		return json_of(body_cache);
	}
	return { body_cache, Variant() };
}

// Explain why the body stream cannot start another operation, or return null.
Ref<Err> GDWebRequest::body_error() const {
	if (body_busy) return Err::make("request body is already being read", Err::ALREADY_EXISTS);
	if (srv.is_null() || !srv->has_conn(id)) return Err::make("request body is no longer available", Err::INTERRUPTED);
	return Ref<Err>();
}

// Explain why the whole body cannot be read, since a saved body is no longer in the stream.
Ref<Err> GDWebRequest::body_whole_error() const {
	if (body_saved && !body_busy) return Err::make("request body was saved to a file; read that file instead", Err::INVALID_DATA);
	return body_error();
}

// Start one body operation, preventing concurrent access to the same stream.
// A body already read whole is answered from the retained bytes.
VariantPair GDWebRequest::body_call(int p_mode, int64_t p_want, bool p_deferred) {
	if (body_cached && p_mode != GDWebBodyCall::READ && !body_busy) {
		const VariantPair result = cached_body(p_mode);
		return p_deferred ? VariantPair{ Async::ready_pair(result), Variant() } : result;
	}
	const Ref<Err> why = p_mode == GDWebBodyCall::READ ? body_error() : body_whole_error();
	if (why.is_valid()) {
		const VariantPair result{ Variant(), why };
		return p_deferred ? VariantPair{ Async::ready_pair(result), Variant() } : result;
	}
	Ref<GDWebBodyCall> call;
	call.instantiate();
	body_busy = true;
	body_active = call;
	return call->start(Ref<GDWebRequest>(this), srv, id, p_mode, p_want, String(), p_deferred);
}

// Read one body chunk from the stream.
VariantPair GDWebRequest::read(int64_t p_bytes) {
	if (p_bytes < 1) {
		return { Variant(), Err::make("request body read size must be positive", Err::INVALID_DATA) };
	}
	return body_call(GDWebBodyCall::READ, p_bytes);
}

// Read all remaining body bytes.
VariantPair GDWebRequest::bytes() {
	return body_call(GDWebBodyCall::BYTES);
}

// Defer a partial read so listeners can attach before completion.
Signal GDWebRequest::read_async(int64_t p_bytes) {
	if (p_bytes < 1) return Async::ready_pair({ Variant(), Err::make("request body read size must be positive", Err::INVALID_DATA) });
	return body_call(GDWebBodyCall::READ, p_bytes, true).value;
}

// Defer collecting the remaining body bytes.
Signal GDWebRequest::bytes_async() {
	return body_call(GDWebBodyCall::BYTES, 0, true).value;
}

// Return the body length without reading a large body into memory.
int64_t GDWebRequest::body_size() const {
	return srv->get_body_size(id);
}

// Set this request's body-reader limit.
Ref<Err> GDWebRequest::limit(int64_t p_bytes) {
	if (p_bytes < 0) return Err::make("request body limit must be zero or greater", Err::INVALID_DATA);
	if (body_busy) return Err::make("request body limit must be set before reading", Err::INVALID_DATA);
	if (srv.is_null() || !srv->set_request_body_limit(id, p_bytes)) return Err::make("request body is no longer available", Err::INTERRUPTED);
	return Ref<Err>();
}

// Save the body incrementally inside a mount.
Signal GDWebRequest::save(const String &p_path) {
	GD_PERM_FAIL_V(WRITE, p_path, Async::ready_pair({ Variant(), Err::make(vformat("cannot write %s", p_path), Err::PERMISSION_DENIED) }));
	String why;
	const String real = Mount::resolve(p_path, true, why);
	if (real.is_empty()) {
		return Async::ready_pair({ Variant(), Err::make(why.is_empty() ? vformat("cannot write %s", p_path) : why, Err::PERMISSION_DENIED) });
	}
	const bool cached = body_cached && !body_busy; // A body already read whole is written from memory.
	const Ref<Err> refused = cached ? Ref<Err>() : body_whole_error();
	if (refused.is_valid()) return Async::ready_pair({ Variant(), refused });
	Ref<GDWebBodyCall> call;
	call.instantiate();
	call->sink.instantiate();
	// Delegate file open and writes to workers, keeping file I/O off the handler event loop.
	call->sink->open(p_path, false);
	if (cached) {
		call->sink->push(body_cache);
		call->sink->close();
		call->flushing = true;
	} else {
		body_saved = true;
	}
	body_busy = true;
	body_active = call;
	return call->start(Ref<GDWebRequest>(this), srv, id, GDWebBodyCall::SAVE, 0, p_path).value;
}

// Decode the remaining body as UTF-8 text.
VariantPair GDWebRequest::text() {
	return body_call(GDWebBodyCall::TEXT);
}

// Decode the remaining body as JSON.
VariantPair GDWebRequest::json() {
	return body_call(GDWebBodyCall::JSON);
}

// Defer decoding the remaining body as text.
Signal GDWebRequest::text_async() {
	return body_call(GDWebBodyCall::TEXT, 0, true).value;
}

// Defer decoding the remaining body as JSON.
Signal GDWebRequest::json_async() {
	return body_call(GDWebBodyCall::JSON, 0, true).value;
}

// Read JSON and apply middleware validation rules.
Signal GDWebRequest::json_valid(const Dictionary &p_rule, const String &p_name) {
	// Validate a body that an earlier stage already read whole.
	if (body_cached && !body_busy) {
		const VariantPair decoded = json_of(body_cache);
		if (Ref<Err>(decoded.error).is_valid()) {
			return Async::ready_pair(decoded);
		}
		const VariantPair checked = GDWebValid::check(decoded.value, p_rule);
		if (checked.error.get_type() == Variant::NIL) {
			keep("valid:" + p_name, checked.value);
			return Async::ready_pair({ Variant(), Variant() });
		}
		return Async::ready_pair(checked);
	}
	const Ref<Err> why = body_whole_error();
	if (why.is_valid()) return Async::ready_pair({ Variant(), why });
	Ref<GDWebBodyCall> call;
	call.instantiate();
	call->rule = p_rule;
	call->keep_name = p_name;
	body_busy = true;
	body_active = call;
	return call->start(Ref<GDWebRequest>(this), srv, id, GDWebBodyCall::VALIDATE, 0, String()).value;
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
	ClassDB::bind_method(D_METHOD("save", "path"), &GDWebRequest::save);
	ClassDB::bind_method(D_METHOD("text"), &GDWebRequest::text);
	ClassDB::bind_method(D_METHOD("json"), &GDWebRequest::json);
	ClassDB::bind_method(D_METHOD("read_async", "bytes"), &GDWebRequest::read_async, DEFVAL(32768));
	ClassDB::bind_method(D_METHOD("bytes_async"), &GDWebRequest::bytes_async);
	ClassDB::bind_method(D_METHOD("save_async", "path"), &GDWebRequest::save);
	ClassDB::bind_method(D_METHOD("text_async"), &GDWebRequest::text_async);
	ClassDB::bind_method(D_METHOD("json_async"), &GDWebRequest::json_async);
	ADD_AWAIT("read", "Pair:PackedByteArray");
	ADD_AWAIT("bytes", "Pair:PackedByteArray");
	ADD_AWAIT("save", "Pair:int");
	ADD_AWAIT("text", "Pair:String");
	ADD_AWAIT("json", "Pair:Variant");
	ADD_AWAIT("read_async", "Pair:PackedByteArray");
	ADD_AWAIT("bytes_async", "Pair:PackedByteArray");
	ADD_AWAIT("save_async", "Pair:int");
	ADD_AWAIT("text_async", "Pair:String");
	ADD_AWAIT("json_async", "Pair:Variant");
	ADD_PAIR_RESULT("read", "PackedByteArray");
	ADD_PAIR_RESULT("bytes", "PackedByteArray");
	ADD_PAIR_RESULT("text", "String");
	ADD_PAIR_RESULT("json", "Variant");
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
	GDPairCall::start([p_path]() { return Os::read_bytes(p_path); }, false).connect(callable_mp(call.ptr(), &GDWebViewCall::loaded), Object::CONNECT_ONE_SHOT);
	return signal;
}

// Pass a loaded template to the default CPU renderer or a custom renderer.
void GDWebViewCall::loaded(const Variant &p_value, const Variant &p_error) {
	if (self_hold.is_null()) {
		return;
	}
	if (Ref<Err>(p_error).is_valid()) {
		finish({ Variant(), Ref<Err>(p_error)->with_partial(p_value) });
		return;
	}
	const PackedByteArray bytes = p_value;
	if (!renderer.is_valid()) {
		if (!build) {
			build = std::make_shared<HtmlBuild>();
		}
		GDValueCall::start([build = build, bytes, data = data, status = status]() -> Variant {
			const VariantPair decoded = utf8_text(bytes.ptr(), bytes.size());
			if (decoded.error.get_type() != Variant::NIL) return decoded.error;
			Error err = build->step(decoded.value);
			if (err == ERR_BUSY) {
				return build->needed();
			}
			String body;
			if (err == OK) err = build->render(data, body);
			if (err != OK) return build->failure(err);
			return Http::html(body, status);
		}).connect(callable_mp(this, &GDWebViewCall::prepared), Object::CONNECT_ONE_SHOT);
		return;
	}
	GDValueCall::start([bytes]() -> Variant {
		const VariantPair decoded = utf8_text(bytes.ptr(), bytes.size());
		return decoded.error.get_type() == Variant::NIL ? decoded.value : decoded.error;
	}).connect(callable_mp(this, &GDWebViewCall::custom_ready), Object::CONNECT_ONE_SHOT);
}

// Queue only missing partials for I/O, retaining paused parsing state for CPU work.
void GDWebViewCall::prepared(const Variant &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	if (p_result.get_type() != Variant::STRING) {
		if (Ref<GDWebResponse>(p_result).is_valid()) {
			finish({ p_result, Variant() });
			return;
		}
		const Ref<Err> error = p_result;
		finish({ Variant(), error.is_valid() ? Variant(error) : Variant(Err::make("invalid template build result", Err::INVALID_DATA)) });
		return;
	}
	part = p_result;
	const String root = path.get_base_dir();
	const String name = part;
	GDPairCall::start([root, name]() -> VariantPair {
		if (!Html::partial_ok(name)) {
			return { Variant(), Err::make("invalid partial name", Err::INVALID_DATA) };
		}
		const String file = Path::under(root.path_join("partials"), name + ".html");
		return file.is_empty() ? VariantPair{ Variant(), Err::make("invalid partial path", Err::INVALID_DATA) } : Os::read_bytes(file);
	}, false).connect(callable_mp(this, &GDWebViewCall::loaded), Object::CONNECT_ONE_SHOT);
}

// After source conversion, run only custom rendering on the script scheduler.
void GDWebViewCall::custom_ready(const Variant &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDWebViewCall> keep(this);
	if (p_result.get_type() != Variant::STRING) {
		const Ref<Err> error = p_result;
		finish({ Variant(), error.is_valid() ? Variant(error) : Variant(Err::make("invalid template source result", Err::INVALID_DATA)) });
		return;
	}
	Variant args[3] = { p_result, data, path };
	const Variant *argv[3] = { &args[0], &args[1], &args[2] };
	Variant out;
	Variant render_error;
	Callable::CallError err;
	err.result_error = &render_error;
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
			state->cancel_awaited();
		}
		return;
	}
	if (err.error != Callable::CallError::CALL_OK || err.runtime_failed) {
		finish({ Variant(), Err::make("template renderer failed", Err::INVALID_DATA) });
		return;
	}
	GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(out.get_validated_object());
	if (state) {
		continuation = Ref<RefCounted>(state);
		state->connect("completed", state->is_pair_return() ? callable_mp(this, &GDWebViewCall::rendered_pair) : callable_mp(this, &GDWebViewCall::rendered), Object::CONNECT_ONE_SHOT);
		return;
	}
	rendered_pair(out, render_error);
}

// Convert rendered text into an HTML reply.
void GDWebViewCall::rendered(const Variant &p_result) {
	rendered_pair(p_result, Variant());
}

// Convert both renderer return slots into an HTML reply.
void GDWebViewCall::rendered_pair(const Variant &p_result, const Variant &p_error) {
	if (self_hold.is_null()) {
		return;
	}
	continuation.unref();
	const Ref<Err> why = Ref<Err>(p_error).is_valid() ? Ref<Err>(p_error) : Ref<Err>(p_result);
	if (why.is_valid()) { finish({ Variant(), why }); return; }
	if (p_result.get_type() != Variant::STRING) {
		finish({ Variant(), Err::make("template renderer must return text", Err::INVALID_DATA) });
		return;
	}
	finish({ Http::html(p_result, status), Variant() });
}

// Return the template reply exactly once and release retained references.
void GDWebViewCall::finish(const VariantPair &p_result) {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDWebViewCall> keep(this);
	renderer = Callable();
	data = Dictionary();
	build.reset();
	self_hold.unref();
	Async::finish(this, SNAME("finished"), p_result.value, p_result.error);
}

// Stop a custom renderer continuation when its request disappears.
void GDWebViewCall::cancel() {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDWebViewCall> keep(this);
	GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(continuation.ptr());
	if (state) {
		state->cancel_awaited();
	}
	continuation.unref();
	finish({ Variant(), Err::make("template rendering canceled", Err::INTERRUPTED) });
}

// Register completion and cancellation.
void GDWebViewCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDWebViewCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "reply", PROPERTY_HINT_RESOURCE_TYPE, "GDWebResponse"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
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
VariantPair GDWebApp::listen(int64_t p_port, const String &p_host) {
	if (opening.is_valid() || srv.is_valid()) return { Variant(), Err::make("server already started", Err::ALREADY_EXISTS) };
	return listen_at(p_port, p_host, Ref<GDTLSIdentity>());
}

// Apply identical HTTP limits and scheduling to plain and encrypted listeners.
VariantPair GDWebApp::listen_at(int64_t p_port, const String &p_host, const Ref<GDTLSIdentity> &p_identity) {
	if (config_error.is_valid()) return { Variant(), config_error };
	if (shutting) {
		return { Variant(), Err::make("server is shutting down", Err::ALREADY_EXISTS) };
	}
	srv.instantiate();
	srv->set_identity(p_identity);
	srv->set_body_limit(max_body);
	srv->set_header_limits(header_bytes, header_values);
	srv->set_header_timeout(head_seconds);
	srv->set_body_timeout(body_seconds);
	const VariantPair opened = srv->listen(p_port, p_host);
	if (Ref<Err>(opened.error).is_valid()) {
		srv.unref();
		return opened;
	}
	root_ctx = Async::ctx();
	// Dispatch only kernel-ready connections to the runtime serve task.
	srv->set_ready_callback(callable_mp(this, &GDWebApp::poll));
	return {};
}

// Return the application's actual listen port.
int GDWebApp::port() const {
	return srv.is_valid() ? srv->get_port() : 0;
}

// Stop new accepts and keepalive reuse while retaining active requests.
Signal GDWebApp::shutdown(const Ref<GDAsyncContext> &p_ctx) {
	if (p_ctx.is_null()) {
		return Async::ready_pair({ Variant(), Err::make("shutdown needs a context", Err::INVALID_DATA) });
	}
	if (shutdown_wait.is_valid()) {
		return Async::ready_pair({ Variant(), Err::make("shutdown is already waiting", Err::ALREADY_EXISTS) });
	}
	if (srv.is_null()) {
		opening.unref();
		return Async::ready_pair({ Variant(), Variant() });
	}
	shutting = true;
	shutdown_ctx = p_ctx;
	shutdown_wait.instantiate();
	shutdown_wait->self_hold = shutdown_wait;
	p_ctx->connect("canceled", callable_mp(this, &GDWebApp::poll), Object::CONNECT_ONE_SHOT);
	srv->begin_shutdown();
	post_poll(); // With only idle connections, finish immediately on the next runtime task.
	return Signal(shutdown_wait.ptr(), "finished_pair");
}

// Deliver one graceful-shutdown result and release its deadline.
void GDWebApp::finish_shutdown(const VariantPair &p_result) {
	if (shutdown_wait.is_valid()) {
		Ref<GDWait> wait = shutdown_wait;
		shutdown_wait.unref();
		shutdown_ctx.unref();
		wait->done(p_result.value, &p_result.error);
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
		finish_shutdown({ Variant(), Variant() });
		return;
	}
	if (shutdown_wait.is_valid() && shutdown_ctx.is_valid() && shutdown_ctx->is_done()) {
		finish_shutdown({ Variant(), shutdown_ctx->get_reason() });
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
	finish_shutdown({ Variant(), Err::make("server stopped", Err::INTERRUPTED) });
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
			// Stop suspended work when its connection can no longer carry a response.
			const Ref<GDAsyncContext> context = waiting->value.req->get_context();
			if (waiting->value.stage != ENCODE && !srv->request_alive(id, context)) {
				Job dead = waiting->value;
				drop_job_time(id, dead);
				jobs.erase(id);
				cancel_job(dead);
				srv->abort_request(id);
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
			run(id, job, { Variant(), Variant() }, false, until);
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
bool GDWebApp::park(const Variant &p_value, int p_id, Job &p_job) {
	const Variant ret = GDTask::as_signal(p_value); // A handler may return a started task.
	const bool signal_wait = ret.get_type() == Variant::SIGNAL;
	Object *obj = signal_wait ? nullptr : ret.get_type() == Variant::OBJECT ? ret.get_validated_object() : nullptr;
	if (!signal_wait && (!obj || !obj->is_class("GDScriptFunctionState"))) {
		return false;
	}
	if (p_job.stage != ENCODE && job_max > 0 && awaiting >= job_max && !jobs.has(p_id)) {
		// Reject additional suspension when the configured pending-work capacity is exhausted.
		if (signal_wait) {
			const Signal signal = ret;
			Object *owner = signal.get_object();
			if (owner && owner->has_method("cancel")) {
				owner->call("cancel");
			}
		} else {
			GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(obj);
			if (state) {
				state->cancel_awaited();
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
		p_job.wait_signal = ret;
		p_job.hold = Ref<RefCounted>(Object::cast_to<RefCounted>(p_job.wait_signal.get_object()));
		if (p_job.stage == ENCODE) utf8_watch(ret, callable_mp(srv.ptr(), &GDWebServer::has_request).bind(p_id));
	} else {
		p_job.hold = Ref<RefCounted>(Object::cast_to<RefCounted>(obj));
		p_job.wait_signal = Signal(obj, "completed");
	}
	Object *signal_owner = p_job.wait_signal.get_object();
	const GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(signal_owner);
	p_job.pair_pending = state && state->is_pair_return();
	if (!p_job.pair_pending && signal_owner) {
		List<MethodInfo> signals;
		signal_owner->get_signal_list(&signals);
		for (const MethodInfo &info : signals) {
			if (info.name != p_job.wait_signal.get_name()) continue;
			p_job.pair_pending = info.arguments.size() == 2 && info.arguments[1].type == Variant::OBJECT && info.arguments[1].hint_string == "Err";
			break;
		}
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
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWebApp::resumed_unavailable).bind(p_id));
		return true;
	}
	// Keep a half-closed request writable while observing full disconnects.
	const Ref<GDAsyncContext> context = p_job.req->get_context();
	if (p_job.stage != ENCODE && !srv->request_alive(p_id, context)) {
		Job dead = jobs[p_id];
		drop_job_time(p_id, dead);
		jobs.erase(p_id);
		cancel_job(dead);
		srv->abort_request(p_id);
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
		state->cancel_awaited();
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

// Send a generic failure before headers, then release the faulted request's work.
void GDWebApp::fault_job(int p_id, Job &p_job) {
	ERR_PRINT("web handler stopped after a script runtime fault");
	if (srv.is_valid()) {
		if (p_job.stage != ENCODE && srv->has_request(p_id)) srv->respond(p_id, 500, String("internal error").to_utf8_buffer(), TEXT_TYPE);
		else srv->abort_request(p_id);
	}
	cancel_job(p_job);
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
		const int id = *p_args[p_count - 1];
		const auto job = jobs.find(id);
		if (job && job->value.pair_pending) {
			resumed(p_count == 3 ? VariantPair{ *p_args[0], *p_args[1] } : VariantPair{ Variant(), Err::make("callback returned an invalid result pair", Err::INVALID_DATA) }, id);
		} else {
			resumed({ Async::signal_value(p_args, p_count - 1), Variant() }, id);
		}
	}
	return Variant();
}

// Report an unavailable signal through the ordinary request error path.
void GDWebApp::resumed_unavailable(int p_id) {
	resumed({ Variant(), Err::make("handler returned an unavailable signal", Err::INVALID_DATA) }, p_id);
}

// Resume request processing with an asynchronous result.
void GDWebApp::resumed(const VariantPair &p_result, int p_id) {
	HashMap<int, Job>::Iterator it = jobs.find(p_id);
	if (!it) {
		return;
	}
	Job job = it->value;
	drop_job_time(p_id, job);
	jobs.erase(p_id);
	arm_jobs();
	GDScriptFunctionState *state = Object::cast_to<GDScriptFunctionState>(job.hold.ptr());
	const bool faulted = state && state->is_runtime_faulted();
	job.wait_signal = Signal();
	job.wait_call = Callable();
	if (faulted) {
		fault_job(p_id, job);
		if (shutting) post_poll();
		return;
	}
	job.hold.unref();
	if (job.stage == ENCODE && (srv.is_null() || !srv->has_request(p_id))) {
		cancel_job(job);
		if (shutting) post_poll();
		return;
	}
	busy++;
	current_id = p_id;
	// A completed coroutine may return another operation that must finish first.
	if (p_result.error.get_type() != Variant::NIL || !park(p_result.value, p_id, job)) {
		run(p_id, job, p_result, true, GDClock::usec() + GD_SCHED_SLICE_USEC);
	}
	current_id = -1;
	leave();
	if (shutting) {
		post_poll(); // Reclaim closed connections after the final asynchronous handler completes.
	}
}

// Execute request middleware and handlers in order.
void GDWebApp::run(int p_id, Job &p_job, const VariantPair &p_back, bool p_resumed, uint64_t p_until) {
	const bool sliced = GDScriptFunction::begin_time_slice(p_until);
	const uint64_t outer = GDScriptFunction::native_time_slice_deadline();
	if (outer) p_until = p_until ? MIN(p_until, outer) : outer;
	if (p_resumed) {
		p_job.pair_pending = false;
		step(p_job, p_back);
	}

	// Advance the phase position after each call and return immediately on suspension.
	while (p_job.stage != DONE) {
		VariantPair ret;
		bool called = false;
		bool faulted = false;

		switch (p_job.stage) {
			case PRE: {
				if (p_job.at < (int)pres.size()) {
					ret = web_call(faulted, pres[p_job.at].fn, p_job.req);
					called = true;
				} else {
					pick(p_job);
				}
			} break;

			case STATIC: {
				ret = { pick_static(p_job), Variant() };
				called = ret.value.get_type() == Variant::SIGNAL;
			} break;

			case USE: {
				if (p_job.at < (int)uses.size()) {
					ret = web_call(faulted, uses[p_job.at].fn, p_job.req);
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
					ret = web_call(faulted, mid[p_job.at].fn, p_job.req);
					called = true;
				} else {
					p_job.stage = ROUTE;
					p_job.at = 0;
				}
			} break;

			case ROUTE: {
				if (p_job.mids >= 0 && p_job.at < (int)route_mids[p_job.mids].size()) {
					ret = web_call(faulted, route_mids[p_job.mids][p_job.at].fn, p_job.req);
					called = true;
				} else {
					p_job.stage = HANDLE;
					p_job.at = 0;
				}
			} break;

			case HANDLE: {
				if (p_job.handler.is_valid()) {
					ret = web_call(faulted, p_job.handler, p_job.req);
					p_job.handler = Callable();
					called = true;
				} else {
					p_job.stage = FAIL;
					p_job.at = 0;
				}
			} break;

			case FAIL: {
				const Ref<Err> why = p_job.at == 0 ? Ref<Err>(p_job.out) : Ref<Err>();
				if (why.is_valid()) {
					if (on_fail.is_valid()) {
						ret = web_call(faulted, on_fail, p_job.req, why);
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
				}
				p_job.stage = AFTER;
				p_job.at = p_job.after_at;
			} break;

			case AFTER: {
				if (p_job.at < (int)afters.size()) {
					ret = web_call(faulted, afters[p_job.at], p_job.req, p_job.out);
					called = true;
				} else {
					p_job.stage = ENCODE;
				}
			} break;

			case ENCODE: {
				// Reject unsent response failures before selecting an encoding path.
				const Ref<Err> error = p_job.out;
				if (error.is_valid()) { failed(p_job, error); break; }
				const Ref<GDWebResponse> native = p_job.out;
				if (p_job.out.get_type() == Variant::OBJECT && native.is_null()) {
					failed(p_job, Err::make("response must be data or an envelope", Err::INVALID_DATA));
					break;
				}
				// Check wrapped object bodies before choosing an encoding path.
				if (p_job.out.get_type() == Variant::DICTIONARY || native.is_valid()) {
					static const Dictionary empty; // No per-reply dictionary for native responses.
					Dictionary response = native.is_valid() ? empty : Dictionary(p_job.out);
					const Variant *body = native.is_valid() ? &native->body_value() : response.getptr(SNAME("body"));
					if (body && body->get_type() == Variant::OBJECT) {
						const Variant value = *body;
						const Ref<Err> error = value;
						if (error.is_valid()) { failed(p_job, error); break; }
						if (Ref<GDBodySource>(value).is_null()) {
							failed(p_job, Err::make("response body must be data or a byte source", Err::INVALID_DATA));
							break;
						}
					}
				}
				const Variant value = p_job.out;
				// Borrow body storage while the retained envelope is unchanged in this turn.
				const Variant *content = nullptr;
				if (native.is_valid()) {
					content = &native->body_value();
				} else if (value.get_type() == Variant::DICTIONARY) {
					const Dictionary response = value;
					content = response.getptr(SNAME("body"));
				}
				// Measure omitted text only after middleware has finalized its representation.
				if (srv->is_head(p_id)) {
					const Variant &text = content ? *content : value;
					if (text.get_type() == Variant::STRING || text.get_type() == Variant::STRING_NAME) {
						p_job.head_text = true;
						ret = { utf8_size(text, p_until), Variant() };
						called = true;
						break;
					}
				}
				// Scalar text does not need response-envelope lookups.
				if (value.get_type() == Variant::STRING || value.get_type() == Variant::STRING_NAME) {
					p_job.encoded_type = HTTP_TYPE_TEXT;
					ret = { utf8_reply(value, p_until, [](const PackedByteArray &p_bytes) -> Variant { return p_bytes; }), Variant() };
					called = true;
					break;
				}
				const bool raw_json = value.get_type() == Variant::DICTIONARY && !content;
				const bool text_body = content && content->get_type() != Variant::PACKED_BYTE_ARRAY && content->get_type() != Variant::OBJECT;
				const bool plain_value = native.is_null() && value.get_type() != Variant::DICTIONARY && value.get_type() != Variant::PACKED_BYTE_ARRAY && value.get_type() != Variant::NIL;
				if (!raw_json && !text_body && !plain_value) {
					p_job.stage = DONE;
					break;
				}
				// Keep resumable conversion local until its time slice requires a worker continuation.
				if (raw_json) {
					p_job.encoded_type = HTTP_TYPE_JSON;
					ret = JsonData::encode_reply(value, p_until);
					called = true;
					break;
				}
				// Encode response text with the same resumable path as scalar text.
				if (text_body && (content->get_type() == Variant::STRING || content->get_type() == Variant::STRING_NAME)) {
					p_job.body_encoded = true;
					ret = { utf8_reply(*content, p_until, [](const PackedByteArray &p_bytes) -> Variant { return p_bytes; }), Variant() };
					called = true;
					break;
				}
				// User callbacks and non-resumable formatting retain their worker context.
				p_job.body_encoded = text_body;
				p_job.encoded_type = HTTP_TYPE_TEXT;
				const Variant body = text_body ? *content : value;
				ret = { GDValueCall::start([body]() -> Variant { return Pool::text(body).to_utf8_buffer(); }), Variant() };
				called = true;
			} break;
		}

		if (faulted) {
			fault_job(p_id, p_job);
			if (sliced) GDScriptFunction::end_time_slice();
			return;
		}
		if (!called) {
			continue;
		}
		if (ret.error.get_type() == Variant::NIL && park(ret.value, p_id, p_job)) {
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
	p_job.encoded_type = HTTP_TYPE_CUSTOM;
	if (p_job.failing) {
		const Ref<Err> combined = Err::join(p_job.prior_error, p_error);
		ERR_PRINT(vformat("error response failed: %s", LogState::flat(combined->text())));
		p_job.out = Http::bytes_out(String("Internal Server Error").to_utf8_buffer(), TEXT_TYPE, 500);
		p_job.stage = DONE;
		return;
	}
	p_job.after_at = p_job.stage == AFTER ? p_job.at + 1 : p_job.stage == ENCODE ? afters.size() : 0;
	p_job.failing = true;
	p_job.prior_error = p_error;
	p_job.out = p_error;
	p_job.stage = FAIL;
	p_job.at = 0;
}

// Advance the asynchronous processing state by one phase.
void GDWebApp::step(Job &p_job, const VariantPair &p_result) {
	const Variant &p_ret = p_result.value;
	Ref<Err> result_error = p_result.error;
	if (result_error.is_null()) result_error = Ref<Err>(p_ret);
	if (p_job.stage != STATIC) {
		if (result_error.is_valid()) { failed(p_job, result_error); return; }
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
			if (p_job.file.is_valid()) {
				const Ref<Err> error = result_error;
				if (error.is_valid()) {
					p_job.file->abort();
					p_job.file.unref();
					if (!error->is(Err::NOT_FOUND)) failed(p_job, error);
					return;
				}
				const HttpType kind = http_type_of(p_job.file_type);
				p_job.out = GDWebResponse::make(200, kind, p_job.file, kind == HTTP_TYPE_CUSTOM ? p_job.file_type : String());
				p_job.ready = true;
				p_job.stage = USE;
				p_job.at = 0;
				break;
			}
			const Ref<Err> error = result_error;
			if (error.is_valid()) {
				if (!error->is(Err::NOT_FOUND)) failed(p_job, error);
				return; // Missing files may be supplied by another static root.
			}
			const Ref<GDWebResponse> served = p_ret;
			if (served.is_null()) {
				return; // Inspect the next static root.
			}
			p_job.out = served;
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
	// Prefer an explicit HEAD route; GET supplies its metadata when no HEAD route exists.
	auto select = [&](const String &p_method) -> bool {
		const auto hit = exact.find(req->route_txt);
		if (hit) {
			for (const Slot &slot : hit->value) {
				if (slot.method != p_method) continue;
				p_job.handler = slot.handler;
				p_job.band = slot.group;
				p_job.mids = slot.mids;
				p_job.stage = USE;
				p_job.at = 0;
				return true;
			}
		}
		if (!routes.is_empty() && req->path_parts.is_empty()) {
			req->path_parts = req->path_txt.split("/", true); // Split only when named routes can use the segments.
		}
		for (const Route &route : routes) {
			if (route.method != p_method || !match(route, req->path_parts, req)) continue;
			p_job.handler = route.handler;
			p_job.band = route.group;
			p_job.mids = route.mids;
			p_job.stage = USE;
			p_job.at = 0;
			return true;
		}
		return false;
	};
	const String method = req->get_method();
	if (select(method) || (method == "HEAD" && select("GET"))) return;

	// If no route matches, inspect static files on a worker.
	p_job.static_at = 0;
	p_job.stage = STATIC;
}

// Inspect the next static root and return its file-open completion signal.
Variant GDWebApp::pick_static(Job &p_job) {
	GDWebRequest *req = p_job.req.ptr();
	const String method = req->get_method();
	// Collect route methods only when a request has missed dispatch.
	auto route_methods = [&]() {
		PackedStringArray allowed;
		auto add = [&allowed](const String &p_method) {
			if (!allowed.has(p_method)) allowed.push_back(p_method);
		};
		const auto hit = exact.find(req->route_txt);
		if (hit) for (const Slot &slot : hit->value) add(slot.method);
		for (const Route &route : routes) {
			if (match(route, req->path_parts, nullptr)) add(route.method);
		}
		if (allowed.has("GET")) add("HEAD");
		return allowed;
	};
	for (; p_job.static_at < (int)statics.size(); p_job.static_at++) {
		const Pair<String, String> &st = statics[p_job.static_at];
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
		if (method != "GET" && method != "HEAD") {
			// Inspect only the matching file; absent paths still resolve to 404.
			PackedStringArray allowed = route_methods();
			if (!allowed.has("GET")) allowed.push_back("GET");
			if (!allowed.has("HEAD")) allowed.push_back("HEAD");
			allowed.sort();
			return GDPairCall::start([full, allowed]() -> VariantPair {
				if (!GDFile::exists(full)) return { Dictionary(), Variant() };
				return { Http::head(Http::text("method not allowed", 405), "Allow", String(", ").join(allowed)), Variant() };
			});
		}
		if (!afters.is_empty()) {
			// Build the reply dictionary expected by postprocessors while offloading file I/O.
			return GDPairCall::start([full]() { return file_reply(full); }, false);
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
	// Identify methods for a known route only after static lookup has missed.
	PackedStringArray allowed = route_methods();
	if (!allowed.is_empty()) {
		allowed.sort();
		p_job.out = Http::head(Http::text("method not allowed", 405), "Allow", String(", ").join(allowed));
		p_job.ready = true;
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
			if (p_req) got[part.substr(1)] = p_target[i];
		} else if (part != p_target[i]) {
			return false;
		}
	}
	if (p_req) p_req->params_map = got;
	return true;
}

// Read a file on the I/O queue and preserve its original result.
Signal GDWebApp::file_at(const String &p_path) const {
	return GDPairCall::start([p_path]() { return file_reply(p_path); }, false);
}

// Complete processing and release request resources.
void GDWebApp::finish(int p_id, Job &p_job) {
	const Dictionary &reply_headers = p_job.req->reply_headers;
	// Preserve the fixed-response path when middleware supplied no headers.
	auto send_fixed = [&](int p_status, const PackedByteArray &p_body, HttpType p_type) {
		if (reply_headers.is_empty()) {
			srv->respond_fixed(p_id, p_status, p_body, p_type);
		} else {
			Dictionary headers = reply_headers.duplicate();
			headers[SNAME("Content-Type")] = String(http_type_bytes(p_type));
			srv->respond_with(p_id, p_status, headers, p_body);
		}
	};
	// Do not stringify unsupported objects into public responses; retain details only in logs.
	const Ref<GDWebResponse> native = p_job.out;
	if (p_job.out.get_type() == Variant::OBJECT && native.is_null()) {
		const Ref<Err> why = p_job.out;
		const String detail = why.is_valid() ? why->text() : String("returned an object");
		ERR_PRINT(vformat("handler failed: %s", LogState::flat(detail)));
		if (p_job.file.is_valid()) {
			p_job.file->abort();
			p_job.file.unref();
		}
		send_fixed(500, String("internal error").to_utf8_buffer(), HTTP_TYPE_TEXT);
		p_job.req->finish_context("request finished");
		if (p_job.req->get_reference_count() == 1) {
			spare = p_job.req;
		}
		return;
	}
	const Variant &p_out = p_job.out;
	bool streaming = false;
	if (p_out.get_type() == Variant::DICTIONARY || native.is_valid()) {
		static const Dictionary empty; // No per-reply dictionary for native responses.
		const Dictionary out = native.is_valid() ? empty : Dictionary(p_out);
		const Variant *content = native.is_valid() ? &native->body_value() : out.getptr(SNAME("body"));
		bool sent = false;
		if (content) {
			// Read final metadata after encoding, preserving shared-envelope updates during await.
			const Variant *status_value = native.is_valid() ? nullptr : out.getptr(SNAME("status"));
			const Variant *header_value = native.is_valid() ? nullptr : out.getptr(SNAME("headers"));
			const Variant *type_value = native.is_valid() ? nullptr : out.getptr(SNAME("type"));
			const int status = native.is_valid() ? safe_status(native->get_status()) : status_value ? safe_status(*status_value) : 200;
			const HttpType kind = native.is_valid() ? native->get_type() : HTTP_TYPE_CUSTOM;
			static const String text_type(TEXT_TYPE); // Share the default type across replies.
			const String custom_type = native.is_valid() ? native->get_custom_type() : type_value ? String(*type_value) : text_type;
			const Variant body = p_job.body_encoded ? p_job.body : *content;
			const Ref<GDBodySource> source = body;
			if (header_value || (native.is_valid() && native->has_extra()) || !reply_headers.is_empty()) {
				// Borrow existing headers without allocating an empty dictionary first.
				Dictionary headers = native.is_valid() && native->has_extra() ? *native->extra_headers() : header_value ? Dictionary(*header_value) : Dictionary();
				if (!reply_headers.is_empty()) {
					headers = headers.duplicate();
					for (const Variant &key : reply_headers.keys()) {
						if (!find_header(headers, String(key))) headers[key] = reply_headers[key];
					}
				}
				if ((type_value || native.is_valid()) && !find_header(headers, "Content-Type")) {
					const String type_name = kind == HTTP_TYPE_CUSTOM ? custom_type : String(http_type_bytes(kind));
					if (!type_name.is_empty()) {
						headers = headers.duplicate();
						headers[SNAME("Content-Type")] = type_name;
					}
				}
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
			} else if (source.is_valid()) {
				streaming = source->watch_disconnect();
				if (streaming) source->set_context(p_job.req->get_context());
				if (kind != HTTP_TYPE_CUSTOM) srv->respond_file_fixed(p_id, status, source, kind);
				else srv->respond_file_plain(p_id, status, source, custom_type);
				p_job.file.unref();
				sent = true;
			} else if (body.get_type() == Variant::PACKED_BYTE_ARRAY) {
				if (kind != HTTP_TYPE_CUSTOM) srv->respond_fixed(p_id, status, body, kind);
				else srv->respond(p_id, status, body, custom_type);
				sent = true;
			}
		}
		if (!sent) {
			send_fixed(500, String("invalid response").to_utf8_buffer(), HTTP_TYPE_TEXT);
		}
	} else if (p_job.head_text && p_job.body_encoded) {
		if (reply_headers.is_empty()) {
			srv->respond_file_fixed(p_id, 200, p_job.body, HTTP_TYPE_TEXT);
		} else {
			Dictionary headers = reply_headers.duplicate();
			headers[SNAME("Content-Type")] = String(http_type_bytes(HTTP_TYPE_TEXT));
			srv->respond_file(p_id, 200, headers, p_job.body, String());
		}
	} else if (p_out.get_type() == Variant::PACKED_BYTE_ARRAY) {
		send_fixed(200, p_out, p_job.encoded_type == HTTP_TYPE_CUSTOM ? HTTP_TYPE_BYTES : p_job.encoded_type);
	} else if (p_out.get_type() == Variant::NIL) {
		send_fixed(204, PackedByteArray(), HTTP_TYPE_TEXT); // No response body.
	} else {
		send_fixed(500, String("invalid response").to_utf8_buffer(), HTTP_TYPE_TEXT);
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
	ADD_AWAIT("file_at_async", "Pair:GDWebResponse");
	ADD_AWAIT("file_at", "Pair:GDWebResponse");
	ADD_AUTO_WAIT("file_at");
	ClassDB::bind_method(D_METHOD("listen", "port", "host"), &GDWebApp::listen, DEFVAL("127.0.0.1"));
	ClassDB::bind_method(D_METHOD("listen_tls", "port", "cert", "key", "host", "opts"), &GDWebApp::listen_tls, DEFVAL("127.0.0.1"), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("listen_tls_async", "port", "cert", "key", "host", "opts"), &GDWebApp::listen_tls, DEFVAL("127.0.0.1"), DEFVAL(Dictionary()));
	ADD_AWAIT("listen_tls", "Pair:Variant");
	ADD_AWAIT("listen_tls_async", "Pair:Variant");
	ADD_AUTO_WAIT("listen_tls");
	ADD_PAIR_RESULT("listen", "Variant");
	ClassDB::bind_method(D_METHOD("port"), &GDWebApp::port);
	ClassDB::bind_method(D_METHOD("shutdown", "context"), &GDWebApp::shutdown);
	ClassDB::bind_method(D_METHOD("shutdown_async", "context"), &GDWebApp::shutdown);
	ADD_AWAIT("shutdown", "Pair:Variant");
	ADD_AWAIT("shutdown_async", "Pair:Variant");
	ADD_AUTO_WAIT("shutdown");
	ClassDB::bind_method(D_METHOD("stop"), &GDWebApp::stop);
	ClassDB::bind_method(D_METHOD("is_listening"), &GDWebApp::is_listening);
	ClassDB::bind_method(D_METHOD("serve_error"), &GDWebApp::serve_error);
	ClassDB::bind_method(D_METHOD("poll"), &GDWebApp::poll);
}

// ---------------- Response constructors ----------------

// Construct a response with numeric metadata and no header dictionary.
Ref<GDWebResponse> GDWebResponse::make(int64_t p_status, HttpType p_type, const Variant &p_body, const String &p_custom) {
	Ref<GDWebResponse> out;
	out.instantiate();
	out->status = safe_status(p_status);
	out->type = p_type;
	out->custom_type = p_custom;
	out->body = p_body;
	return out;
}

// Keep mutable status values within the transport's valid range.
void GDWebResponse::set_status(int64_t p_status) {
	status = safe_status(p_status);
}

// Expose a content type only when script code inspects response metadata.
String GDWebResponse::type_name() const {
	if (extra) {
		if (const Variant *override = find_header(*extra, "Content-Type")) return String(*override);
	}
	return type == HTTP_TYPE_CUSTOM ? custom_type : String(http_type_bytes(type));
}

// Materialize headers only for explicit inspection or modification.
Dictionary GDWebResponse::get_headers() const {
	Dictionary headers = extra ? extra->duplicate(true) : Dictionary();
	if (!find_header(headers, "Content-Type") && (type != HTTP_TYPE_CUSTOM || !custom_type.is_empty())) headers["Content-Type"] = type_name();
	return headers;
}

// Replace optional header storage while preserving numeric default metadata.
void GDWebResponse::set_headers(const Dictionary &p_headers) {
	extra = p_headers.is_empty() ? nullptr : std::make_unique<Dictionary>(p_headers.duplicate(true));
}

// Copy response metadata before changing a single header.
Ref<GDWebResponse> GDWebResponse::with_header(const String &p_name, const Variant &p_value, bool p_append) const {
	Ref<GDWebResponse> out = make(status, type, body, custom_type);
	out->extra = std::make_unique<Dictionary>(extra ? extra->duplicate(true) : Dictionary());
	const Variant *prior = find_header(*out->extra, p_name);
	if (p_append && prior) {
		Array values;
		if (prior->get_type() == Variant::ARRAY) values = Array(*prior).duplicate();
		else values.push_back(*prior);
		values.push_back(p_value);
		for (const Variant &key : out->extra->keys()) if (String(key).nocasecmp_to(p_name) == 0) out->extra->erase(key);
		(*out->extra)[p_name] = values;
		return out;
	}
	for (const Variant &key : out->extra->keys()) if (String(key).nocasecmp_to(p_name) == 0) out->extra->erase(key);
	(*out->extra)[p_name] = p_value;
	return out;
}

// Add protective headers without replacing explicit caller values.
Ref<GDWebResponse> GDWebResponse::guarded() const {
	static const char *pairs[][2] = {
		{ "X-Content-Type-Options", "nosniff" }, // Reject content sniffing.
		{ "X-Frame-Options", "DENY" }, // Block embedding in frames.
		{ "Referrer-Policy", "no-referrer" }, // Omit referrer details.
		{ "Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'" }, // Restrict content origins.
		{ "Strict-Transport-Security", "max-age=15552000; includeSubDomains" }, // Retain HTTPS preference.
		{ "Permissions-Policy", "geolocation=(), microphone=(), camera=()" }, // Disable browser features.
		{ "Cross-Origin-Opener-Policy", "same-origin" }, // Isolate opener access.
		{ "Cross-Origin-Resource-Policy", "same-origin" }, // Restrict resource reuse.
		{ "X-Permitted-Cross-Domain-Policies", "none" }, // Reject legacy policy files.
	};
	Ref<GDWebResponse> out = make(status, type, body, custom_type);
	out->extra = std::make_unique<Dictionary>(extra ? extra->duplicate(true) : Dictionary());
	for (const char *const *one : pairs) if (!find_header(*out->extra, one[0])) (*out->extra)[one[0]] = one[1];
	return out;
}

// Bind editable response fields for postprocessors and script inspection.
void GDWebResponse::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status"), &GDWebResponse::get_status);
	ClassDB::bind_method(D_METHOD("set_status", "status"), &GDWebResponse::set_status);
	ClassDB::bind_method(D_METHOD("get_body"), &GDWebResponse::get_body);
	ClassDB::bind_method(D_METHOD("set_body", "body"), &GDWebResponse::set_body);
	ClassDB::bind_method(D_METHOD("get_headers"), &GDWebResponse::get_headers);
	ClassDB::bind_method(D_METHOD("set_headers", "headers"), &GDWebResponse::set_headers);
	ClassDB::bind_method(D_METHOD("get_type"), &GDWebResponse::type_name);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "status"), "set_status", "get_status");
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "body"), "set_body", "get_body");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "headers"), "set_headers", "get_headers");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "type"), "", "get_type");
}

// Send an HTTP request and return an asynchronous response.
Signal Http::fetch(const String &p_url, const Dictionary &p_opts, const Ref<GDHTTPTransport> &p_transport) {
	Ref<GDHTTPCall> call;
	call.instantiate();
	call->begin(p_url, p_opts, p_transport);
	return Signal(call.ptr(), "finished");
}

// Return retained content as text.
Ref<GDWebResponse> Http::text(const String &p_body, int64_t p_status) {
	return GDWebResponse::make(p_status, HTTP_TYPE_TEXT, p_body);
}

// Create an HTML response.
Ref<GDWebResponse> Http::html(const String &p_body, int64_t p_status) {
	return GDWebResponse::make(p_status, HTTP_TYPE_HTML, p_body);
}

// Create the response on the encoding path that completes its body.
VariantPair Http::json_out(const Variant &p_data, int64_t p_status, uint64_t p_until) {
	return JsonData::encode_reply(p_data, p_until, [p_status](const VariantPair &p_encoded) -> VariantPair {
		return json_reply(p_encoded, p_status);
	});
}

// Return assembled content without an intermediate text conversion.
Ref<GDWebResponse> Http::bytes_out(const PackedByteArray &p_body, const String &p_type, int64_t p_status) {
	const HttpType type = http_type_of(p_type);
	return GDWebResponse::make(p_status, type, p_body, type == HTTP_TYPE_CUSTOM ? p_type : String());
}

// Add protective headers to a copied response.
Ref<GDWebResponse> Http::guard(const Ref<GDWebResponse> &p_reply) {
	return p_reply.is_valid() ? p_reply->guarded() : Ref<GDWebResponse>();
}

// Default redirects to the same origin to avoid trusting external input as a destination.
// An unchecked external destination can disguise a phishing redirect behind a trusted origin.
// Callers must explicitly set away to permit external redirects.
Ref<GDWebResponse> Http::redirect(const String &p_to, int64_t p_status, bool p_away) {
	String to = p_to;
	if (!p_away) {
		// Reject external schemes and network paths while preserving same-origin relative references.
		if (!local_location(to)) {
			to = "/"; // Replace an untrusted target with the local root.
		}
	}
	return head(GDWebResponse::make(p_status, HTTP_TYPE_CUSTOM, String()), "Location", to);
}

// Create a 404 response.
Ref<GDWebResponse> Http::not_found(const String &p_msg) {
	return text(p_msg, 404);
}

// Replace a response header.
Ref<GDWebResponse> Http::head(const Ref<GDWebResponse> &p_reply, const String &p_name, const Variant &p_value) {
	return p_reply.is_valid() ? p_reply->with_header(p_name, p_value, false) : Ref<GDWebResponse>();
}

// Append without removing existing values; dictionaries retain one value per key.
// Promote the second occurrence to a sequence.
Ref<GDWebResponse> Http::add_head(const Ref<GDWebResponse> &p_reply, const String &p_name, const Variant &p_value) {
	return p_reply.is_valid() ? p_reply->with_header(p_name, p_value, true) : Ref<GDWebResponse>();
}

// Return the HTTP status corresponding to an Err.
int Http::status_of(const Ref<Err> &p_err) {
	if (p_err.is_null()) {
		return 500;
	}
	const Variant explicit_status = p_err->get_info().get("http_status", Variant());
	if (explicit_status.get_type() == Variant::INT && int64_t(explicit_status) >= 400 && int64_t(explicit_status) <= 599) {
		return int(int64_t(explicit_status));
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
