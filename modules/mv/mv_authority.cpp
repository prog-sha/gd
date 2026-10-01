/**************************************************************************/
/*  mv_authority.cpp                                                      */
/**************************************************************************/

// Does not trust IDs claimed by Clients; only Players bound to a TLS route are passed to the Online side world.

#include "mv_authority.h"

#ifdef MV_SECRET_ENABLED

#include "mv_client_script.h"
#include "mv_frame.h"
#include "mv_gate.h"
#include "mv_link.h"
#include "mv_local_id.h"
#include "mv_online.h"
#include "mv_secret.h"
#include "mv_runtime.h"
#include "mv_thing.h"
#include "mv_vault.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/crypto/crypto_core.h"
#include "core/io/json.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/stream_peer_tls.h"
#include "core/io/tcp_server.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "modules/gdscript/gdscript_online.h"

static constexpr uint64_t HANDSHAKE_MS = 10000; // Time to keep a connection whose TLS is not established
static constexpr uint64_t AUTH_MS = 30000; // Time to keep a TLS connection that does not log in

// Describe a configured account provider without embedding any service credentials or URLs.
struct Provider {
	String authorize_url; // Browser endpoint.
	String token_url; // Authorization-code exchange endpoint.
	String user_url; // Endpoint returning the verified account identity.
	String scope; // Scopes requested from the provider.
	String client_id; // Public application identifier.
	String client_secret; // Server-only application secret.
	String id_field; // Identity key in the user response.
	String name_field; // Display-name key in the user response.
};
static constexpr int LOGIN_PORT = 4435; // Browser callback on this device.

// Only encrypted remote endpoints or a local development provider may receive credentials.
static bool provider_url(const String &p_url) {
	String scheme, host, path, fragment;
	int port = 0;
	const int start = p_url.find("://");
	const String authority = start >= 0 ? p_url.substr(start + 3).get_slice("/", 0).get_slice("?", 0) : String();
	if (authority.contains("@") || p_url.parse_url(scheme, host, port, path, fragment) != OK || host.is_empty() || !fragment.is_empty()) {
		return false;
	}
	return scheme == "https://" || (scheme == "http://" && host == "127.0.0.1" && port > 0 && port <= 65535);
}

// Read one complete provider configuration from the Secret's server-only settings.
static bool provider_of(const Dictionary &p_config, const String &p_name, Provider &r_provider) {
	const Variant entries = p_config.get("account-providers", Variant());
	if (entries.get_type() != Variant::DICTIONARY) {
		return false;
	}
	const Variant entry = Dictionary(entries).get(p_name, Variant());
	if (entry.get_type() != Variant::DICTIONARY) {
		return false;
	}
	const Dictionary one = entry;
	auto field = [&](const char *p_key) -> String {
		const Variant value = one.get(p_key, Variant());
		return value.get_type() == Variant::STRING ? String(value).strip_edges() : String();
	};
	r_provider.authorize_url = field("authorize_url");
	r_provider.token_url = field("token_url");
	r_provider.user_url = field("user_url");
	r_provider.scope = field("scope");
	r_provider.client_id = field("client_id");
	r_provider.client_secret = field("client_secret");
	r_provider.id_field = field("id_field");
	r_provider.name_field = field("name_field");
	if (r_provider.id_field.is_empty()) {
		r_provider.id_field = "id";
	}
	if (r_provider.name_field.is_empty()) {
		r_provider.name_field = "name";
	}
	return provider_url(r_provider.authorize_url) && provider_url(r_provider.token_url) && provider_url(r_provider.user_url) &&
			!r_provider.client_id.is_empty() && !r_provider.client_secret.is_empty();
}

// PKCE. SHA-256 of the verifier in URL-safe base64
static String challenge_of(const String &p_verifier) {
	const PackedByteArray digest = p_verifier.sha256_buffer();
	return CryptoCore::b64_encode_str(digest.ptr(), digest.size()).replace("+", "-").replace("/", "_").replace("=", "");
}

static String redirect_uri() {
	return vformat("http://127.0.0.1:%d/", LOGIN_PORT);
}

// Small listener handling only partial TCP and TLS send/receive
class MVTLSServer {
	struct Peer {
		Ref<StreamPeerTLS> tls; // Per-connection TLS state
		PackedByteArray out; // Remainder of a partial write
		String from; // TCP peer address
		uint64_t made = 0; // Handshake start time
		bool ready = false; // Whether the connect notice was passed upward
		bool closing = false; // Close after flushing what is left to send
	};

	Ref<TCPServer> listener; // Raw TCP listener
	Ref<TLSOptions> options; // Server key and certificate
	int peers_max = 64; // Limit of TLS connections kept, including those in handshake
	int per_room = 64; // Allowance for one room. Multiplied by the room count when handling identity only
	HashMap<String, Peer> peers; // Maps a random route to its TLS connection
	Vector<String> pending_drop; // Routes asked to close during poll. Closed together after the scan
	bool polling = false; // Whether peers is being scanned. No erase during the scan

public:
	// Opens the TLS listener without exposing the key
	String open(int p_port, const String &p_bind, const Ref<CryptoKey> &p_key, const Ref<X509Certificate> &p_cert, int p_room_max) {
		// Accepts extra for handovers and unauthenticated connections. Not capped by room_max
		per_room = CLAMP(p_room_max, 1, (int)Secret::ROOM_LIMIT) * 2 + 8;
		peers_max = per_room;
		listener.instantiate();
		options = TLSOptions::server(p_key, p_cert);
		if (options.is_null() || listener->listen(p_port, IPAddress(p_bind)) != OK) {
			return vformat(U"Cannot listen for TLS on %s:%d", p_bind, p_port);
		}
		return String();
	}

	// Advances handshake, receive and partial send once.
	// Even when asked to close from within receive, no erase happens here. Freeing the peer being scanned
	// would make the following send and the next-element fetch touch freed memory
	void poll(MVAuthority *p_owner) {
		// When handling identity only, players and hosts of every room arrive here.
		// With a single room's allowance the entry would stall as rooms increase
		peers_max = per_room * p_owner->rooms_open();
		while (peers.size() < peers_max && listener->is_connection_available()) {
			Ref<StreamPeerTCP> raw = listener->take_connection();
			Ref<StreamPeerTLS> stream = Ref<StreamPeerTLS>(StreamPeerTLS::create());
			if (raw.is_null() || stream.is_null() || stream->accept_stream(raw, options) != OK) {
				continue;
			}
			Peer peer;
			peer.from = String(raw->get_connected_host());
			// One peer must not monopolize the entry. Stop accepting beyond one room's allowance
			int same = 0;
			for (const KeyValue<String, Peer> &other : peers) {
				same += other.value.from == peer.from ? 1 : 0;
			}
			if (same >= per_room) {
				continue;
			}
			peer.tls = stream;
			peer.made = Time::get_singleton()->get_ticks_msec();
			peers[MVLocalID::random_hex(16)] = peer;
		}
		Vector<String> drop;
		polling = true;
		for (KeyValue<String, Peer> &entry : peers) {
			Peer &peer = entry.value;
			peer.tls->poll();
			if (!peer.ready && Time::get_singleton()->get_ticks_msec() - peer.made >= HANDSHAKE_MS) {
				drop.push_back(entry.key);
				continue;
			}
			switch (peer.tls->get_status()) {
				case StreamPeerTLS::STATUS_CONNECTED: {
					if (!peer.ready) {
						peer.ready = true;
						p_owner->connected(entry.key);
					}
					const int count = MIN(peer.tls->get_available_bytes(), MVFrame::READ_MAX);
					if (count > 0) {
						PackedByteArray data;
						data.resize(count);
						if (peer.tls->get_data(data.ptrw(), count) == OK) {
							p_owner->received(entry.key, data);
						}
					}
					if (!peer.out.is_empty()) {
						int sent = 0;
						if (peer.tls->put_partial_data(peer.out.ptr(), peer.out.size(), sent) == OK && sent > 0) {
							peer.out = peer.out.slice(sent);
						}
					}
					if (peer.closing && peer.out.is_empty()) {
						drop.push_back(entry.key);
					}
				} break;
				case StreamPeerTLS::STATUS_ERROR:
				case StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH:
				case StreamPeerTLS::STATUS_DISCONNECTED:
					drop.push_back(entry.key);
					break;
				default:
					break;
			}
		}
		polling = false;
		// Close what accumulated during the scan first. Pending sends were flushed at the end of this pass
		const Vector<String> deferred = pending_drop;
		pending_drop.clear();
		for (const String &route : deferred) {
			drop_route(route, p_owner);
		}
		for (const String &route : drop) {
			drop_route(route, p_owner);
		}
	}

