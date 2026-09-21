/**************************************************************************/
/*  math.cpp                                                              */
/**************************************************************************/
/*                          Command-line runtime                         */
/**************************************************************************/

// Implement floating-point and bit operations declared in math.h.

#include "cli/api/math.h"
#include "cli/api/erf_table.h"

#include "core/object/class_db.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr double PI = 3.141592653589793238462643383279502884; // Circle constant pi.

// Evaluate eight coefficients in independent pairs to shorten dependent arithmetic chains.
double poly8(const double *p_c, double p_x, double p_x2, double p_x4) {
	const double a = (p_c[0] + p_c[1] * p_x) + (p_c[2] + p_c[3] * p_x) * p_x2;
	const double b = (p_c[4] + p_c[5] * p_x) + (p_c[6] + p_c[7] * p_x) * p_x2;
	return a + b * p_x4;
}

// Evaluate the inverse with polynomial interpolation on exact binary input intervals.
double inverse_erf(double p_x) {
	const double x = std::fabs(p_x);
	if (!(x < 1.0)) {
		return x == 1.0 ? std::copysign(std::numeric_limits<double>::infinity(), p_x) : std::numeric_limits<double>::quiet_NaN();
	}
	if (x < std::sqrt(std::numeric_limits<double>::epsilon())) {
		return p_x * ErfTable::SLOPE;
	}
	if (x < 0.875) {
		const unsigned row = unsigned(x * 16.0);
		const double z = x - ErfTable::CENTER_AT[row];
		const double z2 = z * z, z4 = z2 * z2, z8 = z4 * z4;
		const double *c = ErfTable::CENTER[row];
		return p_x * (poly8(c, z, z2, z4) + z8 * poly8(c + 8, z, z2, z4));
	}
	// Subtraction is exact here; the complement is a normal number between 2^-53 and 1/8.
	const double q = 1.0 - x;
	uint64_t bits;
	memcpy(&bits, &q, sizeof(bits));
	const unsigned row = 1020 - unsigned((bits >> 52) & 0x7ff);
	bits = (bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
	double mantissa;
	memcpy(&mantissa, &bits, sizeof(mantissa));
	const double z = mantissa - 1.5, z2 = z * z, z4 = z2 * z2, z8 = z4 * z4;
	const double *c = ErfTable::TAIL[row];
	const double y = poly8(c, z, z2, z4) + z8 * (poly8(c + 8, z, z2, z4) + z8 * poly8(c + 16, z, z2, z4));
	return std::copysign(y, p_x);
}

// Pack two values in declared order.
Array pair_of(const Variant &p_a, const Variant &p_b) {
	Array out;
	out.resize(2);
	out[0] = p_a;
	out[1] = p_b;
	return out;
}

} // namespace

// Create the integer-bit API.
GDMathAPI::GDMathAPI() {
	bits = memnew(GDBitsAPI);
}

// Release the integer-bit API.
GDMathAPI::~GDMathAPI() {
	memdelete(bits);
}

// Return significant-bit count for a uint64 represented by int64.
int64_t GDBitsAPI::len64(int64_t p_x) const {
	uint64_t x = (uint64_t)p_x;
	int64_t n = 0;
	while (x) {
		x >>= 1;
		n++;
	}
	return n;
}

// Count leading zero bits.
int64_t GDBitsAPI::leading_zeros64(int64_t p_x) const { return 64 - len64(p_x); }

// Count trailing zero bits.
int64_t GDBitsAPI::trailing_zeros64(int64_t p_x) const {
	uint64_t x = (uint64_t)p_x;
	if (x == 0) {
		return 64;
	}
	int64_t n = 0;
	while ((x & 1) == 0) {
		x >>= 1;
		n++;
	}
	return n;
}

// Count set bits.
int64_t GDBitsAPI::ones_count64(int64_t p_x) const {
	uint64_t x = (uint64_t)p_x;
	int64_t n = 0;
	while (x) {
		x &= x - 1;
		n++;
	}
	return n;
}

