/**************************************************************************/
/*  mv_rep.cpp                                                            */
/**************************************************************************/

// Collects diffs of Online side Nodes following @online marks and applies them safely to the public world.

#include "mv_rep.h"

#include "mv_client_script.h"
#include "mv_frame.h"
#include "mv_gate.h"
#include "mv_secret.h"

#include "core/math/math_funcs.h"
#include "core/os/time.h"
#include "scene/2d/physics/character_body_2d.h"
#include "scene/3d/physics/character_body_3d.h"

#include "modules/gdscript/gdscript_online.h"

// Opens the script read-only boundary only while Online side diffs are written into the Client copy
class MVTrustedWriteScope {
public:
	MVTrustedWriteScope() { GDScriptOnline::begin_trusted_write(); }
	~MVTrustedWriteScope() { GDScriptOnline::end_trusted_write(); }
};

// Makes arrays and dictionaries placed on the Client independent values that are immutable down to nested levels
static Variant client_readonly(const Variant &p_value) {
	if (p_value.get_type() == Variant::ARRAY) {
		Array out = Array(p_value).duplicate(false);
		for (int i = 0; i < out.size(); i++) {
			out[i] = client_readonly(out[i]);
		}
		out.make_read_only();
		return out;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary out = Dictionary(p_value).duplicate(false);
		for (const Variant &key : out.keys()) {
			out[key] = client_readonly(out[key]);
		}
		out.make_read_only();
		return out;
	}
	return p_value;
}

// Finds initial spawns not yet collected by uid when restoring saves
static Node *find_uid(Node *p_node, int64_t p_uid) {
	if (int64_t(p_node->get_meta(SNAME("uid"), 0)) == p_uid) {
		return p_node;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *found = find_uid(p_node->get_child(i), p_uid);
		if (found != nullptr) {
			return found;
		}
	}
	return nullptr;
}

// Checks only the relative path text for being under the Server, so Nodes that do not exist yet can be awaited
static bool stable_path(const String &p_path) {
	const NodePath path(p_path);
	if (path.is_absolute() || path.get_subname_count() != 0 || path.get_name_count() < 1 ||
			path.get_name_count() > MVGate::DEPTH_MAX || String(path.get_name(0)) != MVGate::SERVER) {
		return false;
	}
	for (int i = 0; i < path.get_name_count(); i++) {
		if (!MVGate::safe_name(path.get_name(i))) {
			return false;
		}
	}
	return true;
}

// Safely looks up a property of the current Node or its child from the mark's name
static Node *field_target(Node *p_node, const StringName &p_name, StringName &r_property) {
	const String field = p_name;
	const int dot = field.rfind_char('.');
	r_property = dot < 0 ? StringName(field) : StringName(field.substr(dot + 1));
	if (!MVGate::safe_name(r_property)) {
		return nullptr;
	}
	if (dot < 0) {
		return p_node;
	}
	// The display target may be in another scene. An absolute path is followed from the tree root
	const NodePath path(field.substr(0, dot));
	const int limit = path.is_absolute() ? int(MVGate::DEPTH_MAX) * 2 : int(MVGate::DEPTH_MAX);
	if (path.get_subname_count() != 0 || path.get_name_count() < 1 || path.get_name_count() > limit) {
		return nullptr;
	}
	for (int i = 0; i < path.get_name_count(); i++) {
		if (!MVGate::safe_name(path.get_name(i))) {
			return nullptr;
		}
	}
	return p_node->get_node_or_null(path);
}

// Reads the property named by the mark. Paths that do not exist are not delivered
static bool read_field(Node *p_node, const StringName &p_name, Variant &r_value) {
	StringName property;
	Node *target = field_target(p_node, p_name, property);
	if (target == nullptr) {
		return false;
	}
	bool valid = false;
	r_value = target->get(property, &valid);
	return valid;
}

// Writes the Online side variable, and on the Client copy also applies it to the given display target
static bool write_field(Node *p_node, const StringName &p_name, const Variant &p_value) {
	StringName property;
	Node *target = field_target(p_node, p_name, property);
	if (target == nullptr) {
		return false;
	}
	bool valid = false;
	target->set(property, p_value, &valid);
	return valid;
}

// Returns the Client display target bound by an Online side variable, or the variable itself
static Node *shown_target(Node *p_node, const StringName &p_name, StringName &r_property) {
	const Dictionary bindings = p_node->get_meta(SNAME("bind"), Dictionary());
	const String key = p_name;
	return field_target(p_node, bindings.has(key) ? StringName(String(bindings[key])) : p_name, r_property);
}

// Puts a value into the Client display target. It is a display spot, so the type is matched here
static bool write_shown(Node *p_node, const StringName &p_name, const Variant &p_value) {
	StringName property;
	Node *target = shown_target(p_node, p_name, property);
	if (target == nullptr) {
		return false;
	}
	Variant value = p_value;
	bool has = false;
	const Variant::Type want = target->get(property, &has).get_type();
	// Numbers do not go into text as is, so convert them to strings
	if (has && (want == Variant::STRING || want == Variant::STRING_NAME) &&
			!Variant::can_convert_strict(value.get_type(), want)) {
		value = want == Variant::STRING ? Variant(p_value.operator String()) : Variant(StringName(p_value.operator String()));
	}
	bool valid = false;
	target->set(property, value, &valid);
	return valid;
}

// Decides whether this is a standard display property that can be set before Node creation
bool MVRep::spawn_property(const StringName &p_name) {
	return p_name == SNAME("position") || p_name == SNAME("rotation") || p_name == SNAME("scale") ||
			p_name == SNAME("visible") || p_name == SNAME("modulate");
}

// Passes arrived value changes to the common signal. The first arrival is not a change, so it is not emitted
static void emit_online_changed(Node *p_node, const StringName &p_name, const Variant &p_value, const Variant &p_previous) {
	if (p_value == p_previous || p_previous.get_type() == Variant::NIL ||
			!p_node->has_signal(SNAME("online_changed"))) {
		return;
	}
	const Variant name = p_name;
	const Variant *args[] = { &name, &p_value, &p_previous };
	p_node->emit_signalp(SNAME("online_changed"), args, 3);
}

// Stops physics movement of the Client copy and displays it only by interpolating Online side coordinates
static void freeze_client_body(Node *p_node, bool p_smoothing) {
	if (!p_smoothing || p_node == nullptr ||
			(!p_node->is_class("RigidBody2D") && !p_node->is_class("RigidBody3D"))) {
		return;
	}
	bool valid = false;
	p_node->set(SNAME("freeze"), true, &valid);
}

// Hidden display is not interpolated; it is aligned to the latest state before becoming visible again
static bool hidden_display(Node *p_node) {
	bool valid = false;
	const Variant visible = p_node->get(SNAME("visible"), &valid);
	return valid && visible.get_type() == Variant::BOOL && !bool(visible);
}

// Classifies the display value interpolation method from Node and property
static SmoothMode smooth_mode(Node *p_node, const StringName &p_property, Variant::Type p_type) {
	const String name = p_property;
	if (p_node->is_class("Range") && name == "value") {
		return SMOOTH_VALUE;
	}
	// Colors are picked up by type, not name. Control modulate is also caught here
	if (p_type == Variant::COLOR) {
		return SMOOTH_VALUE;
	}
	if (p_node->is_class("Control")) {
		return SMOOTH_NONE;
	}
	const bool spatial = p_node->is_class("Node2D") || p_node->is_class("Node3D");
	if (!spatial) {
		return SMOOTH_NONE;
	}
	if (name == "position" || name == "global_position") {
		return SMOOTH_POSITION;
	}
	if (name == "rotation" || name == "global_rotation" || name == "skew" || name == "global_skew") {
		return SMOOTH_ANGLE;
	}
	if (name == "rotation_degrees" || name == "global_rotation_degrees") {
		return SMOOTH_DEGREES;
	}
	if (name == "transform" || name == "global_transform" || name == "basis" ||
			name == "global_basis" || name == "quaternion" || name == "scale" || name == "global_scale") {
		return SMOOTH_VALUE;
	}
	if (p_node->is_class("Camera2D") || p_node->is_class("Camera3D")) {
		if (name == "offset" || name == "zoom" || name == "fov" || name == "size" || name == "frustum_offset") {
			return SMOOTH_VALUE;
		}
	}
	return SMOOTH_NONE;
}

