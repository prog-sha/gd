/**************************************************************************/
/*  mv_store.cpp                                                          */
/**************************************************************************/

// Stores @online_save values in Redis over RESP and restores them at login and world startup. Never blocks

#include "mv_store.h"

#include "mv_secret.h"

#ifdef MV_SECRET_ENABLED

#include "mv_frame.h"
#include "mv_gate.h"

#include "core/io/ip.h"
#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"

// Converts text into the raw bytes of one RESP argument
static PackedByteArray word_of(const String &p_text) {
	return p_text.to_utf8_buffer();
}

// Encodes a value in binary form. The binary form keeps the type; Vector3 and Color are not turned into text.
// Objects cannot be copied, so a value containing one is not written
static bool encode_value(const Variant &p_value, PackedByteArray &r_out) {
	if (!MVFrame::plain(p_value)) {
		return false;
	}
	int len = 0;
	if (encode_variant(p_value, nullptr, len, false) != OK || len <= 0) {
		return false;
	}
	r_out.resize(len);
	return encode_variant(p_value, r_out.ptrw(), len, false) == OK;
}

// Decodes stored binary back into a value. Objects are never created
static bool decode_value(const PackedByteArray &p_raw, Variant &r_out) {
	int len = 0;
	if (p_raw.is_empty() || decode_variant(r_out, p_raw.ptr(), p_raw.size(), &len, false) != OK) {
		return false;
	}
	return MVFrame::plain(r_out);
}

// Reads a redis setting written in the Secret's inspector
String MVStore::setting(const Dictionary &p_wrote, const String &p_name, const String &p_default) {
	const String wrote = String(p_wrote.get("redis-" + p_name, String())).strip_edges();
	return wrote.is_empty() ? p_default : wrote;
}

// Saves nothing without a host. Otherwise starts connecting; the connection completes inside poll
bool MVStore::open() {
	close();
	const Dictionary wrote = Secret::config_of_project();
	const String host = setting(wrote, "host", String());
	if (host.is_empty()) {
		wanted = false;
		return false;
	}
	// A world with a destination reconnects when dropped. A world without one never saves
	wanted = true;
	prefix = setting(wrote, "prefix", "online");
	if (!MVGate::safe_name(prefix)) {
		ERR_PRINT("Online: Secret's redis_prefix must be alphanumeric and _");
		return false;
	}
	address = host.is_valid_ip_address() ? IPAddress(host) : IP::get_singleton()->resolve_hostname(host);
	if (!address.is_valid()) {
		ERR_PRINT(vformat("Online: cannot resolve Redis host %s.", host));
		return false;
	}
	port = setting(wrote, "port", "6379").to_int();
	password = setting(wrote, "password", String());
	tcp.instantiate();
	connect_at = OS::get_singleton()->get_ticks_msec();
	if (tcp->connect_to_host(address, port) != OK) {
		tcp.unref();
		ERR_PRINT(vformat("Online: cannot reach Redis at %s.", host));
		return false;
	}
	return true;
}

