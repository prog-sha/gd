/**************************************************************************/
/*  match.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Matchmaking server for online games.
// Rooms announce their address and player count; clients ask for one room with free seats.
// State lives in process memory, or in a key-value server shared by several processes.

#include "cli/db/redis.h"
#include "cli/net/serve.h"

#include "core/templates/hash_map.h"

// One announcement body; secret announcements describe the identity gateway instead of a room.
struct OnlineRoom {
	PackedByteArray id; // Room name as UTF-8, empty for the identity gateway.
	PackedByteArray host; // Address clients connect to, as UTF-8 that may hold NUL.
	int64_t port = 0; // Port clients connect to.
	int64_t players = 0; // Players currently in the room.
	int64_t max = 0; // Room capacity.
	int64_t free = 0; // Free rooms behind the identity gateway.
	bool secret = false; // Whether this announces the identity gateway.
};

// Wire format helpers shared by the handler and tests.
namespace OnlineWire {
constexpr int BODY_MAX = 4096; // Announcement body bytes read before decoding.
constexpr int GRANT_LIFE = 60; // Seconds a hosting grant stays valid.
bool game_of(const String &p_path, String &r_game); // Extract the game name from a request path.
bool valid(const OnlineRoom &p_room); // Check the shape of an announcement.
bool decode(const PackedByteArray &p_body, bool p_more, OnlineRoom &r_room); // Decode one JSON body.
String grant(const String &p_key, int64_t p_now); // Mint "<expiry>.<nonce>.<signature>", or empty without a key.
String sign(const String &p_key, const String &p_payload); // Hex HMAC-SHA-256 of a grant payload.
PackedByteArray place(const Variant &p_room, const Variant &p_host, int64_t p_port, const String &p_grant, bool p_host_here); // Encode a destination as one JSON line.
PackedByteArray bytes(const Variant &p_value); // Convert a stored value to UTF-8 bytes.
int64_t number(const Variant &p_value); // Convert a stored decimal value, or zero when malformed.
int rune(const uint8_t *p_s, int p_n, char32_t &r_rune); // Decode one UTF-8 sequence and return its length.
} // namespace OnlineWire

// Room directory; each call returns a value and Err, or a Signal delivering both.
// Replies keep the key-value server's shapes so both directories share one reader:
// secret() gives [host, port, free] with a null host when absent, pick() gives [room, host, port] or null.
class OnlineStore {
protected:
	String prefix; // Key prefix separating deployments.
	int64_t alive_ms = 0; // Milliseconds an announcement stays live.

public:
	virtual ~OnlineStore() {} // Allow deletion through the base type.
	void setup(const String &p_prefix, int64_t p_alive_ms); // Set the key prefix and the live time.
	String head(const String &p_game) const; // Key prefix of one game.
	int64_t ttl(int p_times) const; // Whole-second expiry of a multiple of the live time.
	virtual VariantPair serve(const String &p_game, const OnlineRoom &p_room) = 0; // Record the identity gateway.
	virtual VariantPair secret(const String &p_game) = 0; // Return the identity gateway's address and free rooms.
	virtual VariantPair announce(const String &p_game, const OnlineRoom &p_room) = 0; // Record one room.
	virtual VariantPair pick(const String &p_game) = 0; // Return the fullest room with a free seat.
};

// Directory kept in this process.
class OnlineMemory : public OnlineStore {
	// One room record with its own expiry.
	struct Entry {
		PackedByteArray host; // Address clients connect to.
		int64_t port = 0, players = 0, max = 0, seen = 0; // Seen is wall-clock milliseconds.
		int64_t until = 0; // Expiry in wall-clock milliseconds.
	};
	// All records of one game.
	struct Game {
		HashMap<String, Entry> rooms; // Room records by name.
		HashMap<String, int64_t> index; // Room names scored by player count.
		int64_t index_until = 0; // Expiry of the whole index.
		OnlineRoom gate; // Identity gateway.
		int64_t gate_until = 0; // Expiry of the gateway record; zero when absent.
	};
	HashMap<String, Game> games; // Games by name.
	int64_t swept = 0; // Earliest time of the next sweep in wall-clock milliseconds.

	Game *live(const String &p_game, int64_t p_now); // Drop expired records and return the game, or null.
	void sweep(int64_t p_now); // Drop expired records of all games from time to time.

public:
	// Directory operations described on OnlineStore.
	VariantPair serve(const String &p_game, const OnlineRoom &p_room) override;
	VariantPair secret(const String &p_game) override;
	VariantPair announce(const String &p_game, const OnlineRoom &p_room) override;
	VariantPair pick(const String &p_game) override;
};

// Directory in a key-value server, sharing its layout with other matchmaking processes.
class OnlineRedis : public OnlineStore {
	Ref<GDRedisClient> db; // Opened connection.

public:
	// Use an opened connection.
	explicit OnlineRedis(const Ref<GDRedisClient> &p_db) :
			db(p_db) {}
	// Directory operations described on OnlineStore.
	VariantPair serve(const String &p_game, const OnlineRoom &p_room) override;
	VariantPair secret(const String &p_game) override;
	VariantPair announce(const String &p_game, const OnlineRoom &p_room) override;
	VariantPair pick(const String &p_game) override;
};

class GDOnlineMatch;

// One matchmaking request, suspended while the body or the directory is not ready.
class GDOnlineCall : public RefCounted {
	GDCLASS(GDOnlineCall, RefCounted);

	Ref<GDOnlineCall> self_hold; // Keep this request alive until it answers.
	Ref<GDOnlineMatch> owner; // Server settings and directory.
	Ref<GDWebRequest> req; // Request being answered.
	String game; // Game named by the path.
	PackedByteArray body; // Body bytes read so far.
	Variant out; // Reply produced before the caller waits.
	bool starting = false; // Whether the first step is still on the caller's stack.
	void (GDOnlineCall::*next)(const VariantPair &) = nullptr; // Step receiving the pending result.

	VariantPair run(void (GDOnlineCall::*p_step)()); // Run the first step and return its reply or a Signal.
	void wait(const VariantPair &p_pending, void (GDOnlineCall::*p_next)(const VariantPair &)); // Continue with a result now or later.
	void resume(const Variant &p_value, const Variant &p_error); // Receive a deferred result.
	void finish(const Variant &p_reply); // Deliver the reply exactly once.
	void read(); // Read the next body chunk.
	void got_chunk(const VariantPair &p_chunk); // Collect a body chunk and continue reading or decide.
	void route(); // Decide between announcement and matching.
	void match(); // Look up the identity gateway first.
	void got_secret(const VariantPair &p_found); // Send the client to the gateway, or pick a room when none is known.
	void got_room(const VariantPair &p_found); // Send the client to the picked room.
	void stored(const VariantPair &p_done); // Confirm a stored announcement.
	void announce(OnlineRoom p_room); // Check the key and shape, then store the announcement.

	friend class GDOnlineMatch;

protected:
	static void _bind_methods();

public:
	void cancel(); // Drop the request when the server abandons it.
};

// Matchmaking endpoint usable as a web handler or as its own server.
class GDOnlineMatch : public RefCounted {
	GDCLASS(GDOnlineMatch, RefCounted);

	String key; // Shared secret required for announcements; empty refuses them.
	OnlineStore *store = nullptr; // Room directory owned by this endpoint.
	Ref<GDWebApp> app; // Server created by listen.

	friend class GDOnlineCall;

protected:
	static void _bind_methods();

public:
	static VariantPair make(const Dictionary &p_opts); // Validate options and create an endpoint.
	static Ref<GDWebResponse> fail(int p_status, const String &p_msg); // Plain-text error reply.
	// Answer one matchmaking request.
	VariantPair handle(const Ref<GDWebRequest> &p_req);
	// Serve every path from a web application owned by this endpoint.
	VariantPair listen(int64_t p_port, const String &p_host);
	// Return the port opened by listen(), or 0 before it.
	int port() const { return app.is_valid() ? app->port() : 0; }
	// Stop the server opened by listen().
	void stop();
	~GDOnlineMatch(); // Release the directory.
};

// Entry point for online-game services.
class GDOnlineAPI : public Object {
	GDCLASS(GDOnlineAPI, Object);

protected:
	static void _bind_methods();

public:
	// Create a matchmaking endpoint.
	// @param opts `key` (announcement key, empty refuses announcements), `alive` (seconds an announcement is kept, default 5.0), key-value client `GD.database.redis` (an opened client sharing the directory), and `prefix` (key prefix, default "match").
	VariantPair match(const Dictionary &p_opts) const { return GDOnlineMatch::make(p_opts); }
};