// Gives the auto rate and display prediction to a classified Node use
static SyncRule auto_rule(SmoothMode p_smooth, const StringName &p_property,
		bool p_character, bool p_body, bool p_camera, bool p_spatial, bool p_slow) {
	SyncRule rule;
	rule.smooth = p_smooth;
	if (rule.smooth == SMOOTH_NONE) {
		return rule;
	}
	if (p_slow) {
		rule.hz = 5;
		return rule;
	}
	const bool position = rule.smooth == SMOOTH_POSITION;
	const bool moving = p_spatial && p_property != SNAME("scale") && p_property != SNAME("global_scale");
	rule.hz = p_body || p_camera || moving ? 20 : 10;
	rule.flow = p_property != SNAME("scale") && p_property != SNAME("global_scale");
	rule.extrapolate = position;
	rule.arc = p_character && position;
	return rule;
}

// Decides the sync rule from a Node and property whose display target is known
static SyncRule rule_for(Node *p_node, Node *shown, const StringName &property, Variant::Type p_type) {
	if (shown == nullptr) {
		return SyncRule();
	}
	const bool character = p_node->is_class("CharacterBody2D") || p_node->is_class("CharacterBody3D") ||
			shown->is_class("CharacterBody2D") || shown->is_class("CharacterBody3D");
	const bool body = character ||
			p_node->is_class("RigidBody2D") || p_node->is_class("RigidBody3D") ||
			p_node->is_class("VehicleBody3D") || p_node->is_class("AnimatableBody2D") || p_node->is_class("AnimatableBody3D") ||
			shown->is_class("RigidBody2D") || shown->is_class("RigidBody3D") ||
			shown->is_class("VehicleBody3D") || shown->is_class("AnimatableBody2D") || shown->is_class("AnimatableBody3D");
	const bool camera = p_node->is_class("Camera2D") || p_node->is_class("Camera3D") ||
			shown->is_class("Camera2D") || shown->is_class("Camera3D");
	const bool spatial = p_node->is_class("Node2D") || p_node->is_class("Node3D") ||
			shown->is_class("Node2D") || shown->is_class("Node3D");
	const bool slow = shown->is_class("Range") || p_type == Variant::COLOR;
	return auto_rule(smooth_mode(shown, property, p_type), property, character, body, camera, spatial, slow);
}

// Routes only fixed-size continuous values that can be interpolated through the loss-tolerant latest-value path
static bool can_flow(Variant::Type p_type) {
	switch (p_type) {
		case Variant::FLOAT:
		case Variant::VECTOR2:
		case Variant::VECTOR3:
		case Variant::VECTOR4:
		case Variant::RECT2:
		case Variant::QUATERNION:
		case Variant::TRANSFORM2D:
		case Variant::TRANSFORM3D:
			return true;
		default:
			return false;
	}
}

// Overrides the automatic choice with the smoothing the author picked, as in @online(position, 20, "arc")
static SyncRule with_choice(SyncRule p_rule, int p_flags, Variant::Type p_type) {
	const int choice = GDScriptOnline::smooth_of(p_flags);
	if (choice == GDScriptOnline::S_AUTO) {
		return p_rule;
	}
	if (choice == GDScriptOnline::S_SNAP) {
		p_rule.smooth = SMOOTH_NONE;
		p_rule.flow = false;
		p_rule.extrapolate = false;
		p_rule.arc = false;
		return p_rule;
	}
	// Smooth even things not smoothed automatically, if the author chose it
	if (p_rule.smooth == SMOOTH_NONE && can_flow(p_type)) {
		p_rule.smooth = SMOOTH_VALUE;
		p_rule.hz = 20;
		p_rule.flow = true;
	}
	p_rule.extrapolate = choice != GDScriptOnline::S_SMOOTH;
	p_rule.arc = choice == GDScriptOnline::S_ARC;
	return p_rule;
}

// Takes floor, velocity and up direction after the physics update from a Character syncing position.
// Horizontal only needs to approach the arrived value, but vertical is driven by gravity, so the same arc is advanced locally.
// Only these three are needed; neither the jump nor the landing spot needs tracking
static bool character_state(Node *p_node, const StringName &p_name, bool &r_floor, Variant &r_motion) {
	StringName property;
	Node *shown = shown_target(p_node, p_name, property);
	if (property != SNAME("position") && property != SNAME("global_position") &&
			property != SNAME("transform") && property != SNAME("global_transform")) {
		return false;
	}
	CharacterBody2D *body2 = Object::cast_to<CharacterBody2D>(shown);
	CharacterBody3D *body3 = Object::cast_to<CharacterBody3D>(shown);
	if (body2 == nullptr && body3 == nullptr) {
		return false;
	}
	// Velocity belongs to the body itself. Looked up by name so 2D and 3D are not split.
	// The up direction exists locally too, so it is not read here
	r_floor = body2 != nullptr ? body2->is_on_floor() : body3->is_on_floor();
	r_motion = shown->get(SNAME("velocity"));
	return true;
}

// Vertical amount along up. The single measure for treating horizontal and vertical separately
static double along(const Variant &p_v, const Variant &p_up) {
	if (p_v.get_type() == Variant::VECTOR2 && p_up.get_type() == Variant::VECTOR2) {
		return Vector2(p_v).dot(Vector2(p_up));
	}
	if (p_v.get_type() == Variant::VECTOR3 && p_up.get_type() == Variant::VECTOR3) {
		return Vector3(p_v).dot(Vector3(p_up));
	}
	return 0.0;
}

// Returns the value with horizontal kept and only vertical replaced
static Variant with_along(const Variant &p_v, const Variant &p_up, double p_amount) {
	if (p_v.get_type() == Variant::VECTOR2 && p_up.get_type() == Variant::VECTOR2) {
		const Vector2 up = p_up;
		const Vector2 v = p_v;
		return v - up * v.dot(up) + up * real_t(p_amount);
	}
	if (p_v.get_type() == Variant::VECTOR3 && p_up.get_type() == Variant::VECTOR3) {
		const Vector3 up = p_up;
		const Vector3 v = p_v;
		return v - up * v.dot(up) + up * real_t(p_amount);
	}
	return p_v;
}

// Converts the change count over 10 seconds into a send rate
static int measured_rate(int p_changes, int p_max) {
	const int hz = p_changes >= 120 ? 20 : p_changes >= 60 ? 10
			: p_changes >= 20							   ? 5
			: p_changes >= 5							   ? 2
														   : 0;
	return MIN(hz, p_max);
}

// Rounds up to a tick interval not exceeding the max Hz
static int rate_ticks(int p_hz) {
	return p_hz > 0 ? MAX(1, (MVRep::TICK_HZ + p_hz - 1) / p_hz) : 1;
}

// Builds a reproducible spreading key from Node and property
static uint32_t phase_key(int64_t p_uid, const StringName &p_name) {
	const uint64_t id = uint64_t(p_uid);
	return (uint32_t(id) ^ uint32_t(id >> 32)) * 2654435761U + p_name.hash();
}

// Spreads sends of the same period over stable phases per Node and property
static uint32_t phase_due(uint32_t p_now, int p_ticks, int64_t p_uid, const StringName &p_name) {
	const uint32_t ticks = uint32_t(MAX(1, p_ticks));
	const uint32_t base = p_now + ticks;
	const uint32_t phase = phase_key(p_uid, p_name) % ticks;
	return base + (phase + ticks - base % ticks) % ticks;
}

// Picks the latest reliable baseline slot within 20 ticks of the next regular send
static uint32_t key_slot(uint32_t p_now, uint32_t p_first, int p_interval) {
	const uint32_t interval = uint32_t(MAX(1, p_interval));
	const uint32_t deadline = p_now + MVRep::KEY_TICKS;
	if (p_first >= deadline) {
		return p_first;
	}
	return p_first + ((deadline - p_first) / interval) * interval;
}