// Rotate left within 64 bits, accepting negative shifts.
int64_t GDBitsAPI::rotate_left64(int64_t p_x, int64_t p_k) const {
	const uint64_t x = (uint64_t)p_x;
	const unsigned int k = (unsigned int)((uint64_t)p_k & 63);
	return (int64_t)(k == 0 ? x : (x << k) | (x >> (64 - k)));
}

// Reverse all 64 bits.
int64_t GDBitsAPI::reverse64(int64_t p_x) const {
	uint64_t x = (uint64_t)p_x;
	x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
	x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
	x = ((x >> 4) & 0x0f0f0f0f0f0f0f0fULL) | ((x & 0x0f0f0f0f0f0f0f0fULL) << 4);
	return reverse_bytes64((int64_t)x);
}

// Reverse the order of eight bytes.
int64_t GDBitsAPI::reverse_bytes64(int64_t p_x) const {
	uint64_t x = (uint64_t)p_x;
	x = ((x >> 8) & 0x00ff00ff00ff00ffULL) | ((x & 0x00ff00ff00ff00ffULL) << 8);
	x = ((x >> 16) & 0x0000ffff0000ffffULL) | ((x & 0x0000ffff0000ffffULL) << 16);
	return (int64_t)((x >> 32) | (x << 32));
}

// Add uint64 values and return carry.
Array GDBitsAPI::add64(int64_t p_x, int64_t p_y, int64_t p_carry) const {
	const uint64_t x = (uint64_t)p_x;
	const uint64_t y = (uint64_t)p_y;
	const uint64_t first = x + y;
	const uint64_t sum = first + ((uint64_t)p_carry & 1);
	return pair_of((int64_t)sum, int64_t(first < x || sum < first));
}

// Subtract uint64 values and return borrow.
Array GDBitsAPI::sub64(int64_t p_x, int64_t p_y, int64_t p_borrow) const {
	const uint64_t x = (uint64_t)p_x;
	const uint64_t y = (uint64_t)p_y;
	const uint64_t first = x - y;
	const uint64_t diff = first - ((uint64_t)p_borrow & 1);
	return pair_of((int64_t)diff, int64_t(x < y || first < diff));
}

// Return the high and low halves of a uint64 product.
Array GDBitsAPI::mul64(int64_t p_x, int64_t p_y) const {
	const uint64_t x = (uint64_t)p_x;
	const uint64_t y = (uint64_t)p_y;
	const uint64_t xh = x >> 32;
	const uint64_t yh = y >> 32;
	uint64_t t = uint64_t(uint32_t(x)) * uint32_t(y);
	const uint64_t low = t & 0xffffffffULL;
	uint64_t carry = t >> 32;
	t = xh * uint32_t(y) + carry;
	const uint64_t middle = t & 0xffffffffULL;
	const uint64_t high = t >> 32;
	t = uint64_t(uint32_t(x)) * yh + middle;
	return pair_of((int64_t)(xh * yh + high + (t >> 32)), (int64_t)((t << 32) + low));
}

// Register integer-bit methods.
void GDBitsAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("len64", "x"), &GDBitsAPI::len64);
	ClassDB::bind_method(D_METHOD("leading_zeros64", "x"), &GDBitsAPI::leading_zeros64);
	ClassDB::bind_method(D_METHOD("trailing_zeros64", "x"), &GDBitsAPI::trailing_zeros64);
	ClassDB::bind_method(D_METHOD("ones_count64", "x"), &GDBitsAPI::ones_count64);
	ClassDB::bind_method(D_METHOD("rotate_left64", "x", "k"), &GDBitsAPI::rotate_left64);
	ClassDB::bind_method(D_METHOD("reverse64", "x"), &GDBitsAPI::reverse64);
	ClassDB::bind_method(D_METHOD("reverse_bytes64", "x"), &GDBitsAPI::reverse_bytes64);
	ClassDB::bind_method(D_METHOD("add64", "x", "y", "carry"), &GDBitsAPI::add64);
	ClassDB::bind_method(D_METHOD("sub64", "x", "y", "borrow"), &GDBitsAPI::sub64);
	ClassDB::bind_method(D_METHOD("mul64", "x", "y"), &GDBitsAPI::mul64);
}

