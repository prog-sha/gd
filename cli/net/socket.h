/**************************************************************************/
/*  socket.h                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Expose TCP streams and UDP packets as asynchronous APIs that suspend only their callers.

#include "cli/sys/std.h"

#include "cli/net/datagram.h"
#include "cli/net/tls.h"
#include "cli/net/stream.h"
#include "core/object/ref_counted.h"
#include "core/templates/list.h"

class GDTCPConn;
class GDTCPListener;
class GDUDPPacketConn;
class GDTLSDialCall;

// One TCP I/O operation, retained until completion or cancellation.
class GDTCPCall : public RefCounted {
	GDCLASS(GDTCPCall, RefCounted);

	Ref<GDTCPCall> self_hold;
	GDTCPConn *owner = nullptr; // Caller detached when the connection completes the operation.
	PackedByteArray data;
	int64_t at = 0; // Bytes already written.
	int max = 0; // Requested read capacity.
	uint64_t due = 0; // Deadline, or zero for no deadline.
	bool writing = false; // Whether this is a write operation.

	friend class GDTCPConn;
	void done(const Ref<R> &p_result);

protected:
	static void _bind_methods();

public:
	// Cancel the operation from its waiting caller.
	void cancel();
};

// One connection attempt, using workers for DNS and kernel readiness for connection waits.
class GDTCPDialCall : public RefCounted {
	GDCLASS(GDTCPDialCall, RefCounted);

	struct Lane {
		PackedStringArray addresses; // Candidates belonging to one address family.
		Ref<GDStream> peer; // Connection currently attempted in this family.
		Ref<R> error; // First error returned if all candidates fail.
		int next = 0; // Next candidate to attempt.
		int retries = 0; // Ephemeral-port reselection count, not an address-candidate limit.
		uint64_t due = 0; // Deadline allocated to the current candidate.
	};
	Ref<GDTCPDialCall> self_hold;
	Lane lanes[2]; // Primary and fallback lanes for the two address families.
	String host;
	int port = 0;
	uint64_t due = 0; // Deadline, or zero for no deadline.
	uint64_t fallback = 0; // Time to start the fallback family.
	uint64_t wake_due = 0; // Earliest deadline registered with the kernel wait.
	bool watching = false; // Whether event-loop notifications are attached.
	bool prepared = false; // Whether name resolution has completed.
	Signal lookup; // This connection's independent lookup wait.

	void resolved(const Ref<R> &p_result);
	void step();
	bool advance(Lane &p_lane); // Try candidates in order within one family.
	void arm_deadline(); // Combine candidate, fallback, and overall deadlines into one notification.
	void done(const Ref<R> &p_result);
	void watch(bool p_on);

protected:
	static void _bind_methods();

public:
	static Signal start(const String &p_host, int64_t p_port, const Dictionary &p_opts);
	// Cancel all connection waits at process shutdown.
	static void shutdown_all();
	void cancel();
};

// One TCP byte stream with independent FIFO serialization for reads and writes.
class GDTCPConn : public RefCounted {
	GDCLASS(GDTCPConn, RefCounted);

	Ref<GDStream> native; // Native plaintext TCP transport.
	Ref<GDTLS> tls; // Encrypted stream used only for TLS connections.
	String tls_name; // Server name used for certificate checks and connection state.
	List<Ref<GDTCPCall>> reads;
	List<Ref<GDTCPCall>> writes;
	uint64_t read_due = 0; // Deadline for new and pending reads.
	uint64_t write_due = 0; // Deadline for new and pending writes.
	uint64_t due = 0; // Earliest deadline registered with the kernel wait.
	bool watching = false; // Active only while operations are pending.

	friend class GDTCPCall;
	friend class GDTCPDialCall;
	friend class GDTCPListener;
	friend class GDTLSDialCall;
	friend class GDWireDial;
	void step();
	void watch(bool p_on);
	void arm_deadline();
	void cancel_call(GDTCPCall *p_call);
	void fail_all(const Ref<R> &p_result);
	static Ref<GDTCPConn> take(const Ref<GDStream> &p_peer);
	static Ref<GDTCPConn> take_tls(const Ref<GDStream> &p_peer, const Ref<GDTLS> &p_tls, const String &p_name);

protected:
	static void _bind_methods();

public:
	Signal read(int64_t p_max);
	Signal read_async(int64_t p_max) { return read(p_max); }
	Signal write(const PackedByteArray &p_data);
	Signal write_async(const PackedByteArray &p_data) { return write(p_data); }
	Ref<R> set_deadline(double p_seconds);
	Ref<R> set_read_deadline(double p_seconds);
	Ref<R> set_write_deadline(double p_seconds);
	Dictionary local_addr() const;
	Dictionary remote_addr() const;
	Dictionary connection_state() const;
	bool is_open() const;
	void close();
	~GDTCPConn();
};

// One TLS connection and handshake sharing an overall deadline.
class GDTLSDialCall : public RefCounted {
	GDCLASS(GDTLSDialCall, RefCounted);

	Ref<GDTLSDialCall> self_hold;
	Ref<GDStream> peer; // Native TCP connection selected from all candidates.
	Ref<GDTLS> tls; // Encryption state over the native connection.
	Signal pending; // TCP connection wait receiving cancellation.
	String host;
	String server_name;
	String ca_file;
	Ref<GDTrust> ca;
	Ref<GDTLSIdentity> identity; // Client credentials loaded before connecting and retained through authentication.
	PackedStringArray protocols; // Ordered ALPN offers validated before starting network work.
	int port = 0;
	uint64_t due = 0; // Shared deadline for connection and handshake.
	bool insecure = false; // Disable certificate verification only on explicit request.
	bool watching = false; // Whether event-loop notifications are attached.

	void prepared(const Ref<R> &p_result);
	void connected(const Ref<R> &p_result); // Start the TLS handshake after TCP completion.
	void step();
	void done(const Ref<R> &p_result);
	void watch(bool p_on);

protected:
	static void _bind_methods();

public:
	// Start a TLS connection and return its completion signal.
	static Signal start(const String &p_host, int64_t p_port, const Dictionary &p_opts);
	// Cancel all handshake waits at process shutdown.
	static void shutdown_all();
	// Cancel connection setup and the handshake.
	void cancel();
};

// One TCP accept operation, completed as cancelled when its listener closes.
class GDTCPAcceptCall : public RefCounted {
	GDCLASS(GDTCPAcceptCall, RefCounted);

	Ref<GDTCPAcceptCall> self_hold;
	GDTCPListener *owner = nullptr; // Listener detached on completion.
	uint64_t due = 0; // Deadline, or zero for no deadline.

	friend class GDTCPListener;
	void done(const Ref<R> &p_result);

protected:
	static void _bind_methods();

public:
	void cancel();
};

// TCP listener registered with the event loop only while accepts are pending.
class GDTCPListener : public RefCounted {
	GDCLASS(GDTCPListener, RefCounted);

	Ref<GDStream> server;
	List<Ref<GDTCPAcceptCall>> accepts;
	uint64_t accept_due = 0; // Deadline for new and pending accept operations.
	uint64_t due = 0; // Earliest deadline registered with the kernel wait.
	String host;
	bool watching = false; // Active only while accepts are pending.

	friend class GDTCPAcceptCall;
	void step();
	void watch(bool p_on);
	void arm_deadline();
	void cancel_call(GDTCPAcceptCall *p_call);

protected:
	static void _bind_methods();

public:
	static Ref<R> listen(const String &p_host, int64_t p_port);
	Signal accept();
	Signal accept_async() { return accept(); }
	Ref<R> set_deadline(double p_seconds);
	Dictionary addr() const;
	bool is_open() const;
	void close();
	~GDTCPListener();
};

// One UDP operation preserving packet boundaries and peer addresses.
class GDUDPCall : public RefCounted {
	GDCLASS(GDUDPCall, RefCounted);

	Ref<GDUDPCall> self_hold;
	GDUDPPacketConn *owner = nullptr; // UDP endpoint detached on completion.
	PackedByteArray data;
	String host;
	int port = 0;
	int max = 0; // Requested receive capacity.
	uint64_t due = 0; // Deadline, or zero for no deadline.
	bool writing = false; // Whether this is a send operation.

	friend class GDUDPPacketConn;
	void done(const Ref<R> &p_result);

protected:
	static void _bind_methods();

public:
	void cancel();
};

// UDP endpoint with independent receive/send FIFOs, waking all waiters on close.
class GDUDPPacketConn : public RefCounted {
	GDCLASS(GDUDPPacketConn, RefCounted);

	Ref<GDDatagram> peer;
	List<Ref<GDUDPCall>> reads;
	List<Ref<GDUDPCall>> writes;
	uint64_t read_due = 0; // Deadline for new and pending receives.
	uint64_t write_due = 0; // Deadline for new and pending sends.
	uint64_t due = 0; // Earliest deadline registered with the kernel wait.
	String host;
	bool watching = false; // Active only while operations are pending.

	friend class GDUDPCall;
	void step();
	void watch(bool p_on);
	void arm_deadline();
	void cancel_call(GDUDPCall *p_call);
	void fail_all(const Ref<R> &p_result);

protected:
	static void _bind_methods();

public:
	static Ref<R> listen(const String &p_host, int64_t p_port, int64_t p_buffer);
	Signal read_from(int64_t p_max);
	Signal read_from_async(int64_t p_max) { return read_from(p_max); }
	Signal write_to(const PackedByteArray &p_data, const String &p_host, int64_t p_port);
	Signal write_to_async(const PackedByteArray &p_data, const String &p_host, int64_t p_port) { return write_to(p_data, p_host, p_port); }
	Ref<R> set_deadline(double p_seconds);
	Ref<R> set_read_deadline(double p_seconds);
	Ref<R> set_write_deadline(double p_seconds);
	Dictionary addr() const;
	bool is_open() const;
	void close();
	~GDUDPPacketConn();
};
