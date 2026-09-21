// Convert IP literals into values for normalization and CIDR matching without OS queries.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

// Internal value distinguishing IPv4, IPv6, and invalid addresses; not a public basic type.
class GDIP {
	uint64_t hi = 0; // Upper 64 bits of an IPv6 address.
	uint64_t lo = 0; // Lower 64 bits of IPv6, or lower 32 bits of IPv4.
	int width = 0; // Address width: 0 for invalid, 32 for IPv4, 128 for IPv6.
	std::string zone; // IPv6 scope preserved without OS resolution.

public:
	// Parse strict IPv4 or IPv6 notation, returning an invalid value on failure.
	static GDIP parse(std::string_view p_text);
	// Return the address family's width in bits.
	int bit_len() const { return width; }
	// Report a scope that cannot participate in CIDR matching.
	bool zoned() const { return !zone.empty(); }
	size_t pack(uint8_t *p_output) const; // Write four or sixteen network-order bytes into a sixteen-byte destination, excluding the zone.
	// Preserve the family and abbreviate the longest run of zeros.
	std::string text() const;
	// Compare the requested prefix bits, treating mapped IPv6 as IPv6.
	bool contains(const GDIP &p_ip, int p_bits) const;
};
