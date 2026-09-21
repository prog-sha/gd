// Resolve negotiation choices using protocol widths and the available independent cryptographic engines.
#include "handshake_protocol.h"
#include "cli/data/aes_core.h"

namespace GDCrypto {
// Decode identifier vectors transactionally and reject partial trailing identifiers.
bool tls_list(Bytes field, unsigned width, std::vector<uint16_t> &output) {
	TLSView reader(field); Bytes list;
	if (!reader.vector(width,list) || !reader.empty() || !list.size || list.size%2) return false;
	TLSView entries(list); std::vector<uint16_t> values; uint32_t value;
	while (!entries.empty()) {if (!entries.number(2,value)) return false; values.push_back(uint16_t(value));}
	output = std::move(values); return true;
}

// Require the exact key family and the protocol-specific curve/hash binding before private or public signature work.
bool tls_signature(uint16_t scheme, KeyKind family, size_t bits, bool modern, SignatureInfo &output) {
	if (family == KeyKind::RSA && bits < 1024) return false;
	SignatureInfo value;
	if (scheme == 0x0807) {if (family != KeyKind::ED25519) return false; value = {SignatureInfo::ED25519,0};}
	else if (scheme >= 0x0804 && scheme <= 0x0806) {
		value = {SignatureInfo::PSS,unsigned(scheme-0x0803)};
		const size_t width = value.hash == 1 ? 32 : value.hash == 2 ? 48 : 64;
		if (family != KeyKind::RSA || (bits+6)/8 < 2*width+2) return false;
	} else if ((scheme&255) == 3 && (scheme>>8) >= 4 && (scheme>>8) <= 6) {
		value = {SignatureInfo::EC,unsigned((scheme>>8)-3)};
		const size_t curve = value.hash == 1 ? 256 : value.hash == 2 ? 384 : 521;
		if (family != KeyKind::EC || (modern && bits != curve)) return false;
	} else if (!modern && (scheme&255) == 1 && (scheme>>8) >= 4 && (scheme>>8) <= 6) {
		value = {SignatureInfo::RSA,unsigned((scheme>>8)-3)};
		const size_t width = value.hash == 1 ? 32 : value.hash == 2 ? 48 : 64;
		if (family != KeyKind::RSA || (bits+7)/8 < width+30) return false;
	} else return false;
	output = value; return true;
}

// Retain every supported cipher while avoiding arithmetic-only block encryption as the first preference.
std::vector<uint16_t> tls_suites(bool peer_chacha) {
	if (!AES::accelerated() || peer_chacha) return {0x1303,0x1301,0x1302,0xcca9,0xcca8,0xc02b,0xc02f,0xc02c,0xc030,0xc009,0xc013,0xc00a,0xc014};
	return {0x1301,0x1302,0x1303,0xc02b,0xc02f,0xc02c,0xc030,0xcca9,0xcca8,0xc009,0xc013,0xc00a,0xc014};
}

// Propagate a failed nested encoder rather than accidentally emitting an empty valid extension.
bool tls_extension(TLSWriter &output, uint16_t kind, const TLSWriter &body) { return body.ok() && output.number(2,kind) && output.vector(2,body.view()); }

// Require a complete ALPN list while preserving opaque protocol-name bytes.
bool tls_protocols(Bytes bytes, std::vector<std::string> &output) {
	TLSView reader(bytes); Bytes list;
	if (!reader.vector(2,list) || !reader.empty() || !list.size) return false;
	TLSView entries(list); std::vector<std::string> values;
	while (!entries.empty()) {Bytes value; if (!entries.vector(1,value) || !value.size) return false; values.emplace_back(reinterpret_cast<const char *>(value.data),value.size);}
	output = std::move(values); return true;
}
}
