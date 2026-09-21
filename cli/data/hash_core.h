// Accumulate fixed-width digests with bounded working storage and explicit byte order.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace GDCrypto {
void sha256_block(uint32_t *p_state, const uint8_t *p_data); // Transform one complete block with caller-owned chaining words.
// Own a chaining value and the unfinished bytes of a digest stream.
class Hash32 {
public:
	enum Kind { SHA256, MD5, SHA1 }; // Select the message transform and digest width.
private:
	Kind kind = SHA256; // Transform selected at construction or reset.
	std::array<uint32_t, 8> state{}; // Chaining words; MD5 uses the first four.
	std::array<uint8_t, 64> pending{}; // Unfinished input block.
	uint64_t count = 0; // Total bytes modulo the encoded length field.
public:
	explicit Hash32(Kind p_kind = SHA256) { reset(p_kind); } // Initialize a new stream.
	void reset(Kind p_kind); // Discard preceding input and select a digest algorithm.
	void write(const void *p_data, size_t p_size); // Consume bytes without retaining complete messages.
	void sum(uint8_t *p_out) const; // Inspect the digest without consuming the running stream.
	void constant_sum(uint8_t *p_out) const; // Finalize with two compression blocks regardless of the private tail length.
	void keep() const; // Preserve balancing work even when its digest is deliberately discarded.
	size_t size() const { return kind == MD5 ? 16 : (kind == SHA1 ? 20 : 32); } // Return the selected digest width.
	static constexpr size_t block_size() { return 64; } // Return the compression block width.
};
bool random_fill(uint8_t *p_out, size_t p_size); // Fill a destination completely from OS cryptographic randomness.
}
