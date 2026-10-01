/**************************************************************************/
/*  file_stream.cpp                                                      */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Perform regular-file operations in submission order on I/O workers.

#include "cli/sys/file_stream.h"

#include "cli/sys/file_job.h"
#include "cli/sys/task.h"
#include "cli/sys/source_error.h"

#include "core/object/class_db.h"

namespace {
constexpr int64_t IO_CHUNK = 1LL << 30; // Maximum bytes per file system call.

// Defer an immediate stream result until a waiter can connect to its signal.
Signal ready(const Ref<Err> &p_error = Ref<Err>()) {
	return GDPairCall::start([p_error]() -> VariantPair { return { Variant(), p_error }; }, false);
}
}

// Execute the target file operation on a worker.
void GDFileStreamCall::run() {
	outcome = owner.is_valid() ? owner->run_call(this) : VariantPair{ Variant(), Err::make("file stream is closed", Err::INTERRUPTED) };
}

// Deliver the result on the main thread and advance to the next operation.
void GDFileStreamCall::finish() {
	Ref<GDFileStream> keep = owner;
	Async::finish(this, SNAME("finished"), outcome.value, outcome.error);
	if (keep.is_valid()) {
		keep->call_finished(this);
	}
	data.clear();
	owner.unref();
	outcome = VariantPair();
}

// Register the script completion signal.
void GDFileStreamCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}

// Convert file failures to the shared Err type.
VariantPair GDFileStream::fail_file(const String &p_path, const String &p_action, Error p_error, const Variant &p_value) {
	Dictionary info;
	info["path"] = p_path;
	info["operation"] = p_action;
	Ref<Err> error = Err::make(vformat("cannot %s %s", p_action, p_path), Err::of(SourceError::put(info, p_error)), info);
	return { p_value, error->with_partial(p_value) };
}

// Execute one operation on a worker.
VariantPair GDFileStream::run_call(GDFileStreamCall *p_call) {
	SourceError::clear();
	switch (p_call->op) {
		case GDFileStreamCall::OPEN: {
			GDFile::Mode flags = GDFile::READ;
			if (p_call->mode == "write") {
				flags = GDFile::WRITE;
			} else if (p_call->mode == "append") {
				flags = GDFile::APPEND;
			} else if (p_call->mode == "read_write") {
				flags = GDFile::READ_WRITE;
			} else if (p_call->mode != "read") {
				return { Variant(), Err::make("file stream mode must be read, write, append, or read_write", Err::INVALID_DATA) };
			}
			file = GDFile::open(p_call->path, flags);
			if (file.is_null()) {
				return fail_file(p_call->path, "open", GDFile::get_open_error());
			}
			opened.set();
			return { Ref<GDFileStream>(this), Variant() };
		}
		case GDFileStreamCall::READ: {
			if (file.is_null()) {
				return { Variant(), Err::make("file stream is closed", Err::INTERRUPTED) };
			}
			PackedByteArray out;
			const int64_t want = MIN(p_call->number, IO_CHUNK);
			if (out.resize(want) != OK) {
				return { Variant(), Err::make("cannot allocate file read buffer", Err::LIMITED) };
			}
			const int64_t got = file->get_buffer(out.ptrw(), want);
			out.resize(got);
			const Error error = file->get_error();
			if (error != OK && error != ERR_FILE_EOF) {
				return fail_file(file->get_path(), "read", error, out);
			}
			return { out, Variant() };
		}
		case GDFileStreamCall::WRITE: {
			if (file.is_null()) {
				return { Variant(), Err::make("file stream is closed", Err::INTERRUPTED) };
			}
			const int64_t written = file->write(p_call->data.ptr(), p_call->data.size());
			if (file->get_error() != OK) {
				return fail_file(file->get_path(), "write", file->get_error(), written);
			}
			return { written, Variant() };
		}
		case GDFileStreamCall::SEEK: {
			if (file.is_null()) {
				return { Variant(), Err::make("file stream is closed", Err::INTERRUPTED) };
			}
			file->seek(p_call->number);
			const Error error = file->get_error();
			return error == OK || error == ERR_FILE_EOF ? VariantPair() : fail_file(file->get_path(), "seek", error);
		}
		case GDFileStreamCall::CLOSE:
			if (file.is_valid()) {
				const String path = file->get_path();
				const Error error = file->close();
				file.unref();
				opened.clear();
				if (error != OK) return fail_file(path, "close", error);
			}
			opened.clear();
			return {};
	}
	return { Variant(), Err::make("unknown file stream operation", Err::INVALID_DATA) };
}

// Release implicitly closed files on a worker without blocking the main thread.
GDFileStream::~GDFileStream() {
	Ref<GDFile> held = file;
	file.unref();
	if (held.is_valid()) {
		GDPairCall::start([held]() mutable -> VariantPair {
			held.unref();
			return {};
		}, false);
	}
}

