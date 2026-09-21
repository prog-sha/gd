/**************************************************************************/
/*  json.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Perform strict conversion between JSON bytes and values.

#include "cli/data/json.h"
#include "cli/data/json_scan.h"
#include "cli/sys/pool.h"
#include "cli/sys/clock.h"
#include "cli/sys/file_job.h"

#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

#include "thirdparty/grisu2/grisu2.h"

#include <cstdlib>
#include <locale.h>
#include <memory>
#include <utility>
#ifdef WINDOWS_ENABLED
#include <charconv>
#include <limits>
#endif
#ifdef MACOS_ENABLED
#include <xlocale.h>
#endif

namespace {

constexpr int JSON_DEPTH_MAX = 10000; // Maximum decoding nesting depth.
constexpr uint64_t JSON_CACHE_BYTES = 64 * 1024; // Retained traversal storage target; larger inputs still complete normally.
constexpr int JSON_SCAN_PREFIX = 8; // Consecutive ordinary characters before block setup is worthwhile.
constexpr int JSON_SCAN_MIN = 16; // Minimum known string span that amortizes a native block call.

// Keep wide-loop code out of the scalar encoder's hot instruction footprint.
_NO_INLINE_ size_t json_ascii(const char32_t *p_src, uint8_t *p_dst, size_t p_size, bool p_html) {
	return GDJson::ascii(p_src, p_dst, p_size, p_html);
}

// Keep vector setup and its register pressure out of short-string parsing.
_NO_INLINE_ size_t json_ascii(const uint8_t *p_src, char32_t *p_dst, size_t p_size) {
	return GDJson::ascii(p_src, p_dst, p_size);
}

#ifndef WINDOWS_ENABLED
// Dedicated C locale for locale-independent JSON number parsing.
struct JsonLocale {
	locale_t value = newlocale(LC_NUMERIC_MASK, "C", nullptr); // Immutable locale shared across workers.

	// Release the numeric-conversion locale.
	~JsonLocale() {
		if (value) {
			freelocale(value);
		}
	}
};
#endif

// Round validated decimal notation to float64, including subnormal values.
bool json_float(const uint8_t *p_src, int64_t p_size, [[maybe_unused]] int64_t p_order, double &r_value) {
#ifdef WINDOWS_ENABLED
	// Convert the complete span: separate mantissa/exponent conversion can overflow
	// even when they cancel to a finite value. Never truncate significant digits.
	const char *start = reinterpret_cast<const char *>(p_src);
	const auto result = std::from_chars(start, start + p_size, r_value);
	if (result.ptr != start + p_size) return false;
	if (result.ec == std::errc::result_out_of_range) {
		// Range errors leave the output unchanged. The validated decimal order
		// distinguishes rounded underflow from overflow without another conversion.
		r_value = p_order < 0 ? 0.0 : std::numeric_limits<double>::infinity();
		if (*p_src == '-') r_value = -r_value;
		return true;
	}
	return result.ec == std::errc{};
#else
	static const JsonLocale locale;
	if (!locale.value) {
		return false;
	}
	PackedByteArray text;
	if (p_size == INT64_MAX || text.resize(p_size + 1) != OK) {
		return false;
	}
	memcpy(text.ptrw(), p_src, p_size);
	text.ptrw()[p_size] = '\0';
	const char *start = reinterpret_cast<const char *>(text.ptr());
	char *end = nullptr;
	r_value = strtod_l(start, &end, locale.value);
	return end == start + p_size;
#endif
}

// Name-value pair for deterministic object ordering.
struct JsonMember {
	String name;
	Variant value;
};

// Sort object names by Unicode order.
struct JsonMemberOrder {
	bool operator()(const JsonMember &p_a, const JsonMember &p_b) const {
		const int end = MIN(p_a.name.length(), p_b.name.length());
		for (int i = 0; i < end; i++) {
			if (p_a.name[i] != p_b.name[i]) return p_a.name[i] < p_b.name[i];
		}
		return p_a.name.length() < p_b.name.length();
	}
};

// Describe how one JSON string rune is encoded.
struct JsonRune {
	const char *escaped = nullptr; // Two-byte short escape.
	char32_t rune = 0; // Value encoded as UTF-8 or a Unicode escape.
	int bytes = 0; // Encoded byte count.
	bool unicode = false; // Whether to use a four-digit Unicode escape.
};

// Return one rune's JSON escape and UTF-8 width.
JsonRune json_rune(char32_t p_c, bool p_escape_html) {
	switch (p_c) {
		case '"': return { "\\\"", p_c, 2, false };
		case '\\': return { "\\\\", p_c, 2, false };
		case '\b': return { "\\b", p_c, 2, false };
		case '\f': return { "\\f", p_c, 2, false };
		case '\n': return { "\\n", p_c, 2, false };
		case '\r': return { "\\r", p_c, 2, false };
		case '\t': return { "\\t", p_c, 2, false };
		default: break;
	}
	if (p_c < 0x20 || p_c == 0x2028 || p_c == 0x2029 || (p_escape_html && (p_c == '<' || p_c == '>' || p_c == '&'))) {
		return { nullptr, p_c, 6, true };
	}
	const char32_t rune = (p_c >= 0xd800 && p_c <= 0xdfff) || p_c > 0x10ffff ? 0xfffd : p_c;
	const int bytes = rune <= 0x7f ? 1 : rune <= 0x7ff ? 2 : rune <= 0xffff ? 3 : 4;
	return { nullptr, rune, bytes, false };
}

// Write one rune's UTF-8 into preallocated storage.
void put_utf8(uint8_t *&r_out, char32_t p_c) {
	if (p_c <= 0x7f) {
		*r_out++ = p_c;
	} else if (p_c <= 0x7ff) {
		*r_out++ = 0xc0 | (p_c >> 6);
		*r_out++ = 0x80 | (p_c & 0x3f);
	} else if (p_c <= 0xffff) {
		*r_out++ = 0xe0 | (p_c >> 12);
		*r_out++ = 0x80 | ((p_c >> 6) & 0x3f);
		*r_out++ = 0x80 | (p_c & 0x3f);
	} else {
		*r_out++ = 0xf0 | (p_c >> 18);
		*r_out++ = 0x80 | ((p_c >> 12) & 0x3f);
		*r_out++ = 0x80 | ((p_c >> 6) & 0x3f);
		*r_out++ = 0x80 | (p_c & 0x3f);
	}
}

// Encode values as JSON without recursive calls.
class StrictJSONOut {
	enum Action {
		VALUE,
		TEXT,
		BYTE,
		LEAVE,
		ARRAY,
		OBJECT,
		MEMBERS,
	};
	struct Item {
		Action action = VALUE;
		Variant value;
		String text;
		const void *id = nullptr;
		int depth = 0;
		uint8_t byte = 0;
		int at = 0; // Next string rune or array element.
		int64_t offset = -1; // Negative until the opening quote has been emitted.
		Dictionary::ConstIterator next; // Next object member; value retains its dictionary.
		Variant key; // Next key retained by value when handing traversal to a worker.
		bool saved = false; // Whether a raw iterator has been replaced by its key.
		bool cache = false; // This object is a direct element of an array with possible repeated layouts.
		int count = 0; // Original container size for mutation diagnostics.
		std::shared_ptr<LocalVector<JsonMember>> members; // Sorted fields owned only until their object finishes.
	};
	struct Key {
		String name; // Retain immutable character storage, never just a hash or borrowed address.
		int64_t offset = 0; // Encoded quotes and name already present in the append-only output.
		int size = 0; // Zero denotes an unused slot.
	};

	PackedByteArray out; // Own all output with a 64-bit length and avoid a full copy on return.
	LocalVector<Item> work;
	LocalVector<Key> keys; // Most recently encoded name at each member position in this call.
	uint64_t key_bytes = 0; // Retained key characters and slots, excluding bytes already owned by output.
	HashSet<const void *> active; // Identity of containers on the active traversal path.
	String why;
	Err::Kind kind = Err::INVALID_DATA; // Failure category independent of diagnostic wording.
	int64_t max_bytes = 0; // Explicit caller-selected output byte limit.
	int max_depth = 0;
	bool deterministic = false;
	bool escape_html = false;
	uint64_t until = 0; // Cooperative deadline; zero lets a worker complete all remaining work.
	uint32_t peak = 0; // Largest pending traversal in the current encoding, retained across handoff.

	// Yield without discarding input positions or encoded bytes.
	bool expired() const {
		return until && GDClock::usec() >= until;
	}

	// Replace borrowed iterators before another thread can resume traversal.
	bool pause() {
		for (Item &item : work) {
			if (item.action == OBJECT && !item.saved && item.next) {
				item.key = item.next->key;
				item.next = Dictionary::ConstIterator();
				item.saved = true;
			}
		}
		return false;
	}

	// Preserve only the first failure.
	bool fail(const String &p_why, Err::Kind p_kind = Err::INVALID_DATA) {
		if (why.is_empty()) {
			why = p_why;
			kind = p_kind;
		}
		return false;
	}

	// Check explicit and representational boundaries before allocating additional capacity.
	bool room(uint64_t p_size) {
		if (p_size > uint64_t(INT64_MAX) - out.size()) {
			return fail("JSON output exceeds the representation limit", Err::LIMITED);
		}
		if (max_bytes > 0 && (out.size() > uint64_t(max_bytes) || p_size > uint64_t(max_bytes) - out.size())) {
			return fail("JSON output exceeds max_bytes", Err::LIMITED);
		}
		return out.reserve(out.size() + p_size) == OK || fail("cannot allocate JSON output", Err::LIMITED);
	}

	// Append UTF-8 bytes unchanged.
	bool raw(const char *p_data, int p_size) {
		const int size = p_size;
		if (!room(size)) {
			return false;
		}
		const int64_t at = out.size();
		out.resize(at + size);
		memcpy(out.ptrw() + at, p_data, size);
		return true;
	}

	// Encode and append text as UTF-8.
	bool raw(const String &p_text) {
		const CharString src = p_text.utf8();
		return raw(src.get_data(), src.length());
	}

	// Append one ASCII JSON byte.
	bool byte(uint8_t p_byte) {
		if (!room(1)) {
			return false;
		}
		out.push_back(p_byte);
		return true;
	}

	// Append a signed 64-bit integer in decimal without an intermediate String.
	bool integer(int64_t p_value) {
		char buf[21];
		char *at = buf + sizeof(buf);
		const bool negative = p_value < 0;
		uint64_t value = negative ? uint64_t(-(p_value + 1)) + 1 : uint64_t(p_value);
		do {
			*--at = '0' + value % 10;
			value /= 10;
		} while (value > 0);
		if (negative) {
			*--at = '-';
		}
		return raw(at, buf + sizeof(buf) - at);
	}

	// Emit round-trippable float64 digits and retain a decimal point to distinguish floats from integers.
	bool floating(double p_value) {
		if (!Math::is_finite(p_value)) {
			return fail("JSON cannot encode a non-finite number");
		}
		char buf[std::numeric_limits<double>::max_digits10 + 8]; // Capacity for 17 digits, sign, decimal point, exponent, and a leading fixed-point zero.
		char *at = buf;
		if (std::signbit(p_value)) {
			*at++ = '-';
			p_value = -p_value;
		}
		char *end = at;
		if (p_value == 0) {
			*end++ = '0';
		} else {
			int length = 0;
			int exponent = 0;
			grisu2::grisu2_wrap(at, length, exponent, p_value);
			end = grisu2::format_buffer(at, length, exponent, -6, 21); // Select fixed notation within the configured decimal-exponent range.
		}
		bool fraction = false;
		for (char *c = at; c < end; c++) {
			fraction = fraction || *c == '.' || *c == 'e';
		}
		if (!fraction) {
			*end++ = '.';
			*end++ = '0';
		}
		// Remove an unnecessary leading zero in a one-digit negative exponent.
		if (end - at >= 4 && end[-4] == 'e' && end[-3] == '-' && end[-2] == '0') {
			end[-2] = end[-1];
			end--;
		}
		return raw(buf, end - buf);
	}

	// Encode strings in one pass, narrowing ordinary ASCII and retaining the exact continuation.
	bool text(Item &r_item) {
		const String &src = r_item.text;
		const char *hex = "0123456789abcdef";
		constexpr int SPAN = 256; // Scratch batch and clock-check spacing; input continues across batches.
		uint8_t buf[SPAN * 6 + 2]; // Six-byte escapes plus both quotes fit in a single batch.
		do {
			if (expired()) return false;
			const int end = r_item.at + MIN(SPAN, src.length() - r_item.at);
			uint8_t *dst = buf;
			if (r_item.offset < 0) {
				*dst++ = '"';
				r_item.offset = 0;
			}
			// Probe once per scratch batch, not after every exceptional character.
			// Mixed Unicode keeps the ordinary loop's cost; long ASCII copies in bulk.
			if (end - r_item.at >= JSON_SCAN_MIN) {
				const int count = json_ascii(src.ptr() + r_item.at, dst, end - r_item.at, escape_html);
				r_item.at += count;
				dst += count;
			}
			while (r_item.at < end) {
				const char32_t c = src[r_item.at++];
				if (c >= 0x20 && c < 0x80 && c != '"' && c != '\\' && (!escape_html || (c != '<' && c != '>' && c != '&'))) {
					*dst++ = c;
					continue;
				}
				const JsonRune rune = json_rune(c, escape_html);
				if (rune.escaped) {
					memcpy(dst, rune.escaped, rune.bytes);
					dst += rune.bytes;
				} else if (rune.unicode) {
					*dst++ = '\\';
					*dst++ = 'u';
					*dst++ = hex[(rune.rune >> 12) & 0xf];
					*dst++ = hex[(rune.rune >> 8) & 0xf];
					*dst++ = hex[(rune.rune >> 4) & 0xf];
					*dst++ = hex[rune.rune & 0xf];
				} else {
					put_utf8(dst, rune.rune);
				}
			}
			if (r_item.at == src.length()) *dst++ = '"';
			if (!raw(reinterpret_cast<const char *>(buf), dst - buf)) return true;
		} while (r_item.at < src.length());
		return true;
	}

	// Convert dictionary keys to JSON object names.
	bool name_of(const Variant &p_key, String &r_name) {
		switch (p_key.get_type()) {
			case Variant::STRING:
			case Variant::STRING_NAME:
				r_name = p_key;
				return true;
			case Variant::INT:
				r_name = String::num_int64(p_key);
				return true;
			default:
				return fail("JSON object name must be a string or integer");
		}
	}

	// Push work for later processing.
	void push(Action p_action, const Variant &p_value = Variant(), const String &p_text = String(), const void *p_id = nullptr, int p_depth = 0, uint8_t p_byte = 0) {
		Item item;
		item.action = p_action;
		item.value = p_value;
		item.text = p_text;
		item.id = p_id;
		item.depth = p_depth;
		item.byte = p_byte;
		work.push_back(static_cast<Item &&>(item));
	}

	// Push one ASCII byte for later output.
	void push_byte(uint8_t p_byte) {
		push(BYTE, Variant(), String(), nullptr, 0, p_byte);
	}

	// Open an array and schedule its elements in original order.
	bool array(const Array &p_array, int p_depth, const void *p_id) {
		if (max_depth > 0 && p_depth >= max_depth) {
			return fail("JSON nesting is too deep", Err::LIMITED);
		}
		if (p_id && active.has(p_id)) {
			return fail("JSON cannot encode a cyclic array");
		}
		if (p_id) {
			active.insert(p_id);
			push(LEAVE, Variant(), String(), p_id);
		}
		push(ARRAY, p_array, String(), nullptr, p_depth);
		work[work.size() - 1].count = p_array.size();
		return byte('[');
	}

	// Open a dictionary, reject duplicate names, and schedule its fields.
	bool object(const Dictionary &p_object, int p_depth) {
		if (max_depth > 0 && p_depth >= max_depth) {
			return fail("JSON nesting is too deep", Err::LIMITED);
		}
		const void *id = p_object.id();
		if (active.has(id)) {
			return fail("JSON cannot encode a cyclic object");
		}
		// Repeated array elements can repay name retention; singleton nested objects cannot.
		const bool cache = !work.is_empty() && work[work.size() - 1].action == ARRAY && work[work.size() - 1].count > 1;
		if (!deterministic) {
			active.insert(id);
			push(LEAVE, Variant(), String(), id);
			Item item;
			item.action = OBJECT;
			item.value = p_object;
			item.depth = p_depth;
			item.next = p_object.begin();
			item.count = p_object.size();
			item.cache = cache;
			work.push_back(std::move(item));
			return byte('{');
		}
		LocalVector<JsonMember> members;
		HashSet<String> names;
		members.reserve(p_object.size());
		names.reserve(p_object.size());
		for (const KeyValue<Variant, Variant> &kv : p_object) {
			JsonMember member;
			if (!name_of(kv.key, member.name)) {
				return false;
			}
			if (names.has(member.name)) {
				return fail("duplicate JSON object name after key conversion");
			}
			names.insert(member.name);
			member.value = kv.value;
			members.push_back(static_cast<JsonMember &&>(member));
		}
		if (deterministic) {
			members.sort_custom<JsonMemberOrder>();
		}
		active.insert(id);
		push(LEAVE, Variant(), String(), id);
		Item item;
		item.action = MEMBERS;
		item.depth = p_depth;
		item.members = std::make_shared<LocalVector<JsonMember>>(std::move(members));
		item.cache = cache;
		work.push_back(std::move(item));
		return byte('{');
	}

	// Convert one value into type-specific encoding work.
	bool value(const Variant &p_value, int p_depth) {
		switch (p_value.get_type()) {
			case Variant::NIL: return raw("null", 4);
			case Variant::BOOL: return (bool)p_value ? raw("true", 4) : raw("false", 5);
			case Variant::INT: return integer(p_value);
			case Variant::FLOAT: return floating(p_value);
			case Variant::STRING:
			case Variant::STRING_NAME: {
				Item item;
				item.action = TEXT;
				item.text = p_value;
				if (!text(item)) work.push_back(std::move(item));
				return why.is_empty();
			}
			case Variant::ARRAY: {
				const Array a = p_value;
				return array(a, p_depth, a.id());
			}
			case Variant::DICTIONARY: return object(p_value, p_depth);
			case Variant::PACKED_BYTE_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY:
			case Variant::PACKED_FLOAT32_ARRAY:
			case Variant::PACKED_FLOAT64_ARRAY:
			case Variant::PACKED_STRING_ARRAY: return array(Array(p_value), p_depth, nullptr);
			default: return fail(vformat("JSON cannot encode %s", Variant::get_type_name(p_value.get_type())));
		}
	}

	// Emit common values immediately and schedule conversions that require a worker.
	void emit(const Variant &p_value, int p_depth) {
		if (until && ((deterministic && p_value.get_type() == Variant::DICTIONARY) || p_value.get_type() >= Variant::PACKED_BYTE_ARRAY)) {
			push(VALUE, p_value, String(), nullptr, p_depth);
		} else {
			value(p_value, p_depth);
		}
	}

	// Remember completed names without copying encoded bytes or retaining request data after reset.
	void remember(const String &p_name, uint32_t p_index, int64_t p_offset) {
		const uint64_t slots = p_index < keys.size() ? 0 : uint64_t(p_index) + 1 - keys.size();
		const uint64_t old = p_index < keys.size() ? uint64_t(keys[p_index].name.length()) * sizeof(char32_t) : 0;
		const uint64_t bytes = key_bytes - old + uint64_t(p_name.length()) * sizeof(char32_t) + slots * sizeof(Key);
		// This is only a retention budget: every uncached key still encodes normally.
		if (bytes > JSON_CACHE_BYTES || out.size() - p_offset > INT_MAX) return;
		if (slots) keys.resize(p_index + 1);
		Key &key = keys[p_index];
		key.name = p_name;
		key.offset = p_offset;
		key.size = int(out.size() - p_offset);
		key_bytes = bytes;
	}

	// Write a member, reusing verified names across repeated nested object layouts.
	// Re-encoding repeated names repeats escape classification and Unicode conversion;
	// retaining completed output offsets avoids both without assuming immutable objects.
	void member(const String &p_name, const Variant &p_value, int p_depth, uint32_t p_index, bool p_cache) {
		// Position is a hint, not a shape guarantee: compare every code point, including NUL.
		// Type, collision and cycle validation remain outside this escape-only optimization.
		if (p_cache && p_index < keys.size()) {
			const Key &key = keys[p_index];
			if (key.size && key.name.length() == p_name.length() && (p_name.is_empty() || key.name.ptr() == p_name.ptr() || memcmp(key.name.ptr(), p_name.ptr(), size_t(p_name.length()) * sizeof(char32_t)) == 0)) {
				const int64_t offset = out.size();
				if (!room(key.size)) return;
				out.resize(offset + key.size);
				// Obtain pointers after growth; output offsets survive buffer reallocation.
				uint8_t *dst = out.ptrw();
				memcpy(dst + offset, dst + key.offset, key.size);
				if (byte(':')) emit(p_value, p_depth);
				return;
			}
		}
		Item name;
		name.action = TEXT;
		name.text = p_name;
		const int64_t offset = out.size();
		if (!text(name)) {
			push(VALUE, p_value, String(), nullptr, p_depth);
			push_byte(':');
			work.push_back(std::move(name));
		} else if (why.is_empty()) {
			if (p_cache) remember(p_name, p_index, offset);
			if (byte(':')) emit(p_value, p_depth);
		}
	}

public:
	// Release request data while retaining traversal storage for subsequent encodings.
	void reset() {
		out = PackedByteArray();
		keys.clear();
		key_bytes = 0;
		// Release oversized storage once the workload shrinks, retaining repeated deep traversals.
		if (uint64_t(work.get_capacity()) * sizeof(Item) > JSON_CACHE_BYTES && peak < work.get_capacity() / 2) {
			work.reset();
			active.reset();
		} else {
			work.clear();
			active.clear();
		}
		why = String();
		kind = Err::INVALID_DATA;
		max_bytes = max_depth = until = 0;
		peak = 0;
		deterministic = escape_html = false;
	}

	// Start response encoding without an application-specified byte limit.
	void start_reply(const Variant &p_value) {
		push(VALUE, p_value);
	}

	// Validate options and retain the root for incremental traversal.
	Ref<R> prepare(const Variant &p_value, const Dictionary &p_opts) {
		static const char *known[] = { "deterministic", "escape_html", "max_bytes", "max_depth", nullptr }; // Accepted configuration names.
		for (const KeyValue<Variant, Variant> &kv : p_opts) {
			if (kv.key.get_type() != Variant::STRING && kv.key.get_type() != Variant::STRING_NAME) {
				return R::err("JSON option name must be a string", Err::INVALID_DATA);
			}
			const String name = kv.key;
			int at = 0;
			while (known[at] && name != known[at]) at++;
			if (!known[at]) return R::err("unknown JSON option: " + name, Err::INVALID_DATA);
			const Variant::Type type = at < 2 ? Variant::BOOL : Variant::INT;
			if (kv.value.get_type() != type) {
				return R::err(vformat("JSON option %s must be %s", name, Variant::get_type_name(type)), Err::INVALID_DATA);
			}
		}
		deterministic = p_opts.get("deterministic", false);
		escape_html = p_opts.get("escape_html", false);
		const int64_t bytes = p_opts.get("max_bytes", 0);
		const int64_t depth = p_opts.get("max_depth", 0);
		if (bytes < 0 || depth < 0 || depth > INT32_MAX) {
			return R::err("JSON limits are out of range", Err::INVALID_DATA);
		}
		max_bytes = bytes;
		max_depth = depth;
		push(VALUE, p_value);
		return Ref<R>();
	}

	// Resume traversal, returning false when the current time slice ends.
	bool advance(uint64_t p_until = 0) {
		until = p_until;
		while (!work.is_empty() && why.is_empty()) {
			peak = MAX(peak, work.size());
			if (expired()) return pause();
			const Item &next = work[work.size() - 1];
			// Sorting and packed-container conversion require a native worker stack.
			if (until && next.action == VALUE && ((deterministic && next.value.get_type() == Variant::DICTIONARY) || next.value.get_type() >= Variant::PACKED_BYTE_ARRAY)) return pause();
			Item item = static_cast<Item &&>(work[work.size() - 1]);
			work.remove_at(work.size() - 1);
			switch (item.action) {
				case VALUE: value(item.value, item.depth); break;
				case TEXT: {
					if (!text(item)) {
						work.push_back(std::move(item));
						return pause();
					}
				} break;
				case BYTE: byte(item.byte); break;
				case LEAVE: active.erase(item.id); break;
				case ARRAY: {
					const Array values = item.value;
					if (values.size() != item.count) {
						fail("JSON input changed during encoding");
						break;
					}
					if (item.at >= values.size()) {
						byte(']');
						break;
					}
					if (item.at) byte(',');
					const Variant child = values[item.at++];
					const int depth = item.depth + 1;
					work.push_back(std::move(item));
					emit(child, depth);
				} break;
				case OBJECT: {
					const Dictionary values = item.value;
					if (values.size() != item.count) {
						fail("JSON input changed during encoding");
						break;
					}
					if ((!item.saved && !item.next) || item.at == item.count) {
						byte('}');
						break;
					}
					const Variant key = item.saved ? item.key : item.next->key;
					String name;
					if (!name_of(key, name)) break;
					// String-like keys are already unique; only decimal integer names can collide.
					if (key.get_type() == Variant::INT && !values.is_typed_key() && values.has(name)) {
						fail("duplicate JSON object name after key conversion");
						break;
					}
					const Variant *found = item.saved ? values.getptr(key) : &item.next->value;
					if (!found) {
						fail("JSON input changed during encoding");
						break;
					}
					const Variant child = *found;
					if (item.saved) {
						const Variant *next_key = values.next(&key);
						if (!next_key && item.at + 1 < item.count) {
							fail("JSON input changed during encoding");
							break;
						}
						item.key = next_key ? *next_key : Variant();
					} else {
						++item.next;
					}
					if (item.at++) byte(',');
					const int depth = item.depth + 1;
					const uint32_t index = item.at - 1;
					const bool cache = item.cache;
					work.push_back(std::move(item));
					member(name, child, depth, index, cache);
				} break;
				case MEMBERS: {
					if (uint32_t(item.at) == item.members->size()) {
						byte('}');
						break;
					}
					const JsonMember child = (*item.members)[item.at];
					if (item.at++) byte(',');
					const int depth = item.depth + 1;
					const uint32_t index = item.at - 1;
					const bool cache = item.cache;
					work.push_back(std::move(item));
					member(child.name, child.value, depth, index, cache);
				} break;
			}
		}
		return true;
	}

	// Materialize the public success-or-error object only when the caller needs it.
	Ref<R> result() const {
		return why.is_empty() ? R::ok(out) : R::err(why, kind);
	}

	// Transfer successful response bytes without allocating an intermediate result object.
	Variant reply() const {
		return why.is_empty() ? Variant(out) : Variant(result());
	}

	// Complete the same encoder synchronously on its owning worker.
	Ref<R> encode(const Variant &p_value, const Dictionary &p_opts) {
		const Ref<R> error = prepare(p_value, p_opts);
		if (error.is_valid()) return error;
		advance();
		return result();
	}
};

thread_local std::shared_ptr<StrictJSONOut> spare_encoder; // Exclusive idle traversal storage for this execution thread.

// Borrow idle storage without sharing an active encoder with nested calls.
std::shared_ptr<StrictJSONOut> take_encoder() {
	return spare_encoder ? std::exchange(spare_encoder, nullptr) : std::make_shared<StrictJSONOut>();
}

// Return traversal storage after output ownership has transferred to the result.
void keep_encoder(const std::shared_ptr<StrictJSONOut> &p_encoder) {
	p_encoder->reset();
	spare_encoder = p_encoder;
}

// Keep the outgoing bytes or error alive before returning idle traversal storage.
Variant encoded_reply(const std::shared_ptr<StrictJSONOut> &p_encoder, const std::function<Variant(const Variant &)> &p_finish) {
	const Variant result = p_encoder->reply();
	keep_encoder(p_encoder);
	return p_finish ? p_finish(result) : result;
}

// Decode four hexadecimal digits in a JSON string.
int hex4(const uint8_t *p_src) {
	int out = 0;
	for (int i = 0; i < 4; i++) {
		const uint8_t c = p_src[i];
		const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (n < 0) {
			return -1;
		}
		out = out * 16 + n;
	}
	return out;
}

// Parse decimal integer notation into signed 64 bits without precision loss.
bool int64_at(const uint8_t *p_src, int64_t p_size, int64_t &r_value) {
	if (p_size < 1) {
		return false;
	}
	int64_t at = 0;
	const bool negative = p_src[at] == '-';
	if (negative && ++at == p_size) {
		return false;
	}
	const uint64_t limit = negative ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
	uint64_t value = 0;
	for (; at < p_size; at++) {
		const uint8_t c = p_src[at];
		if (c < '0' || c > '9') {
			return false;
		}
		const uint64_t digit = c - '0';
		if (value > (limit - digit) / 10) {
			return false;
		}
		value = value * 10 + digit;
	}
	r_value = negative ? (value == uint64_t(INT64_MAX) + 1 ? INT64_MIN : -int64_t(value)) : int64_t(value);
	return true;
}

// Read one UTF-8 character, validating shortest form and Unicode scalar values.
bool rune_at(const uint8_t *p_src, int64_t p_size, int64_t &r_at, char32_t &r_rune) {
	const uint8_t a = p_src[r_at++];
	if (a < 0x80) {
		r_rune = a;
		return true;
	}
	int n = 0;
	char32_t rune = 0;
	char32_t low = 0;
	if (a >= 0xc2 && a <= 0xdf) {
		n = 1;
		rune = a & 0x1f;
		low = 0x80;
	} else if (a >= 0xe0 && a <= 0xef) {
		n = 2;
		rune = a & 0x0f;
		low = 0x800;
	} else if (a >= 0xf0 && a <= 0xf4) {
		n = 3;
		rune = a & 0x07;
		low = 0x10000;
	} else {
		return false;
	}
	if (n > p_size - r_at) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		const uint8_t c = p_src[r_at++];
		if ((c & 0xc0) != 0x80) {
			return false;
		}
		rune = (rune << 6) | (c & 0x3f);
	}
	if (rune < low || rune > 0x10ffff || (rune >= 0xd800 && rune <= 0xdfff)) {
		return false;
	}
	r_rune = rune;
	return true;
}

// Validate JSON structure with an iterative scanner.
class StrictJSON {
	enum Kind {
		OBJECT,
		ARRAY,
	};
	enum State {
		KEY_OR_END,
		KEY,
		COLON,
		VALUE_OR_END,
		VALUE,
		COMMA_OR_END,
	};
	struct Frame {
		Kind kind = ARRAY;
		State state = VALUE_OR_END;
		String key; // Name for the next object value.
		Variant data; // Object or array being assembled.
	};

	const uint8_t *src = nullptr;
	int64_t size = 0; // Total input byte length, independent of individual String limits.
	int64_t at = 0; // 64-bit input scan position.
	LocalVector<Frame> stack;
	bool root_done = false;
	Variant root;
	String why;
	String pending; // Flushed string content retained across a suspended key or value.
	bool reading = false; // The opening quote has already been consumed.
	uint64_t until = 0; // Shared turn deadline; zero permits uninterrupted worker parsing.
	int left = 0; // Remaining speculative main-thread work before preserving worker parallelism.
	static constexpr int batch = 64; // Inline work and native number-token allowance, never an input limit.
	char32_t buf[batch] = {}; // Decoded characters awaiting one combined string append.
	int used = 0; // Characters retained in the staging buffer across a handoff.

	// Bound speculation by both the caller's time slice and a small transferable work quantum.
	bool expired() {
		if (!until) return false;
		if (left == batch && GDClock::usec() >= until) return true;
		return left-- <= 0;
	}

	// Preserve only the first parsing failure.
	bool fail(const String &p_why) {
		if (why.is_empty()) {
			why = vformat("%s at byte %d", p_why, at);
		}
		return false;
	}

	// Skip the four whitespace characters permitted by JSON.
	bool space() {
		while (at < size && (src[at] == ' ' || src[at] == '\t' || src[at] == '\r' || src[at] == '\n')) {
			if (expired()) return false;
			at++;
		}
		return true;
	}

	// Update parent state after completing one value.
	bool value_done(const Variant &p_value) {
		if (stack.is_empty()) {
			if (root_done) {
				return fail("multiple JSON values");
			}
			root = p_value;
			root_done = true;
			return true;
		}
		Frame &f = stack[stack.size() - 1];
		if (f.state != VALUE && f.state != VALUE_OR_END) {
			return fail("unexpected JSON value");
		}
		if (f.kind == OBJECT) {
			Dictionary object = f.data;
			object[f.key] = p_value;
		} else {
			Array array = f.data;
			array.push_back(p_value);
		}
		f.state = COMMA_OR_END;
		return true;
	}

	// Append a decoded batch by length, including embedded zero characters.
	bool flush() {
		if (!used) return true;
		const int n = pending.length();
		if (used > INT_MAX - 1 - n) return fail("JSON string exceeds the native string capacity");
		if (pending.resize_uninitialized(n + used + 1) != OK) return fail("cannot allocate JSON string");
		char32_t *dst = pending.ptrw();
		memcpy(dst + n, buf, used * sizeof(char32_t));
		dst[n + used] = 0;
		used = 0;
		return true;
	}

	// Stage validated scalars without resizing the string for each character.
	bool append(char32_t p_rune) {
		if (used == batch && !flush()) return false;
		buf[used++] = p_rune;
		return true;
	}

	// Decode a JSON string and its escapes.
	bool string(String &r_text) {
		if (!reading) {
			if (at >= size || src[at++] != '"') return fail("JSON string is missing");
			reading = true;
		}
		int plain = 0;
		while (at < size) {
			if (expired()) return false;
			const uint8_t c = src[at];
			if (c == '"') {
				at++;
				if (!flush()) return false;
				r_text = std::move(pending);
				reading = false;
				return true;
			}
			if (c == '\\') {
				plain = 0;
				at++;
				if (at >= size) {
					return fail("unfinished JSON escape");
				}
				const uint8_t e = src[at++];
				char32_t rune = 0;
				switch (e) {
					case '"': rune = '"'; break;
					case '\\': rune = '\\'; break;
					case '/': rune = '/'; break;
					case 'b': rune = '\b'; break;
					case 'f': rune = '\f'; break;
					case 'n': rune = '\n'; break;
					case 'r': rune = '\r'; break;
					case 't': rune = '\t'; break;
					case 'u': {
						if (4 > size - at) {
							return fail("unfinished JSON unicode escape");
						}
						const int first = hex4(src + at);
						at += 4;
						if (first < 0) {
							return fail("invalid JSON unicode escape");
						}
						if (first >= 0xd800 && first <= 0xdbff) {
							if (6 > size - at || src[at] != '\\' || src[at + 1] != 'u') {
								return fail("unpaired JSON surrogate");
							}
							const int second = hex4(src + at + 2);
							if (second < 0xdc00 || second > 0xdfff) {
								return fail("unpaired JSON surrogate");
							}
							at += 6;
							rune = 0x10000 + ((first - 0xd800) << 10) + second - 0xdc00;
						} else if (first >= 0xdc00 && first <= 0xdfff) {
							return fail("unpaired JSON surrogate");
						} else {
							rune = first;
						}
						break;
					}
					default: return fail("invalid JSON escape");
				}
				if (!append(rune)) return false;
				continue;
			}
			char32_t rune = 0;
			if (!rune_at(src, size, at, rune)) {
				return fail("invalid UTF-8 in JSON string");
			}
			if (rune < 0x20) {
				return fail("control character in JSON string");
			}
			if (!append(rune)) return false;
			// Accumulate a real run before probing: short keys and mixed Unicode stay
			// on the scalar path. Additional characters consume the existing budget.
			plain = rune < 0x80 ? plain + 1 : 0;
			if (plain == JSON_SCAN_PREFIX) {
				const int span = int(MIN(int64_t(batch - used), size - at));
				const int count = json_ascii(src + at, buf + used, until ? MIN(span, left) : span);
				at += count;
				used += count;
				if (until) left -= count;
				plain = 0;
			}
		}
		return fail("unfinished JSON string");
	}

	// Parse a JSON number as float64 and reject overflow.
	bool number(Variant &r_value) {
		// Keep uninterruptible decimal conversion off the main turn for long tokens.
		if (until) {
			int64_t end = at;
			while (end < size) {
				const uint8_t c = src[end];
				if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) break;
				if (end - at == batch) return false;
				end++;
			}
		}
		const int64_t start = at;
		const bool negative = src[at] == '-';
		bool integer = true;
		if (src[at] == '-') {
			at++;
			if (at >= size) {
				return fail("unfinished JSON number");
			}
		}
		int64_t digits_before = 0;
		int64_t digit_index = 0;
		int64_t first_nonzero = -1;
		auto take_digit = [&](uint8_t p_digit, bool p_integer) {
			if (p_integer) {
				digits_before++;
			}
			if (first_nonzero < 0 && p_digit != '0') {
				first_nonzero = digit_index;
			}
			digit_index++;
		};
		if (src[at] == '0') {
			take_digit(src[at], true);
			at++;
			if (at < size && src[at] >= '0' && src[at] <= '9') {
				return fail("leading zero in JSON number");
			}
		} else if (src[at] >= '1' && src[at] <= '9') {
			while (at < size && src[at] >= '0' && src[at] <= '9') {
				take_digit(src[at], true);
				at++;
			}
		} else {
			return fail("invalid JSON number");
		}
		if (at < size && src[at] == '.') {
			integer = false;
			at++;
			if (at >= size || src[at] < '0' || src[at] > '9') {
				return fail("invalid JSON fraction");
			}
			while (at < size && src[at] >= '0' && src[at] <= '9') {
				take_digit(src[at], false);
				at++;
			}
		}
		int64_t exponent = 0;
		bool exponent_negative = false;
		if (at < size && (src[at] == 'e' || src[at] == 'E')) {
			integer = false;
			at++;
			if (at < size && (src[at] == '+' || src[at] == '-')) {
				exponent_negative = src[at] == '-';
				at++;
			}
			if (at >= size || src[at] < '0' || src[at] > '9') {
				return fail("invalid JSON exponent");
			}
			while (at < size && src[at] >= '0' && src[at] <= '9') {
				const int digit = src[at] - '0';
				const int64_t cap = INT64_MAX; // Saturate only the numeric exponent, not input consumption.
				exponent = exponent > (cap - digit) / 10 ? cap : exponent * 10 + digit;
				at++;
			}
		}
		int64_t integer_value = 0;
		if (integer && int64_at(src + start, at - start, integer_value)) {
			r_value = integer_value;
			return true;
		}
		if (first_nonzero < 0) {
			r_value = negative ? -0.0 : 0.0;
			return true;
		}
		const int64_t base_order = digits_before - first_nonzero - 1;
		const int64_t order = exponent_negative ? (base_order < INT64_MIN + exponent ? INT64_MIN : base_order - exponent) :
				(base_order > INT64_MAX - exponent ? INT64_MAX : base_order + exponent);
		if (order > 308) {
			return fail("JSON number overflows float64");
		}
		if (order < -324) {
			r_value = negative ? -0.0 : 0.0;
			return true;
		}
		double value = 0;
		if (!json_float(src + start, at - start, order, value)) {
			return fail("JSON number conversion failed");
		}
		if (!Math::is_finite(value)) {
			return fail("JSON number overflows float64");
		}
		r_value = value;
		return true;
	}

	// Match a fixed token at the current position and advance.
	bool literal(const char *p_text, int p_len) {
		if (p_len > size - at || memcmp(src + at, p_text, p_len) != 0) {
			return fail("invalid JSON literal");
		}
		at += p_len;
		return true;
	}

	// Read a value prefix and push nested containers onto an explicit stack.
	bool value() {
		if (!reading && !space()) return false;
		if (!reading && at >= size) {
			return fail("JSON value is missing");
		}
		const uint8_t c = reading ? '"' : src[at];
		if (c == '{' || c == '[') {
			if ((int)stack.size() >= JSON_DEPTH_MAX) {
				return fail("JSON nesting is too deep");
			}
			Frame f;
			f.kind = c == '{' ? OBJECT : ARRAY;
			f.state = c == '{' ? KEY_OR_END : VALUE_OR_END;
			f.data = c == '{' ? Variant(Dictionary()) : Variant(Array());
			stack.push_back(f);
			at++;
			return true;
		}
		Variant data;
		bool ok = false;
		if (c == '"') {
			String text;
			ok = string(text);
			data = text;
		} else if (c == '-' || (c >= '0' && c <= '9')) {
			ok = number(data);
		} else if (c == 't') {
			ok = literal("true", 4);
			data = true;
		} else if (c == 'f') {
			ok = literal("false", 5);
			data = false;
		} else if (c == 'n') {
			ok = literal("null", 4);
			data = Variant();
		} else {
			return fail("invalid JSON value");
		}
		return ok && value_done(data);
	}

public:
	// Validate the entire input as exactly one unambiguous JSON value.
	bool parse(const PackedByteArray &p_src, uint64_t p_until = 0) {
		src = p_src.ptr();
		size = p_src.size();
		until = p_until;
		left = batch;
		if (expired()) return false;
		if (!root_done && stack.is_empty() && !value()) {
			return false;
		}
		while (!stack.is_empty()) {
			if (expired() || (!reading && !space())) return false;
			Frame &f = stack[stack.size() - 1];
			if (!reading && at >= size) {
				return fail(f.kind == OBJECT && f.state == VALUE ? "JSON value is missing" : "unfinished JSON container");
			}
			if (f.kind == OBJECT) {
				if (f.state == KEY_OR_END || f.state == KEY) {
					if (!reading && src[at] == '}' && f.state == KEY_OR_END) {
						const Variant closed = f.data;
						at++;
						stack.remove_at(stack.size() - 1);
						if (!value_done(closed)) {
							return false;
						}
						continue;
					}
					String name;
					if (!string(name)) {
						return false;
					}
					// Every preceding value is already stored, including null and nested containers.
					if (Dictionary(f.data).has(name)) {
						return fail("duplicate JSON object name");
					}
					f.key = name;
					f.state = COLON;
					continue;
				}
				if (f.state == COLON) {
					if (src[at++] != ':') {
						return fail("JSON object needs a colon");
					}
					f.state = VALUE;
				}
				if (f.state == VALUE) {
					if (!value()) {
						return false;
					}
					continue;
				}
				if (src[at] == '}') {
					const Variant closed = f.data;
					at++;
					stack.remove_at(stack.size() - 1);
					if (!value_done(closed)) {
						return false;
					}
				} else if (src[at] == ',') {
					at++;
					f.state = KEY;
				} else {
					return fail("JSON object needs a comma or end");
				}
				continue;
			}
			if (!reading && f.state == VALUE_OR_END && src[at] == ']') {
				const Variant closed = f.data;
				at++;
				stack.remove_at(stack.size() - 1);
				if (!value_done(closed)) {
					return false;
				}
				continue;
			}
			if (f.state == VALUE_OR_END || f.state == VALUE) {
				if (!value()) {
					return false;
				}
				continue;
			}
			if (src[at] == ']') {
				const Variant closed = f.data;
				at++;
				stack.remove_at(stack.size() - 1);
				if (!value_done(closed)) {
					return false;
				}
			} else if (src[at] == ',') {
				at++;
				f.state = VALUE;
			} else {
				return fail("JSON array needs a comma or end");
			}
		}
		if (!space()) return false;
		return root_done && at == size ? true : fail("data after JSON value");
	}

	// Distinguish unfinished work from a completed value or parsing failure.
	bool failed() const { return !why.is_empty(); }
	Ref<R> result() const { return failed() ? R::err(why, Err::INVALID_DATA) : R::ok(root); }
};

} // namespace

// Validate a value and encode it as UTF-8 JSON bytes.
Ref<R> JsonData::encode(const Variant &p_value, const Dictionary &p_opts) {
	auto encoder = take_encoder();
	const Ref<R> result = encoder->encode(p_value, p_opts);
	keep_encoder(encoder);
	return result;
}

// Keep short encoding local and transfer unfinished traversal without restarting it.
// Immediate replies avoid allocating a worker job and suspending the handler for an
// already-complete value. The shared deadline protects other requests; its expiry
// moves the same encoder state to a worker, without truncating or serializing twice.
// The native-callback probe in tests/net/encode detects unnecessary suspension here.
Variant JsonData::encode_reply(const Variant &p_value, uint64_t p_until, std::function<Variant(const Variant &)> p_finish) {
	auto encoder = take_encoder();
	encoder->start_reply(p_value);
	if (encoder->advance(p_until)) return encoded_reply(encoder, p_finish);
	return GDValueCall::start([encoder, finish = std::move(p_finish)]() -> Variant {
		encoder->advance();
		return encoded_reply(encoder, finish);
	});
}

// Validate JSON bytes and convert them to runtime values.
Ref<R> JsonData::decode(const PackedByteArray &p_src) {
	StrictJSON scan;
	scan.parse(p_src);
	return scan.result();
}

// Avoid a worker round trip when strict decoding completes within the caller's shared turn.
// Keep this speculative parse on the caller: unconditional worker submission adds
// queueing and VM continuation costs even for a fully buffered, immediately usable value.
// The work quantum is a handoff threshold, not a JSON size limit. Move the scanner's
// position, partial strings and container stack intact; restarting would duplicate
// work, while a separate fast parser could disagree on UTF-8, duplicate keys or errors.
Variant JsonData::decode_reply(const PackedByteArray &p_src, uint64_t p_until) {
	StrictJSON scan;
	if (scan.parse(p_src, p_until) || scan.failed()) return scan.result();
	auto pending = std::make_shared<StrictJSON>(std::move(scan));
	return GDValueCall::start([pending = std::move(pending), bytes = p_src]() mutable -> Variant {
		pending->parse(bytes);
		const Ref<R> result = pending->result();
		pending.reset(); // Destroy rejected partial containers on the worker that parsed them.
		return result;
	});
}
