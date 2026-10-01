/**************************************************************************/
/*  std.cpp                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement error values and test assertions declared in std.h.

#include "cli/sys/std.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/string/string_builder.h"
#include "core/string/print_string.h"

// ---------------- Error reasons ----------------

namespace {

// Error categories and their native-dictionary spellings.
const char *KIND_NAMES[] = {
	"Error", "NotFound", "PermissionDenied", "AlreadyExists",
	"InvalidData", "TimedOut", "Interrupted", "Unsupported",
	"Unauthenticated", "Limited"
};

const char *KIND_CONSTANTS[] = {
	"ERROR", "NOT_FOUND", "PERMISSION_DENIED", "ALREADY_EXISTS",
	"INVALID_DATA", "TIMED_OUT", "INTERRUPTED", "UNSUPPORTED",
	"UNAUTHENTICATED", "LIMITED"
};

Ref<Err> shared_errors[Err::LIMITED + 1]; // One immutable value per error category.

// Accept named fields that can be looked up without coercing their values.
bool searchable_fields(const Dictionary &p_fields) {
	for (const Variant &key : p_fields.keys()) {
		if ((key.get_type() != Variant::STRING && key.get_type() != Variant::STRING_NAME) || String(key).is_empty()) return false;
	}
	return true;
}

// Require every field to match on one error node with the same value type.
bool matches_fields(const Dictionary &p_info, const Dictionary &p_fields) {
	for (const Variant &key : p_fields.keys()) {
		if (!p_info.has(key)) return false;
		const Variant actual = p_info[key];
		const Variant expected = p_fields[key];
		if (actual.get_type() != expected.get_type()) return false;
		bool valid = false;
		Variant same;
		Variant::evaluate(Variant::OP_EQUAL, actual, expected, same, valid);
		if (!valid || !bool(same)) return false;
	}
	return true;
}

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

// Isolate detail values and make the outer dictionary read-only.
Ref<Err> Err::make(const String &p_msg, Kind p_kind, const Dictionary &p_info, const Variant &p_partial) {
	Ref<Err> e;
	e.instantiate();
	e->msg = p_msg;
	e->kind = (int)p_kind < 0 || (int)p_kind > (int)LIMITED ? NONE : p_kind; // A value outside the enum names no category.
	e->info = p_info.duplicate(true);
	e->info.make_read_only();
	e->partial = p_partial;
	return e;
}

// Resolve an error class attribute to its shared category value.
Ref<Err> Err::constant(const StringName &p_name) {
	for (int i = 0; i <= LIMITED; i++) {
		if (p_name == StringName(KIND_CONSTANTS[i])) {
			return category(Kind(i));
		}
	}
	return Ref<Err>();
}

// List the category attributes exposed by the error class.
PackedStringArray Err::names() {
	PackedStringArray out;
	for (const char *name : KIND_CONSTANTS) {
		out.push_back(name);
	}
	return out;
}

// Create shared error values before scripts can refer to class constants.
void Err::init_categories() {
	for (int i = 0; i <= LIMITED; i++) {
		shared_errors[i] = make(String(), Kind(i));
		shared_errors[i]->shared = true;
	}
}

// Return the category value shared by all scripts.
Ref<Err> Err::category(Kind p_kind) {
	ERR_FAIL_COND_V(p_kind < NONE || p_kind > LIMITED, Ref<Err>());
	return shared_errors[p_kind];
}

// Release shared references before the runtime unregisters its classes.
void Err::clear_categories() {
	for (Ref<Err> &value : shared_errors) {
		value.unref();
	}
}

// Share a named failure without a message and allocate only when detail is present.
Ref<Err> Err::named(const String &p_msg, Kind p_kind) {
	return p_msg.is_empty() ? category(p_kind) : make(p_msg, p_kind);
}

// Report a missing target.
Ref<Err> Err::not_found(const String &p_msg) { return named(p_msg, NOT_FOUND); }
// Report rejected access.
Ref<Err> Err::permission_denied(const String &p_msg) { return named(p_msg, PERMISSION_DENIED); }
// Report a duplicate target.
Ref<Err> Err::already_exists(const String &p_msg) { return named(p_msg, ALREADY_EXISTS); }
// Report malformed input.
Ref<Err> Err::invalid_data(const String &p_msg) { return named(p_msg, INVALID_DATA); }
// Report an expired deadline.
Ref<Err> Err::timed_out(const String &p_msg) { return named(p_msg, TIMED_OUT); }
// Report interrupted work.
Ref<Err> Err::interrupted(const String &p_msg) { return named(p_msg, INTERRUPTED); }
// Report unavailable behavior.
Ref<Err> Err::unsupported(const String &p_msg) { return named(p_msg, UNSUPPORTED); }
// Report failed authentication.
Ref<Err> Err::unauthenticated(const String &p_msg) { return named(p_msg, UNAUTHENTICATED); }
// Report exhausted capacity.
Ref<Err> Err::limited(const String &p_msg) { return named(p_msg, LIMITED); }

// Convert a reason while retaining the selected shared category.
Ref<Err> Err::from_value(const Variant &p_reason, const Ref<Err> &p_kind) {
	if (p_kind.is_valid() && !p_kind->is_shared()) return invalid_data("Expected a shared Err category.");
	return from(p_reason, p_kind.is_valid() ? p_kind->kind : NONE);
}

// Normalize a failure reason without discarding an existing Err.
Ref<Err> Err::from(const Variant &p_reason, Kind p_kind) {
	Ref<Err> error = p_reason;
	if (error.is_null()) {
		const String msg = p_reason.get_type() == Variant::NIL ? String() : String(p_reason);
		return msg.is_empty() ? category(p_kind) : make(msg, p_kind);
	}
	return p_kind != NONE && error->get_kind() != p_kind ? error->as_kind(p_kind) : error;
}

// Group independent failures without losing their categories or details.
Ref<Err> Err::join(const Array &p_errors) {
	Vector<Ref<Err>> members;
	for (const Variant &value : p_errors) {
		if (value.get_type() == Variant::NIL) {
			continue;
		}
		Ref<Err> error = value;
		if (error.is_null()) error = invalid_data("Err.join expects Err or null values.");
		members.push_back(error);
	}
	if (members.is_empty()) {
		return Ref<Err>();
	}
	Ref<Err> result = make(String(), NONE);
	result->categorized = false;
	result->causes = members;
	return result;
}

// Combine two independent native failures without building a script array.
Ref<Err> Err::join(const Ref<Err> &p_first, const Ref<Err> &p_second) {
	if (p_first.is_null()) return p_second;
	if (p_second.is_null()) return p_first;
	Ref<Err> result = make(String(), NONE);
	result->categorized = false;
	result->causes.push_back(p_first);
	result->causes.push_back(p_second);
	return result;
}

// Copy error details into a new value.
Ref<Err> Err::copy() const {
	Ref<Err> e;
	e.instantiate();
	e->msg = msg;
	e->kind = kind;
	e->info = info;
	e->cause = cause;
	e->causes = causes;
	e->partial = partial;
	e->categorized = categorized;
	e->partial_wrapper = partial_wrapper;
	return e;
}

// Report this error under another category, keeping it as the cause.
Ref<Err> Err::as_kind(Kind p_kind) const {
	if (kind == p_kind) {
		return Ref<Err>(const_cast<Err *>(this));
	}
	Ref<Err> e = copy();
	e->kind = p_kind;
	e->categorized = true;
	e->partial_wrapper = false;
	e->msg = String();
	e->cause = Ref<Err>(const_cast<Err *>(this));
	e->causes.clear();
	return e;
}

// Reclassify an error with a shared category value.
Ref<Err> Err::as_category(const Ref<Err> &p_kind) const {
	if (p_kind.is_valid() && !p_kind->is_shared()) return invalid_data("Expected a shared Err category.");
	return as_kind(p_kind.is_valid() ? p_kind->kind : NONE);
}

// Wrap this error with additional context while retaining the cause.
Ref<Err> Err::note(const String &p_msg, const Dictionary &p_info) const {
	Ref<Err> e = copy();
	e->msg = p_msg;
	if (!p_info.is_empty()) {
		const Dictionary added = p_info.duplicate(true);
		e->info = info.duplicate(true);
		for (const Variant &key : added.keys()) e->info[key] = added[key];
		e->info.make_read_only();
	}
	e->cause = Ref<Err>(const_cast<Err *>(this));
	e->causes.clear();
	e->partial_wrapper = false;
	return e;
}

// Attach completed work without changing an existing error reference.
Ref<Err> Err::with_partial(const Variant &p_value) const {
	Ref<Err> e = copy();
	e->partial = p_value;
	e->cause = Ref<Err>(const_cast<Err *>(this));
	e->partial_wrapper = true;
	return e;
}

// Find a cause with the selected shared category.
bool Err::is(Kind p_kind) const {
	Vector<const Err *> pending;
	pending.push_back(this);
	while (!pending.is_empty()) {
		const Err *cur = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		if (cur->categorized && cur->kind == p_kind) {
			return true;
		}
		if (!cur->partial_wrapper) {
			for (int i = cur->causes.size() - 1; i >= 0; i--) pending.push_back(cur->causes[i].ptr());
		}
		if (cur->cause.is_valid()) pending.push_back(cur->cause.ptr());
	}
	return false;
}

// Find the first cause matching a value, category, or fields on one node.
Ref<Err> Err::find_value(const Variant &p_target, const Dictionary &p_where) const {
	const Err *target = nullptr;
	Dictionary fields;
	if (p_target.get_type() == Variant::DICTIONARY) {
		fields = p_target;
		if (fields.is_empty()) return Ref<Err>();
	} else if (p_target.get_type() == Variant::OBJECT) {
		target = Object::cast_to<Err>(p_target.get_validated_object());
		if (!target) return Ref<Err>();
	} else {
		return Ref<Err>();
	}
	if (!searchable_fields(fields) || !searchable_fields(p_where)) return Ref<Err>();
	Vector<const Err *> pending;
	pending.push_back(this);
	while (!pending.is_empty()) {
		const Err *cur = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		const bool target_matches = !target || cur == target || (cur->categorized && target->shared && cur->kind == target->kind);
		if (target_matches && matches_fields(cur->info, fields) && matches_fields(cur->info, p_where)) {
			return Ref<Err>(const_cast<Err *>(cur));
		}
		if (!cur->partial_wrapper) {
			for (int i = cur->causes.size() - 1; i >= 0; i--) pending.push_back(cur->causes[i].ptr());
		}
		if (cur->cause.is_valid()) pending.push_back(cur->cause.ptr());
	}
	return Ref<Err>();
}

// Return the independent failures grouped by this value.
Array Err::get_causes() const {
	Array out;
	for (const Ref<Err> &error : causes) out.push_back(error);
	return out;
}

// Spell the category represented by a shared error value.
String Err::category_name(const Ref<Err> &p_kind) {
	return p_kind.is_valid() ? name_of(p_kind->kind) : String();
}

// Format this error and its causes as text.
String Err::text() const {
	StringBuilder out;
	struct Part {
		const Err *error;
		bool newline;
		bool colon;
	};
	Vector<Part> pending;
	pending.push_back({ this, false, false });
	while (!pending.is_empty()) {
		const Part part = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		const Err *cur = part.error;
		if (part.newline) out += "\n";
		if (part.colon) out += ": ";
		if (!cur->causes.is_empty() && cur->msg.is_empty()) {
			for (int i = cur->causes.size() - 1; i >= 0; i--) pending.push_back({ cur->causes[i].ptr(), i > 0, false });
			continue;
		}
		if (cur->partial_wrapper && cur->cause.is_valid()) {
			pending.push_back({ cur->cause.ptr(), false, false });
			continue;
		}
		if (cur->cause.is_valid() && cur->kind == cur->cause->kind && !cur->msg.is_empty()) {
			out += cur->msg;
		} else if (cur->kind == NONE) {
			out += cur->msg.is_empty() ? String("Error") : cur->msg;
		} else {
			out += cur->msg.is_empty() ? name_of(cur->kind) : vformat("%s: %s", name_of(cur->kind), cur->msg);
		}
		if (cur->cause.is_valid()) pending.push_back({ cur->cause.ptr(), false, true });
	}
	return out.as_string();
}

// Present an error's message when printing the value directly.
String Err::_to_string() {
	return text();
}

// Register public script methods and properties.
void Err::_bind_methods() {
	ClassDB::bind_static_method("Err", D_METHOD("not_found", "msg"), &Err::not_found, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("permission_denied", "msg"), &Err::permission_denied, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("already_exists", "msg"), &Err::already_exists, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("invalid_data", "msg"), &Err::invalid_data, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("timed_out", "msg"), &Err::timed_out, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("interrupted", "msg"), &Err::interrupted, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("unsupported", "msg"), &Err::unsupported, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("unauthenticated", "msg"), &Err::unauthenticated, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("limited", "msg"), &Err::limited, DEFVAL(String()));
	ClassDB::bind_static_method("Err", D_METHOD("from", "reason", "kind"), &Err::from_value, DEFVAL(Ref<Err>()));
	ClassDB::bind_static_method("Err", D_METHOD("join", "errors"), static_cast<Ref<Err> (*)(const Array &)>(&Err::join));
	ClassDB::bind_method(D_METHOD("get_msg"), &Err::get_msg);
	ClassDB::bind_method(D_METHOD("get_kind"), &Err::get_category);
	ClassDB::bind_method(D_METHOD("get_info"), &Err::get_info);
	ClassDB::bind_method(D_METHOD("get_cause"), &Err::get_cause);
	ClassDB::bind_method(D_METHOD("get_causes"), &Err::get_causes);
	ClassDB::bind_method(D_METHOD("get_partial"), &Err::get_partial);
	ClassDB::bind_method(D_METHOD("as_kind", "kind"), &Err::as_category);
	ClassDB::bind_method(D_METHOD("note", "msg", "info"), &Err::note, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("with_partial", "value"), &Err::with_partial);
	ClassDB::bind_method(D_METHOD("find", "target", "where"), &Err::find_value, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("text"), &Err::text);

	ClassDB::bind_static_method("Err", D_METHOD("name_of", "kind"), &Err::category_name);
	ClassDB::bind_static_method("Err", D_METHOD("names"), &Err::names);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "msg"), "", "get_msg");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "kind", PROPERTY_HINT_RESOURCE_TYPE, "Err"), "", "get_kind");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "info"), "", "get_info");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cause", PROPERTY_HINT_RESOURCE_TYPE, "Err"), "", "get_cause");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "causes", PROPERTY_HINT_ARRAY_TYPE, "Err"), "", "get_causes");
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "partial", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT), "", "get_partial");

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

// Assert that an operation returned no error.
void GDTestCheck::succeeds(const Ref<Err> &p_error, const String &p_label) {
	count++;
	if (p_error.is_valid()) {
		miss(vformat("%s: %s", p_label, p_error->text()));
	}
}

// Assert that an operation failed with the requested category.
void GDTestCheck::fails(const Ref<Err> &p_error, Err::Kind p_kind, const String &p_label) {
	count++;
	if (p_error.is_null()) {
		miss(vformat(String::utf8("%s: 成功してしまった"), p_label));
		return;
	}
	if (p_kind != Err::NONE && !p_error->is(p_kind)) {
		miss(vformat(String::utf8("%s: 種別が %s ではなく %s"), p_label, Err::name_of(p_kind), Err::name_of(p_error->get_kind())));
	}
}

// Assert a failure against a shared category value.
void GDTestCheck::fails_value(const Ref<Err> &p_error, const Ref<Err> &p_kind, const String &p_label) {
	fails(p_error, p_kind.is_valid() ? p_kind->get_kind() : Err::NONE, p_label);
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
	ClassDB::bind_method(D_METHOD("succeeds", "error", "label"), &GDTestCheck::succeeds, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("fails", "error", "kind", "label"), &GDTestCheck::fails_value, DEFVAL(Ref<Err>()), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("get_failures"), &GDTestCheck::get_failures);
	ClassDB::bind_method(D_METHOD("get_count"), &GDTestCheck::get_count);
	ClassDB::bind_method(D_METHOD("code"), &GDTestCheck::code);
	ClassDB::bind_method(D_METHOD("report"), &GDTestCheck::report);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "failures"), "", "get_failures");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "count"), "", "get_count");
}
