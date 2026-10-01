/**************************************************************************/
/*  mv_runtime.cpp                                                        */
/**************************************************************************/

// Runs both the Online world and the Client world with one runtime. The Online side runs the author's single GDScript
// and distributes deltas; the Client side only reads the world it receives and sends commands to the Online side.

#include "mv_runtime.h"

#include "mv_check.h"
#include "mv_client.h"
#include "mv_client_script.h"
#include "mv_classes.h"
#include "mv_fire.h"
#include "mv_frame.h"
#include "mv_gate.h"
#include "mv_link.h"
#include "mv_online.h"
#include "mv_secret.h"
#include "mv_stage.h"
#include "mv_thing.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/time.h"

#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_online.h"

static constexpr int FIRE_MAX = 64; // Maximum signals distributed per destination per tick
static constexpr uint64_t STREAM_MS = 50; // Continuous input interval, matching the Online-side tick
static constexpr int QUEUE_MAX = 40; // Number of one-shot events that can be held

// Checks for a callable Secret entry. Tells the author why when it fails
static Dictionary secret_args(const Dictionary &p_fns, const String &p_name, const Array &p_args) {
	const Dictionary checked = p_fns.has(p_name) ? MVFrame::arguments(p_fns[p_name], p_args) : Dictionary();
	if (!bool(checked.get("ok", false))) {
		ERR_PRINT(vformat(U"Online: %s() is not a Secret entry point, or the arguments do not match", p_name));
	}
	return checked;
}

// Passes only answers that can be sent over the network
static Variant sendable(const Variant &p_out, const String &p_name) {
	if (MVFrame::plain(p_out)) {
		return p_out;
	}
	ERR_PRINT(vformat(U"Online: the return value of %s() cannot be sent. Node, Resource, Callable and Signal cannot be returned", p_name));
	return Variant();
}

// Returns the live fixed Server
Online *MVRuntime::server_node() const {
	return Object::cast_to<Online>(ObjectDB::get_instance(server));
}

// Flushes whatever is not yet stored, then closes. The store itself is owned elsewhere
MVRuntime::~MVRuntime() {
	if (saves != nullptr) {
		saves->poll();
	}
}

// Collects GDScript public methods and @online marks into a small table for Clients
Dictionary MVRuntime::api_of(const Ref<Script> &p_script, const String &p_author_path) const {
	Dictionary api;
	Dictionary marks = GDScriptOnline::fields_of(p_author_path);
	marks.merge(GDScriptOnline::fields_of(p_script->get_path()), true);
	Dictionary fns;
	List<MethodInfo> methods;
	p_script->get_script_method_list(&methods);
	for (const MethodInfo &method : methods) {
		// @online funcs are called from Clients, and @online funcs inside a Secret from Online scripts.
		// Both record their argument shapes here, checked before accepting a call
		if (!(int(marks.get(method.name, 0)) & (GDScriptOnline::F_METHOD | GDScriptOnline::F_SECRET))) {
			continue;
		}
		// Name the Online side moved a function body to. Not an entry callable from the Client
		if (String(method.name).begins_with(MVClientScript::IMPLEMENTATION)) {
			continue;
		}
		Dictionary spec;
		Array types;
		for (const PropertyInfo &arg : method.arguments) {
			types.push_back(arg.type);
		}
		spec["count"] = method.arguments.size();
		spec["types"] = types;
		fns[method.name] = spec;
	}
	Dictionary signals;
	List<MethodInfo> signal_list;
	p_script->get_script_signal_list(&signal_list);
	for (const MethodInfo &signal : signal_list) {
		if (!(int(marks.get(signal.name, 0)) & GDScriptOnline::F_SIGNAL)) {
			continue;
		}
		// The Client-side port is built from the received argument count. Counts that cannot be built are reported to the author here
		if (signal.arguments.size() > MVClientScript::ARGS_SANE) {
			ERR_PRINT(vformat(U"Online: signal %s has %d arguments; at most %d are allowed", signal.name, signal.arguments.size(), (int)MVClientScript::ARGS_SANE));
			continue;
		}
		signals[signal.name] = signal.arguments.size();
	}
	api["fns"] = fns;
	api["marks"] = marks;
	Dictionary binds = GDScriptOnline::bindings_of(p_author_path);
	binds.merge(GDScriptOnline::bindings_of(p_script->get_path()), true);
	api["binds"] = binds;
	api["signals"] = signals;
	return api;
}

// Attaches the author script onto the shared Online-side base while keeping Scene values
bool MVRuntime::dress(Node *p_node, const String &p_kind, const String &p_path, bool p_server) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || file->get_length() > MVCheck::SRC_MAX) {
		return false;
	}
	const String source = file->get_as_text();
	const Dictionary checked = MVCheck::check(source);
	if (!bool(checked.get("ok", false))) {
		ERR_PRINT(vformat(U"Online: %s:%d: %s", p_path, checked.get("line", 0), checked.get("reason", String())));
		return false;
	}
	Dictionary keep;
	// A Node whose script was removed before entering the tree receives the values held at that time here
	MVStage::take_held(p_node, keep);
	MVStage::script_values(p_node, keep);
	const String native = p_server ? String(Online::KIND) : MVClientScript::native_of(p_kind);
	const String bound = MVClientScript::authority_source(source, native, p_server, GDScriptOnline::fields_of(p_path));
	if (bound.is_empty()) {
		return false;
	}
	const String key = p_kind + ":" + bound.sha256_text();
	Ref<Script> script;
	const HashMap<String, Ref<Script>>::ConstIterator found = scripts.find(key);
	if (found) {
		script = found->value;
	} else {
		Ref<GDScript> made;
		made.instantiate();
		made->set_path("online-authority://" + p_kind + "-" + bound.sha256_text().left(16) + ".gd");
		// The rewritten script drops @online from functions the engine calls, so if the original script belongs to the world, mark it first.
		// Without this, my and world are not substituted during parsing
		if (GDScriptOnline::marked(p_path)) {
			GDScriptOnline::touch(made->get_path());
		}
		made->set_source_code(bound);
		if (made->reload() != OK) {
			ERR_PRINT(vformat(U"Online: cannot rebuild %s as an Online-side script", p_path));
			return false;
		}
		script = made;
		scripts[key] = script;
	}
	p_node->set_script(script);
	for (const Variant &name : keep.keys()) {
		p_node->set(name, keep[name]);
	}
	// Position and rotation can be added now that the Node type is known.
	// The table with position and rotation added is also sent to Clients. Clients use it as received and add nothing themselves
	Dictionary api = api_of(script, p_path);
	const Dictionary marks = MVClasses::with_auto(p_node, api.get("marks", Dictionary()), GDScriptOnline::marked(p_path));
	api["marks"] = marks;
	p_node->set_meta(SNAME("mark"), marks);
	p_node->set_meta(SNAME("bind"), api.get("binds", Dictionary()));
	p_node->set_meta(SNAME("fns"), api.get("fns", Dictionary()));
	// Scene connections can be wired now that the script is attached
	MVClientScript::wire_signals(p_node, p_kind, true);
	// Online scripts are the Online side itself. Only Nodes present on both sides need to split which side emits
	if (!p_server) {
		MVClientScript::split_signals(p_node, marks, true);
	}
	// Unmarked scripts run on every player, the host included, called by the engine.
	// Only physics scripts advance on the world clock, so the flag depends on whether that entry exists
	p_node->set_process(p_node->has_method(SNAME("_process")));
	p_node->set_physics_process(p_node->has_method(MVClientScript::STEP));
	const int64_t uid = p_node->get_meta(SNAME("uid"), 0);
	if (uid > 0) {
		apis[uid] = api;
		apis_dirty[uid] = api;
		const Dictionary signals = api.get("signals", Dictionary());
		for (const Variant &name_value : signals.keys()) {
			const StringName name = name_value;
			const bool only_mine = int(marks.get(name, 0)) & GDScriptOnline::F_MY;
			const String to = only_mine ? own_of(p_node) : String();
			// An @online_my signal on an ownerless Node would go to everyone with no destination.
			// Using a Player scene for ownerless things such as bots is normal, so it is silently left unconnected
			if (only_mine && to.is_empty()) {
				continue;
			}
			// One relay that accepts any argument count. Written like an ordinary signal
			p_node->connect(name, Callable(memnew(MVFire(this, uid, String(name), to))));
		}
	}
	const String path = where(p_node);
	if (!path.is_empty()) {
		live[path] = p_node->get_instance_id();
	}
	return true;
}