// Return the largest finite double.
double GDMathAPI::get_max_float64() const { return std::numeric_limits<double>::max(); }
// Return the smallest nonzero double.
double GDMathAPI::get_smallest_nonzero_float64() const { return std::numeric_limits<double>::denorm_min(); }

// Return absolute value.
double GDMathAPI::abs(double p_x) const { return std::fabs(p_x); }
// Return inverse cosine.
double GDMathAPI::acos(double p_x) const { return std::acos(p_x); }
// Return inverse hyperbolic cosine.
double GDMathAPI::acosh(double p_x) const { return std::acosh(p_x); }
// Return inverse sine.
double GDMathAPI::asin(double p_x) const { return std::asin(p_x); }
// Return inverse hyperbolic sine.
double GDMathAPI::asinh(double p_x) const { return std::asinh(p_x); }
// Return inverse tangent.
double GDMathAPI::atan(double p_x) const { return std::atan(p_x); }
// Return inverse hyperbolic tangent.
double GDMathAPI::atanh(double p_x) const { return std::atanh(p_x); }
// Return cube root.
double GDMathAPI::cbrt(double p_x) const { return std::cbrt(p_x); }
// Round toward positive infinity.
double GDMathAPI::ceil(double p_x) const { return std::ceil(p_x); }
// Return cosine.
double GDMathAPI::cos(double p_x) const { return std::cos(p_x); }
// Return hyperbolic cosine.
double GDMathAPI::cosh(double p_x) const { return std::cosh(p_x); }
// Return the error function.
double GDMathAPI::erf(double p_x) const { return std::erf(p_x); }
// Return the complementary error function.
double GDMathAPI::erfc(double p_x) const { return std::erfc(p_x); }
// Return inverse error function.
double GDMathAPI::erfinv(double p_x) const { return inverse_erf(p_x); }
// Return inverse complementary error function.
double GDMathAPI::erfcinv(double p_x) const { return inverse_erf(1.0 - p_x); }
// Return the natural exponential.
double GDMathAPI::exp(double p_x) const { return std::exp(p_x); }
// Return the base-two exponential.
double GDMathAPI::exp2(double p_x) const { return std::exp2(p_x); }
// Return the natural exponential minus one.
double GDMathAPI::expm1(double p_x) const { return std::expm1(p_x); }
// Round toward negative infinity.
double GDMathAPI::floor(double p_x) const { return std::floor(p_x); }
// Return the Gamma function.
double GDMathAPI::gamma(double p_x) const { return std::tgamma(p_x); }

// Normalize platform C-math Bessel functions to the public contract.
double GDMathAPI::j0(double p_x) const {
#ifdef WINDOWS_ENABLED
	return ::_j0(p_x);
#else
	return ::j0(p_x);
#endif
}
// Return the first-kind Bessel function of order one.
double GDMathAPI::j1(double p_x) const {
#ifdef WINDOWS_ENABLED
	return ::_j1(p_x);
#else
	return ::j1(p_x);
#endif
}
// Return the binary exponent as an integer.
int64_t GDMathAPI::ilogb(double p_x) const {
	if (p_x == 0.0) {
		return std::numeric_limits<int32_t>::min();
	}
	if (std::isnan(p_x) || std::isinf(p_x)) {
		return std::numeric_limits<int32_t>::max();
	}
	return std::ilogb(p_x);
}
// Return the natural logarithm.
double GDMathAPI::log(double p_x) const { return std::log(p_x); }
// Return the natural logarithm of one plus the input.
double GDMathAPI::log1p(double p_x) const { return std::log1p(p_x); }
// Return the base-two logarithm.
double GDMathAPI::log2(double p_x) const { return std::log2(p_x); }
// Return the base-ten logarithm.
double GDMathAPI::log10(double p_x) const { return std::log10(p_x); }
// Return the binary exponent as a floating-point value.
double GDMathAPI::logb(double p_x) const { return std::logb(p_x); }
// Round to the nearest integer with ties away from zero.
double GDMathAPI::round(double p_x) const { return std::round(p_x); }

