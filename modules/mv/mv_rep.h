/**************************************************************************/
/*  mv_rep.h                                                              */
/**************************************************************************/

// Delivers the contents under the Server to everyone, following the marks the author put on properties.
//
// Only changes are delivered. The last delivered value is kept per serial number and name,
// and nothing is emitted when it is unchanged. A mark with a rate thins updates down to that rate.
// Entries untouched the longest are dropped first, so the memory does not keep growing.

#pragma once

#include "mv_secret.h"

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/main/node.h"

// Interpolation method for Client display
enum SmoothMode {
	SMOOTH_NONE,
	SMOOTH_VALUE,
	SMOOTH_POSITION,
	SMOOTH_ANGLE,
	SMOOTH_DEGREES,
};

// Sync rule decided by Node and property. Fixed for the same object and name, so decided only once
struct SyncRule {
	SmoothMode smooth = SMOOTH_NONE; // Interpolation used for Client display
	int hz = 0; // Maximum send rate that auto selection may pick
	bool flow = false; // Whether to use latest-value delivery that tolerates loss
	bool extrapolate = false; // Whether to briefly extrapolate during packet loss
	bool arc = false; // Whether to keep a Character's jump acceleration
};

class MVRep : public RefCounted {
	GDCLASS(MVRep, RefCounted);

	// Memory for one property
	struct Cell {
		SyncRule rule; // Sync rule decided once
		bool rule_known = false; // Whether the rule is already decided
		Variant value; // Last delivered value
		Variant seen; // Last value observed for rate estimation
		uint32_t until = 0; // How long to keep this. Extended on every touch
		uint32_t due = 0; // Next tick allowed to look. Used for rate thinning
		uint32_t changed_at = 0; // Tick of the last observed change
		uint32_t key_due = 0; // Next tick to send a reliable latest value
		uint32_t window_at = 0; // Tick the auto rate review window began
		uint32_t observed_at = 0; // Previous change tick for auto promotion
		uint16_t window_changes = 0; // Changes within the review window
		uint8_t changes = 0; // Consecutive changes without a gap
		uint8_t auto_hz = 0; // Current auto rate picked from the Node's use
		uint8_t observed_streak = 0; // Changes in a short burst
		bool flowing = false; // Whether the last value sent unreliably needs confirming
		bool floor_known = false; // Whether the Character's floor state was observed once
		bool on_floor = false; // Whether the Character was on the floor in the last physics update
		bool gated_known = false; // Whether it was decided once if this is read only when written
		bool gated = false; // Script body variable (not a container). Not read on ticks without a written mark
	};

	// One continuous value shown on the Client
	struct Track {
		SyncRule rule; // Sync rule decided once
		uint8_t frames = 1; // Past frames mixed in when reading speed
		ObjectID node; // Node the display is written to
		StringName property; // Property the display is written to
		Variant from; // Current interpolation start value
		Variant to; // Latest Online side value
		Variant previous; // Previous Online side value for discontinuity checks
		Variant velocity; // Estimated velocity used to advance briefly during loss
		Variant acceleration; // Estimated acceleration keeping a Character's jump arc
		double elapsed = 0.0; // Seconds since interpolation began
		double duration = 0.0; // Interpolation seconds for this step
		uint64_t arrived = 0; // Arrival time of the previous sample
		uint32_t sample_tick = 0; // Tick at which the Server made the previous sample
		uint8_t mode = 0; // Interpolation mode picked from type and use
		bool active = false; // Whether updating every draw frame is needed
		bool extrapolate = false; // Whether to keep advancing briefly during packet loss
		bool character = false; // Whether this Node uses acceleration estimation including gravity
		bool motion = false; // Mark to skip estimating the discontinuous acceleration right after takeoff or landing
		bool on_floor = true; // Whether the Online side saw it on the floor. Vertical motion is advanced locally only while airborne
		Variant up; // Character up direction. The component along it is vertical
		Variant gravity; // Fall acceleration measured from airborne samples
	};

