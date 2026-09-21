// Keep wire bounds, duplicate detection, and cursor rollback separate from handshake-state decisions.
#include "handshake_wire.h"
#include "cli/data/crypto_core.h"
#include <limits>

namespace GDCrypto {
// Consume fixed fields without allowing an overflowing end-position calculation.
bool TLSView::take(size_t count, Bytes &value) {
	if (count > rest.size || (count && !rest.data)) return false;
	value = {rest.data,count}; if (count) rest.data += count; rest.size -= count; return true;
}

// Decode network-order integer fields without exposing a partial result on truncation.
bool TLSView::number(unsigned width, uint32_t &value) {
	if (!width || width > 4) return false;
	Bytes field; if (!take(width,field)) return false;
	uint32_t decoded = 0; for (size_t at = 0; at != width; ++at) decoded = (decoded<<8)|field.data[at];
	value = decoded; return true;
}

// Advance past a prefix only if the entire declared vector is present.
bool TLSView::vector(unsigned width, Bytes &value) {
	TLSView cursor = *this; uint32_t count; Bytes field;
	if (!cursor.number(width,count) || !cursor.take(count,field)) return false;
	*this = cursor; value = field; return true;
}

// Encode integers under their actual wire width rather than an application-size policy.
bool TLSWriter::number(unsigned width, uint32_t value) {
	if (!valid || !width || width > 4 || (width < 4 && value >= (uint32_t(1)<<(width*8)))) {valid = false; return false;}
	for (unsigned at = width; at; --at) bytes.push_back(uint8_t(value>>((at-1)*8)));
	return true;
}

// Copy valid borrowed fields while keeping the builder unusable after any invalid input.
bool TLSWriter::append(Bytes value) {
	if (!valid || (value.size && !value.data) || value.size > bytes.max_size()-bytes.size()) {valid = false; return false;}
	std::vector<uint8_t> copy; if (!stabilize(value,copy)) return false;
	if (value.size) bytes.insert(bytes.end(),value.data,value.data+value.size);
	return true;
}

// Check the encoded length before narrowing size_t or appending payload bytes.
bool TLSWriter::vector(unsigned width, Bytes value) {
	if (value.size > std::numeric_limits<uint32_t>::max()) {valid = false; return false;}
	std::vector<uint8_t> copy; if (!stabilize(value,copy)) return false;
	return number(width,uint32_t(value.size)) && append(value);
}

// Snapshot only a fully contained self-reference, rejecting partial intersections with unowned memory.
bool TLSWriter::stabilize(Bytes &value, std::vector<uint8_t> &copy) {
	if (!valid || (value.size && !value.data)) {valid = false; return false;}
	if (overlap(value.data,value.size,bytes.data(),bytes.size())) {
		const uintptr_t offset = reinterpret_cast<uintptr_t>(value.data)-reinterpret_cast<uintptr_t>(bytes.data());
		if (offset > bytes.size() || value.size > bytes.size()-offset) {valid = false; return false;}
		copy.assign(value.data,value.data+value.size); value = {copy.data(),copy.size()};
	}
	return true;
}

// Publish only valid construction and leave an existing destination unchanged after failure.
bool TLSWriter::finish(std::vector<uint8_t> &output) {
	if (!valid) return false;
	output = std::move(bytes); bytes.clear(); return true;
}

// Reject duplicate extension identifiers before their message-specific contents are interpreted.
bool tls_extensions(Bytes bytes, TLSExtensions &output) {
	TLSView reader(bytes); TLSExtensions values;
	while (!reader.empty()) {
		uint32_t kind; Bytes field;
		if (!reader.number(2,kind) || !reader.vector(2,field) || !values.emplace(uint16_t(kind),field).second) return false;
	}
	output = std::move(values); return true;
}

// Validate hello framing while retaining unknown extensions for the negotiating state machine.
bool TLSHello::read(Bytes body, bool client, TLSHello &output) {
	TLSView reader(body); TLSHello hello; uint32_t value;
	if (!reader.number(2,value) || !reader.take(32,hello.random) || !reader.vector(1,hello.session) || hello.session.size > 32) return false;
	hello.version = uint16_t(value);
	if (client) {
		if (!reader.vector(2,hello.suites) || !hello.suites.size || hello.suites.size%2 || !reader.vector(1,hello.compression) || !hello.compression.size) return false;
	} else {
		if (!reader.number(2,value) || !reader.take(1,hello.compression)) return false;
		hello.suite = uint16_t(value);
	}
	if (!reader.empty()) {Bytes list; if (!reader.vector(2,list) || !tls_extensions(list,hello.extensions) || !reader.empty()) return false;}
	output = std::move(hello); return true;
}
}
