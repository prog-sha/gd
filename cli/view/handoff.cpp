/**************************************************************************/
/*  handoff.cpp                                                           */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Delegate package commands from the display executable to its headless runtime.

#include "cli/view/handoff.h"

#ifdef GD_VIEW
#include "cli/main/spec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {

const int PATH_CAP = 16384; // Room for an absolute executable path, including long directories.

// Report whether the name carries a directory.
bool has_sep(const char *path) {
	return strchr(path, '/') != nullptr
#ifdef _WIN32
			|| strchr(path, '\\') != nullptr
#endif
			;
}

#ifdef _WIN32

// Convert a UTF-8 path so wide file calls keep characters outside the ANSI page.
wchar_t *to_wide(const char *text) {
	int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (count <= 0) {
		return nullptr;
	}
	wchar_t *out = new wchar_t[count];
	MultiByteToWideChar(CP_UTF8, 0, text, -1, out, count);
	return out;
}

// Store a wide path as UTF-8 for the shared sibling builder.
bool to_utf8(const wchar_t *text, char *out, size_t out_n) {
	return WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)out_n, nullptr, nullptr) > 0;
}

// Resolve a UTF-8 path without interpreting it as the ANSI code page.
bool absolute_utf8(const char *src, char *out, size_t out_n) {
	wchar_t *wide = to_wide(src);
	if (!wide) {
		return false;
	}
	wchar_t resolved[PATH_CAP];
	wchar_t *got = _wfullpath(resolved, wide, PATH_CAP);
	delete[] wide;
	return got && to_utf8(got, out, out_n);
}

#else

// Resolve a path to an absolute directory entry.
bool absolute_utf8(const char *src, char *out, size_t out_n) {
	char resolved[PATH_CAP];
	if (!realpath(src, resolved)) {
		return false;
	}
	size_t len = strlen(resolved);
	if (len + 1 > out_n) {
		return false;
	}
	memcpy(out, resolved, len + 1);
	return true;
}

#endif

// Read the path of the executable that is already running.
bool running_path(char *out, size_t out_n) {
#ifdef _WIN32
	wchar_t wide[PATH_CAP];
	DWORD count = GetModuleFileNameW(nullptr, wide, PATH_CAP);
	if (count == 0 || count >= PATH_CAP) {
		return false;
	}
	return to_utf8(wide, out, out_n);
#elif defined(__APPLE__)
	char raw[PATH_CAP];
	uint32_t count = sizeof(raw);
	if (_NSGetExecutablePath(raw, &count) != 0) {
		return false;
	}
	return absolute_utf8(raw, out, out_n);
#else
	char raw[PATH_CAP];
	ssize_t count = readlink("/proc/self/exe", raw, sizeof(raw) - 1);
	if (count <= 0 || count >= (ssize_t)sizeof(raw) - 1) {
		return false;
	}
	raw[count] = '\0';
	return absolute_utf8(raw, out, out_n);
#endif
}

// Find a bare executable name in PATH and return its absolute path.
bool lookup_path(const char *name, char *out, size_t out_n) {
	if (has_sep(name)) {
		return false;
	}
#ifdef _WIN32
	DWORD need = GetEnvironmentVariableW(L"PATH", nullptr, 0);
	if (need == 0) {
		return false;
	}
	wchar_t *envp = new wchar_t[need];
	if (GetEnvironmentVariableW(L"PATH", envp, need) == 0) {
		delete[] envp;
		return false;
	}
	wchar_t *wname = to_wide(name);
	if (!wname) {
		delete[] envp;
		return false;
	}
	bool found = false;
	const wchar_t *suffixes[] = { L"", L".exe" }; // Bare names are launched with the executable suffix.
	int suffix_count = wcschr(wname, L'.') ? 1 : 2;
	wchar_t *at = envp;
	while (*at && !found) {
		wchar_t *end = wcschr(at, L';');
		if (end) {
			*end = L'\0';
		}
		for (int suffix = 0; suffix < suffix_count && !found; suffix++) {
			wchar_t candidate[PATH_CAP];
			if (at[0] == L'\0') {
				_snwprintf(candidate, PATH_CAP, L"%s%s", wname, suffixes[suffix]);
			} else {
				_snwprintf(candidate, PATH_CAP, L"%s\\%s%s", at, wname, suffixes[suffix]);
			}
			candidate[PATH_CAP - 1] = L'\0';
			if (_waccess(candidate, 0) == 0) {
				wchar_t resolved[PATH_CAP];
				if (_wfullpath(resolved, candidate, PATH_CAP) && to_utf8(resolved, out, out_n)) {
					found = true;
				}
			}
		}
		if (!end) {
			break;
		}
		at = end + 1;
	}
	delete[] wname;
	delete[] envp;
	return found;
#else
	const char *envp = getenv("PATH");
	if (!envp) {
		return false;
	}
	const char *at = envp;
	while (*at) {
		const char *end = strchr(at, ':');
		size_t len = end ? (size_t)(end - at) : strlen(at);
		char candidate[PATH_CAP];
		int wrote = len == 0
				? snprintf(candidate, sizeof(candidate), "%s", name)
				: snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)len, at, name);
		if (wrote > 0 && (size_t)wrote < sizeof(candidate) && access(candidate, X_OK) == 0 && absolute_utf8(candidate, out, out_n)) {
			return true;
		}
		if (!end) {
			break;
		}
		at = end + 1;
	}
	return false;
#endif
}

