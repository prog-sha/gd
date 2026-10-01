/**************************************************************************/
/*  mv_authority.h                                                        */
/**************************************************************************/

// Runs TLS authentication, DTLS state delivery and the Online side world behind one server entry point.

#pragma once

#include "mv_frame.h"
#include "mv_store.h"
#include "mv_vault.h"

#include "core/object/object_id.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/main/http_request.h"
#include "scene/main/node.h"

class MVLink;
class MVRuntime;
class MVTLSServer;

class MVAuthority : public Node {
	GDCLASS(MVAuthority, Node);

	friend class MVTLSServer;

	struct Route {
		Ref<MVFrame> parser; // Reassembles TCP fragments into frames
		String who; // Player name authenticated by the Secret
		String room; // Room this route announced as host. Empty for a Client
		String want; // Name of a room the Client says it created itself
		Dictionary holding; // AUTH body waiting for the room's reply
		bool busy = false; // In the middle of login or reconnect. No further request is accepted until it completes or is denied
		uint64_t held_at = 0; // Time the wait for the room's reply began
		uint64_t made = 0; // Time TLS was established
		uint64_t heard = 0; // Time the last frame was received
		uint64_t rate_at = 0; // Start time of the command count window
		int rate = 0; // Commands received in the current window
		HashSet<String> told_bad; // Reasons for requests that cannot pass. Each reason is reported only once
		String link_provider; // Provider name while an external login is in progress
		String link_state; // Random value binding the authorization returned from the browser to this route
		String link_verifier; // PKCE verifier. Proves at token exchange that the authorization code belongs to this device
		String link_attempt; // Current exchange identifier used to reject replies from an older login
		bool link_committing = false; // Account storage is being updated for the current exchange
	};

	// Room held by a host. Destination of a Client's state traffic
	struct Room {
		String route; // TLS route of the host that announced this room
		String host; // State traffic destination returned to Clients
		int port = 0; // State traffic port returned to Clients
		String cert; // State traffic certificate. Created here and handed to the host
		String name; // Name written in the certificate
		int max = 0; // Player limit of this room
		int refused = 0; // Players refused (or unanswered) in a row. The room is closed at REFUSE_MAX
	};

	MVTLSServer *tls = nullptr; // TCP entry receiving encrypted Client credentials
	Dictionary config; // Settings from the Secret inspector
	Ref<CryptoKey> key; // Listener key. Opened after the world starts running
	Ref<X509Certificate> cert;
	String cert_path; // Location of the certificate given to Clients
	String bind; // Listener bind address
	int port = 0; // Listener port
	ObjectID runtime; // Runner of the Online side world
	MVVault vault; // Vault holding identities and handovers. Never given to the world
	MVStore store; // Access to store @online_save values in Redis. The world only gets this access
	ObjectID state; // DTLS entry carrying the latest state
	HashMap<String, Route> routes; // Auth state per random route
	HashMap<String, String> sessions; // Maps a session to a Player name
	HashMap<String, String> ids; // Maps a product User ID to a Player name
	HashMap<String, String> who_route; // Maps a Player name to the current TLS route
	HashMap<String, uint64_t> expires; // Reconnect deadline for keeping disconnected Players
	int next_player = 1; // Number for the next Player name
	bool started = false; // Whether the listener and the Online side world have started
	ObjectID teller; // HTTPRequest reporting availability to the matchmaking server
	String match_url; // Report destination. Empty means no reports
	String match_key; // Key attached to reports
	String room_id;
	String room_host; // Destination clients should be given
	int room_port = 0; // Listener port clients should be given
	uint64_t told_at = 0; // Time of the last report
	uint64_t swept_at = 0; // Time of the last sweep
	int told_free = -1; // Free room count in the last report
	bool told_store = false; // Store read/write availability last reported to the host
	bool secret_only = false; // Whether the host holds the world and this side handles only identity and Secret bodies
	HashMap<String, Room> rooms; // Maps a room name to its host
	HashMap<String, String> who_room; // Maps a Player name to the room they are currently in

	// Only the host of the room the owner is in may write that account's saved values.
	// Hosts are not trusted, so a named account is not taken at face value
	struct Grant {
		String room; // Room the owner is in
		uint64_t until = 0; // Deadline for writes after leaving. 0 while in the room
	};
	HashMap<String, Grant> grants; // Maps an accountID to the host room allowed to write it
	HashMap<String, int64_t> used_grants; // Consumed hosting grants held only until their expiry
	HashMap<String, Vector<uint64_t>> fresh_from; // Maps a peer address to the times it created new accounts

	enum {
		ANNOUNCE_MS = MVFrame::ALIVE_MS, // Interval between availability reports
		HOLD_MS = MVFrame::DEAD_MS, // Maximum wait for a room's reply
		SWEEP_MS = 250, // Interval between expiry sweeps
		GRANT_MS = 10000, // Time a host may still send the last save of a player who left
		REFUSE_MAX = 3, // Close a room after it refuses this many players in a row. Do not keep sending players to a room nobody can enter
		FRESH_MAX = 8, // New accounts one address may create within the window. Existing accounts are not counted
		FRESH_MS = 600000, // That window (10 minutes)
		FULL_RETRY_MS = 5000, // Wait before a player refused for a full room retries
	};

