/**************************************************************************/
/*  mv_client.cpp                                                         */
/**************************************************************************/

// Client-side Node that authenticates with Secret, reconnects, and replicates the public world.

#include "mv_client.h"

#include "mv_client_script.h"
#include "mv_gate.h"
#include "mv_guest.h"
#include "mv_host.h"
#include "mv_link.h"
#include "mv_local_id.h"
#include "mv_online.h"
#include "mv_runtime.h"
#include "mv_secret.h"
#include "mv_thing.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/main/scene_tree.h"

#include "modules/gdscript/gdscript_online.h"

MVClient *MVClient::current = nullptr;

// Returns the live runtime from ObjectDB
MVRuntime *MVClient::runtime_node() const {
	return Object::cast_to<MVRuntime>(ObjectDB::get_instance(runtime));
}

// Returns the live local Player from ObjectDB
Node *MVClient::player_node() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(player));
}

// Returns the live DTLS state link from ObjectDB
MVLink *MVClient::state_link() const {
	return Object::cast_to<MVLink>(ObjectDB::get_instance(state));
}

// On a normal game launch, reads settings and starts a guest or account connection
void MVClient::ready() {
	set_process(true);
	const Dictionary config = settings();
	// With match-url set, no server is chosen here; matchmaking tells us a free room
	const String match_url = config.get("match-url", String());
	PackedStringArray need;
	need.push_back("secret-name");
	need.push_back("ca");
	if (match_url.is_empty()) {
		need.push_back("room-host");
		need.push_back("room-port");
	}
	for (const String &key : need) {
		if (String(config.get(key, String())).is_empty()) {
			fail_start(vformat(U"Set %s in the Secret inspector", String(key).replace("-", "_")));
			return;
		}
	}
	start_config = config;
	if (match_url.is_empty()) {
		begin_with(config);
		return;
	}
	// Asking for the destination in plain text lets it be rewritten in transit, making everyone host or nobody able to join. Plain text is allowed only for local tests
	const String match_host = match_url.get_slice("://", 1).get_slice("/", 0).get_slice(":", 0);
	if (!match_url.begins_with("https://") && match_host != "127.0.0.1" && match_host != "localhost") {
		fail_start(U"match_url must start with https:// unless it points at 127.0.0.1");
		return;
	}
	HTTPRequest *ask = memnew(HTTPRequest);
	ask->set_name("Match");
	add_child(ask);
	asker = ask->get_instance_id();
	ask->connect(SNAME("request_completed"), callable_mp(this, &MVClient::matched));
	ask_match();
}

// Asks matchmaking where to go. Used at startup and when the room to join is gone
void MVClient::ask_match() {
	HTTPRequest *ask = Object::cast_to<HTTPRequest>(ObjectDB::get_instance(asker));
	if (ask == nullptr) {
		return;
	}
	match_at = 0;
	asked_at = OS::get_singleton()->get_ticks_msec();
	if (ask->request(String(start_config["match-url"]), PackedStringArray(), HTTPClient::METHOD_POST, "{}") != OK) {
		// The previous query may simply not be finished yet. Asks again after a short delay
		match_at = asked_at + RETRY_MS;
	}
}

// The owned room closed. Matchmaking decides where to go, so ask again
void MVClient::room_closed() {
	ask_again(U"The room closed");
}

void MVClient::ask_again(const String &p_reason) {
	closed = true;
	retry_at = 0;
	leave_room();
	if (p_reason != told_lost) {
		told_lost = p_reason;
		WARN_PRINT(String(U"Online: ") + p_reason);
	}
	// Players whose room was shut down ask right away. Only repeated refusals with no destination are limited to once per RETRY_MS
	match_at = MAX(OS::get_singleton()->get_ticks_msec(), asked_at + RETRY_MS);
}

// Shuts down the room this device owned so it is not carried over to the next host selection
void MVClient::leave_room() {
	if (Node *old = Object::cast_to<Node>(ObjectDB::get_instance(room))) {
		remove_child(old);
		old->queue_free();
	}
	room = ObjectID();
}

// No destination. If never joined, reports that startup is impossible and ends;
// if joined before, it is only temporary, so asks again
void MVClient::no_room(const String &p_reason) {
	if (matched_once) {
		ask_again(p_reason);
	} else {
		fail_start(p_reason);
	}
}

