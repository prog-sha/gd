/**************************************************************************/
/*  mv_runtime.h                                                          */
/**************************************************************************/

// Runs both the Online world and the Client world with one runtime. The only difference is the authority switch.
// The Online side runs author scripts and distributes deltas; the Client side only reads and sends commands to the Online side.

#pragma once

#include "mv_gate.h"
#include "mv_rep.h"
#include "mv_reply.h"
#include "mv_online.h"
#include "mv_saves.h"

#include "core/object/object_id.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/main/node.h"

class MVClient;
class MY;
class Online;
class Secret;
class Script;

// Port for asking a Secret whose script lives in another process.
// Lets a world holding its own Secret and a world whose host holds it be swapped in the same shape
class MVSecretLine {
public:
	virtual ~MVSecretLine() {}

	virtual bool call_secret(int64_t p_id, const String &p_path, const String &p_name, const Array &p_args) = 0; // Asks the side holding the instance to call a function
};

class MVRuntime : public Node {
	GDCLASS(MVRuntime, Node);

	bool authority = false; // Whether this is the Online side. If false, this is the Client world: scripts do not run, it only sends commands
	String me; // Client world only. The player's own Player name decided by the Secret
	HashMap<int64_t, Dictionary> input_sent; // Client only. Last values of the player's own @online_input sent to the Online side
	uint64_t input_at = 0; // Client only. Time input was last sent
	Array events; // Client world only. Queue of one-shot commands in call order
	Dictionary streams; // Latest arguments per function name called from _process
	Dictionary stream_replies; // Reply holders waiting for an answer, per latest input
	PackedStringArray stream_order; // Order for sending continuous inputs fairly
	Array sent_at; // Send times within the last second
	uint64_t stream_at = 0; // Time continuous input was last sent
	bool warned_full = false; // Marks that command overflow has been reported once
	Ref<MVGate> gate; // Gate that checks creation and deletion under the Server
	Ref<MVRep> rep; // Replicator that collects only changed public values
	ObjectID server; // Fixed Node holding the author's Server script
	HashMap<String, ObjectID> live; // Maps world paths to running Nodes
	MVSaves *saves = nullptr; // Save store in use. Nothing is saved without one
	Dictionary save_live; // Current save values collected per world path
	Dictionary drop_later; // Ownerless Nodes removed while the store was down: path -> names to delete. Dropped after reconnecting
	Vector<Dictionary> user_later; // Last save values of players who left while the store was down. Stored after reconnecting
	HashMap<String, String> owned_saves; // Owner and kind -> path of the last thing stored. Used to notice a second one
	HashSet<String> warned_saves; // Owner and kind pairs already warned about holding two of the same kind
	HashMap<String, ObjectID> secrets; // Maps world paths to Secret instances moved out of the world
	MVSecretLine *secret_line = nullptr; // Port for asking when the instance lives in another process
	Dictionary door_api; // Callable Secret entry declarations per world path
	HashSet<String> remote_secrets; // World paths of Secrets whose instance is not local
	HashMap<int64_t, Ref<MVReply>> secret_waiting; // Reply holders waiting for Secret answers, per request number
	int64_t secret_asked = 0; // Number assigned to each Secret request
	bool secret_only = false; // Whether this side handles only identity and Secret scripts without running the world
	Dictionary pending_world; // Save values of ownerless Nodes that have not appeared yet
	Dictionary pending_user; // Save values not yet applied, per Player name
	HashMap<String, Ref<MY>> users; // Maps participant names to their my
	Dictionary apis; // Maps Node numbers to Client function and sync declarations
	Dictionary apis_dirty; // Only public APIs changed since the last distribution
	HashMap<String, Ref<Script>> scripts; // Online-side scripts reused per scene kind
	Array fires; // @online signals distributed to everyone this tick
	Dictionary fires_my; // @online_my signals per Player name
	Dictionary carry_my; // @online_my signals carried over to Players not yet sent the full state
	Dictionary carry_all; // Signals for everyone carried over to Players not yet sent the full state
	HashSet<String> joined; // Player names already sent the full state
	String current; // World path of the author Node running now
	Vector<String> context_stack; // Execution positions to return to from nested author calls
	Vector<String> server_stack; // Execution positions before calling a Server method
	uint64_t frame = 0; // Number of times the Online world has advanced
	bool users_dirty = true; // Marks the participant list for the next delta
	bool warned_fires = false; // Marks that signal overflow has been reported once
	bool saving = false; // Whether the last distribution could write to the save store
	bool closing = false; // Whether the world has started closing. Once closing, the gate holds nothing
	bool world_ready = false; // Whether saves are restored, scripts attached, and the world running
	String world_path; // Path of the world's own script
	Callable ready_done; // Called once the world starts running
	void world_loaded(const Dictionary &p_saved); // World save values have returned. Attaches scripts and starts the world
	void user_loaded(const Dictionary &p_saved, const String &p_who, bool p_fresh); // Records the player's save values and places them or restores them onto existing things
	void reply_later(const Variant &p_value, const Variant &p_reply); // Puts the answer into the reply holder on the next idle