// Assigns a world path to one author script
void MVRuntime::begin_path(const String &p_path) {
	context_stack.push_back(current);
	current = p_path;
}

// Looks up a Node's world path and begins its author script
void MVRuntime::begin(Node *p_node) {
	begin_path(where(p_node));
}

// Closes the author script's execution position
void MVRuntime::end() {
	current = context_stack.is_empty() ? String() : context_stack[context_stack.size() - 1];
	if (!context_stack.is_empty()) {
		context_stack.resize(context_stack.size() - 1);
	}
}

// Stops the gate once the world starts closing. Otherwise the gate would hold children until the engine's
// memdelete during cleanup, and the tree could not close without deleting Online
void MVRuntime::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE || p_what == NOTIFICATION_EXIT_TREE) {
		close_world();
	}
}

// Announces closing. The gate no longer holds this world
void MVRuntime::close_world() {
	closing = true;
}

// Builds the world. The Online side auto-detects scenes and the Server script and starts running;
// the Client side places an empty world of the same shape and waits for deltas
String MVRuntime::start(bool p_authority, const Callable &p_ready) {
	authority = p_authority;
	const String reason = authority ? MVClientScript::configure_authority() : MVClientScript::configure();
	if (!reason.is_empty()) {
		return reason;
	}
	Online *made_server = MVStage::build(this, authority);
	if (made_server == nullptr) {
		return U"Cannot create the world";
	}
	server = made_server->get_instance_id();
	// The Client world only reads. It needs no gate, replicator, or script replacement
	if (!authority) {
		return String();
	}
	gate.instantiate();
	rep.instantiate();
	rep->know(made_server);
	ready_done = p_ready;
	// A side handling only Secrets does not attach world scripts. Otherwise the same _ready would
	// run twice, on the host and here, and the Secret contents would run twice
	if (secret_only) {
		dress_secrets(made_server);
		world_ready = true;
		if (ready_done.is_valid()) {
			ready_done.call();
		}
		return String();
	}
	// Validate the world script here. Finding out later that it cannot be attached would be after listening has started
	world_path = MVClientScript::world_script();
	Ref<FileAccess> world_file = world_path.is_empty() ? Ref<FileAccess>() : FileAccess::open(world_path, FileAccess::READ);
	const String world_source = world_file.is_valid() ? world_file->get_as_text() : String();
	const Dictionary world_check = world_source.is_empty() ? Dictionary() : MVCheck::check(world_source);
	if (!world_source.is_empty() && !bool(world_check.get("ok", false))) {
		ERR_PRINT(vformat(U"Online: %s:%d: %s", world_path, world_check.get("line", 0), world_check.get("reason", String())));
	}
	if (world_source.is_empty() || !bool(world_check.get("ok", false)) || String(world_check.get("cls", String())) != Online::KIND) {
		return U"The world needs one scene whose root is Online, with a script attached to that root";
	}
	// The store is bound from outside. A local Redis and a port that asks the Secret fit the same shape.
	// Scripts are attached after the world save values return, so values are already set at _ready.
	// Do not start while the store is unreadable. Starting with initial values would erase the real data on the next save
	if (saves != nullptr) {
		saves->load_world(callable_mp(this, &MVRuntime::world_loaded));
	} else {
		world_loaded(Dictionary());
	}
	return String();
}

// World save values have returned. Attaches scripts, restores values, and starts running
void MVRuntime::world_loaded(const Dictionary &p_saved) {
	Online *made_server = server_node();
	if (made_server == nullptr) {
		return;
	}
	pending_world = p_saved;
	// Do not confuse a script that exists but fails to attach with a missing script.
	// Confusing them would send the author looking for a file that does exist
	if (!dress(made_server, Online::KIND, world_path, true)) {
		ERR_PRINT(vformat(U"Online: cannot load %s as the Online-side script", world_path));
		return;
	}
	// The world Node's own save values are restored before _ready, like other Nodes
	restore_in(made_server, MVGate::SERVER, String());
	// Author scripts under Online get the same setup as spawned things before the world starts
	dress_fixed(made_server);
	// Secrets run only on the Online side. Replaces scripts attached in the scene with Online-side scripts
	dress_secrets(made_server);
	if (made_server->is_ready()) {
		begin(made_server);
		GDScriptOnline::ready(made_server);
		end();
	}
	world_ready = true;
	if (ready_done.is_valid()) {
		ready_done.call();
	}
}

// Carries signals fired this round over to players not yet receiving distributions.
// Drops the oldest first. They would never arrive anyway, so overflow is not reported
void MVRuntime::carry_fire(const String &p_who, const Array &p_fires, Dictionary &r_carry) {
	if (joined.has(p_who)) {
		return;
	}
	Array kept = r_carry.get(p_who, Array());
	kept.append_array(p_fires);
	if (kept.size() > FIRE_MAX) {
		kept = kept.slice(kept.size() - FIRE_MAX);
	}
	r_carry[p_who] = kept;
}

Dictionary MVRuntime::tick() {
	Node *root = this;
	if (server_node() == nullptr || !world_ready) {
		return Dictionary();
	}
	gate->begin(root);
	Vector<String> gone;
	// Only Nodes present now advance this round. Things spawned during _physics_process do not advance in the same round
	Vector<Pair<String, ObjectID>> stepping;
	for (const KeyValue<String, ObjectID> &entry : live) {
		stepping.push_back(Pair<String, ObjectID>(entry.key, entry.value));
	}
	for (const Pair<String, ObjectID> &entry : stepping) {
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(entry.second));
		if (node == nullptr) {
			gone.push_back(entry.first);
			continue;
		}
		if (node->is_physics_processing() && node->has_method(MVClientScript::STEP)) {
			begin(node);
			node->call(MVClientScript::STEP, 1.0 / double(MVRep::TICK_HZ));
			end();
		}
	}
	for (const String &path : gone) {
		live.erase(path);
	}
	Dictionary out = rep->collect(root);
	// Secret instances live outside the world, so only their save values are added here
	for (const KeyValue<String, ObjectID> &one : secrets) {
		if (Node *inner = Object::cast_to<Node>(ObjectDB::get_instance(one.value))) {
			rep->save_secret(one.key, inner, out);
		}
	}
	out["frame"] = ++frame;
	if (!pending_world.is_empty()) {
		pending_world = rep->restore(root, pending_world);
	}
	save_out(out);
	for (const Variant &uid : Array(out.get("del", Array()))) {
		if (apis.erase(uid)) {
			apis_dirty[uid] = Variant();
		}
	}
	PackedStringArray who;
	for (const KeyValue<String, Ref<MY>> &one : users) {
		who.push_back(one.key);
	}
	if (users_dirty) {
		out["who"] = who;
		users_dirty = false;
	}
	if (!apis_dirty.is_empty()) {
		out["apis"] = apis_dirty.duplicate(true);
		apis_dirty.clear();
	}
	if (!fires.is_empty()) {
		out["fire"] = fires.duplicate(true);
		// A newly spawned player is not yet receiving distributions.
		// Carry signals fired in the same round as their spawn so none are missed
		for (const KeyValue<String, Ref<MY>> &one : users) {
			carry_fire(one.key, fires, carry_all);
		}
		fires.clear();
	}
	if (!fires_my.is_empty()) {
		out["fire_my"] = fires_my.duplicate(true);
		// Spawns happen at login, but that player receives distributions only after ENet connects.
		// What arrives too early is not dropped but delivered with the first full state
		for (const Variant &who_value : fires_my.keys()) {
			carry_fire(who_value, fires_my[who_value], carry_my);
		}
		fires_my.clear();
	}
	return out;
}

