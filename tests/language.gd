# Verify inferred values, typed collections, and fallible return contracts.
extends RefCounted


# Infer a successful result while retaining a typed failure.
func number(valid: bool) -> int, Err:
	if not valid:
		return 0, Err("missing", Err.NOT_FOUND)
	return 21


# Propagate failures without discarding the successful value type.
func doubled(valid: bool) -> int, Err:
	var value := number(valid)?
	return value * 2


# Check result decomposition and collection element types at execution time.
func main() -> int:
	var ck := GD.test.check()
	var value, failure := doubled(true)
	ck.eq(value, 42, "inferred arithmetic")
	ck.eq(failure, null, "successful result")
	var _missing, problem := doubled(false)
	ck.ok(problem is Err and (problem.find(Err.NOT_FOUND) != null), "error propagation")
	var _failed, failed_error := doubled(false)
	ck.fails(failed_error, Err.NOT_FOUND, "failed result retains its error")
	var values: Array[int] = [value, 8]
	var indexed: Dictionary[String, int] = {"answer": values[0]}
	ck.eq(indexed["answer"], 42, "typed collections")
	var ready, ready_error := number(true)
	ck.eq(ready, 21, "ready result value")
	ck.eq(ready_error, null, "ready result error")
	print("release:language:%d" % ck.code())
	return ck.code()
