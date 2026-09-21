/**************************************************************************/
/*  jail_unix.cpp                                                         */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Pin Unix file operations to mount-relative descriptors as declared in jail_unix.h.

#include "cli/sys/jail_unix.h"
#include "cli/sys/clock.h"

#ifdef UNIX_ENABLED

#include "cli/sys/mount.h"
#include "cli/sys/source_error.h"

#include "core/os/os.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cerrno>
#if !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__) && !defined(WEB_ENABLED)
#include <sys/xattr.h>
#endif
#include <unistd.h>

namespace {
constexpr int SAVE_ATTEMPTS = 10000; // Temporary-name collision retry budget.
} // namespace

// Authorize the path and acquire its parent through the shared mount boundary.
static bool _jail_at(const String &p_path, Perm::Kind p_kind, Mount::At &r_at, bool p_log = true, Error *r_err = nullptr, bool p_leaf_link = false) {
	String why;
	Error err = OK;
	if (Mount::at(p_path, p_kind == Perm::WRITE, r_at, why, &err, p_leaf_link)) {
		return true;
	}
	if (r_err) {
		*r_err = err;
	}
	// Match diagnostics to the actual cause; absence is not a permission denial.
	if (p_log || err == ERR_UNAUTHORIZED) {
		const char *kind = "FileError";
		if (err == ERR_UNAUTHORIZED || err == ERR_FILE_NO_PERMISSION) {
			kind = "PermissionDenied";
		} else if (err == ERR_FILE_NOT_FOUND || err == ERR_DOES_NOT_EXIST) {
			kind = "NotFound";
		} else if (err == ERR_OUT_OF_MEMORY) {
			kind = "Limited";
		}
		ERR_PRINT(vformat("%s: %s", kind, why));
	}
	return false;
}

// Determine whether strict named-mount access must avoid symlinks.
static bool _no_link(const Mount::At &p_at) {
	return Mount::strict_mode() && !p_at.root;
}

// Convert the original OS failure into a portable error category.
// Keep permission denial, absence, and resource exhaustion distinct.
static Error _errno_kind(int p_errno) {
	SourceError::posix(p_errno);
	return SourceError::posix_error(p_errno);
}

// Reject final symlinks in strict named mounts; otherwise inspect the target.
static bool _jail_stat(const String &p_path, Perm::Kind p_kind, struct stat &r_st) {
	Mount::At at;
	if (!_jail_at(p_path, p_kind, at, false)) {
		return false;
	}
	const bool no_link = _no_link(at);
	const int flags = no_link ? AT_SYMLINK_NOFOLLOW : 0;
	return ::fstatat(at.fd, at.leaf.utf8().get_data(), &r_st, flags) == 0 && (!no_link || !S_ISLNK(r_st.st_mode));
}

// Pin the final file by descriptor so metadata operations use the same object.
static int _jail_open(const String &p_path, Perm::Kind p_kind, int p_flags) {
	Mount::At at;
	if (!_jail_at(p_path, p_kind, at)) {
		return -1;
	}
	const int flags = _no_link(at) ? O_NOFOLLOW : 0;
	return ::openat(at.fd, at.leaf.utf8().get_data(), p_flags | flags | O_CLOEXEC);
}

// Close the file and commit any safe replacement within the same parent directory.
void FileJailUnix::_finish() {
	if (swap_fd < 0) {
		_close();
		return;
	}
	if (f) {
		::fclose(f);
		f = nullptr;
		if (close_notification_func) {
			close_notification_func(path, flags);
		}
	}
	const int err = ::renameat(swap_fd, swap_from.utf8().get_data(), swap_fd, swap_to.utf8().get_data());
	if (err != 0) {
		::unlinkat(swap_fd, swap_from.utf8().get_data(), 0);
		if (close_fail_notify) {
			close_fail_notify(swap_full);
		}
	}
	::close(swap_fd);
	swap_fd = -1;
	swap_from.clear();
	swap_to.clear();
	swap_full.clear();
	ERR_FAIL_COND_MSG(err != 0, "Safe file replacement failed.");
}

