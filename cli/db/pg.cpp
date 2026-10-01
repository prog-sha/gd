/**************************************************************************/
/*  pg.cpp                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement remote SQL connections declared in pg.h.

#include "cli/db/pg.h"
#include "cli/db/options.h"
#include "cli/sys/system.h"
#include "cli/sys/clock.h"
#include "cli/data/utf8.h"
#include "cli/data/json.h"
#include "cli/db/rows.h"
#include "cli/sys/limit.h"
#include "cli/sys/file_job.h"

#include "cli/data/bytes.h"
#include "cli/data/codec.h"
#include "cli/sys/sched.h"
#include "cli/sys/perm.h"
#include "cli/sys/task.h" // Async::ready for lazy-pool configuration results.
#include "cli/sys/wait.h" // IdleWait for immediate buffered continuation.

#include "core/core_bind.h"
#include "cli/data/digest.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"

namespace {

HashSet<GDPostgresClient *> postgres_clients; // Connections requiring shutdown cleanup.

// Validate authentication settings before the connection uses typed values.
bool pg_credentials(const Dictionary &p_opts) {
	String value;
	if (!DbOption::text(p_opts, "user", "postgres", value) || !DbOption::text(p_opts, "database", "postgres", value) ||
			!DbOption::text(p_opts, "password", "", value)) return false;
	return p_opts.get("allow_cleartext_password", false).get_type() == Variant::BOOL;
}

// Build a native success with separate result slots.
VariantPair pg_ok(const Variant &p_value = Variant()) {
	return { p_value, Variant() };
}

// Keep completed work on an error without allocating a result object.
VariantPair pg_fail(const Variant &p_reason, Err::Kind p_kind = Err::NONE, const Variant &p_value = Variant()) {
	const Ref<Err> error = Err::from(p_reason, p_kind);
	return { p_value, error->with_partial(p_value) };
}

// Read the failure slot of an internal result.
Ref<Err> pg_result_error(const VariantPair &p_result) {
	return p_result.error;
}

constexpr int STMT_MAX = 512; // Default statement-cache capacity.
constexpr int SPARE_MAX = 64; // Reusable operation objects retained in reserve.
constexpr int PARAM_MAX = 65535; // Maximum bind count representable by the protocol's uint16 field.
constexpr int PARAM_DEPTH_MAX = Variant::MAX_RECURSION_DEPTH; // Recursion boundary supported by Variant traversal.
constexpr int SEND_BYTES_MAX = 0x3fffffff - 1; // Maximum long-message size accepted by the server.
constexpr int PACK_BYTES_MAX = INT_MAX; // Encoded-batch representation boundary for LocalVector and socket APIs.
constexpr int STMT_CLOSE_BOUND = 32; // Conservative maximum bytes for closing one evicted statement.
constexpr int INLINE_PACK_MAX = 4 * 1024; // Encode small sends inline to avoid a worker round trip.
constexpr int PG_READ_CHUNK = 8192; // Minimum receive chunk size.
constexpr int PROTO_V3 = 196608; // protocol 3.0

// Create a deadline without overflow; zero means no deadline.
uint64_t deadline_after(uint64_t p_wait) {
	const uint64_t now = GDClock::msec();
	return p_wait == 0 ? 0 : (p_wait > UINT64_MAX - now ? UINT64_MAX : now + p_wait);
}
constexpr int SCRAM_MIN_ITERS = 4096; // Minimum key-derivation iterations required by RFC 5802.
constexpr int SCRAM_MAX_ITERS = 10000000; // CPU-work ceiling for untrusted server iteration counts.

// Return a remote SQL MD5 authentication value only when both digest stages succeed.
VariantPair md5_password(const String &p_password, const String &p_user, const uint8_t *p_salt) {
	PackedByteArray first;
	const Ref<Err> first_error = GDDigest::md5((p_password + p_user).to_utf8_buffer(), first);
	if (first_error.is_valid()) return { Variant(), first_error };
	const CharString inner = Encoding::hex_encode(first).utf8();
	PackedByteArray input;
	if (input.resize(inner.length() + 4) != OK) return { Variant(), Err::make("cannot allocate MD5 authentication input", Err::LIMITED) };
	memcpy(input.ptrw(), inner.get_data(), inner.length());
	memcpy(input.ptrw() + inner.length(), p_salt, 4);
	PackedByteArray last;
	const Ref<Err> last_error = GDDigest::md5(input, last);
	return last_error.is_valid() ? VariantPair{ Variant(), last_error } : VariantPair{ "md5" + Encoding::hex_encode(last), Variant() };
}

// Append directly to the shared buffer instead of allocating a small container
// for each protocol field or message in a query batch.
void put8(ByteBuf &r_out, uint8_t p_v) {
	r_out.push_back(p_v);
}

// Append a 16-bit value in network byte order.
void put16(ByteBuf &r_out, int16_t p_v) {
	const uint32_t at = r_out.size();
	r_out.resize(at + 2);
	uint8_t *w = r_out.ptr() + at;
	w[0] = (p_v >> 8) & 0xFF;
	w[1] = p_v & 0xFF;
}

// Append a 32-bit value in network byte order.
void put32(ByteBuf &r_out, int32_t p_v) {
	const uint32_t at = r_out.size();
	r_out.resize(at + 4);
	uint8_t *w = r_out.ptr() + at;
	w[0] = (p_v >> 24) & 0xFF;
	w[1] = (p_v >> 16) & 0xFF;
	w[2] = (p_v >> 8) & 0xFF;
	w[3] = p_v & 0xFF;
}

// Write a 32-bit value in network byte order at a reserved position.
void set32(ByteBuf &r_out, uint32_t p_at, int32_t p_v) {
	uint8_t *w = r_out.ptr() + p_at;
	w[0] = (p_v >> 24) & 0xFF;
	w[1] = (p_v >> 16) & 0xFF;
	w[2] = (p_v >> 8) & 0xFF;
	w[3] = p_v & 0xFF;
}

// Fill in the message length in the four bytes following its type marker.
void fix_len(ByteBuf &r_out, uint32_t p_len_at) {
	set32(r_out, p_len_at, (int32_t)(r_out.size() - p_len_at));
}

// Append a NUL-terminated string.
void put_cstr(ByteBuf &r_out, const String &p_s) {
	const CharString u = p_s.utf8();
	const uint32_t n = (uint32_t)u.length();
	const uint32_t at = r_out.size();
	r_out.resize(at + n + 1);
	uint8_t *w = r_out.ptr() + at;
	memcpy(w, u.get_data(), n);
	w[n] = 0;
}

// Format numeric arrays without intermediate element strings.
// Append a numeric array directly using brace-delimited text such as {1,2,3}.
template <typename T>
// Append numeric array elements in protocol text format.
void put_nums(ByteBuf &r_out, const T &p_nums) {
	r_out.push_back('{');
	for (int i = 0; i < p_nums.size(); i++) {
		if (i > 0) {
			r_out.push_back(',');
		}
		push_digits(r_out, p_nums[i]);
	}
	r_out.push_back('}');
}

// Read four bytes in network byte order.
int32_t read32p(const uint8_t *p_b, int p_at) {
	return (int32_t)(((uint32_t)p_b[p_at] << 24) | ((uint32_t)p_b[p_at + 1] << 16) | ((uint32_t)p_b[p_at + 2] << 8) | p_b[p_at + 3]);
}

// Read two bytes in network byte order.
int read16p(const uint8_t *p_b, int p_at) {
	return (p_b[p_at] << 8) | p_b[p_at + 1];
}

// Decode protocol text while preserving a leading byte-order mark as column data.
String pg_text(const uint8_t *p_data, int p_len, Error *r_error = nullptr) {
	String text;
	const bool bom = p_len >= 3 && p_data[0] == 0xef && p_data[1] == 0xbb && p_data[2] == 0xbf;
	if (bom) text = String::chr(0xfeff);
	const Error error = text.append_utf8((const char *)p_data, p_len);
	if (r_error) *r_error = error;
	return text;
}

// Read a NUL-terminated string and advance the cursor.
String read_cstrp(const uint8_t *p_b, int p_len, int p_at, int &r_next) {
	int end = p_at;
	while (end < p_len && p_b[end] != 0) {
		end++;
	}
	r_next = end + 1;
	return pg_text(p_b + p_at, end - p_at);
}

// Validate session status before startup completion or result decoding can use its settings.
Ref<Err> status_error(const uint8_t *p_data, int p_len) {
	int next = 0;
	const String key = read_cstrp(p_data, p_len, 0, next);
	if (next >= p_len) return Err::make("invalid parameter status", Err::INVALID_DATA);
	const String value = read_cstrp(p_data, p_len, next, next);
	if (next != p_len) return Err::make("invalid parameter status", Err::INVALID_DATA);
	if (key == "client_encoding" && value != "UTF8") {
		return Err::make("postgres client_encoding must remain UTF8", Err::UNSUPPORTED);
	}
	return Ref<Err>();
}

// Read a NUL-terminated string and advance the cursor.
String read_cstr(const PackedByteArray &p_b, int p_at, int &r_next) {
	int end = p_at;
	while (end < p_b.size() && p_b[end] != 0) {
		end++;
	}
	r_next = end + 1;
	return String::utf8((const char *)p_b.ptr() + p_at, end - p_at);
}

// Decode binary values in network byte order.
Variant from_binary(const uint8_t *p_b, int p_len, int32_t p_oid) {
	switch (p_oid) {
		case 16: // bool
			return p_len > 0 && p_b[0] != 0;
		case 21: // int2
			if (p_len < 2) {
				return 0;
			}
			return (int64_t)(int16_t)(((uint16_t)p_b[0] << 8) | p_b[1]);
		case 23: { // int4
			if (p_len < 4) {
				return 0;
			}
			return (int64_t)(int32_t)(((uint32_t)p_b[0] << 24) | ((uint32_t)p_b[1] << 16) | ((uint32_t)p_b[2] << 8) | p_b[3]);
		}
		case 26: { // Keep unsigned OIDs positive at their upper boundary.
			if (p_len < 4) {
				return 0;
			}
			const uint32_t v = ((uint32_t)p_b[0] << 24) | ((uint32_t)p_b[1] << 16) | ((uint32_t)p_b[2] << 8) | p_b[3];
			return (int64_t)v;
		}
		case 20: { // int8
			if (p_len < 8) {
				return 0;
			}
			uint64_t v = 0;
			for (int i = 0; i < 8; i++) {
				v = (v << 8) | p_b[i];
			}
			return (int64_t)v;
		}
		case 700: { // float4
			if (p_len < 4) {
				return 0.0;
			}
			uint32_t v = ((uint32_t)p_b[0] << 24) | ((uint32_t)p_b[1] << 16) | ((uint32_t)p_b[2] << 8) | p_b[3];
			float f = 0.0f;
			memcpy(&f, &v, 4);
			return (double)f;
		}
		case 701: { // float8
			if (p_len < 8) {
				return 0.0;
			}
			uint64_t v = 0;
			for (int i = 0; i < 8; i++) {
				v = (v << 8) | p_b[i];
			}
			double d = 0.0;
			memcpy(&d, &v, 8);
			return d;
		}
		default:
			return Variant();
	}
}

// Use binary results for numeric, boolean, and UTF-8 text types with supported layouts.
// Leave complex date-time and array representations in text form.
bool binary_ok(int32_t p_oid) {
	switch (p_oid) {
		case 16: // bool
		case 20: // int8
		case 21: // int2
		case 23: // int4
		case 26: // oid
		case 701: // float8
		case 25: // Binary text is its UTF-8 body.
		case 1042: // Binary bpchar is also its UTF-8 body.
		case 1043: // Binary varchar is also its UTF-8 body.
			return true;
		// Keep float4 in text form so four-byte binary precision does not change
		// the visible value between first and subsequent executions of the same query.
		default:
			return false;
	}
}

// Copy a message body for infrequent paths such as authentication.
PackedByteArray _slice(const uint8_t *p_b, int p_len) {
	PackedByteArray out;
	out.resize(p_len);
	if (p_len > 0) {
		memcpy(out.ptrw(), p_b, p_len);
	}
	return out;
}

// Retain server errors as display text and machine-readable fields.
struct PgError {
	String msg = "unknown error"; // Short user-facing message.
	String detail; // Details used to extract constraint columns.
	String code; // Locale-independent SQLSTATE.
	String schema; // Related schema name.
	String table; // Related table name.
	String column; // Column name supplied directly by the server.
	String constraint; // Related constraint name.
};

// Extract standard protocol fields from an error response.
PgError pg_error(const uint8_t *p_data, int p_len) {
	PgError out;
	int at = 0;
	while (at < p_len && p_data[at] != 0) {
		const int field = p_data[at];
		int next = 0;
		const String got = read_cstrp(p_data, p_len, at + 1, next);
		if (field == 'M') {
			out.msg = got;
		} else if (field == 'D') {
			out.detail = got;
		} else if (field == 'C') {
			out.code = got;
		} else if (field == 's') {
			out.schema = got;
		} else if (field == 't') {
			out.table = got;
		} else if (field == 'c') {
			out.column = got;
		} else if (field == 'n') {
			out.constraint = got;
		}
		at = next;
	}
	return out;
}

// Parse column names in Detail's Key (...) notation while respecting quotes.
PackedStringArray key_columns(const String &p_detail) {
	PackedStringArray out;
	const int begin = p_detail.find("Key (");
	if (begin < 0) {
		return out;
	}
	String name;
	bool quoted = false;
	bool was_quoted = false;
	bool quote_closed = false;
	for (int i = begin + 5; i < p_detail.length(); i++) {
		const char32_t ch = p_detail[i];
		if (quoted) {
			if (ch != '"') {
				name += ch;
				continue;
			}
			if (i + 1 < p_detail.length() && p_detail[i + 1] == '"') {
				name += '"';
				i++;
				continue;
			}
			quoted = false;
			quote_closed = true;
			continue;
		}
		if (quote_closed && ch != ',' && !(ch == ')' && i + 2 < p_detail.length() && p_detail[i + 1] == '=' && p_detail[i + 2] == '(')) {
			if (ch == ' ' || ch == '\t') {
				continue;
			}
			return PackedStringArray();
		}
		if (ch == '"' && name.strip_edges().is_empty()) {
			name = String();
			quoted = true;
			was_quoted = true;
			continue;
		}
		if (ch == ',') {
			const String column = was_quoted ? name : name.strip_edges();
			if (column.is_empty()) {
				return PackedStringArray();
			}
			out.push_back(column);
			name = String();
			was_quoted = false;
			quote_closed = false;
			continue;
		}
		if (ch == ')' && i + 2 < p_detail.length() && p_detail[i + 1] == '=' && p_detail[i + 2] == '(') {
			const String column = was_quoted ? name : name.strip_edges();
			if (column.is_empty()) {
				return PackedStringArray();
			}
			out.push_back(column);
			return out;
		}
		name += ch;
	}
	return PackedStringArray();
}

// Recognize only known constraint messages when driver text lacks SQLSTATE.
String violation_of(const PgError &p_error) {
	const bool text_only = p_error.code.is_empty(); // Use message heuristics only when SQLSTATE is absent.
	if (p_error.code == "23505" || (text_only && p_error.msg.begins_with("duplicate key value violates unique constraint \""))) {
		return "duplicate";
	}
	if (p_error.code == "23502" || (text_only && p_error.msg.begins_with("null value in column \"") && p_error.msg.ends_with("violates not-null constraint"))) {
		return "not_null";
	}
	if (p_error.code == "23503" || (text_only && (p_error.msg.begins_with("insert or update on table \"") || p_error.msg.begins_with("update or delete on table \"")) && p_error.msg.contains(" violates foreign key constraint \""))) {
		return "foreign_key";
	}
	return String();
}

// Recover a missing column field from the standard NOT NULL message.
String not_null_column(const String &p_msg) {
	const String prefix = "null value in column \"";
	if (!p_msg.begins_with(prefix)) {
		return String();
	}
	const int end = p_msg.find("\" of relation \"", prefix.length());
	return end < 0 ? String() : p_msg.substr(prefix.length(), end - prefix.length());
}

// Convert remote SQL errors into inspectable Err values without exposing data values.
Ref<Err> query_error(const PgError &p_error) {
	Dictionary info;
	info["source"] = "postgres";
	if (!p_error.code.is_empty()) {
		info["code"] = p_error.code;
	}
	if (!p_error.schema.is_empty()) {
		info["schema"] = p_error.schema;
	}
	if (!p_error.table.is_empty()) {
		info["table"] = p_error.table;
	}
	if (!p_error.constraint.is_empty()) {
		info["constraint"] = p_error.constraint;
	}
	const String violation = violation_of(p_error);
	if (!violation.is_empty()) {
		info["violation"] = violation;
		PackedStringArray columns;
		if (!p_error.column.is_empty()) {
			columns.push_back(p_error.column);
		} else {
			columns = key_columns(p_error.detail);
			if (columns.is_empty() && violation == "not_null") {
				const String column = not_null_column(p_error.msg);
				if (!column.is_empty()) {
					columns.push_back(column);
				}
			}
		}
		info["columns"] = columns;
	}
	const Err::Kind kind = violation == "duplicate" ? Err::ALREADY_EXISTS : Err::NONE;
	return Err::make(p_error.msg, kind, info);
}

// Convert scalar values for standalone columns and array elements.
Variant convert(const String &p_raw, int p_oid);

// Decode quoted array elements and dimensions without losing nulls or explicit lower bounds.
Variant array_value(const String &p_raw, int p_oid) {
	if (!p_raw.begins_with("{")) {
		return p_raw; // Retain dimensions that ordinary arrays cannot represent.
	}
	LocalVector<Array> stack;
	bool need = true;
	for (int at = 0; at < p_raw.length();) {
		const char32_t ch = p_raw[at];
		if (ch == '{' && need) {
			stack.push_back(Array());
			at++;
		} else if (ch == '}' && !stack.is_empty() && (!need || stack[stack.size() - 1].is_empty())) {
			Array value = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			at++;
			if (stack.is_empty()) {
				return at == p_raw.length() ? Variant(value) : Variant(p_raw);
			}
			stack[stack.size() - 1].push_back(value);
			need = false;
		} else if (ch == ',' && !need && !stack.is_empty()) {
			need = true;
			at++;
		} else if (need && !stack.is_empty()) {
			const bool quoted = ch == '"'; // Quoted NULL and empty strings are ordinary values.
			bool closed = !quoted;
			bool escaped = false;
			String text;
			if (quoted) at++;
			while (at < p_raw.length()) {
				char32_t c = p_raw[at++];
				if (c == '\\') {
					if (at == p_raw.length()) return p_raw;
					escaped = true;
					c = p_raw[at++];
				} else if (quoted && c == '"') {
					closed = true;
					break;
				} else if (!quoted && (c == ',' || c == '}')) {
					at--;
					break;
				} else if (!quoted && (c == '{' || c == '"')) {
					return p_raw;
				}
				text += c;
			}
			if (!closed || (!quoted && text.is_empty())) return p_raw;
			Variant value;
			if (quoted || escaped || text != "NULL") {
				if (p_oid == 1009) {
					value = text;
				} else if (p_oid == 1000) {
					if (text != "t" && text != "f") return p_raw;
					value = text == "t";
				} else if (p_oid == 1021 || p_oid == 1022) {
					value = convert(text, p_oid == 1021 ? 700 : 701);
				} else if (p_oid == 1231) {
					value = convert(text, 1700);
				} else {
					if (!text.is_valid_int()) return p_raw;
					const int64_t number = text.to_int();
					if (String::num_int64(number) != text || (p_oid == 1005 && (number < INT16_MIN || number > INT16_MAX)) || (p_oid == 1007 && (number < INT32_MIN || number > INT32_MAX))) return p_raw;
					value = number;
				}
			}
			stack[stack.size() - 1].push_back(value);
			need = false;
		} else {
			return p_raw;
		}
	}
	return p_raw; // Incomplete array syntax remains lossless source text.
}

// Convert a text value into Variant according to its type OID.
Variant convert(const String &p_raw, int p_oid) {
	switch (p_oid) {
		case 20: // int8
		case 21: // int2
		case 23: // int4
		case 26: // oid
			return p_raw.to_int();
		case 700: // float4
		case 701: // float8
			// Recognize NaN and infinity before to_float would collapse them to zero.
			if (p_raw == "NaN") {
				return Math::NaN;
			}
			if (p_raw == "Infinity") {
				return Math::INF;
			}
			if (p_raw == "-Infinity") {
				return -Math::INF;
			}
			return p_raw.to_float();
		case 1700: // numeric
			return p_raw; // Preserve exact decimal digits because Variant has no decimal type.
		case 16: // bool
			return p_raw == "t";
		case 114: // json
		case 3802: { // jsonb
			// Preserve exact scalar values, retaining source text when it cannot be represented unambiguously.
			const VariantPair json = JsonData::decode(p_raw.to_utf8_buffer());
			return json.error.get_type() == Variant::NIL ? json.value : Variant(p_raw);
		}
		case 1000: // Array types.
		case 1005:
		case 1007:
		case 1009:
		case 1016:
		case 1021:
		case 1022:
		case 1231: {
			return array_value(p_raw, p_oid);
		}
		default:
			break;
	}
	return p_raw;
}

// Decode the two server output forms of a binary SQL value into its original octets.
bool bytea_value(const uint8_t *p_data, int p_len, PackedByteArray &r_value) {
	auto hex = [](uint8_t p_ch) -> int {
		if (p_ch >= '0' && p_ch <= '9') return p_ch - '0';
		if (p_ch >= 'a' && p_ch <= 'f') return p_ch - 'a' + 10;
		if (p_ch >= 'A' && p_ch <= 'F') return p_ch - 'A' + 10;
		return -1;
	};
	if (p_len >= 2 && p_data[0] == '\\' && p_data[1] == 'x') {
		if ((p_len - 2) % 2 != 0) return false;
		for (int i = 2; i < p_len; i += 2) {
			const int hi = hex(p_data[i]);
			const int lo = hex(p_data[i + 1]);
			if (hi < 0 || lo < 0) return false;
			r_value.push_back((hi << 4) | lo);
		}
		return true;
	}
	for (int i = 0; i < p_len; i++) {
		if (p_data[i] != '\\') {
			r_value.push_back(p_data[i]);
			continue;
		}
		if (++i >= p_len) return false;
		if (p_data[i] == '\\') {
			r_value.push_back('\\');
			continue;
		}
		if (i + 2 >= p_len || p_data[i] < '0' || p_data[i] > '3' ||
				p_data[i + 1] < '0' || p_data[i + 1] > '7' || p_data[i + 2] < '0' || p_data[i + 2] > '7') return false;
		r_value.push_back(((p_data[i] - '0') << 6) | ((p_data[i + 1] - '0') << 3) | (p_data[i + 2] - '0'));
		i += 2;
	}
	return true;
}

// Append a text-format bind value directly to the send buffer.
void put_param(ByteBuf &r_out, const Variant &p_v) {
	if (p_v.get_type() == Variant::NIL) {
		put32(r_out, -1);
		return;
	}
	const uint32_t len_at = r_out.size();
	put32(r_out, 0);
	const uint32_t value_at = r_out.size();
	switch (p_v.get_type()) {
		case Variant::BOOL:
			r_out.push_back((bool)p_v ? 't' : 'f');
			break;
		case Variant::INT:
			push_digits(r_out, int64_t(p_v));
			break;
		case Variant::PACKED_BYTE_ARRAY: {
			// Encode bytea using hexadecimal notation.
			const PackedByteArray raw = p_v;
			static const char hex[] = "0123456789abcdef"; // Map each byte to two hexadecimal digits.
			r_out.push_back('\\');
			r_out.push_back('x');
			for (int i = 0; i < raw.size(); i++) {
				r_out.push_back(hex[raw[i] >> 4]);
				r_out.push_back(hex[raw[i] & 15]);
			}
			break;
		}
		case Variant::PACKED_INT32_ARRAY: {
			put_nums(r_out, PackedInt32Array(p_v));
			break;
		}
		case Variant::PACKED_INT64_ARRAY: {
			put_nums(r_out, PackedInt64Array(p_v));
			break;
		}
		default: {
			const CharString text = Pool::text(p_v).utf8();
			put_raw(r_out, reinterpret_cast<const uint8_t *>(text.get_data()), text.length());
			break;
		}
	}
	set32(r_out, len_at, int32_t(r_out.size() - value_at));
}

// Saturate conservative estimates one past the implementation's representation boundary.
void add_bound(int64_t &r_bytes, int64_t p_bytes) {
	if (r_bytes > PACK_BYTES_MAX || p_bytes > PACK_BYTES_MAX - r_bytes) {
		r_bytes = int64_t(PACK_BYTES_MAX) + 1;
		return;
	}
	r_bytes += p_bytes;
}

// Estimate text-format bind bytes before sending.
int64_t param_bound(const Variant &p_v, int p_depth = 0) {
	switch (p_v.get_type()) {
		case Variant::NIL:
			return 0;
		case Variant::BOOL:
			return 1;
		case Variant::INT:
			return 20;
		case Variant::FLOAT:
			return 64;
		case Variant::OBJECT:
			return 64; // Invoke callbacks only when finalizing wire values, not during estimation.
		case Variant::STRING:
		case Variant::STRING_NAME:
			return int64_t(Pool::text(p_v).length()) * (p_depth > 0 ? 8 : 4) + 2;
		case Variant::PACKED_BYTE_ARRAY:
			return int64_t(PackedByteArray(p_v).size()) * (p_depth > 0 ? 6 : 2) + 32;
		case Variant::PACKED_INT32_ARRAY:
			return int64_t(PackedInt32Array(p_v).size()) * (p_depth > 0 ? 13 : 12) + 32;
		case Variant::PACKED_INT64_ARRAY:
			return int64_t(PackedInt64Array(p_v).size()) * (p_depth > 0 ? 22 : 21) + 32;
		case Variant::ARRAY: {
			const Array values = p_v;
			if (p_depth >= PARAM_DEPTH_MAX) {
				return -1;
			}
			int64_t bytes = 2;
			for (const Variant &value : values) {
				const int64_t child = param_bound(value, p_depth + 1);
				if (child < 0) {
					return -1;
				}
				add_bound(bytes, child + 2);
			}
			return bytes;
		}
		case Variant::DICTIONARY: {
			const Dictionary values = p_v;
			if (p_depth >= PARAM_DEPTH_MAX) {
				return -1;
			}
			int64_t bytes = 2;
			for (const Variant &key : values.keys()) {
				const int64_t key_bytes = param_bound(key, p_depth + 1);
				const int64_t value_bytes = param_bound(values[key], p_depth + 1);
				if (key_bytes < 0 || value_bytes < 0) {
					return -1;
				}
				add_bound(bytes, key_bytes);
				add_bound(bytes, value_bytes + 4);
			}
			return bytes;
		}
		default:
			return -1;
	}
}

template <typename T>
// Return the actual text-format bytes of a numeric array.
int64_t nums_bytes(const T &p_nums) {
	int64_t bytes = 2 + MAX(0, p_nums.size() - 1);
	for (int i = 0; i < p_nums.size(); i++) {
		bytes += String::num_int64(p_nums[i]).length();
	}
	return bytes;
}

// Return the actual wire-byte count of a bind value.
int64_t param_bytes(const Variant &p_v) {
	switch (p_v.get_type()) {
		case Variant::NIL:
			return 0;
		case Variant::BOOL:
			return 1;
		case Variant::INT:
			return String::num_int64(int64_t(p_v)).length();
		case Variant::PACKED_BYTE_ARRAY:
			return int64_t(PackedByteArray(p_v).size()) * 2 + 2;
		case Variant::PACKED_INT32_ARRAY:
			return nums_bytes(PackedInt32Array(p_v));
		case Variant::PACKED_INT64_ARRAY:
			return nums_bytes(PackedInt64Array(p_v));
		default:
			return utf8_bytes(Pool::text(p_v));
	}
}

// Estimate query encoding work and retained queue bytes together.
struct QuerySize {
	int64_t work = 0; // Work needed to encode the query itself.
	int64_t queued = 0; // Conservative retained bytes including statement-cache eviction.
};

// Return encoding work and queue reservation for one query.
QuerySize query_size(const String &p_sql, const Array &p_rows) {
	QuerySize out;
	out.work = int64_t(p_sql.length()) * 4 + 128;
	for (const Variant &row : p_rows) {
		add_bound(out.work, 64);
		if (row.get_type() != Variant::ARRAY) {
			continue;
		}
		const Array args = row;
		add_bound(out.work, int64_t(args.size()) * 4);
		for (const Variant &arg : args) {
			const int64_t bound = param_bound(arg);
			add_bound(out.work, bound < 0 ? 64 : bound);
		}
	}
	out.queued = out.work;
	add_bound(out.queued, STMT_CLOSE_BOUND);
	return out;
}

// Find values requiring Object string-conversion callbacks without revisiting cycles.
bool has_object(const Variant &p_value) {
	if (p_value.get_type() == Variant::OBJECT) {
		return true;
	}
	if (p_value.get_type() != Variant::ARRAY && p_value.get_type() != Variant::DICTIONARY) {
		return false;
	}
	HashSet<const void *> seen;
	LocalVector<Variant> todo;
	todo.push_back(p_value);
	while (!todo.is_empty()) {
		const Variant value = todo[todo.size() - 1];
		todo.remove_at(todo.size() - 1);
		if (value.get_type() == Variant::OBJECT) {
			return true;
		}
		if (value.get_type() == Variant::ARRAY) {
			const Array array = value;
			if (seen.has(array.id())) {
				continue;
			}
			seen.insert(array.id());
			for (const Variant &item : array) {
				todo.push_back(item);
			}
		} else if (value.get_type() == Variant::DICTIONARY) {
			const Dictionary dict = value;
			if (seen.has(dict.id())) {
				continue;
			}
			seen.insert(dict.id());
			for (const Variant &key : dict.keys()) {
				todo.push_back(key);
				todo.push_back(dict[key]);
			}
		}
	}
	return false;
}

// Snapshot argument rows to isolate them from caller mutations.
Array snapshot_rows(const Array &p_rows) {
	Array out;
	out.resize(p_rows.size());
	for (int r = 0; r < p_rows.size(); r++) {
		if (p_rows[r].get_type() != Variant::ARRAY) {
			out[r] = p_rows[r];
			continue;
		}
		const Array args = p_rows[r];
		bool deep = false;
		for (const Variant &arg : args) {
			if (arg.get_type() >= Variant::DICTIONARY) { // Dictionaries, arrays, and packed arrays are mutable.
				deep = true;
				break;
			}
		}
		out[r] = args.duplicate(deep);
	}
	return out;
}

} // namespace

// ---------------- Query operations ----------------

// Schedule connection processing once on the ready queue.
void GDPostgresCallInternal::schedule() {
	if (step_posted || self_hold.is_null()) {
		return;
	}
	step_posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDPostgresCallInternal::dispatch).bind(generation));
}

// Reject notifications queued before the operation was reused.
void GDPostgresCallInternal::dispatch(uint64_t p_generation) {
	if (p_generation == generation) {
		step();
	}
}

// Register query deadlines with shared timers; zero waits until cancellation.
void GDPostgresCallInternal::set_due(uint64_t p_wait) {
	Async::drop_deadline(this, due);
	due = deadline_after(p_wait);
	Async::track_deadline(this, due, callable_mp(this, &GDPostgresCallInternal::step));
}

// Deliver asynchronous failures on the next event-loop turn.
void GDPostgresCallInternal::fail_later(const VariantPair &p_out) {
	if (self_hold.is_null()) {
		return; // Do not reschedule an already completed operation.
	}
	if (Pool::is_stopping(true)) {
		cancel_deferred = false;
		cancel_finished = false;
		done(p_out); // Release references immediately during shutdown when no further runtime turn remains.
		return;
	}
	if (dropped && cancel_deferred) {
		cancel_finished = true; // Do not reuse the operation before cancellation is delivered.
		return;
	}
	if (dropped && notified) {
		done(p_out); // Clean up deadlines and self-references after a canceled operation leaves the queue.
		return;
	}
	pending = p_out;
	pending_ready = true;
	if (!pending_posted) {
		pending_posted = true;
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDPostgresCallInternal::deliver_pending).bind(generation));
	}
}

// Advance only the operation generation that queued the pending result.
void GDPostgresCallInternal::deliver_pending(uint64_t p_generation) {
	if (!pending_posted || p_generation != generation) {
		return;
	}
	pending_posted = false;
	step();
}

// Advance the asynchronous operation by one state.
void GDPostgresCallInternal::step() {
	step_posted = false;
	Ref<GDPostgresCallInternal> keep(this); // Remain alive even when close removes this object from the reserve.
	if (self_hold.is_null()) {
		return; // Do not advance completed objects from stale completion entries.
	}
	// Close sockets and watched descriptors on connection or authentication failure.
	auto stop = [&](const VariantPair &p_out) {
		Ref<GDPostgresClient> owner = db;
		if (owner.is_valid()) {
			if (owner->opening.ptr() == this) {
				owner->opening.unref();
			}
			owner->close();
		}
		done(p_out);
	};
	if (pending_ready) {
		pending_posted = false;
		const VariantPair out = pending;
		pending = VariantPair();
		pending_ready = false;
		if (dropped) {
			// Notify cancellation but retain queue position because the reply is still expected.
			notify(out);
			if (mode != QUERYING && db.is_valid()) {
				db->close(); // Before connection setup there is no reply order to preserve; release resources immediately.
			}
			return;
		}
		done(out);
		return;
	}
	if (due > 0 && GDClock::msec() >= due) {
		const VariantPair expired = pg_fail("postgres did not answer in time", Err::TIMED_OUT);
		if (shared && mode == QUERYING) interrupt(expired);
		else stop(expired);
		return;
	}
	if (mode == RESOLVING) {
		return; // Wait for worker-based name resolution.
	}
	if (mode == CONNECTING) {
		db->sock.poll();
		const Wire::State st = db->sock.state();
		if (st == Wire::FAILED || st == Wire::CLOSED) {
			// Propagate the transport cause without reclassifying encrypted connections.
			stop(pg_fail(sock_error(db->sock, "connection closed")));
			return;
		}
		if (st != Wire::READY) {
			return; // Wait for connection establishment or the active handshake.
		}
		// Request TLS on the plain remote SQL connection, then begin the handshake
		// on that same connection only after an S response. Reject N instead of
		// silently continuing in plaintext, since an intermediary could otherwise
		// disable encryption by substituting that response.
		if (db->tls_at == GDPostgresClient::TLS_ASK) {
			put32(db->out_buf, 8); // SSLRequest message length.
			put32(db->out_buf, 80877103); // SSLRequest protocol code.
			if (!sock_flush(db->sock, db->out_buf)) {
				stop(pg_fail(sock_error(db->sock, "tls request send failed")));
				return;
			}
			db->tls_at = GDPostgresClient::TLS_WAIT;
			return; // Resume any incomplete request write before TLS_WAIT on a later turn.
		}
		if (db->tls_at == GDPostgresClient::TLS_WAIT) {
			if (!db->out_buf.is_empty()) {
				if (!sock_flush(db->sock, db->out_buf)) {
					stop(pg_fail(sock_error(db->sock, "tls request send failed")));
				}
				return; // Finish sending the request before reading the response.
			}
			if (db->sock.available() < 1) {
				return; // Wait for the peer's response byte.
			}
			uint8_t answer = 0;
			int got = 0;
			const Error read = db->sock.read(&answer, 1, got);
			if (read == ERR_BUSY) return;
			if (read != OK || got != 1) {
				stop(pg_fail(sock_error(db->sock, "tls request read failed")));
				return;
			}
			if (answer != 'S') {
				stop(pg_fail(vformat("the server refused tls (answered '%c')", (char)answer), Err::PERMISSION_DENIED));
				return;
			}
			db->tls_at = GDPostgresClient::TLS_ON;
			if (db->sock.wrap(db->host, db->guard, db->ca) != OK) {
				stop(pg_fail(sock_error(db->sock, "TLS setup failed", Err::NONE)));
			}
			return; // Continue the handshake on the next turn.
		}
		db->buf = PackedByteArray();
		db->buf_at = 0;
		// Send the startup message without a message-type byte.
		ByteBuf body;
		put32(body, PROTO_V3);
		put_cstr(body, "user");
		put_cstr(body, opts.get("user", "postgres"));
		put_cstr(body, "database");
		put_cstr(body, opts.get("database", "postgres"));
		put_cstr(body, "client_encoding");
		put_cstr(body, "UTF8");
		body.push_back(0); // Terminate the parameter sequence.
		if (db->send(String(), body) != OK) {
			stop(pg_fail("startup send failed", Err::INVALID_DATA));
			return;
		}
		mode = HANDSHAKE;
		return;
	}

	const Ref<Err> got = db->fill(PG_READ_CHUNK);
	if (got.is_valid()) {
		stop(pg_fail(got));
		return;
	}
	char kind = 0;
	int at = 0;
	int len = 0;
	while (db->next_msg(kind, at, len)) {
		const int message_bytes = len + 4;
		if (message_bytes > SEND_BYTES_MAX) {
			stop(pg_fail("postgres authentication message exceeds the protocol maximum", Err::LIMITED));
			return;
		}
		Ref<GDPostgresClient> owner = db;
		if (kind == 'S') {
			const Ref<Err> error = status_error(db->buf.ptr() + at, len);
			if (error.is_valid()) {
				stop(pg_fail(error));
				return;
			}
		}
		if (take(kind, db->buf.ptr() + at, len)) {
			if (owner.is_valid() && !owner->ready) {
				owner->close(); // Release sockets and watched descriptors on authentication failure.
			}
			return;
		}
	}
	const int64_t waiting_bytes = db->next_msg_size();
	if (waiting_bytes < 0 || waiting_bytes > int64_t(SEND_BYTES_MAX) + 1) {
		stop(pg_fail("postgres authentication message exceeds the protocol maximum", Err::LIMITED));
	} else if (db->sock.available() > 0) {
		schedule(); // Continue ready input beyond this read chunk in the next time slice.
	}
}

// Deliver a query result once to its direct waiter or pool operation.
void GDPostgresCallInternal::notify(const VariantPair &p_out) {
	if (notified) {
		return;
	}
	notified = true;
	if (pool_call) {
		Ref<GDPostgresPoolCall> target(pool_call);
		target->answered(p_out);
		return;
	}
	const Ref<Err> error = pg_result_error(p_out);
	Async::finish(this, SNAME("finished"), p_out.value, error.is_valid() ? Variant(error->with_partial(p_out.value)) : Variant());
}

// Receive worker-resolved addresses and proceed to socket connection.
void GDPostgresClient::resolved(const Variant &p_value, const Ref<Err> &p_error, const Ref<GDPostgresCallInternal> &p_call) {
	if (p_call.is_null() || opening.ptr() != p_call.ptr() || p_call->self_hold.is_null() || p_call->db.ptr() != this || p_call->mode != GDPostgresCallInternal::RESOLVING) {
		return;
	}
	if (Pool::is_stopping()) {
		p_call->fail_later(pg_fail("worker pool stopped", Err::INTERRUPTED));
		return;
	}
	if (p_error.is_valid()) {
		p_call->fail_later(pg_fail(p_error));
		return;
	}
	const Dictionary prepared = p_value;
	const String addr = prepared.get("address", "");
	ca = prepared.get("ca", Variant());
	if (addr.is_empty()) {
		p_call->fail_later(pg_fail(vformat("cannot resolve \"%s\"", host), Err::NOT_FOUND));
		return;
	}
	p_call->mode = GDPostgresCallInternal::CONNECTING;
	if (sock.open(addr, port, p_call->due) != OK) {
		p_call->fail_later(pg_fail(vformat("cannot reach %s:%d", host, port), Err::NOT_FOUND));
	} else {
		p_call->schedule();
	}
}

// Process one authentication or query-result message.
bool GDPostgresCallInternal::take(char p_kind, const uint8_t *p_body, int p_len) {
	if (mode == HANDSHAKE) {
		if (p_kind == 'R') { // Authentication request.
			if (p_len < 4) {
				done(pg_fail("short auth message", Err::INVALID_DATA));
				return true; // Do not read an authentication code without four bytes.
			}
			const int code = read32p(p_body, 0);
			if (db->auth_ok || (db->scram_pending && code != 11 && code != 12) || (db->scram_done && code != 0)) {
				done(pg_fail("unexpected authentication message", Err::PERMISSION_DENIED));
				return true;
			}
			Ref<Err> step_out;
			switch (code) {
				case 0: // Authentication success.
					if (p_len != 4) {
						done(pg_fail("invalid authentication success message", Err::INVALID_DATA));
						return true;
					}
					if (String(opts.get("auth", "any")) == "scram" && !db->scram_done) {
						done(pg_fail("server did not use SCRAM authentication", Err::PERMISSION_DENIED));
						return true;
					}
					if (String(opts.get("auth", "any")) == "md5" && !db->md5_done) {
						done(pg_fail("server did not use MD5 authentication", Err::PERMISSION_DENIED));
						return true;
					}
					db->auth_ok = true;
					return false;
				case 3: { // Cleartext password request.
					// Sending this response over a plain connection exposes the password.
					// A malicious peer could request it directly, so allow this authentication
					// mode only when the caller explicitly enables it.
					const String required = opts.get("auth", "any");
					if (required == "scram") {
						done(pg_fail("server did not use SCRAM authentication", Err::PERMISSION_DENIED));
						return true;
					}
					if (required == "md5") {
						done(pg_fail("server did not use MD5 authentication", Err::PERMISSION_DENIED));
						return true;
					}
					if (!(bool)opts.get("allow_cleartext_password", false)) {
						done(pg_fail("server asked for a cleartext password; pass allow_cleartext_password=true to permit it", Err::PERMISSION_DENIED));
						return true;
					}
					ByteBuf pass;
					put_cstr(pass, opts.get("password", ""));
					db->send("p", pass);
					return false;
				}
				case 5: { // MD5 password authentication.
					if (p_len != 8) {
						done(pg_fail("invalid MD5 authentication message", Err::INVALID_DATA));
						return true;
					}
					if (String(opts.get("auth", "any")) == "scram") {
						done(pg_fail("server did not use SCRAM authentication", Err::PERMISSION_DENIED));
						return true;
					}
					const VariantPair password = md5_password(opts.get("password", ""), opts.get("user", "postgres"), p_body + 4);
					if (password.error.get_type() != Variant::NIL) {
						done(pg_fail(password.error)); // Do not send authentication data after digest failure; return to connection cleanup.
						return true;
					}
					ByteBuf pass;
					put_cstr(pass, password.value);
					db->send("p", pass);
					db->md5_done = true;
					return false;
				}
				case 10: // Start SASL authentication.
					if (String(opts.get("auth", "any")) == "md5") {
						done(pg_fail("server did not use MD5 authentication", Err::PERMISSION_DENIED));
						return true;
					}
					db->scram_pending = true;
					step_out = db->sasl_begin(_slice(p_body, p_len));
					break;
				case 11: // Continue SASL authentication.
					step_out = db->sasl_continue(_slice(p_body, p_len), opts.get("password", ""));
					break;
				case 12: // Finish SASL authentication and verify the signature.
					step_out = db->sasl_final(_slice(p_body, p_len));
					if (step_out.is_null()) {
						db->scram_pending = false; // Server proof has been verified.
						db->scram_done = true;
					}
					break;
				default:
					done(pg_fail(vformat("auth method %d is not supported", code), Err::UNSUPPORTED));
					return true;
			}
			if (step_out.is_valid()) {
				done(pg_fail(step_out));
				return true;
			}
			return false;
		}
		if (p_kind == 'E') {
			done(pg_fail(pg_error(p_body, p_len).msg, Err::PERMISSION_DENIED));
			return true;
		}
		if (p_kind == 'Z') { // ReadyForQuery.
			if (!db->auth_ok) {
				done(pg_fail("server skipped authentication completion", Err::PERMISSION_DENIED));
				return true;
			}
			if (!db->ready_state(p_body, p_len)) {
				done(pg_fail("invalid postgres transaction state", Err::INVALID_DATA));
				return true;
			}
			db->ready = true;
			done(pg_ok());
			return true;
		}
		return false; // Skip parameter-status and backend-key messages.
	}
	// Count completions before writing results so surplus replies cannot index beyond a batch.
	if (p_kind == 'C' && want_n > 0) {
		if (done_n >= want_n) {
			if (done_n == want_n) {
				const Ref<Err> extra = Err::make("too many query completions", Err::INVALID_DATA);
				if (failed.is_valid()) extra_failures.push_back(extra);
				else failed = extra;
			}
			done_n++;
			return false; // Drain the remaining synchronization boundaries before reporting failure.
		}
		done_n++;
	}
	// Count regular batch replies directly when individual results are discarded.
	if (discard_many && p_kind == '2') { // BindComplete
		return false;
	}
	if (discard_many && p_kind == 'C') { // CommandComplete
		return false;
	}
	if (p_kind == '1') { // ParseComplete validates a newly cached statement.
		parse_pending = false;
		if (db.is_valid()) db->stmt_pending.erase(stmt);
		return false;
	}

	// Drain canceled rows and command tags without allocating results, but retain metadata and Sync boundaries.
	if (dropped && (p_kind == 'D' || p_kind == 'C')) return false;
	// Collect query results.
	if (p_kind == 'T') { // Row description.
		db->torn = String();
		columns = db->row_desc(p_body, p_len);
		keys.resize(columns.size());
		for (uint32_t i = 0; i < keys.size(); i++) {
			keys[i] = StringName(columns[i]); // Hash each column name once rather than once per row.
		}
		if (stream) {
			stream->postgres_columns(columns);
		}
		if (!db->torn.is_empty()) {
			const Ref<Err> error = Err::make(db->torn, Err::INVALID_DATA);
			if (failed.is_valid()) extra_failures.push_back(error);
			else failed = error;
			statement_failed = true;
		}
		if (!db->torn.is_empty()) return false; // Never cache malformed metadata for later queries.
		oids = db->oids;
		const String *live_stmt = db.is_valid() ? db->stmts.getptr(sql) : nullptr;
		if (live_stmt && *live_stmt == stmt) {
			db->cols[sql] = columns; // Reuse column metadata without another description request.
			db->key_memo[sql] = keys;
			db->oid_memo[sql] = oids;
			// Request binary formats for supported columns on later executions.
			LocalVector<uint8_t> want;
			want.resize(oids.size());
			for (uint32_t i = 0; i < oids.size(); i++) {
				want[i] = binary_ok(oids[i]) ? 1 : 0;
			}
			db->fmt_memo[sql] = want;
		}
	} else if (p_kind == 'n') { // Statement description with no result columns.
		const String *live_stmt = db.is_valid() ? db->stmts.getptr(sql) : nullptr;
		if (live_stmt && *live_stmt == stmt) {
			db->cols[sql] = PackedStringArray(); // Skip Describe on later executions.
			db->key_memo[sql] = LocalVector<StringName>();
			db->oid_memo[sql] = LocalVector<int32_t>();
			db->fmt_memo[sql] = LocalVector<uint8_t>();
		}
	} else if (p_kind == 'D') { // One result row.
		if (discard_many) {
			return false; // Do not construct rows for completion-count-only operations.
		}
		if (stream_mode) {
			if (!dropped && failed.is_null() && stream) {
				db->torn = String();
				const Array row = db->data_row(p_body, p_len, keys, oids, fmts, true);
				if (db->torn.is_empty()) {
					result_rows++;
					stream_paused = true;
					db->sock.read_wait(false);
					stream->postgres_row(row);
				} else if (failed.is_null()) {
					failed = Err::make(db->torn, Err::INVALID_DATA);
				}
			}
			return false;
		}
		if (first_only && result_rows > 0) {
			return false; // Drain remaining rows to the protocol boundary without constructing values.
		}
		if (!statement_failed && ((!first_only && max_rows > 0 && result_rows >= max_rows) || (max_bytes > 0 && result_bytes + p_len > max_bytes))) {
			const Ref<Err> error = Err::make("postgres result exceeds the limit", Err::LIMITED);
			if (failed.is_valid()) extra_failures.push_back(error);
			else failed = error;
			statement_failed = true;
		} else if (!statement_failed) {
			db->torn = String();
			Variant row;
			if (flat_values) {
				db->data_row(p_body, p_len, keys, oids, fmts, true, &values);
			} else {
				row = db->data_row(p_body, p_len, keys, oids, fmts, values_only);
			}
			if (flat_values) {
				// data_row appends values directly to the contiguous destination.
			} else if (flat_many) {
				batches.push_back(row);
			} else {
				rows.push_back(row);
			}
			result_bytes += p_len;
			result_rows++;
			if (!db->torn.is_empty()) {
				const Ref<Err> error = Err::make(db->torn, Err::INVALID_DATA);
				if (failed.is_valid()) extra_failures.push_back(error);
				else failed = error;
				statement_failed = true;
			}
		}
	} else if (p_kind == 'C') { // Command completion.
		if (want_n > 0) {
			// Complete one query within the batch.
			if (!discard_many && !flat_many) {
				int next = 0;
				tag = read_cstrp(p_body, p_len, 0, next);
				Dictionary one;
				one["columns"] = columns;
				one["rows"] = rows;
				one["tag"] = tag;
				batches[sync_n] = one;
			}
			rows = Array();
		} else {
			int next = 0;
			tag = read_cstrp(p_body, p_len, 0, next);
		}
	} else if (p_kind == 'E') {
		const PgError error = pg_error(p_body, p_len);
		const Ref<Err> reason = query_error(error);
		if (failed.is_valid()) extra_failures.push_back(reason);
		else failed = reason;
		statement_failed = true;
		// Discard a statement whose Parse failed or whose server state was invalidated.
		// Keep a successfully parsed statement after an execution error.
		if (db.is_valid() && !sql.is_empty() && (parse_pending || error.code == "0A000" || error.code == "26000")) {
			const String *current = db->stmts.getptr(sql);
			// An older pending error must not invalidate a replacement prepared by another caller.
			if (current && *current == stmt) db->forget(sql, !parse_pending && error.code == "0A000");
		}
	} else if (p_kind == 'Z') { // Ready to accept another query.
		// Defer notification to GDPostgresClient::pump after queue removal.
		// Otherwise the callback would still count this completed operation as waiting.
		statement_failed = false;
		if (want_n > 0 && ++sync_n < want_n) {
			// Drain every remaining individual boundary even after an intermediate error.
			// Removing the operation early would assign its remaining replies to the next call.
			return false;
		}
		if (temporary_stmt && !parse_pending && db.is_valid()) db->close_stmt(stmt);
		if (dropped) return true;
		outcome_ready = true;
		if (failed.is_valid()) {
			// Keep completed batch work alongside the first failure and later causes.
			const Variant partial = want_n > 0 ? (discard_many ? Variant(done_n) : flat_values ? Variant(values) : Variant(batches)) : Variant();
			outcome = pg_fail(all_failures(), Err::NONE, partial);
			return true;
		}
		if (want_n > 0) {
			// Reject mismatches between requested and returned query counts.
			// Otherwise scripts iterating by expected count could silently process incomplete results.
			if (done_n != want_n) {
				outcome = pg_fail(vformat("asked %d, got %d", want_n, done_n), Err::INVALID_DATA);
				return true;
			}
			outcome = discard_many ? pg_ok(done_n) : (flat_values ? pg_ok(values) : pg_ok(batches));
			return true;
		}
		if (first_only) {
			outcome = rows.is_empty() ? pg_fail("database query returned no rows", Err::NOT_FOUND) : pg_ok(rows[0]);
			return true;
		}
		Dictionary out;
		out["columns"] = columns;
		if (flat_values) {
			out["values"] = values;
			out["row_count"] = result_rows;
		} else {
			out["rows"] = rows;
		}
		out["tag"] = tag;
		outcome = pg_ok(out);
		return true;
	}
	return false;
}

// Combine each independent SQL failure only once when the batch completes.
Ref<Err> GDPostgresCallInternal::all_failures() const {
	if (extra_failures.is_empty()) return failed;
	Array errors;
	errors.push_back(failed);
	for (const Ref<Err> &error : extra_failures) errors.push_back(error);
	return Err::join(errors);
}

// Emit a result after removing its operation from the queue.
void GDPostgresCallInternal::finish() {
	if (stream_mode && stream) {
		stream->postgres_done(all_failures(), tag);
	}
	done(outcome_ready ? outcome : pg_ok());
}

// Reset private operation state after completion.
void GDPostgresCallInternal::reset() {
	Async::drop_deadline(this, due);
	mode = QUERYING;
	due = 0;
	pending = VariantPair();
	outcome = VariantPair();
	pending_ready = false;
	outcome_ready = false;
	opts = Dictionary();
	columns = PackedStringArray();
	keys.clear();
	oids.clear();
	sql = String();
	stmt = String();
	parse_pending = false;
	temporary_stmt = false;
	// Replace result arrays instead of clearing them because scripts may retain
	// the shared backing storage of previously delivered results.
	rows = Array();
	batches = Array();
	tag = String();
	max_rows = 0;
	max_bytes = 0;
	result_bytes = 0;
	result_rows = 0;
	want_n = 0;
	done_n = 0;
	sync_n = 0;
	statement_failed = false;
	discard_many = false;
	flat_many = false;
	values_only = false;
	flat_values = false;
	first_only = false;
	stream = nullptr;
	stream_mode = false;
	stream_paused = false;
	cancel_deferred = false;
	cancel_finished = false;
	pending_posted = false;
	step_posted = false;
	generation++;
	pool_call = nullptr;
	values = Array();
	fmts.clear();
	failed.unref(); // Do not retain the previous error.
	extra_failures.clear();
	dropped = false;
	shared = false;
	notified = false;
}

// Finish the operation and deliver its result to the waiter.
void GDPostgresCallInternal::done(const VariantPair &p_out) {
	if (self_hold.is_null()) {
		return;
	}
	if (dropped && cancel_deferred) {
		cancel_finished = true; // Retain the object so deferred delivery cannot touch stale or reused state.
		return;
	}
	// Retain this object before releasing self_hold, which may own its final
	// reference while subsequent cleanup still needs to access the object.
	Ref<GDPostgresCallInternal> keep(this);
	Ref<GDPostgresClient> owner = db;
	if (stream_mode && stream) {
		stream->postgres_done(pg_result_error(p_out), tag);
	}
	if (owner.is_valid() && owner->opening.ptr() == this) {
		owner->opening.unref();
	}
	Async::drop_deadline(this, due);
	due = 0;
	Ref<GDPostgresPoolCall> target = pool_call ? Ref<GDPostgresPoolCall>(pool_call) : Ref<GDPostgresPoolCall>();
	if (!notified && !dropped) {
		notify(p_out);
	} else if (!notified && pending_ready) {
		// Deliver any pending cancellation before returning the operation for reuse.
		// A reply may arrive before scheduled cancellation and otherwise recycle
		// the operation without notification, leaving its waiter suspended forever.
		const VariantPair late = pending;
		pending = VariantPair();
		pending_ready = false;
		notify(late);
	}
	// Return pool connections only after the protocol boundary; notify before caching
	// the operation to prevent its reuse within the callback.
	if (target.is_valid()) {
		target->settled();
	} else {
		emit_signal("settled");
	}
	pool_call = nullptr;
	GDDatabaseRows *rows_owner = stream;
	stream = nullptr;
	if (rows_owner) {
		rows_owner->postgres_detach(this);
	}
	self_hold.unref();
	// Reuse only private pool operations so public signals retain their original request identity.
	if (owner.is_valid() && target.is_valid()) {
		owner->give_back(this);
	}
}

// Resume receive parsing when Rows requests its next row.
void GDPostgresCallInternal::resume_stream() {
	if (!stream_mode || dropped || self_hold.is_null()) {
		return;
	}
	stream_paused = false;
	if (db.is_valid()) {
		db->sock.read_wait(true);
		db->watch(true);
		db->post_pump(); // Resume buffered rows without waiting for another kernel notification.
	}
}

// Drain to the protocol boundary without building values after early Rows closure.
void GDPostgresCallInternal::close_stream() {
	if (!stream_mode || self_hold.is_null() || dropped) {
		return;
	}
	dropped = true;
	notified = true; // Rows reports its own completion; no internal finished waiter remains.
	stream_paused = false;
	if (db.is_valid()) {
		db->sock.read_wait(true);
		db->watch(true);
		db->post_pump(); // Drain buffered ReadyForQuery before returning the connection to its pool.
	}
}

// Deliver cancellation after the waiter attaches to the signal.
void GDPostgresCallInternal::deliver_cancel(uint64_t p_generation) {
	Ref<GDPostgresCallInternal> keep(this);
	if (!cancel_deferred || p_generation != generation) {
		return; // Ignore already-notified or reused operations.
	}
	cancel_deferred = false;
	if (self_hold.is_null() || !dropped) {
		return;
	}
	const VariantPair out = pending_ready ? pending : pg_fail("cancelled", Err::INTERRUPTED);
	notify(out);
	if (cancel_finished) {
		cancel_finished = false;
		done(out); // Clean up deadlines, self-references, and pooled operations if the query also ended.
	}
}

// Notify one interrupted caller and drain its wire position without disrupting other queries.
void GDPostgresCallInternal::interrupt(const VariantPair &p_out) {
	if (self_hold.is_null() || dropped) {
		return; // Ignore an already completed operation.
	}
	dropped = true;
	if (shared) {
		Async::drop_deadline(this, due);
		due = 0;
	}
	rows = Array();
	values = Array();
	batches = Array();
	// Notify on the next turn so the caller has time to attach its waiter.
	pending = p_out;
	pending_ready = true;
	cancel_deferred = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDPostgresCallInternal::deliver_cancel).bind(generation));
}

// Cancel this waiter while preserving any already-sent query's response boundary.
void GDPostgresCallInternal::cancel() {
	interrupt(pg_fail("cancelled", Err::INTERRUPTED));
}

// Discard an abandoned connection when no caller remains to wait for synchronization.
void GDPostgresCallInternal::abort() {
	if (db.is_valid()) db->close();
	else cancel();
}

// Register public methods and properties with script.
void GDPostgresCallInternal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDPostgresCallInternal::cancel);
	ClassDB::bind_method(D_METHOD("abort"), &GDPostgresCallInternal::abort);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
	ADD_SIGNAL(MethodInfo("settled"));
}

// ---------------- Connections ----------------

bool GDPostgresClient::is_open() const {
	return sock.is_valid() && sock.is_ready() && ready;
}

// Register the connection for shutdown cleanup.
GDPostgresClient::GDPostgresClient() {
	postgres_clients.insert(this);
	sock.set_wait_callback(callable_mp(this, &GDPostgresClient::socket_ready));
}

GDPostgresClient::~GDPostgresClient() {
	postgres_clients.erase(this);
	sock.set_wait_callback(Callable());
}

// Close and release retained resources.
void GDPostgresClient::close() {
	wire_generation++;
	watch(false); // Detach event delivery before the connection can be destroyed.
	// Discard buffered queries without sending after closure is chosen.
	// Sending now could execute a query already reported as failed to its caller.
	out_buf.clear();
	next_buf.clear();
	flush_queued = false;
	packing_bytes = 0;
	if (sock.is_valid()) {
		send("X", ByteBuf());
		sock.close();
	}
	// Notify all waiters instead of silently discarding their unfinished operations.
	drain(pg_fail("connection closed", Err::INTERRUPTED));
	// Discard cached server-side statement names when their connection ends.
	stmts.clear();
	stmt_pending.clear();
	stmt_lru.clear();
	stmt_lru_pos.clear();
	cols.clear();
	key_memo.clear();
	oid_memo.clear();
	fmt_memo.clear();
	out_buf.clear();
	flush_queued = false;
	spare.clear(); // Release reserved operation objects during closure.
	buf = PackedByteArray();
	buf_at = 0;
	ca.unref();
	ready = false;
}

// Detach delivery and notify all waiters with the same failure reason.
// Defer notification until the next turn because closure may follow immediately
// after queueing, before the caller has attached to the signal.
void GDPostgresClient::drain(const VariantPair &p_why) {
	wire_generation++;
	scram_job.unref(); // Ignore authentication work that completes after this connection ends.
	// Only unrecoverable protocol failures enter this path. Close before notification
	// so callbacks returning the connection to a pool cannot make it reusable.
	sock.close();
	ready = false;
	out_buf.clear();
	next_buf.clear();
	flush_queued = false;
	if (opening.is_valid()) {
		Ref<GDPostgresCallInternal> call = opening;
		opening.unref();
		call->fail_later(p_why);
	}
	while (!packing.is_empty()) {
		Ref<GDPostgresPackJob> job = packing.front()->get();
		packing.pop_front();
		if (job->call.is_valid() && job->call->self_hold.is_valid()) {
			job->call->fail_later(p_why);
		}
	}
	packing_bytes = 0;
	while (!inflight.is_empty()) {
		Ref<GDPostgresCallInternal> c = inflight.front()->get();
		inflight.pop_front();
		const Ref<Err> transport = p_why.error;
		c->fail_later({ p_why.value, Err::join(c->all_failures(), transport) });
	}
	stmts.clear();
	stmt_pending.clear();
	stmt_lru.clear();
	stmt_lru_pos.clear();
	cols.clear();
	key_memo.clear();
	oid_memo.clear();
	fmt_memo.clear();
	buf = PackedByteArray();
	buf_at = 0;
	watch(false);
}

// Queue a protocol message for transmission.
// Append to the same outbound buffer as batched queries to preserve wire order.
// Only flush_out sends data, keeping replies aligned with their requests.
Error GDPostgresClient::send(const String &p_kind, const ByteBuf &p_body) {
	if (!p_kind.is_empty()) {
		out_buf.push_back(p_kind.to_utf8_buffer()[0]);
	}
	put32(out_buf, (int32_t)p_body.size() + 4);
	put_raw(out_buf, p_body.ptr(), p_body.size());
	flush_out();
	return sock.is_valid() ? OK : ERR_UNCONFIGURED;
}

// Append socket bytes to the receive buffer.
Ref<Err> GDPostgresClient::fill(int p_max_bytes) {
	return sock_fill(sock, buf, buf_at, p_max_bytes, int64_t(SEND_BYTES_MAX) + 1);
}

// Extract complete messages by buffer position and length without copying bodies.
// This avoids a separate allocation for each protocol message.
// Keep message types as bytes to avoid another string allocation.
bool GDPostgresClient::next_msg(char &r_kind, int &r_at, int &r_len) {
	const int left = buf.size() - buf_at;
	if (left < 5) {
		return false;
	}
	const uint8_t *message = buf.ptr() + buf_at;
	const int32_t length = read32p(message, 1);
	// Compare message sizes in 64 bits: adding one to a peer-supplied INT_MAX
	// in int would wrap and falsely classify incomplete input as a complete message.
	if (length < 4 || (int64_t)left < (int64_t)length + 1) {
		return false; // Leave malformed-length rejection to the waiting operation.
	}
	r_kind = (char)message[0];
	r_at = buf_at + 5;
	r_len = length - 4;
	buf_at += 1 + length;
	return true;
}

// Return the next message's full byte count without advancing the cursor.
int64_t GDPostgresClient::next_msg_size() const {
	const int left = buf.size() - buf_at;
	if (left < 5) {
		return 0;
	}
	const int32_t length = read32p(buf.ptr() + buf_at, 1);
	return length < 4 ? -1 : int64_t(length) + 1;
}

// Check whether another remote SQL message can be decoded without a kernel wait.
bool GDPostgresClient::has_complete_msg() const {
	const int64_t n = next_msg_size();
	return n > 0 && n <= (int64_t)buf.size() - buf_at;
}

// ---------------- SCRAM-SHA-256 ----------------

// Build the initial remote SQL SCRAM authentication message.
Ref<Err> GDPostgresClient::sasl_begin(const PackedByteArray &p_data) {
	PackedStringArray mechs;
	int at = 4; // The first four bytes contain the authentication type.
	while (at < p_data.size() && p_data[at] != 0) {
		int next = 0;
		mechs.push_back(read_cstr(p_data, at, next));
		at = next;
	}
	if (!mechs.has("SCRAM-SHA-256")) {
		return Err::from(vformat("server offers %s, none supported", mechs), Err::UNSUPPORTED);
	}

	const PackedByteArray raw = GDDigest::random(18);
	if (raw.size() != 18) return Err::from("cannot generate SCRAM nonce", Err::INVALID_DATA);
	nonce = Encoding::base64_encode(raw);
	first_bare = vformat("n=,r=%s", nonce);
	const PackedByteArray initial = ("n,," + first_bare).to_utf8_buffer();

	ByteBuf body;
	put_cstr(body, "SCRAM-SHA-256");
	put32(body, initial.size());
	put_raw(body, initial.ptr(), initial.size());
	if (send("p", body) != OK) {
		return Err::from("sasl send failed", Err::INVALID_DATA);
	}
	return Ref<Err>();
}

// Derive authentication keys outside the event loop.
// Run peer-selected iteration work on a CPU worker.
// Keeping this computation off the main thread preserves listener responsiveness.
void ScramKeyJob::run() {
	salted = Hash::pbkdf2_sha256(password, salt, iters);
}

// Build the continuation on the main thread after key derivation.
void ScramKeyJob::finish() {
	Ref<GDPostgresClient> keep = db;
	if (keep.is_valid() && keep->scram_job.ptr() == this) {
		keep->scram_job.unref();
		if (Pool::is_stopping(true)) {
			keep->close(); // Close authentication connections and calls when shutdown cannot await responses.
		} else {
			keep->sasl_derived(salted, combined, server_first);
		}
	}
	db.unref();
	password.clear();
	salt.clear();
	salted = VariantPair();
}

// Build the remote SQL SCRAM authentication proof.
Ref<Err> GDPostgresClient::sasl_continue(const PackedByteArray &p_data, const String &p_password) {
	if (!scram_pending || nonce.is_empty() || !server_sig.is_empty()) {
		return Err::from("unexpected sasl continuation", Err::INVALID_DATA);
	}
	const PackedByteArray tail = p_data.slice(4);
	const String server_first = String::utf8((const char *)tail.ptr(), tail.size());
	Dictionary fields;
	for (const String &part : server_first.split(",")) {
		if (part.length() > 2) {
			fields[part.substr(0, 1)] = part.substr(2);
		}
	}
	if (!(fields.has("r") && fields.has("s") && fields.has("i"))) {
		return Err::from("malformed server-first", Err::INVALID_DATA);
	}
	// Reject empty-password derivation before entering this path.
	// Otherwise empty derived values could compare equal without a valid proof.
	if (p_password.is_empty()) {
		return Err::from("scram needs a password", Err::INVALID_DATA);
	}
	const String combined = fields["r"];
	// Require the server to extend the client nonce rather than merely echo it,
	// as specified by RFC 5802.
	if (!combined.begins_with(nonce) || combined.length() <= nonce.length()) {
		return Err::from("server nonce mismatch", Err::PERMISSION_DENIED);
	}

	const VariantPair decoded = Encoding::base64_decode(fields["s"]);
	if (decoded.error.get_type() != Variant::NIL) return Err::from("invalid SCRAM salt", Err::INVALID_DATA);
	const PackedByteArray salt = decoded.value;
	// Read iteration counts in 64 bits before validating them.
	// Narrowing first could truncate a large value past the lower-bound check.
	const int64_t iters = String(fields["i"]).to_int();
	if (iters < SCRAM_MIN_ITERS || iters > SCRAM_MAX_ITERS) {
		return Err::from(vformat("server asked for %d rounds (need %d..%d)", iters, SCRAM_MIN_ITERS, SCRAM_MAX_ITERS), Err::PERMISSION_DENIED);
	}
	// Reject duplicate derivation requests to prevent one job's result from being
	// combined with another request's authentication inputs.
	if (scram_job.is_valid()) {
		return Err::from("server sent a second sasl continue", Err::INVALID_DATA);
	}
	// Dispatch peer-selected key derivation to a worker to keep listeners responsive.
	// Build the continuation in sasl_derived after the worker completes.
	scram_job.instantiate();
	scram_job->db = Ref<GDPostgresClient>(this);
	scram_job->password = p_password.to_utf8_buffer();
	scram_job->salt = salt;
	scram_job->iters = (int)iters;
	scram_job->combined = combined;
	scram_job->server_first = server_first;
	if (!scram_job->submit(true)) { // Schedule CPU-bound derivation within processor capacity.
		scram_job.unref();
		return Err::from("worker pool stopped", Err::INTERRUPTED);
	}
	return Ref<Err>();
}

// Build the authentication continuation after worker key derivation completes.
void GDPostgresClient::sasl_derived(const VariantPair &p_salted, const String &p_combined, const String &p_server_first) {
	if (p_salted.error.get_type() != Variant::NIL) { drain(pg_fail(p_salted.error)); return; }
	const PackedByteArray salted = p_salted.value;
	const String combined = p_combined;
	const String server_first = p_server_first;
	const PackedByteArray client_key = Hash::hmac_sha256(salted, String("Client Key").to_utf8_buffer());
	const PackedByteArray stored_key = Hash::sha256(client_key);
	// Stop if any derivation stage fails; otherwise an empty expected signature
	// could accept an empty server response as a valid proof.
	if (salted.is_empty() || client_key.is_empty() || stored_key.is_empty()) {
		drain(pg_fail("cannot derive the scram keys", Err::INVALID_DATA));
		return;
	}

	const String final_bare = vformat("c=biws,r=%s", combined);
	const String auth_msg = vformat("%s,%s,%s", first_bare, server_first, final_bare);
	const PackedByteArray client_sig = Hash::hmac_sha256(stored_key, auth_msg.to_utf8_buffer());
	const PackedByteArray proof = Hash::xor_bytes(client_key, client_sig);

	// Compute the expected server signature for final verification.
	const PackedByteArray server_key = Hash::hmac_sha256(salted, String("Server Key").to_utf8_buffer());
	const PackedByteArray want_sig = Hash::hmac_sha256(server_key, auth_msg.to_utf8_buffer());
	if (client_sig.is_empty() || proof.is_empty() || want_sig.is_empty()) {
		drain(pg_fail("cannot derive the scram keys", Err::INVALID_DATA));
		return;
	}
	server_sig = Encoding::base64_encode(want_sig);

	const String out = vformat("%s,p=%s", final_bare, Encoding::base64_encode(proof));
	ByteBuf proof_msg;
	const PackedByteArray proof_raw = out.to_utf8_buffer();
	put_raw(proof_msg, proof_raw.ptr(), proof_raw.size());
	if (send("p", proof_msg) != OK) {
		drain(pg_fail("sasl proof send failed", Err::INVALID_DATA));
	}
}

// Verify the remote SQL SCRAM server signature.
Ref<Err> GDPostgresClient::sasl_final(const PackedByteArray &p_data) {
	if (!scram_pending || scram_job.is_valid() || server_sig.is_empty()) {
		return Err::from("unexpected sasl final", Err::PERMISSION_DENIED);
	}
	const PackedByteArray tail = p_data.slice(4);
	const String msg = String::utf8((const char *)tail.ptr(), tail.size());
	for (const String &part : msg.split(",")) {
		if (part.begins_with("v=")) {
			// Compare in constant time to avoid revealing matching signature prefixes.
			if (!Hash::equal_ct(part.substr(2).to_utf8_buffer(), server_sig.to_utf8_buffer())) {
				return Err::from("server signature mismatch", Err::PERMISSION_DENIED);
			}
			return Ref<Err>();
		}
	}
	return Err::from("no server signature", Err::INVALID_DATA);
}

// ---------------- Result decoding ----------------

PackedStringArray GDPostgresClient::row_desc(const uint8_t *p_data, int p_len) {
	PackedStringArray names;
	oids.clear();
	if (p_len < 2) {
		torn = "row description has no column count";
		return names; // Reject input without even a column count.
	}
	const int n = read16p(p_data, 0);
	int at = 2;
	bool cut = false;
	for (int i = 0; i < n; i++) {
		int next = 0;
		// Validate each column's complete metadata instead of trusting the declared count.
		if (at >= p_len) {
			cut = true;
			break;
		}
		names.push_back(read_cstrp(p_data, p_len, at, next));
		at = next;
		if (at + 18 > p_len) {
			names.resize(names.size() - 1); // Do not count a column whose type metadata is incomplete.
			cut = true;
			break;
		}
		if (read16p(p_data, at + 16) > 1) {
			cut = true;
			break;
		}
		oids.push_back(read32p(p_data, at + 6)); // Skip tableOID and column number.
		at += 18;
	}
	// Record mismatched declared and decoded column counts as malformed input,
	// even when bounds checks prevented an out-of-buffer read.
	if (cut || (int)names.size() != n || at != p_len) {
		torn = vformat("row description says %d columns but holds %d", n, names.size());
	}
	return names;
}

// Decode a DataRow protocol message into result values.
Variant GDPostgresClient::data_row(const uint8_t *p_data, int p_len, const LocalVector<StringName> &p_keys, const LocalVector<int32_t> &p_oids, const LocalVector<uint8_t> &p_fmts, bool p_values, Array *r_flat) {
	Dictionary row;
	Array values;
	if (p_len < 2) {
		torn = "data row has no column count";
		return p_values ? Variant(values) : Variant(row); // Reject input without even a column count.
	}
	const int n = read16p(p_data, 0);
	// Reject rows whose column count differs from the description before allocating.
	if (n != (int)p_keys.size() || n != (int)p_oids.size()) {
		torn = vformat("row says %d values but description has %d", n, p_keys.size());
		return p_values ? Variant(values) : Variant(row);
	}
	if (r_flat) {
		// The caller retains the shared destination for all rows.
	} else if (p_values) {
		values.reserve(n); // Append column-ordered values without filling unused initial elements.
	} else {
		row.reserve(n); // Reserve known column capacity to avoid per-row hash-map growth.
	}
	// Share protocol parsing while selecting only the output representation.
	auto set_value = [&](int p_i, Variant p_value) {
		if (r_flat) {
			r_flat->push_back(p_value);
		} else if (p_values) {
			values.push_back(p_value);
		} else {
			const Variant key = p_i < (int)p_keys.size() ? Variant(p_keys[p_i]) : Variant(String::num_int64(p_i));
			row[key] = p_value;
		}
	};
	int at = 2;
	const uint8_t *b = p_data;
	for (int i = 0; i < n; i++) {
		// Validate peer-supplied lengths before accessing buffer contents.
		if (at + 4 > p_len) {
			torn = vformat("row says %d values but holds %d", n, i);
			break;
		}
		const int32_t len = read32p(p_data, at);
		at += 4;
		// Compare by subtraction so adding at and len cannot overflow an int.
		if (len < -1 || (len > 0 && (int64_t)len > (int64_t)p_len - at)) {
			torn = vformat("value %d says %d bytes but %d remain", i, len, p_len - at);
			break;
		}
		if (len == -1) {
			set_value(i, Variant());
			continue;
		}
		const int32_t oid = i < (int)p_oids.size() ? p_oids[i] : 25;
		const bool bin = i < (int)p_fmts.size() && p_fmts[i] == 1;
		if (oid == 17) {
			PackedByteArray value;
			if (!bin && !bytea_value(b + at, len, value)) {
				torn = vformat("binary value %d is malformed", i);
				break;
			}
			if (bin) {
				value.resize(len);
				if (len > 0) memcpy(value.ptrw(), b + at, len);
			}
			set_value(i, value);
			at += len;
			continue;
		}
		if (bin) {
			int width = -1;
			switch (oid) {
				case 16:
					width = 1;
					break; // bool
				case 21:
					width = 2;
					break; // int2
				case 23: // int4
				case 26: // oid
				case 700:
					width = 4;
					break; // float4
				case 20: // int8
				case 701:
					width = 8;
					break; // float8
			}
			// Reject both short and oversized fixed-width values instead of decoding a different value.
			if ((width >= 0 && len != width) || (oid == 16 && b[at] > 1)) {
				torn = vformat("binary value %d has invalid length or value", i);
				break;
			}
			// Decode binary values directly in network byte order.
			if (width >= 0) {
				set_value(i, from_binary(b + at, len, oid));
				at += len;
				continue;
			}
		}
		// Parse numeric and boolean values directly from bytes.
		// Avoid allocating a temporary string for every converted column.
		if (oid == 20 || oid == 21 || oid == 23 || oid == 26) {
			uint64_t v = 0;
			bool neg = false;
			int k = 0;
			if (len > 0 && b[at] == '-') {
				neg = true;
				k = 1;
			}
			const uint64_t limit = neg ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
			if (k >= len) {
				torn = vformat("integer value %d is malformed", i);
			}
			for (; torn.is_empty() && k < len; k++) {
				const uint8_t c = b[at + k];
				if (c < '0' || c > '9') {
					torn = vformat("integer value %d is malformed", i);
					break;
				}
				const uint64_t digit = c - '0';
				if (v > (limit - digit) / 10) {
					torn = vformat("integer value %d is out of range", i);
					break;
				}
				v = v * 10 + digit;
			}
			if (torn.is_empty()) {
				const int64_t signed_v = neg ? (v == uint64_t(INT64_MAX) + 1 ? INT64_MIN : -(int64_t)v) : (int64_t)v;
				if ((oid == 21 && (signed_v < INT16_MIN || signed_v > INT16_MAX)) ||
						(oid == 23 && (signed_v < INT32_MIN || signed_v > INT32_MAX)) ||
						(oid == 26 && (signed_v < 0 || v > UINT32_MAX))) {
					torn = vformat("integer value %d exceeds its type range", i);
				} else {
					set_value(i, signed_v);
				}
			}
			at += len;
			continue;
		}
		if (oid == 16) { // bool
			if (len != 1 || (b[at] != 't' && b[at] != 'f')) {
				torn = vformat("boolean value %d is malformed", i);
				break;
			}
			set_value(i, b[at] == 't');
			at += len;
			continue;
		}
		Error error = OK;
		const String raw = pg_text(b + at, len, &error);
		if (error != OK) {
			torn = "invalid UTF8 in text value";
			break;
		}
		at += len;
		set_value(i, bin ? Variant(raw) : convert(raw, oid));
	}
	if (torn.is_empty() && at != p_len) {
		torn = vformat("data row has %d trailing bytes", p_len - at);
	}
	return p_values ? Variant(values) : Variant(row);
}

// ---------------- Public entry points ----------------

Signal GDPostgresClient::open(const String &p_host, int64_t p_port, const Dictionary &p_opts) {
	// Retain the original hostname for certificate matching before address resolution.
	host = p_host;
	if (p_host.is_empty() || p_port < Limit::PORT_MIN || p_port > Limit::PORT_MAX) {
		Ref<GDPostgresCallInternal> bad = lend();
		bad->self_hold = bad;
		bad->fail_later(pg_fail("postgres address is invalid", Err::INVALID_DATA));
		return Signal(bad.ptr(), "finished");
	}
	port = int(p_port);
	Ref<GDPostgresCallInternal> call = lend();
	call->db = Ref<GDPostgresClient>(this);
	call->self_hold = call;
	call->mode = GDPostgresCallInternal::CONNECTING;
	call->opts = p_opts;
	uint64_t connect_ms = 0;
	uint64_t query_ms = 0;
	if (!DbOption::seconds(p_opts, "connect_timeout", 0.0, connect_ms) ||
			!DbOption::seconds(p_opts, "timeout", 0.0, query_ms)) {
		call->fail_later(pg_fail("timeouts must be zero or a positive number of seconds", Err::INVALID_DATA));
		return Signal(call.ptr(), "finished");
	}
	call->set_due(connect_ms);
	wait_ms = query_ms;
	scram_pending = false;
	auth_ok = false;
	nonce = String();
	server_sig = String();
	scram_done = false;
	md5_done = false;
	String auth;
	if (!DbOption::text(p_opts, "auth", "any", auth) || (auth != "any" && auth != "scram" && auth != "md5") || !pg_credentials(p_opts)) {
		call->fail_later(pg_fail("auth must be any, scram, or md5", Err::INVALID_DATA));
		return Signal(call.ptr(), "finished");
	}

	// Select verify-full for explicit true or external-host defaults, and disable for default loopback.
	if (!Wire::guard_of(p_opts.get("tls", Wire::default_guard(p_host)), guard)) {
		call->fail_later(pg_fail("tls must be one of disable / require / verify-full", Err::INVALID_DATA));
		return Signal(call.ptr(), "finished");
	}
	String ca_path;
	if (!DbOption::text(p_opts, "ca", "", ca_path)) {
		call->fail_later(pg_fail("ca path must be text", Err::INVALID_DATA));
		return Signal(call.ptr(), "finished");
	}
	ca.unref();
	tls_at = guard == Wire::NONE ? TLS_OFF : TLS_ASK;
	// Accept hostnames as well as numeric addresses for container and production endpoints.
	// Check network permission before resolving names, since a later connection check
	// would still allow the DNS query to leave the process.
	if (!Perm::check(Perm::NET, vformat("%s:%d", p_host, p_port))) {
		call->fail_later(pg_fail(vformat("net access to \"%s\" is not allowed", p_host), Err::PERMISSION_DENIED));
		return Signal(call.ptr(), "finished");
	}
	if (opening.is_valid() || sock.is_valid() || !inflight.is_empty() || !packing.is_empty()) {
		close(); // Finish the previous connection and unfinished operations before reopening.
	}
	opening = call;
	if (!ca_path.is_empty()) {
		call->mode = GDPostgresCallInternal::RESOLVING;
		GDPairCall::start([p_host, ca_path]() { return Wire::prepare(p_host, ca_path); }, false).connect(
				callable_mp(this, &GDPostgresClient::resolved).bind(call), Object::CONNECT_ONE_SHOT);
		return Signal(call.ptr(), "finished");
	}
	if (sock.open(p_host, p_port, call->due) != OK) {
		call->fail_later(pg_fail(vformat("cannot reach %s:%d", p_host, p_port), Err::NOT_FOUND));
	} else {
		call->schedule();
	}
	return Signal(call.ptr(), "finished");
}

// Send Close for a named server-side statement.
void GDPostgresClient::close_stmt(const String &p_name) {
	put8(out_buf, 'C');
	const int len_at = out_buf.size();
	put32(out_buf, 0);
	put8(out_buf, 'S');
	put_cstr(out_buf, p_name);
	fix_len(out_buf, len_at);
	if (!flush_queued) {
		flush_queued = true;
		callable_mp(this, &GDPostgresClient::flush_out).call_deferred();
	}
}

// Prepare each SQL after its first Parse is confirmed by the server.
// Give overlapping requests temporary names so they never Bind an unconfirmed statement.
String GDPostgresClient::prepare(const String &p_sql, bool &r_is_new) {
	HashMap<String, String>::Iterator found = stmts.find(p_sql);
	if (found) {
		if (stmt_pending.has(found->value)) {
			// Keep the first cache entry until its reply establishes whether reuse is safe.
			r_is_new = true;
			return vformat("gds%d", ++stmt_seq);
		}
		List<String>::Element **at = stmt_lru_pos.getptr(p_sql);
		if (at) {
			stmt_lru.move_to_front(*at);
		}
		r_is_new = false;
		return found->value;
	}
	// At capacity, close only the least recently used statement.
	if (stmts.size() >= STMT_MAX && !stmt_lru.is_empty()) {
		const String oldest = stmt_lru.back()->get();
		forget(oldest, true);
	}
	r_is_new = true;
	const String name = vformat("gds%d", ++stmt_seq);
	stmts.insert(p_sql, name);
	stmt_pending.insert(name);
	stmt_lru.push_front(p_sql);
	stmt_lru_pos.insert(p_sql, stmt_lru.front());
	return name;
}

// Validate synchronization messages before their state can influence connection reuse.
bool GDPostgresClient::ready_state(const uint8_t *p_body, int p_len) {
	if (p_len != 1 || (p_body[0] != 'I' && p_body[0] != 'T' && p_body[0] != 'E')) return false;
	tx_status = p_body[0];
	return true;
}

// Deliver available replies to the first waiter in send order.
// The server replies sequentially, so complete the first operation before the next.
void GDPostgresClient::pump() {
	pump_posted = false;
	const uint64_t active_generation = wire_generation;
	// Retain this connection until the pump returns because result callbacks may
	// release its final external reference during delivery.
	Ref<GDPostgresClient> keep(this);
	// Flush incomplete output first so sending resumes when the peer begins reading.
	if (!out_buf.is_empty()) {
		flush_out();
		if (active_generation != wire_generation) {
			return;
		}
	}
	// Expire callers without stopping encoders; do not reuse operations until their workers finish.
	const uint64_t now = GDClock::msec();
	for (const Ref<GDPostgresPackJob> &job : packing) {
		if (job->call.is_null() || job->call->due == 0 || now < job->call->due) {
			continue;
		}
		if (job->is_new) {
			forget(job->sql);
		}
		if (!job->call->dropped) {
			job->call->interrupt(pg_fail("postgres batch encoding timed out", Err::TIMED_OUT));
		}
		Async::drop_deadline(job->call.ptr(), job->call->due);
		job->call->due = 0;
	}
	if (inflight.is_empty() && out_buf.is_empty() && packing.is_empty()) {
		sock.read_wait(false);
		watch(false);
		return;
	}
	// Advance deadlines even when the shared work budget is exhausted.
	auto expire = [&]() -> bool {
		if (inflight.is_empty()) {
			return false;
		}
		Ref<GDPostgresCallInternal> head = inflight.front()->get();
		if (head->due <= 0 || GDClock::msec() < head->due) {
			return false;
		}
		if (head->shared) {
			head->interrupt(pg_fail("timeout", Err::TIMED_OUT));
			return false;
		}
		inflight.pop_front();
		head->db.unref();
		close();
		head->fail_later(pg_fail("timeout", Err::TIMED_OUT));
		return true;
	};
	if (expire()) {
		return;
	}
	if (!inflight.is_empty() && inflight.front()->get()->stream_paused) {
		return; // Do not read the next row from the socket or buffer until Next is requested.
	}
	const Ref<Err> live = fill(PG_READ_CHUNK);
	// Preserve complete buffered replies before applying a transport failure to unfinished calls.

	char kind = 0;
	int at = 0;
	int len = 0;
	const uint64_t slice_due = GDClock::usec() + GD_SCHED_SLICE_USEC;
	while (!inflight.is_empty()) {
		if (inflight.front()->get()->stream_paused) {
			break; // Retain the next buffered row until Rows.Next.
		}
		if (!next_msg(kind, at, len)) {
			const int64_t waiting_bytes = next_msg_size();
			if (waiting_bytes == 0 || (waiting_bytes > 0 && waiting_bytes <= int64_t(SEND_BYTES_MAX) + 1)) {
				break;
			}
			Ref<GDPostgresCallInternal> head = inflight.front()->get();
			inflight.pop_front();
			head->db.unref();
			close();
			head->fail_later(pg_fail("postgres message exceeds the protocol maximum", Err::LIMITED));
			return;
		}
		const int message_bytes = len + 4;
		if (message_bytes > SEND_BYTES_MAX) {
			Ref<GDPostgresCallInternal> head = inflight.front()->get();
			inflight.pop_front();
			head->db.unref();
			close();
			head->fail_later(pg_fail("postgres message exceeds the protocol maximum", Err::LIMITED));
			return;
		}
		// Preserve the negotiated text contract before decoding results or reusing this connection.
		if (kind == 'S') {
			const Ref<Err> error = status_error(buf.ptr() + at, len);
			if (error.is_valid()) {
				drain(pg_fail(error));
				return;
			}
		}
		if (kind == 'Z' && !ready_state(buf.ptr() + at, len)) {
			drain(pg_fail("invalid postgres transaction state", Err::INVALID_DATA));
			return;
		}
		GDPostgresCallInternal *head = inflight.front()->get().ptr();
		const bool finished = head->take(kind, buf.ptr() + at, len);
		if (active_generation != wire_generation) {
			return; // Do not apply stale cleanup after a callback closes or reopens the connection.
		}
		if (finished) {
			inflight.pop_front(); // Remove before notification so callbacks no longer count this operation as waiting.
			head->finish();
			if (active_generation != wire_generation) {
				return; // Leave a connection reopened by the completion handler to its new pump.
			}
		}
		if (GDClock::usec() >= slice_due) {
			if (has_complete_msg()) {
				post_pump(); // Yield to the end of the ready queue when the time slice expires.
			}
			break;
		}
	}
	const bool paused = !inflight.is_empty() && inflight.front()->get()->stream_paused;
	if (live.is_valid() && !has_complete_msg() && !paused) {
		drain(pg_fail(live));
		return;
	}
	if (has_complete_msg() || sock.available() > 0) {
		// Requeue buffered or kernel-ready continuation without awaiting a new edge.
		post_pump();
	}

	if (expire()) {
		return;
	}
	if (inflight.is_empty() && out_buf.is_empty() && packing.is_empty()) {
		sock.read_wait(false);
		watch(false);
	}
}

// Flush buffered queries once at the turn boundary.
void GDPostgresClient::flush_out() {
	Ref<GDPostgresClient> keep(this); // Remain alive while send failures notify waiters that may release this connection.
	flush_queued = false;
	if (out_buf.is_empty()) {
		sock.write_wait(false);
		return;
	}
	if (!sock.is_valid() || !sock.is_ready()) {
		sock.write_wait(false);
		out_buf.clear();
		drain(pg_fail(sock_error(sock, "connection lost")));
		return;
	}
	if (!sock_flush(sock, out_buf)) {
		drain(pg_fail(sock_error(sock, "send failed")));
		return;
	}
	if (out_buf.is_empty() && !next_buf.is_empty()) {
		out_buf = static_cast<ByteBuf &&>(next_buf); // Move to the next send without copying the whole buffer.
		start_pack(); // Advance encoding while retaining at most two outbound buffers.
		// The write notification is consumed; ensure the promoted buffer runs on the next main-loop turn.
		if (!flush_queued) {
			flush_queued = true;
			callable_mp(this, &GDPostgresClient::flush_out).call_deferred();
		}
	}
	if (!out_buf.is_empty()) {
		watch(true); // Resume the incomplete send on a later readiness notification.
	}
}

// Borrow a completed operation when available.
Ref<GDPostgresCallInternal> GDPostgresClient::lend() {
	if (!spare.is_empty()) {
		Ref<GDPostgresCallInternal> got = spare[spare.size() - 1];
		spare.remove_at(spare.size() - 1);
		got->reset();
		return got;
	}
	Ref<GDPostgresCallInternal> made;
	made.instantiate();
	return made;
}

// Retain completed operations within the reserve capacity.
void GDPostgresClient::give_back(GDPostgresCallInternal *p_call) {
	if (spare.size() >= SPARE_MAX) {
		return;
	}
	// Release the owner reference before caching to avoid a reference cycle.
	p_call->db.unref();
	p_call->reset(); // Do not retain the previous result or password.
	spare.push_back(Ref<GDPostgresCallInternal>(p_call));
}

// Discard all statement metadata so its next query prepares it again.
void GDPostgresClient::forget(const String &p_sql, bool p_close) {
	const String *name = stmts.getptr(p_sql);
	if (name) {
		stmt_pending.erase(*name);
		if (p_close) {
			close_stmt(*name);
		}
	}
	stmts.erase(p_sql);
	List<String>::Element **at = stmt_lru_pos.getptr(p_sql);
	if (at) {
		stmt_lru.erase(*at);
		stmt_lru_pos.erase(p_sql);
	}
	cols.erase(p_sql);
	key_memo.erase(p_sql);
	oid_memo.erase(p_sql);
	fmt_memo.erase(p_sql);
}

// Return cached column names for the SQL.
const PackedStringArray *GDPostgresClient::known_cols(const String &p_sql) const {
	return cols.getptr(p_sql);
}

// Return cached prehashed column keys for the SQL.
const LocalVector<StringName> *GDPostgresClient::known_keys(const String &p_sql) const {
	return key_memo.getptr(p_sql);
}

// Return cached column type OIDs for the SQL.
const LocalVector<int32_t> *GDPostgresClient::known_oids(const String &p_sql) const {
	return oid_memo.getptr(p_sql);
}

// Return cached result formats for the SQL.
const LocalVector<uint8_t> *GDPostgresClient::known_fmts(const String &p_sql) const {
	return fmt_memo.getptr(p_sql);
}

// Advance only connections reported ready by the kernel.
void GDPostgresClient::socket_ready() {
	pump_posted = false;
	if (opening.is_valid()) {
		opening->step();
		return;
	}
	if (watching) {
		pump();
	}
}

// Schedule buffered continuation even without new socket input.
void GDPostgresClient::post_pump() {
	if (!watching || pump_posted) {
		return;
	}
	pump_posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDPostgresClient::dispatch_pump));
}

// Ignore completion entries whose work has already been processed.
void GDPostgresClient::dispatch_pump() {
	if (!pump_posted) {
		return;
	}
	pump_posted = false;
	if (watching) {
		pump();
	}
}

// Track whether queries are active; socket wakeups arrive directly from descriptors.
void GDPostgresClient::watch(bool p_on) {
	watching = p_on;
	if (!p_on) {
		pump_posted = false;
	}
}

// Validate values and encode extended-query wire bytes on a worker.
void GDPostgresPackJob::run() {
	const int64_t sql_bytes = utf8_bytes(sql);
	const int64_t stmt_bytes = utf8_bytes(stmt);
	if ((check_only || is_new) && (stmt_bytes > SEND_BYTES_MAX - 8 || sql_bytes > SEND_BYTES_MAX - 8 - stmt_bytes)) {
		error = Err::make(check_only ? "postgres check SQL exceeds the byte limit" : "postgres SQL exceeds the byte limit", Err::LIMITED);
		return;
	}
	if (check_only) {
		put8(packed, 'P');
		int len_at = packed.size();
		put32(packed, 0);
		put_cstr(packed, stmt);
		put_cstr(packed, sql);
		put16(packed, 0);
		fix_len(packed, len_at);
		put8(packed, 'D');
		len_at = packed.size();
		put32(packed, 0);
		put8(packed, 'S');
		put_cstr(packed, stmt);
		fix_len(packed, len_at);
		put8(packed, 'C');
		len_at = packed.size();
		put32(packed, 0);
		put8(packed, 'S');
		put_cstr(packed, stmt);
		fix_len(packed, len_at);
		put8(packed, 'S');
		put32(packed, 4);
		return;
	}
	int64_t send_bound = (is_new ? sql_bytes + stmt_bytes + 8 : 0) + STMT_CLOSE_BOUND + 128;
	add_bound(send_bound, int64_t(fmts.size()) * 2 * rows.size());
	for (int r = 0; r < rows.size(); r++) {
		if (rows[r].get_type() != Variant::ARRAY) {
		error = Err::make(vformat("postgres batch row %d is not an Array", r + 1), Err::INVALID_DATA);
			return;
		}
		const Array args = rows[r];
		if (args.size() > PARAM_MAX) {
		error = Err::make(vformat("postgres batch row %d parameter count exceeds the limit", r + 1), Err::LIMITED);
			return;
		}
		add_bound(send_bound, 64 + int64_t(args.size()) * 4);
		for (int i = 0; i < args.size(); i++) {
			const int64_t value_bound = param_bound(args[i]);
			if (value_bound < 0) {
			error = Err::make(vformat("postgres parameter %d in row %d has an unsupported type", i + 1, r + 1), Err::INVALID_DATA);
				return;
			}
			add_bound(send_bound, value_bound);
		}
		if (due > 0 && (r & 255) == 255 && GDClock::msec() > due) {
		error = Err::make("postgres batch encoding timed out", Err::TIMED_OUT);
			return;
		}
	}
	if (send_bound > PACK_BYTES_MAX) {
		send_bound = (is_new ? sql_bytes + stmt_bytes + 8 : 0) + STMT_CLOSE_BOUND + 128;
		add_bound(send_bound, int64_t(fmts.size()) * 2 * rows.size());
		for (const Variant &row : rows) {
			const Array args = row;
			add_bound(send_bound, 64 + int64_t(args.size()) * 4);
			for (const Variant &arg : args) {
				add_bound(send_bound, param_bytes(arg));
			}
		}
		if (send_bound > PACK_BYTES_MAX) {
		error = Err::make("postgres batch exceeds the in-memory representation", Err::LIMITED);
			return;
		}
	}

	// Register the statement once, then append Bind, Execute, and Sync for each argument row.
	if (is_new) {
		put8(packed, 'P');
		const int len_at = packed.size();
		put32(packed, 0);
		put_cstr(packed, stmt);
		put_cstr(packed, sql);
		put16(packed, 0);
		fix_len(packed, len_at);
	}
	bool desc = ask_desc;
	for (int r = 0; r < rows.size(); r++) {
		const Array args = rows[r];
		put8(packed, 'B');
		int len_at = packed.size();
		put32(packed, 0);
		put_cstr(packed, "");
		put_cstr(packed, stmt);
		put16(packed, 0);
		put16(packed, args.size());
		for (const Variant &arg : args) {
			put_param(packed, arg);
		}
		if (fmts.size() > PARAM_MAX) {
			packed.reset();
		error = Err::make("postgres result format count exceeds the protocol maximum", Err::LIMITED);
			return;
		}
		if (!fmts.is_empty()) {
			put16(packed, (int16_t)fmts.size());
			for (uint32_t f = 0; f < fmts.size(); f++) {
				put16(packed, (int16_t)fmts[f]);
			}
		} else {
			put16(packed, 0);
		}
		if ((int64_t)packed.size() - len_at > SEND_BYTES_MAX) {
			packed.reset();
		error = Err::make("postgres bind message exceeds the protocol maximum", Err::LIMITED);
			return;
		}
		fix_len(packed, len_at);
		if (desc) {
			put8(packed, 'D');
			len_at = packed.size();
			put32(packed, 0);
			put8(packed, 'P');
			put_cstr(packed, "");
			fix_len(packed, len_at);
			desc = false;
		}
		put8(packed, 'E');
		len_at = packed.size();
		put32(packed, 0);
		put_cstr(packed, "");
		put32(packed, 0);
		fix_len(packed, len_at);
		put8(packed, 'S');
		put32(packed, 4);
	}
}

// Return worker results to the connection's arrival-ordered queue.
void GDPostgresPackJob::finish() {
	if (db.is_valid()) {
		if (Pool::is_stopping(true)) {
			db->close(); // Do not send; close other unfinished calls on the same connection too.
		} else {
			db->packed(this);
		}
	}
	db.unref();
	call.unref();
	rows.clear();
}

// Synchronously close all live connections at process shutdown.
void GDPostgresClient::shutdown_all() {
	LocalVector<Ref<GDPostgresClient>> clients;
	for (GDPostgresClient *client : postgres_clients) {
		clients.push_back(Ref<GDPostgresClient>(client));
	}
	for (const Ref<GDPostgresClient> &client : clients) {
		client->close();
	}
}

// Prepare only the first encoding job on a given connection.
void GDPostgresClient::queue_pack(const Ref<GDPostgresPackJob> &p_job) {
	packing_bytes += p_job->queued_bytes;
	const bool first = packing.is_empty();
	packing.push_back(p_job);
	watch(true); // Keep receiving deadline wakeups during encoding.
	if (first && next_buf.is_empty()) {
		start_pack();
	}
}

// Prepare queued sends in order, retaining one shared slice for local encoding.
void GDPostgresClient::start_pack() {
	if (packing_active) return;
	packing_active = true;
	const uint64_t until = GDClock::usec() + GD_SCHED_SLICE_USEC;
	while (!packing.is_empty() && next_buf.is_empty()) {
		Ref<GDPostgresPackJob> job = packing.front()->get();
		if (!job->call.is_valid() || !job->call->self_hold.is_valid()) {
			packing.pop_front();
			packing_bytes = MAX(int64_t(0), packing_bytes - job->queued_bytes);
			continue;
		}
		if (!is_open()) {
			packing.pop_front();
			packing_bytes = MAX(int64_t(0), packing_bytes - job->queued_bytes);
			job->call->fail_later(pg_fail("not connected", Err::NOT_FOUND));
			continue;
		}
		if (job->started) break; // A worker already owns the first job.
		if (job->call->dropped) {
			packing.pop_front();
			packing_bytes = MAX(int64_t(0), packing_bytes - job->queued_bytes);
			job->call->done(pg_fail("cancelled", Err::INTERRUPTED));
			continue;
		}
		job->started = true;
		bool local = job->work_bytes <= INLINE_PACK_MAX && GDClock::usec() < until;
		if (job->check_only) {
			job->stmt = vformat("gdc%d", ++stmt_seq);
		} else {
			job->stmt = prepare(job->sql, job->is_new);
			job->call->sql = job->sql;
			job->call->stmt = job->stmt;
			job->call->parse_pending = job->is_new;
			const String *cached = stmts.getptr(job->sql);
			job->call->temporary_stmt = job->is_new && (!cached || *cached != job->stmt);
			const PackedStringArray *known = known_cols(job->sql);
			job->ask_desc = (known == nullptr);
			if (known) {
				job->call->columns = *known;
				if (job->call->stream) job->call->stream->postgres_columns(*known);
				const LocalVector<StringName> *keys = known_keys(job->sql);
				if (keys) job->call->keys = *keys;
				const LocalVector<int32_t> *kinds = known_oids(job->sql);
				if (kinds) job->call->oids = *kinds;
				const LocalVector<uint8_t> *want = known_fmts(job->sql);
				if (want) {
					job->fmts = *want;
					job->call->fmts = *want;
				}
			}
			local = local && !has_object(job->rows);
		}
		if (local) {
			job->run(); // Short single and batched sends share the same encoder without a worker round trip.
			packed(job.ptr());
			continue;
		}
		if (job->submit(true)) break;
		packing.pop_front();
		packing_bytes = MAX(int64_t(0), packing_bytes - job->queued_bytes);
		if (job->is_new && stmts.getptr(job->sql) && *stmts.getptr(job->sql) == job->stmt) forget(job->sql);
		job->call->done(pg_fail("worker pool stopped", Err::INTERRUPTED));
	}
	packing_active = false;
}

// Move encoded queries into the send queue in arrival order.
void GDPostgresClient::packed(GDPostgresPackJob *p_job) {
	if (packing.is_empty() || packing.front()->get().ptr() != p_job) {
		return; // Ignore jobs from a closed or previous connection.
	}
	Ref<GDPostgresPackJob> job = packing.front()->get();
	packing.pop_front();
	packing_bytes = MAX(int64_t(0), packing_bytes - job->queued_bytes);
	if (!job->call.is_valid() || !job->call->self_hold.is_valid()) {
		if (job->is_new && stmts.getptr(job->sql) && *stmts.getptr(job->sql) == job->stmt) {
			forget(job->sql);
		}
	} else if (job->call->dropped) {
		if (job->is_new && stmts.getptr(job->sql) && *stmts.getptr(job->sql) == job->stmt) {
			forget(job->sql);
		}
		job->call->done(pg_fail("cancelled", Err::INTERRUPTED));
	} else {
		if (job->error.is_valid()) {
			if (job->is_new && stmts.getptr(job->sql) && *stmts.getptr(job->sql) == job->stmt) {
				forget(job->sql);
			}
			job->call->fail_later(pg_fail(job->error));
		} else if (!is_open()) {
			job->call->fail_later(pg_fail("not connected", Err::NOT_FOUND));
		} else if (job->due > 0 && GDClock::msec() >= job->due) {
			if (job->is_new && stmts.getptr(job->sql) && *stmts.getptr(job->sql) == job->stmt) {
				forget(job->sql);
			}
			job->call->fail_later(pg_fail("postgres batch encoding timed out", Err::TIMED_OUT));
		} else {
			if (job->call->want_n > 0 && !job->call->discard_many && !job->call->flat_many) {
				job->call->batches.resize(job->rows.size());
			} else if (job->call->flat_many) {
				job->call->batches.reserve(job->rows.size());
			}
			if (out_buf.is_empty() && next_buf.is_empty()) {
				out_buf = static_cast<ByteBuf &&>(job->packed);
			} else {
				next_buf = static_cast<ByteBuf &&>(job->packed);
			}
			if (!job->flush_now && !flush_queued) {
				flush_queued = true;
				callable_mp(this, &GDPostgresClient::flush_out).call_deferred();
			}
			inflight.push_back(job->call);
			sock.read_wait(true); // Route response readiness directly to this connection's callback.
			watch(true);
			if (job->flush_now) {
				flush_out(); // Leave only unsent bytes waiting for write readiness.
			}
		}
	}
	if (next_buf.is_empty()) {
		start_pack(); // Other connections' first jobs can run concurrently in the CPU pool.
	}
}

// Accept an extended query and dispatch validation and encoding to CPU workers.
Signal GDPostgresClient::send_rows(const String &p_sql, const Array &p_rows, bool p_many, bool p_discard, bool p_flat, int p_max_rows, int64_t p_max_bytes, bool p_values, bool p_flat_values, bool p_first, bool p_flush_now) {
	Ref<GDPostgresCallInternal> call = lend();
	call->db = Ref<GDPostgresClient>(this);
	call->self_hold = call;
	call->set_due(wait_ms);
	call->want_n = p_many ? p_rows.size() : 0;
	call->discard_many = p_discard;
	call->flat_many = p_flat;
	call->values_only = p_values;
	call->flat_values = p_flat_values;
	call->first_only = p_first;
	call->max_rows = p_max_rows;
	call->max_bytes = p_max_bytes;
	if (!is_open()) {
		call->fail_later(pg_fail("not connected", Err::NOT_FOUND));
		return Signal(call.ptr(), "finished");
	}
	if (p_rows.is_empty()) {
		call->fail_later(p_many ? (p_discard ? pg_ok(0) : pg_ok(Array())) : pg_fail("no args", Err::INVALID_DATA));
		return Signal(call.ptr(), "finished");
	}
	Ref<GDPostgresPackJob> job;
	job.instantiate();
	job->db = Ref<GDPostgresClient>(this);
	job->call = call;
	job->sql = p_sql;
	job->rows = snapshot_rows(p_rows);
	job->due = call->due;
	job->flush_now = p_flush_now;
	const QuerySize size = query_size(p_sql, job->rows);
	job->work_bytes = size.work;
	job->queued_bytes = size.queued;
	queue_pack(job);
	return Signal(call.ptr(), "finished");
}

// Execute a query and return its result.
Signal GDPostgresClient::query(const String &p_sql, const Array &p_args) {
	Array one;
	one.push_back(p_args);
	return send_rows(p_sql, one, false, false);
}

// Return row arrays in column order without constructing a column-name hash map per row.
Signal GDPostgresClient::query_values(const String &p_sql, const Array &p_args) {
	Array one;
	one.push_back(p_args);
	return send_rows(p_sql, one, false, false, false, 0, 0, true);
}

// Append all values directly to one array without allocating separate row arrays.
Signal GDPostgresClient::query_flat(const String &p_sql, const Array &p_args) {
	Array one;
	one.push_back(p_args);
	return send_rows(p_sql, one, false, false, false, 0, 0, true, true);
}

// Send shared database queries with explicit result row and byte limits.
Signal GDPostgresClient::query_limited(const String &p_sql, const Array &p_args, int p_max_rows, int64_t p_max_bytes) {
	Array one;
	one.push_back(p_args);
	return send_rows(p_sql, one, false, false, false, p_max_rows, p_max_bytes);
}

// Decode only the first row and drain the remainder without retaining values.
Signal GDPostgresClient::query_row(const String &p_sql, const Array &p_args, int64_t p_max_bytes) {
	Array one;
	one.push_back(p_args);
	return send_rows(p_sql, one, false, false, false, 0, p_max_bytes, false, false, true);
}

// Create sequential Rows with at most one prefetched row.
Ref<GDDatabaseRows> GDPostgresClient::start_rows(const String &p_sql, const Array &p_args, bool p_flush_now) {
	Array one;
	one.push_back(p_args);
	const Signal signal = send_rows(p_sql, one, false, false, false, 0, 0, true, false, false, p_flush_now);
	Ref<GDPostgresCallInternal> call = Ref<GDPostgresCallInternal>(Object::cast_to<GDPostgresCallInternal>(signal.get_object()));
	Ref<GDDatabaseRows> out;
	out.instantiate();
	out->attach_postgres(call);
	return out;
}

// Return one row per Next call without accumulating the complete result.
Signal GDPostgresClient::query_rows(const String &p_sql, const Array &p_args) {
	return Async::ready_pair({ start_rows(p_sql, p_args), Variant() });
}

// Send multiple SQL executions as one operation.
Signal GDPostgresClient::query_many(const String &p_sql, const Array &p_rows) {
	return send_rows(p_sql, p_rows, true, false);
}

// Return only flattened rows without per-query result boundaries.
Signal GDPostgresClient::fetch_many(const String &p_sql, const Array &p_rows) {
	return send_rows(p_sql, p_rows, true, false, true);
}

// Return pipeline rows in column order without per-row hash maps.
Signal GDPostgresClient::fetch_values_many(const String &p_sql, const Array &p_rows) {
	return send_rows(p_sql, p_rows, true, false, true, 0, 0, true);
}

// Append every pipeline row directly to one value array.
Signal GDPostgresClient::fetch_flat_many(const String &p_sql, const Array &p_rows) {
	return send_rows(p_sql, p_rows, true, false, true, 0, 0, true, true);
}

// Batch updates and return only the completion count without individual results.
Signal GDPostgresClient::exec_many(const String &p_sql, const Array &p_rows) {
	return send_rows(p_sql, p_rows, true, true);
}

// Validate without execution by sending Parse and Describe but no Execute.
// Return success or preserve the server's validation failure.
Signal GDPostgresClient::check(const String &p_sql) {
	// Use a fresh operation for this infrequent validation path.
	// Server error transitions make incomplete reusable-state resets especially risky here.
	Ref<GDPostgresCallInternal> call;
	call.instantiate();
	call->db = Ref<GDPostgresClient>(this);
	call->self_hold = call;
	call->set_due(wait_ms);

	if (!is_open()) {
		call->fail_later(pg_fail("not connected", Err::NOT_FOUND));
		return Signal(call.ptr(), "finished");
	}
	Ref<GDPostgresPackJob> job;
	job.instantiate();
	job->db = Ref<GDPostgresClient>(this);
	job->call = call;
	job->sql = p_sql;
	job->due = call->due;
	job->check_only = true;
	job->work_bytes = MIN(int64_t(SEND_BYTES_MAX) + 1, int64_t(p_sql.length()) * 4 + 512);
	job->queued_bytes = job->work_bytes;
	queue_pack(job);
	return Signal(call.ptr(), "finished");
}

// ---------------- Connection pools ----------------

enum PgPoolKind {
	PG_POOL_ACQUIRE, // Borrow only a transaction connection.
	PG_POOL_ROWS, // Borrow a connection until Rows closes.
	PG_POOL_QUERY, // Return ordinary rows.
	PG_POOL_LIMITED, // Return rows with explicit row and byte limits.
	PG_POOL_ROW, // Return only the first row.
	PG_POOL_VALUES, // Return column-ordered rows.
	PG_POOL_FLAT, // Return flattened values.
	PG_POOL_MANY, // Return grouped individual results.
	PG_POOL_FETCH, // Return only rows from multiple queries.
	PG_POOL_FETCH_VALUES, // Return multiple queries in column order.
	PG_POOL_FETCH_FLAT, // Return flattened values from multiple queries.
	PG_POOL_EXEC, // Return only the completed-update count.
};

// Start the selected query on a borrowed connection.
void GDPostgresPoolCall::start(const Ref<GDPostgresClient> &p_conn) {
	if (done || p_conn.is_null()) {
		return;
	}
	conn = p_conn;
	if (kind == PG_POOL_ACQUIRE) {
		pending = pg_ok(Ref<GDPostgresPoolCall>(this));
		pending_ready = true;
		delivery_posted = true;
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDPostgresPoolCall::deliver_lease));
		return;
	}
	const uint64_t full_wait = conn->wait_ms;
	if (due > 0) {
		const uint64_t now = GDClock::msec();
		if (now >= due) {
			fail_later(pg_fail("postgres pool wait timed out", Err::TIMED_OUT));
			return;
		}
		conn->wait_ms = MAX(uint64_t(1), due - now);
	}
	if (kind == PG_POOL_ROWS) {
		stream = conn->start_rows(sql, args, true);
		conn->wait_ms = full_wait;
		due = 0;
		if (stream.is_null() || stream->pg.is_null()) {
			fail_later(pg_fail("postgres pool Rows did not start", Err::INTERRUPTED));
			return;
		}
		inner = stream->pg;
		inner->pool_call = this; // Receive the Rows protocol boundary directly without an intermediate signal.
		args = Array();
		sql = String();
		pending = pg_ok(stream); // Retain Rows through completion-queue delivery even if the protocol finishes first.
		pending_ready = true;
		delivery_posted = true;
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDPostgresPoolCall::deliver_stream));
		return;
	}
	Array rows;
	bool many = false;
	bool discard = false;
	bool flat = false;
	bool values = false;
	bool flat_values = false;
	bool first = false;
	switch (kind) {
		case PG_POOL_QUERY:
			break;
		case PG_POOL_LIMITED:
			break;
		case PG_POOL_ROW:
			first = true;
			break;
		case PG_POOL_VALUES:
			values = true;
			break;
		case PG_POOL_FLAT:
			values = true;
			flat_values = true;
			break;
		case PG_POOL_MANY:
			many = true;
			break;
		case PG_POOL_FETCH:
			many = true;
			flat = true;
			break;
		case PG_POOL_FETCH_VALUES:
			many = true;
			flat = true;
			values = true;
			break;
		case PG_POOL_FETCH_FLAT:
			many = true;
			flat = true;
			values = true;
			flat_values = true;
			break;
		case PG_POOL_EXEC:
			many = true;
			discard = true;
			break;
		default:
			conn->wait_ms = full_wait;
			fail_later(pg_fail("invalid postgres pool query", Err::INVALID_DATA));
			return;
	}
	if (many) {
		rows = args;
	} else {
		rows.push_back(args);
	}
	Signal signal = conn->send_rows(sql, rows, many, discard, flat, max_rows, max_bytes, values, flat_values, first, true);
	conn->wait_ms = full_wait;
	due = 0;
	inner = Ref<GDPostgresCallInternal>(Object::cast_to<GDPostgresCallInternal>(signal.get_object()));
	if (inner.is_null()) {
		fail_later(pg_fail("postgres pool query did not start", Err::INTERRUPTED));
		return;
	}
	inner->pool_call = this; // Deliver directly into pool-operation state without an internal signal round trip.
	args = Array(); // Release the waiting snapshot after transferring arguments to the query.
	sql = String();
}

// Deliver connection results to the caller.
void GDPostgresPoolCall::answered(const VariantPair &p_result) {
	if (notified || delivery_posted) {
		return;
	}
	// Ordinary results arrive at the protocol boundary; cancellation must still await draining.
	if (inner.is_valid() && !inner->dropped) {
		finish(p_result);
		return;
	}
	post_result(p_result, false);
}

// Return canceled-query connections only after reaching the protocol boundary.
void GDPostgresPoolCall::settled() {
	if (kind == PG_POOL_ROWS && !notified && stream.is_valid()) {
		if (!delivery_posted) {
			post_result(pg_ok(stream), true);
		} else {
			release();
		}
		return;
	}
	release();
}

// Deliver pool-side failures on the next turn.
void GDPostgresPoolCall::fail_later(const VariantPair &p_result) {
	if (done || notified || delivery_posted) {
		return;
	}
	finish(p_result);
}

// Emit the exact completed value and its existing error details.
void GDPostgresPoolCall::emit_result(const VariantPair &p_result) {
	Async::finish(this, SNAME("finished"), p_result.value, p_result.error);
}

// Deliver the retained result once from the completion queue.
void GDPostgresPoolCall::deliver() {
	delivery_posted = false;
	if (notified || !pending_ready) {
		return;
	}
	const VariantPair out = pending;
	pending = VariantPair();
	pending_ready = false;
	notified = true;
	Ref<GDPostgresPoolCall> keep(this);
	emit_result(out);
}

// Deliver the exclusive transaction connection after its receiver attaches to the signal.
void GDPostgresPoolCall::deliver_lease() {
	delivery_posted = false;
	if (done || notified || !pending_ready) {
		return;
	}
	if (conn.is_null() || !conn->is_open() || pool.is_null() || !pool->leased.has(conn.ptr())) {
		pending = VariantPair();
		pending_ready = false;
		finish(pg_fail("postgres pool was closed before acquire", Err::INTERRUPTED));
		return;
	}
	if (due > 0 && GDClock::msec() >= due) {
		pending = VariantPair();
		pending_ready = false;
		finish(pg_fail("postgres pool wait timed out", Err::TIMED_OUT));
		return;
	}
	const VariantPair out = pending;
	pending = VariantPair();
	pending_ready = false;
	notified = true;
	Ref<GDPostgresPoolCall> keep(this);
	emit_result(out);
}

// Deliver sequential Rows while retaining the connection through the protocol boundary.
void GDPostgresPoolCall::deliver_stream() {
	delivery_posted = false;
	if (notified || !pending_ready) {
		return;
	}
	if (!done && (conn.is_null() || !conn->is_open() || pool.is_null() || !pool->leased.has(conn.ptr()))) {
		pending = VariantPair();
		pending_ready = false;
		stream->close();
		stream.unref();
		finish(pg_fail("postgres pool was closed before Rows started", Err::INTERRUPTED));
		return;
	}
	const VariantPair out = pending;
	pending = VariantPair();
	pending_ready = false;
	notified = true;
	Ref<GDPostgresPoolCall> keep(this);
	stream.unref();
	emit_result(out);
}

// Retain a result and schedule it once for the next event-loop turn.
void GDPostgresPoolCall::post_result(const VariantPair &p_result, bool p_release) {
	if (notified || delivery_posted) {
		return;
	}
	pending = p_result;
	pending_ready = true;
	delivery_posted = true;
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDPostgresPoolCall::deliver));
	if (p_release) {
		release();
	}
}

// Return the borrowed connection and enqueue the retained result for completion.
void GDPostgresPoolCall::finish(const VariantPair &p_result) {
	if (done || notified || delivery_posted) {
		return;
	}
	post_result(p_result, true);
}

// Return the connection to its pool and release retained values.
void GDPostgresPoolCall::release() {
	if (done) {
		return;
	}
	done = true;
	Ref<GDPostgresPoolCall> keep(this);
	const Ref<GDPostgresPool> owner = pool;
	const Ref<GDPostgresClient> used = conn;
	inner.unref();
	stream.unref();
	conn.unref();
	pool.unref();
	args = Array();
	sql = String();
	if (owner.is_valid() && used.is_valid()) {
		owner->released(used);
	}
	self_hold.unref();
}

// Remove waiting operations or close the borrowed connection to stop active work.
void GDPostgresPoolCall::cancel() {
	if (done) {
		return;
	}
	if (conn.is_valid()) {
		conn->close();
		if (kind == PG_POOL_ACQUIRE && notified) release();
		return;
	}
	if (pool.is_valid()) {
		pool->cancel_wait(this);
	}
	fail_later(pg_fail("cancelled", Err::INTERRUPTED));
}

// Free a pool slot when its abandoned query cannot be safely awaited.
void GDPostgresPoolCall::abort() {
	if (done) return;
	if (conn.is_valid()) conn->close();
	else cancel();
}

// Register completion signals and cancellation.
void GDPostgresPoolCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDPostgresPoolCall::cancel);
	ClassDB::bind_method(D_METHOD("abort"), &GDPostgresPoolCall::abort);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}

// Leave the default connection count uncapped while creating only connections in demand.
GDPostgresPool::GDPostgresPool() {
	default_size = INT_MAX;
	max_open = default_size;
}

// Retain an explicit factory capacity or select the uncapped default.
void GDPostgresPool::set_default_size(int64_t p_size) {
	default_size = p_size == 0 ? INT_MAX : (p_size > 0 && p_size <= INT_MAX ? int(p_size) : 0);
	max_open = default_size;
}

// Configure destination and capacity, creating physical connections only as demand requires.
Signal GDPostgresPool::open(const String &p_host, int64_t p_port, const Dictionary &p_opts, int64_t p_size) {
	if (p_size < 0 || p_size > INT_MAX) {
		return Async::ready_pair({ Variant(), Err::make("pool size must be between 0 and 2147483647", Err::INVALID_DATA) });
	}
	if (p_host.is_empty() || p_port < Limit::PORT_MIN || p_port > Limit::PORT_MAX) {
		return Async::ready_pair({ Variant(), Err::make("postgres address is invalid", Err::INVALID_DATA) });
	}
	const int want = p_size > 0 ? int(p_size) : default_size;
	if (want < 1) {
		return Async::ready_pair({ Variant(), Err::make("pool size must be a positive 32-bit integer", Err::INVALID_DATA) });
	}
	uint64_t wait = 0;
	uint64_t connect_wait = 0;
	int64_t idle = 2;
	if (!DbOption::seconds(p_opts, "timeout", 0.0, wait) ||
			!DbOption::seconds(p_opts, "connect_timeout", 0.0, connect_wait)) {
		return Async::ready_pair({ Variant(), Err::make("timeouts must be zero or a positive number of seconds", Err::INVALID_DATA) });
	}
	if (!DbOption::integer(p_opts, "max_idle", 2, idle) || idle > INT_MAX) {
		return Async::ready_pair({ Variant(), Err::make("max_idle must be a 32-bit integer", Err::INVALID_DATA) });
	}
	String auth, ca_path;
	Wire::Guard guard;
	if (!DbOption::text(p_opts, "auth", "any", auth) || !DbOption::text(p_opts, "ca", "", ca_path) || !pg_credentials(p_opts) ||
			(auth != "any" && auth != "scram" && auth != "md5") ||
			!Wire::guard_of(p_opts.get("tls", Wire::default_guard(p_host)), guard)) {
		return Async::ready_pair({ Variant(), Err::make("postgres pool connection options are invalid", Err::INVALID_DATA) });
	}
	close();
	host = p_host;
	port = int(p_port);
	opts = p_opts.duplicate(true);
	opts.erase("max_idle");
	max_open = want;
	max_idle = int(MIN(int64_t(want), MAX(int64_t(0), idle)));
	query_wait_ms = wait;
	configured = true;
	return Async::ready_pair({ Variant(), Variant() });
}

// Borrow only an idle connection so independent operations cannot affect one another.
Ref<GDPostgresClient> GDPostgresPool::pick() const {
	for (const Ref<GDPostgresClient> &c : conns) {
		if (c.is_null() || !c->is_open() || c->tx_status != 'I' || leased.has(c.ptr())) {
			continue;
		}
		if (c->in_flight() != 0) continue;
		// Detect an idle peer close before assigning a new query to this connection.
		c->sock.poll();
		if (c->is_open()) return c;
		c->close();
	}
	return Ref<GDPostgresClient>();
}

// Borrow a connection, waiting in arrival order when all are occupied.
Signal GDPostgresPool::send(int p_kind, const String &p_sql, const Array &p_args, int p_max_rows, int64_t p_max_bytes) {
	if (!configured) {
		return no_conn();
	}
	Ref<GDPostgresPoolCall> call;
	call.instantiate();
	call->self_hold = call;
	call->pool = Ref<GDPostgresPool>(this);
	call->kind = p_kind;
	call->sql = p_sql;
	call->args = p_args;
	call->max_rows = p_max_rows;
	call->max_bytes = p_max_bytes;
	call->due = deadline_after(query_wait_ms);
	const Ref<GDPostgresClient> c = waits.is_empty() ? pick() : Ref<GDPostgresClient>();
	if (c.is_valid()) {
		leased.insert(c.ptr());
		call->start(c);
	} else {
		call->args = p_args.duplicate(true);
		trim_closed();
		if (int64_t(conns.size()) + opening >= max_open) {
			call->queued_at = GDClock::usec();
			wait_count++;
		}
		waits.push_back(call);
		call->wait_entry = waits.back();
		if (call->due > 0) {
			deadlines.insert(GDPostgresPoolDeadline{ call->due, call.ptr() });
			sync_wait();
		}
		dispatch();
	}
	return Signal(call.ptr(), "finished");
}

// Remove closed connections so capacity reflects usable resources.
void GDPostgresPool::trim_closed() {
	for (int i = conns.size() - 1; i >= 0; i--) {
		const Ref<GDPostgresClient> &conn = conns[i];
		if (conn.is_null() || (!conn->is_open() && !leased.has(conn.ptr()))) {
			conns.remove_at(i);
		}
	}
}

// Release idle connections above the configured retention count.
void GDPostgresPool::trim_idle(const Ref<GDPostgresClient> &p_preferred) {
	if (!configured) return;
	int idle = 0;
	for (const Ref<GDPostgresClient> &conn : conns) {
		if (conn.is_valid() && conn->is_open() && conn->tx_status == 'I' && conn->in_flight() == 0 && !leased.has(conn.ptr())) idle++;
	}
	if (idle <= max_idle) return;
	if (p_preferred.is_valid() && p_preferred->is_open() && p_preferred->tx_status == 'I' &&
			p_preferred->in_flight() == 0 && !leased.has(p_preferred.ptr())) {
		p_preferred->close();
		max_idle_closed++;
		idle--;
	}
	for (int i = conns.size() - 1; i >= 0 && idle > max_idle; i--) {
		const Ref<GDPostgresClient> &conn = conns[i];
		if (conn.is_valid() && conn->is_open() && conn->tx_status == 'I' && conn->in_flight() == 0 && !leased.has(conn.ptr())) {
			conn->close();
			max_idle_closed++;
			idle--;
		}
	}
	trim_closed();
}

// Create only the missing connections needed by waiters; capacity is not a preallocation target.
void GDPostgresPool::grow() {
	trim_closed();
	while (configured && opening < waits.size() && int64_t(conns.size()) + opening < max_open) {
		Ref<GDPostgresClient> conn;
		conn.instantiate();
		opening++;
		const uint64_t current = generation;
		conn->open(host, port, opts).connect(callable_mp(this, &GDPostgresPool::opened).bind(conn, current), Object::CONNECT_ONE_SHOT);
	}
}

// Adopt a lazy connection or report its failure to the corresponding first waiter.
void GDPostgresPool::opened(const Variant &p_value, const Variant &p_error, const Ref<GDPostgresClient> &p_conn, uint64_t p_generation) {
	if (!configured || p_generation != generation) {
		if (p_conn.is_valid()) {
			p_conn->close();
		}
		return;
	}
	opening = MAX(0, opening - 1);
	if (p_error.get_type() == Variant::NIL && p_conn.is_valid() && p_conn->is_open()) {
		conns.push_back(p_conn);
	} else if (!waits.is_empty()) {
		Ref<GDPostgresPoolCall> call = waits.front()->get();
		record_wait(call.ptr());
		waits.pop_front();
		call->fail_later(p_error.get_type() == Variant::OBJECT ? pg_fail(Ref<Err>(p_error), Err::NONE, p_value) : pg_fail("postgres connection failed", Err::INTERRUPTED));
	}
	dispatch();
	trim_idle(p_conn);
}

// Accumulate each acquisition wait once.
void GDPostgresPool::record_wait(GDPostgresPoolCall *p_call) {
	if (!p_call) {
		return;
	}
	if (p_call->due > 0) {
		deadlines.erase(GDPostgresPoolDeadline{ p_call->due, p_call });
	}
	p_call->wait_entry = nullptr;
	if (p_call->queued_at == 0) {
		return;
	}
	const uint64_t now = GDClock::usec();
	wait_usec += int64_t(now >= p_call->queued_at ? now - p_call->queued_at : 0);
	p_call->queued_at = 0;
}

// Register only the earliest deadline with runtime timers.
void GDPostgresPool::sync_wait() {
	const uint64_t next = deadlines.is_empty() ? 0 : deadlines.front()->get().due;
	if (wait_due != next) {
		Async::drop_deadline(this, wait_due);
		wait_due = next;
		Async::track_deadline(this, wait_due, callable_mp(this, &GDPostgresPool::step));
	}
}

// Finish expired waiters together in deadline order.
void GDPostgresPool::step() {
	if (wait_due == 0 || GDClock::msec() < wait_due) {
		return;
	}
	LocalVector<Ref<GDPostgresPoolCall>> expired;
	const uint64_t now = GDClock::msec();
	while (!deadlines.is_empty() && deadlines.front()->get().due <= now) {
		GDPostgresPoolCall *raw = deadlines.front()->get().call;
		if (!raw || !raw->wait_entry) {
			deadlines.erase(deadlines.front()->get());
			continue;
		}
		Ref<GDPostgresPoolCall> call = raw->wait_entry->get();
		List<Ref<GDPostgresPoolCall>>::Element *entry = raw->wait_entry;
		record_wait(raw);
		waits.erase(entry);
		expired.push_back(call);
	}
	sync_wait();
	for (const Ref<GDPostgresPoolCall> &call : expired) {
		call->fail_later(pg_fail("postgres pool wait timed out", Err::TIMED_OUT));
	}
	dispatch(); // Expired waiters no longer block later acquisitions.
}

// Assign available connections to waiting queries in arrival order.
void GDPostgresPool::dispatch() {
	if (!configured) return; // Closing connections may synchronously release canceled operations.
	trim_closed();
	while (!waits.is_empty()) {
		const Ref<GDPostgresClient> c = pick();
		if (c.is_null()) {
			grow();
			sync_wait();
			return;
		}
		Ref<GDPostgresPoolCall> call = waits.front()->get();
		record_wait(call.ptr());
		waits.pop_front();
		if (call->done || (call->due > 0 && GDClock::msec() >= call->due)) {
			call->fail_later(pg_fail("postgres pool wait timed out", Err::TIMED_OUT));
			continue;
		}
		leased.insert(c.ptr());
		call->start(c);
	}
	sync_wait();
}

// Remove only canceled waiters from the queue.
void GDPostgresPool::cancel_wait(GDPostgresPoolCall *p_call) {
	if (p_call && p_call->wait_entry) {
		List<Ref<GDPostgresPoolCall>>::Element *entry = p_call->wait_entry;
		record_wait(p_call);
		waits.erase(entry);
		sync_wait();
		dispatch();
	}
}

// Lend a completed query's connection to the next waiter.
void GDPostgresPool::released(const Ref<GDPostgresClient> &p_conn) {
	if (p_conn.is_valid()) {
		leased.erase(p_conn.ptr());
	}
	// A returned connection must not transfer an unfinished transaction to another caller.
	if (p_conn.is_valid() && p_conn->is_open() && (p_conn->tx_status != 'I' || p_conn->in_flight() > 0)) {
		p_conn->close();
	}
	dispatch();
	trim_idle(p_conn);
}

// Report an unopened pool only after the caller can attach its waiter.
Signal GDPostgresPool::no_conn() {
	Ref<GDPostgresCallInternal> call;
	call.instantiate();
	call->self_hold = call;
	call->fail_later(pg_fail("pool is not open", Err::NOT_FOUND));
	return Signal(call.ptr(), "finished");
}

// Execute a query and return its result.
Signal GDPostgresPool::query(const String &p_sql, const Array &p_args) {
	return send(PG_POOL_QUERY, p_sql, p_args);
}

// Send a result-limited query through an available connection.
Signal GDPostgresPool::query_limited(const String &p_sql, const Array &p_args, int p_max_rows, int64_t p_max_bytes) {
	return send(PG_POOL_LIMITED, p_sql, p_args, p_max_rows, p_max_bytes);
}

// Return only the first row through an available connection.
Signal GDPostgresPool::query_row(const String &p_sql, const Array &p_args, int64_t p_max_bytes) {
	return send(PG_POOL_ROW, p_sql, p_args, 0, p_max_bytes);
}

// Return rows sequentially while retaining the borrowed connection until Rows closes.
Signal GDPostgresPool::query_rows(const String &p_sql, const Array &p_args) {
	return send(PG_POOL_ROWS, p_sql, p_args);
}

// Request column-ordered rows through an available connection.
Signal GDPostgresPool::query_values(const String &p_sql, const Array &p_args) {
	return send(PG_POOL_VALUES, p_sql, p_args);
}

// Request flattened values through an available connection.
Signal GDPostgresPool::query_flat(const String &p_sql, const Array &p_args) {
	return send(PG_POOL_FLAT, p_sql, p_args);
}

// Batch queries through one connection for a single round trip.
Signal GDPostgresPool::query_many(const String &p_sql, const Array &p_rows) {
	return send(PG_POOL_MANY, p_sql, p_rows);
}

// Send a row-only batch through an available connection.
Signal GDPostgresPool::fetch_many(const String &p_sql, const Array &p_rows) {
	return send(PG_POOL_FETCH, p_sql, p_rows);
}

// Send a column-ordered row batch through an available connection.
Signal GDPostgresPool::fetch_values_many(const String &p_sql, const Array &p_rows) {
	return send(PG_POOL_FETCH_VALUES, p_sql, p_rows);
}

// Send a flat-value batch through an available connection.
Signal GDPostgresPool::fetch_flat_many(const String &p_sql, const Array &p_rows) {
	return send(PG_POOL_FETCH_FLAT, p_sql, p_rows);
}

// Batch updates and return only the completion count without individual results.
Signal GDPostgresPool::exec_many(const String &p_sql, const Array &p_rows) {
	return send(PG_POOL_EXEC, p_sql, p_rows);
}

// Acquire a transaction connection through the ordinary FIFO query queue.
Signal GDPostgresPool::acquire() {
	return send(PG_POOL_ACQUIRE, String(), Array());
}

// Close and release retained resources.
void GDPostgresPool::close() {
	configured = false;
	generation++;
	opening = 0;
	LocalVector<Ref<GDPostgresPoolCall>> pending;
	while (!waits.is_empty()) {
		Ref<GDPostgresPoolCall> call = waits.front()->get();
		record_wait(call.ptr());
		waits.pop_front();
		pending.push_back(call);
	}
	sync_wait();
	for (Ref<GDPostgresClient> &c : conns) {
		if (c.is_valid()) {
			c->close();
		}
	}
	conns.clear();
	leased.clear();
	host = String();
	opts = Dictionary();
	query_wait_ms = 0;
	for (const Ref<GDPostgresPoolCall> &call : pending) {
		call->fail_later(pg_fail("pool is closed", Err::INTERRUPTED));
	}
}

// Return the number of active queries.
int GDPostgresPool::in_flight() const {
	int n = 0;
	for (const Ref<GDPostgresClient> &c : conns) {
		if (c.is_valid()) {
			n += c->in_flight();
		}
	}
	return n + waits.size();
}

// Return connection counts, occupancy, and cumulative wait statistics.
Dictionary GDPostgresPool::stats() const {
	int open = 0;
	int used = 0;
	for (const Ref<GDPostgresClient> &c : conns) {
		if (c.is_valid() && c->is_open()) {
			open++;
			used += leased.has(c.ptr()) || c->in_flight() > 0 ? 1 : 0;
		}
	}
	Dictionary out;
	out["max_open_connections"] = max_open == INT_MAX ? 0 : max_open;
	out["open_connections"] = open;
	out["in_use"] = used;
	out["idle"] = open - used;
	out["wait_count"] = wait_count;
	out["wait_duration_ms"] = wait_usec / 1000;
	out["max_idle_closed"] = max_idle_closed;
	out["max_idle_time_closed"] = int64_t(0); // No idle timeout is configured.
	out["max_lifetime_closed"] = int64_t(0); // No connection lifetime is configured.
	return out;
}

// Check whether any connection remains able to accept queries.
bool GDPostgresPool::is_open() const {
	return configured;
}

// Register public methods and properties with script.
void GDPostgresPool::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "host", "port", "opts", "size"), &GDPostgresPool::open, DEFVAL("127.0.0.1"), DEFVAL(5432), DEFVAL(Dictionary()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("query", "sql", "args"), &GDPostgresPool::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_row", "sql", "args", "max_bytes"), &GDPostgresPool::query_row, DEFVAL(Array()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("query_rows", "sql", "args"), &GDPostgresPool::query_rows, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_values", "sql", "args"), &GDPostgresPool::query_values, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_flat", "sql", "args"), &GDPostgresPool::query_flat, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_many", "sql", "rows"), &GDPostgresPool::query_many);
	ClassDB::bind_method(D_METHOD("fetch_many", "sql", "rows"), &GDPostgresPool::fetch_many);
	ClassDB::bind_method(D_METHOD("fetch_values_many", "sql", "rows"), &GDPostgresPool::fetch_values_many);
	ClassDB::bind_method(D_METHOD("fetch_flat_many", "sql", "rows"), &GDPostgresPool::fetch_flat_many);
	ClassDB::bind_method(D_METHOD("exec_many", "sql", "rows"), &GDPostgresPool::exec_many);
	ClassDB::bind_method(D_METHOD("open_async", "host", "port", "opts", "size"), &GDPostgresPool::open, DEFVAL("127.0.0.1"), DEFVAL(5432), DEFVAL(Dictionary()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("query_async", "sql", "args"), &GDPostgresPool::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_row_async", "sql", "args", "max_bytes"), &GDPostgresPool::query_row, DEFVAL(Array()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("query_rows_async", "sql", "args"), &GDPostgresPool::query_rows, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_values_async", "sql", "args"), &GDPostgresPool::query_values, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_flat_async", "sql", "args"), &GDPostgresPool::query_flat, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_many_async", "sql", "rows"), &GDPostgresPool::query_many);
	ClassDB::bind_method(D_METHOD("fetch_many_async", "sql", "rows"), &GDPostgresPool::fetch_many);
	ClassDB::bind_method(D_METHOD("fetch_values_many_async", "sql", "rows"), &GDPostgresPool::fetch_values_many);
	ClassDB::bind_method(D_METHOD("fetch_flat_many_async", "sql", "rows"), &GDPostgresPool::fetch_flat_many);
	ClassDB::bind_method(D_METHOD("exec_many_async", "sql", "rows"), &GDPostgresPool::exec_many);
	ADD_AWAIT("open", "Pair:Variant");
	ADD_AWAIT("query", "Pair:Dictionary");
	ADD_AWAIT("query_row", "Pair:Dictionary");
	ADD_AWAIT("query_rows", "Pair:GDDatabaseRows");
	ADD_AWAIT("query_values", "Pair:Dictionary");
	ADD_AWAIT("query_flat", "Pair:Dictionary");
	ADD_AWAIT("query_many", "Pair:Array");
	ADD_AWAIT("fetch_many", "Pair:Array");
	ADD_AWAIT("fetch_values_many", "Pair:Array");
	ADD_AWAIT("fetch_flat_many", "Pair:Array");
	ADD_AWAIT("exec_many", "Pair:int");
	ADD_AWAIT("open_async", "Pair:Variant");
	ADD_AWAIT("query_async", "Pair:Dictionary");
	ADD_AWAIT("query_row_async", "Pair:Dictionary");
	ADD_AWAIT("query_rows_async", "Pair:GDDatabaseRows");
	ADD_AWAIT("query_values_async", "Pair:Dictionary");
	ADD_AWAIT("query_flat_async", "Pair:Dictionary");
	ADD_AWAIT("query_many_async", "Pair:Array");
	ADD_AWAIT("fetch_many_async", "Pair:Array");
	ADD_AWAIT("fetch_values_many_async", "Pair:Array");
	ADD_AWAIT("fetch_flat_many_async", "Pair:Array");
	ADD_AWAIT("exec_many_async", "Pair:int");
	ADD_AUTO_WAIT("open");
	ADD_AUTO_WAIT("query");
	ADD_AUTO_WAIT("query_row");
	ADD_AUTO_WAIT("query_rows");
	ADD_AUTO_WAIT("query_values");
	ADD_AUTO_WAIT("query_flat");
	ADD_AUTO_WAIT("query_many");
	ADD_AUTO_WAIT("fetch_many");
	ADD_AUTO_WAIT("fetch_values_many");
	ADD_AUTO_WAIT("fetch_flat_many");
	ADD_AUTO_WAIT("exec_many");
	ClassDB::bind_method(D_METHOD("close"), &GDPostgresPool::close);
	ClassDB::bind_method(D_METHOD("size"), &GDPostgresPool::size);
	ClassDB::bind_method(D_METHOD("in_flight"), &GDPostgresPool::in_flight);
	ClassDB::bind_method(D_METHOD("stats"), &GDPostgresPool::stats);
}

// Register public methods and properties with script.
void GDPostgresClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "host", "port", "opts"), &GDPostgresClient::open, DEFVAL("127.0.0.1"), DEFVAL(5432), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("query", "sql", "args"), &GDPostgresClient::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_row", "sql", "args", "max_bytes"), &GDPostgresClient::query_row, DEFVAL(Array()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("query_rows", "sql", "args"), &GDPostgresClient::query_rows, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_values", "sql", "args"), &GDPostgresClient::query_values, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_flat", "sql", "args"), &GDPostgresClient::query_flat, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_many", "sql", "rows"), &GDPostgresClient::query_many);
	ClassDB::bind_method(D_METHOD("fetch_many", "sql", "rows"), &GDPostgresClient::fetch_many);
	ClassDB::bind_method(D_METHOD("fetch_values_many", "sql", "rows"), &GDPostgresClient::fetch_values_many);
	ClassDB::bind_method(D_METHOD("fetch_flat_many", "sql", "rows"), &GDPostgresClient::fetch_flat_many);
	ClassDB::bind_method(D_METHOD("exec_many", "sql", "rows"), &GDPostgresClient::exec_many);
	ClassDB::bind_method(D_METHOD("open_async", "host", "port", "opts"), &GDPostgresClient::open, DEFVAL("127.0.0.1"), DEFVAL(5432), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("query_async", "sql", "args"), &GDPostgresClient::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_row_async", "sql", "args", "max_bytes"), &GDPostgresClient::query_row, DEFVAL(Array()), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("query_rows_async", "sql", "args"), &GDPostgresClient::query_rows, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_values_async", "sql", "args"), &GDPostgresClient::query_values, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_flat_async", "sql", "args"), &GDPostgresClient::query_flat, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_many_async", "sql", "rows"), &GDPostgresClient::query_many);
	ClassDB::bind_method(D_METHOD("fetch_many_async", "sql", "rows"), &GDPostgresClient::fetch_many);
	ClassDB::bind_method(D_METHOD("fetch_values_many_async", "sql", "rows"), &GDPostgresClient::fetch_values_many);
	ClassDB::bind_method(D_METHOD("fetch_flat_many_async", "sql", "rows"), &GDPostgresClient::fetch_flat_many);
	ClassDB::bind_method(D_METHOD("exec_many_async", "sql", "rows"), &GDPostgresClient::exec_many);
	ADD_AWAIT("open", "Pair:Variant");
	ADD_AWAIT("query", "Pair:Dictionary");
	ADD_AWAIT("query_row", "Pair:Dictionary");
	ADD_AWAIT("query_rows", "Pair:GDDatabaseRows");
	ADD_AWAIT("query_values", "Pair:Dictionary");
	ADD_AWAIT("query_flat", "Pair:Dictionary");
	ADD_AWAIT("query_many", "Pair:Array");
	ADD_AWAIT("fetch_many", "Pair:Array");
	ADD_AWAIT("fetch_values_many", "Pair:Array");
	ADD_AWAIT("fetch_flat_many", "Pair:Array");
	ADD_AWAIT("exec_many", "Pair:int");
	ADD_AWAIT("open_async", "Pair:Variant");
	ADD_AWAIT("query_async", "Pair:Dictionary");
	ADD_AWAIT("query_row_async", "Pair:Dictionary");
	ADD_AWAIT("query_rows_async", "Pair:GDDatabaseRows");
	ADD_AWAIT("query_values_async", "Pair:Dictionary");
	ADD_AWAIT("query_flat_async", "Pair:Dictionary");
	ADD_AWAIT("query_many_async", "Pair:Array");
	ADD_AWAIT("fetch_many_async", "Pair:Array");
	ADD_AWAIT("fetch_values_many_async", "Pair:Array");
	ADD_AWAIT("fetch_flat_many_async", "Pair:Array");
	ADD_AWAIT("exec_many_async", "Pair:int");
	ADD_AUTO_WAIT("open");
	ADD_AUTO_WAIT("query");
	ADD_AUTO_WAIT("query_row");
	ADD_AUTO_WAIT("query_rows");
	ADD_AUTO_WAIT("query_values");
	ADD_AUTO_WAIT("query_flat");
	ADD_AUTO_WAIT("query_many");
	ADD_AUTO_WAIT("fetch_many");
	ADD_AUTO_WAIT("fetch_values_many");
	ADD_AUTO_WAIT("fetch_flat_many");
	ADD_AUTO_WAIT("exec_many");
	ClassDB::bind_method(D_METHOD("is_open"), &GDPostgresClient::is_open);
	ClassDB::bind_method(D_METHOD("close"), &GDPostgresClient::close);
	ClassDB::bind_method(D_METHOD("check", "sql"), &GDPostgresClient::check);
	ADD_AWAIT("check", "Pair:Variant");
	ClassDB::bind_method(D_METHOD("check_async", "sql"), &GDPostgresClient::check);
	ADD_AWAIT("check_async", "Pair:Variant");
	ADD_AUTO_WAIT("check");
	ClassDB::bind_method(D_METHOD("in_flight"), &GDPostgresClient::in_flight);
	ClassDB::bind_method(D_METHOD("cached_stmts"), &GDPostgresClient::cached_stmts);
}
