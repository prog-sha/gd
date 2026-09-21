// Perform file and directory system calls relative to pinned mount roots.
#include "cli/sys/native_file.h"
#include "cli/sys/mount.h"
#include "cli/sys/perm.h"
#include "cli/sys/source_error.h"
#include "cli/sys/assets.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#ifdef WINDOWS_ENABLED
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#endif

thread_local Error GDFile::open_error = OK;
thread_local Error GDDir::open_error = OK;

namespace {
constexpr uint64_t MAX_RW = 1ULL << 30; // Maximum bytes per file system call.

// Preserve the original OS failure before cleanup can overwrite it.
Error failed() {
#ifdef WINDOWS_ENABLED
	const DWORD code = GetLastError();
	SourceError::win32(code);
	return SourceError::win32_error(code);
#else
	const int code = errno;
	SourceError::posix(code);
	return SourceError::posix_error(code);
#endif
}

#ifdef WINDOWS_ENABLED
// Resolve Windows mounts and pass UTF-16 paths to the OS.
String resolve(const String &p_path, bool p_write) {
	SourceError::clear();
	String why;
	const String name = Mount::resolve(p_path, p_write, why);
	if (name.is_empty()) ERR_PRINT(vformat("PermissionDenied: %s", why));
	return name;
}

// Own the search handle and prefetched first directory entry.
struct Search {
	HANDLE handle = INVALID_HANDLE_VALUE; // FindFirstFile search handle.
	WIN32_FIND_DATAW data = {}; // Current directory entry.
	bool first = true; // Whether the first entry has not yet been returned.
};
#else
// Authorize and pin the target through the shared mount boundary.
bool locate(const String &p_path, bool p_write, Mount::At &r_at, Error &r_error, bool p_link = false) {
	String why;
	if (Mount::at(p_path, p_write, r_at, why, &r_error, p_link)) return true;
	if (r_error == ERR_UNAUTHORIZED) ERR_PRINT(vformat("PermissionDenied: %s", why));
	if (r_error == OK) r_error = ERR_CANT_OPEN;
	return false;
}

// Inspect the path itself within the same mount boundary.
bool inspect(const String &p_path, struct stat &r_info) {
	Mount::At at;
	Error error;
	if (!locate(p_path, false, at, error)) return false;
	const int flags = Mount::strict_mode() && !at.root ? AT_SYMLINK_NOFOLLOW : 0;
	if (::fstatat(at.fd, at.leaf.utf8().get_data(), &r_info, flags) == 0)
		return !(flags && S_ISLNK(r_info.st_mode));
	failed();
	return false;
}
#endif
}

