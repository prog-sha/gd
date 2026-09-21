/**************************************************************************/
/*  format.cpp                                                            */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement human-oriented format parsing and serialization declared in format.h.
// Keep result shapes consistent with their public adapters.

#include "cli/data/format.h"
#include "cli/sys/pool.h"

#include "cli/api/text.h"
#include "cli/data/json.h"
#include "cli/sys/os.h"

#include "core/core_bind.h"
#include "core/io/xml_parser.h"
#include "core/templates/local_vector.h"

namespace {

// Reject runtime behavior, cycles, and nesting outside the data representation.
bool tree_ok(const Variant &p_v, int p_depth, HashSet<const void *> &r_active) {
	if (p_depth >= Variant::MAX_RECURSION_DEPTH || p_v.get_type() == Variant::OBJECT ||
			p_v.get_type() == Variant::CALLABLE || p_v.get_type() == Variant::SIGNAL) {
		return false;
	}
	if (p_v.get_type() == Variant::ARRAY) {
		const Array a = p_v;
		if (r_active.has(a.id())) {
			return false;
		}
		r_active.insert(a.id());
		for (int i = 0; i < a.size(); i++) {
			if (!tree_ok(a[i], p_depth + 1, r_active)) {
				r_active.erase(a.id());
				return false;
			}
		}
		r_active.erase(a.id());
		return true;
	}
	if (p_v.get_type() == Variant::DICTIONARY) {
		const Dictionary d = p_v;
		if (r_active.has(d.id())) {
			return false;
		}
		r_active.insert(d.id());
		for (const KeyValue<Variant, Variant> &kv : d) {
			if (kv.key.get_type() == Variant::ARRAY || kv.key.get_type() == Variant::DICTIONARY || !tree_ok(kv.key, p_depth + 1, r_active)) {
				r_active.erase(d.id());
				return false;
			}
			if (!tree_ok(kv.value, p_depth + 1, r_active)) {
				r_active.erase(d.id());
				return false;
			}
		}
		r_active.erase(d.id());
	}
	return true;
}

// Apply shared cycle validation before public serialization.
bool tree_ok(const Variant &p_v, int p_depth = 0) {
	HashSet<const void *> active;
	return tree_ok(p_v, p_depth, active);
}

// Remove surrounding quotes and return their kind in r_quote.
// Leave format-specific escape decoding to the caller.
String unquote(const String &s, char32_t *r_quote = nullptr) {
	if (r_quote) {
		*r_quote = 0;
	}
	if (s.length() >= 2 && ((s.begins_with("\"") && s.ends_with("\"")) || (s.begins_with("'") && s.ends_with("'")))) {
		if (r_quote) {
			*r_quote = s[0];
		}
		return s.substr(1, s.length() - 2);
	}
	return s;
}

// Decode escapes left to right so escaped backslashes do not turn into newlines during bulk replacement.
String unesc_dq(const String &s) {
	String out;
	for (int i = 0; i < s.length(); i++) {
		if (s[i] != '\\' || i + 1 >= s.length()) {
			out += String::chr(s[i]);
			continue;
		}
		i++;
		switch (s[i]) {
			case 'n':
				out += "\n";
				break;
			case 'r':
				out += "\r";
				break;
			case 't':
				out += "\t";
				break;
			case '\\':
			case '"':
				out += String::chr(s[i]);
				break;
			default:
				out += "\\";
				out += String::chr(s[i]);
				break;
		}
	}
	return out;
}

// Determine whether structural characters require quoting and escaping.
bool needs_dq(const String &s) {
	// Quote values beginning with quote marks so the reader cannot strip literal content.
	return s.is_empty() || s.contains("\n") || s.contains("\r") || s.contains("\t") ||
			s.contains("\"") || s.contains("\\") || s != s.strip_edges() ||
			s.begins_with("'") || s.begins_with("\"");
}

// Quote and escape a value.
// Use matching writer and reader escapes for newlines and quote characters.
// Preserve structure when values are read back.
String as_dq(const String &s) {
	return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t") + "\"";
}

// Remove surrounding quotes and decode escapes only for double quotes.
String unquote_dq(const String &s) {
	char32_t q = 0;
	const String body = unquote(s, &q);
	return q == '"' ? unesc_dq(body) : body;
}

// Find the first marker outside quotes, or return -1.
// Return its position and leave format-specific context checks to the caller.
int mark_at(const String &s, char32_t p_mark, int p_from) {
	// Track the exact quote kind rather than a boolean.
	// A single quote inside double quotes must not change quoted state.
	char32_t quote = 0;
	for (int i = p_from; i < s.length(); i++) {
		const char32_t c = s[i];
		// Escaped quotes are content and do not toggle state.
		if (quote != 0 && c == '\\') {
			i++;
			continue;
		}
		if (quote != 0) {
			if (c == quote) {
				quote = 0;
			}
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == p_mark) {
			return i;
		}
	}
	return -1;
}

// Infer numeric or boolean types from unquoted notation.
Variant typed_of(const String &v) {
	const String low = v.to_lower();
	if (low == "true") {
		return true;
	}
	if (low == "false") {
		return false;
	}
	if (v.is_valid_int()) {
		return v.to_int();
	}
	if (v.is_valid_float()) {
		return v.to_float();
	}
	return unquote_dq(v);
}

// Split only outside brackets and quotes.
Vector<String> split_top(const String &s, char32_t sep) {
	Vector<String> out;
	int depth = 0;
	char32_t quote = 0; // Active quote kind, matching mark_at semantics.
	int start = 0;
	const int n = s.length();
	for (int i = 0; i < n; i++) {
		const char32_t c = s[i];
		if (quote != 0 && c == '\\') {
			i++; // An escaped quote is content.
			continue;
		}
		if (quote != 0) {
			if (c == quote) {
				quote = 0;
			}
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else {
			if (c == '[' || c == '{') {
				depth++;
			} else if (c == ']' || c == '}') {
				depth--;
			} else if (c == sep && depth == 0) {
				out.push_back(s.substr(start, i - start));
				start = i + 1;
			}
		}
	}
	out.push_back(s.substr(start));
	return out;
}

} // namespace

// ---------------- CSV ----------------

// Parse CSV records.
Ref<R> Csv::parse(const String &p_src, const String &p_sep) {
	const char32_t sep = p_sep.is_empty() ? ',' : p_sep[0];
	Array rows;
	Array row;
	String field;
	bool quoted = false; // Whether parsing is inside a quoted field.
	bool closed = false; // Whether a quoted field just ended, for detecting trailing characters.
	bool had = false; // Whether any content was read, for distinguishing blank lines.
	const int n = p_src.length();
	const char32_t *r = p_src.ptr();
	int i = 0;

	while (i < n) {
		const char32_t c = r[i];
		if (quoted) {
			if (c == '"') {
				if (i + 1 < n && r[i + 1] == '"') {
					field += "\"";
					i += 2;
					continue;
				}
				quoted = false;
				closed = true;
				i++;
				continue;
			}
			field += String::chr(c);
			i++;
			continue;
		}
		// Reject characters after a closing field quote.
		// Otherwise a quoted value with trailing text could collapse to the same value as unquoted input.
		// Only a separator or record ending may follow a quoted field.
		if (closed && c != sep && c != '\r' && c != '\n') {
			return R::err("extra character after quoted field", Err::INVALID_DATA);
		}
		if (c == '"') {
			// Reject quotes inside an unquoted field to prevent alternate ambiguous spellings.
			// A bare quote cannot appear in an unquoted field.
			if (!field.is_empty()) {
				return R::err("bare quote in non-quoted field", Err::INVALID_DATA);
			}
			quoted = true;
			had = true;
			i++;
			continue;
		}
		if (c == sep) {
			row.push_back(field);
			field = "";
			closed = false;
			had = true;
			i++;
			continue;
		}
		if (c == '\r') {
			i++;
			continue;
		}
		if (c == '\n') {
			if (had || !field.is_empty() || !row.is_empty()) {
				row.push_back(field);
				rows.push_back(row);
			}
			row = Array();
			field = "";
			closed = false;
			had = false;
			i++;
			continue;
		}
		field += String::chr(c);
		had = true;
		i++;
	}

	if (quoted) {
		return R::err("unterminated quote", Err::INVALID_DATA);
	}
	if (had || !field.is_empty() || !row.is_empty()) {
		row.push_back(field);
		rows.push_back(row);
	}
	return R::ok(rows);
}

// Serialize CSV rows.
String Csv::stringify(const Array &p_rows, const String &p_sep) {
	const String sep = p_sep.is_empty() ? String(",") : p_sep;
	String out;
	for (int i = 0; i < p_rows.size(); i++) {
		const Array cells = p_rows[i];
		for (int c = 0; c < cells.size(); c++) {
			if (c > 0) {
				out += sep;
			}
			const String s = Pool::text(cells[c]);
			// Quote fields containing separators, quotes, or newlines, and also a single empty-field record.
			// An unquoted empty record would be skipped as a blank line by the reader.
			if (s.contains(sep) || s.contains("\"") || s.contains("\n") || s.contains("\r") ||
					(s.is_empty() && cells.size() == 1)) {
				out += "\"" + s.replace("\"", "\"\"") + "\"";
			} else {
				out += s;
			}
		}
		out += "\n";
	}
	return out;
}

// Parse CSV rows as dictionaries using the first row's field names.
Ref<R> Csv::parse_objects(const String &p_src, const String &p_sep) {
	const Ref<R> got = parse(p_src, p_sep);
	if (got->get_e().is_valid()) {
		return got;
	}
	const Array rows = got->get_v();
	if (rows.is_empty()) {
		return R::ok(Array());
	}
	const Array head = rows[0];
	Array out;
	for (int r = 1; r < rows.size(); r++) {
		const Array row = rows[r];
		// Reject rows with more fields than the header.
		// Silently dropping extras would make distinct records indistinguishable.
		// Fill missing fields with empty values up to the header width.
		if (row.size() > head.size()) {
			return R::err(vformat("row %d has %d fields but the header has %d", r, row.size(), head.size()), Err::INVALID_DATA);
		}
		Dictionary obj;
		for (int c = 0; c < head.size(); c++) {
			obj[Pool::text(head[c])] = c < row.size() ? row[c] : Variant("");
		}
		out.push_back(obj);
	}
	return R::ok(out);
}

// Serialize dictionaries as CSV with a header row.
String Csv::stringify_objects(const Array &p_items, const String &p_sep) {
	if (p_items.is_empty()) {
		return String();
	}
	const Dictionary first = p_items[0];
	const Array head = first.keys();
	Array rows;
	rows.push_back(head);
	for (int i = 0; i < p_items.size(); i++) {
		const Dictionary obj = p_items[i];
		Array row;
		for (int c = 0; c < head.size(); c++) {
			row.push_back(obj.get(head[c], ""));
		}
		rows.push_back(row);
	}
	return stringify(rows, p_sep);
}

// ---------------- INI ----------------

// Parse INI sections and keys.
Ref<R> Ini::parse(const String &p_src) {
	Dictionary out;
	String section; // Store keys at the root before the first section.
	int line_no = 0;
	for (const String &raw : p_src.split("\n")) {
		line_no++;
		const String line = raw.strip_edges();
		if (line.is_empty() || line.begins_with(";") || line.begins_with("#")) {
			continue;
		}
		if (line.begins_with("[") && line.ends_with("]")) {
			section = unquote_dq(line.substr(1, line.length() - 2).strip_edges());
			if (!out.has(section)) {
				out[section] = Dictionary();
			}
			continue;
		}
		const int eq = mark_at(line, '=', 0);
		if (eq < 0) {
			return R::err(vformat("no '=' at line %d", line_no), Err::INVALID_DATA);
		}
		const String key = unquote_dq(line.substr(0, eq).strip_edges());
		const Variant val = typed_of(line.substr(eq + 1).strip_edges());
		if (section.is_empty()) {
			out[key] = val;
		} else {
			Dictionary box = out[section];
			box[key] = val;
		}
	}
	return R::ok(out);
}

namespace {

// Serialize INI scalars directly for numbers and booleans, quoting structural text.
// Unescaped newlines could inject later sections or key assignments.
// Quote them so parsing preserves the original structure.
String ini_text(const Variant &p_v) {
	if (p_v.get_type() == Variant::BOOL) {
		return (bool)p_v ? "true" : "false";
	}
	if (p_v.get_type() == Variant::INT || p_v.get_type() == Variant::FLOAT) {
		return Pool::text(p_v);
	}
	const String s = Pool::text(p_v);
	// Quote text that resembles numbers or booleans.
	// Otherwise a string such as 1 would return as a numeric value.
	const String low = s.to_lower();
	const bool typed = s.is_valid_int() || s.is_valid_float() || low == "true" || low == "false";
	return (needs_dq(s) || typed) ? as_dq(s) : s;
}

// Validate bare keys with an allowed-character set rather than an incomplete rejection list.
// The caller supplies characters permitted by each format.
bool bare_ok(const String &s, const String &p_extra) {
	if (needs_dq(s)) {
		return false;
	}
	for (int i = 0; i < s.length(); i++) {
		const char32_t c = s[i];
		const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		if (!word && p_extra.find_char(c) < 0) {
			return false;
		}
	}
	return true;
}

// Quote keys and section names containing assignment, bracket, or comment markers.
String ini_name(const String &s) {
	return bare_ok(s, "_-. ") ? s : as_dq(s);
}

} // namespace

// Serialize an INI dictionary.
String Ini::stringify(const Dictionary &p_data) {
	String out;
	// Write keys outside sections first.
	for (const Variant &k : p_data.keys()) {
		if (p_data[k].get_type() != Variant::DICTIONARY) {
			out += vformat("%s = %s\n", ini_name(Pool::text(k)), ini_text(p_data[k]));
		}
	}
	for (const Variant &k : p_data.keys()) {
		if (p_data[k].get_type() != Variant::DICTIONARY) {
			continue;
		}
		out += vformat("\n[%s]\n", ini_name(Pool::text(k)));
		const Dictionary box = p_data[k];
		for (const Variant &k2 : box.keys()) {
			out += vformat("%s = %s\n", ini_name(Pool::text(k2)), ini_text(box[k2]));
		}
	}
	return out.lstrip("\n");
}

// ---------------- TOML ----------------

namespace {

// Strip comments while preserving hash characters inside quotes.
String drop_hash(const String &line) {
	const int at = mark_at(line, '#', 0);
	return at < 0 ? line : line.substr(0, at);
}

Variant toml_value(const String &s, bool &r_bad, String &r_why, int depth);

// Parse a TOML array.
Variant toml_array(const String &s, bool &r_bad, String &r_why, int depth) {
	const int close = s.rfind_char(']');
	Array out;
	for (const String &part : split_top(s.substr(1, close - 1), ',')) {
		const String t = part.strip_edges();
		if (t.is_empty()) {
			continue;
		}
		const Variant v = toml_value(t, r_bad, r_why, depth + 1);
		if (r_bad) {
			return Variant();
		}
		out.push_back(v);
	}
	return out;
}

// Parse a TOML inline table.
Variant toml_inline(const String &s, bool &r_bad, String &r_why, int depth) {
	const int close = s.rfind_char('}');
	Dictionary out;
	for (const String &part : split_top(s.substr(1, close - 1), ',')) {
		const String t = part.strip_edges();
		if (t.is_empty()) {
			continue;
		}
		const int eq = mark_at(t, '=', 0);
		if (eq < 0) {
			r_bad = true;
			r_why = "no '=' in inline table";
			return Variant();
		}
		const Variant v = toml_value(t.substr(eq + 1).strip_edges(), r_bad, r_why, depth + 1);
		if (r_bad) {
			return Variant();
		}
		out[unquote_dq(t.substr(0, eq).strip_edges())] = v;
	}
	return out;
}

// Parse a TOML value with type inference.
Variant toml_value(const String &s, bool &r_bad, String &r_why, int depth) {
	// Bound nesting to prevent stack exhaustion from repeated opening brackets.
	if (depth > Variant::MAX_RECURSION_DEPTH) {
		r_bad = true;
		r_why = "too deep";
		return Variant();
	}
	if (s.is_empty()) {
		r_bad = true;
		r_why = "empty value";
		return Variant();
	}
	if (s.begins_with("\"") || s.begins_with("'")) {
		return unquote_dq(s);
	}
	if (s.begins_with("[")) {
		return toml_array(s, r_bad, r_why, depth);
	}
	if (s.begins_with("{")) {
		return toml_inline(s, r_bad, r_why, depth);
	}
	const String low = s.to_lower();
	if (low == "true") {
		return true;
	}
	if (low == "false") {
		return false;
	}
	if (s.is_valid_int()) {
		return s.to_int();
	}
	if (s.is_valid_float()) {
		return s.to_float();
	}
	return s; // Retain dates and other unsupported scalar forms as text.
}

// Find or create a nested table.
Dictionary toml_dig(Dictionary p_root, const Vector<String> &p_path) {
	Dictionary cur = p_root;
	for (const String &seg : p_path) {
		const String key = unquote_dq(seg.strip_edges());
		if (key.is_empty()) {
			continue;
		}
		if (!cur.has(key)) {
			cur[key] = Dictionary();
		}
		if (cur[key].get_type() == Variant::ARRAY) {
			Array arr = cur[key];
			if (arr.is_empty()) {
				arr.push_back(Dictionary());
			}
			cur = arr[arr.size() - 1];
		} else {
			cur = cur[key];
		}
	}
	return cur;
}

} // namespace

// Parse TOML documents.
Ref<R> Toml::parse(const String &p_src) {
	Dictionary root;
	Dictionary cur = root;
	int line_no = 0;
	bool bad = false;
	String why;

	for (const String &raw : p_src.split("\n")) {
		line_no++;
		const String line = drop_hash(raw).strip_edges();
		if (line.is_empty()) {
			continue;
		}

		// Array of tables.
		if (line.begins_with("[[") && line.ends_with("]]")) {
			const String path = line.substr(2, line.length() - 4).strip_edges();
			const Vector<String> segs = split_top(path, '.');
			const String last = unquote_dq(segs[segs.size() - 1].strip_edges());
			Dictionary parent = toml_dig(root, segs.slice(0, segs.size() - 1));
			if (!parent.has(last) || parent[last].get_type() != Variant::ARRAY) {
				parent[last] = Array();
			}
			Array arr = parent[last];
			Dictionary item;
			arr.push_back(item);
			cur = item;
			continue;
		}

		// Table.
		if (line.begins_with("[") && line.ends_with("]")) {
			cur = toml_dig(root, split_top(line.substr(1, line.length() - 2).strip_edges(), '.'));
			continue;
		}

		// Key-value assignment.
		const int eq = mark_at(line, '=', 0);
		if (eq < 0) {
			return R::err(vformat("no '=' at line %d", line_no), Err::INVALID_DATA);
		}
		const Variant v = toml_value(line.substr(eq + 1).strip_edges(), bad, why, 0);
		if (bad) {
			return R::err(vformat("%s at line %d", why, line_no), Err::INVALID_DATA);
		}
		cur[unquote_dq(line.substr(0, eq).strip_edges())] = v;
	}
	return R::ok(root);
}

namespace {

// Recognize dictionary-only arrays for array-of-table notation.
bool is_table_array(const Variant &p_v) {
	if (p_v.get_type() != Variant::ARRAY) {
		return false;
	}
	const Array a = p_v;
	if (a.is_empty()) {
		return false;
	}
	for (int i = 0; i < a.size(); i++) {
		if (a[i].get_type() != Variant::DICTIONARY) {
			return false;
		}
	}
	return true;
}

String toml_name(const String &s);

// Serialize a TOML scalar value.
String toml_text(const Variant &p_v) {
	switch (p_v.get_type()) {
		case Variant::BOOL:
			return (bool)p_v ? "true" : "false";
		case Variant::INT:
		case Variant::FLOAT:
			return Pool::text(p_v);
		case Variant::ARRAY: {
			const Array a = p_v;
			PackedStringArray parts;
			for (int i = 0; i < a.size(); i++) {
				parts.push_back(toml_text(a[i]));
			}
			return "[" + String(", ").join(parts) + "]";
		}
		default:
			// Unescaped newlines or quotes could become additional keys when read back.
			// Quote and escape those values.
			return as_dq(Pool::text(p_v));
	}
}

// Allow bare keys only with alphanumerics, underscore, and hyphen; dots separate table paths.
String toml_name(const String &s) {
	return bare_ok(s, "_-") ? s : as_dq(s);
}

} // namespace

namespace {

// Serialize a validated dictionary as TOML.
String toml_write(const Dictionary &p_data, const String &p_prefix) {
	String out;
	// Write scalar values first.
	for (const Variant &k : p_data.keys()) {
		const Variant v = p_data[k];
		if (v.get_type() == Variant::DICTIONARY || is_table_array(v)) {
			continue;
		}
		out += vformat("%s = %s\n", toml_name(Pool::text(k)), toml_text(v));
	}
	// Write tables and arrays of tables afterward.
	for (const Variant &k : p_data.keys()) {
		const Variant v = p_data[k];
		const String leaf = toml_name(Pool::text(k));
		const String name = p_prefix.is_empty() ? leaf : p_prefix + "." + leaf;
		if (v.get_type() == Variant::DICTIONARY) {
			out += vformat("\n[%s]\n", name) + toml_write(v, name);
		} else if (is_table_array(v)) {
			const Array items = v;
			for (int i = 0; i < items.size(); i++) {
				out += vformat("\n[[%s]]\n", name) + toml_write(items[i], name);
			}
		}
	}
	return out;
}

} // namespace

// Serialize a TOML dictionary with its prefix.
Ref<R> Toml::stringify(const Dictionary &p_data, const String &p_prefix) {
	if (!tree_ok(p_data)) {
		return R::err("unsupported value, cyclic data, or invalid nesting depth", Err::INVALID_DATA);
	}
	return R::ok(toml_write(p_data, p_prefix));
}

// ---------------- YAML ----------------

namespace {

// Prepared state for one input line.
struct Line {
	int depth = 0;
	String text;
	int no = 0;
};

// Strip comments while preserving hashes inside quotes or words.
String drop_yaml_comment(const String &line) {
	// A YAML hash begins a comment only at line start or after whitespace.
	for (int at = mark_at(line, '#', 0); at >= 0; at = mark_at(line, '#', at + 1)) {
		if (at == 0 || line[at - 1] == ' ') {
			return line.substr(0, at);
		}
	}
	return line;
}

// Find the first colon outside quotes.
int colon_at(const String &s) {
	// Require whitespace or line end after a mapping colon so values such as 12:30 remain intact.
	for (int at = mark_at(s, ':', 0); at >= 0; at = mark_at(s, ':', at + 1)) {
		if (at + 1 >= s.length() || s[at + 1] == ' ') {
			return at;
		}
	}
	return -1;
}

// Parse a YAML scalar with type inference.
Variant yaml_scalar(const String &s) {
	if (s.begins_with("[") && s.ends_with("]")) {
		Array out;
		for (const String &part : split_top(s.substr(1, s.length() - 2), ',')) {
			const String t = part.strip_edges();
			if (!t.is_empty()) {
				out.push_back(yaml_scalar(t));
			}
		}
		return out;
	}
	if (s.begins_with("{") && s.ends_with("}")) {
		Dictionary box;
		for (const String &part : split_top(s.substr(1, s.length() - 2), ',')) {
			const String t = part.strip_edges();
			if (t.is_empty()) {
				continue;
			}
			const int c = colon_at(t);
			if (c >= 0) {
				box[unquote_dq(t.substr(0, c).strip_edges())] = yaml_scalar(t.substr(c + 1).strip_edges());
			}
		}
		return box;
	}
	if (s.begins_with("\"") || s.begins_with("'")) {
		return unquote_dq(s);
	}
	const String low = s.to_lower();
	if (low == "true" || low == "yes") {
		return true;
	}
	if (low == "false" || low == "no") {
		return false;
	}
	if (low == "null" || low == "~") {
		return Variant();
	}
	if (s.is_valid_int()) {
		return s.to_int();
	}
	if (s.is_valid_float()) {
		return s.to_float();
	}
	return s;
}

struct YamlReader {
	const LocalVector<Line> *lines = nullptr;
	int at = 0;
	bool bad = false;
	String why;

