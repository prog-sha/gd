/**************************************************************************/
/*  format.h                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Read and write human-oriented formats in native code.
// Expose file-format operations through GD.file while hiding implementation names.
//
// Keep character-by-character parsing native to avoid per-character script overhead.
// Fallible operations return a value and typed error through R.

#pragma once

#include "cli/sys/std.h"

// Parse CSV while respecting separators and newlines inside quoted fields.
class Csv {
public:
	static Ref<R> parse(const String &p_src, const String &p_sep);
	// Use the first row as dictionary keys for subsequent rows.
	static Ref<R> parse_objects(const String &p_src, const String &p_sep);
	static String stringify_objects(const Array &p_items, const String &p_sep);
	static String stringify(const Array &p_rows, const String &p_sep);
};

// Read and write INI sections and keys.
class Ini {
public:
	static Ref<R> parse(const String &p_src);
	static String stringify(const Dictionary &p_data);
};

// Support TOML tables, arrays of tables, and inline tables.
class Toml {
public:
	static Ref<R> parse(const String &p_src);
	static Ref<R> stringify(const Dictionary &p_data, const String &p_prefix);
};

// Support indented YAML mappings and sequences, excluding anchors and aliases.
class Yaml {
public:
	static Ref<R> parse(const String &p_src);
	static Ref<R> stringify(const Variant &p_data, int p_depth);
};

// Strip comments and trailing commas from extended JSON.
class Jsonc {
public:
	static String strip(const String &p_src);
	// Remove comments before decoding.
	static Ref<R> parse(const String &p_src);
};

// Separate document front matter from its body.
// Recognize YAML with ---, TOML with +++, and JSON with braces.
class Front {
public:
	// Return attrs, body, and kind.
	static Ref<R> parse(const String &p_src);
	static bool has(const String &p_src);
	static Ref<R> stringify(const Dictionary &p_attrs, const String &p_body, const String &p_kind);
};

// Read .env assignments.
class Dotenv {
public:
	static Ref<R> parse(const String &p_src);
	static Ref<R> stringify(const Dictionary &p_box);
};

// Represent XML trees as dictionaries, with @name attributes and #text content.
class Xml {
public:
	static Ref<R> parse(const String &p_src);
	static Ref<R> stringify(const Dictionary &p_data, int p_indent);
};

// Yield one JSON value per completed line.
// Accept incremental fragments without retaining every result in an array.
class GDJSONLReader : public RefCounted {
	GDCLASS(GDJSONLReader, RefCounted);

	String buf; // Input whose line is not complete yet.
	int line_no = 0;

protected:
	static void _bind_methods();

public:
	Ref<R> feed(const String &p_chunk);
	Ref<R> finish(); // Call when the input source ends.
};

// Read and write newline-delimited JSON.
class Jsonl {
public:
	static Ref<R> parse(const String &p_src);
	static Ref<R> stringify(const Array &p_items);
};
