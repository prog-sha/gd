// Maintain independent header compression state for each direction of a multiplexed connection.
#pragma once
#include "huffman_core.h"
#include <deque>
#include <functional>
#include <unordered_map>

namespace GDH2 {
struct Field {
	std::string name, value; // Opaque header octets before semantic validation.
	bool sensitive = false; // Never-indexed forwarding requirement.
	uint64_t size() const { return uint64_t(name.size()) + value.size() + 32; } // Uncompressed table accounting.
};
// Index recent entries by insertion identity, avoiding scans or renumbering after eviction.
class HeaderTable {
	struct Entry { Field field; uint64_t id; }; // Owned field and stable insertion identity.
	struct Index {
		uint64_t newest = 0; // Most recently inserted field sharing this name.
		std::unordered_map<std::string, uint64_t> values; // Latest exact matches.
	};
	std::deque<Entry> entries; // Newest entries first for direct wire index lookup.
	std::unordered_map<std::string, Index> lookup; // Encoder-only reverse index.
	uint64_t used = 0, serial = 0; // Accounted octets and monotonic insertion identity.
	uint32_t capacity = 4096; // Initial advertised dynamic-table size.
	bool indexed; // Decoder tables do not allocate reverse indexes.
	void evict(); // Remove oldest fields and stale reverse indexes.
public:
	explicit HeaderTable(bool search) : indexed(search) {} // Select direction-specific indexing costs.
	void resize(uint32_t value); // Apply the negotiated capacity and evict immediately.
	void add(const Field &field); // Insert a complete field with protocol overhead accounting.
	const Field *at(uint64_t index) const; // Resolve static and dynamic wire indexes.
	uint64_t find(const Field &field, bool &exact) const; // Prefer a static exact match, then dynamic exact or named matches.
	uint32_t limit() const { return capacity; } // Report current table capacity.
	uint64_t size() const { return used; } // Report currently retained protocol octets.
};
// Decode each representation once, retaining only unfinished field state across frame boundaries.
class HeaderDecoder {
	enum Phase { FIELD, INTEGER, NAME_LENGTH, VALUE_LENGTH, STRING }; // Incremental representation positions.
	enum Number { INDEX, NAME, TABLE_SIZE, LENGTH }; // Meaning assigned to the active prefixed integer.
	HeaderTable table{false};
	Phase phase = FIELD;
	Number number = INDEX;
	uint64_t integer = 0, remaining = 0; // Active integer and encoded string remainder.
	unsigned shift = 0; // Position of the next base-128 continuation digit.
	uint32_t allowed = 4096; // Negotiated table bound.
	size_t max_string = 0; // Optional decoded and encoded string bound; zero is unlimited.
	Field field;
	Huffman huffman;
	bool valid = true, first = true, indexing = false, name_string = false, compressed = false, emitting = true;
	std::function<void(Field &&)> emit; // Synchronous field consumer, allowed to disable further emission.
	bool start_number(uint8_t byte, unsigned bits, Number kind); // Consume the prefix and transition only when complete.
	bool complete_number(); // Apply one validated integer to the active representation.
	bool complete_string(); // Validate padding and move from name to value or publish the field.
	bool publish(); // Commit table insertion and deliver one complete field.
public:
	void begin(std::function<void(Field &&)> callback, size_t string_limit = 0); // Start a block without resetting compression history.
	void allow(uint32_t value) { allowed = value; } // Set the peer's acknowledged table-size ceiling.
	void emitting_fields(bool value) { emitting = value; } // Preserve compression history while discarding an oversized field list.
	bool feed(const uint8_t *data, size_t size); // Consume arbitrary fragments without retaining caller storage.
	bool finish(); // Reject an incomplete representation and end a header block.
	uint64_t table_size() const { return table.size(); } // Inspect retained state for accounting tests.
};
// Encode complete fields using peer settings and table sizes applied at block boundaries.
class HeaderEncoder {
	HeaderTable table{true};
	uint32_t ceiling = 4096, smallest = UINT32_MAX; // Local compression budget and pending minimum capacity.
	bool update = false; // Whether the next block must announce a capacity change.
public:
	void limit(uint32_t value); // Bound encoder memory independently of the peer's allowed size.
	void resize(uint32_t value); // Apply a peer size change and remember intermediate shrinkage.
	void begin(std::vector<uint8_t> &out); // Emit pending size changes before any block fields.
	void write(const Field &field, std::vector<uint8_t> &out); // Append a representation, preserving sensitive-field semantics.
	uint64_t table_size() const { return table.size(); } // Inspect retained state for accounting tests.
};
}
