/**************************************************************************/
/*  handoff.h                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Delegate editor-side package commands before display startup.

namespace View {

// Return 0 to continue in this process. Return a process status when the
// package command cannot be handed off. A successful replacement does not return.
int handoff(int argc, char **argv);

} // namespace View
