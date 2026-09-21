/**************************************************************************/
/*  coll_call.h                                                           */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Split callback-based collection operations across short scheduling turns.

#pragma once

#include "cli/sys/std.h"

#include "core/templates/list.h"

// Invoke main-thread Callables incrementally and signal the final result.
class GDCollectionCall : public RefCounted {
	GDCLASS(GDCollectionCall, RefCounted);

public:
	static void shutdown(); // Release unfinished collection operations and callbacks at shutdown.
	enum Mode {
		GROUP, // Group values into arrays by key.
		MAP_VALUES, // Transform dictionary values.
		FILTER_KEYS, // Keep dictionary keys satisfying a predicate.
		PARTITION, // Partition values into two groups.
		UNIQUE_BY, // Remove values with duplicate selected keys.
		SORT_BY, // Sort by selected keys.
		SUM_OF, // Sum selected numbers.
		MAX_BY, // Select the value with the largest key.
		MIN_BY, // Select the value with the smallest key.
		INDEX_BY, // Index values by selected keys.
	};

private:
	Ref<GDCollectionCall> self_hold; // Retain the operation through completion.
	Mode mode = GROUP;
	Callable fn; // Caller callback invoked on the main thread.
	Array items; // Array items or dictionary keys.
	Dictionary src; // Source dictionary.
	Dictionary out; // Intermediate dictionary result.
	Dictionary seen; // Previously encountered uniqueness keys.
	Array yes; // Array result or true side of a partition.
	Array no; // False side of a partition.
	Array ranks; // Sort keys and their values.
	Variant best; // Selected maximum or minimum item.
	double best_value = 0.0; // Current comparison value for an extremum.
	double total = 0.0; // Running sum.
	int at = 0; // Next item position.
	bool posted = false; // Prevent duplicate continuation enqueue.
	static List<Ref<GDCollectionCall>> queue; // Shared queue providing fair progress for all operations.
	static GDCollectionCall *runner; // Target delivering the shared queue's continuation.

	// Advance operations one at a time within a shared time slice.
	void dispatch();
	bool step_one(); // Invoke at most one Callable and report whether work remains.
	static void enqueue(const Ref<GDCollectionCall> &p_call); // Append an operation to the shared queue.
	static void reschedule(); // Move ready delivery to the queue head.
	// Receive worker-sorted values.
	void sorted(const Variant &p_value);
	// Deliver the final value once.
	void done(const Variant &p_value);
	// Switch the shared runner's ready callback.
	void watch(bool p_on);

protected:
	static void _bind_methods();

public:
	// Start an array operation.
	static Signal start_array(Mode p_mode, const Array &p_items, const Callable &p_fn);
	// Start a dictionary operation.
	static Signal start_dict(Mode p_mode, const Dictionary &p_src, const Callable &p_fn);
};