// Waits for the connection, flushes queued commands once connected, and parses replies. Reconnects after a delay when dropped
void MVStore::poll() {
	if (tcp.is_null()) {
		if (!wanted) {
			return;
		}
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		if (retry_at != 0 && now < retry_at) {
			return;
		}
		retry_at = now + RETRY_MS;
		if (open()) {
			print_line(U"Online: reconnecting to Redis. @online_save is being saved again");
		}
		return;
	}
	tcp->poll();
	const StreamPeerTCP::Status status = tcp->get_status();
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (status == StreamPeerTCP::STATUS_CONNECTING) {
		// A peer that accepts but never answers is as good as disconnected. Waiting forever would stall world startup
		if (now - connect_at >= REPLY_MS) {
			fail("Redis did not accept the connection");
		}
		return;
	}
	if (status != StreamPeerTCP::STATUS_CONNECTED) {
		fail("lost the Redis connection (closed)");
		return;
	}
	if (asked_at != 0 && now - asked_at >= REPLY_MS) {
		fail("Redis did not answer");
		return;
	}
	if (!live) {
		live = true;
		tcp->set_no_delay(true);
		// The password goes first. It is placed before commands queued while connecting, and its receiver goes to the front too.
		// Replies return in send order, so a misalignment here would hand the password reply to a queued receiver
		// Sets the client name so Redis CLIENT LIST shows which world the link belongs to
		PackedByteArray first;
		if (!password.is_empty()) {
			first = encode({ word_of("AUTH"), word_of(password) });
			waiting.push_back(callable_mp(this, &MVStore::auth_got));
		}
		first.append_array(encode({ word_of("CLIENT"), word_of("SETNAME"), word_of(prefix) }));
		waiting.push_back(Callable());
		// Move the queued receivers behind these two commands
		List<Callable> queued;
		while (waiting.size() > (password.is_empty() ? 1 : 2)) {
			queued.push_back(waiting.front()->get());
			waiting.pop_front();
		}
		for (const Callable &done : queued) {
			waiting.push_back(done);
		}
		first.append_array(outbox);
		outbox = first;
		if (!outbox.is_empty()) {
			tcp->put_data(outbox.ptr(), outbox.size());
			outbox.clear();
		}
		if (asked_at == 0 && !waiting.is_empty()) {
			asked_at = now;
		}
	}
	if (world_done.is_valid() && !world_asked && now >= world_retry_at) {
		ask_world();
	}
	read();
}

void MVStore::close() {
	if (tcp.is_valid()) {
		tcp->disconnect_from_host();
	}
	tcp.unref();
	live = false;
	multi = false;
	inbox.clear();
	outbox.clear();
	asked_at = 0;
	world_asked = false;
	// Do not reconnect right after dropping, to avoid hammering a downed Redis
	retry_at = OS::get_singleton()->get_ticks_msec() + RETRY_MS;
	// Close pending answers as failures. Otherwise login or world startup would stall there
	while (!waiting.is_empty()) {
		const Callable done = waiting.front()->get();
		waiting.pop_front();
		if (done.is_valid()) {
			done.call(refused("the store is closed"));
		}
	}
}

Dictionary MVStore::refused(const String &p_why) {
	Dictionary out;
	out["error"] = p_why;
	return out;
}

bool MVStore::is_refused(const Variant &p_reply) {
	return p_reply.get_type() == Variant::DICTIONARY && Dictionary(p_reply).has("error");
}

// Gives up saving. The world keeps running without saves
void MVStore::fail(const String &p_why) {
	if (live) {
		ERR_PRINT(vformat("Online: %s. @online_save values are no longer saved.", p_why));
	}
	close();
}

// Encodes one command as a RESP array
PackedByteArray MVStore::encode(const Vector<PackedByteArray> &p_words) {
	PackedByteArray out;
	out.append_array(word_of("*" + itos(p_words.size()) + "\r\n"));
	for (const PackedByteArray &word : p_words) {
		out.append_array(word_of("$" + itos(word.size()) + "\r\n"));
		out.append_array(word);
		out.append_array(word_of("\r\n"));
	}
	return out;
}

// Sends one command. Queues it until connected
void MVStore::send(const Vector<PackedByteArray> &p_words, const Callable &p_done) {
	if (tcp.is_null()) {
		if (p_done.is_valid()) {
			p_done.call(refused("the store is closed"));
		}
		return;
	}
	const PackedByteArray out = encode(p_words);
	waiting.push_back(p_done);
	if (!live) {
		outbox.append_array(out);
		return;
	}
	if (asked_at == 0) {
		asked_at = OS::get_singleton()->get_ticks_msec();
	}
	if (tcp->put_data(out.ptr(), out.size()) != OK) {
		fail("lost the Redis connection (write)");
	}
}

