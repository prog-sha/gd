/**************************************************************************/
/*  file_stream.h                                                        */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Access regular files incrementally through shared Reader, Writer, and Closer interfaces.

#include "cli/sys/pool.h"
#include "cli/sys/std.h"

#include "cli/sys/native_file.h"
#include "core/templates/list.h"

class GDFileStream;

// Carry one file operation to an I/O worker.
class GDFileStreamCall : public PoolJob {
	GDCLASS(GDFileStreamCall, PoolJob);

public:
	enum Op {
		OPEN,
		READ,
		WRITE,
		SEEK,
		CLOSE,
	};

private:
	Ref<GDFileStream> owner; // Keep the target alive through completion.
	Op op = READ;
	PackedByteArray data; // Bytes to write.
	String path; // Path to open.
	String mode; // Open mode.
	int64_t number = 0; // Read length or seek offset.
	Ref<R> outcome; // Worker-produced result.

	friend class GDFileStream;

protected:
	static void _bind_methods();
	void run() override;
	void finish() override;
};

// Read and write one regular file sequentially in submission order.
class GDFileStream : public RefCounted {
	GDCLASS(GDFileStream, RefCounted);

	Ref<GDFile> file; // File accessed only by workers.
	List<Ref<GDFileStreamCall>> calls; // Operations in arrival order.
	SafeFlag opened; // Whether opening succeeded.
	bool closing = false; // Whether close has been accepted.

	Signal enqueue(GDFileStreamCall::Op p_op, const PackedByteArray &p_data = PackedByteArray(), int64_t p_number = 0);
	Ref<R> run_call(GDFileStreamCall *p_call); // Perform the file operation on a worker.
	void call_finished(GDFileStreamCall *p_call); // Submit the next operation to a worker.
	static Ref<R> fail_file(const String &p_path, const String &p_action, Error p_error, const Variant &p_value = Variant());

	friend class GDFileStreamCall;

protected:
	static void _bind_methods();

public:
	~GDFileStream();
	// Open on a worker and return the stream.
	static Signal open(const String &p_path, const String &p_mode);
	// Read up to the requested bytes; an empty success result denotes EOF.
	Signal read(int64_t p_max = 32768);
	Signal read_async(int64_t p_max = 32768) { return read(p_max); }
	// Write all supplied bytes and return the written count.
	Signal write(const PackedByteArray &p_data);
	Signal write_async(const PackedByteArray &p_data) { return write(p_data); }
	// Move the next I/O position to a byte offset from the start.
	Signal seek(int64_t p_offset);
	Signal seek_async(int64_t p_offset) { return seek(p_offset); }
	// Close the file after preceding operations.
	Signal close();
	Signal close_async() { return close(); }
	bool is_open() const { return opened.is_set() && !closing; }
};
