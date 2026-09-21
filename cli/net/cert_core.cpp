// Validate certificate envelopes, names, and calendar fields without external certificate parsers.
#include "cert_core.h"
#include "cli/data/crypto_core.h"
#include <set>
#include <string_view>

namespace GDCrypto {
namespace {
// Validate canonical Unicode scalar encodings without allocating decoded text.
bool valid_unicode(Bytes input) {
	for (size_t at = 0; at < input.size;) {
		uint32_t value = input.data[at++];
		if (value < 128) continue;
		const unsigned more = value >= 0xc2 && value <= 0xdf ? 1 : value >= 0xe0 && value <= 0xef ? 2 : value >= 0xf0 && value <= 0xf4 ? 3 : 0;
		if (!more || more > input.size - at) return false;
		value &= (1U << (6 - more)) - 1;
		for (unsigned byte = 0; byte != more; ++byte) {
			const uint8_t next = input.data[at++]; if ((next & 0xc0) != 0x80) return false;
			value = (value << 6) | (next & 63);
		}
		if (value < (more == 1 ? 128U : more == 2 ? 2048U : 65536U) || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
	}
	return true;
}

// Count Gregorian leap years without consulting host calendar functions.
bool leap(unsigned year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

// Convert a nonnegative calendar year to absolute days before its first day.
int64_t year_days(unsigned year) {
	return int64_t(year) * 365 + (year + 3) / 4 - (year + 99) / 100 + (year + 399) / 400;
}

// Validate each relative distinguished-name attribute while retaining the original signature bytes.
bool name_fields(Bytes input) {
	DER names(input); Bytes group, attribute, oid, value; uint8_t tag;
	while (!names.empty()) {
		if (!names.take(0x31, group)) return false;
		DER set(group);
		while (!set.empty()) {
			if (!set.take(0x30, attribute)) return false;
			DER fields(attribute);
			if (!fields.oid(oid) || !fields.any(tag, value) || !certificate_attribute(tag, value) || !fields.empty()) return false;
		}
	}
	return true;
}

// Require one encoded time of a supported calendar representation.
bool read_time(DER &reader, int64_t &output) {
	uint8_t tag; Bytes value;
	return reader.any(tag, value) && certificate_time(tag, value, output);
}

// Validate the public-key envelope while leaving algorithm support and mathematics to its consumer.
bool public_fields(Bytes input) {
	DER reader(input); Bytes value, encoded, bits; Algorithm algorithm; uint8_t unused;
	return reader.take(0x30, value, &encoded) && parse_algorithm(encoded, algorithm) && reader.bits(bits, unused) && reader.empty();
}

// Index borrowed identifiers with comparison-based lookup to avoid collision-driven duplicate checking.
bool extension_fields(Bytes input, std::vector<CertExtension> &output) {
	DER reader(input); Bytes content;
	if (!reader.take(0x30, content) || !reader.empty()) return false;
	DER extensions(content); std::set<std::string_view> seen;
	while (!extensions.empty()) {
		if (!extensions.take(0x30, content)) return false;
		DER fields(content); CertExtension extension;
		if (!fields.oid(extension.oid)) return false;
		if (fields.peek(1) && !fields.boolean(extension.critical)) return false;
		if (!fields.take(4, extension.value) || !fields.empty()) return false;
		if (!seen.emplace(reinterpret_cast<const char *>(extension.oid.data), extension.oid.size).second) return false;
		output.push_back(extension);
	}
	return true;
}
}

// Accept supported name encodings without introducing a character-count policy.
bool certificate_text(uint8_t tag, Bytes input) {
	if (input.size && !input.data) return false;
	if (tag == 12) return valid_unicode(input);
	if (tag == 20) return true;
	if (tag == 30) {
		if (input.size % 2) return false;
		for (size_t at = 0; at != input.size; at += 2) {
			const unsigned point = unsigned(input.data[at]) * 256 + input.data[at + 1];
			if (point >= 0xfffe || (point >= 0xfdd0 && point <= 0xfdef) || (point >= 0xd800 && point <= 0xdfff)) return false;
		}
		return true;
	}
	if (tag != 18 && tag != 19 && tag != 22) return false;
	for (size_t at = 0; at != input.size; ++at) {
		const uint8_t byte = input.data[at];
		const bool numeric = (byte >= '0' && byte <= '9') || byte == ' ';
		if (tag == 22 ? byte > 127 : tag == 18 ? !numeric : !(numeric || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '\'' && byte <= '/') || byte == ':' || byte == '=' || byte == '?' || byte == '&')) return false;
	}
	return true;
}

// Validate typed distinguished-name values without forcing integers or opaque attributes into text.
bool certificate_attribute(uint8_t tag, Bytes input) {
	if (input.size && !input.data) return false;
	if (tag == 12 || tag == 18 || tag == 19 || tag == 20 || tag == 22 || tag == 30) return certificate_text(tag,input);
	if (tag == 2) {
		if (!input.size || input.size > sizeof(int64_t)) return false;
		return input.size == 1 || !((input.data[0] == 0 && !(input.data[1] & 128)) || (input.data[0] == 255 && (input.data[1] & 128)));
	}
	if (tag == 1) return input.size == 1 && (input.data[0] == 0 || input.data[0] == 255);
	if (tag == 3) { Bytes bits; uint8_t unused; return DER::bit_value(input,bits,unused); }
	if (tag == 23 || tag == 24) { int64_t time; return certificate_time(tag,input,time); }
	if (tag == 6) {
		if (!input.size) return false;
		uint32_t arc = 0; bool first = true;
		for (size_t i = 0; i < input.size; ++i) {
			const uint8_t byte = input.data[i];
			if ((first && byte == 128) || arc > (uint32_t(INT32_MAX)-(byte & 127))/128) return false;
			arc = arc*128+(byte & 127);
			first = !(byte & 128);
			if (first) arc = 0;
		}
		return first;
	}
	return true;
}

// Parse canonical timezone-bearing fields, including minute-precision legacy timestamps.
bool certificate_time(uint8_t tag, Bytes input, int64_t &output) {
	if (!input.data || (tag != 23 && tag != 24) || input.size < 11 || input.size > 19) return false;
	const bool utc = tag == 23, zulu = input.data[input.size - 1] == 'Z';
	const size_t digits = input.size - (zulu ? 1 : 5), years = utc ? 2 : 4;
	if (digits != years + 10 && !(utc && digits == years + 8)) return false;
	unsigned values[6]{}, offset = 0; size_t at = 0;
	for (size_t field = 0; field != 6; ++field) {
		const size_t width = field == 0 ? years : 2;
		if (at == digits && field == 5) break;
		for (size_t byte = 0; byte != width; ++byte) {
			const uint8_t digit = input.data[at++]; if (digit < '0' || digit > '9') return false;
			values[field] = values[field] * 10 + digit - '0';
		}
	}
	if (!zulu) {
		if (input.data[digits] != '+' && input.data[digits] != '-') return false;
		unsigned zone[2]{};
		for (size_t byte = 0; byte != 4; ++byte) {
			const uint8_t digit = input.data[digits + 1 + byte]; if (digit < '0' || digit > '9') return false;
			zone[byte / 2] = zone[byte / 2] * 10 + digit - '0';
		}
		if (zone[0] > 24 || zone[1] > 59 || (!zone[0] && !zone[1])) return false;
		offset = (zone[0] * 60 + zone[1]) * 60;
	}
	const unsigned year = utc ? values[0] + (values[0] < 50 ? 2000 : 1900) : values[0];
	const unsigned month = values[1], day = values[2];
	constexpr unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31}; // Calendar month lengths before the leap-day adjustment.
	constexpr unsigned preceding[] = {0,31,59,90,120,151,181,212,243,273,304,334}; // Days before each month in a common year.
	if (!month || month > 12 || !day || day > month_days[month - 1] + unsigned(month == 2 && leap(year)) || values[3] > 23 || values[4] > 59 || values[5] > 59) return false;
	const int64_t days = year_days(year) - year_days(1970) + preceding[month - 1] + (month > 2 && leap(year)) + day - 1;
	output = days * 86400 + values[3] * 3600 + values[4] * 60 + values[5] + (zulu ? 0 : input.data[digits] == '+' ? -int64_t(offset) : int64_t(offset));
	return true;
}

// Publish a borrowed certificate only after its complete envelope and mandatory fields are valid.
bool parse_certificate(Bytes input, CertificateInfo &output) {
	DER envelope(input); Bytes content, tbs, inner, outer, field; CertificateInfo value; value.raw = input;
	if (!envelope.take(0x30, content) || !envelope.empty()) return false;
	DER body(content);
	if (!body.take(0x30, tbs, &value.signed_data) || !body.take(0x30, field, &outer) || !body.bits(value.signature, value.signature_unused) || !body.empty()) return false;
	DER signed_fields(tbs);
	if (signed_fields.peek(0xa0)) {
		if (!signed_fields.take(0xa0, field)) return false;
		DER version(field); uint64_t number;
		if (!version.number(number) || number > 2 || !version.empty()) return false;
		value.version = unsigned(number);
	}
	if (!signed_fields.integer(value.serial) || !signed_fields.take(0x30, field, &inner) || inner.size != outer.size || !equal(inner.data, outer.data, inner.size) || !parse_algorithm(inner, value.algorithm)) return false;
	if (!signed_fields.take(0x30, field, &value.issuer) || !name_fields(field) || !signed_fields.take(0x30, content)) return false;
	DER validity(content);
	if (!read_time(validity, value.not_before) || !read_time(validity, value.not_after) || !validity.empty()) return false;
	if (!signed_fields.take(0x30, field, &value.subject) || !name_fields(field) || !signed_fields.take(0x30, field, &value.public_key) || !public_fields(field)) return false;
	if (value.version > 0) {
		for (uint8_t tag : {0x81,0x82}) if (signed_fields.peek(tag)) {
			Bytes bits; uint8_t unused;
			if (!signed_fields.take(tag, field) || !DER::bit_value(field, bits, unused)) return false;
		}
		if (value.version == 2 && signed_fields.peek(0xa3)) {
			if (!signed_fields.take(0xa3, field) || !extension_fields(field, value.extensions)) return false;
		}
	}
	if (!signed_fields.empty()) return false;
	output = std::move(value); return true;
}
}
