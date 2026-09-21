/**************************************************************************/
/*  std.h                                                                 */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Core standard-library result and error types.
// Carry failures as Err and R values rather than exceptions.
// Use explicit result handling throughout script execution.
// Deliver success and failure through the same return-value path.

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

protected:
	static void _bind_methods();

public:
	static Ref<Err> make(const String &p_msg, Kind p_kind, const Dictionary &p_info = Dictionary());

	String get_msg() const { return msg; }
	Kind get_kind() const { return kind; }
	Dictionary get_info() const { return info; }
	Ref<Err> get_cause() const { return cause; }

	// Add operation context while retaining the original cause.
	Ref<Err> note(const String &p_msg) const;
	// Check whether the cause chain contains a category.
	bool is(Kind p_kind) const;
	// Return the first matching category in the cause chain.
	Ref<Err> find(Kind p_kind) const;
	// Return a one-line description including causes.
	String text() const;
};

VARIANT_ENUM_CAST(Err::Kind);

// Carry a processed value and failure reason, preserving partial completion.
// The script dialect diagnoses discarded results and supports ?, !, and destructuring.
class R : public RefCounted {
	GDCLASS(R, RefCounted);

	Variant v; // Success value, or the partial value obtained before failure.
	Ref<Err> e; // Failure reason.

protected:
	static void _bind_methods();

public:
	static Ref<Err> unpack(Variant &r_value); // Unwrap nested results while preserving failures and detecting cycles.
	static Ref<R> ok(const Variant &p_v = Variant());
	// Return a fresh success directly to a variant-valued native caller.
	static Variant okv(const Variant &p_v);
	// Create failure from text or Err with an optional partial value.
	static Ref<R> err(const Variant &p_reason, Err::Kind p_kind = Err::NONE, const Variant &p_v = Variant());

	// Return the success value or the partial value retained on failure.
	Variant get_v() const;
	Ref<Err> get_e() const { return e; }
	bool get_ok() const { return e.is_null(); }
	// Return an alternative value on failure.
	Variant v_or(const Variant &p_fallback) const;
	// Add operation context to a failure.
	Ref<R> note(const String &p_msg) const;
};

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
	void succeeds(const Ref<R> &p_r, const String &p_label);
	// Supply kind to check the error category too.
	void fails(const Ref<R> &p_r, Err::Kind p_kind, const String &p_label);

	int get_failures() const { return failures; }
	int get_count() const { return count; }
	int code() const { return failures == 0 ? 0 : 1; } // Return the test exit code.
	void report() const; // Print a one-line summary.
};
