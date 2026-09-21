// Decode owned textual key containers without retaining plaintext secrets in released allocations.
#pragma once
#include "crypto_storage.h"
#include <string_view>

namespace GDCrypto {
// Keep decoded bytes owned while borrowing the label from the caller's encoded input.
struct PEM {
	std::string_view label; // Exact case-sensitive container type.
	CryptoStorage<uint8_t,0> bytes; // Decoded contents, erased across full allocation capacity on release.
	bool headers = false; // Presence of legacy metadata, without implying payload encryption or decryption.
};

bool decode_pem(std::string_view &input, PEM &output); // Consume one valid container and its preamble, preserving both arguments when none is found.
}
