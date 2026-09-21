// Validate extension encodings and preserve every constraint needed by certificate path verification.
#include "extension_core.h"
#include "name_core.h"
#include "ip.h"
#include <set>
#include <string_view>
#include <utility>

namespace GDCrypto {
namespace {
// Unwrap one complete constructed field while rejecting trailing encodings.
bool sequence(Bytes input, Bytes &output) { DER reader(input); return reader.take(0x30,output) && reader.empty(); }

// Read a canonical nonnegative implicit integer without changing the destination on failure.
bool implicit_count(DER &reader, uint8_t tag, uint64_t &output) {
	Bytes value; if (!reader.take(tag,value) || !value.size || (value.data[0]&128) || (value.size>1 && !value.data[0] && !(value.data[1]&128))) return false;
	uint64_t count = 0;
	for (size_t at = 0; at != value.size; ++at) {if (count > (UINT64_MAX>>8)) return false; count = (count<<8)|value.data[at];}
	output = count; return true;
}

// Extract a path counter representable by the signed public certificate fields.
bool counter(DER &reader, uint8_t tag, int64_t &output) {
	uint64_t value;
	if (!(tag == 2 ? reader.number(value) : implicit_count(reader,tag,value)) || value > uint64_t(INT64_MAX)) return false;
	output = int64_t(value); return true;
}

// Read a context-specific general name without assuming its family is supported by the verifier.
bool general_name(DER &reader, CertName &output) {
	CertName name; if (!reader.any(name.tag,name.value) || (name.tag&0xc0) != 0x80 || (name.tag&31) > 8) return false;
	output = name; return true;
}

// Extract alternative names and validate primitive text and binary address encodings.
bool alternatives(Bytes input, CertConstraints &output, bool &handled) {
	Bytes content; if (!sequence(input,content)) return false;
	DER names(content); output.san = true; handled = false;
	while (!names.empty()) {
		CertName name; if (!general_name(names,name)) return false;
		if (name.tag == 0x81 || name.tag == 0x82 || name.tag == 0x86) {
			if (!certificate_text(0x16,name.value)) return false;
			if (name.tag == 0x86) {std::string host; if (!certificate_uri({reinterpret_cast<const char *>(name.value.data),name.value.size},host)) return false;}
			handled = true;
		} else if (name.tag == 0x87) {
			if (name.value.size != 4 && name.value.size != 16) return false;
			handled = true;
		}
		output.names.push_back(name);
	}
	return true;
}

// Validate an address mask as a contiguous prefix rather than accepting arbitrary bit patterns.
bool address_mask(Bytes value) {
	if (value.size != 8 && value.size != 32) return false;
	bool zero = false;
	for (size_t at = value.size/2; at != value.size; ++at) {
		for (unsigned bit = 0; bit != 8; ++bit) {const bool one = value.data[at]&(128>>bit); if (zero && one) return false; zero |= !one;}
	}
	return true;
}

// Preserve complete subtree schemas and identify unsupported general-name families.
bool subtrees(Bytes input, std::vector<CertSubtree> &output, bool &handled) {
	DER reader(input);
	while (!reader.empty()) {
		Bytes encoded; if (!reader.take(0x30,encoded)) return false;
		DER fields(encoded); CertSubtree subtree;
		if (!general_name(fields,subtree.name)) return false;
		if (fields.peek(0x80) && !counter(fields,0x80,subtree.minimum)) return false;
		if (fields.peek(0x81) && !counter(fields,0x81,subtree.maximum)) return false;
		if (!fields.empty()) return false;
		if (subtree.name.tag == 0x87) {if (!address_mask(subtree.name.value)) return false;}
		else if (subtree.name.tag == 0x81 || subtree.name.tag == 0x82 || subtree.name.tag == 0x86) {
			if (!certificate_text(0x16,subtree.name.value)) return false;
			const std::string_view name(reinterpret_cast<const char *>(subtree.name.value.data),subtree.name.value.size);
			if (subtree.name.tag == 0x81 && name.find('@') != name.npos) {std::string local; std::string_view domain; if (!certificate_mailbox(name,local,domain)) return false;}
			else if (!certificate_domain(name,true)) return false;
			if (subtree.name.tag == 0x86) {const auto ip = GDIP::parse(name); if (ip.bit_len() && !ip.zoned()) return false;}
		}
		else handled = false;
		output.push_back(subtree);
	}
	return true;
}

// Decode permitted and excluded subtrees while keeping absent and empty constraints distinct.
bool constraints(Bytes input, CertConstraints &output, bool &handled) {
	Bytes content, permitted, excluded; if (!sequence(input,content)) return false;
	DER fields(content); output.constrained = true;
	if (fields.peek(0xa0) && !fields.take(0xa0,permitted)) return false;
	if (fields.peek(0xa1) && !fields.take(0xa1,excluded)) return false;
	return fields.empty() && (permitted.size || excluded.size) && subtrees(permitted,output.permitted,handled) && subtrees(excluded,output.excluded,handled);
}

// Validate key identifiers and the optional issuer-and-serial fields in their declared order.
bool authority(Bytes input, Bytes &output) {
	Bytes content, value; if (!sequence(input,content)) return false;
	DER fields(content);
	if (fields.peek(0x80) && !fields.take(0x80,output)) return false;
	if (fields.peek(0xa1)) {
		if (!fields.take(0xa1,value)) return false;
		DER names(value); while (!names.empty()) {CertName name; if (!general_name(names,name)) return false;}
	}
	if (fields.peek(0x82)) {
		if (!fields.take(0x82,value) || !value.size || (value.data[0]&128) || (value.size>1 && !value.data[0] && !(value.data[1]&128))) return false;
	}
	return fields.empty();
}

// Retain unique policy identifiers and validate the complete qualifier envelopes.
bool policies(Bytes input, std::vector<Bytes> &output) {
	Bytes content; if (!sequence(input,content)) return false;
	DER fields(content); std::set<std::string_view> seen;
	while (!fields.empty()) {
		Bytes encoded, oid; if (!fields.take(0x30,encoded)) return false;
		DER policy(encoded); if (!policy.oid(oid) || !seen.emplace(reinterpret_cast<const char *>(oid.data),oid.size).second) return false;
		output.push_back(oid);
		if (!policy.empty()) {
			if (!policy.take(0x30,encoded)) return false;
			DER qualifiers(encoded);
			while (!qualifiers.empty()) {
				if (!qualifiers.take(0x30,encoded)) return false;
				DER qualifier(encoded); uint8_t tag; Bytes value;
				if (!qualifier.oid(oid) || !qualifier.any(tag,value) || !qualifier.empty()) return false;
			}
		}
		if (!policy.empty()) return false;
	}
	return true;
}

// Validate advisory access descriptions without performing network requests during parsing.
bool access(Bytes input) {
	Bytes content; if (!sequence(input,content)) return false;
	DER fields(content);
	while (!fields.empty()) {
		Bytes encoded, oid; if (!fields.take(0x30,encoded)) return false;
		DER description(encoded); CertName name;
		if (!description.oid(oid) || !general_name(description,name) || !description.empty()) return false;
	}
	return true;
}

// Validate advisory distribution-point envelopes without treating them as revocation evidence.
bool distribution(Bytes input) {
	Bytes content; if (!sequence(input,content)) return false;
	DER points(content);
	while (!points.empty()) {
		Bytes encoded, value; if (!points.take(0x30,encoded)) return false;
		DER point(encoded);
		if (point.peek(0xa0)) {
			if (!point.take(0xa0,value)) return false;
			DER name(value); uint8_t tag;
			if (!name.any(tag,value) || !name.empty() || (tag != 0xa0 && tag != 0xa1)) return false;
			DER fields(value);
			while (!fields.empty()) {
				if (tag == 0xa0) {CertName general; if (!general_name(fields,general)) return false;}
				else {
					Bytes attribute, oid; if (!fields.take(0x30,attribute)) return false;
					DER pair(attribute); uint8_t kind;
					if (!pair.oid(oid) || !pair.any(kind,attribute) || !certificate_attribute(kind,attribute) || !pair.empty()) return false;
				}
			}
		}
		if (point.peek(0x81)) {uint8_t unused; Bytes bits; if (!point.take(0x81,value) || !DER::bit_value(value,bits,unused)) return false;}
		if (point.peek(0xa2)) {
			if (!point.take(0xa2,value)) return false;
			DER names(value); while (!names.empty()) {CertName general; if (!general_name(names,general)) return false;}
		}
		if (!point.empty()) return false;
	}
	return true;
}

// Dispatch standardized extension schemas without imposing certificate or collection-size caps.
bool extension(const CertExtension &item, CertConstraints &output, bool &handled) {
	unsigned id = item.oid.size == 3 && item.oid.data[0] == 0x55 && item.oid.data[1] == 0x1d ? item.oid.data[2] : 0;
	DER reader(item.value); Bytes content, value;
	switch (id) {
		case 14: return !item.critical && reader.take(4,output.subject_id) && reader.empty();
		case 15: {
			uint8_t unused; if (!reader.bits(value,unused) || !reader.empty()) return false;
			for (unsigned bit = 0; bit != 9 && bit/8 < value.size; ++bit) if (value.data[bit/8]&(128>>(bit%8))) output.usage |= uint16_t(1<<bit);
			return true;
		}
		case 17: return alternatives(item.value,output,handled);
		case 19: {
			if (!sequence(item.value,content)) return false;
			DER fields(content); output.basic = true;
			if (fields.peek(1) && !fields.boolean(output.ca)) return false;
			if (fields.peek(2) && !counter(fields,2,output.path_length)) return false;
			return fields.empty();
		}
		case 30: return constraints(item.value,output,handled);
		case 31: return distribution(item.value);
		case 32: return policies(item.value,output.policies);
		case 33: {
			if (!sequence(item.value,content)) return false;
			DER fields(content);
			while (!fields.empty()) {
				Bytes encoded, issuer, subject; if (!fields.take(0x30,encoded)) return false;
				DER mapping(encoded); if (!mapping.oid(issuer) || !mapping.oid(subject) || !mapping.empty()) return false;
				output.mappings.emplace_back(issuer,subject);
			}
			return true;
		}
		case 35: return !item.critical && authority(item.value,output.authority_id);
		case 36: {
			if (!sequence(item.value,content)) return false;
			DER fields(content);
			if (fields.peek(0x80) && !counter(fields,0x80,output.explicit_policy)) return false;
			if (fields.peek(0x81) && !counter(fields,0x81,output.mapping)) return false;
			return fields.empty();
		}
		case 37: {
			if (!sequence(item.value,content)) return false;
			DER fields(content); while (!fields.empty()) {if (!fields.oid(value)) return false; output.extended.push_back(value);}
			return true;
		}
		case 54: return counter(reader,2,output.any_policy) && reader.empty();
		default: break;
	}
	constexpr uint8_t aia[] = {0x2b,0x06,0x01,0x05,0x05,0x07,0x01,0x01}; // Authority information access identifier.
	if (item.oid.size == sizeof(aia) && std::string_view(reinterpret_cast<const char *>(item.oid.data),item.oid.size) == std::string_view(reinterpret_cast<const char *>(aia),sizeof(aia))) return !item.critical && access(item.value);
	handled = false; return true;
}
}

// Publish extension fields only after every recognized schema is valid.
bool parse_extensions(const CertificateInfo &certificate, CertConstraints &output) {
	CertConstraints value;
	for (const auto &item : certificate.extensions) {
		if ((item.oid.size && !item.oid.data) || (item.value.size && !item.value.data)) return false;
		bool handled = true; if (!extension(item,value,handled)) return false;
		if (!handled && item.critical) value.unhandled.push_back(item.oid);
	}
	output = std::move(value); return true;
}
}