// Parses one reply at r_at in inbox. Returns 1 when parsed, 0 when more data is needed, -1 when malformed
int MVStore::parse(int64_t &r_at, Variant &r_out, int p_depth) {
	if (p_depth > NEST_MAX) {
		return -1;
	}
	const uint8_t *buf = inbox.ptr();
	const int64_t n = inbox.size();
	int64_t end = r_at;
	while (end + 1 < n && !(buf[end] == '\r' && buf[end + 1] == '\n')) {
		end++;
	}
	if (end + 1 >= n) {
		return 0;
	}
	const String head = String::utf8((const char *)buf + r_at, end - r_at);
	if (head.is_empty()) {
		return -1;
	}
	const String rest = head.substr(1);
	int64_t at = end + 2;
	switch (head[0]) {
		case '-':
			// Only this one command was refused. The connection is alive, so saving continues. Each reason is reported once
			if (rest != last_error) {
				last_error = rest;
				ERR_PRINT(vformat("Online: Redis said %s.", rest));
			}
			r_out = refused(rest);
			break;
		case '+':
		case ':':
			r_out = rest;
			break;
		case '$': {
			const int64_t size = rest.to_int();
			if (size < 0) {
				r_out = Variant();
				break;
			}
			if (at + size + 2 > n) {
				return 0;
			}
			PackedByteArray body;
			body.resize(size);
			memcpy(body.ptrw(), buf + at, size);
			r_out = body;
			at += size + 2;
		} break;
		case '*': {
			const int64_t count = rest.to_int();
			Array items;
			for (int64_t i = 0; i < count; i++) {
				Variant item;
				const int got = parse(at, item, p_depth + 1);
				if (got != 1) {
					return got;
				}
				items.push_back(item);
			}
			r_out = items;
		} break;
		default:
			return -1;
	}
	r_at = at;
	return 1;
}

// Reads arrived data and hands parsed replies out in order
void MVStore::read() {
	const int count = tcp->get_available_bytes();
	if (count > 0) {
		const int64_t was = inbox.size();
		inbox.resize(was + count);
		int got = 0;
		if (tcp->get_partial_data(inbox.ptrw() + was, count, got) != OK) {
			fail("lost the Redis connection (read)");
			return;
		}
		inbox.resize(was + got);
	}
	int64_t at = 0;
	while (at < inbox.size()) {
		Variant reply;
		const int got = parse(at, reply, 0);
		if (got < 0) {
			fail("Redis sent something unexpected");
			return;
		}
		if (got == 0) {
			break;
		}
		Callable done;
		if (!waiting.is_empty()) {
			done = waiting.front()->get();
			waiting.pop_front();
		}
		asked_at = waiting.is_empty() ? 0 : OS::get_singleton()->get_ticks_msec();
		if (done.is_valid()) {
			done.call(reply);
		}
		if (tcp.is_null()) {
			return; // Closed inside the answer
		}
	}
	if (at > 0) {
		inbox = inbox.slice(at);
	}
}

// Password reply. If refused, nothing is saved from then on
void MVStore::auth_got(const Variant &p_reply) {
	if (p_reply.get_type() != Variant::STRING) {
		fail("Redis refused the password");
	}
}

// Hands the HGETALL reply (alternating field names and raw bytes) as { thing: { name: value } }. null if unreadable.
// Field names are "<thing>.<name>". A thing is a kind or a world path, neither of which contains "."
void MVStore::hash_got(const Variant &p_reply, const Callable &p_done) {
	if (p_reply.get_type() != Variant::ARRAY) {
		p_done.call(Variant());
		return;
	}
	Dictionary out;
	const Array items = p_reply;
	for (int i = 0; i + 1 < items.size(); i += 2) {
		if (items[i].get_type() != Variant::PACKED_BYTE_ARRAY || items[i + 1].get_type() != Variant::PACKED_BYTE_ARRAY) {
			continue;
		}
		const PackedByteArray raw = items[i];
		const String field = String::utf8((const char *)raw.ptr(), raw.size());
		const int dot = field.rfind_char('.');
		Variant value;
		if (dot <= 0 || dot + 1 >= field.length() || !decode_value(items[i + 1], value)) {
			continue;
		}
		const String head = field.substr(0, dot);
		Dictionary one = out.get(head, Dictionary());
		one[field.substr(dot + 1)] = value;
		out[head] = one;
	}
	p_done.call(out);
}

