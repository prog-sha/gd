/**************************************************************************/
/*  mv_secret_client.cpp                                                 */
/**************************************************************************/

// Client TLS connection to the Secret through a Relay, with heartbeat and silence detection.

#include "mv_secret_client.h"

#include "mv_frame.h"

#include "core/io/ip.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

// Connect TCP to the Relay and start Secret TLS with the given CA and certificate name
String MVSecretClient::open(const String &p_host, int p_port, const String &p_common_name, const Ref<X509Certificate> &p_ca) {
	if (tcp.is_valid() || tls.is_valid()) {
		return U"Already connected to Secret";
	}
	if (p_host.is_empty() || p_port < 1 || p_port > 65535 || p_common_name.is_empty()) {
		return U"Invalid Secret address";
	}
	const IPAddress address = IP::get_singleton()->resolve_hostname(p_host);
	if (!address.is_valid()) {
		return vformat(U"Cannot resolve room_host=%s", p_host);
	}
	failed = false;
	was_ready = false;
	pending.clear();
	heard_at = OS::get_singleton()->get_ticks_msec();
	ping_at = heard_at;
	polled_at = heard_at;
	common_name = p_common_name;
	options = TLSOptions::client(p_ca);
	tcp.instantiate();
	const Error error = tcp->connect_to_host(address, p_port);
	return error == OK ? String() : vformat(U"Cannot connect to %s:%d. Check that the server is running", p_host, p_port);
}

// Advance connection setup, receive, heartbeat and disconnect once
void MVSecretClient::poll() {
	if (failed || tcp.is_null()) {
		return;
	}
	tcp->poll();
	// Time this side was stalled (no poll for 1 s or more, e.g. loading) is not counted as peer silence.
	// Otherwise a local hiccup would be taken as the peer disconnecting
	const uint64_t at = OS::get_singleton()->get_ticks_msec();
	if (polled_at != 0 && at - polled_at >= MVFrame::ALIVE_MS) {
		heard_at = at;
	}
	polled_at = at;
	// Silence while connecting is cut off after the same time as silence after connecting,
	// so a stalled peer does not cost tens of seconds in TCP connect or TLS handshake
	if (!was_ready && at - heard_at >= MVFrame::DEAD_MS) {
		fail(U"No response from the server");
		return;
	}
	if (tls.is_null()) {
		const StreamPeerTCP::Status status = tcp->get_status();
		if (status == StreamPeerTCP::STATUS_CONNECTED) {
			tls = Ref<StreamPeerTLS>(StreamPeerTLS::create());
			if (tls.is_null() || tls->connect_to_stream(tcp, common_name, options) != OK) {
				fail(U"Cannot start TLS");
			}
		} else if (status == StreamPeerTCP::STATUS_ERROR || status == StreamPeerTCP::STATUS_NONE) {
			fail(U"Cannot reach the room. Check that the server is running and room_host/room_port match");
		}
		return;
	}
	tls->poll();
	switch (tls->get_status()) {
		case StreamPeerTLS::STATUS_CONNECTED: {
			const uint64_t now = OS::get_singleton()->get_ticks_msec();
			if (!was_ready) {
				was_ready = true;
				heard_at = now;
				ping_at = now;
				emit_signal(SNAME("ready"));
			}
			if (now - ping_at >= MVFrame::ALIVE_MS) {
				if (send(MVFrame::pack(MVFrame::PING, Dictionary()).to_utf8_buffer())) {
					ping_at = now;
				}
			}
			const int count = MIN(tls->get_available_bytes(), MVFrame::READ_MAX);
			if (count > 0) {
				PackedByteArray data;
				data.resize(count);
				if (tls->get_data(data.ptrw(), count) == OK) {
					heard_at = now;
					emit_signal(SNAME("received"), data);
				}
			}
			if (now - heard_at >= MVFrame::DEAD_MS) {
				fail(U"No response from the server");
				return;
			}
			flush();
		} break;
		case StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH:
			fail(U"Certificate name mismatch. Check that secret_name matches the server settings");
			break;
		case StreamPeerTLS::STATUS_ERROR:
			fail(U"Cannot verify the certificate. Check that ca points to the CA certificate the server printed");
			break;
		case StreamPeerTLS::STATUS_DISCONNECTED:
			fail(U"Connection to the server was lost");
			break;
		default:
			break;
	}
}

// Append send data within limits to the TLS write queue
bool MVSecretClient::send(const PackedByteArray &p_data) {
	if (tls.is_null() || tls->get_status() != StreamPeerTLS::STATUS_CONNECTED || p_data.is_empty() ||
			p_data.size() > MVFrame::READ_MAX || pending.size() + p_data.size() > MVFrame::WRITE_MAX) {
		return false;
	}
	pending.append_array(p_data);
	return true;
}

// Close TCP and TLS, and notify disconnect if it was connected
void MVSecretClient::close() {
	if (tls.is_valid()) {
		tls->disconnect_from_stream();
	} else if (tcp.is_valid()) {
		tcp->disconnect_from_host();
	}
	tls.unref();
	tcp.unref();
	pending.clear();
	if (was_ready) {
		was_ready = false;
		emit_signal(SNAME("parted"));
	}
}

// Report only the first connection failure and close
void MVSecretClient::fail(const String &p_reason) {
	if (failed) {
		return;
	}
	failed = true;
	emit_signal(SNAME("failed"), p_reason);
	close();
}

// Remove the written part from the send queue
void MVSecretClient::flush() {
	if (pending.is_empty()) {
		return;
	}
	int sent = 0;
	if (tls->put_partial_data(pending.ptr(), pending.size(), sent) == OK && sent > 0) {
		pending = pending.slice(sent);
	}
}

// Expose Secret connection signals to GDScript
void MVSecretClient::_bind_methods() {
	ADD_SIGNAL(MethodInfo("ready"));
	ADD_SIGNAL(MethodInfo("received", PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data")));
	ADD_SIGNAL(MethodInfo("failed", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("parted"));
}
