// Borrow response framing and owned body ranges without building an intermediate array.
#pragma once
#include "cli/sys/body_chunk.h"

// Describe one borrowed region retained by the caller until its write completes.
struct GDWrite {
	const uint8_t *data = nullptr; // Start of readable bytes.
	int64_t size = 0; // Remaining bytes in this region.
};

// Visit only the prefix that the current native transfer can consume.
class GDWrites {
	GDWrite head, tail; // Framing before and after the body.
	const BodyChunk *body = nullptr; // Owner retained by the transport caller.
	int64_t at = 0; // Byte position within the body.
	uint32_t index = 0; // Next retained region.
public:
	GDWrites(GDWrite p_head, const BodyChunk *p_body = nullptr, int64_t p_at = 0, GDWrite p_tail = {}) :
			head(p_head), tail(p_tail), body(p_body), at(p_at), index(body ? body->find(at) : 0) {} // Start at the first unsent region.
	uint64_t count() const { return uint64_t(bool(head.size)) + (body ? body->count() - index : 0) + bool(tail.size); } // Count remaining descriptors without scanning.
	bool next(GDWrite &r_part) { // Borrow one nonempty framing or body region in wire order.
		if (head.size) { r_part = head; head = {}; return true; }
		if (body && index < body->count()) {
			const int64_t end = body->part(index).end;
			r_part = {body->ptr(index++, at), end - at};
			at = end;
			return true;
		}
		r_part = tail;
		tail = {};
		return r_part.size != 0;
	}
};
