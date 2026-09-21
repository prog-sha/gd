// Encode and measure UTF-8 text with resumable state and checked output lengths.
#pragma once

#include "cli/data/utf8_code.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include <functional>
#include <utility>

// Preserve arbitrary bytes as text when lossless, or as a byte array otherwise.
Variant utf8_value(const uint8_t *p_data, int64_t p_size, bool p_strict = false);

// Retain measurement and encoding progress independently of the owning HTTP operation.
struct Utf8Text {
	String text; // Immutable source storage.
	int begin = 0, count = -1; // Selected character range; negative count selects the complete input.
	PackedByteArray bytes; // Encoded output, grown when speculative ASCII needs wider encoding.
	int at = 0; // Next source rune in the current pass.
	int64_t size = 0; // Required byte count.
	int64_t offset = -1; // Output position, or negative while measuring.
	bool ascii = true; // Entire measured source can be narrowed without per-character branches.
	bool measure = false; // Return the byte length without allocating an encoded body.
	Error error = OK; // Allocation failure must not produce a successful truncated response.

	bool diagnosed = false; // Count this operation once across continuation and worker turns.
	bool probe = false; // Speculative ASCII range output, resumed at probe_at.
	int probe_at = 0; // Characters already narrowed into speculative storage.
	// Return false only when more work remains after the supplied soft deadline.
	bool advance(uint64_t p_until = 0);
	bool progressed() const { return at > 0 || offset >= 0 || probe_at > 0; }
};

// Encode text in the current time slice, retrying an exhausted turn before offloading costly work.
Variant utf8_reply(const String &p_text, uint64_t p_until, std::function<Variant(const PackedByteArray &)> p_reply);

// Measure text in the current time slice, retrying an exhausted turn before offloading costly work.
Variant utf8_size(const String &p_text, uint64_t p_until);

// Guard suspended native text without adding callback allocation to the inline path.
void utf8_watch(const Signal &p_signal, const Callable &p_alive);

// Continue an owned writer range, returning complete bytes or an allocation/cancellation error.
Signal utf8_continue(Utf8Text &&p_text);

// Preserve diagnostics without delegating allocation to an unchecked conversion.
inline void utf8_note(uint32_t p_c) {
	if (p_c > 0x1fffff) {
		String().print_unicode_error(vformat(p_c <= 0x7fffffff ? "Invalid unicode codepoint (%x)" : "Invalid unicode codepoint (%x), cannot represent as UTF-8", p_c), p_c > 0x7fffffff);
	}
}

// Accumulate encoded character widths in 64 bits without allocating.
inline int64_t utf8_bytes(const String &p_text) {
	int64_t bytes = 0;
	for (int i = 0; i < p_text.length(); i++) {
		const uint32_t c = p_text[i];
		bytes += GDUtf8::width(c);
	}
	return bytes;
}

// Accumulate UTF-8 in bounded scratch batches while allowing the final byte array to grow.
class Utf8Out {
	PackedByteArray bytes; // Final output with a 64-bit length.
	uint8_t buf[1024]; // Staging capacity only; a full batch is appended without truncation.
	int used = 0; // Bytes staged since the last flush.
	Error error = OK; // First allocation or representation failure.

	// Append a completed batch without narrowing the accumulated byte count.
	bool flush() {
		if (error != OK) return false;
		const int64_t at = bytes.size();
		if (used > INT64_MAX - at) {
			error = ERR_OUT_OF_MEMORY;
			return false;
		}
		error = bytes.resize_uninitialized(at + used);
		if (error != OK) return false;
		if (used) memcpy(bytes.ptrw() + at, buf, used);
		used = 0;
		return true;
	}

public:
	// Encode one character with the native string conversion's byte representation.
	bool add(char32_t p_c) {
		if (error != OK || (used > int(sizeof(buf)) - 6 && !flush())) return false;
		// This staged API replaces exceptional values before encoding, unlike raw string conversion.
		if (p_c > 0x1fffff) {
			String().print_unicode_error(vformat("Invalid unicode codepoint (%x)", uint32_t(p_c)), true);
			p_c = 0xfffd;
		}
		used += GDUtf8::encode(p_c, buf + used);
		return true;
	}

	// Encode a text span directly into the staged byte output.
	bool add(const String &p_text) {
		for (int i = 0; i < p_text.length(); i++) {
			if (!add(p_text[i])) return false;
		}
		return error == OK;
	}

	// Copy pre-encoded literals into available scratch space without decoding them.
	bool add(const PackedByteArray &p_bytes) {
		int64_t at = 0;
		while (at < p_bytes.size()) {
			if (error != OK || (used == int(sizeof(buf)) && !flush())) return false;
			const int n = int(MIN(int64_t(sizeof(buf)) - used, p_bytes.size() - at));
			memcpy(buf + used, p_bytes.ptr() + at, n);
			used += n;
			at += n;
		}
		return error == OK;
	}

	// Append an ASCII entity without constructing a temporary string.
	bool add(const char *p_text) {
		while (*p_text) {
			if (!add(char32_t(uint8_t(*p_text++)))) return false;
		}
		return error == OK;
	}

	// Transfer the complete output only after the final batch succeeds.
	Error finish(PackedByteArray &r_out) {
		if (flush()) r_out = std::move(bytes);
		return error;
	}
};
