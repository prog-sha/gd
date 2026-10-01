/**************************************************************************/
/*  mv_vault.cpp                                                          */
/**************************************************************************/

// Keeps auth sessions and Player creation rights inside the Server only, separated from the public SceneTree.

#include "mv_vault.h"

#include "mv_guest.h"
#include "mv_local_id.h"
#include "mv_store.h"

#include "core/crypto/crypto.h"
#include "core/object/callable_mp.h"
#include "core/crypto/hashing_context.h"
#include "core/os/os.h"

// Copy only the values from an auth context that Server scripts may see
Dictionary MVVault::public_context(const Dictionary &p_one) const {
	Dictionary out;
	out["id"] = p_one.get("id", String());
	out["session"] = p_one.get("session", String());
	out["authenticated"] = p_one.get("authenticated", false);
	out["guest"] = p_one.get("guest", false);
	out["name"] = p_one.get("name", String());
	return out;
}

// Revoke the current and in-exchange resume tokens of a context
void MVVault::drop_tokens(Dictionary &p_one) {
	const String current = p_one.get("token_hash", String());
	const String pending_hash = p_one.get("pending_hash", String());
	if (!current.is_empty()) {
		tokens.erase(current);
	}
	if (!pending_hash.is_empty()) {
		tokens.erase(pending_hash);
	}
	p_one["token_hash"] = String();
	p_one["pending_hash"] = String();
	p_one["pending_token"] = String();
}

// Check that an external auth ID is not empty, too long, a path, or containing control chars
bool MVVault::safe_user_id(const String &p_id) const {
	if (p_id.is_empty() || p_id.length() > USER_ID_MAX || p_id.contains("/")) {
		return false;
	}
	for (int i = 0; i < p_id.length(); i++) {
		const char32_t c = p_id[i];
		if (c < 0x20 || c == 0x7f) {
			return false;
		}
	}
	return true;
}

// Validate an install or device ID and turn it into a fixed-length digest that does not keep the original
Dictionary MVVault::guest_login(const Variant &p_input) const {
	Dictionary denied;
	denied["ok"] = false;
	denied["reason"] = U"Guest ID rejected";
	if (p_input.get_type() != Variant::DICTIONARY) {
		return denied;
	}
	const Dictionary input = p_input;
	const Variant source_value = input.get("source", Variant());
	const Variant id_value = input.get("id", Variant());
	if (source_value.get_type() != Variant::STRING || id_value.get_type() != Variant::STRING) {
		return denied;
	}
	const String source = source_value;
	const String id = id_value;
	const bool install = source == "install" && MVLocalID::valid(id, MVGuest::ID_BYTES);
	const bool device = source == "device" && !id.is_empty() && id.length() <= DEVICE_ID_MAX;
	if (!install && !device) {
		return denied;
	}
	Dictionary accepted;
	accepted["ok"] = true;
	accepted["provider"] = "guest:" + source;
	// A device ID is visible to other apps through the OS; it is not a secret only the user knows.
	// Using it as a carry-over key would let anyone who learns the device ID take the account.
	// user:// Login is allowed as a fallback where user:// is not writable, but a new account is used each time
	accepted["account_id"] = device ? MVLocalID::random_hex(32) : id.sha256_text();
	return accepted;
}

// Turn an account at an external provider into an ID that cannot be matched outside this product
String MVVault::device_key(const String &p_provider, const String &p_account_id) const {
	if (!safe_user_id(p_provider) || !safe_user_id(p_account_id)) {
		return String();
	}
	// The key is the Secret inspector's connect_key, or one made in this machine's user:// if missing.
	// A made key belongs to that one machine, so accounts do not carry over on rebuild or a second machine. Authors who want that set it
	const String key_text = connect_key.is_empty() ? MVLocalID::read_or_make("user://online_connect_key", 32) : connect_key;
	Ref<Crypto> crypto = Crypto::create();
	if (key_text.is_empty() || crypto.is_null()) {
		return String();
	}
	const PackedByteArray key = key_text.hex_decode();
	const PackedByteArray message = (p_provider + "\n" + p_account_id).to_utf8_buffer();
	const PackedByteArray digest = crypto->hmac_digest(HashingContext::HASH_SHA256, key, message);
	return String("dev:") + String::hex_encode_buffer(digest.ptr(), digest.size());
}

// Account the device key points to. Empty if not bound yet, null if unreadable.
// While a store is configured but not connected it counts as unreadable. Entering with a throwaway account
// would let saves after reconnecting erase the real one
void MVVault::bound_account(const String &p_device, const Callable &p_done) {
	if (store == nullptr || !store->is_wanted()) {
		const HashMap<String, String>::ConstIterator known = loose.find(p_device);
		p_done.call(known ? known->value : String());
		return;
	}
	if (!store->is_open()) {
		p_done.call(Variant());
		return;
	}
	store->account_of(p_device, callable_mp(this, &MVVault::bound_got).bind(p_done));
}