// Returns the whole current tree, values, and APIs to a late joiner
Dictionary MVRuntime::snapshot(const String &p_who) {
	if (server_node() == nullptr || rep.is_null()) {
		return Dictionary();
	}
	Dictionary out = rep->snapshot(this);
	out["reset"] = true;
	out["frame"] = frame;
	PackedStringArray who;
	for (const KeyValue<String, Ref<MY>> &one : users) {
		who.push_back(one.key);
	}
	out["who"] = who;
	out["apis"] = apis;
	// The recipient of the full state becomes a regular distribution target from now on
	joined.insert(p_who);
	const Array waited = carry_all.get(p_who, Array());
	if (!waited.is_empty()) {
		Array fire = Array(out.get("fire", Array())).duplicate(false);
		fire.append_array(waited);
		out["fire"] = fire;
	}
	carry_all.erase(p_who);
	Array kept = carry_my.get(p_who, Array());
	if (fires_my.has(p_who)) {
		kept.append_array(fires_my[p_who]);
		fires_my.erase(p_who);
	}
	if (!kept.is_empty()) {
		Dictionary carry;
		carry[p_who] = kept;
		out["fire_my"] = carry;
	}
	carry_my.erase(p_who);
	return for_player(out, p_who);
}

// Narrows the my box to this player only and removes the save box before sending
Dictionary MVRuntime::for_player(const Dictionary &p_frame, const String &p_who) const {
	Dictionary out = p_frame.duplicate(false);
	out.erase("save");
	out.erase("save_del");
	out["my"] = Dictionary(p_frame.get("my", Dictionary())).get(p_who, Dictionary());
	out["flow_my"] = Dictionary(p_frame.get("flow_my", Dictionary())).get(p_who, Dictionary());
	out["settle_my"] = Dictionary(p_frame.get("settle_my", Dictionary())).get(p_who, Dictionary());
	out["motion_my"] = Dictionary(p_frame.get("motion_my", Dictionary())).get(p_who, Dictionary());
	const Array personal = Dictionary(p_frame.get("fire_my", Dictionary())).get(p_who, Array());
	if (!personal.is_empty()) {
		Array fire = Array(p_frame.get("fire", Array())).duplicate(false);
		fire.append_array(personal);
		out["fire"] = fire;
	}
	out.erase("fire_my");
	return out;
}

// Ownerless things are stored by path, Player things by the player's pseudonymous ID. Only the p_values names are written
void MVRuntime::save_entry(const String &p_path, const Dictionary &p_entry, const Dictionary &p_values) {
	const String own = p_entry.get("own", String());
	const String kind = p_entry.get("kind", String());
	if (own.is_empty()) {
		if (saves->keeps_world()) {
			// Also store the kind name, so values are not restored when a different kind is placed at the same path
			Dictionary values = p_values.duplicate();
			values["@kind"] = kind;
			saves->save_world(p_path, values);
		}
		return;
	}
	// An owned thing has one slot per kind in the account. With two of the same kind only the later survives, so warn once
	const String slot = own + ":" + kind;
	const HashMap<String, String>::ConstIterator held = owned_saves.find(slot);
	if (held && held->value != p_path && !warned_saves.has(slot)) {
		warned_saves.insert(slot);
		WARN_PRINT(vformat(U"Online: %s owns more than one %s with @online_save. Only one of them is kept", own, kind));
	}
	owned_saves[slot] = p_path;
	const Ref<MY> mine = my_of(own);
	saves->save_user(mine.is_valid() ? mine->get_id() : String(), kind, p_values);
}

void MVRuntime::save_out(const Dictionary &p_frame) {
	// Store replies are not advanced here. If a spawn happened inside a reply, the script port would arrive before
	// the spawn notice and the Client would not attach that thing's script. They advance outside the world tick.
	// While disconnected, current values and removed things keep being recorded. Deltas are collected only once,
	// so dropping them here would leave old values even after reconnecting
	const bool open = saves != nullptr && saves->is_open();
	if (open) {
		// Everything stored in one collection pass is one batch, so a drop midway never leaves half of it
		saves->begin_save();
		if (!saving) {
			saving = true;
			// Right after reconnecting, Redis lacks values that changed while disconnected.
			// Sending only deltas would leave them stale, so store the full current set again and drop removed things
			for (const Variant &path : drop_later.get_key_list()) {
				saves->drop_world(path, drop_later[path]);
			}
			drop_later.clear();
			for (const Dictionary &later : user_later) {
				const Dictionary entry = later["entry"];
				saves->save_user(later["id"], entry["kind"], entry["values"]);
			}
			user_later.clear();
			for (const Variant &key : save_live.get_key_list()) {
				const Dictionary entry = save_live[key];
				save_entry(key, entry, entry["values"]);
			}
		}
	} else {
		saving = false;
	}
	// Handle removed Nodes first, so that when another Player enters the same path in the same tick,
	// the newer full record is not erased. A player's own things stay in Redis until the next login
	for (const Variant &key : Array(p_frame.get("save_del", Array()))) {
		const String path = key;
		const Dictionary entry = save_live.get(path, Dictionary());
		save_live.erase(path);
		if (!String(entry.get("own", String())).is_empty() || !saves->keeps_world()) {
			continue;
		}
		PackedStringArray fields;
		for (const Variant &field : Dictionary(entry.get("values", Dictionary())).get_key_list()) {
			fields.push_back(field);
		}
		fields.push_back("@kind");
		if (open) {
			saves->drop_world(path, fields);
		} else {
			drop_later[path] = fields;
		}
	}
	const Dictionary save = p_frame.get("save", Dictionary());
	for (const Variant &key : save.get_key_list()) {
		const String path = key;
		const Dictionary diff = save[key];
		// Only changed fields are collected. Only those are stored, and the record is merged into the current values
		Dictionary entry = Dictionary(save_live.get(path, Dictionary())).duplicate(true);
		Dictionary values = entry.get("values", Dictionary());
		const Dictionary changed = diff.get("values", Dictionary());
		for (const Variant &field : changed.get_key_list()) {
			values[field] = changed[field];
		}
		entry["kind"] = diff.get("kind", String());
		entry["own"] = diff.get("own", String());
		entry["values"] = values;
		save_live[path] = entry;
		if (open) {
			save_entry(path, entry, changed);
		}
	}
	if (open) {
		saves->end_save();
	}
}

// Restores values stored by the player's pseudonymous ID or by path onto a spawned Node
void MVRuntime::restore_in(Node *p_node, const String &p_path, const String &p_own) {
	if (rep.is_null()) {
		return;
	}
	const String kind = p_node->get_meta(SNAME("kind"), String());
	Dictionary values;
	if (p_own.is_empty()) {
		values = pending_world.get(p_path, Dictionary());
		pending_world.erase(p_path);
		// Do not restore values if a different kind was placed at the same path
		if (values.has("@kind") && String(values["@kind"]) != kind) {
			return;
		}
	} else {
		Dictionary mine = pending_user.get(p_own, Dictionary());
		values = mine.get(kind, Dictionary());
		mine.erase(kind);
		pending_user[p_own] = mine;
	}
	if (values.is_empty()) {
		return;
	}
	rep->restore_node(p_node, values);
}

