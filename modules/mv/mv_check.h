/**************************************************************************/
/*  mv_check.h                                                            */
/**************************************************************************/

// Checks the text of a delivered @online script before accepting it.
//
// It checks three things the engine's language cannot tell apart:
//   1 runtime-internal names (double underscore)
//   2 annotations that run code on the editor side
//   3 project-wide names (class_name), which cannot live in scripts reloaded on both sides
// Tools reachable by name, endless loops and by-name access to properties are handled by the engine's language.

#pragma once

#include "core/object/ref_counted.h"

class MVCheck : public RefCounted {
	GDCLASS(MVCheck, RefCounted);

protected:
	static void _bind_methods();

public:
	enum {
		SRC_MAX = 4 * 1024 * 1024, // Max size of one script. Does not restrict how authors write; only rejects broken files
	};

	static Dictionary check(const String &p_src); // Decide whether it may be accepted. Returns the type and the rejection reason
};
