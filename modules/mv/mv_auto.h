/**************************************************************************/
/*  mv_auto.h                                                             */
/**************************************************************************/

// Checks once whether @online marks exist and starts the needed networking automatically.

#pragma once

#include "scene/main/node.h"

class SceneTree;

class MVAuto : public Node {
	GDCLASS(MVAuto, Node);

	static bool script_uses_online(const String &p_path); // Check one script for real annotation marks
	static bool dir_uses_online(const String &p_root, const String &p_skip = String()); // Scan a folder tree
	static bool project_uses_online(); // Return whether any @online mark was found

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static void install(SceneTree *p_tree); // Schedule the one-time check in a normal game
};