	// Queues a frame within the limit for the route
	bool send(const String &p_route, const PackedByteArray &p_data) {
		HashMap<String, Peer>::Iterator found = peers.find(p_route);
		if (!found || !found->value.ready || p_data.is_empty() || found->value.out.size() + p_data.size() > MVFrame::WRITE_MAX) {
			return false;
		}
		found->value.out.append_array(p_data);
		return true;
	}

	// Returns the route's peer address. The host destination is taken from here, not from its announcement
	String where_from(const String &p_route) const {
		const HashMap<String, Peer>::ConstIterator found = peers.find(p_route);
		return found ? found->value.from : String();
	}

	// Closes the given route and hands auth record cleanup upward
	void drop_route(const String &p_route, MVAuthority *p_owner) {
		HashMap<String, Peer>::Iterator found = peers.find(p_route);
		if (!found) {
			return;
		}
		if (polling) {
			// Close after the scan ends. Do not queue the same route twice
			if (pending_drop.find(p_route) < 0) {
				pending_drop.push_back(p_route);
			}
			return;
		}
		// Flush what is left before closing. Cutting before the denial reason arrives makes the peer silently reconnect.
		// If the peer is already gone nothing can be flushed, so do not wait. Waiting would leave this connection open forever and the room never closed
		if (found->value.ready && !found->value.out.is_empty() && found->value.tls->get_status() == StreamPeerTLS::STATUS_CONNECTED) {
			int sent = 0;
			if (found->value.tls->put_partial_data(found->value.out.ptr(), found->value.out.size(), sent) == OK && sent > 0) {
				found->value.out = found->value.out.slice(sent);
			}
			if (!found->value.out.is_empty()) {
				found->value.closing = true;
				return;
			}
		}
		const bool ready = found->value.ready;
		found->value.tls->disconnect_from_stream();
		peers.erase(p_route);
		if (ready) {
			p_owner->parted(p_route);
		}
	}

	// Closes the listener and all connections
	void close(MVAuthority *p_owner) {
		listener->stop();
		Vector<String> all;
		for (KeyValue<String, Peer> &entry : peers) {
			entry.value.out.clear(); // Do not wait when shutting down
			all.push_back(entry.key);
		}
		for (const String &route : all) {
			drop_route(route, p_owner);
		}
		listener.unref();
		options.unref();
	}
};

MVRuntime *MVAuthority::runtime_node() const {
	return Object::cast_to<MVRuntime>(ObjectDB::get_instance(runtime));
}

// Returns the live DTLS state entry
MVLink *MVAuthority::state_link() const {
	return Object::cast_to<MVLink>(ObjectDB::get_instance(state));
}

// Sends one denial. A denial with again means another room would admit the player
void MVAuthority::deny(const String &p_route, const String &p_reason, bool p_again, bool p_stop) {
	if (HashMap<String, Route>::Iterator route = routes.find(p_route)) {
		route->value.busy = false;
	}
	Dictionary denied;
	denied["reason"] = p_reason;
	if (p_again) {
		denied["again"] = true;
	}
	if (p_stop) {
		denied["stop"] = true;
	}
	send(p_route, MVFrame::DENY, denied);
}

// External login failure. The connection stays. The peer notifies the author with login_failed
void MVAuthority::deny_link(const String &p_route, const String &p_reason) {
	Dictionary denied;
	denied["reason"] = p_reason;
	denied["link"] = true;
	send(p_route, MVFrame::DENY, denied);
}

// Denial that passes after a short wait. The peer reconnects and asks the same again
void MVAuthority::deny_later(const String &p_route, const String &p_reason, int p_wait_ms) {
	if (HashMap<String, Route>::Iterator route = routes.find(p_route)) {
		route->value.busy = false;
	}
	Dictionary denied;
	denied["reason"] = p_reason;
	denied["retry"] = p_wait_ms;
	send(p_route, MVFrame::DENY, denied);
}

// New accounts are limited to FRESH_MAX per address per 10 minutes. Stops mass creation by discarding and recreating accounts,
// and abuse that bloats the account table. Logins to existing accounts are not counted
bool MVAuthority::may_create(const String &p_route) {
	const String from = tls != nullptr ? tls->where_from(p_route) : String();
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	Vector<uint64_t> &made = fresh_from[from];
	while (!made.is_empty() && now - made[0] >= FRESH_MS) {
		made.remove_at(0);
	}
	return made.size() < FRESH_MAX;
}

// Returns the Secret's anonymous context once TLS is established
void MVAuthority::connected(const String &p_route) {
	Route route;
	route.parser.instantiate();
	route.made = Time::get_singleton()->get_ticks_msec();
	route.heard = route.made;
	route.rate_at = route.made;
	routes[p_route] = route;
	Dictionary body;
	body["my"] = vault.session_begin(p_route);
	send(p_route, MVFrame::AUTH, body);
}

// Rebuilds TLS bytes into frames and handles them in order
void MVAuthority::received(const String &p_route, const PackedByteArray &p_data) {
	HashMap<String, Route>::Iterator found = routes.find(p_route);
	if (!found) {
		return;
	}
	found->value.heard = Time::get_singleton()->get_ticks_msec();
	const String text = String::utf8(reinterpret_cast<const char *>(p_data.ptr()), p_data.size());
	for (const Variant &value : found->value.parser->feed(text)) {
		handle(p_route, value);
	}
}

// Keeps the Player for reconnection until the session deadline after TLS disconnects
void MVAuthority::parted(const String &p_route) {
	HashMap<String, Route>::Iterator found = routes.find(p_route);
	if (!found) {
		return;
	}
	const String who = found->value.who;
	const bool was_host = !found->value.room.is_empty();
	// Disconnected while waiting for the room's reply. Unless the seat is freed,
	// the room stays full with nobody in it
	const String waiting = Dictionary(found->value.holding).get("player", String());
	routes.erase(p_route);
	if (was_host) {
		host_gone(p_route);
		return;
	}
	if (!waiting.is_empty()) {
		tell_room(waiting, true);
	}
	vault.session_forget(p_route);
	// If this player already re-entered on another route, cleanup of the old one must not break the new one
	const HashMap<String, String>::ConstIterator current = who_route.find(who);
	const bool still_mine = current && current->value == p_route;
	if (!who.is_empty() && still_mine) {
		who_route.erase(who);
		expires[who] = Time::get_singleton()->get_ticks_msec() + uint64_t(vault.get_keep_sec()) * 1000;
		if (MVLink *link = state_link()) {
			link->revoke(who);
		}
		if (MVRuntime *rt = runtime_node()) {
			rt->disconnect_player(who);
		}
		// When the host holds the world, only this side knows the player left.
		// Room records are kept while waiting for reconnection
		tell_room(who, false);
	}
}

// Tells the room's host that this player left
void MVAuthority::tell_room(const String &p_who, bool p_gone) {
	const HashMap<String, String>::ConstIterator room = who_room.find(p_who);
	if (!room) {
		return;
	}
	const HashMap<String, Room>::ConstIterator host = rooms.find(room->value);
	if (host) {
		Dictionary body;
		body["who"] = p_who;
		body["gone"] = p_gone;
		send(host->value.route, MVFrame::HOST_PART, body);
	}
	if (p_gone) {
		leave_room(p_who);
	}
}

// Removes the player from room records. The host sends the last save on a following tick, so writes close after a grace period
void MVAuthority::leave_room(const String &p_who) {
	who_room.erase(p_who);
	for (const KeyValue<String, String> &entry : ids) {
		if (entry.value == p_who) {
			expire_grant(entry.key);
		}
	}
}

void MVAuthority::expire_grant(const String &p_id) {
	HashMap<String, Grant>::Iterator grant = grants.find(p_id);
	if (grant && grant->value.until == 0) {
		grant->value.until = Time::get_singleton()->get_ticks_msec() + GRANT_MS;
	}
}

