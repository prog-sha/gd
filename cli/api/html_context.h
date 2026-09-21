/**************************************************************************/
/*  html_context.h                                                        */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Track HTML template position and context-specific value conversion.

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

class Utf8Out;

namespace HtmlContext {

enum State : uint8_t {
	TEXT,
	TAG,
	ATTR_NAME,
	AFTER_NAME,
	BEFORE_VALUE,
	COMMENT,
	RCDATA,
	ATTR_VALUE,
	URL,
	SRCSET,
	META_CONTENT,
	META_CONTENT_URL,
	JS,
	JS_DQ,
	JS_SQ,
	JS_TEMPLATE,
	JS_REGEXP,
	JS_BLOCK_COMMENT,
	JS_LINE_COMMENT,
	CSS,
	CSS_DQ,
	CSS_SQ,
	CSS_URL,
	CSS_DQ_URL,
	CSS_SQ_URL,
	CSS_BLOCK_COMMENT,
	CSS_LINE_COMMENT,
	ERROR,
};

enum Delim : uint8_t {
	DELIM_NONE,
	DELIM_DOUBLE,
	DELIM_SINGLE,
	DELIM_SPACE,
};

enum UrlPart : uint8_t {
	URL_NONE,
	URL_PATH,
	URL_QUERY,
	URL_UNKNOWN,
};

enum JsPart : uint8_t {
	JS_REGEXP_PART,
	JS_DIV_PART,
	JS_UNKNOWN_PART,
};

enum Attr : uint8_t {
	ATTR_NONE,
	ATTR_SCRIPT,
	ATTR_SCRIPT_TYPE,
	ATTR_STYLE,
	ATTR_URL,
	ATTR_SRCSET,
	ATTR_META_CONTENT,
};

enum Element : uint8_t {
	ELEMENT_NONE,
	ELEMENT_SCRIPT,
	ELEMENT_STYLE,
	ELEMENT_TEXTAREA,
	ELEMENT_TITLE,
	ELEMENT_META,
};

enum Escape : uint8_t {
	ESC_HTML,
	ESC_RCDATA,
	ESC_ATTR,
	ESC_NAME,
	ESC_URL,
	ESC_URL_PATH,
	ESC_URL_QUERY,
	ESC_SRCSET,
	ESC_META_URL,
	ESC_JS_VALUE,
	ESC_JS_STRING,
	ESC_JS_TEMPLATE,
	ESC_JS_REGEXP,
	ESC_CSS_VALUE,
	ESC_CSS_STRING,
	ESC_CSS_STRING_URL,
	ESC_COMMENT,
};

// Conversion selected immediately before a value and its attribute quoting.
struct Action {
	Escape escape = ESC_HTML;
	Delim delim = DELIM_NONE;
};

// Current position within partially scanned markup, scripts, or styles.
struct Context {
	State state = TEXT;
	Delim delim = DELIM_NONE;
	UrlPart url_part = URL_NONE;
	JsPart js_part = JS_REGEXP_PART;
	Attr attr = ATTR_NONE;
	Element element = ELEMENT_NONE;
	Vector<int> js_braces;
	String attr_text;
	bool attr_dynamic = false;
	bool comment_line = false;
};

bool same(const Context &p_a, const Context &p_b);
Error advance(Context &r_ctx, String &r_text, String &r_bad);
Error action(Context &r_ctx, Action &r_action, String &r_bad);
Error join(const Context &p_a, const Context &p_b, Context &r_out, String &r_bad);
Error finish(const Context &p_ctx, String &r_bad);
Error render(const Action &p_action, const Variant &p_value, int64_t p_max, String &r_out);
Error render_bytes(const Action &p_action, const Variant &p_value, Utf8Out &r_out);

} // namespace HtmlContext
