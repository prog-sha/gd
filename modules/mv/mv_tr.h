/**************************************************************************/
/*  mv_tr.h                                                               */
/**************************************************************************/

// Japanese text for editor UI strings. Adds only this module's entries without touching the engine's translation tables.

#pragma once

#ifdef TOOLS_ENABLED

class MVTr {
public:
	static const char *SECRET_HINT; // Explanation for the ⚠ shown on an Online without a Secret
	static void install(); // Add Japanese to the editor language
};

#endif
