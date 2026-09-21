# Verify data conversion, malformed-input rejection, and numerical boundary behavior.
extends RefCounted


# Compare public conversion outputs against fixed values and round trips.
func main() -> int:
	var ck := GD.test.check()
	var source := {"text": "Aé日本😀\n\"\\", "items": [1, true, null]}
	var encoded: R = GD.data.json_encode(source)
	ck.succeeds(encoded, "encode JSON")
	if encoded.ok:
		ck.eq(GD.data.json_decode(encoded.v).v, source, "Unicode JSON round trip")
	var rows: Array = []
	for i: int in range(32):
		rows.append({"id": i, "name": "日本"})
	rows.append({"name": "changed", "id": 32, "other": true})
	var repeated: R = GD.data.json_encode(rows)
	ck.succeeds(repeated, "repeated object keys")
	if repeated.ok:
		ck.eq(GD.data.json_decode(repeated.v).v, rows, "changed object shape")
	for text: String in ['{"a":1,"a":2}', '[1,]', '"\\ud800"', '1e999']:
		ck.fails(GD.data.json_decode(text.to_utf8_buffer()), Err.INVALID_DATA, "reject malformed JSON")
	ck.fails(GD.data.json_decode(PackedByteArray([34, 192, 128, 34])), Err.INVALID_DATA, "reject overlong UTF-8")
	var zero: R = GD.data.json_decode('"a\\u0000b"'.to_utf8_buffer())
	ck.succeeds(zero, "embedded zero")
	if zero.ok:
		ck.eq(zero.v.length(), 3, "zero does not truncate")
		ck.eq(zero.v.unicode_at(1), 0, "zero code point")
	var bytes := PackedByteArray([0, 1, 127, 128, 255])
	ck.eq(GD.data.hex_encode(bytes), "00017f80ff", "hex output")
	ck.eq(GD.data.hex_decode("00017F80FF").v, bytes, "hex input")
	ck.fails(GD.data.hex_decode("0x"), Err.INVALID_DATA, "bad hex")
	ck.eq(GD.data.base64_decode(GD.data.base64_encode(bytes)).v, bytes, "base64 round trip")
	ck.fails(GD.data.base64_decode("!"), Err.INVALID_DATA, "bad base64")
	ck.eq(GD.data.hex_encode(GD.data.sha256("abc".to_utf8_buffer())), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 vector")
	# Check numerical inverse, endpoint, and pole behavior.
	for i: int in range(-63, 64):
		var x := float(i) / 64.0
		ck.ok(GD.math.abs(GD.math.erf(GD.math.erfinv(x)) - x) <= 4e-16, "inverse error round trip")
	var tail := GD.math.nextafter(1.0, 0.0)
	ck.ok(GD.math.abs(GD.math.erfinv(tail) - 5.8635847487551676) <= 2e-15, "inverse error tail")
	var negative_zero := GD.math.copysign(0.0, -1.0)
	ck.ok(GD.math.signbit(GD.math.erfinv(negative_zero)), "inverse preserves negative zero")
	ck.ok(GD.math.erfinv(GD.math.smallest_nonzero_float64) > 0.0, "inverse preserves subnormal input")
	ck.ok(GD.math.is_nan(GD.math.erfinv(1.01)), "inverse rejects outside domain")
	ck.ok(GD.math.is_inf(GD.math.erfinv(-1.0), -1), "inverse negative endpoint")
	for i: int in range(8):
		var log_gamma: Array = GD.math.lgamma(-float(i) - 0.75)
		ck.eq(log_gamma[1], -1 if i % 2 == 0 else 1, "Gamma reflection sign")
	var tiny_gamma: Array = GD.math.lgamma(-GD.math.smallest_nonzero_float64)
	ck.eq(tiny_gamma[1], -1, "Gamma subnormal sign")
	ck.ok(not GD.math.is_inf(tiny_gamma[0]) and not GD.math.is_nan(tiny_gamma[0]), "Gamma subnormal logarithm")
	print("release:data:%d" % ck.code())
	return ck.code()
