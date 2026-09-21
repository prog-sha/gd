// Provide incremental digests and operating-system cryptographic random bytes.
#pragma once
#include "cli/data/hash_core.h"
#include "cli/sys/std.h"
#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

class GDDigest : public RefCounted {
	GDCrypto::Hash32 hash; // Inline chaining state and partial block.
	bool active = false; // Whether start has initialized the current digest.

public:
	Error start(); // Start SHA-256.
	Error update(const PackedByteArray &p_data); // Append input bytes.
	PackedByteArray finish(); // Return the digest and release state.
	static PackedByteArray random(int p_size); // Generate cryptographic bytes seeded by the OS.
	static Ref<R> md5(const PackedByteArray &p_data); // Return a 16-byte protocol MD5 digest or computation failure.
};
