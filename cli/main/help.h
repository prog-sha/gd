/**************************************************************************/
/*  help.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Display CLI usage with only supported flags.
//
// Render the command registry used by argument validation.
//
// Implementation is in help.cpp.

#pragma once

#include "core/string/ustring.h"

class Help {
public:
	static void show();
	static bool command(const String &p_name);
};
