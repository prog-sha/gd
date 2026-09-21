/**************************************************************************/
/*  html_context.cpp                                                      */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Escape template values according to the surrounding HTML context.

#include "cli/api/html_context.h"
#include "cli/data/utf8.h"
#include "cli/sys/pool.h"

#include "cli/data/json.h"

#include "core/math/math_funcs.h"
#include "core/string/char_utils.h"
#include "core/string/string_builder.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

namespace HtmlContext {
namespace {

constexpr const char *FAILSAFE = "ZgdunsafeZ"; // Replacement marker for unsafe values.
constexpr int LEGACY_ENTITY_MAX = 6; // Longest legacy entity name accepted without a semicolon.

// Build a one-time lookup table from whitespace-separated names.
HashSet<String> name_set(const char *p_names) {
	HashSet<String> out;
	for (const String &name : String(p_names).split_spaces()) {
		out.insert(name);
	}
	return out;
}

// Recognize HTML syntax whitespace.
bool html_space(char32_t p_c) {
	return p_c == ' ' || p_c == '\t' || p_c == '\n' || p_c == '\f' || p_c == '\r';
}

// Decode one entity that can affect attribute syntax.
bool entity_at(const String &p_text, int p_at, char32_t &r_c, int &r_size) {
	if (p_at < 0 || p_at >= p_text.length() || p_text[p_at] != '&') {
		return false;
	}
	int at = p_at + 1;
	if (at < p_text.length() && p_text[at] == '#') {
		at++;
		const bool hex = at < p_text.length() && (p_text[at] == 'x' || p_text[at] == 'X');
		at += hex ? 1 : 0;
		const int start = at;
		uint64_t value = 0;
		while (at < p_text.length()) {
			const char32_t c = p_text[at];
			int digit = -1;
			if (c >= '0' && c <= '9') {
				digit = c - '0';
			} else if (hex && (c | 32) >= 'a' && (c | 32) <= 'f') {
				digit = (c | 32) - 'a' + 10;
			}
			if (digit < 0 || (!hex && digit > 9)) {
				break;
			}
			value = MIN(uint64_t(0x110000), value * (hex ? 16 : 10) + uint64_t(digit));
			at++;
		}
		if (at == start) {
			return false;
		}
		r_c = value <= 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ? 0xfffd : char32_t(value);
		r_size = at - p_at + (at < p_text.length() && p_text[at] == ';' ? 1 : 0);
		return true;
	}
	static const HashMap<String, char32_t> named = []() {
		HashMap<String, char32_t> out;
		out.insert("quot", '"');
		out.insert("QUOT", '"');
		out.insert("apos", '\'');
		out.insert("amp", '&');
		out.insert("AMP", '&');
		out.insert("lt", '<');
		out.insert("LT", '<');
		out.insert("gt", '>');
		out.insert("GT", '>');
		out.insert("sol", '/');
		out.insert("bsol", '\\');
		out.insert("lpar", '(');
		out.insert("rpar", ')');
		out.insert("lcub", '{');
		out.insert("rcub", '}');
		out.insert("lsqb", '[');
		out.insert("rsqb", ']');
		out.insert("semi", ';');
		out.insert("colon", ':');
		out.insert("quest", '?');
		out.insert("num", '#');
		out.insert("equals", '=');
		out.insert("grave", '`');
		out.insert("DiacriticalGrave", '`');
		out.insert("Hat", '^');
		out.insert("VerticalLine", '|');
		out.insert("ast", '*');
		out.insert("comma", ',');
		out.insert("commat", '@');
		out.insert("dollar", '$');
		out.insert("excl", '!');
		out.insert("lbrace", '{');
		out.insert("lbrack", '[');
		out.insert("midast", '*');
		out.insert("percnt", '%');
		out.insert("period", '.');
		out.insert("plus", '+');
		out.insert("rbrace", '}');
		out.insert("rbrack", ']');
		out.insert("verbar", '|');
		out.insert("vert", '|');
		out.insert("Tab", '\t');
		out.insert("NewLine", '\n');
		out.insert("nbsp", 0xa0);
		out.insert("NonBreakingSpace", 0xa0);
		out.insert("ensp", 0x2002);
		out.insert("emsp", 0x2003);
		out.insert("emsp13", 0x2004);
		out.insert("emsp14", 0x2005);
		out.insert("numsp", 0x2007);
		out.insert("puncsp", 0x2008);
		out.insert("ThinSpace", 0x2009);
		out.insert("thinsp", 0x2009);
		out.insert("VeryThinSpace", 0x200a);
		out.insert("hairsp", 0x200a);
		out.insert("MediumSpace", 0x205f);
		out.insert("UnderBar", '_');
		out.insert("lowbar", '_');
		return out;
	}();
	const int start = at;
	while (at < p_text.length()) {
		const char32_t c = p_text[at];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
			break;
		}
		at++;
	}
	const bool semi = at < p_text.length() && p_text[at] == ';';
	if (semi) {
		const HashMap<String, char32_t>::ConstIterator found = named.find(p_text.substr(start, at - start));
		if (found) {
			r_c = found->value;
			r_size = at - p_at + 1;
			return true;
		}
	}
	// Search only the bounded set of legacy names valid without semicolons.
	for (int end = MIN(at, start + LEGACY_ENTITY_MAX); end > start; end--) {
		const String name = p_text.substr(start, end - start);
		const HashMap<String, char32_t>::ConstIterator found = named.find(name);
		const bool legacy = name == "AMP" || name == "GT" || name == "LT" || name == "QUOT" || name == "amp" || name == "gt" || name == "lt" || name == "quot";
		if (found && legacy) {
			r_c = found->value;
			r_size = end - p_at;
			return true;
		}
	}
	return false;
}

// Recognize ASCII alphanumeric characters.
bool ascii_word(char32_t p_c) {
	return (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z') || (p_c >= '0' && p_c <= '9');
}

// Decode entities only inside attributes when reading one syntax character.
bool scan_char_at(const String &p_text, int p_at, bool p_decode, char32_t &r_c, int &r_size) {
	if (p_at < 0 || p_at >= p_text.length()) {
		return false;
	}
	r_c = p_text[p_at];
	r_size = 1;
	if (p_decode) {
		entity_at(p_text, p_at, r_c, r_size);
	}
	return true;
}

// Read a syntax sequence with entity decoding confined to attributes.
bool scan_text_at(const String &p_text, int p_at, const String &p_word, bool p_decode, bool p_fold, int &r_end) {
	int at = p_at;
	for (int i = 0; i < p_word.length(); i++) {
		char32_t c = 0;
		int size = 0;
		if (!scan_char_at(p_text, at, p_decode, c, size)) {
			return false;
		}
		if (p_fold && c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}
		if (c != p_word[i]) {
			return false;
		}
		at += size;
	}
	r_end = at;
	return true;
}

// Match ASCII keywords case-insensitively even when split by entities.
bool decoded_word_at(const String &p_text, int p_at, const String &p_word, int &r_end) {
	return scan_text_at(p_text, p_at, p_word, true, true, r_end);
}

// Return the preceding decoded character, including a final entity.
bool decoded_prev_at(const String &p_text, int p_at, char32_t &r_c) {
	if (p_at <= 0 || p_at > p_text.length()) {
		return false;
	}
	r_c = p_text[p_at - 1];
	// Verify that a candidate start actually forms the immediately preceding entity.
	auto decode = [&](int p_start) {
		int size = 0;
		char32_t decoded = 0;
		if (p_start >= 0 && entity_at(p_text, p_start, decoded, size) && p_start + size == p_at) {
			r_c = decoded;
			return true;
		}
		return false;
	};
	if (r_c == ';') {
		int at = p_at - 2;
		while (at >= 0 && (ascii_word(p_text[at]) || p_text[at] == '#')) {
			at--;
		}
		decode(at);
		return true;
	}
	// Scan semicolon-free numeric entities backward without a digit-count limit.
	int at = p_at - 1;
	while (at >= 0 && is_digit(p_text[at])) {
		at--;
	}
	if (at >= 1 && p_text[at] == '#' && p_text[at - 1] == '&' && decode(at - 1)) {
		return true;
	}
	at = p_at - 1;
	while (at >= 0 && is_hex_digit(p_text[at])) {
		at--;
	}
	if (at >= 2 && (p_text[at] == 'x' || p_text[at] == 'X') && p_text[at - 1] == '#' && p_text[at - 2] == '&' && decode(at - 2)) {
		return true;
	}
	// Search only supported legacy names for semicolon-free named entities.
	for (at = p_at - 2; at >= MAX(0, p_at - LEGACY_ENTITY_MAX - 1); at--) {
		if (p_text[at] == '&' && decode(at)) {
			break;
		}
	}
	return true;
}

// Return the original position after consuming entity-decoded HTML whitespace.
int decoded_space_end(const String &p_text, int p_at) {
	char32_t c = 0;
	int size = 0;
	while (scan_char_at(p_text, p_at, true, c, size) && html_space(c)) {
		p_at += size;
	}
	return p_at;
}

// Recognize ASCII characters permitted within a CSS identifier.
bool css_name(char32_t p_c) {
	return ascii_word(p_c) || p_c == '-' || p_c == '_' || p_c >= 0x80;
}

// Compare text case-insensitively at the selected position.
bool starts_ci(const String &p_text, int p_at, const String &p_word) {
	return p_at >= 0 && p_at + p_word.length() <= p_text.length() && p_text.substr(p_at, p_word.length()).nocasecmp_to(p_word) == 0;
}

// Recognize safe tag-name terminators.
bool tag_boundary(const String &p_text, int p_at) {
	return p_at < p_text.length() && (p_text[p_at] == '>' || p_text[p_at] == '/' || html_space(p_text[p_at]));
}

// Detect a special element's closing tag at the current position.
bool special_end(const Context &p_ctx, const String &p_text, int p_at) {
	String name;
	switch (p_ctx.element) {
		case ELEMENT_SCRIPT:
			name = "script";
			break;
		case ELEMENT_STYLE:
			name = "style";
			break;
		case ELEMENT_TEXTAREA:
			name = "textarea";
			break;
		case ELEMENT_TITLE:
			name = "title";
			break;
		default:
			return false;
	}
	const int end = p_at + 2 + name.length();
	return starts_ci(p_text, p_at, "</" + name) && tag_boundary(p_text, end);
}

// Detect sequences inside script strings that also affect HTML parsing.
bool script_literal_tag(const String &p_text, int p_at) {
	return starts_ci(p_text, p_at, "<script") || starts_ci(p_text, p_at, "</script") || starts_ci(p_text, p_at, "<!--");
}

// Classify an opening tag.
Element element_of(const String &p_name) {
	const String name = p_name.to_lower();
	if (name == "script") {
		return ELEMENT_SCRIPT;
	}
	if (name == "style") {
		return ELEMENT_STYLE;
	}
	if (name == "textarea") {
		return ELEMENT_TEXTAREA;
	}
	if (name == "title") {
		return ELEMENT_TITLE;
	}
	if (name == "meta") {
		return ELEMENT_META;
	}
	return ELEMENT_NONE;
}

// Identify URL-valued attributes using known names and conservative rules.
bool url_name(const String &p_name) {
	static const HashSet<String> names = name_set("action archive background cite classid codebase data formaction href icon longdesc manifest poster profile src usemap xmlns");
	return names.has(p_name) || p_name.contains("src") || p_name.contains("uri") || p_name.contains("url");
}

// Strip namespace and data- prefixes in order before classifying the attribute.
Attr attr_of(String p_name, Element p_element) {
	p_name = p_name.to_lower();
	if (p_element == ELEMENT_SCRIPT && p_name == "type") {
		return ATTR_SCRIPT_TYPE;
	}
	if (p_element == ELEMENT_META && p_name == "content") {
		return ATTR_META_CONTENT;
	}
	if (p_name.begins_with("data-")) {
		p_name = p_name.substr(5);
	} else {
		const int colon = p_name.find_char(':');
		if (colon >= 0) {
			if (p_name.substr(0, colon) == "xmlns") {
				return ATTR_URL;
			}
			p_name = p_name.substr(colon + 1);
		}
	}
	if (p_name == "style") {
		return ATTR_STYLE;
	}
	if (p_name == "srcset") {
		return ATTR_SRCSET;
	}
	if (p_name.begins_with("on")) {
		return ATTR_SCRIPT;
	}
	static const HashSet<String> non_urls = name_set("accept accept-charset alt async autocomplete autofocus autoplay border checked challenge charset class cols colspan content contenteditable contextmenu controls coords crossorigin datetime default defer dir dirname disabled draggable dropzone enctype for form formenctype formmethod formnovalidate formtarget headers height hidden high hreflang http-equiv id ismap keytype kind label lang language list loop low max maxlength media mediagroup method min multiple name novalidate open optimum pattern placeholder preload pubdate radiogroup readonly rel required reversed rows rowspan sandbox scope scoped seamless selected shape size sizes span spellcheck srcdoc srclang start step tabindex target title type value width wrap");
	if (non_urls.has(p_name)) {
		return ATTR_NONE;
	}
	return url_name(p_name) ? ATTR_URL : ATTR_NONE;
}

// Check whether an ordinary attribute is safe as a dynamic name.
bool plain_name(const String &p_raw) {
	if (p_raw.is_empty()) {
		return false;
	}
	const String name = p_raw.to_lower();
	if (attr_of(name, ELEMENT_NONE) != ATTR_NONE) {
		return false;
	}
	static const HashSet<String> unsafe = name_set("accept-charset async challenge charset content crossorigin defer enctype form formenctype formmethod formnovalidate http-equiv keytype language method novalidate pattern rel sandbox srcdoc type value");
	if (unsafe.has(name)) {
		return false;
	}
	for (int i = 0; i < name.length(); i++) {
		if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9'))) {
			return false;
		}
	}
	return true;
}

// Check whether script type denotes executable script or structured data.
bool js_type(String p_type) {
	const int semicolon = p_type.find_char(';');
	if (semicolon >= 0) {
		p_type = p_type.substr(0, semicolon);
	}
	p_type = p_type.strip_edges().to_lower();
	static const HashSet<String> names = name_set("application/ecmascript application/javascript application/json application/ld+json application/x-ecmascript application/x-javascript module text/ecmascript text/javascript text/javascript1.0 text/javascript1.1 text/javascript1.2 text/javascript1.3 text/javascript1.4 text/javascript1.5 text/jscript text/livescript text/x-ecmascript text/x-javascript");
	return p_type.is_empty() || names.has(p_type);
}

// Finish an attribute value and return to tag context.
void close_attr(Context &r_ctx) {
	Element element = r_ctx.element;
	if (r_ctx.attr == ATTR_SCRIPT_TYPE && !r_ctx.attr_dynamic && !js_type(r_ctx.attr_text)) {
		element = ELEMENT_NONE;
	}
	r_ctx = Context();
	r_ctx.state = TAG;
	r_ctx.element = element;
}

// Choose the initial value state from its attribute kind.
State value_state(Attr p_attr) {
	switch (p_attr) {
		case ATTR_SCRIPT:
			return JS;
		case ATTR_STYLE:
			return CSS;
		case ATTR_URL:
			return URL;
		case ATTR_SRCSET:
			return SRCSET;
		case ATTR_META_CONTENT:
			return META_CONTENT;
		default:
			return ATTR_VALUE;
	}
}

// Recognize whitespace within executable scripts.
bool js_space(char32_t p_c) {
	return p_c == '\f' || p_c == '\n' || p_c == '\r' || p_c == '\t' || p_c == '\v' || p_c == 0x20 || p_c == 0xa0 || p_c == 0x1680 ||
			(p_c >= 0x2000 && p_c <= 0x200a) || p_c == 0x2028 || p_c == 0x2029 || p_c == 0x202f || p_c == 0x205f || p_c == 0x3000 || p_c == 0xfeff;
}

// Return the preceding character with entity decoding only inside attributes.
bool scan_prev_at(const String &p_text, int p_at, bool p_decode, char32_t &r_c) {
	if (p_decode) {
		return decoded_prev_at(p_text, p_at, r_c);
	}
	if (p_at <= 0 || p_at > p_text.length()) {
		return false;
	}
	r_c = p_text[p_at - 1];
	return true;
}

// Match script keywords at entity-decoded identifier boundaries.
bool js_keyword_at(const String &p_text, int p_at, bool p_decode, int &r_end) {
	char32_t before = 0;
	if (scan_prev_at(p_text, p_at, p_decode, before) && (is_unicode_identifier_continue(before) || before == '$')) {
		return false;
	}
	static const char *words[] = { "instanceof", "continue", "finally", "typeof", "break", "delete", "return", "throw", "case", "else", "void", "try", "do", "in" }; // Keywords after which a regular-expression literal may begin.
	for (const char *raw : words) {
		const String word = raw;
		int end = 0;
		if (!scan_text_at(p_text, p_at, word, p_decode, false, end)) {
			continue;
		}
		char32_t next = 0;
		int size = 0;
		if (!scan_char_at(p_text, end, p_decode, next, size) || (!is_unicode_identifier_continue(next) && next != '$')) {
			r_end = end;
			return true;
		}
	}
	return false;
}

// Determine whether the next slash begins a regular expression or division.
JsPart js_after(const String &p_text, int p_at, bool p_decode, JsPart p_old, int &r_end) {
	char32_t c = 0;
	int size = 0;
	if (!scan_char_at(p_text, p_at, p_decode, c, size)) {
		return p_old;
	}
	r_end = p_at + size;
	if (js_space(c)) {
		return p_old;
	}
	if (js_keyword_at(p_text, p_at, p_decode, r_end)) {
		return JS_REGEXP_PART;
	}
	if (String(",<>=*%&|^?!~([{;:").contains(String::chr(c))) {
		return JS_REGEXP_PART;
	}
	char32_t before = 0;
	if (c == '+' || c == '-') {
		return scan_prev_at(p_text, p_at, p_decode, before) && before == c ? (p_old == JS_REGEXP_PART ? JS_DIV_PART : JS_REGEXP_PART) : JS_REGEXP_PART;
	}
	if (c == '.') {
		return scan_prev_at(p_text, p_at, p_decode, before) && is_digit(before) ? JS_DIV_PART : JS_REGEXP_PART;
	}
	if (c == '}') {
		return JS_REGEXP_PART;
	}
	return JS_DIV_PART;
}

// Compare two parser states.
bool context_same(const Context &p_a, const Context &p_b) {
	if (p_a.state != p_b.state || p_a.delim != p_b.delim || p_a.url_part != p_b.url_part || p_a.js_part != p_b.js_part || p_a.attr != p_b.attr || p_a.element != p_b.element || p_a.attr_text != p_b.attr_text || p_a.attr_dynamic != p_b.attr_dynamic || p_a.comment_line != p_b.comment_line || p_a.js_braces.size() != p_b.js_braces.size()) {
		return false;
	}
	for (int i = 0; i < p_a.js_braces.size(); i++) {
		if (p_a.js_braces[i] != p_b.js_braces[i]) {
			return false;
		}
	}
	return true;
}

// Advance to the position produced by inserting an empty value.
Context nudged(Context p_ctx) {
	if (p_ctx.state == TAG || p_ctx.state == AFTER_NAME) {
		p_ctx.state = ATTR_NAME;
		p_ctx.attr = ATTR_NONE;
	} else if (p_ctx.state == BEFORE_VALUE) {
		p_ctx.state = value_state(p_ctx.attr);
		p_ctx.delim = DELIM_SPACE;
		p_ctx.attr = ATTR_NONE;
	}
	return p_ctx;
}

// Convert Variant to ordinary value text.
String plain(const Variant &p_value) {
	return p_value.get_type() == Variant::NIL ? String() : Pool::text(p_value);
}

// Retain output only within String's remaining character capacity.
struct Output {
	LocalVector<char32_t> text;
	int64_t left = 0;
	int64_t size = 0;
	bool limited = false;