// Picks the destination from the free-room reply and proceeds to login
void MVClient::matched(int p_result, int p_code, const PackedStringArray &, const PackedByteArray &p_body) {
	if (p_result != HTTPRequest::RESULT_SUCCESS || p_code != 200) {
		no_room(vformat(U"No room available (%d)", p_code));
		return;
	}
	const Variant parsed = JSON::parse_string(String::utf8((const char *)p_body.ptr(), p_body.size()));
	const Dictionary place = parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();
	const String where = place.get("host", String());
	const int room_port = int(place.get("port", 0));
	if (where.is_empty() || room_port < 1 || room_port > 65535) {
		no_room(U"Invalid reply from the matchmaking server");
		return;
	}
	matched_once = true;
	Dictionary config = start_config.duplicate();
	config["room-host"] = where;
	config["room-port"] = room_port;
	// With no free room, this device owns a room. Matchmaking decides who owns it,
	// and the device introduces itself to Secret with that grant
	if (bool(place.get("you_host", false))) {
		config["grant"] = place.get("grant", String());
		begin_host(config);
		return;
	}
	begin_with(config);
}

// Matchmaking chose this device, so it starts running the world here.
// Identity stays with Secret. The device also joins its own room as a normal Player
void MVClient::begin_host(const Dictionary &p_config) {
	const Ref<X509Certificate> secret_ca = read_ca(p_config);
	if (secret_ca.is_null()) {
		return;
	}
	leave_room();
	MVHost *made = memnew(MVHost);
	made->set_name("Room");
	add_child(made);
	room = made->get_instance_id();
	const String reason = made->start(p_config["room-host"], int(p_config["room-port"]), p_config["secret-name"], secret_ca, p_config.get("grant", String()));
	if (!reason.is_empty()) {
		fail_start(reason);
		return;
	}
	// Joins its own room as a normal Player. Without naming the room,
	// it would be sent to another room with space and its own room would stay empty.
	// Logging in before the room opens would be refused for lack of a room
	Dictionary config = p_config.duplicate();
	config["room"] = made->room();
	made->connect(SNAME("opened"), callable_mp(this, &MVClient::begin_with).bind(config), CONNECT_ONE_SHOT);
	// Does not stop if the room fails to start or closes midway; asks for a destination again
	made->connect(SNAME("closed"), callable_mp(this, &MVClient::room_closed), CONNECT_ONE_SHOT);
}

// Reads the CA certificate. Stops startup and returns empty if it cannot be read
Ref<X509Certificate> MVClient::read_ca(const Dictionary &p_config) {
	Ref<X509Certificate> out = Ref<X509Certificate>(X509Certificate::create());
	if (out.is_null() || out->load(String(p_config["ca"])) != OK) {
		fail_start(vformat(U"Cannot read the CA certificate: %s. Copy the CA certificate the server printed at startup", String(p_config["ca"])));
		return Ref<X509Certificate>();
	}
	return out;
}

// Logs in with settings whose destination is fixed
void MVClient::begin_with(const Dictionary &p_config) {
	hosts = String(p_config["room-host"]).split(",", false);
	port = int(p_config["room-port"]);
	secret_name = p_config["secret-name"];
	// The room name is given only when joining a room this device created.
	// Secret decides which room other players join
	want_room = p_config.get("room", String());
	ca = read_ca(p_config);
	if (ca.is_null()) {
		return;
	}
	const Dictionary input = account_input(p_config);
	const String reason = replace(input, input.is_empty());
	if (!reason.is_empty()) {
		fail_start(reason);
	}
}

// Advances reconnection, the command queue and TLS traffic by one frame
void MVClient::process() {
	if (match_at > 0 && OS::get_singleton()->get_ticks_msec() >= match_at) {
		ask_match();
	}
	if (closed) {
		return;
	}
	if (retry_at > 0 && OS::get_singleton()->get_ticks_msec() >= retry_at) {
		retry_at = 0;
		open_net();
	}
	const uint64_t run = generation;
	if (MVRuntime *rt = runtime_node()) {
		rt->flush_commands();
	}
	Ref<MVRep> live_rep = rep;
	if (live_rep.is_valid()) {
		live_rep->advance(get_process_delta_time());
	}
	if (closed || generation != run) {
		return;
	}
	Ref<MVSecretClient> live_net = net;
	if (live_net.is_valid()) {
		live_net->poll();
	}
	if (catcher.is_valid()) {
		catch_code();
	}
}

