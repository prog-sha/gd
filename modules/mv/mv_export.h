/**************************************************************************/
/*  mv_export.h                                                           */
/**************************************************************************/

// Export hook that keeps Secret scripts out of the Client PCK.
// Dropping them at runtime would only mean "does not run". Keeping them out of the export
// means "cannot be had".

#pragma once

#ifdef TOOLS_ENABLED

#include "core/templates/hash_set.h"
#include "editor/export/editor_export_plugin.h"

class MVExport : public EditorExportPlugin {
	GDCLASS(MVExport, EditorExportPlugin);

	bool keeps_secret = false; // Whether this preset is an export allowed to hold Secret scripts
	bool replaced = false; // Whether this hook replaced the current file
	bool debug_build = false; // Whether this export uses the debug template
	String export_path; // Output path of this export. For server exports, the container definition is written next to it
	HashSet<String> secret_scripts; // Scripts used only under Secret
	HashSet<String> plain_scenes; // Scenes found to have no Secret during the pre-scan. Not reloaded

	void find_secret_scripts(); // Find scripts under Secret before exporting
	static void collect(Node *p_node, bool p_inside, HashSet<String> &r_inside, HashSet<String> &r_outside); // Inspect one scene

	void strip_secret(const String &p_path, const String &p_type); // Keep Secret contents out of the PCK
	void keep_text(const String &p_path); // Put the world scene into the PCK as text
	void replace(const String &p_path, const String &p_text); // Put this text in place of the original file
	void replace(const String &p_path, const Vector<uint8_t> &p_bytes); // Same, with raw bytes
	void blank(const String &p_path, const String &p_why); // Empty out a scene that could not be stripped
	void write_container(); // Write Dockerfile and compose.yaml next to the server export

protected:
	void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;
	void _export_end() override;

public:
	static const char *SERVER_FEATURE; // Feature marker of presets allowed to hold Secret scripts
	static String container_userdata(); // Where user:// lives inside the container

	static bool server_preset(const Ref<EditorExportPreset> &p_preset, bool p_debug); // Whether the preset points to a server template
	static int listen_port(); // Port the Online side listens on. Secret's listen_port, else room_port, else the default

	String get_name() const override { return "Online"; }
};

#endif // TOOLS_ENABLED
