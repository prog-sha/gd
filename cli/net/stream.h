// Connect TCP dialing, accepting, and partial I/O to kernel readiness notifications.
#pragma once

#include "cli/net/native.h"
#include "cli/net/write.h"
#include "core/variant/dictionary.h"

class GDStream : public GDNative {
public:
	enum State { CLOSED, CONNECTING, CONNECTED, LISTENING, BROKEN }; // Descriptor communication state.
	static constexpr int MAX_WRITE = 1 << 30; // Maximum bytes from one region in a stream system call.

private:
	State state = CLOSED; // State from connection setup through closure.
	bool eof = false; // Receive-side EOF, independent of send-side closure.
	int dial_error = 0; // OS connection error used to decide whether to select another ephemeral port.
	void defaults(); // Apply the default TCP settings to a connected socket.

public:
	Error dial(const String &p_host, int p_port); // Begin connecting to a numeric address.
	Error listen(const String &p_host, int p_port, bool p_reuse = false); // Open a listener using the OS backlog setting.
	Error accept(Ref<GDStream> &r_peer); // Accept only connections that have already arrived.
	void poll(); // Confirm connection completion with SO_ERROR and getpeername.
	void probe(); // Check for receive EOF without consuming data.
	bool retryable() const; // Identify retryable self-connections and EADDRNOTAVAIL failures.
	State status() const { return is_open() ? state : CLOSED; } // A closed descriptor reports CLOSED regardless of its previous state.
	bool read_eof() const { return eof; } // Report a received FIN.
	int available() const; // Query the kernel for the required read-buffer size.
	Error read(uint8_t *p_data, int p_size, int &r_got); // Distinguish partial reads, EOF, and EAGAIN.
	Error write(const uint8_t *p_data, int p_size, int &r_sent); // Return the partial write count to the caller.
	Error write_parts(GDWrites p_parts, int64_t &r_sent); // Gather framing and body without copying their contents.
	Dictionary addr(bool p_remote = false) const; // Return the socket's local or remote address.
};
