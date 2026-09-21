// Detect construction of runtime components forbidden by development flags.
#pragma once

namespace RunGuard {
inline bool no_tree = false; // Startup option treating SceneTree creation as a fatal contract violation.
void scene_tree(); // Check before SceneTree initialization and terminate on violation.
}
