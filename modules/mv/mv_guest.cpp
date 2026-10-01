/**************************************************************************/
/*  mv_guest.cpp                                                         */
/**************************************************************************/

// Builds the Client's guest identity input from a stored install ID or the device ID.

#include "mv_guest.h"

#include "mv_local_id.h"

#include "core/os/os.h"

// Prefer a per-project install ID; fall back to the device ID only where it cannot be saved
Dictionary MVGuest::input() {
	String id = MVLocalID::read_or_make("user://online_guest_id", ID_BYTES);
	String source = "install";
	if (id.is_empty() && OS::get_singleton()->get_name() != "Web") {
		id = OS::get_singleton()->get_unique_id().strip_edges();
		if (!id.is_empty()) {
			source = "device";
		}
	}
	Dictionary out;
	if (id.is_empty()) {
		return out;
	}
	Dictionary guest;
	guest["source"] = source;
	guest["id"] = id;
	out["guest"] = guest;
	return out;
}