// Receives an auth context, places the player's managed Node, then notifies the author
bool MVRuntime::join_context(const String &p_who, const Dictionary &p_context, bool p_on, const Dictionary &p_saved) {
	Online *host = server_node();
	if (host == nullptr || !MVGate::safe_name(p_who)) {
		return false;
	}
	if (!p_on) {
		const Ref<MY> leaving = my_of(p_who);
		const Vector<Node *> mine = owned(p_who);
		// Once removed from the tree they miss the next collection, so store the author's last save values first
		for (Node *one : mine) {
			save_last(one, p_who);
		}
		users.erase(p_who);
		pending_user.erase(p_who);
		carry_my.erase(p_who);
		carry_all.erase(p_who);
		joined.erase(p_who);
		users_dirty = true;
		if (leaving.is_valid()) {
			begin(host);
			// Decide on removal before notifying, so the player is not counted when the author counts
			for (Node *one : mine) {
				queue_node(one);
			}
			host->emit_signal(SNAME("player_left"), leaving);
			end();
		}
		return true;
	}
	// A returning player keeps the same my, so anything the author remembered keyed by my continues
	if (users.has(p_who)) {
		const String before = users[p_who]->get_id();
		users[p_who]->set_context(p_who, p_context.duplicate(true));
		users_dirty = true;
		// External login bound them to another account. Restore that account's values onto the player's placed things
		if (before != users[p_who]->get_id()) {
			user_loaded(p_saved, p_who, false);
		}
		return true;
	}
	if (!world_ready || !bool(p_context.get("authenticated", false)) || room_full_for(p_who)) {
		return false;
	}
	Ref<MY> mine;
	mine.instantiate();
	mine->set_context(p_who, p_context.duplicate(true));
	users[p_who] = mine;
	users_dirty = true;
	// Identity reads the player's save values and passes them in, so values are already set at _ready
	user_loaded(p_saved, p_who, true);
	return true;
}

// Records the player's save values. On first join, notifies the author to place them; on account change, restores onto placed things
void MVRuntime::user_loaded(const Dictionary &p_saved, const String &p_who, bool p_fresh) {
	Online *host = server_node();
	if (host == nullptr || !users.has(p_who)) {
		return; // Left before the values returned
	}
	pending_user[p_who] = p_saved;
	if (p_fresh) {
		begin(host);
		// If the Secret sets player_scene, the engine places the player's thing. The author's signal comes after
		const Ref<PackedScene> scene = Secret::player_scene_of(host);
		if (scene.is_valid()) {
			host->spawn(scene, users[p_who], Dictionary());
		}
		host->emit_signal(SNAME("player_joining"), users[p_who]);
		end();
		return;
	}
	for (Node *one : owned(p_who)) {
		restore_in(one, where(one), p_who);
	}
}

// Returns that player's my. It is not placed in the tree, so this table is the only place owners live
Ref<MY> MVRuntime::my_of(const String &p_who) const {
	const HashMap<String, Ref<MY>>::ConstIterator found = users.find(p_who);
	return found ? found->value : Ref<MY>();
}

// Collects this player's things directly under Online
Vector<Node *> MVRuntime::owned(const String &p_who) const {
	Vector<Node *> out;
	Online *host = server_node();
	for (int i = 0; host != nullptr && i < host->get_child_count(); i++) {
		Node *child = host->get_child(i);
		if (String(child->get_meta(SNAME("own"), String())) == p_who) {
			out.push_back(child);
		}
	}
	return out;
}

// Whether no seat is left for this player to newly join. Room size comes from the Secret's room_max.
// Rejoining returns to the original seat, so it passes even when full
bool MVRuntime::room_full_for(const String &p_who) const {
	return !users.has(p_who) && int(users.size()) >= Secret::room_max_of(server_node());
}

// Maps Nodes received as serial numbers back to this world.
// Lookup is inside the world, so forged numbers cannot point elsewhere
Array MVRuntime::unpack_nodes(const Array &p_args, bool &r_gone, bool &r_outside) {
	Array out = p_args.duplicate();
	for (int i = 0; i < out.size(); i++) {
		if (out[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary one = out[i];
		if (!one.has(MVFrame::NODE_KEY)) {
			continue;
		}
		const int64_t uid = one[MVFrame::NODE_KEY];
		Node *found = uid > 0 && rep.is_valid() ? rep->at_uid(this, uid) : nullptr;
		// Only things in the world (under Online) may be passed. Things without a number, the runtime, and Secret instances are not.
		// Passing 0 would search from the root, hit the runtime itself, and let queue_free take down the whole room
		if (uid <= 0 || (found != nullptr && (int64_t(found->get_meta(SNAME("uid"), 0)) != uid || !in_world(found)))) {
			found = nullptr;
			r_outside = true;
		}
		r_gone = r_gone || found == nullptr;
		out[i] = Variant(found);
	}
	return out;
}

// Notifies all of the player's things of disconnection
void MVRuntime::disconnect_player(const String &p_who) {
	// Not a distribution target until rejoining. Signals in the meantime carry over to the next full state
	joined.erase(p_who);
	for (Node *one : owned(p_who)) {
		// Resets a disconnected player's input to declared defaults, so held keys do not keep running
		const Dictionary rest = one->get_meta(SNAME("input_rest"), Dictionary());
		for (const KeyValue<Variant, Variant> &field : rest) {
			GDScriptOnline::begin_trusted_write();
			one->set(field.key, field.value);
			GDScriptOnline::end_trusted_write();
		}
		begin(one);
		MVThing::disconnected(one);
		end();
	}
}

// Calls only @online functions published by the player's own things after type checks, and hands the answer to p_done. Others' things cannot be named.
// If the script awaits, the answer is handed over once it is ready
void MVRuntime::ask_player(const String &p_who, int64_t p_uid, const String &p_fn, const Array &p_args, const Callable &p_done) {
	// @online_input on the player's own things. Receives {uid: {name: value}} in one item and sets only matching types. No answer
	if (p_fn == "=") {
		const Dictionary changed = p_args.size() == 1 ? Dictionary(p_args[0]) : Dictionary();
		for (const KeyValue<Variant, Variant> &node : changed) {
			Node *one = rep.is_valid() ? rep->at_uid(this, node.key) : nullptr;
			if (one == nullptr || String(one->get_meta(SNAME("own"), String())) != p_who || node.value.get_type() != Variant::DICTIONARY) {
				refuse(p_who, U"not owned by the caller");
				continue;
			}
			const Dictionary marks = one->get_meta(SNAME("mark"), Dictionary());
			for (const KeyValue<Variant, Variant> &field : Dictionary(node.value)) {
				const Variant now = one->get(field.key);
				const bool number = now.get_type() == Variant::FLOAT && field.value.get_type() == Variant::INT;
				if (!(int(marks.get(field.key, 0)) & GDScriptOnline::F_INPUT) || (field.value.get_type() != now.get_type() && !number)) {
					refuse(p_who, U"invalid input value");
					continue;
				}
				GDScriptOnline::begin_trusted_write();
				one->set(field.key, number ? Variant(double(int64_t(field.value))) : field.value);
				GDScriptOnline::end_trusted_write();
			}
		}
		return;
	}
	Node *target = rep.is_valid() ? rep->at_uid(this, p_uid) : nullptr;
	if (target == nullptr || !users.has(p_who) || String(target->get_meta(SNAME("own"), String())) != p_who) {
		refuse(p_who, U"not owned by the caller");
		finish(Variant(), p_fn, p_done);
		return;
	}
	const Dictionary fns = target->get_meta(SNAME("fns"), Dictionary());
	const Dictionary checked = fns.has(p_fn) ? MVFrame::arguments(fns[p_fn], p_args) : Dictionary();
	if (GDScriptOnline::is_callback(p_fn) || p_fn.begins_with("@") || !bool(checked.get("ok", false))) {
		refuse(p_who, U"invalid @online function or arguments");
		finish(Variant(), p_fn, p_done);
		return;
	}
	// If the Node the Client saw is gone, the request is stale. Spares the author a null check
	bool gone = false;
	bool outside = false;
	const Array args = unpack_nodes(checked.get("args", Array()), gone, outside);
	if (outside) {
		refuse(p_who, U"passed a node that is not in the world");
	}
	if (gone) {
		finish(Variant(), p_fn, p_done);
		return;
	}
	target->set("__ctx", users[p_who]);
	begin(target);
	const Variant out = target->callv(p_fn, args);
	end();
	Object *state = out;
	if (state != nullptr && state->is_class("GDScriptFunctionState")) {
		state->connect(SNAME("completed"), callable_mp(this, &MVRuntime::finish).bind(p_fn, p_done), CONNECT_ONE_SHOT);
		return;
	}
	finish(out, p_fn, p_done);
}

// Hands over the answer, keeping only what can be sent over the network
void MVRuntime::finish(const Variant &p_value, const String &p_fn, const Callable &p_done) {
	if (p_done.is_valid()) {
		p_done.call(sendable(p_value, p_fn));
	}
}

// Marks that script shared by everyone is running. @online func calls during it do nothing, as on the Client
void MVRuntime::begin_local() {
	local_depth++;
}

void MVRuntime::end_local() {
	if (local_depth > 0) {
		local_depth--;
	}
}

bool MVRuntime::in_local() const {
	return local_depth > 0;
}

// Gives fixed Nodes holding author scripts under Online the same setup as spawned things.
// Skipping this would leave marked values silently unsynced and run Client-only callbacks
void MVRuntime::dress_fixed(Node *p_node) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		if (Object::cast_to<Secret>(child) != nullptr) {
			continue; // Secrets are handled by dress_secrets
		}
		const String kind = child->get_meta(SNAME("kind"), String());
		const String source = MVClientScript::source_of(kind);
		if (!kind.is_empty() && !source.is_empty()) {
			const String path = where(child);
			if (!dress(child, kind, source, false)) {
				ERR_PRINT(vformat(U"Online: cannot load the script of %s for the Online side", source));
			} else {
				// Save values are restored before _ready
				restore_in(child, path, String());
				begin(child);
				GDScriptOnline::ready(child);
				end();
			}
		}
		dress_fixed(child);
	}
}

