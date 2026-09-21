/**************************************************************************/
/*  jail_windows.cpp                                                      */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Capture native Windows error codes in the filesystem wrapper before they are lost.

#include "cli/sys/jail_windows.h"

#ifdef WINDOWS_ENABLED

#include "cli/sys/source_error.h"

#include <cerrno>
#include <cstdlib>
#include <windows.h>

namespace {

// Clear thread-local error state before the next API call.
void begin_error() {
	SourceError::clear();
	_set_doserrno(0);
	errno = 0;
	SetLastError(ERROR_SUCCESS);
}

// Preserve Win32 codes and map only errors with a known meaning.
Error finish_error(Error p_fallback) {
	const DWORD code = GetLastError();
	if (code == ERROR_SUCCESS) {
		return p_fallback;
	}
	SourceError::win32(code);
	const Error mapped = SourceError::win32_error(code);
	return mapped == FAILED ? p_fallback : mapped;
}

// Prefer the original Win32 error retained by the CRT.
Error finish_file_error(Error p_fallback) {
	unsigned long code = 0;
	_get_doserrno(&code);
	if (code != 0) {
		SourceError::win32(code);
		const Error mapped = SourceError::win32_error(code);
		return mapped == FAILED ? p_fallback : mapped;
	}
	return finish_error(p_fallback);
}

} // namespace

// Capture the file-open Win32 error before another operation overwrites it.
Error FileJailWindows::open_internal(const String &p_path, int p_mode_flags) {
	begin_error();
	const Error err = FileJail<FileAccessWindows>::open_internal(p_path, p_mode_flags);
	return err == OK ? OK : finish_file_error(err);
}

// Preserve the error from starting directory enumeration.
Error DirJailWindows::list_dir_begin() {
	if (!Perm::check(Perm::READ, get_current_dir())) {
		SourceError::clear();
		return ERR_UNAUTHORIZED;
	}
	const Perm::Trusted trust;
	begin_error();
	const Error err = DirAccessWindows::list_dir_begin();
	return err == OK ? OK : finish_error(err);
}

// Preserve enumeration failure codes other than normal EOF.
String DirJailWindows::get_next() {
	begin_error();
	const String name = DirAccessWindows::get_next();
	const DWORD code = GetLastError();
	if (code != ERROR_SUCCESS && code != ERROR_NO_MORE_FILES) {
		SourceError::win32(code);
	}
	return name;
}

// Preserve the reason directory navigation failed.
Error DirJailWindows::change_dir(String p_dir) {
	begin_error();
	const Error err = DirJail<DirAccessWindows>::change_dir(p_dir);
	return err == OK ? OK : finish_error(err);
}

// Preserve the reason directory creation failed.
Error DirJailWindows::make_dir(String p_dir) {
	begin_error();
	const Error err = DirJail<DirAccessWindows>::make_dir(p_dir);
	return err == OK ? OK : finish_error(err);
}

// Perform native replacement rename without deleting the destination first.
Error DirJailWindows::rename(String p_from, String p_to) {
	SourceError::clear();
	const String from = this->_at(p_from);
	const String to = this->_at(p_to);
	GD_PERM_FAIL_V(READ, from, ERR_UNAUTHORIZED);
	GD_PERM_FAIL_V(WRITE, from, ERR_UNAUTHORIZED);
	GD_PERM_FAIL_V(WRITE, to, ERR_UNAUTHORIZED);
	const Perm::Trusted trust;
	begin_error();
	const String src = fix_path(p_from);
	const String dst = fix_path(p_to);
	if (MoveFileExW((LPCWSTR)src.utf16().get_data(), (LPCWSTR)dst.utf16().get_data(), MOVEFILE_REPLACE_EXISTING)) {
		return OK;
	}
	return finish_error(FAILED);
}

// Remove files and directories using the appropriate Win32 API.
Error DirJailWindows::remove(String p_path) {
	SourceError::clear();
	GD_PERM_FAIL_V(WRITE, this->_at(p_path), ERR_UNAUTHORIZED);
	const Perm::Trusted trust;
	begin_error();
	const String path = fix_path(p_path);
	const Char16String wide = path.utf16();
	const DWORD attr = GetFileAttributesW((LPCWSTR)wide.get_data());
	if (attr == INVALID_FILE_ATTRIBUTES) {
		return finish_error(FAILED);
	}
	const BOOL ok = (attr & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW((LPCWSTR)wide.get_data()) : DeleteFileW((LPCWSTR)wide.get_data());
	return ok ? OK : finish_error(FAILED);
}

#endif