// Computes interpolable values directly per type
static Variant blend_value(const Variant &p_from, const Variant &p_to, double p_weight, SmoothMode p_mode) {
	const real_t w = real_t(CLAMP(p_weight, 0.0, 1.0));
	if (p_mode == SMOOTH_ANGLE && p_from.get_type() == Variant::FLOAT) {
		return Math::lerp_angle(double(p_from), double(p_to), double(w));
	}
	if (p_mode == SMOOTH_DEGREES && p_from.get_type() == Variant::FLOAT) {
		return Math::rad_to_deg(Math::lerp_angle(Math::deg_to_rad(double(p_from)), Math::deg_to_rad(double(p_to)), double(w)));
	}
	switch (p_from.get_type()) {
		case Variant::FLOAT:
			return Math::lerp(double(p_from), double(p_to), double(w));
		case Variant::VECTOR2:
			return Vector2(p_from).lerp(Vector2(p_to), w);
		case Variant::VECTOR3: {
			const Vector3 a = p_from;
			const Vector3 b = p_to;
			if (p_mode == SMOOTH_DEGREES) {
				return Vector3(Math::rad_to_deg(Math::lerp_angle(Math::deg_to_rad(a.x), Math::deg_to_rad(b.x), w)),
						Math::rad_to_deg(Math::lerp_angle(Math::deg_to_rad(a.y), Math::deg_to_rad(b.y), w)),
						Math::rad_to_deg(Math::lerp_angle(Math::deg_to_rad(a.z), Math::deg_to_rad(b.z), w)));
			}
			if (p_mode == SMOOTH_ANGLE) {
				return Vector3(Math::lerp_angle(a.x, b.x, w), Math::lerp_angle(a.y, b.y, w), Math::lerp_angle(a.z, b.z, w));
			}
			return a.lerp(b, w);
		}
		case Variant::VECTOR4:
			return Vector4(p_from).lerp(Vector4(p_to), w);
		case Variant::COLOR:
			return Color(p_from).lerp(Color(p_to), w);
		case Variant::RECT2: {
			const Rect2 a = p_from;
			const Rect2 b = p_to;
			return Rect2(a.position.lerp(b.position, w), a.size.lerp(b.size, w));
		}
		case Variant::QUATERNION:
			return Quaternion(p_from).slerp(Quaternion(p_to), w);
		case Variant::BASIS:
			return Basis(p_from).slerp(Basis(p_to), w);
		case Variant::TRANSFORM2D:
			return Transform2D(p_from).interpolate_with(Transform2D(p_to), w);
		case Variant::TRANSFORM3D:
			return Transform3D(p_from).interpolate_with(Transform3D(p_to), w);
		default:
			return p_weight >= 1.0 ? p_to : p_from;
	}
}

// Decides whether this is a large position jump or a non-interpolable scale change.
// How far counts as a teleport depends on the world size, so the author writes it in the Secret
static bool discontinuous(const Variant &p_old, const Variant &p_next, SmoothMode p_mode, const StringName &p_property, double p_teleport) {
	if (p_mode == SMOOTH_POSITION) {
		if (p_old.get_type() == Variant::VECTOR2) {
			return Vector2(p_old).distance_to(Vector2(p_next)) > p_teleport;
		}
		if (p_old.get_type() == Variant::VECTOR3) {
			return Vector3(p_old).distance_to(Vector3(p_next)) > p_teleport;
		}
	}
	if (p_old.get_type() == Variant::TRANSFORM2D) {
		return Transform2D(p_old).get_origin().distance_to(Transform2D(p_next).get_origin()) > p_teleport;
	}
	if (p_old.get_type() == Variant::TRANSFORM3D) {
		return Transform3D(p_old).origin.distance_to(Transform3D(p_next).origin) > p_teleport;
	}
	if (p_property == SNAME("scale") || p_property == SNAME("global_scale")) {
		if (p_old.get_type() == Variant::VECTOR2) {
			const Vector2 a = p_old;
			const Vector2 b = p_next;
			return a.x * b.x <= 0.0 || a.y * b.y <= 0.0;
		}
		if (p_old.get_type() == Variant::VECTOR3) {
			const Vector3 a = p_old;
			const Vector3 b = p_next;
			return a.x * b.x <= 0.0 || a.y * b.y <= 0.0 || a.z * b.z <= 0.0;
		}
	}
	return false;
}

// Computes per-second velocity from two position samples
static Variant sample_velocity(const Variant &p_from, const Variant &p_to, double p_seconds) {
	if (p_from.get_type() == Variant::VECTOR2) {
		return (Vector2(p_to) - Vector2(p_from)) / real_t(p_seconds);
	}
	if (p_from.get_type() == Variant::VECTOR3) {
		return (Vector3(p_to) - Vector3(p_from)) / real_t(p_seconds);
	}
	return Variant();
}

// Computes a Character's acceleration from the velocity difference and clamps abnormal samples
static Variant sample_acceleration(const Variant &p_old, const Variant &p_next, double p_seconds) {
	if (p_old.get_type() == Variant::VECTOR2 && p_next.get_type() == Variant::VECTOR2) {
		Vector2 value = (Vector2(p_next) - Vector2(p_old)) / real_t(p_seconds);
		return value.limit_length(5000.0);
	}
	if (p_old.get_type() == Variant::VECTOR3 && p_next.get_type() == Variant::VECTOR3) {
		Vector3 value = (Vector3(p_next) - Vector3(p_old)) / real_t(p_seconds);
		return value.limit_length(100.0);
	}
	return Variant();
}

// Estimates position during packet loss for up to 150ms
static Variant extrapolated(const Variant &p_to, const Variant &p_velocity, const Variant &p_acceleration,
		bool p_character, double p_seconds) {
	if (p_to.get_type() == Variant::VECTOR2 && p_velocity.get_type() == Variant::VECTOR2) {
		Vector2 value = Vector2(p_to) + Vector2(p_velocity) * real_t(p_seconds);
		if (p_character && p_acceleration.get_type() == Variant::VECTOR2) {
			value += Vector2(p_acceleration) * real_t(0.5 * p_seconds * p_seconds);
		}
		return value;
	}
	if (p_to.get_type() == Variant::VECTOR3 && p_velocity.get_type() == Variant::VECTOR3) {
		Vector3 value = Vector3(p_to) + Vector3(p_velocity) * real_t(p_seconds);
		if (p_character && p_acceleration.get_type() == Variant::VECTOR3) {
			value += Vector3(p_acceleration) * real_t(0.5 * p_seconds * p_seconds);
		}
		return value;
	}
	return p_to;
}

// Seconds that may be predicted while nothing arrives. Same for every object.
// The longer the prediction, the larger the error when wrong. Jumps look right because the always-delivered position
// advances vertical motion from floor and velocity, so this need not be longer
static constexpr double EXTRA_SEC = 0.15;

// Destination boxes. Kept by number instead of building names each time
enum Box {
	BOX_ALL,
	BOX_FLOW,
	BOX_SETTLE,
	BOX_MOTION,
};
const char *MVRep::FLOOR_KEY = "__floor";
const char *MVRep::MOTION_KEY = "__motion";

static const String BOX_KEY[] = { "all", "flow", "settle", "motion" };
static const String BOX_MY_KEY[] = { "my", "flow_my", "settle_my", "motion_my" };

// Looks up an object by serial number. The Server itself if not remembered
Node *MVRep::at_uid(Node *p_root, int64_t p_uid) const {
	if (p_uid == MVGate::UID_SERVER) {
		return p_root->get_node_or_null(NodePath(MVGate::SERVER));
	}
	const HashMap<int64_t, ObjectID>::ConstIterator e = known.find(p_uid);
	Node *known_node = e ? Object::cast_to<Node>(ObjectDB::get_instance(e->value)) : nullptr;
	return known_node != nullptr ? known_node : find_uid(p_root, p_uid);
}

void MVRep::know(Node *p_node) {
	ERR_FAIL_NULL(p_node);
	known[p_node->get_meta(SNAME("uid"), 0)] = p_node->get_instance_id();
	freeze_client_body(p_node, smoothing);
}

