// Compile the database amalgamation with thread safety and restricted extension access.

#define SQLITE_THREADSAFE 1 // Enable internal synchronization.
#define SQLITE_DQS 0 // Require standard quoting for string literals.
#define SQLITE_OMIT_DEPRECATED 1 // Exclude obsolete interfaces.
#define SQLITE_OMIT_LOAD_EXTENSION 1 // Keep executable extension loading behind the runtime API.
#define SQLITE_OMIT_SHARED_CACHE 1 // Use independent connection caches.

#include "sqlite3.c"
