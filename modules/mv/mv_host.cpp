/**************************************************************************/
/*  mv_host.cpp                                                           */
/**************************************************************************/

// Host side that runs the world room, relaying identity, saves and Secret calls through the Secret.

#include "mv_host.h"

#include "mv_link.h"
#include "mv_local_id.h"
#include "mv_online.h"
#include "mv_secret.h"
#include "mv_thing.h"

#include "core/io/net_socket.h"
#include "core/object/callable_mp.h"

MVRuntime *MVHost::runtime_node() const {
	return Object::cast_to<MVRuntime>(ObjectDB::get_instance(runtime));
}

MVLink *MVHost::state_link() const {
	return Object::cast_to<MVLink>(ObjectDB::get_instance(state));
}

// Connect to the Secret and prepare the room certificate and listener
String MVHost::start(const String &p_host, int p_port, const String &p_secret_name, const Ref<X509Certificate> &p_ca, const String &p_grant) {
	room_id = MVLocalID::random_hex(8);
	grant = p_grant;
	// The state link uses the port next to identity. If taken, it is shifted when opening
	port = p_port + 1;
	parser.instantiate();
	line.instantiate();
	line->connect(SNAME("ready"), callable_mp(this, &MVHost::ready_line));
	line->connect(SNAME("received"), callable_mp(this, &MVHost::took));
	line->connect(SNAME("failed"), callable_mp(this, &MVHost::lost));
	line->connect(SNAME("parted"), callable_mp(this, &MVHost::lost).bind(String(U"Lost the connection to Secret")));
	const String reason = line->open(p_host, p_port, p_secret_name, p_ca);
	if (!reason.is_empty()) {
		return reason;
	}
	set_process(true);
	return String();
}

// TLS to the Secret is up, so announce the room.
// The world cannot start before the entry list arrives, so send only the name first
void MVHost::ready_line() {
	Dictionary body;
	body["room"] = room_id;
	body["grant"] = grant;
	send(MVFrame::HOST_HELLO, body);
}

// Turn bytes from the Secret back into frames
void MVHost::took(const PackedByteArray &p_data) {
	const String text = String::utf8(reinterpret_cast<const char *>(p_data.ptr()), p_data.size());
	for (const Variant &value : parser->feed(text)) {
		handle(value);
	}
}

void MVHost::handle(const Dictionary &p_frame) {
	const Dictionary body = p_frame.get("body", Dictionary());
	switch (int(p_frame.get("type", -1))) {
		case MVFrame::HOST_READY:
			begin_world(body);
			break;
		case MVFrame::HOST_ALLOW:
			allow_in(body);
			break;
		case MVFrame::HOST_PART:
			part_out(body);
			break;
		case MVFrame::HOST_DONE:
			if (MVRuntime *rt = runtime_node()) {
				rt->secret_answered(int64_t(body.get("id", 0)), body.get("value", Variant()));
			}
			break;
		case MVFrame::HOST_STORE:
			// When storage drops, hold off; when it returns, the world re-saves everything. Same behavior as the local store
			saving = bool(body.get("on", false));
			break;
		case MVFrame::DENY:
			lost(body.get("reason", U"Secret refused the room"));
			break;
		default:
			break;
	}
}