// Writes one entry into the container for its destination
// Numbers go on the wire instead of field names. Both sides build the same order from the same mark table, so the table is not sent.
// Floor and velocity are companion values not in the marks, so they are fixed to negative numbers
PackedStringArray MVRep::field_names(Node *p_node) {
	if (p_node->has_meta(SNAME("fields"))) {
		return p_node->get_meta(SNAME("fields"));
	}
	PackedStringArray names;
	const Dictionary mark = p_node->get_meta(SNAME("mark"), Dictionary());
	for (const Variant &k : mark.get_key_list()) {
		if (!(int(mark[k]) & (GDScriptOnline::F_SIGNAL | GDScriptOnline::F_METHOD | GDScriptOnline::F_INPUT))) {
			names.push_back(k);
		}
	}
	names.sort();
	if (!mark.is_empty()) {
		p_node->set_meta(SNAME("fields"), names); // Do not cache an empty order from before marks are set
	}
	return names;
}

int MVRep::field_id(Node *p_node, const StringName &p_name) {
	if (p_name == StringName(FLOOR_KEY)) {
		return -1;
	}
	if (p_name == StringName(MOTION_KEY)) {
		return -2;
	}
	// Build the name -> number table once per Node. No linear search per field every tick
	HashMap<ObjectID, HashMap<StringName, int>>::Iterator table = field_ids.find(p_node->get_instance_id());
	if (!table) {
		HashMap<StringName, int> ids;
		const PackedStringArray names = field_names(p_node);
		for (int i = 0; i < names.size(); i++) {
			ids.insert(StringName(names[i]), i);
		}
		if (!p_node->has_meta(SNAME("fields"))) {
			const HashMap<StringName, int>::ConstIterator hit = ids.find(p_name);
			return hit ? hit->value : -3; // No marks yet. Do not cache the table
		}
		table = field_ids.insert(p_node->get_instance_id(), ids);
	}
	const HashMap<StringName, int>::ConstIterator hit = table->value.find(p_name);
	return hit ? hit->value : -3;
}

String MVRep::field_name(Node *p_node, int p_id) {
	if (p_id == -1) {
		return FLOOR_KEY;
	}
	if (p_id == -2) {
		return MOTION_KEY;
	}
	const PackedStringArray names = field_names(p_node);
	return p_id >= 0 && p_id < names.size() ? names[p_id] : String();
}

void MVRep::put(Dictionary &r_box, Node *p_node, int64_t p_uid, const StringName &p_name, const Variant &p_value) {
	const int id = field_id(p_node, p_name);
	if (id == -3) {
		return; // Fields not in the marks are not sent (they have no number)
	}
	if (!r_box.has(p_uid)) {
		r_box[p_uid] = Dictionary();
	}
	// The container is shared by reference, so writing to the fetched one updates it in place
	Dictionary d = r_box[p_uid];
	d[id] = p_value;
}

// Reports an author-fixable binding error with its Node and property, only the first time
void MVRep::warn_field(Node *p_node, const StringName &p_name, const String &p_reason) {
	const String path = p_node->is_inside_tree() ? String(p_node->get_path()) : String(p_node->get_name());
	const String kind = p_node->get_meta(SNAME("kind"), p_node->get_class());
	const String key = kind + ":" + String(p_name) + ":" + p_reason;
	if (warned.has(key)) {
		return;
	}
	warned.insert(key);
	ERR_PRINT(vformat(U"@online binding %s.%s: %s", path, p_name, p_reason));
}

// Leaves the Online side value unchanged and queues only the Client display suited to the Node use for interpolation
bool MVRep::smooth_field(Node *p_node, int64_t p_uid, const StringName &p_name, const Variant &p_value, int p_flags, uint32_t p_tick, bool p_flow, bool p_settle, bool p_motion, bool p_snap, const Dictionary &p_body) {
	if (!smoothing) {
		return false;
	}
	StringName property;
	Node *target = shown_target(p_node, p_name, property);
	HashMap<StringName, Track> &node_tracks = tracks[p_uid];
	Track *old = node_tracks.getptr(p_name);
	// The rule depends on object and name. From the second time on, the remembered one is used
	const SyncRule rule = old != nullptr ? old->rule : with_choice(rule_for(p_node, target, property, p_value.get_type()), p_flags, p_value.get_type());
	const uint8_t frames = old != nullptr ? old->frames : MAX(1, GDScriptOnline::frames_of(p_flags));
	if (target == nullptr || rule.smooth == SMOOTH_NONE) {
		return false;
	}
	bool valid = false;
	const Variant current = target->get(property, &valid);
	if (!valid || current.get_type() != p_value.get_type()) {
		return false;
	}
	const uint64_t arrived = Time::get_singleton()->get_ticks_msec();
	const bool fresh = old == nullptr;
	if (fresh) {
		Track made;
		made.rule = rule;
		made.frames = frames;
		old = &node_tracks.insert(p_name, made)->value;
	}
	// The first entry, initial placement, teleports and takeoff/landing set a baseline without smoothing
	if (fresh || p_snap || p_motion || discontinuous(old->previous, p_value, rule.smooth, property, double(teleport_px))) {
		old->node = target->get_instance_id();
		old->property = property;
		old->mode = rule.smooth;
		old->character = rule.arc;
		old->from = p_value;
		old->to = p_value;
		old->previous = p_value;
		old->elapsed = 0.0;
		old->duration = 0.0;
		old->arrived = arrived;
		old->sample_tick = p_tick;
		old->active = false;
		old->extrapolate = false;
		old->motion = false;
		old->velocity = p_body.has(MOTION_KEY) ? p_body[MOTION_KEY] : sample_velocity(p_value, p_value, 1.0);
		old->acceleration = sample_velocity(p_value, p_value, 1.0);
		old->on_floor = bool(p_body.get(FLOOR_KEY, true));
		old->up = p_body.has(MOTION_KEY) ? target->get(SNAME("up_direction")) : Variant();
		old->gravity = Variant();
		write_shown(p_node, p_name, p_value);
		target->reset_physics_interpolation();
		return true;
	}
	double sample_seconds = CLAMP(old->arrived > 0 ? double(arrived - old->arrived) / 1000.0 : 0.1, 0.05, 0.2);
	if (p_tick > old->sample_tick && old->sample_tick > 0) {
		sample_seconds = double(p_tick - old->sample_tick) / double(TICK_HZ);
	}
	sample_seconds = MAX(sample_seconds, 0.001);
	const double duration = CLAMP(sample_seconds, 0.05, 0.2);
	// No estimate when velocity arrived. The jump arc is correct from the first frame
	const bool has_body = p_body.has(MOTION_KEY);
	const bool floor = bool(p_body.get(FLOOR_KEY, true));
	Variant velocity = has_body ? Variant(p_body[MOTION_KEY]) : sample_velocity(old->previous, p_value, sample_seconds);
	// More past frames smooth the speed, resisting jitter at the cost of slower response
	if (!has_body && old->frames > 1) {
		velocity = blend_value(old->velocity, velocity, 1.0 / double(old->frames), SMOOTH_VALUE);
	}
	if (has_body) {
		// Fall acceleration is a game constant and cannot be read. The difference between airborne samples is the answer itself
		if (!floor && !old->on_floor) {
			const Variant measured = sample_acceleration(old->velocity, velocity, sample_seconds);
			old->gravity = old->gravity.get_type() == measured.get_type()
					? blend_value(old->gravity, measured, 0.5, SMOOTH_VALUE)
					: measured;
		}
		old->on_floor = floor;
		if (old->up.get_type() == Variant::NIL) {
			old->up = target->get(SNAME("up_direction")); // A scene property. It does not change, so read only once
		}
	}
	const Variant acceleration = old->character && !old->motion ? sample_acceleration(old->velocity, velocity, sample_seconds) : Variant();
	old->node = target->get_instance_id();
	old->property = property;
	old->from = current;
	old->to = p_value;
	old->previous = p_value;
	old->elapsed = 0.0;
	const bool character_position = rule.arc && rule.smooth == SMOOTH_POSITION;
	old->duration = target->is_class("Range") ? 0.12 : p_settle ? 0.08
			: character_position								? CLAMP(sample_seconds * 1.5, 0.075, 0.2)
																: duration;
	old->arrived = arrived;
	old->sample_tick = p_tick;
	old->mode = rule.smooth;
	old->velocity = p_settle && !has_body ? sample_velocity(p_value, p_value, 1.0) : velocity;
	old->acceleration = p_settle ? old->velocity : acceleration;
	old->character = rule.arc;
	old->extrapolate = rule.extrapolate && !p_settle && !p_motion;
	old->motion = p_motion;
	// While airborne, vertical keeps advancing even if the arrived value is the same
	old->active = old->from != old->to || old->extrapolate || (has_body && !floor);
	if (old->active) {
		moving.insert(p_uid);
	}
	return true;
}

