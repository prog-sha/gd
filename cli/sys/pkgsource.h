// Fingerprint the publishable contents of a local checkout without following links.
#pragma once

#include "core/io/dir_access.h"
#include "core/io/file_access.h"

class PkgSource {
public:
	// Hash sorted entry names and contents using the same exclusions as checkout copying.
	static String stamp(const String &p_path) {
		Ref<DirAccess> dir = DirAccess::open(p_path);
		if (dir.is_null() || dir->list_dir_begin() != OK) {
			return String();
		}
		Vector<String> entries;
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			if (name.begins_with(".") || dir->is_link(name)) {
				continue;
			}
			const String child = p_path.path_join(name);
			const bool folder = dir->current_is_dir();
			if (folder && (name == "pkg" || name == "tmp" || FileAccess::exists(child.path_join("gd.json")))) {
				continue;
			}
			const String hash = folder ? stamp(child) : FileAccess::get_sha256(child);
			if (hash.is_empty()) {
				return String();
			}
			entries.push_back(vformat("%s:%d:%s:%s", folder ? "d" : "f", name.length(), name, hash));
		}
		dir->list_dir_end();
		entries.sort();
		return String("\n").join(entries).sha256_text();
	}
};
