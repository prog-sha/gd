// Bind a certificate chain to an independently validated private signing identity.
#pragma once
#include "cert_store.h"
#include "signing_core.h"

namespace GDCrypto {
// Share immutable credentials across connections without sharing temporary handshake state.
class TLSIdentity {
	std::vector<Cert::Ptr> certificates; // Configured leaf-first certificate chain retained for handshake encoding.
	std::unique_ptr<SigningKey> private_key; // Owned key whose derived public identity matches the first certificate.
	TLSIdentity() = default; // Prevent access to credentials before the owning factory validates their signer.
public:
	static std::shared_ptr<const TLSIdentity> read(std::string_view certificates, std::string_view key); // Load unencrypted textual credentials without treating their chain as a trust decision.
	const std::vector<Cert::Ptr> &chain() const { return certificates; } // Inspect immutable certificate encodings for handshake transmission.
	const SigningKey &signer() const { return *private_key; } // Use the validated immutable signer after successful credential construction.
};
}
