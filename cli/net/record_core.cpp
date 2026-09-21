// Frame authenticated records and keep peer failures terminal before plaintext publication.
#include "record_core.h"
#include <cstring>
#include <limits>

namespace GDCrypto {
namespace {
constexpr size_t plain_limit = 16384; // Maximum protocol plaintext fragment, independent of application message size.
constexpr uint8_t unexpected = 10, bad_mac = 20, overflow = 22, version_error = 70, internal = 80; // Wire alert descriptions.

// Encode bounded network-order fields without depending on host byte order.
void network(uint64_t value, uint8_t *output, size_t size) {
	while (size) { output[--size] = uint8_t(value); value >>= 8; }
}

// Construct the fixed record header after the encoded payload size is known.
void header(uint8_t type, size_t size, uint8_t *output) {
	output[0] = type; output[1] = 3; output[2] = 3; network(size, output + 3, 2);
}

// Accept protocol content kinds while leaving message ordering to the handshake layer.
bool content(uint8_t type, bool modern) {
	return type == 21 || type == 22 || type == 23 || (!modern && type == 20);
}
}

// Resolve wire suites to their record and key-schedule parameters as one transaction.
bool TLSCipher::find(uint16_t suite, TLSCipher &output) {
	TLSCipher value;
	switch (suite) {
		case 0x1301: value = { GCM, 16, 12, 0, true, false }; break;
		case 0x1302: value = { GCM, 32, 12, 0, true, true }; break;
		case 0x1303: value = { CHACHA, 32, 12, 0, true, false }; break;
		case 0xc02f: case 0xc02b: value = { GCM, 16, 4, 0, false, false }; break;
		case 0xc030: case 0xc02c: value = { GCM, 32, 4, 0, false, true }; break;
		case 0xcca8: case 0xcca9: value = { CHACHA, 32, 12, 0, false, false }; break;
		case 0xc013: case 0xc009: value = { CBC, 16, 16, 20, false, false }; break;
		case 0xc014: case 0xc00a: value = { CBC, 32, 16, 20, false, false }; break;
		default: return false;
	}
	output = value; return true;
}

// Erase retained output before releasing vector allocation and owned traffic keys.
TLSRecord::~TLSRecord() { prepare(0); erase(iv.data(), iv.size()); }

// Reuse only storage whose previous contents have been explicitly erased.
void TLSRecord::prepare(size_t size) {
	erase(scratch.data(), scratch.size()); scratch.resize(size);
}

// Record failures cannot be retried with the same sequence and partially consumed stream.
bool TLSRecord::fail(uint8_t alert) {
	if (!error) error = alert;
	prepare(0); return false;
}

// Replace the full protection epoch without retaining any usable previous key on failure.
bool TLSRecord::reset(uint16_t suite, Bytes key, Bytes salt, Bytes mac) {
	ready = false; error = 0; sequence = 0; cipher.emplace<std::monostate>();
	erase(iv.data(), iv.size()); prepare(0);
	if (!TLSCipher::find(suite, config) || key.size != config.key || salt.size != config.iv || mac.size != config.mac || !key.data || !salt.data || (mac.size && !mac.data)) return false;
	if (config.mode == TLSCipher::CBC) ready = cipher.emplace<TLSCBC>().reset(key, mac);
	else {
		std::memcpy(iv.data(), salt.data, salt.size);
		ready = config.mode == TLSCipher::GCM ? cipher.emplace<AESGCM>().reset(key.data, key.size) : cipher.emplace<ChaChaPoly>().reset(key.data, key.size);
	}
	return ready;
}

// Build an explicit legacy nonce or a sequence-XOR nonce without changing the fixed salt.
void TLSRecord::nonce(uint8_t *output, const uint8_t *explicit_nonce) const {
	std::memcpy(output, iv.data(), iv.size());
	if (!config.modern && config.mode == TLSCipher::GCM) std::memcpy(output + 4, explicit_nonce, 8);
	else for (size_t at = 0; at != 8; ++at) output[11 - at] ^= uint8_t(sequence >> (8 * at));
}

// Authenticate legacy protocol metadata using the implicit directional sequence number.
void TLSRecord::aad(uint8_t type, size_t size, uint8_t *output) const {
	network(sequence, output, 8); header(type, size, output + 8);
}

// Keep the detached authentication tag interface identical across AEAD engines.
bool TLSRecord::protect(bool encrypt, const uint8_t *nonce, Bytes input, Bytes associated, uint8_t *output, uint8_t *tag) {
	if (config.mode == TLSCipher::GCM) {
		auto &engine = std::get<AESGCM>(cipher);
		return encrypt ? engine.seal(nonce, input.data, input.size, associated.data, associated.size, output, tag) : engine.open(nonce, input.data, input.size, associated.data, associated.size, tag, output);
	}
	auto &engine = std::get<ChaChaPoly>(cipher);
	return encrypt ? engine.seal(nonce, input.data, input.size, associated.data, associated.size, output, tag) : engine.open(nonce, input.data, input.size, associated.data, associated.size, tag, output);
}

// Publish the framed ciphertext only after complete protection and sequence advancement.
bool TLSRecord::seal(uint8_t type, Bytes plain, std::vector<uint8_t> &output, size_t padding) {
	if (!ready || error) return false;
	if (sequence == std::numeric_limits<uint64_t>::max()) return fail(internal);
	if (!content(type, config.modern) || (config.modern && !plain.size && type != 23) || plain.size > plain_limit || (plain.size && !plain.data) || padding > plain_limit - plain.size || (!config.modern && padding) || overlap(plain.data, plain.size, scratch.data(), scratch.size())) return false;
	if (config.mode == TLSCipher::CBC) {
		prepare(0);
		if (!std::get<TLSCBC>(cipher).seal(sequence, type, plain, scratch)) return fail(internal);
		scratch.insert(scratch.begin(), 5, 0); header(type, scratch.size() - 5, scratch.data());
	} else {
		const size_t explicit_size = !config.modern && config.mode == TLSCipher::GCM ? 8 : 0;
		const size_t size = plain.size + (config.modern ? 1 + padding : 0);
		prepare(5 + explicit_size + size + 16);
		header(config.modern ? 23 : type, scratch.size() - 5, scratch.data());
		uint8_t number[8]{}, unique[12]{}, associated[13]{};
		network(sequence, number, 8); nonce(unique, number); aad(type, plain.size, associated);
		if (explicit_size) std::memcpy(scratch.data() + 5, number, 8);
		uint8_t *body = scratch.data() + 5 + explicit_size;
		if (plain.size) std::memcpy(body, plain.data, plain.size);
		if (config.modern) body[plain.size] = type;
		const Bytes bound = config.modern ? Bytes{ scratch.data(), 5 } : Bytes{ associated, 13 };
		if (!protect(true, unique, { body, size }, bound, body, body + size)) return fail(internal);
	}
	++sequence; output.swap(scratch); return true;
}

// Authenticate exactly one framed record; plaintext and content type remain unchanged on failure.
bool TLSRecord::open(Bytes record, uint8_t &type, std::vector<uint8_t> &output) {
	if (!ready || error) return false;
	if (sequence == std::numeric_limits<uint64_t>::max()) return fail(internal);
	if (!record.data || record.size < 5 || overlap(record.data, record.size, scratch.data(), scratch.size())) return fail(unexpected);
	const size_t size = size_t(record.data[3]) * 256 + record.data[4];
	if (size > plain_limit + (config.modern ? 256 : 2048)) return fail(overflow);
	if (size != record.size - 5) return fail(unexpected);
	if (record.data[1] != 3 || record.data[2] != 3) return fail(version_error);
	uint8_t kind = record.data[0];
	if (!content(kind, config.modern) || (config.modern && kind != 23)) return fail(unexpected);
	if (config.mode == TLSCipher::CBC) {
		prepare(0);
		if (!std::get<TLSCBC>(cipher).open(sequence, kind, { record.data + 5, size }, scratch)) return fail(bad_mac);
	} else {
		const size_t explicit_size = !config.modern && config.mode == TLSCipher::GCM ? 8 : 0;
		if (size < explicit_size + 16) return fail(bad_mac);
		const size_t count = size - explicit_size - 16;
		uint8_t unique[12]{}, associated[13]{}, tag[16]{};
		nonce(unique, record.data + 5); aad(kind, count, associated);
		std::memcpy(tag, record.data + record.size - 16, 16); prepare(count);
		const Bytes bound = config.modern ? Bytes{ record.data, 5 } : Bytes{ associated, 13 };
		if (!protect(false, unique, { record.data + 5 + explicit_size, count }, bound, scratch.data(), tag)) return fail(bad_mac);
		if (config.modern) {
			if (count > plain_limit + 1) return fail(overflow);
			size_t end = count;
			while (end && scratch[end - 1] == 0) --end;
			if (!end || !content(scratch[end - 1], true) || (end == 1 && scratch[0] != 23)) return fail(unexpected);
			kind = scratch[end - 1]; erase(scratch.data() + end - 1, count - end + 1); scratch.resize(end - 1);
		}
	}
	if (scratch.size() > plain_limit) return fail(overflow);
	++sequence; output.swap(scratch); type = kind; return true;
}
}
