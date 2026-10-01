/**************************************************************************/
/*  mv_frame.h                                                            */
/**************************************************************************/

// Packs one Online message into a single text line and unpacks it again.

#pragma once

#include "core/object/ref_counted.h"

class MVFrame : public RefCounted {
	GDCLASS(MVFrame, RefCounted);

	String buf; // Holds the part of a line that has arrived so far

	static bool plain_at(const Variant &p_v, int p_depth); // Checks, with a depth limit, that a value is made of plain values only

protected:
	static void _bind_methods();

public:
	static const char *NODE_KEY; // Marker for a Node passed by its serial number
	enum {
		// Commands travel over the state channel. TLS carries identity only (login, reconnect, handover)
		REP = 0x10, // Server->Client: distributes public diffs
		LOGIN = 0x20, // Client->Online side: login input
		RESUME = 0x21, // Client->Online side: one-time reconnect token
		AUTH = 0x22, // Online side->Client: public my and the next token
		DENY = 0x23, // Online side->Client: reason for denial
		CONFIRM = 0x24, // Client->Online side: acknowledges receipt of the token
		PING = 0x25, // Client->Online side: heartbeat
		PONG = 0x26, // Online side->Client: heartbeat reply
		LOGIN_WITH = 0x27, // Client->Online side: wants to start an external login; the body is the provider name
		AUTH_URL = 0x28, // Online side->Client: URL to open in the browser and the port that receives the return
		LOGIN_CODE = 0x29, // Client->Online side: authorization code returned from the browser
		LINKED = 0x2A, // Online side->Client: external login finished; the new public my
		LOGOUT = 0x2B, // Client->Online side: unlinks this device from the account
		// Frames between the host and Secret, used only when a host owns the room
		HOST_HELLO = 0x40, // Host->Secret: room introduction
		HOST_READY = 0x41, // Secret->Host: accepts the introduction; entry list and the world's saved values
		HOST_ALLOW = 0x42, // Secret->Host: authenticated Player, state channel token, and that player's saved values
		HOST_JOINED = 0x43, // Host->Secret: whether the player entered the world
		HOST_PART = 0x44, // Secret->Host: this player's identity connection has closed
		HOST_SAVE = 0x45, // Host->Secret: stores @online_save values
		HOST_CALL = 0x46, // Host->Secret: calls a function in the Secret script
		HOST_DONE = 0x47, // Secret->Host: result of the called function
		HOST_STORE = 0x48, // Secret->Host: whether storage is readable/writable now. Hold back while down, store again once back
		BUF_MAX = 1 << 20, // Max bytes buffered without a newline. Cuts off a stalled stream
		READ_MAX = 1 << 16, // Bytes read from TLS per call
		WRITE_MAX = 1 << 20, // Max pending send bytes per connection
		DEPTH_MAX = 16, // Max nesting depth of a body. Overly deep shapes are rejected before opening
		// One liveness rule: report every second, treat as disconnected after 5 seconds of silence.
		// Applies to Client/host TLS, state DTLS, room reply waits, and matchmaking reports alike
		ALIVE_MS = 1000, // Interval between liveness reports
		DEAD_MS = 5000, // Treat as disconnected after this long without anything arriving
	};

	static String pack(int p_type, const Dictionary &p_body); // Packs one message into one line
	static Dictionary open(const String &p_line); // Opens one line into type and body
	static bool plain(const Variant &p_v); // Whether a value is made of plain values only
	static int size_of(const Variant &p_v); // Size of a value when put on the wire
	static Dictionary arguments(const Dictionary &p_spec, const Array &p_args); // Checks @online arguments for type and finite values
	Array feed(const String &p_text); // Extracts completed lines from arriving fragments
};