// Finds Secrets under Online and replaces scripts attached in the scene with Online-side scripts.
// Clients never receive this tree, so scripts and values attached here never leave
void MVRuntime::dress_secrets(Node *p_node) {
	// Nodes are removed while walking the tree, so collect targets first, then move them
	Vector<ObjectID> found;
	collect_secrets(p_node, found);
	for (const ObjectID &id : found) {
		Secret *inner = Object::cast_to<Secret>(ObjectDB::get_instance(id));
		if (inner != nullptr) {
			move_secret(inner);
		}
	}
}

// Collects Secrets in the world, including nested ones
void MVRuntime::collect_secrets(Node *p_node, Vector<ObjectID> &r_found) {
	if (Object::cast_to<Secret>(p_node) != nullptr) {
		r_found.push_back(p_node->get_instance_id());
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect_secrets(p_node->get_child(i), r_found);
	}
}

// Attaches a script with only the callable functions to an entry
void MVRuntime::put_door(Secret *p_door, const String &p_source, const String &p_name, const Dictionary &p_doors) {
	const String body = MVClientScript::door_source(p_doors);
	if (body.is_empty()) {
		return;
	}
	Ref<GDScript> made;
	made.instantiate();
	made->set_path("online-secret://" + p_name + "-" + body.sha256_text().left(16) + ".gd");
	made->set_source_code(body);
	if (made->reload() == OK) {
		p_door->set_script(made);
	} else {
		ERR_PRINT(vformat(U"Online: cannot create the entry point for %s", p_source));
	}
}

// Moves a Secret instance out of the world and leaves only an entry at the same spot.
// The author's $Vault then points to the entry, so only @online funcs inside the Secret
// are callable. Its variables and child Nodes cannot be touched from the world.
// The shape is the same when the host is the Online side, so single process and host behave alike
void MVRuntime::move_secret(Secret *p_inner) {
	Node *parent = p_inner->get_parent();
	if (parent == nullptr) {
		return;
	}
	const String path = where(p_inner);
	const String name = p_inner->get_name();
	const int at = p_inner->get_index();
	const String source = p_inner->get_meta(SNAME("secret_src"), String());
	// The entry: same type, same name, same settings as the author placed.
	// The instance leaves the world, so every setting readable from the world is copied here
	Secret *door = memnew(Secret);
	door->set_name(name);
	door->copy_settings(p_inner);
	door->set_meta(SNAME("kind"), Secret::KIND);
	MVThing::allow_tree_change(parent, p_inner);
	parent->remove_child(p_inner);
	MVThing::allow_tree_change(parent, door);
	parent->add_child(door);
	if (at >= 0 && at < parent->get_child_count()) {
		MVThing::allow_tree_change(parent, door);
		parent->move_child(door, at);
	}
	// The instance leaves the world. From here Online scripts cannot reach it
	add_child(p_inner);
	Dictionary doors;
	// When the instance lives in the Secret process, no script is kept here.
	// Only the callable function declarations are received, and an entry of the same shape is placed
	if (secret_line != nullptr) {
		remote_secrets.insert(path);
		doors = door_api.get(path, Dictionary());
		if (doors.is_empty()) {
			WARN_PRINT(vformat(U"Online: the Secret in %s has no @online func callable from outside", path));
		}
		put_door(door, source, name, doors);
		return;
	}
	if (source.is_empty() || !dress(p_inner, Secret::KIND, source, false)) {
		if (!source.is_empty()) {
			ERR_PRINT(vformat(U"Online: cannot load the script of %s for the Online side", source));
		}
		return;
	}
	secrets[path] = p_inner->get_instance_id();
	live[path] = p_inner->get_instance_id();
	// Secrets are outside distribution, but their stored values stay as world data.
	// They are restored before the script's _ready, so the author can read continued values in _ready
	restore_in(p_inner, path, String());
	const Dictionary marks = p_inner->get_meta(SNAME("mark"), Dictionary());
	for (const Variant &field : marks.get_key_list()) {
		const int flags = marks[field];
		if (flags & GDScriptOnline::F_SECRET) {
			doors[field] = Dictionary(p_inner->get_meta(SNAME("fns"), Dictionary())).get(field, Dictionary());
			continue;
		}
		if (!(flags & GDScriptOnline::F_SAVE)) {
			WARN_PRINT(vformat(U"Online: %s in %s. Inside Secret only @online_save and @online on functions apply", source, field));
		}
	}
	// Bindings to display targets concern the Client's look. Not needed for a Secret that never leaves
	p_inner->set_meta(SNAME("bind"), Dictionary());
	put_door(door, source, name, doors);
	begin(p_inner);
	GDScriptOnline::ready(p_inner);
	end();
	// Secrets under a Secret get the same treatment
	Vector<ObjectID> deeper;
	for (int i = 0; i < p_inner->get_child_count(); i++) {
		collect_secrets(p_inner->get_child(i), deeper);
	}
	for (const ObjectID &id : deeper) {
		Secret *nested = Object::cast_to<Secret>(ObjectDB::get_instance(id));
		if (nested != nullptr) {
			move_secret(nested);
		}
	}
}

// Stores a Player's @online_save values just before leaving the tree, while the pseudonymous ID still exists
void MVRuntime::save_last(Node *p_node, const String &p_who) {
	if (rep.is_null() || saves == nullptr) {
		return;
	}
	const Ref<MY> mine = my_of(p_who);
	const String id = mine.is_valid() ? mine->get_id() : String();
	if (id.is_empty()) {
		return;
	}
	const Dictionary values = MVRep::save_of(p_node);
	if (values.is_empty()) {
		return;
	}
	const String path = where(p_node);
	Dictionary entry = Dictionary(save_live.get(path, Dictionary())).duplicate(true);
	Dictionary merged = entry.get("values", Dictionary());
	for (const Variant &field : values.get_key_list()) {
		merged[field] = values[field];
	}
	entry["kind"] = p_node->get_meta(SNAME("kind"), String());
	entry["own"] = p_who;
	entry["values"] = merged;
	save_live[path] = entry;
	if (saves->is_open()) {
		saves->save_user(id, entry["kind"], values);
		return;
	}
	// A player who left while disconnected disappears from save_live on the next collection. Stored after reconnecting
	Dictionary later;
	later["id"] = id;
	later["entry"] = entry;
	user_later.push_back(later);
}

