/**************************************************************************/
/*  file_source.h                                                         */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Read files on workers and deliver chunks to the event loop through a bounded queue.
// Pause reading when the socket stalls instead of buffering the entire file.

#pragma once

#include "cli/sys/pool.h"
#include "cli/sys/body_source.h"
#include "cli/sys/std.h"

#include "cli/sys/native_file.h"
#include "core/os/mutex.h"
#include "core/templates/list.h"

class FileSource;

// One worker job filling the file-read queue.
class FileSourceJob : public PoolJob {
	GDCLASS(FileSourceJob, PoolJob);

	Ref<FileSource> source; // Keep the source alive while the job runs.

	friend class FileSource;

protected:
	static void _bind_methods() {}
	virtual void run() override;
	virtual void finish() override;
};

// Incremental file source; take returns only arrived chunks after open completes.
class FileSource : public GDBodySource {
	GDCLASS(FileSource, GDBodySource);

	static constexpr int CHUNK_BYTES = 32 * 1024; // Incremental transfer-buffer width.
	static constexpr int PENDING_MAX = CHUNK_BYTES; // Do not schedule another read until the socket consumes the current chunk.

	Ref<GDFile> file; // Source file accessed only by workers.
	String path; // Path opened by the worker.
	Mutex mutex; // Protect the read queue and state.
	List<PackedByteArray> queue; // Chunks in file-read order.
	int64_t pending = 0; // Bytes currently queued.
	int64_t length = 0; // Complete length obtained when opening.
	int64_t left = 0; // Bytes not yet read by the worker.
	Ref<Err> why; // Original read failure and its source metadata.
	bool running = false; // Whether a worker is running.
	bool opened = false; // Whether the file opened successfully.
	bool open_sent = false; // Whether the open result has been signalled.
	bool finished = false; // The file is closed and no more chunks will arrive.
	bool stopping = false; // Connection is gone and reading is being cancelled.
	bool read_body = true; // Prefetch body data except for HEAD.
	Callable ready; // Resume transmission after the worker appends a chunk.

	friend class FileSourceJob;

	void fill_on_worker(); // Read up to queue capacity on a worker.
	void job_finished(); // Deliver the open result on the main thread.
	void kick(); // Schedule another worker if capacity permits.

protected:
	static void _bind_methods();

public:
	// Open on a worker and signal R once the complete length is known.
	Signal open(const String &p_path, bool p_read_body = true);
	// Take an available chunk, returning false when a wait is required.
	bool take(BodyChunk &r_chunk) override;
	// Return the opened file's complete length.
	int64_t size() override;
	// Report whether reading and queue consumption are complete.
	bool done() override;
	// Return read failure detail, or empty on success.
	String error() override;
	// Close on a worker without reading the remainder after disconnect.
	void abort() override;
	// Set the internal continuation for worker progress.
	void set_ready_callback(const Callable &p_call) override { ready = p_call; }
};
