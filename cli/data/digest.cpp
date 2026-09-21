// Compute digests directly and obtain cryptographic randomness from the operating system.
#include "cli/data/digest.h"

// Initialize incremental SHA-256 state.
Error GDDigest::start() {
	hash.reset(GDCrypto::Hash32::SHA256);
	active = true;
	return OK;
}

// Feed bytes into the digest without copying.
Error GDDigest::update(const PackedByteArray &p_data) {
	if (!active) return FAILED;
	hash.write(p_data.ptr(), p_data.size());
	return OK;
}

// Return the completed digest and reset to reusable empty state.
PackedByteArray GDDigest::finish() {
	PackedByteArray out;
	if (!active || out.resize(32) != OK) return out;
	hash.sum(out.ptrw());
	active = false;
	return out;
}

// Enforce MD5's fixed width and report computation failure rather than success.
Ref<R> GDDigest::md5(const PackedByteArray &p_data) {
	constexpr int bytes = 16; // Fixed MD5 digest width.
	PackedByteArray out;
	if (out.resize(bytes) != OK) return R::err("cannot allocate MD5 digest", Err::LIMITED);
	GDCrypto::Hash32 hash(GDCrypto::Hash32::MD5);
	hash.write(p_data.ptr(), p_data.size());
	hash.sum(out.ptrw());
	return R::ok(out);
}

// Return the requested cryptographic bytes without exposing partial results on failure.
PackedByteArray GDDigest::random(int p_size) {
	PackedByteArray out;
	if (p_size < 0 || out.resize(p_size) != OK) return out;
	CRASH_COND_MSG(!GDCrypto::random_fill(out.ptrw(), p_size), "operating system cryptographic random source failed");
	return out;
}
