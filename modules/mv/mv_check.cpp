/**************************************************************************/
/*  mv_check.cpp                                                          */
/**************************************************************************/

// Checks author GDScript by its text and accepts only safe scripts that can go to isolated execution.

#include "mv_check.h"

#include "mv_client_script.h"

#include "mv_classes.h"
#include "mv_secret.h"
#include "mv_online.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

// Whether the char can start a word
static _FORCE_INLINE_ bool is_head(char32_t c) {
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Whether the char can continue a word
static _FORCE_INLINE_ bool is_body(char32_t c) {
	return is_head(c) || (c >= '0' && c <= '9');
}

// Return the length of the word starting at this position. 0 if not a word
static int word_len(const char32_t *p_s, int p_i, int p_n) {
	if (p_i >= p_n || !is_head(p_s[p_i])) {
		return 0;
	}
	int e = p_i;
	while (e < p_n && is_body(p_s[e])) {
		e++;
	}
	return e - p_i;
}

// Return the position after the end of the string, or -1 if unclosed. Handles triple quotes and multi-line
static int skip_text(const char32_t *p_s, int p_i, int p_n) {
	const char32_t q = p_s[p_i];
	const bool triple = p_i + 2 < p_n && p_s[p_i + 1] == q && p_s[p_i + 2] == q;
	const int mark = triple ? 3 : 1;
	int i = p_i + mark;
	while (i < p_n) {
		// An escape skips the next char too, so a quote right after \\ is not taken as the end
		if (p_s[i] == '\\') {
			i += 2;
			continue;
		}
		if (p_s[i] == '\n' && !triple) {
			return -1;
		}
		if (p_s[i] == q && (mark == 1 || (i + 2 < p_n && p_s[i + 1] == q && p_s[i + 2] == q))) {
			return i + mark;
		}
		i++;
	}
	return -1;
}

// Build a rejection result
static Dictionary ng(const String &p_reason, int p_line) {
	Dictionary d;
	d["ok"] = false;
	d["reason"] = p_reason;
	d["line"] = p_line;
	d["cls"] = String();
	return d;
}

// Read the leading extends declaration. Returns the type and its line number.
// Works on positions only, without cutting out each line
static Dictionary head_of(const String &p_src) {
	const char32_t *s = p_src.ptr();
	const int n = p_src.length();
	int line = 0;
	int at = 0;
	while (at <= n) {
		line++;
		int e = at;
		while (e < n && s[e] != '\n') {
			e++;
		}
		// Trim whitespace at both ends of the line
		int a = at;
		int b = e;
		while (a < b && s[a] <= ' ') {
			a++;
		}
		while (b > a && s[b - 1] <= ' ') {
			b--;
		}
		at = e + 1;
		// Skip blank lines, comments and annotations placed before extends
		if (a == b || s[a] == '#' || s[a] == '@') {
			continue;
		}
		if (word_len(s, a, b) != 7 || p_src.substr(a, 7) != "extends") {
			return ng(U"extends must come first", line);
		}
		// Separator may be spaces or tabs; a trailing comment is allowed
		int c = a + 7;
		while (c < b && s[c] <= ' ') {
			c++;
		}
		const int cl = word_len(s, c, b);
		const String cls = p_src.substr(c, cl);
		int t = c + cl;
		while (t < b && s[t] <= ' ') {
			t++;
		}
		if (cl == 0 || (t < b && s[t] != '#')) {
			return ng(U"only the type name may follow extends", line);
		}
		if (!MVClasses::has(cls)) {
			return ng(String(U"cannot extend: ") + cls, line);
		}
		Dictionary d;
		d["cls"] = cls;
		d["line"] = line;
		return d;
	}
	return ng(U"empty script", 1);
}

// Decide whether the string names a runtime internal itself.
// All internal entry points are __-prefixed and live in known places,
// so reject only when the name exists there. Ordinary strings are not affected
static bool names_inside(const String &p_text) {
	if (!p_text.begins_with("__")) {
		return false;
	}
	if (Engine::get_singleton()->has_singleton(p_text)) {
		return true;
	}
	for (const String &owner : { String("MVOnline"), String("MVRuntime"),
				 String("MVClient"), String("MVLink"), String(Online::KIND), String(Secret::KIND) }) {
		if (ClassDB::class_exists(owner) && ClassDB::has_method(owner, p_text, true)) {
			return true;
		}
	}
	return false;
}

// Check each word after the extends declaration
static Dictionary scan(const String &p_src, int p_skip) {
	const char32_t *s = p_src.ptr();
	const int n = p_src.length();
	int line = 1;
	int i = 0;
	while (i < n) {
		const char32_t c = s[i];
		// Skip comments to the end of the line
		if (c == '#') {
			while (i < n && s[i] != '\n') {
				i++;
			}
			continue;
		}
		// String contents are not checked, except strings that name a runtime internal itself.
		// Otherwise call("__spawn") or get_singleton("__Online") would slip past the bare-word check.
		// Ordinary strings like print("__hello") pass
		if (c == '"' || c == '\'') {
			const int e = skip_text(s, i, n);
			if (e < 0) {
				return ng(U"unterminated string", line);
			}
			const String body = p_src.substr(i + 1, MAX(0, e - i - 2));
			if (line != p_skip && names_inside(body)) {
				return ng(vformat(U"%s is internal to the runtime", body), line);
			}
			for (int k = i; k < e; k++) {
				if (s[k] == '\n') {
					line++;
				}
			}
			i = e;
			continue;
		}
		// Reject only annotations that run code in the editor
		if (c == '@') {
			const int al = word_len(s, i + 1, n);
			if (al == 18 && p_src.substr(i + 1, al) == "export_tool_button") {
				return ng(vformat(U"@%s is not allowed", p_src.substr(i + 1, al)), line);
			}
			i += 1 + al;
			continue;
		}
		if (c == '\n') {
			line++;
			i++;
			continue;
		}
		// Pick up words and compare with forbidden ones
		const int wl = word_len(s, i, n);
		if (wl == 0) {
			i++;
			continue;
		}
		const int at = i;
		i += wl;
		if (line == p_skip) {
			continue;
		}
		if (wl >= 2 && s[at] == '_' && s[at + 1] == '_') {
			return ng(vformat(U"%s is internal to the runtime", p_src.substr(at, wl)), line);
		}
		// Name the Online side moves @online func bodies to. An author using it would collide with the moved body
		if (p_src.substr(at, wl).begins_with(MVClientScript::IMPLEMENTATION)) {
			return ng(vformat(U"%s is reserved for the runtime", p_src.substr(at, wl)), line);
		}
		// World scripts are reloaded under different names on both sides, so they cannot hold project-wide names
		if (wl == 10 && p_src.substr(at, wl) == "class_name") {
			return ng(U"class_name is not allowed", line);
		}
		// Internal type names are not author names either. They cannot be removed from ClassDB
		// because singletons need them for type resolution, but world scripts reject them
		if (wl >= 2 && s[at] == 'M' && s[at + 1] == 'V') {
			const String word = p_src.substr(at, wl);
			if (ClassDB::class_exists(word)) {
				return ng(vformat(U"%s is internal to the runtime", word), line);
			}
		}
	}
	Dictionary d;
	d["ok"] = true;
	d["reason"] = String();
	d["line"] = 0;
	return d;
}

// Decide whether it may be accepted. Returns the inherited type and the rejection reason
Dictionary MVCheck::check(const String &p_src) {
	Dictionary head = head_of(p_src);
	const String cls = head["cls"];
	if (cls.is_empty()) {
		return ng(head["reason"], head["line"]);
	}
	Dictionary r = scan(p_src, head["line"]);
	r["cls"] = bool(r["ok"]) ? cls : String();
	return r;
}

// Checks are called only from C++. Nothing is exposed to GDScript
void MVCheck::_bind_methods() {
}
