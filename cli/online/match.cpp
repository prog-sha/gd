/**************************************************************************/
/*  match.cpp                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Answer matchmaking requests on one endpoint whose path names the game.
// Rooms and the identity gateway announce themselves with a shared key; clients ask without naming a room.
// The answer is a destination only; game traffic never passes through this server.

#include "cli/online/match.h"

#include "cli/api/cli.h"
#include "cli/data/codec.h"
#include "cli/sys/clock.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "cli/sys/task.h"

namespace {

// Build a reply from raw bytes.
// An empty type sends no Content-Type, as for replies without a body.
Ref<GDWebResponse> reply(int p_status, const PackedByteArray &p_body, const String &p_type) {
	return Http::bytes_out(p_body, p_type, p_status);
}

// Reply with a destination.
Ref<GDWebResponse> place(const Variant &p_room, const Variant &p_host, int64_t p_port, const String &p_grant = String(), bool p_host_here = false) {
	return reply(200, OnlineWire::place(p_room, p_host, p_port, p_grant, p_host_here), "application/json");
}

// Log a directory failure; clients only see the status.
void report(const char *p_what, const VariantPair &p_result) {
	const Ref<Err> error = p_result.error;
	const String why = error.is_valid() ? error->text() : String("no result");
	ERR_PRINT(vformat("%s: %s", p_what, LogState::flat(why)));
}

} // namespace

// Run the first step; a reply ready before any wait is returned directly.
VariantPair GDOnlineCall::run(void (GDOnlineCall::*p_step)()) {
	self_hold = Ref<GDOnlineCall>(this);
	starting = true;
	(this->*p_step)();
	starting = false;
	return { self_hold.is_null() ? out : Variant(Signal(this, "finished")), Variant() };
}

// Continue with a result that is either ready or delivered later by a signal.
void GDOnlineCall::wait(const VariantPair &p_pending, void (GDOnlineCall::*p_next)(const VariantPair &)) {
	if (p_pending.value.get_type() != Variant::SIGNAL) {
		(this->*p_next)(p_pending);
		return;
	}
	next = p_next;
	Signal(p_pending.value).connect(callable_mp(this, &GDOnlineCall::resume), CONNECT_ONE_SHOT);
}

// Hand a deferred result to the step that asked for it.
void GDOnlineCall::resume(const Variant &p_value, const Variant &p_error) {
	void (GDOnlineCall::*step)(const VariantPair &) = next;
	next = nullptr;
	if (step) {
		(this->*step)(VariantPair{ p_value, p_error });
	}
}

// Deliver the reply once, directly or through the finished signal.
void GDOnlineCall::finish(const Variant &p_reply) {
	if (self_hold.is_null()) {
		return;
	}
	Ref<GDOnlineCall> keep(this);
	self_hold.unref();
	if (starting) {
		out = p_reply;
		return;
	}
	Async::finish(this, SNAME("finished"), p_reply, Variant());
}

// Read the body up to one byte past the limit, which tells a cut value from a complete one.
void GDOnlineCall::read() {
	wait(req->read(OnlineWire::BODY_MAX + 1 - body.size()), &GDOnlineCall::got_chunk);
}

// Collect one chunk; an empty chunk ends the body.
void GDOnlineCall::got_chunk(const VariantPair &p_chunk) {
	if (Ref<Err>(p_chunk.error).is_valid() || p_chunk.value.get_type() != Variant::PACKED_BYTE_ARRAY) {
		return finish(GDOnlineMatch::fail(400, "bad body"));
	}
	const PackedByteArray part = p_chunk.value;
	body.append_array(part);
	if (!part.is_empty() && body.size() <= OnlineWire::BODY_MAX) {
		return read();
	}
	route();
}

// Treat a body naming a room or the gateway as an announcement, anything else as a question.
void GDOnlineCall::route() {
	const bool more = body.size() > OnlineWire::BODY_MAX;
	if (more) {
		body.resize(OnlineWire::BODY_MAX);
	}
	OnlineRoom room;
	if (!OnlineWire::decode(body, more, room)) {
		return finish(GDOnlineMatch::fail(400, "bad body"));
	}
	if (room.id.is_empty() && !room.secret) {
		return match();
	}
	announce(room);
}

// Accept an announcement only with the shared key, so arbitrary paths cannot grow the directory.
void GDOnlineCall::announce(OnlineRoom p_room) {
	const String &key = owner->key;
	if (key.is_empty() || !Hash::equal_ct(req->header("X-Room-Key").to_utf8_buffer(), key.to_utf8_buffer())) {
		return finish(GDOnlineMatch::fail(403, "bad key"));
	}
	if (!OnlineWire::valid(p_room)) {
		return finish(GDOnlineMatch::fail(400, "bad room"));
	}
	// Default to the sender's address rather than an address from inside its network.
	if (p_room.host.is_empty()) {
		p_room.host = req->get_ip().to_utf8_buffer();
	}
	OnlineStore *store = owner->store;
	wait(p_room.secret ? store->serve(game, p_room) : store->announce(game, p_room), &GDOnlineCall::stored);
}

// Confirm the announcement without a body.
void GDOnlineCall::stored(const VariantPair &p_done) {
	if (!Ref<Err>(p_done.error).is_valid()) {
		return finish(reply(204, PackedByteArray(), ""));
	}
	report("announce", p_done);
	finish(GDOnlineMatch::fail(503, "store"));
}

// Ask for the identity gateway first; when one is known every client goes there.
void GDOnlineCall::match() {
	wait(owner->store->secret(game), &GDOnlineCall::got_secret);
}

// Send the client to the gateway, asking it to host when no room is free.
// A failed lookup falls back to picking a room directly.
void GDOnlineCall::got_secret(const VariantPair &p_found) {
	const Array gate = !Ref<Err>(p_found.error).is_valid() && p_found.value.get_type() == Variant::ARRAY ? Array(p_found.value) : Array();
	if (gate.size() != 3 || gate[0].get_type() == Variant::NIL) {
		return wait(owner->store->pick(game), &GDOnlineCall::got_room);
	}
	// Only the client chosen to host receives a grant the gateway will accept.
	const bool host_here = OnlineWire::number(gate[2]) < 1;
	const String grant = host_here ? OnlineWire::grant(owner->key, int64_t(GDClock::unix_time())) : String();
	finish(place(Variant(), gate[0], OnlineWire::number(gate[1]), grant, host_here));
}

// Send the client to the picked room, or report that none has a free seat.
void GDOnlineCall::got_room(const VariantPair &p_found) {
	const Ref<Err> error = p_found.error;
	const bool ok = error.is_null();
	if (!ok && String(error->get_info().get("code", "")) != "nil") {
		report("match", p_found);
		return finish(GDOnlineMatch::fail(503, "store"));
	}
	const Array room = ok && p_found.value.get_type() == Variant::ARRAY ? Array(p_found.value) : Array();
	if (room.size() != 3) {
		return finish(GDOnlineMatch::fail(503, "no room"));
	}
	finish(place(room[0], room[1], OnlineWire::number(room[2])));
}

// Stop waiting and release the request; a late result then finds no step to run.
void GDOnlineCall::cancel() {
	next = nullptr;
	req.unref();
	self_hold.unref();
}

// Expose completion and cancellation of a suspended request.
void GDOnlineCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDOnlineCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}

// Create an endpoint from options; unknown names and wrong types are refused.
VariantPair GDOnlineMatch::make(const Dictionary &p_opts) {
	for (const Variant &name : p_opts.keys()) {
		if (name.get_type() != Variant::STRING || !String("key prefix alive redis").split(" ").has(name)) {
			return { Variant(), Err::make(vformat("unknown match option: %s", name), Err::INVALID_DATA) };
		}
	}
	const Variant key = p_opts.get("key", "");
	const Variant prefix = p_opts.get("prefix", "match");
	const Variant alive = p_opts.get("alive", 5.0);
	const Variant db = p_opts.get("redis", Variant());
	if (key.get_type() != Variant::STRING || prefix.get_type() != Variant::STRING) {
		return { Variant(), Err::make("key and prefix must be text", Err::INVALID_DATA) };
	}
	// Any positive time up to the longest duration a signed 64-bit nanosecond count holds.
	const bool number = alive.get_type() == Variant::INT || alive.get_type() == Variant::FLOAT;
	const double sec = number ? double(alive) : 0.0;
	if (!(sec > 0.0 && sec <= 9223372036.854775)) {
		return { Variant(), Err::make("alive must be a positive number of seconds", Err::INVALID_DATA) };
	}
	const Ref<GDRedisClient> client = db;
	if (db.get_type() != Variant::NIL && client.is_null()) {
		return { Variant(), Err::make("redis must be a client from GD.database.redis.client()", Err::INVALID_DATA) };
	}
	Ref<GDOnlineMatch> out;
	out.instantiate();
	out->key = key;
	out->store = client.is_valid() ? static_cast<OnlineStore *>(memnew(OnlineRedis(client))) : memnew(OnlineMemory);
	out->store->setup(prefix, int64_t(sec * 1000.0));
	return { out, Variant() };
}

// Reply with plain text and a trailing newline.
Ref<GDWebResponse> GDOnlineMatch::fail(int p_status, const String &p_msg) {
	return Http::head(reply(p_status, (p_msg + "\n").to_utf8_buffer(), "text/plain; charset=utf-8"), "X-Content-Type-Options", "nosniff");
}

// Answer health checks, reject unusable paths and methods, and start everything else.
VariantPair GDOnlineMatch::handle(const Ref<GDWebRequest> &p_req) {
	if (p_req.is_null()) {
		return { fail(500, "no request"), Variant() };
	}
	const String path = p_req->get_path();
	if (path == "/healthz") {
		return { reply(200, String("ok").to_utf8_buffer(), "text/plain"), Variant() };
	}
	String game;
	if (!OnlineWire::game_of(path, game)) {
		return { fail(404, "bad game"), Variant() };
	}
	const String method = p_req->get_method();
	if (method != "GET" && method != "POST") {
		return { fail(405, "post only"), Variant() };
	}
	Ref<GDOnlineCall> call;
	call.instantiate();
	call->owner = Ref<GDOnlineMatch>(this);
	call->req = p_req;
	call->game = game;
	return call->run(method == "GET" ? &GDOnlineCall::match : &GDOnlineCall::read);
}

// Serve every path from a web application owned by this endpoint.
VariantPair GDOnlineMatch::listen(int64_t p_port, const String &p_host) {
	if (app.is_null()) {
		app.instantiate();
		app->otherwise(callable_mp(this, &GDOnlineMatch::handle));
	}
	return app->listen(p_port, p_host);
}

// Stop the server started by listen.
void GDOnlineMatch::stop() {
	if (app.is_valid()) {
		app->stop();
	}
}

// Release the directory.
GDOnlineMatch::~GDOnlineMatch() {
	if (store) {
		memdelete(store);
	}
}

// Expose the endpoint to script.
void GDOnlineMatch::_bind_methods() {
	ClassDB::bind_method(D_METHOD("handle", "req"), &GDOnlineMatch::handle);
	ClassDB::bind_method(D_METHOD("listen", "port", "host"), &GDOnlineMatch::listen, DEFVAL("127.0.0.1"));
	ClassDB::bind_method(D_METHOD("port"), &GDOnlineMatch::port);
	ClassDB::bind_method(D_METHOD("stop"), &GDOnlineMatch::stop);
	ADD_PAIR_RESULT("handle", "GDWebResponse");
	ADD_PAIR_RESULT("listen", "Variant");
}

// Expose online-game services to script.
void GDOnlineAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("match", "opts"), &GDOnlineAPI::match, DEFVAL(Dictionary()));
	ADD_PAIR_RESULT("match", "GDOnlineMatch");
}
