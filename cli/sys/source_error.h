/**************************************************************************/
/*  source_error.h                                                        */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Preserve per-operation OS or database codes before reducing them to Error categories.

#pragma once

#include "core/error/error_list.h"
#include "core/variant/variant.h"

class R;

class SourceError {
public:
	enum Source {
		NONE,
		POSIX,
		WINDOWS,
	};

	struct Value {
		Source source = NONE; // Subsystem defining the numeric code.
		int64_t code = 0; // Original errno or GetLastError value.
	};

private:
	static thread_local Value value;

public:
	static void clear();
	static void posix(int p_code);
	static void win32(uint32_t p_code);
	static void not_dir();
	static bool kept();
	static Value take();
	// Map POSIX causes only when their meaning is known.
	static Error posix_error(int p_code);
	// Map Win32 causes only when their meaning is known.
	static Error win32_error(uint32_t p_code);
	// Expose original subsystem and code in info and return a machine-readable Error.
	static Error put(Dictionary &r_info, Error p_fallback);
	// Create a file failure with operation, path, and original cause.
	static Ref<R> path(const String &p_path, const char *p_op, Error p_fallback, const String &p_msg);
};