// Shows the reason the gate refused a spawn where the author can see it
void MVRuntime::refused_spawn(const String &p_kind) {
	const Array why = gate->take_rejects();
	if (why.is_empty()) {
		WARN_PRINT(vformat(U"Online: spawn(\"%s\") could not place anything", p_kind));
		return;
	}
	for (const Variant &one : why) {
		WARN_PRINT(vformat(U"Online: spawn(\"%s\"): %s", p_kind, String(Dictionary(one).get("reason", one))));
	}
}

// Too many signals queued in one tick. Signals the author fired are lost, so report once
void MVRuntime::overflowed(const String &p_name) {
	if (warned_fires) {
		return;
	}
	warned_fires = true;
	WARN_PRINT(vformat(U"Online: at most %d signals can fire per tick. %s and later are dropped", (int)FIRE_MAX, p_name));
}

// Drops a rejected command without running author scripts.
// The sender, not the author, is the one to fix it, so only a warning is printed and scripts are not told
void MVRuntime::refuse(const String &p_who, const String &p_reason) {
	WARN_PRINT(vformat(U"Online: %s: %s", p_who, p_reason));
}

// Queues a signal on the next public or per-player delta
void MVRuntime::fire(int64_t p_uid, const String &p_name, const String &p_to, const Array &p_args) {
	// Types are rejected when written, but array contents are known only at run time.
	// Rather than silently dropping the signal, tell the author which signal was dropped
	if (!MVFrame::plain(p_args)) {
		WARN_PRINT(vformat(U"Online: the arguments of signal %s cannot be sent. Node, Resource, Callable and Signal cannot be sent. This signal is dropped", p_name));
		return;
	}
	Dictionary one;
	one["uid"] = p_uid;
	one["name"] = p_name;
	one["args"] = p_args;
	if (p_to.is_empty()) {
		if (fires.size() < FIRE_MAX) {
			fires.push_back(one);
		} else {
			overflowed(p_name);
		}
		return;
	}
	Array own = fires_my.get(p_to, Array());
	if (own.size() >= FIRE_MAX) {
		overflowed(p_name);
		return;
	}
	own.push_back(one);
	fires_my[p_to] = own;
}

// Returns a Node's path seen from the world root
String MVRuntime::where(Node *p_node) const {
	return p_node != nullptr && is_ancestor_of(p_node) ? String(get_path_to(p_node)) : String();
}

// Whether it belongs to the world (under Online). Secret instances moved directly under the runtime and the runtime itself do not
bool MVRuntime::in_world(Node *p_node) const {
	const String path = where(p_node);
	return path == String(MVGate::SERVER) || path.begins_with(String(MVGate::SERVER) + "/");
}

// Returns the owner the Secret assigned to a Node
String MVRuntime::own_of(Node *p_node) const {
	return p_node != nullptr ? String(p_node->get_meta(SNAME("own"), String())) : String();
}

// Whether it belongs to the player. Always false on the Online side, which has no player
bool MVRuntime::is_my(Node *p_node) const {
	return !me.is_empty() && own_of(p_node) == me;
}

// Creates a scene under the Server and attaches the author script and initial values
Node *MVRuntime::spawn_node(Node *p_parent, const String &p_kind, const String &p_own, const Dictionary &p_props) {
	// The Client cannot place things in the world. Report why instead of failing silently
	if (!authority) {
		WARN_PRINT(String(U"Online: spawn(\"") + p_kind + U"\") can only run on the Online side. Call it from an @online func");
		return nullptr;
	}
	// The gate checks whether the target is under Online. Which Node's script is calling does not matter
	if (p_parent == nullptr) {
		return nullptr;
	}
	Node *node = gate->prepare(this, p_parent, p_kind, p_own);
	if (node == nullptr) {
		// A misspelled kind, a count or depth limit, or a scene whose root is a Secret.
		// Silently returning null for any of these would leave the author without a single clue
		refused_spawn(p_kind);
		return nullptr;
	}
	// Secrets in the scene have their scripts removed before entering the tree. Online-side scripts are attached after attach
	MVStage::hold_secrets(node);
	const String source = MVClientScript::source_of(p_kind);
	if (!source.is_empty() && !dress(node, p_kind, source, false)) {
		memdelete(node);
		ERR_PRINT(vformat(U"Online: cannot load the script of spawn(\"%s\") for the Online side", p_kind));
		return nullptr;
	}
	// An owned thing's my stays its owner even when not requested, so Online scripts calling it directly get the same person
	if (!p_own.is_empty() && !source.is_empty()) {
		node->set("__ctx", my_of(p_own));
		// Records declared defaults of @online_input, restored here on disconnect
		Dictionary rest;
		const Dictionary marks = node->get_meta(SNAME("mark"), Dictionary());
		for (const KeyValue<Variant, Variant> &mark : marks) {
			if (int(mark.value) & GDScriptOnline::F_INPUT) {
				rest[mark.key] = node->get(mark.key);
			}
		}
		if (!rest.is_empty()) {
			node->set_meta(SNAME("input_rest"), rest);
		}
		MVThing::own_cameras(node, is_my(node)); // On a host device only its own things; on a dedicated server all are disabled
	}
	// Named things keep save values at that named path. Unnamed things get per-launch numbers, so nothing is kept.
	// Duplicate names under one parent are not allowed. If the engine silently renamed one, save values would not return to that path
	if (p_props.has("name")) {
		const String name = p_props["name"];
		if (!MVGate::safe_name(name) || p_parent->has_node(NodePath(name))) {
			memdelete(node);
			WARN_PRINT(vformat(U"Online: spawn(\"%s\") cannot use the name \"%s\". Names are letters, digits and _, and one per parent", p_kind, name));
			return nullptr;
		}
		node->set_name(name);
		node->set_meta(SNAME("named"), true);
	}
	// Spawn position and rotation ride on the reliable spawn notice, so Clients do not start at the origin.
	// Values not marked @online, such as damage or hp, are not included
	Dictionary born;
	for (const Variant &name : p_props.keys()) {
		if (String(name) != "name" && !String(name).begins_with("__") && MVFrame::plain(p_props[name])) {
			bool valid = false;
			node->get(name, &valid);
			if (valid) {
				node->set(name, p_props[name]);
				if (MVRep::spawn_property(StringName(name))) {
					born[name] = p_props[name];
				}
			}
		}
	}
	if (!born.is_empty()) {
		node->set_meta(SNAME("mv_spawn"), born);
	}
	Node *placed = settle_node(p_parent, node, p_own);
	if (placed == nullptr) {
		refused_spawn(p_kind);
	}
	return placed;
}

// Restores save values, adds the Node to the tree through the gate, and completes the Online-side _ready.
// Discards the whole Node if it cannot be placed
Node *MVRuntime::settle_node(Node *p_parent, Node *p_node, const String &p_own) {
	const String path = where(p_parent).path_join(p_node->get_name());
	// attach runs _ready. Values the author reads in _ready must be save values, not defaults
	restore_in(p_node, path, p_own);
	begin_path(path);
	const bool attached = gate->attach(this, p_parent, p_node);
	end();
	if (!attached) {
		memdelete(p_node);
		return nullptr;
	}
	live[path] = p_node->get_instance_id();
	dress_secrets(p_node);
	// Setup is called by the engine during attach, which runs the shared scripts and then the host scripts
	return p_node;
}