// Starts login with an external account. Secret builds the URL, so this only requests it
void MVClient::login_with(const String &p_provider) {
	if (!online) {
		ERR_PRINT(U"Online: login_with() needs the connection first. Wait for logged_in");
		return;
	}
	Dictionary body;
	body["provider"] = p_provider;
	send_frame(MVFrame::LOGIN_WITH, body);
}

// External login did not finish. Shows the reason and emits a signal so the author can display it
void MVClient::login_failed(const String &p_reason) {
	print_line(String(U"Online: ") + p_reason);
	MVRuntime *rt = runtime_node();
	Node *host = rt != nullptr ? rt->server_node() : nullptr;
	if (host != nullptr) {
		host->emit_signal(SNAME("login_failed"), p_reason);
	}
}

// Erases this device's key first, then has the Online side unlink too. The Online side stops us once unlinked.
// Erasing first ensures that a cut midway does not let the next launch enter the previous player's account
void MVClient::logout() {
	if (!online) {
		ERR_PRINT(U"Online: logout() needs the connection first");
		return;
	}
	MVLocalID::forget("user://online_guest_id");
	send_frame(MVFrame::LOGOUT, Dictionary());
}

// Reads the authorization code returned from the browser. Once read, sends it to Secret and closes the listener.
// Tells the browser only that it may close
void MVClient::catch_code() {
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (now >= catch_until) {
		visitor.unref();
		catcher.unref();
		login_failed(U"The login timed out. Call login_with() again");
		return;
	}
	if (visitor.is_null()) {
		if (!catcher->is_connection_available()) {
			return;
		}
		visitor = catcher->take_connection();
		visit_at = now;
		return;
	}
	visitor->poll();
	const int count = visitor->get_available_bytes();
	if (count <= 0) {
		if (now - visit_at >= VISIT_MS || visitor->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			visitor.unref();
		}
		return;
	}
	// The first line holds code and state. The browser sends the request in one go
	PackedByteArray got;
	got.resize(MIN(count, 8192));
	int read = 0;
	visitor->get_partial_data(got.ptrw(), got.size(), read);
	const String line = String::utf8((const char *)got.ptr(), read).get_slice("\r\n", 0);
	const String query = line.get_slice(" ", 1).get_slice("?", 1);
	String code;
	String state;
	for (const String &pair : query.split("&", false)) {
		const String key = pair.get_slice("=", 0);
		const String value = pair.get_slice("=", 1).uri_decode();
		if (key == "code") {
			code = value;
		} else if (key == "state") {
			state = value;
		}
	}
	// Whether it succeeded is unknown here. Tells the browser only that it may return
	const String page = "<!doctype html><meta charset=\"utf-8\"><title>Godot Online</title><p>You can close this tab and go back to the game.</p>";
	const CharString reply = ("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " + itos(page.utf8().length()) + "\r\n\r\n" + page).utf8();
	visitor->put_data((const uint8_t *)reply.get_data(), reply.length());
	visitor->disconnect_from_host();
	visitor.unref();
	// Drops non-authorization requests such as favicon, and requests with a different mark, then waits for the next.
	// Requests thrown in by other programs or web pages on the same device must not lock out the real return
	if (code.is_empty() || state != link_state) {
		return;
	}
	catcher.unref();
	Dictionary body;
	body["code"] = code;
	body["state"] = state;
	send_frame(MVFrame::LOGIN_CODE, body);
}

// External login finished. Same player with a different account, so emits logged_in again with the new my
void MVClient::linked(const Dictionary &p_my) {
	MVRuntime *rt = runtime_node();
	Node *host = rt != nullptr ? rt->server_node() : nullptr;
	if (host == nullptr) {
		return;
	}
	if (mine.is_null()) {
		mine.instantiate();
	}
	mine->set_context(who, p_my.duplicate(true));
	host->emit_signal(SNAME("logged_in"), mine);
}

