/**************************************************************************/
/*  database.cpp                                                          */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement the shared remote SQL and embedded SQL CRUD client.

#include "cli/db/database.h"
#include "cli/db/options.h"
#include "cli/sys/clock.h"
#include "cli/data/utf8.h"
#include "cli/sys/file_job.h"
#include "cli/sys/limit.h"
#include "cli/sys/wait.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"
#include "cli/sys/task.h"

namespace {

HashSet<GDDatabaseClient *> database_clients; // Clients whose dedicated embedded SQL workers need shutdown.
uint64_t next_tx_scope = 0; // Unique identity for a transaction callback's coroutine tree.

constexpr int ROW_DEFAULT = 0; // Apply result row limits only when explicitly requested.
constexpr int BYTES_DEFAULT = 0; // Apply result byte limits only when explicitly requested.

// Estimate queued bind storage for types accepted by at least one database driver.
int64_t args_bytes(const Array &p_args) {
	int64_t total = 0;
	for (int i = 0; i < p_args.size(); i++) {
		const Variant &value = p_args[i];
		int64_t bytes = sizeof(Variant);
		switch (value.get_type()) {
			case Variant::NIL:
			case Variant::BOOL:
			case Variant::INT:
			case Variant::FLOAT:
				break;
			case Variant::STRING:
				bytes += utf8_bytes(String(value));
				break;
			case Variant::PACKED_BYTE_ARRAY:
				bytes += PackedByteArray(value).size();
				break;
			default:
				break;
		}
		total = bytes > INT64_MAX - total ? INT64_MAX : total + bytes;
	}
	return total;
}

// Validate shared SQL and bind values, returning bytes used for queue accounting.
Ref<Err> query_bytes(const String &p_sql, const Array &p_args, int &r_sql, int64_t &r_args) {
	const int64_t bytes = utf8_bytes(p_sql);
	if (bytes >= INT_MAX) {
		return Err::make("database SQL exceeds the native string capacity", Err::LIMITED);
	}
	r_sql = int(bytes);
	if (r_sql == 0) {
		return Err::make("database SQL is empty", Err::INVALID_DATA);
	}
	for (const Variant &arg : p_args) {
		switch (arg.get_type()) {
			case Variant::NIL:
			case Variant::BOOL:
			case Variant::INT:
			case Variant::FLOAT:
			case Variant::STRING:
			case Variant::STRING_NAME:
			case Variant::PACKED_BYTE_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY:
			case Variant::ARRAY:
			case Variant::DICTIONARY:
			case Variant::OBJECT:
				break;
			default:
				return Err::make("database parameter has an unsupported type", Err::INVALID_DATA);
		}
	}
	r_args = args_bytes(p_args);
	return Ref<Err>();
}

// Publish an immediate query failure without allocating a result object.
Signal query_fail(const String &p_message, Err::Kind p_kind) {
	return Async::ready_pair({ Variant(), Err::make(p_message, p_kind) });
}

// Find a server-reported serialization failure within a wrapped or joined error.
bool serialization_failure(const Ref<Err> &p_error) {
	if (p_error.is_null()) return false;
	Vector<Ref<Err>> pending;
	HashSet<Err *> seen;
	pending.push_back(p_error);
	while (!pending.is_empty()) {
		const Ref<Err> error = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		if (error.is_null() || seen.has(error.ptr())) continue;
		seen.insert(error.ptr());
		const Dictionary info = error->get_info();
		if (info.get("source", "") == "postgres" && info.get("code", "") == "40001") return true;
		const Ref<Err> cause = error->get_cause();
		if (cause.is_valid()) pending.push_back(cause);
		for (const Variant &item : error->get_causes()) {
			const Ref<Err> child = item;
			if (child.is_valid()) pending.push_back(child);
		}
	}
	return false;
}

} // namespace

// Send SQL through the transaction's dedicated connection.
Signal GDDatabaseTx::query(const String &p_sql, const Array &p_args) {
	return call.is_valid() ? call->query(p_sql, p_args) : query_fail("database transaction is closed", Err::INTERRUPTED);
}

// Read only the first row through the transaction client.
Signal GDDatabaseTx::query_row(const String &p_sql, const Array &p_args) {
	return call.is_valid() ? call->query_row(p_sql, p_args) : query_fail("database transaction is closed", Err::INTERRUPTED);
}

// Return sequential Rows within a transaction.
Signal GDDatabaseTx::query_rows(const String &p_sql, const Array &p_args) {
	return call.is_valid() ? call->query_rows(p_sql, p_args) : query_fail("database transaction is closed", Err::INTERRUPTED);
}

// Check whether the transaction remains usable.
bool GDDatabaseTx::is_active() const {
	return call.is_valid() && call->is_active();
}

// Expose methods permitted inside a transaction to script.
void GDDatabaseTx::_bind_methods() {
	ClassDB::bind_method(D_METHOD("query", "sql", "args"), &GDDatabaseTx::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_async", "sql", "args"), &GDDatabaseTx::query_async, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_row", "sql", "args"), &GDDatabaseTx::query_row, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_row_async", "sql", "args"), &GDDatabaseTx::query_row_async, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_rows", "sql", "args"), &GDDatabaseTx::query_rows, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_rows_async", "sql", "args"), &GDDatabaseTx::query_rows_async, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("is_active"), &GDDatabaseTx::is_active);
	ADD_AWAIT("query", "Pair:Dictionary");
	ADD_AWAIT("query_async", "Pair:Dictionary");
	ADD_AWAIT("query_row", "Pair:Dictionary");
	ADD_AWAIT("query_row_async", "Pair:Dictionary");
	ADD_AWAIT("query_rows", "Pair:GDDatabaseRows");
	ADD_AWAIT("query_rows_async", "Pair:GDDatabaseRows");
	ADD_AUTO_WAIT("query");
	ADD_AUTO_WAIT("query_row");
	ADD_AUTO_WAIT("query_rows");
}

// Check whether the transaction remains usable.
bool GDDatabaseTxCall::is_active() const {
	return !finished && !ending && owner.is_valid() &&
			(postgres.is_valid() ? owner->pool_txs.has(const_cast<GDDatabaseTxCall *>(this)) : owner->tx_active && owner->tx_generation == generation);
}

// Deliver closure or cancellation after the waiter connects.
void GDDatabaseTxCall::step() {
	if (pending_set) {
		const VariantPair result = pending;
		pending = VariantPair();
		pending_set = false;
		done(result, false);
	}
}

// Accept SQL from the transaction client.
Signal GDDatabaseTxCall::query(const String &p_sql, const Array &p_args) {
	if (!is_active()) return query_fail("database transaction is closed", Err::INTERRUPTED);
	int sql_bytes = 0;
	int64_t values_bytes = 0;
	const Ref<Err> invalid = query_bytes(p_sql, p_args, sql_bytes, values_bytes);
	if (invalid.is_valid()) return Async::ready_pair({ Variant(), invalid });
	return postgres.is_valid() ? postgres->query_limited(p_sql, p_args, owner->max_rows, owner->max_bytes) : owner->query_tx(generation, p_sql, p_args);
}

// Read only the first row from the transaction's physical connection.
Signal GDDatabaseTxCall::query_row(const String &p_sql, const Array &p_args) {
	if (!is_active()) return query_fail("database transaction is closed", Err::INTERRUPTED);
	int sql_bytes = 0;
	int64_t values_bytes = 0;
	const Ref<Err> invalid = query_bytes(p_sql, p_args, sql_bytes, values_bytes);
	if (invalid.is_valid()) return Async::ready_pair({ Variant(), invalid });
	return postgres.is_valid() ? postgres->query_row(p_sql, p_args, owner->max_bytes) : owner->query_tx(generation, p_sql, p_args, true);
}

