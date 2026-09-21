// Frame worker-compressed bytes as gzip and serialize writes through the destination contract.
#include "cli/data/gzip_writer.h"
#include "cli/sys/file_job.h"
#include "cli/sys/task.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "modules/gdscript/gdscript_function.h"
#include "thirdparty/zlib/zlib.h"

struct GDGzipWriter::State {
	z_stream stream{}; // Reusable raw deflate dictionary and bit buffer.
	uint32_t checksum = 0, size = 0; // Gzip CRC and uncompressed size modulo the wire width.
	bool initialized = false, header = false; // Compression allocation and emitted member header.
};

// Release compression state only when no job can access it.
GDGzipWriter::~GDGzipWriter() {
	if (state) {
		if (state->initialized) deflateEnd(&state->stream);
		memdelete(state);
	}
}

// Reject invalid destinations before creating an unusable stream.
Ref<R> GDGzipWriter::create(const Ref<RefCounted> &p_target, int64_t p_level) {
	if (p_level < -2 || p_level > 9) return R::err("invalid gzip level", Err::INVALID_DATA);
	if (p_target.is_null() || !p_target->has_method("write")) return R::err("gzip destination must provide write(bytes)", Err::INVALID_DATA);
	Ref<GDGzipWriter> writer;
	writer.instantiate();
	writer->target = p_target;
	writer->level = int(p_level);
	return R::ok(writer);
}

// Preserve call order without imposing a request-count or byte-count rejection cap.
Signal GDGzipWriter::enqueue(GDGzipCall::Mode p_mode, const PackedByteArray &p_data, const Ref<RefCounted> &p_target) {
	Ref<GDGzipCall> call;
	call.instantiate();
	call->mode = p_mode;
	call->input = p_data;
	call->target = p_target;
	call->owner = Ref<GDGzipWriter>(this);
	calls.push_back(call);
	schedule();
	return Signal(call.ptr(), "finished");
}

// Post only one pending runtime continuation.
void GDGzipWriter::schedule() {
	if (posted || busy || !wait.is_null()) return;
	posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDGzipWriter::step));
}

// Start work only after the preceding compressed bytes have reached their destination.
void GDGzipWriter::step() {
	posted = false;
	if (busy || !wait.is_null() || calls.is_empty()) return;
	const Ref<GDGzipCall> call = calls.front()->get();
	if (call->mode == GDGzipCall::RESET) {
		if (call->target.is_null() || call->target.ptr() == this || !call->target->has_method("write")) {
			failure = Err::make("invalid gzip reset destination", Err::INVALID_DATA);
			finish();
			return;
		}
		failure.unref();
		closed = false;
		target = call->target;
		header = Dictionary();
	} else if (failure.is_valid() || closed) {
		if (failure.is_null() && call->mode == GDGzipCall::WRITE) failure = Err::make("gzip writer is closed", Err::INVALID_DATA);
		finish();
		return;
	}
	if (call->mode != GDGzipCall::RESET && (!state || !state->header)) call->header = header.duplicate(true);
	busy = true;
	const Ref<GDGzipWriter> keep(this);
	Signal done = GDFileCall::start([keep, call]() -> Ref<R> { keep->compress(call); return R::ok(); }, true);
	done.connect(callable_mp(this, &GDGzipWriter::compressed), Object::CONNECT_ONE_SHOT);
}