// Layers destination settings from weakest to strongest:
// defaults < Secret inspector < `--name=value` command-line arguments.
// Secret is exported with the scene, so it connects to the same place without shipping a settings file
Dictionary MVClient::settings() const {
	Dictionary out;
	out["room-host"] = Secret::HOST_DEFAULT;
	out["room-port"] = (int)Secret::PORT_DEFAULT;
	out["secret-name"] = Secret::NAME_DEFAULT;
	out["ca"] = Secret::CA_DEFAULT;
	out["teleport-px"] = (int)Secret::TELEPORT_DEFAULT;
	const Dictionary wrote = Secret::config_of_project();
	for (const Variant &key : wrote.get_key_list()) {
		out[key] = wrote[key];
	}
	for (const String &arg : OS::get_singleton()->get_cmdline_user_args()) {
		if (arg.begins_with("--") && arg.contains("=")) {
			out[arg.trim_prefix("--").get_slice("=", 0)] = arg.get_slice("=", 1);
		}
	}
	return out;
}

// Moves only `login-` prefixed settings into the credential input for the login provider
Dictionary MVClient::account_input(const Dictionary &p_settings) const {
	Dictionary out;
	for (const Variant &key_value : p_settings.get_key_list()) {
		const String key = key_value;
		if (key.begins_with("login-")) {
			out[key.trim_prefix("login-")] = p_settings[key_value];
		}
	}
	return out;
}

// Drops the current connection and creates a new one with the given credentials
String MVClient::replace(const Dictionary &p_login, bool p_guest) {
	close();
	Dictionary input = p_login.duplicate(true);
	if (p_guest) {
		input = MVGuest::input();
		if (input.is_empty()) {
			return U"Cannot save the guest install ID";
		}
	}
	return open(input);
}

// Prepares the Client display scene and creates an empty public world and connection state
String MVClient::open(const Dictionary &p_login) {
	if (port < 1 || port > 65535) {
		return U"Invalid client settings";
	}
	login_input = p_login.duplicate(true);
	MVRuntime *rt = memnew(MVRuntime);
	rt->set_name("Runtime");
	add_child(rt);
	runtime = rt->get_instance_id();
	const String scene_error = rt->start(false);
	if (!scene_error.is_empty()) {
		return scene_error;
	}
	rep.instantiate();
	rep->set_smoothing(true);
	rep->set_teleport_px(int(start_config.get("teleport-px", (int)Secret::TELEPORT_DEFAULT)));
	rep->know(rt->server_node());
	parser.instantiate();
	net.instantiate();
	net->connect(SNAME("received"), callable_mp(this, &MVClient::take));
	net->connect(SNAME("failed"), callable_mp(this, &MVClient::lost));
	closed = false;
	set_process(true);
	open_net();
	return String();
}

// Stops reconnecting and releases TLS, commands and the public world
void MVClient::close(bool p_exiting) {
	generation++;
	closed = true;
	online = false;
	set_process(false);
	retry_at = 0;
	if (net.is_valid()) {
		net->disconnect(SNAME("received"), callable_mp(this, &MVClient::take));
		net->disconnect(SNAME("failed"), callable_mp(this, &MVClient::lost));
		net->close();
		net.unref();
	}
	MVLink *live_state = state_link();
	if (live_state != nullptr) {
		live_state->close();
		remove_child(live_state);
		live_state->queue_free();
	}
	state = ObjectID();
	MVRuntime *live_rt = runtime_node();
	if (live_rt != nullptr) {
		live_rt->clear_commands();
		MVGate::quiet(live_rt);
		// On SceneTree exit, leaves freeing to the parent Node and stops playing Audio synchronously
		if (!(p_exiting && live_rt->get_parent() == this)) {
			if (live_rt->get_parent() != nullptr) {
				live_rt->get_parent()->remove_child(live_rt);
			}
			live_rt->queue_free();
		}
	}
	runtime = ObjectID();
	player = ObjectID();
	rep.unref();
	parser.unref();
	who.clear();
	token.clear();
	login_pending = false;
	login_input.clear();
	login_sent = false;
	resuming = false;
	announced = false;
}

// Opens TLS to the current Relay candidate and resets the parser and login state
void MVClient::open_net() {
	if (closed || net.is_null()) {
		return;
	}
	const uint64_t run = generation;
	Ref<MVSecretClient> live_net = net;
	parser.instantiate();
	login_sent = false;
	const String host = hosts[host_i % hosts.size()];
	const String reason = live_net->open(host, port, secret_name, ca);
	if (!reason.is_empty() && !closed && generation == run && net.ptr() == live_net.ptr()) {
		lost(reason);
	}
}

