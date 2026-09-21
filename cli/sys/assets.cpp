// Read the compiled PCK index and stream embedded content through native file access.
#include "cli/sys/assets.h"
#include "cli/sys/system.h"
#include "cli/sys/source_error.h"
#include <cerrno>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <set>
#ifdef WINDOWS_ENABLED
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace {
std::once_flag once; // Initialize the index once per process.
std::map<std::string, GDAssets::Entry> files; // Relative embedded paths and byte ranges.
constexpr uint32_t MAGIC = 0x43504447; // GDPC marker in PCK headers and footers.
constexpr uint64_t MAX_RW = 1ULL << 30; // Maximum bytes per file system call.

// Hold one executable handle for both index and content, independent of path replacement.
struct Pack {
	intptr_t fd = -1; // Read handle retained for the process lifetime.
	uint64_t size = 0; // Length of the opened executable.
#ifdef WINDOWS_ENABLED
	std::mutex mutex; // Serialize positioned reads on a synchronous Windows handle.
#endif
	// Open the executable and obtain its length from the same handle.
	Pack() {
#ifdef WINDOWS_ENABLED
		const String name = GDSystem::executable();
		fd = (intptr_t)CreateFileW((LPCWSTR)name.utf16().get_data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		LARGE_INTEGER length = {};
		if (fd != -1 && GetFileSizeEx((HANDLE)fd, &length)) size = length.QuadPart;
#else
#ifdef __linux__
		const String name = "/proc/self/exe"; // Open the running inode even after unlink.
#else
		const String name = GDSystem::executable();
#endif
		do { fd = ::open(name.utf8().get_data(), O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
		struct stat info = {};
		if (fd != -1 && ::fstat(fd, &info) == 0) size = info.st_size;
#endif
	}
	// Close the retained executable during process shutdown.
	~Pack() {
		if (fd == -1) return;
#ifdef WINDOWS_ENABLED
		CloseHandle((HANDLE)fd);
#else
		::close(fd);
#endif
	}
	// Return partial reads without sharing each stream's logical position.
	uint64_t read(uint8_t *p_data, uint64_t p_size, uint64_t p_at, Error &r_error) {
		r_error = OK;
		if (p_at > size) { r_error = ERR_FILE_EOF; return 0; }
		p_size = MIN(MIN(p_size, size - p_at), MAX_RW);
		if (!p_size) return 0;
#ifdef WINDOWS_ENABLED
		std::lock_guard<std::mutex> lock(mutex);
		OVERLAPPED at = {}; // Explicit read offset for the synchronous handle.
		at.Offset = uint32_t(p_at); at.OffsetHigh = uint32_t(p_at >> 32);
		DWORD got = 0;
		if (!ReadFile((HANDLE)fd, p_data, DWORD(p_size), &got, &at)) {
			const DWORD code = GetLastError();
			if (code == ERROR_HANDLE_EOF) r_error = ERR_FILE_EOF;
			else { SourceError::win32(code); r_error = SourceError::win32_error(code); }
			return 0;
		}
#else
		ssize_t got;
		do { got = ::pread(fd, p_data, p_size, p_at); } while (got < 0 && errno == EINTR);
		if (got < 0) { const int code = errno; SourceError::posix(code); r_error = SourceError::posix_error(code); return 0; }
#endif
		if (!got) r_error = ERR_FILE_EOF;
		return got;
	}
};
std::unique_ptr<Pack> pack; // Owner of the executable corresponding to the published index.

// Normalize only res:// paths; never map absolute paths or other mounts to embedded assets.
std::string key(const String &p_path) {
	if (!p_path.is_relative_path() && !p_path.begins_with("res://")) return {};
	const String path = p_path.trim_prefix("res://").simplify_path();
	if (path == "." || path.is_empty()) return "/";
	if (path == ".." || path.begins_with("../")) return {};
	return path.utf8().get_data();
}

// Read the PCK index from the binary footer and publish it only if every entry is in range.
void load() {
	auto input = std::make_unique<Pack>();
	const uint64_t length = input->size;
	if (length < 12) return;
	uint64_t pos = length - 12; // Current index-read position.
	bool valid = true; // Do not publish truncated or out-of-range input.
	// Read required index bytes, continuing partial reads.
	const auto bytes = [&](void *dst, uint64_t size) {
		if (!valid || pos > length || size > length - pos) { valid = false; return; }
		for (uint64_t at = 0; at < size;) {
			Error error;
			const uint64_t got = input->read(static_cast<uint8_t *>(dst) + at, size - at, pos, error);
			if (!got || error != OK) { valid = false; return; }
			at += got; pos += got;
		}
	};
	// Read fixed-width little-endian integers.
	const auto number = [&](int size) {
		unsigned char raw[8] = {}; // Largest PCK integer width.
		bytes(raw, size);
		uint64_t value = 0;
		for (int i = 0; i < size; i++) value |= uint64_t(raw[i]) << (8 * i);
		return value;
	};
	const uint64_t size = number(8);
	if (number(4) != MAGIC || size > uint64_t(length) - 12) return;
	const uint64_t begin = uint64_t(length) - 12 - size;
	const uint64_t end = uint64_t(length) - 12;
	pos = begin;
	if (number(4) != MAGIC) return;
	const uint64_t version = number(4);
	pos += 12; // Skip engine major, minor, and patch fields.
	const uint64_t flags = number(4);
	if ((version != 3 && version != 4) || flags != 2) return; // Supported unencrypted format with relative file base.
	const uint64_t base = number(8), directory = number(8);
	if (base > size || directory > size) return;
	pos = begin + directory;
	const uint64_t count = number(4);
	std::map<std::string, GDAssets::Entry> found;
	for (uint64_t i = 0; i < count; i++) {
		const uint64_t namesize = number(4);
		if (!valid || pos > end || namesize > end - pos) return;
		std::string path(namesize, '\0');
		bytes(path.data(), path.size());
		while (!path.empty() && path.back() == '\0') path.pop_back();
		const uint64_t offset = number(8), length = number(8);
		pos += 16; // PCK MD5 field, separate from content transfer length.
		const uint64_t kind = number(4);
		if (!valid || pos > end || kind != 0 || offset > size - base || length > size - base - offset) return;
		const std::string pathkey = key(String::utf8(path.data(), path.size()));
		if (pathkey.empty() || pathkey == "/" || path.find('\0') != path.npos) return;
		found[pathkey] = { begin + base + offset, length };
	}
	if (!valid) return;
	pack = std::move(input);
	files = std::move(found);
}
}

// Borrow the executable matching the index; GDFile must not close it.
intptr_t GDAssets::handle() { return pack ? pack->fd : -1; }

// Pin the executable at startup before paths can be replaced ahead of the first asset read.
void GDAssets::init() { std::call_once(once, load); }

// Read embedded content at an independent stream position.
uint64_t GDAssets::read(uint8_t *p_data, uint64_t p_size, uint64_t p_at, Error &r_error) {
	if (!pack) { r_error = ERR_UNCONFIGURED; return 0; }
	return pack->read(p_data, p_size, p_at, r_error);
}

// Find an embedded file and return its content offset and size.
bool GDAssets::find(const String &p_path, Entry &r_entry) {
	const std::string path = key(p_path);
	if (path.empty()) return false;
	init();
	const auto found = files.find(path);
	if (found == files.end()) return false;
	r_entry = found->second;
	return true;
}

// Return unique immediate children of an embedded directory.
bool GDAssets::directory(const String &p_path, PackedStringArray &r_names) {
	const std::string path = key(p_path);
	if (path.empty()) return false;
	init();
	const std::string prefix = path == "/" ? "" : path + "/";
	std::set<std::string> names;
	for (auto item = files.lower_bound(prefix); item != files.end() && item->first.compare(0, prefix.size(), prefix) == 0; ++item) {
		const std::string tail = item->first.substr(prefix.size());
		const auto slash = tail.find('/');
		names.insert(slash == tail.npos ? tail : tail.substr(0, slash) + "/");
	}
	for (const auto &name : names) r_names.push_back(String::utf8(name.c_str()));
	return !names.empty();
}