// Open the file after permission checks.
Error FileJailUnix::open_internal(const String &p_path, int p_mode_flags) {
	if (Perm::is_trusted()) {
		_finish();
		return FileAccessUnix::open_internal(p_path, p_mode_flags);
	}
	_finish();

	int of = 0;
	const char *mode = nullptr;
	switch (p_mode_flags) {
		case READ:
			of = O_RDONLY;
			mode = "rb";
			break;
		case WRITE:
			of = O_WRONLY | O_CREAT | O_TRUNC;
			mode = "wb";
			break;
		case READ_WRITE:
			of = O_RDWR;
			mode = "rb+";
			break;
		case WRITE_READ:
			of = O_RDWR | O_CREAT | O_TRUNC;
			mode = "wb+";
			break;
		case APPEND:
			of = O_WRONLY | O_CREAT | O_APPEND;
			mode = "ab";
			break;
		default:
			return ERR_INVALID_PARAMETER;
	}

	Mount::At at;
	const Perm::Kind kind = p_mode_flags == READ ? Perm::READ : Perm::WRITE;
	// Preserve the mount's distinction between missing parents and permission denial.
	Error why = OK;
	if (!_jail_at(p_path, kind, at, true, &why)) {
		last_error = why;
		return why;
	}
	int fd = -1;
	int open_code = 0; // Open-related errno captured before cleanup.
	const bool swap = is_backup_save_enabled() && p_mode_flags == WRITE;
	if (swap) {
		const uint64_t seed = GDClock::usec();
		for (uint64_t i = 0; i < SAVE_ATTEMPTS; i++) {
			swap_from = vformat(".%s.%d.tmp", at.leaf, seed + i);
			fd = ::openat(at.fd, swap_from.utf8().get_data(), of | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
			if (fd >= 0 || errno != EEXIST) {
				break;
			}
		}
		if (fd >= 0) {
			swap_fd = ::dup(at.fd);
			if (swap_fd < 0) {
				open_code = errno;
				::unlinkat(at.fd, swap_from.utf8().get_data(), 0);
				::close(fd);
				fd = -1;
			} else {
				swap_to = at.leaf;
				swap_full = at.full;
				struct stat old = {};
				if (::fstatat(at.fd, at.leaf.utf8().get_data(), &old, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(old.st_mode)) {
					::fchmod(fd, old.st_mode & 0777);
				}
			}
		}
	} else {
		const int link_flag = _no_link(at) ? O_NOFOLLOW : 0;
		fd = ::openat(at.fd, at.leaf.utf8().get_data(), of | link_flag | O_NONBLOCK | O_CLOEXEC, 0644);
	}
	if (fd < 0) {
		swap_from.clear();
		const Error kind_of = _errno_kind(open_code != 0 ? open_code : errno);
		last_error = kind_of == FAILED ? ERR_FILE_CANT_OPEN : kind_of;
		return last_error;
	}
	struct stat st = {};
	if (::fstat(fd, &st) != 0) {
		const int code = errno;
		::close(fd);
		if (swap_fd >= 0) {
			::unlinkat(swap_fd, swap_from.utf8().get_data(), 0);
			::close(swap_fd);
			swap_fd = -1;
		}
		last_error = _errno_kind(code);
		if (last_error == FAILED) {
			last_error = ERR_FILE_CANT_OPEN;
		}
		return last_error;
	}
	if (!S_ISREG(st.st_mode)) {
		::close(fd);
		if (swap_fd >= 0) {
			::unlinkat(swap_fd, swap_from.utf8().get_data(), 0);
			::close(swap_fd);
			swap_fd = -1;
		}
		SourceError::clear();
		return last_error = ERR_FILE_CANT_OPEN;
	}
	// Avoid blocking on substituted FIFOs; restore regular-file mode after validating the target.
	const int status = ::fcntl(fd, F_GETFL);
	if (status >= 0) {
		::fcntl(fd, F_SETFL, status & ~O_NONBLOCK);
	}
	f = ::fdopen(fd, mode);
	if (!f) {
		const int code = errno;
		::close(fd);
		if (swap_fd >= 0) {
			::unlinkat(swap_fd, swap_from.utf8().get_data(), 0);
			::close(swap_fd);
			swap_fd = -1;
		}
		last_error = _errno_kind(code);
		if (last_error == FAILED) {
			last_error = ERR_FILE_CANT_OPEN;
		}
		return last_error;
	}
	path_src = p_path;
	path = swap ? at.full.get_base_dir().path_join(swap_from) : at.full;
	flags = p_mode_flags;
	last_error = OK;
	return OK;
}

// Treat public close and safe-replacement commit as one completion operation.
void FileJailUnix::close() {
	_finish();
}

// Commit temporary files during destruction and release the parent descriptor.
FileJailUnix::~FileJailUnix() {
	_finish();
}

// Check file existence within the permitted scope.
bool FileJailUnix::file_exists(const String &p_path) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::file_exists(p_path);
	}
	struct stat st = {};
	return _jail_stat(p_path, Perm::READ, st) && S_ISREG(st.st_mode);
}

