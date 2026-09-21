/**************************************************************************/
/*  text.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement text, path, URL, and version helpers declared in text.h.

#include "cli/api/text.h"
#include "cli/data/utf8.h"
#include "cli/sys/pool.h"

#include "cli/api/html_context.h"
#include "cli/sys/mount.h"
#include "cli/sys/limit.h"

#include "cli/net/datagram.h"
#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/char_utils.h"
#include "core/string/string_builder.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"

#include <memory>

namespace {

const char32_t SEP = '/';
// Split on a delimiter and discard empty segments.
Vector<String> cut(const String &p_path) {
	Vector<String> out;
	const int n = p_path.length();
	int at = 0;
	while (at < n) {
		while (at < n && p_path[at] == SEP) {
			at++;
		}
		int end = at;
		while (end < n && p_path[end] != SEP) {
			end++;
		}
		if (end > at) {
			out.push_back(p_path.substr(at, end - at));
		}
		at = end;
	}
	return out;
}
} // namespace

// ---------------- Paths ----------------

String Path::normalize(const String &p_path) {
	if (p_path.is_empty()) {
		return ".";
	}
	// Preserve the scheme prefix before normalizing path segments so its double slash
	// is not collapsed; restore the prefix after normalization.
	String rest;
	const String scheme = Mount::scheme_of(p_path, rest);
	if (!scheme.is_empty()) {
		const String body = normalize(rest);
		return scheme + "://" + (body == "." ? String() : body);
	}
	const bool rooted = p_path[0] == SEP;
	LocalVector<String> stack;
	for (const String &seg : cut(p_path)) {
		if (seg == ".") {
			continue;
		}
		if (seg == "..") {
			if (!stack.is_empty() && stack[stack.size() - 1] != "..") {
				stack.remove_at(stack.size() - 1);
			} else if (!rooted) {
				stack.push_back(seg);
			}
			continue;
		}
		stack.push_back(seg);
	}
	String body;
	for (uint32_t i = 0; i < stack.size(); i++) {
		if (i > 0) {
			body += "/";
		}
		body += stack[i];
	}
	if (rooted) {
		return "/" + body;
	}
	return body.is_empty() ? "." : body;
}

// Join path segments with a path separator.
String Path::join(const PackedStringArray &p_parts) {
	String out;
	for (int i = 0; i < p_parts.size(); i++) {
		const String p = p_parts[i];
		if (p.is_empty()) {
			continue;
		}
		if (out.is_empty()) {
			out = p;
		} else if (out.ends_with("://")) {
			out += p.lstrip("/"); // Preserve the scheme root separator.
		} else {
			out = out.rstrip("/") + "/" + p.lstrip("/");
		}
	}
	return normalize(out);
}

// Return the parent directory of a path.
String Path::dirname(const String &p_path) {
	const String d = p_path.rstrip("/").get_base_dir();
	return d.is_empty() ? "." : d;
}

// Return the final name in a path.
String Path::basename(const String &p_path, const String &p_suffix) {
	const String b = p_path.rstrip("/").get_file();
	if (!p_suffix.is_empty() && b.ends_with(p_suffix) && b != p_suffix) {
		return b.substr(0, b.length() - p_suffix.length());
	}
	return b;
}

// Return the extension of a path.
String Path::extname(const String &p_path) {
	const String e = p_path.get_extension();
	return e.is_empty() ? String() : "." + e;
}

// Check whether a path is absolute.
bool Path::is_absolute(const String &p_path) {
	return !p_path.is_empty() && p_path[0] == SEP;
}

// Resolve a path within a mount; return an empty string for an escape.
// Treat filenames literally here; URL percent decoding belongs at the input boundary.
String Path::under(const String &p_dir, const String &p_name) {
	// Normalize alternate separators before checking platform-independent containment.
	const String name = p_name.replace("\\", "/");
	if (name.is_empty() || name.begins_with("/")) {
		return String(); // Reject empty names and root-relative names.
	}

	const String base = normalize(p_dir.is_empty() ? String(".") : p_dir);
	const String full = normalize(base + "/" + name);
	// Check containment after normalization using the same predicate as Mount.
	if (!Mount::inside(base, full)) {
		return String();
	}
	return full;
}

// Return a path relative to a base path.
String Path::relative(const String &p_from, const String &p_to) {
	// Paths with different schemes cannot share a relative route.
	String from_rest;
	String to_rest;
	const String from_scheme = Mount::scheme_of(p_from, from_rest);
	const String to_scheme = Mount::scheme_of(p_to, to_rest);
	if (from_scheme != to_scheme) {
		return String();
	}
	const String from = normalize(from_scheme.is_empty() ? p_from : from_rest);
	const String to = normalize(to_scheme.is_empty() ? p_to : to_rest);
	// An absolute path and a relative path have no common depth reference.
	// Reject the combination instead of returning a meaningless route.
	if (is_absolute(from) != is_absolute(to)) {
		return String();
	}
	// A dot denotes the current location, not a segment requiring an extra parent step.
	const Vector<String> a = from == "." ? Vector<String>() : cut(from);
	const Vector<String> b = to == "." ? Vector<String>() : cut(to);
	int same = 0;
	while (same < a.size() && same < b.size() && a[same] == b[same]) {
		same++;
	}
	String out;
	for (int i = same; i < a.size(); i++) {
		out += out.is_empty() ? ".." : "/..";
	}
	for (int i = same; i < b.size(); i++) {
		out += out.is_empty() ? b[i] : "/" + b[i];
	}
	return out.is_empty() ? "." : out;
}

// Parse a path into its directory, basename, and extension.
Dictionary Path::parse(const String &p_path) {
	Dictionary out;
	const String ext = extname(p_path);
	out["dir"] = dirname(p_path);
	out["base"] = basename(p_path, String());
	out["ext"] = ext;
	out["name"] = basename(p_path, ext);
	return out;
}

// Format path components into a path string.
String Path::format(const Dictionary &p_parts) {
	const String dir = p_parts.get("dir", "");
	String base = p_parts.get("base", "");
	if (base.is_empty()) {
		base = String(p_parts.get("name", "")) + String(p_parts.get("ext", ""));
	}
	if (dir.is_empty() || dir == ".") {
		return base;
	}
	return dir.rstrip("/") + "/" + base;
}

// ---------------- HTML ----------------

// Replace HTML-sensitive characters with character references.
// Count the expanded size before allocating so escaping writes directly
// into one destination buffer instead of creating a string per character.
String Html::escape(const String &p_text) {
	const int n = p_text.length();
	if (n == 0) {
		return p_text;
	}
	const char32_t *r = p_text.ptr();

	// Count expansion and detect input that can be returned unchanged.
	int extra = 0;
	for (int i = 0; i < n; i++) {
		switch (r[i]) {
			case '&':
				extra += 4; // &amp;
				break;
			case '<':
			case '>':
				extra += 3; // &lt; &gt;
				break;
			case '"':
				extra += 5; // &quot;
				break;
			case '\'':
				extra += 4; // &#39;
				break;
			default:
				break;
		}
	}
	if (extra == 0) {
		return p_text; // Reuse the input when no characters require escaping.
	}

	String out;
	out.resize_uninitialized(n + extra + 1);
	char32_t *w = out.ptrw();
	int at = 0;
	for (int i = 0; i < n; i++) {
		const char *ent = nullptr;
		switch (r[i]) {
			case '&':
				ent = "&amp;";
				break;
			case '<':
				ent = "&lt;";
				break;
			case '>':
				ent = "&gt;";
				break;
			case '"':
				ent = "&quot;";
				break;
			case '\'':
				ent = "&#39;";
				break;
			default:
				w[at++] = r[i];
				continue;
		}
		while (*ent) {
			w[at++] = (char32_t)*ent++;
		}
	}
	w[at] = 0;
	return out;
}

// Decode escaped HTML character references.
String Html::unescape(const String &p_text) {
	// Decode ampersands last to avoid decoding a reference twice.
	return p_text.replace("&lt;", "<")
			.replace("&gt;", ">")
			.replace("&quot;", "\"")
			.replace("&#39;", "'")
			.replace("&apos;", "'")
			.replace("&nbsp;", " ")
			.replace("&amp;", "&");
}

// Escape an HTML attribute value safely.
String Html::attr(const String &p_value) {
	return "\"" + escape(p_value) + "\"";
}

// Validate tag and attribute names before assembling markup.
// Names cannot be quoted like values; reject characters that could inject another attribute.
bool Html::name_ok(const String &p_s) {
	if (p_s.is_empty()) {
		return false;
	}
	for (int i = 0; i < p_s.length(); i++) {
		const char32_t c = p_s[i];
		const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		if (!word && c != '-' && c != '_' && c != ':') {
			return false;
		}
	}
	return true;
}

// Require an alphanumeric partial name contained beneath the partials directory.
bool Html::partial_ok(const String &p_s) {
	if (p_s.is_empty()) {
		return false;
	}
	for (const String &seg : p_s.split("/", true)) {
		if (seg.is_empty()) {
			return false;
		}
		for (int i = 0; i < seg.length(); i++) {
			const char32_t c = seg[i];
			if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '-') {
				return false;
			}
		}
	}
	return true;
}

// Restrict HTML tag names to safe characters.
String Html::tag(const String &p_name, const String &p_body, const Dictionary &p_attrs) {
	// Reject names containing anything beyond letters and separators.
	// Returning the input as body text would hide the caller's invalid markup.
	ERR_FAIL_COND_V_MSG(!Html::name_ok(p_name), escape(p_body), vformat("GD.html.tag: bad tag name \"%s\"", p_name));
	String head = "<" + p_name;
	for (const Variant &k : p_attrs.keys()) {
		const String name = Pool::text(k);
		if (!Html::name_ok(name)) {
			WARN_PRINT(vformat("GD.html.tag: dropped bad attribute name \"%s\"", name));
			continue;
		}
		head += vformat(" %s=%s", name, attr(Pool::text(p_attrs[k])));
	}
	return head + ">" + escape(p_body) + "</" + p_name + ">";
}

namespace {

// Store a parsed dotted path to avoid parsing strings during rendering.
struct ValuePath {
	enum Base : uint8_t {
		HERE,
		ROOT,
		KEY,
		INDEX,
		FIRST,
		LAST,
	};
	Base base = HERE;
	int parents = 0;
	Vector<String> names;
};

// Store one parsed template instruction for reuse after a single source scan.
struct Piece {
	enum Kind {
		LIT, // Literal text.
		VAR, // Escaped value.
		RAW, // Unescaped value.
		IF, // Render the body when true.
		UNLESS, // Render the body when false.
		EACH, // Iterate an array or dictionary.
		WITH, // Render with a different current value.
		PART, // Render another template with the current value.
	};
	Kind kind = LIT;
	HtmlContext::Action action; // Escape action for the value's insertion context.
	String text; // Literal contents or partial name.
	PackedByteArray bytes; // Literal UTF-8 prepared after HTML context rewriting.
	ValuePath path; // Parsed lookup path for a value instruction.
	String part; // Context-specific derived name used by a partial.
	Vector<Piece> kids; // Block body.
	Vector<Piece> alt; // Else body.
};

// Release nested vectors without recursion, regardless of depth.
void clear_piece_tree(Vector<Piece> &r_root) {
	List<Vector<Piece>> todo;
	todo.push_back(static_cast<Vector<Piece> &&>(r_root));
	while (!todo.is_empty()) {
		Vector<Piece> &pieces = todo.front()->get();
		for (int i = 0; i < pieces.size(); i++) {
			Piece &piece = pieces.write[i];
			if (!piece.kids.is_empty()) {
				todo.push_back(static_cast<Vector<Piece> &&>(piece.kids));
			}
			if (!piece.alt.is_empty()) {
				todo.push_back(static_cast<Vector<Piece> &&>(piece.alt));
			}
		}
		pieces.clear();
		todo.pop_front();
	}
}

// Store immutable parsed instructions owned by the public renderer.
struct HtmlProgram {
	Vector<Piece> pieces;
	HashMap<String, Vector<Piece>> parts;

	// Release deeply nested blocks and partials without using the C++ call stack.
	~HtmlProgram() {
		clear_piece_tree(pieces);
		for (KeyValue<String, Vector<Piece>> &part : parts) {
			clear_piece_tree(part.value);
		}
		parts.clear();
	}
};

// Share immutable parsed results without copying the tree for one-shot calls.
using CachedHtmlProgram = std::shared_ptr<const HtmlProgram>;

constexpr int TEMPLATE_DEPTH_MAX = 100000; // Maximum nested template invocation depth.
constexpr int FILL_CACHE_MAX = 64; // Reusable parsed entries for one-shot calls; overflow evicts rather than rejects.
constexpr int64_t FILL_CACHE_BYTES = 8 * 1024 * 1024; // Cached source bytes for one-shot calls; larger sources are reparsed.

// Cache context-checked trees for one-shot templates without partials.
HashMap<String, CachedHtmlProgram> &fill_cache() {
	static HashMap<String, CachedHtmlProgram> cache;
	return cache;
}

// Track the source bytes retained by the one-shot cache.
int64_t &fill_cache_bytes() {
	static int64_t bytes = 0;
	return bytes;
}

// Protect the one-shot cache shared across threads.
Mutex &fill_cache_mutex() {
	static Mutex mutex;
	return mutex;
}

// Map a supported block name to its rendering instruction.
bool block_kind(const String &p_name, Piece::Kind &r_kind) {
	if (p_name == "if") {
		r_kind = Piece::IF;
		return true;
	}
	if (p_name == "unless") {
		r_kind = Piece::UNLESS;
		return true;
	}
	if (p_name == "each") {
		r_kind = Piece::EACH;
		return true;
	}
	if (p_name == "with") {
		r_kind = Piece::WITH;
		return true;
	}
	return false;
}

// Validate a path segment as a dictionary key or array index.
bool path_part_ok(const String &p_part) {
	if (p_part.is_empty() || (!is_unicode_identifier_start(p_part[0]) && !is_digit(p_part[0]))) {
		return false;
	}
	for (int i = 1; i < p_part.length(); i++) {
		if (!is_unicode_identifier_continue(p_part[i]) && p_part[i] != '-') {
			return false;
		}
	}
	return true;
}

// Validate supported dotted paths and compile their lookup steps.
bool compile_path(const String &p_raw, ValuePath &r_out) {
	String path = p_raw.strip_edges();
	int at = 0;
	while (at + 2 < path.length() && path[at] == '.' && path[at + 1] == '.' && path[at + 2] == '/') {
		r_out.parents++;
		at += 3;
	}
	if (at > 0) {
		path = path.substr(at);
	}
	if (path == "." || path == "this") {
		return true;
	}
	if (path.begins_with("./")) {
		path = path.substr(2);
	} else if (path.begins_with("this.")) {
		path = path.substr(5);
	} else if (path == "@root") {
		r_out.base = ValuePath::ROOT;
		return true;
	} else if (path.begins_with("@root.")) {
		r_out.base = ValuePath::ROOT;
		path = path.substr(6);
	} else if (path.begins_with("@")) {
		const int dot = path.find(".");
		const String key = dot < 0 ? path.substr(1) : path.substr(1, dot - 1);
		if (key == "key") {
			r_out.base = ValuePath::KEY;
		} else if (key == "index") {
			r_out.base = ValuePath::INDEX;
		} else if (key == "first") {
			r_out.base = ValuePath::FIRST;
		} else if (key == "last") {
			r_out.base = ValuePath::LAST;
		} else {
			return false;
		}
		if (dot < 0) {
			return true;
		}
		path = path.substr(dot + 1);
	}
	const PackedStringArray parts = path.split(".", true);
	if (parts.is_empty()) {
		return false;
	}
	for (const String &part : parts) {
		if (!path_part_ok(part)) {
			return false;
		}
		r_out.names.push_back(part);
	}
	return true;
}

// Track parser blocks explicitly so deeply nested templates do not consume the call stack.
struct CompileFrame {
	String end; // Closing tag name.
	Piece::Kind kind = Piece::LIT; // Instruction kind emitted when the block closes.
	ValuePath path; // Parsed value path referenced by this block.
	Vector<Piece> kids; // Block body.
	Vector<Piece> alt; // Else body.
	bool in_alt = false; // Whether the parser has encountered else.

	// Release deeply nested blocks without recursion after a parse failure.
	~CompileFrame() {
		clear_piece_tree(kids);
		clear_piece_tree(alt);
	}
};

// Return the instruction list receiving the current block's next instruction.
Vector<Piece> &compile_out(CompileFrame &r_frame) {
	return r_frame.in_alt ? r_frame.alt : r_frame.kids;
}

// Scan a template once and build rendering instructions with an explicit stack.
Error compile_tpl(const String &p_tpl, Vector<Piece> &r_out, String &r_bad) {
	LocalVector<CompileFrame> frames;
	frames.push_back(CompileFrame());
	const int n = p_tpl.length();
	int at = 0;
	while (at < n) {
		CompileFrame &frame = frames[frames.size() - 1];
		Vector<Piece> &out = compile_out(frame);
		const int open = p_tpl.find("{{", at);
		if (open < 0) {
			Piece lit;
			lit.text = p_tpl.substr(at);
			out.push_back(lit);
			at = n;
			break;
		}
		if (open > at) {
			Piece lit;
			lit.text = p_tpl.substr(at, open - at);
			out.push_back(lit);
		}
		// Skip long comments to their dedicated terminator without parsing internal braces.
		if (p_tpl.substr(open, 5) == "{{!--") {
			const int close = p_tpl.find("--}}", open + 5);
			if (close < 0) {
				r_bad = "comment";
				return ERR_PARSE_ERROR;
			}
			at = close + 4;
			continue;
		}
		// Parse triple braces before double braces because they insert raw values.
		if (p_tpl.substr(open, 3) == "{{{") {
			const int close = p_tpl.find("}}}", open + 3);
			if (close < 0) {
				r_bad = "raw value";
				return ERR_PARSE_ERROR;
			}
			Piece raw;
			raw.kind = Piece::RAW;
			const String path = p_tpl.substr(open + 3, close - open - 3).strip_edges();
			if (!compile_path(path, raw.path)) {
				r_bad = path;
				return ERR_PARSE_ERROR;
			}
			out.push_back(raw);
			at = close + 3;
			continue;
		}
		const int close = p_tpl.find("}}", open + 2);
		if (close < 0) {
			r_bad = "value";
			return ERR_PARSE_ERROR;
		}
		const String name = p_tpl.substr(open + 2, close - open - 2).strip_edges();
		at = close + 2;
		if (name.begins_with("!")) {
			continue; // Suppress both short comments and comments enclosed in double hyphens.
		}
		if (name == "else") {
			if (frames.size() == 1 || frame.in_alt) {
				r_bad = name;
				return ERR_PARSE_ERROR;
			}
			frame.in_alt = true;
			continue;
		}
		if (name.begins_with("/")) {
			const String end = name.substr(1).strip_edges();
			if (frames.size() == 1 || end != frame.end) {
				r_bad = name;
				return ERR_PARSE_ERROR;
			}
			Piece block;
			block.kind = frame.kind;
			block.path = frame.path;
			block.kids = static_cast<Vector<Piece> &&>(frame.kids);
			block.alt = static_cast<Vector<Piece> &&>(frame.alt);
			frames.remove_at(frames.size() - 1);
			compile_out(frames[frames.size() - 1]).push_back(block);
			continue;
		}
		if (name.begins_with("#")) {
			const Vector<String> words = name.substr(1).strip_edges().split_spaces(1);
			CompileFrame block;
			if (words.size() != 2 || !block_kind(words[0], block.kind) || !compile_path(words[1], block.path)) {
				r_bad = name;
				return ERR_PARSE_ERROR;
			}
			block.end = words[0];
			frames.push_back(block);
			continue;
		}
		if (name.begins_with(">")) {
			Piece part;
			part.kind = Piece::PART;
			part.text = name.substr(1).strip_edges();
			if (!Html::partial_ok(part.text)) {
				r_bad = name;
				return ERR_PARSE_ERROR;
			}
			out.push_back(part);
			continue;
		}
		Piece var;
		var.kind = Piece::VAR;
		if (!compile_path(name, var.path)) {
			r_bad = name;
			return ERR_PARSE_ERROR;
		}
		out.push_back(var);
	}
	if (frames.size() != 1) {
		r_bad = frames[frames.size() - 1].end;
		return ERR_PARSE_ERROR;
	}
	r_out = static_cast<Vector<Piece> &&>(frames[0].kids);
	return OK;
}

// Parse a template into an instruction tree.
Error compiled_of(const String &p_tpl, Vector<Piece> &r_out, String &r_bad) {
	return compile_tpl(p_tpl, r_out, r_bad);
}

// Store a prepared partial with its input and output HTML contexts.
struct PreparedPart {
	HtmlContext::Context end;
};

// Derive a partial name from its complete input context.
String context_key(const String &p_name, const HtmlContext::Context &p_ctx) {
	if (p_ctx.state == HtmlContext::TEXT && p_ctx.delim == HtmlContext::DELIM_NONE && p_ctx.url_part == HtmlContext::URL_NONE && p_ctx.js_part == HtmlContext::JS_REGEXP_PART && p_ctx.attr == HtmlContext::ATTR_NONE && p_ctx.element == HtmlContext::ELEMENT_NONE && p_ctx.js_braces.is_empty() && !p_ctx.attr_dynamic && !p_ctx.comment_line) {
		return p_name;
	}
	String key = vformat("%d:%s|%d|%d|%d|%d|%d|%d|%d|%d", p_name.length(), p_name,
			int(p_ctx.state), int(p_ctx.delim), int(p_ctx.url_part), int(p_ctx.js_part), int(p_ctx.attr), int(p_ctx.element),
			int(p_ctx.attr_dynamic), int(p_ctx.comment_line));
	for (int depth : p_ctx.js_braces) {
		key += "|" + itos(depth);
	}
	return key;
}

// Track partial sources and lookup state for one context-analysis pass.
struct PrepareState {
	PrepareState *parent = nullptr; // Committed state consulted during a trial.
	Dictionary given; // Partials supplied directly by the caller.
	HashMap<String, String> sources; // Source text of parsed partials.
	HashMap<String, Vector<Piece>> raw; // Unmodified syntax trees indexed by partial name.
	HashMap<String, Vector<Piece>> derived; // Prepared partials indexed by input context.
	HashMap<String, PreparedPart> ready; // Output contexts indexed by derived name.
	HashSet<String> called; // Derived names called during fixed-point analysis.
	Html::PartLoader loader = nullptr;
	void *loader_ctx = nullptr;

	PrepareState() = default;

	// Release only this trial's changes using an explicit stack.
	void clear() {
		for (KeyValue<String, Vector<Piece>> &piece : raw) {
			clear_piece_tree(piece.value);
		}
		for (KeyValue<String, Vector<Piece>> &piece : derived) {
			clear_piece_tree(piece.value);
		}
		parent = nullptr;
		given = Dictionary();
		sources.clear();
		raw.clear();
		derived.clear();
		ready.clear();
		called.clear();
		loader = nullptr;
		loader_ctx = nullptr;
	}

	// Layer a new fixed-point trial over committed state.
	void reset_overlay(PrepareState *p_parent) {
		clear();
		parent = p_parent;
	}

	// Commit only the state introduced by this trial to its parent.
	void commit() {
		ERR_FAIL_NULL(parent);
		for (const KeyValue<String, String> &item : sources) {
			parent->sources.insert(item.key, item.value);
		}
		for (const KeyValue<String, Vector<Piece>> &item : raw) {
			parent->raw.insert(item.key, item.value);
		}
		for (const KeyValue<String, Vector<Piece>> &item : derived) {
			parent->derived.insert(item.key, item.value);
		}
		for (const KeyValue<String, PreparedPart> &item : ready) {
			parent->ready.insert(item.key, item.value);
		}
		for (const String &key : called) {
			parent->called.insert(key);
		}
	}

	// Find loaded source text in this state or its ancestors.
	const String *source(const String &p_name) const {
		for (const PrepareState *state = this; state; state = state->parent) {
			const String *found = state->sources.getptr(p_name);
			if (found) {
				return found;
			}
		}
		return nullptr;
	}

	// Find an unmodified syntax tree in this state or its ancestors.
	const Vector<Piece> *raw_tree(const String &p_name) const {
		for (const PrepareState *state = this; state; state = state->parent) {
			const Vector<Piece> *found = state->raw.getptr(p_name);
			if (found) {
				return found;
			}
		}
		return nullptr;
	}

	// Find a derived partial's output context in this state or its ancestors.
	const PreparedPart *prepared(const String &p_key) const {
		for (const PrepareState *state = this; state; state = state->parent) {
			const PreparedPart *found = state->ready.getptr(p_key);
			if (found) {
				return found;
			}
		}
		return nullptr;
	}

	// Find a caller-supplied partial in this state or its ancestors.
	bool given_part(const String &p_name, String &r_source) const {
		for (const PrepareState *state = this; state; state = state->parent) {
			if (state->given.has(p_name)) {
				r_source = Pool::text(state->given[p_name]);
				return true;
			}
		}
		return false;
	}

	// Find the web partial loader in this state or its ancestors.
	PrepareState *loader_state() {
		for (PrepareState *state = this; state; state = state->parent) {
			if (state->loader) {
				return state;
			}
		}
		return nullptr;
	}

	// Release deeply nested partial trees using an explicit stack.
	~PrepareState() { clear(); }
};

// Obtain a partial's source directly or through the web loader.
Error prepare_source(PrepareState &r_state, const String &p_name, String &r_tpl) {
	const String *found = r_state.source(p_name);
	if (found) {
		r_tpl = *found;
		return OK;
	}
	Error err = OK;
	PrepareState *loader = nullptr;
	if (r_state.given_part(p_name, r_tpl)) {
		// Use source text supplied directly by the caller.
	} else if ((loader = r_state.loader_state())) {
		err = loader->loader(loader->loader_ctx, p_name, r_tpl);
	} else {
		err = ERR_FILE_NOT_FOUND;
	}
	if (err == OK) {
		r_state.sources.insert(p_name, r_tpl);
	}
	return err;
}

Error prepare_contexts(Vector<Piece> &r_pieces, HtmlContext::Context &r_ctx, PrepareState &r_state, String &r_bad, bool p_commit = true);

// Parse a partial once and return its unmodified syntax tree.
Error prepare_raw(PrepareState &r_state, const String &p_name, Vector<Piece> &r_out, String &r_bad) {
	const Vector<Piece> *found = r_state.raw_tree(p_name);
	if (found) {
		r_out = *found;
		return OK;
	}
	String source;
	Error err = prepare_source(r_state, p_name, source);
	if (err != OK) {
		return err;
	}
	err = compiled_of(source, r_out, r_bad);
	if (err == OK) {
		r_state.raw.insert(p_name, r_out);
	}
	return err;
}

// Store fixed-point inference as work items to avoid recursive partial calls.
struct PreparePartWork {
	PrepareState *target = nullptr; // Caller receiving the committed state.
	PrepareState trial; // Overlay containing only the current trial.
	Vector<Piece> raw; // Original syntax tree.
	Vector<Piece> child; // Derived tree for this input context.
	String name; // Original partial name.
	String key; // Derived name including the input context.
	HtmlContext::Context assumed; // Output context assumed for recursive calls.
	int pass = 0; // Fixed-point inference pass count.
	bool commit = true; // Whether to commit the derived name to the call instruction.

	// Release deeply nested partial trees without using the C++ call stack.
	~PreparePartWork() {
		clear_piece_tree(raw);
		clear_piece_tree(child);
	}
};

// Track context-analysis continuations explicitly instead of recursing through blocks.
struct PrepareFrame {
		enum Wait {
		READ,
		BODY,
		REPEAT,
		ALT,
		PART_BODY,
	};
	Vector<Piece> *pieces = nullptr; // Instruction list currently being visited.
	PrepareState *state = nullptr; // Partial analysis state.
	List<PrepareState>::Element *owned_state = nullptr; // State owned only by a range recheck.
	std::shared_ptr<PreparePartWork> part_work; // Partial fixed-point inference work.
	Piece *block = nullptr; // Block awaiting child completion.
	HtmlContext::Context ctx; // Current HTML context.
	HtmlContext::Context start; // Context at block entry.
	HtmlContext::Context yes; // Context after the block body.
	int at = 0; // Next instruction index.
	Wait wait = READ; // Continuation after a child returns.
	bool commit = true; // Whether escape results are committed to the executable tree.
};

// Merge both block branches and advance the parent instruction.
Error finish_block(PrepareFrame &r_frame, const HtmlContext::Context &p_no, String &r_bad) {
	const Error err = HtmlContext::join(r_frame.yes, p_no, r_frame.ctx, r_bad);
	if (err == OK) {
		r_frame.at++;
		r_frame.block = nullptr;
		r_frame.wait = PrepareFrame::READ;
	}
	return err;
}

// Own analysis continuations so pointers remain valid across partial loading.
struct PrepareWalk {
	List<PrepareFrame> frames; // Return positions and HTML contexts for child analysis.
	List<PrepareState> repeat_states; // Overlays owned by repeated each analysis.
	std::shared_ptr<PreparePartWork> pending; // Partial work awaiting source loading.
	HtmlContext::Context &context; // Destination for the final context.

	// Push the root analysis position and retain references to its owners.
	PrepareWalk(Vector<Piece> &p_pieces, HtmlContext::Context &p_ctx, PrepareState &p_state, bool p_commit) : context(p_ctx) {
		PrepareFrame root;
		root.pieces = &p_pieces;
		root.state = &p_state;
		root.ctx = p_ctx;
		root.commit = p_commit;
		frames.push_back(root);
	}

	// Resume from the saved AST position until completion or another partial request.
	Error step(String &r_bad);
};

// Walk the AST with an explicit stack to assign escaping contexts to values, branches, and partials.
Error PrepareWalk::step(String &r_bad) {
	while (!frames.is_empty()) {
		PrepareFrame &frame = frames.back()->get();
		if (frame.part_work && frame.wait == PrepareFrame::READ) {
			PreparePartWork &work = *frame.part_work;
			work.trial.reset_overlay(work.target);
			PreparedPart guess;
			guess.end = work.assumed;
			work.trial.ready.insert(work.key, guess);
			clear_piece_tree(work.child);
			work.child = work.raw;
			frame.wait = PrepareFrame::PART_BODY;
			PrepareFrame body;
			body.pieces = &work.child;
			body.state = &work.trial;
			body.ctx = work.assumed;
			frames.push_back(body);
			continue;
		}
		if (frame.at >= frame.pieces->size()) {
			const HtmlContext::Context end = frame.ctx;
			List<PrepareState>::Element *owned = frame.owned_state;
			frames.pop_back();
			if (owned) {
				repeat_states.erase(owned);
			}
			if (frames.is_empty()) {
				context = end;
				return OK;
			}
			PrepareFrame &parent = frames.back()->get();
			if (parent.part_work && parent.wait == PrepareFrame::PART_BODY) {
				PreparePartWork &work = *parent.part_work;
				if (!work.trial.called.has(work.key) || HtmlContext::same(end, work.assumed)) {
					PreparedPart done;
					done.end = end;
					work.trial.ready.insert(work.key, done);
					work.trial.derived.insert(work.key, work.child);
					work.trial.commit();
					const String key = work.key;
					const bool commit = work.commit;
					frames.pop_back();
					if (frames.is_empty()) {
						return ERR_BUG;
					}
					PrepareFrame &caller = frames.back()->get();
					if (commit) {
						caller.block->part = key;
					}
					caller.ctx = end;
					caller.at++;
					caller.block = nullptr;
					caller.wait = PrepareFrame::READ;
					continue;
				}
				if (++work.pass < 2) {
					work.assumed = end;
					parent.wait = PrepareFrame::READ;
					continue;
				}
				r_bad = "cannot compute output context for partial " + work.name;
				return ERR_PARSE_ERROR;
			}
			if (parent.wait == PrepareFrame::BODY) {
				parent.yes = end;
				if (parent.block->kind == Piece::EACH) {
					// A pass returning to its entry context needs no identical deterministic rescan.
					if (!HtmlContext::same(parent.start, end)) {
						parent.wait = PrepareFrame::REPEAT;
						PrepareFrame repeat;
						repeat.pieces = &parent.block->kids;
						repeat_states.push_back(PrepareState());
						repeat.owned_state = repeat_states.back();
						repeat.state = &repeat.owned_state->get();
						repeat.state->reset_overlay(parent.state);
						repeat.ctx = end;
						repeat.commit = false;
						frames.push_back(repeat);
						continue;
					}
				}
				if (!parent.block->alt.is_empty()) {
					parent.wait = PrepareFrame::ALT;
					PrepareFrame alt;
					alt.pieces = &parent.block->alt;
					alt.state = parent.state;
					alt.ctx = parent.start;
					alt.commit = parent.commit;
					frames.push_back(alt);
					continue;
				}
				const Error err = finish_block(parent, parent.start, r_bad);
				if (err != OK) {
					return err;
				}
				continue;
			}
			if (parent.wait == PrepareFrame::REPEAT) {
				HtmlContext::Context joined;
				Error err = HtmlContext::join(parent.yes, end, joined, r_bad);
				if (err != OK) {
					r_bad = "each loop re-entry: " + r_bad;
					return err;
				}
				parent.yes = joined;
				if (!parent.block->alt.is_empty()) {
					parent.wait = PrepareFrame::ALT;
					PrepareFrame alt;
					alt.pieces = &parent.block->alt;
					alt.state = parent.state;
					alt.ctx = parent.start;
					alt.commit = parent.commit;
					frames.push_back(alt);
					continue;
				}
				err = finish_block(parent, parent.start, r_bad);
				if (err != OK) {
					return err;
				}
				continue;
			}
			const Error err = finish_block(parent, end, r_bad);
			if (err != OK) {
				return err;
			}
			continue;
		}

		Piece &piece = frame.pieces->write[frame.at];
		if (piece.kind == Piece::LIT) {
			String text = piece.text;
			const Error err = HtmlContext::advance(frame.ctx, frame.commit ? piece.text : text, r_bad);
			if (err != OK) {
				return err;
			}
			// Cache the final literal only when committing this context-specific instruction.
			if (frame.commit) {
				Utf8Out bytes;
				if (!bytes.add(piece.text) || bytes.finish(piece.bytes) != OK) return ERR_OUT_OF_MEMORY;
			}
			frame.at++;
			continue;
		}
		if (piece.kind == Piece::VAR) {
			HtmlContext::Action action = piece.action;
			const Error err = HtmlContext::action(frame.ctx, frame.commit ? piece.action : action, r_bad);
			if (err != OK) {
				return err;
			}
			frame.at++;
			continue;
		}
		if (piece.kind == Piece::RAW) {
			if (frame.ctx.state != HtmlContext::TEXT || frame.ctx.delim != HtmlContext::DELIM_NONE) {
				r_bad = "raw value outside HTML text";
				return ERR_PARSE_ERROR;
			}
			frame.at++;
			continue;
		}
		if (piece.kind == Piece::PART) {
			if (frame.ctx.attr == HtmlContext::ATTR_SCRIPT_TYPE) {
				frame.ctx.attr_dynamic = true;
				frame.ctx.attr_text = String();
			}
			const String name = piece.text;
			const String key = context_key(name, frame.ctx);
			const PreparedPart *found = frame.state->prepared(key);
			if (found) {
				frame.state->called.insert(key);
				if (frame.commit) {
					piece.part = key;
				}
				frame.ctx = found->end;
				frame.at++;
				continue;
			}
			std::shared_ptr<PreparePartWork> work = pending ? pending : std::make_shared<PreparePartWork>();
			work->target = frame.state;
			work->name = name;
			work->key = key;
			work->assumed = frame.ctx;
			work->commit = frame.commit;
			const Error err = prepare_raw(*frame.state, name, work->raw, r_bad);
			if (err == ERR_BUSY) {
				pending = work;
				return err;
			}
			pending.reset();
			if (err != OK) {
				r_bad = r_bad.is_empty() ? name : r_bad;
				return err;
			}
			frame.block = &piece;
			frame.wait = PrepareFrame::PART_BODY;
			PrepareFrame part;
			part.part_work = work;
			frames.push_back(part);
			continue;
		}
		frame.block = &piece;
		frame.start = frame.ctx;
		frame.wait = PrepareFrame::BODY;
		PrepareFrame body;
		body.pieces = &piece.kids;
		body.state = frame.state;
		body.ctx = frame.ctx;
		body.commit = frame.commit;
		frames.push_back(body);
	}
	return ERR_BUG;
}

// Finish analyzing in-memory partials with the same explicit stack.
Error prepare_contexts(Vector<Piece> &r_pieces, HtmlContext::Context &r_ctx, PrepareState &r_state, String &r_bad, bool p_commit) {
	PrepareWalk walk(r_pieces, r_ctx, r_state, p_commit);
	return walk.step(r_bad);
}

// Own incremental analysis state that yields CPU work only when a file partial is needed.
struct HtmlBuildState {
	Vector<Piece> pieces; // Root syntax tree; its location remains stable across resumptions.
	HtmlProgram program; // Immutable tree used only for rendering after preparation.
	PrepareState prepared; // Cache of sources, raw trees, and derived contexts.
	HtmlContext::Context context; // HTML context after root analysis.
	std::unique_ptr<PrepareWalk> walk; // Suspended traversal and fixed-point trials.
	String need; // Next partial name to load on the I/O queue.
	String bad; // Failure location.
	Error error = OK; // Parse or render status.
	bool done = false; // Whether all contexts have passed safety checks.

	// Record the requested partial source without performing file I/O.
	static Error request(void *p_ctx, const String &p_name, String &) {
		static_cast<HtmlBuildState *>(p_ctx)->need = p_name;
		return ERR_BUSY;
	}

	// Release pointer-bearing traversal frames before destroying deep trees iteratively.
	~HtmlBuildState() {
		walk.reset();
		clear_piece_tree(pieces);
	}
};

// Stack current values and each metadata from the root for parent lookups.
struct Frame {
	Variant value;
	Variant key; // Dictionary key during each iteration.
	int at = -1; // Position within each, or -1 outside an iteration.
	int size = 0; // Number of items in the each input.
};

// Share current-value state and failure information during one render.
struct RenderState {
	LocalVector<Frame> frames;
	String bad;
	Error error = OK;
};

String text_of(const Variant &p_value);

// Render textual results while checking the native string representation boundary.
struct StringRender {
	StringBuilder out; // Pieces retained until the final string is assembled.
	int64_t chars = 0; // Accumulated character count.

	// Append one piece when the complete string remains representable.
	Error add(const String &p_text) {
		if (p_text.length() > INT_MAX - 1 - chars) return ERR_OUT_OF_MEMORY;
		chars += p_text.length();
		out += p_text;
		return OK;
	}

	// Append the textual literal retained by the prepared instruction.
	Error literal(const Piece &p_piece) { return add(p_piece.text); }

	// Apply the prepared escaping action within the remaining string capacity.
	Error escaped(const HtmlContext::Action &p_action, const Variant &p_value) {
		String text;
		const Error err = HtmlContext::render(p_action, p_value, INT_MAX - 1 - chars, text);
		return err == OK ? add(text) : err;
	}

	// Assemble one successful textual result.
	Error finish(String &r_out) {
		r_out = out.as_string();
		return OK;
	}
};

// Render byte results without building an intermediate full-document string.
struct ByteRender {
	Utf8Out out; // UTF-8 destination owned by this execution.

	// Append literal or explicitly unescaped text directly as bytes.
	Error add(const String &p_text) { return out.add(p_text) ? OK : ERR_OUT_OF_MEMORY; }

	// Copy the literal bytes prepared once during context analysis.
	Error literal(const Piece &p_piece) { return out.add(p_piece.bytes) ? OK : ERR_OUT_OF_MEMORY; }

	// Apply the same context decision used by textual rendering.
	Error escaped(const HtmlContext::Action &p_action, const Variant &p_value) { return HtmlContext::render_bytes(p_action, p_value, out); }

	// Finish the byte destination without copying the completed body.
	Error finish(PackedByteArray &r_out) { return out.finish(r_out); }
};

// Traverse dictionary or array children without accessing Object properties or methods.
Variant child_of(const Variant &p_value, const String &p_name) {
	if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary dict = p_value;
		return dict.get(p_name, Variant());
	}
	if (p_value.get_type() == Variant::ARRAY && p_name.is_valid_int()) {
		const Array list = p_value;
		const int64_t at = p_name.to_int();
		return at >= 0 && at < list.size() ? list[(int)at] : Variant();
	}
	return Variant();
}

// Resolve parsed parent, iteration, and root references from the current value.
Variant value_of(const LocalVector<Frame> &p_frames, const ValuePath &p_path) {
	if (p_frames.is_empty()) {
		return Variant();
	}
	int frame = p_frames.size() - 1 - p_path.parents;
	if (frame < 0) {
		return Variant();
	}
	Variant value;
	const Frame &now = p_frames[frame];
	switch (p_path.base) {
		case ValuePath::ROOT: value = p_frames[0].value; break;
		case ValuePath::KEY: value = now.key; break;
		case ValuePath::INDEX: value = now.at >= 0 ? Variant(now.at) : Variant(); break;
		case ValuePath::FIRST: value = now.at >= 0 ? Variant(now.at == 0) : Variant(); break;
		case ValuePath::LAST: value = now.at >= 0 ? Variant(now.at == now.size - 1) : Variant(); break;
		default: value = now.value; break;
	}
	for (const String &name : p_path.names) {
		value = child_of(value, name);
		if (value.get_type() == Variant::NIL) {
			break;
		}
	}
	return value;
}

// Treat empty values as false in template conditions.
bool truthy(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::NIL:
			return false;
		case Variant::BOOL:
			return p_value;
		case Variant::INT:
			return (int64_t)p_value != 0;
		case Variant::FLOAT:
			return (double)p_value != 0.0;
		case Variant::STRING:
			return !Pool::text(p_value).is_empty();
		case Variant::ARRAY:
			return !Array(p_value).is_empty();
		default:
			return true;
	}
}

// Render missing values as empty text and convert other values to display strings.
String text_of(const Variant &p_value) {
	return p_value.get_type() == Variant::NIL ? String() : Pool::text(p_value);
}

// Represent rendering as iterative work; only template calls increase invocation depth.
struct RenderTask {
	enum Kind {
		LIST,
		POP_FRAME,
		EACH_ARRAY,
		EACH_DICTIONARY,
	};
	Kind kind = LIST;
	const Vector<Piece> *pieces = nullptr;
	const Piece *piece = nullptr;
	Array values;
	Dictionary dict;
	int at = 0;
	int depth = 0;
};

// Push an instruction sequence onto the rendering work stack.
void push_list(LocalVector<RenderTask> &r_tasks, const Vector<Piece> &p_pieces, int p_depth) {
	RenderTask task;
	task.pieces = &p_pieces;
	task.depth = p_depth;
	r_tasks.push_back(task);
}

// Temporarily push a current value and schedule its removal after rendering.
void push_frame(RenderState &r_state, LocalVector<RenderTask> &r_tasks, const Vector<Piece> &p_pieces, int p_depth, const Variant &p_value, int p_at = -1, int p_size = 0, const Variant &p_key = Variant()) {
	Frame frame;
	frame.value = p_value;
	frame.key = p_key;
	frame.at = p_at;
	frame.size = p_size;
	r_state.frames.push_back(frame);
	RenderTask pop;
	pop.kind = RenderTask::POP_FRAME;
	r_tasks.push_back(pop);
	push_list(r_tasks, p_pieces, p_depth);
}

// Render parsed instructions using an explicit stack.
template <class Output = StringRender, class Result>
Error render_program(const HtmlProgram &p_program, const Dictionary &p_data, Result &r_out, String &r_bad) {
	RenderState state;
	Frame root;
	root.value = p_data;
	state.frames.push_back(root);
	Output out;
	LocalVector<RenderTask> tasks;
	push_list(tasks, p_program.pieces, 0);
	while (!tasks.is_empty() && state.error == OK) {
		RenderTask &task = tasks[ tasks.size() - 1 ];
		if (task.kind == RenderTask::POP_FRAME) {
			state.frames.remove_at(state.frames.size() - 1);
			tasks.remove_at(tasks.size() - 1);
			continue;
		}
		if (task.kind == RenderTask::EACH_ARRAY || task.kind == RenderTask::EACH_DICTIONARY) {
			if (task.at >= task.values.size()) {
				tasks.remove_at(tasks.size() - 1);
				continue;
			}
			const int at = task.at++;
			const int depth = task.depth;
			const Piece *piece = task.piece;
			const Variant key = task.kind == RenderTask::EACH_DICTIONARY ? task.values[at] : Variant();
			const Variant value = task.kind == RenderTask::EACH_DICTIONARY ? task.dict[key] : task.values[at];
			push_frame(state, tasks, piece->kids, depth, value, at, task.values.size(), key);
			continue;
		}
		if (!task.pieces || task.at >= task.pieces->size()) {
			tasks.remove_at(tasks.size() - 1);
			continue;
		}
		const Piece &pc = (*task.pieces)[task.at++];
		const int depth = task.depth;
		switch (pc.kind) {
			case Piece::LIT:
				state.error = out.literal(pc);
				break;
			case Piece::VAR: {
				state.error = out.escaped(pc.action, value_of(state.frames, pc.path));
			} break;
			case Piece::RAW:
				state.error = out.add(text_of(value_of(state.frames, pc.path)));
				break;
			case Piece::IF:
			case Piece::UNLESS: {
				const bool yes = truthy(value_of(state.frames, pc.path)) != (pc.kind == Piece::UNLESS);
				push_list(tasks, yes ? pc.kids : pc.alt, depth);
			} break;
			case Piece::WITH: {
				const Variant value = value_of(state.frames, pc.path);
				if (truthy(value)) {
					push_frame(state, tasks, pc.kids, depth, value);
				} else {
					push_list(tasks, pc.alt, depth);
				}
			} break;
			case Piece::EACH: {
				const Variant value = value_of(state.frames, pc.path);
				RenderTask each;
				each.piece = &pc;
				each.depth = depth;
				if (value.get_type() == Variant::ARRAY && !Array(value).is_empty()) {
					each.kind = RenderTask::EACH_ARRAY;
					each.values = value;
					tasks.push_back(each);
				} else if (value.get_type() == Variant::DICTIONARY && !Dictionary(value).is_empty()) {
					each.kind = RenderTask::EACH_DICTIONARY;
					each.dict = value;
					each.values = each.dict.keys();
					tasks.push_back(each);
				} else {
					push_list(tasks, pc.alt, depth);
				}
			} break;
			case Piece::PART: {
				if (depth >= TEMPLATE_DEPTH_MAX) {
					state.error = ERR_PARAMETER_RANGE_ERROR;
					state.bad = "template depth";
					break;
				}
				const HashMap<String, Vector<Piece>>::ConstIterator found = p_program.parts.find(pc.part);
				if (!found) {
					state.error = ERR_BUG;
					state.bad = pc.part;
					break;
				}
				push_list(tasks, found->value, depth + 1);
			} break;
		}
	}
	if (state.error == OK) {
		state.error = out.finish(r_out);
	}
	if (state.error == ERR_OUT_OF_MEMORY) state.bad = "output representation limit";
	r_bad = state.bad;
	return state.error;
}

// Analyze partial-free templates through HTML context checking and reuse identical source trees.
Error prepared_of(const String &p_tpl, CachedHtmlProgram &r_out, String &r_bad) {
	{
		MutexLock lock(fill_cache_mutex());
		const HashMap<String, CachedHtmlProgram>::ConstIterator found = fill_cache().find(p_tpl);
		if (found) {
			r_out = found->value;
			return OK;
		}
	}
	Vector<Piece> pieces;
	Error err = compiled_of(p_tpl, pieces, r_bad);
	PrepareState prepared;
	HtmlContext::Context context;
	if (err == OK) {
		err = prepare_contexts(pieces, context, prepared, r_bad);
	}
	if (err == OK) {
		err = HtmlContext::finish(context, r_bad);
	}
	if (err != OK) {
		clear_piece_tree(pieces);
		return err;
	}
	std::shared_ptr<HtmlProgram> made = std::make_shared<HtmlProgram>();
	made->pieces = static_cast<Vector<Piece> &&>(pieces);
	const int64_t source_bytes = p_tpl.utf8().length();
	if (source_bytes <= FILL_CACHE_BYTES) {
		MutexLock lock(fill_cache_mutex());
		const HashMap<String, CachedHtmlProgram>::ConstIterator found = fill_cache().find(p_tpl);
		if (found) {
			r_out = found->value;
			return OK;
		}
		if (fill_cache().size() >= FILL_CACHE_MAX || fill_cache_bytes() + source_bytes > FILL_CACHE_BYTES) {
			fill_cache().clear();
			fill_cache_bytes() = 0;
		}
		fill_cache().insert(p_tpl, made);
		fill_cache_bytes() += source_bytes;
	}
	r_out = made;
	return OK;
}

// Map invalid templates and execution limits to public failure categories.
Err::Kind template_error(Error p_err) {
	return p_err == ERR_OUT_OF_MEMORY || p_err == ERR_TIMEOUT || p_err == ERR_PARAMETER_RANGE_ERROR ? Err::LIMITED : Err::INVALID_DATA;
}
} // namespace

// Allocate analysis state; CPU-intensive work begins in step.
HtmlBuild::HtmlBuild() {
	state = memnew(HtmlBuildState);
}

// Release the AST and suspended state when their final owner is destroyed.
HtmlBuild::~HtmlBuild() {
	memdelete(static_cast<HtmlBuildState *>(state));
}

// Accept source loaded by I/O and resume from the saved analysis position.
Error HtmlBuild::step(const String &p_part) {
	HtmlBuildState &build = *static_cast<HtmlBuildState *>(state);
	if (build.done || build.error != OK) {
		return build.error;
	}
	if (!build.walk) {
		build.error = compiled_of(p_part, build.pieces, build.bad);
		if (build.error != OK) {
			return build.error;
		}
		build.prepared.loader = HtmlBuildState::request;
		build.prepared.loader_ctx = &build;
		build.walk = std::make_unique<PrepareWalk>(build.pieces, build.context, build.prepared, true);
	} else if (!build.need.is_empty()) {
		build.prepared.sources.insert(build.need, p_part);
		build.need = String();
	}
	const Error err = build.walk->step(build.bad);
	if (err == ERR_BUSY) {
		return err;
	}
	build.error = err == OK ? HtmlContext::finish(build.context, build.bad) : err;
	if (build.error == OK) {
		build.walk.reset();
		build.program.pieces = std::move(build.pieces);
		build.program.parts = std::move(build.prepared.derived);
		build.done = true;
	}
	return build.error;
}

// Pass the next unread partial name to the I/O queue.
String HtmlBuild::needed() const {
	return static_cast<const HtmlBuildState *>(state)->need;
}

// Render values into a fully checked tree without accessing files.
Error HtmlBuild::render(const Dictionary &p_data, String &r_out) {
	HtmlBuildState &build = *static_cast<HtmlBuildState *>(state);
	return build.done ? render_program(build.program, p_data, r_out, build.bad) : ERR_UNCONFIGURED;
}

// Retain the parse or render location without exposing it as an HTTP response.
Ref<R> HtmlBuild::failure(Error p_error) const {
	const HtmlBuildState &build = *static_cast<const HtmlBuildState *>(state);
	return R::err(vformat("template failed near \"%s\"", build.bad), template_error(p_error));
}

// Release parsed instructions.
GDHTMLTemplate::~GDHTMLTemplate() {
	if (program) {
		memdelete(static_cast<HtmlProgram *>(program));
		program = nullptr;
	}
}

// Render new values using the same immutable parsed program.
Ref<R> GDHTMLTemplate::execute(const Dictionary &p_data) const {
	if (!program) {
		return R::err("template is not initialized", Err::INTERRUPTED);
	}
	String out;
	String bad;
	const Error err = render_program(*static_cast<const HtmlProgram *>(program), p_data, out, bad);
	return err == OK ? R::ok(out) : R::err(vformat("template execution failed near \"%s\"", bad.left(128)), template_error(err));
}

// Render directly into response-ready UTF-8 while sharing the immutable parsed program.
Ref<R> GDHTMLTemplate::execute_bytes(const Dictionary &p_data) const {
	if (!program) return R::err("template is not initialized", Err::INTERRUPTED);
	PackedByteArray out;
	String bad;
	const Error err = render_program<ByteRender>(*static_cast<const HtmlProgram *>(program), p_data, out, bad);
	return err == OK ? R::ok(out) : R::err(vformat("template execution failed near \"%s\"", bad.left(128)), template_error(err));
}

// Register the parsed-template execution API.
void GDHTMLTemplate::_bind_methods() {
	ClassDB::bind_method(D_METHOD("execute", "data"), &GDHTMLTemplate::execute);
	ClassDB::bind_method(D_METHOD("execute_bytes", "data"), &GDHTMLTemplate::execute_bytes);
	ADD_RESULT("execute", "String");
	ADD_RESULT("execute_bytes", "PackedByteArray");
}

// Analyze template and partial HTML contexts into an immutable renderer.
Ref<R> Html::template_of(const String &p_tpl, const Dictionary &p_partials) {
	Vector<Piece> pieces;
	String bad;
	Error err = compiled_of(p_tpl, pieces, bad);
	PrepareState prepared;
	if (err == OK) {
		prepared.given = p_partials;
		HtmlContext::Context context;
		err = prepare_contexts(pieces, context, prepared, bad);
		if (err == OK) {
			err = HtmlContext::finish(context, bad);
		}
	}
	if (err != OK) {
		clear_piece_tree(pieces);
		return R::err(vformat("invalid template near \"%s\"", bad.left(128)), template_error(err));
	}
	Ref<GDHTMLTemplate> out;
	out.instantiate();
	HtmlProgram *program = memnew(HtmlProgram);
	program->pieces = static_cast<Vector<Piece> &&>(pieces);
	program->parts = static_cast<HashMap<String, Vector<Piece>> &&>(prepared.derived);
	out->program = program;
	return R::ok(out);
}

// Render a template and preserve syntax failures as explicit results.
Ref<R> Html::fill(const String &p_tpl, const Dictionary &p_data, const Dictionary &p_partials) {
	String out;
	String bad;
	const Error err = fill_checked(p_tpl, p_data, p_partials, out, bad);
	return err == OK ? R::ok(out) : R::err(vformat("invalid template near \"%s\"", bad), template_error(err));
}

// Render with distinct load and syntax failures for the HTTP layer.
Error Html::fill_checked(const String &p_tpl, const Dictionary &p_data, const Dictionary &p_partials, String &r_out, String &r_bad) {
	if (p_partials.is_empty()) {
		CachedHtmlProgram program;
		const Error err = prepared_of(p_tpl, program, r_bad);
		return err == OK ? render_program(*program, p_data, r_out, r_bad) : err;
	}
	Vector<Piece> pieces;
	Error err = compiled_of(p_tpl, pieces, r_bad);
	if (err != OK) {
		clear_piece_tree(pieces);
		return err;
	}
	PrepareState prepared;
	prepared.given = p_partials;
	HtmlContext::Context context;
	err = prepare_contexts(pieces, context, prepared, r_bad);
	if (err != OK) {
		clear_piece_tree(pieces);
		return err;
	}
	err = HtmlContext::finish(context, r_bad);
	if (err != OK) {
		clear_piece_tree(pieces);
		return err;
	}
	HtmlProgram program;
	program.pieces = static_cast<Vector<Piece> &&>(pieces);
	program.parts = static_cast<HashMap<String, Vector<Piece>> &&>(prepared.derived);
	return render_program(program, p_data, r_out, r_bad);
}

// ---------------- Strings ----------------

int Text::distance(const String &p_a, const String &p_b) {
	if (p_a == p_b) {
		return 0;
	}
	const int an = p_a.length();
	const int bn = p_b.length();
	if (an == 0) {
		return bn;
	}
	if (bn == 0) {
		return an;
	}
	LocalVector<int> prev;
	LocalVector<int> cur;
	prev.resize(bn + 1);
	cur.resize(bn + 1);
	for (int j = 0; j <= bn; j++) {
		prev[j] = j;
	}
	const char32_t *ra = p_a.ptr();
	const char32_t *rb = p_b.ptr();
	for (int i = 1; i <= an; i++) {
		cur[0] = i;
		for (int j = 1; j <= bn; j++) {
			const int cost = ra[i - 1] == rb[j - 1] ? 0 : 1;
			cur[j] = MIN(MIN(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
		}
		for (int j = 0; j <= bn; j++) {
			prev[j] = cur[j];
		}
	}
	return prev[bn];
}

// Convert a string to snake_case.
String Text::snake(const String &p_text) {
	String out;
	const int n = p_text.length();
	const char32_t *r = p_text.ptr();
	for (int i = 0; i < n; i++) {
		const char32_t c = r[i];
		if (c >= 'A' && c <= 'Z') {
			if (!out.is_empty() && !out.ends_with("_")) {
				out += "_";
			}
			out += String::chr(c + 32);
		} else if (c == ' ' || c == '-') {
			if (!out.ends_with("_")) {
				out += "_";
			}
		} else {
			out += String::chr(c);
		}
	}
	return out;
}

// Convert a string to camelCase.
String Text::camel(const String &p_text) {
	const Vector<String> parts = snake(p_text).split("_", false);
	if (parts.is_empty()) {
		return String();
	}
	String out = parts[0];
	for (int i = 1; i < parts.size(); i++) {
		out += parts[i].substr(0, 1).to_upper() + parts[i].substr(1);
	}
	return out;
}

// Format a string as title words.
String Text::title(const String &p_text) {
	const Vector<String> parts = p_text.split(" ", false);
	String out;
	for (int i = 0; i < parts.size(); i++) {
		if (i > 0) {
			out += " ";
		}
		out += parts[i].substr(0, 1).to_upper() + parts[i].substr(1).to_lower();
	}
	return out;
}

// Format rows as a text table with aligned columns.
String Text::table(const Array &p_rows, int p_gap) {
	if (p_rows.is_empty()) {
		return String();
	}
	// Measure the maximum width of each column.
	LocalVector<int> widths;
	for (int i = 0; i < p_rows.size(); i++) {
		const Array row = p_rows[i];
		for (int c = 0; c < row.size(); c++) {
			const int w = Pool::text(row[c]).length();
			while ((int)widths.size() <= c) {
				widths.push_back(0);
			}
			if (w > widths[c]) {
				widths[c] = w;
			}
		}
	}
	// Do not pad the final column, to avoid trailing whitespace.
	String out;
	for (int i = 0; i < p_rows.size(); i++) {
		const Array row = p_rows[i];
		for (int c = 0; c < row.size(); c++) {
			const String cell = Pool::text(row[c]);
			out += cell;
			if (c < row.size() - 1) {
				const int pad = widths[c] - cell.length() + p_gap;
				for (int k = 0; k < pad; k++) {
					out += " ";
				}
			}
		}
		out += "\n";
	}
	return out;
}

// Return the candidate with the smallest edit distance.
String Text::closest(const String &p_word, const PackedStringArray &p_options) {
	String best;
	int best_d = 0x7fffffff;
	for (const String &o : p_options) {
		const int d = distance(p_word, o);
		if (d < best_d) {
			best_d = d;
			best = o;
		}
	}
	return best_d <= MAX(2, p_word.length() / 3) ? best : String();
}

// Shorten a long string and append an ellipsis.
String Text::ellipsis(const String &p_text, int p_width) {
	if (p_text.length() <= p_width) {
		return p_text;
	}
	if (p_width <= 1) {
		return p_text.substr(0, p_width);
	}
	return p_text.substr(0, p_width - 1) + U"…";
}

// Format a byte count with a readable unit.
String Text::size_of(int64_t p_bytes) {
	const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" }; // Units advance in powers of 1024.
	double v = (double)p_bytes;
	int at = 0;
	while (v >= 1024.0 && at < 5) {
		v /= 1024.0;
		at++;
	}
	if (at == 0) {
		return vformat("%d B", p_bytes);
	}
	return vformat("%.1f %s", v, String(units[at]));
}

// Format seconds as a readable duration.
String Text::duration(double p_ms) {
	if (p_ms < 1.0) {
		return vformat("%.0f us", p_ms * 1000.0);
	}
	if (p_ms < 1000.0) {
		return vformat("%.1f ms", p_ms);
	}
	const double sec = p_ms / 1000.0;
	if (sec < 60.0) {
		return vformat("%.2f s", sec);
	}
	const int m = (int)(sec / 60.0);
	return vformat("%dm %.0fs", m, sec - m * 60.0);
}

// Check whether standard input is an interactive terminal.
bool Text::stdin_tty() {
	return OS::get_singleton()->get_stdin_type() == OS::STD_HANDLE_CONSOLE;
}

// Check whether standard output is an interactive terminal.
bool Text::stdout_tty() {
	return OS::get_singleton()->get_stdout_type() == OS::STD_HANDLE_CONSOLE;
}

// Check whether standard error is an interactive terminal.
bool Text::stderr_tty() {
	return OS::get_singleton()->get_stderr_type() == OS::STD_HANDLE_CONSOLE;
}

// Apply ANSI decoration only when enabled for the destination.
String Text::paint(const String &p_text, int p_code, bool p_enabled) {
	if (!p_enabled) {
		return p_text;
	}
	return vformat("\x1b[%dm%s\x1b[0m", p_code, p_text);
}

// ---------------- URL ----------------

namespace {

// Check whether a character is allowed in a URL registered name.
bool url_reg_char(char32_t p_c) {
	const bool alpha = (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z');
	const bool digit = p_c >= '0' && p_c <= '9';
	return alpha || digit || p_c == '!' || p_c == '$' || p_c == '&' || p_c == '\'' || p_c == '(' || p_c == ')' || p_c == '*' || p_c == '+' || p_c == ',' || p_c == '-' || p_c == '.' || p_c == '_' || p_c == '~' || p_c == ';' || p_c == '=';
}

// Validate a URL host as a registered name or canonical IP address.
bool url_host_ok(const String &p_host, bool p_bracketed) {
	if (p_host.is_empty()) {
		return false;
	}
	if (p_bracketed) {
		return p_host.contains(":") && !p_host.contains("%") && GDDatagram::is_ip(p_host);
	}
	if (p_host.contains(":") || p_host.contains("[") || p_host.contains("]")) {
		return false;
	}
	for (int i = 0; i < p_host.length(); i++) {
		const char32_t c = p_host[i];
		if (c == '%' && i + 2 < p_host.length() && is_hex_digit(p_host[i + 1]) && is_hex_digit(p_host[i + 2])) {
			i += 2;
			continue;
		}
		if (!url_reg_char(c)) {
			return false;
		}
	}
	return true;
}

// Decode the next byte; return false for a malformed percent escape.
bool next_byte(const uint8_t *p_data, int p_len, bool p_plus_space, int &r_i, uint8_t &r_b) {
	const uint8_t c = p_data[r_i];
	if (c == '%') {
		if (r_i + 2 >= p_len || !is_hex_digit(p_data[r_i + 1]) || !is_hex_digit(p_data[r_i + 2])) {
			return false;
		}
		const char hex[3] = { (char)p_data[r_i + 1], (char)p_data[r_i + 2], 0 };
		r_b = (uint8_t)strtol(hex, nullptr, 16);
		r_i += 3;
	} else {
		r_b = (c == '+' && p_plus_space) ? ' ' : c;
		r_i++;
	}
	return true;
}

// Decode query bytes without applying path or host restrictions.
Ref<R> query_part(const String &p_raw) {
	const CharString src = p_raw.utf8();
	PackedByteArray bytes;
	if (bytes.resize(src.length()) != OK) return R::err("cannot allocate query value", Err::LIMITED);
	int at = 0, used = 0;
	while (at < src.length()) {
		if (!next_byte((const uint8_t *)src.ptr(), src.length(), true, at, bytes.ptrw()[used])) {
			return R::err("query contains a bad percent escape", Err::INVALID_DATA);
		}
		used++;
	}
	const Variant value = utf8_value(bytes.ptr(), used);
	const Ref<R> result = value;
	return result.is_valid() ? result : R::ok(value);
}

// Decode independent pairs, retaining valid entries and the first failure.
Ref<R> decode_query_inner(const String &p_raw) {
	Dictionary out;
	Ref<Err> error;
	int from = 0;
	for (int i = 0; i <= p_raw.length(); i++) {
		if (i < p_raw.length() && p_raw[i] != '&') continue;
		const String pair = p_raw.substr(from, i - from);
		from = i + 1;
		if (pair.is_empty()) continue;
		const int eq = pair.find_char('=');
		if (pair.contains(";")) {
			if (error.is_null()) error = Err::make("query contains an unescaped semicolon", Err::INVALID_DATA);
			continue;
		}
		const Ref<R> key = query_part(eq < 0 ? pair : pair.substr(0, eq));
		const Ref<R> value = key->get_ok() ? query_part(eq < 0 ? String() : pair.substr(eq + 1)) : key;
		if (!value->get_ok()) {
			if (error.is_null()) error = value->get_e();
			continue;
		}
		Array values = out.get(key->get_v(), Array());
		values.push_back(value->get_v());
		out[key->get_v()] = values;
	}
	return error.is_valid() ? R::err(error, Err::NONE, out) : R::ok(out);
}

} // namespace

// Decode a URL query into a key-value dictionary.
Ref<R> Url::decode_query(const String &p_raw) {
	return decode_query_inner(p_raw);
}

// Validate percent-decoded input as UTF-8 without control characters.
//
// Lengthless string construction could truncate a decoded NUL, and replacement
// decoding could collapse distinct invalid byte sequences to the same character.
// Reject both cases so validation and use refer to the same value.
bool Url::decode_check(const uint8_t *p_data, int p_len, bool p_plus_space, CharString *r_out) {
	static const uint32_t least[4] = { 0, 0x80, 0x800, 0x10000 }; // Minimum code point for each UTF-8 sequence width.
	int i = 0;
	while (i < p_len) {
		uint8_t b = 0;
		if (!next_byte(p_data, p_len, p_plus_space, i, b)) {
			return false;
		}
		if (r_out) {
			*r_out += (char)b;
		}
		int extra = 0; // Number of continuation bytes.
		uint32_t ord = b; // Decoded code point.
		if (b >= 0x80) {
			if ((b & 0xe0) == 0xc0) {
				extra = 1;
				ord = b & 0x1f;
			} else if ((b & 0xf0) == 0xe0) {
				extra = 2;
				ord = b & 0x0f;
			} else if ((b & 0xf8) == 0xf0) {
				extra = 3;
				ord = b & 0x07;
			} else {
				return false; // Reject stray continuation bytes and invalid leading bytes.
			}
		}
		for (int k = 0; k < extra; k++) {
			uint8_t c = 0;
			if (i >= p_len || !next_byte(p_data, p_len, p_plus_space, i, c) || (c & 0xc0) != 0x80) {
				return false; // Reject missing or malformed continuation bytes.
			}
			if (r_out) {
				*r_out += (char)c;
			}
			ord = (ord << 6) | (c & 0x3f);
		}
		if (ord < least[extra] || ord > 0x10ffff || (ord >= 0xd800 && ord <= 0xdfff)) {
			return false; // Reject overlong sequences, surrogate code points, and values outside Unicode.
		}
		if (ord < 0x20 || ord == 0x7f || (ord >= 0x80 && ord <= 0x9f)) {
			return false; // Reject C0, DEL, and C1 control characters.
		}
	}
	return true;
}

// Percent-decode a URL component.
bool Url::decode_part(const String &p_raw, bool p_plus_space, String &r_out) {
	const CharString src = p_raw.utf8();
	CharString res;
	if (!decode_check((const uint8_t *)src.get_data(), src.length(), p_plus_space, &res)) {
		return false;
	}
	// Prefix a marker during conversion to preserve an input-leading BOM, then remove the marker.
	CharString guarded;
	guarded.resize_uninitialized(res.length() + 2);
	guarded.ptrw()[0] = 'x';
	if (res.length() > 0) {
		memcpy(guarded.ptrw() + 1, res.get_data(), res.length());
	}
	guarded.ptrw()[res.length() + 1] = 0;
	r_out = String::utf8(guarded.get_data(), guarded.length()).substr(1);
	return true;
}

namespace {

// Preserve explicit byte values when encoding query keys and values.
PackedByteArray query_bytes(const Variant &p_value) {
	return p_value.get_type() == Variant::PACKED_BYTE_ARRAY ? PackedByteArray(p_value) : Pool::text(p_value).to_utf8_buffer();
}

// Escape one byte sequence with query-specific space and reserved-character rules.
String query_escape(const PackedByteArray &p_bytes) {
	static const char hex[] = "0123456789ABCDEF"; // Uppercase percent-escape alphabet.
	int64_t size = p_bytes.size();
	for (int64_t i = 0; i < p_bytes.size(); i++) {
		const uint8_t c = p_bytes[i];
		if (!is_ascii_alphanumeric_char(c) && c != '-' && c != '_' && c != '.' && c != '~' && c != ' ') size += 2;
	}
	CharString out;
	if (size > INT_MAX - 1 || out.resize_uninitialized(size + 1) != OK) return String();
	char *dst = out.ptrw();
	int at = 0;
	for (int64_t i = 0; i < p_bytes.size(); i++) {
		const uint8_t c = p_bytes[i];
		if (is_ascii_alphanumeric_char(c) || c == '-' || c == '_' || c == '.' || c == '~') dst[at++] = c;
		else if (c == ' ') dst[at++] = '+';
		else { dst[at++] = '%'; dst[at++] = hex[c >> 4]; dst[at++] = hex[c & 15]; }
	}
	dst[at] = 0;
	return String::ascii(Span<char>(dst, at));
}

} // namespace

// Encode every query value in byte-sorted key order.
String Url::encode_query(const Dictionary &p_query) {
	struct Item {
		PackedByteArray key; // Raw key bytes determine ordering.
		Variant value; // Scalar convenience input or all values for this key.
		// Compare byte sequences without Unicode normalization or type-based ordering.
		bool operator<(const Item &p_other) const {
			const int64_t n = MIN(key.size(), p_other.key.size());
			const int order = n ? memcmp(key.ptr(), p_other.key.ptr(), n) : 0;
			return order ? order < 0 : key.size() < p_other.key.size();
		}
	};
	LocalVector<Item> items;
	for (const Variant &key : p_query.keys()) items.push_back({ query_bytes(key), p_query[key] });
	items.sort();
	StringBuilder out;
	bool first = true;
	for (const Item &item : items) {
		const String key = query_escape(item.key);
		Array values;
		if (item.value.get_type() == Variant::ARRAY || item.value.get_type() == Variant::PACKED_STRING_ARRAY) values = item.value;
		else values.push_back(item.value);
		for (const Variant &value : values) {
			if (!first) out.append("&");
			first = false;
			out.append(key); out.append("="); out.append(query_escape(query_bytes(value)));
		}
	}
	return out.as_string();
}

// Return the default port for a URL scheme.
int Url::default_port(const String &p_scheme) {
	return p_scheme == "https" ? 443 : 80;
}

// Separate host and port using matching brackets for IPv6 addresses such as [::1]:6379.
// Leave port validity to the caller: split_host requires a number, while permission
// matching only needs to distinguish an omitted port from an explicitly supplied one.
String Url::host_port(const String &p_text, String &r_port) {
	r_port = String();
	if (p_text.begins_with("[")) {
		const int close = p_text.find_char(']');
		if (close < 1) {
			return String();
		}
		const String tail = p_text.substr(close + 1);
		if (!tail.is_empty()) {
			if (!tail.begins_with(":")) {
				return String();
			}
			r_port = tail.substr(1);
		}
		return p_text.substr(1, close - 1);
	}
	const int colon = p_text.rfind_char(':');
	if (colon < 0) {
		return p_text;
	}
	r_port = p_text.substr(colon + 1);
	return p_text.substr(0, colon);
}

// Split host and port while validating their syntax.
Dictionary Url::split_host(const String &p_text, int64_t p_default_port) {
	Dictionary out;
	if (p_default_port < Limit::PORT_MIN || p_default_port > Limit::PORT_MAX) {
		return out;
	}
	String rest = p_text;

	// Treat the prefix before the final at sign as user information.
	const int at = rest.rfind_char('@');
	if (at >= 0) {
		const String who = rest.substr(0, at);
		rest = rest.substr(at + 1);
		const int mark = who.find_char(':');
		out["user"] = mark >= 0 ? who.substr(0, mark) : who;
		out["password"] = mark >= 0 ? who.substr(mark + 1) : String();
	}

	String port_txt;
	const String host = host_port(rest, port_txt);
	const bool bracketed = rest.begins_with("[");
	if (!url_host_ok(host, bracketed)) {
		return Dictionary();
	}
	int64_t port = p_default_port;
	if (!port_txt.is_empty()) {
		// Reject nonnumeric ports instead of silently connecting to port zero.
		if (!port_txt.is_valid_int()) {
			return Dictionary();
		}
		port = port_txt.to_int();
		if (port < Limit::PORT_MIN || port > Limit::PORT_MAX) {
			return Dictionary();
		}
	}
	out["host"] = host;
	out["port"] = int(port);
	return out;
}

// Parse a URL into its components.
Ref<R> Url::parse(const String &p_raw) {
	for (int i = 0; i < p_raw.length(); i++) {
		if (p_raw[i] < 0x20 || p_raw[i] == 0x7f) {
			return R::err("URL contains a control character", Err::INVALID_DATA);
		}
	}
	String rest = p_raw;
	const int sep = rest.find("://");
	if (sep < 0) {
		return R::err("URL has no scheme", Err::INVALID_DATA);
	}
	const String scheme = rest.substr(0, sep).to_lower();
	if (scheme != "http" && scheme != "https") {
		return R::err("URL scheme must be http or https", Err::INVALID_DATA);
	}
	rest = rest.substr(sep + 3);

	String fragment;
	const int hash_at = rest.find_char('#');
	if (hash_at >= 0) {
		fragment = rest.substr(hash_at + 1);
		rest = rest.substr(0, hash_at);
	}

	Dictionary query;
	String raw_query;
	const int q_at = rest.find_char('?');
	if (q_at >= 0) {
		raw_query = rest.substr(q_at + 1);
		query = decode_query_inner(raw_query)->get_v();
		rest = rest.substr(0, q_at);
	}

	String path = "/";
	const int slash = rest.find_char('/');
	if (slash >= 0) {
		path = rest.substr(slash);
		rest = rest.substr(0, slash);
	}

	const Dictionary hp = split_host(rest, default_port(scheme));
	if (hp.is_empty()) {
		return R::err("URL host is invalid", Err::INVALID_DATA);
	}

	Dictionary u;
	u["scheme"] = scheme;
	u["host"] = hp["host"];
	u["port"] = hp["port"];
	if (hp.has("user")) {
		u["user"] = hp["user"];
		u["password"] = hp["password"];
	}
	u["path"] = path;
	u["query"] = query;
	if (q_at >= 0) u["raw_query"] = raw_query;
	u["fragment"] = fragment;
	return R::ok(u);
}

// Build an HTTP request target from URL components.
String Url::request_target(const Dictionary &p_url) {
	String out = Pool::text(p_url.get("path", "/"));
	if (out.is_empty()) {
		out = "/";
	}
	if (p_url.has("raw_query")) return out + "?" + String(p_url["raw_query"]);
	const Dictionary query = p_url.get("query", Dictionary());
	if (!query.is_empty()) {
		out += "?" + encode_query(query);
	}
	return out;
}

// Assemble a URL string from its components.
String Url::build(const Dictionary &p_url) {
	const String scheme = Pool::text(p_url.get("scheme", "http"));
	// Restore IPv6 brackets so the port separator is unambiguous.
	String host = Pool::text(p_url.get("host", ""));
	if (host.contains(":") && !host.begins_with("[")) {
		host = "[" + host + "]";
	}
	String out = vformat("%s://%s", scheme, host);
	const int port = p_url.get("port", 0);
	if (port != 0 && port != default_port(scheme)) {
		out += vformat(":%d", port);
	}
	out += request_target(p_url);
	const String fragment = Pool::text(p_url.get("fragment", ""));
	if (!fragment.is_empty()) {
		out += "#" + fragment;
	}
	return out;
}

// ---------------- Versions ----------------

namespace {

// Parse a semantic version into its components.
Ref<R> ver_of(const String &p_raw) {
	String s = p_raw.strip_edges().lstrip("vV=");
	Dictionary v;
	String build;
	PackedStringArray pre;

	const int plus = s.find_char('+');
	if (plus >= 0) {
		build = s.substr(plus + 1);
		s = s.substr(0, plus);
	}
	const int dash = s.find_char('-');
	if (dash >= 0) {
		pre = s.substr(dash + 1).split(".", false);
		s = s.substr(0, dash);
	}
	const PackedStringArray nums = s.split(".");
	if (nums.size() < 1 || !nums[0].is_valid_int()) {
		return R::err(vformat("invalid version \"%s\"", p_raw), Err::INVALID_DATA);
	}
	v["major"] = nums[0].to_int();
	v["minor"] = (nums.size() > 1 && nums[1].is_valid_int()) ? nums[1].to_int() : 0;
	v["patch"] = (nums.size() > 2 && nums[2].is_valid_int()) ? nums[2].to_int() : 0;
	v["pre"] = pre;
	v["build"] = build;
	return R::ok(v);
}

// Compare prerelease segments numerically when both are numbers, otherwise lexically.
int cmp_part(const String &a, const String &b) {
	const bool an = a.is_valid_int();
	const bool bn = b.is_valid_int();
	if (an && bn) {
		const int64_t x = a.to_int();
		const int64_t y = b.to_int();
		return x == y ? 0 : (x < y ? -1 : 1);
	}
	if (an) {
		return -1; // Numeric identifiers sort before nonnumeric identifiers.
	}
	if (bn) {
		return 1;
	}
	return a == b ? 0 : (a < b ? -1 : 1);
}

// Compare version precedence; return negative, zero, or positive and ignore build metadata.
int ver_cmp(const Dictionary &p_a, const Dictionary &p_b) {
	const int64_t amaj = p_a.get("major", 0);
	const int64_t bmaj = p_b.get("major", 0);
	if (amaj != bmaj) {
		return amaj < bmaj ? -1 : 1;
	}
	const int64_t amin = p_a.get("minor", 0);
	const int64_t bmin = p_b.get("minor", 0);
	if (amin != bmin) {
		return amin < bmin ? -1 : 1;
	}
	const int64_t apat = p_a.get("patch", 0);
	const int64_t bpat = p_b.get("patch", 0);
	if (apat != bpat) {
		return apat < bpat ? -1 : 1;
	}
	const PackedStringArray ap = p_a.get("pre", PackedStringArray());
	const PackedStringArray bp = p_b.get("pre", PackedStringArray());
	// A version with prerelease identifiers sorts below a release.
	if (ap.is_empty() && bp.is_empty()) {
		return 0;
	}
	if (ap.is_empty()) {
		return 1;
	}
	if (bp.is_empty()) {
		return -1;
	}
	const int n = MIN(ap.size(), bp.size());
	for (int i = 0; i < n; i++) {
		const int c = cmp_part(ap[i], bp[i]);
		if (c != 0) {
			return c;
		}
	}
	if (ap.size() == bp.size()) {
		return 0;
	}
	return ap.size() < bp.size() ? -1 : 1;
}

// Check whether a version satisfies one constraint.
bool one_cond(const Dictionary &v, const String &cond) {
	String op;
	String rest = cond;
	static const char *ops[] = { ">=", "<=", "^", "~", ">", "<", "=", nullptr };
	for (int i = 0; ops[i]; i++) {
		if (cond.begins_with(ops[i])) {
			op = ops[i];
			rest = cond.substr(strlen(ops[i]));
			break;
		}
	}
	const Ref<R> parsed = ver_of(rest);
	if (parsed->get_e().is_valid()) {
		return false;
	}
	const Dictionary other = parsed->get_v();
	const int c = ver_cmp(v, other);
	const int64_t omaj = other.get("major", 0);
	const int64_t omin = other.get("minor", 0);
	const int64_t vmaj = v.get("major", 0);
	const int64_t vmin = v.get("minor", 0);

	if (op == ">=") {
		return c >= 0;
	}
	if (op == "<=") {
		return c <= 0;
	}
	if (op == ">") {
		return c > 0;
	}
	if (op == "<") {
		return c < 0;
	}
	if (op == "^") {
		// Keep the leading version component unchanged; for 0.x, preserve the minor component.
		if (omaj > 0) {
			return c >= 0 && vmaj == omaj;
		}
		if (omin > 0) {
			return c >= 0 && vmaj == 0 && vmin == omin;
		}
		return c >= 0 && vmaj == 0 && vmin == 0;
	}
	if (op == "~") {
		return c >= 0 && vmaj == omaj && vmin == omin;
	}
	return c == 0;
}

} // namespace

// Check whether a version satisfies a range.
bool Semver::satisfies(const Dictionary &p_v, const String &p_range) {
	for (const String &part : p_range.split("||")) {
		const String txt = part.strip_edges();
		if (txt.is_empty() || txt == "*") {
			return true;
		}
		bool all = true;
		for (const String &cond : txt.split(" ", false)) {
			if (!one_cond(p_v, cond)) {
				all = false;
				break;
			}
		}
		if (all) {
			return true;
		}
	}
	return false;
}

// Parse a semantic version into a result value.
Ref<R> Semver::parse(const String &p_raw) {
	return ver_of(p_raw);
}

// Require an unambiguous complete semantic version for package versions.
bool Semver::is_canonical(const String &p_raw) {
	const Ref<R> got = ver_of(p_raw);
	if (got->get_e().is_valid()) {
		return false;
	}
	const Dictionary v = got->get_v();
	if ((int64_t)v.get("major", -1) < 0 || (int64_t)v.get("minor", -1) < 0 || (int64_t)v.get("patch", -1) < 0 || text(v) != p_raw) {
		return false;
	}
	for (int group = 0; group < 2; group++) {
		const PackedStringArray ids = group == 0 ? PackedStringArray(v.get("pre", PackedStringArray())) : String(v.get("build", "")).split(".", false);
		for (const String &id : ids) {
			if (id.is_empty() || (group == 0 && id.length() > 1 && id[0] == '0' && id.is_valid_int())) {
				return false;
			}
			for (int i = 0; i < id.length(); i++) {
				const char32_t c = id[i];
				if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')) {
					return false;
				}
			}
		}
	}
	return true;
}

// Compare two versions.
int Semver::compare(const Dictionary &p_a, const Dictionary &p_b) {
	return ver_cmp(p_a, p_b);
}

// Check whether a version is a stable release.
bool Semver::is_stable(const Dictionary &p_v) {
	return PackedStringArray(p_v.get("pre", PackedStringArray())).is_empty();
}

// Return the stored version as a string.
String Semver::text(const Dictionary &p_v) {
	String out = vformat("%d.%d.%d", (int64_t)p_v.get("major", 0), (int64_t)p_v.get("minor", 0), (int64_t)p_v.get("patch", 0));
	const PackedStringArray pre = p_v.get("pre", PackedStringArray());
	if (!pre.is_empty()) {
		out += "-" + String(".").join(pre);
	}
	const String build = p_v.get("build", "");
	if (!build.is_empty()) {
		out += "+" + build;
	}
	return out;
}

// Select the best version satisfying a range.
Ref<R> Semver::best(const PackedStringArray &p_list, const String &p_range) {
	Dictionary top;
	String top_text;
	bool found = false;
	for (const String &raw : p_list) {
		const Ref<R> got = ver_of(raw);
		if (got->get_e().is_valid()) {
			continue;
		}
		const Dictionary v = got->get_v();
		if (!satisfies(v, p_range)) {
			continue;
		}
		const String txt = text(v);
		if (found) {
			const int order = ver_cmp(v, top);
			// Use the complete spelling as a tie-breaker for equal precedence.
			if (order < 0 || (order == 0 && (txt == top_text || txt < top_text))) {
				continue;
			}
		}
		top = v;
		top_text = txt;
		found = true;
	}
	if (!found) {
		return R::err(vformat("no version matches \"%s\"", p_range), Err::NOT_FOUND);
	}
	return R::ok(top);
}

// ---------------- Media types ----------------

namespace {

constexpr const char *MEDIA_FALLBACK = "application/octet-stream"; // Fallback type for unknown content.
constexpr const char *MEDIA_CHARSET = "; charset=utf-8"; // Charset suffix for textual media.

// Map common file extensions to media types.
struct MediaPair {
	const char *ext;
	const char *kind;
};

const MediaPair MEDIA_TABLE[] = {
	{ "txt", "text/plain" },
	{ "md", "text/markdown" },
	{ "html", "text/html" },
	{ "htm", "text/html" },
	{ "css", "text/css" },
	{ "csv", "text/csv" },
	{ "js", "text/javascript" },
	{ "mjs", "text/javascript" },
	{ "json", "application/json" },
	{ "jsonc", "application/json" },
	{ "map", "application/json" },
	{ "xml", "application/xml" },
	{ "yaml", "application/yaml" },
	{ "yml", "application/yaml" },
	{ "toml", "application/toml" },
	{ "wasm", "application/wasm" },
	{ "pdf", "application/pdf" },
	{ "zip", "application/zip" },
	{ "gz", "application/gzip" },
	{ "tar", "application/x-tar" },
	{ "svg", "image/svg+xml" },
	{ "png", "image/png" },
	{ "jpg", "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "gif", "image/gif" },
	{ "webp", "image/webp" },
	{ "avif", "image/avif" },
	{ "ico", "image/x-icon" },
	{ "woff", "font/woff" },
	{ "woff2", "font/woff2" },
	{ "ttf", "font/ttf" },
	{ "otf", "font/otf" },
	{ "mp3", "audio/mpeg" },
	{ "wav", "audio/wav" },
	{ "ogg", "audio/ogg" },
	{ "mp4", "video/mp4" },
	{ "webm", "video/webm" },
	{ "gd", "text/plain" },
	{ nullptr, nullptr },
};

// Identify textual media prefixes that receive a charset.
const char *MEDIA_TEXTUAL[] = {
	"text", "application/json", "application/xml", "application/yaml",
	"application/toml", "image/svg+xml", "text/javascript", nullptr
};

} // namespace

// Return the media type corresponding to a file extension.
String Media::by_extension(const String &p_ext) {
	const String key = p_ext.lstrip(".").to_lower();
	for (int i = 0; MEDIA_TABLE[i].ext; i++) {
		if (key == MEDIA_TABLE[i].ext) {
			return MEDIA_TABLE[i].kind;
		}
	}
	return MEDIA_FALLBACK;
}

// Check whether a media type is primarily textual.
bool Media::is_textual(const String &p_kind) {
	for (int i = 0; MEDIA_TEXTUAL[i]; i++) {
		if (p_kind.begins_with(MEDIA_TEXTUAL[i])) {
			return true;
		}
	}
	return false;
}

// Return the media type corresponding to a path.
String Media::by_path(const String &p_path) {
	const String kind = by_extension(p_path.get_extension());
	if (is_textual(kind) && !kind.contains("charset")) {
		return kind + MEDIA_CHARSET;
	}
	return kind;
}

// Return the extension corresponding to a media type.
String Media::extension(const String &p_kind) {
	const String base = p_kind.split(";")[0].strip_edges().to_lower();
	for (int i = 0; MEDIA_TABLE[i].ext; i++) {
		if (base == MEDIA_TABLE[i].kind) {
			return MEDIA_TABLE[i].ext;
		}
	}
	return String();
}
