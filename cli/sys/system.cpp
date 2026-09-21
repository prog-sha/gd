// Query environment, working directory, and processor availability through native OS interfaces.
#include "cli/sys/system.h"
#include "cli/data/hash_core.h"
#include <cerrno>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif
#ifdef WINDOWS_ENABLED
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef MACOS_ENABLED
#include <mach-o/dyld.h>
#endif
#endif

// Read the working directory without imposing a fixed path width.
String GDSystem::cwd() {
#ifdef WINDOWS_ENABLED
	DWORD size = GetCurrentDirectoryW(0, nullptr);
	while (size) {
		std::wstring path(size, L'\0');
		const DWORD got = GetCurrentDirectoryW(size, path.data());
		if (!got) return String();
		if (got < size) return String::utf16(reinterpret_cast<const char16_t *>(path.data()), got).replace_char('\\', '/');
		size = got + 1;
	}
	return String();
#else
	std::vector<char> path(256); // Initial buffer, expanded on long working directories.
	for (;;) {
		if (::getcwd(path.data(), path.size())) return String::utf8(path.data());
		if (errno != ERANGE) return String();
		path.resize(path.size() * 2);
	}
#endif
}

// Read the executable path without depending on project or scene initialization.
String GDSystem::executable() {
#ifdef WINDOWS_ENABLED
	std::wstring path(256, L'\0'); // Initial buffer, expanded when necessary.
	for (;;) {
		const DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
		if (!size) return String();
		if (size < path.size()) return String::utf16(reinterpret_cast<const char16_t *>(path.data()), size).replace_char('\\', '/');
		path.resize(path.size() * 2);
	}
#elif defined(MACOS_ENABLED)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> path(size); // Exact width reported by the loader.
	if (_NSGetExecutablePath(path.data(), &size) != 0) return String();
	char *resolved = ::realpath(path.data(), nullptr);
	if (!resolved) return String::utf8(path.data());
	const String out = String::utf8(resolved);
	std::free(resolved);
	return out;
#else
	std::vector<char> path(256); // Initial buffer, expanded when necessary.
	for (;;) {
		const ssize_t size = ::readlink("/proc/self/exe", path.data(), path.size());
		if (size < 0) return String();
		if (size_t(size) < path.size()) return String::utf8(path.data(), size);
		path.resize(path.size() * 2);
	}
#endif
}

