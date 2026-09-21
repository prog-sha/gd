# Rewrite 0.7.2 public names to their 0.7.3 forms.

extends RefCounted


const TYPES := { # Public type replacements.
	"AsyncContext": "GDAsyncContext",
	"BinaryHeap": "GDBinaryHeap",
	"BodySource": "GDBodySource",
	"CLIFlags": "GDCLIFlags",
	"JSONLReader": "GDJSONLReader",
	"LRUCache": "GDLRUCache",
	"MemoizedCallable": "GDMemoizedCallable",
	"PriorityQueue": "GDPriorityQueue",
	"TestCheck": "GDTestCheck",
}

const DATA := { # Methods moved from file handling to data conversion.
	"csv": true,
	"to_csv": true,
	"csv_objects": true,
	"to_csv_objects": true,
	"ini": true,
	"to_ini": true,
	"toml": true,
	"to_toml": true,
	"yaml": true,
	"to_yaml": true,
	"jsonc": true,
	"strip_jsonc": true,
	"jsonl": true,
	"to_jsonl": true,
	"jsonl_reader": true,
	"front_matter": true,
	"has_front_matter": true,
	"to_front_matter": true,
	"xml": true,
	"to_xml": true,
	"env": true,
	"to_env": true,
	"tar": true,
	"untar": true,
	"csv_async": true,
	"to_csv_async": true,
	"csv_objects_async": true,
	"to_csv_objects_async": true,
	"ini_async": true,
	"to_ini_async": true,
	"toml_async": true,
	"to_toml_async": true,
	"yaml_async": true,
	"to_yaml_async": true,
	"jsonc_async": true,
	"strip_jsonc_async": true,
	"jsonl_async": true,
	"to_jsonl_async": true,
	"front_matter_async": true,
	"to_front_matter_async": true,
	"xml_async": true,
	"to_xml_async": true,
	"env_async": true,
	"to_env_async": true,
	"tar_async": true,
	"untar_async": true,
}

const HTML := { # Methods moved into the HTML namespace.
	"html_fill": "fill",
	"html_template": "template",
	"html_attr": "attr",
	"html_tag": "tag",
	"html_escape": "escape",
	"html_unescape": "unescape",
	"html_escape_async": "escape_async",
	"html_unescape_async": "unescape_async",
	"html_attr_async": "attr_async",
	"html_tag_async": "tag_async",
	"html_fill_async": "fill_async",
}

const CLI := { # Methods moved into the command-line namespace.
	"tty": "stdout_tty",
	"paint": "paint",
	"paint_async": "paint",
	"red": "red",
	"green": "green",
	"yellow": "yellow",
	"blue": "blue",
	"gray": "gray",
	"bold": "bold",
}

var manual := false # Whether a source contains a conversion that needs human judgment.


# Check whether a Unicode character can start an identifier.
func id0(ch: String) -> bool:
	return ch.is_valid_unicode_identifier()


# Check whether a Unicode character can continue an identifier.
func idc(ch: String) -> bool:
	return ("a" + ch).is_valid_unicode_identifier()


# Skip horizontal spacing without crossing a statement boundary.
func skip_space(src: String, at: int) -> int:
	while at < src.length() and (src[at] == " " or src[at] == "\t"):
		at += 1
	return at


# Find the end of a quoted string.
func skip_str(src: String, at: int) -> int:
	var q: String = src[at]
	var n: int = src.length()
	var pos: int = at + 1
	if at + 2 < n and src[at + 1] == q and src[at + 2] == q:
		pos = at + 3
		while pos + 2 < n:
			if src[pos] == "\\":
				pos += 2
				continue
			if src[pos] == q and src[pos + 1] == q and src[pos + 2] == q:
				return pos + 3
			pos += 1
		return n
	while pos < n:
		if src[pos] == "\\":
			pos += 2
			continue
		if src[pos] == q:
			return pos + 1
		pos += 1
	return n


# Read a dotted identifier chain and retain each component boundary.
func read_chain(src: String, at: int) -> Dictionary:
	var names: PackedStringArray = []
	var ends: PackedInt32Array = []
	var pos := at
	while pos < src.length() and id0(src[pos]):
		var end := pos + 1
		while end < src.length() and idc(src[end]):
			end += 1
		var _name_added: bool = names.append(src.substr(pos, end - pos))
		var _end_added: bool = ends.append(end)
		var dot := skip_space(src, end)
		if dot >= src.length() or src[dot] != ".":
			break
		pos = skip_space(src, dot + 1)
	return {"names": names, "ends": ends}