// Advances only display values moving in Client rendering
void MVRep::advance(double p_delta) {
	if (!smoothing || p_delta <= 0.0) {
		return;
	}
	MVTrustedWriteScope write_scope;
	Vector<int64_t> stopped;
	for (const int64_t uid : moving) {
		HashMap<int64_t, HashMap<StringName, Track>>::Iterator node = tracks.find(uid);
		if (!node) {
			stopped.push_back(uid);
			continue;
		}
		Vector<StringName> gone;
		bool active = false;
		for (KeyValue<StringName, Track> &entry : node->value) {
			Track &track = entry.value;
			Node *target = Object::cast_to<Node>(ObjectDB::get_instance(track.node));
			if (target == nullptr) {
				gone.push_back(entry.key);
				continue;
			}
			if (!track.active) {
				continue;
			}
			track.elapsed += p_delta;
			const double linear = track.duration > 0.0 ? track.elapsed / track.duration : 1.0;
			Variant shown;
			// While a Character is airborne, horizontal and vertical are handled separately.
			// Horizontal is another player's air control, so do not predict it; just approach the arrived value.
			// Vertical is driven by gravity, so the same arc is advanced locally even without arrivals
			if (track.character && !track.on_floor && track.up.get_type() != Variant::NIL) {
				const double t = track.elapsed;
				const double high = along(track.to, track.up) + along(track.velocity, track.up) * t +
						0.5 * along(track.gravity, track.up) * t * t;
				const Variant side = blend_value(track.from, track.to, MIN(linear, 1.0), SMOOTH_POSITION);
				shown = with_along(side, track.up, high);
				if (t - track.duration >= EXTRA_SEC) {
					track.active = false; // Nothing has arrived for too long. Stop reading
				}
			} else if (linear < 1.0) {
				const double weight = track.mode == SMOOTH_POSITION ? linear : linear * linear * (3.0 - 2.0 * linear);
				shown = blend_value(track.from, track.to, weight, SmoothMode(track.mode));
			} else if (track.extrapolate) {
				const double extra = MIN(EXTRA_SEC, track.elapsed - track.duration);
				shown = extrapolated(track.to, track.velocity, track.acceleration, track.character, extra);
				if (extra >= EXTRA_SEC) {
					track.active = false;
				}
			} else {
				shown = track.to;
				track.active = false;
			}
			bool valid = false;
			target->set(track.property, shown, &valid);
			if (!valid) {
				track.active = false;
			}
			active = active || track.active;
		}
		for (const StringName &name : gone) {
			node->value.erase(name);
		}
		if (!active || node->value.is_empty()) {
			stopped.push_back(uid);
		}
	}
	for (const int64_t uid : stopped) {
		moving.erase(uid);
	}
}

// Puts one property into the container for each destination, following its mark.
// Only continuous changes flow latest-first; stop values and periodic baselines are delivered reliably
void MVRep::send(Node *p_root, Node *p_node, int64_t p_uid, const String &p_own,
		const StringName &p_name, int p_flags, bool p_first, bool p_touched, Dictionary &r_out) {
	if (p_flags & (GDScriptOnline::F_SIGNAL | GDScriptOnline::F_METHOD | GDScriptOnline::F_INPUT)) {
		return; // Input flows from the owner to the Online side. Not delivered
	}
	Cell &c = last[p_uid][p_name];
	c.until = now + KEEP_TICKS;
	// Script body variables change only when written. Ticks without a written mark do not even read them
	if (c.gated && !p_touched) {
		return;
	}
	StringName property;
	Node *target = field_target(p_node, p_name, property);
	bool valid = false;
	const Variant value = target != nullptr ? target->get(property, &valid) : Variant();
	if (!valid) {
		warn_field(p_node, p_name, U"node or property not found");
		return;
	}
	if (!c.gated_known) {
		c.gated_known = true;
		// Reading may be skipped only for this object's script body variables of types whose contents cannot be changed in place.
		// Built-in Node positions are written by physics, and arrays and dictionaries change via append without the mark
		bool member = false;
		if (target == p_node && p_node->get_script_instance() != nullptr) {
			p_node->get_script_instance()->get_property_type(property, &member);
		}
		const Variant::Type t = value.get_type();
		c.gated = member && t != Variant::NIL && t != Variant::ARRAY && t != Variant::DICTIONARY && t != Variant::OBJECT && t < Variant::PACKED_BYTE_ARRAY;
	}
	// The value of `@online var hp := $Health.value` comes from the display target. If still empty,
	// it is the moment before the declaration initializer runs. Delivering it empty would leave the receiver unable to write the display target
	if (value.get_type() == Variant::NIL && Dictionary(p_node->get_meta(SNAME("bind"), Dictionary())).has(String(p_name))) {
		return;
	}
	// The rule depends on object and name. Decided and remembered on the first time instead of walking class names each time
	if (!c.rule_known) {
		StringName property;
		Node *shown = shown_target(p_node, p_name, property);
		c.rule = with_choice(rule_for(p_node, shown, property, value.get_type()), p_flags, value.get_type());
		c.rule_known = true;
	}
	const SyncRule &rule = c.rule;
	bool floor = false;
	Variant body_motion;
	// Only the always-delivered position carries companion values.
	// @online(position) keeps meaning deliver that one value
	const bool has_floor = (p_flags & GDScriptOnline::F_AUTO) && rule.arc &&
			character_state(p_node, p_name, floor, body_motion);
	const int limit = rule.hz;
	const int explicit_hz = GDScriptOnline::rate_of(p_flags);
	const bool motion_edge = has_floor && c.floor_known && c.on_floor != floor;
	// Re-remember only on normal collection. Full state sends must not consume floor transitions
	if (has_floor && !full) {
		c.floor_known = true;
		c.on_floor = floor;
	}
	int box = BOX_ALL;
	if (!full) {
		if (limit > 0) {
			if (c.auto_hz == 0 && c.window_at == 0) {
				c.auto_hz = limit;
				c.window_at = now;
			}
			if (c.seen.get_type() == Variant::NIL || c.seen != value) {
				const int wake_ticks = explicit_hz > 0 ? MAX(FLOW_GAP_TICKS,
																 rate_ticks(explicit_hz))
													   : FLOW_GAP_TICKS;
				const bool woke = c.observed_at == 0 || now - c.observed_at > uint32_t(wake_ticks);
				c.seen = value;
				c.window_changes = MIN((int)UINT16_MAX, (int)c.window_changes + 1);
				c.observed_streak = c.observed_at > 0 && now - c.observed_at <= 5 ? MIN((int)UINT8_MAX, (int)c.observed_streak + 1) : 1;
				c.observed_at = now;
				if (woke) {
					c.due = now;
				}
				if (c.observed_streak >= FLOW_START && c.auto_hz < limit) {
					c.auto_hz = limit;
					if (explicit_hz == 0) {
						c.due = now;
					}
				}
			}
			if (now - c.window_at >= RATE_WINDOW_TICKS) {
				// Use the measured change rate over 10 seconds as the rate for the next 10 seconds
				c.auto_hz = measured_rate(c.window_changes, limit);
				c.window_at = now;
				c.window_changes = 0;
			}
		}
		int hz = limit > 0 ? c.auto_hz : explicit_hz;
		if (explicit_hz > 0) {
			hz = hz > 0 ? MIN(hz, explicit_hz) : explicit_hz;
		}
		if (!motion_edge && now < c.due) {
			return;
		}
		// An explicit rate is a cap and does not exceed the rate estimated from the Node use
		const int interval = rate_ticks(hz);
		c.due = phase_due(now, interval, p_uid, p_name);
		if (motion_edge) {
			c.value = value;
			c.changed_at = now;
			c.flowing = false;
			box = BOX_MOTION;
		} else if (c.value == value) {
			c.changes = 0;
			if (!c.flowing) {
				return;
			}
			c.flowing = false;
			box = BOX_SETTLE;
		} else {
			const bool recent = c.changed_at > 0 && now - c.changed_at <= FLOW_GAP_TICKS;
			c.changes = recent ? MIN((int)UINT8_MAX, (int)c.changes + 1) : 1;
			c.changed_at = now;
			c.value = value;
			if (rule.flow && !p_first && can_flow(value.get_type()) && (c.flowing || c.changes >= FLOW_START)) {
				if (!c.flowing) {
					c.flowing = true;
					c.key_due = key_slot(now, c.due, interval);
					box = BOX_FLOW;
				} else if (now >= c.key_due) {
					box = BOX_ALL;
					c.key_due = key_slot(now, c.due, interval);
				} else {
					box = BOX_FLOW;
				}
			}
		}
	}
	// Only values decided to be delivered need their contents checked
	if (!MVFrame::plain(value)) {
		warn_field(p_node, p_name, U"Object and Resource values cannot be synced");
		return;
	}
	if (p_flags & GDScriptOnline::F_MY) {
		// Without an owner there is nobody to deliver to. Using a Player scene for ownerless objects,
		// like a bot, is normal, so nothing is reported.
		// With _save, saving continues below, so only delivery is skipped
		if (!p_own.is_empty()) {
			// Only for the owner and the server. Kept separately per destination
			Dictionary my = r_out[BOX_MY_KEY[box]];
			if (!my.has(p_own)) {
				my[p_own] = Dictionary();
			}
			Dictionary his = my[p_own];
			put(his, p_node, p_uid, p_name, value);
		}
	} else {
		Dictionary all = r_out[BOX_KEY[box]];
		put(all, p_node, p_uid, p_name, value);
		// Carries the two values needed to advance vertical motion locally in the same package as position.
		// The up direction is a scene property also present locally. Not carried
		if (has_floor) {
			put(all, p_node, p_uid, StringName(FLOOR_KEY), floor);
			put(all, p_node, p_uid, StringName(MOTION_KEY), body_motion);
		}
	}
	// Ownerless objects are kept by path. Names of objects not named by spawn are per-launch numbers,
	// so keeping them by path would restore to a different object on next launch. Not kept; reported once
	if ((p_flags & GDScriptOnline::F_SAVE) && p_own.is_empty() && p_uid > MVGate::fixed_max() && !p_node->has_meta(SNAME("named"))) {
		const String kind = p_node->get_meta(SNAME("kind"), String());
		if (!warned_unnamed.has(kind)) {
			warned_unnamed.insert(kind);
			WARN_PRINT(vformat(U"Online: spawn(\"%s\") has no name, so its @online_save values are not kept. Pass {\"name\": \"...\"} to keep them", kind));
		}
		return;
	}
	if (p_flags & GDScriptOnline::F_SAVE) {
		Dictionary save = r_out["save"];
		const String path = String(p_root->get_path_to(p_node));
		save_paths[p_uid] = path;
		Dictionary entry = save.get(path, Dictionary());
		entry["kind"] = p_node->get_meta(SNAME("kind"), String());
		entry["own"] = p_own;
		Dictionary values = entry.get("values", Dictionary());
		values[p_name] = value;
		entry["values"] = values;
		save[path] = entry;
	}
}

