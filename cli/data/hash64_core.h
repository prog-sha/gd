// Accumulate wide digests with a fixed circular schedule and non-destructive finalization.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace GDCrypto {
// Own wide chaining words and a partial compression block.
class Hash64 {
public:
	enum Kind { SHA384, SHA512 }; // Select the initial state and digest width.
private:
	Kind kind = SHA384; // Selected digest transform.
	std::array<uint64_t, 8> state{}; // Chaining words after complete blocks.
	std::array<uint8_t, 128> pending{}; // Unfinished input block.
	uint64_t low = 0, high = 0; // Unsigned 128-bit byte count modulo the encoded length.
public:
	explicit Hash64(Kind p_kind = SHA384) { reset(p_kind); } // Initialize the selected stream.
	void reset(Kind p_kind); // Discard input and initialize the selected digest.
	void write(const void *data, size_t size); // Consume input without buffering full messages.
	void sum(uint8_t *output) const; // Inspect a digest while preserving its stream state.
	size_t size() const { return kind == SHA384 ? 48 : 64; } // Return the selected digest width.
	static constexpr size_t block_size() { return 128; } // Return the compression block width.
};
}