// Produce one output slice, retaining the remainder in the compressor rather than collecting the body.
void GDGzipWriter::compress(const Ref<GDGzipCall> &p_call) {
	if (!state) state = memnew(State);
	p_call->output = PackedByteArray();
	if (p_call->mode == GDGzipCall::RESET) {
		if (state->initialized && deflateReset(&state->stream) != Z_OK) p_call->failure = "cannot reset gzip compressor";
		state->checksum = state->size = 0;
		state->header = false;
		p_call->done = true;
		return;
	}
	if (!state->header) {
		const Dictionary &meta = p_call->header;
		for (const Variant &key : meta.keys()) {
			const String field = key;
			const Variant value = meta[key];
			const Variant::Type type = field == "name" || field == "comment" ? Variant::STRING : field == "extra" ? Variant::PACKED_BYTE_ARRAY : field == "mod_time" || field == "os" ? Variant::INT : Variant::NIL;
			if (type == Variant::NIL || value.get_type() != type) { p_call->failure = "invalid gzip header field"; return; }
		}
		const String name = meta.get("name", String()), comment = meta.get("comment", String());
		const PackedByteArray extra = meta.get("extra", PackedByteArray());
		const int64_t os = meta.get("os", 255), modified = meta.get("mod_time", 0);
		if (os < 0 || os > 255 || extra.size() > 65535) { p_call->failure = "gzip header exceeds its wire field width"; return; }
		for (const String &value : {name, comment}) {
			for (int64_t i = 0; i < value.length(); ++i) if (value[i] == 0 || value[i] > 255) { p_call->failure = "gzip header text must be non-NUL Latin-1"; return; }
		}
		const bool has_extra = meta.has("extra");
		const int64_t size = 10 + (has_extra ? extra.size() + 2 : 0) + (name.is_empty() ? 0 : int64_t(name.length()) + 1) + (comment.is_empty() ? 0 : int64_t(comment.length()) + 1);
		if (p_call->output.resize(size) != OK) { p_call->failure = "cannot allocate gzip header"; return; }
		uint8_t *out = p_call->output.ptrw();
		const uint8_t fixed[10] = {31, 139, 8, uint8_t((has_extra ? 4 : 0) | (name.is_empty() ? 0 : 8) | (comment.is_empty() ? 0 : 16)), 0, 0, 0, 0, uint8_t(level == 9 ? 2 : level == 1 ? 4 : 0), uint8_t(os)}; // Fixed framing fields; optional fields follow in wire order.
		memcpy(out, fixed, sizeof(fixed));
		for (unsigned byte = 0; byte < 4; ++byte) out[4 + byte] = uint8_t((modified > 0 ? uint32_t(modified) : 0) >> (8 * byte));
		int64_t at = 10;
		if (has_extra) {
			out[at++] = uint8_t(extra.size());
			out[at++] = uint8_t(extra.size() >> 8);
			if (!extra.is_empty()) memcpy(out + at, extra.ptr(), extra.size());
			at += extra.size();
		}
		for (const String &value : {name, comment}) if (!value.is_empty()) {
			for (int64_t i = 0; i < value.length(); ++i) out[at++] = uint8_t(value[i]);
			out[at++] = 0;
		}
		state->header = true;
		return;
	}
	if (!state->initialized) {
		state->initialized = deflateInit2(&state->stream, level == -2 ? Z_DEFAULT_COMPRESSION : level, Z_DEFLATED, -MAX_WBITS, 8, level == -2 ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY) == Z_OK;
		if (!state->initialized) { p_call->failure = "cannot initialize gzip compressor"; return; }
	}
	constexpr int CHUNK = 32768; // Output transfer unit, never an input or response-length limit.
	if (p_call->output.resize(CHUNK) != OK) { p_call->failure = "cannot allocate gzip output"; return; }
	z_stream &stream = state->stream;
	stream.next_out = p_call->output.ptrw();
	stream.avail_out = CHUNK;
	const int mode = p_call->mode == GDGzipCall::CLOSE ? Z_FINISH : p_call->mode == GDGzipCall::FLUSH ? Z_SYNC_FLUSH : Z_NO_FLUSH;
	int result = Z_OK;
	do {
		const uInt count = uInt(MIN(p_call->input.size() - p_call->at, int64_t(UINT_MAX)));
		const uint8_t *input = count ? p_call->input.ptr() + p_call->at : nullptr;
		stream.next_in = const_cast<Bytef *>(input);
		stream.avail_in = count;
		result = deflate(&stream, mode);
		const uInt used = count - stream.avail_in;
		if (used) state->checksum = uint32_t(crc32(state->checksum, input, used));
		state->size += used;
		p_call->at += used;
		if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) { p_call->failure = "gzip compression failed"; break; }
		p_call->done = result == Z_STREAM_END || (mode != Z_FINISH && p_call->at == p_call->input.size() && (mode == Z_NO_FLUSH || stream.avail_out));
	} while (!p_call->done && stream.avail_out);
	const int produced = CHUNK - stream.avail_out;
	stream.next_in = stream.next_out = nullptr;
	p_call->output.resize(produced);
	if (result == Z_STREAM_END) {
		if (p_call->output.resize(produced + 8) != OK) { p_call->failure = "cannot allocate gzip trailer"; return; }
		uint8_t *tail = p_call->output.ptrw() + produced;
		for (unsigned byte = 0; byte < 4; ++byte) {
			tail[byte] = uint8_t(state->checksum >> (8 * byte));
			tail[byte + 4] = uint8_t(state->size >> (8 * byte));
		}
	}
}

