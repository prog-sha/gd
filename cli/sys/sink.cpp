/**************************************************************************/
/*  sink.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement incremental file writing declared in sink.h.

#include "cli/sys/sink.h"

#include "cli/sys/file_job.h"
#include "cli/sys/os.h"
#include "cli/sys/source_error.h"
#include "cli/sys/wait.h"

// Capture the source error on its worker before another operation overwrites it.
static Ref<Err> sink_error(const String &p_path, const String &p_op, Error p_error) {
	Dictionary info;
	info["path"] = p_path;
	info["op"] = p_op;
	return Err::make(vformat("cannot %s %s", p_op, p_path), Err::of(SourceError::put(info, p_error)), info);
}

// Write queued chunks on a worker until the queue is empty.
void FileSinkJob::run() {
	if (sink.is_valid()) {
		sink->drain_on_worker();
	}
}

// Release references on the main thread; callers query Sink for the result.
void FileSinkJob::finish() {
	if (sink.is_valid()) {
		sink->job_finished();
	}
	sink.unref();
}

// Wake pending body processing after worker progress.
void FileSink::job_finished() {
	if (ready.is_valid()) {
		ready.call();
	}
}

// Begin opening the destination on a worker.
void FileSink::open(const String &p_path, bool p_want_hash) {
	{
		MutexLock lock(mutex);
		path = p_path;
		want_hash = p_want_hash;
	}
	kick();
}

// Accept a chunk and preserve submission order.
void FileSink::push(const PackedByteArray &p_chunk) {
	if (p_chunk.is_empty()) {
		return;
	}
	{
		MutexLock lock(mutex);
		if (closing || closed || why.is_valid()) {
			return; // Already closed or failed.
		}
		queue.push_back(p_chunk);
		queued_bytes += p_chunk.size();
	}
	kick();
}

// Signal that no more chunks will arrive.
void FileSink::close() {
	{
		MutexLock lock(mutex);
		if (closing) {
			return;
		}
		closing = true;
	}
	kick(); // Close only after writing the remainder.
}

// Schedule one worker if none is running.
// Keep exactly one writer active for each sink.
void FileSink::kick() {
	{
		MutexLock lock(mutex);
		if (running || closed) {
			return;
		}
		if (queue.is_empty() && !closing && opened) {
			return; // No data to write and no closure requested.
		}
		running = true;
	}
	Ref<FileSinkJob> job;
	job.instantiate();
	job->sink = Ref<FileSink>(this);
	job->submit();
}

// Access the file only on workers and release the shared mutex during OS waits.
void FileSink::drain_on_worker() {
	for (;;) {
		PackedByteArray chunk;
		String target;
		bool need_open = false, finish = false, digest = false;
		{
			MutexLock lock(mutex);
			finish = aborted || (opened && closing && queue.is_empty());
			need_open = !finish && !opened;
			if (need_open) { target = path; digest = want_hash; }
			else if (!finish && !queue.is_empty()) {
				chunk = queue.front()->get();
				queue.pop_front();
				queued_bytes -= chunk.size();
			} else if (!finish) {
				running = false;
				return;
			}
		}
		if (need_open) {
			SourceError::clear();
			file = GDFile::open(target, GDFile::WRITE);
			Error error = file.is_valid() ? OK : GDFile::get_open_error();
			if (error == OK && digest) { hash.instantiate(); error = hash->start(); }
			const bool ok = error == OK;
			if (ok) {
				MutexLock lock(mutex);
				opened = true;
				continue;
			}
			{ MutexLock lock(mutex); why = sink_error(target, "open", error); }
			finish = true;
		}
		if (!finish) {
			SourceError::clear();
			const uint64_t count = file->write(chunk.ptr(), chunk.size());
			Error error = file->get_error();
			Ref<Err> failed;
			if (count != uint64_t(chunk.size()) || error != OK) failed = sink_error(path, "write", error == OK ? ERR_FILE_CANT_WRITE : error);
			else if (hash.is_valid() && (error = hash->update(chunk)) != OK) failed = sink_error(path, "hash", error);
			{ MutexLock lock(mutex); written += count; if (failed.is_valid()) why = failed; }
			if (failed.is_null()) continue;
		}
		// Report close failures and delete partial files only after closing.
		SourceError::clear();
		const Error error = file.is_valid() ? file->close() : OK;
		const Ref<Err> close_error = error == OK ? Ref<Err>() : sink_error(path, "close", error);
		file.unref();
		String drop;
		{
			MutexLock lock(mutex);
			if (close_error.is_valid() && why.is_null()) why = close_error;
			queue.clear();
			queued_bytes = 0;
			closed = true;
			running = false;
			drop = drop_path;
			drop_path.clear();
		}
		if (!drop.is_empty()) Os::remove(drop);
		return;
	}
}

// Report whether writing is complete.
bool FileSink::done() {
	MutexLock lock(mutex);
	return closed;
}

// Return bytes retained in the pending queue.
int64_t FileSink::pending_bytes() {
	MutexLock lock(mutex);
	return queued_bytes;
}

// Return the failure detail.
Ref<Err> FileSink::error() {
	MutexLock lock(mutex);
	return why;
}

// Report bytes that reached the file rather than bytes merely queued.
int64_t FileSink::count() {
	MutexLock lock(mutex);
	return written;
}

// Return the written content's SHA-256 in hexadecimal.
// Read only after closed; the worker owns the hash until closure.
String FileSink::digest() {
	MutexLock lock(mutex);
	if (hash.is_null() || !closed || running) {
		return String();
	}
	const PackedByteArray raw = hash->finish();
	hash.unref();
	String out;
	for (int i = 0; i < raw.size(); i++) {
		out += String::num_uint64(raw[i] >> 4, 16) + String::num_uint64(raw[i] & 0xf, 16);
	}
	return out;
}

// Abort writing and remove the partial file after worker-side closure.
void FileSink::abort(const String &p_drop_path) {
	bool drop_later = false;
	{
		MutexLock lock(mutex);
		queue.clear();
		queued_bytes = 0;
		closing = true;
		aborted = true;
		drop_path = p_drop_path;
		if (closed && !running) {
			drop_later = !drop_path.is_empty();
			drop_path = String();
		}
	}
	if (drop_later) {
		GDFileCall::start([p_drop_path]() { return Os::remove(p_drop_path); });
	} else {
		kick(); // Delegate file cleanup to the worker.
	}
}
