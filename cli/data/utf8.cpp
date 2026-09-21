// Encode response text incrementally while preserving native string byte semantics.
#include "cli/data/utf8.h"

#include "cli/data/ascii.h"
#include "cli/data/text_diag.h"
#include "cli/sys/clock.h"
#include "cli/sys/task.h"
#include "cli/sys/pool.h"
#include <atomic>
#include "cli/sys/sched.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "modules/gdscript/gdscript_function.h"
#include <memory>

// Decode once without replacement, BOM stripping, or NUL truncation.
Variant utf8_value(const uint8_t *p_data, int64_t p_size, bool p_strict) {
	// Keep binary values outside the native string representation lossless.
	auto binary = [&]() -> Variant {
		PackedByteArray bytes;
		if (bytes.resize(p_size) != OK) return R::err("cannot allocate byte value", Err::LIMITED);
		if (p_size) memcpy(bytes.ptrw(), p_data, p_size);
		return bytes;
	};
	// Text-only formats reject malformed sequences instead of accepting binary fallback.
	auto invalid = [&]() -> Variant { return p_strict ? Variant(R::err("invalid UTF-8 text", Err::INVALID_DATA)) : binary(); };
	if (!p_size) return String();
	if (p_size > INT_MAX - 1) return binary();
	String text;
	if (text.resize_uninitialized(p_size + 1) != OK) return R::err("cannot allocate text value", Err::LIMITED);
	char32_t *dst = text.ptrw();
	int used = 0;
	bool has_nul = false; // Embedded NUL requires binary storage but remains valid UTF-8.
	for (int64_t i = 0; i < p_size;) {
		uint32_t c = p_data[i++];
		if (!c) has_nul = true;
		if (c >= 0x80) {
			const int extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
			if (!extra || i + extra > p_size) return invalid();
			c &= (1U << (6 - extra)) - 1;
			for (int j = 0; j < extra; j++) {
				const uint8_t next = p_data[i++];
				if ((next & 0xc0) != 0x80) return invalid();
				c = (c << 6) | (next & 0x3f);
			}
			const uint32_t least = extra == 1 ? 0x80 : extra == 2 ? 0x800 : 0x10000;
			if (c < least || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return invalid();
		}
		dst[used++] = c;
	}
	if (has_nul) return binary();
	dst[used] = 0;
	text.resize_uninitialized(used + 1);
	return text;
}

namespace {

constexpr int text_scratch = 64; // Stack capacity for short output and speculative prefix validation.

// Adapt clock-check spacing to observed work without imposing a response-size limit.
struct TextSlice {
	uint64_t until; // Shared scheduler deadline, or zero for synchronous completion.
	uint64_t last = 0; // Time at the start of the preceding batch.
	int span = sizeof(uintptr_t); // Initial machine-word probe before adapting to observed cost.

	// Return the next batch boundary, or the current position when time has expired.
	int end(int p_at, int p_length) {
		if (!until) return p_length;
		const uint64_t now = GDClock::usec();
		if (now >= until) return p_at;
		if (last) {
			span = now - last < (until - now) / 2 ? int(MIN(int64_t(span) * 2, int64_t(p_length - p_at))) : MAX(1, span / 2);
		}
		last = now;
		return p_at + MIN(span, p_length - p_at);
	}
};

} // namespace

// Save exact pass progress when the caller yields its native execution turn.
bool Utf8Text::advance(uint64_t p_until) {
	TextDiag::Phase phase;
	if (phase.active) {
		TextDiag::add(TextDiag::ADVANCES);
		if (!diagnosed) { diagnosed = true; TextDiag::add(TextDiag::OPERATIONS); }
	}
	const int length = count >= 0 ? count : text.length();
	const char32_t *src = length ? text.ptr() + begin : nullptr;
	// Narrow likely ASCII text in one pass while retaining resumable batch boundaries.
	if (probe) {
		if (bytes.is_empty() && length) {
			if (p_until && GDClock::usec() >= p_until) return false;
			phase.next(TextDiag::ALLOCATE_US);
			error = bytes.resize_uninitialized(length);
			if (phase.active && error == OK) TextDiag::peak(bytes.size());
			phase.next(TextDiag::PROBE_US);
			if (error != OK) return true;
		}
		TextSlice slice{ p_until };
		while (probe_at < length) {
			const int end = slice.end(probe_at, length);
			if (end == probe_at) return false;
			if (!GDUtf8::ascii(src + probe_at, bytes.ptrw() + probe_at, end - probe_at)) {
				// Keep only fully validated batches; the failing batch may be partially written.
				probe = false;
				ascii = false;
				at = probe_at;
				size = probe_at;
				break;
			}
			probe_at = end;
		}
		if (probe) { size = length; return true; }
	}
	// Finish short ASCII in one scan; all other input retains resumable conversion.
	uint8_t small[text_scratch]; // Stack storage only; larger strings use the complete conversion below.
	if (offset < 0 && at == 0 && length <= int(sizeof(small))) {
		if (p_until && GDClock::usec() >= p_until) return false;
		if (GDUtf8::ascii(src, small, length)) {
			size = length;
			if (!measure) {
				phase.next(TextDiag::ALLOCATE_US);
				error = bytes.resize_uninitialized(length);
				if (phase.active && error == OK) TextDiag::peak(bytes.size());
				phase.next(TextDiag::ENCODE_US);
				if (error == OK && length) memcpy(bytes.ptrw(), small, length);
			}
			return true;
		}
	}
	if (offset < 0) {
		phase.next(TextDiag::MEASURE_US);
		TextSlice slice{ p_until };
		while (at < length) {
			const int end = slice.end(at, length);
			if (end == at) return false;
			uint32_t bits = 0;
			// Measure ASCII-only prefixes without computing wider character counts.
			if (ascii) {
				for (int i = at; i < end; i++) bits |= src[i];
				if (bits <= 0x7f) {
					size += end - at;
					at = end;
					continue;
				}
			}
			int64_t added = end - at;
			// Classify and count ordinary widths in the same vectorizable pass.
			for (int i = at; i < end; i++) {
				const uint32_t c = src[i];
				bits |= c;
				added += (c > 0x7f) + (c > 0x7ff) + (c > 0xffff);
			}
			if (bits > 0x1fffff) {
				// Preserve extended encodings and their diagnostics outside the ordinary range.
				added = end - at;
				for (int i = at; i < end; i++) {
					const uint32_t c = src[i];
					added += GDUtf8::width(c) - 1;
					utf8_note(c);
				}
			}
			at = end;
			ascii = ascii && bits <= 0x7f;
			size += added;
		}
		if (p_until && GDClock::usec() >= p_until) return false;
		if (measure) return true;
		phase.next(TextDiag::ALLOCATE_US);
		error = bytes.resize_uninitialized(size);
		if (phase.active && error == OK) TextDiag::peak(bytes.size());
		if (error != OK) return true;
		// Reallocation preserves the validated prefix from speculative narrowing.
		offset = probe_at;
		at = probe_at;
	}
	phase.next(TextDiag::ENCODE_US);
	uint8_t *dst = bytes.ptrw();
	TextSlice slice{ p_until };
	while (at < length) {
		const int end = slice.end(at, length);
		if (end == at) return false;
		if (ascii) {
			// Narrow validated ASCII in contiguous batches, preserving embedded zero bytes.
			for (int i = at; i < end; i++) dst[i] = src[i];
			at = end;
			offset = at;
			continue;
		}
		// Keep cursors local while byte stores may alias object fields, then save the complete batch.
		int64_t written = offset;
		for (int i = at; i < end; i++) {
			written += GDUtf8::encode(src[i], dst + written);
		}
		at = end;
		offset = written;
	}
	return true;
}

// Own conversion exclusively on the main thread or one CPU worker, never both.
class GDTextCall : public PoolJob {
	GDCLASS(GDTextCall, PoolJob);
	Utf8Text text;
	Callable alive; // Evaluated only on the main thread.
	std::atomic_bool canceled{ false }; // Workers observe cancellation between conversion batches.
	bool working = false; // Main-thread ownership flag; the pool retains running jobs.
	bool unavailable = false;
	uint64_t queued_at = 0, started_at = 0, done_at = 0, main_at = 0; // Timings owned by the current execution phase.
	bool ran = false; // Distinguish a resumed worker slice from first submission.

	void post_step() {
		if (TextDiag::enabled()) main_at = GDClock::usec();
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDTextCall::step));
	}
	std::function<Variant(const Utf8Text &)> reply;

	void complete() {
		if (canceled.load(std::memory_order_relaxed)) { text = Utf8Text(); reply = {}; alive = Callable(); return; }
		if (!alive.is_null() && (!alive.is_valid() || !bool(alive.call()))) {
			cancel();
			emit_signal("finished", Variant());
			return;
		}
		const Variant result = unavailable ? Variant(R::err("text worker unavailable", Err::INTERRUPTED)) :
				text.error == OK ? reply(text) : Variant(R::err("cannot allocate response text", Err::LIMITED));
		text = Utf8Text();
		reply = {};
		alive = Callable();
		emit_signal("finished", result);
	}

	void offload() {
		working = true;
		if (TextDiag::enabled()) TextDiag::add(TextDiag::OFFLOADS);
		if (!submit(true)) {
			working = false;
			unavailable = true;
			Async::post(Ref<RefCounted>(this), callable_mp(this, &GDTextCall::complete));
		}
	}

	// An expired turn alone says nothing about the conversion cost: first allow useful work.
	void step() {
		if (TextDiag::enabled() && main_at) { TextDiag::add(TextDiag::MAIN_WAIT_US, GDClock::usec() - main_at); main_at = 0; }
		if (canceled.load(std::memory_order_relaxed)) return;
		if (!alive.is_null() && (!alive.is_valid() || !bool(alive.call()))) { complete(); return; }
		const bool sliced = GDScriptFunction::begin_time_slice();
		if (text.advance(GDScriptFunction::native_time_slice_deadline())) complete();
		else if (text.progressed()) offload();
		else post_step();
		if (sliced) GDScriptFunction::end_time_slice();
	}

	void cancel() {
		if (!canceled.exchange(true, std::memory_order_relaxed) && TextDiag::enabled()) TextDiag::add(TextDiag::CANCELS);
		reply = {};
		alive = Callable();
		if (!working) text = Utf8Text(); // Never free storage still owned by a worker.
	}

