// Produce response bytes through a Writer with transport-driven backpressure.
#pragma once
#include "cli/data/utf8.h"
#include "cli/sys/body_source.h"
#include <memory>
#include "cli/sys/task.h"
#include "core/templates/list.h"

// Keep one byte write alive until transport completion or cancellation.
class GDWebWriteCall : public RefCounted {
	GDCLASS(GDWebWriteCall, RefCounted);
	friend class GDWebWriter;
	int64_t end = 0, count = 0; // Completion position and byte count within the complete response.
	Ref<R> result; // Single completion result, including a partial byte count.
	bool waiting = false; // Publish a signal only after the caller actually suspends.
	void settle(const Ref<R> &p_result); // Defer delivery outside the transport stack.
	void deliver(); // Resume the waiting producer exactly once.
protected:
	static void _bind_methods();
};

// Connect a lazy producer to a response without buffering its complete body.
class GDWebWriter : public GDBodySource {
	GDCLASS(GDWebWriter, GDBodySource);
	Callable producer, ready, drain; // Producer, scheduled readiness, and immediate transport progress.
	Signal wait; // Suspended producer completion.
	Ref<RefCounted> hold; // Suspended producer kept alive until completion or cancellation.
	Ref<GDAsyncContext> ctx; // Request cancellation while response production is active.
	List<Ref<GDWebWriteCall>> queue; // Ordered barriers waiting for transport progress.
	// Separate unknown-length text input from byte-position completion barriers.
	struct Input {
		PackedByteArray bytes; // Immutable byte range retained until preceding text is ready.
		int64_t offset = 0, count = 0; // Byte selection, or zero until text measurement completes.
		std::shared_ptr<Utf8Text> text; // Exclusive conversion progress, empty for byte writes.
		Ref<Err> error; // Preserve conversion failure categories before publishing any bytes.
		Ref<GDWebWriteCall> call; // Completion allocated only for deferred input.
	};
	Signal conversion; // Only the head input may own suspended conversion.
	Ref<RefCounted> conversion_hold; // Retain queued or running conversion until completion.
	void converted(const Variant &p_value); // Publish complete conversion to the head input on the main thread.
	void cancel_conversion(); // Detach completion before invalidating worker ownership.
	List<Input> input; // Accepted operations in producer order, including empty flush writes.
	bool pump_posted = false, pumping = false; // Coalesce work and exclude transport reentry.
	void post_pump(); // Resume only runnable conversion; transport acknowledgements release backpressure.
	void pump(); // Publish one complete operation at a time within the shared turn.
	Variant enqueue(Input &&p_input); // Retain input and return its eventual completion signal.
	Variant accept(const PackedByteArray &p_bytes, int64_t p_offset, int64_t p_count, const Ref<GDWebWriteCall> &p_call = Ref<GDWebWriteCall>()); // Commit measured bytes using normal barriers.
	void check_length(); // Check producer EOF only after all unknown lengths are resolved.
	BodyChunk buffered; // Accepted ranges waiting for the next transport batch.
	int64_t consumed = 0, sending = 0; // Acknowledged bytes and the current transport batch size.
	int64_t length = -1, total = 0; // Declared length and accepted input count.
	String why; // Terminal producer, length, or transport error.
	bool batch = false; // A handed-off batch may contain only a header flush barrier.
	bool started = false, ended = false, posted = false; // Producer lifetime and coalesced notification.
	void step(); // Start producer code only when HTTP requests the first body chunk.
	void received(const Variant &p_value); // Resolve producer suspension and its final result.
	void post(); // Schedule a transport notification outside the current call stack.
	void notify(); // Wake the transport when producer output or EOF becomes available.
	void discard(int64_t p_sent); // Release producer state without ending its request context.
protected:
	static void _bind_methods();
public:
	static Dictionary reply(const Callable &p_producer, int64_t p_length, const String &p_type, int64_t p_status); // Construct a lazy response.
	Variant write(const PackedByteArray &p_bytes, int64_t p_offset = 0, int64_t p_count = -1); // Accept a range and wait when transport backpressure applies.
	Signal write_async(const PackedByteArray &p_bytes, int64_t p_offset = 0, int64_t p_count = -1); // Expose a completion signal even when the transport is already writable.
	Variant write_text(const String &p_text, int64_t p_offset = 0, int64_t p_count = -1); // Encode a character range and return accepted UTF-8 bytes with normal write backpressure.
	Signal write_text_async(const String &p_text, int64_t p_offset = 0, int64_t p_count = -1); // Expose text write completion as a signal.
	Variant flush() { return write(PackedByteArray()); } // Wait behind preceding writes without ending the response.
	Signal flush_async() { return write_async(PackedByteArray()); } // Expose an explicit asynchronous flush completion.
	bool take(BodyChunk &r_chunk) override;
	int64_t size() override { return length; }
	bool last() override { return ended && !pumping && input.is_empty() && buffered.is_empty() && why.is_empty(); } // Finish framing only after successful producer completion.
	bool done() override { return ended && !pumping && input.is_empty() && queue.is_empty() && buffered.is_empty() && !batch; }
	String error() override { return why; }
	void abort() override { abort_sent(0); }
	void suppress() override { discard(0); }
	void finish() override;
	void abort_sent(int64_t p_sent) override;
	void set_context(const Ref<GDAsyncContext> &p_ctx) override { ctx = p_ctx; }
	void set_ready_callback(const Callable &p_call) override { ready = p_call; if (!p_call.is_valid()) drain = Callable(); }
	void set_write_callback(const Callable &p_call) override { drain = p_call; }
	bool watch_disconnect() const override { return true; }
	void peer_closed() override;
};
