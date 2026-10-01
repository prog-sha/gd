/**************************************************************************/
/*  mv_client.h                                                           */
/**************************************************************************/

// Manages Secret authentication, reconnection and public world replication in one built-in Node.

#pragma once

#include "mv_frame.h"
#include "mv_online.h"
#include "mv_rep.h"
#include "mv_reply.h"
#include "mv_secret_client.h"

#include "core/crypto/crypto.h"
#include "core/io/tcp_server.h"
#include "core/templates/hash_set.h"
#include "scene/main/http_request.h"
#include "scene/main/node.h"

class MVLink;
class MVRuntime;

class MVClient : public Node {
	GDCLASS(MVClient, Node);

	static constexpr uint64_t RETRY_MS = 1000; // Interval between reconnect attempts to Relay candidates
	static constexpr uint64_t LOGIN_MS = 180000; // Max wait for a browser login
	static constexpr uint64_t VISIT_MS = 3000; // Max time to read one connection from the browser

	Ref<TCPServer> catcher; // Receives the authorization code returned from the browser. Open only during login
	Ref<StreamPeerTCP> visitor; // Browser connection being read now
	uint64_t catch_until = 0; // Deadline for waiting on authorization
	String link_state; // Mark of the pending authorization. Requests with other marks are dropped without closing the listener
	uint64_t visit_at = 0; // Time reading of the connection started

	Ref<MVSecretClient> net; // TLS connection and send/receive queue
	Ref<MVFrame> parser; // Turns received bytes back into frames
	Ref<MVRep> rep; // Copies Secret diffs into the public world
	Ref<X509Certificate> ca; // Trusted Secret certificate
	ObjectID runtime; // Runtime owning the local world
	ObjectID player; // Player that Secret assigned to this player
	ObjectID state; // Built-in link receiving the latest state over DTLS
	PackedStringArray hosts; // Relay host candidates to rotate through
	ObjectID asker; // HTTPRequest asking the matchmaking server for a free room
	ObjectID room; // Room this device owns after matchmaking chose it
	Dictionary start_config; // Settings decided at startup. Used to ask again when the room is gone
	uint64_t match_at = 0; // Next time to ask matchmaking again. 0 means do not ask
	uint64_t asked_at = 0; // Last time matchmaking was asked. Re-asking is limited to once per RETRY_MS
	bool matched_once = false; // Whether a destination was ever received. Distinguishes from players who cannot start at all
	int host_i = 0; // Index of the next Relay host
	int port = 0;
	String secret_name; // Secret certificate name to verify
	String want_room; // Name of the room this device created. Empty lets Secret pick a free room
	Dictionary login_input; // Credential input passed to the login provider
	Ref<MY> mine; // Local owner. Same object for the whole run; only its content changes on login, account link or rejoin
	bool login_pending = false; // Emits logged_in only after the script is loaded
	String told_lost; // Remembers the last disconnect reason so it is not repeated
	HashMap<int64_t, Ref<MVReply>> waiting_replies; // Reply holders waiting for answers, by ID
	int64_t asked = 0; // ID assigned to sent commands

	void answered(int64_t p_id, const Variant &p_value); // Delivers a returned answer
	void drop_replies(); // Closes pending answers with empty values
	String who; // Player name decided by Secret
	String token; // Token used only for the next resume
	bool login_sent = false; // Whether login input was sent on the current TLS
	bool resuming = false; // Whether a token receipt confirmation is needed
	bool online = false; // Whether commands can be sent over authenticated TLS
	bool announced = false; // Whether connected was announced for the current connection
	bool closed = true; // Whether the connection was closed explicitly
	uint64_t retry_at = 0; // Time to start the next Relay connection
	uint64_t generation = 0; // Tells apart connections switched during a signal
	HashSet<String> told_bad; // Remembers reasons already shown so they are not repeated

	MVRuntime *runtime_node() const; // Returns the live runtime
	Node *player_node() const; // Returns the live local Player
	MVLink *state_link() const; // Returns the live DTLS state link
	void begin_host(const Dictionary &p_config); // Starts a room because matchmaking chose this device
	void ask_match(); // Asks matchmaking where to go
	void no_room(const String &p_reason); // Decides whether to ask again or give up when there is no destination
	void ask_again(const String &p_reason); // Destination unknown; asks again after a short delay
	void leave_room(); // Shuts down the room this device owned
	void room_closed(); // Owned room closed; asks for a destination again
	void ready(); // Reads settings and starts auto-connect on a normal launch
	void process(); // Advances command sending, TLS traffic and reconnection by one frame
	Dictionary settings() const; // Returns project settings overridden by command-line arguments
	Dictionary account_input(const Dictionary &p_settings) const; // Extracts only account credentials
	String replace(const Dictionary &p_login, bool p_guest); // Switches credentials and rebuilds the connection
	String open(const Dictionary &p_login); // Opens the public world and TLS from validated settings
	void close(bool p_exiting = false); // Stops reconnecting and frees Client resources and the public world
	void open_net(); // Opens Secret TLS to the current Relay candidate
	void start_login(); // Sends credential input to Secret once
	bool start_state(const Dictionary &p_body); // Starts state traffic with the TLS-authenticated token
	void take(const PackedByteArray &p_data); // Routes received frames to authentication or world diffs
	void apply(const Dictionary &p_frame); // Applies tree, API and values to the public world, in that order
	void report_bad(const Array &p_bad); // Reports each reason for diffs that could not be applied, once
	Node *player_for(const String &p_who) const; // Finds the local Player from ownership info
	void reset_world(); // Replaces the public world with an empty one when the Server is recreated
	void lost(const String &p_reason); // Announces the disconnect and schedules the next Relay candidate
	bool send_frame(int p_type, const Dictionary &p_body); // Queues one frame for TLS sending
	Ref<X509Certificate> read_ca(const Dictionary &p_config); // Reads the CA certificate. Stops startup if it cannot be read
	void begin_with(const Dictionary &p_config); // Starts login once the destination is decided
	void matched(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body); // Receives the free-room reply
	void fail_start(const String &p_reason); // Shows why startup failed and exits
	void catch_code(); // Reads the authorization code returned from the browser and sends it to Secret
	void login_failed(const String &p_reason); // External login did not finish. Notifies the author
	void linked(const Dictionary &p_my); // External login finished. Emits logged_in once more

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static MVClient *current; // Running Client. Followed by Online.login_with()
	bool ask(int64_t p_uid, const String &p_fn, const Array &p_args, const Variant &p_reply); // Passes a command from the runtime to authenticated TLS
	void login_with(const String &p_provider); // Starts login with an external account
	void logout(); // Unlinks this device from the account and stops
};
