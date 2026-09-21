/**************************************************************************/
/*  os.h                                                                  */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Provide environment, child-process, and filesystem operations.
// Include filesystem-tree traversal helpers.
//
// GDDir and GDFile enforce permissions at their entry points.

#pragma once

#include "cli/sys/std.h"

// Internal resource retaining a file lock for its object lifetime.
class GDFileLock : public RefCounted {
	GDCLASS(GDFileLock, RefCounted);
	void *state = nullptr; // Platform-specific lock implementation.

protected:
	static void _bind_methods() {}

public:
	Ref<R> take(const String &p_path);
	~GDFileLock();
};

class Os {
public:
	// --- Files ---
	static Ref<R> read_text(const String &p_path);
	static Ref<R> read_bytes(const String &p_path, int64_t p_offset = 0, int64_t p_max = 0);
	static Ref<R> write_text(const String &p_path, const String &p_body);
	static Ref<R> replace_text(const String &p_path, const Variant &p_old, const String &p_body);
	static Ref<R> remove_text(const String &p_path, const String &p_old);
	static Ref<R> lock_file(const String &p_path);
	static Ref<R> write_bytes(const String &p_path, const PackedByteArray &p_body);
	static Ref<R> append_bytes(const String &p_path, const PackedByteArray &p_body);
	static Ref<R> append_text(const String &p_path, const String &p_body);
	static bool exists(const String &p_path);
	static Ref<R> remove(const String &p_path);
	static Ref<R> size_of(const String &p_path);
	static Ref<R> copy(const String &p_src, const String &p_dst);
	static Ref<R> rename(const String &p_src, const String &p_dst);

	// --- Directories ---
	static Ref<R> list_dir(const String &p_path);
	static Ref<R> make_dir(const String &p_path);
	static Ref<R> ensure_dir(const String &p_path); // Accept an existing directory.

	// --- Tree traversal ---
	static Ref<R> walk(const String &p_path, bool p_want_dirs, bool p_hidden);
	static Ref<R> glob(const String &p_path, const String &p_pattern, bool p_hidden);
	static Ref<R> remove_all(const String &p_path);

	// --- Permissions ---
	// Probe the operation because permission checks occur at the underlying entry point.
	static bool can_read(const String &p_path);
};
