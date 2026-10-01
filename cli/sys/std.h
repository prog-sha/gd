/**************************************************************************/
/*  std.h                                                                 */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Core standard-library error type and test assertions.
// Carry failures as Err values in a separate return slot.

#include "core/object/ref_counted.h"
#include "core/variant/binder_common.h"

// Error reason with a category, message, and traversable cause chain.
class Err : public RefCounted {
	GDCLASS(Err, RefCounted);

public:
	// Common error categories.
	enum Kind {
		NONE,
		NOT_FOUND, // Target not found.
		PERMISSION_DENIED, // Insufficient permission.
		ALREADY_EXISTS, // Target already exists.
		INVALID_DATA, // Malformed data.
		TIMED_OUT, // Deadline expired.
		INTERRUPTED, // Operation interrupted.
		UNSUPPORTED, // Unsupported operation.
		UNAUTHENTICATED, // Authentication failed.
		LIMITED, // Usage limit exceeded.
	};

	// Category spelling used by native result dictionaries.
	static Kind kind_of(const String &p_name);
	// Map the actual Error returned by an operation into an Err category.
	// Centralize mapping so callers do not guess failure categories.
	static Kind of(Error p_err);
	static String name_of(Kind p_kind);
	Ref<Err> as_kind(Kind p_kind) const;

private:
	String msg; // Description of what happened.
	Kind kind = NONE;
	Dictionary info; // Machine-readable details supplied by the source.
	Ref<Err> cause; // Wrapped original cause.
	Vector<Ref<Err>> causes; // Independent failures grouped in one result.
	Variant partial; // Value completed before the failure.
	bool shared = false; // Whether this is a stable category value.
	bool categorized = true; // Whether this value itself names an error category.
	bool partial_wrapper = false; // Whether this value only attaches completed work to its cause.
	Ref<Err> copy() const; // Copy this reason without sharing mutable return details.
	static Ref<Err> named(const String &p_msg, Kind p_kind); // Reuse the shared kind when no detail is supplied.

protected:
	String _to_string() override; // Print the error description through ordinary value formatting.
	static void _bind_methods();

public:
	static Ref<Err> make(const String &p_msg, Kind p_kind, const Dictionary &p_info = Dictionary(), const Variant &p_partial = Variant());
	// Return the shared value that names an error category.
	static Ref<Err> constant(const StringName &p_name);
	static PackedStringArray names();
	static void init_categories();
	static Ref<Err> category(Kind p_kind);
	static void clear_categories();
	// Report a missing target with optional detail.
	static Ref<Err> not_found(const String &p_msg = String());
	// Report rejected access with optional detail.
	static Ref<Err> permission_denied(const String &p_msg = String());
	// Report a duplicate target with optional detail.
	static Ref<Err> already_exists(const String &p_msg = String());
	// Report malformed input with optional detail.
	static Ref<Err> invalid_data(const String &p_msg = String());
	// Report an expired deadline with optional detail.
	static Ref<Err> timed_out(const String &p_msg = String());
	// Report interrupted work with optional detail.
	static Ref<Err> interrupted(const String &p_msg = String());
	// Report unavailable behavior with optional detail.
	static Ref<Err> unsupported(const String &p_msg = String());
	// Report failed authentication with optional detail.
	static Ref<Err> unauthenticated(const String &p_msg = String());
	// Report exhausted capacity with optional detail.
	static Ref<Err> limited(const String &p_msg = String());
	// Turn a reason into Err while retaining an existing cause and category.
	static Ref<Err> from(const Variant &p_reason, Kind p_kind = NONE);
	static Ref<Err> from_value(const Variant &p_reason, const Ref<Err> &p_kind = Ref<Err>());
	static Ref<Err> join(const Array &p_errors);
	static Ref<Err> join(const Ref<Err> &p_first, const Ref<Err> &p_second);

	String get_msg() const { return msg; }
	Kind get_kind() const { return kind; }
	bool is_shared() const { return shared; }
	Ref<Err> get_category() const { return categorized ? category(kind) : Ref<Err>(); }
	Dictionary get_info() const { return info; }
	Ref<Err> get_cause() const { return cause; }
	Array get_causes() const;
	Variant get_partial() const { return partial; }

	// Add operation context while retaining the original cause.
	Ref<Err> note(const String &p_msg, const Dictionary &p_info = Dictionary()) const;
	// Attach completed work to a new error without changing the original.
	Ref<Err> with_partial(const Variant &p_value) const;
	// Return the first matching error in the cause chain.
	bool is(Kind p_kind) const;
	Ref<Err> find_value(const Variant &p_target, const Dictionary &p_where = Dictionary()) const;
	Ref<Err> as_category(const Ref<Err> &p_kind) const;
	static String category_name(const Ref<Err> &p_kind);
	// Return a one-line description including causes.
	String text() const;
};

VARIANT_ENUM_CAST(Err::Kind);

// Support test assertions whose final exit code determines success.
// Count mismatches and reflect them in the final result.
class GDTestCheck : public RefCounted {
	GDCLASS(GDTestCheck, RefCounted);

	int failures = 0; // Number of mismatches.
	int count = 0; // Number of checks.

	void miss(const String &p_line); // Record one mismatch.
	static bool same(const Variant &p_a, const Variant &p_b);

protected:
	static void _bind_methods();

public:
	void eq(const Variant &p_got, const Variant &p_want, const String &p_label);
	void ne(const Variant &p_got, const Variant &p_other, const String &p_label);
	void ok(bool p_cond, const String &p_label);
	void no(bool p_cond, const String &p_label);
	void close_to(double p_got, double p_want, double p_slack, const String &p_label);
	void has(const Variant &p_box, const Variant &p_item, const String &p_label);
	void succeeds(const Ref<Err> &p_error, const String &p_label);
	// Supply kind to check the error category too.
	void fails(const Ref<Err> &p_error, Err::Kind p_kind, const String &p_label);
	void fails_value(const Ref<Err> &p_error, const Ref<Err> &p_kind, const String &p_label);

	int get_failures() const { return failures; }
	int get_count() const { return count; }
	int code() const { return failures == 0 ? 0 : 1; } // Return the test exit code.
	void report() const; // Print a one-line summary.
};
