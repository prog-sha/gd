// Reject malformed encodings before advancing borrowed certificate cursors.
#include "der_core.h"

// Read a canonical definite-length field without allocating or traversing its contents.
bool GDCrypto::DER::any(uint8_t &tag, Bytes &value, Bytes *encoded) {
	if (!rest.data || rest.size < 2 || (rest.data[0] & 31) == 31) return false;
	size_t offset = 2, size = rest.data[1];
	if (size & 128) {
		const size_t width = size & 127;
		if (!width || width > sizeof(size_t) || width > rest.size - offset || rest.data[offset] == 0) return false;
		size = 0;
		for (size_t at = 0; at != width; ++at) size = (size << 8) | rest.data[offset++];
		if (size < 128) return false;
	}
	if (size > rest.size - offset) return false;
	tag = rest.data[0];
	value = {rest.data + offset, size};
	if (encoded) *encoded = {rest.data, offset + size};
	rest.data += offset + size; rest.size -= offset + size;
	return true;
}

// Match the expected field tag without consuming a mismatched or malformed value.
bool GDCrypto::DER::take(uint8_t tag, Bytes &value, Bytes *encoded) {
	if (!peek(tag)) return false;
	uint8_t actual = 0;
	return any(actual, value, encoded);
}

// Validate minimal nonnegative integer encoding and strip only its sign byte.
bool GDCrypto::DER::integer(Bytes &value) {
	DER copy = *this;
	Bytes integer;
	if (!copy.take(2, integer) || !integer.size || (integer.data[0] & 128)) return false;
	if (integer.size > 1 && integer.data[0] == 0) {
		if (!(integer.data[1] & 128)) return false;
		++integer.data; --integer.size;
	}
	value = integer; *this = copy;
	return true;
}

// Accumulate a bounded integer only after validating its full encoding.
bool GDCrypto::DER::number(uint64_t &value) {
	DER copy = *this;
	Bytes integer;
	if (!copy.integer(integer) || integer.size > sizeof(value)) return false;
	uint64_t result = 0;
	for (size_t at = 0; at != integer.size; ++at) result = (result << 8) | integer.data[at];
	value = result; *this = copy;
	return true;
}

// Decode the two allowed canonical boolean byte values.
bool GDCrypto::DER::boolean(bool &value) {
	DER copy = *this;
	Bytes item;
	if (!copy.take(1, item) || item.size != 1 || (item.data[0] != 0 && item.data[0] != 255)) return false;
	value = item.data[0] != 0; *this = copy;
	return true;
}

// Validate the unused-bit count and require all padding bits to be zero.
bool GDCrypto::DER::bits(Bytes &value, uint8_t &unused) {
	DER copy = *this;
	Bytes item;
	if (!copy.take(3, item) || !bit_value(item, value, unused)) return false;
	*this = copy;
	return true;
}

// Validate shared bit-string contents without assuming the schema's enclosing tag.
bool GDCrypto::DER::bit_value(Bytes input, Bytes &value, uint8_t &unused) {
	if (!input.data || !input.size || input.data[0] > 7 ||
			(input.size == 1 ? input.data[0] != 0 : (input.data[input.size - 1] & ((1U << input.data[0]) - 1)) != 0)) return false;
	unused = input.data[0]; value = { input.data + 1, input.size - 1 };
	return true;
}

// Require octet-aligned public-key or signature bits.
bool GDCrypto::DER::octets(Bytes &value) {
	DER copy = *this;
	Bytes item;
	uint8_t unused = 0;
	if (!copy.bits(item, unused) || unused) return false;
	value = item; *this = copy;
	return true;
}

// Validate minimal base-128 arcs without converting them to bounded machine integers.
bool GDCrypto::DER::oid(Bytes &value) {
	DER copy = *this;
	Bytes item;
	if (!copy.take(6, item) || !item.size) return false;
	bool first = true;
	for (size_t at = 0; at != item.size; ++at) {
		if (first && item.data[at] == 128) return false;
		first = !(item.data[at] & 128);
	}
	if (!first) return false;
	value = item; *this = copy;
	return true;
}
