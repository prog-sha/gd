# Run a matchmaking server configured by MATCH_* environment variables.
var server: GDOnlineMatch # Endpoint kept alive while serving.

const UNITS := {"ns": 1e-9, "us": 1e-6, "µs": 1e-6, "ms": 1e-3, "s": 1.0, "m": 60.0, "h": 3600.0} # Seconds per duration unit.


# Read a variable, treating an empty value as unset.
func env(name: String, fallback: String) -> String:
	var value: String = GD.cli.env(name, "")
	return fallback if value == "" else value


# Split "host:port"; an empty host means every interface.
func address(text: String) -> Array:
	var at := text.rfind(":")
	return [text.substr(0, at).trim_prefix("[").trim_suffix("]"), int(text.substr(at + 1))]


# Parse durations such as "5s", "1500ms", or "1m30s"; malformed or non-positive values give 5 seconds.
func seconds(text: String) -> float:
	var total := 0.0
	var rest := text
	while rest != "":
		var digits := 0
		while digits < rest.length() and (rest[digits].is_valid_int() or rest[digits] == "."):
			digits += 1
		var unit := ""
		for name: String in UNITS:
			if rest.substr(digits).begins_with(name) and name.length() > unit.length():
				unit = name
		if digits == 0 or unit == "" or not rest.substr(0, digits).is_valid_float():
			return 5.0
		total += rest.substr(0, digits).to_float() * float(UNITS[unit])
		rest = rest.substr(digits + unit.length())
	return total if total > 0.0 else 5.0


# Connect the optional shared store, then serve until stopped.
func main() -> int:
	var opts := {"key": env("MATCH_KEY", ""), "prefix": env("MATCH_PREFIX", "match"), "alive": seconds(env("MATCH_ALIVE", "5s"))}
	var redis := env("MATCH_REDIS", "")
	if redis != "":
		var at := address(redis)
		var db := GD.database.redis.client()
		db.open(at[0], at[1], {"password": env("MATCH_REDIS_PASS", ""), "timeout": 2.0})!
		opts["redis"] = db
	if opts["key"] == "":
		print("MATCH_KEY is empty. Announcements are refused")
	server = GD.online.match(opts)!
	var addr := address(env("MATCH_ADDR", ":8080"))
	var host: String = addr[0] if addr[0] != "" else "0.0.0.0"
	server.listen(addr[1], host)!
	print("matchmaking on %s:%d" % [host, server.port()])
	return 0