	explicit Output(int64_t p_max) :
			left(MAX(int64_t(0), p_max)) {}

	// Append a string only if the entire value fits.
	bool add(const String &p_text) {
		if (p_text.is_empty()) {
			return true;
		}
		if (p_text.length() > left) {
			limited = true;
			return false;
		}
		left -= p_text.length();
		size += p_text.length();
		const uint32_t at = text.size();
		text.resize(at + p_text.length());
		memcpy(text.ptr() + at, p_text.ptr(), p_text.length() * sizeof(char32_t));
		return true;
	}

	// Append ASCII constants directly without intermediate String allocation.
	bool add(const char *p_text) {
		const int length = strlen(p_text);
		if (length > left) {
			limited = true;
			return false;
		}
		left -= length;
		size += length;
		const uint32_t at = text.size();
		text.resize(at + length);
		for (int i = 0; i < length; i++) {
			text[at + i] = uint8_t(p_text[i]);
		}
		return true;
	}

	// Append only the requested source range without copying the original string.
	bool add(const String &p_text, int p_from, int p_size) {
		if (p_size == 0) {
			return true;
		}
		if (p_size > left) {
			limited = true;
			return false;
		}
		left -= p_size;
		size += p_size;
		const uint32_t at = text.size();
		text.resize(at + p_size);
		memcpy(text.ptr() + at, p_text.ptr() + p_from, p_size * sizeof(char32_t));
		return true;
	}

