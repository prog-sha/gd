/**************************************************************************/
/*  mv_local_id.cpp                                                      */
/**************************************************************************/

// Race-free, owner-only storage of local identifiers and random hex values.

#include "mv_local_id.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

// Decide whether a saved value is lowercase hex of the given byte count
bool MVLocalID::valid(const String &p_value, int p_bytes) {
	if (p_bytes < 1 || p_bytes > 1024 || p_value.length() != p_bytes * 2) {
		return false;
	}
	for (int i = 0; i < p_value.length(); i++) {
		const char32_t c = p_value[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

// Turn cryptographic random bytes into hex. Used for routes, tokens, and room and player numbers
String MVLocalID::random_hex(int p_bytes) {
	Ref<Crypto> crypto = Crypto::create();
	if (crypto.is_null()) {
		return String();
	}
	const PackedByteArray bytes = crypto->generate_random_bytes(p_bytes);
	return String::hex_encode_buffer(bytes.ptr(), bytes.size());
}

// Restrict saved files on POSIX systems to the current user only
bool MVLocalID::protect(const String &p_path, bool p_directory) {
	const String os = OS::get_singleton()->get_name();
	const bool posix = os == "Linux" || os == "FreeBSD" || os == "NetBSD" || os == "OpenBSD" ||
			os == "BSD" || os == "macOS" || os == "iOS";
	if (!posix) {
		return true;
	}
	const int mode = p_directory ? 0700 : 0600;
	return FileAccess::set_unix_permissions(p_path, mode) == OK;
}

// Read a saved identifier after checking both its format and permissions
String MVLocalID::read(const String &p_path, int p_bytes) {
	const String value = FileAccess::get_file_as_string(p_path).strip_edges();
	return valid(value, p_bytes) && protect(p_path) ? value : String();
}

// Clean up the temporary file and directory used for exclusive creation
void MVLocalID::release(const String &p_lock, const String &p_pending) {
	if (FileAccess::exists(p_pending)) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_pending));
	}
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_lock));
}

// Generate the same identifier only once even when several processes race
String MVLocalID::read_or_make(const String &p_path, int p_bytes) {
	if (FileAccess::exists(p_path)) {
		return read(p_path, p_bytes);
	}
	const String lock = p_path + ".lock";
	const String lock_abs = ProjectSettings::get_singleton()->globalize_path(lock);
	if (DirAccess::make_dir_absolute(lock_abs) != OK) {
		const uint64_t until = OS::get_singleton()->get_ticks_msec() + 1000;
		while (OS::get_singleton()->get_ticks_msec() < until) {
			if (FileAccess::exists(p_path)) {
				return read(p_path, p_bytes);
			}
			OS::get_singleton()->delay_usec(5000);
		}
		return String();
	}
	if (!protect(lock_abs, true)) {
		DirAccess::remove_absolute(lock_abs);
		return String();
	}
	const String value = random_hex(p_bytes);
	if (value.is_empty()) {
		DirAccess::remove_absolute(lock_abs);
		return String();
	}
	const String pending = lock + "/value";
	Ref<FileAccess> file = FileAccess::open(pending, FileAccess::WRITE);
	if (file.is_null() || !protect(pending)) {
		release(lock, pending);
		return String();
	}
	file->store_string(value);
	file->flush();
	const Error error = file->get_error();
	file->close();
	if (error == OK) {
		DirAccess::rename_absolute(ProjectSettings::get_singleton()->globalize_path(pending),
				ProjectSettings::get_singleton()->globalize_path(p_path));
	}
	release(lock, pending);
	return FileAccess::exists(p_path) ? read(p_path, p_bytes) : String();
}

void MVLocalID::forget(const String &p_path) {
	if (FileAccess::exists(p_path)) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
	}
}