// Separates pre-login and authenticated commands by route state
void MVAuthority::handle(const String &p_route, const Dictionary &p_frame) {
	HashMap<String, Route>::Iterator found = routes.find(p_route);
	if (!found) {
		return;
	}
	const int type = p_frame.get("type", -1);
	const Dictionary body = p_frame.get("body", Dictionary());
	// Path taken only when hosts hold rooms. An announced route is treated as host-only
	if (!secret_only && type >= MVFrame::HOST_HELLO && type <= MVFrame::HOST_STORE) {
		// This side runs the world itself. Tell the host it is not ready to hold rooms.
		// Dropping it silently leaves the announcing host stuck waiting for a reply
		deny(p_route, U"This destination runs its own world. Rooms are handed out by the --online-secret side");
		return;
	}
	if (secret_only) {
		switch (type) {
			case MVFrame::HOST_HELLO:
				if (found->value.who.is_empty() && spare(found->value)) {
					host_hello(p_route, body);
				}
				return;
			case MVFrame::HOST_JOINED:
			case MVFrame::HOST_SAVE:
			case MVFrame::HOST_CALL:
				if (found->value.room.is_empty()) {
					return;
				}
				// Hosts carry a whole room's traffic, so one Client's limit is not enough.
				// A limit is still needed. The Secret alone handles logins for every room
				if (!spare_host(found->value)) {
					if (type == MVFrame::HOST_CALL) {
						secret_done(Variant(), p_route, int64_t(body.get("id", 0))); // Still answer. Without an answer the host's await never returns
					}
					return;
				}
				if (type == MVFrame::HOST_JOINED) {
					host_joined(p_route, body);
				} else if (type == MVFrame::HOST_SAVE) {
					host_save(p_route, body);
				} else {
					host_call(p_route, body);
				}
				return;
			default:
				break;
		}
		if (!found->value.room.is_empty()) {
			// An announced host does not use Client identity commands.
			// Liveness checks are answered the same way
			if (type == MVFrame::PING) {
				send(p_route, MVFrame::PONG, Dictionary());
			}
			return;
		}
	}
	switch (type) {
		case MVFrame::LOGIN:
			if (found->value.who.is_empty() && !found->value.busy && spare(found->value)) {
				found->value.busy = true;
				login(p_route, body.get("input", Variant()), body.get("room", String()));
			}
			break;
		case MVFrame::RESUME:
			if (found->value.who.is_empty() && !found->value.busy && spare(found->value)) {
				found->value.busy = true;
				resume(p_route, body.get("token", String()));
			}
			break;
		case MVFrame::CONFIRM:
			if (!found->value.who.is_empty() && spare(found->value)) {
				vault.session_confirm(p_route);
			}
			break;
		case MVFrame::PING:
			// Building a PONG also costs, so it goes through the same per-second limit as other commands
			if (spare(found->value)) {
				send(p_route, MVFrame::PONG, Dictionary());
			}
			break;
		case MVFrame::LOGIN_WITH:
			if (!found->value.who.is_empty() && spare(found->value)) {
				login_with(p_route, body.get("provider", String()));
			}
			break;
		case MVFrame::LOGIN_CODE:
			if (!found->value.who.is_empty() && spare(found->value)) {
				login_code(p_route, body.get("code", String()), body.get("state", String()));
			}
			break;
		case MVFrame::LOGOUT:
			if (!found->value.who.is_empty() && spare(found->value)) {
				logout(p_route);
			}
			break;
		default:
			break;
	}
}

// Starts an external login. Builds and returns the URL to open in the browser.
// The client id and URL are built here, so the Client needs no settings
void MVAuthority::login_with(const String &p_route, const String &p_provider) {
	HashMap<String, Route>::Iterator found = routes.find(p_route);
	if (!found) {
		return;
	}
	Route &route = found->value;
	if (route.link_committing) {
		deny_link(p_route, U"An account is already being linked");
		return;
	}
	Provider provider;
	if (!provider_of(config, p_provider, provider)) {
		deny_link(p_route, vformat(U"Account provider %s is not configured", p_provider));
		return;
	}
	route.link_provider = p_provider;
	route.link_state = MVLocalID::random_hex(16);
	route.link_attempt = route.link_state;
	// The verifier never leaves here. An authorization code taken on another device and injected into this connection fails at exchange
	route.link_verifier = MVLocalID::random_hex(32);
	Dictionary body;
	const String separator = provider.authorize_url.contains("?") ? "&" : "?";
	body["url"] = provider.authorize_url + separator + "response_type=code&client_id=" + provider.client_id.uri_encode() +
			"&redirect_uri=" + redirect_uri().uri_encode() + "&scope=" + provider.scope.uri_encode() +
			"&state=" + route.link_state + "&code_challenge=" + challenge_of(route.link_verifier) + "&code_challenge_method=S256";
	body["port"] = LOGIN_PORT;
	body["state"] = route.link_state;
	send(p_route, MVFrame::AUTH_URL, body);
}

// Receives the authorization code returned from the browser and exchanges it for a token with the client secret. The secret never leaves here
void MVAuthority::login_code(const String &p_route, const String &p_code, const String &p_state) {
	Route &route = routes[p_route];
	Provider provider;
	if (!provider_of(config, route.link_provider, provider) || route.link_state.is_empty() || p_state != route.link_state || p_code.is_empty()) {
		deny_link(p_route, U"The login was not started from this connection");
		return;
	}
	route.link_state = String();
	const String attempt = route.link_attempt;
	const String verifier = route.link_verifier;
	route.link_verifier = String();
	HTTPRequest *http = memnew(HTTPRequest);
	http->set_name("Login");
	http->set_max_redirects(0);
	http->set_timeout(double(AUTH_MS) / 1000.0);
	add_child(http);
	PackedStringArray headers;
	headers.push_back("Content-Type: application/x-www-form-urlencoded");
	const String body = "grant_type=authorization_code&code=" + p_code.uri_encode() +
			"&redirect_uri=" + redirect_uri().uri_encode() +
			"&client_id=" + provider.client_id.uri_encode() +
			"&client_secret=" + provider.client_secret.uri_encode() +
			"&code_verifier=" + verifier;
	http->connect(SNAME("request_completed"), callable_mp(this, &MVAuthority::token_got).bind(p_route, route.link_provider, attempt, http->get_instance_id()), CONNECT_ONE_SHOT);
	if (http->request(provider.token_url, headers, HTTPClient::METHOD_POST, body) != OK) {
		http->queue_free();
		deny_link(p_route, U"Cannot reach the login provider");
	}
}

// Token reply. Asks for the peer's id and name
void MVAuthority::token_got(int p_result, int p_code, const PackedStringArray &, const PackedByteArray &p_body, const String &p_route, const String &p_provider, const String &p_attempt, ObjectID p_http) {
	HTTPRequest *http = Object::cast_to<HTTPRequest>(ObjectDB::get_instance(p_http));
	const HashMap<String, Route>::ConstIterator route = routes.find(p_route);
	if (!route || route->value.link_provider != p_provider || route->value.link_attempt != p_attempt) {
		if (http != nullptr) {
			http->queue_free();
		}
		return;
	}
	Provider provider;
	const bool configured = provider_of(config, p_provider, provider);
	const Variant parsed = JSON::parse_string(String::utf8((const char *)p_body.ptr(), p_body.size()));
	const String token = parsed.get_type() == Variant::DICTIONARY ? String(Dictionary(parsed).get("access_token", String())) : String();
	if (http == nullptr || !configured || p_result != HTTPRequest::RESULT_SUCCESS || p_code != 200 || token.is_empty()) {
		if (http != nullptr) {
			http->queue_free();
		}
		deny_link(p_route, U"The login provider refused the login");
		return;
	}
	PackedStringArray headers;
	headers.push_back("Authorization: Bearer " + token);
	http->connect(SNAME("request_completed"), callable_mp(this, &MVAuthority::user_got).bind(p_route, p_provider, p_attempt, p_http), CONNECT_ONE_SHOT);
	if (http->request(provider.user_url, headers, HTTPClient::METHOD_GET) != OK) {
		http->queue_free();
		deny_link(p_route, U"Cannot reach the login provider");
	}
}

// Reply with the peer's id and name. The account is bound only here
void MVAuthority::user_got(int p_result, int p_code, const PackedStringArray &, const PackedByteArray &p_body, const String &p_route, const String &p_provider, const String &p_attempt, ObjectID p_http) {
	if (HTTPRequest *http = Object::cast_to<HTTPRequest>(ObjectDB::get_instance(p_http))) {
		http->queue_free();
	}
	HashMap<String, Route>::Iterator route = routes.find(p_route);
	if (!route || route->value.link_provider != p_provider || route->value.link_attempt != p_attempt) {
		return;
	}
	Provider provider;
	const bool configured = provider_of(config, p_provider, provider);
	const Variant parsed = JSON::parse_string(String::utf8((const char *)p_body.ptr(), p_body.size()));
	const Dictionary user = parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();
	const String id = configured ? String(user.get(provider.id_field, Variant())) : String();
	if (!configured || p_result != HTTPRequest::RESULT_SUCCESS || p_code != 200 || id.is_empty()) {
		deny_link(p_route, U"The login provider did not tell who you are");
		return;
	}
	route->value.link_committing = true;
	link_done(p_route, p_provider, id, String(user.get(provider.name_field, id)), p_attempt);
}

