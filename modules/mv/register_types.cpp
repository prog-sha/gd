/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

// Exposes the fixed Server, gate, distribution, intake checks, row format and type table to GDScript.

#include "register_types.h"

#include "mv_auto.h"
#include "mv_client.h"
#include "mv_client_script.h"
#include "mv_host.h"
#include "mv_link.h"
#include "mv_online.h"
#include "mv_reply.h"
#include "mv_secret.h"
#include "mv_secret_client.h"
#include "mv_stage.h"
#include "mv_thing.h"

#include "modules/gdscript/gdscript_online.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#ifdef TOOLS_ENABLED
#include "mv_container.h"
#include "mv_export.h"
#include "mv_tr.h"

#include "editor/editor_node.h"
#include "editor/export/editor_export.h"
#endif
#include "mv_runtime.h"
#ifdef MV_SECRET_ENABLED
#include "mv_authority.h"
#include "mv_vault.h"
#endif

#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

static MVOnline *online_singleton = nullptr; // Internal singleton used only by generated code

#ifdef TOOLS_ENABLED
// Plug in the hook that strips Secret bodies from the PCK on every export
static void mv_editor_init() {
	MVTr::install();
	Ref<MVExport> plugin;
	plugin.instantiate();
	EditorExport::get_singleton()->add_export_plugin(plugin);
	MVContainer::install();
}
#endif

void initialize_mv_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorNode::add_init_callback(mv_editor_init);
		return;
	}
#endif
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	// The world owner answers the language's "is this mine" check, so only the owner's side can write @online_input
	GDScriptOnline::owner_check = [](Object *p_node) { return MVThing::is_my(Object::cast_to<Node>(p_node)); };
	GDREGISTER_INTERNAL_CLASS(MVClient);
	GDREGISTER_INTERNAL_CLASS(MVLink);
	GDREGISTER_INTERNAL_CLASS(MVReply);
	GDREGISTER_INTERNAL_CLASS(MVSecretClient);
	GDREGISTER_INTERNAL_CLASS(MVThing);
	// The Online-side world is in every build. This is what lets a host become the Online side
	GDREGISTER_INTERNAL_CLASS(MVRuntime);
	GDREGISTER_INTERNAL_CLASS(MVHost);
	// Built-in Nodes of this fork, shown in the editor's "Add Node" dialog.
	// Secret registers its type even in the Client binary. The world breaks if a scene cannot load on both sides
	GDREGISTER_CLASS(Online);
	GDREGISTER_ABSTRACT_CLASS(MY);
	GDREGISTER_CLASS(Secret);
	GDREGISTER_ABSTRACT_CLASS(MVOnline);
	Node::set_queue_free_hook(&MVThing::guard_queue_free);
	Node::set_free_hook(&MVThing::guard_free);
	Node::set_tree_change_hook(&MVThing::guard_tree_change);
	SceneTree::set_initialize_callback(&MVAuto::install);
	online_singleton = memnew(MVOnline);
	// Authors extend the type name Online. The singleton is registered under a __-prefixed internal name authors cannot write
	Engine::get_singleton()->add_singleton(Engine::Singleton("__Online", online_singleton, "MVOnline"));
}

void uninitialize_mv_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	MVClientScript::clear();
	Node::set_queue_free_hook(nullptr);
	Node::set_free_hook(nullptr);
	Node::set_tree_change_hook(nullptr);
	SceneTree::set_initialize_callback(nullptr);
	Engine::get_singleton()->remove_singleton("__Online");
	memdelete(online_singleton);
	online_singleton = nullptr;
}