// Open sequential Rows on the transaction's physical connection.
Signal GDDatabaseTxCall::query_rows(const String &p_sql, const Array &p_args) {
	if (!is_active()) return query_fail("database transaction is closed", Err::INTERRUPTED);
	int sql_bytes = 0;
	int64_t values_bytes = 0;
	const Ref<Err> invalid = query_bytes(p_sql, p_args, sql_bytes, values_bytes);
	if (invalid.is_valid()) return Async::ready_pair({ Variant(), invalid });
	if (postgres.is_valid()) {
		Ref<GDDatabaseRows> result = postgres->start_rows(p_sql, p_args);
		owner->track_tx_rows(result, generation);
		return Async::ready_pair({ result, Variant() });
	}
	return owner->query_tx_rows(generation, p_sql, p_args);
}

// Proceed from BEGIN to the callback or migration.
void GDDatabaseTxCall::on_begin(const Variant &p_value, const Variant &p_error) {
	if (finished) {
		return;
	}
	if (p_error.get_type() != Variant::NIL) {
		const Ref<Err> error = p_error;
		done({ p_value, error->note("begin transaction")->with_partial(p_value) }, false);
		return;
	}
	if (mode == MIGRATION) {
		next_statement();
		return;
	}
	Async::spawn_pair(callable_mp(this, &GDDatabaseTxCall::call_action)).connect(callable_mp(this, &GDDatabaseTxCall::on_action), Object::CONNECT_ONE_SHOT);
}

// Acquire a connection from the pool's FIFO queue and begin a transaction on it.
void GDDatabaseTxCall::on_acquire(const Variant &p_value, const Variant &p_error) {
	acquire_call.unref();
	if (finished || pending_set) {
		return;
	}
	if (p_error.get_type() != Variant::NIL) {
		const Ref<Err> error = p_error;
		done({ p_value, error->note("acquire transaction connection")->with_partial(p_value) }, false);
		return;
	}
	Ref<GDPostgresPoolCall> lease = p_value;
	if (lease.is_null() || lease->connection().is_null()) {
		done({ Variant(), Err::make("database transaction did not acquire a connection", Err::INTERRUPTED) }, false);
		return;
	}
	this->lease = lease;
	postgres = lease->connection();
	const uint64_t full_wait = postgres->wait_ms;
	if (lease->due > 0) {
		const uint64_t now = GDClock::msec();
		postgres->wait_ms = MAX(uint64_t(1), lease->due > now ? lease->due - now : uint64_t(1));
	}
	lease->due = 0;
	Signal begun = query("BEGIN", Array());
	postgres->wait_ms = full_wait;
	begun.connect(callable_mp(this, &GDDatabaseTxCall::on_begin), Object::CONNECT_ONE_SHOT);
}

// Pass the dedicated transaction client as the callback's ordinary argument.
VariantPair GDDatabaseTxCall::call_action() {
	const Variant tx_arg = tx;
	const Variant *args[] = { &tx_arg };
	Variant result;
	Variant result_error;
	Callable::CallError error;
	error.result_error = &result_error;
	const uint64_t previous_scope = Async::scope();
	Async::set_scope(scope);
	action.callp(args, 1, result, error);
	Async::set_scope(previous_scope);
	return error.error == Callable::CallError::CALL_OK && !error.runtime_failed ? VariantPair{ result, result_error } : VariantPair{ Variant(), Err::make("cannot call database transaction callback", Err::INVALID_DATA) };
}

// Choose COMMIT or ROLLBACK from the callback result.
void GDDatabaseTxCall::on_action(const Variant &p_value, const Variant &p_error) {
	if (finished) {
		return;
	}
	if (p_error.get_type() == Variant::NIL && p_value.get_type() == Variant::OBJECT) {
		const Ref<Err> failure = p_value;
		if (failure.is_valid()) {
			outcome = { Variant(), failure };
			end(false);
			return;
		}
	}
	outcome = { p_value, p_error };
	end(p_error.get_type() == Variant::NIL);
}

// Send the next migration statement.
void GDDatabaseTxCall::next_statement() {
	if (finished) {
		return;
	}
	if (at >= statements.size()) {
		outcome = { at, Variant() };
		end(true);
		return;
	}
	query(statements[at], Array()).connect(callable_mp(this, &GDDatabaseTxCall::on_statement), Object::CONNECT_ONE_SHOT);
}

// Proceed to the next statement or ROLLBACK from the statement result.
void GDDatabaseTxCall::on_statement(const Variant &p_value, const Variant &p_error) {
	if (finished) {
		return;
	}
	if (p_error.get_type() != Variant::NIL) {
		const Ref<Err> error = p_error;
		outcome = { at, error->note(vformat("migration statement %d", at + 1))->with_partial(at) };
		end(false);
		return;
	}
	at++;
	next_statement();
}

// Send COMMIT or ROLLBACK.
void GDDatabaseTxCall::end(bool p_commit) {
	ending = true;
	committing = p_commit;
	owner->close_tx_rows(generation);
	Signal sent = postgres.is_valid() ? postgres->query(p_commit ? "COMMIT" : "ROLLBACK", Array()) : owner->query_tx(generation, p_commit ? "COMMIT" : "ROLLBACK", Array());
	sent.connect(callable_mp(this, &GDDatabaseTxCall::on_end), Object::CONNECT_ONE_SHOT);
}

// Convert transaction termination into the final outcome.
void GDDatabaseTxCall::on_end(const Variant &p_value, const Variant &p_error) {
	if (finished) {
		return;
	}
	if (p_error.get_type() != Variant::NIL) {
		const Ref<Err> error = p_error;
		Ref<Err> ending = error->note(committing ? "commit transaction" : "rollback transaction")->with_partial(outcome.value);
		const Ref<Err> original = outcome.error;
		if (!committing && original.is_valid()) {
			done({ outcome.value, Err::join(original, ending) }, true);
		} else {
			done({ outcome.value, ending }, true);
		}
		return;
	}
	if (committing && p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary result = p_value;
		if (String(result.get("tag", "")).to_upper() == "ROLLBACK") {
			done({ Variant(), Err::make("database transaction was rolled back during commit", Err::INVALID_DATA) }, close_after_end);
			return;
		}
	}
	done(outcome, close_after_end);
}

// Deliver the final result exactly once.
void GDDatabaseTxCall::done(const VariantPair &p_result, bool p_close) {
	if (finished) {
		return;
	}
	finished = true;
	Ref<GDDatabaseTxCall> keep(this);
	Ref<GDDatabaseClient> db = owner;
	if (db.is_valid() && db->pool_txs.has(this)) {
		db->close_tx_rows(generation);
		db->pool_txs.erase(this);
		if (p_close && postgres.is_valid()) postgres->close();
	} else if (db.is_valid() && db->tx_generation == generation && db->tx_call.ptr() == this) {
		db->finish_tx(generation);
		if (p_close) {
			db->close();
		}
	}
	postgres.unref();
	acquire_call.unref();
	if (lease.is_valid()) lease->release_lease();
	lease.unref();
	if (db.is_valid() && db->closing) db->close();
	Async::finish(this, SNAME("finished"), p_result.value, p_result.error);
	if (tx.is_valid()) {
		tx->call.unref();
	}
	tx.unref();
	owner.unref();
	action = Callable();
	statements.clear();
	outcome = VariantPair();
	pending = VariantPair();
	pending_set = false;
	self_hold.unref();
}

// Report rollback caused by owner closure as completion.
void GDDatabaseTxCall::owner_closed() {
	if (!finished && !pending_set) {
		pending = { outcome.value, Err::join(Ref<Err>(outcome.error), Err::make("database transaction was closed", Err::INTERRUPTED)) };
		pending_set = true;
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDDatabaseTxCall::step));
	}
}

