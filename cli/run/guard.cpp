// Report forbidden SceneTree construction without returning a partially initialized object.
#include "cli/run/guard.h"

#include "core/error/error_macros.h"

#include <cstdio>
#include <cstdlib>

// Stop before initializing a forbidden runtime rather than returning midway through construction.
void RunGuard::scene_tree() {
	if (no_tree) {
		ERR_PRINT("--no-scene-tree: SceneTree construction is forbidden; use the GD serve runtime.");
		std::fflush(stderr);
		std::_Exit(EXIT_FAILURE); // Avoid cleanup of partially initialized objects or worker state.
	}
}
