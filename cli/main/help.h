/**************************************************************************/
/*  help.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Display CLI usage with only supported flags.
//
// Keep the help list aligned with this executable's argument parser.
// Listing unsupported runtime flags would suggest options that cannot take effect.
//
// Implementation is in help.cpp.

#pragma once

class Help {
public:
	static void show(const char *p_binary);
};
