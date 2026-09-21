// Connect native UDP sockets to asynchronous readiness notifications.
#pragma once

#include "cli/net/native.h"
#include "core/variant/dictionary.h"

class GDDatagram : public GDNative {
	int family = 0; // Socket address family.

public:
	static bool is_ip(const String &p_host); // Validate a numeric address without DNS.
	Error open(const String &p_host, int p_port, int p_buffer); // Open a nonblocking UDP endpoint.
	Error read(int p_max, PackedByteArray &r_data, Dictionary &r_packet); // Read one packet into caller-owned storage retained across waits.
	Error write(const PackedByteArray &p_data, const String &p_host, int p_port); // Send one packet without splitting it.
	int port() const; // Return the local port selected by the kernel.
};
