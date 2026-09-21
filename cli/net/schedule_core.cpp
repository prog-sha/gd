// Implement labelled key separation and non-destructive handshake authentication.
#include "schedule_core.h"
#include <cstring>

namespace GDCrypto {
namespace {
// Reject nonempty byte ranges without backing storage before any digest accesses them.
bool backed(Bytes input) { return !input.size || input.data; }

// Keep keyed recurrence state local and erase each intermediate digest after expansion.
template <class Hash> void p_hash(typename Hash::Kind kind, Bytes secret, Bytes label, Bytes seed, uint8_t *output, size_t count) {
	HMAC<Hash> mac(kind, secret.data, secret.size);
	std::array<uint8_t, 48> chain{}, block{};
	mac.write(label.data, label.size); mac.write(seed.data, seed.size); mac.sum(chain.data());
	for (size_t at = 0; at != count;) {
		mac.reset(); mac.write(chain.data(), mac.size()); mac.write(label.data, label.size); mac.write(seed.data, seed.size); mac.sum(block.data());
		const size_t take = std::min(mac.size(), count - at);
		std::memcpy(output + at, block.data(), take); at += take;
		if (at != count) { mac.reset(); mac.write(chain.data(), mac.size()); mac.sum(chain.data()); }
	}
	erase(chain.data(), chain.size()); erase(block.data(), block.size());
}

// Authenticate a transcript digest without exposing or retaining the keyed hash prefixes.
template <class Hash> void authenticate(typename Hash::Kind kind, Bytes key, Bytes input, uint8_t *output) {
	HMAC<Hash> mac(kind, key.data, key.size); mac.write(input.data, input.size); mac.sum(output);
}
}

// Select the transcript digest without allocating message storage.
TLSHash::TLSHash(bool sha384) { reset(sha384); }

// Erase whichever inline compression state is active before destroying it.
TLSHash::~TLSHash() { std::visit([](auto &state) { erase(&state, sizeof(state)); }, hash); }

// Discard the old transcript before initializing a fresh digest state.
void TLSHash::reset(bool sha384) {
	std::visit([](auto &state) { erase(&state, sizeof(state)); }, hash);
	if (sha384) hash.emplace<Hash64>(Hash64::SHA384); else hash.emplace<Hash32>(Hash32::SHA256);
}

// Inspect the selected digest width without hashing another message.
size_t TLSHash::size() const { return std::visit([](const auto &state) { return state.size(); }, hash); }

// Add complete or partial encoded handshake messages without changing failure state.
bool TLSHash::write(Bytes input) {
	if (!backed(input)) return false;
	std::visit([&](auto &state) { state.write(input.data, input.size); }, hash); return true;
}

// Finalize only the digest's private copy so later messages can extend the transcript.
bool TLSHash::sum(uint8_t *output) const {
	if (!output) return false;
	std::visit([&](const auto &state) { state.sum(output); }, hash); return true;
}

// Bind a retry transcript to the first hello using the synthetic handshake message type.
void TLSHash::retry() {
	std::array<uint8_t, 52> message{};
	const size_t width = size(); message[0] = 254; message[3] = uint8_t(width); sum(message.data() + 4);
	reset(width == 48); write({ message.data(), width + 4 }); erase(message.data(), message.size());
}

// Validate all reusable inputs before publishing any legacy key-block bytes.
bool TLSKDF::prf(Bytes secret, Bytes label, Bytes seed, uint8_t *output, size_t count) const {
	if (!backed(secret) || !backed(label) || !backed(seed) || (count && !output) || overlap(label.data, label.size, output, count) || overlap(seed.data, seed.size, output, count)) return false;
	if (wide) p_hash<Hash64>(Hash64::SHA384, secret, label, seed, output, count);
	else p_hash<Hash32>(Hash32::SHA256, secret, label, seed, output, count);
	return true;
}

// Treat an absent key-schedule input as a digest-width zero string, not an empty IKM.
bool TLSKDF::extract(Bytes secret, Bytes salt, uint8_t *output) const {
	if (!backed(secret) || !backed(salt) || !output) return false;
	std::array<uint8_t, 48> zeros{};
	if (!secret.size) secret = { zeros.data(), size() };
	if (wide) GDCrypto::extract<Hash64>(Hash64::SHA384, secret.data, secret.size, salt.data, salt.size, output);
	else GDCrypto::extract<Hash32>(Hash32::SHA256, secret.data, secret.size, salt.data, salt.size, output);
	return true;
}

// Copy label context before expansion so bounded caller-owned inputs may overlap the output.
bool TLSKDF::expand(Bytes secret, Bytes label, Bytes context, uint8_t *output, size_t count) const {
	if (!backed(secret) || !backed(label) || !backed(context) || !label.size || label.size > 249 || context.size > 255 || count > 255 * size() || (count && !output)) return false;
	std::array<uint8_t, 514> info{};
	info[0] = uint8_t(count >> 8); info[1] = uint8_t(count); info[2] = uint8_t(label.size + 6);
	std::memcpy(info.data() + 3, "tls13 ", 6); std::memcpy(info.data() + 9, label.data, label.size);
	info[9 + label.size] = uint8_t(context.size);
	if (context.size) std::memcpy(info.data() + 10 + label.size, context.data, context.size);
	const size_t length = 10 + label.size + context.size;
	const bool valid = wide ? GDCrypto::expand<Hash64>(Hash64::SHA384, secret.data, secret.size, info.data(), length, output, count) : GDCrypto::expand<Hash32>(Hash32::SHA256, secret.data, secret.size, info.data(), length, output, count);
	erase(info.data(), info.size()); return valid;
}

// Derive against the full selected transcript digest rather than accepting mismatched hash widths.
bool TLSKDF::derive(Bytes secret, Bytes label, const TLSHash &transcript, uint8_t *output) const {
	if (transcript.size() != size()) return false;
	std::array<uint8_t, 48> digest{}; transcript.sum(digest.data());
	const bool valid = expand(secret, label, { digest.data(), size() }, output, size());
	erase(digest.data(), digest.size()); return valid;
}

// Separate directional finished keys from traffic keys before authenticating the transcript.
bool TLSKDF::finished(Bytes secret, const TLSHash &transcript, uint8_t *output) const {
	if (!output || transcript.size() != size()) return false;
	std::array<uint8_t, 48> key{}, digest{};
	const bool valid = expand(secret, { reinterpret_cast<const uint8_t *>("finished"), 8 }, {}, key.data(), size());
	if (valid) {
		transcript.sum(digest.data());
		if (wide) authenticate<Hash64>(Hash64::SHA384, { key.data(), size() }, { digest.data(), size() }, output);
		else authenticate<Hash32>(Hash32::SHA256, { key.data(), size() }, { digest.data(), size() }, output);
	}
	erase(key.data(), key.size()); erase(digest.data(), digest.size()); return valid;
}
}
