// Encode and incrementally decode header compression representations with bounded table ownership.
#include "hpack_core.h"
#include <algorithm>
#include <limits>

namespace GDH2 {
namespace {
const Field STATIC[] = { // Fixed wire indexes, with an unused zero entry.
	{}, {":authority", ""}, {":method", "GET"}, {":method", "POST"}, {":path", "/"}, {":path", "/index.html"},
	{":scheme", "http"}, {":scheme", "https"}, {":status", "200"}, {":status", "204"}, {":status", "206"},
	{":status", "304"}, {":status", "400"}, {":status", "404"}, {":status", "500"},
	{"accept-charset", ""}, {"accept-encoding", "gzip, deflate"}, {"accept-language", ""}, {"accept-ranges", ""},
	{"accept", ""}, {"access-control-allow-origin", ""}, {"age", ""}, {"allow", ""}, {"authorization", ""},
	{"cache-control", ""}, {"content-disposition", ""}, {"content-encoding", ""}, {"content-language", ""},
	{"content-length", ""}, {"content-location", ""}, {"content-range", ""}, {"content-type", ""}, {"cookie", ""},
	{"date", ""}, {"etag", ""}, {"expect", ""}, {"expires", ""}, {"from", ""}, {"host", ""}, {"if-match", ""},
	{"if-modified-since", ""}, {"if-none-match", ""}, {"if-range", ""}, {"if-unmodified-since", ""},
	{"last-modified", ""}, {"link", ""}, {"location", ""}, {"max-forwards", ""}, {"proxy-authenticate", ""},
	{"proxy-authorization", ""}, {"range", ""}, {"referer", ""}, {"refresh", ""}, {"retry-after", ""},
	{"server", ""}, {"set-cookie", ""}, {"strict-transport-security", ""}, {"transfer-encoding", ""},
	{"user-agent", ""}, {"vary", ""}, {"via", ""}, {"www-authenticate", ""},
};
constexpr size_t STATIC_END = sizeof(STATIC) / sizeof(*STATIC); // First dynamic wire index.
// Cache immutable name-to-static-index groups without per-field linear table scans.
const std::unordered_map<std::string, std::vector<uint64_t>> &static_names() {
	static const auto names = [] {
		std::unordered_map<std::string, std::vector<uint64_t>> result;
		for (uint64_t i = 1; i < STATIC_END; ++i) result[STATIC[i].name].push_back(i);
		return result;
	}();
	return names;
}
// Append a prefixed base-128 integer while retaining representation flags.
void put_number(uint64_t value, unsigned bits, uint8_t flags, std::vector<uint8_t> &out) {
	const unsigned prefix = (1u << bits) - 1;
	out.push_back(flags | uint8_t(std::min<uint64_t>(prefix, value)));
	if (value < prefix) return;
	value -= prefix;
	while (value >= 128) { out.push_back(uint8_t(value) | 128); value >>= 7; }
	out.push_back(uint8_t(value));
}
// Compress only strings whose complete encoded length becomes shorter.
void put_string(std::string_view value, std::vector<uint8_t> &out) {
	const size_t length = Huffman::size(value);
	const bool compressed = length < value.size();
	put_number(compressed ? length : value.size(), 7, compressed ? 128 : 0, out);
	if (compressed) Huffman::encode(value, out);
	else out.insert(out.end(), value.begin(), value.end());
}
}
// Evict oldest fields without invalidating newer duplicates in the encoder index.
void HeaderTable::evict() {
	while (used > capacity && !entries.empty()) {
		const Entry &old = entries.back();
		if (indexed) {
			auto name = lookup.find(old.field.name);
			auto value = name->second.values.find(old.field.value);
			if (value != name->second.values.end() && value->second == old.id) name->second.values.erase(value);
			if (name->second.newest == old.id) lookup.erase(name);
		}
		used -= old.field.size();
		entries.pop_back();
	}
}
// Apply capacity changes before another field is admitted.
void HeaderTable::resize(uint32_t value) {
	capacity = value;
	evict();
}
// Preserve field ownership through eviction, including insertion from a borrowed table entry.
void HeaderTable::add(const Field &field) {
	if (field.size() > capacity) { entries.clear(); lookup.clear(); used = 0; return; }
	Field owned = field;
	owned.sensitive = false;
	if (serial == UINT64_MAX) {
		serial = entries.size();
		lookup.clear();
		for (size_t i = 0; i < entries.size(); ++i) entries[i].id = serial - i;
		if (indexed) for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
			Index &index = lookup[it->field.name];
			index.newest = it->id;
			index.values[it->field.value] = it->id;
		}
	}
	used += owned.size();
	entries.push_front({std::move(owned), ++serial});
	if (indexed) {
		Index &index = lookup[entries.front().field.name];
		index.newest = serial;
		index.values[entries.front().field.value] = serial;
	}
	evict();
}
// Resolve one-based static indexes and reverse-chronological dynamic indexes.
const Field *HeaderTable::at(uint64_t index) const {
	if (!index) return nullptr;
	if (index < STATIC_END) return &STATIC[index];
	index -= STATIC_END;
	return index < entries.size() ? &entries[size_t(index)].field : nullptr;
}
// Reuse exact values unless sensitivity requires a literal representation.
uint64_t HeaderTable::find(const Field &field, bool &exact) const {
	exact = false;
	uint64_t named = 0;
	auto fixed = static_names().find(field.name);
	if (fixed != static_names().end()) {
		named = fixed->second.front();
		if (!field.sensitive) for (uint64_t i : fixed->second) {
			if (STATIC[i].value == field.value) { exact = true; return i; }
		}
	}
	auto name = lookup.find(field.name);
	if (name != lookup.end()) {
		auto value = name->second.values.find(field.value);
		if (!field.sensitive && value != name->second.values.end()) {
			exact = true;
			return STATIC_END + serial - value->second;
		}
		if (!named) named = STATIC_END + serial - name->second.newest;
	}
	return named;
}
// Set the next block's consumer only after the preceding representation is complete.
void HeaderDecoder::begin(std::function<void(Field &&)> callback, size_t string_limit) {
	if (phase != FIELD) valid = false;
	emit = std::move(callback);
	max_string = string_limit;
	first = true;
	emitting = true;
}
// Decode the inline integer prefix or retain a continuation state.
bool HeaderDecoder::start_number(uint8_t byte, unsigned bits, Number kind) {
	number = kind;
	integer = byte & ((1u << bits) - 1);
	shift = 0;
	phase = INTEGER;
	return integer == ((1u << bits) - 1) || complete_number();
}
// Validate indexes and lengths before allocating decoded string storage.
bool HeaderDecoder::complete_number() {
	if (number == TABLE_SIZE) {
		if (!first || integer > allowed) return false;
		table.resize(uint32_t(integer));
		phase = FIELD;
	} else if (number == INDEX || (number == NAME && integer)) {
		const Field *found = table.at(integer);
		if (!found) return false;
		field.name = found->name;
		if (number == INDEX) { field.value = found->value; return publish(); }
		phase = VALUE_LENGTH;
	} else if (number == NAME) {
		phase = NAME_LENGTH;
	} else {
		if (integer > SIZE_MAX || (max_string && integer > max_string)) return false;
		remaining = integer;
		huffman = Huffman{};
		phase = STRING;
		if (!remaining) return complete_string();
	}
	return true;
}
// Finish a complete string without accepting invalid terminal padding.
bool HeaderDecoder::complete_string() {
	if (compressed && !huffman.finish()) return false;
	if (name_string) phase = VALUE_LENGTH;
	else return publish();
	return true;
}
// Commit only complete fields and preserve table state when emission is disabled.
bool HeaderDecoder::publish() {
	if (max_string && (field.name.size() > max_string || field.value.size() > max_string)) return false;
	if (indexing) table.add(field);
	phase = FIELD;
	first = false;
	if (emitting && emit) emit(std::move(field));
	field = {};
	return true;
}
// Advance one finite-state transition per encoded integer digit or string fragment.
bool HeaderDecoder::feed(const uint8_t *data, size_t size) {
	if (!valid || (!data && size)) return valid = false;
	for (size_t at = 0; at < size && valid;) {
		const uint8_t byte = data[at++];
		if (phase == FIELD) {
			field = {};
			indexing = (byte & 0xc0) == 0x40;
			field.sensitive = (byte & 0xf0) == 0x10;
			if (byte & 0x80) valid = start_number(byte, 7, INDEX);
			else if (indexing) valid = start_number(byte, 6, NAME);
			else if (byte & 0x20) valid = start_number(byte, 5, TABLE_SIZE);
			else valid = start_number(byte, 4, NAME);
		} else if (phase == INTEGER) {
			integer += uint64_t(byte & 127) << shift;
			if (!(byte & 128)) valid = complete_number();
			else { shift += 7; if (shift >= 63) valid = false; }
		} else if (phase == NAME_LENGTH || phase == VALUE_LENGTH) {
			name_string = phase == NAME_LENGTH;
			compressed = byte & 128;
			valid = start_number(byte, 7, LENGTH);
		} else {
			std::string &text = name_string ? field.name : field.value;
			const bool keep = emitting || indexing;
			if (compressed) valid = huffman.byte(byte, text, max_string, keep);
			else {
				const size_t take = size_t(std::min<uint64_t>(remaining, size - at + 1));
				if (keep) text.append(reinterpret_cast<const char *>(data + at - 1), take);
				at += take - 1;
				remaining -= take - 1;
			}
			if (--remaining == 0 && valid) valid = complete_string();
		}
	}
	return valid;
}
// Close the block only at a representation boundary.
bool HeaderDecoder::finish() {
	if (phase != FIELD) valid = false;
	first = true;
	return valid;
}
// Apply a local encoder memory bound without enlarging an already smaller table.
void HeaderEncoder::limit(uint32_t value) {
	ceiling = value;
	if (table.limit() > value) resize(value);
}
// Preserve the smallest pending size so shrink-then-grow changes evict identically on the peer.
void HeaderEncoder::resize(uint32_t value) {
	value = std::min(value, ceiling);
	smallest = std::min(smallest, value);
	table.resize(value);
	update = true;
}
// Synchronize table-size changes before the next block starts.
void HeaderEncoder::begin(std::vector<uint8_t> &out) {
	if (!update) return;
	if (smallest < table.limit()) put_number(smallest, 5, 0x20, out);
	put_number(table.limit(), 5, 0x20, out);
	smallest = UINT32_MAX;
	update = false;
}
// Select indexed, literal, or never-indexed representations without rescanning the dynamic table.
void HeaderEncoder::write(const Field &field, std::vector<uint8_t> &out) {
	bool exact;
	const uint64_t index = table.find(field, exact);
	if (exact) { put_number(index, 7, 0x80, out); return; }
	const bool indexing = !field.sensitive && field.size() <= table.limit();
	put_number(index, indexing ? 6 : 4, field.sensitive ? 0x10 : indexing ? 0x40 : 0, out);
	if (!index) put_string(field.name, out);
	put_string(field.value, out);
	if (indexing) table.add(field);
}
}