// Read the file modification time.
uint64_t FileJailUnix::_get_modified_time(const String &p_file) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_get_modified_time(p_file);
	}
	struct stat st = {};
	return _jail_stat(p_file, Perm::READ, st) ? st.st_mtime : 0;
}

// Read the file access time.
uint64_t FileJailUnix::_get_access_time(const String &p_file) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_get_access_time(p_file);
	}
	struct stat st = {};
	return _jail_stat(p_file, Perm::READ, st) ? st.st_atime : 0;
}

// Read the file size.
int64_t FileJailUnix::_get_size(const String &p_file) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_get_size(p_file);
	}
	struct stat st = {};
	return _jail_stat(p_file, Perm::READ, st) && S_ISREG(st.st_mode) ? st.st_size : -1;
}

// Read Unix permission bits.
BitField<FileAccess::UnixPermissionFlags> FileJailUnix::_get_unix_permissions(const String &p_file) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_get_unix_permissions(p_file);
	}
	struct stat st = {};
	return _jail_stat(p_file, Perm::READ, st) ? BitField<FileAccess::UnixPermissionFlags>(st.st_mode & 0xFFF) : BitField<FileAccess::UnixPermissionFlags>();
}

// Set Unix permission bits.
Error FileJailUnix::_set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_set_unix_permissions(p_file, p_permissions);
	}
	const int fd = _jail_open(p_file, Perm::WRITE, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		return ERR_UNAUTHORIZED;
	}
	const int err = ::fchmod(fd, p_permissions);
	::close(fd);
	return err == 0 ? OK : FAILED;
}

// Read the hidden attribute.
bool FileJailUnix::_get_hidden_attribute(const String &p_file) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_get_hidden_attribute(p_file);
	}
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__APPLE__)
	struct stat st = {};
	return _jail_stat(p_file, Perm::READ, st) && (st.st_flags & UF_HIDDEN);
#else
	return false;
#endif
}

// Set the hidden attribute.
Error FileJailUnix::_set_hidden_attribute(const String &p_file, bool p_hidden) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_set_hidden_attribute(p_file, p_hidden);
	}
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__APPLE__)
	const int fd = _jail_open(p_file, Perm::WRITE, O_RDONLY | O_NONBLOCK);
	struct stat st = {};
	if (fd < 0 || ::fstat(fd, &st) != 0) {
		if (fd >= 0) {
			::close(fd);
		}
		return FAILED;
	}
	const int err = ::fchflags(fd, p_hidden ? st.st_flags | UF_HIDDEN : st.st_flags & ~UF_HIDDEN);
	::close(fd);
	return err == 0 ? OK : FAILED;
#else
	return ERR_UNAVAILABLE;
#endif
}

// Read the read-only attribute.
bool FileJailUnix::_get_read_only_attribute(const String &p_file) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_get_read_only_attribute(p_file);
	}
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
	struct stat st = {};
	return _jail_stat(p_file, Perm::READ, st) && (st.st_flags & UF_IMMUTABLE);
#else
	return false;
#endif
}

