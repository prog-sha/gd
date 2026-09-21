/**************************************************************************/
/*  source_error.cpp                                                      */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Deliver the preceding OS operation's numeric error once to the same-thread caller.

#include "cli/sys/source_error.h"

#include "cli/sys/std.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#else
#include <cerrno>
#endif

thread_local SourceError::Value SourceError::value;

// Clear stale failure state before another operation.
void SourceError::clear() {
	value = Value();
}

// Preserve POSIX errno.
void SourceError::posix(int p_code) {
	value.source = POSIX;
	value.code = p_code;
}

// Preserve Windows GetLastError.
void SourceError::win32(uint32_t p_code) {
	value.source = WINDOWS;
	value.code = p_code;
}

// Record the platform-specific error for traversing a file as a directory.
void SourceError::not_dir() {
#ifdef WINDOWS_ENABLED
	win32(ERROR_PATH_NOT_FOUND);
#else
	posix(ENOTDIR);
#endif
}

// Report retained error state without consuming it.
bool SourceError::kept() {
	return value.source != NONE;
}

// Return and clear the original cause.
SourceError::Value SourceError::take() {
	const Value out = value;
	clear();
	return out;
}

// Map errno to the shared file-API categories.
Error SourceError::posix_error(int p_code) {
#ifndef WINDOWS_ENABLED
	switch (p_code) {
		case EACCES:
		case EPERM:
			return ERR_FILE_NO_PERMISSION;
		case ENOENT:
			return ERR_FILE_NOT_FOUND;
		case EEXIST:
		case ENOTEMPTY:
			return ERR_ALREADY_EXISTS;
		case EMFILE:
		case ENFILE:
			return ERR_OUT_OF_MEMORY;
		default:
			return FAILED;
	}
#else
	(void)p_code;
	return FAILED;
#endif
}

// Map Win32 codes to the shared file-API categories.
Error SourceError::win32_error(uint32_t p_code) {
#ifdef WINDOWS_ENABLED
	switch (p_code) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
		case ERROR_BAD_NETPATH:
			return ERR_FILE_NOT_FOUND;
		case ERROR_ACCESS_DENIED:
			return ERR_FILE_NO_PERMISSION;
		case ERROR_FILE_EXISTS:
		case ERROR_ALREADY_EXISTS:
		case ERROR_DIR_NOT_EMPTY:
			return ERR_ALREADY_EXISTS;
		case ERROR_TOO_MANY_OPEN_FILES:
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY:
			return ERR_OUT_OF_MEMORY;
		case ERROR_FILENAME_EXCED_RANGE:
		case ERROR_BAD_PATHNAME:
			return ERR_FILE_BAD_PATH;
		default:
			return FAILED;
	}
#else
	(void)p_code;
	return FAILED;
#endif
}

// Expose one original cause and return its corresponding Err category.
Error SourceError::put(Dictionary &r_info, Error p_fallback) {
	const Value source = take();
	if (source.source == POSIX) {
		r_info["source"] = "posix";
		r_info["source_code"] = source.code;
		const Error mapped = posix_error(source.code);
		return mapped == FAILED ? p_fallback : mapped;
	} else if (source.source == WINDOWS) {
		r_info["source"] = "win32";
		r_info["source_code"] = source.code;
		const Error mapped = win32_error(source.code);
		return mapped == FAILED ? p_fallback : mapped;
	} else {
		r_info["source"] = "engine";
		r_info["source_code"] = (int)p_fallback;
		return p_fallback;
	}
}

// Normalize path-operation failures to one result shape.
Ref<R> SourceError::path(const String &p_path, const char *p_op, Error p_fallback, const String &p_msg) {
	Dictionary info;
	info["op"] = p_op;
	info["path"] = p_path;
	const Error err = put(info, p_fallback);
	return R::err(Err::make(p_msg, Err::of(err), info));
}