	HashMap<int64_t, HashMap<StringName, Cell>> last; // serial number -> name -> memory
	HashMap<int64_t, ObjectID> known; // serial number -> the object. Includes objects sent by full collection. Emits del when it disappears
	HashSet<int64_t> announced; // Objects whose spawn notice was emitted by normal collection. Full collection is not added, or the others would never get the spawn notice
	HashSet<String> warned_unnamed; // Types already warned that saved values of unnamed objects are not kept
	HashMap<int64_t, String> save_paths; // serial number -> stable path used to erase Secret saves
	HashMap<String, Dictionary> kept; // Secret path -> last stored @online_save values
	HashMap<int64_t, HashMap<StringName, Track>> tracks; // Continuous values shown on the Client
	int teleport_px = Secret::TELEPORT_DEFAULT; // Single-step movement treated as a teleport
	HashSet<int64_t> moving; // Node ids that still need interpolation this frame
	HashSet<ObjectID> dirty; // Objects whose variables are read in this collection. Only those written to marked variables since the last collection
	HashSet<String> warned; // Memory to avoid repeating the same binding error
	Vector<ObjectID> born; // Nodes that appeared in this delivery. Announced once set up
	uint32_t now = 0; // Delivery count
	uint32_t swept = 0; // Tick of the last sweep
	bool full = false; // Whether to collect everything
	bool smoothing = false; // Whether to auto-interpolate Client display only

	void walk(Node *p_root, Node *p_node, Dictionary &r_out, HashSet<int64_t> &r_seen);
	void save_only(const String &p_path, Node *p_node, Dictionary &r_out); // Collects only the Secret's kept values into the save box
	void send(Node *p_root, Node *p_node, int64_t p_uid, const String &p_own,
			const StringName &p_name, int p_flags, bool p_first, bool p_touched, Dictionary &r_out);
	void warn_field(Node *p_node, const StringName &p_name, const String &p_reason); // Reports a binding error only the first time
	bool smooth_field(Node *p_node, int64_t p_uid, const StringName &p_name, const Variant &p_value, int p_flags, uint32_t p_tick, bool p_flow, bool p_settle, bool p_motion, bool p_snap, const Dictionary &p_body); // Schedules a continuous value for the display target
	void sweep(); // Drops memory no longer touched
	void put(Dictionary &r_box, Node *p_node, int64_t p_uid, const StringName &p_name, const Variant &p_value); // Puts a field into the box under its number
	static PackedStringArray field_names(Node *p_node); // Order of fields sent on the wire (from the mark table; identical on both sides)
	int field_id(Node *p_node, const StringName &p_name); // field name -> number. -3 if none
	HashMap<ObjectID, HashMap<StringName, int>> field_ids; // Per-Node name -> number table
	static String field_name(Node *p_node, int p_id); // number -> field name. Empty if none

protected:
	static void _bind_methods();

public:
	// Values carried with a Character's position. Authors cannot write names starting with __, so they never collide.
	// The up direction is a scene property also present locally, so it is not carried
	static const char *FLOOR_KEY; // Whether on the floor
	static const char *MOTION_KEY; // Velocity at that moment

	enum {
		TICK_HZ = 20, // Ticks per second. Rate thinning divides this
		FLOW_GAP_TICKS = 10, // Maximum gap still counted as continuous change
		FLOW_START = 3, // Consecutive changes before switching to latest-first
		KEY_TICKS = 20, // Interval for sending a reliable latest value during continuous change
		RATE_WINDOW_TICKS = 200, // Review the auto rate every 10 seconds
		KEEP_TICKS = 3600, // Ticks to keep untouched memory. 3 minutes at 20Hz
		SWEEP_TICKS = 100, // Sweep interval
	};

	Dictionary collect(Node *p_root); // Collects only the changes
	Dictionary snapshot(Node *p_root); // Collects the current contents in full
	void save_secret(const String &p_path, Node *p_node, Dictionary &r_out); // Collects saved values of Secrets outside the world
	Array apply(Node *p_root, const Dictionary &p_frame); // Applies what arrived
	Dictionary restore(Node *p_root, const Dictionary &p_values); // Restores saved values to Nodes whose stable path matches
	Dictionary restore_node(Node *p_node, const Dictionary &p_values); // Restores saved values to one Node and returns what could not be placed
	static Dictionary save_of(Node *p_node); // Extracts values marked @online_save as they are now
	static bool spawn_property(const StringName &p_name); // Whether this is a standard display property allowed in spawn notices
	Node *at_uid(Node *p_root, int64_t p_uid) const; // Looks up an object by serial number
	void know(Node *p_node); // Remembers an existing object by serial number
	void reset(); // Drops remembered values
	void set_smoothing(bool p_enabled); // Toggles auto-interpolation of Client display
	void set_teleport_px(int p_px) { teleport_px = MAX(0, p_px); } // A single move beyond this is not smoothed
	void advance(double p_delta); // Advances only synced display by one draw frame
	const Vector<ObjectID> &newborn() const { return born; } // Returns Nodes that appeared in this delivery
	void forget_newborn() { born.clear(); } // Forgets the ones already announced
};
