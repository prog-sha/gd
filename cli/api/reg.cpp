/**************************************************************************/
/*  reg.cpp                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Register and clean up standard-library types declared in reg.h.

#include "cli/api/reg.h"
#include "cli/run/loop.h"

#include "cli/api/box.h"
#include "cli/api/coll_call.h"
#include "cli/api/cli.h"
#include "cli/api/gen.h"
#include "cli/api/math.h"
#include "cli/api/singleton.h"
#include "cli/api/text.h"
#include "cli/data/codec.h"
#include "cli/data/format.h"
#include "cli/db/database.h"
#include "cli/db/pg.h"
#include "cli/db/redis.h"
#include "cli/db/sqlite.h"
#include "cli/net/fetch.h"
#include "cli/net/lookup.h"
#include "cli/sys/file_job.h"
#include "cli/sys/file_source.h"
#include "cli/sys/file_stream.h"
#include "cli/sys/pool.h"
#include "cli/sys/proc.h"
#include "cli/sys/sink.h"
#include "cli/net/http.h"
#include "cli/net/mw.h"
#include "cli/net/serve.h"
#include "cli/net/body_source.h"
#include "cli/net/tls_call.h"
#include "cli/net/socket.h"
#include "cli/sys/os.h"
#include "cli/sys/std.h"
#include "cli/sys/task.h"

#include "core/object/class_db.h"

// Register native standard-library types visible to scripts.
// Keep registration in the runtime rather than duplicating it in scripts.
void register_cli_types() {
	// Results and asynchronous control flow.
	GDREGISTER_CLASS(Err);
	GDREGISTER_CLASS(R);
	GDREGISTER_ABSTRACT_CLASS(GDTestCheck);
	GDREGISTER_INTERNAL_CLASS(GDWait);
	GDREGISTER_INTERNAL_CLASS(GDLoop);
	GDREGISTER_ABSTRACT_CLASS(GDAsyncContext);

	// Command entry points and logging.
	GDREGISTER_ABSTRACT_CLASS(GDCLIFlags);
	GDREGISTER_INTERNAL_CLASS(GDFileLock);
	GDREGISTER_INTERNAL_ABSTRACT_CLASS(PoolJob);
	GDREGISTER_INTERNAL_CLASS(GDFileCall);
	GDREGISTER_INTERNAL_CLASS(GDLogCall);
	GDREGISTER_INTERNAL_CLASS(GDValueCall);
	GDREGISTER_INTERNAL_CLASS(GDFormatCall);
	GDREGISTER_INTERNAL_CLASS(GDFormatJob);
	GDREGISTER_INTERNAL_CLASS(GDCollectionCall);
	GDREGISTER_ABSTRACT_CLASS(GDBodySource);
	GDREGISTER_INTERNAL_CLASS(FileSource);
	GDREGISTER_ABSTRACT_CLASS(GDWebWriter);
	GDREGISTER_INTERNAL_CLASS(GDWebWriteCall);
	GDREGISTER_ABSTRACT_CLASS(GDGzipWriter);
	GDREGISTER_INTERNAL_CLASS(GDGzipCall);
	GDREGISTER_INTERNAL_CLASS(FileSourceJob);
	GDREGISTER_ABSTRACT_CLASS(GDFileStream);
	GDREGISTER_INTERNAL_CLASS(GDFileStreamCall);
	GDREGISTER_INTERNAL_CLASS(FileSink);
	GDREGISTER_INTERNAL_CLASS(FileSinkJob);
	GDREGISTER_INTERNAL_CLASS(GDProcCall);
	GDREGISTER_INTERNAL_CLASS(ProcWaitJob);

	// Networking.
	GDREGISTER_INTERNAL_CLASS(GDHTTPCall);
	GDREGISTER_INTERNAL_CLASS(GDHTTPPeer);
	GDREGISTER_INTERNAL_CLASS(GDTCPCall);
	GDREGISTER_INTERNAL_CLASS(GDTCPDialCall);
	GDREGISTER_INTERNAL_CLASS(GDTLSDialCall);
	GDREGISTER_INTERNAL_CLASS(GDTrust);
	GDREGISTER_INTERNAL_CLASS(GDTLSIdentity);
	GDREGISTER_INTERNAL_CLASS(GDWebTLSCall);
	GDREGISTER_INTERNAL_CLASS(GDLookupJob);
	GDREGISTER_INTERNAL_CLASS(GDLookupCall);
	GDREGISTER_INTERNAL_CLASS(GDWireDial);
	GDREGISTER_ABSTRACT_CLASS(GDTCPConn);
	GDREGISTER_INTERNAL_CLASS(GDTCPAcceptCall);
	GDREGISTER_ABSTRACT_CLASS(GDTCPListener);
	GDREGISTER_INTERNAL_CLASS(GDUDPCall);
	GDREGISTER_ABSTRACT_CLASS(GDUDPPacketConn);
	GDREGISTER_ABSTRACT_CLASS(GDWebServer);
	GDREGISTER_INTERNAL_CLASS(GDWebBodyCall);
	GDREGISTER_ABSTRACT_CLASS(GDWebRequest);
	GDREGISTER_ABSTRACT_CLASS(GDWebMiddleware);
	GDREGISTER_INTERNAL_CLASS(GDWebJwt);
	GDREGISTER_INTERNAL_CLASS(GDWebJwtCall);
	GDREGISTER_INTERNAL_CLASS(GDWebViewCall);
	GDREGISTER_INTERNAL_CLASS(GDWebValid);
	GDREGISTER_INTERNAL_CLASS(GDWebRateLimit);
	GDREGISTER_INTERNAL_CLASS(GDWebCSRF);
	GDREGISTER_ABSTRACT_CLASS(GDWebSessionStore);
	GDREGISTER_ABSTRACT_CLASS(GDWebApp);
	GDREGISTER_ABSTRACT_CLASS(GDWebRouteGroup);
	GDREGISTER_ABSTRACT_CLASS(GDHTTPResponse);

	// Databases.
	GDREGISTER_ABSTRACT_CLASS(GDPostgresClient);
	GDREGISTER_INTERNAL_CLASS(GDPostgresCallInternal);
	GDREGISTER_INTERNAL_CLASS(GDPostgresPoolCall);
	GDREGISTER_INTERNAL_CLASS(ScramKeyJob);
	GDREGISTER_INTERNAL_CLASS(GDPostgresPackJob);
	GDREGISTER_ABSTRACT_CLASS(GDPostgresPool);
	GDREGISTER_ABSTRACT_CLASS(GDDatabaseTx);
	GDREGISTER_ABSTRACT_CLASS(GDDatabaseRows);
	GDREGISTER_INTERNAL_CLASS(GDDatabaseTxCall);
	GDREGISTER_ABSTRACT_CLASS(GDDatabaseClient);
	GDREGISTER_INTERNAL_CLASS(GDDatabaseCall);
	GDREGISTER_ABSTRACT_CLASS(GDRedisClient);
	GDREGISTER_INTERNAL_CLASS(GDRedisPackJob);
	GDREGISTER_INTERNAL_CLASS(GDRedisCallInternal);
	GDREGISTER_INTERNAL_CLASS(GDRedisPoolCall);
	GDREGISTER_ABSTRACT_CLASS(GDRedisPool);
	GDREGISTER_ABSTRACT_CLASS(GDSQLiteDB);
	GDREGISTER_ABSTRACT_CLASS(GDSQLiteStatement);
	GDREGISTER_INTERNAL_CLASS(GDSQLiteAPI);

	// Encodings and archives.
	GDREGISTER_ABSTRACT_CLASS(GDJSONLReader);

	// Containers.
	GDREGISTER_ABSTRACT_CLASS(GDBinaryHeap);
	GDREGISTER_ABSTRACT_CLASS(GDPriorityQueue);
	GDREGISTER_ABSTRACT_CLASS(GDLRUCache);
	GDREGISTER_ABSTRACT_CLASS(GDMemoizedCallable);

	// Register global API objects under names distinct from their types.
	GDREGISTER_INTERNAL_CLASS(GDAsyncAPI);
	GDREGISTER_INTERNAL_CLASS(GDLogAPI);
	GDREGISTER_INTERNAL_CLASS(GDNetAPI);
	GDREGISTER_INTERNAL_CLASS(GDHTTPAPI);
	GDREGISTER_INTERNAL_CLASS(GDFSAPI);
	GDREGISTER_INTERNAL_CLASS(GDCollectionsAPI);
	GDREGISTER_INTERNAL_CLASS(GDCodecAPI);
	GDREGISTER_INTERNAL_CLASS(GDIDAPI);
	GDREGISTER_INTERNAL_CLASS(GDTextAPI);
	GDREGISTER_INTERNAL_CLASS(GDHTMLAPI);
	GDREGISTER_ABSTRACT_CLASS(GDHTMLTemplate);
	GDREGISTER_INTERNAL_CLASS(GDBitsAPI);
	GDREGISTER_INTERNAL_CLASS(GDMathAPI);
	GDREGISTER_INTERNAL_CLASS(GDSemanticVersionAPI);
	GDREGISTER_INTERNAL_CLASS(GDDateTimeAPI);
	GDREGISTER_INTERNAL_CLASS(GDCLIAPI);
	GDREGISTER_INTERNAL_CLASS(GDTestAPI);
	GDREGISTER_INTERNAL_CLASS(GDAPI);
	GDREGISTER_INTERNAL_CLASS(GDWebAPI);
	GDREGISTER_INTERNAL_CLASS(GDDatabaseAPI);
	GDREGISTER_INTERNAL_CLASS(GDPostgresAPI);
	GDREGISTER_INTERNAL_CLASS(GDRedisAPI);
	register_cli_singletons();
}

// Release worker-retained scripts while their language runtime remains alive.
void shutdown_cli_runtime() {
	Pool::shutdown(); // Stop waiting workers before destroying their signal sources.
	GDHTTPCall::shutdown_all(); // Cancel pending event-loop HTTP requests.
	GDWait::shutdown_all(); // Cancel timers and composed waits.
	GDTCPDialCall::shutdown_all(); // Cancel TCP dials, including those past name resolution.
	GDTLSDialCall::shutdown_all(); // Cancel TLS handshakes, including those past name resolution.
	GDLookupCall::shutdown_all(); // Release waiters on shared resolver jobs.
	GDDatabaseClient::shutdown_all(); // Stop database connections and dedicated embedded SQL workers outside Pool.
	GDPostgresClient::shutdown_all(); // Close remote SQL connections awaiting replies.
	GDRedisClient::shutdown_all(); // Close key-value connections awaiting replies.
	GDCollectionCall::shutdown(); // Release script references held by incomplete collection operations.
	Async::shutdown(); // Release references held by undelivered futures last.
	GDTrust::shutdown(); // Release shared trust after certificate workers and delivery finish.
}

// Clean up startup-failure waits before unregistering global APIs.
void unregister_cli_types() {
	shutdown_cli_runtime();
	unregister_cli_singletons(); // Unregister global APIs.
}
