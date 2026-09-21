/**************************************************************************/
/*  bytes.h                                                               */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Build and compact byte sequences.
// Avoid intermediate string allocation for each number or protocol message.
// Response and protocol construction use these operations repeatedly per request.

#pragma once

#include "cli/data/json.h"
#include "cli/sys/std.h"
#include "cli/sys/wait.h"
#include "cli/sys/task.h"
#include "cli/net/wire.h"
#include "core/os/main_loop.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
#include "core/variant/callable.h"
#include "core/variant/variant.h"

// Keep byte-buffer capacity when clearing content so repeated construction can reuse storage.
// Use storage whose empty state does not discard its allocation.
using ByteBuf = LocalVector<uint8_t>;

// Append raw bytes unchanged.
inline void put_raw(LocalVector<uint8_t> &r_out, const uint8_t *p_src, int p_len) {
	if (p_len <= 0) {
		return;
	}
	const uint32_t at = r_out.size();
	r_out.resize(at + (uint32_t)p_len);
	memcpy(r_out.ptr() + at, p_src, p_len);
}

// Write decimal digits directly, including a sign for negative values.
inline void push_digits(LocalVector<uint8_t> &r_out, int64_t p_v) {
	char tmp[24]; // Capacity for signed int64 decimal text with spare room.
	int n = 0;
	uint64_t u;
	if (p_v < 0) {
		r_out.push_back('-');
		u = (uint64_t)(-(p_v + 1)) + 1; // Compute magnitude without overflowing at the minimum signed value.
	} else {
		u = (uint64_t)p_v;
	}
	if (u == 0) {
		tmp[n++] = '0';
	}
	while (u > 0) {
		tmp[n++] = (char)('0' + (u % 10));
		u /= 10;
	}
	const uint32_t at = r_out.size();
	r_out.resize(at + n);
	uint8_t *w = r_out.ptr() + at;
	for (int i = 0; i < n; i++) {
		w[i] = (uint8_t)tmp[n - 1 - i]; // Reverse digits collected least-significant first.
	}
}

// Compact consumed bytes out of the receive buffer.
// Compacting after every message repeatedly copies the entire remaining batch.
// Compact only before receiving more input to avoid quadratic copying.
inline void compact_recv(PackedByteArray &r_buf, int &r_at) {
	if (r_at == 0) {
		return;
	}
	if (r_at >= r_buf.size()) {
		r_buf.resize(0);
		r_at = 0;
		return;
	}
	const int left = r_buf.size() - r_at;
	memmove(r_buf.ptrw(), r_buf.ptr() + r_at, left);
	r_buf.resize(left);
	r_at = 0;
}

constexpr uint32_t BUFFER_REUSE_MAX = 8192; // Allocation capacity retained for small I/O buffers.

// Move a small live suffix out of a large buffer so it cannot pin excessive capacity.
inline void keep_tail(ByteBuf &r_buf, uint32_t p_at, uint32_t p_len) {
	const uint64_t reuse = MAX((uint64_t)BUFFER_REUSE_MAX, (uint64_t)p_len * 2);
	if (r_buf.get_capacity() > reuse) {
		ByteBuf tail;
		tail.resize(p_len);
		memcpy(tail.ptr(), r_buf.ptr() + p_at, p_len);
		r_buf = std::move(tail);
		return;
	}
	memmove(r_buf.ptr(), r_buf.ptr() + p_at, p_len);
	r_buf.resize(p_len);
}

// Flush available output and retain the remainder for a later turn.
// Never wait for a full peer buffer, which would stall unrelated connections.
// Return false only for a broken connection, clearing its buffer on failure.
inline bool sock_flush(Wire &p_wire, ByteBuf &r_buf) {
	uint32_t at = 0;
	while (at < r_buf.size()) {
		int sent = 0;
		const Error err = p_wire.send(r_buf.ptr() + at, r_buf.size() - at, sent);
		if (err != OK) {
			r_buf.clear();
			p_wire.write_wait(false);
			return false; // The connection is broken.
		}
		at += (uint32_t)MAX(0, sent);
		if (sent <= 0) {
			break; // Socket capacity is exhausted; retain the remainder for later.
		}
	}
	if (at >= r_buf.size()) {
		// Reuse small output buffers and release large capacity after flushing.
		if (r_buf.get_capacity() > BUFFER_REUSE_MAX) {
			r_buf.reset();
		} else {
			r_buf.clear();
		}
		p_wire.write_wait(false);
		return true;
	}
	keep_tail(r_buf, at, r_buf.size() - at);
	p_wire.write_wait(true);
	return true;
}

// Decode received bytes as strict JSON.
inline Ref<R> json_of(const PackedByteArray &p_body) {
	return JsonData::decode(p_body);
}

// Preserve the transport cause, supplying a category only when no cause exists.
inline Ref<R> sock_error(const Wire &p_wire, const String &p_msg, Err::Kind p_kind = Err::INTERRUPTED) {
	return R::err(p_wire.error().is_valid() ? p_wire.error() : Err::make(p_msg, p_kind));
}

// Append up to the requested byte boundary, returning failures through R.
// Read directly into storage after removing consumed bytes.
// Use the same interface for plain and TLS-wrapped connections.
inline Ref<R> sock_fill(Wire &p_wire, PackedByteArray &r_buf, int &r_at, int p_read_max, int64_t p_buf_max) {
	if (!p_wire.is_valid()) {
		return sock_error(p_wire, "connection closed");
	}
	p_wire.poll();
	if (!p_wire.is_ready()) {
		return sock_error(p_wire, "connection lost");
	}
	const int n = MIN(p_wire.available(), MAX(0, p_read_max));
	if (n > 0) {
		// Enforce the caller's retained-input boundary before accepting additional bytes.
		if ((int64_t)r_buf.size() - r_at + n > p_buf_max) {
			return R::err("peer sent too much", Err::INVALID_DATA);
		}
		compact_recv(r_buf, r_at);
		const int at = r_buf.size();
		if (r_buf.resize(at + n) != OK) {
			return R::err("receive buffer allocation failed", Err::LIMITED);
		}
		// Undo reserved growth when no bytes were read.
		// Otherwise uninitialized buffer capacity would be parsed as received content.
		int got = 0;
		const Error error = p_wire.read(r_buf.ptrw() + at, n, got);
		r_buf.resize(at + got);
		if (error != OK && error != ERR_BUSY) {
			return sock_error(p_wire, "read failed");
		}
	}
	return R::ok();
}