// Round ties to even, preserving signed zero and large integral values.
double GDMathAPI::round_to_even(double p_x) const {
	if (!std::isfinite(p_x) || p_x == 0.0 || std::fabs(p_x) >= 4503599627370496.0) {
		return p_x;
	}
	const double a = std::fabs(p_x);
	double whole = std::floor(a);
	const double frac = a - whole;
	if (frac > 0.5 || (frac == 0.5 && std::fmod(whole, 2.0) != 0.0)) {
		whole += 1.0;
	}
	return std::copysign(whole, p_x);
}

// Report whether the sign bit is set.
bool GDMathAPI::signbit(double p_x) const { return std::signbit(p_x); }
// Return sine.
double GDMathAPI::sin(double p_x) const { return std::sin(p_x); }
// Return hyperbolic sine.
double GDMathAPI::sinh(double p_x) const { return std::sinh(p_x); }
// Return square root.
double GDMathAPI::sqrt(double p_x) const { return std::sqrt(p_x); }
// Return tangent.
double GDMathAPI::tan(double p_x) const { return std::tan(p_x); }
// Return hyperbolic tangent.
double GDMathAPI::tanh(double p_x) const { return std::tanh(p_x); }
// Truncate the fractional part toward zero.
double GDMathAPI::trunc(double p_x) const { return std::trunc(p_x); }
// Return the second-kind Bessel function of order zero.
double GDMathAPI::y0(double p_x) const {
#ifdef WINDOWS_ENABLED
	return ::_y0(p_x);
#else
	return ::y0(p_x);
#endif
}
// Return the second-kind Bessel function of order one.
double GDMathAPI::y1(double p_x) const {
#ifdef WINDOWS_ENABLED
	return ::_y1(p_x);
#else
	return ::y1(p_x);
#endif
}
// Return NaN.
double GDMathAPI::nan() const { return std::numeric_limits<double>::quiet_NaN(); }
// Return infinity with the selected sign.
double GDMathAPI::inf(int64_t p_sign) const { return std::copysign(std::numeric_limits<double>::infinity(), p_sign < 0 ? -1.0 : 1.0); }
// Check for infinity with the selected sign.
bool GDMathAPI::is_inf(double p_x, int64_t p_sign) const { return std::isinf(p_x) && (p_sign == 0 || (p_sign > 0) == !std::signbit(p_x)); }
// Check for NaN.
bool GDMathAPI::is_nan(double p_x) const { return std::isnan(p_x); }
// Return the angle for the supplied coordinates.
double GDMathAPI::atan2(double p_y, double p_x) const { return std::atan2(p_y, p_x); }
// Combine one value's magnitude with another's sign.
double GDMathAPI::copysign(double p_value, double p_sign) const { return std::copysign(p_value, p_sign); }
// Return the positive difference of two values.
double GDMathAPI::dim(double p_x, double p_y) const { return std::fdim(p_x, p_y); }
// Return a fused multiply-add with one rounding.
double GDMathAPI::fma(double p_x, double p_y, double p_z) const { return std::fma(p_x, p_y, p_z); }
// Return the hypotenuse length.
double GDMathAPI::hypot(double p_x, double p_y) const { return std::hypot(p_x, p_y); }
// Scale a fraction by a power of two.
double GDMathAPI::ldexp(double p_frac, int64_t p_exp) const {
	const int exp = p_exp > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() :
			p_exp < std::numeric_limits<int>::min() ? std::numeric_limits<int>::min() : (int)p_exp;
	return std::ldexp(p_frac, exp);
}

// Select the maximum with explicit NaN and signed-zero handling.
double GDMathAPI::max(double p_x, double p_y) const {
	if (std::isnan(p_x) || std::isnan(p_y)) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	if (p_x == 0.0 && p_y == 0.0) {
		return std::signbit(p_x) && std::signbit(p_y) ? -0.0 : 0.0;
	}
	return p_x > p_y ? p_x : p_y;
}

