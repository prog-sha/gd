# Prove failed public checks produce a failing process exit in distribution builds.
extends RefCounted


# Emit an intentional failure independently of assert compilation settings.
func main() -> int:
	var ck := GD.test.check()
	ck.eq(1, 2, "intentional failure")
	print("release:check_failure:%d" % ck.code())
	return ck.code()
