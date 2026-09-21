/**************************************************************************/
/*  box.h                                                                 */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Store and retrieve values with defined ordering and capacity policies.

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Min-heap supporting caller-selected ordering keys.
class GDBinaryHeap : public RefCounted {
	GDCLASS(GDBinaryHeap, RefCounted);

	LocalVector<Variant> items;
	Callable pick; // Callback selecting a rank, or empty to rank by the value itself.
	mutable bool ranking = false; // Reject mutations while a ranking callback is active.

	double rank_of(const Variant &p_v) const;
	void sink(int p_at);

protected:
	static void _bind_methods();

public:
	void set_pick(const Callable &p_pick);
	void push(const Variant &p_v);
	Variant pop();
	Variant peek() const;
	Array drain();
	int size() const;
	bool is_empty() const;
};

// Priority queue returning lower numeric priorities first.
// Preserve insertion order for equal priorities.
class GDPriorityQueue : public RefCounted {
	GDCLASS(GDPriorityQueue, RefCounted);

	Ref<GDBinaryHeap> heap;
	int seq = 0; // Sequence preserving insertion order.

	// Use insertion sequence to break equal-priority ties.
	static double rank_slot(const Variant &p_slot);

protected:
	static void _bind_methods();

public:
	GDPriorityQueue();
	void push(const Variant &p_value, double p_priority);
	Variant pop(); // Return null when empty.
	Variant peek() const;
	int size() const;
	bool is_empty() const;
};

// Capacity-bounded cache evicting least recently used entries.
// Load missing keys through a factory and retain its result.
// An optional TTL expires retained values by time.
class GDLRUCache : public RefCounted {
	GDCLASS(GDLRUCache, RefCounted);

	// Link entries by access order so eviction does not scan the whole cache.
	struct Slot {
		Variant value;
		Variant key; // Key used to remove an evicted entry from the map.
		uint64_t at = 0; // Insertion time used for expiry.
		Slot *prev = nullptr; // Next older entry.
		Slot *next = nullptr; // Next newer entry.
	};
	// Treat String and StringName as the same key.
	HashMap<Variant, Slot, HashMapHasherDefault, StringLikeVariantComparator> box;
	int cap = 1024; // Maximum retained entries.
	uint64_t ttl_ms = 0; // Retention duration; zero disables expiry.
	Slot *oldest = nullptr; // Next entry to evict.
	Slot *newest = nullptr; // Most recently accessed entry.
	uint64_t hits = 0;
	uint64_t misses = 0;

	void evict(); // Evict overflow in least-recently-used order.
	void unlink(Slot *p_s); // Unlink an entry from access order.
	void touch(Slot *p_s); // Move an entry to the newest end.
	bool expired(const Slot &p_s) const; // Check whether an entry expired.

protected:
	static void _bind_methods();

public:
	void setup(int p_cap, int p_ttl_ms = 0); // Configure capacity and TTL.
	void put(const Variant &p_key, const Variant &p_value);
	Variant take(const Variant &p_key, const Variant &p_fallback = Variant()); // Look up a key or return the fallback.
	// Load a missing key through the factory, cache it, and return it.
	Variant fetch(const Variant &p_key, const Callable &p_make);
	bool has(const Variant &p_key);
	void erase(const Variant &p_key);
	void clear();
	int size() const { return box.size(); }
	int limit() const { return cap; }
	uint64_t hit_count() const { return hits; }
	uint64_t miss_count() const { return misses; }
	double hit_rate() const; // Return the hit ratio from zero to one.
};

// Memoize results for repeated arguments.
// Argument sequences form keys, so values must support text representation.
class GDMemoizedCallable : public RefCounted {
	GDCLASS(GDMemoizedCallable, RefCounted);

	Ref<GDLRUCache> box;
	Callable fn;

protected:
	static void _bind_methods();

public:
	void setup(const Callable &p_fn, int p_limit);
	Variant call_with(const Array &p_args);
};

// Pure array and dictionary transformations.
class Coll {
public:
	static Dictionary group_by(const Array &p_items, const Callable &p_key);
	static Dictionary map_values(const Dictionary &p_src, const Callable &p_fn);
	static Dictionary filter_keys(const Dictionary &p_src, const Callable &p_pred);
	static Array partition(const Array &p_items, const Callable &p_pred);
	static Array chunk(const Array &p_items, int p_size);
	static Array unique_by(const Array &p_items, const Callable &p_key);
	static Array unique(const Array &p_items);
	static Array sort_by(const Array &p_items, const Callable &p_pick);
	// Sort by field name without one script callback per item.
	static Array sort_key(const Array &p_items, const String &p_key);
	static Array zip(const Array &p_a, const Array &p_b); // Pair elements up to the shorter array's length.
	static double sum_of(const Array &p_items, const Callable &p_pick);
	static Variant max_by(const Array &p_items, const Callable &p_pick); // Return null for empty input.
	static Variant min_by(const Array &p_items, const Callable &p_pick);
	static Dictionary index_by(const Array &p_items, const Callable &p_key);
	static Dictionary deep_merge(const Dictionary &p_base, const Dictionary &p_over);
};
