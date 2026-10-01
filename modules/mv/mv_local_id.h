/**************************************************************************/
/*  mv_local_id.h                                                        */
/**************************************************************************/

// Stores the Client identifier and Secret key race-free and readable only by the current user.

#pragma once

#include "core/string/ustring.h"

class MVLocalID {
	static String read(const String &p_path, int p_bytes); // Read a saved value after validating it
	static void release(const String &p_lock, const String &p_pending); // Clean up the temporary save

public:
	static bool protect(const String &p_path, bool p_directory = false); // Restrict Unix permissions to the current user
	static String random_hex(int p_bytes); // Cryptographic random bytes as hex. Empty if unavailable
	static bool valid(const String &p_value, int p_bytes); // Check for fixed-length lowercase hex
	static String read_or_make(const String &p_path, int p_bytes); // Generate once and share across all processes
	static void forget(const String &p_path); // Delete the saved value. The next read_or_make makes a new one
};
