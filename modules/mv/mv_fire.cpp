/**************************************************************************/
/*  mv_fire.cpp                                                           */
/**************************************************************************/

// @online signal relay that bundles the arguments as-is, without counting, and passes them to the Online side.

#include "mv_fire.h"

#include "mv_runtime.h"

#include "core/object/object.h"

MVFire::MVFire(Object *p_runtime, int64_t p_uid, const String &p_name, const String &p_to) {
	runtime = p_runtime != nullptr ? p_runtime->get_instance_id() : ObjectID();
	uid = p_uid;
	name = p_name;
	to = p_to;
}

// Same relay when the Node, signal and target all match
bool MVFire::equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	const MVFire *a = static_cast<const MVFire *>(p_a);
	const MVFire *b = static_cast<const MVFire *>(p_b);
	return a->runtime == b->runtime && a->uid == b->uid && a->name == b->name && a->to == b->to;
}

// Stable ordering for sets. The field meanings do not matter
bool MVFire::less(const CallableCustom *p_a, const CallableCustom *p_b) {
	const MVFire *a = static_cast<const MVFire *>(p_a);
	const MVFire *b = static_cast<const MVFire *>(p_b);
	// Compare the same four fields as equal, in the same order. Dropping one would make ordering disagree with equality
	if (a->runtime != b->runtime) {
		return a->runtime < b->runtime;
	}
	if (a->uid != b->uid) {
		return a->uid < b->uid;
	}
	if (a->name != b->name) {
		return a->name < b->name;
	}
	return a->to < b->to;
}

uint32_t MVFire::hash() const {
	return name.hash() ^ uint32_t(uid) ^ to.hash();
}

String MVFire::get_as_text() const {
	return "@online signal " + name;
}

// Pass the emitted arguments to the Online side as-is. Their count is not checked
void MVFire::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const {
	r_return_value = Variant();
	r_call_error.error = Callable::CallError::CALL_OK;
	MVRuntime *rt = Object::cast_to<MVRuntime>(ObjectDB::get_instance(runtime));
	if (rt == nullptr) {
		return;
	}
	Array args;
	args.resize(p_argcount);
	for (int i = 0; i < p_argcount; i++) {
		args[i] = *p_arguments[i];
	}
	rt->fire(uid, name, to, args);
}
