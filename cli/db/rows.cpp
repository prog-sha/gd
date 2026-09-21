/**************************************************************************/
/*  rows.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Expose sequential remote SQL and embedded SQL result rows to script.

#include "cli/db/rows.h"

#include "cli/db/database.h"
#include "cli/db/pg.h"
#include "cli/db/sqlite.h"
#include "cli/sys/task.h"
#include "cli/sys/wait.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

// Close remaining backend resources.
GDDatabaseRows::~GDDatabaseRows() {
	close();
	if (pg.is_valid() && pg->stream == this) {
		pg->stream = nullptr; // Continue draining the protocol even after the caller discards Rows.
	}
}

// Attach sequential reading to a remote SQL query.
void GDDatabaseRows::attach_postgres(const Ref<GDPostgresCallInternal> &p_call) {
	pg = p_call;
	if (pg.is_valid()) {
		pg->stream = this;
		pg->stream_mode = true;
		names = pg->columns; // Retain column names when prepared execution skips Describe.
	}
}

// Receive remote SQL column names.
void GDDatabaseRows::postgres_columns(const PackedStringArray &p_names) {
	MutexLock lock(mutex);
	names = p_names;
}

// Receive one remote SQL row and deliver it to the pending Next call.
void GDDatabaseRows::postgres_row(const Array &p_values) {
	bool notify = false;
	{
		MutexLock lock(mutex);
		if (closed) {
			return;
		}
		current = p_values;
		row_ready = true;
		row_count++;
		notify = next_pending;
	}
	if (notify) {
		deliver(true);
	}
}

// Receive remote SQL EOF or failure and finish a pending Next call.
void GDDatabaseRows::postgres_done(const Ref<Err> &p_error, const String &p_tag) {
	bool notify = false;
	{
		MutexLock lock(mutex);
		if (ended) {
			return;
		}
		failed = p_error;
		tag = p_tag;
		ended = true;
		closed = true;
		notify = next_pending;
	}
	release_transaction();
	if (notify) {
		deliver(false);
	}
}

// Detach references before reusing a remote SQL query operation.
void GDDatabaseRows::postgres_detach(GDPostgresCallInternal *p_call) {
	if (pg.ptr() == p_call) {
		pg.unref();
	}
}

// Register an embedded SQL statement on its worker.
void GDDatabaseRows::sqlite_opened(sqlite3_stmt *p_stmt, const PackedStringArray &p_names) {
	MutexLock lock(mutex);
	stmt = p_stmt;
	names = p_names;
}

// Publish one row or termination from the embedded SQL worker.
void GDDatabaseRows::sqlite_result(const Array &p_values, bool p_done, const Ref<Err> &p_error, const String &p_tag) {
	{
		MutexLock lock(mutex);
		if (!closed && !p_done) {
			current = p_values;
			row_count++;
		}
		if (p_done) {
			failed = p_error;
			tag = p_tag;
			ended = true;
			closed = true;
		}
		worker_value = !closed && !p_done;
		worker_ready = true;
	}
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDDatabaseRows::step));
}

// Close embedded SQL Rows when their connection ends.
void GDDatabaseRows::sqlite_shutdown(const Ref<Err> &p_error) {
	{
		MutexLock lock(mutex);
		if (!closed) {
			failed = p_error;
		}
		stmt = nullptr;
		ended = true;
		closed = true;
		worker_value = false;
		worker_ready = true;
	}
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDDatabaseRows::step));
}

// Deliver a pending Next result through a main-thread signal.
void GDDatabaseRows::deliver(bool p_value) {
	{
		MutexLock lock(mutex);
		if (!next_pending) {
			return;
		}
		p_value = p_value && !closed; // Deferred prefetched rows cannot become visible after closure.
		next_pending = false;
		row_ready = false;
		current_valid = p_value;
	}
	emit_signal("nexted", p_value);
}

// Transfer embedded SQL worker results to the main thread.
void GDDatabaseRows::step() {
	bool ready = false;
	bool value = false;
	{
		MutexLock lock(mutex);
		if (worker_ready) {
			worker_ready = false;
			ready = true;
			value = worker_value;
		}
	}
	if (!ready) {
		return;
	}
	if (!value) {
		release_transaction();
	}
	deliver(value);
}

// Remove Rows from transaction tracking.
void GDDatabaseRows::release_transaction() {
	Ref<GDDatabaseClient> db;
	uint64_t generation = 0;
	{
		MutexLock lock(mutex);
		generation = tx_generation;
		tx_generation = 0;
		db = owner;
	}
	if (generation > 0 && db.is_valid()) {
		db->untrack_tx_rows(this, generation);
	}
}

// Advance one row; return false on EOF or failure.
Signal GDDatabaseRows::next() {
	const Signal signal(this, "nexted");
	Ref<GDPostgresCallInternal> pg_call;
	Ref<GDDatabaseClient> sqlite_owner;
	bool cached = false;
	bool done = false;
	bool duplicate = false;
	{
		MutexLock lock(mutex);
		if (next_pending) {
			failed = Err::make("database Rows.Next is already waiting", Err::ALREADY_EXISTS);
			ended = true;
			closed = true;
			duplicate = true;
		} else {
			if (!row_ready) {
				current = Array(); // Retain a prefetched row until Next delivery.
			}
			current_valid = false;
			next_pending = true;
			cached = row_ready;
			done = ended || closed;
		}
		pg_call = pg;
		sqlite_owner = owner;
	}
	if (duplicate) {
		if (pg_call.is_valid()) {
			pg_call->close_stream();
		} else if (sqlite_owner.is_valid()) {
			sqlite_owner->request_sqlite_rows(this, true);
		}
		callable_mp(this, &GDDatabaseRows::deliver).bind(false).call_deferred();
	} else if (cached) {
		callable_mp(this, &GDDatabaseRows::deliver).bind(true).call_deferred();
	} else if (done) {
		callable_mp(this, &GDDatabaseRows::deliver).bind(false).call_deferred();
	} else if (pg_call.is_valid()) {
		pg_call->resume_stream();
	} else if (sqlite_owner.is_valid()) {
		if (!sqlite_owner->request_sqlite_rows(this, false)) {
			sqlite_shutdown(Err::make("database Rows is not active", Err::INTERRUPTED));
		}
	} else {
		sqlite_shutdown(Err::make("database Rows has no connection", Err::INTERRUPTED));
	}
	return signal;
}

// Copy the current row into a column-name dictionary.
Ref<R> GDDatabaseRows::scan() const {
	MutexLock lock(mutex);
	if (!current_valid) {
		return R::err("database Rows.Scan called without a current row", Err::INVALID_DATA);
	}
	Dictionary row;
	for (int i = 0; i < names.size() && i < current.size(); i++) {
		row[names[i]] = current[i];
	}
	return R::ok(row);
}

// Return current-row values in column order.
Ref<R> GDDatabaseRows::values() const {
	MutexLock lock(mutex);
	return current_valid ? R::ok(current) : R::err("database Rows.Values called without a current row", Err::INVALID_DATA);
}

// Return result column names.
PackedStringArray GDDatabaseRows::columns() const {
	MutexLock lock(mutex);
	return names;
}

// Return the reason Next returned false, or null for EOF.
Ref<Err> GDDatabaseRows::err() const {
	MutexLock lock(mutex);
	return failed;
}

// Return the backend completion tag.
String GDDatabaseRows::command_tag() const {
	MutexLock lock(mutex);
	return tag;
}

// Close Rows without reading the remainder.
void GDDatabaseRows::close() {
	Ref<GDPostgresCallInternal> pg_call;
	Ref<GDDatabaseClient> sqlite_owner;
	bool notify = false;
	{
		MutexLock lock(mutex);
		if (closed) {
			return;
		}
		closed = true;
		ended = true;
		current = Array();
		current_valid = false;
		row_ready = false;
		notify = next_pending;
		pg_call = pg;
		sqlite_owner = owner;
		stopped.set();
	}
	if (notify) {
		deliver(false);
	}
	if (pg_call.is_valid()) {
		pg_call->close_stream();
	} else if (sqlite_owner.is_valid()) {
		sqlite_owner->request_sqlite_rows(this, true);
	}
	release_transaction();
}

// Close Rows with a cancellation error distinct from EOF.
void GDDatabaseRows::cancel() {
	{
		MutexLock lock(mutex);
		if (!closed) {
			failed = Err::make("database Rows was cancelled", Err::INTERRUPTED);
		}
	}
	close();
}

// Check whether Rows reached EOF, failure, or explicit closure.
bool GDDatabaseRows::is_closed() const {
	MutexLock lock(mutex);
	return closed;
}

// Expose Rows methods and signals to script.
void GDDatabaseRows::_bind_methods() {
	ClassDB::bind_method(D_METHOD("next"), &GDDatabaseRows::next);
	ClassDB::bind_method(D_METHOD("next_async"), &GDDatabaseRows::next);
	ClassDB::bind_method(D_METHOD("scan"), &GDDatabaseRows::scan);
	ClassDB::bind_method(D_METHOD("values"), &GDDatabaseRows::values);
	ClassDB::bind_method(D_METHOD("columns"), &GDDatabaseRows::columns);
	ClassDB::bind_method(D_METHOD("err"), &GDDatabaseRows::err);
	ClassDB::bind_method(D_METHOD("command_tag"), &GDDatabaseRows::command_tag);
	ClassDB::bind_method(D_METHOD("close"), &GDDatabaseRows::close);
	ClassDB::bind_method(D_METHOD("cancel"), &GDDatabaseRows::cancel);
	ClassDB::bind_method(D_METHOD("is_closed"), &GDDatabaseRows::is_closed);
	ADD_SIGNAL(MethodInfo("nexted", PropertyInfo(Variant::BOOL, "has_row")));
	ADD_RESULT("scan", "Dictionary");
	ADD_RESULT("values", "Array");
	ADD_AWAIT("next", "bool");
	ADD_AWAIT("next_async", "bool");
	ADD_AUTO_WAIT("next");
}
