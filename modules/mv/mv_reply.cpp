/**************************************************************************/
/*  mv_reply.cpp                                                          */
/**************************************************************************/

// One-shot answer container that authors await.

#include "mv_reply.h"

#include "core/object/class_db.h"

// Stay alive until the answer is returned. Signals do not count their owner,
// so without holding one reference here it would vanish before the await connects
void MVReply::hold() {
	keep = Ref<MVReply>(this);
}

void MVReply::finish(const Variant &p_value) {
	// Releasing first would destroy this object mid-function
	const Ref<MVReply> alive = keep;
	keep.unref();
	emit_signal(SNAME("__done"), p_value);
}

// Not reachable by name from authors. Only holds the signal used as the await target
void MVReply::_bind_methods() {
	ClassDB::bind_method(D_METHOD("__finish", "value"), &MVReply::finish);
	ADD_SIGNAL(MethodInfo("__done", PropertyInfo(Variant::NIL, "value")));
}
