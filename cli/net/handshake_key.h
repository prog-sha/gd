// Own one ephemeral key share and erase its private scalar independently of handshake-message storage.
#pragma once
#include "cli/data/ec_core.h"
#include <vector>

namespace GDCrypto {
// Derive agreement secrets only from validated peer points in the selected named group.
class TLSShare {
	PrimeCurve curve; // Public prime-curve parameters when the group is not the compact Montgomery curve.
	CryptoStorage<uint8_t,0> secret; // Owned private scalar, cleared before replacement and release.
	std::vector<uint8_t> point; // Encoded public share transmitted in the handshake.
	uint16_t group = 0; // Successfully initialized wire identifier.
public:
	TLSShare() = default; // Start without reusable agreement material.
	TLSShare(const TLSShare &) = delete; // Keep one lifetime for each ephemeral private scalar.
	TLSShare &operator=(const TLSShare &) = delete; // Prevent accidental duplication across retries or connections.
	bool reset(uint16_t selected); // Generate a fresh supported share from OS randomness without retaining previous key material.
	Bytes public_key() const { return {point.data(),point.size()}; } // Borrow only the encoded public share.
	bool agree(Bytes peer, CryptoStorage<uint8_t,0> &output) const; // Validate and derive a shared secret, preserving the destination on failure.
	static unsigned bits(uint16_t selected); // Recognize supported named groups without assigning negotiation preference.
};
}