// Hands a text reply. Empty if missing, null if unreadable
void MVStore::text_got(const Variant &p_reply, const Callable &p_done) {
	if (p_reply.get_type() == Variant::PACKED_BYTE_ARRAY) {
		const PackedByteArray body = p_reply;
		p_done.call(String::utf8((const char *)body.ptr(), body.size()));
	} else if (p_reply.get_type() == Variant::STRING) {
		p_done.call(String(p_reply));
	} else if (p_reply.get_type() == Variant::NIL) {
		p_done.call(String());
	} else {
		p_done.call(Variant());
	}
}

// SET reply. With NX, a null reply means the key was already taken
void MVStore::set_got(const Variant &p_reply, const Callable &p_done) {
	if (p_reply.get_type() == Variant::STRING) {
		p_done.call(true);
	} else if (p_reply.get_type() == Variant::NIL) {
		p_done.call(false);
	} else {
		p_done.call(Variant());
	}
}

void MVStore::begin_multi() {
	if (saving && !multi) {
		multi = true;
		send({ word_of("MULTI") });
	}
}

// Writes one thing's values as "<thing>.<name>" fields one by one. Only changed names arrive, so only those are written.
// Replies are discarded. Names are author variable names or "@kind"; other spellings are rejected
void MVStore::put(const String &p_key, const String &p_head, const Dictionary &p_values) {
	if (tcp.is_null() || p_head.is_empty()) {
		return;
	}
	for (const Variant &name : p_values.get_key_list()) {
		const String field = name;
		PackedByteArray body;
		if (!(MVGate::safe_name(field) || field == "@kind") || !encode_value(p_values[name], body)) {
			continue;
		}
		begin_multi();
		send({ word_of("HSET"), word_of(p_key), word_of(p_head + "." + field), body });
	}
}

// Deletes one thing's fields
void MVStore::wipe(const String &p_key, const String &p_head, const PackedStringArray &p_fields) {
	if (tcp.is_null() || p_head.is_empty() || p_fields.is_empty()) {
		return;
	}
	Vector<PackedByteArray> words = { word_of("HDEL"), word_of(p_key) };
	for (const String &field : p_fields) {
		words.push_back(word_of(p_head + "." + field));
	}
	begin_multi();
	send(words);
}

// Groups one collection pass into one batch, so a drop midway never leaves half of it stored
void MVStore::begin_save() {
	saving = true;
}

void MVStore::end_save() {
	saving = false;
	if (multi) {
		multi = false;
		send({ word_of("EXEC") });
	}
}

// Keys only by pseudonymous IDs made by the Secret. Checks the form cannot collide with world keys
bool MVStore::user_key(const String &p_id, String &r_key) const {
	if (!p_id.begins_with("user:") || p_id.length() > 128) {
		return false;
	}
	r_key = prefix + ":" + p_id;
	return true;
}

// Hands the user's save values as per-kind dictionaries
void MVStore::load_user(const String &p_id, const Callable &p_done) {
	String key;
	if (!user_key(p_id, key)) {
		p_done.call(Variant());
		return;
	}
	send({ word_of("HGETALL"), word_of(key) }, callable_mp(this, &MVStore::hash_got).bind(p_done));
}

// Hands the world save as per-path dictionaries. Keeps asking until it can be read.
// Starting without it would run a world with initial values and erase the real data on the next save
void MVStore::load_world(const Callable &p_done) {
	world_done = p_done;
	world_retry_at = 0;
	if (live) {
		ask_world();
	}
}