// Announces login start and sends credentials to Secret once without exposing them
void MVClient::start_login() {
	if (closed || net.is_null()) {
		return;
	}
	login_sent = true;
	Dictionary body;
	body["input"] = login_input;
	body["room"] = want_room;
	send_frame(MVFrame::LOGIN, body);
}

// Connects to Secret DTLS through a relay with the one-time token issued over TLS
bool MVClient::start_state(const Dictionary &p_body) {
	const String state_token = p_body.get("state_token", String());
	if (state_token.is_empty()) {
		return false;
	}
	MVLink *old = state_link();
	if (old != nullptr) {
		old->close();
		remove_child(old);
		old->queue_free();
	}
	MVLink *link = memnew(MVLink);
	link->set_name("State");
	add_child(link);
	state = link->get_instance_id();
	link->connect(SNAME("__repped"), callable_mp(this, &MVClient::apply));
	link->connect(SNAME("__answered"), callable_mp(this, &MVClient::answered));
	link->connect(SNAME("__failed"), callable_mp(this, &MVClient::lost));
	// When another device owns the world, the destination and certificate come in the identity reply.
	// Players do not write per-room settings
	const String at = p_body.get("state_host", String());
	String where = hosts[host_i % hosts.size()];
	int at_port = port;
	String name = secret_name;
	Ref<X509Certificate> trust = ca;
	if (!at.is_empty()) {
		Ref<X509Certificate> room = Ref<X509Certificate>(X509Certificate::create());
		if (room.is_null() || room->load_from_string(p_body.get("state_cert", String())) != OK) {
			lost(U"Cannot read the room certificate");
			return false;
		}
		where = at;
		at_port = int(p_body.get("state_port", 0));
		name = p_body.get("state_name", String());
		trust = room;
	}
	const String reason = link->join(where, at_port, name, trust, state_token);
	if (!reason.is_empty()) {
		lost(reason);
		return false;
	}
	return true;
}

