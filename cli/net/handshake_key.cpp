// Generate fresh handshake shares with rejection sampling and validated constant-width agreement.
#include "handshake_key.h"
#include "cli/data/x25519_core.h"
#include "cli/data/hash_core.h"

namespace GDCrypto {
// Map wire identifiers to mathematical domains rather than treating a buffer capacity as a group policy.
unsigned TLSShare::bits(uint16_t selected) { return selected == 29 ? 255 : selected == 23 ? 256 : selected == 24 ? 384 : selected == 25 ? 521 : 0; }

// Replace key material only with a fresh scalar sampled in the selected group's valid range.
bool TLSShare::reset(uint16_t selected) {
	group = 0; wipe_storage(secret); secret.clear(); point.clear();
	const unsigned width = bits(selected); if (!width) return false;
	secret.resize((width+7)/8);
	if (selected == 29) {
		point.resize(32);
		if (!random_fill(secret.data(),secret.size()) || !x25519_public(secret.data(),point.data())) {wipe_storage(secret); point.clear(); return false;}
	} else {
		if (!curve.reset(width)) return false;
		point.resize(1+2*secret.size());
		for (;;) {
			if (!random_fill(secret.data(),secret.size())) {wipe_storage(secret); point.clear(); return false;}
			if (width%8) secret[0] &= uint8_t((1U<<(width%8))-1);
			if (curve.public_key({secret.data(),secret.size()},point.data(),point.size())) break;
		}
	}
	group = selected; return true;
}

// Keep failed or non-contributory peer shares from replacing an already held shared secret.
bool TLSShare::agree(Bytes peer, CryptoStorage<uint8_t,0> &output) const {
	if (!group || !peer.data) return false;
	CryptoStorage<uint8_t,0> shared(secret.size());
	if (group == 29) {if (peer.size != 32 || !x25519(secret.data(),peer.data,shared.data())) return false;}
	else if (!curve.shared({secret.data(),secret.size()},peer,shared.data(),shared.size())) return false;
	output = std::move(shared); return true;
}
}
