// Adapt asynchronous response producers to ordered byte writes without scene-tree work.
#include "cli/net/body_source.h"
#include "cli/data/utf8.h"
#include "cli/sys/clock.h"
#include "cli/sys/sched.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "modules/gdscript/gdscript_function.h"

namespace {
#ifdef WINDOWS_ENABLED
constexpr int64_t high_water = 16 * 1024; // Published-byte backpressure, never a text reservation limit.
#else
constexpr int64_t high_water = 64 * 1024; // Published-byte backpressure, never a text reservation limit.
#endif
}

// Create a response while leaving producer execution lazy for HEAD and suppressed bodies.
Dictionary GDWebWriter::reply(const Callable &p_producer, int64_t p_length, const String &p_type, int64_t p_status) {
	Ref<GDWebWriter> writer;
	writer.instantiate();
	writer->producer = p_producer;
	writer->length = p_length;
	if (!p_producer.is_valid() || p_length < -1) { writer->why = "invalid response producer"; writer->ended = true; }
	Dictionary headers, response;
	headers["Content-Type"] = p_type;
	response["headers"] = headers;
	response["status"] = p_status;
	response["body"] = writer;
	return response;
}

// Acknowledge the preceding batch, then transfer all currently queued ranges together.
bool GDWebWriter::take(BodyChunk &r_chunk) {
	if (batch) {
		consumed += sending;
		sending = 0;
		batch = false;
		while (!queue.is_empty() && queue.front()->get()->end <= consumed) {
			const Ref<GDWebWriteCall> call = queue.front()->get();
			call->settle(R::ok(call->count));
			queue.pop_front();
		}
	}
	post_pump();
	if (!buffered.is_empty() || !queue.is_empty()) {
		batch = true;
		sending = buffered.size();
		r_chunk = std::move(buffered);
		buffered.clear();
		return true;
	}
	if (!started && !ended) {
		started = true;
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWebWriter::step));
	}
	return false;
}

// Retain complete input ranges and apply backpressure without rejecting oversized writes.
Variant GDWebWriter::write(const PackedByteArray &p_bytes, int64_t p_offset, int64_t p_count) {
	if (ended || !why.is_empty()) return R::err("response writer is closed", Err::INTERRUPTED, 0);
	if (p_offset < 0 || p_offset > p_bytes.size() || p_count < -1 || p_count > p_bytes.size() - p_offset)
		return R::err("invalid write range", Err::INVALID_DATA, 0);
	if (p_count < 0) p_count = p_bytes.size() - p_offset;
	if (input.is_empty()) return accept(p_bytes, p_offset, p_count);
	Input item;
	item.bytes = p_bytes;
	item.offset = p_offset;
	item.count = p_count;
	return enqueue(std::move(item));
}

// Commit only measured operations; producer EOF does not reject previously queued input.
Variant GDWebWriter::accept(const PackedByteArray &p_bytes, int64_t p_offset, int64_t p_count, const Ref<GDWebWriteCall> &p_call) {
	if (!why.is_empty()) return R::err("response writer is closed", Err::INTERRUPTED, 0);
	if (p_offset < 0 || p_offset > p_bytes.size() || p_count < -1 || p_count > p_bytes.size() - p_offset)
		return R::err("invalid write range", Err::INVALID_DATA, 0);
	if (p_count < 0) p_count = p_bytes.size() - p_offset;
	if (p_count > INT64_MAX - total || (length >= 0 && p_count > length - total)) {
		why = "response exceeds declared length";
		post();
		return R::err(why, Err::INVALID_DATA, 0);
	}
	total += p_count;
	buffered.append(p_bytes, p_offset, p_count);
	// Let consecutive small writes share a transport batch; an empty write is a flush barrier.
	if (p_count && total - consumed < high_water) {
		post();
		return R::okv(p_count);
	}
	Ref<GDWebWriteCall> call = p_call;
	if (call.is_null()) call.instantiate();
	call->end = total;
	call->count = p_count;
	queue.push_back(call);
	if (drain.is_valid()) drain.call();
	else post();
	if (call->result.is_valid()) return call->result;
	call->waiting = true;
	return Signal(call.ptr(), "finished");
}