// Handles authentication, diffs and denials arriving over TLS
void MVClient::take(const PackedByteArray &p_data) {
	if (parser.is_null()) {
		return;
	}
	const uint64_t run = generation;
	Ref<MVFrame> live_parser = parser;
	const String text = String::utf8(reinterpret_cast<const char *>(p_data.ptr()), p_data.size());
	for (const Variant &value : live_parser->feed(text)) {
		if (closed || generation != run || parser.ptr() != live_parser.ptr()) {
			return;
		}
		const Dictionary frame = value;
		const Dictionary body = frame.get("body", Dictionary());
		switch (int(frame.get("type", -1))) {
			case MVFrame::AUTH: {
				const Dictionary context = body.get("my", Dictionary());
				if (bool(context.get("authenticated", false))) {
					who = body.get("player", String());
					if (MVRuntime *rt = runtime_node()) {
						rt->clear_commands();
						rt->set_me(who);
					}
					token = body.get("resume_token", String());
					online = true;
					told_lost = String();
					if (!start_state(body)) {
						return;
					}
					// Announces this player's login only on the Nodes of this Client's world.
					// At this point those Nodes do not have the author's script yet,
					// so inspector-connected targets cannot be called. Emits after the script is loaded
					// The local owner keeps the same object and swaps its content, so the author's dictionaries keyed by my stay valid
					if (mine.is_null()) {
						mine.instantiate();
					}
					mine->set_context(who, context.duplicate(true));
					login_pending = true;
					if (resuming) {
						resuming = false;
						if (!send_frame(MVFrame::CONFIRM, Dictionary())) {
							return;
						}
					}
				} else if (!token.is_empty()) {
					resuming = true;
					Dictionary resume;
					resume["token"] = token;
					if (!send_frame(MVFrame::RESUME, resume)) {
						return;
					}
				} else if (!login_sent) {
					start_login();
				}
			} break;
			case MVFrame::REP:
				apply(body);
				break;
			case MVFrame::AUTH_URL: {
				// Opens the return listener before opening the browser. If it cannot open, shows the reason and stops
				const int port = int(body.get("port", 0));
				catcher.unref();
				visitor.unref();
				link_state = String();
				Ref<TCPServer> made;
				made.instantiate();
				if (made->listen(port, IPAddress("127.0.0.1")) != OK) {
					login_failed(vformat(U"Cannot open port %d for the login. Close the other program using it", port));
					break;
				}
				catcher = made;
				catch_until = OS::get_singleton()->get_ticks_msec() + LOGIN_MS;
				link_state = body.get("state", String());
				const String url = body.get("url", String());
				// Even where no browser opens, opening this one line allows login
				print_line(String(U"Online: sign in at ") + url);
				OS::get_singleton()->shell_open(url);
			} break;
			case MVFrame::LINKED:
				linked(body.get("my", Dictionary()));
				break;
			case MVFrame::DENY:
				// Another device joined with the same account. Reconnecting would make both knock each other off, so stop here
				if (bool(body.get("stop", false))) {
					print_line(String(U"Online: ") + String(body.get("reason", U"Signed in from another device")));
					close();
					return;
				}
				if (resuming) {
					resuming = false;
					token.clear();
					if (!login_sent) {
						start_login();
					}
				} else if (bool(body.get("link", false))) {
					// External login did not finish. The connection stays. Notifies the author
					visitor.unref();
					catcher.unref();
					login_failed(body.get("reason", U"The login did not finish"));
					return;
				} else if (int(body.get("retry", 0)) > 0) {
					// Refusals that pass after a short wait (room full, storage unreadable, too many new accounts).
					// Reconnects after the given delay and asks the same thing. Not shown as an error
					const String reason = body.get("reason", U"Try again later");
					if (reason != told_lost) {
						told_lost = reason;
						print_line(String(U"Online: ") + reason);
					}
					lost(reason);
					retry_at = OS::get_singleton()->get_ticks_msec() + uint64_t(int(body["retry"]));
					return;
				} else if (bool(body.get("again", false))) {
					// The room to join is gone. Matchmaking decides where to go, so ask again.
					// This is how players in a room whose host left move to the next host
					const String gone = body.get("reason", U"No room available");
					lost(gone);
					ask_again(gone);
					return;
				} else {
					const String reason = body.get("reason", U"Refused by Secret");
					if (closed || generation != run) {
						return;
					}
					// Ordinary refusals such as room full are not shown as errors. Shows only one line of reason.
					// The following "disconnected" message only restates this reason
					if (reason != told_lost) {
						told_lost = reason;
						print_line(String(U"Online: ") + reason);
					}
				}
				break;
			default:
				break;
		}
	}
}

// Reports diffs that could not be applied back to the author.
// Dropping them would leave no clue when Client-side sync does not work
void MVClient::report_bad(const Array &p_bad) {
	for (const Variant &one : p_bad) {
		const String why = one;
		if (told_bad.has(why)) {
			continue;
		}
		// Showing the same reason every frame buries real clues. Once per reason
		told_bad.insert(why);
		WARN_PRINT(vformat(U"Online: cannot apply the received update: %s", why));
	}
}

