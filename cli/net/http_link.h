// Own shared HTTP connection I/O separately from individual request results.
#pragma once
#include "cli/net/wire.h"
#include "cli/net/h2_core.h"
#include <map>
#include <set>

class GDHTTPPeer;

class GDHTTPLink : public RefCounted {
	std::set<GDHTTPPeer *> owners; // Live request reservations, detached before peer destruction.
	std::map<uint32_t, GDHTTPPeer *> streams; // Active response destinations by wire stream ID.
	std::deque<uint32_t> writers; // Ready uploads in fair stream order.
	std::vector<uint8_t> output; // One stable transport unit retained across encrypted write retries.
	size_t at = 0; // Sent prefix of the current output unit.
	bool posted = false, running = false, closed = false; // Notification coalescing and connection lifetime.
	bool drain_error = false; // Preserve an earlier nonzero shutdown error across later graceful notifications.
	void step(); // Advance shared read/write readiness without invoking request callbacks inline.
	void events(); // Route typed protocol events to their request owners.
	void upload(); // Queue one available DATA fragment per ready request.
	void fail(); // Notify every active request when the physical connection fails.
	friend class GDHTTPPeer;
public:
	Wire wire; // Native socket and worker-isolated authenticated record transport.
	std::unique_ptr<GDH2::Connection> h2; // Multiplexed framing, absent for HTTP/1 connections.
	uint64_t idle_at = 0; // Time at which the final active reservation was released.
	Callable idle_call; // Pool timer maintenance when the final reservation leaves, including cancellation.
	void enable(); // Install the authenticated multiplexed protocol and its native readiness target.
	bool available() const; // Determine whether another request can reserve a stream.
	bool alive() const; // Exclude terminal and draining links from reuse.
	bool busy() const { return !owners.empty(); } // Keep active streams outside idle expiry.
	void attach(GDHTTPPeer *peer); // Reserve one logical request without opening a wire stream yet.
	void detach(GDHTTPPeer *peer); // Cancel only the departing request and return its receive credit.
	void kick(); // Schedule protocol work once without waiting for a new kernel edge.
	~GDHTTPLink(); // Close the descriptor after all owners and the idle pool relinquish it.
};
