/**************************************************************************/
/*  register_types.h                                                      */
/**************************************************************************/

// Entry point that registers the world-building parts with the engine.

#pragma once

#include "modules/register_module_types.h"

void initialize_mv_module(ModuleInitializationLevel p_level);
void uninitialize_mv_module(ModuleInitializationLevel p_level);