void MVVault::bound_got(const Variant &p_account, const Callable &p_done) {
	if (p_account.get_type() != Variant::STRING) {
		p_done.call(Variant());
		return;
	}
	const String account = p_account;
	p_done.call(account.begins_with("user:") ? account : String());
}

// Point the device key at an account. true if bound, false if already taken, null if unwritable.
// Worlds without a store remember it only within this process
void MVVault::bind(const String &p_device, const String &p_account, bool p_overwrite, const Callable &p_done) {
	if (p_device.is_empty() || p_account.is_empty()) {
		p_done.call(Variant());
		return;
	}
	if (store == nullptr || !store->is_wanted()) {
		if (!p_overwrite && loose.has(p_device)) {
			p_done.call(false);
			return;
		}
		loose[p_device] = p_account;
		p_done.call(true);
		return;
	}
	if (!store->is_open()) {
		p_done.call(Variant());
		return;
	}
	store->bind_account(p_device, p_account, p_overwrite, p_done);
}

// One layer between device key and account. Repointing it alone is how accounts transfer.
// If missing, create it from randomness unrelated to the device. Using the device key directly would let the ID trace back to the device
// Four answers: a string is an existing account, a dict {account, created} is a just-made one, false means creation refused, null means unreadable
void MVVault::account_of(const String &p_device, bool p_may_create, const Callable &p_done) {
	if (p_device.is_empty()) {
		p_done.call(String());
		return;
	}
	bound_account(p_device, callable_mp(this, &MVVault::account_got).bind(p_device, p_may_create, p_done));
}

void MVVault::account_got(const Variant &p_account, const String &p_device, bool p_may_create, const Callable &p_done) {
	if (p_account.get_type() != Variant::STRING) {
		p_done.call(Variant());
		return;
	}
	if (!String(p_account).is_empty()) {
		p_done.call(p_account);
		return;
	}
	// Stop the same peer from creating new accounts repeatedly in a short time. Existing accounts already passed above.
	// Accounts in worlds without a store live only in this process, so there is nothing to protect and they are not counted
	if (!p_may_create && store != nullptr && store->is_wanted()) {
		p_done.call(false);
		return;
	}
	const String account = String("user:") + MVLocalID::random_hex(32);
	bind(p_device, account, false, callable_mp(this, &MVVault::created_got).bind(account, p_device, p_done));
}

// Bound the new account. If already taken (another login on the same device made one first), reread that one
void MVVault::created_got(const Variant &p_bound, const String &p_account, const String &p_device, const Callable &p_done) {
	if (p_bound.get_type() != Variant::BOOL) {
		p_done.call(Variant());
	} else if (bool(p_bound)) {
		Dictionary made;
		made["account"] = p_account;
		made["created"] = true;
		p_done.call(made);
	} else {
		bound_account(p_device, callable_mp(this, &MVVault::bound_again).bind(p_done));
	}
}

void MVVault::bound_again(const Variant &p_account, const Callable &p_done) {
	p_done.call(p_account.get_type() == Variant::STRING && !String(p_account).is_empty() ? p_account : Variant());
}

// Link an external account (e.g. an OAuth provider) to the currently logged-in account. One rule:
// if the external account already has an account, both device keys point there; otherwise to the current guest account.
// From then on this device enters the same account even on guest start. Values are not merged
void MVVault::session_link(const String &p_route, const String &p_provider, const String &p_account_id, const String &p_name, const Callable &p_done) {
	const HashMap<String, Dictionary>::ConstIterator found = routes.find(p_route);
	const String device = device_key(p_provider, p_account_id);
	if (!found || device.is_empty() || !bool(found->value.get("authenticated", false))) {
		p_done.call(Dictionary());
		return;
	}
	bound_account(device, callable_mp(this, &MVVault::link_got).bind(p_route, device, p_name, p_done));
}

void MVVault::link_got(const Variant &p_account, const String &p_route, const String &p_device, const String &p_name, const Callable &p_done) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (!found || p_account.get_type() != Variant::STRING) {
		p_done.call(Dictionary());
		return;
	}
	const String before = found->value.get("id", String());
	const String account = String(p_account).is_empty() ? before : String(p_account);
	// Bind the external key first, then repoint this device's key. If either cannot be written, nothing is linked
	bind(p_device, account, true, callable_mp(this, &MVVault::link_bound).bind(p_route, account, p_name, p_done));
}

void MVVault::link_bound(const Variant &p_bound, const String &p_route, const String &p_account, const String &p_name, const Callable &p_done) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (!found || p_bound.get_type() != Variant::BOOL) {
		p_done.call(Dictionary());
		return;
	}
	bind(found->value.get("device", String()), p_account, true, callable_mp(this, &MVVault::link_done).bind(p_route, p_account, p_name, p_done));
}

