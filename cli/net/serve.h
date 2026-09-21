/**************************************************************************/
/*  serve.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

#include "cli/sys/sink.h"

// Route requests, invoke handlers, and construct responses.
// GDWebServer handles HTTP/1.1 framing beneath this application layer.
//
// Cross into script execution only when invoking a handler Callable.
// If a handler awaits, wait for its completed signal before responding.

#include "cli/net/fetch.h"
#include "cli/net/http.h"
#include "cli/sys/std.h"
#include "cli/sys/task.h"

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/rb_set.h"

#include <memory>

class GDWebRequest;
class HtmlBuild;
class GDWebTLSCall;

// Consume ready request bodies on the main loop and suspend only unfinished work.
class GDWebBodyCall : public RefCounted {
	GDCLASS(GDWebBodyCall, RefCounted);

	enum Mode {
		READ, // Return one chunk.
		BYTES, // Collect the remaining body as bytes.
		TEXT, // Decode the remaining body as UTF-8.
		JSON, // Decode the remaining body as JSON.
		SAVE, // Save the remaining body incrementally to a file.
		VALIDATE, // Decode JSON and apply middleware validation rules.
	};

	Ref<GDWebBodyCall> self_hold; // Self-reference retained until completion.
	Ref<R> ready; // Result completed before a caller needs to await.
	bool starting = false; // Capture inline completion before signal listeners exist.
	Ref<GDWebRequest> req; // Request ownership preventing overlapping body operations.
	Ref<GDWebServer> srv; // Keep the server alive while reading.
	Ref<FileSink> sink; // Worker-thread writer used by SAVE.
	bool flushing = false; // Reception is complete and output is flushing.
	Ref<Err> failure; // Failure retained until the writer's partial count is final.
	bool converting = false; // A worker is converting body bytes to text or JSON.
	PackedByteArray data; // Bytes collected for READ or whole-body operations.
	String path; // Public path included in save failures.
	Dictionary rule; // Input rule applied by VALIDATE.
	String keep_name; // Request-local name for a validated result.
	int id = 0; // Unique connection identifier.
	int64_t want = 0; // Maximum bytes returned by READ.
	int mode = READ; // Conversion performed at completion.
	int64_t total = 0; // Total bytes read by this operation.
	bool scheduled = false; // Whether the operation is already in the runtime ready queue.

	void step();
	bool advance(); // Return true when another body read can proceed without waiting.
	void finish(const Ref<R> &p_result);
	void schedule();
	void converted(const Variant &p_result); // Return worker-converted body data to request state.
	Variant start(const Ref<GDWebRequest> &p_req, const Ref<GDWebServer> &p_srv, int p_id, int p_mode, int64_t p_want, const String &p_path, bool p_deferred = true);

	friend class GDWebRequest;

protected:
	static void _bind_methods();

public:
	// Cancel a body operation no longer needed by its waiter.
	void cancel();
};

// Handler request object that retrieves headers and body lazily from the transport.
class GDWebRequest : public RefCounted {
	GDCLASS(GDWebRequest, RefCounted);

	Ref<GDWebServer> srv;
	int id = 0;
	String raw_path_txt; // Percent-encoded path from the request line.
	String path_txt; // Path decoded once for handlers.
	String route_txt; // Normalized key for direct exact-route lookup.
	PackedStringArray path_parts; // Decoded segments used for route matching.
	String query_txt; // Query text fetched lazily from the transport.
	Dictionary query_map; // First value of each decoded query parameter.
	Dictionary query_values; // All values in arrival order, including binary values.
	bool query_done = false;
	Dictionary params_map; // Values captured by named route parameters.
	Dictionary store; // Request-local values shared between processing stages.
	Ref<GDAsyncContext> ctx; // Per-request context conveying disconnect and completion.
	Ref<GDAsyncContext> ctx_parent; // Parent retained until cancellation monitoring is needed.
	Ref<Err> ctx_reason; // Observed cancellation reason retained without signal connections.
	const char *ctx_end = nullptr; // Static completion reason retained for late context access.
	Ref<GDWebBodyCall> body_active; // Current body operation cancelled with the request.
	bool body_busy = false; // Prevent two simultaneous operations on the same stream.

	// Clear request state for reuse.
	void reset(int p_id, const String &p_path, const Ref<GDAsyncContext> &p_parent);
	void finish_context(const char *p_reason);
	Variant body_call(int p_mode, int64_t p_want = 0, bool p_deferred = false); // Share body ownership and completion policy.
	Signal json_valid(const Dictionary &p_rule, const String &p_name);

	friend class GDWebApp;
	friend class GDWebBodyCall;
	friend class GDWebValid;

protected:
	static void _bind_methods();

public:
	String get_method() const;
	String get_path() const { return path_txt; }
	String get_ip() const;
	Dictionary get_query();
	Dictionary get_query_all(); // Return all values for each query key.
	Dictionary get_params() const { return params_map; }
	String get_target();
	Ref<GDAsyncContext> get_context(); // Materialize cancellation only when observed or awaited.
	// Share request-local values between middleware and later processing.
	void keep(const String &p_name, const Variant &p_value);
	Variant kept(const String &p_name, const Variant &p_fallback) const;
	Variant valid(const String &p_name, const Variant &p_fallback) const;
	String header(const String &p_name) const;
	Dictionary headers() const;
	// Read up to bytes from the stream; an empty success value denotes EOF.
	Variant read(int64_t p_bytes = 32768);
	Signal read_async(int64_t p_bytes = 32768); // Always defer completion for explicit awaiting.
	// Read the entire remaining body into memory.
	Variant bytes();
	Signal bytes_async(); // Always defer collecting the remaining bytes.
	int64_t body_size() const;
	// Set a byte-reader limit for this request only.
	Ref<R> limit(int64_t p_bytes);
	// Save the remaining body inside a mount without expanding it in memory.
	Signal save(const String &p_path);
	Variant text();
	Signal text_async(); // Always defer decoding the remaining text.
	Variant json(); // Distinguish valid JSON null from parse failure through R.
	Signal json_async(); // Always defer decoding the remaining JSON.
};

// Perform template file I/O and rendering outside the event loop.
class GDWebViewCall : public RefCounted {
	GDCLASS(GDWebViewCall, RefCounted);

	Ref<GDWebViewCall> self_hold; // Retain this operation until result delivery.
	Ref<RefCounted> continuation; // Suspended script state for a custom renderer.
	std::shared_ptr<HtmlBuild> build; // Paused parsing state shared with the CPU job.
	Callable renderer; // Caller-supplied renderer, empty for the default renderer.
	Dictionary data; // Values passed to the renderer.
	String path; // Template path.
	int64_t status = 200; // HTTP status on successful rendering.
	String part; // Partial name sent to the I/O queue, empty for the root.

	void loaded(const Variant &p_result); // Pass worker-loaded template data to the renderer.
	void prepared(const Variant &p_result); // Queue missing partials for I/O or return the completed result.
	void custom_ready(const Variant &p_result); // Pass decoded UTF-8 source to the custom script renderer.
	void rendered(const Variant &p_result); // Convert renderer output into a reply.
	void finish(const Ref<R> &p_result); // Deliver a completed reply or cancellation exactly once.

protected:
	static void _bind_methods();

public:
	static Signal start(const String &p_path, const Dictionary &p_data, int64_t p_status, const Callable &p_renderer);
	void cancel(); // Stop continuation when the server releases the request.
};

class GDWebRouteGroup;
class GDWebMiddleware;
class GDWebSessionStore;

// HTTP application with routing.
class GDWebApp : public RefCounted {
	GDCLASS(GDWebApp, RefCounted);
	friend class GDWebTLSCall;
	Ref<RefCounted> opening; // Identity token prevents a cancelled TLS start from opening a listener later.
	Ref<R> listen_at(int64_t p_port, const String &p_host, const Ref<GDTLSIdentity> &p_identity); // Share listener setup across transports.

	// Handler callable and the owner keeping it alive.
	struct Mid {
		Variant hold;
		Callable fn;
	};

	// Routes containing named parameters; exact routes use direct lookup.
	struct Route {
		String method;
		PackedStringArray parts;
		Callable handler;
		int mids = -1; // Route-specific middleware sequence.
		int group = -1; // Owning route group.
	};

	// One exact route with handlers indexed by method.
	struct Slot {
		String method;
		Callable handler;
		int mids = -1; // Route-specific middleware sequence.
		int group = -1;
	};

	// Route group sharing a prefix and group-local middleware.
	struct Band {
		String prefix;
		LocalVector<Mid> mids;
	};

	// Request processing phase retained across suspension.
	enum Stage {
		PRE, // Middleware before route selection.
		STATIC, // Opening a static file on a worker.
		USE, // Global middleware after route selection.
		BAND, // Group-local middleware.
		ROUTE, // Route-local middleware.
		HANDLE, // Request handler.
		FAIL, // Error handler.
		AFTER, // Postprocessing.
		ENCODE, // Convert a raw dictionary into a JSON response.
		DONE, // Response ready for transmission.
	};

	// Continuation retained when a handler suspends with await.
	struct Job {
		Ref<GDWebRequest> req;
		Callable handler; // Selected handler.
		Variant out; // Constructed response.
		int stage = PRE;
		int at = 0; // Position within the current phase.
		int band = -1; // Selected route group.
		int mids = -1; // Selected route's middleware sequence index.
		int static_at = 0; // Next static root to inspect.
		Ref<FileSource> file; // Incremental reader for a static response.
		String file_type; // Static response Content-Type.
		const char *encoded_type = nullptr; // Content type of an internally encoded body without a response envelope.
		Variant body; // Encoded envelope body retained independently of middleware-owned dictionaries.
		bool body_encoded = false; // Whether conversion supplies the final envelope body.
		bool head_text = false; // Preserve the response envelope while measuring omitted text.
		bool failing = false; // Prevent recursive error handling.
		int after_at = 0; // Resume after a failed postprocessor without running it twice.
		bool ready = false; // Whether route selection alone produced a response.
		Ref<RefCounted> hold; // Retained suspended state.
		Signal wait_signal; // Completion signal for a native asynchronous operation.
		Callable wait_call; // Exact completion subscription removed on cancellation.
		uint64_t made = 0; // First suspension time used for expiry.
		uint64_t due = 0; // Deadline in the timeout-ordered index.
	};
	struct JobTime {
		uint64_t due = 0; // Deadline in milliseconds.
		int id = 0; // Request ID distinguishing equal deadlines.

		bool operator<(const JobTime &p_other) const {
			return due == p_other.due ? id < p_other.id : due < p_other.due;
		}
	};
	int job_max = 0; // Explicit pending asynchronous-operation limit; zero is unlimited.
	int awaiting = 0; // Suspended handler count excluding internal response conversion.
	uint64_t job_ms = 0; // Per-request asynchronous timeout; zero is unlimited.
	double head_seconds = 0.0; // Complete-header receive deadline; zero is unlimited.
	double body_seconds = 0.0; // Complete-body receive deadline; zero is unlimited.
	int header_bytes = 1 << 20; // Default total request-header limit.
	int header_values = INT_MAX; // Default field count leaves the header-byte limit as the effective bound.
	uint64_t job_due = 0; // Earliest asynchronous handler deadline.

	Ref<GDWebServer> srv;
	HashMap<String, LocalVector<Slot>> exact; // Exact path mapped to handlers for each method.
	LocalVector<Route> routes;
	LocalVector<Mid> pres; // Middleware before route selection.
	LocalVector<Mid> uses; // Global middleware after route selection.
	LocalVector<LocalVector<Mid>> route_mids; // Route-specific middleware indexed by number so relocation is safe.
	LocalVector<Callable> afters; // Postprocessors that may replace the response.
	LocalVector<Band> bands;
	LocalVector<Pair<String, String>> statics; // Static URL prefixes and filesystem roots.
	Callable fallback; // Handler when no route matches.
	mutable Ref<Err> config_error; // First invalid registration or setting, returned by listen.
	bool invalid(bool p_bad, const String &p_message) const; // Prevent startup with incomplete configuration.
	Callable on_fail; // Handler for request-handler failures.
	bool tell_why = false; // Whether to expose failure details externally; disabled by default.
	int64_t max_body = INT64_MAX; // Do not narrow body size unless explicitly configured.
	HashMap<int, Job> jobs;
	RBSet<JobTime> job_times; // Asynchronous handlers indexed by deadline.
	Ref<GDWebRequest> spare; // Completed request object retained for reuse.
	int busy = 0; // Nesting depth of active work.
	int current_id = -1; // ID of the request that called stop, retained through response completion.
	int stop_after = -1; // Suspended request whose completion should trigger stopping.
	bool stop_wanted = false; // Whether stopping was requested during active work.
	bool shutting = false; // New accepts have stopped and active requests are draining.
	Ref<GDAsyncContext> shutdown_ctx; // Context bounding graceful shutdown.
	Ref<GDWait> shutdown_wait; // Waiter receiving graceful-shutdown completion.
	Ref<GDAsyncContext> root_ctx; // Root context propagating cancellation to application requests.
	LocalVector<int> ready_ids; // FIFO of runnable request IDs from the HTTP parser.
	uint32_t ready_at = 0; // Next request to dispatch to a handler.
	bool poll_posted = false; // Prevent duplicate runtime scheduling of remaining requests.
	bool dispatch_first = false; // Alternate I/O and retained handlers when both remain runnable.

	// Advance one request until it responds or suspends.
	// Only suspended requests enter jobs, avoiding map operations on the synchronous path.
	void run(int p_id, Job &p_job, const Variant &p_back, bool p_resumed, uint64_t p_until);
	void post_poll(); // Schedule another runtime turn when runnable requests remain.
	// Consume one call's result and advance the processing position.
	void failed(Job &p_job, const Ref<Err> &p_error); // Route unsent failures through the shared error handler.
	void step(Job &p_job, const Variant &p_ret);
	// Select a route and handler, or construct an immediate unmatched response.
	void pick(Job &p_job);
	// Inspect the next static root and return its file-open completion signal.
	Variant pick_static(Job &p_job);
	// Resume a suspended handler from its completed signal.
	void resumed(const Variant &p_value, int p_id);
	// Normalize arbitrary signal arguments before resuming the request.
	Variant resume_signal(const Variant **p_args, int p_count, Callable::CallError &r_err);
	// Retain a suspended operation and return true; otherwise return false.
	bool park(const Variant &p_ret, int p_id, Job &p_job);
	// Release asynchronous work for disconnected or expired requests.
	void trim_jobs();
	void drop_job_time(int p_id, const Job &p_job); // Remove a completed job from waiter counts and the deadline index.
	void arm_jobs(); // Register the earliest handler deadline with the runtime timer.
	// Disconnect the awaited source and prevent the suspended script from resuming.
	void cancel_job(Job &p_job);
	// Check graceful shutdown after completion or deadline notification.
	void check_shutdown();
	// Deliver graceful-shutdown results to the waiting caller.
	void finish_shutdown(const Ref<R> &p_result);
	// Finish one request with a text, byte-array, or dictionary response.
	void finish(int p_id, Job &p_job);
	// Match a route, storing named captures in p_req on success.
	bool match(const Route &p_route, const PackedStringArray &p_target, GDWebRequest *p_req) const;
	Mid mid_of(const Variant &p_mid) const;
	LocalVector<Mid> mids_of(const Array &p_mids) const;
	void add_route(const String &p_method, const String &p_pattern, const Callable &p_handler, const Array &p_mids, int p_group);

	friend class GDWebRouteGroup;

protected:
	static void _bind_methods();

public:
	// Centralize web-feature factories on GDWebApp.
	static Ref<R> jwt_sign(const Dictionary &p_claims, const Variant &p_key, const Dictionary &p_opts);
	static Ref<R> jwt_verify(const String &p_token, const Variant &p_key, const Dictionary &p_opts);
	static Ref<GDWebMiddleware> jwt(const Variant &p_key, const Dictionary &p_opts);
	static Ref<GDWebMiddleware> csrf(const Dictionary &p_opts);
	static Ref<GDWebSessionStore> sessions(int64_t p_total, int64_t p_per_user, int64_t p_idle_seconds, int64_t p_life_seconds, const String &p_cookie, const String &p_keep);
	static Dictionary rule_text(int64_t p_min, int64_t p_max);
	static Dictionary rule_integer(int64_t p_min, int64_t p_max);
	static Dictionary rule_number(double p_min, double p_max);
	static Dictionary rule_boolean();
	static Dictionary rule_list(const Dictionary &p_item, int64_t p_min, int64_t p_max);
	static Dictionary rule_object(const Dictionary &p_fields, bool p_extra);
	static Dictionary rule_optional(const Dictionary &p_rule, const Variant &p_fallback);
	static Dictionary rule_one_of(const Array &p_values);
	static Ref<R> validate(const Variant &p_value, const Dictionary &p_rule);
	static Ref<GDWebMiddleware> valid_json(const Dictionary &p_rule, const String &p_name);
	static Ref<GDWebMiddleware> valid_query(const Dictionary &p_rule, const String &p_name);
	static Ref<GDWebMiddleware> valid_params(const Dictionary &p_rule, const String &p_name);

	// Bind an HTTP method and path to a handler.
	void route(const String &p_method, const String &p_pattern, const Callable &p_handler, const Array &p_mids);
	void pre(const Variant &p_mid);
	void use(const Variant &p_mid);
	void after(const Callable &p_after);
	void on_error(const Callable &p_handler);
	// Expose failure details in response bodies only during local development.
	void show_errors(bool p_on);
	// Set the per-request body limit.
	void body_limit(int64_t p_bytes);
	void limits(const Dictionary &p_opts);
	// Expose the cumulative count of unsafe response headers omitted from output.
	// Keep a counter instead of attacker-amplifiable per-header logs.
	uint64_t dropped_headers() const;
	// Create a prefix group whose middleware applies only to its member routes.
	Ref<GDWebRouteGroup> group(const String &p_prefix, const Array &p_mids);
	void static_dir(const String &p_prefix, const String &p_dir);
	void otherwise(const Callable &p_handler);

	Signal file_at(const String &p_path) const; // Read a static file into a response or preserve its failure.

	// Start listening and return failure details through R.
	Ref<R> listen(int64_t p_port, const String &p_host);
	Signal listen_tls(int64_t p_port, const String &p_cert, const String &p_key, const String &p_host, const Dictionary &p_opts); // Load credentials and client-auth policy off-loop and return the listen result.
	// Return the actual listen port, including kernel-selected ports.
	int port() const;
	// Stop accepting and wait for active requests to finish.
	Signal shutdown(const Ref<GDAsyncContext> &p_ctx);
	void stop();
	void leave(); // Honor deferred stop requests at a work boundary.
	bool is_listening() const;
	void poll(); // Advance only requests that received readiness notifications.

	~GDWebApp();
};

// Thin route-group interface that registers prefixed routes on its parent.
class GDWebRouteGroup : public RefCounted {
	GDCLASS(GDWebRouteGroup, RefCounted);

	Ref<GDWebApp> app; // Retain the parent application while the group exists.
	int id = -1;

	friend class GDWebApp;

protected:
	static void _bind_methods();

public:
	void use(const Variant &p_mid);
	// Register a route with the group's prefix and middleware.
	void route(const String &p_method, const String &p_pattern, const Callable &p_handler, const Array &p_mids);
};

// Internal response constructors exposed to scripts through GD.http and Web.
class Http {
public:
	// Issue a client request and return its completion signal.
	// Await the response and its transport error, retaining partial data.
	static Signal fetch(const String &p_url, const Dictionary &p_opts, const Ref<GDHTTPTransport> &p_transport);

	static Dictionary text(const String &p_body, int64_t p_status);
	static Dictionary html(const String &p_body, int64_t p_status);
	static Variant json_out(const Variant &p_data, int64_t p_status, uint64_t p_until = 0);
	// Return already-assembled content without an intermediate text conversion.
	static Dictionary bytes_out(const PackedByteArray &p_body, const String &p_type, int64_t p_status);
	// Redirect within the origin by default; set away explicitly for external targets.
	static Dictionary redirect(const String &p_to, int64_t p_status, bool p_away);
	// Add protective headers against MIME sniffing, framing, injection, and referrer leakage.
	// Use security-oriented defaults that callers may explicitly override.
	static Dictionary guard(const Dictionary &p_reply);
	static Dictionary not_found(const String &p_msg);
	// Add one response header and return the same dictionary for chaining.
	static Dictionary head(const Dictionary &p_reply, const String &p_name, const Variant &p_value);
	// Append another field with the same name, such as an additional Set-Cookie.
	static Dictionary add_head(const Dictionary &p_reply, const String &p_name, const Variant &p_value);
	// Map an error category to an HTTP status.
	static int status_of(const Ref<Err> &p_err);
};