	bool dress(Node *p_node, const String &p_kind, const String &p_path, bool p_server); // Attaches the Online-side script and sync marks to a Node
	Dictionary api_of(const Ref<Script> &p_script, const String &p_author_path) const; // Builds the public API table handed to Clients
	void begin_path(const String &p_path); // Begins author processing at the given world path
	void begin(Node *p_node); // Begins execution rights for an author Node
	void end(); // Closes execution rights for an author Node
	void refuse(const String &p_who, const String &p_reason); // Drops a rejected command and records the reason in the admin log
	void overflowed(const String &p_name); // Reports signal overflow once
	void refused_spawn(const String &p_kind); // Tells the author why placement failed
	void fire(int64_t p_uid, const String &p_name, const String &p_to, const Array &p_args); // Queues a signal for the next delta
	friend class MVFire; // Only the arity-agnostic relay calls fire
	void save_out(const Dictionary &p_frame); // Adds save-box deltas to the full set and flushes to Redis
	void save_entry(const String &p_path, const Dictionary &p_entry, const Dictionary &p_values); // Stores only the p_values names of one thing's save values, by path or by player
	Vector<Node *> owned(const String &p_who) const; // Things owned by this player
	void restore_in(Node *p_node, const String &p_path, const String &p_own); // Restores save values onto a spawned Node
	Node *settle_node(Node *p_parent, Node *p_node, const String &p_own); // Adds a spawned Node to the tree and finishes its Online-side setup
	void carry_fire(const String &p_who, const Array &p_fires, Dictionary &r_carry); // Carries signals over to a player who has not joined yet
	void save_last(Node *p_node, const String &p_who); // Stores the last @online_save values of a leaving Player
	void dress_secrets(Node *p_node); // Moves Secrets under Online out of the world
	void collect_secrets(Node *p_node, Vector<ObjectID> &r_found); // Collects Secrets inside the world
	void move_secret(Secret *p_inner); // Moves a Secret instance out and places an entry at the same spot
	void put_door(Secret *p_door, const String &p_source, const String &p_name, const Dictionary &p_doors); // Attaches a script with only the callable functions to an entry
	void dress_fixed(Node *p_node); // Attaches the Online-side script to fixed Nodes under Online
	void push_event(int64_t p_uid, const String &p_fn, const Array &p_args, const Ref<MVReply> &p_reply); // Records one-shot commands in order within the limit
	static Ref<MVReply> new_reply(); // Creates one reply holder
	static void answer(const Variant &p_reply, const Variant &p_value); // Returns an answer to the waiting script
	static Array pack_nodes(const Array &p_args); // Replaces Node arguments with serial numbers
	static bool sane_call(const String &p_fn, const Array &p_args); // Checks whether a command can be sent and tells the author why not
	void drop_stream(const String &p_fn, bool p_keep); // Removes a latest input and returns it to events if needed

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	~MVRuntime();
	String start(bool p_authority, const Callable &p_ready = Callable()); // Builds the world. Calls p_ready once it starts running
	Online *server_node() const; // Returns the live Server
	void set_me(const String &p_me) { me = p_me; } // Tells the Client world the player's own Player name
	Signal ask(int64_t p_uid, const String &p_fn, const Array &p_args); // Sends one-shot commands in order
	Signal ask_latest(int64_t p_uid, const String &p_fn, const Array &p_args); // Sends only the latest value of _process input
	bool flush_commands(); // Sends one event or latest input within the limit
	void clear_commands(); // Discards commands from before a disconnect
	void set_saves(MVSaves *p_saves) { saves = p_saves; } // Binds the @online_save store
	void set_secret_only(bool p_on) { secret_only = p_on; } // Handles only Secret scripts without running the world
	void close_world(); // Announces closing and stops the gate from admitting
	bool is_closing() const { return closing; } // Whether closing is in progress
	void set_secret_line(MVSecretLine *p_line, const Dictionary &p_doors); // Binds the peer holding Secret instances and the callable function declarations
	Dictionary secret_doors() const; // Returns callable Secret entry declarations per world path
	bool is_secret_path(const String &p_path) const { return secrets.has(p_path); } // Whether the path belongs to a Secret
	void run_secret(const String &p_path, const String &p_name, const Array &p_args, const Callable &p_done); // Calls a local Secret instance directly
	void secret_answered(int64_t p_id, const Variant &p_value); // Hands a requested Secret answer to its waiting reply holder
	void secret_lost(); // The link to the Secret is lost. Closes waiting answers empty
	void tick_secret(); // Stores only Secret save values without running the world
	Dictionary tick(); // Advances physics once and returns the change delta
	Dictionary snapshot(const String &p_who); // Returns the full state for a late joiner
	Dictionary for_player(const Dictionary &p_frame, const String &p_who) const; // Cuts a delta down for one player
	bool join_context(const String &p_who, const Dictionary &p_context, bool p_on = true, const Dictionary &p_saved = Dictionary()); // Binds an auth context to a Player. p_saved holds the player's save values read by identity
	bool room_full_for(const String &p_who) const; // Whether no seat is left for that player to newly join
	void disconnect_player(const String &p_who); // Notifies a Player of disconnection
	Array unpack_nodes(const Array &p_args, bool &r_gone, bool &r_outside); // Maps Nodes received as serial numbers back to this world. Sets r_outside when a thing outside the world is named
	void ask_player(const String &p_who, int64_t p_uid, const String &p_fn, const Array &p_args, const Callable &p_done); // Calls only @online functions on the player's own things and hands the answer to p_done
	void finish(const Variant &p_value, const String &p_fn, const Callable &p_done); // Converts an answer to a sendable form and hands it over
	void begin_local(); // Begins a section of script shared by everyone
	void end_local(); // Closes a section of script shared by everyone
	bool in_local() const; // Whether inside script shared by everyone
	int local_depth = 0; // Nesting depth of script shared by everyone

	String where(Node *p_node) const; // Returns the path seen from the world root
	bool in_world(Node *p_node) const; // Whether the Node belongs to the world (under Online)
	String own_of(Node *p_node) const; // Returns the authenticated owner of a Node
	int player_count() const { return int(users.size()); } // Number of players in the world
	bool is_my(Node *p_node) const;
	Node *spawn_node(Node *p_parent, const String &p_kind, const String &p_own, const Dictionary &p_props); // Spawns a scene with sync. Refused on the Client
	Ref<MY> my_of(const String &p_who) const; // Returns that player's my
	bool queue_node(Node *p_node); // Passes queue_free through the Online-side gate
	bool free_node(Node *p_node); // Passes free() through the gate and lets the release continue
	Signal secret_call(Node *p_door, const String &p_name, const Array &p_args); // Calls a Secret instance through its entry
	bool tree_node(Node *p_parent, Node *p_child) const; // Blocks direct author changes to the synced tree
	void begin_server_call(); // Switches rights to the Server only during a Server method
	void end_server_call(); // Returns to the execution position after leaving a Server method
};