// Set the read-only attribute.
Error FileJailUnix::_set_read_only_attribute(const String &p_file, bool p_ro) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_set_read_only_attribute(p_file, p_ro);
	}
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
	const int fd = _jail_open(p_file, Perm::WRITE, O_RDONLY | O_NONBLOCK);
	struct stat st = {};
	if (fd < 0 || ::fstat(fd, &st) != 0) {
		if (fd >= 0) {
			::close(fd);
		}
		return FAILED;
	}
	const int err = ::fchflags(fd, p_ro ? st.st_flags | UF_IMMUTABLE : st.st_flags & ~UF_IMMUTABLE);
	::close(fd);
	return err == 0 ? OK : FAILED;
#else
	return ERR_UNAVAILABLE;
#endif
}

// Read one extended attribute.
PackedByteArray FileJailUnix::_get_extended_attribute(const String &p_file, const String &p_name) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_get_extended_attribute(p_file, p_name);
	}
	PackedByteArray out;
#if !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__) && !defined(WEB_ENABLED)
	const int fd = _jail_open(p_file, Perm::READ, O_RDONLY | O_NONBLOCK);
	if (fd < 0 || p_name.is_empty()) {
		if (fd >= 0) {
			::close(fd);
		}
		return out;
	}
#ifdef __APPLE__
	const ssize_t n = ::fgetxattr(fd, p_name.utf8().get_data(), nullptr, 0, 0, 0);
#else
	const CharString name = ("user." + p_name).utf8();
	const ssize_t n = ::fgetxattr(fd, name.get_data(), nullptr, 0);
#endif
	if (n > 0) {
		out.resize(n);
#ifdef __APPLE__
		if (::fgetxattr(fd, p_name.utf8().get_data(), out.ptrw(), out.size(), 0, 0) != n) {
			out.clear();
		}
#else
		if (::fgetxattr(fd, name.get_data(), out.ptrw(), out.size()) != n) {
			out.clear();
		}
#endif
	}
	::close(fd);
#endif
	return out;
}

// Set one extended attribute.
Error FileJailUnix::_set_extended_attribute(const String &p_file, const String &p_name, const PackedByteArray &p_data) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_set_extended_attribute(p_file, p_name, p_data);
	}
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(WEB_ENABLED)
	return ERR_UNAVAILABLE;
#else
	const int fd = _jail_open(p_file, Perm::WRITE, O_RDONLY | O_NONBLOCK);
	if (fd < 0 || p_name.is_empty()) {
		if (fd >= 0) {
			::close(fd);
		}
		return FAILED;
	}
#ifdef __APPLE__
	const int err = ::fsetxattr(fd, p_name.utf8().get_data(), p_data.ptr(), p_data.size(), 0, 0);
#else
	const CharString name = ("user." + p_name).utf8();
	const int err = ::fsetxattr(fd, name.get_data(), p_data.ptr(), p_data.size(), 0);
#endif
	::close(fd);
	return err == 0 ? OK : FAILED;
#endif
}

// Remove one extended attribute.
Error FileJailUnix::_remove_extended_attribute(const String &p_file, const String &p_name) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_remove_extended_attribute(p_file, p_name);
	}
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(WEB_ENABLED)
	return ERR_UNAVAILABLE;
#else
	const int fd = _jail_open(p_file, Perm::WRITE, O_RDONLY | O_NONBLOCK);
	if (fd < 0 || p_name.is_empty()) {
		if (fd >= 0) {
			::close(fd);
		}
		return FAILED;
	}
#ifdef __APPLE__
	const int err = ::fremovexattr(fd, p_name.utf8().get_data(), 0);
#else
	const CharString name = ("user." + p_name).utf8();
	const int err = ::fremovexattr(fd, name.get_data());
#endif
	::close(fd);
	return err == 0 ? OK : FAILED;
#endif
}

