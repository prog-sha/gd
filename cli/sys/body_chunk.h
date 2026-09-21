// Retain immutable byte ranges until an incremental response consumes them.
#pragma once

#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Keep byte ownership separate from the ranges selected for transmission.
struct BodyChunk {
	struct Part {
		PackedByteArray bytes; // Shared owner with copy-on-write isolation.
		int64_t offset = 0, end = 0; // Start in the owner and exclusive end in the complete chunk.
	};
	Part first; // Inline storage for the common single-region response.
	LocalVector<Part> rest; // Further ordered regions, without a write-count limit.
	BodyChunk() = default;
	BodyChunk(const PackedByteArray &p_bytes) { append(p_bytes, 0, p_bytes.size()); }
	uint32_t count() const { return first.end ? rest.size() + 1 : 0; } // Return the number of retained regions.
	const Part &part(uint32_t p_index) const { return p_index ? rest[p_index - 1] : first; } // Inspect an owned region.
	int64_t size() const { return rest.is_empty() ? first.end : rest[rest.size() - 1].end; } // Return total selected bytes.
	bool is_empty() const { return first.end == 0; } // Report whether any bytes remain represented.
	void clear() { first = Part(); rest.reset(); } // Release owners and metadata after consumption.
	void append(const PackedByteArray &p_bytes, int64_t p_offset, int64_t p_count) { // Retain a validated range without copying it.
		if (!p_count) return;
		const int64_t end = size();
		const uint32_t n = count();
		// Merge adjacent selections only while they still share the same immutable owner.
		if (n) {
			Part &last = n == 1 ? first : rest[n - 2];
			const int64_t length = end - (n > 1 ? part(n - 2).end : 0);
			if (last.bytes.ptr() == p_bytes.ptr() && last.offset + length == p_offset) {
				last.end += p_count;
				return;
			}
		}
		const Part value{p_bytes, p_offset, end + p_count};
		if (!first.end) first = value;
		else rest.push_back(value);
	}
	uint32_t find(int64_t p_at) const { // Locate a byte without repeatedly scanning earlier regions.
		uint32_t lo = 0, hi = count();
		while (lo < hi) {
			const uint32_t mid = lo + (hi - lo) / 2;
			if (part(mid).end <= p_at) lo = mid + 1;
			else hi = mid;
		}
		return lo;
	}
	const uint8_t *ptr(uint32_t p_index, int64_t p_at) const { // Borrow bytes while this chunk retains their owner.
		return part(p_index).bytes.ptr() + part(p_index).offset + (p_at - (p_index ? part(p_index - 1).end : 0));
	}
};