// Binds the external account to the current one and passes the new my to the world and the owner.
// When the host holds the world it is passed to the host too. Values stay stored by the Secret, so the host keeps nothing
void MVAuthority::link_done(const String &p_route, const String &p_provider, const String &p_id, const String &p_name, const String &p_attempt) {
	const HashMap<String, Route>::ConstIterator route = routes.find(p_route);
	if (!route || route->value.link_attempt != p_attempt) {
		return;
	}
	vault.session_link(p_route, p_provider, p_id, p_name, callable_mp(this, &MVAuthority::linked).bind(p_route, p_attempt));
}

void MVAuthority::linked(const Dictionary &p_linked, const String &p_route, const String &p_attempt) {
	HashMap<String, Route>::Iterator route = routes.find(p_route);
	if (!route || route->value.link_attempt != p_attempt) {
		return;
	}
	route->value.link_committing = false;
	route->value.link_attempt = String();
	const String who = route->value.who;
	if (p_linked.is_empty()) {
		deny_link(p_route, U"Cannot link the account");
		return;
	}
	const Dictionary my = p_linked["my"];
	const String id = my.get("id", String());
	ids.erase(String(p_linked["before"]));
	expire_grant(String(p_linked["before"]));
	// If another device is playing on the same account, drop it. The newly entered device wins
	const HashMap<String, String>::ConstIterator other = ids.find(id);
	if (other && other->value != who) {
		evict(other->value, U"Signed in from another device");
	}
	ids[id] = who;
	if (secret_only) {
		const HashMap<String, String>::ConstIterator room = who_room.find(who);
		Dictionary accepted;
		accepted["player"] = who;
		accepted["state_token"] = MVLocalID::random_hex(32);
		accepted["my"] = my;
		if (room) {
			allow_in(p_route, room->value, accepted);
		}
	} else {
		// Reads the values left on the bound account, then restores them to the owner's existing objects
		load_saves(id, callable_mp(this, &MVAuthority::relinked).bind(p_route, my));
		return;
	}
	Dictionary body;
	body["my"] = my;
	send(p_route, MVFrame::LINKED, body);
}

void MVAuthority::relinked(const Variant &p_saved, const String &p_route, const Dictionary &p_my) {
	MVRuntime *rt = runtime_node();
	if (!routes.has(p_route) || rt == nullptr) {
		return;
	}
	if (p_saved.get_type() != Variant::DICTIONARY) {
		deny_later(p_route, MVVault::NOT_REACHABLE);
		return;
	}
	rt->join_context(routes[p_route].who, p_my, true, p_saved);
	Dictionary body;
	body["my"] = p_my;
	send(p_route, MVFrame::LINKED, body);
}

// Checks only the built-in Guest credentials and passes them to the author's login signal
void MVAuthority::login(const String &p_route, const Variant &p_input, const String &p_room) {
	HashMap<String, Route>::Iterator route = routes.find(p_route);
	if (p_input.get_type() != Variant::DICTIONARY) {
		deny(p_route, U"Guest credentials rejected");
		return;
	}
	// Only a device that created its own room names a room.
	// For others this side picks a room with space
	route->value.want = p_room;
	const Dictionary input = p_input;
	if (!input.has("guest")) {
		deny(p_route, U"This server has no external login provider");
		return;
	}
	const Dictionary result = vault.guest_login(input.get("guest", Variant()));
	if (!bool(result.get("ok", false))) {
		deny(p_route, result.get("reason", U"Guest credentials rejected"));
		return;
	}
	vault.session_connect(p_route, result.get("provider", String()), result.get("account_id", String()), may_create(p_route),
			callable_mp(this, &MVAuthority::guest_connected).bind(p_route));
}

// Account decided. Denies if there is none
void MVAuthority::guest_connected(const Dictionary &p_accepted, const String &p_route) {
	if (!p_accepted.has("my")) {
		if (p_accepted.has("reason")) {
			// The store is unreadable, or too many new accounts. Passes after a short wait
			deny_later(p_route, p_accepted["reason"], String(p_accepted["reason"]) == String(MVVault::TOO_MANY_NEW) ? int(FRESH_MS) : 1000);
		} else {
			deny(p_route, U"Cannot create a guest session");
		}
		return;
	}
	if (bool(p_accepted.get("created", false)) && tls != nullptr) {
		fresh_from[tls->where_from(p_route)].push_back(Time::get_singleton()->get_ticks_msec());
	}
	authorize(p_route, p_accepted);
}

// Checks a one-time token and moves the route to the existing Player
void MVAuthority::resume(const String &p_route, const String &p_token) {
	Dictionary accepted = vault.session_resume(p_route, p_token);
	const String session = Dictionary(accepted.get("my", Dictionary())).get("session", String());
	if (accepted.is_empty() || !sessions.has(session)) {
		// The vault still had it, but this side forgot the context. Unless the route returns to anonymous,
		// the next LOGIN is denied as already authenticated and silently stalls until the deadline
		if (!accepted.is_empty()) {
			vault.session_revoke(p_route);
			vault.session_begin(p_route);
		}
		deny(p_route, U"Reconnect token rejected");
		return;
	}
	accepted["player"] = sessions[session];
	authorize(p_route, accepted);
}

// Returns the Player and state traffic binding token to an authenticated route
void MVAuthority::authorize(const String &p_route, Dictionary p_accepted) {
	HashMap<String, Route>::Iterator route = routes.find(p_route);
	if (!route) {
		return; // Disconnected before the account was decided
	}
	const Dictionary context = p_accepted.get("my", Dictionary());
	const String id = context.get("id", String());
	String who = p_accepted.get("player", String());
	if (who.is_empty()) {
		const HashMap<String, String>::ConstIterator known = ids.find(id);
		who = known ? known->value : String();
	}
	const bool fresh = who.is_empty();
	if (fresh) {
		who = "Player" + itos(next_player++);
	}
	p_accepted["player"] = who;
	p_accepted["state_token"] = MVLocalID::random_hex(32);
	// The same player entered from another device. Tell the old device why and stop it, drop its identity line,
	// then cut its state traffic too. In reverse order the old device sees the state traffic cut as a disconnect and re-enters,
	// then drops the new device in turn, and they keep dropping each other
	const HashMap<String, String>::ConstIterator prior = who_route.find(who);
	if (prior && prior->value != p_route && tls != nullptr) {
		const String old = prior->value;
		deny(old, U"Signed in from another device", false, true);
		vault.session_revoke(old);
		tls->drop_route(old, this);
		if (secret_only) {
			p_accepted["retake"] = true; // The host cuts the old device's state traffic just before admitting
		} else if (MVLink *link = state_link()) {
			link->revoke(who);
		}
	}
	// Another login of the same player is waiting for a room's reply. Both cannot pass, so the later one waits
	for (const KeyValue<String, Route> &one : routes) {
		if (one.key != p_route && String(Dictionary(one.value.holding).get("player", String())) == who) {
			deny_later(p_route, U"Another login of this account is in progress");
			return;
		}
	}
	// If the host holds the world, the host also decides admission.
	// Only name and identity are decided here; the rest waits for the room's reply
	if (secret_only) {
		// A player already in a room (re-entering from another device, or returning while awaiting reconnect) goes to the same room.
		// The world keeps the same my. Sending them elsewhere would leave their objects in the previous room
		const HashMap<String, String>::ConstIterator staying = who_room.find(who);
		const String room = staying && rooms.has(staying->value) ? staying->value : pick_room(route->value.want);
		if (room.is_empty() || !allow_in(p_route, room, p_accepted)) {
			// Matchmaking decides the room to enter. If it is gone, ask again.
			// Without telling, nobody creates a new room after the host leaves
			deny(p_route, U"No room available", true);
			return;
		}
		route->value.holding = p_accepted;
		route->value.held_at = Time::get_singleton()->get_ticks_msec();
		return;
	}
	MVRuntime *rt = runtime_node();
	// A full room is not about the author's script body. Blaming login handling would cast doubt on a correct body
	if (rt == nullptr) {
		deny(p_route, U"Login was not accepted");
		return;
	}
	if (rt->room_full_for(who)) {
		// Full is a wait, not a denial. Have the player ask again after a pause so they get in once a seat frees
		deny_later(p_route, U"The room is full", FULL_RETRY_MS);
		return;
	}
	// Reads the owner's saved values before admitting. Not admitted while unreadable
	load_saves(id, callable_mp(this, &MVAuthority::user_saved).bind(p_route, p_accepted));
}