protected:
	void run() override { text.advance(); } // Synchronous fallback; CPU scheduling uses the saved slice below.
	bool run_slice() override {
		const bool done = canceled.load(std::memory_order_relaxed) || text.advance(GDClock::usec() + GD_SCHED_SLICE_USEC);
		if (TextDiag::enabled()) TextDiag::add(TextDiag::WORKER_US, GDClock::usec() - started_at);
		return done;
	}
	void queued() override {
		if (!TextDiag::enabled()) return;
		queued_at = GDClock::usec();
		if (ran) TextDiag::add(TextDiag::REQUEUES);
	}
	void started() override {
		if (!TextDiag::enabled()) return;
		started_at = GDClock::usec();
		TextDiag::add(TextDiag::QUEUE_WAIT_US, started_at - queued_at);
		ran = true;
	}
	void completed() override { if (TextDiag::enabled()) done_at = GDClock::usec(); }
	void finish() override {
		if (TextDiag::enabled() && done_at) TextDiag::add(TextDiag::COMPLETION_WAIT_US, GDClock::usec() - done_at);
		working = false;
		complete();
	}
	void cancel_on_shutdown() override {
		if (!canceled.exchange(true, std::memory_order_relaxed) && TextDiag::enabled()) TextDiag::add(TextDiag::CANCELS);
	}
	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("cancel"), &GDTextCall::cancel);
		ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)));
	}