// Defer closure until the result is known after sending COMMIT.
bool GDDatabaseTxCall::defer_close() {
	if (!finished && committing) {
		close_after_end = true;
		return true;
	}
	return false;
}

// Close the connection and roll back when the waiter cancels.
void GDDatabaseTxCall::cancel() {
	if (!finished && owner.is_valid()) {
		if (postgres.is_valid()) {
			postgres->close();
			owner_closed();
		} else if (owner->pool_txs.has(this)) {
			if (acquire_call.is_valid()) acquire_call->cancel();
			owner_closed();
		} else {
			owner->close();
		}
	}
}

// Expose cancellation and completion signals to script.
void GDDatabaseTxCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDDatabaseTxCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}

// Start a fresh transaction for this attempt.
void GDDatabaseRetryCall::run() {
	if (finished || canceled || owner.is_null()) return;
	tries++;
	Signal sent = owner->transaction(action);
	active = Ref<GDDatabaseTxCall>(Object::cast_to<GDDatabaseTxCall>(sent.get_object()));
	sent.connect(callable_mp(this, &GDDatabaseRetryCall::on_result), Object::CONNECT_ONE_SHOT);
}

// Retry only a server serialization conflict after the prior transaction has ended.
void GDDatabaseRetryCall::on_result(const Variant &p_value, const Variant &p_error) {
	active.unref();
	if (finished) return;
	if (canceled) {
		done(Variant(), Err::interrupted("database serialization retry was canceled"));
		return;
	}
	const Ref<Err> error = p_error;
	if (tries < 10 && serialization_failure(error)) {
		const double delay = double(Math::rand() % 20) / 1000.0;
		const Ref<GDWait> timer = Async::start_sleep(delay);
		Signal(timer.ptr(), "finished").connect(callable_mp(this, &GDDatabaseRetryCall::on_delay), Object::CONNECT_ONE_SHOT);
		return;
	}
	done(p_value, p_error);
}

// Continue after a short asynchronous delay.
void GDDatabaseRetryCall::on_delay(const Variant &p_value) {
	if (!finished && !canceled) run();
}

// Deliver the last transaction outcome and release callback state.
void GDDatabaseRetryCall::done(const Variant &p_value, const Variant &p_error) {
	if (finished) return;
	finished = true;
	Ref<GDDatabaseRetryCall> keep(this);
	Async::finish(this, SNAME("finished"), p_value, p_error);
	active.unref();
	owner.unref();
	action = Callable();
	self_hold.unref();
}

// Cancel the current transaction or a scheduled retry.
void GDDatabaseRetryCall::cancel() {
	if (finished) return;
	canceled = true;
	if (active.is_valid()) {
		active->cancel();
	} else {
		done(Variant(), Err::interrupted("database serialization retry was canceled"));
	}
}

// Expose completion and cancellation to asynchronous callers.
void GDDatabaseRetryCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDDatabaseRetryCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}

// Publish embedded SQL worker results safely.
void GDDatabaseCall::complete(const VariantPair &p_result) {
	if (!completed.set_if_clear()) {
		return;
	}
	result = p_result;
	ready.set();
	Async::post(Ref<RefCounted>(this), callable_mp(this, &GDDatabaseCall::step));
}

// Keep sequential results owned by the waiting caller or release them after cancellation.
void GDDatabaseCall::forward(const Variant &p_value, const Variant &p_error) {
	Ref<GDDatabaseCall> keep(this);
	const Ref<Err> failure = p_error;
	if (rows_open && failure.is_null()) {
		Ref<GDDatabaseRows> opened = p_value;
		if (completed.is_set()) {
			if (opened.is_valid()) opened->close();
			forward_hold.unref();
			return;
		}
		rows = opened;
	}
	complete({ p_value, p_error });
	forward_hold.unref();
}

// Deliver context cancellation promptly and interrupt active embedded SQL work.
void GDDatabaseCall::cancel() {
	if (completed.is_set()) {
		if (rows_open && rows.is_valid()) {
			rows->cancel(); // Do not retain adopted Rows and their connection before delivery.
		}
		return;
	}
	canceled.set();
	// Commit cancellation first so a racing worker return cannot change the error category.
	complete({ Variant(), Err::make("database call was cancelled", Err::INTERRUPTED) });
	if (owner.is_valid()) {
		owner->cancel_call(this);
	}
}

// Emit completed results on the main thread.
void GDDatabaseCall::step() {
	if (!ready.is_set() || self_hold.is_null()) {
		return;
	}
	Ref<GDDatabaseCall> keep(this);
	VariantPair out = result;
	if (sqlite.is_valid() && (owner.is_null() || owner->sqlite.ptr() != sqlite.ptr())) {
		// Treat even a prepared successful worker result as closed if closure precedes delivery.
		out = { Variant(), Err::join(Ref<Err>(out.error), Err::make("database is closed", Err::INTERRUPTED)) };
	}
	Async::finish(this, SNAME("finished"), out.value, out.error);
	if (sqlite.is_valid()) {
		owner->sqlite_finished(this, sqlite, queued_bytes);
	}
	result = VariantPair();
	rows.unref();
	sqlite.unref();
	args.clear();
	sql = String();
	owner.unref();
	self_hold.unref();
}

// Dispose undelivered operations directly at process shutdown without a main loop.
void GDDatabaseCall::finish_shutdown() {
	Ref<GDDatabaseCall> keep(this);
	result = VariantPair();
	rows.unref();
	sqlite.unref();
	args.clear();
	sql = String();
	owner.unref();
	self_hold.unref();
	forward_hold.unref();
}

// Register the embedded SQL operation completion signal.
void GDDatabaseCall::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &GDDatabaseCall::cancel);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value"), PropertyInfo(Variant::OBJECT, "error", PROPERTY_HINT_RESOURCE_TYPE, "Err")));
}

// Enter the embedded SQL worker through the thread's C callback.
void GDDatabaseClient::sqlite_entry(void *p_self) {
	static_cast<GDDatabaseClient *>(p_self)->sqlite_loop();
}

// Accumulate connection wait time under sqlite_mutex.
void GDDatabaseClient::record_sqlite_wait(GDDatabaseCall *p_call) {
	if (p_call->counted_wait && !p_call->wait_recorded) {
		sqlite_wait_ms += GDClock::msec() - p_call->queued_at;
		p_call->wait_recorded = true;
	}
}