	// Append one character within remaining capacity.
	bool add(char32_t p_c) {
		if (left < 1) {
			limited = true;
			return false;
		}
		left--;
		size++;
		text.push_back(p_c);
		return true;
	}

	// Return the constructed string.
	String done() {
		if (text.is_empty()) {
			return String();
		}
		String out;
		out.resize_uninitialized(text.size() + 1);
		char32_t *dst = out.ptrw();
		memcpy(dst, text.ptr(), text.size() * sizeof(char32_t));
		dst[text.size()] = 0;
		return out;
	}

	// Report whether output is still empty.
	bool is_empty() const { return size == 0; }
};

// Describe a required HTML character replacement as an ASCII entity or Unicode scalar.
struct HtmlRepl {
	const char *ascii = nullptr; // ASCII entity text.
	char32_t rune = 0; // Non-ASCII replacement character.
	bool changed = false; // Whether the original character must be replaced.
};

// Return the HTML escape required for one character.
HtmlRepl html_repl(char32_t p_c, bool p_no_space) {
	if (p_c == 0) {
		return p_no_space ? HtmlRepl{ "&#xfffd;", 0, true } : HtmlRepl{ nullptr, 0xfffd, true };
	}
	if (p_no_space) {
		switch (p_c) {
			case '\t': return { "&#9;", 0, true };
			case '\n': return { "&#10;", 0, true };
			case '\v': return { "&#11;", 0, true };
			case '\f': return { "&#12;", 0, true };
			case '\r': return { "&#13;", 0, true };
			case ' ': return { "&#32;", 0, true };
			case '=': return { "&#61;", 0, true };
			case '`': return { "&#96;", 0, true };
			default: break;
		}
	}
	switch (p_c) {
		case '&': return { "&amp;", 0, true };
		case '"': return { "&#34;", 0, true };
		case '\'': return { "&#39;", 0, true };
		case '+': return { "&#43;", 0, true };
		case '<': return { "&lt;", 0, true };
		case '>': return { "&gt;", 0, true };
		default: return {};
	}
}

// Replace HTML syntax characters with named or numeric entities.
String html_escape(const String &p_text, bool p_no_space, int64_t p_max, bool &r_limited) {
	Output out(p_max);
	if (p_no_space && p_text.is_empty()) {
		out.add(FAILSAFE);
		r_limited = out.limited;
		return out.done();
	}
	int from = 0;
	bool changed = false;
	for (int i = 0; i < p_text.length() && !out.limited; i++) {
		const HtmlRepl repl = html_repl(p_text[i], p_no_space);
		if (!repl.changed) {
			continue;
		}
		changed = true;
		if (!out.add(p_text, from, i - from) || !(repl.ascii ? out.add(repl.ascii) : out.add(repl.rune))) {
			break;
		}
		from = i + 1;
	}
	if (!changed && p_text.length() <= MAX(int64_t(0), p_max)) {
		r_limited = false;
		return p_text;
	}
	if (!out.limited) {
		out.add(p_text, from, p_text.length() - from);
	}
	r_limited = out.limited;
	return out.done();
}

// Reject URL schemes with unsafe side effects.
bool safe_url(const String &p_url) {
	const int colon = p_url.find_char(':');
	const int slash = p_url.find_char('/');
	if (colon < 0 || (slash >= 0 && slash < colon)) {
		return true;
	}
	const String scheme = p_url.substr(0, colon).to_lower();
	return scheme == "http" || scheme == "https" || scheme == "mailto";
}

// Append one byte as lowercase percent-encoded hexadecimal.
void add_pct(Output &r_out, uint8_t p_byte) {
	const char *hex = "0123456789abcdef";
	r_out.add('%');
	r_out.add(hex[p_byte >> 4]);
	r_out.add(hex[p_byte & 15]);
}

// Recognize ASCII punctuation safe to retain in a URL.
bool url_reserved(char32_t p_c) {
	return p_c == '!' || p_c == '#' || p_c == '$' || p_c == '&' || p_c == '*' || p_c == '+' || p_c == ',' || p_c == '/' || p_c == ':' || p_c == ';' || p_c == '=' || p_c == '?' || p_c == '@' || p_c == '[' || p_c == ']';
}

// Encode one character as UTF-8 bytes and percent-encode each byte.
void add_utf8_pct(Output &r_out, char32_t p_c) {
	uint8_t bytes[4];
	int size = 0;
	if (p_c <= 0x7ff) {
		bytes[0] = 0xc0 | (p_c >> 6);
		bytes[1] = 0x80 | (p_c & 0x3f);
		size = 2;
	} else if (p_c >= 0xd800 && p_c <= 0xdfff) {
		bytes[0] = 0xef;
		bytes[1] = 0xbf;
		bytes[2] = 0xbd;
		size = 3;
	} else if (p_c <= 0xffff) {
		bytes[0] = 0xe0 | (p_c >> 12);
		bytes[1] = 0x80 | ((p_c >> 6) & 0x3f);
		bytes[2] = 0x80 | (p_c & 0x3f);
		size = 3;
	} else if (p_c <= 0x10ffff) {
		bytes[0] = 0xf0 | (p_c >> 18);
		bytes[1] = 0x80 | ((p_c >> 12) & 0x3f);
		bytes[2] = 0x80 | ((p_c >> 6) & 0x3f);
		bytes[3] = 0x80 | (p_c & 0x3f);
		size = 4;
	} else {
		bytes[0] = 0xef;
		bytes[1] = 0xbf;
		bytes[2] = 0xbd;
		size = 3;
	}
	for (int i = 0; i < size && !r_out.limited; i++) {
		add_pct(r_out, bytes[i]);
	}
}

// Convert URL characters incrementally for query or path context.
String url_escape(const String &p_text, bool p_norm, int64_t p_max, bool &r_limited) {
	Output out(p_max);
	for (int i = 0; i < p_text.length() && !out.limited; i++) {
		const char32_t c = p_text[i];
		if (c > 0x7f) {
			add_utf8_pct(out, c);
			continue;
		}
		const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		const bool unreserved = alnum || c == '-' || c == '.' || c == '_' || c == '~';
		const bool encoded = c == '%' && i + 2 < p_text.length() && is_hex_digit(p_text[i + 1]) && is_hex_digit(p_text[i + 2]);
		if (unreserved || (p_norm && (url_reserved(c) || encoded))) {
			out.add(c);
		} else {
			add_pct(out, uint8_t(c));
		}
	}
	r_limited = out.limited;
	return out.done();
}

// Escape characters that could change embedded string structure.
String js_string(const String &p_text, bool p_template, bool p_regexp, int64_t p_max, bool &r_limited) {
	Output out(p_max);
	const String regexp_chars = "$()*-.?[\\]^{|}";
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		if (c < 0x20 || c == 0x2028 || c == 0x2029) {
			switch (c) {
				case '\t':
					out.add("\\t");
					break;
				case '\n':
					out.add("\\n");
					break;
				case '\f':
					out.add("\\f");
					break;
				case '\r':
					out.add("\\r");
					break;
				default:
					out.add(vformat("\\u%04x", int(c)));
					break;
			}
			if (out.limited) {
				break;
			}
			continue;
		}
		if (p_regexp && regexp_chars.contains(String::chr(c))) {
			out.add("\\" + String::chr(c));
			if (out.limited) {
				break;
			}
			continue;
		}
		switch (c) {
			case '"':
				out.add("\\u0022");
				break;
			case '\'':
				out.add("\\u0027");
				break;
			case '`':
				out.add("\\u0060");
				break;
			case '&':
				out.add("\\u0026");
				break;
			case '+':
				out.add("\\u002b");
				break;
			case '/':
				out.add("\\/");
				break;
			case '<':
				out.add("\\u003c");
				break;
			case '>':
				out.add("\\u003e");
				break;
			case '\\':
				out.add("\\\\");
				break;
			case '$':
				out.add(p_template ? "\\u0024" : "$");
				break;
			case '{':
				out.add(p_template ? "\\u007b" : "{");
				break;
			case '}':
				out.add(p_template ? "\\u007d" : "}");
				break;
			default:
				out.add(c);
				break;
		}
		if (out.limited) {
			break;
		}
	}
	if (p_regexp && out.is_empty() && !out.limited) {
		out.add("(?:)");
	}
	r_limited = out.limited;
	return out.done();
}

