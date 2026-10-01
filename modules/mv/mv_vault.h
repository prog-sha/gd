/**************************************************************************/
/*  mv_vault.h                                                            */
/**************************************************************************/

// Fixed Secret type holding auth results and Player creation rights, separated from Server scripts.

#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"

class Node;
class MVStore;

class MVVault : public Object {
	GDCLASS(MVVault, Object);

	MVStore *store = nullptr; // Store holding accounts and transfer passphrases
	HashMap<String, Dictionary> routes; // TLS route -> auth context
	HashMap<String, Dictionary> tokens; // Resume token hash -> auth context
	HashMap<String, String> loose; // Device key -> account, valid only while there is no store
	HashMap<String, String> profiles; // Linked account -> display name, used when no store is configured
	int keep_sec = SESSION_SEC; // Seconds a disconnected player can return within. Set by the Secret's keep_sec
	String connect_key; // HMAC key (hex) deriving device keys. The Secret's connect_key. If empty, one is made in user://

	Dictionary public_context(const Dictionary &p_one) const; // Copy only the context shown to authors
	void drop_tokens(Dictionary &p_one); // Revoke all resume tokens of a context
	bool safe_user_id(const String &p_id) const; // Check that an external ID is usable in the world
	String device_key(const String &p_provider, const String &p_account_id) const; // Turn an external ID into a device key for this product
	void account_of(const String &p_device, bool p_may_create, const Callable &p_done); // Pass the account the device key points to to p_done. Create one if missing (when allowed)
	void bound_account(const String &p_device, const Callable &p_done); // Pass the account the device key points to to p_done. Empty if none
	void bind(const String &p_device, const String &p_account, bool p_overwrite, const Callable &p_done); // Point the device key at an account
	void bound_got(const Variant &p_account, const Callable &p_done); // Store answer. Empty if not account-shaped, null if unreadable
	void account_got(const Variant &p_account, const String &p_device, bool p_may_create, const Callable &p_done); // Create and bind if missing
	void created_got(const Variant &p_bound, const String &p_account, const String &p_device, const Callable &p_done); // Whether the created account could be bound
	void bound_again(const Variant &p_account, const Callable &p_done); // Reread the earlier account
	void connect_got(const Variant &p_account, const String &p_route, const String &p_device, const Callable &p_done); // Read the profile once the account is decided
	void profile_got(const Variant &p_profile, const String &p_route, const String &p_device, const String &p_account, bool p_created, const Callable &p_done); // Bind the context with stored account membership
	void link_got(const Variant &p_account, const String &p_route, const String &p_device, const String &p_name, const Callable &p_done); // Bind once it is known whether the external account exists
	void link_bound(const Variant &p_bound, const String &p_route, const String &p_account, const String &p_name, const Callable &p_done); // The external key was bound
	void link_done(const Variant &p_bound, const String &p_route, const String &p_account, const String &p_name, const Callable &p_done); // This device's key was also repointed
	void profile_saved(const Variant &p_saved, const String &p_route, const String &p_account, const String &p_name, const Callable &p_done); // Confirm linked membership after it is saved

protected:
	static void _bind_methods();

public:
	static constexpr const char32_t *NOT_REACHABLE = U"The save store is not reachable. Try again later"; // Login refusal while the store is unreadable
	static constexpr const char32_t *TOO_MANY_NEW = U"Too many new players from this address. Try again later"; // Too many new accounts from the same peer

	enum {
		SESSION_SEC = 10, // Default seconds an authenticated session can resume. Changeable by the Secret's keep_sec
		ANONYMOUS_SEC = 60, // Seconds to keep a pre-login context
		USER_ID_MAX = 128, // Max chars of an external auth ID
		DEVICE_ID_MAX = 512, // Max chars accepted for a per-OS device ID
	};

	void set_store(MVStore *p_store) { store = p_store; } // Attach the account store
	void set_keep_sec(int p_sec) { keep_sec = p_sec > 0 ? p_sec : (int)SESSION_SEC; }
	void set_connect_key(const String &p_hex) { connect_key = p_hex; } // Key deriving device keys. Authors can hold it so accounts survive rebuilding the Secret
	int get_keep_sec() const { return keep_sec; }
	Dictionary session_begin(const String &p_route); // Start an anonymous auth context
	Dictionary guest_login(const Variant &p_input) const; // Validate guest input and turn it into an auth source for a product User
	void session_connect(const String &p_route, const String &p_provider, const String &p_account_id, bool p_may_create, const Callable &p_done); // Bind external auth to a product User and pass the AUTH body to p_done. The caller decides if a new account may be created
	void session_link(const String &p_route, const String &p_provider, const String &p_account_id, const String &p_name, const Callable &p_done); // Link an external account to the current one and pass the new my to p_done
	Dictionary session_resume(const String &p_route, const String &p_token); // Repoint the route by token
	bool session_confirm(const String &p_route); // Confirm receipt of the token exchange
	void session_forget(const String &p_route); // Remove only the route
	void session_unbind(const String &p_route, const Callable &p_done); // Unbind this device's key from the account. true if unbound, null otherwise
	void session_revoke(const String &p_route); // Revoke the context and tokens
	void session_touch(const String &p_route); // Extend the deadline while connected
	void session_sweep(); // Clean up expired contexts
};