// Reads the owner's saved values. A world without a store answers empty at once
void MVAuthority::load_saves(const String &p_id, const Callable &p_done) {
	if (store.is_wanted()) {
		store.load_user(p_id, p_done);
	} else {
		p_done.call(Dictionary());
	}
}

// The owner's saved values returned. Puts them into the world and returns the destination
void MVAuthority::user_saved(const Variant &p_saved, const String &p_route, const Dictionary &p_accepted) {
	if (!routes.has(p_route)) {
		return; // Disconnected before the reply
	}
	MVRuntime *rt = runtime_node();
	if (p_saved.get_type() != Variant::DICTIONARY || rt == nullptr) {
		deny_later(p_route, MVVault::NOT_REACHABLE);
		return;
	}
	const String who = p_accepted.get("player", String());
	if (!rt->join_context(who, p_accepted.get("my", Dictionary()), true, p_saved)) {
		deny(p_route, U"Login was not accepted");
		return;
	}
	settle(p_route, p_accepted);
}

// Records the Player put into the world and returns the state traffic destination and token to the owner
void MVAuthority::settle(const String &p_route, Dictionary p_accepted) {
	const String who = p_accepted.get("player", String());
	const Dictionary context = p_accepted.get("my", Dictionary());
	const String session = context.get("session", String());
	const String id = context.get("id", String());
	HashMap<String, Route>::Iterator route = routes.find(p_route);
	if (!route) {
		return;
	}
	route->value.who = who;
	route->value.busy = false;
	who_route[who] = p_route;
	// The same player's previous session is not needed. Keeping it would grow on every re-entry
	Vector<String> stale;
	for (const KeyValue<String, String> &entry : sessions) {
		if (entry.value == who && entry.key != session) {
			stale.push_back(entry.key);
		}
	}
	for (const String &old_session : stale) {
		sessions.erase(old_session);
	}
	sessions[session] = who;
	ids[id] = who;
	expires.erase(who);
	if (MVLink *link = state_link()) {
		link->allow(who, p_accepted.get("state_token", String()));
	}
	send(p_route, MVFrame::AUTH, p_accepted);
}

// Picks the room to enter. A player naming a room they created enters it.
// Otherwise this side picks a room with space
String MVAuthority::pick_room(const String &p_want) const {
	const HashMap<String, Room>::ConstIterator wanted = p_want.is_empty() ? rooms.end() : rooms.find(p_want);
	if (wanted && wanted->value.port > 0 && room_players(p_want) < wanted->value.max) {
		return p_want;
	}
	// To keep players from scattering, pick the fullest room that still has space
	String best;
	int most = -1;
	for (const KeyValue<String, Room> &one : rooms) {
		const int here = room_players(one.key);
		if (one.value.port > 0 && here < one.value.max && here > most) {
			most = here;
			best = one.key;
		}
	}
	return best;
}

// Counts players in that room. Only this side knows who is in which room,
// so host reports are not relied on. Players awaiting reconnect keep their seats
int MVAuthority::room_players(const String &p_room) const {
	int count = 0;
	for (const KeyValue<String, String> &one : who_room) {
		if (one.value == p_room) {
			count++;
		}
	}
	return count;
}

// Asks the room's host whether this player may enter
bool MVAuthority::allow_in(const String &p_route, const String &p_room, const Dictionary &p_accepted) {
	if (!rooms.has(p_room)) {
		return false;
	}
	who_room[String(p_accepted.get("player", String()))] = p_room;
	const String id = Dictionary(p_accepted.get("my", Dictionary())).get("id", String());
	Grant grant;
	grant.room = p_room;
	grants[id] = grant;
	// Passes this player's @online_save values too. The host has no Redis. Asks once they return
	load_saves(id, callable_mp(this, &MVAuthority::user_saved_for).bind(p_route, p_room, p_accepted));
	return true;
}

void MVAuthority::user_saved_for(const Variant &p_saved, const String &p_route, const String &p_room, const Dictionary &p_accepted) {
	const String who = p_accepted.get("player", String());
	const HashMap<String, Room>::ConstIterator found = rooms.find(p_room);
	const HashMap<String, String>::ConstIterator asked = who_room.find(who);
	// The room closed before the reply, or the player disconnected and freed the seat. Passing it to the host would leave an unknown identity in the world
	if (!found || !asked || asked->value != p_room || !routes.has(p_route)) {
		return;
	}
	if (p_saved.get_type() != Variant::DICTIONARY) {
		leave_room(who);
		routes[p_route].holding = Dictionary();
		deny_later(p_route, MVVault::NOT_REACHABLE);
		return;
	}
	Dictionary body;
	body["who"] = p_accepted.get("player", String());
	body["token"] = p_accepted.get("state_token", String());
	body["context"] = p_accepted.get("my", Dictionary());
	body["save"] = p_saved;
	body["retake"] = bool(p_accepted.get("retake", false));
	send(found->value.route, MVFrame::HOST_ALLOW, body);
}

// Passes a command from state traffic to the world and returns the answer to the requester.
// The sender is fixed by the DTLS binding, so claims are ignored
void MVAuthority::state_asked(const String &p_who, int64_t p_id, int64_t p_uid, const String &p_fn, const Array &p_args) {
	MVRuntime *rt = runtime_node();
	MVLink *link = state_link();
	if (rt == nullptr || link == nullptr) {
		return;
	}
	rt->ask_player(p_who, p_uid, p_fn, p_args, link->answer_to(p_who, p_id));
}

// Receives a host's room announcement and returns the entry list and world saved values
void MVAuthority::host_hello(const String &p_route, const Dictionary &p_body) {
	MVRuntime *rt = runtime_node();
	if (rt == nullptr) {
		return;
	}
	// Hosts are never trusted. So no key is needed, and none is handed to the Client side.
	// Identity, saves and Secret contents never go past here
	const String id = p_body.get("room", String());
	HashMap<String, Route>::Iterator route = routes.find(p_route);
	if (!MVGate::safe_name(id)) {
		deny(p_route, U"Invalid room announcement");
		return;
	}
	// One connection holds one room. Only this shape lets everything be closed on disconnect
	if (!route->value.room.is_empty() && route->value.room != id) {
		deny(p_route, U"One room per connection");
		return;
	}
	// Two announcements. The first registers the name and receives the entry list,
	// the second, once the world starts and the state traffic port is set, opens the room.
	// The first needs the grant matchmaking gave to the device told to hold a room
	if (route->value.room.is_empty() && !valid_grant(p_body.get("grant", String()))) {
		deny(p_route, U"Hosting needs a grant from the matchmaking server");
		return;
	}
	const HashMap<String, Room>::Iterator known = rooms.find(id);
	if (known && known->value.route != p_route) {
		deny(p_route, U"That room name is already in use");
		return;
	}
	if (known) {
		// Second one. Opens the room at the reported port. The player limit comes from the author's Secret room_max; host claims are not trusted
		const int port = int(p_body.get("port", 0));
		const int max = CLAMP(int(p_body.get("max", 0)), 0, Secret::room_max_of(rt->server_node()));
		if (port < 1 || port > 65535 || max < 1 || known->value.host.is_empty()) {
			deny(p_route, U"Invalid room announcement");
			return;
		}
		known->value.port = port;
		known->value.max = max;
		return;
	}
	// First one. The room's state traffic key and certificate are made here and handed to the host. The host makes no key.
	// P-256 is created instantly (RSA takes seconds). Clients receive this certificate in the identity reply and verify it
	Ref<Crypto> crypto = Crypto::create();
	Ref<CryptoKey> key = crypto.is_valid() ? crypto->generate_ec() : Ref<CryptoKey>();
	Room room;
	room.route = p_route;
	room.host = tls != nullptr ? tls->where_from(p_route) : String();
	room.name = "room-" + id;
	Ref<X509Certificate> cert = key.is_valid() ? crypto->generate_self_signed_certificate(key, "CN=" + room.name + ",O=Godot Online", "20260101000000", "20360101000000") : Ref<X509Certificate>();
	room.cert = cert.is_valid() ? cert->save_to_string() : String();
	if (room.host.is_empty() || room.cert.is_empty()) {
		deny(p_route, U"Cannot create the room certificate");
		return;
	}
	route->value.room = id;
	rooms[id] = room;
	// Returns the entry list and key. World saved values are not passed. Each room has its own world, so ownerless objects are not kept
	Dictionary body;
	body["doors"] = rt->secret_doors();
	body["saving"] = store.is_open();
	body["key"] = key->save_to_string();
	body["cert"] = room.cert;
	body["name"] = room.name;
	send(p_route, MVFrame::HOST_READY, body);
}