// Convert Variant into a value safe in both executable scripts and structured data.
String js_value(const Variant &p_value, int64_t p_max, bool &r_limited) {
	const Ref<R> encoded = JsonData::encode(p_value, { { "deterministic", true }, { "max_bytes", INT_MAX - 1 } });
	String raw;
	if (encoded->get_ok()) {
		const PackedByteArray bytes = encoded->get_v();
		raw = String::utf8((const char *)bytes.ptr(), bytes.size());
	} else if (encoded->get_e()->is(Err::LIMITED)) {
		r_limited = true;
		return String();
	} else {
		raw = "null";
	}
	Output out(p_max);
	const bool pad = !raw.is_empty() && (ascii_word(raw[0]) || raw[0] == '_' || raw[0] == '$' || ascii_word(raw[raw.length() - 1]) || raw[raw.length() - 1] == '_' || raw[raw.length() - 1] == '$');
	if (pad) {
		out.add(' ');
	}
	for (int i = 0; i < raw.length() && !out.limited; i++) {
		switch (raw[i]) {
			case '<':
				out.add("\\u003c");
				break;
			case '>':
				out.add("\\u003e");
				break;
			case '&':
				out.add("\\u0026");
				break;
			case 0x2028:
				out.add("\\u2028");
				break;
			case 0x2029:
				out.add("\\u2029");
				break;
			default:
				out.add(raw[i]);
				break;
		}
	}
	if (pad && !out.limited) {
		out.add(' ');
	}
	r_limited = out.limited;
	return out.done();
}

// Remove HTML and CSS boundary characters from CSS strings through escaping.
String css_string(const String &p_text, int64_t p_max, bool &r_limited) {
	Output out(p_max);
	const String specials = "\t\n\f\r\"&'()+/:;<>\\{}";
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		if (c == '\\') {
			out.add("\\\\");
		} else if (c == 0 || specials.contains(String::chr(c))) {
			out.add(c == 0 ? String("\\0") : vformat("\\%x", int(c)));
			if (i + 1 == p_text.length() || (p_text[i + 1] < 128 && (is_hex_digit(p_text[i + 1]) || html_space(p_text[i + 1])))) {
				out.add(' ');
			}
		} else {
			out.add(c);
		}
		if (out.limited) {
			break;
		}
	}
	r_limited = out.limited;
	return out.done();
}

// Decode CSS escapes to match browser interpretation.
String css_decode(const String &p_text) {
	StringBuilder out;
	for (int i = 0; i < p_text.length(); i++) {
		if (p_text[i] != '\\') {
			out += String::chr(p_text[i]);
			continue;
		}
		if (i + 1 >= p_text.length()) {
			break;
		}
		int at = i + 1;
		if (!is_hex_digit(p_text[at])) {
			out += String::chr(p_text[at]);
			i = at;
			continue;
		}
		char32_t value = 0;
		int digits = 0;
		while (at < p_text.length() && digits < 6 && is_hex_digit(p_text[at])) {
			const char32_t c = p_text[at++];
			value = value * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
			digits++;
		}
		if (value > 0x10ffff) {
			value >>= 4;
			at--;
		}
		out += String::chr(value);
		if (at < p_text.length() && html_space(p_text[at])) {
			if (p_text[at] == '\r' && at + 1 < p_text.length() && p_text[at + 1] == '\n') {
				at++;
			}
			at++;
		}
		i = at - 1;
	}
	return out.as_string();
}