// Select the minimum with explicit NaN and signed-zero handling.
double GDMathAPI::min(double p_x, double p_y) const {
	if (std::isnan(p_x) || std::isnan(p_y)) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	if (p_x == 0.0 && p_y == 0.0) {
		return std::signbit(p_x) || std::signbit(p_y) ? -0.0 : 0.0;
	}
	return p_x < p_y ? p_x : p_y;
}

// Return the remainder after truncated division.
double GDMathAPI::mod(double p_x, double p_y) const { return std::fmod(p_x, p_y); }
// Return the adjacent representable value toward the target.
double GDMathAPI::nextafter(double p_x, double p_y) const { return std::nextafter(p_x, p_y); }
// Return a power.
double GDMathAPI::pow(double p_x, double p_y) const { return std::pow(p_x, p_y); }
// Return ten raised to an integral power.
double GDMathAPI::pow10(int64_t p_n) const { return std::pow(10.0, (double)p_n); }
// Return the remainder using the nearest integral quotient.
double GDMathAPI::remainder(double p_x, double p_y) const { return std::remainder(p_x, p_y); }

// Split a value into fraction and binary exponent.
Array GDMathAPI::frexp(double p_x) const {
	int exp = 0;
	const double fraction = std::frexp(p_x, &exp);
	return pair_of(fraction, exp);
}

// Return log absolute Gamma and its sign.
Array GDMathAPI::lgamma(double p_x) const {
	if (std::isnan(p_x) || std::isinf(p_x)) {
		return pair_of(p_x, 1);
	}
	if (p_x < 0.0 && std::trunc(p_x) != p_x) {
		const double x = -p_x;
		// The pole term dominates within one machine epsilon of zero.
		if (x < std::numeric_limits<double>::epsilon()) {
			return pair_of(-std::log(x), -1);
		}
		const double whole = std::floor(x);
		const int sign = (uint64_t(whole) & 1) == 0 ? -1 : 1;
		// Nonintegral finite inputs fit within the exact integer range of the representation.
		const double fraction = x - whole;
		const double distance = fraction <= 0.5 ? fraction : 1.0 - fraction;
		const double value = std::log(PI) - std::log(std::sin(PI * distance)) - std::log(x) - std::lgamma(x);
		return pair_of(value, sign);
	}
	return pair_of(std::lgamma(p_x), 1);
}

// Split a value into integral and fractional parts.
Array GDMathAPI::modf(double p_x) const {
	double whole = 0.0;
	const double frac = std::modf(p_x, &whole);
	return pair_of(whole, frac);
}

// Return sine and cosine together.
Array GDMathAPI::sincos(double p_x) const { return pair_of(std::sin(p_x), std::cos(p_x)); }

// Return a double's integer bit pattern.
int64_t GDMathAPI::float64_bits(double p_x) const {
	uint64_t bits = 0;
	memcpy(&bits, &p_x, sizeof(bits));
	return (int64_t)bits;
}