// Decides from the host's reply whether to admit or deny the waiting Client
void MVAuthority::host_joined(const String &p_route, const Dictionary &p_body) {
	const HashMap<String, Route>::ConstIterator teller = routes.find(p_route);
	const String who = p_body.get("who", String());
	const HashMap<String, String>::ConstIterator asked = who_room.find(who);
	// Replies are accepted only from the room the player was sent to.
	// If it claims to have admitted someone not sent there, make that room remove them. Otherwise an unknown identity stays in the world
	if (!teller) {
		return;
	}
	if (!asked || asked->value != teller->value.room) {
		if (bool(p_body.get("ok", false)) && MVGate::safe_name(who)) {
			Dictionary part;
			part["who"] = who;
			part["gone"] = true;
			send(p_route, MVFrame::HOST_PART, part);
		}
		return;
	}
	const HashMap<String, Room>::Iterator room = rooms.find(teller->value.room);
	String waiting;
	for (const KeyValue<String, Route> &one : routes) {
		if (String(Dictionary(one.value.holding).get("player", String())) == who) {
			waiting = one.key;
			break;
		}
	}
	HashMap<String, Route>::Iterator route = waiting.is_empty() ? routes.end() : routes.find(waiting);
	if (!route || !room) {
		return;
	}
	const Dictionary accepted = route->value.holding;
	route->value.holding = Dictionary();
	if (!bool(p_body.get("ok", false))) {
		leave_room(who);
		// A denial like a full room, where another room would admit, makes the player ask for a destination again
		deny(waiting, p_body.get("reason", U"Could not enter the room"), bool(p_body.get("again", false)));
		room_refused(teller->value.room);
		return;
	}
	room->value.refused = 0;
	// The state traffic destination is the host. The certificate is passed here too,
	// so neither authors nor players write any host settings
	Dictionary out = accepted;
	out.erase("retake");
	out["state_host"] = room->value.host;
	out["state_port"] = room->value.port;
	out["state_name"] = room->value.name;
	out["state_cert"] = room->value.cert;
	settle(waiting, out);
}

// Stores @online_save values sent by a host in Redis
void MVAuthority::host_save(const String &p_route, const Dictionary &p_body) {
	MVRuntime *rt = runtime_node();
	if (!store.is_open() || rt == nullptr) {
		return;
	}
	HashMap<String, Route>::Iterator route = routes.find(p_route);
	if (!route) {
		return;
	}
	String why;
	// A host may store only saved values of accounts in its room. World saved values (ownerless objects and
	// Secret contents) share the same box, so hosts may not write them
	if (String(p_body.get("op", String())) != "user") {
		refuse_host(p_route, U"the world save. Hosts keep no world save");
		return;
	}
	if (!may_save(route->value, p_body, why)) {
		refuse_host(p_route, why);
		return;
	}
	store.save_user(p_body.get("id", String()), p_body.get("kind", String()), p_body.get("values", Dictionary()));
}

// Only the host of the room the owner is in may write an account's saved values, and only that type's @online_save fields.
// A host freely writing values of players in its room cannot be prevented anyway, since it runs the world.
// What is narrowed here is writes to unknown accounts and unknown fields
bool MVAuthority::may_save(const Route &p_route, const Dictionary &p_body, String &r_why) const {
	const String id = p_body.get("id", String());
	const HashMap<String, Grant>::ConstIterator grant = grants.find(id);
	if (!grant || grant->value.room != p_route.room ||
			(grant->value.until != 0 && Time::get_singleton()->get_ticks_msec() >= grant->value.until)) {
		r_why = U"the save values of an account that is not in the room";
		return false;
	}
	const String kind = p_body.get("kind", String());
	// An identity-only process does not spawn, so the type is looked up by name
	if (MVGate::safe_name(kind) && MVClientScript::source_of(kind).is_empty()) {
		(void)MVClientScript::ensure_kind(kind);
	}
	const String source = MVGate::safe_name(kind) ? MVClientScript::source_of(kind) : String();
	if (source.is_empty()) {
		r_why = vformat(U"the save values of an unknown kind %s", kind);
		return false;
	}
	const Dictionary fields = GDScriptOnline::fields_of(source);
	for (const Variant &field : Dictionary(p_body.get("values", Dictionary())).get_key_list()) {
		if (!(int(fields.get(field, 0)) & GDScriptOnline::F_SAVE)) {
			r_why = vformat(U"%s, which is not an @online_save value of %s", String(field), kind);
			return false;
		}
	}
	return true;
}

// Calls a Secret body function requested by a host on this side's instance and returns the result
void MVAuthority::host_call(const String &p_route, const Dictionary &p_body) {
	MVRuntime *rt = runtime_node();
	if (rt == nullptr) {
		return;
	}
	rt->run_secret(p_body.get("path", String()), p_body.get("fn", String()), p_body.get("args", Array()),
			callable_mp(this, &MVAuthority::secret_done).bind(p_route, int64_t(p_body.get("id", 0))));
}

void MVAuthority::secret_done(const Variant &p_value, const String &p_route, int64_t p_id) {
	Dictionary body;
	body["id"] = p_id;
	body["value"] = p_value;
	send(p_route, MVFrame::HOST_DONE, body);
}

// When a host disconnects, closes its room and cuts that room's players on the identity side too
void MVAuthority::host_gone(const String &p_route) {
	String id;
	for (const KeyValue<String, Room> &one : rooms) {
		if (one.value.route == p_route) {
			id = one.key;
			break;
		}
	}
	if (id.is_empty()) {
		return;
	}
	rooms.erase(id);
	Vector<String> lost;
	for (const KeyValue<String, String> &one : who_room) {
		if (one.value == id) {
			lost.push_back(one.key);
		}
	}
	for (const String &who : lost) {
		leave_room(who);
		// Tell both current and entering players right away that the room is gone.
		// If they only notice when the identity line is cut, they ask again only after reconnecting and retrying.
		// Once told, they ask matchmaking on the next frame and become the next host if no room is free
		for (KeyValue<String, Route> &one : routes) {
			const bool inside = one.value.who == who;
			const bool entering = String(Dictionary(one.value.holding).get("player", String())) == who;
			if (inside || entering) {
				one.value.holding = Dictionary();
				deny(one.key, U"The room closed", true);
			}
		}
	}
}

// Limits commands per second so one connection cannot monopolize Online side processing
bool MVAuthority::spare(Route &r_route) {
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (now - r_route.rate_at >= MVLink::RATE_MS) {
		r_route.rate_at = now;
		r_route.rate = 0;
	}
	return ++r_route.rate <= MVLink::RATE_MAX;
}

bool MVAuthority::spare_host(Route &r_route) {
	MVRuntime *rt = runtime_node();
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (now - r_route.rate_at >= MVLink::RATE_MS) {
		r_route.rate_at = now;
		r_route.rate = 0;
	}
	return ++r_route.rate <= MVLink::RATE_MAX * (rt != nullptr ? Secret::room_max_of(rt->server_node()) + 1 : 1);
}

// Grant given only to the device matchmaking told to hold a room. Formatted "<expiry>.<random>.<signature>",
// the signature being an HMAC with the shared key. Blocks unchosen devices from announcing rooms, gathering players and refusing them
bool MVAuthority::valid_grant(const String &p_grant) {
	const PackedStringArray parts = p_grant.split(".");
	if (match_key.is_empty() || parts.size() != 3 || parts[2].length() != 64 || used_grants.has(p_grant)) {
		return false;
	}
	if (parts[0].to_int() <= int64_t(OS::get_singleton()->get_unix_time())) {
		return false;
	}
	Ref<Crypto> crypto = Crypto::create();
	if (crypto.is_null()) {
		return false;
	}
	const PackedByteArray digest = crypto->hmac_digest(HashingContext::HASH_SHA256, match_key.to_utf8_buffer(), (parts[0] + "." + parts[1]).to_utf8_buffer());
	const String want = String::hex_encode_buffer(digest.ptr(), digest.size());
	// Compare every character. Stopping early leaks the matching length through timing
	int differ = 0;
	for (int i = 0; i < 64; i++) {
		differ |= int(want[i]) ^ int(parts[2][i]);
	}
	if (differ != 0) {
		return false;
	}
	used_grants[p_grant] = parts[0].to_int();
	return true;
}