	MVRuntime *runtime_node() const; // Returns the live Online side runner
	MVLink *state_link() const; // Returns the live DTLS entry
	void deny(const String &p_route, const String &p_reason, bool p_again = false, bool p_stop = false); // Sends one denial. With stop the peer no longer reconnects
	void deny_later(const String &p_route, const String &p_reason, int p_wait_ms = 1000); // Denial that passes after a short wait. The peer waits that long, reconnects and asks again
	bool may_create(const String &p_route); // Whether this peer may create a new account
	void deny_link(const String &p_route, const String &p_reason); // External login failure. The peer stays connected and emits login_failed
	void logout(const String &p_route); // Unbinds this device from its account and stops it
	void logged_out(const Variant &p_done, const String &p_route); // Stops after the binding is cut
	void serve(); // Opens the listener once the world starts running
	void guest_connected(const Dictionary &p_accepted, const String &p_route); // Turns a guest with a settled account into a Player
	void linked(const Dictionary &p_linked, const String &p_route, const String &p_attempt); // Pass the committed account to the world and owner
	void evict(const String &p_who, const String &p_reason); // Drops an older device of the same account and removes it from the world
	void forget_player(const String &p_who); // Erases all records of a player whose reconnect deadline passed
	void leave_room(const String &p_who); // Removes the player from room records and gives account writes a grace period
	void expire_grant(const String &p_id); // Closes writes to this account after a grace period
	bool may_save(const Route &p_route, const Dictionary &p_body, String &r_why) const; // Whether a save from a host may pass
	void secret_done(const Variant &p_value, const String &p_route, int64_t p_id); // Returns the answer to a Secret body the host asked for
	void user_saved_for(const Variant &p_saved, const String &p_route, const String &p_room, const Dictionary &p_accepted); // Saved values returned, so asks the room's host to admit the player
	void load_saves(const String &p_id, const Callable &p_done); // Reads the owner's saved values. Answers empty at once without a store
	void user_saved(const Variant &p_saved, const String &p_route, const Dictionary &p_accepted); // Saved values returned, so puts the player into the world
	void relinked(const Variant &p_saved, const String &p_route, const Dictionary &p_my); // Saved values of the bound account returned, so restores them to the owner
	void connected(const String &p_route); // Returns an anonymous context once TLS is established
	void received(const String &p_route, const PackedByteArray &p_data); // Handles frames inside TLS
	void parted(const String &p_route); // Starts the reconnect deadline after TLS disconnects
	void handle(const String &p_route, const Dictionary &p_frame); // Dispatches a frame to its auth stage
	void login(const String &p_route, const Variant &p_input, const String &p_room); // Binds Guest credentials to a Player
	void resume(const String &p_route, const String &p_token); // Uses a reconnect token only once
	void login_with(const String &p_route, const String &p_provider); // Builds and returns the external login URL
	void login_code(const String &p_route, const String &p_code, const String &p_state); // Exchanges the returned authorization code for a token
	void token_got(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body, const String &p_route, const String &p_provider, const String &p_attempt, ObjectID p_http); // Token reply
	void user_got(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body, const String &p_route, const String &p_provider, const String &p_attempt, ObjectID p_http); // Reply with the peer's id and name
	void link_done(const String &p_route, const String &p_provider, const String &p_id, const String &p_name, const String &p_attempt); // Bind the external account and notify the owner
	void authorize(const String &p_route, Dictionary p_accepted); // Returns the Player and DTLS token to an authenticated route
	void settle(const String &p_route, Dictionary p_accepted); // Records the Player put into the world and replies to the owner
	bool spare(Route &r_route); // Checks the per-connection command limit
	bool spare_host(Route &r_route); // Hosts carry commands for a whole room, so checks against a limit widened by player count
	bool valid_grant(const String &p_grant); // Verify and consume a room-holding grant issued by matchmaking
	void refuse_host(const String &p_route, const String &p_why); // Reports a host request that cannot pass, once per reason
	void room_refused(const String &p_room); // A room refused a player. Closes it if this continues
	bool send(const String &p_route, int p_type, const Dictionary &p_body); // Queues a frame for TLS send
	void state_joined(const String &p_who); // Sends the full state after DTLS binding
	void state_asked(const String &p_who, int64_t p_id, int64_t p_uid, const String &p_fn, const Array &p_args); // Passes a command from state traffic to the world
	void host_hello(const String &p_route, const Dictionary &p_body); // Receives a host's room announcement. The first creates and hands over a key (P-256) and certificate, the second opens with the port

	void host_joined(const String &p_route, const Dictionary &p_body); // Receives the host's reply on whether the player entered the world
	void host_save(const String &p_route, const Dictionary &p_body); // Stores @online_save values sent by a host
	void host_call(const String &p_route, const Dictionary &p_body); // Calls a Secret body function requested by a host
	void host_gone(const String &p_route); // Closes a room whose host disconnected and makes its players ask again right away
	void tell_room(const String &p_who, bool p_gone); // Tells the room's host that a player left
	String pick_room(const String &p_want) const; // Picks the room to enter. Uses the requested room if it has space
	int room_players(const String &p_room) const; // Counts the players in that room
	bool allow_in(const String &p_route, const String &p_room, const Dictionary &p_accepted); // Asks the room's host to admit a player
	void announce(); // Reports availability to the matchmaking server
	PackedStringArray headers_for_match() const; // Builds headers with the key for reports
	void sweep(); // Sweeps unauthenticated connections, heartbeats and reconnect deadlines
	void stop(); // Closes traffic and the Online side world

public:
	int rooms_open() const; // Number of rooms with players, plus one room about to open

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	String start(bool p_secret_only = false); // Starts the server with a key and listener built from project settings
};
