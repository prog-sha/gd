# Verify explicit waits, asynchronous composition, and ordinary signal values.
extends RefCounted

signal pulse(value: int)


# Preserve a script-owned signal as an ordinary return value.
func plain() -> Signal:
	return pulse


# Emit a value from the next deferred turn.
func fire() -> void:
	pulse.emit(42)


# Exercise asynchronous contracts without depending on elapsed-time thresholds.
func main() -> int:
	var ck := GD.test.check()
	var seen: Array[int] = []
	var callback := func() -> void: seen.append(1)
	callback.call_deferred()
	var completed: Array = await GD.async.all([GD.async.sleep(0.001)])
	ck.eq(completed.size(), 1, "native signal returns completed values")
	ck.eq(seen, [1], "deferred callback completes while awaiting")
	ck.eq(plain.call(), pulse, "ordinary Signal remains a value")
	fire.call_deferred()
	var value: int = await pulse
	ck.eq(value, 42, "await receives signal value")
	var first: Signal = GD.async.sleep(0.001)
	var second: Signal = GD.async.sleep(0.002)
	var finished: Array = await GD.async.all([first, second])
	ck.eq(finished.size(), 2, "wait for both signals")
	var context := GD.async.context()
	context.cancel("complete")
	ck.ok(context.is_done(), "context cancellation")
	ck.ok(context.reason.is(Err.INTERRUPTED), "cancellation kind")
	print("release:async:%d" % ck.code())
	return ck.code()