// Does not deliver Secret contents; collects only @online_save kept values into the save box.
// No spawn notices or display values are emitted, so the Client never knows this tree
void MVRep::save_only(const String &p_path, Node *p_node, Dictionary &r_out) {
	// Full collection goes to newly entering players. Secrets are not passed, so nothing is needed
	if (full) {
		return;
	}
	const Dictionary mark = p_node->get_meta(SNAME("mark"), Dictionary());
	Dictionary &seen = kept[p_path];
	Dictionary values;
	for (const Variant &k : mark.get_key_list()) {
		const int flags = mark[k];
		if (!(flags & GDScriptOnline::F_SAVE) || (flags & (GDScriptOnline::F_SIGNAL | GDScriptOnline::F_METHOD))) {
			continue;
		}
		const StringName name = k;
		Variant value;
		if (!read_field(p_node, name, value)) {
			warn_field(p_node, name, U"node or property not found");
			continue;
		}
		if (!MVFrame::plain(value)) {
			warn_field(p_node, name, U"Object and Resource values cannot be saved");
			continue;
		}
		// Keep the Secret light. Unchanged values are not written
		if (seen.has(name) && seen[name] == value) {
			continue;
		}
		seen[name] = value.duplicate(true);
		values[name] = value;
	}
	if (!values.is_empty()) {
		Dictionary save = r_out["save"];
		Dictionary entry = save.get(p_path, Dictionary());
		entry["kind"] = p_node->get_meta(SNAME("kind"), String());
		entry["own"] = String();
		entry["values"] = values;
		save[p_path] = entry;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		save_only(p_path.path_join(p_node->get_child(i)->get_name()), p_node->get_child(i), r_out);
	}
}

// Collects saved values of Secrets moved outside the world, keyed by their path in the world
void MVRep::save_secret(const String &p_path, Node *p_node, Dictionary &r_out) {
	if (!r_out.has("save")) {
		r_out["save"] = Dictionary();
	}
	save_only(p_path, p_node, r_out);
}

