# Read asset channels through the versioned catalog API.
# One host is the public catalog. The other is the reviewed catalog.
# Callers keep the host they asked for; this client does not switch hosts after an error.
extends RefCounted

const OFFICIAL: String = "https://store.godotengine.org/api/v1" # Public catalog channel.
const HOME: String = "https://gd.progsha.com/asset/v1" # Reviewed catalog channel.


# Return the default catalogs in browser order. The public catalog stays first.
func channels() -> Dictionary:
	var listed := {}
	listed["godotengine.org (Official)"] = OFFICIAL
	listed["gd"] = HOME
	return listed


# Resolve a channel name to its base URL.
func channel(name: String) -> String:
	if name == "official" or name.is_empty():
		return OFFICIAL
	if name == "gd" or name == "home":
		return HOME
	return name.trim_suffix("/")


# Report the first missing field in a search hit.
func missing_hit(card: Variant) -> String:
	if not card is Dictionary:
		return "asset"
	var doc: Dictionary = card
	for key: String in ["name", "slug", "store_url", "license_type", "license_url", "reviews_score", "publisher"]:
		if not doc.has(key):
			return key
	var publisher: Variant = doc["publisher"]
	if not publisher is Dictionary:
		return "publisher.name"
	var who: Dictionary = publisher
	if not who.has("name"):
		return "publisher.name"
	return ""


# Report the first missing field in an asset detail document.
func missing_detail(doc: Variant) -> String:
	var hit := missing_hit(doc)
	if not hit.is_empty():
		return hit
	var fields: Dictionary = doc
	for key: String in ["body_bbcode", "source"]:
		if not fields.has(key):
			return key
	var publisher: Dictionary = fields["publisher"]
	if not publisher.has("slug") or not publisher.has("verified"):
		return "publisher"
	return ""


# Report the first missing field in one download record.
func missing_release(doc: Variant) -> String:
	if not doc is Dictionary:
		return "release"
	var rel: Dictionary = doc
	for key: String in ["download_url", "version", "stable", "min_godot_version", "max_godot_version", "changes_bbcode"]:
		if not rel.has(key):
			return key
	return ""


# Join a channel base and a path that already includes its leading slash.
func url(host: String, path: String) -> String:
	return host.trim_suffix("/") + path


# Decode a JSON document from one channel request.
func fetch_json(target: String) -> Variant, Err:
	var response, failure := GD.http.fetch(target, {"timeout": 20.0})
	if failure != null:
		return null, failure.note("fetch asset catalog")
	if int(response.status) != 200:
		return null, Err("status %d" % int(response.status), Err.INVALID_DATA)
	return GD.data.json_decode(response.body)


# Search one channel and require the hit fields the browser reads.
func search(host: String, text: String, compat: String = "", kind: int = 0, sort: String = "relevance", page: int = 1, batch: int = 24) -> Dictionary, Err:
	var params := {
		"query": text,
		"require_release": "true",
		"type": str(kind),
		"sort": sort,
		"page": str(page),
	}
	if not compat.is_empty():
		params["compatibility"] = compat
	if batch != 24:
		params["batch_size"] = str(batch)
	var encoded: String = GD.http.encode_query(params)
	var decoded, decode_error := fetch_json(url(host, "/search/query/?" + encoded))
	if decode_error != null:
		return {}, decode_error
	if not decoded is Dictionary:
		return {}, Err("search document", Err.INVALID_DATA)
	var doc: Dictionary = decoded
	if not doc.has("count") or not doc.has("hits") or not doc["hits"] is Array:
		return {}, Err("search document", Err.INVALID_DATA)
	for raw: Variant in doc["hits"]:
		if not raw is Dictionary:
			return {}, Err("search hit", Err.INVALID_DATA)
		var row: Dictionary = raw
		if not row.has("asset"):
			return {}, Err("search hit", Err.INVALID_DATA)
		var missing := missing_hit(row["asset"])
		if not missing.is_empty():
			return {}, Err("search hit " + missing, Err.INVALID_DATA)
	return doc


