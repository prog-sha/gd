/**************************************************************************/
/*  entry.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Instantiate a standalone script on SceneTree for ordinary runs or the dedicated persistent runtime.
// Treat scripts that inherit neither SceneTree nor MainLoop as command execution units.
// Keep this adapter outside ClassDB and the public script API.

#pragma once

#include "core/error/error_macros.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/object/script_language.h"
#include "core/templates/list.h"
#include "core/variant/variant.h"
#include "cli/main/cmd.h"
#include "cli/sys/std.h"
#include "cli/run/loop.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "modules/gdscript/gdscript_function.h"

#include <cstdio>
#include <cstdlib>

// Record failures encountered during script execution.
// A failed main() may otherwise return an empty value and incorrectly produce exit code zero.
class GDFail {
	static inline ErrorHandlerList handler;

	static void on_error(void *, const char *, const char *, int, const char *, const char *, bool, ErrorHandlerType p_type) {
		if (p_type == ERR_HANDLER_SCRIPT) {
			hit = true;
		}
	}

public:
	static inline bool hit = false;

	// Stop the program where ! failed; a resident server only fails the handler that used it.
	static void stop() {
		if (Cmd::serve) {
			return;
		}
		fflush(nullptr);
		std::_Exit(EXIT_FAILURE);
	}

	static void watch() {
		handler.errfunc = on_error;
		add_error_handler(&handler);
		GDScriptFunction::on_forced_failure = stop;
	}
	static void unwatch() { remove_error_handler(&handler); }
};

class GDEntry : public Object {
	GDCLASS(GDEntry, Object);

	Ref<Script> script; // Script to execute.
	Object *obj = nullptr; // Object hosting the script.
	ObjectID obj_id; // Identity preventing double deletion if the script frees itself.
	Ref<RefCounted> obj_ref; // Lifetime ownership for reference-counted instances.
	Array args; // Arguments passed to main().
	bool node_owned_by_tree = false; // Whether the instance was added to the tree as a Node.
	Variant pending; // Retained coroutine state needed to resume execution.

public:
	static inline bool serve_mode = false; // Whether the command uses persistent serving.

private:

	// Convert the result into an exit code and stop the loop.
	// Keep persistent serving alive after the entry function completes.
	void _quit(const Variant &p_ret) {
		int code = 0;
		Ref<R> result = p_ret;
		if (result.is_valid()) {
			if (result->get_e().is_valid()) {
				print_error("error: " + result->get_e()->text());
				GDFail::hit = true;
				code = 1;
			} else if (result->get_v().get_type() == Variant::INT) {
				code = (int)result->get_v();
			}
		} else if (p_ret.get_type() == Variant::INT) {
			code = (int)p_ret;
		}
		if (serve_mode) {
			if (code != 0) {
				GDLoop *loop = Object::cast_to<GDLoop>(OS::get_singleton()->get_main_loop());
				if (loop) {
					loop->quit(code);
				}
			}
			return;
		}
		SceneTree *tree = SceneTree::get_singleton();
		if (tree) {
			tree->quit(code);
		}
	}

	// Receive completion of an asynchronous main().
	void _on_completed(const Variant &p_ret) {
		pending = Variant();
		_quit(p_ret);
	}

public:
	// Instantiate the script, running _init when present.
	Error setup(const Ref<Script> &p_script, const List<String> &p_args) {
		script = p_script;
		StringName base = script->get_instance_base_type();
		if (base == StringName()) {
			base = SNAME("RefCounted");
		}
		obj = ClassDB::instantiate(base);
		ERR_FAIL_NULL_V_MSG(obj, ERR_CANT_CREATE, vformat("Can't instantiate base type \"%s\".", String(base)));
		obj_id = obj->get_instance_id();

		RefCounted *rc = Object::cast_to<RefCounted>(obj);
		if (rc) {
			obj_ref = Ref<RefCounted>(rc);
		}
		obj->set_script(script); // Attaching the script invokes _init.

		for (const String &a : p_args) {
			args.push_back(a);
		}
		return OK;
	}

	// Start from the runtime and invoke main() when present.
	void start() {
		SceneTree *tree = SceneTree::get_singleton();

		// Attach Node-derived scripts to the tree for their node signals and timers.
		Node *node = Object::cast_to<Node>(obj);
		if (tree && node && !node->is_inside_tree()) {
			tree->get_root()->add_child(node);
			node_owned_by_tree = true;
		}

		if (!obj->has_method(SNAME("main"))) {
			_quit(Variant()); // Without main(), execution ends when _init completes.
			return;
		}

		// Choose invocation arguments to match main()'s arity.
		int argc = 0;
		if (script->has_method(SNAME("main"))) {
			argc = script->get_method_info(SNAME("main")).arguments.size();
		}

		Callable::CallError err;
		Variant arg0 = args;
		const Variant *argp[1] = { &arg0 };
		const bool sliced = serve_mode && GDScriptFunction::begin_time_slice();
		Variant ret;
		{
			GDScriptFunction::SuspendableCall suspendable;
			ret = obj->callp(SNAME("main"), argc > 0 ? argp : nullptr, argc > 0 ? 1 : 0, err);
		}
		if (sliced) {
			GDScriptFunction::end_time_slice();
		}
		if (err.error != Callable::CallError::CALL_OK) {
			ERR_PRINT(vformat("Error calling main(): %s", Variant::get_call_error_text(obj, SNAME("main"), argp, argc > 0 ? 1 : 0, err)));
			_quit(1);
			return;
		}

		// Await coroutine completion so main() may suspend naturally.
		Object *state = ret.get_type() == Variant::OBJECT ? (Object *)ret : nullptr;
		if (state && state->has_signal(SNAME("completed"))) {
			// Retain the state so its await continuation cannot be freed before resumption.
			pending = ret;
			state->connect(SNAME("completed"), callable_mp(this, &GDEntry::_on_completed), Object::CONNECT_ONE_SHOT);
			return;
		}
		_quit(ret);
	}

	~GDEntry() {
		if (ObjectDB::get_instance(obj_id) && obj_ref.is_null() && !node_owned_by_tree) {
			memdelete(obj);
		}
	}
};
