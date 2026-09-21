// Drive persistent work independently of the frame-processing pipeline.
#include "cli/run/loop.h"
#include "cli/sys/clock.h"

#include "cli/sys/pool.h"
#include "cli/sys/task.h"
#include "cli/sys/wait.h"
#include "cli/sys/sched.h"
#include "core/object/class_db.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "modules/gdscript/gdscript_function.h"
#include "scene/main/scene_tree.h"

// Allow shutdown requests through the current main loop.
void GDLoop::_bind_methods() {
	ClassDB::bind_method(D_METHOD("quit", "exit_code"), &GDLoop::quit, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("has_scene_tree"), &GDLoop::has_scene_tree);
	ClassDB::bind_method(D_METHOD("iterations"), &GDLoop::iterations);
}

// Detect SceneTree instances created separately from the main loop.
bool GDLoop::has_scene_tree() const {
	return SceneTree::get_singleton() != nullptr;
}

// Store the exit code and interrupt kernel waiting.
void GDLoop::quit(int p_code) {
	stopped = true;
	OS::get_singleton()->set_exit_code(p_code);
	IdleWait::wake();
}

// Deliver runnable work and deadlines, preferring runnable tasks over timed sleep.
bool GDLoop::iteration() {
	turns++;
	const uint64_t now = GDClock::usec();
	if (now - polled >= GD_SCHED_SLICE_USEC) {
		// Periodically scan I/O even when runnable work never runs out.
		IdleWait::wait(0);
		polled = now;
	}
	IdleWait::dispatch();
	Pool::drain();
	GDScriptFunctionState::drain_scheduler();
	Async::drain();
	MessageQueue::get_singleton()->flush();
	if (stopped) {
		return true;
	}
	if (Async::has_ready() || GDScriptFunctionState::has_scheduled()) {
		return false;
	}
	const double next = Async::time_to_next_timer();
	const double max_sec = double(UINT64_MAX) / 1000000.0;
	const uint64_t usec = next < 0.0 || next >= max_sec ? UINT64_MAX : uint64_t(next * 1000000.0);
	IdleWait::wait(usec);
	polled = GDClock::usec();
	return false;
}