// Reports a host request that cannot pass. A modified host may send it endlessly, so once per reason
void MVAuthority::refuse_host(const String &p_route, const String &p_why) {
	HashMap<String, Route>::Iterator route = routes.find(p_route);
	if (route && !route->value.told_bad.has(p_why)) {
		route->value.told_bad.insert(p_why);
		WARN_PRINT(vformat(U"Online: room %s tried to write %s. Refused", route->value.room, p_why));
	}
}

// A room refused a player. Stop sending players to a room that keeps refusing. Once closed, the next player holds a new room
void MVAuthority::room_refused(const String &p_room) {
	HashMap<String, Room>::Iterator room = rooms.find(p_room);
	if (!room) {
		return;
	}
	room->value.refused++;
	if (room->value.refused < REFUSE_MAX || tls == nullptr) {
		return;
	}
	WARN_PRINT(vformat(U"Online: room %s refused %d players in a row. Closed", p_room, room->value.refused));
	tls->drop_route(room->value.route, this);
}

// Rooms with players, plus one room about to open. Rooms that were only announced and have no players are not counted.
// Counting them would let announce-only connections keep raising the entry limit
int MVAuthority::rooms_open() const {
	if (!secret_only) {
		return 1;
	}
	int open = 1;
	for (const KeyValue<String, Room> &one : rooms) {
		if (one.value.port > 0 && room_players(one.key) > 0) {
			open++;
		}
	}
	return open;
}

// Queues one frame for TLS send
bool MVAuthority::send(const String &p_route, int p_type, const Dictionary &p_body) {
	if (tls == nullptr) {
		return false;
	}
	const PackedByteArray data = MVFrame::pack(p_type, p_body).to_utf8_buffer();
	return tls->send(p_route, data);
}

// Sends the full current state only after DTLS is bound to the Player
void MVAuthority::state_joined(const String &p_who) {
	MVLink *link = state_link();
	MVRuntime *rt = runtime_node();
	if (link != nullptr && rt != nullptr) {
		link->push(p_who, rt->snapshot(p_who));
	}
}

// Sweeps records past heartbeat, unauthenticated and reconnect deadlines
void MVAuthority::sweep() {
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	// All deadlines checked are in seconds. No need to scan every connection each draw frame
	if (swept_at != 0 && now - swept_at < SWEEP_MS) {
		return;
	}
	swept_at = now;
	Vector<String> drop;
	Vector<String> waited;
	for (const KeyValue<String, Route> &entry : routes) {
		// Do not abandon players whose room never replies. Kept waiting,
		// the Client silently stalls waiting for the login reply
		if (!entry.value.holding.is_empty() && now - entry.value.held_at >= HOLD_MS) {
			waited.push_back(entry.key);
		}
		const bool nameless = entry.value.who.is_empty() && entry.value.room.is_empty();
		if (now - entry.value.heard >= MVFrame::DEAD_MS || (nameless && now - entry.value.made >= AUTH_MS)) {
			drop.push_back(entry.key);
			continue;
		}
		// Do not erase the context of a live connection. The deadline is the grace after disconnection
		vault.session_touch(entry.key);
	}
	for (const String &one : waited) {
		HashMap<String, Route>::Iterator route = routes.find(one);
		if (!route) {
			continue;
		}
		const String who = Dictionary(route->value.holding).get("player", String());
		const HashMap<String, String>::ConstIterator slow = who_room.find(who);
		const String slow_room = slow ? slow->value : String();
		tell_room(who, true);
		route->value.holding = Dictionary();
		deny(one, U"The room did not answer", true);
		room_refused(slow_room);
	}
	for (const String &route : drop) {
		if (tls != nullptr) {
			tls->drop_route(route, this);
		}
	}
	Vector<String> gone;
	for (const KeyValue<String, uint64_t> &entry : expires) {
		if (now >= entry.value) {
			gone.push_back(entry.key);
		}
	}
	for (const String &who : gone) {
		forget_player(who);
	}
	Vector<String> closed;
	for (const KeyValue<String, Grant> &entry : grants) {
		if (entry.value.until != 0 && now >= entry.value.until) {
			closed.push_back(entry.key);
		}
	}
	for (const String &id : closed) {
		grants.erase(id);
	}
	Vector<String> spent;
	const int64_t unix_now = int64_t(OS::get_singleton()->get_unix_time());
	for (const KeyValue<String, int64_t> &entry : used_grants) {
		if (entry.value <= unix_now) {
			spent.push_back(entry.key);
		}
	}
	for (const String &one : spent) {
		used_grants.erase(one);
	}
	Vector<String> quiet;
	for (KeyValue<String, Vector<uint64_t>> &entry : fresh_from) {
		while (!entry.value.is_empty() && now - entry.value[0] >= FRESH_MS) {
			entry.value.remove_at(0);
		}
		if (entry.value.is_empty()) {
			quiet.push_back(entry.key);
		}
	}
	for (const String &from : quiet) {
		fresh_from.erase(from);
	}
	vault.session_sweep();
}

// Erases all records of this player and removes them from the world
void MVAuthority::forget_player(const String &p_who) {
	if (secret_only) {
		tell_room(p_who, true);
	} else if (MVRuntime *rt = runtime_node()) {
		rt->join_context(p_who, Dictionary(), false);
	}
	expires.erase(p_who);
	String old_session;
	for (const KeyValue<String, String> &entry : sessions) {
		if (entry.value == p_who) {
			old_session = entry.key;
			break;
		}
	}
	if (!old_session.is_empty()) {
		sessions.erase(old_session);
	}
	// Clean up the product ID table too. Left alone it grows with every entry and exit
	String old_id;
	for (const KeyValue<String, String> &entry : ids) {
		if (entry.value == p_who) {
			old_id = entry.key;
			break;
		}
	}
	if (!old_id.is_empty()) {
		ids.erase(old_id);
	}
	who_route.erase(p_who);
}

// Unbinds this device from its account, then stops it. The next launch becomes a new guest
void MVAuthority::logout(const String &p_route) {
	vault.session_unbind(p_route, callable_mp(this, &MVAuthority::logged_out).bind(p_route));
}

void MVAuthority::logged_out(const Variant &p_done, const String &p_route) {
	const HashMap<String, Route>::ConstIterator route = routes.find(p_route);
	if (!route) {
		return;
	}
	if (p_done.get_type() != Variant::BOOL) {
		deny_later(p_route, MVVault::NOT_REACHABLE);
		return;
	}
	evict(route->value.who, U"Signed out");
}

// Drops an older device of the same account. Tells it why and stops it, revokes its reconnect ticket and removes it from the world at once
void MVAuthority::evict(const String &p_who, const String &p_reason) {
	const HashMap<String, String>::ConstIterator found = who_route.find(p_who);
	if (found && tls != nullptr) {
		const String old = found->value;
		deny(old, p_reason, false, true);
		vault.session_revoke(old);
		tls->drop_route(old, this);
	}
	forget_player(p_who);
}

// Reports availability to the matchmaking server. Neither authors nor players choose a server.
// The Secret's room_max is the room's player limit as is
void MVAuthority::announce() {
	if (match_url.is_empty()) {
		return;
	}
	int free_rooms = 0;
	for (const KeyValue<String, Room> &one : rooms) {
		if (one.value.port > 0 && room_players(one.key) < one.value.max) {
			free_rooms++;
		}
	}
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	// Report without waiting for the interval when the free count changes. Otherwise players arriving meanwhile
	// create their own room even though a free one exists
	const bool changed = secret_only && free_rooms != told_free;
	if (told_at != 0 && !changed && now - told_at < ANNOUNCE_MS) {
		return;
	}
	HTTPRequest *post = Object::cast_to<HTTPRequest>(ObjectDB::get_instance(teller));
	MVRuntime *rt = runtime_node();
	if (post == nullptr || rt == nullptr) {
		return;
	}
	if (post->get_http_client_status() != HTTPClient::STATUS_DISCONNECTED) {
		// The previous report has not finished. If stuck for long, give up and resend next time.
		// Without giving up, one stall would stop all later reports
		if (now - told_at >= ANNOUNCE_MS * 2) {
			post->cancel_request();
		}
		return;
	}
	told_at = now;
	Dictionary body;
	// When the host holds the world, this side is the identity entry. Announces that it is not a room
	if (secret_only) {
		told_free = free_rooms;
		body["secret"] = true;
		body["host"] = room_host;
		body["port"] = room_port;
		body["free"] = free_rooms;
		post->request(match_url, headers_for_match(), HTTPClient::METHOD_POST, JSON::stringify(body));
		return;
	}
	body["room"] = room_id;
	body["host"] = room_host;
	body["port"] = room_port;
	// Report players actually in the world. Sending the TLS connection count would exceed max
	// through pre-login connections and overlapping reconnects, and the matchmaking server
	// would reject it with 400, silently dropping the room from the list
	body["players"] = MIN(rt->player_count(), Secret::room_max_of(rt->server_node()));
	body["max"] = Secret::room_max_of(rt->server_node());
	post->request(match_url, headers_for_match(), HTTPClient::METHOD_POST, JSON::stringify(body));
}