// Passes queue_free through the gate for Server rights and fixed Node protection
bool MVRuntime::queue_node(Node *p_node) {
	const String path = where(p_node);
	if (!MVGate::from_server(path)) {
		return false;
	}
	// The Client cannot remove. Only the Online side can
	if (!authority) {
		WARN_PRINT(String(U"Online: only the Online side can remove synced nodes: ") + path);
		return true;
	}
	// Do not count something already scheduled for deletion twice. A hit signal can arrive twice
	// in the same frame, so the second one passes and does nothing
	if (p_node->is_queued_for_deletion()) {
		return true;
	}
	// Placing and removing weigh the same. All author scripts on the Online side are equally trusted,
	// so like spawn(), removal is allowed from any Node's @online func
	if (p_node == server_node()) {
		WARN_PRINT(U"Online: Online cannot be removed");
		return true;
	}
	gate->destroy(this, server_node(), p_node);
	return true;
}

// free() removes immediately as in ordinary Godot. Detaching from the tree through the gate
// is done here, and the engine continues with the release itself.
// Returns whether it was refused. Only a refusal stops the release
bool MVRuntime::free_node(Node *p_node) {
	if (!authority) {
		return queue_node(p_node);
	}
	const String path = where(p_node);
	if (!MVGate::from_server(path)) {
		return false;
	}
	if (p_node == server_node()) {
		WARN_PRINT(U"Online: Online cannot be removed");
		return true;
	}
	gate->detach(this, server_node(), p_node);
	return false;
}

// Runs a Secret function called through an entry on the Secret instance outside the world.
// The answer always returns on the next idle. Authors receive it with await,
// so the code and the waiting are the same in a single process and on a host
Signal MVRuntime::secret_call(Node *p_door, const String &p_name, const Array &p_args) {
	// The Client world has no Secret contents
	if (!authority) {
		ERR_PRINT(vformat(U"Online: %s() can only be called on the Online side", p_name));
		return Signal();
	}
	Ref<MVReply> reply;
	reply.instantiate();
	const Signal waiting(reply.ptr(), SNAME("__done"));
	// Keep the reply holder alive until the answer returns
	reply->hold();
	const String path = where(p_door);
	// A Secret whose instance lives in another process is asked, then its answer is awaited.
	// For the author the code stays the same await as when it is local
	if (remote_secrets.has(path)) {
		const Dictionary checked = secret_args(door_api.get(path, Dictionary()), p_name, p_args);
		if (!bool(checked.get("ok", false))) {
			reply->call_deferred(SNAME("__finish"), Variant());
			return waiting;
		}
		const int64_t id = ++secret_asked;
		secret_waiting[id] = reply;
		if (!secret_line->call_secret(id, path, p_name, checked.get("args", p_args))) {
			secret_waiting.erase(id);
			ERR_PRINT(vformat(U"Online: cannot ask Secret for %s(). The connection to Secret is down", p_name));
			reply->call_deferred(SNAME("__finish"), Variant());
		}
		return waiting;
	}
	run_secret(path, p_name, p_args, callable_mp(this, &MVRuntime::reply_later).bind(reply));
	return waiting;
}

void MVRuntime::reply_later(const Variant &p_value, const Variant &p_reply) {
	answer(p_reply, p_value);
}

// Calls a local Secret instance directly and hands only network-sendable answers to p_done. If the script awaits, after the answer is ready
void MVRuntime::run_secret(const String &p_path, const String &p_name, const Array &p_args, const Callable &p_done) {
	const HashMap<String, ObjectID>::ConstIterator found = secrets.find(p_path);
	Node *inner = found ? Object::cast_to<Node>(ObjectDB::get_instance(found->value)) : nullptr;
	if (inner == nullptr) {
		ERR_PRINT(vformat(U"Online: no Secret found at %s", p_path));
		finish(Variant(), p_name, p_done);
		return;
	}
	const Dictionary checked = secret_args(inner->get_meta(SNAME("fns"), Dictionary()), p_name, p_args);
	if (!bool(checked.get("ok", false))) {
		finish(Variant(), p_name, p_done);
		return;
	}
	begin(inner);
	const Variant out = inner->callv(p_name, checked.get("args", p_args));
	end();
	Object *state = out;
	if (state != nullptr && state->is_class("GDScriptFunctionState")) {
		state->connect(SNAME("completed"), callable_mp(this, &MVRuntime::finish).bind(p_name, p_done), CONNECT_ONE_SHOT);
		return;
	}
	finish(out, p_name, p_done);
}

// Binds the peer holding Secret instances and the functions callable there
void MVRuntime::set_secret_line(MVSecretLine *p_line, const Dictionary &p_doors) {
	secret_line = p_line;
	door_api = p_doors;
}

// Returns the externally callable Secret entry declarations per world path
Dictionary MVRuntime::secret_doors() const {
	Dictionary out;
	for (const KeyValue<String, ObjectID> &one : secrets) {
		Node *inner = Object::cast_to<Node>(ObjectDB::get_instance(one.value));
		if (inner == nullptr) {
			continue;
		}
		const Dictionary marks = inner->get_meta(SNAME("mark"), Dictionary());
		const Dictionary fns = inner->get_meta(SNAME("fns"), Dictionary());
		Dictionary doors;
		for (const Variant &field : marks.get_key_list()) {
			if (int(marks[field]) & GDScriptOnline::F_SECRET) {
				doors[field] = fns.get(field, Dictionary());
			}
		}
		out[one.key] = doors;
	}
	return out;
}

// Hands a requested Secret answer to its waiting reply holder
void MVRuntime::secret_answered(int64_t p_id, const Variant &p_value) {
	const HashMap<int64_t, Ref<MVReply>>::Iterator found = secret_waiting.find(p_id);
	if (!found) {
		return;
	}
	const Ref<MVReply> reply = found->value;
	secret_waiting.erase(p_id);
	reply->call_deferred(SNAME("__finish"), p_value);
}

// When the link to the Secret is lost, closes waiting awaits empty.
// Otherwise the author's processing would stay stuck there
void MVRuntime::secret_lost() {
	Vector<Ref<MVReply>> waiting;
	for (const KeyValue<int64_t, Ref<MVReply>> &one : secret_waiting) {
		waiting.push_back(one.value);
	}
	secret_waiting.clear();
	for (const Ref<MVReply> &one : waiting) {
		one->call_deferred(SNAME("__finish"), Variant());
	}
}

// Stores only the @online_save values written by Secrets without running the world
void MVRuntime::tick_secret() {
	if (rep.is_null()) {
		return;
	}
	Dictionary out;
	for (const KeyValue<String, ObjectID> &one : secrets) {
		if (Node *inner = Object::cast_to<Node>(ObjectDB::get_instance(one.value))) {
			rep->save_secret(one.key, inner, out);
		}
	}
	save_out(out);
}

// Lets only the spawn and queue_free C++ gates change the parent-child structure under the Server
bool MVRuntime::tree_node(Node *p_parent, Node *p_child) const {
	const String parent = where(p_parent);
	const String child = where(p_child);
	if (!MVGate::from_server(parent) && !MVGate::from_server(child)) {
		return false;
	}
	WARN_PRINT(U"Online: the tree under Online can only be changed by spawn() and queue_free() on the Online side");
	return true;
}

// Switches to Server rights only while inside a native Server method
void MVRuntime::begin_server_call() {
	server_stack.push_back(current);
	current = MVGate::SERVER;
}

// Returns to the position after leaving a native Server method
void MVRuntime::end_server_call() {
	current = server_stack.is_empty() ? String() : server_stack[server_stack.size() - 1];
	if (!server_stack.is_empty()) {
		server_stack.resize(server_stack.size() - 1);
	}
}