public:
	void watch(const Callable &p_alive) { alive = p_alive; }
	static Signal start(Utf8Text &&p_text, std::function<Variant(const Utf8Text &)> p_reply) {
		Ref<GDTextCall> call;
		call.instantiate();
		call->text = std::move(p_text);
		call->reply = std::move(p_reply);
		if (call->text.progressed()) call->offload();
		else call->post_step();
		return Signal(call.ptr(), "finished");
	}
};

// Writer input uses the same ownership and cancellation policy as fixed response text.
Signal utf8_continue(Utf8Text &&p_text) {
	return GDTextCall::start(std::move(p_text), [](const Utf8Text &p_done) -> Variant { return p_done.bytes; });
}

// Attach request-liveness checks only to this native continuation type.
void utf8_watch(const Signal &p_signal, const Callable &p_alive) {
	GDTextCall *call = Object::cast_to<GDTextCall>(p_signal.get_object());
	if (call) call->watch(p_alive);
}

// Complete inline, retry an exhausted turn, or offload conversion that already made progress.
template <class Reply>
Variant text_reply(const String &p_text, uint64_t p_until, bool p_measure, Reply p_reply) {
	Utf8Text local;
	local.text = p_text;
	local.measure = p_measure;
	// Avoid speculative allocation when the prefix already requires wider encoding.
	uint8_t small[text_scratch];
	local.probe = !p_measure && p_text.length() > text_scratch && GDUtf8::ascii(p_text.ptr(), small, text_scratch);
	if (local.advance(p_until)) return local.error == OK ? p_reply(local) : Variant(R::err("cannot allocate response text", Err::LIMITED));
	return GDTextCall::start(std::move(local), std::move(p_reply));
}

// Encode the complete text before constructing its response value.
Variant utf8_reply(const String &p_text, uint64_t p_until, std::function<Variant(const PackedByteArray &)> p_reply) {
	return text_reply(p_text, p_until, false, [reply = std::move(p_reply)](const Utf8Text &p_text) { return reply(p_text.bytes); });
}

// Measure text with the same resumable scan used by the encoder.
Variant utf8_size(const String &p_text, uint64_t p_until) {
	return text_reply(p_text, p_until, true, [](const Utf8Text &p_text) -> Variant { return p_text.size; });
}
