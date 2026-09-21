/**************************************************************************/
/*  fmt.h                                                                 */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Format .gd files with one fixed layout policy.
//
// Normalize line layout only.
// Use tabs for indentation, treating four spaces as one tab.
// Remove trailing whitespace.
// Collapse three or more consecutive blank lines to two.
// End each file with exactly one newline.
// Preserve triple-quoted string contents.
//
// Leave expression spacing such as `a+b` unchanged.
// Changing expression layout requires syntax-aware rewriting.

#pragma once

#include "cli/main/cmd.h"

#include "core/io/file_access.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/templates/list.h"

class GDFmt {
	// Recognize triple-quote delimiters and close only with the matching delimiter.
	static constexpr const char *TRIPLE_D = "\"\"\"";
	static constexpr const char *TRIPLE_S = "'''";

	// Convert indentation spaces to tabs, rounding partial groups upward.
	static String _indent(const String &p_line, int &r_body_at) {
		int spaces = 0;
		int tabs = 0;
		int at = 0;
		while (at < p_line.length()) {
			const char32_t c = p_line[at];
			if (c == '\t') {
				tabs++;
			} else if (c == ' ') {
				spaces++;
			} else {
				break;
			}
			at++;
		}
		r_body_at = at;
		return String("\t").repeat(tabs + (spaces + 3) / 4);
	}

public:
	// Format one source string.
	static String format(const String &p_src) {
		const Vector<String> lines = p_src.split("\n");
		List<String> out;
		String open_mark; // Opening delimiter while inside a triple-quoted string.
		int blank_run = 0;

		for (const String &raw : lines) {
			// Preserve triple-quoted contents and close only with the matching delimiter.
			// Accepting either delimiter would format literal contents containing the other kind.
			if (!open_mark.is_empty()) {
				// The line closing the string is code again after the delimiter, so its trailing blanks are removed.
				const int close_at = raw.find(open_mark);
				out.push_back(close_at < 0 ? raw : raw.rstrip(" \t"));
				if (close_at >= 0) {
					open_mark = String();
				}
				continue;
			}

			int body_at = 0;
			const String indent = _indent(raw, body_at);
			const String whole = raw.substr(body_at);

			// Find a triple-quoted string left open by this line.
			String mark;
			for (int i = 0; i + 2 < whole.length(); i++) {
				const String m = whole.substr(i, 3);
				if (m != TRIPLE_D && m != TRIPLE_S) {
					continue;
				}
				if (mark.is_empty()) {
					mark = m;
				} else if (m == mark) {
					mark = String();
				}
				i += 2;
			}

			// Preserve trailing contents after an opening delimiter in an unfinished string.
			const String body = mark.is_empty() ? whole.rstrip(" \t") : whole;

			if (body.is_empty()) {
				blank_run++;
				if (blank_run <= 2) {
					out.push_back(String());
				}
				continue;
			}
			blank_run = 0;
			out.push_back(indent + body);
			open_mark = mark;
		}

		// Remove trailing blank lines and append exactly one newline.
		while (!out.is_empty() && out.back()->get().is_empty()) {
			out.pop_back();
		}
		String result;
		for (const String &line : out) {
			result += line + "\n";
		}
		return result;
	}

	// Format and write files, or report differences without writing in check mode.
	static int run(const String &p_path, bool p_check) {
		// Reuse Cmd's file collection and empty-result error handling.
		List<String> files;
		if (!Cmd::collect_or_fail(p_path, ".gd", files)) {
			return EXIT_FAILURE;
		}

		int changed = 0;
		for (const String &file : files) {
			Ref<FileAccess> f = FileAccess::open(file, FileAccess::READ);
			if (f.is_null()) {
				ERR_PRINT(vformat("Cannot read %s.", file));
				continue;
			}
			const String src = f->get_as_text();
			f->close();

			const String out = format(src);
			if (out == src) {
				continue;
			}
			changed++;
			print_line(file);
			if (p_check) {
				continue;
			}
			Ref<FileAccess> w = FileAccess::open(file, FileAccess::WRITE);
			if (w.is_null()) {
				ERR_PRINT(vformat("Cannot write %s.", file));
				continue;
			}
			w->store_string(out);
			w->close();
		}
		return (p_check && changed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
	}
};
