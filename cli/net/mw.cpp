/**************************************************************************/
/*  mw.cpp                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement incoming-request validation middleware declared in mw.h.

#include "cli/net/mw.h"
#include "cli/sys/pool.h"
#include "cli/sys/clock.h"

#include "cli/data/codec.h"
#include "cli/data/json.h"
#include "cli/sys/limit.h"

#include "cli/data/digest.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"

#include <string.h>

namespace {

constexpr int JWT_KEY_BYTE_MIN = 32; // Minimum signing-key size for HS256.

// Convert an interval to an absolute deadline without overflow.
uint64_t after_ms(uint64_t p_now, uint64_t p_span) {
	return p_span > UINT64_MAX - p_now ? UINT64_MAX : p_now + p_span;
}

// Check that a cookie name contains only HTTP token characters.
bool cookie_name_ok(const String &p_name) {
	if (p_name.is_empty()) {
		return false;
	}
	static const String separators = "()<>@,;:\\\"/[]?={} \t";
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if (c <= 32 || c >= 127 || separators.contains(String::chr(c))) {
			return false;
		}
	}
	return true;
}

// Normalize an external signing key to bytes.
PackedByteArray bytes_of(const Variant &p_value) {
	return p_value.get_type() == Variant::PACKED_BYTE_ARRAY ? PackedByteArray(p_value) : Pool::text(p_value).to_utf8_buffer();
}

// Decode a JWT JSON component as a dictionary.
//
// Require the unpadded base64url spelling specified by RFC 7515.
// Alternative spellings would bypass token-text deduplication or revocation lists.
Ref<R> jwt_part(const String &p_part, const String &p_name) {
	Ref<R> decoded = Encoding::base64url_raw_decode(p_part);
	if (!decoded->get_ok()) {
		return R::err(p_name + " is not base64url", Err::INVALID_DATA);
	}
	const PackedByteArray raw = decoded->get_v();
	const Ref<R> json = JsonData::decode(raw);
	if (!json->get_ok() || json->get_v().get_type() != Variant::DICTIONARY) {
		return R::err(p_name + " is not an object", Err::INVALID_DATA);
	}
	return json;
}

// Validate numeric time claims and normalize them to seconds.
bool claim_time(const Dictionary &p_claims, const String &p_name, double &r_value) {
	if (!p_claims.has(p_name)) {
		return false;
	}
	const Variant v = p_claims[p_name];
	if (v.get_type() != Variant::INT && v.get_type() != Variant::FLOAT) {
		return false;
	}
	r_value = v;
	return Math::is_finite(r_value);
}

// Create a consistently typed validation failure.
Ref<R> invalid(const String &p_path, const String &p_msg) {
	return R::err(p_path + " " + p_msg, Err::INVALID_DATA);
}

// Identify shared container storage, returning null for scalar values.
const void *container_id(const Variant &p_value) {
	if (p_value.get_type() == Variant::ARRAY) {
		return Array(p_value).id();
	}
	return p_value.get_type() == Variant::DICTIONARY ? Dictionary(p_value).id() : nullptr;
}

// Check added values for cycles, visiting shared substructures only once.
bool nested_ok(const Variant &p_value, HashSet<const void *> *p_seen = nullptr) {
	struct Frame {
		Variant value; // Container being inspected.
		Array keys; // Dictionary traversal order.
		int at = 0; // Next element to inspect.
	};
	LocalVector<Frame> stack;
	HashSet<const void *> active; // Containers on the current traversal path.
	HashSet<const void *> local; // Visited set for a standalone check.
	HashSet<const void *> &seen = p_seen ? *p_seen : local; // Previously checked shared containers.
	Variant next = p_value;
	for (;;) {
		const void *id = container_id(next);
		if (id && !seen.has(id)) {
			if (active.has(id)) {
				return false;
			}
			active.insert(id);
			stack.push_back(Frame{ next, next.get_type() == Variant::DICTIONARY ? Dictionary(next).keys() : Array(), 0 });
		}
		bool child = false;
		while (!stack.is_empty()) {
			Frame &frame = stack[stack.size() - 1];
			const bool list = frame.value.get_type() == Variant::ARRAY;
			const int size = list ? Array(frame.value).size() : frame.keys.size();
			if (frame.at < size) {
				next = list ? Array(frame.value)[frame.at] : Dictionary(frame.value)[frame.keys[frame.at]];
				frame.at++;
				child = true;
				break;
			}
			id = container_id(frame.value);
			active.erase(id);
			seen.insert(id);
			stack.remove_at(stack.size() - 1);
		}
		if (!child) {
			return true;
		}
	}
}

// Compare candidate values iteratively without recursive comparison of deep containers.
bool same_value(const Variant &p_left, const Variant &p_right) {
	struct Frame {
		Variant left; // Input container being compared.
		Variant right; // Candidate container.
		Array keys; // Dictionary comparison order.
		int at = 0; // Next element to compare.
	};
	LocalVector<Frame> stack;
	HashMap<const void *, HashSet<const void *>> seen; // Previously compared pairs of shared references.
	Variant left = p_left;
	Variant right = p_right;
	for (;;) {
		const void *id = container_id(left);
		const void *other = container_id(right);
		if (id || other) {
			if (!id || !other || left.get_type() != right.get_type()) {
				return false;
			}
			if (!seen[id].has(other)) {
				seen[id].insert(other);
				const bool array = left.get_type() == Variant::ARRAY;
				const int size = array ? Array(left).size() : Dictionary(left).size();
				if (size != (array ? Array(right).size() : Dictionary(right).size())) {
					return false;
				}
				stack.push_back(Frame{ left, right, array ? Array() : Dictionary(left).keys(), 0 });
			}
		} else if (left != right) {
			return false;
		}
		bool child = false;
		while (!stack.is_empty()) {
			Frame &frame = stack[stack.size() - 1];
			const bool array = frame.left.get_type() == Variant::ARRAY;
			const int size = array ? Array(frame.left).size() : frame.keys.size();
			if (frame.at < size) {
				if (array) {
					left = Array(frame.left)[frame.at];
					right = Array(frame.right)[frame.at];
				} else {
					const Variant key = frame.keys[frame.at];
					const Dictionary object = frame.right;
					if (!object.has(key)) {
						return false;
					}
					left = Dictionary(frame.left)[key];
					right = object[key];
				}
				frame.at++;
				child = true;
				break;
			}
			stack.remove_at(stack.size() - 1);
		}
		if (!child) {
			return true;
		}
	}
}

// Pass length-delimited UTF-8 to the pure IP parser without changing the basic string type.
GDIP parse_ip(const String &p_text) {
	const CharString text = p_text.utf8();
	return GDIP::parse(std::string_view(text.get_data(), text.length()));
}

} // namespace

// ---------------- Middleware factories ----------------

Ref<R> GDWebApp::jwt_sign(const Dictionary &p_claims, const Variant &p_key, const Dictionary &p_opts) {
	return GDWebJwt::sign(p_claims, p_key, p_opts);
}

// Verify JWT signatures and claims.
Ref<R> GDWebApp::jwt_verify(const String &p_token, const Variant &p_key, const Dictionary &p_opts) {
	return GDWebJwt::verify(p_token, p_key, p_opts);
}

// Create Bearer JWT authentication middleware.
Ref<GDWebMiddleware> GDWebApp::jwt(const Variant &p_key, const Dictionary &p_opts) {
	return GDWebJwt::auth(p_key, p_opts);
}

// Create middleware restricting browser-originated state changes to the same origin.
Ref<GDWebMiddleware> GDWebApp::csrf(const Dictionary &p_opts) {
	return GDWebCSRF::make(p_opts);
}

// Create an expiring session store.
Ref<GDWebSessionStore> GDWebApp::sessions(int64_t p_total, int64_t p_per_user, int64_t p_idle_seconds, int64_t p_life_seconds, const String &p_cookie, const String &p_keep) {
	return GDWebSessionStore::make(p_total, p_per_user, p_idle_seconds, p_life_seconds, p_cookie, p_keep);
}

// Create a text validation rule.
Dictionary GDWebApp::rule_text(int64_t p_min, int64_t p_max) {
	return GDWebValid::text(p_min, p_max);
}

// Create an integer validation rule.
Dictionary GDWebApp::rule_integer(int64_t p_min, int64_t p_max) {
	return GDWebValid::integer(p_min, p_max);
}

// Create a numeric validation rule.
Dictionary GDWebApp::rule_number(double p_min, double p_max) {
	return GDWebValid::number(p_min, p_max);
}

// Create a boolean validation rule.
Dictionary GDWebApp::rule_boolean() {
	return GDWebValid::boolean();
}

// Create a list validation rule.
Dictionary GDWebApp::rule_list(const Dictionary &p_item, int64_t p_min, int64_t p_max) {
	return GDWebValid::list(p_item, p_min, p_max);
}

// Create an object validation rule.
Dictionary GDWebApp::rule_object(const Dictionary &p_fields, bool p_extra) {
	return GDWebValid::object(p_fields, p_extra);
}

// Create an optional-value validation rule.
Dictionary GDWebApp::rule_optional(const Dictionary &p_rule, const Variant &p_fallback) {
	return GDWebValid::optional(p_rule, p_fallback);
}

// Create a one_of validation rule.
Dictionary GDWebApp::rule_one_of(const Array &p_values) {
	return GDWebValid::one_of(p_values);
}

// Validate a value and return only its accepted content.
Ref<R> GDWebApp::validate(const Variant &p_value, const Dictionary &p_rule) {
	return GDWebValid::check(p_value, p_rule);
}

// Create JSON validation middleware.
Ref<GDWebMiddleware> GDWebApp::valid_json(const Dictionary &p_rule, const String &p_name) {
	return GDWebValid::json(p_rule, p_name);
}

// Create query validation middleware.
Ref<GDWebMiddleware> GDWebApp::valid_query(const Dictionary &p_rule, const String &p_name) {
	return GDWebValid::query(p_rule, p_name);
}

// Create route-parameter validation middleware.
Ref<GDWebMiddleware> GDWebApp::valid_params(const Dictionary &p_rule, const String &p_name) {
	return GDWebValid::params(p_rule, p_name);
}

// ---------------- JWT ----------------

// Normalize the signing key to bytes.
PackedByteArray GDWebJwt::key_of(const Variant &p_key) {
	return bytes_of(p_key);
}

// Sign claims with HS256, adding iat and exp only when ttl is supplied.
Ref<R> GDWebJwt::sign(const Dictionary &p_claims, const Variant &p_key, const Dictionary &p_opts) {
	const PackedByteArray secret = key_of(p_key);
	if (secret.size() < JWT_KEY_BYTE_MIN) {
		return R::err("JWT key must be at least 32 bytes", Err::INVALID_DATA);
	}
	Dictionary claims = p_claims.duplicate(false);
	const int64_t ttl = p_opts.get("ttl", 900);
	if (ttl < 0) {
		return R::err("JWT ttl must not be negative", Err::INVALID_DATA);
	}
	if (ttl > 0) {
		const int64_t now = (int64_t)GDClock::unix_time();
		if (ttl > INT64_MAX - now) {
			return R::err("JWT ttl is too large", Err::INVALID_DATA);
		}
		if (!claims.has("iat")) {
			claims["iat"] = now;
		}
		if (!claims.has("exp")) {
			claims["exp"] = now + ttl;
		}
	}
	Dictionary head;
	head["alg"] = "HS256";
	head["typ"] = "JWT";
	const Dictionary json_opts = { { "deterministic", true } };
	const Ref<R> head_json = JsonData::encode(head, json_opts);
	const Ref<R> claims_json = JsonData::encode(claims, json_opts);
	if (!head_json->get_ok() || !claims_json->get_ok()) {
		return R::err("JWT claims contain a value that JSON cannot encode", Err::INVALID_DATA);
	}
	const String a = Encoding::base64url_encode(head_json->get_v());
	const String b = Encoding::base64url_encode(claims_json->get_v());
	const String body = a + "." + b;
	const String sig = Encoding::base64url_encode(Hash::hmac_sha256(secret, body.to_utf8_buffer()));
	return R::ok(body + "." + sig);
}

// Verify the signature and standard time claims, accepting only HS256.
Ref<R> GDWebJwt::claims_of(const String &p_token, const Variant &p_key, const Dictionary &p_opts) {
	const PackedByteArray secret = key_of(p_key);
	if (secret.size() < JWT_KEY_BYTE_MIN) {
		return R::err("JWT key must be at least 32 bytes", Err::INVALID_DATA);
	}
	const int first = p_token.find(".");
	const int second = first < 0 ? -1 : p_token.find(".", first + 1);
	if (second < 0 || p_token.find(".", second + 1) >= 0) {
		return R::err("JWT must have three parts", Err::INVALID_DATA);
	}
	const String parts[] = { p_token.substr(0, first), p_token.substr(first + 1, second - first - 1), p_token.substr(second + 1) }; // Retain only the three required token components.
	Ref<R> got = jwt_part(parts[0], "JWT header");
	if (!got->get_ok()) {
		return got;
	}
	const Dictionary head = got->get_v();
	if (head.get("alg", "") != "HS256") {
		return R::err("JWT algorithm is not HS256", Err::INVALID_DATA);
	}
	got = jwt_part(parts[1], "JWT claims");
	if (!got->get_ok()) {
		return got;
	}
	const Dictionary claims = got->get_v();
	Ref<R> sig_r = Encoding::base64url_raw_decode(parts[2]);
	if (!sig_r->get_ok()) {
		return R::err("JWT signature is not base64url", Err::INVALID_DATA);
	}
	const PackedByteArray actual = sig_r->get_v();
	const PackedByteArray want = Hash::hmac_sha256(secret, (parts[0] + "." + parts[1]).to_utf8_buffer());
	if (!Hash::equal_ct(actual, want)) {
		return R::err("JWT signature does not match", Err::INVALID_DATA);
	}

	const double now = GDClock::unix_time();
	const double leeway = p_opts.get("leeway", 0.0);
	if (!Math::is_finite(leeway) || leeway < 0.0) {
		return R::err("JWT leeway must be a finite non-negative number", Err::INVALID_DATA);
	}
	double at = 0.0;
	if (!claim_time(claims, "exp", at)) {
		if ((bool)p_opts.get("require_exp", true)) {
			return R::err("JWT has no numeric exp", Err::INVALID_DATA);
		}
	} else if (now - leeway >= at) {
		return R::err("JWT expired", Err::TIMED_OUT);
	}
	if (claims.has("nbf")) {
		if (!claim_time(claims, "nbf", at) || now + leeway < at) {
			return R::err("JWT is not active", Err::INVALID_DATA);
		}
	}
	const String iss = Pool::text(p_opts.get("iss", ""));
	if (!iss.is_empty() && claims.get("iss", "") != iss) {
		return R::err("JWT issuer does not match", Err::INVALID_DATA);
	}
	const String aud = Pool::text(p_opts.get("aud", ""));
	if (!aud.is_empty()) {
		const Variant claim = claims.get("aud", Variant());
		bool matched = claim.get_type() == Variant::STRING && claim == aud;
		if (claim.get_type() == Variant::ARRAY) {
			matched = Array(claim).has(aud);
		}
		if (!matched) {
			return R::err("JWT audience does not match", Err::INVALID_DATA);
		}
	}
	return R::ok(claims);
}

// Apply application revocation checks without replacing operational failures.
Ref<R> GDWebJwt::check_claims(const Dictionary &claims, const Dictionary &p_opts) {
	const Variant check_value = p_opts.get("check", Callable());
	const Callable check = check_value.get_type() == Variant::CALLABLE ? Callable(check_value) : Callable();
	if (p_opts.has("check")) {
		if (!check.is_valid()) {
			return R::err("JWT check must be a valid Callable", Err::INVALID_DATA);
		}
		Callable::CallError err;
		const Variant arg = claims;
		const Variant *args[] = { &arg };
		Variant accepted;
		check.callp(args, 1, accepted, err);
		if (err.error != Callable::CallError::CALL_OK) return R::err("JWT check could not be invoked", Err::INVALID_DATA);
		const Ref<Err> failed = R::unpack(accepted);
		if (failed.is_valid()) return R::err(failed);
		if (accepted.get_type() != Variant::BOOL) return R::err("JWT check must return a bool", Err::INVALID_DATA);
		if (!(bool)accepted) return R::err("JWT was revoked", Err::UNAUTHENTICATED);
	}
	return R::ok(claims);
}

// Validate token contents before applying the application check.
Ref<R> GDWebJwt::verify(const String &p_token, const Variant &p_key, const Dictionary &p_opts) {
	const Ref<R> got = claims_of(p_token, p_key, p_opts);
	return got->get_ok() ? check_claims(got->get_v(), p_opts) : got;
}

// Create Bearer authentication; accepted claims are available through req.kept.
Ref<GDWebJwt> GDWebJwt::auth(const Variant &p_key, const Dictionary &p_opts) {
	Ref<GDWebJwt> out;
	out.instantiate();
	out->key = key_of(p_key);
	out->opts = p_opts.duplicate(true);
	out->keep_name = p_opts.get("keep", "jwt");
	return out;
}

// Validate or transform input before passing it to the next handler.
Variant GDWebJwt::handle(const Ref<GDWebRequest> &p_req) const {
	const String value = p_req->header("authorization").strip_edges();
	const int space = value.find(" ");
	if (space < 0 || value.substr(0, space).to_lower() != "bearer") {
		return Err::make("Bearer token required", Err::UNAUTHENTICATED);
	}
	Ref<R> got = claims_of(value.substr(space + 1).strip_edges(), key, opts);
	if (!got->get_ok()) {
		return got->get_e()->as_kind(Err::UNAUTHENTICATED);
	}
	got = check_claims(got->get_v(), opts);
	if (!got->get_ok()) return got->get_e();
	p_req->keep(keep_name, got->get_v());
	return Variant();
}

// Register public script methods and properties.
void GDWebJwt::_bind_methods() {
	ClassDB::bind_method(D_METHOD("handle", "req"), &GDWebJwt::handle);
}

// ---------------- Input validation ----------------

Dictionary GDWebValid::text(int64_t p_min, int64_t p_max) {
	return { { "type", "text" }, { "min", p_min }, { "max", p_max } };
}

// Create an integer validation rule.
Dictionary GDWebValid::integer(int64_t p_min, int64_t p_max) {
	return { { "type", "int" }, { "min", p_min }, { "max", p_max } };
}

// Create a numeric validation rule.
Dictionary GDWebValid::number(double p_min, double p_max) {
	return { { "type", "number" }, { "min", p_min }, { "max", p_max } };
}

// Create a boolean validation rule.
Dictionary GDWebValid::boolean() {
	return { { "type", "bool" } };
}

// Create a list validation rule.
Dictionary GDWebValid::list(const Dictionary &p_item, int64_t p_min, int64_t p_max) {
	return { { "type", "list" }, { "item", p_item }, { "min", p_min }, { "max", p_max } };
}

// Create an object validation rule.
Dictionary GDWebValid::object(const Dictionary &p_fields, bool p_extra) {
	return { { "type", "object" }, { "fields", p_fields }, { "extra", p_extra } };
}

// Create an optional-value validation rule.
Dictionary GDWebValid::optional(const Dictionary &p_rule, const Variant &p_fallback) {
	Dictionary out = p_rule.duplicate(false);
	out["optional"] = true;
	out["fallback"] = p_fallback;
	return out;
}

// Create a one_of validation rule.
Dictionary GDWebValid::one_of(const Array &p_values) {
	return { { "type", "one" }, { "values", p_values } };
}

// Validate scalar rules within their specified ranges.
bool GDWebValid::leaf(const Variant &p_value, const Dictionary &p_rule, const String &p_type, String &r_msg) {
	if (p_type == "text") {
		if (p_value.get_type() != Variant::STRING) {
			r_msg = "must be text";
			return false;
		}
		const String value = p_value;
		const int64_t min = p_rule.get("min", 0);
		const int64_t max = p_rule.get("max", 4096);
		if (min < 0 || max < min || max > INT_MAX || value.length() < min || value.length() > max) {
			r_msg = "has an invalid length";
			return false;
		}
		return true;
	}
	if (p_type == "int") {
		if (p_value.get_type() != Variant::INT) {
			r_msg = "must be an integer";
			return false;
		}
		const int64_t value = p_value;
		const int64_t min = p_rule.get("min", INT64_MIN);
		const int64_t max = p_rule.get("max", INT64_MAX);
		if (max < min || value < min || value > max) {
			r_msg = "is out of range";
			return false;
		}
		return true;
	}
	if (p_type == "number") {
		if (p_value.get_type() != Variant::INT && p_value.get_type() != Variant::FLOAT) {
			r_msg = "must be a number";
			return false;
		}
		const double value = p_value;
		const double min = p_rule.get("min", -1e308);
		const double max = p_rule.get("max", 1e308);
		if (!Math::is_finite(value) || !Math::is_finite(min) || !Math::is_finite(max) || max < min || value < min || value > max) {
			r_msg = "is out of range";
			return false;
		}
		return true;
	}
	if (p_type == "bool") {
		if (p_value.get_type() != Variant::BOOL) {
			r_msg = "must be true or false";
			return false;
		}
		return true;
	}
	if (p_type == "one") {
		const Array values = p_rule.get("values", Array());
		if (!nested_ok(p_value)) {
			r_msg = "contains a cycle";
			return false;
		}
		for (const Variant &value : values) {
			if (same_value(p_value, value)) {
				return true;
			}
		}
		r_msg = "is not allowed";
		return false;
	}
	r_msg = "has an unknown rule";
	return false;
}

// Traverse rules with an explicit stack, checking shared input once per rule.
bool GDWebValid::walk(const Variant &p_value, const Dictionary &p_rule, Variant &r_value, String &r_path, String &r_msg) {
	struct Frame {
		Variant value; // Input container.
		Dictionary rule; // Rule being applied.
		Dictionary fields; // Array-element rules or object-field rules.
		Variant out; // Container receiving validated elements.
		Array keys; // Object-rule traversal order.
		Variant key; // Position at which to place the result in its parent.
		int at = 0; // Next child element.
	};
	LocalVector<Frame> stack;
	HashSet<const void *> active; // Input containers on the current validation path.
	HashSet<const void *> extras; // Visited shared extras, avoiding repeated validation.
	HashMap<const void *, HashMap<const void *, Variant>> memo; // Validated results indexed by input and rule.
	Variant value = p_value;
	Dictionary rule = p_rule;
	Variant key;
	Variant out;
	for (;;) {
		const String type = Pool::text(rule.get("type", ""));
		const bool list = type == "list";
		bool ready = false;
		if (list || type == "object") {
			if (value.get_type() != (list ? Variant::ARRAY : Variant::DICTIONARY)) {
				r_msg = list ? "must be a list" : "must be an object";
				break;
			}
			const void *id = container_id(value);
			if (active.has(id)) {
				r_msg = "contains a cycle";
				break;
			}
			const auto *known = memo.getptr(id);
			const Variant *cached = known ? known->getptr(rule.id()) : nullptr;
			if (cached) {
				out = *cached;
				ready = true;
			} else {
				Array keys;
				Dictionary fields;
				if (list) {
					const int size = Array(value).size();
					const int64_t min = rule.get("min", 0);
					const int64_t max = rule.get("max", 1024);
					if (min < 0 || max < min || max > INT_MAX || size < min || size > max) {
						r_msg = "has an invalid size";
						break;
					}
					Array items;
					items.resize(size);
					out = items;
					fields = rule.get("item", Dictionary());
				} else {
					fields = rule.get("fields", Dictionary());
					keys = fields.keys();
					Dictionary fields_out;
					if ((bool)rule.get("extra", false)) {
						for (const KeyValue<Variant, Variant> &kv : Dictionary(value)) {
							if (!fields.has(kv.key)) {
								if (!nested_ok(kv.value, &extras)) {
									r_path = "." + Pool::text(kv.key);
									r_msg = "contains a cycle";
									break;
								}
								fields_out[kv.key] = kv.value;
							}
						}
					}
					if (!r_msg.is_empty()) {
						break;
					}
					out = fields_out;
				}
				active.insert(id);
				stack.push_back(Frame{ value, rule, fields, out, keys, key, 0 });
			}
		} else {
			if (!leaf(value, rule, type, r_msg)) {
				break;
			}
			out = value;
			ready = true;
		}

		// Return the child's result to its parent and advance to the next unchecked child.
		bool child = false;
		while (!stack.is_empty()) {
			Frame &frame = stack[stack.size() - 1];
			const bool array = frame.value.get_type() == Variant::ARRAY;
			if (ready) {
				if (array) {
					Array(frame.out)[int(key)] = out;
				} else {
					Dictionary(frame.out)[key] = out;
				}
				ready = false;
			}
			const int size = array ? Array(frame.value).size() : frame.keys.size();
			if (frame.at < size) {
				key = array ? Variant(frame.at) : frame.keys[frame.at];
				frame.at++;
				if (array) {
					value = Array(frame.value)[int(key)];
					rule = frame.fields;
				} else {
					rule = frame.fields[key];
					const Dictionary object = frame.value;
					if (!object.has(key)) {
						if (!(bool)rule.get("optional", false)) {
							r_msg = "is required";
							break;
						}
						Dictionary(frame.out)[key] = rule.get("fallback", Variant());
						continue;
					}
					value = object[key];
				}
				child = true;
				break;
			}
			out = frame.out;
			key = frame.key;
			const void *id = container_id(frame.value);
			memo[id].insert(frame.rule.id(), out);
			active.erase(id);
			stack.remove_at(stack.size() - 1);
			ready = true;
		}
		if (!r_msg.is_empty()) {
			break;
		}
		if (!child) {
			r_value = out;
			return true;
		}
	}
	// Build the root-to-element path only on failure.
	String path;
	for (const Frame &frame : stack) {
		if (frame.at > 0) {
			path += frame.value.get_type() == Variant::ARRAY ? vformat("[%d]", frame.at - 1) : "." + Pool::text(frame.keys[frame.at - 1]);
		}
	}
	r_path = path + r_path;
	return false;
}

// Check whether input satisfies the rule.
Ref<R> GDWebValid::check(const Variant &p_value, const Dictionary &p_rule) {
	Variant out;
	String path;
	String msg;
	return walk(p_value, p_rule, out, path, msg) ? R::ok(out) : invalid("value" + path, msg);
}

// Validate configuration and construct a new value.
Ref<GDWebValid> GDWebValid::make(Source p_source, const Dictionary &p_rule, const String &p_name) {
	Ref<GDWebValid> out;
	out.instantiate();
	out->source = p_source;
	out->rule = p_rule;
	out->keep_name = p_name;
	return out;
}

// Decode retained content as JSON.
Ref<GDWebValid> GDWebValid::json(const Dictionary &p_rule, const String &p_name) {
	return make(JSON_BODY, p_rule, p_name);
}

// Evaluate the query and return its result.
Ref<GDWebValid> GDWebValid::query(const Dictionary &p_rule, const String &p_name) {
	return make(QUERY, p_rule, p_name);
}

// Create a route-parameter validation rule.
Ref<GDWebValid> GDWebValid::params(const Dictionary &p_rule, const String &p_name) {
	return make(PARAMS, p_rule, p_name);
}

// Validate or transform input before passing it to the next handler.
Variant GDWebValid::handle(const Ref<GDWebRequest> &p_req) const {
	Variant value;
	switch (source) {
		case JSON_BODY: {
			return p_req->json_valid(rule, keep_name);
		}
		case QUERY:
			value = p_req->get_query();
			break;
		case PARAMS:
			value = p_req->get_params();
			break;
	}
	Ref<R> got = check(value, rule);
	if (!got->get_ok()) {
		return got->get_e();
	}
	p_req->keep("valid:" + keep_name, got->get_v());
	return Variant();
}

// Register public script methods and properties.
void GDWebValid::_bind_methods() {
	ClassDB::bind_method(D_METHOD("handle", "req"), &GDWebValid::handle);
}

// ---------------- Rate limiting ----------------

Ref<GDWebRateLimit> GDWebRateLimit::make(const Dictionary &p_opts) {
	const int64_t limit = p_opts.get("limit", 60);
	const int64_t keys = p_opts.get("keys", 10000);
	const double window = p_opts.get("window", 60.0);
	uint64_t window_ms = 0;
	if (limit < 1 || limit > INT_MAX || keys < 1 || keys > INT_MAX || window <= 0.0 || !Limit::seconds_ms(window, window_ms)) {
		ERR_PRINT("rate limit settings are out of range");
		return Ref<GDWebRateLimit>();
	}
	Ref<GDWebRateLimit> out;
	out.instantiate();
	out->limit = (int)limit;
	out->key_max = (int)keys;
	out->window_ms = window_ms;
	out->key = p_opts.get("key", Callable());
	PackedStringArray proxies;
	const Variant proxy_value = p_opts.get("trusted_proxies", PackedStringArray());
	if (proxy_value.get_type() == Variant::PACKED_STRING_ARRAY) {
		proxies = proxy_value;
	} else if (proxy_value.get_type() == Variant::ARRAY) {
		const Array values = proxy_value;
		for (const Variant &value : values) {
			if (value.get_type() != Variant::STRING) {
				ERR_PRINT("trusted proxies must be IP address strings");
				return Ref<GDWebRateLimit>();
			}
			proxies.push_back(value);
		}
	} else {
		ERR_PRINT("trusted_proxies must be an array");
		return Ref<GDWebRateLimit>();
	}
	for (const String &entry : proxies) {
		const int slash = entry.find("/");
		const String text = slash < 0 ? entry : entry.left(slash);
		ProxyNet proxy;
		proxy.ip = parse_ip(text);
		if (!proxy.ip.bit_len() || proxy.ip.zoned()) {
			ERR_PRINT("trusted proxy must be an IP address or CIDR");
			return Ref<GDWebRateLimit>();
		}
		const int max_bits = proxy.ip.bit_len();
		proxy.bits = slash < 0 ? max_bits : entry.substr(slash + 1).to_int();
		if (slash >= 0 && (entry.substr(slash + 1).is_empty() || proxy.bits < 0 || proxy.bits > max_bits || String::num_int64(proxy.bits) != entry.substr(slash + 1))) {
			ERR_PRINT("trusted proxy CIDR prefix is out of range");
			return Ref<GDWebRateLimit>();
		}
		out->proxies.push_back(proxy);
	}
	return out;
}

// Check the actual peer against explicitly trusted proxy networks.
bool GDWebRateLimit::trusted(const String &p_ip) const {
	const GDIP ip = parse_ip(p_ip);
	for (const ProxyNet &proxy : proxies) {
		if (proxy.ip.contains(ip, proxy.bits)) {
			return true;
		}
	}
	return false;
}

// Remove trusted proxies from the right to select the first untrusted client address.
String GDWebRateLimit::client_ip(const Ref<GDWebRequest> &p_req) const {
	const String peer = p_req->get_ip();
	if (proxies.is_empty() || !trusted(peer)) {
		return peer;
	}
	const String forwarded = p_req->header("x-forwarded-for");
	String current = peer;
	int end = forwarded.length();
	while (end > 0 && trusted(current)) {
		const int comma = forwarded.rfind(",", end - 1);
		const String candidate = forwarded.substr(comma + 1, end - comma - 1).strip_edges();
		const GDIP ip = parse_ip(candidate);
		if (!ip.bit_len()) {
			return peer;
		}
		const std::string text = ip.text();
		current = String::utf8(text.data(), text.size());
		end = comma < 0 ? 0 : comma;
	}
	return current;
}

// Remove one expired key from the expiry-ordered index.
bool GDWebRateLimit::drop_expired(uint64_t p_now) {
	if (slot_times.is_empty() || slot_times.front()->get().due > p_now) {
		return false;
	}
	const SlotTime timed = slot_times.front()->get();
	slot_times.erase(timed);
	slots.erase(timed.key);
	return true;
}

// Consume one request allowance, returning a failure mapped to 429 when exhausted.
Variant GDWebRateLimit::handle(const Ref<GDWebRequest> &p_req) {
	String name = client_ip(p_req);
	if (key.is_valid()) {
		Callable::CallError err;
		const Variant arg = p_req;
		const Variant *args[] = { &arg };
		Variant result;
		key.callp(args, 1, result, err);
		if (err.error != Callable::CallError::CALL_OK) return Err::make("rate limit key failed", Err::INVALID_DATA);
		const Ref<Err> failed = R::unpack(result);
		if (failed.is_valid()) return failed;
		if (result.get_type() != Variant::STRING) return Err::make("rate limit key must return text", Err::INVALID_DATA);
		name = result;
	}
	const uint64_t now = GDClock::msec();
	Slot *slot = slots.getptr(name);
	if (slot && slot->due <= now) {
		slot_times.erase(SlotTime{ slot->due, name });
		slot->due = after_ms(now, window_ms);
		slot->count = 0;
		slot_times.insert(SlotTime{ slot->due, name });
	}
	if (!slot) {
		if ((int)slots.size() >= key_max) {
			drop_expired(now);
		}
		if ((int)slots.size() >= key_max) {
			return Err::make("rate limit key capacity reached", Err::LIMITED);
		}
		Slot fresh;
		fresh.due = after_ms(now, window_ms);
		slots.insert(name, fresh);
		slot_times.insert(SlotTime{ fresh.due, name });
		slot = slots.getptr(name);
	}
	if (slot->count >= limit) {
		return Err::make("rate limit exceeded", Err::LIMITED);
	}
	slot->count++;
	return Variant();
}

// Register public script methods and properties.
void GDWebRateLimit::_bind_methods() {
	ClassDB::bind_method(D_METHOD("handle", "req"), &GDWebRateLimit::handle);
}

// ---------------- CSRF ----------------

// Create Fetch Metadata CSRF middleware from configuration.
Ref<GDWebCSRF> GDWebCSRF::make(const Dictionary &p_opts) {
	Ref<GDWebCSRF> out;
	out.instantiate();
	out->allow_missing = p_opts.get("allow_missing", false);
	return out;
}

// Allow state-changing browser requests only when metadata establishes the same origin.
Variant GDWebCSRF::handle(const Ref<GDWebRequest> &p_req) const {
	const String method = p_req->get_method();
	if (method == "GET" || method == "HEAD" || method == "OPTIONS") {
		return Variant();
	}
	const String site = p_req->header("sec-fetch-site").to_lower();
	if (site == "same-origin" || (site.is_empty() && allow_missing)) {
		return Variant();
	}
	return Err::make("same-origin request required", Err::PERMISSION_DENIED);
}

// Register public script methods and properties.
void GDWebCSRF::_bind_methods() {
	ClassDB::bind_method(D_METHOD("handle", "req"), &GDWebCSRF::handle);
}

// ---------------- Sessions ----------------

Ref<GDWebSessionStore> GDWebSessionStore::make(int64_t p_total, int64_t p_per_user, int64_t p_idle_seconds, int64_t p_life_seconds, const String &p_cookie, const String &p_keep) {
	Ref<GDWebSessionStore> out;
	out.instantiate();
	if (p_total < 1 || p_total > INT_MAX || p_per_user < 1 || p_per_user > INT_MAX || p_idle_seconds < 1 || p_idle_seconds > Limit::SEC_MAX || p_life_seconds < 1 || p_life_seconds > Limit::SEC_MAX || !cookie_name_ok(p_cookie)) {
		ERR_PRINT("session settings are out of range");
		out->total_max = 0;
		return out;
	}
	out->total_max = p_total;
	out->user_max = p_per_user;
	out->idle_ms = uint64_t(p_idle_seconds) * 1000;
	out->life_ms = uint64_t(p_life_seconds) * 1000;
	out->cookie_name = p_cookie;
	out->keep_name = p_keep;
	return out;
}

// Check whether a stored value has expired.
bool GDWebSessionStore::expired(const Slot &p_slot, uint64_t p_now) const {
	return p_slot.due <= p_now;
}

// Index a session by expiry, global access order, and per-user access order.
void GDWebSessionStore::index(const String &p_id, const Slot &p_slot) {
	slot_times.insert(SlotTime{ p_slot.due, p_id });
	slot_orders.insert(SlotOrder{ p_slot.order, p_id });
	user_orders.insert(UserOrder{ p_slot.user, p_slot.order, p_id });
	user_times.insert(UserTime{ p_slot.user, p_slot.due, p_id });
	int *count = user_counts.getptr(p_slot.user);
	if (count) {
		(*count)++;
	} else {
		user_counts.insert(p_slot.user, 1);
	}
}

// Remove session indexes, preserving user counts when refreshing access.
void GDWebSessionStore::unindex(const String &p_id, const Slot &p_slot, bool p_user) {
	slot_times.erase(SlotTime{ p_slot.due, p_id });
	slot_orders.erase(SlotOrder{ p_slot.order, p_id });
	user_orders.erase(UserOrder{ p_slot.user, p_slot.order, p_id });
	user_times.erase(UserTime{ p_slot.user, p_slot.due, p_id });
	if (!p_user) {
		return;
	}
	int *count = user_counts.getptr(p_slot.user);
	if (count && --(*count) == 0) {
		user_counts.erase(p_slot.user);
	}
}

// Remove the selected session from storage and all indexes.
void GDWebSessionStore::erase(const String &p_id) {
	Slot *slot = slots.getptr(p_id);
	if (!slot) {
		return;
	}
	const Slot copy = *slot;
	unindex(p_id, copy, true);
	slots.erase(p_id);
}

// Remove one expired session from the expiry index.
bool GDWebSessionStore::drop_expired(uint64_t p_now) {
	if (slot_times.is_empty() || slot_times.front()->get().due > p_now) {
		return false;
	}
	const String id = slot_times.front()->get().id;
	erase(id);
	return true;
}

// Remove one expired session from the user's expiry index.
bool GDWebSessionStore::drop_expired(const String &p_user, uint64_t p_now) {
	auto *first = user_times.lower_bound(UserTime{ p_user, 0, String() });
	if (!first || first->get().user != p_user || first->get().due > p_now) {
		return false;
	}
	const String id = first->get().id;
	erase(id);
	return true;
}

// Evict the globally least recently accessed session.
void GDWebSessionStore::drop_oldest() {
	if (!slot_orders.is_empty()) {
		const String id = slot_orders.front()->get().id;
		erase(id);
	}
}

// Evict the user's least recently accessed session.
void GDWebSessionStore::drop_oldest(const String &p_user) {
	RBSet<UserOrder>::Element *first = user_orders.lower_bound(UserOrder{ p_user, 0, String() });
	if (first && first->get().user == p_user) {
		const String id = first->get().id;
		erase(id);
	}
}

// Issue a new ID, replacing older sessions when the configured capacity is reached.
Ref<R> GDWebSessionStore::issue(const Variant &p_value) {
	if (total_max < 1) {
		return R::err("session settings are out of range", Err::INVALID_DATA);
	}
	const Variant::Type value_type = p_value.get_type();
	if (value_type != Variant::STRING && value_type != Variant::STRING_NAME && value_type != Variant::INT) {
		return R::err("session value must be a text or integer identifier", Err::INVALID_DATA);
	}
	const String user = String(p_value);
	const uint64_t now = GDClock::msec();
	String id;
	do {
		const PackedByteArray raw = GDDigest::random(32);
		if (raw.size() != 32) return R::err("cannot generate session identifier");
		id = Encoding::base64url_encode(raw);
	} while (slots.has(id));
	const int *count = user_counts.getptr(user);
	if (count && *count >= user_max) {
		if (!drop_expired(user, now)) {
			drop_oldest(user);
		}
	}
	if ((int)slots.size() >= total_max) {
		if (!drop_expired(now)) {
			drop_oldest();
		}
	}
	Slot slot;
	slot.value = p_value;
	slot.user = user;
	slot.made = now;
	slot.due = MIN(after_ms(now, idle_ms), after_ms(now, life_ms));
	slot.order = ++touch;
	slots.insert(id, slot);
	index(id, slot);
	return R::ok(id);
}

// Return a valid session and extend its idle deadline.
Ref<R> GDWebSessionStore::take(const String &p_id) {
	const uint64_t now = GDClock::msec();
	Slot *slot = slots.getptr(p_id);
	if (!slot || expired(*slot, now)) {
		if (slot) {
			erase(p_id);
		}
		return R::err("session is missing or expired", Err::UNAUTHENTICATED);
	}
	const Slot before = *slot;
	unindex(p_id, before, false);
	slot->due = MIN(after_ms(now, idle_ms), after_ms(slot->made, life_ms));
	slot->order = ++touch;
	slot_times.insert(SlotTime{ slot->due, p_id });
	slot_orders.insert(SlotOrder{ slot->order, p_id });
	user_orders.insert(UserOrder{ slot->user, slot->order, p_id });
	user_times.insert(UserTime{ slot->user, slot->due, p_id });
	return R::ok(slot->value);
}

// Invalidate the selected stored value.
void GDWebSessionStore::drop(const String &p_id) {
	erase(p_id);
}

// Remove all stored values.
void GDWebSessionStore::clear() {
	slots.clear();
	user_counts.clear();
	slot_times.clear();
	slot_orders.clear();
	user_orders.clear();
	user_times.clear();
}

// Remove only expired index entries and return the active session count.
int GDWebSessionStore::size() {
	const uint64_t now = GDClock::msec();
	while (drop_expired(now)) {
	}
	return slots.size();
}

// Extract only the configured name from the Cookie header.
String GDWebSessionStore::cookie_of(const String &p_header) const {
	// Scan once up to the requested name instead of allocating a string for every cookie.
	int at = 0;
	while (at < p_header.length()) {
		while (at < p_header.length() && (p_header[at] == ' ' || p_header[at] == '\t')) {
			at++;
		}
		const int begin = at;
		while (at < p_header.length() && p_header[at] != '=' && p_header[at] != ';') {
			at++;
		}
		int name_end = at;
		while (name_end > begin && (p_header[name_end - 1] == ' ' || p_header[name_end - 1] == '\t')) {
			name_end--;
		}
		const bool match = at < p_header.length() && p_header[at] == '=' && name_end - begin == cookie_name.length() && p_header.substr(begin, name_end - begin) == cookie_name;
		const int value_at = at < p_header.length() && p_header[at] == '=' ? ++at : at;
		while (at < p_header.length() && p_header[at] != ';') {
			at++;
		}
		if (match) {
			int value_end = at;
			while (value_end > value_at && (p_header[value_end - 1] == ' ' || p_header[value_end - 1] == '\t')) {
				value_end--;
			}
			return p_header.substr(value_at, value_end - value_at);
		}
		at++;
	}
	return String();
}

// Build a safe Cookie header for a session ID.
String GDWebSessionStore::cookie(const String &p_id, bool p_secure) const {
	String out = cookie_name + "=" + p_id + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" + String::num_int64(life_ms / 1000);
	return p_secure ? out + "; Secure" : out;
}

// Build a header that deletes the session cookie.
String GDWebSessionStore::clear_cookie(bool p_secure) const {
	String out = cookie_name + "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
	return p_secure ? out + "; Secure" : out;
}

// Validate or transform input before passing it to the next handler.
Variant GDWebSessionStore::handle(const Ref<GDWebRequest> &p_req) {
	const String id = cookie_of(p_req->header("cookie"));
	if (id.is_empty()) {
		return Err::make("session cookie required", Err::UNAUTHENTICATED);
	}
	Ref<R> got = take(id);
	if (!got->get_ok()) {
		return got->get_e();
	}
	p_req->keep(keep_name, got->get_v());
	return Variant();
}

// Register public script methods and properties.
void GDWebSessionStore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("issue", "value"), &GDWebSessionStore::issue);
	ADD_RESULT("issue", "String");
	ClassDB::bind_method(D_METHOD("take", "id"), &GDWebSessionStore::take);
	ClassDB::bind_method(D_METHOD("drop", "id"), &GDWebSessionStore::drop);
	ClassDB::bind_method(D_METHOD("clear"), &GDWebSessionStore::clear);
	ClassDB::bind_method(D_METHOD("size"), &GDWebSessionStore::size);
	ClassDB::bind_method(D_METHOD("cookie", "id", "secure"), &GDWebSessionStore::cookie, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("clear_cookie", "secure"), &GDWebSessionStore::clear_cookie, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("handle", "req"), &GDWebSessionStore::handle);
}
