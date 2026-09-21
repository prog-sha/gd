/**************************************************************************/
/*  scene_tree.cpp                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "scene_tree.h"
#include "cli/run/guard.h"

STATIC_ASSERT_INCOMPLETE_TYPE(class, RenderingServer);

#include "cli/sys/wait.h"
#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "scene/main/multiplayer_api.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

#ifndef _3D_DISABLED
#endif // _3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
#include "servers/physics_2d/physics_server_2d.h"
#endif // PHYSICS_2D_DISABLED

#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif // PHYSICS_3D_DISABLED

void SceneTreeTimer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_time_left", "time"), &SceneTreeTimer::set_time_left);
	ClassDB::bind_method(D_METHOD("get_time_left"), &SceneTreeTimer::get_time_left);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_left", PROPERTY_HINT_NONE, "suffix:s"), "set_time_left", "get_time_left");

	ADD_SIGNAL(MethodInfo("timeout"));
}

void SceneTreeTimer::set_time_left(double p_time) {
	time_left = p_time;
}

double SceneTreeTimer::get_time_left() const {
	return MAX(time_left, 0.0);
}

void SceneTreeTimer::set_process_always(bool p_process_always) {
	process_always = p_process_always;
}

bool SceneTreeTimer::is_process_always() {
	return process_always;
}

void SceneTreeTimer::set_process_in_physics(bool p_process_in_physics) {
	process_in_physics = p_process_in_physics;
}

bool SceneTreeTimer::is_process_in_physics() {
	return process_in_physics;
}

void SceneTreeTimer::set_ignore_time_scale(bool p_ignore) {
	ignore_time_scale = p_ignore;
}

bool SceneTreeTimer::is_ignoring_time_scale() {
	return ignore_time_scale;
}

void SceneTreeTimer::release_connections() {
	List<Connection> signal_connections;
	get_all_signal_connections(&signal_connections);

	for (const Connection &connection : signal_connections) {
		disconnect(connection.signal.get_name(), connection.callable);
	}
}

#ifndef _3D_DISABLED
// This should be called once per physics tick, to make sure the transform previous and current
// is kept up to date on the few Node3Ds that are using client side physics interpolation.
// Provide headless stubs for optional game features.
#endif // _3D_DISABLED


void SceneTree::tree_changed() {
	emit_signal(tree_changed_name);
}

void SceneTree::node_added(Node *p_node) {
	emit_signal(node_added_name, p_node);
}

void SceneTree::node_removed(Node *p_node) {
	// Nodes can only be removed from the main thread.
	if (current_scene == p_node) {
		current_scene = nullptr;
	}
	emit_signal(node_removed_name, p_node);
	if (nodes_removed_on_group_call_lock) {
		nodes_removed_on_group_call.insert(p_node);
	}
}

void SceneTree::node_renamed(Node *p_node) {
	emit_signal(node_renamed_name, p_node);
}

SceneTreeGroup *SceneTree::add_to_group(const StringName &p_group, Node *p_node) {
	_THREAD_SAFE_METHOD_

	HashMap<StringName, SceneTreeGroup>::Iterator E = group_map.find(p_group);
	if (!E) {
		E = group_map.insert(p_group, SceneTreeGroup());
	}

	ERR_FAIL_COND_V_MSG(E->value.nodes.has(p_node), &E->value, "Already in group: " + p_group + ".");
	E->value.nodes.push_back(p_node);
	E->value.changed = true;
	return &E->value;
}

void SceneTree::remove_from_group(const StringName &p_group, Node *p_node) {
	_THREAD_SAFE_METHOD_

	HashMap<StringName, SceneTreeGroup>::Iterator E = group_map.find(p_group);
	ERR_FAIL_COND(!E);

	E->value.nodes.erase(p_node);
	if (E->value.nodes.is_empty()) {
		group_map.remove(E);
	}
}

void SceneTree::flush_transform_notifications() {
	_THREAD_SAFE_METHOD_

	SelfList<Node> *n = xform_change_list.first();
	while (n) {
		Node *node = n->self();
		SelfList<Node> *nx = n->next();
		xform_change_list.remove(n);
		n = nx;
		node->notification(NOTIFICATION_TRANSFORM_CHANGED);
	}
}

bool SceneTree::is_accessibility_enabled() const { return false; } // Keep accessibility support in the optional game layer.

bool SceneTree::is_accessibility_supported() const { return false; }

// Keep accessibility support in the optional game layer.

// Keep accessibility support in the optional game layer.

// Provide a headless stub for an optional game feature.

// Keep accessibility support in the optional game layer.

void SceneTree::_flush_ugc() {
	ugc_locked = true;

	while (unique_group_calls.size()) {
		HashMap<UGCall, Vector<Variant>, UGCall>::Iterator E = unique_group_calls.begin();

		const Variant **argptrs = (const Variant **)alloca(E->value.size() * sizeof(Variant *));

		for (int i = 0; i < E->value.size(); i++) {
			argptrs[i] = &E->value[i];
		}

		call_group_flagsp(GROUP_CALL_DEFAULT, E->key.group, E->key.call, argptrs, E->value.size());

		unique_group_calls.remove(E);
	}

	ugc_locked = false;
}

void SceneTree::_update_group_order(SceneTreeGroup &g) {
	if (!g.changed) {
		return;
	}
	if (g.nodes.is_empty()) {
		return;
	}

	Node **gr_nodes = g.nodes.ptrw();
	int gr_node_count = g.nodes.size();

	SortArray<Node *, Node::Comparator> node_sort;
	node_sort.sort(gr_nodes, gr_node_count);

	g.changed = false;
}

RequiredResult<Node> SceneTree::get_root() const {
	return root;
}

void SceneTree::call_group_flagsp(uint32_t p_call_flags, const StringName &p_group, const StringName &p_function, const Variant **p_args, int p_argcount) {
	Vector<Node *> nodes_copy;

	{
		_THREAD_SAFE_METHOD_

		HashMap<StringName, SceneTreeGroup>::Iterator E = group_map.find(p_group);
		if (!E) {
			return;
		}
		SceneTreeGroup &g = E->value;
		if (g.nodes.is_empty()) {
			return;
		}

		if (p_call_flags & GROUP_CALL_UNIQUE && p_call_flags & GROUP_CALL_DEFERRED) {
			ERR_FAIL_COND(ugc_locked);

			UGCall ug;
			ug.call = p_function;
			ug.group = p_group;

			if (unique_group_calls.has(ug)) {
				return;
			}

			Vector<Variant> args;
			for (int i = 0; i < p_argcount; i++) {
				args.push_back(*p_args[i]);
			}

			unique_group_calls[ug] = args;
			return;
		}

		_update_group_order(g);
		nodes_copy = g.nodes;
	}

	Node **gr_nodes = nodes_copy.ptrw();
	int gr_node_count = nodes_copy.size();

	{
		_THREAD_SAFE_METHOD_
		nodes_removed_on_group_call_lock++;
	}

	if (p_call_flags & GROUP_CALL_REVERSE) {
		for (int i = gr_node_count - 1; i >= 0; i--) {
			if (nodes_removed_on_group_call_lock && nodes_removed_on_group_call.has(gr_nodes[i])) {
				continue;
			}

			Node *node = gr_nodes[i];
			if (!(p_call_flags & GROUP_CALL_DEFERRED)) {
				Callable::CallError ce;
				node->callp(p_function, p_args, p_argcount, ce);
				if (unlikely(ce.error != Callable::CallError::CALL_OK && ce.error != Callable::CallError::CALL_ERROR_INVALID_METHOD)) {
					ERR_PRINT(vformat("Error calling group method on node \"%s\": %s.", node->get_name(), Variant::get_callable_error_text(Callable(node, p_function), p_args, p_argcount, ce)));
				}
			} else {
				MessageQueue::get_singleton()->push_callp(node, p_function, p_args, p_argcount);
			}
		}

	} else {
		for (int i = 0; i < gr_node_count; i++) {
			if (nodes_removed_on_group_call_lock && nodes_removed_on_group_call.has(gr_nodes[i])) {
				continue;
			}

			Node *node = gr_nodes[i];
			if (!(p_call_flags & GROUP_CALL_DEFERRED)) {
				Callable::CallError ce;
				node->callp(p_function, p_args, p_argcount, ce);
				if (unlikely(ce.error != Callable::CallError::CALL_OK && ce.error != Callable::CallError::CALL_ERROR_INVALID_METHOD)) {
					ERR_PRINT(vformat("Error calling group method on node \"%s\": %s.", node->get_name(), Variant::get_callable_error_text(Callable(node, p_function), p_args, p_argcount, ce)));
				}
			} else {
				MessageQueue::get_singleton()->push_callp(node, p_function, p_args, p_argcount);
			}
		}
	}

	{
		_THREAD_SAFE_METHOD_
		nodes_removed_on_group_call_lock--;
		if (nodes_removed_on_group_call_lock == 0) {
			nodes_removed_on_group_call.clear();
		}
	}
}

void SceneTree::notify_group_flags(uint32_t p_call_flags, const StringName &p_group, int p_notification) {
	Vector<Node *> nodes_copy;
	{
		_THREAD_SAFE_METHOD_
		HashMap<StringName, SceneTreeGroup>::Iterator E = group_map.find(p_group);
		if (!E) {
			return;
		}
		SceneTreeGroup &g = E->value;
		if (g.nodes.is_empty()) {
			return;
		}

		_update_group_order(g);

		nodes_copy = g.nodes;
	}

	Node **gr_nodes = nodes_copy.ptrw();
	int gr_node_count = nodes_copy.size();

	{
		_THREAD_SAFE_METHOD_
		nodes_removed_on_group_call_lock++;
	}

	if (p_call_flags & GROUP_CALL_REVERSE) {
		for (int i = gr_node_count - 1; i >= 0; i--) {
			if (nodes_removed_on_group_call.has(gr_nodes[i])) {
				continue;
			}

			if (!(p_call_flags & GROUP_CALL_DEFERRED)) {
				gr_nodes[i]->notification(p_notification, true);
			} else {
				MessageQueue::get_singleton()->push_notification(gr_nodes[i], p_notification);
			}
		}

	} else {
		for (int i = 0; i < gr_node_count; i++) {
			if (nodes_removed_on_group_call.has(gr_nodes[i])) {
				continue;
			}

			if (!(p_call_flags & GROUP_CALL_DEFERRED)) {
				gr_nodes[i]->notification(p_notification);
			} else {
				MessageQueue::get_singleton()->push_notification(gr_nodes[i], p_notification);
			}
		}
	}

	{
		_THREAD_SAFE_METHOD_
		nodes_removed_on_group_call_lock--;
		if (nodes_removed_on_group_call_lock == 0) {
			nodes_removed_on_group_call.clear();
		}
	}
}

void SceneTree::set_group_flags(uint32_t p_call_flags, const StringName &p_group, const String &p_name, const Variant &p_value) {
	Vector<Node *> nodes_copy;
	{
		_THREAD_SAFE_METHOD_

		HashMap<StringName, SceneTreeGroup>::Iterator E = group_map.find(p_group);
		if (!E) {
			return;
		}
		SceneTreeGroup &g = E->value;
		if (g.nodes.is_empty()) {
			return;
		}

		_update_group_order(g);

		nodes_copy = g.nodes;
	}
	Node **gr_nodes = nodes_copy.ptrw();
	int gr_node_count = nodes_copy.size();

	{
		_THREAD_SAFE_METHOD_
		nodes_removed_on_group_call_lock++;
	}

	if (p_call_flags & GROUP_CALL_REVERSE) {
		for (int i = gr_node_count - 1; i >= 0; i--) {
			if (nodes_removed_on_group_call.has(gr_nodes[i])) {
				continue;
			}

			if (!(p_call_flags & GROUP_CALL_DEFERRED)) {
				gr_nodes[i]->set(p_name, p_value);
			} else {
				MessageQueue::get_singleton()->push_set(gr_nodes[i], p_name, p_value);
			}
		}

	} else {
		for (int i = 0; i < gr_node_count; i++) {
			if (nodes_removed_on_group_call.has(gr_nodes[i])) {
				continue;
			}

			if (!(p_call_flags & GROUP_CALL_DEFERRED)) {
				gr_nodes[i]->set(p_name, p_value);
			} else {
				MessageQueue::get_singleton()->push_set(gr_nodes[i], p_name, p_value);
			}
		}
	}

	{
		_THREAD_SAFE_METHOD_
		nodes_removed_on_group_call_lock--;
		if (nodes_removed_on_group_call_lock == 0) {
			nodes_removed_on_group_call.clear();
		}
	}
}

void SceneTree::notify_group(const StringName &p_group, int p_notification) {
	notify_group_flags(GROUP_CALL_DEFAULT, p_group, p_notification);
}

void SceneTree::set_group(const StringName &p_group, const String &p_name, const Variant &p_value) {
	set_group_flags(GROUP_CALL_DEFAULT, p_group, p_name, p_value);
}

void SceneTree::initialize() {
	GodotProfileZone("SceneTree::initialize");
	ERR_FAIL_NULL(root);
	MainLoop::initialize();
	root->_set_tree(this);
}

// Provide a headless stub for an optional game feature.

#ifndef _3D_DISABLED
// Provide a headless stub for an optional game feature.

// Provide a headless stub for an optional game feature.
#endif

void SceneTree::iteration_prepare() {} // Keep frame interpolation in the optional game layer.

bool SceneTree::physics_process(double p_time) {
	current_frame++;

	flush_transform_notifications();

	if (MainLoop::physics_process(p_time)) {
		_quit = true;
	}
	physics_process_time = p_time;

	emit_signal(SNAME("physics_frame"));

#if !defined(PHYSICS_2D_DISABLED) || !defined(PHYSICS_3D_DISABLED)
	call_group(SNAME("_picking_viewports"), SNAME("_process_picking"));
#endif // !defined(PHYSICS_2D_DISABLED) || !defined(PHYSICS_3D_DISABLED)

	_process(true);

	_flush_ugc();
	MessageQueue::get_singleton()->flush(); //small little hack

	process_timers(p_time, true); //go through timers

	flush_transform_notifications();

	// This should happen last because any processing that deletes something beforehand might expect the object to be removed in the same frame.
	_flush_delete_queue();

	_call_idle_callbacks();

	return _quit;
}

void SceneTree::iteration_end() {} // Keep frame interpolation in the optional game layer.

bool SceneTree::process(double p_time) {
	if (MainLoop::process(p_time)) {
		_quit = true;
	}

	process_time = p_time;

	if (multiplayer_poll) {
		multiplayer->poll();
		for (KeyValue<NodePath, Ref<MultiplayerAPI>> &E : custom_multiplayers) {
			E.value->poll();
		}
	}

	emit_signal(SNAME("process_frame"));

	MessageQueue::get_singleton()->flush(); //small little hack

	flush_transform_notifications();

	_process(false);

	_flush_ugc();
	MessageQueue::get_singleton()->flush(); //small little hack
	flush_transform_notifications(); //transforms after world update, to avoid unnecessary enter/exit notifications

	if (unlikely(pending_new_scene_id.is_valid())) {
		_flush_scene_change();
	}

	process_timers(p_time, false); //go through timers

	flush_transform_notifications(); // Additional transforms after timers update.

	// This should happen last because any processing that deletes something beforehand might expect the object to be removed in the same frame.
	_flush_delete_queue();

	_flush_accessibility_changes();

	_call_idle_callbacks();

#ifdef TOOLS_ENABLED
#ifndef _3D_DISABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		String env_path = GLOBAL_GET("rendering/environment/defaults/default_environment");
		env_path = env_path.strip_edges(); // User may have added a space or two.

		bool can_load = true;
		if (env_path.begins_with("uid://")) {
			// If an uid path, ensure it is mapped to a resource which could not be
			// the case if the editor is still scanning the filesystem.
			ResourceUID::ID id = ResourceUID::get_singleton()->text_to_id(env_path);
			can_load = ResourceUID::get_singleton()->has_id(id);
			if (can_load) {
				env_path = ResourceUID::get_singleton()->get_id_path(id);
			}
		}

		if (can_load) {
			String cpath;
			Ref<Environment> fallback = get_root()->get_world_3d()->get_fallback_environment();
			if (fallback.is_valid()) {
				cpath = fallback->get_path();
			}
			if (cpath != env_path) {
				if (!env_path.is_empty()) {
					fallback = ResourceLoader::load(env_path);
					if (fallback.is_null()) {
						//could not load fallback, set as empty
						ProjectSettings::get_singleton()->set("rendering/environment/defaults/default_environment", "");
					}
				} else {
					fallback.unref();
				}
				get_root()->get_world_3d()->set_fallback_environment(fallback);
			}
		}
	}
#endif // _3D_DISABLED
#endif // TOOLS_ENABLED

	return _quit;
}

void SceneTree::process_timers(double p_delta, bool p_physics_frame) {
	_THREAD_SAFE_METHOD_
	const List<Ref<SceneTreeTimer>>::Element *L = timers.back(); // Last element.
	const double unscaled_delta = Engine::get_singleton()->get_process_step();

	for (List<Ref<SceneTreeTimer>>::Element *E = timers.front(); E;) {
		List<Ref<SceneTreeTimer>>::Element *N = E->next();
		Ref<SceneTreeTimer> timer = E->get();

		if ((paused && !timer->is_process_always()) || (timer->is_process_in_physics() != p_physics_frame)) {
			if (E == L) {
				break; // Break on last, so if new timers were added during list traversal, ignore them.
			}
			E = N;
			continue;
		}

		double time_left = timer->get_time_left();
		time_left -= timer->is_ignoring_time_scale() ? unscaled_delta : p_delta;
		timer->set_time_left(time_left);

		if (time_left <= 0) {
			E->get()->emit_signal(SNAME("timeout"));
			timers.erase(E);
		}
		if (E == L) {
			break; // Break on last, so if new timers were added during list traversal, ignore them.
		}
		E = N;
	}
}

// Provide a headless stub for an optional game feature.

void SceneTree::finalize() {
	_flush_delete_queue();

	_flush_ugc();

	if (root) {
		root->_set_tree(nullptr);
		root->_propagate_after_exit_tree();
		memdelete(root); //delete root
		root = nullptr;

		// In case deletion of some objects was queued when destructing the `root`.
		// E.g. if `queue_free()` was called for some node outside the tree when handling NOTIFICATION_PREDELETE for some node in the tree.
		_flush_delete_queue();
	}

	MainLoop::finalize();

	// Cleanup timers.
	for (Ref<SceneTreeTimer> &timer : timers) {
		timer->release_connections();
	}
	timers.clear();

}

void SceneTree::quit(int p_exit_code) {
	_THREAD_SAFE_METHOD_

	OS::get_singleton()->set_exit_code(p_exit_code);
	_quit = true;
}

void SceneTree::_main_window_close() {
	if (accept_quit) {
		_quit = true;
	}
}

void SceneTree::_main_window_go_back() {
	if (quit_on_go_back) {
		_quit = true;
	}
}

void SceneTree::_main_window_focus_in() {
	Input *id = Input::get_singleton();
	if (id) {
		id->ensure_touch_mouse_raised();
	}
}

void SceneTree::_notification(int p_notification) {
	if (!get_root()) {
		return;
	}

	switch (p_notification) {
		case NOTIFICATION_TRANSLATION_CHANGED: {
			get_root()->propagate_notification(p_notification);
		} break;

		case NOTIFICATION_OS_MEMORY_WARNING:
		case NOTIFICATION_OS_IME_UPDATE:
		case NOTIFICATION_WM_ABOUT:
		case NOTIFICATION_CRASH:
		case NOTIFICATION_APPLICATION_RESUMED:
		case NOTIFICATION_APPLICATION_PAUSED:
		case NOTIFICATION_APPLICATION_PIP_MODE_ENTERED:
		case NOTIFICATION_APPLICATION_PIP_MODE_EXITED: {
			// Pass these to nodes, since they are mirrored.
			get_root()->propagate_notification(p_notification);
		} break;

		case NOTIFICATION_APPLICATION_FOCUS_IN:
		case NOTIFICATION_APPLICATION_FOCUS_OUT: {
			if (Input::get_singleton()) {
				Input::get_singleton()->application_focused = p_notification == NOTIFICATION_APPLICATION_FOCUS_IN;

				// `release_pressed_events()` already preserves joypad state when the
				// unfocused joypad setting is disabled, but keyboard state still needs
				// to be released after focus loss.
				Input::get_singleton()->release_pressed_events();
			}

			// Pass these to nodes, since they are mirrored.
			get_root()->propagate_notification(p_notification);
		} break;
	}
}

bool SceneTree::is_auto_accept_quit() const {
	return accept_quit;
}

void SceneTree::set_auto_accept_quit(bool p_enable) {
	accept_quit = p_enable;
}

bool SceneTree::is_quit_on_go_back() const {
	return quit_on_go_back;
}

void SceneTree::set_quit_on_go_back(bool p_enable) {
	quit_on_go_back = p_enable;
}

#ifdef DEBUG_ENABLED
void SceneTree::set_debug_collisions_hint(bool p_enabled) {
	debug_collisions_hint = p_enabled;
}

bool SceneTree::is_debugging_collisions_hint() const {
	return debug_collisions_hint;
}

void SceneTree::set_debug_paths_hint(bool p_enabled) {
	debug_paths_hint = p_enabled;
}

bool SceneTree::is_debugging_paths_hint() const {
	return debug_paths_hint;
}

void SceneTree::set_debug_navigation_hint(bool p_enabled) {
	debug_navigation_hint = p_enabled;
}

bool SceneTree::is_debugging_navigation_hint() const {
	return debug_navigation_hint;
}
#endif

void SceneTree::set_debug_collisions_color(const Color &p_color) {
	debug_collisions_color = p_color;
}

Color SceneTree::get_debug_collisions_color() const {
	return debug_collisions_color;
}

void SceneTree::set_debug_collision_contact_color(const Color &p_color) {
	debug_collision_contact_color = p_color;
}

Color SceneTree::get_debug_collision_contact_color() const {
	return debug_collision_contact_color;
}

void SceneTree::set_debug_paths_color(const Color &p_color) {
	debug_paths_color = p_color;
}

Color SceneTree::get_debug_paths_color() const {
	return debug_paths_color;
}

void SceneTree::set_debug_paths_width(float p_width) {
	debug_paths_width = p_width;
}

float SceneTree::get_debug_paths_width() const {
	return debug_paths_width;
}

// Provide a headless stub for an optional game feature.

// Provide a headless stub for an optional game feature.

// Provide a headless stub for an optional game feature.

void SceneTree::set_pause(bool p_enabled) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Pause can only be set from the main thread.");
	ERR_FAIL_COND_MSG(suspended, "Pause state cannot be modified while suspended.");

	if (p_enabled == paused) {
		return;
	}

	paused = p_enabled;

#ifndef PHYSICS_3D_DISABLED
	PhysicsServer3D::get_singleton()->set_active(!p_enabled);
#endif // PHYSICS_3D_DISABLED
#ifndef PHYSICS_2D_DISABLED
	PhysicsServer2D::get_singleton()->set_active(!p_enabled);
#endif // PHYSICS_2D_DISABLED
	if (get_root()) {
		get_root()->_propagate_pause_notification(p_enabled);
	}
}

bool SceneTree::is_paused() const {
	return paused;
}

void SceneTree::set_suspend(bool p_enabled) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Suspend can only be set from the main thread.");

	if (p_enabled == suspended) {
		return;
	}

	suspended = p_enabled;

	Engine::get_singleton()->set_freeze_time_scale(p_enabled);

#ifndef PHYSICS_3D_DISABLED
	PhysicsServer3D::get_singleton()->set_active(!p_enabled && !paused);
#endif // PHYSICS_3D_DISABLED
#ifndef PHYSICS_2D_DISABLED
	PhysicsServer2D::get_singleton()->set_active(!p_enabled && !paused);
#endif // PHYSICS_2D_DISABLED
	if (get_root()) {
		get_root()->_propagate_suspend_notification(p_enabled);
	}
}

bool SceneTree::is_suspended() const {
	return suspended;
}

void SceneTree::_process_group(ProcessGroup *p_group, bool p_physics) {
	// When reading this function, keep in mind that this code must work in a way where
	// if any node is removed, this needs to continue working.

	p_group->call_queue.flush(); // Flush messages before processing.

	Vector<Node *> &nodes = p_physics ? p_group->physics_nodes : p_group->nodes;
	if (nodes.is_empty()) {
		return;
	}

	if (p_physics) {
		if (p_group->physics_node_order_dirty) {
			nodes.sort_custom<Node::ComparatorWithPhysicsPriority>();
			p_group->physics_node_order_dirty = false;
		}
	} else {
		if (p_group->node_order_dirty) {
			nodes.sort_custom<Node::ComparatorWithPriority>();
			p_group->node_order_dirty = false;
		}
	}

	// Make a copy, so if nodes are added/removed from process, this does not break
	Vector<Node *> nodes_copy = nodes;

	uint32_t node_count = nodes_copy.size();
	Node **nodes_ptr = (Node **)nodes_copy.ptr(); // Force cast, pointer will not change.

	for (uint32_t i = 0; i < node_count; i++) {
		Node *n = nodes_ptr[i];
		if (nodes_removed_on_group_call.has(n)) {
			// Node may have been removed during process, skip it.
			// Keep in mind removals can only happen on the main thread.
			continue;
		}

		if (!n->can_process() || !n->is_inside_tree()) {
			continue;
		}

		if (p_physics) {
			if (n->is_physics_processing_internal()) {
				n->notification(Node::NOTIFICATION_INTERNAL_PHYSICS_PROCESS);
			}
			if (n->is_physics_processing()) {
				n->notification(Node::NOTIFICATION_PHYSICS_PROCESS);
			}
		} else {
			if (n->is_processing_internal()) {
				n->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
			}
			if (n->is_processing()) {
				n->notification(Node::NOTIFICATION_PROCESS);
			}
		}
	}

	p_group->call_queue.flush(); // Flush messages also after processing (for potential deferred calls).
}

void SceneTree::_process_groups_thread(uint32_t p_index, bool p_physics) {
	Node::current_process_thread_group = local_process_group_cache[p_index]->owner;
	_process_group(local_process_group_cache[p_index], p_physics);
	Node::current_process_thread_group = nullptr;
}

void SceneTree::_process(bool p_physics) {
	if (process_groups_dirty) {
		{
			// First, remove dirty groups.
			// This needs to be done when not processing to avoid problems.
			ProcessGroup **pg_ptr = (ProcessGroup **)process_groups.ptr(); // discard constness.
			uint32_t pg_count = process_groups.size();

			for (uint32_t i = 0; i < pg_count; i++) {
				if (pg_ptr[i]->removed) {
					// Replace removed with last.
					pg_ptr[i] = pg_ptr[pg_count - 1];
					// Retry
					i--;
					pg_count--;
				}
			}
			if (pg_count != process_groups.size()) {
				process_groups.resize(pg_count);
			}
		}
		{
			// Then, re-sort groups.
			process_groups.sort_custom<ProcessGroupSort>();
		}

		process_groups_dirty = false;
	}

	// Cache the group count, because during processing new groups may be added.
	// They will be added at the end, hence for consistency they will be ignored by this process loop.
	// No group will be removed from the array during processing (this is done earlier in this function by marking the groups dirty).
	uint32_t group_count = process_groups.size();

	if (group_count == 0) {
		return;
	}

	process_last_pass++; // Increment pass
	uint32_t from = 0;
	uint32_t process_count = 0;
	nodes_removed_on_group_call_lock++;

	int current_order = process_groups[0]->owner ? process_groups[0]->owner->data.process_thread_group_order : 0;
	bool current_threaded = process_groups[0]->owner ? process_groups[0]->owner->data.process_thread_group == Node::PROCESS_THREAD_GROUP_SUB_THREAD : false;

	for (uint32_t i = 0; i <= group_count; i++) {
		int order = i < group_count && process_groups[i]->owner ? process_groups[i]->owner->data.process_thread_group_order : 0;
		bool threaded = i < group_count && process_groups[i]->owner ? process_groups[i]->owner->data.process_thread_group == Node::PROCESS_THREAD_GROUP_SUB_THREAD : false;

		if (i == group_count || current_order != order || current_threaded != threaded) {
			if (process_count > 0) {
				// Proceed to process the group.
				bool using_threads = process_groups[from]->owner && process_groups[from]->owner->data.process_thread_group == Node::PROCESS_THREAD_GROUP_SUB_THREAD && !node_threading_disabled;

				if (using_threads) {
					local_process_group_cache.clear();
				}
				for (uint32_t j = from; j < i; j++) {
					if (process_groups[j]->last_pass == process_last_pass) {
						if (using_threads) {
							local_process_group_cache.push_back(process_groups[j]);
						} else {
							_process_group(process_groups[j], p_physics);
						}
					}
				}

				if (using_threads) {
					WorkerThreadPool::GroupID id = WorkerThreadPool::get_singleton()->add_template_group_task(this, &SceneTree::_process_groups_thread, p_physics, local_process_group_cache.size(), -1, true);
					WorkerThreadPool::get_singleton()->wait_for_group_task_completion(id);
				}
			}

			if (i == group_count) {
				// This one is invalid, no longer process
				break;
			}

			from = i;
			current_threaded = threaded;
			current_order = order;
		}

		if (process_groups[i]->removed) {
			continue;
		}

		ProcessGroup *pg = process_groups[i];

		// Validate group for processing
		bool process_valid = false;
		if (p_physics) {
			if (!pg->physics_nodes.is_empty()) {
				process_valid = true;
			} else if ((pg == &default_process_group || (pg->owner != nullptr && pg->owner->data.process_thread_messages.has_flag(Node::FLAG_PROCESS_THREAD_MESSAGES_PHYSICS))) && pg->call_queue.has_messages()) {
				process_valid = true;
			}
		} else {
			if (!pg->nodes.is_empty()) {
				process_valid = true;
			} else if ((pg == &default_process_group || (pg->owner != nullptr && pg->owner->data.process_thread_messages.has_flag(Node::FLAG_PROCESS_THREAD_MESSAGES))) && pg->call_queue.has_messages()) {
				process_valid = true;
			}
		}

		if (process_valid) {
			pg->last_pass = process_last_pass; // Enable for processing
			process_count++;
		}
	}

	nodes_removed_on_group_call_lock--;
	if (nodes_removed_on_group_call_lock == 0) {
		nodes_removed_on_group_call.clear();
	}
}

bool SceneTree::ProcessGroupSort::operator()(const ProcessGroup *p_left, const ProcessGroup *p_right) const {
	int left_order = p_left->owner ? p_left->owner->data.process_thread_group_order : 0;
	int right_order = p_right->owner ? p_right->owner->data.process_thread_group_order : 0;

	if (left_order == right_order) {
		int left_threaded = p_left->owner != nullptr && p_left->owner->data.process_thread_group == Node::PROCESS_THREAD_GROUP_SUB_THREAD ? 0 : 1;
		int right_threaded = p_right->owner != nullptr && p_right->owner->data.process_thread_group == Node::PROCESS_THREAD_GROUP_SUB_THREAD ? 0 : 1;
		return left_threaded < right_threaded;
	} else {
		return left_order < right_order;
	}
}

void SceneTree::_remove_process_group(Node *p_node) {
	_THREAD_SAFE_METHOD_
	ProcessGroup *pg = (ProcessGroup *)p_node->data.process_group;
	ERR_FAIL_NULL(pg);
	ERR_FAIL_COND(pg->removed);
	pg->removed = true;
	pg->owner = nullptr;
	p_node->data.process_group = nullptr;
	process_groups_dirty = true;
}

void SceneTree::_add_process_group(Node *p_node) {
	_THREAD_SAFE_METHOD_
	ERR_FAIL_NULL(p_node);

	ProcessGroup *pg = memnew(ProcessGroup);

	pg->owner = p_node;
	p_node->data.process_group = pg;

	process_groups.push_back(pg);

	process_groups_dirty = true;
}

void SceneTree::_remove_node_from_process_group(Node *p_node, Node *p_owner) {
	_THREAD_SAFE_METHOD_
	ProcessGroup *pg = p_owner ? (ProcessGroup *)p_owner->data.process_group : &default_process_group;

	if (p_node->is_processing() || p_node->is_processing_internal()) {
		bool found = pg->nodes.erase(p_node);
		ERR_FAIL_COND(!found);
	}

	if (p_node->is_physics_processing() || p_node->is_physics_processing_internal()) {
		bool found = pg->physics_nodes.erase(p_node);
		ERR_FAIL_COND(!found);
	}
}

void SceneTree::_add_node_to_process_group(Node *p_node, Node *p_owner) {
	_THREAD_SAFE_METHOD_
	ProcessGroup *pg = p_owner ? (ProcessGroup *)p_owner->data.process_group : &default_process_group;

	if (p_node->is_processing() || p_node->is_processing_internal()) {
		pg->nodes.push_back(p_node);
		pg->node_order_dirty = true;
	}

	if (p_node->is_physics_processing() || p_node->is_physics_processing_internal()) {
		pg->physics_nodes.push_back(p_node);
		pg->physics_node_order_dirty = true;
	}
}

// Keep input dispatch in the optional game layer.

void SceneTree::_call_group_flags(const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	r_error.error = Callable::CallError::CALL_OK;

	ERR_FAIL_COND(p_argcount < 3);
	ERR_FAIL_COND(!p_args[0]->is_num());
	ERR_FAIL_COND(!p_args[1]->is_string());
	ERR_FAIL_COND(!p_args[2]->is_string());

	int flags = *p_args[0];
	StringName group = *p_args[1];
	StringName method = *p_args[2];

	call_group_flagsp(flags, group, method, p_args + 3, p_argcount - 3);
}

void SceneTree::_call_group(const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	r_error.error = Callable::CallError::CALL_OK;

	ERR_FAIL_COND(p_argcount < 2);
	ERR_FAIL_COND(!p_args[0]->is_string());
	ERR_FAIL_COND(!p_args[1]->is_string());

	StringName group = *p_args[0];
	StringName method = *p_args[1];

	call_group_flagsp(GROUP_CALL_DEFAULT, group, method, p_args + 2, p_argcount - 2);
}

int64_t SceneTree::get_frame() const {
	return current_frame;
}

TypedArray<Node> SceneTree::_get_nodes_in_group(const StringName &p_group) {
	_THREAD_SAFE_METHOD_
	TypedArray<Node> ret;
	HashMap<StringName, SceneTreeGroup>::Iterator E = group_map.find(p_group);
	if (!E) {
		return ret;
	}

	_update_group_order(E->value); //update order just in case
	int nc = E->value.nodes.size();
	if (nc == 0) {
		return ret;
	}

	ret.resize(nc);

	Node **ptr = E->value.nodes.ptrw();
	for (int i = 0; i < nc; i++) {
		ret[i] = ptr[i];
	}

	return ret;
}

bool SceneTree::has_group(const StringName &p_identifier) const {
	_THREAD_SAFE_METHOD_
	return group_map.has(p_identifier);
}

int SceneTree::get_node_count_in_group(const StringName &p_group) const {
	_THREAD_SAFE_METHOD_
	HashMap<StringName, SceneTreeGroup>::ConstIterator E = group_map.find(p_group);
	if (!E) {
		return 0;
	}

	return E->value.nodes.size();
}

Node *SceneTree::get_first_node_in_group(const StringName &p_group) {
	_THREAD_SAFE_METHOD_
	HashMap<StringName, SceneTreeGroup>::Iterator E = group_map.find(p_group);
	if (!E) {
		return nullptr; // No group.
	}

	_update_group_order(E->value); // Update order just in case.

	if (E->value.nodes.is_empty()) {
		return nullptr;
	}

	return E->value.nodes[0];
}

Vector<Node *> SceneTree::get_nodes_in_group(const StringName &p_group) {
	_THREAD_SAFE_METHOD_
	HashMap<StringName, SceneTreeGroup>::Iterator E = group_map.find(p_group);
	if (!E) {
		return {};
	}

	_update_group_order(E->value); //update order just in case
	int nc = E->value.nodes.size();
	if (nc == 0) {
		return {};
	}

	return E->value.nodes;
}

void SceneTree::_flush_delete_queue() {
	_THREAD_SAFE_METHOD_

	while (delete_queue.size()) {
		Object *obj = ObjectDB::get_instance(delete_queue.front()->get());
		if (obj) {
			memdelete(obj);
		}
		delete_queue.pop_front();
	}
}

void SceneTree::queue_delete(RequiredParam<Object> rp_object) {
	_THREAD_SAFE_METHOD_
	EXTRACT_PARAM_OR_FAIL(p_object, rp_object);
	p_object->_is_queued_for_deletion = true;
	delete_queue.push_back(p_object->get_instance_id());
	IdleWait::wake(); // Wake for deletion at the next SceneTree tail even without continuous frames.
}

int SceneTree::get_node_count() const {
	return nodes_in_tree_count;
}

void SceneTree::set_edited_scene_root(Node *p_node) {
#ifdef TOOLS_ENABLED
	edited_scene_root = p_node;
#endif
}

Node *SceneTree::get_edited_scene_root() const {
#ifdef TOOLS_ENABLED
	return edited_scene_root;
#else
	return nullptr;
#endif
}

void SceneTree::set_current_scene(Node *p_scene) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Changing scene can only be done from the main thread.");
	ERR_FAIL_COND(p_scene && p_scene->get_parent() != root);
	current_scene = p_scene;
}

Node *SceneTree::get_current_scene() const {
	return current_scene;
}

void SceneTree::_flush_scene_change() {
	if (prev_scene_id.is_valid()) {
		// Might have already been freed externally.
		Node *prev_scene = ObjectDB::get_instance<Node>(prev_scene_id);
		if (prev_scene) {
			memdelete(prev_scene);
		}
		prev_scene_id = ObjectID();
	}

	DEV_ASSERT(pending_new_scene_id.is_valid());
	Node *pending_new_scene = ObjectDB::get_instance<Node>(pending_new_scene_id);
	if (pending_new_scene) {
		// Ensure correct state before `add_child` (might enqueue subsequent scene change).
		current_scene = pending_new_scene;
		pending_new_scene_id = ObjectID();

		root->add_child(pending_new_scene);

		// Only on successful scene change.
		emit_signal(SNAME("scene_changed"));
	} else {
		current_scene = nullptr;
		pending_new_scene_id = ObjectID();
		ERR_PRINT("Scene instance has been freed before becoming the current scene. No current scene is set.");
	}
}

Error SceneTree::change_scene_to_file(const String &p_path) {
	ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), ERR_INVALID_PARAMETER, "Changing scene can only be done from the main thread.");
	Ref<PackedScene> new_scene = ResourceLoader::load(p_path);
	if (new_scene.is_null()) {
		return ERR_CANT_OPEN;
	}

	return change_scene_to_packed(new_scene);
}

Error SceneTree::change_scene_to_packed(RequiredParam<PackedScene> rp_scene) {
	EXTRACT_PARAM_OR_FAIL_V_MSG(p_scene, rp_scene, ERR_INVALID_PARAMETER, "Can't change to a null scene. Use unload_current_scene() if you wish to unload it.");

	Node *new_scene = p_scene->instantiate();
	ERR_FAIL_NULL_V(new_scene, ERR_CANT_CREATE);

	return change_scene_to_node(new_scene);
}

Error SceneTree::change_scene_to_node(RequiredParam<Node> rp_node) {
	EXTRACT_PARAM_OR_FAIL_V_MSG(p_node, rp_node, ERR_INVALID_PARAMETER, "Can't change to a null node. Use unload_current_scene() if you wish to unload it.");
	ERR_FAIL_COND_V_MSG(p_node->is_inside_tree(), ERR_UNCONFIGURED, "The new scene node can't already be inside scene tree.");

	// If called again while a change is pending.
	if (pending_new_scene_id.is_valid()) {
		Node *pending_new_scene = ObjectDB::get_instance<Node>(pending_new_scene_id);
		if (pending_new_scene) {
			queue_delete(pending_new_scene);
		}
		pending_new_scene_id = ObjectID();
	}

	if (current_scene) {
		prev_scene_id = current_scene->get_instance_id();
		// Let as many side effects as possible happen or be queued now,
		// so they are run before the scene is actually deleted.
		root->remove_child(current_scene);
	}
	DEV_ASSERT(!current_scene);

	pending_new_scene_id = p_node->get_instance_id();
	return OK;
}

Error SceneTree::reload_current_scene() {
	ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), ERR_INVALID_PARAMETER, "Reloading scene can only be done from the main thread.");
	ERR_FAIL_NULL_V(current_scene, ERR_UNCONFIGURED);
	String fname = current_scene->get_scene_file_path();
	return change_scene_to_file(fname);
}

void SceneTree::unload_current_scene() {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Unloading the current scene can only be done from the main thread.");
	if (current_scene) {
		memdelete(current_scene);
		current_scene = nullptr;
	}
}

void SceneTree::add_current_scene(Node *p_current) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Adding a current scene can only be done from the main thread.");
	current_scene = p_current;
	root->add_child(p_current);
}

RequiredResult<SceneTreeTimer> SceneTree::create_timer(double p_delay_sec, bool p_process_always, bool p_process_in_physics, bool p_ignore_time_scale) {
	_THREAD_SAFE_METHOD_
	Ref<SceneTreeTimer> stt;
	stt.instantiate();
	stt->set_process_always(p_process_always);
	stt->set_time_left(p_delay_sec);
	stt->set_process_in_physics(p_process_in_physics);
	stt->set_ignore_time_scale(p_ignore_time_scale);
	timers.push_back(stt);
	return stt;
}

// Provide a headless stub for an optional game feature.

// Provide a headless stub for an optional game feature.

// Provide a headless stub for an optional game feature.

RequiredResult<MultiplayerAPI> SceneTree::get_multiplayer(const NodePath &p_for_path) const {
	ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), Ref<MultiplayerAPI>(), "Multiplayer can only be manipulated from the main thread.");
	if (p_for_path.is_empty()) {
		return multiplayer;
	}

	const Vector<StringName> tnames = p_for_path.get_names();
	const StringName *nptr = tnames.ptr();
	for (const KeyValue<NodePath, Ref<MultiplayerAPI>> &E : custom_multiplayers) {
		const Vector<StringName> snames = E.key.get_names();
		if (tnames.size() < snames.size()) {
			continue;
		}
		const StringName *sptr = snames.ptr();
		bool valid = true;
		for (int i = 0; i < snames.size(); i++) {
			if (sptr[i] != nptr[i]) {
				valid = false;
				break;
			}
		}
		if (valid) {
			return E.value;
		}
	}

	return multiplayer;
}

void SceneTree::set_multiplayer(Ref<MultiplayerAPI> p_multiplayer, const NodePath &p_root_path) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Multiplayer can only be manipulated from the main thread.");
	if (p_root_path.is_empty()) {
		ERR_FAIL_COND(p_multiplayer.is_null());
		if (multiplayer.is_valid()) {
			multiplayer->object_configuration_remove(nullptr, NodePath("/" + root->get_name()));
		}
		multiplayer = p_multiplayer;
		multiplayer->object_configuration_add(nullptr, NodePath("/" + root->get_name()));
	} else {
		if (custom_multiplayers.has(p_root_path)) {
			custom_multiplayers[p_root_path]->object_configuration_remove(nullptr, p_root_path);
		} else if (p_multiplayer.is_valid()) {
			const Vector<StringName> tnames = p_root_path.get_names();
			const StringName *nptr = tnames.ptr();
			for (const KeyValue<NodePath, Ref<MultiplayerAPI>> &E : custom_multiplayers) {
				const Vector<StringName> snames = E.key.get_names();
				if (tnames.size() < snames.size()) {
					continue;
				}
				const StringName *sptr = snames.ptr();
				bool valid = true;
				for (int i = 0; i < snames.size(); i++) {
					if (sptr[i] != nptr[i]) {
						valid = false;
						break;
					}
				}
				ERR_FAIL_COND_MSG(valid, "Multiplayer is already configured for a parent of this path: '" + String(p_root_path) + "' in '" + String(E.key) + "'.");
			}
		}
		if (p_multiplayer.is_valid()) {
			custom_multiplayers[p_root_path] = p_multiplayer;
			p_multiplayer->object_configuration_add(nullptr, p_root_path);
		} else {
			custom_multiplayers.erase(p_root_path);
		}
	}
}

void SceneTree::set_multiplayer_poll_enabled(bool p_enabled) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Multiplayer can only be manipulated from the main thread.");
	multiplayer_poll = p_enabled;
}

bool SceneTree::is_multiplayer_poll_enabled() const {
	return multiplayer_poll;
}

void SceneTree::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_root"), &SceneTree::get_root);
	ClassDB::bind_method(D_METHOD("has_group", "name"), &SceneTree::has_group);

	ClassDB::bind_method(D_METHOD("is_accessibility_enabled"), &SceneTree::is_accessibility_enabled);
	ClassDB::bind_method(D_METHOD("is_accessibility_supported"), &SceneTree::is_accessibility_supported);

	ClassDB::bind_method(D_METHOD("is_auto_accept_quit"), &SceneTree::is_auto_accept_quit);
	ClassDB::bind_method(D_METHOD("set_auto_accept_quit", "enabled"), &SceneTree::set_auto_accept_quit);
	ClassDB::bind_method(D_METHOD("is_quit_on_go_back"), &SceneTree::is_quit_on_go_back);
	ClassDB::bind_method(D_METHOD("set_quit_on_go_back", "enabled"), &SceneTree::set_quit_on_go_back);

	ClassDB::bind_method(D_METHOD("set_debug_collisions_hint", "enable"), &SceneTree::set_debug_collisions_hint);
	ClassDB::bind_method(D_METHOD("is_debugging_collisions_hint"), &SceneTree::is_debugging_collisions_hint);
	ClassDB::bind_method(D_METHOD("set_debug_paths_hint", "enable"), &SceneTree::set_debug_paths_hint);
	ClassDB::bind_method(D_METHOD("is_debugging_paths_hint"), &SceneTree::is_debugging_paths_hint);
	ClassDB::bind_method(D_METHOD("set_debug_navigation_hint", "enable"), &SceneTree::set_debug_navigation_hint);
	ClassDB::bind_method(D_METHOD("is_debugging_navigation_hint"), &SceneTree::is_debugging_navigation_hint);

	ClassDB::bind_method(D_METHOD("set_edited_scene_root", "scene"), &SceneTree::set_edited_scene_root);
	ClassDB::bind_method(D_METHOD("get_edited_scene_root"), &SceneTree::get_edited_scene_root);

	ClassDB::bind_method(D_METHOD("set_pause", "enable"), &SceneTree::set_pause);
	ClassDB::bind_method(D_METHOD("is_paused"), &SceneTree::is_paused);

	ClassDB::bind_method(D_METHOD("create_timer", "time_sec", "process_always", "process_in_physics", "ignore_time_scale"), &SceneTree::create_timer, DEFVAL(true), DEFVAL(false), DEFVAL(false));

	ClassDB::bind_method(D_METHOD("get_node_count"), &SceneTree::get_node_count);
	ClassDB::bind_method(D_METHOD("get_frame"), &SceneTree::get_frame);
	ClassDB::bind_method(D_METHOD("quit", "exit_code"), &SceneTree::quit, DEFVAL(EXIT_SUCCESS));


	ClassDB::bind_method(D_METHOD("queue_delete", "obj"), &SceneTree::queue_delete);

	MethodInfo mi;
	mi.name = "call_group_flags";
	mi.arguments.push_back(PropertyInfo(Variant::INT, "flags"));
	mi.arguments.push_back(PropertyInfo(Variant::STRING_NAME, "group"));
	mi.arguments.push_back(PropertyInfo(Variant::STRING_NAME, "method"));

	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "call_group_flags", &SceneTree::_call_group_flags, mi);

	ClassDB::bind_method(D_METHOD("notify_group_flags", "call_flags", "group", "notification"), &SceneTree::notify_group_flags);
	ClassDB::bind_method(D_METHOD("set_group_flags", "call_flags", "group", "property", "value"), &SceneTree::set_group_flags);

	MethodInfo mi2;
	mi2.name = "call_group";
	mi2.arguments.push_back(PropertyInfo(Variant::STRING_NAME, "group"));
	mi2.arguments.push_back(PropertyInfo(Variant::STRING_NAME, "method"));

	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "call_group", &SceneTree::_call_group, mi2);

	ClassDB::bind_method(D_METHOD("notify_group", "group", "notification"), &SceneTree::notify_group);
	ClassDB::bind_method(D_METHOD("set_group", "group", "property", "value"), &SceneTree::set_group);

	ClassDB::bind_method(D_METHOD("get_nodes_in_group", "group"), &SceneTree::_get_nodes_in_group);
	ClassDB::bind_method(D_METHOD("get_first_node_in_group", "group"), &SceneTree::get_first_node_in_group);
	ClassDB::bind_method(D_METHOD("get_node_count_in_group", "group"), &SceneTree::get_node_count_in_group);

	ClassDB::bind_method(D_METHOD("set_current_scene", "child_node"), &SceneTree::set_current_scene);
	ClassDB::bind_method(D_METHOD("get_current_scene"), &SceneTree::get_current_scene);

	ClassDB::bind_method(D_METHOD("change_scene_to_file", "path"), &SceneTree::change_scene_to_file);
	ClassDB::bind_method(D_METHOD("change_scene_to_packed", "packed_scene"), &SceneTree::change_scene_to_packed);
	ClassDB::bind_method(D_METHOD("change_scene_to_node", "node"), &SceneTree::change_scene_to_node);

	ClassDB::bind_method(D_METHOD("reload_current_scene"), &SceneTree::reload_current_scene);
	ClassDB::bind_method(D_METHOD("unload_current_scene"), &SceneTree::unload_current_scene);

	ClassDB::bind_method(D_METHOD("set_multiplayer", "multiplayer", "root_path"), &SceneTree::set_multiplayer, DEFVAL(NodePath()));
	ClassDB::bind_method(D_METHOD("get_multiplayer", "for_path"), &SceneTree::get_multiplayer, DEFVAL(NodePath()));
	ClassDB::bind_method(D_METHOD("set_multiplayer_poll_enabled", "enabled"), &SceneTree::set_multiplayer_poll_enabled);
	ClassDB::bind_method(D_METHOD("is_multiplayer_poll_enabled"), &SceneTree::is_multiplayer_poll_enabled);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_accept_quit"), "set_auto_accept_quit", "is_auto_accept_quit");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "quit_on_go_back"), "set_quit_on_go_back", "is_quit_on_go_back");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_collisions_hint"), "set_debug_collisions_hint", "is_debugging_collisions_hint");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_paths_hint"), "set_debug_paths_hint", "is_debugging_paths_hint");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_navigation_hint"), "set_debug_navigation_hint", "is_debugging_navigation_hint");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "set_pause", "is_paused");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "edited_scene_root", PROPERTY_HINT_RESOURCE_TYPE, Node::get_class_static(), PROPERTY_USAGE_NONE), "set_edited_scene_root", "get_edited_scene_root");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "current_scene", PROPERTY_HINT_RESOURCE_TYPE, Node::get_class_static(), PROPERTY_USAGE_NONE), "set_current_scene", "get_current_scene");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "root", PROPERTY_HINT_RESOURCE_TYPE, Node::get_class_static(), PROPERTY_USAGE_NONE), "", "get_root");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "multiplayer_poll"), "set_multiplayer_poll_enabled", "is_multiplayer_poll_enabled");

	ADD_SIGNAL(MethodInfo("tree_changed"));
	ADD_SIGNAL(MethodInfo("scene_changed"));
	ADD_SIGNAL(MethodInfo("tree_process_mode_changed")); //editor only signal, but due to API hash it can't be removed in run-time
	ADD_SIGNAL(MethodInfo("node_added", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_RESOURCE_TYPE, Node::get_class_static())));
	ADD_SIGNAL(MethodInfo("node_removed", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_RESOURCE_TYPE, Node::get_class_static())));
	ADD_SIGNAL(MethodInfo("node_renamed", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_RESOURCE_TYPE, Node::get_class_static())));
	ADD_SIGNAL(MethodInfo("node_configuration_warning_changed", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_RESOURCE_TYPE, Node::get_class_static())));

	ADD_SIGNAL(MethodInfo("process_frame"));
	ADD_SIGNAL(MethodInfo("physics_frame"));

	BIND_ENUM_CONSTANT(GROUP_CALL_DEFAULT);
	BIND_ENUM_CONSTANT(GROUP_CALL_REVERSE);
	BIND_ENUM_CONSTANT(GROUP_CALL_DEFERRED);
	BIND_ENUM_CONSTANT(GROUP_CALL_UNIQUE);
}

SceneTree *SceneTree::singleton = nullptr;

SceneTree::IdleCallback SceneTree::idle_callbacks[SceneTree::MAX_IDLE_CALLBACKS];
int SceneTree::idle_callback_count = 0;

void SceneTree::_call_idle_callbacks() {
	for (int i = 0; i < idle_callback_count; i++) {
		idle_callbacks[i]();
	}
}

void SceneTree::add_idle_callback(IdleCallback p_callback) {
	ERR_FAIL_COND(idle_callback_count >= MAX_IDLE_CALLBACKS);
	idle_callbacks[idle_callback_count++] = p_callback;
}

#ifdef TOOLS_ENABLED
void SceneTree::get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const {
	bool add_options = false;
	if (p_idx == 0) {
		static const Vector<StringName> names = {
			StringName("add_to_group", true),
			StringName("call_group", true),
			StringName("get_first_node_in_group", true),
			StringName("get_node_count_in_group", true),
			StringName("get_nodes_in_group", true),
			StringName("has_group", true),
			StringName("notify_group", true),
			StringName("set_group", true),
		};
		add_options = names.has(p_function);
	} else if (p_idx == 1) {
		static const Vector<StringName> names = {
			StringName("call_group_flags", true),
			StringName("notify_group_flags", true),
			StringName("set_group_flags", true),
		};
		add_options = names.has(p_function);
	}
	if (add_options) {
		HashMap<StringName, String> global_groups(ProjectSettings::get_singleton()->get_global_groups_list());
		for (const KeyValue<StringName, String> &E : global_groups) {
			r_options->push_back(E.key.operator String().quote());
		}
	}
	MainLoop::get_argument_options(p_function, p_idx, r_options);
}
#endif

void SceneTree::set_disable_node_threading(bool p_disable) {
	node_threading_disabled = p_disable;
}

SceneTree::SceneTree() {
	RunGuard::scene_tree(); // Check the prohibition before initializing the root or multiplayer.
	if (singleton == nullptr) {
		singleton = this;
	}
	debug_collisions_color = GLOBAL_DEF("debug/shapes/collision/shape_color", Color(0.0, 0.6, 0.7, 0.42));
	debug_collision_contact_color = GLOBAL_DEF("debug/shapes/collision/contact_color", Color(1.0, 0.2, 0.1, 0.8));
	debug_paths_color = GLOBAL_DEF("debug/shapes/paths/geometry_color", Color(0.1, 1.0, 0.7, 0.4));
	debug_paths_width = GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "debug/shapes/paths/geometry_width", PROPERTY_HINT_RANGE, "0.01,10,0.001,or_greater"), 2.0);
	collision_debug_contacts = GLOBAL_DEF(PropertyInfo(Variant::INT, "debug/shapes/collision/max_contacts_displayed", PROPERTY_HINT_RANGE, "0,20000,1"), 10000);
	accessibility_upd_per_sec = GLOBAL_GET(SNAME("accessibility/general/updates_per_second"));

	GLOBAL_DEF("debug/shapes/collision/draw_2d_outlines", true);

	process_group_call_queue_allocator = memnew(CallQueue::Allocator(64));
	Math::randomize();

	// Create with mainloop.

	// Use a plain headless root; the optional game layer supplies a Window.
	root = memnew(Node);
	root->set_process_mode(Node::PROCESS_MODE_PAUSABLE);
	root->set_name("root");
	root->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);

	// Initialize multiplayer communication.
	set_multiplayer(MultiplayerAPI::create_default_interface());
	current_scene = nullptr;

#ifdef TOOLS_ENABLED
	edited_scene_root = nullptr;
#endif

	process_groups.push_back(&default_process_group);
}

SceneTree::~SceneTree() {
	if (prev_scene_id.is_valid()) {
		Node *prev_scene = ObjectDB::get_instance<Node>(prev_scene_id);
		if (prev_scene) {
			memdelete(prev_scene);
		}
		prev_scene_id = ObjectID();
	}
	if (pending_new_scene_id.is_valid()) {
		Node *pending_new_scene = ObjectDB::get_instance<Node>(pending_new_scene_id);
		if (pending_new_scene) {
			memdelete(pending_new_scene);
		}
		pending_new_scene_id = ObjectID();
	}
	if (root) {
		root->_set_tree(nullptr);
		root->_propagate_after_exit_tree();
		memdelete(root);
	}

	// Process groups are not deleted immediately, they may remain around. Delete them now.
	for (uint32_t i = 0; i < process_groups.size(); i++) {
		if (process_groups[i] != &default_process_group) {
			memdelete(process_groups[i]);
		}
	}

	memdelete(process_group_call_queue_allocator);

	if (singleton == this) {
		singleton = nullptr;
	}
}