// List extended-attribute names.
PackedStringArray FileJailUnix::_get_extended_attributes_list(const String &p_file) {
	if (Perm::is_trusted()) {
		return FileAccessUnix::_get_extended_attributes_list(p_file);
	}
	PackedStringArray out;
#if !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__) && !defined(WEB_ENABLED)
	const int fd = _jail_open(p_file, Perm::READ, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		return out;
	}
#ifdef __APPLE__
	const ssize_t n = ::flistxattr(fd, nullptr, 0, 0);
#else
	const ssize_t n = ::flistxattr(fd, nullptr, 0);
#endif
	if (n > 0) {
		PackedByteArray data;
		data.resize(n);
#ifdef __APPLE__
		const ssize_t got = ::flistxattr(fd, (char *)data.ptrw(), data.size(), 0);
#else
		const ssize_t got = ::flistxattr(fd, (char *)data.ptrw(), data.size());
#endif
		int64_t start = 0;
		for (int64_t i = 0; i < got; i++) {
			if (i == start || data[i] != 0) {
				continue;
			}
			String name = String::utf8((const char *)data.ptr() + start, i - start);
#ifndef __APPLE__
			if (name.begins_with("user.")) {
				name = name.trim_prefix("user.");
			} else {
				start = i + 1;
				continue;
			}
#endif
			out.push_back(name);
			start = i + 1;
		}
	}
	::close(fd);
#endif
	return out;
}

// Reject final symlinks in strict named mounts; otherwise inspect target type.
static bool _dir_stat(const String &p_path, Perm::Kind p_kind, Mount::At &r_at, struct stat &r_st) {
	if (!_jail_at(p_path, p_kind, r_at, false)) {
		return false;
	}
	const bool no_link = _no_link(r_at);
	const int flags = no_link ? AT_SYMLINK_NOFOLLOW : 0;
	return ::fstatat(r_at.fd, r_at.leaf.utf8().get_data(), &r_st, flags) == 0 && (!no_link || !S_ISLNK(r_st.st_mode));
}

// Initialize the current directory to res:// before any access-kind-specific adjustment.
DirJailUnix::DirJailUnix() {
	change_dir("res://");
}

// Enumerate the current directory through a duplicated descriptor, not its path.
Error DirJailUnix::list_dir_begin() {
	SourceError::clear();
	list_dir_end();
	// Keep permission failure distinct from descriptor-allocation failure for caller classification.
	if (!Perm::check(Perm::READ, get_current_dir())) {
		return ERR_UNAUTHORIZED;
	}
	if (current_fd < 0) {
		return ERR_CANT_OPEN;
	}
	const int fd = ::dup(current_fd);
	if (fd < 0) {
		return _errno_kind(errno);
	}
	stream = ::fdopendir(fd);
	if (!stream) {
		const Error why = _errno_kind(errno);
		::close(fd);
		return why;
	}
	return OK;
}

