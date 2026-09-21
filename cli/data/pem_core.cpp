// Scan textual containers once and decode their payload without whitespace-copy buffers.
#include "pem_core.h"

namespace GDCrypto {
namespace {
constexpr std::string_view begin_marker = "-----BEGIN ", end_marker = "-----END "; // Case-sensitive line markers framing a labeled container.

// Remove permitted horizontal padding from one physical line.
std::string_view trim_line(std::string_view line, bool newline) {
	if (newline && !line.empty() && line.back() == '\r') line.remove_suffix(1);
	while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
	return line;
}

// Translate a standard base64 digit without locale-dependent character classification.
int digit(unsigned char byte) {
	if (byte >= 'A' && byte <= 'Z') return byte-'A';
	if (byte >= 'a' && byte <= 'z') return byte-'a'+26;
	if (byte >= '0' && byte <= '9') return byte-'0'+52;
	return byte == '+' ? 62 : byte == '/' ? 63 : -1;
}

// Decode padded quartets while ignoring only the transport's four permitted spacing bytes.
bool payload(std::string_view text, CryptoStorage<uint8_t,0> &output) {
	unsigned value = 0, count = 0, padding = 0; bool ended = false;
	for (unsigned char byte : text) {
		if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') continue;
		if (ended) return false;
		const int decoded = digit(byte);
		if (byte == '=') {if (count < 2 || ++padding > 2) return false;}
		else if (decoded < 0 || padding) return false;
		value = (value<<6) | unsigned(decoded < 0 ? 0 : decoded);
		if (++count == 4) {
			output.push_back(uint8_t(value>>16));
			if (padding < 2) output.push_back(uint8_t(value>>8));
			if (!padding) output.push_back(uint8_t(value));
			ended = padding != 0; count = value = 0;
		}
	}
	return count == 0;
}
}

// Retain only the most recent opening marker before each closing line, avoiding nested rescans.
bool decode_pem(std::string_view &input, PEM &output) {
	std::string_view label; size_t body = 0; bool candidate = false, headers = false, in_headers = false;
	for (size_t at = 0; at < input.size();) {
		const size_t newline = input.find('\n',at), next = newline == input.npos ? input.size() : newline+1;
		const auto raw = input.substr(at,(newline == input.npos ? input.size() : newline)-at);
		const auto line = trim_line(raw,newline != input.npos);
		if (at && line.substr(0,end_marker.size()) == end_marker) {
			if (candidate && !(in_headers && line.find(':') != line.npos) && line.size() == end_marker.size()+label.size()+5 && line.substr(end_marker.size(),label.size()) == label && line.substr(line.size()-5) == "-----") {
				PEM result; result.label = label; result.headers = headers;
				if ((!headers || body < at) && payload(input.substr(body,at-body),result.bytes)) {output = std::move(result); input.remove_prefix(next); return true;}
			}
			candidate = false;
		}
		const size_t marker = line.rfind(begin_marker);
		if (marker != line.npos) {
			candidate = marker == 0 && line.size() >= begin_marker.size()+5 && line.substr(line.size()-5) == "-----";
			if (candidate) {label = line.substr(begin_marker.size(),line.size()-begin_marker.size()-5); body = next; headers = false; in_headers = true;}
		} else if (candidate && in_headers) {
			if (line.find(':') != line.npos) {headers = true; body = next;}
			else in_headers = false;
		}
		at = next;
	}
	return false;
}
}
