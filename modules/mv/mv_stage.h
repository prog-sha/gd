/**************************************************************************/
/*  mv_stage.h                                                            */
/**************************************************************************/

// Builds the fixed Node structure of the public world in C++ only.

#pragma once

#include "core/object/ref_counted.h"

class Node;
class Online;

class MVStage : public RefCounted {
	GDCLASS(MVStage, RefCounted);

protected:
	static void _bind_methods();

public:
	static Online *build(Node *p_parent, bool p_authority); // Put the author's Online under the runtime. On the local side, strip Secret contents
	static bool is_who(const String &p_name); // Check the shape of an in-session Player name
	static bool finds_secret(Node *p_node); // Whether a Secret exists anywhere in the tree
	static void hollow_secrets(Node *p_node); // Drop a Secret's script, values and children, leaving only the entry
	static void hold_secrets(Node *p_node); // Record only the Secret script path and detach it before entering the tree
	static void script_values(Node *p_node, Dictionary &r_keep); // Record only sendable values held by the script
	static void hold_body(Node *p_node); // Record only the script path and values and detach it before entering the tree
	static void take_held(Node *p_node, Dictionary &r_keep); // Take the recorded values and clear the marker
};
