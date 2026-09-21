/**************************************************************************/
/*  wait.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Wait collectively for registered I/O endpoints.
// Let persistent loops sleep until work becomes available.
// Internal socket and pipe implementations register on opening and unregister on closing.
//
// Use kqueue on macOS, epoll on Linux, and WSAPoll on Windows.
// Retain registrations across waits and provide a dedicated cross-thread wakeup channel.

#pragma once

#include "core/variant/callable.h"

#include <stdint.h>

class IdleWait {
public:
	// Initialize the platform wait backend before asynchronous producers start.
	static void init();

	// Add or remove monitoring from socket and pipe implementations.
	static int add(intptr_t p_fd, bool p_read = true, bool p_write = false, const Callable &p_ready = Callable()); // Return zero on success or the original OS error.
	static int change(intptr_t p_fd, bool p_old_read, bool p_old_write, bool p_read, bool p_write, const Callable &p_ready); // Replace nonempty interests on an existing registration.
	static const char *operation(); // Return the system call responsible for registration failure.
	static void remove(intptr_t p_fd, bool p_read = true, bool p_write = false);
	static void callback(intptr_t p_fd, const Callable &p_call); // Update delivery without changing kernel registration.

	// Sleep until a registered endpoint is ready or a deadline arrives.
	// Zero timeout polls immediately; UINT64_MAX waits indefinitely.
	static int wait(uint64_t p_timeout_usec);
	// Invoke only sockets found ready by the preceding kernel wait on the main thread.
	static void dispatch();

	// Return the monitored count for choosing wait behavior.
	static int count();
	// Interrupt sleeping from any thread.
	static void wake(bool p_notify = true);
};
