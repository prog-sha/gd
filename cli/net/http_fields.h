// Share header validation and repeated response-field expansion across wire protocols.
#pragma once
#include "core/variant/dictionary.h"
#include <cstring>

// Reject separators, whitespace, and non-ASCII octets in field names.
inline bool http_field_name(const char *data, int size) {
	if (size <= 0) return false;
	for (int i = 0; i < size; ++i) {
		const uint8_t byte = uint8_t(data[i]);
		if (byte <= 32 || byte >= 127 || std::strchr("()<>@,;:\\\"/[]?={}",byte)) return false;
	}
	return true;
}
// Reject line breaks and nonwhitespace control octets in field values.
inline bool http_field_value(const char *data, int size) {
	for (int i = 0; i < size; ++i) {
		const uint8_t byte = uint8_t(data[i]);
		if ((byte < 32 && byte != '\t') || byte == 127) return false;
	}
	return true;
}
// Expand arrays and omit unsafe or transport-owned fields without per-response callback allocation.
template <class Emit>
void http_response_fields(const Dictionary *fields, uint64_t &dropped, Emit &&emit) {
	if (!fields) return;
	for (const KeyValue<Variant,Variant> &entry : *fields) {
		const String name = entry.key, low = name.to_lower();
		const CharString key = name.utf8();
		if (!http_field_name(key.get_data(),key.length()) || low == "content-length" || low == "transfer-encoding" || low == "connection") { ++dropped; continue; }
		Array values;
		if (entry.value.get_type() == Variant::ARRAY) values = entry.value;
		else if (entry.value.get_type() == Variant::PACKED_STRING_ARRAY) {
			const PackedStringArray many = entry.value;
			for (const String &value : many) values.push_back(value);
		} else values.push_back(entry.value);
		for (const Variant &value : values) {
			const CharString text = String(value).utf8();
			if (!http_field_value(text.get_data(),text.length())) { ++dropped; continue; }
			emit(key,low,text);
		}
	}
}
