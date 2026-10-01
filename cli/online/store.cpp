/**************************************************************************/
/*  store.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Keep the room directory in process memory or in a key-value server.
// Both follow the same expiry and selection rules so a server behaves the same with either.

#include "cli/online/match.h"
#include "cli/sys/clock.h"

#include "core/templates/sort_array.h"

namespace {

constexpr int PICK_SCAN = 200; // Fullest rooms examined per match.

// Choose the fullest live room; stale entries met on the way are removed.
// Keys: 1 = room index, 2 = room key prefix. Arguments: 1 = now in ms, 2 = live time in ms.
const char *const PICK_SCRIPT = R"(
local now = tonumber(ARGV[1])
local ids = redis.call('ZREVRANGE', KEYS[1], 0, 199)
for _, id in ipairs(ids) do
  local key = KEYS[2] .. id
  local room = redis.call('HMGET', key, 'host', 'port', 'players', 'max', 'seen')
  if not room[1] then
    redis.call('ZREM', KEYS[1], id)
  else
    local players = tonumber(room[3]) or 0
    local capacity = tonumber(room[4]) or 0
    local seen = tonumber(room[5]) or 0
    if now - seen > tonumber(ARGV[2]) then
      redis.call('DEL', key)
      redis.call('ZREM', KEYS[1], id)
    elseif players < capacity then
      return {id, room[1], room[2]}
    end
  end
end
return nil
)";

// Return wall-clock milliseconds, the time base shared with other server instances.
int64_t now_ms() {
	return int64_t(GDClock::unix_time() * 1000.0);
}

// Order index entries fullest first, then by name descending.
struct Fuller {
	bool operator()(const Pair<String, int64_t> &p_a, const Pair<String, int64_t> &p_b) const {
		return p_a.second != p_b.second ? p_a.second > p_b.second : p_b.first < p_a.first;
	}
};

} // namespace

// Set the key prefix and the live time.
void OnlineStore::setup(const String &p_prefix, int64_t p_alive_ms) {
	prefix = p_prefix;
	alive_ms = p_alive_ms;
}

// Separate games under the prefix; the unnamed game uses the prefix alone.
String OnlineStore::head(const String &p_game) const {
	return p_game.is_empty() ? prefix : prefix + ":" + p_game;
}

// Convert a multiple of the live time to whole seconds, at least one.
int64_t OnlineStore::ttl(int p_times) const {
	const int64_t ms = alive_ms * p_times;
	return ms < 1000 ? 1 : ms / 1000;
}

// Return a game after dropping its expired index and gateway, or null when nothing remains.
OnlineMemory::Game *OnlineMemory::live(const String &p_game, int64_t p_now) {
	Game *game = games.getptr(p_game);
	if (!game) {
		return nullptr;
	}
	// Rooms are reachable only through the index, so they expire with it.
	if (game->index_until <= p_now) {
		game->index.clear();
		game->rooms.clear();
	}
	if (game->gate_until <= p_now) {
		game->gate_until = 0;
	}
	if (game->index.is_empty() && game->gate_until == 0) {
		games.erase(p_game);
		return nullptr;
	}
	return game;
}

// Forget expired records of every game at most once per room lifetime,
// so rooms that stop announcing do not accumulate while nobody asks for a match.
void OnlineMemory::sweep(int64_t p_now) {
	if (p_now < swept) {
		return;
	}
	swept = p_now + ttl(2) * 1000;
	LocalVector<String> names;
	for (KeyValue<String, Game> &one : games) {
		LocalVector<String> dead;
		for (const KeyValue<String, Entry> &room : one.value.rooms) {
			if (room.value.until <= p_now) {
				dead.push_back(room.key);
			}
		}
		for (const String &id : dead) {
			one.value.rooms.erase(id);
			one.value.index.erase(id);
		}
		names.push_back(one.key);
	}
	for (const String &name : names) {
		live(name, p_now);
	}
}

// Record the identity gateway for twice the live time.
VariantPair OnlineMemory::serve(const String &p_game, const OnlineRoom &p_room) {
	const int64_t now = now_ms();
	sweep(now);
	live(p_game, now);
	Game &game = games[p_game];
	game.gate = p_room;
	game.gate_until = now + ttl(2) * 1000;
	return {};
}

// Return the gateway's host, port, and free count.
VariantPair OnlineMemory::secret(const String &p_game) {
	const Game *game = live(p_game, now_ms());
	if (!game || game->gate_until == 0) {
		return { Array{ Variant(), Variant(), Variant() }, Variant() };
	}
	return { Array{ game->gate.host, game->gate.port, game->gate.free }, Variant() };
}

// Record one room and extend both its record and the game's index.
VariantPair OnlineMemory::announce(const String &p_game, const OnlineRoom &p_room) {
	const int64_t now = now_ms();
	sweep(now);
	live(p_game, now);
	const String id = String::utf8((const char *)p_room.id.ptr(), p_room.id.size());
	Game &game = games[p_game];
	Entry &entry = game.rooms[id];
	entry.host = p_room.host;
	entry.port = p_room.port;
	entry.players = p_room.players;
	entry.max = p_room.max;
	entry.seen = now;
	entry.until = now + ttl(2) * 1000;
	game.index[id] = p_room.players;
	game.index_until = now + ttl(4) * 1000;
	return {};
}

// Return the fullest live room with a free seat among the fullest entries.
VariantPair OnlineMemory::pick(const String &p_game) {
	const int64_t now = now_ms();
	Game *game = live(p_game, now);
	if (!game) {
		return {};
	}
	LocalVector<Pair<String, int64_t>> order;
	for (const KeyValue<String, int64_t> &one : game->index) {
		order.push_back(Pair<String, int64_t>(one.key, one.value));
	}
	// Only the fullest entries are examined, so only they are ordered.
	const int64_t scan = MIN(int64_t(order.size()), int64_t(PICK_SCAN));
	SortArray<Pair<String, int64_t>, Fuller>().partial_sort(0, order.size(), scan, order.ptr());
	for (int64_t i = 0; i < scan; i++) {
		const String &id = order[i].first;
		const Entry *entry = game->rooms.getptr(id);
		// Forget rooms whose record expired or which stopped announcing.
		if (!entry || entry->until <= now || now - entry->seen > alive_ms) {
			game->index.erase(id);
			game->rooms.erase(id);
		} else if (entry->players < entry->max) {
			return { Array{ id, entry->host, entry->port }, Variant() };
		}
	}
	return {};
}

// Write the gateway's address and free count atomically.
VariantPair OnlineRedis::serve(const String &p_game, const OnlineRoom &p_room) {
	const String key = head(p_game) + ":secret";
	return { db->transaction(Array{
			Array{ "HSET", key, "host", p_room.host, "port", p_room.port, "free", p_room.free },
			Array{ "EXPIRE", key, ttl(2) } }), Variant() };
}

// Read the gateway's address and free count.
VariantPair OnlineRedis::secret(const String &p_game) {
	return { db->query("HMGET", Array{ head(p_game) + ":secret", "host", "port", "free" }), Variant() };
}

// Write one room and extend its index atomically.
VariantPair OnlineRedis::announce(const String &p_game, const OnlineRoom &p_room) {
	const String id = String::utf8((const char *)p_room.id.ptr(), p_room.id.size());
	const String key = head(p_game) + ":room:" + id;
	const String index = head(p_game) + ":rooms";
	return { db->transaction(Array{
			Array{ "HSET", key, "host", p_room.host, "port", p_room.port, "players", p_room.players, "max", p_room.max, "seen", now_ms() },
			Array{ "EXPIRE", key, ttl(2) },
			Array{ "ZADD", index, p_room.players, id },
			Array{ "EXPIRE", index, ttl(4) } }), Variant() };
}

// Pick a room on the server so concurrent matchers never race.
VariantPair OnlineRedis::pick(const String &p_game) {
	return { db->query("EVAL", Array{ PICK_SCRIPT, 2, head(p_game) + ":rooms", head(p_game) + ":room:", now_ms(), alive_ms }), Variant() };
}
