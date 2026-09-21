// Derive per-message signature randomness from entropy, a private scalar, and a reduced digest.
#pragma once
#include "der_core.h"
#include "kdf_core.h"

namespace GDCrypto {
// Keep the keyed generator local to one signature and erase its owned state on return.
class HedgedNonce {
	std::array<uint8_t, 64> key{}, value{}; // Full-width authentication key and generator output state.
	// Advance the output state under the current authentication key.
	void step() {
		HMAC<Hash64> mac(Hash64::SHA512, key.data(), key.size());
		mac.write(value.data(), value.size()); mac.sum(value.data());
	}
public:
	// Align the private scalar and digest to separate compression blocks after mixing fresh entropy.
	HedgedNonce(Bytes entropy, Bytes secret, Bytes digest) {
		value.fill(1);
		constexpr std::array<uint8_t, 128> padding{}; // Zero bytes completing a compression block.
		for (uint8_t marker : {uint8_t(0), uint8_t(1)}) {
			HMAC<Hash64> mac(Hash64::SHA512, key.data(), key.size());
			mac.write(value.data(), value.size()); mac.write(&marker, 1);
			mac.write(entropy.data, entropy.size);
			mac.write(padding.data(), (128 - (value.size() + 1 + entropy.size) % 128) % 128);
			mac.write(secret.data, secret.size);
			mac.write(padding.data(), (128 - secret.size % 128) % 128);
			mac.write(digest.data, digest.size); mac.sum(key.data()); step();
		}
	}
	~HedgedNonce() { erase(key.data(), key.size()); erase(value.data(), value.size()); } // Erase both private generator states.
	HedgedNonce(const HedgedNonce &) = delete; // Keep secret state owned by one operation.
	HedgedNonce &operator=(const HedgedNonce &) = delete; // Prevent accidental state cloning.
	// Generate one candidate and advance state for an independently sampled rejection retry.
	void read(uint8_t *output, size_t size) {
		for (size_t at = 0; at < size;) {
			step();
			const size_t count = std::min(value.size(), size - at);
			std::copy_n(value.data(), count, output + at); at += count;
		}
		const uint8_t marker = 0;
		HMAC<Hash64> mac(Hash64::SHA512, key.data(), key.size());
		mac.write(value.data(), value.size()); mac.write(&marker, 1); mac.sum(key.data()); step();
	}
};
}