// Distinguish an empty environment value from an unset variable.
bool GDSystem::has_env(const String &p_name) {
#ifdef WINDOWS_ENABLED
	SetLastError(ERROR_SUCCESS);
	return GetEnvironmentVariableW((LPCWSTR)p_name.utf16().get_data(), nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
#else
	return std::getenv(p_name.utf8().get_data()) != nullptr;
#endif
}

// Read environment variables using the OS character representation.
String GDSystem::env(const String &p_name) {
#ifdef WINDOWS_ENABLED
	const auto name = p_name.utf16();
	for (;;) {
		const DWORD size = GetEnvironmentVariableW((LPCWSTR)name.get_data(), nullptr, 0);
		if (!size) return String();
		std::wstring value(size, L'\0');
		const DWORD got = GetEnvironmentVariableW((LPCWSTR)name.get_data(), value.data(), size);
		if (got >= size) continue;
		return String::utf16(reinterpret_cast<const char16_t *>(value.data()), got);
	}
#else
	const char *value = std::getenv(p_name.utf8().get_data());
	return value ? String::utf8(value) : String();
#endif
}

// Return an existing temporary directory selected by the process environment.
String GDSystem::temp() {
#ifdef WINDOWS_ENABLED
	DWORD size = GetTempPathW(0, nullptr);
	while (size) {
		std::wstring path(size, L'\0');
		const DWORD got = GetTempPathW(size, path.data());
		if (!got) return String();
		if (got < size) return String::utf16(reinterpret_cast<const char16_t *>(path.data()), got).replace_char('\\', '/');
		size = got + 1;
	}
	return String();
#else
	const char *const names[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR", nullptr}; // Conventional temporary-directory variables in precedence order.
	for (int i = 0; names[i]; ++i) {
		const String path = env(names[i]);
		if (!path.is_empty() && is_dir(path)) return path;
	}
	return "/tmp";
#endif
}

// Select package storage independently of the process working directory and temporary files.
String GDSystem::cache_dir() {
	const String custom = env("GD_CACHE_HOME");
	if (!custom.is_empty()) return custom;
#ifdef WINDOWS_ENABLED
	PWSTR path = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
		const String out = String::utf16(reinterpret_cast<const char16_t *>(path)).replace_char('\\', '/').path_join("gd");
		CoTaskMemFree(path);
		return out;
	}
	return String();
#else
	const String xdg = env("XDG_CACHE_HOME");
	if (xdg.is_absolute_path()) return xdg.path_join("gd");
	String base = env("HOME");
	if (base.is_empty()) {
		struct passwd entry = {};
		struct passwd *found = nullptr;
		std::vector<char> data(1024); // Grow to the size required by the account database.
		int err;
		while ((err = getpwuid_r(getuid(), &entry, data.data(), data.size(), &found)) == ERANGE) data.resize(data.size() * 2);
		if (err == 0 && found && found->pw_dir) base = String::utf8(found->pw_dir);
	}
	return base.is_empty() ? String() : base.path_join(".gd");
#endif
}

// Derive a working-directory-specific user path with a separate MD5 calculation.
String GDSystem::user_dir() {
	const CharString raw = cwd().utf8();
	unsigned char digest[16]; // Fixed MD5 width used for namespace separation, not authentication.
	GDCrypto::Hash32 hash(GDCrypto::Hash32::MD5);
	hash.write(raw.get_data(), raw.length());
	hash.sum(digest);
	char name[13] = {}; // Twelve-digit user-directory identifier and terminator.
	const char *hex = "0123456789abcdef"; // lowercase hex
	for (int i = 0; i < 6; i++) { name[i * 2] = hex[digest[i] >> 4]; name[i * 2 + 1] = hex[digest[i] & 15]; }
	return temp().path_join(String("gd-") + name);
}

// Return the public name of the target OS.
String GDSystem::platform() {
#ifdef WINDOWS_ENABLED
	return "Windows";
#elif defined(MACOS_ENABLED)
	return "macOS";
#else
	return "Linux";
#endif
}

// Use thread affinity on Linux and available logical-processor counts on other systems.
int GDSystem::cpus() {
#ifdef __linux__
	constexpr int MAX_CPUS = 64 * 1024; // CPU-affinity mask capacity.
	alignas(cpu_set_t) unsigned char mask[MAX_CPUS / 8] = {}; // Zero any trailing bits not written by the kernel.
	if (::sched_getaffinity(0, sizeof(mask), reinterpret_cast<cpu_set_t *>(mask)) != 0) return 1;
	int count = 0;
	for (unsigned char bits : mask) {
		for (; bits; bits >>= 1) count += bits & 1;
	}
	return MAX(1, count); // Use one processor when the query fails or the mask is empty.
#else
	return MAX(1U, std::thread::hardware_concurrency());
#endif
}

// Prepare native directories before mount initialization.
bool GDSystem::make_dirs(const String &p_path) {
	const String path = p_path.replace_char('\\', '/').simplify_path();
	if (path.is_empty()) return false;
	if (is_dir(path)) return true;
	const String parent = path.get_base_dir();
	if (!parent.is_empty() && parent != path && !is_dir(parent) && !make_dirs(parent)) return false;
#ifdef WINDOWS_ENABLED
	if (CreateDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16().get_data()), nullptr)) return true;
	return GetLastError() == ERROR_ALREADY_EXISTS && is_dir(path);
#else
	if (::mkdir(path.utf8().get_data(), 0777) == 0) return true;
	return errno == EEXIST && is_dir(path);
#endif
}

// Check native directory existence before mount initialization.
bool GDSystem::is_dir(const String &p_path) {
#ifdef WINDOWS_ENABLED
	const DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(p_path.utf16().get_data()));
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat info = {};
	return ::stat(p_path.utf8().get_data(), &info) == 0 && S_ISDIR(info.st_mode);
#endif
}