void MVVault::link_done(const Variant &p_bound, const String &p_route, const String &p_account, const String &p_name, const Callable &p_done) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (!found || p_bound.get_type() != Variant::BOOL) {
		p_done.call(Dictionary());
		return;
	}
	if (store != nullptr && store->is_wanted()) {
		store->save_profile(p_account, p_name, callable_mp(this, &MVVault::profile_saved).bind(p_route, p_account, p_name, p_done));
		return;
	}
	profile_saved(true, p_route, p_account, p_name, p_done);
}

// Mark the account as linked only after its profile can be read by later logins.
void MVVault::profile_saved(const Variant &p_saved, const String &p_route, const String &p_account, const String &p_name, const Callable &p_done) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (!found || p_saved.get_type() != Variant::BOOL || !bool(p_saved)) {
		p_done.call(Dictionary());
		return;
	}
	profiles[p_account] = p_name;
	Dictionary one = found->value;
	const String before = one.get("id", String());
	one["id"] = p_account;
	one["guest"] = false;
	one["name"] = p_name;
	found->value = one;
	const String token = one.get("token_hash", String());
	if (!token.is_empty()) {
		tokens[token] = one;
	}
	const String pending = one.get("pending_hash", String());
	if (!pending.is_empty()) {
		tokens[pending] = one;
	}
	Dictionary out;
	out["my"] = public_context(one);
	out["before"] = before;
	p_done.call(out);
}

// Remove this device's key from the account. The account stays and can be entered again from the external account.
// An exit on shared devices so the next person does not enter the previous person's account
void MVVault::session_unbind(const String &p_route, const Callable &p_done) {
	const HashMap<String, Dictionary>::ConstIterator found = routes.find(p_route);
	const String device = found ? String(found->value.get("device", String())) : String();
	if (device.is_empty() || !bool(found->value.get("authenticated", false))) {
		p_done.call(Variant());
		return;
	}
	if (store == nullptr || !store->is_wanted()) {
		loose.erase(device);
		p_done.call(true);
		return;
	}
	if (!store->is_open()) {
		p_done.call(Variant());
		return;
	}
	store->unbind_account(device, p_done);
}

// Create a short-lived anonymous pre-login context for the route
Dictionary MVVault::session_begin(const String &p_route) {
	session_sweep();
	session_forget(p_route);
	const double now = OS::get_singleton()->get_unix_time();
	Dictionary one;
	one["id"] = String("anonymous:") + MVLocalID::random_hex(12);
	one["session"] = MVLocalID::random_hex(16);
	one["authenticated"] = false;
	one["guest"] = false;
	one["expires"] = now + double(ANONYMOUS_SEC);
	one["token_hash"] = String();
	one["pending_hash"] = String();
	one["pending_token"] = String();
	routes[p_route] = one;
	return public_context(one);
}

// Bind an ID confirmed by external login to the route and return a resume token once, after the account is decided
void MVVault::session_connect(const String &p_route, const String &p_provider, const String &p_account_id, bool p_may_create, const Callable &p_done) {
	const HashMap<String, Dictionary>::ConstIterator found = routes.find(p_route);
	const String device = device_key(p_provider, p_account_id);
	if (!found || device.is_empty() || bool(found->value.get("authenticated", false))) {
		p_done.call(Dictionary());
		return;
	}
	account_of(device, p_may_create, callable_mp(this, &MVVault::connect_got).bind(p_route, device, p_done));
}

void MVVault::connect_got(const Variant &p_answer, const String &p_route, const String &p_device, const Callable &p_done) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (p_answer.get_type() == Variant::BOOL) {
		Dictionary out;
		out["reason"] = String(TOO_MANY_NEW);
		p_done.call(out);
		return;
	}
	const bool created = p_answer.get_type() == Variant::DICTIONARY;
	const Variant account = created ? Variant(Dictionary(p_answer).get("account", String())) : p_answer;
	if (account.get_type() != Variant::STRING) {
		Dictionary out;
		out["reason"] = String(NOT_REACHABLE);
		p_done.call(out);
		return;
	}
	if (!found || String(account).is_empty() || bool(found->value.get("authenticated", false))) {
		p_done.call(Dictionary());
		return;
	}
	if (store != nullptr && store->is_wanted()) {
		store->profile_of(account, callable_mp(this, &MVVault::profile_got).bind(p_route, p_device, String(account), created, p_done));
		return;
	}
	const String profile = profiles.has(String(account)) ? "1" + profiles[String(account)] : String();
	profile_got(profile, p_route, p_device, account, created, p_done);
}

