/**************************************************************************/
/*  mv_store.h                                                            */
/**************************************************************************/

// Save port for the Server template that stores @online_save values in Redis.
// Never blocks. Commands are sent without waiting, and replies go to Callables in arrival order.
// A read answer is the contents on success (possibly empty) and null on failure; "missing" and "unreadable" are never mixed.
// Mixing them would let a player whose read failed enter with initial values and erase the real data on the next save

#pragma once

#include "mv_saves.h"

#include "core/io/stream_peer_tcp.h"
#include "core/object/object.h"
#include "core/templates/list.h"
#include "core/variant/dictionary.h"

// Not exposed to ClassDB. No save API is added for GDScript; the @online_save mark is the only entry point
class MVStore : public Object, public MVSaves {
	GDCLASS(MVStore, Object);

	Ref<StreamPeerTCP> tcp; // The single connection to Redis
	String prefix; // World name prefixed to every key
	String password;
	IPAddress address;
	int port = 0;
	bool live = false; // Whether connected and able to send commands
	bool wanted = false; // Whether this world has a configured destination. Reconnects when the link drops
	bool multi = false; // MULTI has been sent and EXEC is pending
	bool saving = false; // Inside begin_save
	uint64_t retry_at = 0; // Time of the next reconnect attempt
	uint64_t asked_at = 0; // Send time of the oldest unanswered command. 0 when nothing is pending
	uint64_t connect_at = 0; // Time the connection attempt started
	String last_error; // Last reason Redis refused. Avoids repeating the same reason
	Callable world_done; // Receiver waiting for the world save. Asked repeatedly until it can be read
	bool world_asked = false; // World save has been requested and a reply is pending
	uint64_t world_retry_at = 0; // Time to request the world save again
	PackedByteArray outbox; // Commands requested before connecting
	PackedByteArray inbox; // Partially read replies
	List<Callable> waiting; // Receivers of replies, in send order. Empty Callables are discarded

	static String setting(const Dictionary &p_wrote, const String &p_name, const String &p_default); // Reads a redis setting from the Secret

	static PackedByteArray encode(const Vector<PackedByteArray> &p_words);
	void send(const Vector<PackedByteArray> &p_words, const Callable &p_done = Callable()); // Sends one RESP array
	int parse(int64_t &r_at, Variant &r_out, int p_depth); // Parses one reply from inbox
	void read(); // Parses arrived replies and hands them out in order
	void hash_got(const Variant &p_reply, const Callable &p_done); // Hands the HGETALL reply as a dictionary. null on failure
	void text_got(const Variant &p_reply, const Callable &p_done); // Hands a text reply. null on failure
	void auth_got(const Variant &p_reply); // AUTH reply
	void world_got(const Variant &p_reply); // World save reply. On failure, asks again after a delay
	void set_got(const Variant &p_reply, const Callable &p_done); // SET reply. true if bound, false if already taken, null on failure
	void ask_world(); // Requests the world save
	static Dictionary refused(const String &p_why); // Failure reply
	static bool is_refused(const Variant &p_reply); // Whether a reply is a failure
	bool user_key(const String &p_id, String &r_key) const; // Builds a user key from a pseudonymous ID
	void put(const String &p_key, const String &p_head, const Dictionary &p_values); // Writes one thing's values as one field per name
	void wipe(const String &p_key, const String &p_head, const PackedStringArray &p_fields); // Deletes one thing's fields
	void begin_multi(); // Opens MULTI when inside a batched write
	void fail(const String &p_why); // Drops the connection and closes pending answers as failed

protected:
	static void _bind_methods() {}

public:
	enum {
		NEST_MAX = 4, // Maximum depth of nested arrays
		RETRY_MS = 5000, // Interval between reconnects to a lost Redis
		REPLY_MS = 10000, // Maximum wait to connect or for a reply. Treated as disconnected after this
	};

	bool open(); // Starts connecting if configured. Otherwise nothing is saved
	bool is_open() const override { return wanted && tcp.is_valid(); } // Includes the connecting state. Commands flow once connected
	bool is_wanted() const { return wanted; } // Whether this world has a configured destination
	void poll() override; // Connects, advances replies, and reconnects when dropped
	void close(); // Closes the connection

	void load_user(const String &p_id, const Callable &p_done); // Hands the user's save values as per-kind dictionaries. null on failure
	void load_world(const Callable &p_done) override;
	void save_user(const String &p_id, const String &p_kind, const Dictionary &p_values) override;
	void save_world(const String &p_path, const Dictionary &p_values) override;
	void drop_world(const String &p_path, const PackedStringArray &p_fields) override;
	void begin_save() override; // Groups everything until end_save into one MULTI/EXEC
	void end_save() override;

	void account_of(const String &p_device, const Callable &p_done); // Hands the account a device key points to to p_done. Empty if none, null on failure
	void bind_account(const String &p_device, const String &p_account, bool p_overwrite, const Callable &p_done); // Binds a device key to an account. true if bound, false if already taken, null on failure
	void unbind_account(const String &p_device, const Callable &p_done); // Unbinds a device key. true if removed, null on failure
	void profile_of(const String &p_account, const Callable &p_done); // Reads the linked account's display name. Empty if unlinked, null on failure
	void save_profile(const String &p_account, const String &p_name, const Callable &p_done); // Persists a linked account's display name
	void del_got(const Variant &p_reply, const Callable &p_done); // DEL reply
};
