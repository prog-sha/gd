// Share negotiation identifiers and signature constraints without exposing private connection state.
#pragma once
#include "handshake_wire.h"
#include "signature_core.h"
#include <array>
#include <string>

namespace GDCrypto {
inline constexpr std::array<uint16_t,10> tls_signatures{0x0804,0x0403,0x0807,0x0805,0x0806,0x0401,0x0501,0x0601,0x0503,0x0603}; // Strong supported schemes in preference order; legacy SHA-1 is not enabled.
inline constexpr std::array<uint8_t,32> tls_retry_random{0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11,0xbe,0x1d,0x8c,0x02,0x1e,0x65,0xb8,0x91,0xc2,0xa2,0x11,0x16,0x7a,0xbb,0x8c,0x5e,0x07,0x9e,0x09,0xe2,0xc8,0xa8,0x33,0x9c}; // Protocol-defined retry discriminator, not random key material.

bool tls_list(Bytes field, unsigned width, std::vector<uint16_t> &output); // Decode a complete nonempty vector of 16-bit negotiation identifiers.
bool tls_signature(uint16_t scheme, KeyKind family, size_t bits, bool modern, SignatureInfo &output); // Bind a scheme to its key family, encoding capacity, and protocol-specific curve requirements.
std::vector<uint16_t> tls_suites(bool peer_chacha = false); // Prefer an accelerated AEAD when both peers benefit, without changing supported suite coverage.
bool tls_extension(TLSWriter &output, uint16_t kind, const TLSWriter &body); // Append a valid nested extension without hiding child-builder errors.
bool tls_protocols(Bytes bytes, std::vector<std::string> &output); // Decode a nonempty list of exact nonempty application protocol names.
}
