/**************************************************************************/
/*  sink.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Append incoming data to files with writes outside the event loop.
//
// Accept chunks on the event loop and delegate blocking writes to worker threads.
// Write in submission order without concurrent access to one GDFile.
// Keep one worker active per sink, starting each chunk after the preceding write.
//
// Run partial-file removal on the worker after closing.
// Keep permission checks untrusted with thread-local depth zero.

#pragma once

#include "cli/sys/pool.h"
#include "cli/sys/std.h"

#include "cli/data/digest.h"
#include "cli/sys/native_file.h"
#include "core/os/mutex.h"
#include "core/templates/list.h"

class FileSink;

// One worker job writing queued chunks.
class FileSinkJob : public PoolJob {
	GDCLASS(FileSinkJob, PoolJob);

	Ref<FileSink> sink; // Retain the destination while the job runs.

	friend class FileSink;

protected:
	static void _bind_methods() {}
	virtual void run() override;
	virtual void finish() override;
};

// Ordered file sink accepting chunks through push and termination through close.
class FileSink : public RefCounted {
	GDCLASS(FileSink, RefCounted);

	Ref<GDFile> file; // Destination file accessed only by workers.
	Ref<GDDigest> hash; // Optional SHA-256 of written content.
	String path; // Destination path opened by the worker.
	Mutex mutex; // Protect the queue and close state.
	List<PackedByteArray> queue; // Chunks in submission order.
	int64_t queued_bytes = 0; // Bytes retained in the pending queue.
	bool running = false; // One active worker preserving write order.
	bool opened = false; // Whether the worker opened the destination.
	bool want_hash = false; // Whether to compute SHA-256 on the worker.
	bool aborted = false; // Discard the remainder and close after cancellation.
	bool closing = false; // Whether closure has been requested.
	bool closed = false; // Whether closure is complete.
	Ref<Err> why; // Original failure with operation and source metadata.
	int64_t written = 0; // Bytes committed before completion or failure.
	String drop_path; // Path removed after closure on cancellation.
	Callable ready; // Main-thread continuation after worker progress.

	friend class FileSinkJob;

	// Write pending chunks on a worker until the queue empties.
	void drain_on_worker();
	// Schedule one worker when idle; otherwise do nothing.
	void kick();
	void job_finished(); // Notify operations waiting for worker completion.

public:
	// Begin opening the destination on a worker.
	void open(const String &p_path, bool p_want_hash);
	// Accept chunks in the order they must be written.
	void push(const PackedByteArray &p_chunk);
	// Signal end of input; done() becomes true after flushing and closing.
	void close();
	// Report completion, possible only after close was requested.
	bool done();
	// Return pending bytes for backpressure decisions.
	int64_t pending_bytes();
	// Return failure detail, or empty on success.
	Ref<Err> error();
	int64_t count(); // Return committed bytes, including partial writes.
	// Return SHA-256 in hexadecimal, or empty when hashing was not requested.
	String digest();
	// Abort and delete the partial file after writing has stopped.
	// Caller-side deletion could race a worker still opening the destination.
	void abort(const String &p_drop_path = String());
	// Set the internal continuation for worker progress.
	void set_ready_callback(const Callable &p_call) { ready = p_call; }
};