// Open an allowed regular file and own its descriptor without stdio buffering.
Ref<GDFile> GDFile::open(const String &p_path, Mode p_mode) {
	open_error = OK;
	SourceError::clear();
	Ref<GDFile> made;
	made.instantiate();
	made->path = p_path;
	const bool write = p_mode != READ;
	if (!write) {
		GDAssets::Entry entry;
		if (GDAssets::find(p_path, entry)) {
			if (!Perm::check(Perm::READ, p_path)) { open_error = ERR_UNAUTHORIZED; return Ref<GDFile>(); }
			// Borrow the indexed executable while each stream retains only its independent read position.
			made->fd = GDAssets::handle();
			made->embedded = true;
			made->base = entry.offset;
			made->length = entry.size;
			return made;
		}
	}
#ifdef WINDOWS_ENABLED
	const String name = resolve(p_path, write);
	if (name.is_empty()) { open_error = ERR_UNAUTHORIZED; return Ref<GDFile>(); }
	const DWORD access = p_mode == READ ? GENERIC_READ : p_mode == READ_WRITE || p_mode == WRITE_READ ? GENERIC_READ | GENERIC_WRITE : p_mode == APPEND ? FILE_APPEND_DATA : GENERIC_WRITE;
	const DWORD creation = p_mode == WRITE || p_mode == WRITE_READ ? CREATE_ALWAYS : p_mode == APPEND || p_mode == CREATE ? OPEN_ALWAYS : OPEN_EXISTING;
	HANDLE fd = CreateFileW((LPCWSTR)name.utf16().get_data(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (fd == INVALID_HANDLE_VALUE) { open_error = failed(); return Ref<GDFile>(); }
	BY_HANDLE_FILE_INFORMATION info = {};
	if (!GetFileInformationByHandle(fd, &info)) { open_error = failed(); CloseHandle(fd); return Ref<GDFile>(); }
	if (GetFileType(fd) != FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		CloseHandle(fd); open_error = ERR_FILE_CANT_OPEN; return Ref<GDFile>();
	}
	made->fd = (intptr_t)fd;
#else
	Mount::At at;
	if (!locate(p_path, write, at, open_error)) return Ref<GDFile>();
	int flags = p_mode == READ ? O_RDONLY : p_mode == READ_WRITE ? O_RDWR : p_mode == APPEND ? O_WRONLY | O_CREAT | O_APPEND :
			p_mode == WRITE_READ ? O_RDWR | O_CREAT | O_TRUNC : p_mode == CREATE ? O_WRONLY | O_CREAT : O_WRONLY | O_CREAT | O_TRUNC;
	flags |= O_CLOEXEC | O_NONBLOCK;
	if (Mount::strict_mode() && !at.root) flags |= O_NOFOLLOW;
	int fd;
	do { fd = ::openat(at.fd, at.leaf.utf8().get_data(), flags, 0666); } while (fd < 0 && errno == EINTR);
	if (fd < 0) { open_error = failed(); return Ref<GDFile>(); }
	struct stat info = {};
	if (::fstat(fd, &info) < 0) { open_error = failed(); ::close(fd); return Ref<GDFile>(); }
	// Reject substituted FIFOs or devices that could occupy a regular-file worker indefinitely.
	if (!S_ISREG(info.st_mode)) { ::close(fd); open_error = ERR_FILE_CANT_OPEN; return Ref<GDFile>(); }
	const int status = ::fcntl(fd, F_GETFL);
	if (status < 0 || ::fcntl(fd, F_SETFL, status & ~O_NONBLOCK) < 0) { open_error = failed(); ::close(fd); return Ref<GDFile>(); }
	made->fd = fd;
#endif
	return made;
}

// Return partial reads from one system call directly to the caller.
uint64_t GDFile::get_buffer(uint8_t *p_data, uint64_t p_size) {
	error = OK;
	if (fd == -1) { error = ERR_UNCONFIGURED; return 0; }
	if (!p_size) return 0;
	if (embedded) {
		if (position >= length) { error = ERR_FILE_EOF; return 0; }
		const uint64_t got = GDAssets::read(p_data, MIN(p_size, length - position), base + position, error);
		position += got;
		return got;
	}
#ifdef WINDOWS_ENABLED
	DWORD got = 0;
	if (!ReadFile((HANDLE)fd, p_data, DWORD(MIN(p_size, MAX_RW)), &got, nullptr)) { error = failed(); return 0; }
#else
	ssize_t got;
	do { got = ::read(fd, p_data, MIN(p_size, MAX_RW)); } while (got < 0 && errno == EINTR);
	if (got < 0) { error = failed(); return 0; }
#endif
	if (!got) error = ERR_FILE_EOF;
	return got;
}

// Read the requested range, retaining only bytes obtained before EOF.
PackedByteArray GDFile::get_buffer(uint64_t p_size) {
	error = OK;
	PackedByteArray out;
	if (p_size > INT64_MAX || out.resize(p_size) != OK) { error = ERR_OUT_OF_MEMORY; return out; }
	uint64_t at = 0;
	while (at < p_size) {
		const uint64_t got = get_buffer(out.ptrw() + at, p_size - at);
		at += got;
		if (!got || error != OK) break;
	}
	out.resize(at);
	return out;
}

// Continue kernel writes and preserve the completed byte count even on failure.
uint64_t GDFile::write(const uint8_t *p_data, uint64_t p_size) {
	error = OK;
	if (embedded) { error = ERR_UNAUTHORIZED; return 0; }
	if (fd == -1) { error = ERR_UNCONFIGURED; return 0; }
	uint64_t at = 0;
	const uint8_t empty = 0; // Let the OS validate the descriptor even for an empty write.
	do {
		const uint8_t *data = p_size ? p_data + at : &empty;
#ifdef WINDOWS_ENABLED
		DWORD sent = 0;
		if (!WriteFile((HANDLE)fd, data, DWORD(MIN(p_size - at, MAX_RW)), &sent, nullptr)) { error = failed(); return at + sent; }
#else
		ssize_t sent;
		do { sent = ::write(fd, data, MIN(p_size - at, MAX_RW)); } while (sent < 0 && errno == EINTR);
		if (sent < 0) { error = failed(); return at; }
#endif
		if (!sent && at < p_size) { error = ERR_FILE_CANT_WRITE; return at; }
		at += sent;
	} while (at < p_size);
	return at;
}

// Report incomplete whole-buffer writes as failure rather than success.
bool GDFile::store_buffer(const uint8_t *p_data, uint64_t p_size) {
	return write(p_data, p_size) == p_size && error == OK;
}

// Write UTF-8 with its explicit byte length.
bool GDFile::store_string(const String &p_text) {
	const CharString raw = p_text.utf8();
	return store_buffer(reinterpret_cast<const uint8_t *>(raw.get_data()), raw.length());
}

// Set the file position from the beginning.
void GDFile::seek(uint64_t p_at) {
	error = OK;
	if (p_at > uint64_t(INT64_MAX) - base) { error = ERR_INVALID_PARAMETER; return; }
	position = p_at;
	if (embedded) return;
	p_at += base;
#ifdef WINDOWS_ENABLED
	LARGE_INTEGER pos; pos.QuadPart = p_at;
	if (!SetFilePointerEx((HANDLE)fd, pos, nullptr, FILE_BEGIN)) error = failed();
#else
	if (::lseek(fd, p_at, SEEK_SET) < 0) error = failed();
#endif
}

// Set the file position to the end.
void GDFile::seek_end() {
	if (embedded) { seek(length); return; }
	error = OK;
#ifdef WINDOWS_ENABLED
	LARGE_INTEGER pos = {};
	if (!SetFilePointerEx((HANDLE)fd, pos, nullptr, FILE_END)) error = failed();
#else
	if (::lseek(fd, 0, SEEK_END) < 0) error = failed();
#endif
}

// Return the current file position.
uint64_t GDFile::get_position() {
	if (embedded) return position;
	error = OK;
#ifdef WINDOWS_ENABLED
	LARGE_INTEGER zero = {}, pos = {};
	if (!SetFilePointerEx((HANDLE)fd, zero, &pos, FILE_CURRENT)) { error = failed(); return 0; }
	return pos.QuadPart;
#else
	const off_t pos = ::lseek(fd, 0, SEEK_CUR);
	if (pos < 0) { error = failed(); return 0; }
	return pos;
#endif
}

// Read the opened file's size without reopening its path.
uint64_t GDFile::get_length() {
	if (embedded) return length;
	error = OK;
#ifdef WINDOWS_ENABLED
	LARGE_INTEGER size;
	if (!GetFileSizeEx((HANDLE)fd, &size)) { error = failed(); return 0; }
	return size.QuadPart;
#else
	struct stat info;
	if (::fstat(fd, &info) < 0) { error = failed(); return 0; }
	return info.st_size;
#endif
}

// Truncate the opened file itself, immune to path replacement.
Error GDFile::resize(uint64_t p_size) {
	if (embedded) return error = ERR_UNAUTHORIZED;
	if (p_size > INT64_MAX) return error = ERR_INVALID_PARAMETER;
#ifdef WINDOWS_ENABLED
	FILE_END_OF_FILE_INFO info; info.EndOfFile.QuadPart = p_size;
	return error = SetFileInformationByHandle((HANDLE)fd, FileEndOfFileInfo, &info, sizeof(info)) ? OK : failed();
#else
	int result;
	do { result = ::ftruncate(fd, p_size); } while (result < 0 && errno == EINTR);
	return error = result == 0 ? OK : failed();
#endif
}

// Compare native file identities so copying cannot destroy the source through an alias.
bool GDFile::same(const Ref<GDFile> &p_file) {
	error = OK;
#ifdef WINDOWS_ENABLED
	BY_HANDLE_FILE_INFORMATION left = {}, right = {};
	if (!GetFileInformationByHandle((HANDLE)fd, &left) || !GetFileInformationByHandle((HANDLE)p_file->fd, &right)) { error = failed(); return false; }
	return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
#else
	struct stat left, right;
	if (::fstat(fd, &left) < 0 || ::fstat(p_file->fd, &right) < 0) { error = failed(); return false; }
	return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
#endif
}

// Do not retry close, which could close a different file after descriptor reuse.
Error GDFile::close() {
	if (fd == -1) return OK;
	const intptr_t held = fd;
	fd = -1;
	if (embedded) { embedded = false; return error = OK; }
#ifdef WINDOWS_ENABLED
	return error = CloseHandle((HANDLE)held) ? OK : failed();
#else
	return error = ::close(held) == 0 ? OK : failed();
#endif
}

// Reclaim the descriptor with the final reference.
GDFile::~GDFile() { close(); }

// Check file existence within permission boundaries.
bool GDFile::exists(const String &p_path) {
	GDAssets::Entry entry;
	if (GDAssets::find(p_path, entry)) return Perm::check(Perm::READ, p_path);
#ifdef WINDOWS_ENABLED
	const String name = resolve(p_path, false);
	if (name.is_empty()) return false;
	const DWORD attr = GetFileAttributesW((LPCWSTR)name.utf16().get_data());
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat info;
	return inspect(p_path, info) && S_ISREG(info.st_mode);
#endif
}

// Join only relative directory paths to the base.
String GDDir::full(const String &p_path) const { return p_path.is_relative_path() ? path.path_join(p_path) : p_path; }

// Create a directory object for mutation operations.
Ref<GDDir> GDDir::create() { Ref<GDDir> out; out.instantiate(); return out; }

// Pin the directory against path replacement during enumeration.
Ref<GDDir> GDDir::open(const String &p_path) {
	open_error = OK;
	Ref<GDDir> out = create();
	out->path = p_path.is_relative_path() ? String("res://").path_join(p_path) : p_path;
	if (GDAssets::directory(p_path, out->names)) {
		if (!Perm::check(Perm::READ, p_path)) { open_error = ERR_UNAUTHORIZED; return Ref<GDDir>(); }
		out->embedded = true;
		return out;
	}
#ifdef WINDOWS_ENABLED
	const String name = resolve(p_path, false);
	if (name.is_empty()) { open_error = ERR_UNAUTHORIZED; return Ref<GDDir>(); }
	const DWORD attr = GetFileAttributesW((LPCWSTR)name.utf16().get_data());
	if (attr == INVALID_FILE_ATTRIBUTES) { open_error = failed(); return Ref<GDDir>(); }
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { SetLastError(ERROR_PATH_NOT_FOUND); open_error = failed(); return Ref<GDDir>(); }
#else
	Mount::At at;
	if (!locate(p_path, false, at, open_error)) return Ref<GDDir>();
	const int flags = Mount::strict_mode() && !at.root ? O_NOFOLLOW : 0;
	out->fd = ::openat(at.fd, at.leaf.utf8().get_data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | flags);
	if (out->fd < 0) { open_error = failed(); return Ref<GDDir>(); }
#endif
	return out;
}

// Start native directory enumeration.
Error GDDir::list_dir_begin() {
	if (embedded) { next = 0; return OK; }
	list_dir_end();
#ifdef WINDOWS_ENABLED
	const String name = resolve(path, false);
	if (name.is_empty()) return ERR_UNAUTHORIZED;
	Search *search = memnew(Search);
	const String pattern = name.trim_suffix("\\") + "\\*"; // Keep the extended path prefix opaque to lexical joining.
	search->handle = FindFirstFileW((LPCWSTR)pattern.utf16().get_data(), &search->data);
	if (search->handle == INVALID_HANDLE_VALUE) { const Error error = failed(); memdelete(search); return error; }
	stream = search;
#else
	const int copy = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (copy < 0) return failed();
	stream = ::fdopendir(copy);
	if (!stream) { const Error error = failed(); ::close(copy); return error; }
#endif
	return OK;
}

// Return name and type from the same enumeration result.
String GDDir::get_next() {
	if (embedded) {
		if (next >= names.size()) return String();
		const String name = names[next++];
		current_dir = name.ends_with("/");
		current_hidden = name.begins_with(".");
		return current_dir ? name.substr(0, name.length() - 1) : name;
	}
	if (!stream) return String();
#ifdef WINDOWS_ENABLED
	auto *search = static_cast<Search *>(stream);
	if (!search->first && !FindNextFileW(search->handle, &search->data)) {
		if (GetLastError() != ERROR_NO_MORE_FILES) failed();
		return String();
	}
	search->first = false;
	current_dir = (search->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	current_hidden = (search->data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
	return String::utf16(reinterpret_cast<const char16_t *>(search->data.cFileName));
#else
	for (;;) {
		errno = 0;
		dirent *item = ::readdir(static_cast<DIR *>(stream));
		if (!item) { if (errno) failed(); return String(); }
		struct stat info = {};
		if (::fstatat(::dirfd(static_cast<DIR *>(stream)), item->d_name, &info, AT_SYMLINK_NOFOLLOW) < 0) {
			if (errno == ENOENT) continue; // Skip only entries removed concurrently.
			failed();
			return String();
		}
		current_dir = S_ISDIR(info.st_mode);
		current_hidden = item->d_name[0] == '.';
		return String::utf8(item->d_name);
	}
#endif
}

// Release enumeration resources.
void GDDir::list_dir_end() {
	if (!stream) return;
#ifdef WINDOWS_ENABLED
	auto *search = static_cast<Search *>(stream);
	FindClose(search->handle);
	memdelete(search);
#else
	::closedir(static_cast<DIR *>(stream));
#endif
	stream = nullptr;
}

// Release enumeration and base descriptors exactly once.
GDDir::~GDDir() {
	list_dir_end();
#ifndef WINDOWS_ENABLED
	if (fd >= 0) ::close(fd);
#endif
}

// Create one directory level.
Error GDDir::make_dir(const String &p_path) {
	const String target = full(p_path);
#ifdef WINDOWS_ENABLED
	const String name = resolve(target, true);
	if (name.is_empty()) return ERR_UNAUTHORIZED;
	return CreateDirectoryW((LPCWSTR)name.utf16().get_data(), nullptr) ? OK : failed();
#else
	Mount::At at;
	Error error;
	if (!locate(target, true, at, error)) return error;
	return ::mkdirat(at.fd, at.leaf.utf8().get_data(), 0777) == 0 ? OK : failed();
#endif
}

// Remove a symbolic link itself rather than its target.
Error GDDir::remove(const String &p_path) {
	const String target = full(p_path);
#ifdef WINDOWS_ENABLED
	const String name = resolve(target, true);
	if (name.is_empty()) return ERR_UNAUTHORIZED;
	const auto wide = name.utf16();
	const DWORD attr = GetFileAttributesW((LPCWSTR)wide.get_data());
	if (attr == INVALID_FILE_ATTRIBUTES) return failed();
	return ((attr & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW((LPCWSTR)wide.get_data()) : DeleteFileW((LPCWSTR)wide.get_data())) ? OK : failed();
#else
	Mount::At at;
	Error error;
	if (!locate(target, true, at, error, true)) return error;
	const auto name = at.leaf.utf8();
	if (::unlinkat(at.fd, name.get_data(), 0) == 0) return OK;
	const int code = errno;
	if (code == EISDIR || code == EPERM) {
		if (::unlinkat(at.fd, name.get_data(), AT_REMOVEDIR) == 0) return OK;
	}
	return failed();
#endif
}

// Use the OS replacement rename without deleting the destination first.
Error GDDir::rename(const String &p_src, const String &p_dst) {
#ifdef WINDOWS_ENABLED
	const String src = resolve(full(p_src), true), dst = resolve(full(p_dst), true);
	if (src.is_empty() || dst.is_empty()) return ERR_UNAUTHORIZED;
	return MoveFileExW((LPCWSTR)src.utf16().get_data(), (LPCWSTR)dst.utf16().get_data(), MOVEFILE_REPLACE_EXISTING) ? OK : failed();
#else
	Mount::At src, dst;
	Error error;
	if (!locate(full(p_src), true, src, error, true) || !locate(full(p_dst), true, dst, error, true)) return error;
	return ::renameat(src.fd, src.leaf.utf8().get_data(), dst.fd, dst.leaf.utf8().get_data()) == 0 ? OK : failed();
#endif
}

// Check directory existence within permission boundaries.
bool GDDir::dir_exists_absolute(const String &p_path) {
	PackedStringArray names;
	if (GDAssets::directory(p_path, names)) return Perm::check(Perm::READ, p_path);
#ifdef WINDOWS_ENABLED
	const String name = resolve(p_path, false);
	if (name.is_empty()) return false;
	const DWORD attr = GetFileAttributesW((LPCWSTR)name.utf16().get_data());
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat info;
	return inspect(p_path, info) && S_ISDIR(info.st_mode);
#endif
}

// Query native filesystem case-comparison rules.
bool GDDir::is_case_sensitive(const String &p_path) const {
#ifdef WINDOWS_ENABLED
	const String name = resolve(p_path, false);
	if (name.is_empty()) return false;
	HANDLE handle = CreateFileW((LPCWSTR)name.utf16().get_data(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return false;
	FILE_CASE_SENSITIVE_INFO info = {};
	const bool ok = GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo, &info, sizeof(info));
	CloseHandle(handle);
	return ok && (info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR);
#else
	Ref<GDDir> dir = open(p_path);
	if (dir.is_null()) return true;
#ifdef __APPLE__
	return ::fpathconf(dir->fd, _PC_CASE_SENSITIVE) != 0;
#elif defined(__linux__)
	long flags = 0;
	return ::ioctl(dir->fd, FS_IOC_GETFLAGS, &flags) != 0 || !(flags & FS_CASEFOLD_FL);
#else
	return true;
#endif
#endif
}

// Identify alternative paths to the same file by device and inode.
String GDDir::get_identity(const String &p_path) const {
#ifdef WINDOWS_ENABLED
	const String name = resolve(p_path, false);
	if (name.is_empty()) return String();
	HANDLE handle = CreateFileW((LPCWSTR)name.utf16().get_data(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return String();
	BY_HANDLE_FILE_INFORMATION info = {};
	const bool ok = GetFileInformationByHandle(handle, &info);
	CloseHandle(handle);
	return ok ? vformat("%d:%d:%d", info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow) : String();
#else
	struct stat info;
	return inspect(p_path, info) ? vformat("%d:%d", uint64_t(info.st_dev), uint64_t(info.st_ino)) : String();
#endif
}