// Walks one tree and collects newly appeared objects and changed values
void MVRep::walk(Node *p_root, Node *p_node, Dictionary &r_out, HashSet<int64_t> &r_seen) {
	// Objects marked for deletion are not delivered. The next collection reports them as gone
	if (p_node->is_queued_for_deletion()) {
		return;
	}
	// Secrets are outside spawn sync and value sync, but login and persistence need
	// @online_save values, so only those are collected, kept without exposing them to the Client
	if (Object::cast_to<Secret>(p_node) != nullptr) {
		save_only(String(p_root->get_path_to(p_node)), p_node, r_out);
		return;
	}
	// Server-only objects are not delivered. Turning back here also hides descendants
	const int64_t uid = p_node->get_meta(SNAME("uid"), int64_t(0));
	if (uid != 0) {
		r_seen.insert(uid);
		const Dictionary mark = p_node->get_meta(SNAME("mark"), Dictionary());
		const bool first = uid > MVGate::fixed_max() && (full || !announced.has(uid));
		// Objects with no marks need only the spawn notice. Owners do not read them either
		if (mark.is_empty() && !first) {
			for (int i = 0; i < p_node->get_child_count(); i++) {
				walk(p_root, p_node->get_child(i), r_out, r_seen);
			}
			return;
		}
		const String own = p_node->get_meta(SNAME("own"), String());
		// Server and Players exist on both sides from the start. No spawn notice is emitted.
		// Objects included in full collection (for one mid-join player) are remembered too. del is emitted even if they vanish before the next collection
		if (first) {
			known[uid] = p_node->get_instance_id();
			if (!full) {
				announced.insert(uid);
			}
			// Bundle identity and first display values into the spawn notice so it is placed correctly from the first frame
			Dictionary a;
			a["uid"] = uid;
			a["name"] = String(p_node->get_name());
			a["kind"] = p_node->get_meta(SNAME("kind"));
			a["scene"] = MVClientScript::author_scene_of(a["kind"]); // The local side builds the display copy from the same scene
			a["own"] = own;
			a["up"] = int64_t(p_node->get_parent()->get_meta(SNAME("uid"), MVGate::UID_SERVER));
			Dictionary props = p_node->get_meta(SNAME("mv_spawn"), Dictionary()).duplicate();
			// Bound root properties are auto-bundled into the spawn notice from Online side variables to avoid interpolating from the origin.
			// Spawn notices go to everyone, so _my values are not included. Including them would
			// leak a hidden variable to others through its bound display property
			const Dictionary bindings = p_node->get_meta(SNAME("bind"), Dictionary());
			HashSet<StringName> hidden;
			for (const Variant &key : bindings.get_key_list()) {
				const String target = bindings[key];
				const StringName property = target;
				Variant value;
				if (target.find_char('.') < 0 && spawn_property(property) &&
						read_field(p_node, StringName(key), value) && MVFrame::plain(value)) {
					write_shown(p_node, StringName(key), value);
					if (int(mark.get(key, 0)) & GDScriptOnline::F_MY) {
						hidden.insert(property);
					} else {
						props[property] = value;
					}
				}
			}
			// A name passed to spawn() is also dropped from the spawn notice if it matches a _my binding target.
			// Re-reading here would find the _my value already in the Node,
			// slipping past the exclusion above and reaching others
			for (const Variant &key : props.get_key_list()) {
				const StringName property = key;
				if (hidden.has(property)) {
					props.erase(key);
					continue;
				}
				props[key] = p_node->get(property);
			}
			if (!props.is_empty()) {
				a["props"] = props;
			}
			static_cast<Array>(r_out["add"]).push_back(a);
		}
		// Newborn objects and full collection read everything. Otherwise variables are read only for objects written to marked variables
		const bool touched = full || first || dirty.has(p_node->get_instance_id());
		for (const Variant *k = mark.next(nullptr); k != nullptr; k = mark.next(k)) {
			send(p_root, p_node, uid, own, StringName(*k), int(mark[*k]), first, touched, r_out);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		walk(p_root, p_node->get_child(i), r_out, r_seen);
	}
}

// Drops memory past its lifetime. Sweep that keeps it from growing
void MVRep::sweep() {
	Vector<int64_t> empty;
	for (KeyValue<int64_t, HashMap<StringName, Cell>> &kv : last) {
		Vector<StringName> old;
		for (const KeyValue<StringName, Cell> &c : kv.value) {
			if (now > c.value.until) {
				old.push_back(c.key);
			}
		}
		for (const StringName &name : old) {
			kv.value.erase(name);
		}
		if (kv.value.is_empty()) {
			empty.push_back(kv.key);
		}
	}
	for (const int64_t uid : empty) {
		last.erase(uid);
	}
	swept = now;
}

// Collects what to deliver. Only changes since the last pass are included
Dictionary MVRep::collect(Node *p_root) {
	Dictionary out;
	out["add"] = Array();
	out["del"] = Array();
	for (int box = 0; box < 4; box++) {
		out[BOX_KEY[box]] = Dictionary();
		out[BOX_MY_KEY[box]] = Dictionary();
	}
	out["save"] = Dictionary();
	out["save_del"] = Array();
	ERR_FAIL_NULL_V(p_root, out);

	if (!full) {
		now++;
		GDScriptOnline::take_dirty(dirty); // Objects written since the last collection. Writes during collection go to the next pass
	}
	Node *server = p_root->get_node_or_null(NodePath(MVGate::SERVER));
	ERR_FAIL_NULL_V(server, out);

	HashSet<int64_t> seen;
	walk(p_root, server, out, seen);
	if (full) {
		return out;
	}
	// Anything not found in the walk is gone. Its memory is dropped too
	Array del;
	Vector<int64_t> gone;
	for (const KeyValue<int64_t, ObjectID> &kv : known) {
		if (!seen.has(kv.key)) {
			gone.push_back(kv.key);
		}
	}
	for (const int64_t uid : gone) {
		known.erase(uid);
		announced.erase(uid);
		last.erase(uid);
		if (save_paths.has(uid)) {
			static_cast<Array>(out["save_del"]).push_back(save_paths[uid]);
			save_paths.erase(uid);
		}
		del.push_back(uid);
	}
	out["del"] = del;
	if (now - swept >= SWEEP_TICKS) {
		sweep();
	}
	return out;
}

// Collects the current contents in full. Mid-join players get everything instead of diffs.
// Remembered values stay unchanged, so delivery to everyone is not disturbed
Dictionary MVRep::snapshot(Node *p_root) {
	full = true;
	Dictionary out = collect(p_root);
	full = false;
	return out;
}

// Applies what arrived. Only deliveries from the Secret may write state.
// Returns the list of denial reasons
Array MVRep::apply(Node *p_root, const Dictionary &p_frame) {
	Array bad;
	ERR_FAIL_NULL_V(p_root, bad);
	MVTrustedWriteScope write_scope;
	// Newborn Nodes enter the tree after their values are set,
	// so, as on the Online side, delivered values are already in place at _ready
	struct Born {
		int64_t uid = 0; // Serial number matched against memory
		ObjectID node; // Instance still outside the tree
		ObjectID up; // Parent to add it to
	};
	Vector<Born> fresh;

	{
		const Array add = p_frame.get("add", Array());
		for (int i = 0; i < add.size(); i++) {
			const Dictionary a = add[i];
			const int64_t uid = a.get("uid", 0);
			const HashMap<int64_t, ObjectID>::Iterator old = known.find(uid);
			if (uid <= MVGate::fixed_max() || (old && ObjectDB::get_instance(old->value) != nullptr)) {
				continue;
			}
			if (old) {
				known.erase(uid);
			}
			const String kind = a.get("kind", String());
			const String name = a.get("name", String());
			Node *up = at_uid(p_root, a.get("up", MVGate::UID_SERVER));
			if (up == nullptr || !MVGate::safe_name(name)) {
				bad.push_back(vformat(U"cannot place: %d %s", uid, kind));
				continue;
			}
			if (!MVClientScript::has_view(kind) && MVClientScript::ensure_scene(a.get("scene", String())) != kind) {
				bad.push_back(String(U"cannot register: ") + kind);
				continue;
			}
			Node *n = MVClientScript::make(kind, name, a.get("own", String()), uid);
			if (n == nullptr) {
				bad.push_back(String(U"cannot instantiate: ") + kind);
				continue;
			}
			const Dictionary props = a.get("props", Dictionary());
			for (const Variant &key : props.get_key_list()) {
				const StringName field = key;
				if (!spawn_property(field)) {
					continue;
				}
				bool valid = false;
				n->set(field, props[key], &valid);
				if (!valid) {
					bad.push_back(String(U"cannot set spawn value: ") + String(field));
				}
			}
			known[uid] = n->get_instance_id();
			fresh.push_back(Born{ uid, n->get_instance_id(), up->get_instance_id() });
		}
		const Array del = p_frame.get("del", Array());
		for (int i = 0; i < del.size(); i++) {
			// Fixed Nodes present on both sides from the start may not be deleted
			if (int64_t(del[i]) <= MVGate::fixed_max()) {
				continue;
			}
			Node *n = at_uid(p_root, del[i]);
			known.erase(del[i]);
			tracks.erase(del[i]);
			moving.erase(del[i]);
			if (n != nullptr && n->get_parent() != nullptr) {
				MVGate::quiet(n);
				n->get_parent()->remove_child(n);
				n->queue_free();
			}
		}
	}

	const uint32_t sample_tick = MAX(int64_t(0), int64_t(p_frame.get("frame", 0)));
	for (int slot = 0; slot < 8; slot++) {
		// There are 8 box kinds. The smoothing is decided from the number instead of checking name prefixes each time
		const int box = slot / 2;
		const bool flow = box == BOX_FLOW;
		const bool settle = box == BOX_SETTLE;
		const bool motion = box == BOX_MOTION;
		const Dictionary vals = p_frame.get(slot % 2 == 0 ? BOX_KEY[box] : BOX_MY_KEY[box], Dictionary());
		for (const Variant &k : vals.get_key_list()) {
			const int64_t uid = k;
			Node *n = at_uid(p_root, uid);
			// Arrivals for just-removed objects are normal. Nothing to report
			if (n == nullptr || !n->has_meta(SNAME("mark"))) {
				continue;
			}
			const Dictionary mark = n->get_meta(SNAME("mark"), Dictionary());
			const Dictionary bindings = n->get_meta(SNAME("bind"), Dictionary());
			// Map wire numbers back to field names. Both sides' mark tables are identical, so the order matches
			Dictionary d;
			const Dictionary packed = vals[k];
			for (const KeyValue<Variant, Variant> &one : packed) {
				const String name = one.key.get_type() == Variant::INT ? field_name(n, int(one.key)) : String(one.key);
				if (name.is_empty()) {
					bad.push_back(vformat(U"unknown field: %s.%s", String(n->get_name()), String(one.key)));
					continue;
				}
				d[name] = one.value;
			}
			// Floor, velocity and up carried with position are not author properties. Remove and hold them first
			Dictionary body;
			for (const String &side : { String(FLOOR_KEY), String(MOTION_KEY) }) {
				if (d.has(side)) {
					body[side] = d[side];
					d.erase(side);
				}
			}
			const bool appeared = hidden_display(n);
			// Set the Range limit before value to avoid clamping by arrival order.
			// Nodes without a bound limit need one pass, so the display target is not looked up
			bool ranged = false;
			for (const Variant *key = d.next(nullptr); key != nullptr && !ranged; key = d.next(key)) {
				const String target = bindings.get(String(*key), String());
				ranged = target.ends_with("min_value") || target.ends_with("max_value");
			}
			for (int priority = ranged ? 0 : 1; priority < 2; priority++) {
				for (const Variant *key = d.next(nullptr); key != nullptr; key = d.next(key)) {
					const Variant &nk = *key;
					if (ranged) {
						StringName shown_property;
						shown_target(n, StringName(nk), shown_property);
						const bool first = shown_property == SNAME("min_value") || shown_property == SNAME("max_value");
						if (first != (priority == 0)) {
							continue;
						}
					}
					if (!mark.has(nk) || (int(mark[nk]) & GDScriptOnline::F_SIGNAL)) {
						bad.push_back(vformat(U"unmarked property: %s.%s", String(n->get_name()), String(nk)));
						continue;
					}
					const StringName name = nk;
					const Variant value = client_readonly(d[nk]);
					Variant now_value;
					read_field(n, name, now_value);
					const bool bound = bindings.has(String(name));
					if (bound && !write_field(n, name, value)) {
						bad.push_back(vformat(U"cannot write: %s.%s", String(n->get_name()), String(name)));
						continue;
					}
					const bool shown = smooth_field(n, uid, name, value, int(mark[nk]), sample_tick, flow, settle, motion, appeared, body);
					if (!shown && !(bound ? write_shown(n, name, value) : write_field(n, name, value))) {
						bad.push_back(vformat(U"cannot write: %s.%s", String(n->get_name()), String(name)));
						continue;
					}
					emit_online_changed(n, name, value, now_value);
				}
			}
		}
	}

	// Add to the tree once values are fully set. _ready runs only here
	for (const Born &made : fresh) {
		Node *n = Object::cast_to<Node>(ObjectDB::get_instance(made.node));
		if (n == nullptr) {
			continue;
		}
		Node *up = Object::cast_to<Node>(ObjectDB::get_instance(made.up));
		const HashMap<int64_t, ObjectID>::ConstIterator e = known.find(made.uid);
		// Objects removed in the same delivery, or whose parent was removed, are discarded without entering the tree
		if (up == nullptr || !e || e->value != made.node) {
			memdelete(n);
			continue;
		}
		up->add_child(n);
		freeze_client_body(n, smoothing);
		// Not announced here. @online func endpoints, values and _ready are not in place yet.
		// The transport layer emits spawned from newborn() once everything is in place
		born.push_back(n->get_instance_id());
	}

	// Delivers cues. They are not values, so the receiver emits them on arrival
	const Array fire = p_frame.get("fire", Array());
	for (int i = 0; i < fire.size(); i++) {
		const Dictionary f = fire[i];
		Node *n = at_uid(p_root, f.get("uid", 0));
		if (n == nullptr) {
			continue;
		}
		const StringName name = f.get("name", String());
		const Dictionary mark = n->get_meta(SNAME("mark"), Dictionary());
		if (!mark.has(name) || !(int(mark[name]) & GDScriptOnline::F_SIGNAL)) {
			bad.push_back(String(U"unmarked signal: ") + String(name));
			continue;
		}
		const Array args = f.get("args", Array());
		LocalVector<Variant> keep;
		LocalVector<const Variant *> ptrs;
		for (int a = 0; a < args.size(); a++) {
			keep.push_back(args[a]);
		}
		for (uint32_t a = 0; a < keep.size(); a++) {
			ptrs.push_back(&keep[a]);
		}
		n->emit_signalp(name, ptrs.ptr(), ptrs.size());
	}
	return bad;
}

// Restores saved values only to Nodes whose restart-stable path, type and owner match
Dictionary MVRep::restore(Node *p_root, const Dictionary &p_values) {
	Dictionary pending;
	ERR_FAIL_NULL_V(p_root, pending);
	for (const Variant &key : p_values.get_key_list()) {
		if (key.get_type() != Variant::STRING && key.get_type() != Variant::STRING_NAME) {
			continue;
		}
		const String path = key;
		if (!stable_path(path)) {
			continue;
		}
		const Dictionary values = p_values[key];
		Node *node = p_root->get_node_or_null(NodePath(path));
		if (node == nullptr) {
			pending[path] = values;
			continue;
		}
		if (MVGate::real_path(p_root, path) != path) {
			continue;
		}
		// Owned objects are kept in accounts. World box values are restored only to ownerless objects of the same type
		const String kind = node->get_meta(SNAME("kind"), String());
		const String own = node->get_meta(SNAME("own"), String());
		if (!own.is_empty() || (values.has("@kind") && String(values["@kind"]) != kind)) {
			continue;
		}
		const Dictionary left = restore_node(node, values);
		if (!left.is_empty()) {
			pending[path] = left;
		}
	}
	return pending;
}

// Writes back only values with save marks to one Node and returns what could not be placed
Dictionary MVRep::restore_node(Node *p_node, const Dictionary &p_values) {
	Dictionary left;
	ERR_FAIL_NULL_V(p_node, left);
	const Dictionary marks = p_node->get_meta(SNAME("mark"), Dictionary());
	for (const Variant &field : p_values.get_key_list()) {
		const int flags = marks.get(field, 0);
		// "@kind" is a type mark, not a value
		if (String(field) == "@kind" || ((flags & GDScriptOnline::F_SAVE) && !(flags & GDScriptOnline::F_SIGNAL) &&
				write_field(p_node, field, p_values[field]))) {
			continue;
		}
		left[field] = p_values[field];
	}
	return left;
}

// Extracts a leaving Player's last @online_save values before it leaves the tree
Dictionary MVRep::save_of(Node *p_node) {
	Dictionary out;
	ERR_FAIL_NULL_V(p_node, out);
	const Dictionary marks = p_node->get_meta(SNAME("mark"), Dictionary());
	for (const Variant &field : marks.get_key_list()) {
		const int flags = marks[field];
		if (!(flags & GDScriptOnline::F_SAVE) || (flags & GDScriptOnline::F_SIGNAL)) {
			continue;
		}
		Variant value;
		if (read_field(p_node, StringName(field), value) && MVFrame::plain(value)) {
			out[field] = value.duplicate(true);
		}
	}
	return out;
}

// Drops remembered values. The next collection redelivers everything
void MVRep::reset() {
	last.clear();
	known.clear();
	save_paths.clear();
	kept.clear();
	born.clear();
	tracks.clear();
	moving.clear();
	warned.clear();
	now = 0;
	swept = 0;
}

// Enables automatic display interpolation only in the Client's public world
void MVRep::set_smoothing(bool p_enabled) {
	smoothing = p_enabled;
	if (smoothing) {
		for (const KeyValue<int64_t, ObjectID> &e : known) {
			freeze_client_body(Object::cast_to<Node>(ObjectDB::get_instance(e.value)), true);
		}
	} else {
		tracks.clear();
		moving.clear();
	}
}

// Nothing is exposed to scripts. @online marks are the only entry to sync
void MVRep::_bind_methods() {
}
