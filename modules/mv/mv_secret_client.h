/**************************************************************************/
/*  mv_secret_client.h                                                   */
/**************************************************************************/

// Client TLS connection that verifies the Secret certificate through a Relay.

#pragma once

#include "core/crypto/crypto.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/stream_peer_tls.h"
#include "core/object/ref_counted.h"

class MVSecretClient : public RefCounted {
	GDCLASS(MVSecretClient, RefCounted);

	Ref<StreamPeerTCP> tcp; // Raw TCP to the Relay
	Ref<StreamPeerTLS> tls; // Direct TLS to the Secret inside the Relay
	Ref<TLSOptions> options; // Trusted CA
	String common_name; // Certificate name to verify
	PackedByteArray pending; // Pending send data
	uint64_t heard_at = 0; // Last receive time
	uint64_t ping_at = 0; // Last heartbeat time
	uint64_t polled_at = 0; // Last poll time. Time this side was stalled is not counted as peer silence
	bool was_ready = false; // Whether TLS ready was notified
	bool failed = false; // Marker to avoid reporting the same disconnect twice

	void fail(const String &p_reason); // Report the reason and close
	void flush(); // Advance pending sends with partial writes

protected:
	static void _bind_methods();

public:
	String open(const String &p_host, int p_port, const String &p_common_name, const Ref<X509Certificate> &p_ca);
	void poll(); // Advance TCP, TLS, heartbeat, send and receive
	bool send(const PackedByteArray &p_data); // Append to the TLS send queue
	void close(); // Close network resources
};
