// Build immutable canonical prefix-code transitions for compressed header strings.
#include "huffman_core.h"
#include <array>

namespace GDH2 {
namespace {
constexpr uint8_t WIDTHS[] = { // Wire symbol lengths in octet order, followed by the terminal symbol.
	13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28,
	28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	6, 10, 10, 12, 13, 6, 8, 11, 10, 10, 8, 11, 8, 6, 6, 6,
	5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8, 15, 6, 12, 10,
	13, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6,
	15, 5, 6, 5, 6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5,
	6, 7, 6, 5, 5, 6, 7, 7, 7, 7, 7, 15, 11, 14, 13, 28,
	20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
	24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
	22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23,
	21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
	26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
	19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
	20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
	26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26,
	30,
};
struct Node {
	int child[2] = {-1, -1}; // Unfinished bit paths.
	int symbol = -1; // Completed octet or forbidden terminal.
	unsigned depth = 0; // Bits since the preceding completed symbol.
	bool ones = true; // Whether this path is a terminal-prefix candidate.
};
struct Step {
	int16_t next = 0, symbol = -1; // Path and optional single octet after one nibble.
};
// Precompute two transitions per input byte from canonical code lengths.
struct Codes {
	std::array<uint32_t, 257> words{}; // Right-aligned canonical codes.
	std::vector<Node> nodes{Node{}};
	std::vector<std::array<Step, 16>> steps;
	Codes() {
		uint32_t code = 0;
		for (unsigned width = 1; width <= 30; ++width) {
			code <<= 1;
			for (unsigned symbol = 0; symbol < 257; ++symbol) {
				if (WIDTHS[symbol] != width) continue;
				words[symbol] = code++;
				int at = 0;
				for (unsigned bit = width; bit-- > 0;) {
					unsigned one = (words[symbol] >> bit) & 1;
					int next = nodes[at].child[one];
					if (next < 0) {
						next = int(nodes.size());
						Node n;
						n.depth = nodes[at].depth + 1;
						n.ones = nodes[at].ones && one;
						nodes[at].child[one] = next;
						nodes.push_back(n);
					}
					at = next;
				}
				nodes[at].symbol = int(symbol);
			}
		}
		steps.resize(nodes.size());
		for (size_t at = 0; at < nodes.size(); ++at) {
			for (unsigned nibble = 0; nibble < 16; ++nibble) {
				Step &step = steps[at][nibble];
				int next = int(at);
				for (unsigned bit = 4; bit-- > 0;) {
					next = nodes[next].child[(nibble >> bit) & 1];
					if (next < 0 || nodes[next].symbol == 256) { next = -1; break; }
					if (nodes[next].symbol >= 0) {
						step.symbol = int16_t(nodes[next].symbol);
						next = 0;
					}
				}
				step.next = int16_t(next);
			}
		}
	}
};
// Initialize protocol tables once with thread-safe immutable publication.
const Codes &codes() {
	static const Codes table;
	return table;
}
}
// Consume bounded nibble transitions without retaining compressed input.
bool Huffman::byte(uint8_t value, std::string &out, size_t limit, bool keep) {
	if (!valid) return false;
	for (unsigned shift : {4u, 0u}) {
		const Step &step = codes().steps[node][(value >> shift) & 15];
		if (step.next < 0 || (step.symbol >= 0 && keep && limit && out.size() >= limit)) return valid = false;
		node = uint16_t(step.next);
		if (step.symbol >= 0 && keep) out.push_back(char(step.symbol));
	}
	return true;
}
// Reject excessive, nonterminal, or explicit terminal padding.
bool Huffman::finish() const {
	return valid && codes().nodes[node].depth <= 7 && codes().nodes[node].ones;
}
// Sum code widths using byte-sized contributions without bit-count overflow.
size_t Huffman::size(std::string_view value) {
	size_t bytes = 0;
	unsigned bits = 0;
	for (unsigned char ch : value) {
		bits += WIDTHS[ch];
		bytes += bits / 8;
		bits %= 8;
	}
	return bytes + (bits != 0);
}
// Append canonical codes with an accumulator wider than one symbol plus pending bits.
void Huffman::encode(std::string_view value, std::vector<uint8_t> &out) {
	uint64_t word = 0;
	unsigned bits = 0;
	for (unsigned char ch : value) {
		word = (word << WIDTHS[ch]) | codes().words[ch];
		bits += WIDTHS[ch];
		while (bits >= 8) { bits -= 8; out.push_back(uint8_t(word >> bits)); }
	}
	if (bits) out.push_back(uint8_t((word << (8 - bits)) | ((1u << (8 - bits)) - 1)));
}
}
