/**************************************************************************/
/*  json.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Convert JSON bytes and values using strict, symmetric rules.
// Reject invalid UTF-8, duplicate object names, unsupported types, and cycles instead of ambiguous conversion.

#include "cli/sys/std.h"

#include <functional>

class JsonData {
public:
	// Encode a value as UTF-8 JSON bytes.
	static Ref<R> encode(const Variant &p_value, const Dictionary &p_opts = Dictionary());
	// Encode within the current time slice, handing unfinished state to a CPU worker.
	static Variant encode_reply(const Variant &p_value, uint64_t p_until, std::function<Variant(const Variant &)> p_finish = {});
	// Decode strict JSON bytes and distinguish values from failure.
	static Ref<R> decode(const PackedByteArray &p_src);
	// Decode within the current turn and transfer unfinished owned state to a CPU worker.
	static Variant decode_reply(const PackedByteArray &p_src, uint64_t p_until);
};
