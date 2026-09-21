// Validate prime-curve keys and signatures with independent fixed-width field arithmetic.
#pragma once
#include "mod_core.h"

namespace GDCrypto {
// Share immutable curve parameters between agreement and signature verification operations.
class PrimeCurve {
	using Arithmetic = Mod<17>; // Seventeen words hold the largest supported 521-bit field.
	using Field = Arithmetic::Value;
	// Represent affine coordinates as Jacobian residues; a zero Z denotes infinity.
	struct Point { Field x{}, y{}, z{}; };
	Arithmetic field, order; // Coordinate field and prime subgroup order.
	Field coefficient{}; // Curve constant in radix form; the linear coefficient is minus three.
	Point base; // Standard generator in Jacobian form.
	std::array<uint8_t, 66> field_exp{}, order_exp{}; // Public inverse exponents for the two prime moduli.
	bool ready = false; // Initialization status for a recognized parameter set.
	Point twice(const Point &point) const; // Double a point without affine inversion.
	Point add(const Point &left, const Point &right) const; // Add points with masked exceptional-case handling.
	static Point choose(Point left, const Point &right, uint32_t bit); // Select coordinates without secret-dependent addresses.
	Point multiply(const Point &point, Bytes scalar) const; // Multiply with fixed work per encoded scalar bit.
	bool decode(Bytes encoded, Point &point) const; // Validate an uncompressed finite public point.
	bool affine(Point &point) const; // Normalize finite coordinates with a single inversion.
	bool scalar(Bytes input) const; // Require a fixed-width nonzero scalar below the group order.
	Field hash_value(Bytes digest) const; // Truncate a digest to the subgroup bit width and reduce it.
public:
	bool reset(unsigned bits); // Select the 224, 256, 384, or 521-bit prime curve.
	size_t size() const { return ready ? field.size() : 0; } // Return the fixed scalar and coordinate byte width.
	bool valid_public(Bytes peer) const { Point point; return ready && decode(peer, point); } // Check finite curve membership before retaining a parsed public key.
	bool public_key(Bytes secret, uint8_t *output, size_t size) const; // Produce an uncompressed key without exposing partial output on failure.
	bool shared(Bytes secret, Bytes peer, uint8_t *output, size_t size) const; // Compute the fixed-width shared X coordinate after validating both inputs.
	bool verify(Bytes peer, Bytes digest, Bytes signature) const; // Verify canonical integer-pair signatures over an already-computed digest.
	bool sign(Bytes secret, Bytes digest, uint8_t *output, size_t capacity, size_t &written) const; // Sign with hedged randomness and publish only a complete canonical signature.
};
}
