/**************************************************************************/
/*  box.cpp                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement containers declared in box.h.

#include "cli/api/box.h"
#include "cli/sys/clock.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/pair.h"
#include "core/templates/sort_array.h"

// ---------------- Binary heap ----------------

// Use one rejection message for every mutation attempted during ranking.
#define HEAP_BUSY "GDBinaryHeap: cannot change the heap while the pick callable runs."

// Invoke caller-supplied ranking, which may attempt to mutate the heap.
// Mark the callback interval so all mutation entry points reject reentry.
double GDBinaryHeap::rank_of(const Variant &p_v) const {
	if (!pick.is_valid()) {
		return (double)p_v;
	}
	ranking = true;
	const Variant got = pick.call(p_v);
	ranking = false;
	return (double)got;
}

// Set the heap's rank-selection callback.
void GDBinaryHeap::set_pick(const Callable &p_pick) {
	pick = p_pick;
}

// Insert a value and restore heap order.
void GDBinaryHeap::push(const Variant &p_v) {
	ERR_FAIL_COND_MSG(ranking, HEAP_BUSY);
	items.push_back(p_v);
	int at = (int)items.size() - 1;
	while (at > 0) {
		const int parent = (at - 1) / 2;
		if (rank_of(items[at]) >= rank_of(items[parent])) {
			break;
		}
		SWAP(items[at], items[parent]);
		at = parent;
	}
}

// Move a heap entry downward until its rank is ordered.
void GDBinaryHeap::sink(int p_at) {
	int at = p_at;
	while (true) {
		// Recheck size each iteration to avoid stale indices if ranking changes the heap.
		const int n = (int)items.size();
		if (at >= n) {
			return;
		}
		const int left = at * 2 + 1;
		const int right = left + 1;
		int small = at;
		if (left < n && rank_of(items[left]) < rank_of(items[small])) {
			small = left;
		}
		if (right < n && rank_of(items[right]) < rank_of(items[small])) {
			small = right;
		}
		if (small == at) {
			return;
		}
		SWAP(items[at], items[small]);
		at = small;
	}
}

// Remove and return the next value.
Variant GDBinaryHeap::pop() {
	ERR_FAIL_COND_V_MSG(ranking, Variant(), HEAP_BUSY);
	if (items.is_empty()) {
		return Variant();
	}
	const Variant top = items[0];
	const Variant last = items[items.size() - 1];
	items.remove_at(items.size() - 1);
	if (!items.is_empty()) {
		items[0] = last;
		sink(0);
	}
	return top;
}

// Return the next value without removing it.
Variant GDBinaryHeap::peek() const {
	return items.is_empty() ? Variant() : items[0];
}

// Remove all heap values in priority order.
Array GDBinaryHeap::drain() {
	ERR_FAIL_COND_V_MSG(ranking, Array(), HEAP_BUSY);
	Array out;
	while (!items.is_empty()) {
		out.push_back(pop());
	}
	return out;
}

// Return the retained element count.
int GDBinaryHeap::size() const {
	return (int)items.size();
}

// Report whether the container is empty.
bool GDBinaryHeap::is_empty() const {
	return items.is_empty();
}

// Register public script methods and properties.
void GDBinaryHeap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_pick", "pick"), &GDBinaryHeap::set_pick);
	ClassDB::bind_method(D_METHOD("push", "value"), &GDBinaryHeap::push);
	ClassDB::bind_method(D_METHOD("pop"), &GDBinaryHeap::pop);
	ClassDB::bind_method(D_METHOD("peek"), &GDBinaryHeap::peek);
	ClassDB::bind_method(D_METHOD("drain"), &GDBinaryHeap::drain);
	ClassDB::bind_method(D_METHOD("size"), &GDBinaryHeap::size);
	ClassDB::bind_method(D_METHOD("is_empty"), &GDBinaryHeap::is_empty);
}

// ---------------- Priority queue ----------------

double GDPriorityQueue::rank_slot(const Variant &p_slot) {
	const Dictionary d = p_slot;
	return (double)d["p"] * 1000000.0 + (double)d["n"];
}

// Initialize priority-ordered retrieval.
GDPriorityQueue::GDPriorityQueue() {
	heap.instantiate();
	heap->set_pick(callable_mp_static(&GDPriorityQueue::rank_slot));
}

// Insert a value with its priority.
void GDPriorityQueue::push(const Variant &p_value, double p_priority) {
	seq++;
	Dictionary slot;
	slot["v"] = p_value;
	slot["p"] = p_priority;
	slot["n"] = seq;
	heap->push(slot);
}

// Remove and return the next value.
Variant GDPriorityQueue::pop() {
	const Variant slot = heap->pop();
	return slot.get_type() == Variant::DICTIONARY ? Dictionary(slot)["v"] : Variant();
}

// Return the next value without removing it.
Variant GDPriorityQueue::peek() const {
	const Variant slot = heap->peek();
	return slot.get_type() == Variant::DICTIONARY ? Dictionary(slot)["v"] : Variant();
}

// Return the retained element count.
int GDPriorityQueue::size() const {
	return heap->size();
}

// Report whether the container is empty.
bool GDPriorityQueue::is_empty() const {
	return heap->is_empty();
}

// Register public script methods and properties.
void GDPriorityQueue::_bind_methods() {
	ClassDB::bind_method(D_METHOD("push", "value", "priority"), &GDPriorityQueue::push, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("pop"), &GDPriorityQueue::pop);
	ClassDB::bind_method(D_METHOD("peek"), &GDPriorityQueue::peek);
	ClassDB::bind_method(D_METHOD("size"), &GDPriorityQueue::size);
	ClassDB::bind_method(D_METHOD("is_empty"), &GDPriorityQueue::is_empty);
}

// ---------------- LRU cache ----------------

void GDLRUCache::setup(int p_cap, int p_ttl_ms) {
	cap = MAX(1, p_cap);
	ttl_ms = (uint64_t)MAX(0, p_ttl_ms);
	evict();
}

// Check expiry, treating an unlimited TTL as always active.
bool GDLRUCache::expired(const Slot &p_s) const {
	return ttl_ms != 0 && GDClock::msec() - p_s.at > ttl_ms;
}

// Evict entries with the oldest access times first.
// Access-order links avoid scanning all entries for each eviction.
void GDLRUCache::evict() {
	while ((int)box.size() > cap && oldest != nullptr) {
		Slot *gone = oldest;
		unlink(gone);
		box.erase(gone->key);
	}
}

// Unlink an entry from access order.
void GDLRUCache::unlink(Slot *p_s) {
	if (p_s->prev) {
		p_s->prev->next = p_s->next;
	} else {
		oldest = p_s->next;
	}
	if (p_s->next) {
		p_s->next->prev = p_s->prev;
	} else {
		newest = p_s->prev;
	}
	p_s->prev = nullptr;
	p_s->next = nullptr;
}

// Move an entry to the newest end of access order.
void GDLRUCache::touch(Slot *p_s) {
	if (newest == p_s) {
		return;
	}
	if (p_s->prev || p_s->next || oldest == p_s) {
		unlink(p_s);
	}
	p_s->prev = newest;
	p_s->next = nullptr;
	if (newest) {
		newest->next = p_s;
	}
	newest = p_s;
	if (!oldest) {
		oldest = p_s;
	}
}

// Store or replace a cache entry.
void GDLRUCache::put(const Variant &p_key, const Variant &p_value) {
	Slot *found = box.getptr(p_key);
	if (found) {
		found->value = p_value;
		found->at = GDClock::msec();
		touch(found);
		return;
	}
	Slot slot;
	slot.value = p_value;
	slot.key = p_key;
	slot.at = GDClock::msec();
	box.insert(p_key, slot);
	touch(box.getptr(p_key));
	evict();
}

// Look up a key or fallback and refresh access order on a hit.
Variant GDLRUCache::take(const Variant &p_key, const Variant &p_fallback) {
	Slot *found = box.getptr(p_key);
	if (found && expired(*found)) {
		erase(p_key);
		found = nullptr;
	}
	if (!found) {
		misses++;
		return p_fallback;
	}
	hits++;
	touch(found);
	return found->value;
}

// Load and cache missing values through a factory.
Variant GDLRUCache::fetch(const Variant &p_key, const Callable &p_make) {
	Slot *found = box.getptr(p_key);
	if (found && expired(*found)) {
		erase(p_key);
		found = nullptr;
	}
	if (found) {
		hits++;
		touch(found);
		return found->value;
	}
	misses++;
	if (!p_make.is_valid()) {
		return Variant(); // No factory; do not cache a value.
	}
	// Check callback invocation before caching its result.
	// An arity mismatch returning empty must not become a permanent cache hit.
	const Variant *arg = &p_key;
	Variant made;
	Callable::CallError err;
	p_make.callp(&arg, 1, made, err);
	if (err.error != Callable::CallError::CALL_OK) {
		ERR_PRINT(vformat("GDLRUCache.fetch: cannot call the maker (%s)",
				Variant::get_callable_error_text(p_make, &arg, 1, err)));
		return Variant(); // Do not cache failed invocation.
	}
	put(p_key, made);
	return made;
}

// Check existence without refreshing access order, removing expired entries.
bool GDLRUCache::has(const Variant &p_key) {
	Slot *found = box.getptr(p_key);
	if (!found) {
		return false;
	}
	if (expired(*found)) {
		erase(p_key);
		return false;
	}
	return true;
}

// Return the hit ratio, or zero before any lookup.
double GDLRUCache::hit_rate() const {
	const uint64_t total = hits + misses;
	return total == 0 ? 0.0 : (double)hits / (double)total;
}

// Remove one entry.
void GDLRUCache::erase(const Variant &p_key) {
	Slot *found = box.getptr(p_key);
	if (!found) {
		return;
	}
	unlink(found);
	box.erase(p_key);
}

// Clear entries and reset counters.
void GDLRUCache::clear() {
	box.clear();
	oldest = nullptr;
	newest = nullptr;
	hits = 0;
	misses = 0;
}

// Register public script methods and properties.
void GDLRUCache::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "limit", "ttl_ms"), &GDLRUCache::setup, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("put", "key", "value"), &GDLRUCache::put);
	ClassDB::bind_method(D_METHOD("take", "key", "fallback"), &GDLRUCache::take, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("fetch", "key", "make"), &GDLRUCache::fetch);
	ClassDB::bind_method(D_METHOD("has", "key"), &GDLRUCache::has);
	ClassDB::bind_method(D_METHOD("erase", "key"), &GDLRUCache::erase);
	ClassDB::bind_method(D_METHOD("clear"), &GDLRUCache::clear);
	ClassDB::bind_method(D_METHOD("size"), &GDLRUCache::size);
	ClassDB::bind_method(D_METHOD("limit"), &GDLRUCache::limit);
	ClassDB::bind_method(D_METHOD("hit_count"), &GDLRUCache::hit_count);
	ClassDB::bind_method(D_METHOD("miss_count"), &GDLRUCache::miss_count);
	ClassDB::bind_method(D_METHOD("hit_rate"), &GDLRUCache::hit_rate);
}

// ---------------- Memoization ----------------

void GDMemoizedCallable::setup(const Callable &p_fn, int p_limit) {
	fn = p_fn;
	box.instantiate();
	box->setup(p_limit);
}

// Invoke the retained Callable with arguments.
Variant GDMemoizedCallable::call_with(const Array &p_args) {
	const String key = JSON::stringify(p_args);
	if (box->has(key)) {
		return box->take(key);
	}
	const Variant v = fn.callv(p_args);
	box->put(key, v);
	return v;
}

// Register public script methods and properties.
void GDMemoizedCallable::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "fn", "limit"), &GDMemoizedCallable::setup, DEFVAL(128));
	ClassDB::bind_method(D_METHOD("call_with", "args"), &GDMemoizedCallable::call_with);
}

namespace {

// Compare selected keys through Variant ordering, supporting numeric and textual values.
struct RankLess {
	bool operator()(const Pair<Variant, Variant> &p_a, const Pair<Variant, Variant> &p_b) const {
		bool valid = false;
		Variant out;
		Variant::evaluate(Variant::OP_LESS, p_a.first, p_b.first, out, valid);
		if (valid) {
			return (bool)out;
		}
		// Order incomparable values, such as mixed types or missing keys, by type ID.
		// Always returning false would fail to establish consistent ordering.
		// That could disturb even comparable elements during larger sorts.
		return p_a.first.get_type() < p_b.first.get_type();
	}
};

} // namespace

// ---------------- Arrays and dictionaries ----------------

Dictionary Coll::group_by(const Array &p_items, const Callable &p_key) {
	Dictionary out;
	for (int i = 0; i < p_items.size(); i++) {
		const Variant k = p_key.call(p_items[i]);
		if (!out.has(k)) {
			out[k] = Array();
		}
		Array bucket = out[k];
		bucket.push_back(p_items[i]);
	}
	return out;
}

// Partition values by predicate result.
Array Coll::partition(const Array &p_items, const Callable &p_pred) {
	Array yes;
	Array no;
	for (int i = 0; i < p_items.size(); i++) {
		if ((bool)p_pred.call(p_items[i])) {
			yes.push_back(p_items[i]);
		} else {
			no.push_back(p_items[i]);
		}
	}
	Array out;
	out.push_back(yes);
	out.push_back(no);
	return out;
}

// Split an array into chunks of the requested size.
Array Coll::chunk(const Array &p_items, int p_size) {
	Array out;
	if (p_size <= 0) {
		return out;
	}
	int at = 0;
	while (at < p_items.size()) {
		out.push_back(p_items.slice(at, MIN(at + p_size, p_items.size())));
		at += p_size;
	}
	return out;
}

// Keep only values with unique selected keys.
Array Coll::unique_by(const Array &p_items, const Callable &p_key) {
	Dictionary seen;
	Array out;
	for (int i = 0; i < p_items.size(); i++) {
		const Variant k = p_key.call(p_items[i]);
		if (!seen.has(k)) {
			seen[k] = true;
			out.push_back(p_items[i]);
		}
	}
	return out;
}

// Build a dictionary indexed by selected keys.
Dictionary Coll::index_by(const Array &p_items, const Callable &p_key) {
	Dictionary out;
	for (int i = 0; i < p_items.size(); i++) {
		out[p_key.call(p_items[i])] = p_items[i];
	}
	return out;
}

// Merge nested dictionaries recursively.
Dictionary Coll::deep_merge(const Dictionary &p_base, const Dictionary &p_over) {
	Dictionary out = p_base.duplicate(true);
	for (const KeyValue<Variant, Variant> &kv : p_over) {
		if (out.has(kv.key) && out[kv.key].get_type() == Variant::DICTIONARY && kv.value.get_type() == Variant::DICTIONARY) {
			out[kv.key] = deep_merge(out[kv.key], kv.value);
		} else {
			out[kv.key] = kv.value;
		}
	}
	return out;
}

// Transform each dictionary value with a Callable.
Dictionary Coll::map_values(const Dictionary &p_src, const Callable &p_fn) {
	Dictionary out;
	for (const Variant &k : p_src.keys()) {
		out[k] = p_fn.call(p_src[k]);
	}
	return out;
}

// Keep dictionary keys satisfying the predicate.
Dictionary Coll::filter_keys(const Dictionary &p_src, const Callable &p_pred) {
	Dictionary out;
	for (const Variant &k : p_src.keys()) {
		if ((bool)p_pred.call(k)) {
			out[k] = p_src[k];
		}
	}
	return out;
}

// Remove duplicate array values.
Array Coll::unique(const Array &p_items) {
	Dictionary seen;
	Array out;
	for (int i = 0; i < p_items.size(); i++) {
		if (!seen.has(p_items[i])) {
			seen[p_items[i]] = true;
			out.push_back(p_items[i]);
		}
	}
	return out;
}

// Sum selected numeric values.
double Coll::sum_of(const Array &p_items, const Callable &p_pick) {
	double total = 0.0;
	for (int i = 0; i < p_items.size(); i++) {
		total += (double)p_pick.call(p_items[i]);
	}
	return total;
}

// Return the item with the largest selected value.
Variant Coll::max_by(const Array &p_items, const Callable &p_pick) {
	Variant best;
	double best_v = -INFINITY;
	for (int i = 0; i < p_items.size(); i++) {
		const double v = p_pick.call(p_items[i]);
		if (v > best_v) {
			best_v = v;
			best = p_items[i];
		}
	}
	return best;
}

// Return the item with the smallest selected value.
Variant Coll::min_by(const Array &p_items, const Callable &p_pick) {
	Variant best;
	double best_v = INFINITY;
	for (int i = 0; i < p_items.size(); i++) {
		const double v = p_pick.call(p_items[i]);
		if (v < best_v) {
			best_v = v;
			best = p_items[i];
		}
	}
	return best;
}

// Sort by field name without one script callback per item.
Array Coll::sort_key(const Array &p_items, const String &p_key) {
	// Read ranking fields directly from dictionaries instead of invoking scripts.
	const int n = p_items.size();
	LocalVector<Pair<Variant, Variant>> pairs;
	pairs.resize(n);
	for (int i = 0; i < n; i++) {
		const Variant &item = p_items[i];
		if (item.get_type() == Variant::DICTIONARY) {
			pairs[i].first = Dictionary(item).get(p_key, Variant());
		} else {
			bool ok = false;
			pairs[i].first = item.get_named(p_key, ok);
		}
		pairs[i].second = item;
	}
	SortArray<Pair<Variant, Variant>, RankLess> sorter;
	sorter.sort(pairs.ptr(), n);
	Array out;
	out.resize(n);
	for (int i = 0; i < n; i++) {
		out[i] = pairs[i].second;
	}
	return out;
}

// Sort an array by selected keys.
Array Coll::sort_by(const Array &p_items, const Callable &p_pick) {
	// Compute each key once before native sorting.
	// Calling the selector on every comparison would multiply script invocations.
	// Store key-value pairs directly instead of allocating a separate array for each pair.
	const int n = p_items.size();
	LocalVector<Pair<Variant, Variant>> pairs;
	pairs.resize(n);
	for (int i = 0; i < n; i++) {
		pairs[i].first = p_pick.call(p_items[i]); // Retain key types so textual ordering remains available.
		pairs[i].second = p_items[i];
	}
	SortArray<Pair<Variant, Variant>, RankLess> sorter;
	sorter.sort(pairs.ptr(), n);
	Array out;
	out.resize(n);
	for (int i = 0; i < n; i++) {
		out[i] = pairs[i].second;
	}
	return out;
}

// Pair array elements at corresponding positions.
Array Coll::zip(const Array &p_a, const Array &p_b) {
	Array out;
	const int n = MIN(p_a.size(), p_b.size());
	for (int i = 0; i < n; i++) {
		Array one;
		one.push_back(p_a[i]);
		one.push_back(p_b[i]);
		out.push_back(one);
	}
	return out;
}
