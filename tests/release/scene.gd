# Verify node startup, scene-tree access, and timer completion.
extends Node


# Require the attached tree and resume a scene timer before returning.
func main() -> int:
	var ck := GD.test.check()
	ck.ok(is_inside_tree(), "node entered tree")
	ck.ok(get_tree() is SceneTree, "tree is available")
	if get_tree() != null:
		await get_tree().create_timer(0.001).timeout
		ck.ok(is_inside_tree(), "node remains attached after await")
	print("release:scene:%d" % ck.code())
	return ck.code()