// Execute embedded SQL operations in arrival order.
void GDDatabaseClient::sqlite_loop() {
	for (;;) {
		sqlite_ready.wait();
		Ref<GDDatabaseCall> call;
		Ref<GDDatabaseRows> rows_job;
		Ref<GDDatabaseRows> rows_shutdown;
		Ref<GDSQLiteDB> running;
		String sql;
		Array args;
		bool close_rows = false;
		bool stop_worker = false;
		{
			MutexLock lock(sqlite_mutex);
			if (sqlite_stop.is_set()) {
				rows_shutdown = sqlite_rows;
				sqlite_rows.unref();
				sqlite_rows_job.unref();
				sqlite_rows_close = false;
				while (!sqlite_jobs.is_empty()) {
					Ref<GDDatabaseCall> queued = sqlite_jobs.front()->get();
					sqlite_jobs.pop_front();
					queued->sqlite.unref();
					queued->args.clear();
					queued->sql = String();
					queued->queued_bytes = 0;
					queued->complete({ Variant(), Err::make("database is closed", Err::INTERRUPTED) });
				}
				sqlite_queue_bytes = 0;
				stop_worker = true;
			} else if (sqlite_rows.is_valid()) {
				if (sqlite_rows_job.is_valid()) {
					rows_job = sqlite_rows_job;
					sqlite_rows_job.unref();
					close_rows = sqlite_rows_close;
					sqlite_rows_close = false;
					running = rows_job->sqlite;
				}
			} else if (!sqlite_jobs.is_empty()) {
				call = sqlite_jobs.front()->get();
				sqlite_jobs.pop_front();
				record_sqlite_wait(call.ptr());
				if (!call->completed.is_set()) {
					sqlite_active = call;
					running = call->sqlite;
					sql = call->sql;
					args = call->args;
					// Set the flag under the same lock as close so result-wait notifications cannot be lost.
					sqlite_waiting_result.set();
				}
			}
		}
		if (stop_worker) {
			if (rows_shutdown.is_valid() && rows_shutdown->sqlite.is_valid()) {
				rows_shutdown->stopped.set();
				rows_shutdown->sqlite->close_rows(rows_shutdown.ptr());
				rows_shutdown->sqlite_shutdown(Err::make("database is closed", Err::INTERRUPTED));
			}
			return;
		}
		if (rows_job.is_valid()) {
			bool finished = true;
			if (running.is_valid()) {
				if (close_rows) {
					running->close_rows(rows_job.ptr());
					rows_job->sqlite_shutdown(Ref<Err>());
				} else {
					finished = running->step_rows(rows_job.ptr());
				}
			} else {
				rows_job->sqlite_shutdown(Err::make("database is closed", Err::INTERRUPTED));
			}
			if (finished || close_rows) {
				bool wake_jobs = false;
				{
					MutexLock lock(sqlite_mutex);
					if (sqlite_rows.ptr() == rows_job.ptr()) {
						sqlite_rows.unref();
					}
					wake_jobs = !sqlite_jobs.is_empty();
				}
				if (wake_jobs) {
					sqlite_ready.post();
				}
			}
			continue;
		}
		if (call.is_valid() && !call->completed.is_set()) {
			VariantPair out;
			if (sqlite_stop.is_set()) {
				out = { Variant(), Err::make("database is closed", Err::INTERRUPTED) };
			} else if (call->canceled.is_set()) {
				out = { Variant(), Err::make("database call was cancelled", Err::INTERRUPTED) };
			} else if (call->rows_open) {
				const Ref<Err> failure = running->open_rows(call->rows.ptr(), sql, args, &call->canceled);
				out = { Variant(), failure.is_valid() ? Variant(failure) : Variant() };
				bool adopted = false;
				if (out.error.get_type() == Variant::NIL && !call->canceled.is_set()) {
					MutexLock lock(sqlite_mutex);
					if (!sqlite_stop.is_set() && sqlite_rows.is_null()) {
						sqlite_rows = call->rows;
						adopted = true;
						out = { call->rows, Variant() };
					}
				}
				if (!adopted) {
					running->close_rows(call->rows.ptr());
					if (out.error.get_type() == Variant::NIL) {
						out = { Variant(), Err::make("database is closed", Err::INTERRUPTED) };
					}
				}
			} else {
				out = call->one ? running->portable_row(sql, args, &call->canceled) : running->portable_query(sql, args, &call->canceled);
			}
			call->complete(out);
			sqlite_consumed.wait();
			MutexLock lock(sqlite_mutex);
			if (sqlite_active.ptr() == call.ptr()) {
				sqlite_active.unref();
			}
		}
	}
}

// Dispatch Rows Next or Close to the worker currently holding the embedded SQL connection.
bool GDDatabaseClient::request_sqlite_rows(GDDatabaseRows *p_rows, bool p_close) {
	bool queued = false;
	{
		MutexLock lock(sqlite_mutex);
		if (!sqlite_stop.is_set() && sqlite_rows.ptr() == p_rows) {
			if (sqlite_rows_job.is_null()) {
				sqlite_rows_job = Ref<GDDatabaseRows>(p_rows);
			}
			sqlite_rows_close = sqlite_rows_close || p_close;
			queued = true;
		}
	}
	if (queued) {
		sqlite_ready.post();
	}
	return queued;
}

// Track transaction Rows for cleanup at transaction end.
void GDDatabaseClient::track_tx_rows(const Ref<GDDatabaseRows> &p_rows, uint64_t p_generation) {
	if (p_rows.is_null()) {
		return;
	}
	if (p_generation == 0) p_generation = tx_active ? tx_generation : 0;
	if (p_generation == 0) return;
	{
		MutexLock lock(p_rows->mutex);
		p_rows->owner = Ref<GDDatabaseClient>(this);
		p_rows->tx_generation = p_generation;
	}
	if (p_generation == tx_generation && tx_active) {
		tx_rows.insert(p_rows.ptr());
	} else {
		for (GDDatabaseTxCall *call : pool_txs) {
			if (call->generation == p_generation) {
				call->rows.insert(p_rows.ptr());
				break;
			}
		}
	}
}

// Remove closed Rows only from their current transaction.
void GDDatabaseClient::untrack_tx_rows(GDDatabaseRows *p_rows, uint64_t p_generation) {
	if (p_rows && p_generation == tx_generation) {
		tx_rows.erase(p_rows);
	}
	for (GDDatabaseTxCall *call : pool_txs) {
		if (call->generation == p_generation) call->rows.erase(p_rows);
	}
}

// Close Rows from the same generation before COMMIT or ROLLBACK.
void GDDatabaseClient::close_tx_rows(uint64_t p_generation) {
	HashSet<GDDatabaseRows *> *tracked = nullptr;
	if (p_generation == tx_generation && tx_active) tracked = &tx_rows;
	for (GDDatabaseTxCall *call : pool_txs) {
		if (call->generation == p_generation) tracked = &call->rows;
	}
	if (!tracked || tracked->is_empty()) return;
	LocalVector<Ref<GDDatabaseRows>> rows;
	for (GDDatabaseRows *item : *tracked) {
		rows.push_back(Ref<GDDatabaseRows>(item));
	}
	for (const Ref<GDDatabaseRows> &item : rows) {
		item->close();
	}
	tracked->clear();
}

// Receive remote SQL open results and restore reusability on failure.
void GDDatabaseClient::postgres_opened(const Variant &p_value, const Variant &p_error, const Ref<GDDatabaseCall> &p_call,
		const Ref<GDPostgresClient> &p_expected, uint64_t p_generation) {
	if (Pool::is_stopping()) {
		opening = false;
		if (p_expected.is_valid()) {
			p_expected->close();
		}
		postgres.unref();
		p_call->finish_shutdown();
		return;
	}
	VariantPair out{ p_value, p_error };
	if (!opening || p_generation != open_generation || postgres.ptr() != p_expected.ptr()) {
		out = { Variant(), Err::make("database open was cancelled", Err::INTERRUPTED) };
	} else {
		opening = false;
		if (p_error.get_type() != Variant::NIL) {
			postgres.unref();
		}
	}
	p_call->complete(out);
}

// Forward remote SQL pool configuration results to the shared open operation.
void GDDatabaseClient::postgres_pool_opened(const Variant &p_value, const Variant &p_error, const Ref<GDDatabaseCall> &p_call,
		const Ref<GDPostgresPool> &p_expected, uint64_t p_generation) {
	if (Pool::is_stopping()) {
		opening = false;
		if (p_expected.is_valid()) {
			p_expected->close();
		}
		postgres_pool.unref();
		p_call->finish_shutdown();
		return;
	}
	VariantPair out{ p_value, p_error };
	if (!opening || p_generation != open_generation || postgres_pool.ptr() != p_expected.ptr()) {
		out = { Variant(), Err::make("database open was cancelled", Err::INTERRUPTED) };
	} else {
		opening = false;
		if (out.error.get_type() != Variant::NIL) {
			postgres_pool->close();
			postgres_pool.unref();
		}
	}
	p_call->complete(out);
}