// Applies one world diff in the order structure, Client script, synced values
void MVClient::apply(const Dictionary &p_frame) {
	if (bool(p_frame.get("reset", false))) {
		reset_world();
	}
	MVRuntime *live_world = runtime_node();
	if (live_world == nullptr || live_world->server_node() == nullptr) {
		lost(U"Reconnecting: the public world was lost");
		return;
	}
	const uint64_t run = generation;
	const ObjectID live_rt_id = runtime;
	Ref<MVRep> live_rep = rep;
	if (live_rep.is_null()) {
		lost(U"Reconnecting: the public world state was lost");
		return;
	}
	Dictionary shape;
	shape["add"] = p_frame.get("add", Array());
	shape["del"] = p_frame.get("del", Array());
	report_bad(live_rep->apply(live_world, shape));
	if (closed || generation != run || runtime != live_rt_id || rep.ptr() != live_rep.ptr()) {
		return;
	}
	if (p_frame.has("apis")) {
		const Dictionary apis = p_frame["apis"];
		for (const Variant &uid_value : apis.get_key_list()) {
			if (apis[uid_value].get_type() != Variant::NIL) {
				Node *target = live_rep->at_uid(live_world, int64_t(uid_value));
				// If the script cannot be loaded, the Node stays on screen with no values, no _ready and no @online func stubs.
				// Does not continue silently, so the author can trace the cause
				if (target != nullptr && MVClientScript::dress(target, apis[uid_value]) == nullptr) {
					ERR_PRINT(vformat(U"Online: cannot load %s for the client. Check %s",
							String(target->get_name()), MVClientScript::source_of(target->get_meta(SNAME("kind"), String()))));
				} else if (target != nullptr && !String(target->get_meta(SNAME("own"), String())).is_empty()) {
					const bool mine_node = live_world->is_my(target);
					if (mine_node) {
						// The my of the owner's own Thing is the same player locally. Same object passed with logged_in, so it stays valid as a dictionary key
						target->set("__ctx", mine);
					}
					MVThing::own_cameras(target, mine_node);
				}
			}
		}
	}
	// Sets Online-side initial values before ready so author initialization starts from the Server's values
	Dictionary values = p_frame.duplicate(false);
	for (const String &key : { String("reset"), String("add"), String("del"), String("apis"), String("fire"), String("who") }) {
		values.erase(key);
	}
	report_bad(live_rep->apply(live_world, values));
	if (p_frame.has("apis")) {
		const Dictionary apis = p_frame["apis"];
		for (const Variant &uid_value : apis.get_key_list()) {
			Node *node = live_rep->at_uid(live_world, int64_t(uid_value));
			if (node == nullptr || bool(node->get_meta(SNAME("mv_online_ready"), false))) {
				continue;
			}
			node->set_meta(SNAME("mv_online_ready"), true);
			GDScriptOnline::ready(node);
		}
	}
	// Announces this player's login once the author's script is loaded.
	// Emitting at AUTH time would silently fail because the connected functions do not exist yet
	if (login_pending && mine.is_valid()) {
		if (Node *host = live_world->server_node()) {
			if (host->get_script() != Variant()) {
				login_pending = false;
				host->emit_signal(SNAME("logged_in"), mine);
			}
		}
	}
	// Announces appearance only after all setup is done. The Node the author receives
	// already has its @online func stubs, distributed values and _ready done, ready to use
	if (!live_rep->newborn().is_empty()) {
		if (Node *host = live_world->server_node()) {
			for (const ObjectID &id : live_rep->newborn()) {
				if (Node *node = Object::cast_to<Node>(ObjectDB::get_instance(id))) {
					host->emit_signal(SNAME("spawned"), node);
				}
			}
		}
		live_rep->forget_newborn();
	}
	// Emits one-shot signals after initialization so right after connecting the order matches normal frames
	if (p_frame.has("fire")) {
		Dictionary events;
		events["fire"] = p_frame["fire"];
		report_bad(live_rep->apply(live_world, events));
	}
	if (closed || generation != run || runtime != live_rt_id || rep.ptr() != live_rep.ptr()) {
		return;
	}
	if (!who.is_empty()) {
		Node *found = player_for(who);
		player = found != nullptr ? found->get_instance_id() : ObjectID();
	}
	Node *live_player = player_node();
	if (!announced && live_player != nullptr) {
		announced = true;
	}
}

// Returns the Node under Server/Players whose Secret owner matches
Node *MVClient::player_for(const String &p_who) const {
	MVRuntime *rt = runtime_node();
	Node *server = rt != nullptr ? rt->server_node() : nullptr;
	if (server == nullptr) {
		return nullptr;
	}
	for (int i = 0; i < server->get_child_count(); i++) {
		Node *node = server->get_child(i);
		if (String(node->get_meta("own", String())) == p_who) {
			return node;
		}
	}
	return nullptr;
}

// Rebuilds the public world without leftover Nodes when Server children are recreated
void MVClient::reset_world() {
	MVRuntime *old = runtime_node();
	Node *parent = old != nullptr && old->get_parent() != nullptr ? old->get_parent() : this;
	if (old != nullptr) {
		GDScriptOnline::begin_trusted_write();
		parent->remove_child(old);
		old->queue_free();
		GDScriptOnline::end_trusted_write();
	}
	rep->reset();
	MVRuntime *rt = memnew(MVRuntime);
	rt->set_name("Runtime");
	parent->add_child(rt);
	runtime = rt->get_instance_id();
	if (!rt->start(false).is_empty()) {
		return;
	}
	rep->know(rt->server_node());
	rt->set_me(who);
	player = ObjectID();
	announced = false;
}

