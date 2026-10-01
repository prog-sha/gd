/**************************************************************************/
/*  mv_link.cpp                                                           */
/**************************************************************************/

// Carries @online traffic split into reliable events and latest-wins continuous values.

#include "mv_link.h"

#include "mv_frame.h"

#include "mv_gate.h"
#include "mv_secret.h"
#include "mv_stage.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/time.h"
#include "scene/main/multiplayer_api.h"
#include "scene/main/scene_tree.h"

#include "modules/enet/enet_multiplayer_peer.h"

// Target payload size for one UDP packet: MTU (1500) minus IP, UDP, DTLS, ENet
// and RPC overhead. Splits by actual size, not by Node count
static constexpr int FLOW_BYTES = 1100;
static constexpr uint64_t TOKEN_MS = 30000; // Expiry of the DTLS binding issued over TLS. Long enough for slow round trips via a host

// Fixes the sender, reliability and channel of each RPC
MVLink::MVLink() {
	mp = MultiplayerAPI::create_default_interface();
	// State traffic runs only between Secret and each Client, so join notices between Clients are not relayed
	if (mp->has_method(SNAME("set_server_relay_enabled"))) {
		mp->call(SNAME("set_server_relay_enabled"), false);
	}
	Dictionary any;
	any["rpc_mode"] = MultiplayerAPI::RPC_MODE_ANY_PEER;
	any["call_local"] = false;
	any["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_RELIABLE;
	any["channel"] = 0;
	rpc_config("__bind", any);
	// Commands come only from the owner. Carried reliably and in order
	rpc_config("__ask", any);
	Dictionary sure;
	sure["rpc_mode"] = MultiplayerAPI::RPC_MODE_AUTHORITY;
	sure["call_local"] = false;
	sure["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_RELIABLE;
	sure["channel"] = 0;
	rpc_config("__rep", sure);
	rpc_config("__ask_done", sure);
	rpc_config("__settle", sure);
	Dictionary flow = sure.duplicate();
	flow["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_UNRELIABLE;
	flow["channel"] = 1;
	rpc_config("__flow", flow);
}

// Drops silent peers after DEAD_MS. ENet's default (up to 30s) notices a dead host or vanished player too late
static void watch(const Ref<ENetPacketPeer> &p_peer) {
	if (p_peer.is_valid()) {
		p_peer->set_timeout(4, MVFrame::DEAD_MS, MVFrame::DEAD_MS);
	}
}

// Creates an ENet server and starts receiving connect/disconnect notices
String MVLink::open(int p_port, const Ref<CryptoKey> &p_key, const Ref<X509Certificate> &p_cert, const String &p_bind, int p_room_max) {
	Ref<MultiplayerPeer> current = mp->get_multiplayer_peer();
	if (current.is_valid() && Object::cast_to<ENetMultiplayerPeer>(current.ptr())) {
		return U"Already connected";
	}
	Ref<ENetMultiplayerPeer> peer;
	peer.instantiate();
	peer->set_bind_ip(IPAddress(p_bind));
	// During a handover old and new connections coexist, so accept twice room_max.
	// A fixed value here would silently lock out authors who set a large room_max
	const int room = CLAMP(p_room_max, 1, (int)Secret::ROOM_LIMIT);
	if (peer->create_server_dtls(p_port, room * 2, TLSOptions::server(p_key, p_cert)) != OK) {
		return vformat(U"Cannot listen on port %d", p_port);
	}
	peer->get_host()->compress(ENetConnection::COMPRESS_RANGE_CODER); // The same value shapes repeat every tick, so a light coder compresses well
	mp->set_multiplayer_peer(peer);
	mp->connect("peer_connected", callable_mp(this, &MVLink::greet));
	mp->connect("peer_disconnected", callable_mp(this, &MVLink::forget));
	host = true;
	set_process(true);
	return String();
}

// Creates an ENet client and connects to the given server
String MVLink::join(const String &p_addr, int p_port, const String &p_name, const Ref<X509Certificate> &p_ca, const String &p_token) {
	Ref<MultiplayerPeer> current = mp->get_multiplayer_peer();
	if (current.is_valid() && Object::cast_to<ENetMultiplayerPeer>(current.ptr())) {
		return U"Already connected";
	}
	Ref<ENetMultiplayerPeer> peer;
	peer.instantiate();
	if (peer->create_client_dtls(p_addr, p_port, p_name, TLSOptions::client(p_ca)) != OK) {
		return vformat(U"Cannot connect to %s:%d", p_addr, p_port);
	}
	token = p_token;
	peer->get_host()->compress(ENetConnection::COMPRESS_RANGE_CODER); // Same coder as the Online side
	mp->connect("connected_to_server", callable_mp(this, &MVLink::connected));
	mp->connect("connection_failed", callable_mp(this, &MVLink::failed));
	mp->connect("server_disconnected", callable_mp(this, &MVLink::failed));
	mp->set_multiplayer_peer(peer);
	watch(peer->get_peer(1)); // Gives up on unreachable peers after the same time
	return String();
}

// Detaches connection notices and the peer, and discards in-flight records
void MVLink::close() {
	const Callable on = callable_mp(this, &MVLink::greet);
	if (mp->is_connected("peer_connected", on)) {
		mp->disconnect("peer_connected", on);
		mp->disconnect("peer_disconnected", callable_mp(this, &MVLink::forget));
	}
	const Callable ready = callable_mp(this, &MVLink::connected);
	if (mp->is_connected("connected_to_server", ready)) {
		mp->disconnect("connected_to_server", ready);
		mp->disconnect("connection_failed", callable_mp(this, &MVLink::failed));
		mp->disconnect("server_disconnected", callable_mp(this, &MVLink::failed));
	}
	mp->set_multiplayer_peer(Ref<MultiplayerPeer>());
	epochs.clear();
	allowed.clear();
	waiting.clear();
	peer_who.clear();
	who_peer.clear();
	rates.clear();
	token.clear();
	state_seen.clear();
	state_nodes.clear();
	state_epoch = 0;
	host = false;
	set_process(false);
}

// Converts a peer ID to the public Player name
String MVLink::who_of(int p_id) const {
	return peer_who.get(p_id, String());
}

// Returns the current peer ID for a public Player name
PackedStringArray MVLink::players() const {
	PackedStringArray out;
	for (const Variant &who : who_peer.keys()) {
		out.push_back(who);
	}
	return out;
}

// Extracts only the given continuous values from a normal frame
Dictionary MVLink::state_part(Dictionary &p_frame, const String &p_name) {
	Dictionary out;
	const String my = p_name + "_my";
	Dictionary all = p_frame.get(p_name, Dictionary());
	Dictionary own = p_frame.get(my, Dictionary());
	if (!all.is_empty()) {
		out[p_name] = all;
	}
	if (!own.is_empty()) {
		out[my] = own;
	}
	p_frame.erase(p_name);
	p_frame.erase(my);
	return out;
}

// Checks whether there are values to send besides metadata such as tick
bool MVLink::has_data(const Dictionary &p_frame) {
	for (const Variant &key : p_frame.keys()) {
		if (key == "frame" || key == "who") {
			continue;
		}
		const Variant value = p_frame[key];
		if (value.get_type() == Variant::DICTIONARY && !Dictionary(value).is_empty()) {
			return true;
		}
		if (value.get_type() == Variant::ARRAY && !Array(value).is_empty()) {
			return true;
		}
		if (value.get_type() == Variant::PACKED_STRING_ARRAY && !PackedStringArray(value).is_empty()) {
			return true;
		}
		if (value.get_type() != Variant::NIL && bool(value)) {
			return true;
		}
	}
	return false;
}

// Sends state split into reliable values, continuous values, and the ends of continuous values
bool MVLink::push(const String &p_who, const Dictionary &p_frame) {
	if (!host || !who_peer.has(p_who)) {
		return false;
	}
	Dictionary sure = p_frame.duplicate();
	Dictionary flow = state_part(sure, "flow");
	Dictionary settle = state_part(sure, "settle");
	const int id = who_peer[p_who];
	Ref<MultiplayerPeer> transport = mp->get_multiplayer_peer();
	ENetMultiplayerPeer *enet = Object::cast_to<ENetMultiplayerPeer>(transport.ptr());
	const Ref<ENetPacketPeer> peer = enet != nullptr ? enet->get_peer(id) : Ref<ENetPacketPeer>();
	// Does not send trailing diffs of the disconnect frame to a peer that has started closing
	if (peer.is_null() || peer->get_state() != ENetPacketPeer::STATE_CONNECTED || peer->get_channels() < 2) {
		return false;
	}
	int64_t epoch = epochs.get(p_who, 0);
	if (bool(p_frame.get("reset", false))) {
		epoch++;
		epochs[p_who] = epoch;
		sure["_epoch"] = epoch;
	}
	if (has_data(sure)) {
		rpc_id(id, "__rep", sure);
	}
	const int64_t tick = p_frame.get("frame", 0);
	if (has_data(flow)) {
		// Splits into MTU-sized parts so one lost packet does not lose every Player's continuous values
		for (const Variant &box : flow.keys()) {
			const Dictionary nodes = flow[box];
			Dictionary group;
			int bytes = 0;
			for (const Variant &uid : nodes.keys()) {
				const int one = MVFrame::size_of(nodes[uid]);
				// A single item that exceeds the limit is sent alone; it cannot be split
				if (!group.is_empty() && bytes + one > FLOW_BYTES) {
					Dictionary part;
					part[box] = group;
					rpc_id(id, "__flow", epoch, tick, part);
					group = Dictionary();
					bytes = 0;
				}
				group[uid] = nodes[uid];
				bytes += one;
			}
			if (!group.is_empty()) {
				Dictionary part;
				part[box] = group;
				rpc_id(id, "__flow", epoch, tick, part);
			}
		}
	}
	if (has_data(settle)) {
		rpc_id(id, "__settle", epoch, tick, settle);
	}
	return true;
}

// After DTLS is established, sends the one-time token received over TLS to Secret
void MVLink::connected() {
	if (!host && !token.is_empty()) {
		rpc_id(1, "__bind", token);
		token.clear();
	}
}

// Tells the Client that establishing or keeping DTLS failed
void MVLink::failed() {
	if (!host) {
		emit_signal("__failed", U"State connection lost");
	}
}

// Turns a TLS-authenticated one-time token into a DTLS peer-to-Player mapping
void MVLink::_bind(const String &p_token) {
	const int id = mp->get_remote_sender_id();
	bool safe = p_token.length() == 64;
	for (int i = 0; safe && i < p_token.length(); i++) {
		const char32_t c = p_token[i];
		safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	}
	if (!host || !waiting.has(id) || peer_who.has(id) || !safe) {
		mp->get_multiplayer_peer()->disconnect_peer(id, true);
		return;
	}
	waiting.erase(id);
	const String key = p_token.sha256_text();
	if (!allowed.has(key)) {
		mp->get_multiplayer_peer()->disconnect_peer(id, true);
		return;
	}
	const Array grant = allowed[key];
	allowed.erase(key);
	if (Time::get_singleton()->get_ticks_msec() > uint64_t(grant[1])) {
		mp->get_multiplayer_peer()->disconnect_peer(id, true);
		return;
	}
	const String who = grant[0];
	if (who_peer.has(who)) {
		const int old = who_peer[who];
		peer_who.erase(old);
		mp->get_multiplayer_peer()->disconnect_peer(old, true);
	}
	peer_who[id] = who;
	who_peer[who] = id;
	emit_signal("__joined", who);
}

// Passes commands only from authenticated peers to the world. The sender is decided by peer ID.
// The claimed ID is ignored, so nobody can impersonate another player
void MVLink::_ask(int64_t p_id, int64_t p_uid, const String &p_fn, const Array &p_args) {
	const int from = mp->get_remote_sender_id();
	if (!host || !peer_who.has(from)) {
		return;
	}
	const String who = peer_who[from];
	// Limits commands per second so one player cannot monopolize the world
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	Array seen = rates.get(who, Array());
	while (!seen.is_empty() && now - uint64_t(seen[0]) >= RATE_MS) {
		seen.remove_at(0);
	}
	if (seen.size() >= RATE_MAX || MVFrame::size_of(p_args) > ARGS_MAX) {
		// Always returns an answer; otherwise the caller's await never returns
		rpc_id(from, "__ask_done", p_id, Variant());
		return;
	}
	seen.push_back(now);
	rates[who] = seen;
	emit_signal("__asked", who, p_id, p_uid, p_fn, p_args);
}

// Returns the world's answer to the player who asked
void MVLink::ask_done(const String &p_who, int64_t p_id, const Variant &p_value) {
	if (!host || !who_peer.has(p_who)) {
		return;
	}
	rpc_id(int(who_peer[p_who]), "__ask_done", p_id, p_value);
}

// Callable receiving an answer the world produces later. Value first, destination bound after it
static void answer_value(const Variant &p_value, MVLink *p_link, const String &p_who, int64_t p_id) {
	p_link->ask_done(p_who, p_id, p_value);
}

Callable MVLink::answer_to(const String &p_who, int64_t p_id) {
	return callable_mp_static(&answer_value).bind(this, p_who, p_id);
}

// Passes an answer returned from the Online side upward
void MVLink::_ask_done(int64_t p_id, const Variant &p_value) {
	if (!host) {
		emit_signal("__answered", p_id, p_value);
	}
}

bool MVLink::ask(int64_t p_id, int64_t p_uid, const String &p_fn, const Array &p_args) {
	if (host) {
		return false;
	}
	rpc_id(1, "__ask", p_id, p_uid, p_fn, p_args);
	return true;
}

// Assigns a one-time token, which only Secret can create, to a Player
void MVLink::allow(const String &p_who, const String &p_token) {
	if (host && MVStage::is_who(p_who) && !p_token.is_empty()) {
		for (const Variant &key : allowed.keys()) {
			const Array old = allowed[key];
			if (String(old[0]) == p_who) {
				allowed.erase(key);
			}
		}
		Array grant;
		grant.push_back(p_who);
		grant.push_back(Time::get_singleton()->get_ticks_msec() + TOKEN_MS);
		allowed[p_token.sha256_text()] = grant;
	}
}

// Removes unused tokens and DTLS peers of a Player whose TLS route is gone
void MVLink::revoke(const String &p_who) {
	for (const Variant &key : allowed.keys()) {
		const Array grant = allowed[key];
		if (String(grant[0]) == p_who) {
			allowed.erase(key);
		}
	}
	if (who_peer.has(p_who)) {
		const int id = who_peer[p_who];
		peer_who.erase(id);
		who_peer.erase(p_who);
		mp->get_multiplayer_peer()->disconnect_peer(id, true);
	}
	epochs.erase(p_who);
}

// DTLS establishment alone does not make a Player; waits for bind
void MVLink::greet(int p_id) {
	waiting[p_id] = Time::get_singleton()->get_ticks_msec() + TOKEN_MS;
	Ref<ENetMultiplayerPeer> peer = mp->get_multiplayer_peer();
	if (peer.is_valid()) {
		watch(peer->get_peer(p_id));
	}
}
// Cleans up rate and epoch records of a disconnected Player
void MVLink::forget(int p_id) {
	const String who = who_of(p_id);
	waiting.erase(p_id);
	peer_who.erase(p_id);
	if (!who.is_empty()) {
		who_peer.erase(who);
		epochs.erase(who);
		rates.erase(who);
		emit_signal("__parted", who);
	}
}

// Disconnects peers that send no token so they do not hold DTLS slots
void MVLink::process() {
	if (!host) {
		return;
	}
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	for (const Variant &key : allowed.keys()) {
		const Array grant = allowed[key];
		if (now >= uint64_t(grant[1])) {
			allowed.erase(key);
		}
	}
	for (const Variant &id : waiting.keys()) {
		if (now >= uint64_t(waiting[id])) {
			mp->get_multiplayer_peer()->disconnect_peer(id, true);
			waiting.erase(id);
		}
	}
}

// Applies reliable shape changes first, and passes reliable values through per-property tick checks too
void MVLink::_rep(Dictionary p_frame) {
	const int64_t tick = p_frame.get("frame", 0);
	if (bool(p_frame.get("reset", false))) {
		// On late join or reconnect, discards the old per-Node receive cache and starts from a full snapshot
		state_epoch = p_frame.get("_epoch", state_epoch + 1);
		state_seen.clear();
		state_nodes.clear();
	}
	for (const Variant &entry : Array(p_frame.get("add", Array()))) {
		const Dictionary add = entry;
		const int64_t uid = add.get("uid", 0);
		if (uid > MVGate::UID_PLAYERS) {
			state_nodes.insert(uid);
		}
	}
	for (const Variant &uid : Array(p_frame.get("del", Array()))) {
		_forget_state(uid);
		state_nodes.erase(uid);
	}
	Dictionary state;
	for (const String &box : { String("all"), String("my"), String("motion"), String("motion_my") }) {
		const Dictionary values = p_frame.get(box, Dictionary());
		if (!values.is_empty()) {
			state[box] = values;
		}
		p_frame.erase(box);
	}
	p_frame.erase("_epoch");
	const Dictionary fresh = fresh_state(state_epoch, tick, state);
	for (const Variant &box : fresh.keys()) {
		p_frame[box] = fresh[box];
	}
	if (has_data(p_frame)) {
		emit_signal("__repped", p_frame);
	}
}

// Continuous values over UDP (__flow) and their reliably delivered ends (__settle) share the same freshness check
void MVLink::_take_state(int64_t p_epoch, int64_t p_tick, const Dictionary &p_frame) {
	const Dictionary fresh = fresh_state(p_epoch, p_tick, p_frame);
	if (!fresh.is_empty()) {
		emit_signal("__repped", fresh);
	}
}

// Compares world epoch and per-property ticks and extracts only newer values
Dictionary MVLink::fresh_state(int64_t p_epoch, int64_t p_tick, const Dictionary &p_frame) {
	Dictionary fresh;
	if (p_epoch != state_epoch) {
		return fresh;
	}
	for (const Variant &box : p_frame.keys()) {
		Dictionary values;
		Dictionary nodes = p_frame[box];
		for (const Variant &uid : nodes.keys()) {
			const int64_t id = uid;
			// Fixed Nodes present on both sides from the start get no creation notice.
			// Filtering to announced ones only would drop all their values
			if (id > MVGate::fixed_max() && !state_nodes.has(id)) {
				continue;
			}
			Dictionary fields;
			Dictionary props = nodes[uid];
			// The owner-only box is another face of the same Thing. A prefix keeps names from colliding
			const String head = String(box).ends_with("_my") ? "my:" : "";
			HashMap<StringName, int64_t> &seen = state_seen[id];
			for (const Variant &name : props.keys()) {
				const StringName key = head + String(name);
				const HashMap<StringName, int64_t>::Iterator known = seen.find(key);
				if (known && p_tick <= known->value) {
					continue;
				}
				seen[key] = p_tick;
				fields[name] = props[name];
			}
			if (!fields.is_empty()) {
				values[uid] = fields;
			}
		}
		if (!values.is_empty()) {
			fresh[box] = values;
		}
	}
	if (!fresh.is_empty()) {
		fresh["frame"] = p_tick;
	}
	return fresh;
}

// Drops all per-property ticks of a removed Node
void MVLink::_forget_state(int64_t p_uid) {
	state_seen.erase(p_uid);
	state_nodes.erase(p_uid);
}

// Creates a built-in RPC space independent of node paths using a dedicated MultiplayerAPI
void MVLink::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		get_tree()->set_multiplayer(mp, get_path());
	} else if (p_what == NOTIFICATION_PROCESS) {
		process();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		close();
		get_tree()->set_multiplayer(Ref<MultiplayerAPI>(), get_path());
	}
}

// Exposes the networking API and signals to GDScript
void MVLink::_bind_methods() {
	ClassDB::bind_method(D_METHOD("__bind", "token"), &MVLink::_bind);
	ClassDB::bind_method(D_METHOD("__ask", "id", "uid", "fn", "args"), &MVLink::_ask);
	ClassDB::bind_method(D_METHOD("__ask_done", "id", "value"), &MVLink::_ask_done);
	ClassDB::bind_method(D_METHOD("__rep", "frame"), &MVLink::_rep);
	ClassDB::bind_method(D_METHOD("__flow", "epoch", "tick", "frame"), &MVLink::_take_state);
	ClassDB::bind_method(D_METHOD("__settle", "epoch", "tick", "frame"), &MVLink::_take_state);
	ADD_SIGNAL(MethodInfo("__joined", PropertyInfo(Variant::STRING, "who")));
	ADD_SIGNAL(MethodInfo("__parted", PropertyInfo(Variant::STRING, "who")));
	ADD_SIGNAL(MethodInfo("__repped", PropertyInfo(Variant::DICTIONARY, "frame")));
	ADD_SIGNAL(MethodInfo("__failed", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("__asked", PropertyInfo(Variant::STRING, "who"), PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::INT, "uid"), PropertyInfo(Variant::STRING, "fn"), PropertyInfo(Variant::ARRAY, "args")));
	ADD_SIGNAL(MethodInfo("__answered", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::NIL, "value")));
}