	int nest = 0; // Nesting depth checked against the supported boundary.

	Variant block(int depth);
	Variant seq(int depth);
	Variant map(int depth);
	void pair_into(Dictionary &box, const String &text, int depth);
};

// Parse an indented YAML block.
Variant YamlReader::block(int depth) {
	// Bound recursive indentation depth to prevent stack exhaustion.
	if (nest > Variant::MAX_RECURSION_DEPTH) {
		bad = true;
		why = "too deep";
		return Variant();
	}
	if (at >= (int)lines->size()) {
		return Variant();
	}
	if ((*lines)[at].text.begins_with("- ")) {
		return seq(depth);
	}
	return map(depth);
}

// Parse a YAML sequence into an array.
Variant YamlReader::seq(int depth) {
	Array out;
	while (at < (int)lines->size() && !bad) {
		const Line &ln = (*lines)[at];
		if (ln.depth < depth || !ln.text.begins_with("- ")) {
			break;
		}
		at++;
		const String body = ln.text.substr(2).strip_edges();
		// A sequence item with a mapping key starts a map; quoted colons remain content.
		if (colon_at(body) >= 0 || body.ends_with(":")) {
			Dictionary item;
			pair_into(item, body, ln.depth + 2);
			while (at < (int)lines->size() && !bad) {
				const Line &nxt = (*lines)[at];
				if (nxt.depth <= ln.depth) {
					break;
				}
				pair_into(item, nxt.text, nxt.depth);
			}
			out.push_back(item);
			continue;
		}
		if (body.is_empty()) {
			nest++;
			out.push_back(block(ln.depth + 2));
			nest--;
			continue;
		}
		out.push_back(yaml_scalar(body));
	}
	return out;
}

// Parse a YAML mapping into a dictionary.
Variant YamlReader::map(int depth) {
	Dictionary out;
	while (at < (int)lines->size() && !bad) {
		const Line &ln = (*lines)[at];
		if (ln.depth < depth) {
			break;
		}
		if (ln.depth > depth) {
			bad = true;
			why = vformat("unexpected indent at line %d", ln.no);
			break;
		}
		if (ln.text.begins_with("- ")) {
			break;
		}
		pair_into(out, ln.text, depth);
	}
	return out;
}

// Parse and append a mapping key and value.
void YamlReader::pair_into(Dictionary &box, const String &text, int depth) {
	const int colon = colon_at(text);
	if (colon < 0) {
		bad = true;
		why = vformat("no ':' in \"%s\"", text);
		return;
	}
	const String key = unquote_dq(text.substr(0, colon).strip_edges());
	const String val = text.substr(colon + 1).strip_edges();
	at++;

	// Multiline string.
	if (val == "|" || val == ">" || val == "|-" || val == ">-") {
		const bool keep = val.begins_with("|");
		String joined;
		bool first = true;
		while (at < (int)lines->size()) {
			const Line &ln2 = (*lines)[at];
			if (ln2.depth <= depth) {
				break;
			}
			if (!first) {
				joined += keep ? "\n" : " ";
			}
			joined += ln2.text;
			first = false;
			at++;
		}
		box[key] = joined;
		return;
	}

	if (!val.is_empty()) {
		box[key] = yaml_scalar(val);
		return;
	}

	// An empty value takes its content from the nested block.
	if (at < (int)lines->size()) {
		const Line &ln3 = (*lines)[at];
		if (ln3.depth > depth) {
			nest++;
			box[key] = block(ln3.depth);
			nest--;
			return;
		}
	}
	box[key] = Variant();
}

} // namespace

// Parse a YAML document.
Ref<R> Yaml::parse(const String &p_src) {
	LocalVector<Line> lines;
	int no = 0;
	for (const String &raw : p_src.split("\n")) {
		no++;
		const String body = drop_yaml_comment(raw);
		if (body.strip_edges().is_empty()) {
			continue;
		}
		Line ln;
		ln.no = no;
		while (ln.depth < body.length() && body[ln.depth] == ' ') {
			ln.depth++;
		}
		ln.text = body.strip_edges();
		lines.push_back(ln);
	}
	if (lines.is_empty()) {
		return R::ok(Dictionary());
	}
	YamlReader r;
	r.lines = &lines;
	const Variant v = r.block(lines[0].depth);
	if (r.bad) {
		return R::err(r.why, Err::INVALID_DATA);
	}
	return R::ok(v);
}

namespace {

// Normalize YAML null values to the appropriate runtime type.
bool yaml_empty(const Variant &p_v) {
	if (p_v.get_type() == Variant::DICTIONARY) {
		return Dictionary(p_v).is_empty();
	}
	if (p_v.get_type() == Variant::ARRAY) {
		return Array(p_v).is_empty();
	}
	return false;
}

// Identify leading structural characters that cannot be emitted as bare text.
bool yaml_lead(const String &s) {
	if (s.is_empty()) {
		return true;
	}
	static const String head =
			"-?:," // Sequence, mapping-key, and separator markers.
			"[]{}" // Flow-container markers.
			"#&*!|>" // Comments, anchors, aliases, tags, and block scalars.
			"'\"" // Quote characters.
			"%@`~="; // Directives, reserved markers, and null notation.
	return head.find_char(s[0]) >= 0;
}

// Serialize YAML scalars, quoting ambiguous text.
String yaml_text(const Variant &p_v) {
	switch (p_v.get_type()) {
		case Variant::NIL:
			return "null";
		case Variant::BOOL:
			return (bool)p_v ? "true" : "false";
		case Variant::INT:
		case Variant::FLOAT:
			return Pool::text(p_v);
		default:
			break;
	}
	const String s = Pool::text(p_v);
	const String low = s.to_lower();
	// Quote and escape newlines so they cannot become another indentation block.
	// Quote leading YAML indicators so text is not interpreted as a sequence or block.
	const bool tricky = needs_dq(s) || yaml_lead(s) || s.is_valid_float() ||
			low == "true" || low == "false" || low == "null" || low == "yes" || low == "no" ||
			s.contains(": ") || s.contains("#");
	return tricky ? as_dq(s) : s;
}

// Quote keys using the same structural-safety rules as values.
String yaml_name(const String &s) {
	return (bare_ok(s, "_-.") && !yaml_lead(s)) ? s : as_dq(s);
}

} // namespace

namespace {

// Serialize a validated value as YAML.
String yaml_write(const Variant &p_data, int p_depth) {
	const String pad = String("  ").repeat(p_depth);
	if (p_data.get_type() == Variant::DICTIONARY) {
		const Dictionary d = p_data;
		if (d.is_empty()) {
			return pad + "{}\n";
		}
		String out;
		for (const Variant &k : d.keys()) {
			const Variant v = d[k];
			const bool nested = v.get_type() == Variant::DICTIONARY || v.get_type() == Variant::ARRAY;
			if (!nested) {
				out += vformat("%s%s: %s\n", pad, yaml_name(Pool::text(k)), yaml_text(v));
			} else if (yaml_empty(v)) {
				out += vformat("%s%s: %s\n", pad, yaml_name(Pool::text(k)), v.get_type() == Variant::DICTIONARY ? "{}" : "[]");
			} else {
				out += vformat("%s%s:\n", pad, yaml_name(Pool::text(k))) + yaml_write(v, p_depth + 1);
			}
		}
		return out;
	}
	if (p_data.get_type() == Variant::ARRAY) {
		const Array a = p_data;
		if (a.is_empty()) {
			return pad + "[]\n";
		}
		String out;
		for (int i = 0; i < a.size(); i++) {
			const Variant it = a[i];
			if (it.get_type() != Variant::DICTIONARY && it.get_type() != Variant::ARRAY) {
				out += vformat("%s- %s\n", pad, yaml_text(it));
				continue;
			}
			// Replace only the first indentation level with a sequence marker.
			const String body = yaml_write(it, p_depth + 1);
			const int first_nl = body.find_char('\n');
			out += vformat("%s- %s\n", pad, body.substr(0, first_nl).strip_edges()) + body.substr(first_nl + 1);
		}
		return out;
	}
	return pad + yaml_text(p_data) + "\n";
}

} // namespace

// Serialize YAML with the requested indentation depth.
Ref<R> Yaml::stringify(const Variant &p_data, int p_depth) {
	if (p_depth < 0 || !tree_ok(p_data, p_depth)) {
		return R::err("unsupported value, cyclic data, or invalid nesting depth", Err::INVALID_DATA);
	}
	return R::ok(yaml_write(p_data, p_depth));
}

// ---------------- Commented JSON ----------------

String Jsonc::strip(const String &p_src) {
	// First pass: remove comments.
	String mid;
	int i = 0;
	const int n = p_src.length();
	const char32_t *r = p_src.ptr();
	bool in_str = false;
	while (i < n) {
		const char32_t c = r[i];
		if (in_str) {
			mid += String::chr(c);
			if (c == '\\' && i + 1 < n) {
				mid += String::chr(r[i + 1]);
				i += 2;
				continue;
			}
			if (c == '"') {
				in_str = false;
			}
			i++;
			continue;
		}
		if (c == '"') {
			in_str = true;
			mid += String::chr(c);
			i++;
			continue;
		}
		if (c == '/' && i + 1 < n && r[i + 1] == '/') {
			while (i < n && r[i] != '\n') {
				i++;
			}
			continue;
		}
		if (c == '/' && i + 1 < n && r[i + 1] == '*') {
			i += 2;
			while (i + 1 < n && !(r[i] == '*' && r[i + 1] == '/')) {
				i++;
			}
			i += 2;
			continue;
		}
		mid += String::chr(c);
		i++;
	}

	// Second pass: remove trailing commas before closing containers.
	String out;
	i = 0;
	const int mn = mid.length();
	const char32_t *m = mid.ptr();
	in_str = false;
	while (i < mn) {
		const char32_t c = m[i];
		if (in_str) {
			out += String::chr(c);
			if (c == '\\' && i + 1 < mn) {
				out += String::chr(m[i + 1]);
				i += 2;
				continue;
			}
			if (c == '"') {
				in_str = false;
			}
			i++;
			continue;
		}
		if (c == '"') {
			in_str = true;
			out += String::chr(c);
			i++;
			continue;
		}
		if (c == ',') {
			int k = i + 1;
			while (k < mn && (m[k] == ' ' || m[k] == '\t' || m[k] == '\n' || m[k] == '\r')) {
				k++;
			}
			if (k < mn && (m[k] == ']' || m[k] == '}')) {
				i++;
				continue;
			}
		}
		out += String::chr(c);
		i++;
	}
	return out;
}

// Strip comments and decode JSON.
Ref<R> Jsonc::parse(const String &p_src) {
	const Ref<R> decoded = JsonData::decode(strip(p_src).to_utf8_buffer());
	return decoded->get_ok() ? decoded : decoded->note("invalid jsonc");
}

// ---------------- Front matter ----------------

namespace {

// Delimiters and their associated formats.
struct FrontMark {
	const char *mark;
	const char *kind;
};

const FrontMark FRONT_MARKS[] = {
	{ "---", "yaml" },
	{ "+++", "toml" },
	{ nullptr, nullptr },
};

// Build a front-matter result containing attributes, body, and format.
Ref<R> front_of(const Variant &p_attrs, const String &p_body, const String &p_kind) {
	Dictionary box;
	box["attrs"] = p_attrs;
	box["body"] = p_body;
	box["kind"] = p_kind;
	return R::ok(box);
}

} // namespace

// Check whether a document begins with front matter.
bool Front::has(const String &p_src) {
	for (int m = 0; FRONT_MARKS[m].mark; m++) {
		if (p_src.begins_with(String(FRONT_MARKS[m].mark) + "\n")) {
			return true;
		}
	}
	return p_src.begins_with("{");
}

// Parse front matter and retain the document body.
Ref<R> Front::parse(const String &p_src) {
	for (int m = 0; FRONT_MARKS[m].mark; m++) {
		const String mark = FRONT_MARKS[m].mark;
		const String kind = FRONT_MARKS[m].kind;
		if (!p_src.begins_with(mark + "\n")) {
			continue;
		}
		const int end = p_src.find("\n" + mark, mark.length());
		if (end < 0) {
			return R::err("front matter is not closed", Err::INVALID_DATA);
		}
		// For empty front matter, opening and closing line boundaries can overlap.
		// Clamp the extracted length to zero instead of treating a negative length as the remaining document.
		const int head_len = MAX(end - (int)mark.length() - 1, 0);
		const String head = p_src.substr(mark.length() + 1, head_len);
		const String body = p_src.substr(end + mark.length() + 1).lstrip("\n");
		// An empty delimited block still counts as front matter with empty attributes.
		if (head.strip_edges().is_empty()) {
			return front_of(Dictionary(), body, kind);
		}
		const Ref<R> got = kind == "yaml" ? Yaml::parse(head) : Toml::parse(head);
		if (got->get_e().is_valid()) {
			return got->note("front matter");
		}
		return front_of(got->get_v(), body, kind);
	}

	// Read JSON front matter from its opening brace through the matching closing brace.
	if (p_src.begins_with("{")) {
		int depth = 0;
		bool quoted = false; // Do not count braces inside strings as structure.
		bool escaped = false; // Whether an escape precedes the quote.
		for (int i = 0; i < p_src.length(); i++) {
			const char32_t c = p_src[i];
			if (quoted) {
				if (escaped) {
					escaped = false;
				} else if (c == '\\') {
					escaped = true;
				} else if (c == '"') {
					quoted = false;
				}
				continue;
			}
			if (c == '"') {
				quoted = true;
				continue;
			}
			if (c == '{') {
				depth++;
			} else if (c == '}') {
				depth--;
				if (depth == 0) {
					const Ref<R> decoded = JsonData::decode(p_src.substr(0, i + 1).to_utf8_buffer());
					if (!decoded->get_ok()) {
						return decoded->note("invalid json front matter");
					}
					return front_of(decoded->get_v(), p_src.substr(i + 1).lstrip("\n"), "json");
				}
			}
		}
	}
	return front_of(Dictionary(), p_src, "");
}

// Serialize front-matter attributes and document body.
Ref<R> Front::stringify(const Dictionary &p_attrs, const String &p_body, const String &p_kind) {
	// Without attributes, return the body unless its prefix resembles front matter.
	// Wrap such a body with an empty front-matter block to preserve its interpretation.
	const String mk = (p_kind == "toml") ? "+++" : "---";
	// A body beginning with a front-matter marker needs an empty leading block.
	// Otherwise the reader would consume part of the body as metadata.
	if (p_attrs.is_empty()) {
		return R::ok(has(p_body) ? mk + "\n" + mk + "\n\n" + p_body : p_body);
	}
	const Ref<R> head = (p_kind == "toml") ? Toml::stringify(p_attrs, String()) : Yaml::stringify(p_attrs, 0);
	if (!head->get_ok()) return head;
	return R::ok(mk + "\n" + String(head->get_v()) + mk + "\n\n" + p_body);
}

// ---------------- .env ----------------

namespace {

// Remove quotes and decode escapes only inside double-quoted values.
String env_unquote(const String &p_v) {
	char32_t quote = 0;
	const String body = unquote(p_v, &quote);
	if (quote == '"') {
		return unesc_dq(body); // Decode escapes only for double quotes.
	}
	if (quote == '\'') {
		return body;
	}
	// Strip a trailing comment from unquoted values.
	const int at = p_v.find(" #");
	return at >= 0 ? p_v.substr(0, at).strip_edges() : p_v;
}

} // namespace

// Parse .env assignments.
Ref<R> Dotenv::parse(const String &p_src) {
	Dictionary out;
	int line_no = 0;
	for (const String &raw : p_src.split("\n")) {
		line_no++;
		String line = raw.strip_edges();
		if (line.is_empty() || line.begins_with("#")) continue;
		if (line.begins_with("export ")) line = line.substr(7).strip_edges();
		const int eq = line.find_char('=');
		const String key = eq < 0 ? line : line.substr(0, eq).strip_edges();
		if (!bare_ok(key, "_.")) return R::err(vformat("invalid environment name at line %d", line_no), Err::INVALID_DATA);
		const String value = eq < 0 ? String() : line.substr(eq + 1).strip_edges();
		if (!value.is_empty() && (value[0] == '\'' || value[0] == '"')) {
			int end = 1;
			for (; end < value.length(); end++) {
				if (value[end] == '\\' && end + 1 < value.length()) { end++; continue; }
				if (value[end] == value[0]) break;
			}
			if (end == value.length()) return R::err(vformat("unterminated environment quote at line %d", line_no), Err::INVALID_DATA);
			const String tail = value.substr(end + 1).strip_edges();
			if (!tail.is_empty() && !tail.begins_with("#")) return R::err(vformat("unexpected text after environment value at line %d", line_no), Err::INVALID_DATA);
			out[key] = env_unquote(value.substr(0, end + 1));
		} else {
			out[key] = env_unquote(value);
		}
	}
	return R::ok(out);
}

// Serialize environment assignments without dropping invalid names.
Ref<R> Dotenv::stringify(const Dictionary &p_box) {
	if (!tree_ok(p_box)) return R::err("unsupported environment value", Err::INVALID_DATA);
	String out;
	for (const Variant &k : p_box.keys()) {
		const String key = Pool::text(k);
		if (!bare_ok(key, "_.")) return R::err(vformat("invalid environment name %s", key), Err::INVALID_DATA);
		const String v = Pool::text(p_box[k]);
		const bool needs = needs_dq(v) || v.contains(" ") || v.contains("#");
		out += vformat("%s=%s\n", key, needs ? as_dq(v) : v);
	}
	return R::ok(out);
}

// ---------------- XML ----------------

namespace {

const char *XML_TEXT_KEY = "#text"; // Dictionary key for text content.
const char *XML_ATTR_MARK = "@"; // Prefix for attribute keys.
constexpr int XML_DEPTH_MAX = Variant::MAX_RECURSION_DEPTH; // Nesting boundary supported by returned Variant values.

// Retain an open element and text fragments until its closing tag.
struct XmlFrame {
	Dictionary node;
	String name;
	Vector<String> text;
};

// Promote repeated element names to an array.
void xml_attach(Dictionary &p_parent, const String &p_name, const Dictionary &p_node) {
	if (!p_parent.has(p_name)) {
		p_parent[p_name] = p_node;
		return;
	}
	const Variant held = p_parent[p_name];
	if (held.get_type() == Variant::ARRAY) {
		Array arr = held;
		arr.push_back(p_node);
		return;
	}
	Array arr;
	arr.push_back(held);
	arr.push_back(p_node);
	p_parent[p_name] = arr;
}

// Join text fragments once and store them in the element.
void xml_finish(XmlFrame &p_frame) {
	if (!p_frame.text.is_empty()) {
		p_frame.node[XML_TEXT_KEY] = String().join(p_frame.text);
	}
}

} // namespace

// Parse XML iteratively into nested dictionaries of attributes and text.
Ref<R> Xml::parse(const String &p_src) {
	const PackedByteArray raw = p_src.to_utf8_buffer();
	Ref<XMLParser> p;
	p.instantiate();
	if (p->open_buffer(raw) != OK) {
		return R::err("cannot open xml", Err::INVALID_DATA);
	}

	Dictionary root;
	// Track open elements in a stack; shared dictionaries propagate updates to the root.
	Vector<XmlFrame> stack;
	XmlFrame base;
	base.node = root;
	stack.push_back(base);
	Error read_err = OK;
	while ((read_err = p->read()) == OK) {
		const XMLParser::NodeType kind = p->get_node_type();
		if (kind == XMLParser::NODE_ELEMENT) {
			if (!p->is_empty() && stack.size() >= XML_DEPTH_MAX) {
				return R::err("xml exceeds the depth limit", Err::LIMITED);
			}
			Dictionary node;
			for (int i = 0; i < p->get_attribute_count(); i++) {
				node[XML_ATTR_MARK + p->get_attribute_name(i)] = p->get_attribute_value(i);
			}
			Dictionary parent = stack[stack.size() - 1].node;
			xml_attach(parent, p->get_node_name(), node);
			if (!p->is_empty()) {
				XmlFrame frame;
				frame.node = node;
				frame.name = p->get_node_name();
				stack.push_back(frame);
			}
		} else if (kind == XMLParser::NODE_ELEMENT_END) {
			const String close_name = p->get_node_name().rstrip(" \t\r\n");
			if (stack.size() == 1 || stack[stack.size() - 1].name != close_name) {
				return R::err(vformat("xml closing element does not match \"%s\"", p->get_node_name()), Err::INVALID_DATA);
			}
			xml_finish(stack.write[stack.size() - 1]);
			stack.resize(stack.size() - 1);
		} else if (kind == XMLParser::NODE_TEXT || kind == XMLParser::NODE_CDATA) {
			const String txt = (kind == XMLParser::NODE_TEXT ? p->get_node_data() : p->get_node_name()).strip_edges();
			if (!txt.is_empty()) {
				stack.write[stack.size() - 1].text.push_back(txt);
			}
		}
	}
	if (read_err != ERR_FILE_EOF || stack.size() != 1) {
		return R::err("xml ends before all elements are closed", Err::INVALID_DATA);
	}
	xml_finish(stack.write[0]);
	return R::ok(root);
}

namespace {

String xml_one(const String &p_name, const Variant &p_v, int p_indent, Ref<Err> &r_error);

} // namespace

namespace {

// Serialize a validated dictionary as XML.
String xml_write(const Dictionary &p_data, int p_indent, Ref<Err> &r_error) {
	String out;
	for (const Variant &k : p_data.keys()) {
		const String key = Pool::text(k);
		if (key == XML_TEXT_KEY || key.begins_with(XML_ATTR_MARK)) {
			continue;
		}
		if (!Html::name_ok(key)) {
			r_error = Err::make(vformat("invalid XML element name %s", key), Err::INVALID_DATA);
			return String();
		}
		const Variant v = p_data[k];
		if (v.get_type() == Variant::ARRAY) {
			const Array items = v;
			for (int i = 0; i < items.size(); i++) {
				out += xml_one(key, items[i], p_indent, r_error);
				if (r_error.is_valid()) return String();
			}
		} else {
			out += xml_one(key, v, p_indent, r_error);
			if (r_error.is_valid()) return String();
		}
	}
	return out.is_empty() ? String("  ").repeat(p_indent) : out;
}

} // namespace

// Serialize an XML dictionary with the requested indentation.
Ref<R> Xml::stringify(const Dictionary &p_data, int p_indent) {
	if (p_indent < 0 || !tree_ok(p_data, p_indent)) {
		return R::err("unsupported value, cyclic data, or invalid nesting depth", Err::INVALID_DATA);
	}
	Ref<Err> error;
	const String text = xml_write(p_data, p_indent, error);
	return error.is_valid() ? R::err(error) : R::ok(text);
}

namespace {

// Serialize one XML element.
String xml_one(const String &p_name, const Variant &p_v, int p_indent, Ref<Err> &r_error) {
	const String pad = String("  ").repeat(p_indent);
	if (p_v.get_type() != Variant::DICTIONARY) {
		return vformat("%s<%s>%s</%s>\n", pad, p_name, Html::escape(Pool::text(p_v)), p_name);
	}
	const Dictionary node = p_v;
	String attrs;
	for (const Variant &k : node.keys()) {
		const String key = Pool::text(k);
		if (key.begins_with(XML_ATTR_MARK)) {
			const String name = key.substr(1);
			if (!Html::name_ok(name)) {
				r_error = Err::make(vformat("invalid XML attribute name %s", name), Err::INVALID_DATA);
				return String();
			}
			// Always double-quote attributes and escape their contents.
			attrs += vformat(" %s=\"%s\"", name, Html::escape(Pool::text(node[k])));
		}
	}
	const String text = Pool::text(node.get(XML_TEXT_KEY, ""));
	const String inner = xml_write(node, p_indent + 1, r_error);
	if (r_error.is_valid()) return String();
	const bool has_child = !inner.strip_edges().is_empty();
	if (!has_child && text.is_empty()) {
		return vformat("%s<%s%s/>\n", pad, p_name, attrs);
	}
	if (!has_child) {
		return vformat("%s<%s%s>%s</%s>\n", pad, p_name, attrs, Html::escape(text), p_name);
	}
	String head = vformat("%s<%s%s>\n", pad, p_name, attrs);
	if (!text.is_empty()) {
		head += vformat("%s  %s\n", pad, Html::escape(text));
	}
	return head + inner + vformat("%s</%s>\n", pad, p_name);
}

} // namespace

// ---------------- Newline-delimited JSON ----------------

Ref<R> Jsonl::parse(const String &p_src) {
	Array out;
	int no = 0;
	for (const String &raw : p_src.split("\n")) {
		no++;
		const String line = raw.strip_edges();
		if (line.is_empty()) {
			continue;
		}
		const Ref<R> decoded = JsonData::decode(line.to_utf8_buffer());
		if (!decoded->get_ok()) {
			return decoded->note(vformat("invalid json at line %d", no));
		}
		out.push_back(decoded->get_v());
	}
	return R::ok(out);
}

// Serialize strict JSON values one per line.
Ref<R> Jsonl::stringify(const Array &p_items) {
	PackedByteArray out;
	for (int i = 0; i < p_items.size(); i++) {
		const Ref<R> encoded = JsonData::encode(p_items[i]);
		if (!encoded->get_ok()) {
			return encoded->note(vformat("cannot encode json at line %d", i + 1));
		}
		const PackedByteArray line = encoded->get_v();
		const int64_t at = out.size();
		// Respect the UTF-8 decoder's length and terminator boundary, reporting allocation failure.
		if (line.size() > INT_MAX - 2 - at || out.resize(at + line.size() + 1) != OK) {
			return R::err("cannot allocate JSONL string", Err::LIMITED);
		}
		memcpy(out.ptrw() + at, line.ptr(), line.size());
		out.ptrw()[at + line.size()] = '\n';
	}
	return R::ok(String::utf8((const char *)out.ptr(), out.size()));
}

// Register public script methods and properties.
void GDJSONLReader::_bind_methods() {
	ClassDB::bind_method(D_METHOD("feed", "chunk"), &GDJSONLReader::feed);
	ClassDB::bind_method(D_METHOD("finish"), &GDJSONLReader::finish);
	ADD_RESULT("feed", "Array");
	ADD_RESULT("finish", "Array");
}

// Append an input fragment to the incremental decoder.
Ref<R> GDJSONLReader::feed(const String &p_chunk) {
	buf += p_chunk;
	Array out;
	int offset = 0; // First unconsumed character in the shared input buffer.
	while (true) {
		const int nl = buf.find_char('\n', offset);
		if (nl < 0) {
			break;
		}
		const String line = buf.substr(offset, nl - offset).strip_edges();
		offset = nl + 1;
		line_no++;
		if (line.is_empty()) {
			continue;
		}
		const Ref<R> decoded = JsonData::decode(line.to_utf8_buffer());
		if (!decoded->get_ok()) {
			buf = buf.substr(offset);
			return decoded->note(vformat("invalid json at line %d", line_no));
		}
		out.push_back(decoded->get_v());
	}
	buf = buf.substr(offset);
	return R::ok(out);
}

// Finish input and decode any remaining final line.
Ref<R> GDJSONLReader::finish() {
	if (buf.strip_edges().is_empty()) {
		buf = String();
		return R::ok(Array());
	}
	const Ref<R> got = feed("\n");
	buf = String();
	return got;
}