// Builds headers with the shared key for matchmaking reports
PackedStringArray MVAuthority::headers_for_match() const {
	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");
	if (!match_key.is_empty()) {
		headers.push_back("X-Room-Key: " + match_key);
	}
	return headers;
}

// Generates a key and starts TLS, DTLS and the Online side world
String MVAuthority::start(bool p_secret_only) {
	secret_only = p_secret_only;
	if (started) {
		return U"Online Server is already running";
	}
	// Settings live only in the Secret inspector. They are exported with the scene,
	// so the build connects to the same place even without a settings file
	const Dictionary wrote = Secret::config_of_project();
	config = wrote;
	const int listen = int(wrote.get("listen-port", 0));
	port = listen > 0 ? listen : int(wrote.get("room-port", (int)Secret::PORT_DEFAULT));
	bind = String(wrote.get("listen-host", "0.0.0.0"));
	// Settings for announcing to the matchmaking server. Without them, only direct connection
	match_url = wrote.get("match-url", "");
	match_key = wrote.get("match-key", "");
	room_id = wrote.get("match-room", "");
	room_host = wrote.get("room-host", "");
	room_port = int(wrote.get("room-port", port));
	if (room_id.is_empty()) {
		room_id = MVLocalID::random_hex(8);
	}
	if (!match_url.is_empty()) {
		HTTPRequest *post = memnew(HTTPRequest);
		post->set_name("Teller");
		add_child(post);
		teller = post->get_instance_id();
	}
	const String name = String(wrote.get("secret-name", Secret::NAME_DEFAULT));
	const String dir = "user://online_secret";
	const String key_path = dir.path_join("key.pem");
	cert_path = dir.path_join("certificate.pem");
	key = Ref<CryptoKey>(CryptoKey::create());
	cert = Ref<X509Certificate>(X509Certificate::create());
	if (key.is_null() || cert.is_null()) {
		return U"TLS is not available";
	}
	if (DirAccess::make_dir_recursive_absolute(dir) != OK || !MVLocalID::protect(dir, true)) {
		return U"Cannot protect the Secret directory";
	}
	const bool existing = FileAccess::exists(key_path) && FileAccess::exists(cert_path);
	if (existing && !MVLocalID::protect(key_path, false)) {
		return U"Cannot protect the Secret key permissions";
	}
	if (!existing || key->load(key_path) != OK || cert->load(cert_path) != OK) {
		Ref<Crypto> crypto = Crypto::create();
		key = crypto.is_valid() ? crypto->generate_ec() : Ref<CryptoKey>(); // P-256. Created instantly and keeps each connection's handshake light
		cert = key.is_valid() ? crypto->generate_self_signed_certificate(key, "CN=" + name + ",O=Godot Online", "20260101000000", "20360101000000") : Ref<X509Certificate>();
		if (key.is_null() || cert.is_null() || key->save(key_path) != OK || !MVLocalID::protect(key_path, false) || cert->save(cert_path) != OK) {
			return U"Cannot generate the Secret key and certificate";
		}
	}
	MVRuntime *rt = memnew(MVRuntime);
	rt->set_name("Runtime");
	// When the host holds the world, this side runs only the Secret body
	rt->set_secret_only(secret_only);
	add_child(rt);
	// Identity and saves do not belong to the Online side world. They are held on this trusted side,
	// and the world only gets access to the store
	vault.set_store(&store);
	vault.set_connect_key(String(wrote.get("connect-key", String())).strip_edges());
	// Bind the store even if it cannot connect. The world waits until readable and stores once connected
	store.open();
	if (store.is_wanted()) {
		rt->set_saves(&store);
	}
	runtime = rt->get_instance_id();
	// Run before the listener so store replies progress
	set_process(true);
	// The world starts once saved values return. The listener opens after that
	return rt->start(true, callable_mp(this, &MVAuthority::serve));
}

// The world started running. Opens the listener and starts accepting Clients
void MVAuthority::serve() {
	MVRuntime *rt = runtime_node();
	if (rt == nullptr) {
		return;
	}
	// The only place to write a room's player count is the Secret's room_max.
	// Both TLS and DTLS size their entries from this number
	const int room_max = Secret::room_max_of(rt->server_node());
	// The Secret also decides how long a disconnected player's Player is kept. Readable once the world exists.
	// Clients do not read it, so ONLINE_KEEP_SEC can also pass it. Passed values are in config
	const int wrote_keep = int(config.get("keep-sec", 0));
	vault.set_keep_sec(wrote_keep > 0 ? MIN(wrote_keep, (int)Secret::KEEP_LIMIT) : Secret::keep_sec_of(rt->server_node()));
	String reason;
	// Only the side holding the world has state traffic. Not opened when handling identity only
	if (!secret_only) {
		MVLink *link = memnew(MVLink);
		link->set_name("State");
		add_child(link);
		reason = link->open(port, key, cert, bind, room_max);
		state = link->get_instance_id();
		link->connect(SNAME("__joined"), callable_mp(this, &MVAuthority::state_joined));
		link->connect(SNAME("__asked"), callable_mp(this, &MVAuthority::state_asked));
	}
	if (reason.is_empty()) {
		tls = memnew(MVTLSServer);
		reason = tls->open(port, bind, key, cert, room_max);
	}
	if (!reason.is_empty()) {
		ERR_PRINT(String(U"Online Server: ") + reason);
		stop();
		return;
	}
	started = true;
	// No rendering means no vsync, and without a cap it saturates one CPU. The world ticks at 20Hz, so 60 is enough
	if (Engine::get_singleton()->get_max_fps() == 0) {
		Engine::get_singleton()->set_max_fps(60);
	}
	print_line(vformat(secret_only ? U"Godot Online Secret: %s:%d" : U"Godot Online Server: %s:%d", bind, port));
	if (!secret_only) {
		// The world clock is the physics tick. Align the engine tick to 20Hz and advance the world on it
		MVThing::world_clock(true);
		set_physics_process(true);
	}
	print_line(vformat(U"CA certificate for clients: %s", ProjectSettings::get_singleton()->globalize_path(cert_path)));
}

void MVAuthority::stop() {
	set_process(false);
	if (MVRuntime *rt = runtime_node()) {
		rt->close_world();
	}
	if (tls != nullptr) {
		tls->close(this);
		memdelete(tls);
		tls = nullptr;
	}
	if (MVLink *link = state_link()) {
		link->close();
	}
	routes.clear();
	sessions.clear();
	ids.clear();
	who_route.clear();
	expires.clear();
	rooms.clear();
	who_room.clear();
	grants.clear();
	used_grants.clear();
	started = false;
}

// Advances TLS every draw frame and delivers Online side diffs only at 20Hz
void MVAuthority::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		// Store replies progress from before the listener. World saved values return here
		store.poll();
	}
	if (p_what == NOTIFICATION_PROCESS && started) {
		tls->poll(this);
		// Without a world, only Secret saves are advanced
		if (secret_only) {
			if (MVRuntime *rt = runtime_node()) {
				rt->tick_secret();
			}
			// When the store disconnects or returns, tell every room's host.
			// Otherwise saves made while disconnected are silently dropped here and never restored after it returns
			if (store.is_open() != told_store) {
				told_store = store.is_open();
				Dictionary body;
				body["on"] = told_store;
				for (const KeyValue<String, Route> &one : routes) {
					if (!one.value.room.is_empty()) {
						send(one.key, MVFrame::HOST_STORE, body);
					}
				}
			}
			sweep();
			announce();
			return;
		}
		sweep();
		announce();
	} else if (p_what == NOTIFICATION_PHYSICS_PROCESS && started && !secret_only) {
		// The world advances on the physics tick (20Hz). The author's move_and_slide() runs on the same tick
		MVThing::drive_world(runtime, state);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		stop();
	}
}

// Used only from C++. Nothing is exposed to scripts
void MVAuthority::_bind_methods() {
}

#endif
