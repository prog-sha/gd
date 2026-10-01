/**************************************************************************/
/*  mv_fire.h                                                             */
/**************************************************************************/

// Relay that receives an @online signal through one callable regardless of argument count.
// No argument limit, so it can be written like an ordinary signal.

#pragma once

#include "core/object/object_id.h"
#include "core/variant/callable.h"
#include "core/variant/variant.h"

class MVFire : public CallableCustom {
	ObjectID runtime; // Online-side runtime that queues the signal
	int64_t uid = 0; // Serial number of the emitting Node
	String name; // Signal name
	String to; // Target when sent only to the owner. Empty means everyone

	static bool equal(const CallableCustom *p_a, const CallableCustom *p_b); // Whether two relays are the same
	static bool less(const CallableCustom *p_a, const CallableCustom *p_b); // Ordering for sorting

public:
	MVFire(Object *p_runtime, int64_t p_uid, const String &p_name, const String &p_to);

	uint32_t hash() const override;
	String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override { return equal; }
	CompareLessFunc get_compare_less_func() const override { return less; }
	ObjectID get_object() const override { return runtime; }
	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const override;
};