// Receive the entry list and world saved values, and start the Online-side world and state link
void MVHost::begin_world(const Dictionary &p_body) {
	if (running) {
		return;
	}
	running = true;
	saving = bool(p_body.get("saving", false));
	// The Secret makes and hands over a disposable per-room key and certificate. This device makes no keys.
	// They reach Clients in the identity reply, so neither authors nor players write per-room settings
	key = Ref<CryptoKey>(CryptoKey::create());
	cert = Ref<X509Certificate>(X509Certificate::create());
	cert_name = p_body.get("name", String());
	if (key.is_null() || cert.is_null() || key->load_from_string(p_body.get("key", String())) != OK || cert->load_from_string(p_body.get("cert", String())) != OK || cert_name.is_empty()) {
		lost(U"Cannot read the room certificate");
		return;
	}
	// On a device acting as host, the physics tick follows the world clock at 20Hz. Restored on close
	MVThing::world_clock(true);
	set_physics_process(true);
	MVRuntime *rt = memnew(MVRuntime);
	rt->set_name("Runtime");
	// Saves and Secret scripts are held by the Secret. The world goes through this interface only
	rt->set_saves(this);
	rt->set_secret_line(this, p_body.get("doors", Dictionary()));
	add_child(rt);
	// Link before running so the gate can be removed on close
	runtime = rt->get_instance_id();
	const String reason = rt->start(true);
	if (!reason.is_empty()) {
		lost(reason);
		return;
	}
	const int room_max = Secret::room_max_of(rt->server_node());
	MVLink *link = memnew(MVLink);
	link->set_name("State");
	add_child(link);
	// Shift until a free port is found. Two rooms on one device do not collide
	String opened;
	for (int i = 0; i < PORT_TRIES; i++) {
		// Opening a taken port directly makes the engine print a red error.
		// Two rooms on one device is a normal way to test, so check availability first.
		// Probe with a plain socket that disallows port sharing, to match the transport's conditions
		Ref<NetSocket> probe = NetSocket::create();
		IP::Type kind = IP::TYPE_ANY;
		if (probe.is_valid() && probe->open(NetSocket::Family::INET, NetSocket::TYPE_UDP, kind) == OK) {
			const Error taken = probe->bind(NetSocket::Address(IPAddress("0.0.0.0"), port + i));
			probe->close();
			if (taken != OK) {
				opened = vformat(U"Cannot listen on port %d", port + i);
				continue;
			}
		}
		opened = link->open(port + i, key, cert, "0.0.0.0", room_max);
		if (opened.is_empty()) {
			port += i;
			break;
		}
	}
	if (!opened.is_empty()) {
		lost(opened);
		return;
	}
	state = link->get_instance_id();
	link->connect(SNAME("__joined"), callable_mp(this, &MVHost::state_joined));
	link->connect(SNAME("__asked"), callable_mp(this, &MVHost::state_asked));
	// Only now is the destination fixed. Send the opened port and certificate to open the room
	Dictionary body;
	body["room"] = room_id;
	body["port"] = port;
	body["max"] = room_max;
	send(MVFrame::HOST_HELLO, body);
	print_line(vformat(U"Godot Online Room: %s (state port %d)", room_id, port));
	// Nobody can enter until the room opens. Our own login also waits until here
	emit_signal(SNAME("opened"));
}

// Tell the Secret this person could not be admitted. again asks it to look up another room
void MVHost::refuse_in(const String &p_who, const String &p_reason, bool p_again) {
	Dictionary answer;
	answer["who"] = p_who;
	answer["ok"] = false;
	answer["reason"] = p_reason;
	if (p_again) {
		answer["again"] = true;
	}
	send(MVFrame::HOST_JOINED, answer);
}

// Admit a person whose identity the Secret approved, and report whether it worked
void MVHost::allow_in(const Dictionary &p_body) {
	MVRuntime *rt = runtime_node();
	MVLink *link = state_link();
	const String who = p_body.get("who", String());
	const Dictionary context = p_body.get("context", Dictionary());
	if (rt == nullptr || link == nullptr || who.is_empty()) {
		refuse_in(who, U"The room is not running yet", false);
		return;
	}
	// A full room is not about author scripts. Another room may have space, so ask again
	if (rt->room_full_for(who)) {
		refuse_in(who, U"The room is full", true);
		return;
	}
	// Rejoin from another device. Cut the old device's state link first. Otherwise,
	// until the new device connects, the old one could send commands as that person
	if (bool(p_body.get("retake", false))) {
		link->revoke(who);
	}
	// This person's saved values are read by identity and arrive with the reply
	if (!rt->join_context(who, context, true, p_body.get("save", Dictionary()))) {
		refuse_in(who, U"Login was not accepted", false);
		return;
	}
	link->allow(who, p_body.get("token", String()));
	Dictionary answer;
	answer["who"] = who;
	answer["ok"] = true;
	send(MVFrame::HOST_JOINED, answer);
}

