/**************************************************************************/
/*  math.h                                                                */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

#pragma once

// Expose floating-point mathematics through GD.math.

#include "core/object/object.h"

class GDBitsAPI : public Object {
	GDCLASS(GDBitsAPI, Object);

protected:
	// Expose integer bit operations to scripts.
	static void _bind_methods();

public:
	// Operate on the bit counts and layout of uint64 values.
	int64_t len64(int64_t p_x) const;
	int64_t leading_zeros64(int64_t p_x) const;
	int64_t trailing_zeros64(int64_t p_x) const;
	int64_t ones_count64(int64_t p_x) const;
	int64_t rotate_left64(int64_t p_x, int64_t p_k) const;
	int64_t reverse64(int64_t p_x) const;
	int64_t reverse_bytes64(int64_t p_x) const;
	// Return uint64 arithmetic with carry or borrow as two-element arrays.
	Array add64(int64_t p_x, int64_t p_y, int64_t p_carry) const;
	Array sub64(int64_t p_x, int64_t p_y, int64_t p_borrow) const;
	Array mul64(int64_t p_x, int64_t p_y) const;
};

class GDMathAPI : public Object {
	GDCLASS(GDMathAPI, Object);

	GDBitsAPI *bits = nullptr; // Child API for integer bit operations.

protected:
	// Expose mathematical operations to scripts.
	static void _bind_methods();

public:
	GDMathAPI();
	~GDMathAPI();
	// Return the integer-bit API.
	GDBitsAPI *get_bits() const { return bits; }
	// Return mathematical constants.
	double get_e() const { return 2.71828182845904523536; }
	double get_pi() const { return 3.14159265358979323846; }
	double get_phi() const { return 1.61803398874989484820; }
	double get_sqrt2() const { return 1.41421356237309504880; }
	double get_sqrt_e() const { return 1.64872127070012814685; }
	double get_sqrt_pi() const { return 1.77245385090551602730; }
	double get_sqrt_phi() const { return 1.27201964951406896425; }
	double get_ln2() const { return 0.69314718055994530942; }
	double get_log2_e() const { return 1.44269504088896340736; }
	double get_ln10() const { return 2.30258509299404568402; }
	double get_log10_e() const { return 0.43429448190325182765; }
	double get_max_float64() const;
	double get_smallest_nonzero_float64() const;

	// Provide unary mathematical functions.
	double abs(double p_x) const;
	double acos(double p_x) const;
	double acosh(double p_x) const;
	double asin(double p_x) const;
	double asinh(double p_x) const;
	double atan(double p_x) const;
	double atanh(double p_x) const;
	double cbrt(double p_x) const;
	double ceil(double p_x) const;
	double cos(double p_x) const;
	double cosh(double p_x) const;
	double erf(double p_x) const;
	double erfc(double p_x) const;
	double erfinv(double p_x) const;
	double erfcinv(double p_x) const;
	double exp(double p_x) const;
	double exp2(double p_x) const;
	double expm1(double p_x) const;
	double floor(double p_x) const;
	double gamma(double p_x) const;
	double j0(double p_x) const;
	double j1(double p_x) const;
	int64_t ilogb(double p_x) const;
	double log(double p_x) const;
	double log1p(double p_x) const;
	double log2(double p_x) const;
	double log10(double p_x) const;
	double logb(double p_x) const;
	double round(double p_x) const;
	double round_to_even(double p_x) const;
	bool signbit(double p_x) const;
	double sin(double p_x) const;
	double sinh(double p_x) const;
	double sqrt(double p_x) const;
	double tan(double p_x) const;
	double tanh(double p_x) const;
	double trunc(double p_x) const;
	double y0(double p_x) const;
	double y1(double p_x) const;
	double nan() const;
	double inf(int64_t p_sign) const;
	bool is_inf(double p_x, int64_t p_sign) const;
	bool is_nan(double p_x) const;

	// Provide functions using two or more values.
	double atan2(double p_y, double p_x) const;
	double copysign(double p_value, double p_sign) const;
	double dim(double p_x, double p_y) const;
	double fma(double p_x, double p_y, double p_z) const;
	double hypot(double p_x, double p_y) const;
	double ldexp(double p_frac, int64_t p_exp) const;
	double max(double p_x, double p_y) const;
	double min(double p_x, double p_y) const;
	double mod(double p_x, double p_y) const;
	double nextafter(double p_x, double p_y) const;
	double pow(double p_x, double p_y) const;
	double pow10(int64_t p_n) const;
	double remainder(double p_x, double p_y) const;

	// Return multiple results in declared order as an Array.
	Array frexp(double p_x) const;
	Array lgamma(double p_x) const;
	Array modf(double p_x) const;
	Array sincos(double p_x) const;

	// Convert between floating-point values and IEEE 754 integer bit patterns.
	int64_t float64_bits(double p_x) const;
	double float64_from_bits(int64_t p_bits) const;
	int64_t float32_bits(double p_x) const;
	double float32_from_bits(int64_t p_bits) const;
	double nextafter32(double p_x, double p_y) const;
};
