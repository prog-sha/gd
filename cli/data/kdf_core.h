// Derive protocol keys with reusable keyed hash states and explicit encoded-length checks.
#pragma once
#include "crypto_core.h"
#include "hash_core.h"
#include "hash64_core.h"
#include <algorithm>
#include <array>
#include <type_traits>

namespace GDCrypto {
// Retain keyed prefix states so repeated authentication does not repeat key processing.
template <class Hash> class HMAC {
	Hash prefix, suffix, running; // Inner prefix, outer prefix, and current message state.
public:
	// Prepare keyed compression prefixes for the selected digest.
	HMAC(typename Hash::Kind kind, const void *key, size_t key_size) : prefix(kind), suffix(kind), running(kind) {
		std::array<uint8_t, Hash::block_size()> pad{};
		if (key_size > pad.size()) {
			Hash reduced(kind);
			reduced.write(key, key_size);
			reduced.sum(pad.data());
			erase(&reduced, sizeof(reduced));
		} else if (key_size) std::copy_n(static_cast<const uint8_t *>(key), key_size, pad.data());
		for (auto &byte : pad) byte ^= 0x36;
		prefix.write(pad.data(), pad.size());
		for (auto &byte : pad) byte ^= 0x36 ^ 0x5c;
		suffix.write(pad.data(), pad.size());
		running = prefix;
		erase(pad.data(), pad.size());
	}
	// Erase keyed states while they still have valid object lifetimes.
	~HMAC() {
		static_assert(std::is_trivially_copyable<Hash>::value, "keyed state must own only inline value storage");
		erase(&prefix, sizeof(prefix)); erase(&suffix, sizeof(suffix)); erase(&running, sizeof(running));
	}
	void reset() { running = prefix; } // Begin another message under the same key.
	void write(const void *data, size_t size) { running.write(data, size); } // Consume message bytes incrementally.
	void keep() const { running.keep(); } // Preserve deliberately discarded balancing work for compatible digest streams.
	size_t size() const { return running.size(); } // Return the full authenticator width.
	// Finalize a private copy so the caller can continue the message afterward.
	void sum(uint8_t *output) const {
		std::array<uint8_t, 64> inner{};
		running.sum(inner.data());
		Hash final = suffix;
		final.write(inner.data(), size());
		final.sum(output);
		erase(&final, sizeof(final));
		erase(inner.data(), inner.size());
	}
};

// Extract one digest-width pseudorandom key, treating empty salt as a zero-padded key.
template <class Hash> void extract(typename Hash::Kind kind, const void *secret, size_t size, const void *salt, size_t salt_size, uint8_t *output) {
	HMAC<Hash> mac(kind, salt, salt_size);
	mac.write(secret, size);
	mac.sum(output);
}

// Expand within the one-byte counter range, requiring info disjoint from output.
template <class Hash> bool expand(typename Hash::Kind kind, const void *key, size_t key_size, const void *info, size_t info_size, uint8_t *output, size_t size) {
	const size_t width = Hash(kind).size();
	if (size > 255 * width || (size && !output) || (key_size && !key) || (info_size && !info) || overlap(info, info_size, output, size)) return false;
	HMAC<Hash> mac(kind, key, key_size);
	std::array<uint8_t, 64> previous{};
	for (size_t at = 0; at < size;) {
		mac.reset();
		if (at) mac.write(previous.data(), width);
		mac.write(info, info_size);
		const uint8_t counter = uint8_t(at / width + 1);
		mac.write(&counter, 1);
		mac.sum(previous.data());
		const size_t count = std::min(width, size - at);
		std::copy_n(previous.data(), count, output + at);
		at += count;
	}
	erase(previous.data(), previous.size());
	return true;
}

// Derive with a four-byte block index and reusable keyed prefix; salt must not overlap output.
template <class Hash> bool pbkdf2(typename Hash::Kind kind, const void *password, size_t password_size, const void *salt, size_t salt_size, uint64_t rounds, uint8_t *output, size_t size) {
	const size_t width = Hash(kind).size();
	if (!rounds || !size || uint64_t(size) > uint64_t(UINT32_MAX) * width ||
			!output || (password_size && !password) || (salt_size && !salt) || overlap(salt, salt_size, output, size)) return false;
	HMAC<Hash> mac(kind, password, password_size);
	std::array<uint8_t, 64> value{}, total{};
	for (size_t at = 0; at < size;) {
		const uint32_t index = uint32_t(at / width + 1);
		const std::array<uint8_t, 4> encoded = {uint8_t(index >> 24), uint8_t(index >> 16), uint8_t(index >> 8), uint8_t(index)};
		mac.reset(); mac.write(salt, salt_size); mac.write(encoded.data(), encoded.size()); mac.sum(value.data());
		total = value;
		for (uint64_t iteration = 1; iteration < rounds; ++iteration) {
			mac.reset(); mac.write(value.data(), width); mac.sum(value.data());
			for (size_t byte = 0; byte != width; ++byte) total[byte] ^= value[byte];
		}
		const size_t count = std::min(width, size - at);
		std::copy_n(total.data(), count, output + at);
		at += count;
	}
	erase(value.data(), value.size()); erase(total.data(), total.size());
	return true;
}
}