# Read one asset detail from a channel.
func asset(host: String, publisher: String, slug: String) -> Dictionary, Err:
	var decoded, decode_error := fetch_json(url(host, "/assets/%s/%s/" % [publisher.uri_encode(), slug.uri_encode()]))
	if decode_error != null:
		return {}, decode_error
	var missing := missing_detail(decoded)
	if not missing.is_empty():
		return {}, Err("asset " + missing, Err.INVALID_DATA)
	return decoded


# Read download records for one asset.
func releases(host: String, publisher: String, slug: String) -> Array, Err:
	var decoded, decode_error := fetch_json(url(host, "/releases/%s/%s/" % [publisher.uri_encode(), slug.uri_encode()]))
	if decode_error != null:
		return [], decode_error
	if not decoded is Array:
		return [], Err("releases", Err.INVALID_DATA)
	for raw: Variant in decoded:
		var missing := missing_release(raw)
		if not missing.is_empty():
			return [], Err("release " + missing, Err.INVALID_DATA)
	return decoded


# Recognize an engine version token such as 4.7 or 4.7.2.
func version_text(text: String) -> bool:
	if not text.contains("."):
		return false
	for i: int in text.length():
		var c := text.unicode_at(i)
		if not ((c >= 48 and c <= 57) or c == 46):
			return false
	return true


# Print the count and publisher/slug lines from one search document.
func print_hits(doc: Dictionary) -> void:
	print("count=%s" % str(doc["count"]))
	for raw: Variant in doc["hits"]:
		var item: Dictionary = raw["asset"]
		var who: Dictionary = item["publisher"]
		print("%s/%s\t%s" % [who.get("slug", ""), item["slug"], item["name"]])


# Print one search command, detail command, or release list.
# With no catalog name, search reads every default channel.
func main(argv: Array) -> int:
	var defaults: Dictionary = channels()
	if not argv.is_empty() and str(argv[0]) == "channels":
		for key: Variant in defaults:
			print("%s\t%s" % [str(key), str(defaults[key])])
		return 0
	if argv.is_empty():
		print("usage: search|show|releases|channels official|gd|url ...")
		return 2
	var cmd := str(argv[0])
	var at := 1
	var host := ""
	if at < argv.size():
		var maybe := str(argv[at])
		if maybe == "official" or maybe == "gd" or maybe == "home" or maybe.begins_with("https://") or maybe.begins_with("http://"):
			host = channel(maybe)
			at += 1
	var compat := ""
	if cmd == "search" and at < argv.size() and version_text(str(argv[at])):
		compat = str(argv[at])
		at += 1
	if cmd == "search":
		var text := ""
		for i: int in range(at, argv.size()):
			text += " " if not text.is_empty() else ""
			text += str(argv[i])
		if host.is_empty():
			var failed := 0
			for key: Variant in defaults:
				print(str(key))
				var one, one_error := search(str(defaults[key]), text, compat)
				if one_error != null:
					print(one_error.text())
					failed += 1
					continue
				print_hits(one)
			return 1 if failed == defaults.size() else 0
		var found, find_error := search(host, text, compat)
		if find_error != null:
			print(find_error.text())
			return 1
		print_hits(found)
		return 0
	if host.is_empty():
		host = OFFICIAL
	if argv.size() - at != 2:
		print("usage: show|releases official|gd|url publisher slug")
		return 2
	var publisher_slug := str(argv[at])
	var asset_slug := str(argv[at + 1])
	if cmd == "show":
		var detail, detail_error := asset(host, publisher_slug, asset_slug)
		if detail_error != null:
			print(detail_error.text())
			return 1
		print("%s\t%s" % [detail["slug"], detail["name"]])
		return 0
	if cmd == "releases":
		var listed, list_error := releases(host, publisher_slug, asset_slug)
		if list_error != null:
			print(list_error.text())
			return 1
		for raw: Variant in listed:
			var rel: Dictionary = raw
			print("#%s\t%s\t%s" % [rel.get("id", "?"), rel["version"], rel["download_url"]])
		return 0
	print("unknown command " + cmd)
	return 2