void MVStore::ask_world() {
	world_asked = true;
	send({ word_of("HGETALL"), word_of(prefix + ":world") }, callable_mp(this, &MVStore::hash_got).bind(callable_mp(this, &MVStore::world_got)));
}

void MVStore::world_got(const Variant &p_reply) {
	world_asked = false;
	if (p_reply.get_type() != Variant::DICTIONARY) {
		world_retry_at = OS::get_singleton()->get_ticks_msec() + RETRY_MS;
		ERR_PRINT("Online: cannot load the world save from Redis. Trying again");
		return;
	}
	const Callable done = world_done;
	world_done = Callable();
	done.call(p_reply);
}

// Writes the user's save values. Keyed only by a pseudonymous ID that is stable across connections. Fields are "<kind>.<name>"
void MVStore::save_user(const String &p_id, const String &p_kind, const Dictionary &p_values) {
	String key;
	if (user_key(p_id, key) && MVGate::safe_name(p_kind)) {
		put(key, p_kind, p_values);
	}
}

// Writes the world's save values. Nodes without an owner are remembered by path. Fields are "<path>.<name>"
void MVStore::save_world(const String &p_path, const Dictionary &p_values) {
	put(prefix + ":world", p_path, p_values);
}

void MVStore::drop_world(const String &p_path, const PackedStringArray &p_fields) {
	wipe(prefix + ":world", p_path, p_fields);
}

// Hands the account a device key currently points to
void MVStore::account_of(const String &p_device, const Callable &p_done) {
	if (p_device.is_empty()) {
		p_done.call(String());
		return;
	}
	send({ word_of("GET"), word_of(prefix + ":device:" + p_device) }, callable_mp(this, &MVStore::text_got).bind(p_done));
}

void MVStore::unbind_account(const String &p_device, const Callable &p_done) {
	if (p_device.is_empty()) {
		p_done.call(Variant());
		return;
	}
	send({ word_of("DEL"), word_of(prefix + ":device:" + p_device) }, callable_mp(this, &MVStore::del_got).bind(p_done));
}

// Read the linked account marker and name without blocking the auth loop.
void MVStore::profile_of(const String &p_account, const Callable &p_done) {
	String key;
	if (!user_key(p_account, key)) {
		p_done.call(Variant());
		return;
	}
	send({ word_of("GET"), word_of(prefix + ":profile:" + p_account) }, callable_mp(this, &MVStore::text_got).bind(p_done));
}

// Persist the linked account marker and name before confirming the link.
void MVStore::save_profile(const String &p_account, const String &p_name, const Callable &p_done) {
	String key;
	if (!user_key(p_account, key)) {
		p_done.call(Variant());
		return;
	}
	send({ word_of("SET"), word_of(prefix + ":profile:" + p_account), word_of("1" + p_name) }, callable_mp(this, &MVStore::set_got).bind(p_done));
}

void MVStore::del_got(const Variant &p_reply, const Callable &p_done) {
	p_done.call(p_reply.get_type() == Variant::STRING ? Variant(true) : Variant());
}

// Binds a device key to an account. Handing over an account only re-points this one key.
// Newly created accounts are bound with NX, so if two logins on one device create accounts at once, only the first binding remains
void MVStore::bind_account(const String &p_device, const String &p_account, bool p_overwrite, const Callable &p_done) {
	if (p_device.is_empty() || !p_account.begins_with("user:")) {
		p_done.call(Variant());
		return;
	}
	Vector<PackedByteArray> words = { word_of("SET"), word_of(prefix + ":device:" + p_device), word_of(p_account) };
	if (!p_overwrite) {
		words.push_back(word_of("NX"));
	}
	send(words, callable_mp(this, &MVStore::set_got).bind(p_done));
}

#endif // MV_SECRET_ENABLED
