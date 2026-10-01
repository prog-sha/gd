/**************************************************************************/
/*  mv_guest.h                                                           */
/**************************************************************************/

// Prepares the Client's guest identity input without author code.

#pragma once

#include "core/variant/dictionary.h"

class MVGuest {
public:
	enum {
		ID_BYTES = 32, // Random byte count of the install ID
	};

	static Dictionary input(); // Return guest input from the install ID, or the device ID when that is unavailable
};
