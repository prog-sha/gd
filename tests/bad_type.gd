# Supply an intentionally invalid return contract for the compiler rejection check.
extends RefCounted


# Violate the declared result type without executing the function.
func main() -> int:
	return "invalid return type"
