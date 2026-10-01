/**************************************************************************/
/*  options.h                                                             */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Read database options without invoking implicit Variant conversions.

#include "cli/sys/limit.h"
#include "core/variant/dictionary.h"

namespace DbOption {

// Read an integer setting only from an integer script value.
inline bool integer(const Dictionary &p_opts, const char *p_name, int64_t p_default, int64_t &r_value) {
	const Variant value = p_opts.get(p_name, p_default);
	if (value.get_type() != Variant::INT) return false;
	r_value = value;
	return true;
}

// Read a duration only from a finite numeric script value.
inline bool seconds(const Dictionary &p_opts, const char *p_name, double p_default, uint64_t &r_ms) {
	const Variant value = p_opts.get(p_name, p_default);
	if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) return false;
	return Limit::seconds_ms(double(value), r_ms);
}

// Read textual options only from a string or interned string.
inline bool text(const Dictionary &p_opts, const char *p_name, const String &p_default, String &r_value) {
	const Variant value = p_opts.get(p_name, p_default);
	if (value.get_type() != Variant::STRING && value.get_type() != Variant::STRING_NAME) return false;
	r_value = value;
	return true;
}

} // namespace DbOption
