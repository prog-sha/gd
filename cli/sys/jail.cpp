/**************************************************************************/
/*  jail.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Install isolated filesystem wrappers and remove unused resource loaders.

#include "cli/sys/jail.h"

#include "cli/sys/jail_unix.h"
#include "cli/sys/jail_windows.h"

#include "core/config/project_settings.h"
#include "core/extension/gdextension_resource_format.h"
#include "core/io/resource_loader.h"
#include "modules/gdscript/gdscript_resource_format.h"

#if defined(UNIX_ENABLED)
#include "drivers/unix/dir_access_unix.h"
#include "drivers/unix/file_access_unix.h"
#elif defined(WINDOWS_ENABLED)
#include "drivers/windows/dir_access_windows.h"
#include "drivers/windows/file_access_windows.h"
#endif

// Apply filesystem isolation rules to the process.
void install_jail() {
#if defined(UNIX_ENABLED)
#elif defined(WINDOWS_ENABLED)
#else
	return; // This configuration has no native-file access wrapper.
#endif
#if defined(UNIX_ENABLED) || defined(WINDOWS_ENABLED)
	// Cover all three access kinds, including res:// and user:// mounts.
	for (int i = 0; i < FileAccess::ACCESS_MAX; i++) {
		const FileAccess::AccessType at = (FileAccess::AccessType)i;
		if (at == FileAccess::ACCESS_PIPE) {
			continue; // The network access kind has no native filesystem path.
		}
#if defined(UNIX_ENABLED)
		FileAccess::make_default<FileJailUnix>(at);
#else
		FileAccess::make_default<FileJailWindows>(at);
#endif
	}
	for (int i = 0; i < DirAccess::ACCESS_MAX; i++) {
		const DirAccess::AccessType at = (DirAccess::AccessType)i;
		// Preserve an already-loaded package provider for res:// instead of reverting to native files.
		if (at == DirAccess::ACCESS_RESOURCES && ProjectSettings::get_singleton()->is_using_datapack()) {
			continue;
		}
#if defined(UNIX_ENABLED)
		DirAccess::make_default<DirJailUnix>(at);
#else
		DirAccess::make_default<DirJailWindows>(at);
#endif
	}
#endif
}

// Retain only script and extension loaders.
// Identify loaders by type because they are not registered in ClassDB.
static bool _keep_loader(const Ref<ResourceFormatLoader> &p_ld) {
	return Object::cast_to<ResourceFormatLoaderGDScript>(p_ld.ptr()) != nullptr ||
			Object::cast_to<GDExtensionResourceLoader>(p_ld.ptr()) != nullptr;
}

// Remove resource loaders outside the allowed set.
void trim_loaders() {
	// Remove from the back because subsequent entries shift left.
	for (int i = ResourceLoader::get_resource_format_loader_count() - 1; i >= 0; i--) {
		const Ref<ResourceFormatLoader> ld = ResourceLoader::get_resource_format_loader(i);
		if (!_keep_loader(ld)) {
			ResourceLoader::remove_resource_format_loader(ld);
		}
	}
}