// Adopt a worker-opened embedded SQL connection and start its query thread.
void GDDatabaseClient::sqlite_opened(const Variant &p_value, const Variant &p_error, const Ref<GDDatabaseCall> &p_call, uint64_t p_generation) {
	VariantPair out{ p_value, p_error };
	Ref<GDSQLiteDB> made;
	if (p_error.get_type() == Variant::NIL) {
		made = p_value;
	}
	if (Pool::is_stopping()) {
		opening = false;
		if (made.is_valid()) {
			made->close(); // Close only the created connection without starting another dedicated thread.
		}
		p_call->finish_shutdown();
		return;
	}
	if (!opening || p_generation != open_generation) {
		out = { Variant(), Err::make("database open was cancelled", Err::INTERRUPTED) };
	} else {
		opening = false;
		if (made.is_valid()) {
			sqlite = made;
			sqlite_stop.clear();
			sqlite_thread.start(&GDDatabaseClient::sqlite_entry, this);
			if (!sqlite_thread.is_started()) {
				sqlite->close();
				sqlite.unref();
				out = { Variant(), Err::make("cannot start sqlite worker", Err::UNSUPPORTED) };
			}
		}
	}
	if (made.is_valid() && sqlite.ptr() != made.ptr()) {
		GDPairCall::start([made]() -> VariantPair {
			made->close();
			return { Variant(), Variant() };
		});
	}
	p_call->complete(out);
}

// Close the embedded SQL worker and connection safely.
void GDDatabaseClient::close_sqlite() {
	if (sqlite.is_null()) {
		return;
	}
	const Ref<GDSQLiteDB> closing = sqlite;
	sqlite.unref(); // Reject new queries and expose shutdown immediately.
	const bool finish_now = destroying || Pool::is_stopping();
	LocalVector<Ref<GDDatabaseCall>> pending;
	bool release_worker = false;
	{
		// Order shutdown against worker dequeueing and always wake a pending result wait.
		MutexLock lock(sqlite_mutex);
		sqlite_stop.set();
		if (finish_now && sqlite_active.is_valid()) {
			pending.push_back(sqlite_active);
		}
		for (const Ref<GDDatabaseCall> &queued : sqlite_jobs) {
			record_sqlite_wait(queued.ptr());
			if (finish_now) {
				pending.push_back(queued);
			}
		}
		release_worker = sqlite_waiting_result.clear_if_set();
	}
	closing->interrupt();
	sqlite_ready.post();
	if (release_worker) {
		sqlite_consumed.post();
	}
	if (destroying) {
		if (sqlite_thread.is_started()) {
			sqlite_thread.wait_to_finish();
		}
		closing->close();
		for (const Ref<GDDatabaseCall> &call : pending) {
			call->finish_shutdown();
		}
		return;
	}
	sqlite_closing = true;
	const Ref<GDDatabaseClient> keep(this);
	GDPairCall::start([keep, closing]() -> VariantPair {
		if (keep->sqlite_thread.is_started()) {
			keep->sqlite_thread.wait_to_finish();
		}
		closing->close();
		return { Variant(), Variant() };
	}).connect(callable_mp(this, &GDDatabaseClient::sqlite_closed), Object::CONNECT_ONE_SHOT);
}

// Restore the reopenable state after the embedded SQL worker exits.
void GDDatabaseClient::sqlite_closed(const Variant &p_value, const Variant &p_error) {
	(void)p_value;
	(void)p_error;
	sqlite_closing = false;
}

// Deliver embedded SQL results on the main thread and let the worker proceed.
void GDDatabaseClient::sqlite_finished(GDDatabaseCall *p_call, const Ref<GDSQLiteDB> &p_sqlite, int64_t p_queued_bytes) {
	if (sqlite.ptr() != p_sqlite.ptr()) {
		return; // Do not advance a new worker using results from before closure.
	}
	bool release_worker = false;
	{
		MutexLock lock(sqlite_mutex);
		sqlite_queue_bytes = MAX(int64_t(0), sqlite_queue_bytes - p_queued_bytes);
		release_worker = sqlite_active.ptr() == p_call && sqlite_waiting_result.clear_if_set();
	}
	if (release_worker) {
		sqlite_consumed.post();
	}
}

// Stop only canceled-context calls; discard connection setup too while opening.
void GDDatabaseClient::cancel_call(GDDatabaseCall *p_call) {
	if (!p_call || p_call->sqlite.is_null()) {
		close();
		return;
	}
	if (p_call->deferred) {
		MutexLock lock(sqlite_mutex);
		record_sqlite_wait(p_call);
		return;
	}
	Ref<GDSQLiteDB> running;
	{
		MutexLock lock(sqlite_mutex);
		record_sqlite_wait(p_call);
		if (sqlite_active.ptr() == p_call) {
			running = p_call->sqlite;
		}
	}
	if (running.is_valid()) {
		running->interrupt();
	}
}

// Register the client for shutdown cleanup.
GDDatabaseClient::GDDatabaseClient() {
	database_clients.insert(this);
}

// Close the worker and connection on destruction.
GDDatabaseClient::~GDDatabaseClient() {
	database_clients.erase(this);
	destroying = true;
	close();
}

// Synchronously close clients with dedicated embedded SQL workers at process shutdown.
void GDDatabaseClient::shutdown_all() {
	LocalVector<Ref<GDDatabaseClient>> clients;
	for (GDDatabaseClient *client : database_clients) {
		clients.push_back(Ref<GDDatabaseClient>(client));
	}
	for (const Ref<GDDatabaseClient> &client : clients) {
		client->destroying = true;
		client->close();
	}
}

// Select the driver and connection options, then open the required connections.
Signal GDDatabaseClient::open(const Dictionary &p_opts) {
	if (opening || sqlite_closing || postgres.is_valid() || postgres_pool.is_valid() || sqlite.is_valid()) {
		return query_fail("database is already open", Err::ALREADY_EXISTS);
	}
	int64_t rows_raw = 0, bytes_raw = 0;
	if (!DbOption::integer(p_opts, "max_rows", ROW_DEFAULT, rows_raw) || !DbOption::integer(p_opts, "max_bytes", BYTES_DEFAULT, bytes_raw) ||
			rows_raw < 0 || rows_raw > INT_MAX || bytes_raw < 0 || bytes_raw > INT_MAX) {
		return query_fail("database result limits must be between 0 and 2147483647", Err::INVALID_DATA);
	}
	max_rows = (int)rows_raw;
	max_bytes = (int)bytes_raw;
	String driver;
	if (!DbOption::text(p_opts, "driver", "postgres", driver)) return query_fail("database driver must be postgres or sqlite", Err::INVALID_DATA);
	if (driver == "sqlite") {
		String path;
		if (!DbOption::text(p_opts, "path", "", path)) return query_fail("sqlite path must be text", Err::INVALID_DATA);
		Dictionary opts = p_opts.duplicate();
		opts.erase("driver");
		opts.erase("path");
		Ref<GDDatabaseCall> call;
		call.instantiate();
		call->self_hold = call;
		call->owner = Ref<GDDatabaseClient>(this);
		const uint64_t generation = ++open_generation;
		opening = true;
		GDPairCall::start([path, opts]() { return GDSQLiteDB::open(path, opts); }, false).connect(
				callable_mp(this, &GDDatabaseClient::sqlite_opened).bind(call, generation), Object::CONNECT_ONE_SHOT);
		return Signal(call.ptr(), "finished");
	}
	if (driver != "postgres") {
		return query_fail("database driver must be postgres or sqlite", Err::INVALID_DATA);
	}
	String host;
	int64_t port = 0;
	if (!DbOption::text(p_opts, "host", "127.0.0.1", host) || !DbOption::integer(p_opts, "port", 5432, port) ||
			host.is_empty() || port < Limit::PORT_MIN || port > Limit::PORT_MAX) {
		return query_fail("postgres address is invalid", Err::INVALID_DATA);
	}
	Dictionary opts = p_opts.duplicate();
	opts.erase("driver");
	opts.erase("host");
	opts.erase("port");
	int64_t pool_size = 0;
	if (!DbOption::integer(opts, "pool", 0, pool_size) || pool_size < 0 || pool_size > INT_MAX) {
		return query_fail("postgres pool size must be between 0 and 2147483647", Err::INVALID_DATA);
	}
	opts.erase("pool");
	Ref<GDDatabaseCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = Ref<GDDatabaseClient>(this);
	const uint64_t generation = ++open_generation;
	opening = true;
	if (pool_size != 1) {
		postgres_pool.instantiate();
		const Ref<GDPostgresPool> expected = postgres_pool;
		postgres_pool->open(host, port, opts, pool_size).connect(callable_mp(this, &GDDatabaseClient::postgres_pool_opened).bind(call, expected, generation), Object::CONNECT_ONE_SHOT);
		return Signal(call.ptr(), "finished");
	}
	postgres.instantiate();
	const Ref<GDPostgresClient> expected = postgres;
	postgres->open(host, port, opts).connect(callable_mp(this, &GDDatabaseClient::postgres_opened).bind(call, expected, generation), Object::CONNECT_ONE_SHOT);
	return Signal(call.ptr(), "finished");
}

