# Serve a greeting as an HTML page and a JSON response.
var app := GD.web.app()

# Build the shared response data from the requested name.
func greeting(req):
	var name := req.query.get("name", "world")
	if not name is String:
		return null, Err.err("name must be text", Err.INVALID_DATA)
	return {"name": name, "message": "Hello, " + name + "!"}, null

# Render the page with escaped template values.
func home(req):
	return GD.web.view("index.html", greeting(req)?)

# Return the same data as JSON.
func hello(req):
	return GD.web.json(greeting(req)?)

# Listen locally, using an optional port argument.
func main(args):
	var port := 8080 if args.is_empty() else int(args[0])
	app.route("GET", "/", home)
	app.route("GET", "/api/hello", hello)
	app.listen(port, "127.0.0.1")!
	print("http://127.0.0.1:", app.port())
	return 0