// Resolve this executable before deriving its display sibling.
bool self_path(const char *argv0, char *out, size_t out_n) {
	if (running_path(out, out_n)) {
		return true;
	}
	if (argv0 && has_sep(argv0) && absolute_utf8(argv0, out, out_n)) {
		return true;
	}
	return argv0 && lookup_path(argv0, out, out_n);
}

// Report whether the display executable can be started.
bool can_start(const char *path) {
#ifdef _WIN32
	wchar_t *wide = to_wide(path);
	if (!wide) {
		return false;
	}
	bool ok = _waccess(wide, 0) == 0;
	delete[] wide;
	return ok;
#else
	return access(path, X_OK) == 0;
#endif
}

// Replace this process. Return only when the replacement fails.
int replace_with(const char *path, char **argv) {
#ifdef _WIN32
	wchar_t *wpath = to_wide(path);
	if (!wpath) {
		return -1;
	}
	int argc = 0;
	while (argv[argc]) {
		argc++;
	}
	wchar_t **wargv = new wchar_t *[argc + 1];
	for (int i = 0; i < argc; i++) {
		wargv[i] = to_wide(argv[i]);
		if (!wargv[i]) {
			for (int j = 0; j < i; j++) {
				delete[] wargv[j];
			}
			delete[] wargv;
			delete[] wpath;
			return -1;
		}
	}
	wargv[argc] = nullptr;
	_wexecv(wpath, wargv);
	for (int i = 0; i < argc; i++) {
		delete[] wargv[i];
	}
	delete[] wargv;
	delete[] wpath;
	return -1;
#else
	execv(path, argv);
	return -1;
#endif
}

// Report whether the word is a package command handled before display startup.
bool pkg_word(const char *arg) {
	const CLI::Command *entry = CLI::command(arg);
	return entry && (entry->kind & CLI::VIEW);
}

// Build the headless path by dropping the display suffix from the executable name.
bool headless_path(const char *self, char *out, size_t out_n) {
	const char *slash = strrchr(self, '/');
#ifdef _WIN32
	const char *back = strrchr(self, '\\');
	if (back && (!slash || back > slash)) {
		slash = back;
	}
#endif
	const char *name = slash ? slash + 1 : self;
	if (strncmp(name, "gd-godot", 8) != 0) {
		return false;
	}
	const char sep[] = { slash ? *slash : '/', '\0' };
	int wrote = slash
			? snprintf(out, out_n, "%.*s%sgd%s", (int)(slash - self), self, sep, name + 8)
			: snprintf(out, out_n, "gd%s", name + 8);
	return wrote > 0 && (size_t)wrote < out_n;
}

// Move into the project directory named by a display-style path option.
bool enter_project(const char *path) {
#ifdef _WIN32
	wchar_t *wide = to_wide(path);
	if (!wide) {
		return false;
	}
	bool ok = _wchdir(wide) == 0;
	delete[] wide;
	return ok;
#else
	return chdir(path) == 0;
#endif
}

// Run a package command in the headless executable, placing copies under addons/.
// Return 0 when this process should continue into display startup.
int project_command(int argc, char **argv) {
	bool display = false;
	bool pkg = false;
	bool named = false; // Only the first positional word can select a command.
	const char *project = nullptr;
	char **next = new char *[argc + 1];
	int count = 0;
	next[count++] = argv[0];
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			while (i < argc) {
				next[count++] = argv[i++];
			}
			break;
		}
		if (strcmp(argv[i], "--editor") == 0 || strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--run-game") == 0) {
			display = true;
		} else if (!named && argv[i][0] != '-') {
			pkg = pkg_word(argv[i]);
			named = true;
		}
		if (strcmp(argv[i], "--headless") == 0) {
			continue;
		}
		if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
			project = argv[++i];
			continue;
		}
		if (strncmp(argv[i], "--path=", 7) == 0) {
			project = argv[i] + 7;
			continue;
		}
		next[count++] = argv[i];
		const CLI::Option *opt = CLI::option(argv[i]);
		if (opt && opt->value == CLI::REQUIRED && i + 1 < argc) next[count++] = argv[++i];
	}
	next[count] = nullptr;
	if (!pkg) {
		delete[] next;
		return 0;
	}
	if (display) {
		fprintf(stderr, "package commands cannot be combined with display commands\n");
		delete[] next;
		return 1;
	}
	char self[PATH_CAP];
	char path[PATH_CAP];
	const char *original = getenv("GD_HEADLESS_BIN");
	bool found = original && original[0] && can_start(original);
	if (found) {
		snprintf(path, sizeof(path), "%s", original);
	} else {
		found = self_path(argv[0], self, sizeof(self)) && headless_path(self, path, sizeof(path)) && can_start(path);
	}
	if (!found) {
		fprintf(stderr, "missing headless executable beside %s\n", argv[0]);
		delete[] next;
		return 1;
	}
	if (project && !enter_project(project)) {
		fprintf(stderr, "cannot enter project directory: %s\n", project);
		delete[] next;
		return 1;
	}
#ifdef _WIN32
	_putenv_s("GD_FOR_GODOT", "1");
#else
	setenv("GD_FOR_GODOT", "1", 1);
#endif
	replace_with(path, next);
	fprintf(stderr, "failed to start headless executable: %s\n", path);
	delete[] next;
	return 127;
}

} // namespace

#endif

// Leave headless startup to the common parser and delegate editor-side package commands.
int View::handoff(int argc, char **argv) {
#ifdef GD_VIEW
	if (argc > 0 && argv && argv[0]) return project_command(argc, argv);
#endif
	return 0;
}
