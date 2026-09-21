// Load configured credentials atomically and reject mismatched public and private identities.
#include "identity_core.h"
#include "cli/data/pem_core.h"

namespace GDCrypto {
// Parse every configured certificate and the first private-key container before exposing credentials.
std::shared_ptr<const TLSIdentity> TLSIdentity::read(std::string_view certificates, std::string_view key) {
	std::shared_ptr<TLSIdentity> identity(new TLSIdentity); PEM block;
	while (decode_pem(certificates,block)) {
		if (block.label != "CERTIFICATE") continue;
		auto certificate = Cert::read({block.bytes.data(),block.bytes.size()}); if (!certificate) return {};
		identity->certificates.push_back(std::move(certificate));
	}
	if (identity->certificates.empty() || !identity->certificates.front()->key()) return {};
	while (decode_pem(key,block)) {
		constexpr std::string_view suffix = " PRIVATE KEY"; // Container suffix recognized independently of the enclosed private-key schema.
		if (block.label != "PRIVATE KEY" && (block.label.size() < suffix.size() || block.label.substr(block.label.size()-suffix.size()) != suffix)) continue;
		// Metadata cannot enable decryption; the enclosed bytes must themselves be a supported unencrypted key schema.
		identity->private_key = SigningKey::read({block.bytes.data(),block.bytes.size()});
		if (!identity->private_key || !identity->private_key->matches(identity->certificates.front()->info().public_key)) return {};
		return identity;
	}
	return {};
}
}
