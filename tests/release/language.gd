# Verify inferred values, typed collections, and fallible return contracts.
extends RefCounted


# Infer a successful result while retaining a typed failure.
func number(valid: bool):
	if not valid:
		return null, Err.err("missing", Err.NOT_FOUND)
	return 21, null


# Propagate failures without discarding the successful value type.
func doubled(valid: bool) -> int, Err:
	var value := number(valid)?
	return value * 2, null


# Check result decomposition and collection element types at execution time.
func main() -> int:
	var ck := GD.test.check()
	var value, failure := doubled(true)
	ck.eq(value, 42, "inferred arithmetic")
	ck.eq(failure, null, "successful result")
	var _missing, problem := doubled(false)
	ck.ok(problem is Err and problem.is(Err.NOT_FOUND), "error propagation")
	ck.fails(doubled(false), Err.NOT_FOUND, "failed result retains its error")
	var values: Array[int] = [value, 8]
	var indexed: Dictionary[String, int] = {"answer": values[0]}
	ck.eq(indexed["answer"], 42, "typed collections")
	var ready: R = R.ok(value)
	ck.eq(ready.v, 42, "ready result value")
	ck.eq(ready.e, null, "ready result error")
	print("release:language:%d" % ck.code())
	return ck.code()