// Execute shared SQL without checking transaction occupancy.
Signal GDDatabaseClient::query_inner(const String &p_sql, const Array &p_args, bool p_one, bool p_tx) {
	if (closing) return query_fail("database is closing", Err::INTERRUPTED);
	int sql_bytes = 0;
	int64_t values_bytes = 0;
	const Ref<Err> invalid = query_bytes(p_sql, p_args, sql_bytes, values_bytes);
	if (invalid.is_valid()) {
		return Async::ready_pair({ Variant(), invalid });
	}
	// Keep leased transaction SQL on its connection while unrelated pool queries use other leases.
	if (p_tx && tx_postgres.is_valid()) {
		return p_one ? tx_postgres->query_row(p_sql, p_args, max_bytes) : tx_postgres->query_limited(p_sql, p_args, max_rows, max_bytes);
	}
	if (postgres.is_valid()) {
		return p_one ? postgres->query_row(p_sql, p_args, max_bytes) : postgres->query_limited(p_sql, p_args, max_rows, max_bytes);
	}
	if (postgres_pool.is_valid()) {
		return p_one ? postgres_pool->query_row(p_sql, p_args, max_bytes) : postgres_pool->query_limited(p_sql, p_args, max_rows, max_bytes);
	}
	if (sqlite.is_valid()) {
		const int64_t queued_bytes = sql_bytes + values_bytes;
		Ref<GDDatabaseCall> call;
		call.instantiate();
		call->self_hold = call;
		call->owner = Ref<GDDatabaseClient>(this);
		call->sqlite = sqlite;
		call->sql = p_sql;
		call->args = p_args.duplicate(true);
		call->queued_bytes = queued_bytes;
		call->one = p_one;
		call->queued_at = GDClock::msec();
		{
			MutexLock lock(sqlite_mutex);
			call->counted_wait = sqlite_active.is_valid() || !sqlite_jobs.is_empty();
			if (call->counted_wait) {
				sqlite_wait_count++;
			}
			sqlite_jobs.push_back(call);
			sqlite_queue_bytes += queued_bytes;
		}
		sqlite_ready.post();
		return Signal(call.ptr(), "finished");
	}
	return query_fail("database is not open", Err::NOT_FOUND);
}

// Open sequential Rows without checking transaction occupancy.
Signal GDDatabaseClient::query_rows_inner(const String &p_sql, const Array &p_args, bool p_tx) {
	if (closing) return query_fail("database is closing", Err::INTERRUPTED);
	int sql_bytes = 0;
	int64_t values_bytes = 0;
	const Ref<Err> invalid = query_bytes(p_sql, p_args, sql_bytes, values_bytes);
	if (invalid.is_valid()) {
		return Async::ready_pair({ Variant(), invalid });
	}
	// Rows from a transaction retain its lease; ordinary Rows acquire from the pool.
	if (p_tx && tx_postgres.is_valid()) {
		Ref<GDDatabaseRows> rows = tx_postgres->start_rows(p_sql, p_args);
		track_tx_rows(rows);
		return Async::ready_pair({ rows, Variant() });
	}
	if (postgres.is_valid()) {
		Ref<GDDatabaseRows> rows = postgres->start_rows(p_sql, p_args);
		track_tx_rows(rows);
		return Async::ready_pair({ rows, Variant() });
	}
	if (postgres_pool.is_valid()) {
		return postgres_pool->query_rows(p_sql, p_args);
	}
	if (sqlite.is_valid()) {
		const int64_t queued_bytes = sql_bytes + values_bytes;
		Ref<GDDatabaseRows> rows;
		rows.instantiate();
		rows->owner = Ref<GDDatabaseClient>(this);
		rows->sqlite = sqlite;
		track_tx_rows(rows);
		Ref<GDDatabaseCall> call;
		call.instantiate();
		call->self_hold = call;
		call->owner = Ref<GDDatabaseClient>(this);
		call->sqlite = sqlite;
		call->sql = p_sql;
		call->args = p_args.duplicate(true);
		call->queued_bytes = queued_bytes;
		call->rows_open = true;
		call->rows = rows;
		call->queued_at = GDClock::msec();
		{
			MutexLock lock(sqlite_mutex);
			call->counted_wait = sqlite_active.is_valid() || sqlite_rows.is_valid() || !sqlite_jobs.is_empty();
			if (call->counted_wait) {
				sqlite_wait_count++;
			}
			sqlite_jobs.push_back(call);
			sqlite_queue_bytes += queued_bytes;
		}
		sqlite_ready.post();
		return Signal(call.ptr(), "finished");
	}
	return query_fail("database is not open", Err::NOT_FOUND);
}

// Execute shared SQL with bound parameters.
Signal GDDatabaseClient::query(const String &p_sql, const Array &p_args) {
	if (tx_active && postgres_pool.is_null()) {
		if (sqlite.is_valid()) return Async::scope() == tx_call->scope ? query_fail("use the transaction client inside its callback", Err::ALREADY_EXISTS) : defer_sqlite(p_sql, p_args, false, false);
		return query_fail("database connection is in a transaction", Err::ALREADY_EXISTS);
	}
	return query_inner(p_sql, p_args);
}

// Read only the first shared-query row without retaining the remainder.
Signal GDDatabaseClient::query_row(const String &p_sql, const Array &p_args) {
	if (tx_active && postgres_pool.is_null()) {
		if (sqlite.is_valid()) return Async::scope() == tx_call->scope ? query_fail("use the transaction client inside its callback", Err::ALREADY_EXISTS) : defer_sqlite(p_sql, p_args, true, false);
		return query_fail("database connection is in a transaction", Err::ALREADY_EXISTS);
	}
	return query_inner(p_sql, p_args, true);
}

// Read shared-query results one row at a time through Rows.
Signal GDDatabaseClient::query_rows(const String &p_sql, const Array &p_args) {
	if (tx_active && postgres_pool.is_null()) {
		if (sqlite.is_valid()) return Async::scope() == tx_call->scope ? query_fail("use the transaction client inside its callback", Err::ALREADY_EXISTS) : defer_sqlite(p_sql, p_args, false, true);
		return query_fail("database connection is in a transaction", Err::ALREADY_EXISTS);
	}
	return query_rows_inner(p_sql, p_args);
}

