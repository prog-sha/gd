/**************************************************************************/
/*  std.cpp                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement the core result and error types declared in std.h.

#include "cli/sys/std.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"

// ---------------- Error reasons ----------------

namespace {

// Error categories and their native-dictionary spellings.
const char *KIND_NAMES[] = {
	"", "NotFound", "PermissionDenied", "AlreadyExists",
	"InvalidData", "TimedOut", "Interrupted", "Unsupported",
	"Unauthenticated", "Limited"
};

} // namespace

// Map Error values only to Err categories with established meanings.
// Keep unknown failures as NONE with original details rather than guessing NotFound.
Err::Kind Err::of(Error p_err) {
	switch (p_err) {
		case ERR_UNAUTHORIZED: // Mount permission denial.
		case ERR_FILE_NO_PERMISSION: // OS permission denial.
			return PERMISSION_DENIED;
		case ERR_FILE_NOT_FOUND:
		case ERR_DOES_NOT_EXIST:
			return NOT_FOUND;
		case ERR_ALREADY_EXISTS:
			return ALREADY_EXISTS;
		case ERR_OUT_OF_MEMORY: // Resource allocation exhausted.
			return LIMITED;
		case ERR_INVALID_DATA:
		case ERR_INVALID_PARAMETER:
		case ERR_PARAMETER_RANGE_ERROR:
		case ERR_FILE_BAD_PATH:
			return INVALID_DATA;
		case ERR_TIMEOUT:
			return TIMED_OUT;
		case ERR_UNAVAILABLE:
			return UNSUPPORTED;
		default:
			return NONE;
	}
}

// Resolve category spelling from native result dictionaries.
Err::Kind Err::kind_of(const String &p_name) {
	for (int i = 1; i < (int)(sizeof(KIND_NAMES) / sizeof(char *)); i++) {
		if (p_name == KIND_NAMES[i]) {
			return Kind(i);
		}
	}
	return NONE;
}

// Return an Err category's name.
String Err::name_of(Kind p_kind) {
	return KIND_NAMES[CLAMP((int)p_kind, 0, (int)LIMITED)];
}

// Create an error with an immutable copy of its detail dictionary.
Ref<Err> Err::make(const String &p_msg, Kind p_kind, const Dictionary &p_info) {
	Ref<Err> e;
	e.instantiate();
	e->msg = p_msg;
	e->kind = (int)p_kind < 0 || (int)p_kind > (int)LIMITED ? NONE : p_kind; // A value outside the enum names no category.
	e->info = p_info.duplicate(true);
	e->info.make_read_only();
	return e;
}

// Report this error under another category, keeping it as the cause.
Ref<Err> Err::as_kind(Kind p_kind) const {
	Ref<Err> e = make(msg, p_kind, info);
	e->cause = Ref<Err>(const_cast<Err *>(this));
	return e;
}

// Wrap this error with additional context while retaining the cause.
Ref<Err> Err::note(const String &p_msg) const {
	Ref<Err> e = make(p_msg, kind, info);
	e->cause = Ref<Err>(const_cast<Err *>(this));
	return e;
}

// Check the cause chain for a category.
bool Err::is(Kind p_kind) const {
	for (const Err *cur = this; cur; cur = cur->cause.ptr()) {
		if (cur->kind == p_kind) {
			return true;
		}
	}
	return false;
}

// Find an error category in the cause chain.
Ref<Err> Err::find(Kind p_kind) const {
	for (const Err *cur = this; cur; cur = cur->cause.ptr()) {
		if (cur->kind == p_kind) {
			return Ref<Err>(const_cast<Err *>(cur));
		}
	}
	return Ref<Err>();
}

// Format this error and its causes as text.
String Err::text() const {
	String out = kind == NONE ? msg : vformat("%s: %s", name_of(kind), msg);
	if (cause.is_valid()) {
		out += ": " + cause->text();
	}
	return out;
}

// Register public script methods and properties.
void Err::_bind_methods() {
	ClassDB::bind_static_method("Err", D_METHOD("err", "msg", "kind", "info"), &Err::make, DEFVAL(NONE), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("get_msg"), &Err::get_msg);
	ClassDB::bind_method(D_METHOD("get_kind"), &Err::get_kind);
	ClassDB::bind_method(D_METHOD("get_info"), &Err::get_info);
	ClassDB::bind_method(D_METHOD("get_cause"), &Err::get_cause);
	ClassDB::bind_method(D_METHOD("note", "msg"), &Err::note);
	ClassDB::bind_method(D_METHOD("is", "kind"), &Err::is);
	ClassDB::bind_method(D_METHOD("find", "kind"), &Err::find);
	ClassDB::bind_method(D_METHOD("text"), &Err::text);

	ClassDB::bind_static_method("Err", D_METHOD("name_of", "kind"), &Err::name_of);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "msg"), "", "get_msg");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "kind", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM, "Err.Kind"), "", "get_kind");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "info"), "", "get_info");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cause", PROPERTY_HINT_RESOURCE_TYPE, "Err"), "", "get_cause");

	BIND_ENUM_CONSTANT(NONE);
	BIND_ENUM_CONSTANT(NOT_FOUND);
	BIND_ENUM_CONSTANT(PERMISSION_DENIED);
	BIND_ENUM_CONSTANT(ALREADY_EXISTS);
	BIND_ENUM_CONSTANT(INVALID_DATA);
	BIND_ENUM_CONSTANT(TIMED_OUT);
	BIND_ENUM_CONSTANT(INTERRUPTED);
	BIND_ENUM_CONSTANT(UNSUPPORTED);
	BIND_ENUM_CONSTANT(UNAUTHENTICATED);
	BIND_ENUM_CONSTANT(LIMITED);
}

// ---------------- Result values ----------------

// Unwrap result values without losing a nested failure or looping on a cycle.
Ref<Err> R::unpack(Variant &r_value) {
	HashSet<ObjectID> seen;
	while (r_value.get_type() == Variant::OBJECT) {
		const Ref<R> result = r_value;
		if (result.is_null()) return Ref<Err>(r_value);
		if (!result->get_ok()) return result->get_e();
		const Variant next = result->get_v();
		if (Ref<R>(next).is_valid()) {
			if (seen.has(result->get_instance_id())) return Err::make("cyclic response result", Err::INVALID_DATA);
			seen.insert(result->get_instance_id());
		}
		r_value = next;
	}
	return Ref<Err>();
}

// Return a fresh success to a reference-valued caller.
Ref<R> R::ok(const Variant &p_v) {
	Ref<R> r;
	r.instantiate();
	r->v = p_v;
	return r;
}

// Transfer a fresh success directly to its variant owner.
Variant R::okv(const Variant &p_v) {
	R *r = ::new (DefaultAllocator{}) R;
	Variant result(r);
	// Keep the first owner alive before running ordinary object initialization.
	postinitialize_handler(r);
	r->v = p_v;
	return result;
}

// Create a failed R value.
Ref<R> R::err(const Variant &p_reason, Err::Kind p_kind, const Variant &p_v) {
	Ref<R> r;
	r.instantiate();
	Ref<Err> why = p_reason;
	Ref<R> failed = p_reason;
	if (failed.is_valid()) {
		why = failed->e;
	}
	if (why.is_valid() && p_kind != Err::NONE && why->get_kind() != p_kind) {
		why = why->as_kind(p_kind);
	}
	r->e = why.is_valid() ? why : Err::make(String(p_reason), p_kind);
	r->v = p_v;
	return r;
}

// Return the success or partial value.
Variant R::get_v() const {
	return v;
}

// Return the fallback on failure or the value on success.
Variant R::v_or(const Variant &p_fallback) const {
	return e.is_valid() ? p_fallback : v;
}

// Add context to a failed result while preserving partial data.
Ref<R> R::note(const String &p_msg) const {
	if (e.is_null()) {
		return Ref<R>(const_cast<R *>(this));
	}
	return err(e->note(p_msg), Err::NONE, v);
}

// Register public script methods and properties.
void R::_bind_methods() {
	ClassDB::bind_static_method("R", D_METHOD("ok", "v"), &R::ok, DEFVAL(Variant()));
	ClassDB::bind_static_method("R", D_METHOD("err", "reason", "kind", "v"), &R::err, DEFVAL(Err::NONE), DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("get_v"), &R::get_v);
	ClassDB::bind_method(D_METHOD("get_e"), &R::get_e);
	ClassDB::bind_method(D_METHOD("get_ok"), &R::get_ok);
	ClassDB::bind_method(D_METHOD("v_or", "fallback"), &R::v_or);
	ClassDB::bind_method(D_METHOD("note", "msg"), &R::note);

	ADD_PROPERTY(PropertyInfo(Variant::NIL, "v", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT), "", "get_v");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "e", PROPERTY_HINT_RESOURCE_TYPE, "Err"), "", "get_e");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ok"), "", "get_ok");
}

// ---------------- Test assertions ----------------

// Compare values with script equality semantics, including mixed numeric types.
bool GDTestCheck::same(const Variant &p_a, const Variant &p_b) {
	bool valid = false;
	Variant out;
	Variant::evaluate(Variant::OP_EQUAL, p_a, p_b, out, valid);
	return valid && (bool)out;
}

// Record one assertion failure.
void GDTestCheck::miss(const String &p_line) {
	failures++;
	print_error("NG " + p_line);
}

// Assert that two values are equal.
void GDTestCheck::eq(const Variant &p_got, const Variant &p_want, const String &p_label) {
	count++;
	if (!same(p_got, p_want)) {
		miss(vformat("%s: got=%s want=%s", p_label, p_got, p_want));
	}
}

// Assert that two values differ.
void GDTestCheck::ne(const Variant &p_got, const Variant &p_other, const String &p_label) {
	count++;
	if (same(p_got, p_other)) {
		miss(vformat(String::utf8("%s: 同じ値になっている %s"), p_label, p_got));
	}
}

// Assert that a condition is true.
void GDTestCheck::ok(bool p_cond, const String &p_label) {
	count++;
	if (!p_cond) {
		miss(p_label);
	}
}

// Assert that a condition is false.
void GDTestCheck::no(bool p_cond, const String &p_label) {
	ok(!p_cond, p_label);
}

// Assert that two numbers differ by no more than the tolerance.
void GDTestCheck::close_to(double p_got, double p_want, double p_slack, const String &p_label) {
	count++;
	const double gap = Math::abs(p_got - p_want);
	if (gap > p_slack) {
		miss(vformat(String::utf8("%s: got=%f want=%f 差=%f"), p_label, p_got, p_want, gap));
	}
}

// Assert that a collection contains the requested value.
void GDTestCheck::has(const Variant &p_box, const Variant &p_item, const String &p_label) {
	count++;
	bool found = false;
	switch (p_box.get_type()) {
		case Variant::STRING:
			found = String(p_box).contains(p_item);
			break;
		case Variant::DICTIONARY:
			found = Dictionary(p_box).has(p_item);
			break;
		case Variant::ARRAY:
			found = Array(p_box).has(p_item);
			break;
		case Variant::PACKED_STRING_ARRAY:
			found = PackedStringArray(p_box).has(p_item);
			break;
		case Variant::PACKED_BYTE_ARRAY:
			found = PackedByteArray(p_box).has(p_item);
			break;
		case Variant::PACKED_INT32_ARRAY:
			found = PackedInt32Array(p_box).has(p_item);
			break;
		default:
			break;
	}
	if (!found) {
		miss(vformat(String::utf8("%s: %s の中に %s が無い"), p_label, p_box, p_item));
	}
}

// Assert that R succeeded.
void GDTestCheck::succeeds(const Ref<R> &p_r, const String &p_label) {
	count++;
	if (p_r.is_null() || !p_r->get_ok()) {
		miss(vformat("%s: %s", p_label, p_r.is_valid() ? p_r->get_e()->text() : String::utf8("答えが無い")));
	}
}

// Assert that R failed with the requested category.
void GDTestCheck::fails(const Ref<R> &p_r, Err::Kind p_kind, const String &p_label) {
	count++;
	if (p_r.is_null() || p_r->get_ok()) {
		miss(vformat(String::utf8("%s: 成功してしまった"), p_label));
		return;
	}
	if (p_kind != Err::NONE && !p_r->get_e()->is(p_kind)) {
		miss(vformat(String::utf8("%s: 種別が %s ではなく %s"), p_label, Err::name_of(p_kind), Err::name_of(p_r->get_e()->get_kind())));
	}
}

// Print check and failure counts.
void GDTestCheck::report() const {
	print_line(vformat("checks=%d failures=%d", count, failures));
}

// Register public script methods and properties.
void GDTestCheck::_bind_methods() {
	ClassDB::bind_method(D_METHOD("eq", "got", "want", "label"), &GDTestCheck::eq, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("ne", "got", "other", "label"), &GDTestCheck::ne, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("ok", "cond", "label"), &GDTestCheck::ok, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("no", "cond", "label"), &GDTestCheck::no, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("near", "got", "want", "slack", "label"), &GDTestCheck::close_to, DEFVAL(1e-9), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("has", "box", "item", "label"), &GDTestCheck::has, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("succeeds", "r", "label"), &GDTestCheck::succeeds, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("fails", "r", "kind", "label"), &GDTestCheck::fails, DEFVAL(Err::NONE), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("get_failures"), &GDTestCheck::get_failures);
	ClassDB::bind_method(D_METHOD("get_count"), &GDTestCheck::get_count);
	ClassDB::bind_method(D_METHOD("code"), &GDTestCheck::code);
	ClassDB::bind_method(D_METHOD("report"), &GDTestCheck::report);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "failures"), "", "get_failures");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "count"), "", "get_count");
}
