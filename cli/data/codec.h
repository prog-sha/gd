/**************************************************************************/
/*  codec.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Encode, pack, and hash byte sequences in native code.
// Expose operations through GD.data while keeping implementation names private.
//
// Per-byte script loops incur repeated allocation and Variant wrapping.
// Perform complete transformations natively within one script call.
//
// Fallible operations retain their processed value and typed error in R.

#pragma once

#include "cli/sys/std.h"

// Hexadecimal, base64, base32, and varint encodings.
class Encoding {
public:
	static String hex_encode(const PackedByteArray &p_data);
	static Ref<R> hex_decode(const String &p_text);
	static String base64_encode(const PackedByteArray &p_data);
	static Ref<R> base64_decode(const String &p_text, bool p_raw = false);
	static String base64url_encode(const PackedByteArray &p_data);
	static Ref<R> base64url_decode(const String &p_text, bool p_raw = true);
	// Require unpadded base64url where textual spelling is used as an identity key.
	static Ref<R> base64url_raw_decode(const String &p_text);
	static String base32_encode(const PackedByteArray &p_data);
	static Ref<R> base32_decode(const String &p_text, bool p_raw = false);
	static PackedByteArray varint_encode(int64_t p_n);
	static Ref<R> varint_decode(const PackedByteArray &p_data, int p_at);
};

// Byte-sequence operations.
class Bytes {
public:
	static PackedByteArray concat(const Array &p_parts);
	static bool equals(const PackedByteArray &p_a, const PackedByteArray &p_b);
	static bool includes(const PackedByteArray &p_hay, const PackedByteArray &p_needle);
	static int index_of(const PackedByteArray &p_hay, const PackedByteArray &p_needle, int p_from);
	static int last_index_of(const PackedByteArray &p_hay, const PackedByteArray &p_needle);
	static bool starts_with(const PackedByteArray &p_hay, const PackedByteArray &p_prefix);
	static bool ends_with(const PackedByteArray &p_hay, const PackedByteArray &p_suffix);
	static PackedByteArray repeat(const PackedByteArray &p_src, int p_times);
	static PackedByteArray fit(const PackedByteArray &p_src, int p_size);
	static Array split(const PackedByteArray &p_src, const PackedByteArray &p_sep);
};

// Encode and decode MessagePack values.
class Msgpack {
public:
	static Ref<R> encode(const Variant &p_v);
	static Ref<R> decode(const PackedByteArray &p_data);
};

// Encode and decode structured CBOR values.
class Cbor {
public:
	static Ref<R> encode(const Variant &p_v);
	static Ref<R> decode(const PackedByteArray &p_data);
};

// Create and read ustar archives.
class Tar {
public:
	// Each entry contains name, body, mode, mtime, and is_dir.
	static Ref<R> pack(const Array &p_entries);
	static Ref<R> unpack(const PackedByteArray &p_data);
	// Archive a directory tree with names relative to its root.
	static Ref<R> pack_dir(const String &p_root);
	// Extract while rejecting names that escape into parent directories.
	static Ref<R> unpack_to(const PackedByteArray &p_data, const String &p_root);
};

// Provide fixed-size digests, authentication codes, and password-derived keys.
class Hash {
public:
	// Fixed 32-byte output for SCRAM-SHA-256.
	static Ref<R> pbkdf2_sha256(const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds);
	// Derive a key with the selected hash and output length.
	static Ref<R> pbkdf2(const String &p_hash, const PackedByteArray &p_pass, const PackedByteArray &p_salt, int64_t p_rounds, int64_t p_size);
	// Derive a key from secret material, salt, and application context.
	static Ref<R> hkdf(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt, const PackedByteArray &p_info, int64_t p_size);
	// Extract a reusable pseudorandom key from a secret and salt.
	static Ref<R> hkdf_extract(const String &p_hash, const PackedByteArray &p_secret, const PackedByteArray &p_salt);
	// Expand a pseudorandom key and context to the required length.
	static Ref<R> hkdf_expand(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_info, int64_t p_size);
	// Compute a SHA-224 digest.
	static PackedByteArray sha224(const PackedByteArray &p_msg);
	// Compute a SHA-256 digest.
	static PackedByteArray sha256(const PackedByteArray &p_msg);
	// Compute a SHA-384 digest.
	static PackedByteArray sha384(const PackedByteArray &p_msg);
	// Compute a SHA-512 digest.
	static PackedByteArray sha512(const PackedByteArray &p_msg);
	// Compute a SHA3-224 digest.
	static PackedByteArray sha3_224(const PackedByteArray &p_msg);
	// Compute a SHA3-256 digest.
	static PackedByteArray sha3_256(const PackedByteArray &p_msg);
	// Compute a SHA3-384 digest.
	static PackedByteArray sha3_384(const PackedByteArray &p_msg);
	// Compute a SHA3-512 digest.
	static PackedByteArray sha3_512(const PackedByteArray &p_msg);
	// Compute an authentication code with the selected hash.
	static Ref<R> hmac(const String &p_hash, const PackedByteArray &p_key, const PackedByteArray &p_msg);
	// Compute an HMAC-SHA-256 authentication code.
	static PackedByteArray hmac_sha256(const PackedByteArray &p_key, const PackedByteArray &p_msg);
	// Compute a SHA-1 digest.
	static PackedByteArray sha1(const PackedByteArray &p_msg);
	// Combine two byte sequences with XOR.
	static PackedByteArray xor_bytes(const PackedByteArray &p_a, const PackedByteArray &p_b);
	// Compare without early exit at mismatches to avoid content-dependent timing.
	// Use this rather than equality operators for authentication values or password-derived data.
	static bool equal_ct(const PackedByteArray &p_a, const PackedByteArray &p_b);
};