// Hold shared queries outside the active transaction until it completes.
Signal GDDatabaseClient::defer_sqlite(const String &p_sql, const Array &p_args, bool p_one, bool p_rows) {
	int sql_bytes = 0;
	int64_t values_bytes = 0;
	const Ref<Err> invalid = query_bytes(p_sql, p_args, sql_bytes, values_bytes);
	if (invalid.is_valid()) return Async::ready_pair({ Variant(), invalid });
	Ref<GDDatabaseCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = Ref<GDDatabaseClient>(this);
	call->sqlite = sqlite;
	call->sql = p_sql;
	call->args = p_args.duplicate(true);
	call->one = p_one;
	call->rows_open = p_rows;
	call->deferred = true;
	call->queued_at = GDClock::msec();
	call->counted_wait = true;
	{
		MutexLock lock(sqlite_mutex);
		sqlite_wait_count++;
	}
	sqlite_deferred.push_back(call);
	return Signal(call.ptr(), "finished");
}

// Send SQL through the transaction's dedicated connection.
Signal GDDatabaseClient::query_tx(uint64_t p_generation, const String &p_sql, const Array &p_args, bool p_one) {
	if (!tx_active || tx_generation != p_generation) {
		return query_fail("database transaction is closed", Err::INTERRUPTED);
	}
	return query_inner(p_sql, p_args, p_one, true);
}

// Open sequential Rows through the transaction's dedicated connection.
Signal GDDatabaseClient::query_tx_rows(uint64_t p_generation, const String &p_sql, const Array &p_args) {
	if (!tx_active || tx_generation != p_generation) {
		return query_fail("database transaction is closed", Err::INTERRUPTED);
	}
	return query_rows_inner(p_sql, p_args, true);
}

// Release connection ownership only for the current transaction generation.
void GDDatabaseClient::finish_tx(uint64_t p_generation) {
	if (tx_active && tx_generation == p_generation) {
		close_tx_rows(p_generation);
		tx_active = false;
		tx_postgres.unref();
		if (tx_lease.is_valid()) {
			tx_lease->release_lease();
			tx_lease.unref();
		}
		tx_call.unref();
		dispatch_deferred();
	}
}

// Resume shared calls only after the transaction's COMMIT or ROLLBACK result.
void GDDatabaseClient::dispatch_deferred() {
	while (!sqlite_deferred.is_empty()) {
		Ref<GDDatabaseCall> call = sqlite_deferred.front()->get();
		sqlite_deferred.pop_front();
		{
			MutexLock lock(sqlite_mutex);
			record_sqlite_wait(call.ptr());
		}
		call->deferred = false;
		if (call->canceled.is_set()) continue;
		call->forward_hold = call;
		Signal sent = call->rows_open ? query_rows_inner(call->sql, call->args) : query_inner(call->sql, call->args, call->one);
		sent.connect(callable_mp(call.ptr(), &GDDatabaseCall::forward), Object::CONNECT_ONE_SHOT);
	}
}

// Start a callback or statement sequence as a transaction.
Signal GDDatabaseClient::start_tx(const Callable &p_action, const Array &p_statements, bool p_migration) {
	if (closing) return query_fail("database is closing", Err::INTERRUPTED);
	if (!is_open()) {
		return query_fail("database is not open", Err::NOT_FOUND);
	}
	if (tx_active) {
		return query_fail("database transaction is already active", Err::ALREADY_EXISTS);
	}
	if (!p_migration && !p_action.is_valid()) {
		return query_fail("database transaction needs a callable", Err::INVALID_DATA);
	}
	if (p_migration) {
		if (p_statements.is_empty()) {
			return query_fail("database migration needs statements", Err::INVALID_DATA);
		}
		for (int i = 0; i < p_statements.size(); i++) {
			if (p_statements[i].get_type() != Variant::STRING || String(p_statements[i]).strip_edges().is_empty()) {
				return query_fail(vformat("database migration statement %d is not SQL", i + 1), Err::INVALID_DATA);
			}
		}
	}
	Ref<GDDatabaseTxCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = Ref<GDDatabaseClient>(this);
	call->generation = ++tx_generation;
	call->scope = ++next_tx_scope;
	call->action = p_action;
	call->statements = p_statements.duplicate();
	call->mode = p_migration ? GDDatabaseTxCall::MIGRATION : GDDatabaseTxCall::ACTION;
	call->tx.instantiate();
	call->tx->call = call;
	if (postgres_pool.is_valid()) {
		pool_txs.insert(call.ptr());
		Signal acquired = postgres_pool->acquire();
		call->acquire_call = Ref<GDPostgresPoolCall>(Object::cast_to<GDPostgresPoolCall>(acquired.get_object()));
		acquired.connect(callable_mp(call.ptr(), &GDDatabaseTxCall::on_acquire), Object::CONNECT_ONE_SHOT);
	} else {
		tx_active = true;
		tx_call = call;
		query_tx(call->generation, "BEGIN", Array()).connect(callable_mp(call.ptr(), &GDDatabaseTxCall::on_begin), Object::CONNECT_ONE_SHOT);
	}
	return Signal(call.ptr(), "finished");
}

// Execute a callback inside a transaction on the same connection.
Signal GDDatabaseClient::transaction(const Callable &p_action) {
	return start_tx(p_action, Array(), false);
}

// Retry the complete transaction after a serialization conflict.
Signal GDDatabaseClient::serialize(const Callable &p_action) {
	if (postgres.is_null() && postgres_pool.is_null()) return query_fail("serialization retry needs PostgreSQL", Err::UNSUPPORTED);
	if (!p_action.is_valid()) return query_fail("serialization retry needs a callable", Err::INVALID_DATA);
	Ref<GDDatabaseRetryCall> call;
	call.instantiate();
	call->self_hold = call;
	call->owner = Ref<GDDatabaseClient>(this);
	call->action = p_action;
	call->run();
	return Signal(call.ptr(), "finished");
}

// Apply SQL statements sequentially in one transaction.
Signal GDDatabaseClient::migrate(const Array &p_statements) {
	return start_tx(Callable(), p_statements, true);
}

// Close the current connection.
void GDDatabaseClient::close() {
	if (tx_call.is_valid() && tx_call->defer_close()) {
		return;
	}
	for (GDDatabaseTxCall *call : pool_txs) {
		if (call->defer_close()) closing = true;
	}
	if (closing) {
		bool committing = false;
		for (GDDatabaseTxCall *call : pool_txs) committing = committing || call->committing;
		if (committing) return;
	}
	closing = false;
	Ref<GDDatabaseTxCall> active = tx_call;
	LocalVector<Ref<GDDatabaseTxCall>> pooled;
	for (GDDatabaseTxCall *call : pool_txs) pooled.push_back(Ref<GDDatabaseTxCall>(call));
	for (const Ref<GDDatabaseTxCall> &call : pooled) close_tx_rows(call->generation);
	pool_txs.clear();
	while (!sqlite_deferred.is_empty()) {
		Ref<GDDatabaseCall> call = sqlite_deferred.front()->get();
		sqlite_deferred.pop_front();
		{
			MutexLock lock(sqlite_mutex);
			record_sqlite_wait(call.ptr());
		}
		call->complete({ Variant(), Err::make("database is closed", Err::INTERRUPTED) });
	}
	tx_call.unref();
	close_tx_rows(tx_generation);
	tx_active = false;
	tx_generation++;
	open_generation++;
	opening = false;
	if (postgres.is_valid()) {
		postgres->close();
		postgres.unref();
	}
	if (postgres_pool.is_valid()) {
		postgres_pool->close();
		postgres_pool.unref();
	}
	tx_postgres.unref();
	if (tx_lease.is_valid()) {
		tx_lease->release_lease();
		tx_lease.unref();
	}
	close_sqlite();
	if (active.is_valid()) {
		active->owner_closed();
	}
	for (const Ref<GDDatabaseTxCall> &call : pooled) call->owner_closed();
}