// Keep explicit asynchronous calls signal-valued even when the buffer accepts bytes immediately.
Signal GDWebWriter::write_async(const PackedByteArray &p_bytes, int64_t p_offset, int64_t p_count) {
	const Variant result = write(p_bytes, p_offset, p_count);
	return result.get_type() == Variant::SIGNAL ? Signal(result) : Async::ready(result);
}

// Encode a selected character range without copying the source text or bypassing byte-write accounting.
Variant GDWebWriter::write_text(const String &p_text, int64_t p_offset, int64_t p_count) {
	if (ended || !why.is_empty()) return R::err("response writer is closed", Err::INTERRUPTED, 0);
	if (p_offset < 0 || p_offset > p_text.length() || p_count < -1 || p_count > p_text.length() - p_offset)
		return R::err("cannot encode text range", Err::INVALID_DATA, 0);
	Utf8Text text;
	text.text = p_text;
	text.begin = int(p_offset);
	text.count = int(p_count < 0 ? p_text.length() - p_offset : p_count);
	text.probe = true;
	const uint64_t outer = GDScriptFunction::native_time_slice_deadline();
	const uint64_t until = outer ? outer : GDClock::usec() + GD_SCHED_SLICE_USEC;
	if (input.is_empty() && total - consumed < high_water && text.advance(until)) {
		if (text.error != OK) return R::err("cannot encode text range", Err::LIMITED, 0);
		return accept(text.bytes, 0, text.bytes.size());
	}
	Input item;
	item.text = std::make_shared<Utf8Text>(std::move(text));
	return enqueue(std::move(item));
}

// Retain operation order before exposing a completion signal to the producer.
Variant GDWebWriter::enqueue(Input &&p_input) {
	p_input.call.instantiate();
	const Ref<GDWebWriteCall> call = p_input.call;
	call->waiting = true;
	input.push_back(std::move(p_input));
	post_pump();
	return Signal(call.ptr(), "finished");
}

// Schedule only work that can progress; unknown byte reservations never block their own encoder.
void GDWebWriter::post_pump() {
	if (pump_posted || pumping || !conversion.is_null() || input.is_empty() || !why.is_empty() || total - consumed >= high_water) return;
	pump_posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWebWriter::pump));
}

// Process producer input independently of transport completion barriers.
void GDWebWriter::pump() {
	pump_posted = false;
	if (pumping || !conversion.is_null() || !why.is_empty() || input.is_empty()) return;
	pumping = true;
	const bool sliced = GDScriptFunction::begin_time_slice();
	const uint64_t until = GDScriptFunction::native_time_slice_deadline();
	while (!input.is_empty() && why.is_empty() && total - consumed < high_water) {
		if (until && GDClock::usec() >= until) break;
		Input &front = input.front()->get();
		if (front.text) {
			if (front.text->progressed() || !front.text->advance(until)) {
				conversion = utf8_continue(std::move(*front.text));
				front.text.reset();
				conversion_hold = Ref<RefCounted>(Object::cast_to<RefCounted>(conversion.get_object()));
				const Error connected = conversion.connect(callable_mp(this, &GDWebWriter::converted), Object::CONNECT_ONE_SHOT);
				if (connected == OK) break;
				cancel_conversion();
				front.error = Err::make("cannot await text conversion", Err::of(connected));
			} else {
				if (front.text->error != OK) front.error = Err::make("cannot encode text range", Err::LIMITED);
				front.bytes = std::move(front.text->bytes);
				front.offset = 0;
				front.count = front.bytes.size();
				front.text.reset();
			}
		}
		Input item = std::move(front);
		input.pop_front(); // Transport reentry cannot observe this operation twice.
		Variant result;
		if (item.error.is_valid()) {
			result = R::err(item.error, Err::NONE, 0);
		} else {
			result = accept(item.bytes, item.offset, item.count, item.call);
		}
		if (result.get_type() != Variant::SIGNAL) {
			const Ref<R> value = result;
			item.call->settle(value);
			if (!value->get_ok()) {
				// Fail the unpublished suffix without inventing byte offsets for unmeasured text.
				while (!input.is_empty()) {
					input.front()->get().call->settle(R::err("preceding response write failed", Err::INTERRUPTED, 0));
					input.pop_front();
				}
				if (ended && why.is_empty()) why = value->get_e()->text();
				break;
			}
		}
	}
	check_length();
	pumping = false;
	post_pump();
	post();
	if (sliced) GDScriptFunction::end_time_slice();
}