// Construct a double from its bit pattern.
double GDMathAPI::float64_from_bits(int64_t p_bits) const {
	double out = 0.0;
	const uint64_t bits = (uint64_t)p_bits;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

// Return the bit pattern after rounding to single precision.
int64_t GDMathAPI::float32_bits(double p_x) const {
	const float value = (float)p_x;
	uint32_t bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

// Construct a single-precision value from its bit pattern.
double GDMathAPI::float32_from_bits(int64_t p_bits) const {
	const uint32_t bits = (uint32_t)p_bits;
	float out = 0.0f;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

// Return the adjacent single-precision value toward the target.
double GDMathAPI::nextafter32(double p_x, double p_y) const { return std::nextafter((float)p_x, (float)p_y); }

// Register mathematical functions and constants.
void GDMathAPI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_bits"), &GDMathAPI::get_bits);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bits", PROPERTY_HINT_RESOURCE_TYPE, "GDBitsAPI"), "", "get_bits");
#define MATH_ONE(m_name) ClassDB::bind_method(D_METHOD(#m_name, "x"), &GDMathAPI::m_name)
	MATH_ONE(abs); MATH_ONE(acos); MATH_ONE(acosh); MATH_ONE(asin); MATH_ONE(asinh); MATH_ONE(atan); MATH_ONE(atanh);
	MATH_ONE(cbrt); MATH_ONE(ceil); MATH_ONE(cos); MATH_ONE(cosh); MATH_ONE(erf); MATH_ONE(erfc); MATH_ONE(erfinv); MATH_ONE(erfcinv);
	MATH_ONE(exp); MATH_ONE(exp2); MATH_ONE(expm1); MATH_ONE(floor); MATH_ONE(gamma); MATH_ONE(ilogb); MATH_ONE(j0); MATH_ONE(j1);
	MATH_ONE(log); MATH_ONE(log1p); MATH_ONE(log2); MATH_ONE(log10); MATH_ONE(logb); MATH_ONE(round); MATH_ONE(round_to_even);
	MATH_ONE(signbit); MATH_ONE(sin); MATH_ONE(sinh); MATH_ONE(sqrt); MATH_ONE(tan); MATH_ONE(tanh); MATH_ONE(trunc); MATH_ONE(y0); MATH_ONE(y1);
	MATH_ONE(float64_bits); MATH_ONE(float64_from_bits); MATH_ONE(float32_bits); MATH_ONE(float32_from_bits);
#undef MATH_ONE
	ClassDB::bind_method(D_METHOD("nan"), &GDMathAPI::nan);
	ClassDB::bind_method(D_METHOD("inf", "sign"), &GDMathAPI::inf);
	ClassDB::bind_method(D_METHOD("is_inf", "x", "sign"), &GDMathAPI::is_inf, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("is_nan", "x"), &GDMathAPI::is_nan);
	ClassDB::bind_method(D_METHOD("atan2", "y", "x"), &GDMathAPI::atan2);
	ClassDB::bind_method(D_METHOD("copysign", "value", "sign"), &GDMathAPI::copysign);
	ClassDB::bind_method(D_METHOD("dim", "x", "y"), &GDMathAPI::dim);
	ClassDB::bind_method(D_METHOD("fma", "x", "y", "z"), &GDMathAPI::fma);
	ClassDB::bind_method(D_METHOD("hypot", "x", "y"), &GDMathAPI::hypot);
	ClassDB::bind_method(D_METHOD("ldexp", "frac", "exp"), &GDMathAPI::ldexp);
	ClassDB::bind_method(D_METHOD("max", "x", "y"), &GDMathAPI::max);
	ClassDB::bind_method(D_METHOD("min", "x", "y"), &GDMathAPI::min);
	ClassDB::bind_method(D_METHOD("mod", "x", "y"), &GDMathAPI::mod);
	ClassDB::bind_method(D_METHOD("nextafter", "x", "y"), &GDMathAPI::nextafter);
	ClassDB::bind_method(D_METHOD("nextafter32", "x", "y"), &GDMathAPI::nextafter32);
	ClassDB::bind_method(D_METHOD("pow", "x", "y"), &GDMathAPI::pow);
	ClassDB::bind_method(D_METHOD("pow10", "n"), &GDMathAPI::pow10);
	ClassDB::bind_method(D_METHOD("remainder", "x", "y"), &GDMathAPI::remainder);
	ClassDB::bind_method(D_METHOD("frexp", "x"), &GDMathAPI::frexp);
	ClassDB::bind_method(D_METHOD("lgamma", "x"), &GDMathAPI::lgamma);
	ClassDB::bind_method(D_METHOD("modf", "x"), &GDMathAPI::modf);
	ClassDB::bind_method(D_METHOD("sincos", "x"), &GDMathAPI::sincos);
#define MATH_CONST(m_name) \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &GDMathAPI::get_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, #m_name), "", "get_" #m_name)
	MATH_CONST(e); MATH_CONST(pi); MATH_CONST(phi); MATH_CONST(sqrt2); MATH_CONST(sqrt_e); MATH_CONST(sqrt_pi); MATH_CONST(sqrt_phi);
	MATH_CONST(ln2); MATH_CONST(log2_e); MATH_CONST(ln10); MATH_CONST(log10_e); MATH_CONST(max_float64); MATH_CONST(smallest_nonzero_float64);
#undef MATH_CONST
}
