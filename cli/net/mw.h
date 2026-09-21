/**************************************************************************/
/*  mw.h                                                                  */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Standard middleware for validating incoming web requests.
// Each type exposes handle(req) for direct use with GDWebApp.use.

#include "cli/net/ip.h"
#include "cli/net/serve.h"

#include "core/templates/hash_map.h"
#include "core/templates/rb_set.h"

// Common type returned by standard middleware factories.
class GDWebMiddleware : public RefCounted {
	GDCLASS(GDWebMiddleware, RefCounted);

protected:
	static void _bind_methods() {}
};

// Create and verify HS256 JWTs and attach Bearer authentication to requests.
class GDWebJwt : public GDWebMiddleware {
	GDCLASS(GDWebJwt, GDWebMiddleware);

	PackedByteArray key;
	Dictionary opts;
	String keep_name = "jwt";

	static PackedByteArray key_of(const Variant &p_key);
	static Ref<R> claims_of(const String &p_token, const Variant &p_key, const Dictionary &p_opts); // Validate token contents.
	static Ref<R> check_claims(const Dictionary &claims, const Dictionary &p_opts); // Apply application revocation checks.

protected:
	static void _bind_methods();

public:
	static Ref<R> sign(const Dictionary &p_claims, const Variant &p_key, const Dictionary &p_opts);
	static Ref<R> verify(const String &p_token, const Variant &p_key, const Dictionary &p_opts);
	static Ref<GDWebJwt> auth(const Variant &p_key, const Dictionary &p_opts);
	Variant handle(const Ref<GDWebRequest> &p_req) const;
};

// Validate JSON, query, and route values with concise rules.
class GDWebValid : public GDWebMiddleware {
	GDCLASS(GDWebValid, GDWebMiddleware);

	enum Source {
		JSON_BODY,
		QUERY,
		PARAMS,
	};

	Dictionary rule;
	String keep_name;
	Source source = JSON_BODY;

	static bool leaf(const Variant &p_value, const Dictionary &p_rule, const String &p_type, String &r_msg);
	static bool walk(const Variant &p_value, const Dictionary &p_rule, Variant &r_value, String &r_path, String &r_msg);
	static Ref<GDWebValid> make(Source p_source, const Dictionary &p_rule, const String &p_name);

protected:
	static void _bind_methods();

public:
	static Dictionary text(int64_t p_min, int64_t p_max);
	static Dictionary integer(int64_t p_min, int64_t p_max);
	static Dictionary number(double p_min, double p_max);
	static Dictionary boolean();
	static Dictionary list(const Dictionary &p_item, int64_t p_min, int64_t p_max);
	static Dictionary object(const Dictionary &p_fields, bool p_extra);
	static Dictionary optional(const Dictionary &p_rule, const Variant &p_fallback);
	static Dictionary one_of(const Array &p_values);
	static Ref<R> check(const Variant &p_value, const Dictionary &p_rule);
	static Ref<GDWebValid> json(const Dictionary &p_rule, const String &p_name);
	static Ref<GDWebValid> query(const Dictionary &p_rule, const String &p_name);
	static Ref<GDWebValid> params(const Dictionary &p_rule, const String &p_name);
	Variant handle(const Ref<GDWebRequest> &p_req) const;
};

// Limit requests per selected key within a time window.
class GDWebRateLimit : public GDWebMiddleware {
	GDCLASS(GDWebRateLimit, GDWebMiddleware);

	struct Slot {
		uint64_t due = 0; // Time when the next window may replace this one.
		int count = 0;
	};
	struct SlotTime {
		uint64_t due = 0; // Expiry time in milliseconds.
		String key; // Key distinguishing users with the same expiry.

		bool operator<(const SlotTime &p_other) const {
			return due == p_other.due ? key < p_other.key : due < p_other.due;
		}
	};
	struct ProxyNet {
		GDIP ip; // Base address of a trusted proxy network.
		int bits = 0; // Number of prefix bits to compare.
	};

	HashMap<String, Slot> slots;
	RBSet<SlotTime> slot_times; // Expiry-ordered index for visiting only expired keys.
	Callable key;
	LocalVector<ProxyNet> proxies;
	int limit = 60;
	int key_max = 10000;
	uint64_t window_ms = 60000;

