/**************************************************************************/
/*  jail_windows.h                                                        */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Wrap native Windows file operations and preserve their original failure codes.

#pragma once

#ifdef WINDOWS_ENABLED

#include "cli/sys/jail.h"

#include "drivers/windows/dir_access_windows.h"
#include "drivers/windows/file_access_windows.h"

class FileJailWindows : public FileJail<FileAccessWindows> {
protected:
	virtual Error open_internal(const String &p_path, int p_mode_flags) override;
};

class DirJailWindows : public DirJail<DirAccessWindows> {
public:
	virtual Error list_dir_begin() override;
	virtual String get_next() override;
	virtual Error change_dir(String p_dir) override;
	virtual Error make_dir(String p_dir) override;
	virtual Error rename(String p_from, String p_to) override;
	virtual Error remove(String p_path) override;
};

#endif