// Decode one CSS escape after HTML entities and return the consumed source width.
bool scan_css_escape_at(const String &p_text, int p_at, bool p_decode, char32_t &r_c, int &r_size) {
	char32_t slash = 0;
	int slash_size = 0;
	if (!scan_char_at(p_text, p_at, p_decode, slash, slash_size) || slash != '\\') {
		return false;
	}
	int at = p_at + slash_size;
	char32_t c = 0;
	int size = 0;
	if (!scan_char_at(p_text, at, p_decode, c, size)) {
		return false;
	}
	if (!is_hex_digit(c)) {
		r_c = c;
		r_size = at + size - p_at;
		return true;
	}
	char32_t value = 0;
	int digits = 0;
	int last_size = 0;
	while (digits < 6 && scan_char_at(p_text, at, p_decode, c, size) && is_hex_digit(c)) {
		value = value * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
		at += size;
		last_size = size;
		digits++;
	}
	if (value > 0x10ffff) {
		value >>= 4;
		at -= last_size;
	}
	if (scan_char_at(p_text, at, p_decode, c, size) && html_space(c)) {
		at += size;
		char32_t next = 0;
		int next_size = 0;
		if (c == '\r' && scan_char_at(p_text, at, p_decode, next, next_size) && next == '\n') {
			at += next_size;
		}
	}
	r_c = value;
	r_size = at - p_at;
	return true;
}

// Advance URL path or query context for the consumed character.
void advance_url(Context &r_ctx, char32_t p_c) {
	if (p_c == '?' || p_c == '#') {
		r_ctx.url_part = URL_QUERY;
	} else if (!html_space(p_c) && r_ctx.url_part == URL_NONE) {
		r_ctx.url_part = URL_PATH;
	}
}

// Validate characters safe as a standalone CSS value.
String css_value(const String &p_text, int64_t p_max, bool &r_limited) {
	const String text = css_decode(p_text);
	const String bad = "\"'()/;@[\\]`{}<>";
	StringBuilder id_out;
	for (int i = 0; i < text.length(); i++) {
		const char32_t c = text[i];
		if (c == 0 || bad.contains(String::chr(c)) || (c == '-' && i > 0 && text[i - 1] == '-')) {
			Output out(p_max);
			out.add(FAILSAFE);
			r_limited = out.limited;
			return out.done();
		}
		if (c < 128 && (ascii_word(c) || c == '-' || c == '_')) {
			id_out += String::chr(c);
		}
	}
	const String id = id_out.as_string().to_lower();
	Output out(p_max);
	out.add(id.contains("expression") || id.contains("mozbinding") ? String(FAILSAFE) : text);
	r_limited = out.limited;
	return out.done();
}

// Split and validate one srcset URL and descriptor candidate.
void add_srcset_element(const String &p_text, int p_left, int p_right, Output &r_out) {
	int start = p_left;
	while (start < p_right && html_space(p_text[start])) {
		start++;
	}
	int end = start;
	while (end < p_right && !html_space(p_text[end])) {
		end++;
	}
	bool meta_ok = true;
	for (int i = end; i < p_right; i++) {
		if (!html_space(p_text[i]) && !ascii_word(p_text[i])) {
			meta_ok = false;
			break;
		}
	}
	const String url = p_text.substr(start, end - start);
	if (!safe_url(url) || !meta_ok) {
		r_out.add("#" + String(FAILSAFE));
		return;
	}
	if (!r_out.add(p_text, p_left, start - p_left)) {
		return;
	}
	bool url_limited = false;
	const String escaped = url_escape(url, true, r_out.left, url_limited);
	if (url_limited || !r_out.add(escaped) || !r_out.add(p_text, end, p_right - end)) {
		r_out.limited = true;
	}
}

// Validate srcset candidates incrementally at comma boundaries.
String srcset(const String &p_text, int64_t p_max, bool &r_limited) {
	Output out(p_max);
	int left = 0;
	for (int i = 0; i < p_text.length() && !out.limited; i++) {
		if (p_text[i] == ',') {
			add_srcset_element(p_text, left, i, out);
			out.add(',');
			left = i + 1;
		}
	}
	if (!out.limited) {
		add_srcset_element(p_text, left, p_text.length(), out);
	}
	r_limited = out.limited;
	return out.done();
}

} // namespace

// Compare all fields of two HTML contexts.
bool same(const Context &p_a, const Context &p_b) {
	return context_same(p_a, p_b);
}