// Restore a linked account's membership and name before issuing a session token.
void MVVault::profile_got(const Variant &p_profile, const String &p_route, const String &p_device, const String &p_account, bool p_created, const Callable &p_done) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (p_profile.get_type() != Variant::STRING) {
		Dictionary out;
		out["reason"] = String(NOT_REACHABLE);
		p_done.call(out);
		return;
	}
	if (!found || bool(found->value.get("authenticated", false))) {
		p_done.call(Dictionary());
		return;
	}
	const String profile = p_profile;
	const bool linked = profile.begins_with("1");
	Dictionary one = found->value;
	drop_tokens(one);
	const String token = MVLocalID::random_hex(32);
	one["id"] = p_account;
	one["device"] = p_device;
	one["session"] = MVLocalID::random_hex(16);
	one["authenticated"] = true;
	one["guest"] = !linked;
	one["name"] = linked ? profile.substr(1) : String();
	one["expires"] = OS::get_singleton()->get_unix_time() + double(keep_sec);
	one["token_hash"] = token.sha256_text();
	tokens[one["token_hash"]] = one;
	Dictionary out;
	out["my"] = public_context(one);
	out["resume_token"] = token;
	out["created"] = p_created;
	p_done.call(out);
}

// Repoint the route while returning the same next token even for resends before receipt
Dictionary MVVault::session_resume(const String &p_route, const String &p_token) {
	session_sweep();
	if (p_token.length() != 64) {
		return Dictionary();
	}
	const HashMap<String, Dictionary>::ConstIterator route = routes.find(p_route);
	if (route && bool(route->value.get("authenticated", false))) {
		return Dictionary();
	}
	const String key = p_token.sha256_text();
	HashMap<String, Dictionary>::Iterator found = tokens.find(key);
	if (!found) {
		return Dictionary();
	}
	session_forget(p_route);
	Dictionary one = found->value;
	const String session = one.get("session", String());
	Vector<String> old_routes;
	for (const KeyValue<String, Dictionary> &entry : routes) {
		if (String(entry.value.get("session", String())) == session) {
			old_routes.push_back(entry.key);
		}
	}
	for (const String &old : old_routes) {
		routes.erase(old);
	}
	routes[p_route] = one;
	String next = one.get("pending_token", String());
	if (next.is_empty()) {
		next = MVLocalID::random_hex(32);
		one["pending_token"] = next;
		one["pending_hash"] = next.sha256_text();
		tokens[one["pending_hash"]] = one;
	}
	one["expires"] = OS::get_singleton()->get_unix_time() + double(keep_sec);
	Dictionary out;
	out["my"] = public_context(one);
	out["resume_token"] = next;
	return out;
}

// Revoke the old token only after the Client received AUTH
bool MVVault::session_confirm(const String &p_route) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (!found) {
		return false;
	}
	Dictionary one = found->value;
	const String pending_hash = one.get("pending_hash", String());
	if (pending_hash.is_empty()) {
		return false;
	}
	tokens.erase(String(one.get("token_hash", String())));
	one["token_hash"] = pending_hash;
	one["pending_hash"] = String();
	one["pending_token"] = String();
	return true;
}

// Remove only the route; if unauthenticated, discard its secrets too
void MVVault::session_forget(const String &p_route) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (!found) {
		return;
	}
	Dictionary one = found->value;
	routes.erase(p_route);
	if (!bool(one.get("authenticated", false))) {
		drop_tokens(one);
	}
}

// Immediately revoke an auth context whose Player creation failed
void MVVault::session_revoke(const String &p_route) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (!found) {
		return;
	}
	Dictionary one = found->value;
	routes.erase(p_route);
	drop_tokens(one);
}

// keep_sec is "how long one can return after disconnecting", not "how long one may stay connected".
// While connected it is extended every frame. Otherwise, mid-play,
// the context would vanish from the vault and RESUME would stop working
void MVVault::session_touch(const String &p_route) {
	HashMap<String, Dictionary>::Iterator found = routes.find(p_route);
	if (!found) {
		return;
	}
	found->value["expires"] = OS::get_singleton()->get_unix_time() + double(keep_sec);
}

// Clean up expired identities and routes
void MVVault::session_sweep() {
	const double now = OS::get_singleton()->get_unix_time();
	Vector<String> old_routes;
	for (const KeyValue<String, Dictionary> &entry : routes) {
		if (double(entry.value.get("expires", 0.0)) <= now) {
			old_routes.push_back(entry.key);
		}
	}
	for (const String &route : old_routes) {
		session_revoke(route);
	}
	Vector<String> old_tokens;
	for (const KeyValue<String, Dictionary> &entry : tokens) {
		if (double(entry.value.get("expires", 0.0)) <= now) {
			old_tokens.push_back(entry.key);
		}
	}
	for (const String &key : old_tokens) {
		tokens.erase(key);
	}
}

// The Secret calls this directly from C++; no MethodBind is made for author GDScript
void MVVault::_bind_methods() {
}
