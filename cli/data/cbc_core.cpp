// Process complete TLS block-cipher records with independent authentication and fixed-work terminal hashing.
#include "cbc_core.h"

namespace {
constexpr size_t BLOCK = 16; // Block and explicit initialization-vector width.
constexpr size_t TAG = 20; // Authentication output width for the negotiated legacy suite.
constexpr size_t PLAIN = 16384; // Protocol plaintext fragment limit, not an application-message limit.
constexpr size_t CIPHER = PLAIN + 2048; // Protocol allowance for ciphertext expansion.
}

// Erase unpublished record data while its vector storage is still live.
GDCrypto::TLSCBC::~TLSCBC() { erase(scratch.data(), scratch.size()); }

// Reuse scratch capacity without retaining preceding private record contents.
void GDCrypto::TLSCBC::prepare(size_t size) {
	erase(scratch.data(), scratch.size()); scratch.resize(size);
}

// Reject incomplete key installation and discard all preceding authentication state.
bool GDCrypto::TLSCBC::reset(Bytes key, Bytes auth) {
	mac.reset(); prepare(0);
	if (!aes.reset(key.data, key.size)) return false;
	if ((key.size != 16 && key.size != 32) || !auth.data || auth.size != TAG) return false;
	mac.emplace(Hash32::SHA1, auth.data, auth.size);
	return true;
}

// Bind the sequence number, content type, protocol version, and plaintext length before hashing the fragment.
void GDCrypto::TLSCBC::authenticate(uint64_t sequence, uint8_t type, Bytes plain, uint8_t *output) {
	std::array<uint8_t, 13> header{};
	for (unsigned at = 0; at != 8; ++at) header[at] = uint8_t(sequence >> (56 - 8 * at));
	header[8] = type; header[9] = 3; header[10] = 3; header[11] = uint8_t(plain.size >> 8); header[12] = uint8_t(plain.size);
	mac->reset(); mac->write(header.data(), header.size()); mac->write(plain.data, plain.size); mac->sum(output);
}

// Seal one fragment using an unpredictable explicit initialization vector and complete-block padding.
bool GDCrypto::TLSCBC::seal(uint64_t sequence, uint8_t type, Bytes plain, std::vector<uint8_t> &output) {
	if (!mac || plain.size > PLAIN || (!plain.data && plain.size) || overlap(plain.data, plain.size, scratch.data(), scratch.size())) return false;
	const size_t padding = BLOCK - (plain.size + TAG) % BLOCK, size = plain.size + TAG + padding;
	prepare(BLOCK + size);
	if (!random_fill(scratch.data(), BLOCK)) return false;
	if (plain.size) std::copy_n(plain.data, plain.size, scratch.data() + BLOCK);
	authenticate(sequence, type, plain, scratch.data() + BLOCK + plain.size);
	std::fill(scratch.begin() + BLOCK + plain.size + TAG, scratch.end(), uint8_t(padding - 1));
	for (size_t at = BLOCK; at != scratch.size(); at += BLOCK) {
		for (size_t byte = 0; byte != BLOCK; ++byte) scratch[at + byte] ^= scratch[at - BLOCK + byte];
		aes.encrypt(scratch.data() + at, scratch.data() + at);
	}
	output.swap(scratch);
	return true;
}

// Decrypt into private storage, balance hash work across padding choices, and expose only authenticated bytes.
bool GDCrypto::TLSCBC::open(uint64_t sequence, uint8_t type, Bytes record, std::vector<uint8_t> &output) {
	if (!mac || !record.data || record.size < BLOCK * 3 || record.size > CIPHER || record.size % BLOCK || overlap(record.data, record.size, scratch.data(), scratch.size())) return false;
	const size_t size = record.size - BLOCK;
	prepare(size);
	for (size_t at = 0; at != size; at += BLOCK) {
		aes.decrypt(record.data + BLOCK + at, scratch.data() + at);
		for (size_t byte = 0; byte != BLOCK; ++byte) scratch[at + byte] ^= record.data[at + byte];
	}
	const uint8_t padding = scratch[size - 1];
	uint32_t difference = 0;
	for (size_t at = 0; at != std::min(size, size_t(256)); ++at) {
		const uint32_t selected = opaque_mask(0U - uint32_t(at <= padding));
		difference |= (scratch[size - 1 - at] ^ padding) & selected;
	}
	const int proposed = int(size) - int(padding) - int(TAG) - 1;
	const uint32_t nonnegative = opaque_mask(0U - uint32_t(proposed >= 0));
	const size_t length = uint32_t(proposed) & nonnegative;
	std::array<uint8_t, TAG> expected{};
	authenticate(sequence, type, {scratch.data(), length}, expected.data());
	const bool authenticated = equal(expected.data(), scratch.data() + length, TAG);
	// Consume the remaining decrypted bytes after capturing the authenticator so total compression work depends only on record size.
	mac->write(scratch.data() + length, size - length);
	mac->keep();
	if (difference || !nonnegative || !authenticated || length > PLAIN) { prepare(0); return false; }
	erase(scratch.data() + length, size - length); scratch.resize(length);
	output.swap(scratch);
	return true;
}
