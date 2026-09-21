/**************************************************************************/
/*  jail_unix.h                                                           */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Pin Unix filesystem operations to descriptors beneath mount roots.

#pragma once

#ifdef UNIX_ENABLED

#include "cli/sys/jail.h"

#include "drivers/unix/dir_access_unix.h"
#include "drivers/unix/file_access_unix.h"

#include <dirent.h>

class FileJailUnix : public FileJail<FileAccessUnix> {
	int swap_fd = -1; // Parent descriptor for safe replacement.
	String swap_from; // Temporary name relative to the parent.
	String swap_to; // Final name relative to the parent.
	String swap_full; // Final destination displayed on replacement failure.

	void _finish();

protected:
	virtual Error open_internal(const String &p_path, int p_mode_flags) override;
	virtual uint64_t _get_modified_time(const String &p_file) override;
	virtual uint64_t _get_access_time(const String &p_file) override;
	virtual int64_t _get_size(const String &p_file) override;

public:
	virtual void close() override;
	virtual bool file_exists(const String &p_path) override;
	virtual BitField<FileAccess::UnixPermissionFlags> _get_unix_permissions(const String &p_file) override;
	virtual Error _set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions) override;
	virtual bool _get_hidden_attribute(const String &p_file) override;
	virtual Error _set_hidden_attribute(const String &p_file, bool p_hidden) override;
	virtual bool _get_read_only_attribute(const String &p_file) override;
	virtual Error _set_read_only_attribute(const String &p_file, bool p_ro) override;
	virtual PackedByteArray _get_extended_attribute(const String &p_file, const String &p_name) override;
	virtual Error _set_extended_attribute(const String &p_file, const String &p_name, const PackedByteArray &p_data) override;
	virtual Error _remove_extended_attribute(const String &p_file, const String &p_name) override;
	virtual PackedStringArray _get_extended_attributes_list(const String &p_file) override;

	~FileJailUnix();
};

// Perform directory navigation and mutation from pinned descriptors.
class DirJailUnix : public DirJail<DirAccessUnix> {
	DIR *stream = nullptr; // Directory enumeration stream.
	int current_fd = -1; // Current directory descriptor.
	bool next_dir = false; // Whether the most recent entry is a directory.
	bool next_hidden = false; // Whether the most recent entry is hidden.

public:
	DirJailUnix();
	virtual Error list_dir_begin() override;
	virtual String get_next() override;
	virtual bool current_is_dir() const override;
	virtual bool current_is_hidden() const override;
	virtual void list_dir_end() override;

	virtual Error change_dir(String p_dir) override;
	virtual Error make_dir(String p_dir) override;
	virtual bool file_exists(String p_file) override;
	virtual bool dir_exists(String p_dir) override;
	virtual bool is_readable(String p_dir) override;
	virtual bool is_writable(String p_dir) override;
	virtual Error rename(String p_from, String p_to) override;
	virtual Error remove(String p_path) override;
	virtual bool is_link(String p_file) override;
	virtual String read_link(String p_file) override;
	virtual Error create_link(String p_source, String p_target) override;
	virtual bool is_case_sensitive(const String &p_path) const override;
	virtual bool is_equivalent(const String &p_a, const String &p_b) const override;
	virtual String get_identity(const String &p_path) const override;
	virtual uint64_t get_space_left() override;
	virtual String get_filesystem_type() const override;

	~DirJailUnix();
};

#endif
