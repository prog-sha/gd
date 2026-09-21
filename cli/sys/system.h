// Obtain runtime OS metadata through native interfaces.
#pragma once
#include "core/string/ustring.h"

namespace GDSystem {
String cwd(); // Return the current working directory.
String executable(); // Return the running executable's path.
String env(const String &p_name); // Read an environment value.
bool has_env(const String &p_name); // Distinguish empty values from unset variables.
String temp(); // Return the OS temporary directory.
String cache_dir(); // Return persistent per-user package storage, honoring explicit overrides.
String user_dir(); // Return temporary storage partitioned by working directory.
String platform(); // Return the target OS's public name.
int cpus(); // Count available logical processors, respecting Linux affinity.
bool make_dirs(const String &p_path); // Prepare directories before mount setup.
bool is_dir(const String &p_path); // Check directories before mount setup.
}