// Conversion completion is separate from the public byte-write completion barrier.
void GDWebWriter::converted(const Variant &p_value) {
	conversion = Signal();
	conversion_hold.unref();
	if (input.is_empty()) return;
	Input &front = input.front()->get();
	if (p_value.get_type() == Variant::PACKED_BYTE_ARRAY) {
		front.bytes = p_value;
		front.offset = 0;
		front.count = front.bytes.size();
	} else {
		const Ref<R> result = p_value.get_type() == Variant::OBJECT ? Ref<R>(p_value) : Ref<R>();
		front.error = result.is_valid() && !result->get_ok() ? result->get_e() : Err::make("invalid text conversion result", Err::INVALID_DATA);
	}
	post_pump();
}

// Cancellation leaves running storage owned by the pool until its worker stops.
void GDWebWriter::cancel_conversion() {
	if (!conversion.is_null() && conversion.get_object()) {
		const Callable done = callable_mp(this, &GDWebWriter::converted);
		if (conversion.is_connected(done)) conversion.disconnect(done);
		conversion.get_object()->call("cancel");
	}
	conversion = Signal();
	conversion_hold.unref();
}

// Producer completion becomes final only when every queued operation has a known byte length.
void GDWebWriter::check_length() {
	if (ended && input.is_empty() && why.is_empty() && length >= 0 && total != length) why = "response shorter than declared length";
}

// Preserve the explicit signal contract for both immediately accepted and suspended text writes.
Signal GDWebWriter::write_text_async(const String &p_text, int64_t p_offset, int64_t p_count) {
	const Variant result = write_text(p_text, p_offset, p_count);
	return result.get_type() == Variant::SIGNAL ? Signal(result) : Async::ready(result);
}

// Run producer code with the shared scheduler time slice on the owning runtime.
void GDWebWriter::step() {
	if (ended) return;
	const Variant writer = Ref<GDWebWriter>(this);
	const Variant *args[] = {&writer};
	Variant result;
	Callable::CallError error;
	const bool sliced = GDScriptFunction::begin_time_slice();
	{
		GDScriptFunction::SuspendableCall suspendable;
		producer.callp(args, 1, result, error);
	}
	if (sliced) GDScriptFunction::end_time_slice();
	received(error.error == Callable::CallError::CALL_OK ? result : Variant(R::err("response producer failed", Err::INVALID_DATA)));
}

// Retain suspended functions and reject non-void producer results except explicit result wrappers.
void GDWebWriter::received(const Variant &p_value) {
	if (ended) return;
	wait = Signal();
	hold.unref();
	if (p_value.get_type() == Variant::SIGNAL) wait = p_value;
	else if (p_value.get_type() == Variant::OBJECT && Object::cast_to<GDScriptFunctionState>(p_value.get_validated_object())) wait = Signal(p_value.get_validated_object(), "completed");
	if (!wait.is_null()) {
		hold = Ref<RefCounted>(Object::cast_to<RefCounted>(wait.get_object()));
		if (wait.connect(callable_mp(this, &GDWebWriter::received), Object::CONNECT_ONE_SHOT) == OK) return;
		wait = Signal();
		hold.unref();
		why = "cannot await response producer";
	} else {
		const Ref<R> result = p_value.get_type() == Variant::OBJECT ? Ref<R>(p_value) : Ref<R>();
		if (result.is_valid() && !result->get_ok()) why = result->get_e()->text();
		else if (p_value.get_type() != Variant::NIL && result.is_null()) why = "response producer must return void or R";
	}
	ended = true;
	producer = Callable();
	check_length();
	post_pump();
	post();
}

// Coalesce producer events without invoking transport callbacks recursively.
void GDWebWriter::post() {
	if (posted) return;
	posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWebWriter::notify));
}

