/**************************************************************************/
/*  mv_link.h                                                             */
/**************************************************************************/

// Carries @online state split into reliable changes and latest-wins continuous values.

#pragma once

#include "core/crypto/crypto.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/main/node.h"

class MultiplayerAPI;

class MVLink : public Node {
	GDCLASS(MVLink, Node);

public:
	enum {
		RATE_MS = 1000, // Time window for counting commands. Shared by the Online side, Client and Secret
		RATE_MAX = 120, // Max commands per second accepted from one player. Five per-frame calls still arrive at 20Hz
		ARGS_MAX = 16384, // Max argument size per command (bytes). Counting calls alone would let one command send many MB
	};

private:
	bool host = false; // Whether listening as a server
	Ref<MultiplayerAPI> mp; // Dedicated RPC space independent of the node path
	Dictionary epochs; // World epoch per Player
	Dictionary allowed; // Tokens and Players approved once by Secret
	Dictionary waiting; // Disconnect deadlines for DTLS peers that have not sent a token
	Dictionary peer_who; // Authenticated peer IDs to Player
	Dictionary who_peer; // Maps a Player to its current peer ID
	Dictionary rates; // Per Player, times of commands received in the last second
	String token; // DTLS binding token sent once after TLS authentication
	int64_t state_epoch = 0; // World epoch being received
	HashMap<int64_t, HashMap<StringName, int64_t>> state_seen; // Thing -> name -> latest tick
	HashSet<int64_t> state_nodes; // Node IDs whose creation was reliably announced

	void connected(); // Sends the token received over TLS once after DTLS is established
	void failed(); // Tells the upper layer that DTLS could not be opened
	void process(); // Disconnects peers that do not send a token before the deadline
	void greet(int p_id); // Unauthenticated peers wait for bind
	void forget(int p_id); // Cleans up records on disconnect
	static Dictionary state_part(Dictionary &p_frame, const String &p_name); // Splits out continuous values
	static bool has_data(const Dictionary &p_frame); // Checks whether a frame is empty
	Dictionary fresh_state(int64_t p_epoch, int64_t p_tick, const Dictionary &p_frame); // Extracts only newer properties

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	MVLink();
	String open(int p_port, const Ref<CryptoKey> &p_key, const Ref<X509Certificate> &p_cert, const String &p_bind, int p_room_max); // Listens for DTLS as Secret
	String join(const String &p_addr, int p_port, const String &p_name, const Ref<X509Certificate> &p_ca, const String &p_token); // Connects to Secret DTLS through a relay
	void close(); // Closes the connection
	void allow(const String &p_who, const String &p_token); // Assigns a TLS-authenticated token to a Player, usable once
	void revoke(const String &p_who); // Also revokes DTLS rights of a Player whose TLS route closed
	String who_of(int p_id) const; // Returns the Player name for an authenticated peer ID
	PackedStringArray players() const; // Returns connected Players
	bool push(const String &p_who, const Dictionary &p_frame); // Distributes state to an authenticated Player
	void _bind(const String &p_token); // Binds a DTLS peer to a TLS-authenticated Player
	void _ask(int64_t p_id, int64_t p_uid, const String &p_fn, const Array &p_args); // Passes a command from the owner to the world
	void _ask_done(int64_t p_id, const Variant &p_value); // Passes an answer from the Online side upward
	bool ask(int64_t p_id, int64_t p_uid, const String &p_fn, const Array &p_args); // Sends the owner's command to the world
	void ask_done(const String &p_who, int64_t p_id, const Variant &p_value); // Returns an answer to the player who asked
	Callable answer_to(const String &p_who, int64_t p_id); // Callable that returns the answer to the player once it is ready
	void _rep(Dictionary p_frame); // Receives reliable state
	void _take_state(int64_t p_epoch, int64_t p_tick, const Dictionary &p_frame); // Receiver for __flow and __settle. Accepts only newer properties
	void _forget_state(int64_t p_uid); // Drops ticks of a deleted Node
};
