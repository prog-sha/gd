// Run persistent tasks through the ready queue and I/O deadlines without a SceneTree.
#pragma once

#include "core/os/main_loop.h"

class GDLoop : public MainLoop {
	GDCLASS(GDLoop, MainLoop);
	bool stopped = false; // Explicit termination request.
	uint64_t polled = 0; // Last nonblocking I/O scan while runnable work continues.
	uint64_t turns = 0; // Scheduler iteration count for detecting idle spinning.

protected:
	static void _bind_methods(); // Expose shutdown to the running script.

public:
	void quit(int p_code = 0); // Quit at the next safe iteration boundary.
	bool has_scene_tree() const; // Report implicit SceneTree instances for runtime-isolation checks.
	uint64_t iterations() const { return turns; } // Return the iteration count independently of frames.
	bool iteration(); // Advance ready work, sleeping for notifications when none remains.
};
