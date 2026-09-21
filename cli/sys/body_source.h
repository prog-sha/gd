// Supply response chunks on demand without requiring the complete body in memory.
#pragma once

#include "core/object/ref_counted.h"
#include "cli/sys/body_chunk.h"
class GDAsyncContext;

// Shared consumer contract for file and callback-driven response bodies.
class GDBodySource : public RefCounted {
	GDCLASS(GDBodySource, RefCounted);

protected:
	static void _bind_methods() {}

public:
	virtual bool take(BodyChunk &r_chunk) = 0; // Transfer one available chunk to the consumer.
	virtual int64_t size() = 0; // Return the declared length, or -1 when unknown.
	virtual bool last() { return done() && error().is_empty(); } // Identify the final transferred chunk without acknowledging it.
	virtual bool done() = 0; // Report EOF after the last chunk has been consumed.
	virtual String error() = 0; // Report a terminal failure without disguising it as EOF.
	virtual void abort() = 0; // Release pending work after consumer cancellation.
	virtual void suppress() { abort(); } // Omit body production while the consumer retains response completion.
	virtual void finish() {} // Notify successful completion after all response framing reaches the transport.
	virtual void abort_sent(int64_t p_sent) { abort(); } // Report partial consumption when the source supplies a byte Writer.
	virtual void set_ready_callback(const Callable &p_call) = 0; // Wake the consumer when progress is possible.
	virtual void set_write_callback(const Callable &p_call) {} // Try immediate transport progress without resuming a producer recursively.
	virtual bool watch_disconnect() const { return false; } // Observe peer cancellation while user code is suspended.
	virtual void peer_closed() {} // Notify user code of a read-side FIN without forbidding remaining writes.
	virtual void set_context(const Ref<GDAsyncContext> &p_ctx) {} // Preserve producer cancellation through response transforms.
};
