/**************************************************************************/
/*  jail.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Restrict script access with native-file permission gates and resource-loader filtering.
//
// Place the gate immediately before native filesystem access so all script paths pass through it.
//
// Enforce permissions in file and directory wrappers rather than every public API.
// This covers added entry points without requiring a separate check for each one.
//
// Archive, compression, encryption, and PCK access delegate through FileAccess::open.
// Replacing the underlying wrappers applies the same gate to their contents.
//
// Implementation is in jail.cpp.

#pragma once

#include "cli/sys/perm.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"

// Check permission before delegating to the base implementation.
// The base resolves paths and calls virtual methods again, so mark delegation trusted
// to avoid applying the same boundary check twice.
//
// Use matching argument order for GD_JAIL, GD_JAIL_C, and GD_JAIL_S.
// Kind selects READ or WRITE; failure supplies the rejected return; type is the return type.
// Name, arguments, path, and forwarding arguments describe the wrapped operation.
#define GD_JAIL_BODY(m_kind, m_fail, m_name, m_path, m_args) \
	GD_PERM_FAIL_V(m_kind, m_path, m_fail); \
	const Perm::Trusted trust; \
	return T::m_name m_args;

// Wrap one operation that accepts a path.
#define GD_JAIL(m_kind, m_fail, m_ret, m_name, m_params, m_path, m_args) \
	virtual m_ret m_name m_params override { \
		GD_JAIL_BODY(m_kind, m_fail, m_name, m_path, m_args) \
	}

// Use the same argument order as GD_JAIL for const operations.
// Match the base interface's const qualification.
#define GD_JAIL_C(m_kind, m_fail, m_ret, m_name, m_params, m_path, m_args) \
	virtual m_ret m_name m_params const override { \
		GD_JAIL_BODY(m_kind, m_fail, m_name, m_path, m_args) \
	}

// Protect pathless platform-information operations with system permission.
// With kind and path fixed, supply only failure, type, name, arguments, and forwarding arguments.
#define GD_JAIL_S(m_fail, m_ret, m_name, m_params, m_args) \
	virtual m_ret m_name m_params override { \
		GD_JAIL_BODY(SYS, m_fail, m_name, "drives", m_args) \
	}

// Apply filesystem isolation to the file-access wrapper.
template <typename T>
class FileJail : public T {
public:
	GD_JAIL(READ, false, bool, file_exists, (const String &p_path), p_path, (p_path))

	// Protect metadata too: existence, size, and timestamps reveal information without returning content.
	GD_JAIL(READ, 0, BitField<FileAccess::UnixPermissionFlags>, _get_unix_permissions, (const String &p_file), p_file, (p_file))
	GD_JAIL(WRITE, ERR_UNAUTHORIZED, Error, _set_unix_permissions, (const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions), p_file, (p_file, p_permissions))
	GD_JAIL(READ, false, bool, _get_hidden_attribute, (const String &p_file), p_file, (p_file))
	GD_JAIL(WRITE, ERR_UNAUTHORIZED, Error, _set_hidden_attribute, (const String &p_file, bool p_hidden), p_file, (p_file, p_hidden))
	GD_JAIL(READ, false, bool, _get_read_only_attribute, (const String &p_file), p_file, (p_file))
	GD_JAIL(WRITE, ERR_UNAUTHORIZED, Error, _set_read_only_attribute, (const String &p_file, bool p_ro), p_file, (p_file, p_ro))

	GD_JAIL(READ, PackedByteArray(), PackedByteArray, _get_extended_attribute, (const String &p_file, const String &p_name), p_file, (p_file, p_name))
	GD_JAIL(WRITE, ERR_UNAUTHORIZED, Error, _set_extended_attribute, (const String &p_file, const String &p_name, const PackedByteArray &p_data), p_file, (p_file, p_name, p_data))
	GD_JAIL(WRITE, ERR_UNAUTHORIZED, Error, _remove_extended_attribute, (const String &p_file, const String &p_name), p_file, (p_file, p_name))
	GD_JAIL(READ, PackedStringArray(), PackedStringArray, _get_extended_attributes_list, (const String &p_file), p_file, (p_file))

protected:
	// Require read permission for read-only opens and write permission for all other modes.
	// Read-write mode must not open inside a read-only mount.
	virtual Error open_internal(const String &p_path, int p_mode_flags) override {
		const Perm::Kind kind = (p_mode_flags == FileAccess::READ) ? Perm::READ : Perm::WRITE;
		if (!Perm::check(kind, p_path)) {
			return ERR_UNAUTHORIZED;
		}
		const Perm::Trusted trust;
		return T::open_internal(p_path, p_mode_flags);
	}

	GD_JAIL(READ, 0, uint64_t, _get_modified_time, (const String &p_file), p_file, (p_file))
	GD_JAIL(READ, 0, uint64_t, _get_access_time, (const String &p_file), p_file, (p_file))
	GD_JAIL(READ, -1, int64_t, _get_size, (const String &p_file), p_file, (p_file))
};

// Isolate directory access, resolving relative names from the current directory first.
template <typename T>
class DirJail : public T {
public:
	GD_JAIL(READ, false, bool, file_exists, (String p_file), this->_at(p_file), (p_file))
	GD_JAIL(READ, false, bool, dir_exists, (String p_dir), this->_at(p_dir), (p_dir))
	GD_JAIL(READ, false, bool, is_readable, (String p_dir), this->_at(p_dir), (p_dir))
	GD_JAIL(READ, false, bool, is_writable, (String p_dir), this->_at(p_dir), (p_dir))

	GD_JAIL(WRITE, ERR_UNAUTHORIZED, Error, make_dir, (String p_dir), this->_at(p_dir), (p_dir))
	GD_JAIL(READ, ERR_UNAUTHORIZED, Error, change_dir, (String p_dir), this->_at(p_dir), (p_dir))
	GD_JAIL(WRITE, ERR_UNAUTHORIZED, Error, remove, (String p_name), this->_at(p_name), (p_name))

	GD_JAIL(READ, false, bool, is_link, (String p_file), this->_at(p_file), (p_file))
	GD_JAIL(READ, String(), String, read_link, (String p_file), this->_at(p_file), (p_file))
	GD_JAIL_C(READ, false, bool, is_case_sensitive, (const String &p_path), this->_at(p_path), (p_path))
	GD_JAIL_C(READ, false, bool, is_bundle, (const String &p_file), this->_at(p_file), (p_file))

	// Do not expose the host's drive inventory to scripts.
	GD_JAIL_S(0, int, get_drive_count, (), ())
	GD_JAIL_S(String(), String, get_drive, (int p_drive), (p_drive))
	GD_JAIL_S(String(), String, get_drive_label, (int p_drive), (p_drive))
	GD_JAIL_S(0, int, get_current_drive, (), ())

	// Check two-path operations individually because source and destination permissions differ.
	virtual Error rename(String p_from, String p_to) override {
		GD_PERM_FAIL_V(READ, this->_at(p_from), ERR_UNAUTHORIZED);
		GD_PERM_FAIL_V(WRITE, this->_at(p_from), ERR_UNAUTHORIZED);
		GD_PERM_FAIL_V(WRITE, this->_at(p_to), ERR_UNAUTHORIZED);
		const Perm::Trusted trust;
		return T::rename(p_from, p_to);
	}

	// Check both link location and target when creating symlinks.
	// Otherwise a link created inside the mount could lead later access outside it.
	//
	// Resolve relative link targets from the link's parent, matching OS semantics.
	virtual Error create_link(String p_source, String p_target) override {
		const String at = this->_at(p_target);
		GD_PERM_FAIL_V(WRITE, at, ERR_UNAUTHORIZED);
		const String to = p_source.is_relative_path() ? at.get_base_dir().path_join(p_source) : p_source;
		GD_PERM_FAIL_V(READ, to, ERR_UNAUTHORIZED);
		GD_PERM_FAIL_V(WRITE, to, ERR_UNAUTHORIZED);
		const Perm::Trusted trust;
		return T::create_link(p_source, p_target);
	}

	virtual bool is_equivalent(const String &p_a, const String &p_b) const override {
		GD_PERM_FAIL_V(READ, this->_at(p_a), false);
		GD_PERM_FAIL_V(READ, this->_at(p_b), false);
		const Perm::Trusted trust;
		return T::is_equivalent(p_a, p_b);
	}
};

// Install jailed wrappers once before loading scripts.
// Earlier startup loads are not reachable from script execution.
void install_jail();

// Retain only .gd and .gdextension resource loaders.
// Unsupported formats then fail for lack of a loader, reducing exposed parsing surface.
// Filter after normal loader registration without modifying the registration process.
void trim_loaders();
