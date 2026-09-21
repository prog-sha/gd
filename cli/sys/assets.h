// Locate read-only assets embedded in a standalone executable.
#pragma once
#include "core/variant/variant.h"

namespace GDAssets {
void init(); // Pin the executable and index before script execution.
struct Entry { uint64_t offset = 0; uint64_t size = 0; }; // Readable byte range within the binary.
bool find(const String &p_path, Entry &r_entry); // Find an embedded res:// file.
bool directory(const String &p_path, PackedStringArray &r_names); // Enumerate immediate children of an embedded directory.
intptr_t handle(); // Borrow the handle corresponding to the index.
uint64_t read(uint8_t *p_data, uint64_t p_size, uint64_t p_at, Error &r_error); // Read at an explicit offset from the retained executable.
}