// Appends one-shot operations to the fixed-length queue in call order
void MVRuntime::push_event(int64_t p_uid, const String &p_fn, const Array &p_args, const Ref<MVReply> &p_reply) {
	if (events.size() >= QUEUE_MAX) {
		// Overflow is not silently dropped. Issuing dozens of commands per frame
		// is usually a mistake, so tell the author once in a noticeable way
		if (!warned_full) {
			warned_full = true;
			WARN_PRINT(vformat(U"Online: at most %d commands can be sent per frame. %s() and later were not delivered", (int)QUEUE_MAX, p_fn));
		}
		answer(p_reply, Variant());
		return;
	}
	Dictionary one;
	one["uid"] = p_uid;
	one["fn"] = p_fn;
	one["args"] = p_args.duplicate(true);
	one["reply"] = p_reply;
	events.push_back(one);
}

// Creates one reply holder. It keeps itself alive until the answer returns
Ref<MVReply> MVRuntime::new_reply() {
	Ref<MVReply> made;
	made.instantiate();
	made->hold();
	return made;
}

// Returns an answer to the waiting script. Does nothing without a receiver
void MVRuntime::answer(const Variant &p_reply, const Variant &p_value) {
	Ref<MVReply> box = p_reply;
	if (box.is_valid()) {
		box->call_deferred(SNAME("__finish"), p_value);
	}
}

// Node arguments are replaced by serial numbers shared by both sides.
// A reference means nothing in the other world, but a number points to the same thing
Array MVRuntime::pack_nodes(const Array &p_args) {
	Array out = p_args.duplicate();
	for (int i = 0; i < out.size(); i++) {
		Node *one = Object::cast_to<Node>(out[i]);
		if (one == nullptr) {
			continue;
		}
		const int64_t uid = one->get_meta(SNAME("uid"), 0);
		if (uid <= 0) {
			ERR_PRINT(vformat(U"Online: %s is not part of the world and cannot be passed", String(one->get_name())));
			out[i] = Variant();
			continue;
		}
		Dictionary marker;
		marker[MVFrame::NODE_KEY] = uid;
		out[i] = marker;
	}
	return out;
}

// Rather than silently dropping an unsendable command, tells the author which function and what is wrong
bool MVRuntime::sane_call(const String &p_fn, const Array &p_args) {
	// Names and argument counts only arrive in forms the parser accepted. Only contents are known just at run time
	if (MVFrame::plain(p_args)) {
		return true;
	}
	ERR_PRINT(vformat(U"Online: the arguments of %s() cannot be sent. Node, Resource, Callable and Signal cannot be sent", p_fn));
	return false;
}

// Removes a continuous input record; when switching back to one-shot, keeps the last value in order
void MVRuntime::drop_stream(const String &p_fn, bool p_keep) {
	if (!streams.has(p_fn)) {
		return;
	}
	if (p_keep) {
		const Dictionary one = streams[p_fn];
		push_event(one["uid"], p_fn, one["args"], stream_replies.get(p_fn, Variant()));
	} else {
		answer(stream_replies.get(p_fn, Variant()), Variant());
	}
	stream_replies.erase(p_fn);
	streams.erase(p_fn);
	const int at = stream_order.find(p_fn);
	if (at >= 0) {
		stream_order.remove_at(at);
	}
}

// Keeps calls outside _process as reliable events regardless of count
Signal MVRuntime::ask(int64_t p_uid, const String &p_fn, const Array &p_args) {
	Ref<MVReply> reply = new_reply();
	const Array args = pack_nodes(p_args);
	if (!sane_call(p_fn, args)) {
		answer(reply, Variant());
		return Signal(reply.ptr(), SNAME("__done"));
	}
	drop_stream(p_fn, false);
	push_event(p_uid, p_fn, args, reply);
	return Signal(reply.ptr(), SNAME("__done"));
}

// Keeps only the latest value per function for _process input, without piling up old values
Signal MVRuntime::ask_latest(int64_t p_uid, const String &p_fn, const Array &p_args) {
	Ref<MVReply> reply = new_reply();
	const Array args = pack_nodes(p_args);
	if (!sane_call(p_fn, args)) {
		answer(reply, Variant());
		return Signal(reply.ptr(), SNAME("__done"));
	}
	if (!streams.has(p_fn)) {
		stream_order.push_back(p_fn);
	}
	// Old input for the same function is dropped, and its answer closed empty.
	// Otherwise a script waiting on await would never return
	answer(stream_replies.get(p_fn, Variant()), Variant());
	Dictionary one;
	one["uid"] = p_uid;
	one["args"] = args.duplicate(true);
	streams[p_fn] = one;
	stream_replies[p_fn] = reply;
	return Signal(reply.ptr(), SNAME("__done"));
}

// Events come first; continuous input sends only latest values at 20Hz. The destination is the parent Client
bool MVRuntime::flush_commands() {
	MVClient *target = Object::cast_to<MVClient>(get_parent());
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	while (!sent_at.is_empty() && now - uint64_t(sent_at[0]) >= MVLink::RATE_MS) {
		sent_at.remove_at(0);
	}
	if (target == nullptr || sent_at.size() >= MVLink::RATE_MAX) {
		return false;
	}
	if (!events.is_empty()) {
		const Dictionary one = events[0];
		if (!target->ask(one.get("uid", 0), one.get("fn", String()), one.get("args", Array()), one.get("reply", Variant()))) {
			return false;
		}
		events.remove_at(0);
		sent_at.push_back(now);
		return true;
	}
	// @online_input on the player's own things sends only changes, batched into one item at 20Hz. No answer is needed
	if (!me.is_empty() && now - input_at >= STREAM_MS) {
		input_at = now;
		Dictionary changed;
		for (Node *one : owned(me)) {
			const int64_t uid = one->get_meta(SNAME("uid"), 0);
			const Dictionary marks = one->get_meta(SNAME("mark"), Dictionary());
			Dictionary &sent_before = input_sent[uid];
			Dictionary fields;
			for (const KeyValue<Variant, Variant> &mark : marks) {
				if (int(mark.value) & GDScriptOnline::F_INPUT) {
					const Variant value = one->get(mark.key);
					if (!sent_before.has(mark.key) || sent_before[mark.key] != value) {
						fields[mark.key] = value;
						sent_before[mark.key] = value;
					}
				}
			}
			if (!fields.is_empty()) {
				changed[uid] = fields;
			}
		}
		Array packed;
		packed.push_back(changed);
		if (!changed.is_empty() && target->ask(0, "=", packed, Variant())) {
			sent_at.push_back(now);
		}
	}
	if (stream_order.is_empty() || now - stream_at < STREAM_MS) {
		return false;
	}
	// Sends every pending function this round. Rotating one at a time would delay delivery by the number of functions
	bool sent = false;
	while (!stream_order.is_empty() && sent_at.size() < MVLink::RATE_MAX) {
		const String fn = stream_order[0];
		const Dictionary one = streams.get(fn, Dictionary());
		if (!target->ask(one.get("uid", 0), fn, one.get("args", Array()), stream_replies.get(fn, Variant()))) {
			break;
		}
		streams.erase(fn);
		stream_replies.erase(fn);
		stream_order.remove_at(0);
		sent_at.push_back(now);
		sent = true;
	}
	if (sent) {
		stream_at = now;
	}
	return sent;
}

// Empties the send queue so old events are not run across connections
void MVRuntime::clear_commands() {
	// Closes answers of commands that will never be sent as empty. Otherwise await would never return
	for (const Variant &one : events) {
		answer(Dictionary(one).get("reply", Variant()), Variant());
	}
	for (const Variant &key : stream_replies.get_key_list()) {
		answer(stream_replies[key], Variant());
	}
	stream_replies.clear();
	events.clear();
	streams.clear();
	stream_order.clear();
	sent_at.clear();
	stream_at = 0;
	input_sent.clear();
}

// Internal class, so nothing is exposed to authors or GDScript
void MVRuntime::_bind_methods() {
}
