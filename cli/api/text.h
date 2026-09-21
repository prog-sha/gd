/**************************************************************************/
/*  text.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Construct, split, and validate text, paths, and names with pure transformations and HTML parsing state.
// Keep character-wise processing native to avoid repeated script-side allocation.
// Expose role-specific child APIs while keeping implementation names private.

#pragma once

#include "cli/sys/std.h"

#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

class GDHTMLTemplate;

// Pause parsing for missing file partials and resume in the same HTML context.
class HtmlBuild {
	void *state = nullptr; // Implementation state owning the AST and explicit stack.

public:
	HtmlBuild();
	~HtmlBuild();
	HtmlBuild(const HtmlBuild &) = delete;
	HtmlBuild &operator=(const HtmlBuild &) = delete;
	Error step(const String &p_part = String()); // On ERR_BUSY, load the required partial and resume.
	String needed() const; // Return the currently required partial name.
	Error render(const Dictionary &p_data, String &r_out); // Render using only the prepared tree.
	Ref<R> failure(Error p_error) const; // Preserve the parser location and error category.
};

// Manipulate paths using slash separators.
class Path {
public:
	static String join(const PackedStringArray &p_parts);
	static String dirname(const String &p_path);
	static String basename(const String &p_path, const String &p_suffix);
	static String extname(const String &p_path);
	static bool is_absolute(const String &p_path);
	static String normalize(const String &p_path);
	static String relative(const String &p_from, const String &p_to);
	// Join within a root, returning empty for escaping paths.
	// Use this boundary when reading files named by external input.
	static String under(const String &p_dir, const String &p_name);
	// Return dir, base, ext, and name components.
	static Dictionary parse(const String &p_path);
	static String format(const Dictionary &p_parts); // Reconstruct a parsed path.
};

// HTML escaping operations.
class Html {
public:
	// Loader for only those partials required during web rendering.
	using PartLoader = Error (*)(void *p_ctx, const String &p_name, String &r_tpl);

	static String escape(const String &p_text);
	static String unescape(const String &p_text);
	static String attr(const String &p_value); // Wrap in double quotes.
	// Validate output tag or attribute names.
	static bool name_ok(const String &p_s);
	// Validate a relative name within the partial collection.
	static bool partial_ok(const String &p_s);
	static String tag(const String &p_name, const String &p_body, const Dictionary &p_attrs);
	// Interpolate Mustache-style templates with context-sensitive HTML, attribute, and URL escaping.
	// Support raw interpolation, if, unless, each, with, else, and supplied partials.
	static Ref<R> fill(const String &p_tpl, const Dictionary &p_data, const Dictionary &p_partials = Dictionary());
	// Parse templates and partials into an immutable, concurrently executable renderer.
	static Ref<R> template_of(const String &p_tpl, const Dictionary &p_partials = Dictionary());
	// Render internally while preserving parse and partial-loading errors for callers.
	static Error fill_checked(const String &p_tpl, const Dictionary &p_data, const Dictionary &p_partials, String &r_out, String &r_bad);
};

// Template with resolved HTML contexts, executable concurrently without mutation.
class GDHTMLTemplate : public RefCounted {
	GDCLASS(GDHTMLTemplate, RefCounted);

	void *program = nullptr; // Parsed program private to the implementation.

	friend class Html;

protected:
	static void _bind_methods();

public:
	~GDHTMLTemplate();
	Ref<R> execute(const Dictionary &p_data) const; // Return HTML with safely interpolated values.
	Ref<R> execute_bytes(const Dictionary &p_data) const; // Return escaped HTML directly as UTF-8 bytes.
};

// Text operations and formatting.
class Text {
public:
	static int distance(const String &p_a, const String &p_b);
	// Return the closest candidate, or empty when none is close enough.
	static String closest(const String &p_word, const PackedStringArray &p_options);
	static String ellipsis(const String &p_text, int p_width);
	static String size_of(int64_t p_bytes); // Format byte sizes using powers of 1024.
	static String duration(double p_ms);

	// Distinguish interactive standard streams from pipes and files.
	static bool stdin_tty();
	static bool stdout_tty();
	static bool stderr_tty();
	// Apply an SGR code only when color is enabled for the destination.
	static String paint(const String &p_text, int p_code, bool p_enabled);
	static String snake(const String &p_text);
	static String camel(const String &p_text);
	static String title(const String &p_text);
	static String table(const Array &p_rows, int p_gap);
};

// URL parsing and query manipulation.
class Url {
public:
	// Return scheme, host, port, path, query, and fragment.
	// Include user and password when user information is present.
	static Ref<R> parse(const String &p_raw);
	// Split host and port; leave r_port empty when absent and return empty for malformed syntax.
	// Remove brackets from IPv6 hosts without validating the port's numeric form.
	static String host_port(const String &p_text, String &r_port);
	// Also extract user information and validate the port.
	// Use p_default_port when absent; return an empty dictionary for malformed input.
	static Dictionary split_host(const String &p_text, int64_t p_default_port);
	static String build(const Dictionary &p_url); // Rebuild the URL.
	static String request_target(const Dictionary &p_url); // Return only path and query.
	static int default_port(const String &p_scheme);
	// Decode percent notation while validating UTF-8 and rejecting C0, DEL, and C1 controls.
	// Do not normalize malformed input to U+FFFD, which could collapse distinct inputs.
	// Reject it so validated and consumed values remain identical.
	// With null r_out, validate without allocating output bytes.
	// Set p_plus_space only for queries that interpret plus as space.
	static bool decode_check(const uint8_t *p_data, int p_len, bool p_plus_space, CharString *r_out);
	// Decode a URL component, returning false when strict validation fails.
	static bool decode_part(const String &p_raw, bool p_plus_space, String &r_out);
	static Ref<R> decode_query(const String &p_raw);
	static String encode_query(const Dictionary &p_query); // Encode query fields in name order.
};

// Semantic-version comparison.
class Semver {
public:
	// Represent versions with major, minor, patch, pre, and build.
	static Ref<R> parse(const String &p_raw);
	static bool is_canonical(const String &p_raw); // Check complete canonical SemVer notation.
	static int compare(const Dictionary &p_a, const Dictionary &p_b);
	static bool satisfies(const Dictionary &p_v, const String &p_range);
	static bool is_stable(const Dictionary &p_v); // Report stable versions without prerelease identifiers.
	static String text(const Dictionary &p_v);
	// Select the latest version, optionally restricted by a range.
	static Ref<R> best(const PackedStringArray &p_list, const String &p_range);
};

// Map filename extensions to MIME types.
class Media {
public:
	static String by_extension(const String &p_ext);
	static String by_path(const String &p_path); // Include a charset when appropriate.
	static bool is_textual(const String &p_kind);
	static String extension(const String &p_kind); // Return a representative extension for the MIME type.
};