// Scan static template text to determine the next interpolation context.
Error advance(Context &r_ctx, String &r_text, String &r_bad) {
	const String p_text = r_text;
	Vector<int> changes;
	Vector<String> replacements;
	int comment_at = r_ctx.state == COMMENT || ((r_ctx.state == JS_BLOCK_COMMENT || r_ctx.state == JS_LINE_COMMENT || r_ctx.state == CSS_BLOCK_COMMENT || r_ctx.state == CSS_LINE_COMMENT) && r_ctx.delim == DELIM_NONE) ? 0 : -1;
	bool comment_seen = false;
	int i = 0;
	// Protect HTML closing-tag boundaries even at positions skipped inside script literals.
	auto protect_script_tag = [&](int p_at) {
		const bool literal = r_ctx.state == JS_DQ || r_ctx.state == JS_SQ || r_ctx.state == JS_TEMPLATE || r_ctx.state == JS_REGEXP;
		if (r_ctx.element == ELEMENT_SCRIPT && literal && script_literal_tag(p_text, p_at)) {
			changes.push_back(p_at);
			changes.push_back(p_at + 1);
			replacements.push_back("\\x3C");
		}
	};
	// Read one character using the current delimiter and attribute-only entity decoding.
	auto scan_char = [&](int p_at, char32_t &r_c, int &r_size) {
		return scan_char_at(p_text, p_at, r_ctx.delim != DELIM_NONE, r_c, r_size);
	};
	// Read syntax sequences split by entities using the current delimiter.
	auto scan_text = [&](int p_at, const String &p_word, bool p_fold, int &r_end) {
		return scan_text_at(p_text, p_at, p_word, r_ctx.delim != DELIM_NONE, p_fold, r_end);
	};
	while (i < p_text.length()) {
		const bool script_literal = r_ctx.state == JS_DQ || r_ctx.state == JS_SQ || r_ctx.state == JS_TEMPLATE || r_ctx.state == JS_REGEXP;
		const bool script_protected = script_literal || r_ctx.state == JS_BLOCK_COMMENT || r_ctx.state == JS_LINE_COMMENT;
		protect_script_tag(i);
		if (r_ctx.element != ELEMENT_NONE && !script_protected && r_ctx.state != TAG && r_ctx.state != ATTR_NAME && r_ctx.state != AFTER_NAME && r_ctx.state != BEFORE_VALUE && r_ctx.delim == DELIM_NONE && special_end(r_ctx, p_text, i)) {
			if (comment_at >= 0) {
				changes.push_back(comment_at);
				changes.push_back(i);
				const bool js_block = r_ctx.state == JS_BLOCK_COMMENT;
				const bool css_block = r_ctx.state == CSS_BLOCK_COMMENT;
				replacements.push_back(js_block ? (r_ctx.comment_line ? "\n" : " ") : (css_block ? " " : ""));
				comment_at = -1;
			}
			r_ctx = Context();
			continue;
		}

		char32_t c = p_text[i];
		int step = 1;
		int token_end = 0;
		const bool entity = r_ctx.delim != DELIM_NONE && entity_at(p_text, i, c, step);
		if (r_ctx.delim != DELIM_NONE) {
			const bool quoted_end = !entity && ((r_ctx.delim == DELIM_DOUBLE && c == '"') || (r_ctx.delim == DELIM_SINGLE && c == '\''));
			const bool space_end = !entity && r_ctx.delim == DELIM_SPACE && (html_space(c) || c == '>');
			if (quoted_end || space_end) {
				close_attr(r_ctx);
				if (quoted_end) {
					i++;
				}
				continue;
			}
			if (!entity && r_ctx.delim == DELIM_SPACE && (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`')) {
				r_bad = "bad character in unquoted attribute";
				r_ctx.state = ERROR;
				return ERR_PARSE_ERROR;
			}
			if (r_ctx.attr == ATTR_SCRIPT_TYPE && !r_ctx.attr_dynamic) {
				r_ctx.attr_text += String::chr(c);
			}
		}

		switch (r_ctx.state) {
			case TEXT: {
				if (starts_ci(p_text, i, "<!--")) {
					comment_at = i;
					r_ctx.state = COMMENT;
					i += 4;
					break;
				}
				if (c != '<') {
					i++;
					break;
				}
				int at = i + 1;
				bool closing = false;
				if (at < p_text.length() && p_text[at] == '/') {
					closing = true;
					at++;
				}
				if (at >= p_text.length() || !((p_text[at] >= 'a' && p_text[at] <= 'z') || (p_text[at] >= 'A' && p_text[at] <= 'Z'))) {
					if (!starts_ci(p_text, i, "<!doctype")) {
						changes.push_back(i);
						changes.push_back(i + 1);
						replacements.push_back("&lt;");
					}
					i++;
					break;
				}
				const int start = at++;
				while (at < p_text.length() && (ascii_word(p_text[at]) || ((p_text[at] == '-' || p_text[at] == ':') && at + 1 < p_text.length() && ascii_word(p_text[at + 1])))) {
					at++;
				}
				r_ctx = Context();
				r_ctx.state = TAG;
				r_ctx.element = closing ? ELEMENT_NONE : element_of(p_text.substr(start, at - start));
				i = at;
			} break;
			case TAG: {
				if (html_space(c) || c == '/') {
					i++;
					break;
				}
				if (c == '>') {
					switch (r_ctx.element) {
						case ELEMENT_SCRIPT:
							r_ctx.state = JS;
							break;
						case ELEMENT_STYLE:
							r_ctx.state = CSS;
							break;
						case ELEMENT_TEXTAREA:
						case ELEMENT_TITLE:
							r_ctx.state = RCDATA;
							break;
						case ELEMENT_META:
							r_ctx.state = TEXT;
							r_ctx.element = ELEMENT_NONE;
							break;
						default:
							r_ctx.state = TEXT;
							break;
					}
					r_ctx.delim = DELIM_NONE;
					r_ctx.js_part = JS_REGEXP_PART;
					i++;
					break;
				}
				const int start = i;
				while (i < p_text.length() && !html_space(p_text[i]) && p_text[i] != '=' && p_text[i] != '>') {
					if (p_text[i] == '\'' || p_text[i] == '"' || p_text[i] == '<') {
						r_bad = "bad HTML attribute name";
						r_ctx.state = ERROR;
						return ERR_PARSE_ERROR;
					}
					i++;
				}
				const String name = p_text.substr(start, i - start).to_lower();
				r_ctx.attr = attr_of(name, r_ctx.element);
				r_ctx.attr_text = String();
				r_ctx.attr_dynamic = false;
				r_ctx.state = i == p_text.length() ? ATTR_NAME : AFTER_NAME;
			} break;
			case ATTR_NAME:
				if (html_space(c) || c == '=' || c == '>') {
					r_ctx.state = AFTER_NAME;
				} else if (c == '\'' || c == '"' || c == '<') {
					r_bad = "bad HTML attribute name";
					r_ctx.state = ERROR;
					return ERR_PARSE_ERROR;
				} else {
					i++;
				}
				break;
			case AFTER_NAME:
				if (html_space(c)) {
					i++;
				} else if (c == '=') {
					r_ctx.state = BEFORE_VALUE;
					i++;
				} else {
					r_ctx.state = TAG;
				}
				break;
			case BEFORE_VALUE:
				if (html_space(c)) {
					i++;
					break;
				}
				r_ctx.state = value_state(r_ctx.attr);
				if (c == '"') {
					r_ctx.delim = DELIM_DOUBLE;
					i++;
				} else if (c == '\'') {
					r_ctx.delim = DELIM_SINGLE;
					i++;
				} else {
					r_ctx.delim = DELIM_SPACE;
				}
				break;
			case COMMENT:
				if (starts_ci(p_text, i, "-->")) {
					changes.push_back(MAX(0, comment_at));
					changes.push_back(i + 3);
					replacements.push_back(String());
					comment_at = -1;
					r_ctx = Context();
					i += 3;
				} else {
					i++;
				}
				break;
			case RCDATA:
				if (c == '<') {
					changes.push_back(i);
					changes.push_back(i + 1);
					replacements.push_back("&lt;");
				}
				i++;
				break;
			case ATTR_VALUE:
				i += step;
				break;
			case URL:
				advance_url(r_ctx, c);
				i += step;
				break;
			case SRCSET:
				i += step;
				break;
			case META_CONTENT:
				if (r_ctx.delim != DELIM_NONE) {
					int word_end = 0;
					if (decoded_word_at(p_text, i, "url", word_end)) {
						const int at = decoded_space_end(p_text, word_end);
						char32_t next = 0;
						int size = 0;
						if (scan_char_at(p_text, at, true, next, size) && next == '=') {
							r_ctx.state = META_CONTENT_URL;
							i = at + size;
							break;
						}
					}
				}
				i += step;
				break;
			case META_CONTENT_URL:
				if (c == ';') {
					r_ctx.state = META_CONTENT;
				}
				i += step;
				break;
			case JS:
				if (scan_text(i, "<!--", false, token_end) || scan_text(i, "-->", false, token_end)) {
					r_ctx.state = JS_LINE_COMMENT;
					if (r_ctx.delim == DELIM_NONE) {
						comment_at = i;
					}
					i = token_end;
				} else if (c == '#' && scan_text(i, "#!", false, token_end)) {
					r_ctx.state = JS_LINE_COMMENT;
					if (r_ctx.delim == DELIM_NONE) {
						comment_at = i;
					}
					i = token_end;
				} else if (c == '"') {
					r_ctx.state = JS_DQ;
					i += step;
				} else if (c == '\'') {
					r_ctx.state = JS_SQ;
					i += step;
				} else if (c == '`') {
					r_ctx.state = JS_TEMPLATE;
					i += step;
				} else if (c == '/' && scan_text(i, "//", false, token_end)) {
					r_ctx.state = JS_LINE_COMMENT;
					if (r_ctx.delim == DELIM_NONE) {
						comment_at = i;
					}
					i = token_end;
				} else if (c == '/' && scan_text(i, "/*", false, token_end)) {
					r_ctx.state = JS_BLOCK_COMMENT;
					r_ctx.comment_line = false;
					comment_seen = false;
					if (r_ctx.delim == DELIM_NONE) {
						comment_at = i;
					}
					i = token_end;
				} else if (c == '/' && r_ctx.js_part == JS_UNKNOWN_PART) {
					r_bad = "slash appears after ambiguous JavaScript branches";
					return ERR_PARSE_ERROR;
				} else if (c == '/' && r_ctx.js_part == JS_REGEXP_PART) {
					r_ctx.state = JS_REGEXP;
					i += step;
				} else if (c == '/') {
					r_ctx.js_part = JS_REGEXP_PART;
					i += step;
				} else {
					r_ctx.js_part = js_after(p_text, i, r_ctx.delim != DELIM_NONE, r_ctx.js_part, token_end);
					if (!r_ctx.js_braces.is_empty() && c == '{') {
						r_ctx.js_braces.write[r_ctx.js_braces.size() - 1]++;
					} else if (!r_ctx.js_braces.is_empty() && c == '}') {
						int &depth = r_ctx.js_braces.write[r_ctx.js_braces.size() - 1];
						if (--depth < 0) {
							r_ctx.js_braces.resize(r_ctx.js_braces.size() - 1);
							r_ctx.state = JS_TEMPLATE;
						}
					}
					i = token_end;
				}
				break;
			case JS_DQ:
			case JS_SQ:
				if (c == '\\') {
					char32_t next = 0;
					int next_size = 0;
					if (!scan_char(i + step, next, next_size)) {
						r_bad = "unfinished JavaScript escape";
						return ERR_PARSE_ERROR;
					}
					protect_script_tag(i + step);
					i += step + next_size;
				} else if ((r_ctx.state == JS_DQ && c == '"') || (r_ctx.state == JS_SQ && c == '\'')) {
					r_ctx.state = JS;
					r_ctx.js_part = JS_DIV_PART;
					i += step;
				} else {
					i += step;
				}
				break;
			case JS_TEMPLATE:
				if (c == '\\') {
					char32_t next = 0;
					int next_size = 0;
					if (!scan_char(i + step, next, next_size)) {
						r_bad = "unfinished JavaScript template escape";
						return ERR_PARSE_ERROR;
					}
					protect_script_tag(i + step);
					i += step + next_size;
				} else if (c == '`') {
					r_ctx.state = JS;
					r_ctx.js_part = JS_DIV_PART;
					i += step;
				} else if (c == '$' && scan_text(i, "${", false, token_end)) {
					r_ctx.js_braces.push_back(0);
					r_ctx.state = JS;
					i = token_end;
				} else {
					i += step;
				}
				break;
			case JS_REGEXP: {
				bool escaped = false;
				if (c == '\\') {
					char32_t next = 0;
					int next_size = 0;
					if (!scan_char(i + step, next, next_size)) {
						r_bad = "unfinished JavaScript regexp escape";
						return ERR_PARSE_ERROR;
					}
					escaped = true;
					protect_script_tag(i + step);
					i += step + next_size;
				}
				if (!escaped) {
					if (c == '[') {
						bool closed = false;
						i += step;
						while (i < p_text.length()) {
							char32_t part = 0;
							int part_size = 0;
							if (!scan_char(i, part, part_size)) {
								break;
							}
							protect_script_tag(i);
							if (part == ']') {
								closed = true;
								i += part_size;
								break;
							}
							if (part == '\\') {
								char32_t next = 0;
								int next_size = 0;
								if (!scan_char(i + part_size, next, next_size)) {
									break;
								}
								protect_script_tag(i + part_size);
								i += part_size + next_size;
							} else {
								i += part_size;
							}
						}
						if (!closed) {
							r_bad = "unfinished JavaScript regexp charset";
							return ERR_PARSE_ERROR;
						}
					} else if (c == '/') {
						char32_t before = 0;
						int script_end = 0;
						const bool script_slash = r_ctx.delim == DELIM_NONE ? i > 0 && starts_ci(p_text, i - 1, "</script") : decoded_prev_at(p_text, i, before) && before == '<' && decoded_word_at(p_text, i + step, "script", script_end);
						if (script_slash) {
							i += step;
						} else {
							r_ctx.state = JS;
							r_ctx.js_part = JS_DIV_PART;
							i += step;
						}
					} else {
						i += step;
					}
				}
			} break;
			case JS_BLOCK_COMMENT:
				comment_seen = true;
				if (r_ctx.delim == DELIM_NONE && (c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029)) {
					r_ctx.comment_line = true;
				}
				if (c == '*' && scan_text(i, "*/", false, token_end)) {
					if (r_ctx.delim == DELIM_NONE) {
						changes.push_back(MAX(0, comment_at));
						changes.push_back(token_end);
						replacements.push_back(r_ctx.comment_line ? "\n" : " ");
						comment_at = -1;
					}
					r_ctx.state = JS;
					r_ctx.comment_line = false;
					i = token_end;
				} else {
					i += step;
				}
				break;
			case JS_LINE_COMMENT:
				if (c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029) {
					if (r_ctx.delim == DELIM_NONE) {
						changes.push_back(MAX(0, comment_at));
						changes.push_back(i);
						replacements.push_back(String());
						comment_at = -1;
					}
					r_ctx.state = JS;
				}
				i += step;
				break;
			case CSS:
				if (c == '/' && scan_text(i, "/*", false, token_end)) {
					r_ctx.state = CSS_BLOCK_COMMENT;
					comment_seen = false;
					if (r_ctx.delim == DELIM_NONE) {
						comment_at = i;
					}
					i = token_end;
				} else if (c == '/' && scan_text(i, "//", false, token_end)) {
					r_ctx.state = CSS_LINE_COMMENT;
					if (r_ctx.delim == DELIM_NONE) {
						comment_at = i;
					}
					i = token_end;
				} else if (c == '"') {
					r_ctx.state = CSS_DQ;
					r_ctx.url_part = URL_NONE;
					i += step;
				} else if (c == '\'') {
					r_ctx.state = CSS_SQ;
					r_ctx.url_part = URL_NONE;
					i += step;
				} else if (r_ctx.delim != DELIM_NONE) {
					char32_t before = 0;
					const bool boundary = !decoded_prev_at(p_text, i, before) || !css_name(before);
					int word_end = 0;
					if (!boundary || !decoded_word_at(p_text, i, "url", word_end)) {
						i += step;
						break;
					}
					int at = decoded_space_end(p_text, word_end);
					char32_t next = 0;
					int size = 0;
					if (scan_char_at(p_text, at, true, next, size) && next == '(') {
						at = decoded_space_end(p_text, at + size);
						if (scan_char_at(p_text, at, true, next, size) && next == '"') {
							r_ctx.state = CSS_DQ_URL;
							at += size;
						} else if (scan_char_at(p_text, at, true, next, size) && next == '\'') {
							r_ctx.state = CSS_SQ_URL;
							at += size;
						} else {
							r_ctx.state = CSS_URL;
						}
						r_ctx.url_part = URL_NONE;
						i = at;
					} else {
						i += step;
					}
				} else {
					i += step;
				}
				break;
			case CSS_DQ:
			case CSS_SQ:
			case CSS_DQ_URL:
			case CSS_SQ_URL: {
				const bool end = (r_ctx.state == CSS_DQ || r_ctx.state == CSS_DQ_URL) ? c == '"' : c == '\'';
				if (c == '\\') {
					if (r_ctx.delim == DELIM_NONE && i + step < p_text.length() && special_end(r_ctx, p_text, i + step)) {
						r_bad = "unfinished CSS escape";
						return ERR_PARSE_ERROR;
					}
					char32_t decoded = 0;
					int size = 0;
					if (!scan_css_escape_at(p_text, i, r_ctx.delim != DELIM_NONE, decoded, size)) {
						r_bad = "unfinished CSS escape";
						return ERR_PARSE_ERROR;
					}
					advance_url(r_ctx, decoded);
					i += size;
				} else if (end) {
					r_ctx.state = CSS;
					i += step;
				} else {
					advance_url(r_ctx, c);
					i += step;
				}
			} break;
			case CSS_URL:
				if (c == '\\') {
					if (r_ctx.delim == DELIM_NONE && i + step < p_text.length() && special_end(r_ctx, p_text, i + step)) {
						r_bad = "unfinished CSS escape";
						return ERR_PARSE_ERROR;
					}
					char32_t decoded = 0;
					int size = 0;
					if (!scan_css_escape_at(p_text, i, r_ctx.delim != DELIM_NONE, decoded, size)) {
						r_bad = "unfinished CSS escape";
						return ERR_PARSE_ERROR;
					}
					advance_url(r_ctx, decoded);
					i += size;
					break;
				}
				if (c == ')' || html_space(c)) {
					r_ctx.state = CSS;
				} else {
					advance_url(r_ctx, c);
				}
				i += step;
				break;
			case CSS_BLOCK_COMMENT:
				comment_seen = true;
				if (c == '*' && scan_text(i, "*/", false, token_end)) {
					if (r_ctx.delim == DELIM_NONE) {
						changes.push_back(MAX(0, comment_at));
						changes.push_back(token_end);
						replacements.push_back(" ");
						comment_at = -1;
					}
					r_ctx.state = CSS;
					i = token_end;
				} else {
					i += step;
				}
				break;
			case CSS_LINE_COMMENT:
				if (c == '\n' || c == '\r' || c == '\f') {
					if (r_ctx.delim == DELIM_NONE) {
						changes.push_back(MAX(0, comment_at));
						changes.push_back(i);
						replacements.push_back(String());
						comment_at = -1;
					}
					r_ctx.state = CSS;
				}
				i += step;
				break;
			case ERROR:
				return ERR_PARSE_ERROR;
		}
	}
	if (comment_at >= 0) {
		changes.push_back(comment_at);
		changes.push_back(p_text.length());
		if (comment_seen && r_ctx.state == JS_BLOCK_COMMENT) {
			replacements.push_back(r_ctx.comment_line ? "\n" : " ");
		} else if (comment_seen && r_ctx.state == CSS_BLOCK_COMMENT) {
			replacements.push_back(" ");
		} else {
			replacements.push_back(String());
		}
		r_ctx.comment_line = false;
	}
	if (!changes.is_empty()) {
		String clean;
		int at = 0;
		for (int n = 0; n < changes.size(); n += 2) {
			clean += p_text.substr(at, changes[n] - at);
			clean += replacements[n / 2];
			at = changes[n + 1];
		}
		clean += p_text.substr(at);
		r_text = clean;
	}
	return OK;
}

// Select required conversion from the state immediately before interpolation.
Error action(Context &r_ctx, Action &r_action, String &r_bad) {
	r_ctx = nudged(r_ctx);
	r_action.delim = r_ctx.delim;
	switch (r_ctx.state) {
		case TEXT:
			r_action.escape = ESC_HTML;
			break;
		case RCDATA:
			r_action.escape = ESC_RCDATA;
			break;
		case ATTR_VALUE:
			r_action.escape = ESC_ATTR;
			break;
		case ATTR_NAME:
		case TAG:
			r_action.escape = ESC_NAME;
			r_ctx.state = ATTR_NAME;
			break;
		case URL:
		case CSS_URL:
		case CSS_DQ_URL:
		case CSS_SQ_URL:
			if (r_ctx.url_part == URL_UNKNOWN) {
				r_bad = "value appears in an ambiguous URL context";
				return ERR_PARSE_ERROR;
			}
			r_action.escape = r_ctx.url_part == URL_NONE ? ESC_URL : (r_ctx.url_part == URL_PATH ? ESC_URL_PATH : ESC_URL_QUERY);
			break;
		case CSS_DQ:
		case CSS_SQ:
			if (r_ctx.url_part == URL_UNKNOWN) {
				r_bad = "value appears in an ambiguous URL context";
				return ERR_PARSE_ERROR;
			}
			r_action.escape = r_ctx.url_part == URL_NONE ? ESC_CSS_STRING_URL : (r_ctx.url_part == URL_PATH ? ESC_CSS_STRING : ESC_URL_QUERY);
			break;
		case SRCSET:
			r_action.escape = ESC_SRCSET;
			break;
		case META_CONTENT:
			r_action.escape = ESC_ATTR;
			break;
		case META_CONTENT_URL:
			r_action.escape = ESC_META_URL;
			break;
		case JS:
			r_action.escape = ESC_JS_VALUE;
			r_ctx.js_part = JS_DIV_PART;
			break;
		case JS_DQ:
		case JS_SQ:
			r_action.escape = ESC_JS_STRING;
			break;
		case JS_TEMPLATE:
			r_action.escape = ESC_JS_TEMPLATE;
			break;
		case JS_REGEXP:
			r_action.escape = ESC_JS_REGEXP;
			break;
		case CSS:
			r_action.escape = ESC_CSS_VALUE;
			break;
		case COMMENT:
		case JS_BLOCK_COMMENT:
		case JS_LINE_COMMENT:
		case CSS_BLOCK_COMMENT:
			r_action.escape = ESC_COMMENT;
			break;
		case CSS_LINE_COMMENT:
			r_action.escape = ESC_COMMENT;
			break;
		default:
			r_bad = "value appears in an invalid HTML context";
			return ERR_PARSE_ERROR;
	}
	if (r_ctx.attr == ATTR_SCRIPT_TYPE) {
		r_ctx.attr_dynamic = true;
	}
	return OK;
}

// Merge branch endpoints into one compatible escaping context.
Error join(const Context &p_a, const Context &p_b, Context &r_out, String &r_bad) {
	if (context_same(p_a, p_b)) {
		r_out = p_a;
		return OK;
	}
	Context a = p_a;
	a.url_part = p_b.url_part;
	if (context_same(a, p_b)) {
		r_out = p_a;
		r_out.url_part = URL_UNKNOWN;
		return OK;
	}
	a = p_a;
	a.js_part = p_b.js_part;
	if (context_same(a, p_b)) {
		r_out = p_a;
		r_out.js_part = JS_UNKNOWN_PART;
		return OK;
	}
	a = p_a;
	a.attr_text = p_b.attr_text;
	a.attr_dynamic = p_b.attr_dynamic;
	if (p_a.attr == ATTR_SCRIPT_TYPE && context_same(a, p_b)) {
		r_out = p_a;
		r_out.attr_text = String();
		r_out.attr_dynamic = true;
		return OK;
	}
	const Context na = nudged(p_a);
	const Context nb = nudged(p_b);
	if (!context_same(na, p_a) || !context_same(nb, p_b)) {
		return join(na, nb, r_out, r_bad);
	}
	r_bad = "branches end in different HTML contexts";
	return ERR_PARSE_ERROR;
}

// Require the completed template to end in HTML body context.
Error finish(const Context &p_ctx, String &r_bad) {
	if (p_ctx.state == TEXT && p_ctx.delim == DELIM_NONE && p_ctx.element == ELEMENT_NONE) {
		return OK;
	}
	r_bad = "template ends in a non-text HTML context";
	return ERR_PARSE_ERROR;
}

// Apply the selected conversion within the remaining output capacity.
Error render(const Action &p_action, const Variant &p_value, int64_t p_max, String &r_out) {
	const String text = p_action.escape == ESC_JS_VALUE || p_action.escape == ESC_COMMENT ? String() : plain(p_value);
	String out;
	bool limited = false;
	switch (p_action.escape) {
		case ESC_HTML:
		case ESC_RCDATA:
			out = html_escape(text, false, p_max, limited);
			break;
		case ESC_ATTR: {
			Output kept(p_max);
			kept.add(text);
			out = kept.done();
			limited = kept.limited;
		} break;
		case ESC_NAME: {
			Output kept(p_max);
			kept.add(plain_name(text) ? text.to_lower() : String(FAILSAFE));
			out = kept.done();
			limited = kept.limited;
		} break;
		case ESC_URL:
			out = url_escape(safe_url(text) ? text : "#" + String(FAILSAFE), true, p_max, limited);
			break;
		case ESC_URL_PATH:
			out = url_escape(text, true, p_max, limited);
			break;
		case ESC_URL_QUERY:
			out = url_escape(text, false, p_max, limited);
			break;
		case ESC_SRCSET:
			out = srcset(text, p_max, limited);
			break;
		case ESC_META_URL: {
			Output kept(p_max);
			kept.add(safe_url(text) ? text : "#" + String(FAILSAFE));
			out = kept.done();
			limited = kept.limited;
		} break;
		case ESC_JS_VALUE:
			out = js_value(p_value, p_max, limited);
			break;
		case ESC_JS_STRING:
			out = js_string(text, false, false, p_max, limited);
			break;
		case ESC_JS_TEMPLATE:
			out = js_string(text, true, false, p_max, limited);
			break;
		case ESC_JS_REGEXP:
			out = js_string(text, false, true, p_max, limited);
			break;
		case ESC_CSS_VALUE:
			out = css_value(text, p_max, limited);
			break;
		case ESC_CSS_STRING:
			out = css_string(text, p_max, limited);
			break;
		case ESC_CSS_STRING_URL:
			out = css_string(safe_url(text) ? text : "#" + String(FAILSAFE), p_max, limited);
			break;
		case ESC_COMMENT:
			out = String();
			break;
	}
	if (limited) {
		return ERR_OUT_OF_MEMORY;
	}
	if (p_action.delim == DELIM_DOUBLE || p_action.delim == DELIM_SINGLE) {
		out = html_escape(out, false, p_max, limited);
	} else if (p_action.delim == DELIM_SPACE) {
		out = html_escape(out, true, p_max, limited);
	}
	if (limited) {
		return ERR_OUT_OF_MEMORY;
	}
	r_out = out;
	return OK;
}

// Escape ordinary HTML directly into bytes and reuse contextual conversion for other positions.
Error render_bytes(const Action &p_action, const Variant &p_value, Utf8Out &r_out) {
	if ((p_action.escape == ESC_HTML || p_action.escape == ESC_RCDATA) && p_action.delim == DELIM_NONE) {
		const String text = plain(p_value);
		for (int i = 0; i < text.length(); i++) {
			const HtmlRepl repl = html_repl(text[i], false);
			const bool ok = repl.ascii ? r_out.add(repl.ascii) : r_out.add(repl.changed ? repl.rune : text[i]);
			if (!ok) return ERR_OUT_OF_MEMORY;
		}
		return OK;
	}
	String text;
	const Error err = render(p_action, p_value, INT_MAX - 1, text);
	return err != OK ? err : r_out.add(text) ? OK : ERR_OUT_OF_MEMORY;
}

} // namespace HtmlContext