// Stops commands, announces if connected, and schedules the next Relay candidate
void MVClient::lost(const String &p_reason) {
	if (closed) {
		return;
	}
	const uint64_t run = generation;
	Ref<MVSecretClient> live_net = net;
	// Reads what has arrived on the identity connection first. The refusal for being taken over by another device is there
	if (live_net.is_valid()) {
		live_net->poll();
		if (closed || generation != run) {
			return;
		}
	}
	if (MVRuntime *rt = runtime_node()) {
		rt->clear_commands();
	}
	if (live_net.is_valid()) {
		live_net->close();
	}
	// Also closes the world connection. Rebinding identity opens a new one, so it does not keep reconnecting to a peer that is gone.
	// May be called from inside the connection's disconnect notice, so it closes on the next frame
	if (MVLink *link = state_link()) {
		callable_mp(link, &MVLink::close).call_deferred();
	}
	online = false;
	drop_replies();
	if (announced) {
		announced = false;
		Node *live_player = player_node();
		if (live_player != nullptr && live_player->has_signal("disconnected")) {
			live_player->emit_signal("disconnected");
		}
		if (closed || generation != run) {
			return;
		}
	}
	// Does not repeat the same reason every second. A player whose server is simply not up
	// would read dozens of identical lines. Reports only when the reason changes
	// or when a connection that was established drops
	if (p_reason != told_lost) {
		told_lost = p_reason;
		WARN_PRINT(String(U"Online: ") + p_reason);
	}
	host_i = (host_i + 1) % hosts.size();
	retry_at = OS::get_singleton()->get_ticks_msec() + RETRY_MS;
}

// Wraps one frame safely and adds it to the TLS send queue to Secret
bool MVClient::send_frame(int p_type, const Dictionary &p_body) {
	const String packed = MVFrame::pack(p_type, p_body);
	Ref<MVSecretClient> live_net = net;
	if (packed.is_empty() || live_net.is_null() || !live_net->send(packed.to_utf8_buffer())) {
		lost(U"Cannot send to Secret");
		return false;
	}
	return true;
}

bool MVClient::ask(int64_t p_uid, const String &p_fn, const Array &p_args, const Variant &p_reply) {
	if (!online) {
		return false;
	}
	// Commands travel over the world connection. Identity TLS is used only for login and handover.
	// This way the path stays the same even when the world moves to a host
	MVLink *link = state_link();
	const int64_t id = ++asked;
	if (link == nullptr || !link->ask(id, p_uid, p_fn, p_args)) {
		return false;
	}
	Ref<MVReply> box = p_reply;
	if (box.is_valid()) {
		waiting_replies[id] = box;
	}
	return true;
}

// Passes an answer returned from the Online side to the waiting script
void MVClient::answered(int64_t p_id, const Variant &p_value) {
	const HashMap<int64_t, Ref<MVReply>>::Iterator found = waiting_replies.find(p_id);
	if (!found) {
		return;
	}
	Ref<MVReply> box = found->value;
	waiting_replies.remove(found);
	if (box.is_valid()) {
		box->call_deferred(SNAME("__finish"), p_value);
	}
}

// Closes pending answers with empty values. Otherwise scripts awaiting them never return
void MVClient::drop_replies() {
	for (const KeyValue<int64_t, Ref<MVReply>> &one : waiting_replies) {
		if (one.value.is_valid()) {
			one.value->call_deferred(SNAME("__finish"), Variant());
		}
	}
	waiting_replies.clear();
}

// Shows why startup failed and quits the SceneTree so it does not continue as a success
void MVClient::fail_start(const String &p_reason) {
	ERR_PRINT(String(U"Online Client: ") + p_reason);
	if (get_tree() != nullptr) {
		get_tree()->quit(2);
	}
}

// Ties auto-start, network polling and shutdown to the Node lifetime
void MVClient::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
			current = this;
			ready();
			break;
		case NOTIFICATION_PROCESS:
			process();
			break;
		case NOTIFICATION_EXIT_TREE:
			if (current == this) {
				current = nullptr;
			}
			close(true);
			break;
	}
}

// Internal class, so nothing is exposed
void MVClient::_bind_methods() {
}
