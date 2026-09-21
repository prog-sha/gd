/**************************************************************************/
/*  reg.h                                                                 */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Centralize standard-library type registration in ClassDB.
// Expose one registration hook instead of listing application types at each startup call site.

#pragma once

void register_cli_types();
void shutdown_cli_runtime(); // Stop workers and release waits before destroying the script runtime.
void unregister_cli_types();
