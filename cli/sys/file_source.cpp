/**************************************************************************/
/*  file_source.cpp                                                       */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Read files incrementally into a bounded queue as declared in file_source.h.

#include "cli/sys/file_source.h"
#include "cli/sys/source_error.h"

#include "core/object/class_db.h"

// Fill the read queue on a worker.
void FileSourceJob::run() {
	if (source.is_valid()) {
		source->fill_on_worker();
	}
}

// Deliver the open result on the main thread and release references.
void FileSourceJob::finish() {
	if (source.is_valid()) {
		source->job_finished();
	}
	source.unref();
}

// Begin opening a file on a worker.
Signal FileSource::open(const String &p_path, bool p_read_body) {
	const Signal signal(this, "opened");
	{
		MutexLock lock(mutex);
		path = p_path;
		read_body = p_read_body;
	}
	kick();
	return signal;
}

// Schedule another worker when queue capacity permits.
void FileSource::kick() {
	{
		MutexLock lock(mutex);
		if (running || finished || (!stopping && pending >= PENDING_MAX)) {
			return;
		}
		running = true;
	}
	Ref<FileSourceJob> job;
	job.instantiate();
	job->source = Ref<FileSource>(this);
	job->submit();
}

// Open on a worker and read only chunks needed by the consumer.
void FileSource::fill_on_worker() {
	// Close descriptors outside the mutex shared with the main thread.
	const auto finish = [&](const Ref<Err> &p_why) {
		file.unref();
		MutexLock lock(mutex);
		if (p_why.is_valid()) why = p_why;
		finished = true;
		running = false;
	};
	bool stop, need_open;
	String target;
	{
		MutexLock lock(mutex);
		stop = stopping;
		need_open = !opened;
		target = path;
	}
	if (stop) { finish(Ref<Err>()); return; }
	if (need_open) {
		SourceError::clear();
		file = GDFile::open(target, GDFile::READ);
		if (file.is_null()) { finish(SourceError::path(target, "open", GDFile::get_open_error(), "cannot open file")->get_e()); return; }
		const uint64_t size = file->get_length();
		if (file->get_error() != OK) { finish(SourceError::path(target, "stat", file->get_error(), "cannot inspect file")->get_e()); return; }
		if (size > INT64_MAX) { finish(Err::make("file is too large", Err::LIMITED)); return; }
		MutexLock lock(mutex);
		length = size;
		left = read_body ? length : 0;
		opened = true;
	}
	for (;;) {
		int want = 0;
		{
			MutexLock lock(mutex);
			stop = stopping || left == 0;
			if (!stop && pending >= PENDING_MAX) { running = false; return; }
			want = MIN<int64_t>(CHUNK_BYTES, left);
		}
		if (stop) { finish(Ref<Err>()); return; }
		PackedByteArray chunk;
		if (chunk.resize(want) != OK) { finish(Err::make("cannot reserve file read buffer", Err::LIMITED)); return; }
		SourceError::clear();
		const uint64_t got = file->get_buffer(chunk.ptrw(), want);
		if (!got || (file->get_error() != OK && file->get_error() != ERR_FILE_EOF)) { finish(SourceError::path(target, "read", file->get_error() == OK ? ERR_FILE_EOF : file->get_error(), "cannot read complete file")->get_e()); return; }
		chunk.resize(got);
		{
			MutexLock lock(mutex);
			if (!stopping) {
				queue.push_back(chunk);
				pending += got;
				left -= got;
			}
		}
	}
}

// Deliver the open result only on the first worker completion.
void FileSource::job_finished() {
	Ref<R> result;
	bool first = false;
	{
		MutexLock lock(mutex);
		if (!open_sent) {
			open_sent = true;
			first = true;
			result = why.is_null() && opened ? R::ok(length) : why.is_valid() ? R::err(why) : R::err("file read canceled", Err::INTERRUPTED);
		}
	}
	if (first) {
		emit_signal("opened", result);
	}
	if (ready.is_valid()) {
		ready.call();
	}
}

// Take the next available chunk.
bool FileSource::take(BodyChunk &r_chunk) {
	{
		MutexLock lock(mutex);
		if (queue.is_empty()) {
			return false;
		}
		r_chunk = queue.front()->get();
		queue.pop_front();
		pending -= r_chunk.size();
	}
	kick(); // Read ahead only as bytes are handed to the socket.
	return true;
}

// Return the complete file length.
int64_t FileSource::size() {
	MutexLock lock(mutex);
	return length;
}

// Report EOF only after all queued chunks are consumed.
bool FileSource::done() {
	MutexLock lock(mutex);
	return finished && queue.is_empty();
}

// Return the read failure detail.
String FileSource::error() {
	MutexLock lock(mutex);
	return why.is_valid() ? why->text() : String();
}

// Close on a worker without reading the remainder.
void FileSource::abort() {
	{
		MutexLock lock(mutex);
		stopping = true;
		queue.clear();
		pending = 0;
	}
	kick();
}

// Register the open-completion signal.
void FileSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &FileSource::abort);
	ADD_SIGNAL(MethodInfo("opened", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R")));
}