// Read the next name and type from the same directory descriptor.
String DirJailUnix::get_next() {
	if (!stream) {
		return String();
	}
	errno = 0;
	dirent *entry = ::readdir(stream);
	if (!entry) {
		if (errno != 0) {
			_errno_kind(errno);
		}
		list_dir_end();
		return String();
	}
	const String name = fix_unicode_name(entry->d_name);
	struct stat st = {};
	next_dir = ::fstatat(current_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
	next_hidden = is_hidden(name);
	return name;
}

// Report whether the most recent entry is a directory.
bool DirJailUnix::current_is_dir() const {
	return next_dir;
}

// Report whether the most recent entry is hidden.
bool DirJailUnix::current_is_hidden() const {
	return next_hidden;
}

// Close only the enumeration descriptor.
void DirJailUnix::list_dir_end() {
	if (stream) {
		::closedir(stream);
	}
	stream = nullptr;
	next_dir = false;
	next_hidden = false;
}

// Replace the current directory with a safely opened descriptor.
Error DirJailUnix::change_dir(String p_dir) {
	const String path = this->_at(p_dir);
	Mount::At at;
	// Distinguish denial from absence instead of reporting outside directories as missing.
	// A missing parent must likewise not be reported as a permission failure.
	Error why = OK;
	if (!_jail_at(path, Perm::READ, at, false, &why)) {
		return why;
	}
	struct stat st = {};
	if (::fstatat(at.fd, at.leaf.utf8().get_data(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
		return _errno_kind(errno);
	}
	if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
		SourceError::not_dir();
		return FAILED;
	}
	const int flags = _no_link(at) ? O_NOFOLLOW : 0;
	const int fd = ::openat(at.fd, at.leaf.utf8().get_data(), O_RDONLY | O_DIRECTORY | flags | O_CLOEXEC);
	if (fd < 0) {
		return _errno_kind(errno);
	}
	list_dir_end();
	if (current_fd >= 0) {
		::close(current_fd);
	}
	current_fd = fd;
	current_dir = at.full;
	return OK;
}

// Create the final directory beneath a pinned parent.
Error DirJailUnix::make_dir(String p_dir) {
	Mount::At at;
	Error gate = OK;
	if (!_jail_at(this->_at(p_dir), Perm::WRITE, at, false, &gate)) {
		return gate;
	}
	if (::mkdirat(at.fd, at.leaf.utf8().get_data(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == 0) {
		return OK;
	}
	const Error why = _errno_kind(errno);
	return why == FAILED ? ERR_CANT_CREATE : why;
}

// Report existence only when the final component is a regular file.
bool DirJailUnix::file_exists(String p_file) {
	Mount::At at;
	struct stat st = {};
	return _dir_stat(this->_at(p_file), Perm::READ, at, st) && S_ISREG(st.st_mode);
}

// Report existence only when the final component is a directory.
bool DirJailUnix::dir_exists(String p_dir) {
	Mount::At at;
	struct stat st = {};
	return _dir_stat(this->_at(p_dir), Perm::READ, at, st) && S_ISDIR(st.st_mode);
}

// Check read access through the pinned parent.
bool DirJailUnix::is_readable(String p_path) {
	Mount::At at;
	struct stat st = {};
	return _dir_stat(this->_at(p_path), Perm::READ, at, st) &&
			::faccessat(at.fd, at.leaf.utf8().get_data(), R_OK, AT_EACCESS | (_no_link(at) ? AT_SYMLINK_NOFOLLOW : 0)) == 0;
}

// Check write access through the pinned parent.
bool DirJailUnix::is_writable(String p_path) {
	Mount::At at;
	struct stat st = {};
	return _dir_stat(this->_at(p_path), Perm::WRITE, at, st) &&
			::faccessat(at.fd, at.leaf.utf8().get_data(), W_OK, AT_EACCESS | (_no_link(at) ? AT_SYMLINK_NOFOLLOW : 0)) == 0;
}

// Pin source and destination parents before moving the name.
Error DirJailUnix::rename(String p_from, String p_to) {
	SourceError::clear();
	const String from = this->_at(p_from);
	const String to = this->_at(p_to);
	if (!Perm::check(Perm::READ, from) || !Perm::check(Perm::WRITE, from) || !Perm::check(Perm::WRITE, to)) {
		return ERR_UNAUTHORIZED;
	}
	Mount::At src;
	Mount::At dst;
	String why;
	// Preserve the mount's distinction between missing parents and permission denial.
	Error gate = OK;
	if (!Mount::at(from, true, src, why, &gate) || !Mount::at(to, true, dst, why, &gate)) {
		return gate;
	}
	if (::renameat(src.fd, src.leaf.utf8().get_data(), dst.fd, dst.leaf.utf8().get_data()) == 0) {
		return OK;
	}
	return _errno_kind(errno); // Return EXDEV instead of falling back to a non-atomic copy.
}

// Remove the final component through the same parent used to inspect its type.
Error DirJailUnix::remove(String p_path) {
	Mount::At at;
	const String path = this->_at(p_path);
	// Distinguish absence from denial so removing a missing file is not misclassified.
	Error why = OK;
	if (!_jail_at(path, Perm::WRITE, at, true, &why, true)) {
		return why;
	}
	struct stat st = {};
	if (::fstatat(at.fd, at.leaf.utf8().get_data(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
		return _errno_kind(errno);
	}
	const int flags = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) ? AT_REMOVEDIR : 0;
	if (::unlinkat(at.fd, at.leaf.utf8().get_data(), flags) != 0) {
		return _errno_kind(errno);
	}
	if (remove_notification_func) {
		remove_notification_func(at.full);
	}
	return OK;
}

// Inspect the final entry without following a symlink in any authorized mount.
bool DirJailUnix::is_link(String p_file) {
	Mount::At at;
	struct stat st = {};
	return _jail_at(this->_at(p_file), Perm::READ, at, false, nullptr, true) &&
			::fstatat(at.fd, at.leaf.utf8().get_data(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode);
}

// Read the final symlink target for ordinary execution and machine-root access.
String DirJailUnix::read_link(String p_file) {
	Mount::At at;
	if (!_jail_at(this->_at(p_file), Perm::READ, at, false) || _no_link(at)) {
		return String();
	}
	char data[PATH_MAX]; // OS-sized storage for a symlink target.
	const ssize_t size = ::readlinkat(at.fd, at.leaf.utf8().get_data(), data, sizeof(data));
	String out;
	return size > 0 && size < (ssize_t)sizeof(data) && out.append_utf8(data, size) == OK ? out : String();
}

// Create symlinks only for ordinary absolute paths, not inside named mounts.
Error DirJailUnix::create_link(String p_source, String p_target) {
	Mount::At at;
	if (Mount::strict_mode() || !p_target.begins_with("/") || !_jail_at(p_target, Perm::WRITE, at)) {
		ERR_PRINT("PermissionDenied: links are not usable inside mounts.");
		return ERR_UNAUTHORIZED;
	}
	return ::symlinkat(p_source.utf8().get_data(), at.fd, at.leaf.utf8().get_data()) == 0 ? OK : FAILED;
}

// Report whether the target filesystem distinguishes filename case.
bool DirJailUnix::is_case_sensitive(const String &p_path) const {
	Mount::At at;
	struct stat st = {};
	if (!_dir_stat(this->_at(p_path), Perm::READ, at, st)) {
		return false;
	}
	const int fd = ::openat(at.fd, at.leaf.utf8().get_data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
#ifdef MACOS_ENABLED
	// Read volume-specific macOS attributes through the target directory descriptor.
	const bool sensitive = ::fpathconf(fd, _PC_CASE_SENSITIVE) == 1;
#elif defined(LINUXBSD_ENABLED)
	// Read Linux casefold from the target directory because it is a per-directory flag.
	long flags = 0;
	const bool sensitive = ::ioctl(fd, _IOR('f', 1, long), &flags) < 0 || !(flags & 0x40000000 /* FS_CASEFOLD_FL */);
#else
	const bool sensitive = true;
#endif
	::close(fd);
	return sensitive;
}

// Compare file identity through two pinned parents.
bool DirJailUnix::is_equivalent(const String &p_a, const String &p_b) const {
	Mount::At a;
	Mount::At b;
	struct stat sa = {};
	struct stat sb = {};
	return _dir_stat(this->_at(p_a), Perm::READ, a, sa) &&
			_dir_stat(this->_at(p_b), Perm::READ, b, sb) &&
			sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// Read device and inode through the pinned parent without resolving an outside path.
String DirJailUnix::get_identity(const String &p_path) const {
	Mount::At at;
	struct stat st = {};
	if (!_dir_stat(this->_at(p_path), Perm::READ, at, st)) {
		return String();
	}
	return uitos((uint64_t)st.st_dev) + ":" + uitos((uint64_t)st.st_ino);
}

// Query free space through the retained current-directory descriptor.
uint64_t DirJailUnix::get_space_left() {
	struct statvfs vfs = {};
	if (current_fd < 0 || ::fstatvfs(current_fd, &vfs) != 0) {
		return 0;
	}
	return uint64_t(vfs.f_bavail) * uint64_t(vfs.f_frsize);
}

// Return unknown when platform-specific filesystem naming requires an unvalidated path.
String DirJailUnix::get_filesystem_type() const {
	return String();
}

// Close retained enumeration and current-directory descriptors.
DirJailUnix::~DirJailUnix() {
	list_dir_end();
	if (current_fd >= 0) {
		::close(current_fd);
	}
}

#endif
