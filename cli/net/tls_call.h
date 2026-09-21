// Load server credentials on a worker and publish the listener on its owning runtime.
#pragma once
#include "cli/net/serve.h"
#include "cli/sys/pool.h"

class GDWebTLSCall : public PoolJob {
	GDCLASS(GDWebTLSCall, PoolJob);
	friend class GDWebApp;
	Ref<GDWebApp> app; // Application retained until startup completion.
	Ref<RefCounted> token; // Cancelled when the application is stopped or another startup wins.
	Ref<R> result; // Validated immutable identity or a file/PEM error.
	String cert, key, host; // Permission-checked credential paths and bind address.
	Dictionary options; // Immutable TLS startup options copied before worker submission.
	int64_t port = 0; // Requested port, including ephemeral zero.
protected:
	static void _bind_methods();
	void run() override; // Read and parse credentials off-loop.
	void finish() override; // Bind only if this startup still owns the token.
};