void MVHost::part_out(const Dictionary &p_body) {
	MVRuntime *rt = runtime_node();
	const String who = p_body.get("who", String());
	if (rt == nullptr || who.is_empty()) {
		return;
	}
	if (MVLink *link = state_link()) {
		link->revoke(who);
	}
	// Keep the Player while waiting for reconnect. The Secret counts the wait time
	if (bool(p_body.get("gone", false))) {
		rt->join_context(who, Dictionary(), false);
	} else {
		rt->disconnect_player(who);
	}
}

// Send the whole current world to a person whose state link connected
void MVHost::state_joined(const String &p_who) {
	MVLink *link = state_link();
	MVRuntime *rt = runtime_node();
	if (link != nullptr && rt != nullptr) {
		link->push(p_who, rt->snapshot(p_who));
	}
}

// Pass a command from the state link to the world and return the answer to the requester
void MVHost::state_asked(const String &p_who, int64_t p_id, int64_t p_uid, const String &p_fn, const Array &p_args) {
	MVRuntime *rt = runtime_node();
	MVLink *link = state_link();
	if (rt == nullptr || link == nullptr) {
		return;
	}
	rt->ask_player(p_who, p_uid, p_fn, p_args, link->answer_to(p_who, p_id));
}

// The room cannot continue once the route to the Secret drops. Pending answers are closed empty
void MVHost::lost(const String &p_reason) {
	if (told_gone) {
		return;
	}
	told_gone = true;
	// Say nothing when closing after play. Print the reason only when cut off midway
	if (!p_reason.is_empty()) {
		ERR_PRINT(vformat(U"Online: closing the room. %s", p_reason));
	}
	set_process(false);
	set_physics_process(false);
	MVThing::world_clock(false);
	if (MVRuntime *rt = runtime_node()) {
		rt->secret_lost();
		rt->close_world();
	}
	if (MVLink *link = state_link()) {
		link->close();
	}
	if (line.is_valid()) {
		line->close();
	}
	// Only our own Client is waiting. Report the close so it does not stall silently
	if (!p_reason.is_empty()) {
		emit_signal(SNAME("closed"));
	}
}

bool MVHost::send(int p_type, const Dictionary &p_body) {
	return line.is_valid() && line->send(MVFrame::pack(p_type, p_body).to_utf8_buffer());
}

void MVHost::keep(const String &p_op, const Dictionary &p_body) {
	Dictionary body = p_body;
	body["op"] = p_op;
	send(MVFrame::HOST_SAVE, body);
}

// Store a user's saved values with the Secret
void MVHost::save_user(const String &p_id, const String &p_kind, const Dictionary &p_values) {
	Dictionary body;
	body["id"] = p_id;
	body["kind"] = p_kind;
	body["values"] = p_values;
	keep("user", body);
}


// Ask the Secret to run a Secret script function
bool MVHost::call_secret(int64_t p_id, const String &p_path, const String &p_name, const Array &p_args) {
	Dictionary body;
	body["id"] = p_id;
	body["path"] = p_path;
	body["fn"] = p_name;
	body["args"] = p_args;
	return send(MVFrame::HOST_CALL, body);
}

// Advance the Secret link every render frame and send the Online-side diff at 20Hz
void MVHost::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		if (line.is_valid()) {
			line->poll();
		}
	} else if (p_what == NOTIFICATION_PHYSICS_PROCESS) {
		if (running && !told_gone) {
			MVThing::drive_world(runtime, state);
		}
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		lost(String());
	}
}

// Only notify internally that the room opened or closed. Nothing is exposed to GDScript
void MVHost::_bind_methods() {
	ADD_SIGNAL(MethodInfo("opened"));
	ADD_SIGNAL(MethodInfo("closed"));
}
