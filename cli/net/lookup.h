// Resolve names on I/O workers without a persistent application-level DNS cache.
#pragma once

#include "cli/sys/std.h"
#include "cli/sys/pool.h"
#include "core/templates/list.h"

class GDLookupCall;

// Combine concurrent lookups for the same host into one worker job.
class GDLookupJob : public PoolJob {
	GDCLASS(GDLookupJob, PoolJob);
	String host; // Resolver input with case and zone preserved.
	List<Ref<GDLookupCall>> calls; // Independent callers awaiting the shared result.
	VariantPair result; // Address candidates and failure passed from the worker to the main thread.
	friend class GDLookupCall;

protected:
	static void _bind_methods() {} // Keep worker jobs outside the script API.
	void run() override; // Call the OS resolver once.
	void finish() override; // Remove the registration before delivering results to each caller.
};

// Keep cancellation and permissions independent for callers sharing a host lookup.
class GDLookupCall : public RefCounted {
	GDCLASS(GDLookupCall, RefCounted);
	Ref<GDLookupCall> self_hold; // Retain this call until completion delivery.
	Ref<GDLookupJob> job; // Shared resolver job.
	List<Ref<GDLookupCall>>::Element *entry = nullptr; // List position for constant-time cancellation.
	int port = -1; // Check resolved-IP permissions when resolution precedes a connection.
	bool single = false; // Whether the public resolver expects a single value.
	VariantPair pending; // Numeric address delivered after the caller can connect.
	friend class GDLookupJob;
	void done(const VariantPair &p_result); // Adapt the shared result to this caller's result shape.
	void deliver(); // Deliver a numeric result on the next runtime turn.

protected:
	static void _bind_methods(); // Register completion and cancellation only.

public:
	static Signal start(const String &p_host, bool p_single = false, int p_port = -1); // Share concurrent resolutions, not completed results.
	static void shutdown_all(); // Release waiters independently of late OS resolver completion.
	void cancel(); // Leave other callers' shared lookup running.
};

namespace GDLookup {
// Return all candidates in OS order for connection retries by address family.
VariantPair all(const String &p_host);
}
