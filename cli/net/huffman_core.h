// Compress header octets and validate incremental prefix-code strings.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GDH2 {
// Retain only the unfinished code path between compressed string fragments.
class Huffman {
	uint16_t node = 0; // Current decoding path within the immutable prefix tree.
	bool valid = true; // Sticky invalid code or forbidden terminal symbol.
public:
	bool byte(uint8_t value, std::string &out, size_t limit = 0, bool keep = true); // Consume one encoded byte and append decoded octets.
	bool finish() const; // Require an empty path or at most seven terminal-prefix padding bits.
	static size_t size(std::string_view value); // Compute encoded length without allocating output.
	static void encode(std::string_view value, std::vector<uint8_t> &out); // Append complete octets and terminal-prefix padding.
};
}