// Invoke destination code only on the runtime that owns its script state.
void GDGzipWriter::compressed(const Ref<R> &p_result) {
	busy = false;
	const Ref<GDGzipCall> call = calls.front()->get();
	if (p_result.is_null() || !p_result->get_ok() || !call->failure.is_empty()) {
		failure = p_result.is_valid() && !p_result->get_ok() ? p_result->get_e() : Err::make(call->failure.is_empty() ? "gzip worker failed" : call->failure, Err::INVALID_DATA);
		finish();
		return;
	}
	if (call->output.is_empty()) { if (call->done) finish(); else schedule(); return; }
	const Variant bytes = call->output;
	const Variant *args[] = {&bytes};
	Callable::CallError err;
	const bool sliced = GDScriptFunction::begin_time_slice();
	GDScriptFunction::SuspendableCall suspendable;
	const Variant result = target->callp("write", args, 1, err);
	if (sliced) GDScriptFunction::end_time_slice();
	written(err.error == Callable::CallError::CALL_OK ? result : Variant(R::err("gzip destination write failed", Err::INVALID_DATA)));
}

// Connect one suspended destination write while retaining its owner.
bool GDGzipWriter::park(const Variant &p_value) {
	if (p_value.get_type() == Variant::SIGNAL) wait = p_value;
	else if (p_value.get_type() == Variant::OBJECT && Object::cast_to<GDScriptFunctionState>(p_value.get_validated_object())) wait = Signal(p_value.get_validated_object(), "completed");
	if (wait.is_null()) return false;
	hold = Ref<RefCounted>(Object::cast_to<RefCounted>(wait.get_object()));
	if (wait.connect(callable_mp(this, &GDGzipWriter::written), Object::CONNECT_ONE_SHOT) != OK) {
		wait = Signal();
		hold.unref();
		failure = Err::make("cannot await gzip destination", Err::INVALID_DATA);
		finish();
	}
	return true;
}

// Preserve destination failures and reject successful short writes before consuming another slice.
void GDGzipWriter::written(const Variant &p_result) {
	wait = Signal();
	hold.unref();
	if (park(p_result)) return;
	const Ref<GDGzipCall> call = calls.front()->get();
	const Ref<R> result = p_result.get_type() == Variant::OBJECT ? Ref<R>(p_result) : Ref<R>();
	const Variant value = result.is_valid() ? result->get_v() : p_result;
	if (result.is_valid() && !result->get_ok()) failure = result->get_e();
	else if (value.get_type() != Variant::INT || int64_t(value) != call->output.size()) failure = Err::make("gzip destination returned a short or invalid write", Err::INVALID_DATA);
	call->output = PackedByteArray();
	if (failure.is_valid() || call->done) finish();
	else schedule();
}

// Remove ownership before resuming user code, which may enqueue another operation immediately.
void GDGzipWriter::finish() {
	const Ref<GDGzipWriter> keep(this);
	const Ref<GDGzipCall> call = calls.front()->get();
	calls.pop_front();
	if (call->mode == GDGzipCall::CLOSE && failure.is_null()) closed = true;
	const Variant count = call->mode == GDGzipCall::WRITE ? Variant(call->at) : Variant();
	const Ref<R> result = failure.is_valid() ? R::err(failure, Err::NONE, count) : R::ok(count);
	call->owner.unref();
	call->emit_signal("finished", result);
	schedule();
}

// Expose the common completion signal for every ordered operation.
void GDGzipCall::_bind_methods() { ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_RESOURCE_TYPE, "R"))); }

// Publish synchronous-looking Writer operations and explicit signals for composition.
void GDGzipWriter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_header"), &GDGzipWriter::get_header);
	ClassDB::bind_method(D_METHOD("set_header", "header"), &GDGzipWriter::set_header);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "header"), "set_header", "get_header");
	ClassDB::bind_method(D_METHOD("write", "data"), &GDGzipWriter::write);
	ClassDB::bind_method(D_METHOD("write_async", "data"), &GDGzipWriter::write);
	ClassDB::bind_method(D_METHOD("flush"), &GDGzipWriter::flush);
	ClassDB::bind_method(D_METHOD("flush_async"), &GDGzipWriter::flush);
	ClassDB::bind_method(D_METHOD("close"), &GDGzipWriter::close);
	ClassDB::bind_method(D_METHOD("close_async"), &GDGzipWriter::close);
	ClassDB::bind_method(D_METHOD("reset", "writer"), &GDGzipWriter::reset);
	ClassDB::bind_method(D_METHOD("reset_async", "writer"), &GDGzipWriter::reset);
	ADD_AWAIT("write", "R:int"); ADD_AWAIT("write_async", "R:int"); ADD_AUTO_WAIT("write");
	ADD_AWAIT("flush", "R:Variant"); ADD_AWAIT("flush_async", "R:Variant"); ADD_AUTO_WAIT("flush");
	ADD_AWAIT("close", "R:Variant"); ADD_AWAIT("close_async", "R:Variant"); ADD_AUTO_WAIT("close");
	ADD_AWAIT("reset", "R:Variant"); ADD_AWAIT("reset_async", "R:Variant"); ADD_AUTO_WAIT("reset");
}
