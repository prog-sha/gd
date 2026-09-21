// Derive handshake and traffic secrets from incremental transcripts and protocol-separated labels.
#pragma once
#include "cli/data/der_core.h"
#include "cli/data/kdf_core.h"
#include <variant>

namespace GDCrypto {
// Retain only compression state while supporting independent transcript checkpoints.
class TLSHash {
	std::variant<Hash32, Hash64> hash; // Selected handshake digest without retained message bodies.
public:
	explicit TLSHash(bool sha384 = false); // Initialize the digest selected by the negotiated suite.
	~TLSHash(); // Erase the retained compression state.
	void reset(bool sha384); // Start a new transcript with the selected digest.
	size_t size() const; // Return the negotiated transcript digest width.
	bool write(Bytes input); // Consume encoded handshake messages, excluding record headers.
	bool sum(uint8_t *output) const; // Inspect a checkpoint without consuming the transcript.
	void retry(); // Replace the first hello transcript with its synthetic message-hash encoding.
};

// Share protocol derivation operations while leaving epoch ordering to handshake state.
class TLSKDF {
	bool wide = false; // Select SHA-384 instead of SHA-256 for the negotiated suite.
public:
	explicit TLSKDF(bool sha384 = false) : wide(sha384) {} // Bind every derivation to the same handshake digest.
	size_t size() const { return wide ? 48 : 32; } // Return the width of transcript digests and extracted secrets.
	bool prf(Bytes secret, Bytes label, Bytes seed, uint8_t *output, size_t count) const; // Expand legacy secrets with P_hash while requiring seed and label disjoint from output.
	bool extract(Bytes secret, Bytes salt, uint8_t *output) const; // Extract a digest-width secret, substituting digest-width zeros for absent input material.
	bool expand(Bytes secret, Bytes label, Bytes context, uint8_t *output, size_t count) const; // Encode the protocol label and expand within its wire and block-counter bounds.
	bool derive(Bytes secret, Bytes label, const TLSHash &transcript, uint8_t *output) const; // Derive a secret bound to an incremental transcript checkpoint.
	bool finished(Bytes secret, const TLSHash &transcript, uint8_t *output) const; // Authenticate the handshake transcript under the directional finished key.
};
}
