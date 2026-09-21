// Compress byte writes incrementally while preserving destination backpressure.
#pragma once
#include "cli/sys/std.h"
#include "core/templates/list.h"

class GDGzipWriter;

// Retain one ordered operation through compression and destination completion.
class GDGzipCall : public RefCounted {
	GDCLASS(GDGzipCall, RefCounted);
	friend class GDGzipWriter;
	enum Mode { WRITE, FLUSH, CLOSE, RESET }; // Operations sharing the same ordered stream.
	Mode mode = WRITE; // Requested operation.
	PackedByteArray input, output; // Owned input and one compressed transfer buffer.
	Dictionary header; // Immutable metadata snapshot for the first member operation.
	Ref<RefCounted> target; // Destination installed by reset.
	Ref<GDGzipWriter> owner; // Retain the stream until this operation finishes.
	int64_t at = 0; // Input bytes accepted by the compressor.
	bool done = false; // Worker has completed the operation after output delivery.
	String failure; // Worker-only compression error delivered to the runtime.
protected:
	static void _bind_methods();
};

// Adapt any byte Writer without closing the underlying destination.
class GDGzipWriter : public RefCounted {
	GDCLASS(GDGzipWriter, RefCounted);
	struct State;
	State *state = nullptr; // Compressor accessed only by the active CPU worker.
	Ref<RefCounted> target; // Destination providing write(bytes) and an integer result.
	List<Ref<GDGzipCall>> calls; // Pending operations in submission order.
	Signal wait; // Destination write completion currently awaited.
	Ref<RefCounted> hold; // Signal or suspended script owner retained during delivery.
	Ref<Err> failure; // First stream failure, preserved until reset.
	Dictionary header; // Optional name, comment, extra, modification time, and OS metadata.
	int level = -1; // Standard compression level, or -2 for Huffman-only coding.
	bool busy = false, posted = false, closed = false; // Worker ownership, queued continuation, and completed close.
	Signal enqueue(GDGzipCall::Mode p_mode, const PackedByteArray &p_data = PackedByteArray(), const Ref<RefCounted> &p_target = Ref<RefCounted>()); // Submit an ordered operation.
	void schedule(); // Post one continuation without extending a callback stack.
	void step(); // Select the next operation or start another output slice.
	void compress(const Ref<GDGzipCall> &p_call); // Run raw compression and gzip framing on a CPU worker.
	void compressed(const Ref<R> &p_result); // Hand one compressed slice to the destination on the runtime.
	void written(const Variant &p_result); // Resolve destination completion and propagate short writes.
	bool park(const Variant &p_value); // Retain a signal or suspended destination function.
	void finish(); // Publish one result after removing its queue entry.
protected:
	static void _bind_methods();
public:
	Dictionary get_header() const { return header; } // Read metadata to configure before the first write, flush, or close.
	void set_header(const Dictionary &p_header) { header = p_header; } // Set metadata for the next unstarted member.
	~GDGzipWriter(); // Release the native compressor after active jobs release ownership.
	static Ref<R> create(const Ref<RefCounted> &p_target, int64_t p_level); // Validate a destination and compression level.
	Signal write(const PackedByteArray &p_bytes) { return enqueue(GDGzipCall::WRITE, p_bytes); } // Compress bytes and return the accepted input count.
	Signal flush() { return enqueue(GDGzipCall::FLUSH); } // Flush compressed output without ending the member.
	Signal close() { return enqueue(GDGzipCall::CLOSE); } // Finish the member without closing the destination.
	Signal reset(const Ref<RefCounted> &p_target) { return enqueue(GDGzipCall::RESET, PackedByteArray(), p_target); } // Discard stream state and reuse the compressor at the same level.
};
