// Parse canonical certificate fields as borrowed slices without recursion or whole-message copies.
#pragma once
#include <cstddef>
#include <cstdint>

namespace GDCrypto {
// Borrow an immutable byte range whose owner outlives the parser and its results.
struct Bytes {
	const uint8_t *data = nullptr; // First borrowed byte.
	size_t size = 0; // Available byte count.
};

// Consume low-tag-number certificate fields with transactional failure behavior.
class DER {
	Bytes rest; // Unconsumed bytes, retained unchanged after an invalid field.
public:
	explicit DER(Bytes input) : rest(input) {} // Borrow a complete encoded field sequence.
	bool empty() const { return rest.size == 0; } // Require complete consumption where the schema ends.
	Bytes remaining() const { return rest; } // Inspect unconsumed bytes without advancing.
	bool peek(uint8_t tag) const { return rest.size && rest.data && rest.data[0] == tag; } // Check an optional field without consuming it.
	bool any(uint8_t &tag, Bytes &value, Bytes *encoded = nullptr); // Read a definite minimal-length field and optionally retain its exact signature bytes.
	bool take(uint8_t tag, Bytes &value, Bytes *encoded = nullptr); // Consume a field only when its schema tag matches.
	bool integer(Bytes &value); // Read a nonnegative minimal integer, excluding a necessary sign-padding byte.
	bool number(uint64_t &value); // Read a nonnegative integer fitting the destination width.
	bool boolean(bool &value); // Read exactly the canonical false or true byte.
	bool bits(Bytes &value, uint8_t &unused); // Read a bit string and validate zero unused bits.
	static bool bit_value(Bytes input, Bytes &value, uint8_t &unused); // Validate bit-string contents under either a universal or implicit schema tag.
	bool octets(Bytes &value); // Read a bit string that contains complete bytes.
	bool oid(Bytes &value); // Read a minimally encoded object identifier without imposing an arc-value bound.
};
}
