// Verify encoded public-key signatures with independent modular arithmetic and digest primitives.
#pragma once
#include "mod_core.h"
#include "hash_core.h"
#include "hash64_core.h"
#include <optional>

namespace GDCrypto {
// Encode the complete algorithm-bound digest and minimum-width signature padding.
inline bool rsa_digest(unsigned hash_id, Bytes digest, uint8_t *output, size_t size) {
	const size_t width = hash_id == 1 ? 32 : hash_id == 2 ? 48 : hash_id == 3 ? 64 : 0;
	if (!width || digest.size != width || !digest.data || !output || size < width + 30) return false;
	std::fill_n(output, size, uint8_t(0)); output[1] = 1;
	const size_t separator = size - width - 20;
	std::fill(output + 2, output + separator, uint8_t(255));
	const uint8_t prefix[] = {0x30, uint8_t(width + 17), 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, uint8_t(hash_id), 0x05, 0x00, 0x04, uint8_t(width)}; // Digest identifier and canonical enclosing lengths.
	std::copy(std::begin(prefix), std::end(prefix), output + separator + 1);
	std::copy_n(digest.data, width, output + size - width);
	return true;
}

// Apply the digest-counter mask used by randomized signature encoding and verification.
template <class Hash> void rsa_mask(typename Hash::Kind kind, Bytes seed, uint8_t *output, size_t size) {
	const size_t width = Hash(kind).size();
	std::array<uint8_t, 64> mask{};
	for (size_t at = 0; at < size;) {
		const uint32_t counter = uint32_t(at / width);
		const uint8_t encoded[] = {uint8_t(counter >> 24), uint8_t(counter >> 16), uint8_t(counter >> 8), uint8_t(counter)};
		Hash generator(kind); generator.write(seed.data, seed.size); generator.write(encoded, 4); generator.sum(mask.data());
		const size_t count = std::min(width, size - at);
		for (size_t byte = 0; byte != count; ++byte) output[at + byte] ^= mask[byte];
		at += count;
	}
}

// Own a public key in caller-selected storage; protocol policy determines allowed key widths.
template <size_t Words> class RSAPublic {
	using Buffer = CryptoStorage<uint8_t, Words * 4>; // Encoded messages with key-dependent runtime capacity when selected.
	Mod<Words> modulus; // Public modulus and reduction precomputation.
	std::array<uint8_t, 4> exponent{}; // Public exponent in network byte order.
	bool ready = false; // Reject operations after invalid key replacement.
	// Recover a fixed-width encoded message while rejecting noncanonical signature integers.
	bool recover(Bytes signature, Buffer &output) const {
		if (!ready || signature.size != modulus.size()) return false;
		typename Mod<Words>::Value encoded;
		if (!modulus.read(signature, encoded)) return false;
		Bytes power = {exponent.data(), exponent.size()};
		while (power.size && !power.data[0]) { ++power.data; --power.size; }
		const auto plain = modulus.leave(modulus.public_power(modulus.enter(encoded), power));
		return modulus.write(plain, output.data(), modulus.size());
	}
public:
	// Accept an odd positive modulus and a supported public exponent without narrowing key size.
	bool reset(Bytes input, uint32_t power) {
		ready = false; exponent.fill(0);
		if (power < 2 || power > INT32_MAX || !modulus.reset(input)) return false;
		for (unsigned at = 0; at != 4; ++at) exponent[at] = uint8_t(power >> (24 - at * 8));
		ready = true;
		return true;
	}
	size_t bits() const { return ready ? modulus.bits() : 0; } // Expose key strength for protocol policy checks.
	// Check the complete padding and algorithm-bound digest encoding for a fixed-hash signature.
	bool verify_digest(unsigned hash_id, Bytes digest, Bytes signature) const {
		if (bits() < 1024) return false;
		auto decoded = crypto_storage<uint8_t, Words * 4>(modulus.size()), expected = crypto_storage<uint8_t, Words * 4>(modulus.size());
		if (!rsa_digest(hash_id, digest, expected.data(), modulus.size()) || !recover(signature, decoded)) return false;
		return equal(decoded.data(), expected.data(), modulus.size());
	}
	// Verify a randomized encoded signature; an omitted salt length accepts any valid encoded salt.
	template <class Hash> bool verify_pss(typename Hash::Kind kind, Bytes digest, Bytes signature, std::optional<size_t> salt_size = std::nullopt) const {
		const size_t width = Hash(kind).size(), bit_width = bits();
		if (bit_width < 1024 || !digest.data || digest.size != width) return false;
		const size_t length = (bit_width + 6) / 8;
		if (length < width + 2 || (salt_size && *salt_size > length - width - 2)) return false;
		auto encoded = crypto_storage<uint8_t, Words * 4>(modulus.size());
		if (!recover(signature, encoded)) return false;
		const size_t skipped = modulus.size() - length;
		for (size_t at = 0; at != skipped; ++at) if (encoded[at]) return false;
		uint8_t *message = encoded.data() + skipped;
		const unsigned unused = unsigned(length * 8 - (bit_width - 1));
		if (message[length - 1] != 0xbc || (message[0] & uint8_t(0xffU << (8 - unused)))) return false;
		const size_t masked = length - width - 1;
		const uint8_t *hash = message + masked;
		std::array<uint8_t, 64> mask{};
		rsa_mask<Hash>(kind, {hash, width}, message, masked);
		message[0] &= uint8_t(255U >> unused);
		size_t marker = 0;
		if (salt_size) {
			marker = masked - *salt_size - 1;
			for (size_t at = 0; at != marker; ++at) if (message[at]) return false;
		} else while (marker < masked && !message[marker]) ++marker;
		if (marker >= masked || message[marker] != 1) return false;
		const std::array<uint8_t, 8> zeros{};
		Hash final(kind); final.write(zeros.data(), zeros.size()); final.write(digest.data, digest.size); final.write(message + marker + 1, masked - marker - 1); final.sum(mask.data());
		return equal(mask.data(), hash, width);
	}
};
}
