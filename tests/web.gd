# Verify loopback HTTP request conversion and serving without a scene tree.
extends RefCounted


# Decode a body through the ordinary automatic-wait API.
func echo(req: GDWebRequest) -> GDWebResponse:
	var value, failure := req.json()
	return GD.web.text("invalid", 400) if failure != null else GD.web.json(value)!


# Retain the explicit asynchronous body entry point and its task contract.
func later(req: GDWebRequest) -> GDWebResponse:
	var pending: Variant = req.json_async()
	if not pending is GDTask:
		return GD.web.text("expected GDTask", 500)
	var task: GDTask = pending
	var value, failure := await task
	return GD.web.text("invalid", 400) if failure != null else GD.web.json(value)!


# Complete all loopback requests, stop the listener, and terminate the serving loop.
func main() -> int:
	var ck := GD.test.check()
	var loop := Engine.get_main_loop()
	ck.no(loop is SceneTree, "serve starts without a tree")
	if loop.has_method("has_scene_tree"):
		ck.no(bool(loop.call("has_scene_tree")), "no lazy tree at startup")
	var app := GD.web.app()
	app.route("POST", "/echo", echo)
	app.route("POST", "/async", later)
	var _opened, open_error := app.listen(0, "127.0.0.1")
	ck.succeeds(open_error, "listen on an assigned port")
	if open_error == null:
		var base := "http://127.0.0.1:%d" % app.port()
		for route: String in ["/echo", "/async"]:
			var reply, _reply_err := GD.http.fetch(base + route, {"method": "POST", "body": '{"text":"日本😀"}', "headers": {"Content-Type": "application/json"}})
			ck.ok((_reply_err.text() if _reply_err else "").is_empty(), "HTTP request completes")
			if (_reply_err.text() if _reply_err else "").is_empty():
				ck.eq(reply.status, 200, "JSON status")
				ck.eq(GD.data.json_decode(reply.body)!, {"text": "日本😀"}, "JSON body")
		var bad, _bad_err := GD.http.fetch(base + "/echo", {"method": "POST", "body": "{"})
		ck.ok((_bad_err.text() if _bad_err else "").is_empty(), "malformed JSON response")
		if (_bad_err.text() if _bad_err else "").is_empty():
			ck.eq(bad.status, 400, "malformed JSON status")
		var missing, _missing_err := GD.http.fetch(base + "/missing")
		ck.ok((_missing_err.text() if _missing_err else "").is_empty(), "missing route response")
		if (_missing_err.text() if _missing_err else "").is_empty():
			ck.eq(missing.status, 404, "missing route status")
	app.stop()
	if loop.has_method("has_scene_tree"):
		ck.no(bool(loop.call("has_scene_tree")), "serving did not create a tree")
	print("release:web:%d" % ck.code())
	loop.call("quit", ck.code())
	return ck.code()
