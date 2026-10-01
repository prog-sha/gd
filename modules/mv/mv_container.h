/**************************************************************************/
/*  mv_container.h                                                        */
/**************************************************************************/

// Runs the server export in a local container from the editor.
// One menu item does export -> docker compose up -> copy the generated certificate to the Client's ca.

#pragma once

#ifdef TOOLS_ENABLED

#include "core/string/ustring.h"

class MVContainer {
	static bool busy; // Start or stop in progress. Repeated presses do not start a second run

	static String work_dir(); // Where the export and container definitions go (under res://.godot, not in the repo)
	static String project_name(); // Keep containers and private volumes separate for each project
	static bool compose(const char *p_verb, const String &p_flag); // Run docker compose <verb> there and show its output as-is
	static void run_start(void *); // On a worker thread, bring the container up and copy the certificate
	static void run_stop(void *); // On a worker thread, take the container down

public:
	static void install(); // Add start and stop items to Project > Tools
	static void start(); // Export the server preset and start the local test container
	static void stop(); // Stop the local test container
};

#endif // TOOLS_ENABLED
