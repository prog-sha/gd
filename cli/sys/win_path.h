// Convert absolute filesystem paths between logical and extended native spellings.
#pragma once

#include "core/string/ustring.h"

#include <windows.h>

namespace WinPath {

// Preserve share roots when decoding native paths for joining and normalization.
inline String normal(const String &p_path) {
	const String path = p_path.replace_char('\\', '/');
	if (path.begins_with("//?/UNC/")) {
		return "//" + path.substr(8);
	}
	if (path.begins_with("//?/") && path.length() >= 6 && path[5] == ':') {
		return path.substr(4);
	}
	return path;
}

// Return the normalized final path represented by an open filesystem handle.
inline String final(HANDLE p_handle) {
	const DWORD size = ::GetFinalPathNameByHandleW(p_handle, nullptr, 0, VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
	if (size == 0) {
		return String();
	}
	Char16String data;
	data.resize_uninitialized(size + 1);
	const DWORD got = ::GetFinalPathNameByHandleW(p_handle, (LPWSTR)data.ptrw(), size + 1, VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
	return got == 0 || got > size ? String() : normal(String::utf16((const char16_t *)data.ptr(), got));
}

// Normalize absolute paths before opting out of native path normalization.
inline String native(const String &p_path) {
	const String path = normal(p_path).simplify_path().replace_char('/', '\\');
	if (path.begins_with(R"(\\?\)")) {
		return path; // Keep volume GUIDs and other absolute native namespaces intact.
	}
	return path.begins_with(R"(\\)") ? R"(\\?\UNC\)" + path.substr(2) : R"(\\?\)" + path;
}

} // namespace WinPath
