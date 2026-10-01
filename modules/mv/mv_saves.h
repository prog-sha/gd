/**************************************************************************/
/*  mv_saves.h                                                            */
/**************************************************************************/

// Destination for @online_save. The Online-side world knows only this interface.
// A world with a local Redis (MVStore) and one that asks the Secret (host relay)
// can be swapped in the same shape. Reads pass the answer to a Callable once it arrives.
// A user's own saved values are read first by the side that admits them (identity) and passed to join_context, so they are not here

#pragma once

#include "core/variant/callable.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class MVSaves {
public:
	virtual ~MVSaves() {}

	virtual bool is_open() const = 0; // Whether values can be stored now
	virtual bool keeps_world() const { return true; } // Whether values of ownerless things are stored too. If false, save_world and drop_world are not called
	virtual void poll() = 0; // Advance pending replies and reconnect if disconnected
	// Saved values are a "name -> value" dictionary. Only changed names need writing; a read returns all.
	// World things carry their type name in "@kind", so values are not restored when another type sits in the same place
	virtual void load_world(const Callable &p_done) = 0; // Pass the world's saved values to p_done as { place: { name: value } }. Waits until readable
	virtual void save_user(const String &p_id, const String &p_kind, const Dictionary &p_values) = 0; // Write a user's saved values for that type
	virtual void save_world(const String &p_path, const Dictionary &p_values) {} // Write world saved values. Only for keeps_world stores
	virtual void drop_world(const String &p_path, const PackedStringArray &p_fields) {} // Discard saved values of a removed Node. Only for keeps_world stores
	virtual void begin_save() {} // Group writes from here to end_save into one batch
	virtual void end_save() {}
};
