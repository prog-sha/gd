/**************************************************************************/
/*  mv_reply.h                                                            */
/**************************************************************************/

// Small container that returns a network answer exactly once.
// Authors await it, so the answer is always delivered on the next idle.
// Otherwise, in a single process the answer arrives first and the await waits forever.

#pragma once

#include "core/object/ref_counted.h"

class MVReply : public RefCounted {
	GDCLASS(MVReply, RefCounted);

	Ref<MVReply> keep; // Keeps itself alive until the answer is returned

protected:
	static void _bind_methods();

public:
	void hold(); // Stay alive until the answer is returned
	void finish(const Variant &p_value); // Return the answer once and release itself
};
