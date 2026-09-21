// Share native socket ownership and readiness registration across transports.
#pragma once

#include "core/object/ref_counted.h"
#include "cli/sys/std.h"

class GDNative : public RefCounted {
	bool reading = false; // Read interest registered with the kernel.
	bool writing = false; // Write interest registered with the kernel.
	Callable ready; // Delivery target retained when only readiness directions change.
	Ref<R> poll_error; // Kernel registration failure propagated to subsequent waits.
	size_t users = 0; // Descriptor leases acquired and released only by the owning runtime.
	bool closing = false; // Logical closure while an already-dispatched operation still owns the descriptor.
	void destroy(); // Close the OS handle after the final in-flight operation finishes.

protected:
	intptr_t fd = -1; // Owned descriptor, released only after poller removal.
	Error open_socket(int p_family, int p_type); // Open a nonblocking socket that child processes cannot inherit.
	static Error configure(intptr_t p_fd); // Make accepted sockets nonblocking and noninheritable.

public:
	static int last_error(); // Return the OS error immediately after a system call.
	static Error failure(int p_error); // Distinguish EAGAIN as a readiness wait.
	static bool interrupted(int p_error); // Identify EINTR as a reason to retry the same system call.
	void watch(bool p_read, bool p_write, const Callable &p_call); // Register only the required readiness directions with the kernel.
	void read_wait(bool p_on) { watch(p_on, writing, ready); } // Toggle read readiness only.
	void write_wait(bool p_on) { watch(reading, p_on, ready); } // Toggle write readiness only.
	void set_callback(const Callable &p_call) { watch(reading, writing, p_call); } // Replace the delivery target for the current readiness interests.
	bool is_open() const { return fd != -1 && !closing; } // Reject new operations immediately after logical closure.
	bool retain_io(); // Pin the descriptor before dispatching a worker, preventing reuse during its system calls.
	void release_io(); // Release a completed worker's descriptor lease on the owning runtime.
	Ref<R> wait_error() const { return poll_error; } // Distinguish registration failure from an ordinary peer disconnect.
	void close(); // Unregister from the poller before closing the socket.
	~GDNative(); // Release the owned socket.
};