// Wake the transport without treating producer EOF as completed response delivery.
void GDWebWriter::notify() {
	posted = false;
	if (ready.is_valid()) ready.call();
}

// Publish successful completion outside the transport stack after framing has drained.
void GDWebWriter::finish() {
	if (ctx.is_valid()) {
		const Ref<GDAsyncContext> context = ctx;
		ctx.unref();
		Async::post(context, callable_mp(context.ptr(), &GDAsyncContext::cancel).bind("response producer finished", int(Err::INTERRUPTED)));
	}
}

// Fail pending writes and detach suspended script state after peer cancellation.
void GDWebWriter::abort_sent(int64_t p_sent) {
	discard(p_sent);
	peer_closed();
}

// Stop body production while preserving independent response completion notification.
void GDWebWriter::discard(int64_t p_sent) {
	cancel_conversion();
	ended = true;
	ready = Callable();
	drain = Callable();
	producer = Callable();
	if (!wait.is_null() && wait.get_object()) {
		const Callable resume = callable_mp(this, &GDWebWriter::received);
		if (wait.is_connected(resume)) wait.disconnect(resume);
		// Keep the producer's own wait connected so failed writes and context cancellation can resume cleanup.
	}
	wait = Signal();
	hold.unref();
	const int64_t progress = consumed + MIN(MAX(p_sent, int64_t(0)), sending);
	sending = 0;
	batch = false;
	buffered.clear();
	while (!queue.is_empty()) {
		const Ref<GDWebWriteCall> call = queue.front()->get();
		const int64_t sent = CLAMP(progress - (call->end - call->count), int64_t(0), call->count);
		call->settle(R::err("response transport closed", Err::INTERRUPTED, sent));
		queue.pop_front();
	}
	// Published barriers precede every unpublished input, including on cancellation.
	while (!input.is_empty()) {
		input.front()->get().call->settle(R::err("response transport closed", Err::INTERRUPTED, 0));
		input.pop_front();
	}
}

// Propagate read-side cancellation without prohibiting a response to a half-closed connection.
void GDWebWriter::peer_closed() {
	if (ctx.is_valid()) {
		const Ref<GDAsyncContext> context = ctx;
		ctx.unref();
		// User cancellation handlers may destroy the listener, so leave the transport stack first.
		Async::post(context, callable_mp(context.ptr(), &GDAsyncContext::cancel).bind("response peer closed", int(Err::INTERRUPTED)));
	}
}

// Publish completion outside the HTTP send stack.
void GDWebWriteCall::settle(const Ref<R> &p_result) {
	if (result.is_valid()) return;
	result = p_result;
	if (waiting) Async::post(Ref<RefCounted>(this), callable_mp(this, &GDWebWriteCall::deliver));
}

// Resume the producer with its settled write result.
void GDWebWriteCall::deliver() {
	emit_signal("finished", result);
}

// Register the common Writer completion result.
void GDWebWriteCall::_bind_methods() { ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R"))); }

// Expose byte writes and flushing while keeping body lifecycle owned by the HTTP transport.
void GDWebWriter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("write", "data", "offset", "count"), &GDWebWriter::write, DEFVAL(0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("write_async", "data", "offset", "count"), &GDWebWriter::write_async, DEFVAL(0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("write_text", "text", "offset", "count"), &GDWebWriter::write_text, DEFVAL(0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("write_text_async", "text", "offset", "count"), &GDWebWriter::write_text_async, DEFVAL(0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("flush"), &GDWebWriter::flush);
	ClassDB::bind_method(D_METHOD("flush_async"), &GDWebWriter::flush_async);
	ADD_AWAIT("write", "R:int"); ADD_AWAIT("write_async", "R:int"); ADD_AUTO_WAIT("write");
	ADD_AWAIT("write_text", "R:int"); ADD_AWAIT("write_text_async", "R:int"); ADD_AUTO_WAIT("write_text");
	ADD_AWAIT("flush", "R:int"); ADD_AWAIT("flush_async", "R:int"); ADD_AUTO_WAIT("flush");
}
