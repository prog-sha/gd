/**************************************************************************/
/*  coll_call.cpp                                                         */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Split callback-based collection processing across runtime turns.

#include "cli/api/coll_call.h"
#include "cli/sys/clock.h"

#include "cli/api/box.h"
#include "cli/data/bytes.h"
#include "cli/sys/file_job.h"
#include "cli/sys/sched.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/config/engine.h"
#include "core/os/os.h"

List<Ref<GDCollectionCall>> GDCollectionCall::queue;
GDCollectionCall *GDCollectionCall::runner = nullptr;

// Release unfinished script references before language-runtime shutdown.
void GDCollectionCall::shutdown() {
	runner = nullptr;
	while (!queue.is_empty()) {
		Ref<GDCollectionCall> call = queue.front()->get();
		queue.pop_front();
		call->fn = Callable();
		call->self_hold.unref();
	}
}

// Register the script completion signal.
void GDCollectionCall::_bind_methods() {
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)));
}

// Start a cooperatively scheduled array operation.
Signal GDCollectionCall::start_array(Mode p_mode, const Array &p_items, const Callable &p_fn) {
	Ref<GDCollectionCall> call;
	call.instantiate();
	call->self_hold = call;
	call->mode = p_mode;
	call->items = p_items.duplicate(true);
	call->fn = p_fn;
	call->best_value = p_mode == MAX_BY ? -INFINITY : INFINITY;
	enqueue(call);
	return Signal(call.ptr(), "finished");
}

// Start a cooperatively scheduled dictionary operation.
Signal GDCollectionCall::start_dict(Mode p_mode, const Dictionary &p_src, const Callable &p_fn) {
	Ref<GDCollectionCall> call;
	call.instantiate();
	call->self_hold = call;
	call->mode = p_mode;
	call->src = p_src.duplicate(true);
	call->items = call->src.keys();
	call->fn = p_fn;
	enqueue(call);
	return Signal(call.ptr(), "finished");
}

// Append to the shared queue and schedule the first continuation.
void GDCollectionCall::enqueue(const Ref<GDCollectionCall> &p_call) {
	queue.push_back(p_call);
	if (!runner) {
		runner = p_call.ptr();
		runner->watch(true);
	}
}

// Round-robin one callback per operation within the scheduler time slice.
void GDCollectionCall::dispatch() {
	posted = false;
	if (runner != this) {
		return;
	}
	Ref<GDCollectionCall> keep(this); // Retain the completed runner until ownership transfers.
	const uint64_t until = GDClock::usec() + GD_SCHED_SLICE_USEC;
	do {
		Ref<GDCollectionCall> call = queue.front()->get();
		queue.pop_front();
		if (call->step_one()) {
			queue.push_back(call);
		}
	} while (!queue.is_empty() && GDClock::usec() < until);
	reschedule();
	if (!queue.is_empty()) {
		runner->watch(true);
	}
}

// Invoke at most one callback and return a result if the operation finishes.
bool GDCollectionCall::step_one() {
	if (at >= items.size()) {
		if (mode == SORT_BY) {
			const Array work = ranks;
			GDValueCall::start([work]() -> Variant {
				const Array sorted = Coll::sort_key(work, "rank");
				Array values;
				values.resize(sorted.size());
				for (int i = 0; i < sorted.size(); i++) {
					values[i] = Dictionary(sorted[i])["value"];
				}
				return values;
			}).connect(callable_mp(this, &GDCollectionCall::sorted), Object::CONNECT_ONE_SHOT);
			return false;
		}
		switch (mode) {
			case GROUP:
			case MAP_VALUES:
			case FILTER_KEYS:
			case INDEX_BY:
				done(out);
				break;
			case PARTITION: {
				Array parts;
				parts.push_back(yes);
				parts.push_back(no);
				done(parts);
			} break;
			case UNIQUE_BY:
				done(yes);
				break;
			case SUM_OF:
				done(total);
				break;
			case MAX_BY:
			case MIN_BY:
				done(best);
				break;
			case SORT_BY:
				break;
		}
		return false;
	}

	const Variant item = items[at++];
	const Variant value = (mode == MAP_VALUES) ? src[item] : item;
	const Variant picked = fn.call(value);
	switch (mode) {
			case GROUP: {
				Array bucket = out.get(picked, Array());
				bucket.push_back(item);
				out[picked] = bucket;
			} break;
			case MAP_VALUES:
				out[item] = picked;
				break;
			case FILTER_KEYS:
				if ((bool)picked) {
					out[item] = src[item];
				}
				break;
			case PARTITION:
				if ((bool)picked) {
					yes.push_back(item);
				} else {
					no.push_back(item);
				}
				break;
			case UNIQUE_BY:
				if (!seen.has(picked)) {
					seen[picked] = true;
					yes.push_back(item);
				}
				break;
			case SORT_BY: {
				Dictionary rank;
				rank["rank"] = picked;
				rank["value"] = item;
				ranks.push_back(rank);
			} break;
			case SUM_OF:
				total += (double)picked;
				break;
			case MAX_BY: {
				const double number = picked;
				if (number > best_value) {
					best_value = number;
					best = item;
				}
			} break;
			case MIN_BY: {
				const double number = picked;
				if (number < best_value) {
					best_value = number;
					best = item;
				}
			} break;
			case INDEX_BY:
				out[picked] = item;
				break;
	}
	return true;
}

// Move monitoring to the current queue head, stopping when empty.
void GDCollectionCall::reschedule() {
	GDCollectionCall *next = queue.is_empty() ? nullptr : queue.front()->get().ptr();
	if (runner == next) {
		return;
	}
	if (runner) {
		runner->watch(false);
	}
	runner = next;
	if (runner) {
		runner->watch(true);
	}
}

// Deliver worker-sorted values as the final result.
void GDCollectionCall::sorted(const Variant &p_value) {
	done(p_value);
}

// Deliver completion and release retained values.
void GDCollectionCall::done(const Variant &p_value) {
	Ref<GDCollectionCall> keep(this);
	fn = Callable();
	items.clear();
	src.clear();
	emit_signal("finished", p_value);
	self_hold.unref();
}

// Enqueue readiness only when no continuation is already pending.
void GDCollectionCall::watch(bool p_on) {
	if (p_on && !posted) {
		posted = true;
		Async::post(Ref<RefCounted>(this), callable_mp(this, &GDCollectionCall::dispatch));
	}
}