// Check whether the selected driver is connected.
bool GDDatabaseClient::is_open() const {
	return (postgres.is_valid() && postgres->is_open()) || (postgres_pool.is_valid() && postgres_pool->is_open()) || (sqlite.is_valid() && sqlite->is_open());
}

// Return connection state and cumulative wait statistics for the selected driver.
Dictionary GDDatabaseClient::stats() const {
	if (postgres_pool.is_valid()) {
		return postgres_pool->stats();
	}
	Dictionary out;
	int open = 0;
	int used = 0;
	int64_t waits;
	int64_t wait_ms;
	{
		MutexLock lock(sqlite_mutex);
		waits = sqlite_wait_count;
		wait_ms = sqlite_wait_ms;
	}
	if (postgres.is_valid() && postgres->is_open()) {
		open = 1;
		used = postgres->in_flight() > 0 ? 1 : 0;
	} else if (sqlite.is_valid() && sqlite->is_open()) {
		MutexLock lock(sqlite_mutex);
		open = 1;
		used = sqlite_active.is_valid() || sqlite_rows.is_valid() ? 1 : 0;
	}
	out["max_open_connections"] = 1;
	out["open_connections"] = open;
	out["in_use"] = used;
	out["idle"] = open - used;
	out["wait_count"] = waits;
	out["wait_duration_ms"] = wait_ms;
	out["max_idle_closed"] = int64_t(0); // A single connection is not closed by idle-count policy.
	out["max_idle_time_closed"] = int64_t(0); // No idle timeout is configured.
	out["max_lifetime_closed"] = int64_t(0); // No connection lifetime is configured.
	return out;
}

// Expose shared client methods to script.
void GDDatabaseClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "opts"), &GDDatabaseClient::open);
	ClassDB::bind_method(D_METHOD("open_async", "opts"), &GDDatabaseClient::open_async);
	ClassDB::bind_method(D_METHOD("query", "sql", "args"), &GDDatabaseClient::query, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_async", "sql", "args"), &GDDatabaseClient::query_async, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_row", "sql", "args"), &GDDatabaseClient::query_row, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_row_async", "sql", "args"), &GDDatabaseClient::query_row_async, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_rows", "sql", "args"), &GDDatabaseClient::query_rows, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("query_rows_async", "sql", "args"), &GDDatabaseClient::query_rows_async, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("transaction", "action"), &GDDatabaseClient::transaction);
	ClassDB::bind_method(D_METHOD("transaction_async", "action"), &GDDatabaseClient::transaction_async);
	ClassDB::bind_method(D_METHOD("serialize", "action"), &GDDatabaseClient::serialize);
	ClassDB::bind_method(D_METHOD("serialize_async", "action"), &GDDatabaseClient::serialize_async);
	ClassDB::bind_method(D_METHOD("migrate", "statements"), &GDDatabaseClient::migrate);
	ClassDB::bind_method(D_METHOD("migrate_async", "statements"), &GDDatabaseClient::migrate_async);
	ClassDB::bind_method(D_METHOD("stats"), &GDDatabaseClient::stats);
	ClassDB::bind_method(D_METHOD("close"), &GDDatabaseClient::close);
	ClassDB::bind_method(D_METHOD("is_open"), &GDDatabaseClient::is_open);
	ADD_AWAIT("open", "Pair:Variant");
	ADD_AWAIT("open_async", "Pair:Variant");
	ADD_AWAIT("query", "Pair:Dictionary");
	ADD_AWAIT("query_async", "Pair:Dictionary");
	ADD_AWAIT("query_row", "Pair:Dictionary");
	ADD_AWAIT("query_row_async", "Pair:Dictionary");
	ADD_AWAIT("query_rows", "Pair:GDDatabaseRows");
	ADD_AWAIT("query_rows_async", "Pair:GDDatabaseRows");
	ADD_AWAIT("transaction", "Pair:Variant");
	ADD_AWAIT("transaction_async", "Pair:Variant");
	ADD_AWAIT("serialize", "Pair:Variant");
	ADD_AWAIT("serialize_async", "Pair:Variant");
	ADD_AWAIT("migrate", "Pair:int");
	ADD_AWAIT("migrate_async", "Pair:int");
	ADD_AUTO_WAIT("open");
	ADD_AUTO_WAIT("query");
	ADD_AUTO_WAIT("query_row");
	ADD_AUTO_WAIT("query_rows");
	ADD_AUTO_WAIT("transaction");
	ADD_AUTO_WAIT("serialize");
	ADD_AUTO_WAIT("migrate");
}

// Create backend-specific entry points.
GDDatabaseAPI::GDDatabaseAPI() {
	postgres = memnew(GDPostgresAPI);
	redis = memnew(GDRedisAPI);
	sqlite = memnew(GDSQLiteAPI);
}

// Destroy backend-specific entry points.
GDDatabaseAPI::~GDDatabaseAPI() {
	memdelete(sqlite);
	memdelete(redis);
	memdelete(postgres);
}

// Create an unconnected shared client.
Ref<GDDatabaseClient> GDDatabaseAPI::client() const {
	Ref<GDDatabaseClient> out;
	out.instantiate();
	return out;
}

// Create a remote SQL client.
Ref<GDPostgresClient> GDPostgresAPI::client() const {
	Ref<GDPostgresClient> out;
	out.instantiate();
	return out;
}

// Create a remote SQL pool with a default connection count.
Ref<GDPostgresPool> GDPostgresAPI::pool(int64_t p_size) const {
	Ref<GDPostgresPool> out;
	out.instantiate();
	out->set_default_size(p_size);
	return out;
}

// Expose remote SQL factories to script.
void GDPostgresAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("client"), &GDPostgresAPI::client);
	ClassDB::bind_method(D_METHOD("pool", "size"), &GDPostgresAPI::pool, DEFVAL(0));
}

// Create a key-value client.
Ref<GDRedisClient> GDRedisAPI::client() const {
	Ref<GDRedisClient> out;
	out.instantiate();
	return out;
}

// Create a key-value pool with a default connection count.
Ref<GDRedisPool> GDRedisAPI::pool(int64_t p_size) const {
	Ref<GDRedisPool> out;
	out.instantiate();
	out->set_default_size(p_size);
	return out;
}

// Expose key-value factories to script.
void GDRedisAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("client"), &GDRedisAPI::client);
	ClassDB::bind_method(D_METHOD("pool", "size"), &GDRedisAPI::pool, DEFVAL(0));
}

// Expose embedded database creation to script.
void GDSQLiteAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "path", "opts"), &GDSQLiteAPI::open, DEFVAL(Dictionary()));
	ADD_PAIR_RESULT("open", "GDSQLiteDB");
}

// Expose the shared database entry point to script.
void GDDatabaseAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("client"), &GDDatabaseAPI::client);
	ClassDB::bind_method(D_METHOD("get_sqlite"), &GDDatabaseAPI::get_sqlite);
	ClassDB::bind_method(D_METHOD("get_postgres"), &GDDatabaseAPI::get_postgres);
	ClassDB::bind_method(D_METHOD("get_redis"), &GDDatabaseAPI::get_redis);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sqlite", PROPERTY_HINT_NONE, "GDSQLiteAPI"), "", "get_sqlite");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "postgres", PROPERTY_HINT_NONE, "GDPostgresAPI"), "", "get_postgres");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "redis", PROPERTY_HINT_NONE, "GDRedisAPI"), "", "get_redis");
}
