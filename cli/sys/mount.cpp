/**************************************************************************/
/*  mount.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement filesystem mount isolation declared in mount.h.

#include "cli/sys/mount.h"
#include "cli/sys/perm.h"
#include "cli/sys/pkgscope.h"
#include "cli/sys/system.h"
#include "cli/sys/assets.h"

#include "cli/sys/source_error.h"

#include "core/os/os.h"

#ifdef UNIX_ENABLED
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(WINDOWS_ENABLED)
#include "cli/sys/win_path.h"
#include <windows.h>
#endif

HashMap<String, Mount::Box> Mount::boxes;
HashMap<String, String> Mount::pkgs;
bool Mount::strict = false;
bool Mount::pkg_mode = false;

// Internal machine-root mount for absolute paths.
// Its name fails name_ok so scripts cannot select it as a public scheme.
const char *Mount::ROOT = "/";

#ifdef UNIX_ENABLED
namespace {
constexpr int SYMLINK_MAX = 255; // Maximum symbolic links followed during path resolution.
constexpr int SYMLINK_SIZE_MAX = PATH_MAX; // OS-representable symlink-target byte boundary.
} // namespace

// Pin existing internal storage without following symlinks and verify ownership and permissions.
static bool hold_own_dir(const String &p_path) {
	const int fd = ::open(p_path.utf8().get_data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		ERR_PRINT(vformat("\"%s\" is in the way. Remove it; gd needs a plain directory there.", p_path));
		return false;
	}
	struct stat st = {};
	const bool owned = ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::geteuid();
	if (!owned) {
		::close(fd);
		ERR_PRINT(vformat("\"%s\" is not an owned directory.", p_path));
		return false;
	}
	// Secure the opened directory itself without re-resolving a replaceable path.
	const bool closed = ::fchmod(fd, 0700) == 0;
	::close(fd);
	if (!closed) {
		ERR_PRINT(vformat("cannot close \"%s\" to others.", p_path));
	}
	return closed;
}
#endif

Mount::At::~At() {
#ifdef UNIX_ENABLED
	if (fd >= 0) {
		::close(fd);
	}
#endif
}

// Scheme names reserved by runtime and network APIs.
bool Mount::_reserved(const String &p_name) {
	static const char *taken[] = { "res", "user", "uid", "pipe", "local", "libgodot",
		"tcp", "unix", "http", "https", "file", "data", "cache", "pkg", nullptr };
	for (int i = 0; taken[i]; i++) {
		if (p_name == taken[i]) {
			return true;
		}
	}
	return false;
}

// Accept only lowercase alphanumerics and hyphens as mount names.
bool Mount::name_ok(const String &p_name) {
	if (p_name.is_empty()) {
		return false;
	}
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
		if (!ok) {
			return false;
		}
	}
	return true;
}

// Parse --mount name=path:mode at the first equals sign and last colon.
// The middle remains the path, including drive-letter colons such as c:/var/log.
Mount::Parse Mount::parse_flag(const String &p_arg) {
	if (p_arg != "--mount" && !p_arg.begins_with("--mount=")) {
		return NOT_MINE;
	}
#ifdef WINDOWS_ENABLED
	// Reject Windows named mounts until handle-relative operations cover their isolation requirements.
	ERR_PRINT("--mount is not supported on Windows.");
	return BAD;
#endif
	const String body = p_arg.trim_prefix("--mount=");
	if (body == "--mount") {
		ERR_PRINT("--mount needs a value. Write it as --mount=name=/path:rw.");
		return BAD;
	}

	const int eq = body.find_char('=');
	if (eq <= 0) {
		ERR_PRINT(vformat("Bad mount \"%s\". Write it as name=/path:rw.", body));
		return BAD;
	}
	const String name = body.substr(0, eq);
	const String rest = body.substr(eq + 1);
	const int colon = rest.rfind_char(':');
	if (colon <= 0) {
		ERR_PRINT(vformat("Mount \"%s\" has no access mode. End it with :r or :rw.", name));
		return BAD;
	}
	const String mode = rest.substr(colon + 1);
	const String path = rest.substr(0, colon);

	if (!name_ok(name)) {
		ERR_PRINT(vformat("Bad mount name \"%s\". Use lowercase letters, digits and \"-\".", name));
		return BAD;
	}
	if (_reserved(name)) {
		ERR_PRINT(vformat("Mount name \"%s\" is reserved.", name));
		return BAD;
	}
	if (boxes.has(name)) {
		ERR_PRINT(vformat("Mount name \"%s\" is already used. One name, one place.", name));
		return BAD;
	}
	if (mode != "r" && mode != "rw") {
		ERR_PRINT(vformat("Mount \"%s\" has a bad mode \"%s\". Use r or rw.", name, mode));
		return BAD;
	}
	if (path.is_empty()) {
		ERR_PRINT(vformat("Mount \"%s\" has no path.", name));
		return BAD;
	}

	Box box;
	box.root = _root_of(path);
	box.can_write = (mode == "rw");
	boxes[name] = box;
	return TAKEN;
}

// Resolve filesystem targets, including symbolic links invisible to lexical normalization.
// Return the canonical existing prefix followed by any not-yet-created suffix.
// Return empty on uncertainty; callers treat that as outside the mount.
//
// Handle three path forms.
// Fully existing paths resolve directly through realpath.
// Missing suffixes are joined lexically after resolving the existing prefix.
// For dangling symlinks, realpath reports absence, so readlink obtains the target.
// Resolve that target again before applying containment checks.
String Mount::_solid(const String &p_path, int p_hops) {
#ifdef UNIX_ENABLED
	String head = p_path;
	String rest;
	while (!head.is_empty()) {
		char *got = ::realpath(head.utf8().get_data(), nullptr);
		if (got) {
			String out;
			const Error ok = out.append_utf8(got);
			::free(got);
			if (ok != OK) {
				return String();
			}
			return rest.is_empty() ? out : out.path_join(rest);
		}
		if (errno != ENOENT) {
			return String(); // Fail closed on inaccessible, cyclic, overlong, or otherwise unresolved paths.
		}

		// A dangling symlink still exists itself; read its target and resolve again.
		char buf[SYMLINK_SIZE_MAX]; // Storage for one symlink target.
		const ssize_t n = ::readlink(head.utf8().get_data(), buf, sizeof(buf));
		if (n > 0) {
			// Bound symlink traversal to terminate cycles, rejecting unresolved paths at exhaustion.
			if (p_hops >= SYMLINK_MAX || (size_t)n >= sizeof(buf)) {
				return String();
			}
			buf[n] = 0;
			String to;
			if (to.append_utf8(buf) != OK) {
				return String();
			}
			if (!to.is_absolute_path()) {
				to = head.get_base_dir().path_join(to);
			}
			head = rest.is_empty() ? to : to.path_join(rest);
			rest = String();
			p_hops++;
			continue;
		}

		const String base = head.get_file();
		if (base.is_empty()) {
			break; // Reached the root.
		}
		rest = rest.is_empty() ? base : base.path_join(rest);
		head = head.get_base_dir();
	}
	return String();
#elif defined(WINDOWS_ENABLED)
	String head = p_path;
	String rest;
	while (!head.is_empty()) {
		const String native = WinPath::native(head);
		const HANDLE file = ::CreateFileW((LPCWSTR)native.utf16().get_data(), FILE_READ_ATTRIBUTES,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			const String out = WinPath::final(file);
			::CloseHandle(file);
			if (out.is_empty()) {
				return String();
			}
			return rest.is_empty() ? out : out.path_join(rest);
		}

		const DWORD err = ::GetLastError();
		if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
			return String();
		}
		const DWORD attr = ::GetFileAttributesW((LPCWSTR)native.utf16().get_data());
		if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
			return String();
		}
		const String base = head.get_file();
		if (base.is_empty()) {
			break;
		}
		rest = rest.is_empty() ? base : base.path_join(rest);
		head = head.get_base_dir();
	}
	return String();
#else
	return p_path;
#endif
}

// Normalize mount roots to absolute resolved paths without trailing slashes.
// Resolve roots too so equivalent native paths are not incorrectly rejected as outside.
String Mount::_root_of(const String &p_path) {
	const String abs = p_path.is_absolute_path() ? p_path : GDSystem::cwd().path_join(p_path);
	const String flat = abs.simplify_path();
	const String solid = _solid(flat);
	// Keep an unresolved root's lexical spelling rather than deleting the mount with an empty root.
	// Each access still passes through resolve and _solid, preserving the boundary.
	const String out = (solid.is_empty() ? flat : solid).trim_suffix("/");
	return out.is_empty() ? "/" : out; // Retain the machine root after trailing-slash removal.
}

// Prepare internal storage, verifying existing paths are user-owned directories.
//
// The user directory is under a shared temporary root with a predictable working-directory-derived name.
// Another user could precreate a symlink or foreign directory at that path.
// Using it unchecked would redirect script output into the substituted location.
bool Mount::_own_dir(const String &p_path) {
#ifdef UNIX_ENABLED
	struct stat st = {};
	// Inspect without following links; checking only their targets could accept an attacker-created link.
	if (::lstat(p_path.utf8().get_data(), &st) == 0) {
		return hold_own_dir(p_path);
	}
	if (errno != ENOENT) {
		ERR_PRINT(vformat("cannot look at \"%s\".", p_path));
		return false;
	}
	// Create parents normally, but create the final directory with mode 0700.
	// Creating it broadly accessible and tightening later would expose an access window.
	const String up = p_path.get_base_dir();
	if (!up.is_empty() && !GDSystem::make_dirs(up)) {
		ERR_PRINT(vformat("cannot make \"%s\".", up));
		return false;
	}
	if (::mkdir(p_path.utf8().get_data(), 0700) != 0) {
		// Apply the same checks if another process of this user created the directory concurrently.
		if (errno == EEXIST) {
			return hold_own_dir(p_path);
		}
		ERR_PRINT(vformat("cannot make \"%s\".", p_path));
		return false;
	}
	return hold_own_dir(p_path);
#else
	// Windows temporary storage is already beneath the user's own TEMP directory.
	return GDSystem::make_dirs(p_path);
#endif
}

// Install one mount, verifying internal storage first when p_own is true.
// Return true without an ownership check when p_own is false.
// Check the original path before _root_of resolves symbolic links.
// Otherwise a link pointing to a user-owned directory could be accepted.
bool Mount::_add(const char *p_name, const String &p_raw, bool p_write, bool p_own) {
	if (p_own && !_own_dir(p_raw)) {
		return false;
	}
	Box box;
	box.root = _root_of(p_raw);
	box.can_write = p_write;
	box.own = p_own;
	boxes[p_name] = box;
	return true;
}

// Pin the mount root by descriptor so later replacement cannot change the operation base.
bool Mount::_open_roots() {
#ifdef UNIX_ENABLED
	for (KeyValue<String, Box> &kv : boxes) {
		Box &box = kv.value;
		box.fd = ::open(box.root.utf8().get_data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (box.fd < 0 && errno != ENOENT) {
			ERR_PRINT(vformat("cannot hold mount \"%s://\" at \"%s\".", kv.key, box.root));
			return false;
		}
		struct stat st = {};
		if (box.fd >= 0 && box.own && (::fstat(box.fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid())) {
			::close(box.fd);
			box.fd = -1;
			ERR_PRINT(vformat("mount \"%s://\" changed while it was being held.", kv.key));
			return false;
		}
	}
#endif
	return true;
}

// Install built-in mounts, preparing internal storage but leaving caller-supplied roots untouched.
// Do not create a missing caller-supplied root at an unintended location.
bool Mount::setup() {
	GDAssets::init(); // Retain embedded file storage before scripts can replace the executable.
	// Use the current directory, read-only in strict mode.
	// Package commands still write gd.json, gd.lock, and pkg/ there on the operator's behalf.
	_add("res", GDSystem::cwd(), !strict || pkg_mode, false);

	// Expose the host filesystem only when the caller has not requested isolation.
#ifdef UNIX_ENABLED
	if (!strict) {
		_add(ROOT, "/", true, false);
	}
#endif

	// Prepare isolated temporary storage and stop startup if it has been taken over.
	if (!_add("user", GDSystem::user_dir(), true, true)) {
		return false;
	}

	// Provide internal package storage without requiring an explicit mount.
	// Use shared storage for installed dependencies.
	//
	// Permit writes only during package commands because the storage is shared per user.
	// Unrestricted script writes could replace dependencies loaded by another project.
	// Read-only use cannot redirect writes, so ownership checks are unnecessary in that mode.
	const String cache = GDSystem::cache_dir();
	if (cache.is_empty()) {
		ERR_PRINT("Cannot locate per-user package storage; set GD_CACHE_HOME.");
		return false;
	}
	// Provide a read mount on first use, creating private storage if necessary.
	const bool make_cache = !GDSystem::is_dir(cache);
	if (!_add("cache", cache, pkg_mode, pkg_mode || make_cache)) {
		return false;
	}
	return _open_roots();
}

// Point the private local:// mount at one package checkout.
// Package commands repoint it between sequential reads; ordinary scripts cannot create it.
String Mount::local(const String &p_path) {
	if (!pkg_mode || p_path.is_empty()) {
		return String();
	}
	Box *old = boxes.getptr("local");
#ifdef UNIX_ENABLED
	if (old && old->fd >= 0) {
		::close(old->fd);
	}
#endif
	Box box;
	box.root = _root_of(p_path);
#ifdef UNIX_ENABLED
	box.fd = ::open(box.root.utf8().get_data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#endif
	boxes["local"] = box;
	return "local://";
}

// Extract a leading scheme only when its spelling is valid.
String Mount::scheme_of(const String &p_path, String &r_tail) {
	const int at = p_path.find("://");
	if (at <= 0 || !name_ok(p_path.substr(0, at))) {
		r_tail = p_path;
		return String();
	}
	r_tail = p_path.substr(at + 3);
	return p_path.substr(0, at);
}

// Check normalized containment, including the root itself.
// Compare path components rather than a raw textual prefix.
// This prevents /srv/data from matching /srv/data2; avoid adding a separator to the machine root.
// Use the same comparison for permission scopes such as --allow-run.
bool Mount::inside(const String &p_root, const String &p_full) {
	const String edge = p_root.ends_with("/") ? p_root : p_root + "/";
	return p_full == p_root || p_full.begins_with(edge);
}

// Compare resolved paths so extra slashes or dot components cannot disguise a mount root.
bool Mount::is_root(const String &p_path) {
	String tail;
	String why;
	if (_pkg(p_path, tail, why)) {
		return !tail.is_empty() && is_root(tail);
	}
	String name = scheme_of(p_path, tail);
	if (name.is_empty()) {
		if (p_path.is_absolute_path()) {
			return false;
		}
		name = "res";
	}
	const Box *box = boxes.getptr(name);
	return box && _under(name, tail) == box->root;
}

// Return the owning mount using resolve's interpretation, defaulting unschemed paths to res://.
// Absolute paths belong to the machine-root mount without a public scheme.
// Return empty so underlying operations retain their absolute-path form.
String Mount::name_of(const String &p_path) {
	String tail;
	String why;
	if (_pkg(p_path, tail, why)) {
		return tail.is_empty() ? String() : name_of(tail);
	}
	const String name = scheme_of(p_path, tail);
	if (name.is_empty()) {
		return p_path.is_absolute_path() ? String() : "res";
	}
	return boxes.has(name) ? name : String();
}

// Share root joining and normalization between resolve and real.
// Joining a leading slash to the machine root would produce a network-style double slash.
// Remove the leading separator before joining.
String Mount::_under(const String &p_name, const String &p_tail) {
	const String root = boxes[p_name].root;
	const String tail = root.ends_with("/") ? p_tail.trim_prefix("/") : p_tail;
	return root.path_join(tail).simplify_path();
}

// Rewrite pkg://alias/rest onto the root registered for the alias.
// Return false for other paths; return true with an empty result for an unknown alias.
bool Mount::_pkg(const String &p_path, String &r_path, String &r_why) {
	String tail;
	if (scheme_of(p_path, tail) != "pkg") {
		return false;
	}
	// An alias is one segment; a canonical @scope/name@version id spans two.
	String rest;
	const String key = PkgScope::key_of(PkgScope::canonical(p_path), &rest);
	const String *root = pkgs.getptr(key);
	if (!root) {
		r_why = PkgScope::is_id(key)
				? vformat("package %s is not installed. Run gd install.", key)
				: vformat("no package named \"%s\" in gd.json. Add it with gd add.", key);
		r_path = String();
		return true;
	}
	r_path = rest.is_empty() ? *root : root->path_join(rest);
	return true;
}

// Resolve a known package alias without rejecting unrelated or missing paths.
String Mount::unalias(const String &p_path) {
	String out;
	String why;
	return _pkg(p_path, out, why) && !out.is_empty() ? out : p_path;
}

// Resolve one public path through its mount while enforcing access mode and containment.
bool Mount::_path(const String &p_path, bool p_write, String &r_name, String &r_tail, String &r_full, String &r_why) {
	String tail;
	// Resolve package aliases first; their files are never writable from a script.
	if (_pkg(p_path, tail, r_why)) {
		if (tail.is_empty()) {
			return false;
		}
		if (p_write) {
			r_why = vformat("cannot write \"%s\": pkg:// is read-only.", p_path);
			return false;
		}
		return _path(tail, false, r_name, r_tail, r_full, r_why);
	}
	String name = scheme_of(p_path, tail);

	if (name.is_empty()) {
		name = p_path.is_absolute_path() ? ROOT : "res";
		// Route in-project absolute paths through the same pinned boundary as relative paths.
		// Reject outside paths before inspecting their existence or following any links.
#ifdef UNIX_ENABLED
		if (strict && name == ROOT) {
			const Box *res = boxes.getptr("res");
			const String full = p_path.simplify_path();
			if (!res || !inside(res->root, full)) {
				r_why = vformat("\"%s\" is outside the startup directory under --strict. Use an explicit named mount.", p_path);
				return false;
			}
			name = "res";
			tail = full.trim_prefix(res->root).trim_prefix("/");
		}
#endif
	}

	const Box *box = boxes.getptr(name);
	if (!box) {
		// Only Windows lacks the machine-root mount because its volumes have separate roots.
		if (name == ROOT) {
			r_why = vformat("absolute paths are not usable on Windows: \"%s\". "
							"Put the file under res:// or user://.",
					p_path);
			return false;
		}
		r_why = vformat("there is no mount named \"%s://\". Pass --mount=%s=/some/path:rw to add it.", name, name);
		return false;
	}
	if (p_write && !box->can_write) {
		// Display machine-root paths directly because that mount has no public scheme.
		r_why = name == ROOT
				? vformat("cannot write \"%s\" under --strict.", p_path)
				: vformat("\"%s://\" is mounted read-only.", name);
		return false;
	}

	// Reject normalized paths that escape the mount root.
	String full = _under(name, tail);
	if (!inside(box->root, full)) {
		r_why = name == ROOT
				? vformat("\"%s\" leaves the filesystem root.", p_path)
				: vformat("\"%s\" leaves the \"%s://\" mount.", p_path, name);
		return false;
	}

	r_name = name;
	// Make the path root-relative without adding an extra separator to the machine root.
	r_tail = full.trim_prefix(box->root).trim_prefix("/");
	r_full = full;
	return true;
}

// Traverse from pinned descriptors, rejecting intermediate symlinks and exposing final links only for removal.
bool Mount::at(const String &p_path, bool p_write, At &r_at, String &r_why, Error *r_err, bool p_leaf_link) {
	SourceError::clear();
	// Preserve the failure at its source instead of guessing it again in the caller.
	const auto refuse = [&](Error p_kind) {
		if (r_err) {
			*r_err = p_kind;
		}
		return false;
	};
#ifdef UNIX_ENABLED
	if (r_err) {
		*r_err = OK;
	}
	String name;
	String tail;
	String full;
	// Honor thread-local internal authority without registering a script-visible host mount.
	const bool native = Perm::is_trusted() && p_path.begins_with("/");
	if (native) {
		full = p_path.simplify_path();
	} else if (!_path(p_path, p_write, name, tail, full, r_why)) {
		return refuse(ERR_UNAUTHORIZED); // Outside the mount or lacking permission.
	}
	// Let the kernel resolve an unrestricted parent while retaining the final operation name.
	if (native || boxes[name].root == ROOT) {
		const String leaf = full.get_file();
		const String parent = full.get_base_dir();
		int fd;
		do { fd = ::open(parent.utf8().get_data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
		if (fd < 0) {
			const int code = errno;
			SourceError::posix(code);
			r_why = vformat("cannot open the parent directory of \"%s\".", p_path);
			const Error error = SourceError::posix_error(code);
			return refuse(error == FAILED ? ERR_CANT_OPEN : error);
		}
		r_at.fd = fd;
		r_at.leaf = leaf.is_empty() ? "." : leaf;
		r_at.full = leaf.is_empty() ? parent : parent.path_join(leaf);
		r_at.root = true;
		return true;
	}
	const Box &box = boxes[name];
	if (box.fd < 0) {
		r_why = vformat("the \"%s://\" mount does not exist.", name);
		return refuse(ERR_UNAUTHORIZED);
	}

	// Duplicate the mount descriptor without exposing an inheritance window to concurrent forks.
	int fd;
	do { fd = ::fcntl(box.fd, F_DUPFD_CLOEXEC, 0); } while (fd < 0 && errno == EINTR);
	if (fd < 0) {
		SourceError::posix(errno);
		r_why = vformat("cannot hold the \"%s://\" mount.", name);
		return refuse(errno == EMFILE || errno == ENFILE ? ERR_OUT_OF_MEMORY : ERR_CANT_OPEN);
	}
	const Vector<String> bits = tail.split("/", false);
	for (int i = 0; i + 1 < bits.size(); i++) {
		const int next = ::openat(fd, bits[i].utf8().get_data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		const int why = errno;
		if (next < 0) {
			SourceError::posix(why);
			// Reject intermediate symlinks because they may point outside the mount.
			// Since O_NOFOLLOW failures vary between ELOOP and ENOTDIR, inspect the link directly.
			// Report a missing intermediate directory as absence rather than permission denial.
			struct stat mid = {};
			const bool link = ::fstatat(fd, bits[i].utf8().get_data(), &mid, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(mid.st_mode);
			::close(fd);
			r_why = vformat("\"%s\" %s inside the \"%s://\" mount.", p_path, link ? "uses a link" : "has an unusable directory", name);
			if (link) {
				return refuse(ERR_UNAUTHORIZED);
			}
			if (why == ENOENT) {
				return refuse(ERR_FILE_NOT_FOUND);
			}
			if (why == EACCES || why == EPERM) {
				return refuse(ERR_FILE_NO_PERMISSION);
			}
			return refuse(why == EMFILE || why == ENFILE ? ERR_OUT_OF_MEMORY : ERR_CANT_OPEN);
		}
		::close(fd);
		fd = next;
	}

	const String leaf = bits.is_empty() ? "." : bits[bits.size() - 1];
	struct stat st = {};
	const int found = ::fstatat(fd, leaf.utf8().get_data(), &st, AT_SYMLINK_NOFOLLOW);
	if (found == 0 && S_ISLNK(st.st_mode) && !p_leaf_link) {
		::close(fd);
		r_why = vformat("\"%s\" uses a link inside the \"%s://\" mount.", p_path, name);
		return refuse(ERR_UNAUTHORIZED); // Do not traverse a link that could escape the mount.
	}
	if (found != 0 && errno != ENOENT) {
		const int why = errno;
		SourceError::posix(why);
		::close(fd);
		r_why = vformat("cannot inspect \"%s\" inside the \"%s://\" mount.", p_path, name);
		return refuse(why == EACCES || why == EPERM ? ERR_FILE_NO_PERMISSION : ERR_CANT_OPEN);
	}
	r_at.fd = fd;
	r_at.leaf = leaf;
	r_at.full = full;
	return true;
#else
	return refuse(ERR_UNAVAILABLE);
#endif
}

// Resolve script paths to accessible native targets, returning empty and a reason on denial.
String Mount::resolve(const String &p_path, bool p_write, String &r_why) {
	String name;
	String tail;
	String full;
	if (!_path(p_path, p_write, name, tail, full, r_why)) {
		return String();
	}
#ifdef UNIX_ENABLED
	// Permission-only checks allow creating missing parents.
	// Actual operations traverse descriptors to enforce symlink and race boundaries.
	return full;
#else
	const Box &box = boxes[name];
	// Resolve the native target and check again because an in-mount symlink may lead outside.
	// A path's textual containment alone does not constrain where the OS follows it.
	//
	// Check the normalized path form also used by FileAccess::fix_path.
	// Passing that form to real keeps checked and opened paths consistent.
	const String solid = _solid(full);
	if (solid.is_empty() || !inside(box.root, solid)) {
		r_why = vformat("\"%s\" points outside the \"%s://\" mount.", p_path, name);
		return String();
	}
	return WinPath::native(full);
#endif
}

// Expand only the scheme immediately before native opening, without checking permissions.
// Return empty for unschemed paths because relative-path bases depend on the caller.
// Directory operations use their current directory; file operations use res://.
String Mount::real(const String &p_path) {
	String tail;
	const String name = scheme_of(p_path, tail);
	if (name.is_empty()) {
		return String();
	}
	if (!boxes.has(name)) {
		return String();
	}
	return _under(name, tail);
}