# Return a replacement for a moved public path and its consumed boundary.
func move_path(parts: Dictionary) -> Dictionary:
	var names: PackedStringArray = parts.names
	var ends: PackedInt32Array = parts.ends
	if names.size() >= 2 and names[0] == "R" and names[1] == "of":
		manual = true
	if names.size() >= 2 and names[0] == "Err" and names[1] == "make":
		return {"end": ends[1], "text": "Err.err"}
	if names.size() >= 2 and names[0] == "GD" and names[1] == "postgres":
		return {"end": ends[1], "text": "GD.database.postgres"}
	if names.size() >= 2 and names[0] == "GD" and names[1] == "redis":
		return {"end": ends[1], "text": "GD.database.redis"}
	if names.size() < 3 or names[0] != "GD":
		return {}
	if names[1] == "database" and names[2] == "sqlite_sync":
		return {"end": ends[2], "text": "GD.database.sqlite.open"}
	if names[1] == "file" and DATA.has(names[2]):
		return {"end": ends[2], "text": "GD.data." + names[2]}
	if names[1] == "text" and HTML.has(names[2]):
		return {"end": ends[2], "text": "GD.html." + str(HTML[names[2]])}
	if names[1] == "text" and CLI.has(names[2]):
		return {"end": ends[2], "text": "GD.cli." + str(CLI[names[2]])}
	return {}


# Detect a failure-kind test: an instance call whose first argument names an Err kind.
func kind_call(src: String, at: int, end: int) -> bool:
	var before := at - 1
	while before >= 0 and (src[before] == " " or src[before] == "\t"):
		before -= 1
	var after := skip_space(src, end)
	if before < 0 or src[before] != "." or after >= src.length() or src[after] != "(":
		return false
	return src.substr(skip_space(src, after + 1), 4) == "Err."


# Return source with the previous release's public names replaced.
func rewrite(src: String) -> String:
	manual = false
	var out: PackedStringArray = [] # Pieces joined once so long sources stay linear.
	var i := 0
	var n: int = src.length()
	while i < n:
		var ch: String = src[i]
		var end := i + 1
		var piece := ch
		if ch == "#":
			var eol: int = src.find("\n", i)
			end = n if eol < 0 else eol
			piece = src.substr(i, end - i)
		elif ch == '"' or ch == "'":
			end = skip_str(src, i)
			piece = src.substr(i, end - i)
		elif id0(ch):
			var parts := read_chain(src, i)
			var moved: Dictionary = move_path(parts)
			var ends: PackedInt32Array = parts.ends
			end = ends[0]
			piece = src.substr(i, end - i)
			if not moved.is_empty():
				end = moved.end
				piece = str(moved.text)
			elif TYPES.has(piece):
				piece = str(TYPES[piece])
			elif piece == "is_kind" and kind_call(src, i, end):
				piece = "is"
			elif piece == "is_kind":
				manual = true
		var _added: bool = out.append(piece)
		i = end
	return "".join(out)


# Collect script files from the requested path.
func collect(root: String) -> R:
	if root.ends_with(".gd"):
		return R.ok(PackedStringArray([root]))
	var walked: R = GD.file.walk(root)
	if not walked.ok:
		return walked
	var out: PackedStringArray = []
	for path: String in walked.v:
		if path.ends_with(".gd"):
			var _ok: bool = out.append(path)
	return R.ok(out)


# Safely rewrite one script and report changes requiring manual work.
func apply_file(path: String, write: bool) -> R:
	var got: R = GD.file.read_text(path)
	if not got.ok:
		return got
	var next: String = rewrite(str(got.v))
	var changed: bool = next != str(got.v)
	if write and changed:
		var saved: R = GD.file.replace_text(path, got.v, next)
		if not saved.ok:
			return saved
	return R.ok({"changed": changed, "manual": manual})


# Check scripts or rewrite their public references for this release.
func main(argv: Array) -> int:
	if argv.is_empty():
		print("usage: gd tools/migrate_0_7_3.gd <path> | gd tools/migrate_0_7_3.gd -- --check <path>")
		return 1
	var check: bool = argv[0] == "--check"
	if check and argv.size() < 2:
		print("usage: gd tools/migrate_0_7_3.gd <path> | gd tools/migrate_0_7_3.gd -- --check <path>")
		return 1
	var root: String = argv[1] if check else argv[0]
	var gathered: R = collect(root)
	if not gathered.ok:
		var _logged: R = GD.log.error(str(gathered.e))
		return 1
	var files: PackedStringArray = gathered.v
	if files.is_empty():
		print("no .gd: %s" % root)
		return 1
	var changed := 0
	var manual_count := 0
	for path: String in files:
		var applied: R = apply_file(path, not check)
		if not applied.ok:
			var _logged: R = GD.log.error("%s: %s" % [path, applied.e])
			return 1
		if applied.v.changed:
			print(path)
			changed += 1
		if applied.v.manual:
			print("%s: manual migration required: R.of(...)" % path)
			manual_count += 1
	print("changed=%d manual=%d" % [changed, manual_count])
	return 1 if manual_count > 0 or (check and changed > 0) else 0