// Queue operations in arrival order and submit only the head to a worker.
Signal GDFileStream::enqueue(GDFileStreamCall::Op p_op, const PackedByteArray &p_data, int64_t p_number) {
	Ref<GDFileStreamCall> call;
	call.instantiate();
	call->owner = Ref<GDFileStream>(this);
	call->op = p_op;
	call->data = p_data;
	call->number = p_number;
	const Signal signal(call.ptr(), "finished");
	const bool first = calls.is_empty();
	calls.push_back(call);
	if (first) {
		call->submit(false);
	}
	return signal;
}

// Remove the completed head and submit the next operation.
void GDFileStream::call_finished(GDFileStreamCall *p_call) {
	if (calls.is_empty() || calls.front()->get().ptr() != p_call) {
		return;
	}
	calls.pop_front();
	if (calls.is_empty() || calls.front()->get()->submit(false)) {
		return;
	}
	// Complete remaining waiters if the worker pool stops accepting work during shutdown.
	closing = true;
	opened.clear();
	file.unref();
	const Ref<Err> failed = Err::make("file stream worker is shutting down", Err::INTERRUPTED);
	while (!calls.is_empty()) {
		Ref<GDFileStreamCall> pending = calls.front()->get();
		calls.pop_front();
		Async::finish(pending.ptr(), SNAME("finished"), Variant(), failed);
		pending->data.clear();
		pending->owner.unref();
		pending->outcome = VariantPair();
	}
}

// Queue file opening as the first operation.
Signal GDFileStream::open(const String &p_path, const String &p_mode) {
	Ref<GDFileStream> stream;
	stream.instantiate();
	Ref<GDFileStreamCall> call;
	call.instantiate();
	call->owner = stream;
	call->op = GDFileStreamCall::OPEN;
	call->path = p_path;
	call->mode = p_mode;
	const Signal signal(call.ptr(), "finished");
	stream->calls.push_back(call);
	call->submit(false);
	return signal;
}

// Read up to the requested byte count.
Signal GDFileStream::read(int64_t p_max) {
	if (p_max < 0) {
		return ready(Err::make("file stream read size must not be negative", Err::INVALID_DATA));
	}
	if (closing) {
		return ready(Err::make("file stream is closed", Err::INTERRUPTED));
	}
	return enqueue(GDFileStreamCall::READ, PackedByteArray(), p_max);
}

// Write all supplied bytes.
Signal GDFileStream::write(const PackedByteArray &p_data) {
	if (closing) {
		return ready(Err::make("file stream is closed", Err::INTERRUPTED));
	}
	return enqueue(GDFileStreamCall::WRITE, p_data);
}

// Set the next position as a byte offset from the beginning.
Signal GDFileStream::seek(int64_t p_offset) {
	if (p_offset < 0) {
		return ready(Err::make("file stream offset must not be negative", Err::INVALID_DATA));
	}
	if (closing) {
		return ready(Err::make("file stream is closed", Err::INTERRUPTED));
	}
	return enqueue(GDFileStreamCall::SEEK, PackedByteArray(), p_offset);
}

// Close after all preceding operations.
Signal GDFileStream::close() {
	if (closing) {
		return ready();
	}
	closing = true;
	return enqueue(GDFileStreamCall::CLOSE);
}

// Expose Reader, Writer, and Closer methods to scripts.
void GDFileStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("read", "max"), &GDFileStream::read, DEFVAL(32768));
	ClassDB::bind_method(D_METHOD("read_async", "max"), &GDFileStream::read_async, DEFVAL(32768));
	ClassDB::bind_method(D_METHOD("write", "data"), &GDFileStream::write);
	ClassDB::bind_method(D_METHOD("write_async", "data"), &GDFileStream::write_async);
	ClassDB::bind_method(D_METHOD("seek", "offset"), &GDFileStream::seek);
	ClassDB::bind_method(D_METHOD("seek_async", "offset"), &GDFileStream::seek_async);
	ClassDB::bind_method(D_METHOD("close"), &GDFileStream::close);
	ClassDB::bind_method(D_METHOD("close_async"), &GDFileStream::close_async);
	ClassDB::bind_method(D_METHOD("is_open"), &GDFileStream::is_open);
	ADD_AWAIT("read", "Pair:PackedByteArray");
	ADD_AWAIT("read_async", "Pair:PackedByteArray");
	ADD_AWAIT("write", "Pair:int");
	ADD_AWAIT("write_async", "Pair:int");
	ADD_AWAIT("seek", "Pair:Variant");
	ADD_AWAIT("seek_async", "Pair:Variant");
	ADD_AWAIT("close", "Pair:Variant");
	ADD_AWAIT("close_async", "Pair:Variant");
	ADD_AUTO_WAIT("read");
	ADD_AUTO_WAIT("write");
	ADD_AUTO_WAIT("seek");
	ADD_AUTO_WAIT("close");
}
