// Connect file and directory operations to the OS while preserving mounts and original errors.
#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

// Own regular-file descriptors and borrow embedded storage; caller workers serialize operations.
class GDFile : public RefCounted {
	intptr_t fd = -1; // Unix descriptor or Windows HANDLE; borrowed for embedded assets.
	String path; // Caller-supplied path.
	Error error = OK; // Result of the most recent operation.
	bool embedded = false; // Whether this is a read-only embedded range.
	uint64_t base = 0, length = 0, position = 0; // Embedded asset bounds and relative position.
	static thread_local Error open_error; // Open result local to the current thread.

public:
	enum Mode { READ = 1, WRITE = 2, READ_WRITE = 3, WRITE_READ = 7, APPEND = 10, CREATE = 11 }; // Access and creation modes; CREATE does not truncate.
	// Open a regular file within a mount.
	static Ref<GDFile> open(const String &p_path, Mode p_mode);
	static Error get_open_error() { return open_error; } // Return the most recent open failure.
	Error get_error() const { return error; } // Return the most recent I/O failure.
	String get_path() const { return path; } // Return the display path.
	// Read once from the current position, allowing partial results.
	uint64_t get_buffer(uint8_t *p_data, uint64_t p_size);
	PackedByteArray get_buffer(uint64_t p_size); // Read the requested range through EOF.
	// Write until failure, preserving completed bytes and the error reason.
	uint64_t write(const uint8_t *p_data, uint64_t p_size);
	// Write all bytes, continuing after short writes.
	bool store_buffer(const uint8_t *p_data, uint64_t p_size);
	bool store_buffer(const PackedByteArray &p_data) { return store_buffer(p_data.ptr(), p_data.size()); }
	bool store_string(const String &p_text); // Write as UTF-8.
	void seek(uint64_t p_at); // Set the position from the start.
	void seek_end(); // Set the position to the end.
	uint64_t get_position(); // Return the current position.
	uint64_t get_length(); // Return the opened file's length.
	Error resize(uint64_t p_size); // Change the opened file's length.
	bool same(const Ref<GDFile> &p_file); // Compare file identity, including hard links.
	Error close(); // Close once and return any close failure.
	~GDFile(); // Reclaim an unclosed descriptor.
	static bool exists(const String &p_path); // Check file existence within permissions.
};

// Enumerate and modify native directories within mounts.
class GDDir : public RefCounted {
	String path = "res://"; // Base for relative operations.
	void *stream = nullptr; // Unix DIR or Windows search state.
	intptr_t fd = -1; // Pinned directory descriptor on Unix.
	bool current_dir = false; // Whether the current entry is a directory.
	bool current_hidden = false; // Whether the current entry is hidden.
	bool embedded = false; // Whether this is an embedded directory.
	PackedStringArray names; // Immediate embedded-directory children.
	int next = 0; // Embedded enumeration position.
	static thread_local Error open_error; // Most recent open result on this thread.
	String full(const String &p_path) const; // Join a relative path to the base.

public:
	static Ref<GDDir> open(const String &p_path); // Open the actual directory.
	static Ref<GDDir> create(); // Create a base object for mutations.
	static Error get_open_error() { return open_error; } // Return the most recent open failure.
	Error list_dir_begin(); // Begin enumeration.
	String get_next(); // Return the next name, or empty at EOF.
	void list_dir_end(); // Release enumeration resources.
	bool current_is_dir() const { return current_dir; } // Return the current entry's type.
	bool current_is_hidden() const { return current_hidden; } // Return the current entry's hidden attribute.
	String get_current_dir() const { return path; } // Return the operation base.
	Error make_dir(const String &p_path); // Create one directory level.
	Error remove(const String &p_path); // Remove one file or empty directory.
	Error rename(const String &p_src, const String &p_dst); // Use native replacement rename.
	static bool dir_exists_absolute(const String &p_path); // Check directory existence.
	bool dir_exists(const String &p_path) const { return dir_exists_absolute(full(p_path)); }
	bool is_case_sensitive(const String &p_path) const; // Return the filesystem's name-comparison rule.
	String get_identity(const String &p_path) const; // Return the OS identity of a file.
	~GDDir(); // Release enumeration and base descriptors.
};