	bool drop_expired(uint64_t p_now);
	bool trusted(const String &p_ip) const;
	String client_ip(const Ref<GDWebRequest> &p_req) const;

protected:
	static void _bind_methods();

public:
	static Ref<GDWebRateLimit> make(const Dictionary &p_opts);
	Variant handle(const Ref<GDWebRequest> &p_req);
};

// Reject cross-origin writes using browser-supplied Fetch Metadata.
class GDWebCSRF : public GDWebMiddleware {
	GDCLASS(GDWebCSRF, GDWebMiddleware);

	bool allow_missing = false; // Whether non-browser clients may omit metadata headers.

protected:
	static void _bind_methods();

public:
	static Ref<GDWebCSRF> make(const Dictionary &p_opts);
	Variant handle(const Ref<GDWebRequest> &p_req) const;
};

// Single-process session store with per-user counts, idle expiry, and lifetime expiry.
class GDWebSessionStore : public GDWebMiddleware {
	GDCLASS(GDWebSessionStore, GDWebMiddleware);

	struct Slot {
		Variant value;
		String user;
		uint64_t made = 0;
		uint64_t due = 0; // Earlier of idle expiry and lifetime expiry.
		uint64_t order = 0; // Unique access sequence, even within the same millisecond.
	};
	struct SlotTime {
		uint64_t due = 0; // Expiry time in milliseconds.
		String id; // ID distinguishing sessions with the same expiry.

		bool operator<(const SlotTime &p_other) const {
			return due == p_other.due ? id < p_other.id : due < p_other.due;
		}
	};
	struct SlotOrder {
		uint64_t order = 0; // Last-access sequence number.
		String id; // ID breaking ties in access order.

		bool operator<(const SlotOrder &p_other) const {
			return order == p_other.order ? id < p_other.id : order < p_other.order;
		}
	};
	struct UserOrder {
		String user; // User identifier for per-user ordering.
		uint64_t order = 0; // Last-access order within the user.
		String id; // ID breaking ties in access order.

		bool operator<(const UserOrder &p_other) const {
			if (user != p_other.user) {
				return user < p_other.user;
			}
			return order == p_other.order ? id < p_other.id : order < p_other.order;
		}
	};
	struct UserTime {
		String user; // User identifier for the per-user expiry index.
		uint64_t due = 0; // Session expiry.
		String id; // ID distinguishing equal expiry times.

		bool operator<(const UserTime &p_other) const {
			if (user != p_other.user) {
				return user < p_other.user;
			}
			return due == p_other.due ? id < p_other.id : due < p_other.due;
		}
	};

	HashMap<String, Slot> slots;
	HashMap<String, int> user_counts; // Number of retained sessions per user.
	RBSet<SlotTime> slot_times; // Index ordered by session expiry.
	RBSet<SlotOrder> slot_orders; // Global last-access index.
	RBSet<UserOrder> user_orders; // Per-user last-access index.
	RBSet<UserTime> user_times; // Per-user expiry index.
	int total_max = 1024;
	int user_max = 3;
	uint64_t idle_ms = 1800000;
	uint64_t life_ms = 43200000;
	String cookie_name = "sid";
	String keep_name = "user";
	uint64_t touch = 0; // Monotonic sequence for session creation and access.

	bool expired(const Slot &p_slot, uint64_t p_now) const;
	void index(const String &p_id, const Slot &p_slot);
	void unindex(const String &p_id, const Slot &p_slot, bool p_user);
	void erase(const String &p_id);
	bool drop_expired(uint64_t p_now);
	bool drop_expired(const String &p_user, uint64_t p_now);
	void drop_oldest();
	void drop_oldest(const String &p_user);
	String cookie_of(const String &p_header) const;

protected:
	static void _bind_methods();

public:
	static Ref<GDWebSessionStore> make(int64_t p_total, int64_t p_per_user, int64_t p_idle_seconds, int64_t p_life_seconds, const String &p_cookie, const String &p_keep);
	Ref<R> issue(const Variant &p_value);
	Ref<R> take(const String &p_id);
	void drop(const String &p_id);
	void clear();
	int size();
	String cookie(const String &p_id, bool p_secure) const;
	String clear_cookie(bool p_secure) const;
	Variant handle(const Ref<GDWebRequest> &p_req);
};
