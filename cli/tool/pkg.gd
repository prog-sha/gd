# Manage dependencies through an embedded script invoked by commands such as gd install.
#
# Recognize four source forms by their prefixes.
# gd:@scope/name@^1.2.0      Registry script package.
# ext:@scope/name@^0.3.0     Registry binary extension.
# https://example.com/x.gd  Direct URL.
# ./lib/local.gd           Local file.
#
# Keep dependency policy in script source rather than native implementation.
extends RefCounted

const REGISTRY_FALLBACK: String = "https://gd.progsha.com/pkg" # Default package registry.
const PUBLISH_CHUNK: int = 8 * 1024 * 1024 # Maximum file chunk sent in one publishing request.
const MANIFEST_MAX: int = 16 * 1024 * 1024 # Maximum manifest input bytes.
const PACKAGE_MAX: int = 500 * 1024 * 1024 # Maximum combined bytes of package files.

var seen: Dictionary = {} # Original file text used to detect concurrent changes after reading.
var broken: Dictionary = {} # Paths with invalid JSON that must not be overwritten as empty configuration.
var log_failed := false # Reflect diagnostic-output failures in the exit status after cleanup.
var global_names: Dictionary = {} # Global script class names already present in this graph.
var graph_frozen := false # Preserve the exact lockfile bytes during frozen installation.
var graph_active := false # Retain all package undo records until the whole graph commits.
var graph_steps: Array = [] # Durable ordered undo records, including the currently active step.

var meta_cache: Dictionary = {} # Registry version indexes already read in this run, by package.


# Continue failure cleanup and record diagnostic-output failures for the final exit status.
func note(msg: String, level: String = "error") -> void:
	var logged: R = GD.log.write(level, msg)
	if not logged.ok:
		log_failed = true


# Decode UTF-8 JSON with strict standard-data semantics.
func decode_json(text: String) -> R:
	return GD.data.json_decode(text.to_utf8_buffer())


# Encode JSON as UTF-8 within the existing byte budget.
func encode_json(value: Variant, max_bytes: int, newline: bool = false) -> R:
	var spare := 1 if newline else 0
	var encoded: R = GD.data.json_encode(value, {"max_bytes": max_bytes - spare if max_bytes > 0 else 0})
	if not encoded.ok:
		return encoded
	var raw: PackedByteArray = encoded.v
	var text := raw.get_string_from_utf8()
	return R.ok(text + ("\n" if newline else ""))


# ---------------- Configuration and locks ----------------

# Read gd.json only at the jail root; parent traversal is not permitted.
func find_config() -> String:
	return "res://gd.json" if GD.file.exists("res://gd.json") else ""


func base_dir() -> String:
	return "res://"


func load_json(path: String, fallback: Dictionary) -> Dictionary:
	if not GD.file.exists(path):
		if not seen.has(path):
			seen[path] = null
		var _clear_missing: bool = broken.erase(path)
		return fallback
	var r: R = GD.file.read_text(path)
	if r.e != null:
		broken[path] = true
		note("cannot read %s" % path)
		return fallback
	if not seen.has(path):
		seen[path] = r.v
	var parsed: R = decode_json(str(r.v))
	if not parsed.ok or not parsed.v is Dictionary:
		broken[path] = true
		note("cannot parse %s" % path)
		return fallback
	var _clear_valid: bool = broken.erase(path)
	return parsed.v


func save_json(path: String, data: Dictionary) -> bool:
	if broken.has(path):
		note("cannot overwrite unreadable %s" % path)
		return false
	var encoded: R = encode_json(data, 0, true)
	if not encoded.ok:
		note("cannot encode %s: %s" % [path, encoded.e])
		return false
	var body: String = encoded.v
	var w: R = GD.file.replace_text(path, seen.get(path, null), body)
	if w.ok:
		seen[path] = body
	return w.ok


func config() -> Dictionary:
	return load_json(config_path(), {})


func save_config(cfg: Dictionary) -> bool:
	return save_json(config_path(), cfg)


func lock() -> Dictionary:
	return load_json(lock_path(), {})


func save_lock(data: Dictionary) -> bool:
	return save_json(lock_path(), data)


# Return the single configuration-file location.
func config_path() -> String:
	return GD.file.join([base_dir(), "gd.json"])


# Return the single lockfile location.
func lock_path() -> String:
	return GD.file.join([base_dir(), "gd.lock"])


# Check whether JSON was read successfully.
func json_ok(path: String) -> bool:
	return not broken.has(path)


# Return the package-transaction journal location.
func txn_path() -> String:
	return GD.file.join([base_dir(), ".godot", "gd-package-txn.json"])


# Record complete before and after text for a transaction.
func text_change(path: String, after_exists: bool, after: String) -> R:
	var before_exists := GD.file.exists(path)
	var before := ""
	if before_exists:
		var got: R = GD.file.read_text(path)
		if got.e != null:
			return got
		before = str(got.v)
	return R.ok({
		"path": path, "before_exists": before_exists, "before": before,
		"after_exists": after_exists, "after": after,
	})


# Record JSON before and after updates with consistent formatting.
func json_change(path: String, data: Dictionary) -> R:
	if graph_frozen and path == lock_path():
		var original: R = GD.file.read_text(path)
		if original.e != null:
			return original
		var parsed: R = decode_json(str(original.v))
		if parsed.e != null or parsed.v != data:
			return R.err("resolved metadata differs from gd.lock; remove --frozen to update it", Err.INVALID_DATA)
		return text_change(path, true, str(original.v))
	var encoded: R = encode_json(data, 0, true)
	var text: String = encoded.v if encoded.ok else ""
	return encoded if not encoded.ok else text_change(path, true, text)


# Apply a text update only to its original starting contents.
func apply_text(snap: Dictionary) -> R:
	var old: Variant = null
	if snap["before_exists"] == true:
		old = str(snap["before"])
	if snap["after_exists"] == true:
		return GD.file.replace_text(str(snap["path"]), old, str(snap["after"]))
	return GD.file._remove_text(str(snap["path"]), str(snap["before"])) if snap["before_exists"] == true else R.ok()


# Distinguish external edits from transaction-written contents before recovery.
func can_restore_text(snap: Dictionary) -> R:
	var path := str(snap["path"])
	var exists := GD.file.exists(path)
	if not exists:
		if snap["before_exists"] != true:
			return R.ok(false)
		if snap["after_exists"] != true:
			return R.ok(true)
		return R.err("%s changed after the package transaction" % path, Err.ALREADY_EXISTS)
	var got: R = GD.file.read_text(path)
	if got.e != null:
		return got
	var body := str(got.v)
	if snap["before_exists"] == true and body == str(snap["before"]):
		return R.ok(false)
	if snap["after_exists"] == true and body == str(snap["after"]):
		return R.ok(true)
	return R.err("%s changed after the package transaction" % path, Err.ALREADY_EXISTS)


# Restore only text written by the transaction to its starting state.
func restore_text(snap: Dictionary) -> R:
	var check: R = can_restore_text(snap)
	if check.e != null or check.v != true:
		return check
	var path := str(snap["path"])
	var after := str(snap["after"])
	if snap["before_exists"] == true:
		var current: Variant = null
		if snap["after_exists"] == true:
			current = after
		return GD.file.replace_text(path, current, str(snap["before"]))
	return GD.file._remove_text(path, after)


# Validate the transaction's random component as 32 hexadecimal digits.
func txn_nonce(value: String) -> bool:
	if value.length() != 32:
		return false
	for ch: String in value:
		if "0123456789abcdef".find(ch) < 0:
			return false
	return true


# Validate a lowercase hexadecimal SHA-256 digest.
func sha256_text(value: String) -> bool:
	if value.length() != 64:
		return false
	for ch: String in value:
		if "0123456789abcdef".find(ch) < 0:
			return false
	return true


# Require recovery-journal paths to stay within package-managed locations.
func valid_txn(txn: Dictionary) -> bool:
	if typeof(txn.get("had_final")) != TYPE_BOOL:
		return false
	if typeof(txn.get("tree")) != TYPE_BOOL:
		return false
	for field: String in ["before", "after"]:
		var fingerprint := str(txn.get(field, ""))
		if not fingerprint.is_empty() and not sha256_text(fingerprint):
			return false
	var root := pkg_dir()
	var prefix := root.trim_suffix("/") + "/"
	var final_path := str(txn.get("final", ""))
	var backup_path := str(txn.get("backup", ""))
	var stage_path := str(txn.get("stage", ""))
	if final_path.is_empty():
		# A cache-only install changes text files only.
		if txn["had_final"] == true or not backup_path.is_empty() or not stage_path.is_empty():
			return false
	else:
		if not final_path.begins_with(prefix):
			return false
		var rel := final_path.substr(prefix.length())
		if rel.is_empty() or GD.file.under(root, rel) != final_path:
			return false
		if not safe_file_path(rel) or (rel.contains("/") and not (rel.begins_with("@") and rel.count("/") == 1)):
			return false
		if not backup_path.begins_with(final_path + ".") or not backup_path.ends_with(".old"):
			return false
		var nonce := backup_path.trim_prefix(final_path + ".").trim_suffix(".old")
		if not txn_nonce(nonce) or GD.file.under(root, backup_path.substr(prefix.length())) != backup_path:
			return false
		if not stage_path.is_empty():
			if not stage_path.begins_with(prefix + ".stage-") or not txn_nonce(stage_path.trim_prefix(prefix + ".stage-")):
				return false
			if GD.file.under(root, stage_path.substr(prefix.length())) != stage_path:
				return false
	var allowed := PackedStringArray([
		config_path(), lock_path(), GD.file.join([base_dir(), ".godot", "extension_list.cfg"]),
	])
	var snaps: Variant = txn.get("text", [])
	if not snaps is Array:
		return false
	for raw_snap: Variant in snaps:
		if not raw_snap is Dictionary:
			return false
		var snap: Dictionary = raw_snap
		if not allowed.has(str(snap.get("path", ""))) or not snap.has_all(["before_exists", "before", "after_exists", "after"]):
			return false
		if typeof(snap["before_exists"]) != TYPE_BOOL or typeof(snap["before"]) != TYPE_STRING \
				or typeof(snap["after_exists"]) != TYPE_BOOL or typeof(snap["after"]) != TYPE_STRING:
			return false
	return true


# Restore an interrupted package operation to its starting state.
func recover_txn() -> R:
	var path := txn_path()
	if not GD.file.exists(path):
		return R.ok()
	var size: R = GD.file.size_of(path)
	if size.e != null:
		return size
	var got: R = GD.file.read_text(path)
	if got.e != null:
		return got
	var decoded: R = decode_json(str(got.v))
	if not decoded.ok or not decoded.v is Dictionary:
		return R.err("broken package transaction", Err.INVALID_DATA)
	var record: Dictionary = decoded.v
	if record.has("steps"):
		if not record["steps"] is Array:
			return R.err("broken graph transaction", Err.INVALID_DATA)
		var steps: Array = record["steps"]
		for raw_step: Variant in steps:
			if not raw_step is Dictionary:
				return R.err("unsafe graph transaction", Err.PERMISSION_DENIED)
			var step: Dictionary = raw_step
			if not valid_txn(step):
				return R.err("unsafe graph transaction", Err.PERMISSION_DENIED)
		while not steps.is_empty():
			var step: Dictionary = steps.back()
			var restored: R = restore_txn(step)
			if restored.e != null:
				return restored
			var _step: Variant = steps.pop_back()
			var remaining: R = encode_json({"steps": steps}, 0, true)
			if remaining.e != null:
				return remaining
			var updated: R = GD.file.replace_text(path, str(got.v), str(remaining.v))
			if updated.e != null:
				return updated
			got = remaining
	else:
		if not valid_txn(record):
			return R.err("unsafe package transaction", Err.PERMISSION_DENIED)
		var restored: R = restore_txn(record)
		if restored.e != null:
			return restored
	return GD.file.remove(path)


# Restore one journaled step; repeating a completed restoration is harmless.
func restore_txn(txn: Dictionary) -> R:
	var final_path := str(txn.get("final", ""))
	var backup_path := str(txn.get("backup", ""))
	var stage_path := str(txn.get("stage", ""))
	# Stop before restoring any file if external edits are present.
	for raw_snap: Variant in txn.get("text", []):
		var snap: Dictionary = raw_snap
		var checked: R = can_restore_text(snap)
		if checked.e != null:
			return checked
	# Do not remove contents replaced by something other than this transaction.
	if not final_path.is_empty():
		var tree: bool = txn["tree"]
		var current: R = path_fingerprint(final_path, tree)
		if current.e != null:
			return current
		var before := str(txn["before"])
		var after := str(txn["after"])
		var has_backup := GD.file.exists(backup_path)
		if has_backup:
			var backup_fingerprint: R = path_fingerprint(backup_path, tree)
			if backup_fingerprint.e != null:
				return backup_fingerprint
			if str(backup_fingerprint.v) != before:
				return R.err("package backup changed after the transaction", Err.ALREADY_EXISTS)
		if has_backup and not str(current.v).is_empty() and str(current.v) != after:
			return R.err("package output changed after the transaction", Err.ALREADY_EXISTS)
		if not has_backup and txn["had_final"] == true and str(current.v) != before:
			return R.err("package output changed after the transaction", Err.ALREADY_EXISTS)
		if not has_backup and txn["had_final"] != true and not str(current.v).is_empty() and str(current.v) != after:
			return R.err("package output changed after the transaction", Err.ALREADY_EXISTS)
		if has_backup:
			if GD.file.exists(final_path):
				var dropped: R = GD.file.remove_all(final_path)
				if dropped.e != null:
					return dropped
			var restored: R = GD.file.rename(backup_path, final_path)
			if restored.e != null:
				return restored
		elif txn.get("had_final", false) != true and GD.file.exists(final_path):
			var dropped_new: R = GD.file.remove_all(final_path)
			if dropped_new.e != null:
				return dropped_new
	if not stage_path.is_empty() and GD.file.exists(stage_path):
		var dropped_stage: R = GD.file.remove_all(stage_path)
		if dropped_stage.e != null:
			return dropped_stage
	for raw_snap: Variant in txn.get("text", []):
		var snap: Dictionary = raw_snap
		var restored_text: R = restore_text(snap)
		if restored_text.e != null:
			return restored_text
	return R.ok()


# Acquire the project lock and recover interrupted operations first.
func package_lock() -> R:
	var dir := GD.file.join([base_dir(), ".godot"])
	var made: R = GD.file.make_dir(dir)
	if made.e != null:
		return made
	var guard: R = GD.file._lock(GD.file.join([dir, "gd-package.lock"]))
	if guard.e != null:
		return guard
	var recovered: R = recover_txn()
	if recovered.e != null:
		return recovered
	return guard


# Commit recovery information before beginning multi-file updates.
func begin_txn(data: Dictionary) -> R:
	if not valid_txn(data):
		return R.err("unsafe package transaction", Err.PERMISSION_DENIED)
	if graph_active:
		var before: Variant = null
		if GD.file.exists(txn_path()):
			var got: R = GD.file.read_text(txn_path())
			if got.e != null:
				return got
			before = got.v
		graph_steps.append(data)
		var journal: R = encode_json({"steps": graph_steps}, 0, true)
		if journal.e != null:
			return journal
		return GD.file.replace_text(txn_path(), before, str(journal.v))
	var encoded: R = encode_json(data, 0, true)
	var text: String = encoded.v if encoded.ok else ""
	return encoded if not encoded.ok else GD.file.replace_text(txn_path(), null, text)


# Remove the journal after all transaction updates commit.
func end_txn() -> R:
	return R.ok() if graph_active else GD.file.remove(txn_path())


# Roll back a failed transaction and prioritize recovery failures.
func fail_txn(reason: R) -> R:
	if graph_active:
		return reason
	var recovered: R = recover_txn()
	return reason if recovered.e == null else recovered


# Select the registry from gd.json, then environment, then the default.
# Read gd.json first because environment access requires --allow-env.
func registry() -> String:
	var from_cfg: String = str(config().get("registry", ""))
	if not json_ok(config_path()):
		return ""
	if not from_cfg.is_empty():
		return from_cfg.trim_suffix("/")
	if OS.has_environment("GD_REGISTRY"):
		var env: String = OS.get_environment("GD_REGISTRY", "")
		if not env.is_empty():
			return env.trim_suffix("/")
	return REGISTRY_FALLBACK


# Require verified remote transport or a local test server.
func secure_url(url: String) -> bool:
	var parsed: R = GD.http.parse_url(url)
	if parsed.e != null:
		return false
	var parts: Dictionary = parsed.v
	var scheme := str(parts["scheme"])
	if scheme == "https":
		return true
	if scheme != "http":
		return false
	var host := str(parts["host"]).to_lower().trim_suffix(".")
	return host == "localhost" or host == "::1" \
			or (host.begins_with("127.") and GD.net.is_ip(host))


# Identify filenames invalid on Windows.
func windows_reserved(name: String) -> bool:
	var short := name.get_slice(".", 0).to_lower()
	return PackedStringArray(["con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"]).has(short)


# Validate package and alias components against portable path rules.
func safe_segment(name: String) -> bool:
	if name.is_empty() or name.begins_with(".") or name.ends_with("."):
		return false
	var allowed := "-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	for ch: String in name:
		if allowed.find(ch) < 0:
			return false
	if windows_reserved(name):
		return false
	# Reject names resembling legacy Windows short-name aliases.
	var short := name.get_slice(".", 0).to_lower()
	var tilde := short.rfind("~")
	if tilde >= 0 and tilde + 1 < short.length():
		var digits := true
		for ch: String in short.substr(tilde + 1):
			digits = digits and ch >= "0" and ch <= "9"
		if digits:
			return false
	return true


# Restrict module file paths to a safe ASCII subset.
func safe_file_path(path: String) -> bool:
	if path.is_empty() or path.begins_with("/") or path.ends_with("/") or path.contains("\\"):
		return false
	# Reject percent signs because URI decoding could turn them into different paths.
	var allowed := "!#$&()+,-.=@[]^_{}~ 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	for part: String in path.split("/", true):
		if part.is_empty() or part.trim_prefix(".").is_empty() or part.ends_with(".") or windows_reserved(part):
			return false
		for ch: String in part:
			if allowed.find(ch) < 0:
				return false
	return true


# Validate registry names as safe @scope/name pairs.
func safe_package(name: String) -> bool:
	var parts := name.trim_prefix("@").split("/", true)
	return name.begins_with("@") and parts.size() == 2 \
			and safe_segment(parts[0]) and safe_segment(parts[1])


# Check for case collisions among files and parent directories.
func add_portable_path(path: String, paths: Dictionary) -> bool:
	var current := path
	var is_file := true
	while not current.is_empty():
		var fold := current.to_lower()
		if paths.has(fold):
			var old: Dictionary = paths[fold]
			if str(old["path"]) != current or (old["file"] == true) != is_file:
				return false
		else:
			paths[fold] = {"path": current, "file": is_file}
		var slash := current.rfind("/")
		if slash < 0:
			break
		current = current.substr(0, slash)
		is_file = false
	return true


# Hash large files incrementally with SHA-256.
func file_sha256(path: String, size: int) -> R:
	var digest_ctx := HashingContext.new()
	if digest_ctx.start(HashingContext.HASH_SHA256) != OK:
		return R.err("cannot start SHA-256", Err.INVALID_DATA)
	var at := 0
	while at < size:
		var got: R = GD.file.read_bytes(path, at, mini(PUBLISH_CHUNK, size - at))
		if got.e != null:
			return got
		var chunk: PackedByteArray = got.v
		if chunk.is_empty():
			return R.err("file ended while hashing: %s" % path, Err.INVALID_DATA)
		if digest_ctx.update(chunk) != OK:
			return R.err("cannot hash %s" % path, Err.INVALID_DATA)
		at += chunk.size()
	return R.ok(digest_ctx.finish().hex_encode())


# Validate registry fingerprints and byte counts.
func registry_mark(raw: Variant, max_size: int) -> R:
	if not raw is Dictionary:
		return R.err("registry has broken file metadata", Err.INVALID_DATA)
	var mark: Dictionary = raw
	var size := str(mark.get("size", -1)).to_int()
	var sha := str(mark.get("sha256", ""))
	if size < 0 or size > max_size or not sha256_text(sha):
		return R.err("registry has invalid file metadata", Err.INVALID_DATA)
	return R.ok({"size": size, "sha256": sha})


# Read the entry point and file fingerprints as a platform-independent version contract.
func registry_version(raw: Variant) -> R:
	var main_checked: R = registry_mark(raw, PACKAGE_MAX)
	if main_checked.e != null:
		return main_checked
	var doc: Dictionary = raw
	var raw_files: Variant = doc.get("files")
	if not raw_files is Dictionary:
		return R.err("registry has broken version metadata", Err.INVALID_DATA)
	var files: Dictionary = raw_files
	var clean := {}
	var paths := {}
	var total := 0
	var entries := PackedStringArray()
	var main_mark: Dictionary = main_checked.v
	for raw_path: Variant in files:
		var path := str(raw_path)
		if not safe_file_path(path) or not add_portable_path(path, paths):
			return R.err("registry has unsafe file metadata", Err.INVALID_DATA)
		var checked: R = registry_mark(files[raw_path], PACKAGE_MAX - total)
		if checked.e != null:
			return checked
		var mark: Dictionary = checked.v
		total += str(mark["size"]).to_int()
		clean[path] = mark
		if mark == main_mark and (path == "mod.gd" or (safe_segment(path) and path.ends_with(".gdextension"))):
			var _entry := entries.append(path)
	if entries.size() != 1:
		return R.err("registry entry file is not unique", Err.INVALID_DATA)
	var leaf := entries[0]
	# Registry metadata declares a package's imports as ranges; gd.lock records them resolved.
	var raw_imports: Variant = doc.get("requires", doc.get("imports", {}))
	if not raw_imports is Dictionary:
		return R.err("registry has broken import metadata", Err.INVALID_DATA)
	var imports: Dictionary = raw_imports
	var clean_imports := {}
	for raw_alias: Variant in imports:
		var alias := str(raw_alias)
		var spec := str(imports[raw_alias])
		var kind := str(parse_spec(spec)["kind"])
		if not safe_segment(alias) or alias.begins_with("@") or (kind != "gd" and kind != "ext"):
			return R.err("registry has unsafe import metadata", Err.INVALID_DATA)
		clean_imports[alias] = spec
	return R.ok({"sha256": main_mark["sha256"], "size": main_mark["size"], "leaf": leaf, "files": clean, "imports": clean_imports})


# Download registry files in chunks and expose the cache only after complete fingerprint verification.
func fetch_registry_file(url: String, path: String, raw_mark: Variant, max_size: int) -> R:
	var checked: R = registry_mark(raw_mark, max_size)
	if checked.e != null:
		return checked
	var mark: Dictionary = checked.v
	var size := str(mark["size"]).to_int()
	var next := "%s.%s.next" % [path, Crypto.new().generate_random_bytes(16).hex_encode()]
	var parts := maxi(1, (size + PUBLISH_CHUNK - 1) / PUBLISH_CHUNK)
	for part: int in parts:
		var start := part * PUBLISH_CHUNK
		var end := mini(start + PUBLISH_CHUNK, size) - 1
		var opts := {"max_body": PUBLISH_CHUNK}
		if size > 0:
			opts["headers"] = {"Range": "bytes=%d-%d" % [start, end]}
		var response, _response_err := await GD.http.fetch("%s?part=%d" % [url, part], opts)
		if not (_response_err == null and response.ok()):
			var _drop_fetch: R = GD.file.remove(next) if GD.file.exists(next) else R.ok()
			if _response_err: return R.err(_response_err.note("fetch package file"))
			return R.err("cannot fetch %s: HTTP %d" % [url, response.status], Err.NOT_FOUND)
		var chunk: PackedByteArray = response.body
		var expected := mini(PUBLISH_CHUNK, size - part * PUBLISH_CHUNK)
		if chunk.size() != expected:
			var _drop_short: R = GD.file.remove(next) if GD.file.exists(next) else R.ok()
			return R.err("registry file ended early", Err.INVALID_DATA)
		var wrote: R = GD.file.write_bytes(next, chunk) if part == 0 else GD.file.append_bytes(next, chunk)
		if wrote.e != null:
			var _drop_write: R = GD.file.remove(next) if GD.file.exists(next) else R.ok()
			return wrote
	var digest: R = file_sha256(next, size)
	if digest.e != null or str(digest.v) != str(mark["sha256"]):
		var _drop_bad: R = GD.file.remove(next) if GD.file.exists(next) else R.ok()
		return R.err("registry file fingerprint differs", Err.INVALID_DATA)
	var moved: R = GD.file.rename(next, path)
	return moved if moved.e != null else R.ok()


# Fingerprint a published file or native-extension tree.
func path_fingerprint(path: String, tree: bool) -> R:
	if not GD.file.exists(path):
		return R.ok("")
	var files := PackedStringArray([path])
	if tree:
		var walked: R = GD.file.walk(path, false, true)
		if walked.e != null:
			return walked
		files = walked.v
		files.sort()
	var total := 0
	var marks := PackedStringArray()
	for file_path: String in files:
		var size: R = GD.file.size_of(file_path)
		if size.e != null:
			return size
		var count := str(size.v).to_int()
		total += count
		if total > PACKAGE_MAX:
			return R.err("package tree exceeds the byte limit", Err.LIMITED)
		var digest: R = file_sha256(file_path, count)
		if digest.e != null:
			return digest
		var _marked: bool = marks.append("%s\t%d\t%s" % [file_path.trim_prefix(path).trim_prefix("/"), count, digest.v])
	return R.ok(GD.data.hex_encode(GD.data.sha256("\n".join(marks).to_utf8_buffer())))


# Read the registry version index over a verified connection.
func fetch_meta(pkg: String) -> R:
	if meta_cache.has(pkg):
		return R.ok(meta_cache[pkg])
	var url: String = registry() + "/" + pkg + "/meta.json"
	if not secure_url(url):
		return R.err("registry must use HTTPS", Err.PERMISSION_DENIED)
	var res, _res_err := await GD.http.fetch(url)
	if not (_res_err == null and res.ok()):
		if _res_err: return R.err(_res_err.note("fetch package metadata"))
		return R.err("%s: HTTP %d" % [pkg, res.status], Err.NOT_FOUND)
	var decoded: R = res.json()
	if decoded.e != null or not decoded.v is Dictionary:
		return R.err("%s: broken meta.json" % pkg, Err.INVALID_DATA)
	var meta: Dictionary = decoded.v
	if not meta.has("versions") or not meta["versions"] is Dictionary:
		return R.err("%s: broken meta.json" % pkg, Err.INVALID_DATA)
	var versions: Dictionary = meta["versions"]
	for raw: Variant in versions:
		var checked: R = registry_version(versions[raw])
		if not package_version(str(raw)) or checked.e != null:
			return R.err("%s: broken meta.json" % pkg, Err.INVALID_DATA)
		versions[raw] = checked.v
	meta["versions"] = versions
	meta_cache[pkg] = meta
	return R.ok(meta)


# ---------------- Dependency identifiers ----------------

# Parse a dependency string into kind, pkg, range, and url.
# Kinds are gd, ext, url, local, and bad.
func parse_spec(raw: String) -> Dictionary:
	var s: String = raw.strip_edges()
	if s.begins_with("http://") or s.begins_with("https://"):
		return {"kind": "url", "pkg": "", "range": "", "url": s}
	if s.begins_with("./") or s.begins_with("../") or (s.is_absolute_path() and not s.contains("://")):
		return {"kind": "local", "pkg": "", "range": "", "url": s}

	var kind: String = "gd"
	var rest: String = s
	if s.begins_with("gd:"):
		rest = s.substr(3)
	elif s.begins_with("ext:"):
		kind = "ext"
		rest = s.substr(4)

	# Split @scope/name@range at the final at sign, not the scope prefix.
	var pkg: String = rest
	var range_txt: String = ""
	var at: int = rest.rfind("@")
	if at > 0:
		pkg = rest.substr(0, at)
		range_txt = rest.substr(at + 1)
	if not safe_package(pkg):
		return {"kind": "bad", "pkg": pkg, "range": range_txt, "url": ""}
	return {"kind": kind, "pkg": pkg, "range": range_txt, "url": ""}


# Restore dependency notation for writing to gd.json.
func spec_text(sp: Dictionary) -> String:
	var kind: String = str(sp["kind"])
	if kind == "url" or kind == "local":
		return str(sp["url"])
	var head: String = "ext:" if kind == "ext" else "gd:"
	var range_txt: String = str(sp["range"])
	return head + str(sp["pkg"]) if range_txt.is_empty() else head + str(sp["pkg"]) + "@" + range_txt


# Derive an alias, such as greet from @luca/greet, shaped as an identifier so @import binds it as written.
func short_name(pkg: String) -> String:
	var parts: PackedStringArray = pkg.split("/")
	var leaf := parts[parts.size() - 1] if parts.size() > 1 else pkg
	return leaf.replace("-", "_").replace(".", "_")


# Refuse aliases that scripts could not bind: engine class names and script keywords.
func alias_usable(alias: String) -> String:
	if not alias.is_valid_ascii_identifier():
		return "package name must be an identifier, so scripts can write @import %s" % alias.replace("-", "_")
	if ClassDB.class_exists(alias):
		return "%s is an engine class; name the package differently: gd add <name> <package>" % alias
	var keywords := PackedStringArray(["if", "elif", "else", "for", "while", "match", "when", "break", "continue", "pass", "return",
			"class", "class_name", "extends", "is", "in", "as", "self", "super", "signal", "func", "static", "const", "enum", "var",
			"breakpoint", "preload", "await", "yield", "assert", "void", "not", "and", "or", "true", "false", "null", "PI", "TAU", "INF", "NAN"])
	if keywords.has(alias):
		return "%s is a GDScript keyword; name the package differently: gd add <name> <package>" % alias
	return ""


# ---------------- Cache locations ----------------

# Use the machine-wide cache only to avoid repeated downloads.
# Scripts read installed packages through pkg://, which resolves into this cache or into pkg/.
func cache_dir(pkg: String, version: String) -> String:
	# The executable always provides its own cache mount.
	return "cache://" + GD.file.join(["pkg", pkg, version])


# Locate package copies beside gd.json.
func pkg_dir() -> String:
	return GD.file.join([base_dir(), "pkg"])


# Select package placement when gd.json omits it: an editor project reads only res://, so the project.
func default_place() -> String:
	return "project" if GD.file.exists(GD.file.join([base_dir(), "project.godot"])) else "cache"


# Report whether gd.json names a known package placement: the shared cache or the project.
func place_ok() -> bool:
	return str(config().get("place", default_place())) in ["cache", "project"]


# Copy pure script packages under pkg/ only when packages are placed in the project.
func place_project() -> bool:
	return str(config().get("place", default_place())) == "project"


# ---------------- Source references ----------------

# Read significant source tokens while excluding comments and preserving literal extents.
func source_tokens(text: String) -> Array:
	var tokens: Array = []
	var i := 0
	while i < text.length():
		var c := text[i]
		if c in [" ", "\t", "\r", "\n"]:
			i += 1
			continue
		if c == "#":
			var end := text.find("\n", i)
			i = text.length() if end < 0 else end
			continue
		var start := i
		var literal := c == "\"" or c == "'"
		if literal:
			var quote := c.repeat(3) if text.substr(i, 3) == c.repeat(3) else c
			i += quote.length()
			while i < text.length():
				if text[i] == "\\":
					i += 2
				elif text.substr(i, quote.length()) == quote:
					i += quote.length()
					break
				else:
					i += 1
		elif c.is_valid_identifier():
			i += 1
			while i < text.length() and (text[i].is_valid_identifier() or text[i].is_valid_int()):
				i += 1
		else:
			i += 1
		tokens.append({"start": start, "end": mini(i, text.length()), "text": text.substr(start, i - start), "literal": literal})
	return tokens


# Relocate static resource-loading operands without modifying ordinary strings or comments.
func relocate_text(text: String, root: String) -> String:
	var tokens := source_tokens(text)
	var edits: Array = []
	for index: int in tokens.size():
		var token: Dictionary = tokens[index]
		if not token["literal"]:
			continue
		var before := index - 1
		if before >= 0 and str(tokens[before]["text"]) == "r":
			before -= 1
		var previous := str(tokens[before]["text"]) if before >= 0 else ""
		var load_arg := before > 0 and previous == "(" and str(tokens[before - 1]["text"]) in ["preload", "load"]
		var import_arg := before > 0 and previous == "import" and str(tokens[before - 1]["text"]) == "@"
		if previous != "extends" and not load_arg and not import_arg:
			continue
		var raw := str(token["text"])
		var width := 3 if raw.begins_with(raw[0].repeat(3)) else 1
		if raw.substr(width).begins_with("res://"):
			edits.append({"start": str(token["start"]).to_int() + width, "end": str(token["start"]).to_int() + width + 6})
	edits.reverse()
	for edit: Dictionary in edits:
		text = text.substr(0, str(edit["start"]).to_int()) + root.trim_suffix("/") + "/" + text.substr(str(edit["end"]).to_int())
	return text


# Collect declared global names from lexical tokens, excluding documentation and string contents.
func source_classes(text: String) -> PackedStringArray:
	var tokens := source_tokens(text)
	var names := PackedStringArray()
	for i: int in tokens.size():
		if str(tokens[i]["text"]) == "class_name" and i + 1 < tokens.size():
			var name := str(tokens[i + 1]["text"])
			if name.is_valid_identifier():
				var _name := names.append(name)
	return names


# Reserve script and native class names in one graph-wide namespace.
func check_names(names: Array, owner: String) -> R:
	for raw_name: Variant in names:
		var name := str(raw_name)
		if not name.is_valid_unicode_identifier() or ClassDB.class_exists(name) or (global_names.has(name) and str(global_names[name]) != owner):
			return R.err("global class %s conflicts between %s and %s" % [name, global_names.get(name, "engine"), owner], Err.INVALID_DATA)
		global_names[name] = owner
	return R.ok()


# Check declared native names without loading the package binary.
func check_native(path: String, key: String) -> R:
	var text: R = GD.file.read_text(path)
	if text.e != null:
		return text
	var parsed: R = GD.data.ini(str(text.v))
	if parsed.e != null:
		return parsed
	var doc: Dictionary = parsed.v
	var classes: Variant = doc.get("classes", {})
	if not classes is Dictionary:
		return R.err("invalid native class declarations: %s" % key, Err.INVALID_DATA)
	var declared: Dictionary = classes
	return check_names(declared.keys(), key)


# Check global class declarations in verified source files before exposing a package.
func check_globals(root: String, key: String, files: Dictionary) -> R:
	for raw_rel: Variant in files:
		var rel := str(raw_rel)
		if not rel.ends_with(".gd"):
			continue
		var source_text: R = GD.file.read_text(GD.file.under(root, rel))
		if source_text.e != null:
			return source_text
		var reserved: R = check_names(Array(source_classes(str(source_text.v))), key + "/" + rel)
		if reserved.e != null:
			return reserved
	return R.ok()


# Inspect copied local sources using the same global-name table as registry packages.
func check_local_globals(root: String, owner: String) -> R:
	var files := {}
	for name: String in DirAccess.get_files_at(root):
		files[name] = true
	var checked: R = check_globals(root, owner, files)
	if checked.e != null:
		return checked
	for sub: String in DirAccess.get_directories_at(root):
		var nested: R = check_local_globals(root.path_join(sub), owner + "/" + sub)
		if nested.e != null:
			return nested
	return R.ok()


# Return the cache directory holding the loadable form of a stored package directory.
func ready_dir(cache_path: String) -> String:
	return "cache://pkg/_ready/" + cache_path.trim_prefix("cache://pkg/")


# Write the loadable form of verified cache files: scripts relocated to root, everything else as is.
func make_ready(cache_root: String, rels: PackedStringArray, root: String) -> R:
	var out_root := ready_dir(cache_root)
	for rel: String in rels:
		var src := GD.file.under(cache_root, rel)
		var dst := GD.file.under(out_root, rel)
		if src.is_empty() or dst.is_empty():
			return R.err("unsafe package path: %s" % rel, Err.PERMISSION_DENIED)
		var made: R = GD.file.make_dir(GD.file.dirname(dst))
		if made.e != null:
			return made
		var put: R
		if rel.ends_with(".gd"):
			var got: R = GD.file.read_text(src)
			if got.e != null:
				return got
			put = GD.file.write_text(dst, relocate_text(str(got.v), root))
		else:
			put = GD.file.copy(src, dst)
		if put.e != null:
			return put
	return R.ok()


# Relocate every script below a placed copy to the root it will be read from.
func relocate_tree(dir: String, root: String) -> R:
	var listing := DirAccess.open(dir)
	if listing == null:
		return R.err("cannot read %s" % dir, Err.PERMISSION_DENIED)
	listing.include_hidden = true
	if listing.list_dir_begin() != OK:
		return R.err("cannot list %s" % dir, Err.PERMISSION_DENIED)
	var names := PackedStringArray()
	var dirs := PackedStringArray()
	var name := listing.get_next()
	while not name.is_empty():
		if listing.current_is_dir():
			var _d := dirs.append(name)
		elif name.ends_with(".gd"):
			var _n := names.append(name)
		name = listing.get_next()
	listing.list_dir_end()
	for leaf: String in names:
		var path := GD.file.join([dir, leaf])
		var got: R = GD.file.read_text(path)
		if got.e != null:
			return got
		var put: R = GD.file.write_text(path, relocate_text(str(got.v), root))
		if put.e != null:
			return put
	for sub: String in dirs:
		var below: R = relocate_tree(GD.file.join([dir, sub]), root)
		if below.e != null:
			return below
	return R.ok()


# ---------------- Registry metadata ----------------

# Require complete versions without build metadata for package selection.
func package_version(raw: String) -> bool:
	if not GD.version.is_canonical(raw):
		return false
	var parsed: R = GD.version.parse(raw)
	# Confirm the parsed value is a Dictionary before checking build metadata.
	if not parsed.ok or not parsed.v is Dictionary:
		return false
	var version: Dictionary = parsed.v
	return str(version.get("build", "")).is_empty()


# Read available versions shaped as {"versions": {"1.2.0": {}}, "latest": "1.2.0"}.
func fetch_versions(pkg: String) -> R:
	var got: R = await fetch_meta(pkg)
	if got.e != null:
		return got
	var meta: Dictionary = got.v
	var vs: Dictionary = meta["versions"]
	var out: PackedStringArray = []
	for k: Variant in vs.keys():
		var _a: bool = out.append(str(k))
	return R.ok(out)


# Sort compatible versions once, preferring stable versions for an unrestricted query.
func ordered_versions(list: PackedStringArray, range_txt: String) -> PackedStringArray:
	var query := range_txt if not range_txt.is_empty() else "*"
	var matches: Array = []
	for raw: String in list:
		if not package_version(raw):
			continue
		var parsed: R = GD.version.parse(raw)
		var version: Dictionary = parsed.v
		if GD.version.satisfies(version, query):
			matches.append({"raw": raw, "version": version, "stable": GD.version.is_stable(version)})
	matches.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if query == "*" and a["stable"] != b["stable"]:
			return a["stable"]
		var left: Dictionary = a["version"]
		var right: Dictionary = b["version"]
		var order := GD.version.compare(left, right)
		return order > 0 if order != 0 else a["raw"] > b["raw"]
	)
	var out := PackedStringArray()
	for item: Dictionary in matches:
		var _added := out.append(str(item["raw"]))
	return out


# Select the first compatible version using the same policy as native backtracking.
func pick(list: PackedStringArray, range_txt: String) -> String:
	var versions := ordered_versions(list, range_txt)
	return versions[0] if not versions.is_empty() else ""


# ---------------- Installation ----------------

# Download one dependency into the project, select its version, and record it in the lockfile.
# owner names the package that imports it as @scope/name@version, or "" for the project;
# deps holds the resolved imports of the package itself, recorded beside its fingerprints.
func fetch_one(name: String, spec: String, cached_only: bool, frozen: bool, forced_version: String = "", replace_key: String = "", next_cfg: Dictionary = {}, owner: String = "", deps: Dictionary = {}) -> R:
	if not safe_segment(name) or name.begins_with("@"):
		return R.err("package name must be one safe path segment", Err.INVALID_DATA)
	var sp: Dictionary = parse_spec(spec)
	var kind: String = str(sp["kind"])
	if kind == "bad":
		return R.err("%s: cannot read \"%s\". Use gd:@scope/name@range, ext:@scope/name@range, https://... or ./path" % [name, spec], Err.INVALID_DATA)
	if not owner.is_empty() and kind != "gd" and kind != "ext":
		return R.err("%s imports %s from \"%s\"; packages may only import registry packages" % [owner, name, spec], Err.INVALID_DATA)
	if kind == "local":
		return R.ok(name) # Local dependencies need no download.

	var data: Dictionary = lock()
	if not json_ok(lock_path()):
		return R.err("cannot read gd.lock", Err.INVALID_DATA)
	var packages: Dictionary = data.get("packages", {})
	var locked: Dictionary = data.get("imports", {})

	# Honor an existing locked version when selecting what to install.
	var version: String = ""
	var url: String = str(sp["url"])
	var published: Dictionary = {}
	var contract: Dictionary = {}
	var key: String = name
	if kind == "gd" or kind == "ext":
		version = forced_version if not forced_version.is_empty() else pinned_version(name, owner, locked, packages)
		if version.is_empty():
			if frozen:
				return R.err("%s is not in gd.lock. Remove --frozen to add it." % name, Err.INVALID_DATA)
			if cached_only:
				return R.err("%s has no cached version in gd.lock" % name, Err.NOT_FOUND)
		# The lockfile is the contract once it holds the version; read the registry only to learn one.
		if not cached_only and (version.is_empty() or not packages.has(str(sp["pkg"]) + "@" + version)):
			var meta_got: R = await fetch_meta(str(sp["pkg"]))
			if meta_got.e != null:
				return meta_got
			var meta: Dictionary = meta_got.v
			var versions: Dictionary = meta["versions"]
			if version.is_empty():
				var list := PackedStringArray()
				for raw: Variant in versions:
					var _added: bool = list.append(str(raw))
				version = pick(list, str(sp["range"]))
				if version.is_empty():
					return R.err("%s: no version matches \"%s\"." % [name, sp["range"]], Err.NOT_FOUND)
			var raw_mark: Variant = versions.get(version, {})
			if not raw_mark is Dictionary:
				return R.err("%s@%s has no registry fingerprint" % [sp["pkg"], version], Err.INVALID_DATA)
			published = raw_mark
		key = str(sp["pkg"]) + "@" + version
		var locked_version := {}
		if packages.has(key):
			var checked_lock: R = registry_version(packages[key])
			if checked_lock.e != null:
				return R.err("%s has broken version metadata in gd.lock" % name, Err.INVALID_DATA)
			locked_version = checked_lock.v
		if not published.is_empty() and not locked_version.is_empty() and published != locked_version:
			return R.err("%s registry version differs from gd.lock" % name, Err.INVALID_DATA)
		contract = published if not published.is_empty() else locked_version
		if contract.is_empty():
			return R.err("%s has no verified version in gd.lock" % name, Err.INVALID_DATA)
		var leaf := str(contract["leaf"])
		if (kind == "gd" and leaf != "mod.gd") or (kind == "ext" and not leaf.ends_with(".gdextension")):
			return R.err("%s has the wrong package kind" % name, Err.INVALID_DATA)
		url = "%s/%s/%s/%s" % [registry(), sp["pkg"], version, leaf]
	elif frozen:
		var frozen_mark: Dictionary = packages.get(key, {})
		if not sha256_text(str(frozen_mark.get("sha256", ""))):
			return R.err("%s has no verified fingerprint in gd.lock" % name, Err.INVALID_DATA)

	# Separate cache entries by source so different registries cannot collide.
	var source := registry() if not version.is_empty() else url
	var source_hash := GD.data.hex_encode(GD.data.sha256(source.to_utf8_buffer())).substr(0, 32)
	var store: String
	if version.is_empty():
		store = GD.file.join([cache_dir("_url", source_hash), name, "mod.gd"])
	else:
		store = GD.file.join([cache_dir("_registry", source_hash), str(sp["pkg"]), version, GD.file.basename(url, "")])

	var body: String = ""
	var registry_files: Dictionary = {}
	var expected_digest := ""
	var expected_size := -1
	if not contract.is_empty():
		expected_digest = str(contract["sha256"])
		expected_size = str(contract["size"]).to_int()
		registry_files = contract["files"]
		if kind == "ext" and expected_size > MANIFEST_MAX:
			return R.err("extension manifest exceeds the byte limit", Err.LIMITED)
	if not version.is_empty() and not sha256_text(expected_digest):
		return R.err("%s has no valid registry fingerprint" % name, Err.INVALID_DATA)
	if GD.file.exists(store):
		var stored_size: R = GD.file.size_of(store)
		var stored_max := MANIFEST_MAX if kind == "ext" else PACKAGE_MAX
		if stored_size.e != null or str(stored_size.v).to_int() > stored_max:
			return R.err("cached package exceeds the byte limit", Err.LIMITED)
	else:
		if cached_only:
			return R.err("%s is not in the cache. Drop --cached-only to fetch it." % name, Err.NOT_FOUND)
		if not secure_url(url):
			return R.err("remote packages must use HTTPS", Err.PERMISSION_DENIED)
		var fetch_max := MANIFEST_MAX if kind == "ext" else PACKAGE_MAX
		var _mk_store: R = GD.file.make_dir(GD.file.dirname(store))
		if version.is_empty():
			var fetch_opts := {"max_body": fetch_max, "save": store}
			if not expected_digest.is_empty():
				fetch_opts["sha256"] = expected_digest
			var res, _res_err := await GD.http.fetch(url, fetch_opts)
			if not (_res_err == null and res.ok()):
				if _res_err: return R.err(_res_err.note("fetch package"))
				return R.err("%s: cannot fetch %s" % [name, url], Err.NOT_FOUND)
		else:
			var fetched: R = await fetch_registry_file(url, store, contract, fetch_max)
			if fetched.e != null:
				return fetched
	var body_max := MANIFEST_MAX if kind == "ext" else PACKAGE_MAX
	var actual_size: R = GD.file.size_of(store)
	if actual_size.e != null or str(actual_size.v).to_int() > body_max:
		return R.err("package entry exceeds the byte limit", Err.LIMITED)
	if expected_size >= 0 and str(actual_size.v).to_int() != expected_size:
		return R.err("package entry size differs from the registry", Err.INVALID_DATA)
	if kind == "ext":
		var got: R = GD.file.read_text(store)
		if got.e != null:
			return got
		body = str(got.v)

	# Reject fingerprint mismatches.
	var entry_hash: R = file_sha256(store, str(actual_size.v).to_int())
	if entry_hash.e != null:
		return entry_hash
	var digest: String = str(entry_hash.v)
	if not expected_digest.is_empty() and expected_digest != digest:
		var _drop_bad: R = GD.file.remove(store)
		return R.err("%s does not match its verified fingerprint" % name, Err.INVALID_DATA)
	if packages.has(key):
		var entry: Dictionary = packages[key]
		var want: String = str(entry.get("sha256", ""))
		if not want.is_empty() and want != digest:
			return R.err("%s does not match gd.lock. Remove the entry to accept the new content." % name, Err.INVALID_DATA)
	elif frozen:
		return R.err("%s is not in gd.lock. Remove --frozen to add it." % name, Err.INVALID_DATA)
	# Keep pure script packages in the shared cache; copy under pkg/ for native
	# extensions, whose loader needs real files, and when gd.json places packages in the project.
	# The project's own imports are copied as pkg/<name>/; packages only other packages need
	# take their canonical id, pkg/@scope/name@version/, so versions never collide.
	var place := kind == "ext" or place_project()
	if kind == "ext":
		var reserved: R = check_native(store, key)
		if reserved.e != null:
			return reserved
	var dir := name if owner.is_empty() else alias_of(key, config(), locked)
	if dir.is_empty():
		dir = key
	var out_path := GD.file.under(GD.file.under(pkg_dir(), dir), GD.file.basename(url, "") if kind == "ext" else "mod.gd")
	if out_path.is_empty():
		return R.err("package output path leaves pkg", Err.PERMISSION_DENIED)
	var nonce := Crypto.new().generate_random_bytes(16).hex_encode()
	var stage_root := GD.file.join([pkg_dir(), ".stage-" + nonce]) if place else ""
	var next_path := GD.file.join([stage_root, GD.file.basename(out_path, "")]) if place else ""
	if place:
		var _mk_stage: R = GD.file.make_dir(stage_root)
		var put: R = GD.file.write_text(next_path, body) if kind == "ext" else GD.file.copy(store, next_path)
		if put.e != null:
			var _drop_put: R = GD.file.remove_all(stage_root)
			return put
		# Revalidate the installed snapshot to keep concurrent cache edits out of the lockfile.
		var placed_size: R = GD.file.size_of(next_path)
		if placed_size.e != null or str(placed_size.v).to_int() != str(actual_size.v).to_int():
			var _drop_size: R = GD.file.remove_all(stage_root)
			return R.err("placed package changed while it was being verified", Err.INVALID_DATA)
		var placed_hash: R = file_sha256(next_path, str(placed_size.v).to_int())
		if placed_hash.e != null or str(placed_hash.v) != digest:
			var _drop_hash: R = GD.file.remove_all(stage_root)
			return R.err("placed package changed while it was being verified", Err.INVALID_DATA)

	# Verify every script-package file in the cache, staging copies only when placing.
	if kind == "gd":
		var rels := PackedStringArray()
		for raw_rel: Variant in registry_files:
			var rel := str(raw_rel)
			if rel != str(contract["leaf"]):
				var _rel := rels.append(rel)
		var placed_files: R = await fetch_files(
				stage_root, GD.file.dirname(url), GD.file.dirname(store), registry_files,
				rels, cached_only, str(actual_size.v).to_int()
		)
		if placed_files.e != null:
			if place:
				var _drop_files: R = GD.file.remove_all(stage_root)
			return placed_files
	if kind == "ext":
		var libs: R = await fetch_libs(next_path, url, GD.file.dirname(store), registry_files, cached_only)
		if libs.e != null:
			var _drop_libs: R = GD.file.remove_all(stage_root)
			return libs
	if kind == "gd":
		var globals: R = check_globals(GD.file.dirname(store), key, registry_files)
		if globals.e != null:
			if place:
				var _drop_globals: R = GD.file.remove_all(stage_root)
			return globals
	# Scripts keep meaning their own root: relocate the loadable cache form to the canonical id,
	# and a placed copy to where it sits under pkg/.
	if kind == "gd" or kind == "url":
		var rels := PackedStringArray([GD.file.basename(store, "")])
		for raw_rel: Variant in registry_files:
			if str(raw_rel) != rels[0]:
				var _r := rels.append(str(raw_rel))
		var ready: R = make_ready(GD.file.dirname(store), rels, "pkg://" + key)
		if ready.e != null:
			if place:
				var _drop_ready: R = GD.file.remove_all(stage_root)
			return ready
		if place:
			var relocated: R = relocate_tree(stage_root, "res://pkg/" + dir)
			if relocated.e != null:
				var _drop_relocated: R = GD.file.remove_all(stage_root)
				return relocated
	var final_path := GD.file.dirname(out_path) if place else ""
	if place:
		var final_dir: R = GD.file.make_dir(GD.file.dirname(final_path))
		if final_dir.e != null:
			var _drop_dir: R = GD.file.remove_all(stage_root)
			return final_dir

	# Publish the verified snapshot and lockfile in one project transaction.
	if not version.is_empty() and owner.is_empty():
		locked[name] = version
	var new_entry := {"sha256": digest, "url": url}
	if not version.is_empty():
		new_entry["size"] = str(actual_size.v).to_int()
		new_entry["files"] = registry_files
		new_entry["imports"] = deps # Exact resolved dependency edges.
		new_entry["requires"] = contract["imports"] # Immutable declared ranges.
	if not replace_key.is_empty():
		var _drop_replaced: bool = packages.erase(replace_key)
	packages[key] = new_entry
	data["imports"] = locked
	data["packages"] = packages
	data["registry"] = registry()
	var backup_path := final_path + "." + nonce + ".old" if place else ""
	var had_final := place and GD.file.exists(final_path)
	var before := ""
	var after := ""
	if place:
		var before_fingerprint: R = path_fingerprint(final_path, true)
		var after_fingerprint: R = path_fingerprint(stage_root, true)
		if before_fingerprint.e != null or after_fingerprint.e != null:
			var _drop_fingerprint: R = GD.file.remove_all(stage_root)
			return before_fingerprint if before_fingerprint.e != null else after_fingerprint
		before = str(before_fingerprint.v)
		after = str(after_fingerprint.v)
	var lock_snap: R = json_change(lock_path(), data)
	if lock_snap.e != null:
		if place:
			var _drop_snap: R = GD.file.remove_all(stage_root)
		return lock_snap
	var text_snaps: Array = [lock_snap.v]
	if not next_cfg.is_empty():
		var cfg_snap: R = json_change(config_path(), next_cfg)
		if cfg_snap.e != null:
			if place:
				var _drop_cfg_snap: R = GD.file.remove_all(stage_root)
			return cfg_snap
		text_snaps.append(cfg_snap.v)
	if kind == "ext":
		var ext_snap: R = extension_change(out_path, true)
		if ext_snap.e != null:
			var _drop_ext_snap: R = GD.file.remove_all(stage_root)
			return ext_snap
		text_snaps.append(ext_snap.v)
	var started: R = begin_txn({
		"final": final_path, "backup": backup_path, "stage": stage_root,
		"tree": true,
		"had_final": had_final, "before": before, "after": after,
		"text": text_snaps,
	})
	if started.e != null:
		if place:
			var _drop_txn: R = GD.file.remove_all(stage_root)
		return started
	if had_final:
		var backed: R = GD.file.rename(final_path, backup_path)
		if backed.e != null:
			return fail_txn(backed)
	if place:
		var placed: R = GD.file.rename(stage_root, final_path)
		if placed.e != null:
			return fail_txn(placed)
	for raw_snap: Variant in text_snaps:
		var snap: Dictionary = raw_snap
		var changed: R = apply_text(snap)
		if changed.e != null:
			return fail_txn(changed)
	var finished: R = end_txn()
	if finished.e != null:
		return finished
	if had_final and not graph_active:
		var drop_old: R = GD.file.remove_all(backup_path)
		if drop_old.e != null:
			note("cannot remove old package: %s" % backup_path, "warn")

	print("%s %s (%d bytes, sha256 %s)" % [name, version if not version.is_empty() else url, str(actual_size.v).to_int(), digest.substr(0, 12)])
	return R.ok(version)


# Check whether every feature in a set matches the current environment.
func features_match(raw: Variant) -> bool:
	for tag: String in str(raw).strip_edges().trim_prefix('"').trim_suffix('"').split("."):
		if not OS.has_feature(tag):
			return false
	return true


# Parse inline ConfigFile dictionaries retained as strings by the INI parser.
func dependency_group(raw: Variant) -> Dictionary:
	if raw is Dictionary:
		return raw
	var parsed: R = decode_json(str(raw))
	return parsed.v if parsed.ok and parsed.v is Dictionary else {}


# Copy version-manifest-verified files from cache into the package tree.
func fetch_files(out_root: String, base_url: String, cache_root: String, published: Dictionary,
		rels: PackedStringArray, cached_only: bool, used: int) -> R:
	var total := used
	for rel: String in rels:
		var out_path := GD.file.under(out_root, rel) if not out_root.is_empty() else ""
		var cached_path := GD.file.under(cache_root, rel)
		if not safe_file_path(rel) or cached_path.is_empty() or (not out_root.is_empty() and out_path.is_empty()):
			return R.err("unsafe package path: %s" % rel, Err.PERMISSION_DENIED)
		var checked: R = registry_mark(published.get(rel), PACKAGE_MAX - total)
		if checked.e != null:
			return R.err("registry has no valid metadata for file: %s" % rel, Err.INVALID_DATA)
		var mark: Dictionary = checked.v
		var want_size := str(mark["size"]).to_int()
		var want_sha := str(mark["sha256"])
		var cached_ok := false
		if GD.file.exists(cached_path):
			var cached_size: R = GD.file.size_of(cached_path)
			if cached_size.e != null or str(cached_size.v).to_int() > PACKAGE_MAX - total:
				return R.err("package exceeds the total byte limit", Err.LIMITED)
			if str(cached_size.v).to_int() == want_size:
				var cached_hash: R = file_sha256(cached_path, want_size)
				if cached_hash.e != null:
					return cached_hash
				cached_ok = str(cached_hash.v) == want_sha
		if cached_only and not cached_ok:
			if GD.file.exists(cached_path):
				return R.err("cached file does not match gd.lock: %s" % rel, Err.INVALID_DATA)
			return R.err("%s is not in the cache. Drop --cached-only to fetch it." % rel, Err.NOT_FOUND)
		if not cached_ok:
			var made_cache: R = GD.file.make_dir(GD.file.dirname(cached_path))
			if made_cache.e != null:
				return made_cache
			var fetched: R = await fetch_registry_file(base_url + "/" + rel.uri_encode(), cached_path, mark, PACKAGE_MAX - total)
			if fetched.e != null:
				return fetched
		total += want_size
		if total > PACKAGE_MAX:
			return R.err("package exceeds the total byte limit", Err.LIMITED)
		if out_root.is_empty():
			continue
		var made_out: R = GD.file.make_dir(GD.file.dirname(out_path))
		if made_out.e != null:
			return made_out
		var copied: R = GD.file.copy(cached_path, out_path)
		if copied.e != null:
			return copied
		var placed_size: R = GD.file.size_of(out_path)
		if placed_size.e != null or str(placed_size.v).to_int() != want_size:
			return R.err("cached file changed while it was copied: %s" % rel, Err.INVALID_DATA)
		var placed_hash: R = file_sha256(out_path, want_size)
		if placed_hash.e != null or str(placed_hash.v) != want_sha:
			return R.err("cached file changed while it was copied: %s" % rel, Err.INVALID_DATA)
	return R.ok(total)


# Download the current-platform extension binary and its dependent libraries.
func fetch_libs(manifest_path: String, manifest_url: String, cache_root: String, published: Dictionary, cached_only: bool) -> R:
	var txt: R = GD.file.read_text(manifest_path)
	if txt.e != null:
		return txt
	if str(txt.v).to_utf8_buffer().size() > MANIFEST_MAX:
		return R.err(".gdextension exceeds the manifest limit", Err.LIMITED)
	var ini: R = GD.data.ini(str(txt.v))
	if ini.e != null or not ini.v is Dictionary:
		return R.err("broken .gdextension", Err.INVALID_DATA)
	var doc: Dictionary = ini.v
	if not doc.get("libraries") is Dictionary:
		return R.err("%s has no [libraries]" % GD.file.basename(manifest_path, ""), Err.INVALID_DATA)

	# Select the matching binary with the greatest number of feature qualifiers.
	var rels := PackedStringArray()
	var best := -1
	var libs: Dictionary = doc["libraries"]
	for raw_key: Variant in libs:
		var tags := str(raw_key).strip_edges().trim_prefix('"').trim_suffix('"').split(".")
		if features_match(raw_key) and tags.size() > best:
			best = tags.size()
			rels = PackedStringArray([str(libs[raw_key]).strip_edges().trim_prefix('"').trim_suffix('"')])
	if rels.is_empty():
		return R.err("no library for %s in %s" % [platform_tag(), GD.file.basename(manifest_path, "")], Err.NOT_FOUND)

	# Include every dependency from the first matching feature set.
	if doc.get("dependencies") is Dictionary:
		var dependencies: Dictionary = doc["dependencies"]
		for raw_key: Variant in dependencies:
			var group := dependency_group(dependencies[raw_key])
			if features_match(raw_key) and not group.is_empty():
				for raw_rel: Variant in group:
					var rel := str(raw_rel).strip_edges().trim_prefix('"').trim_suffix('"')
					if not rels.has(rel):
						var _add := rels.append(rel)
	var folded := {}
	for rel: String in rels:
		if not safe_file_path(rel):
			return R.err("unsafe library path: %s" % rel, Err.PERMISSION_DENIED)
		if not add_portable_path(rel, folded):
			return R.err("colliding library path: %s" % rel, Err.INVALID_DATA)

	# Verify current-platform files against the shared cross-platform version contract.
	return await fetch_files(
			GD.file.dirname(manifest_path), GD.file.dirname(manifest_url), cache_root,
			published, rels, cached_only, str(txt.v).to_utf8_buffer().size()
	)


# Build before and after snapshots of the native-extension registry.
func extension_change(manifest_path: String, add: bool) -> R:
	var dir: String = GD.file.join([base_dir(), ".godot"])
	var _mk: R = GD.file.make_dir(dir)
	var list_path: String = GD.file.join([dir, "extension_list.cfg"])
	var line: String = "res://" + manifest_path.trim_prefix(base_dir()).trim_prefix("/")
	var body: String = ""
	if GD.file.exists(list_path):
		var got: R = GD.file.read_text(list_path)
		if got.e != null:
			return got
		body = str(got.v)
	var kept := PackedStringArray()
	for old: String in body.split("\n", false):
		if old.strip_edges() != line:
			var _keep := kept.append(old)
	if add:
		var _added: bool = kept.append(line)
	var after := "\n".join(kept) + ("\n" if not kept.is_empty() else "")
	return text_change(list_path, not after.is_empty() or GD.file.exists(list_path), after)


# Return platform feature tags matching .gdextension library keys.
func platform_tag() -> String:
	var os_name: String = OS.get_name().to_lower()
	var tag: String = "linux"
	if os_name == "macos":
		tag = "macos"
	elif os_name == "windows":
		tag = "windows"
	return tag + "." + Engine.get_architecture_name()


# ---------------- Local packages ----------------
# A local package is a checkout the developer edits in place. gd copies it under pkg/<alias>/ with
# its scripts relocated, like any other package, and copies again when its content snapshot changes.

# Mount the developer checkout and return its private read-only path.
func local_source(spec: String) -> String:
	var root := ProjectSettings.globalize_path("res://")
	var path := spec if spec.is_absolute_path() else root.path_join(spec).simplify_path()
	return GD.file._local(path)


# Skip what a checkout never ships: dotfiles, gd's own directories, links, and nested packages.
func local_skips(name: String, is_dir: bool, path: String) -> bool:
	if name.begins_with("."):
		return true
	if is_dir and (name == "pkg" or name == "tmp" or GD.file.exists(GD.file.join([path, "gd.json"]))):
		return true
	return false


# Copy one checkout tree into a staging directory, relocating scripts and dropping the token.
func copy_local(src: String, dst: String, root: String, top: bool) -> R:
	var listing := DirAccess.open(src)
	if listing == null:
		return R.err("cannot read local package: %s" % src, Err.NOT_FOUND)
	listing.include_hidden = true
	if listing.list_dir_begin() != OK:
		return R.err("cannot list local package: %s" % src, Err.PERMISSION_DENIED)
	var made: R = GD.file.make_dir(dst)
	if made.e != null:
		return made
	var name := listing.get_next()
	while not name.is_empty():
		var is_dir := listing.current_is_dir()
		var from := src.path_join(name)
		var to := GD.file.join([dst, name])
		if not listing.is_link(name) and not local_skips(name, is_dir, from):
			if is_dir:
				var below: R = copy_local(from, to, root, false)
				if below.e != null:
					return below
			elif name.ends_with(".gd") or (top and name == "gd.json"):
				var got: R = GD.file.read_text(from)
				if got.e != null:
					return got
				var text := str(got.v)
				if name == "gd.json":
					var doc: R = decode_json(text)
					if doc.e != null or not doc.v is Dictionary:
						return R.err("local package has a broken gd.json: %s" % from, Err.INVALID_DATA)
					var cfg: Dictionary = doc.v
					var _token: bool = cfg.erase("token")
					var encoded: R = encode_json(cfg, 0, true)
					if not encoded.ok:
						return R.err("cannot encode gd.json of %s" % src, Err.INVALID_DATA)
					text = str(encoded.v)
				else:
					text = relocate_text(text, root)
				var put: R = GD.file.write_text(to, text)
				if put.e != null:
					return put
			else:
				var copied: R = GD.file.copy(from, to)
				if copied.e != null:
					return copied
		name = listing.get_next()
	listing.list_dir_end()
	return R.ok()


# Place a local package under pkg/<alias>/ in one transaction, replacing an older copy.
func sync_local(alias: String, spec: String) -> R:
	var src := local_source(spec)
	if not GD.file.exists(src):
		return R.err("%s: no such directory: %s" % [alias, spec], Err.NOT_FOUND)
	var final_path := GD.file.under(pkg_dir(), alias)
	if final_path.is_empty():
		return R.err("package output path leaves pkg", Err.PERMISSION_DENIED)
	var nonce := Crypto.new().generate_random_bytes(16).hex_encode()
	var stage_root := GD.file.join([pkg_dir(), ".stage-" + nonce])
	var stamp := GD.file._local_stamp(src)
	if stamp.is_empty():
		return R.err("cannot fingerprint local package: %s" % spec, Err.INVALID_DATA)
	var copied: R = copy_local(src, stage_root, "res://pkg/" + alias, true)
	if copied.e != null:
		var _drop_copy: R = GD.file.remove_all(stage_root)
		return copied
	if GD.file._local_stamp(src) != stamp:
		var _drop_changed: R = GD.file.remove_all(stage_root)
		return R.err("local package changed while copying: %s" % spec, Err.INVALID_DATA)
	var stamped: R = GD.file.write_text(stage_root.path_join(".gd-source"), stamp)
	if stamped.e != null:
		var _drop_stamp: R = GD.file.remove_all(stage_root)
		return stamped
	var globals: R = check_local_globals(stage_root, "local:" + alias)
	if globals.e != null:
		var _drop_globals: R = GD.file.remove_all(stage_root)
		return globals
	var backup_path := final_path + "." + nonce + ".old"
	var had_final := GD.file.exists(final_path)
	var before_fingerprint: R = path_fingerprint(final_path, true)
	var after_fingerprint: R = path_fingerprint(stage_root, true)
	if before_fingerprint.e != null or after_fingerprint.e != null:
		var _drop_fingerprint: R = GD.file.remove_all(stage_root)
		return before_fingerprint if before_fingerprint.e != null else after_fingerprint
	var _parent: R = GD.file.make_dir(GD.file.dirname(final_path))
	var started: R = begin_txn({
		"final": final_path, "backup": backup_path, "stage": stage_root,
		"tree": true,
		"had_final": had_final, "before": before_fingerprint.v, "after": after_fingerprint.v,
		"text": [],
	})
	if started.e != null:
		var _drop_txn: R = GD.file.remove_all(stage_root)
		return started
	if had_final:
		var backed: R = GD.file.rename(final_path, backup_path)
		if backed.e != null:
			return fail_txn(backed)
	var placed: R = GD.file.rename(stage_root, final_path)
	if placed.e != null:
		return fail_txn(placed)
	var finished: R = end_txn()
	if finished.e != null:
		return finished
	if had_final and not graph_active:
		var drop_old: R = GD.file.remove_all(backup_path)
		if drop_old.e != null:
			note("cannot remove old package: %s" % backup_path, "warn")
	print("%s %s (local copy)" % [alias, spec])
	return R.ok(alias)


# ---------------- Dependency graph ----------------

# Split @scope/name@version into its package name.
func pkg_of_id(id: String) -> String:
	return id.substr(0, id.rfind("@"))


# Split @scope/name@version into its version.
func version_of_id(id: String) -> String:
	return id.substr(id.rfind("@") + 1)


# Return the version the lockfile pins for an import: the project's alias, or an owner's resolution.
func pinned_version(name: String, owner: String, locked: Dictionary, packages: Dictionary) -> String:
	if owner.is_empty():
		return str(locked.get(name, ""))
	var entry: Dictionary = packages.get(owner, {})
	var resolved: Dictionary = entry.get("imports", {})
	var dep := str(resolved.get(name, ""))
	return version_of_id(dep) if not dep.is_empty() else ""


# Return the smallest project alias pinned to a package id, matching runtime placement.
func alias_of(id: String, cfg: Dictionary, locked: Dictionary) -> String:
	var imports: Dictionary = cfg.get("imports", {})
	var aliases: Array = imports.keys()
	aliases.sort()
	for raw_alias: Variant in aliases:
		var alias := str(raw_alias)
		var sp := parse_spec(str(imports[raw_alias]))
		var kind := str(sp["kind"])
		if (kind == "gd" or kind == "ext") and str(sp["pkg"]) == pkg_of_id(id) \
				and str(locked.get(alias, "")) == version_of_id(id):
			return alias
	return ""


# Report whether a version text satisfies a range; an empty range accepts everything.
func satisfies_range(v: String, range_txt: String) -> bool:
	if range_txt.is_empty():
		return true
	var parsed: R = GD.version.parse(v)
	if not parsed.ok or not parsed.v is Dictionary:
		return false
	var version: Dictionary = parsed.v
	return GD.version.satisfies(version, range_txt)


# Prefer a valid lock, then an existing compatible version, then a registry candidate.
func choose(pkg: String, range_txt: String, pinned: String, chosen: Dictionary, cached_only: bool, frozen: bool, who: String) -> R:
	if not pinned.is_empty() and (range_txt.is_empty() or satisfies_range(pinned, range_txt)):
		return R.ok(pinned)
	if frozen:
		return R.err("%s is not in gd.lock or its range changed. Remove --frozen to resolve it." % who, Err.INVALID_DATA)
	var taken: PackedStringArray = chosen.get(pkg, PackedStringArray())
	for v: String in taken:
		if range_txt.is_empty() or satisfies_range(v, range_txt):
			return R.ok(v)
	if cached_only:
		return R.err("%s has no cached version in gd.lock" % who, Err.NOT_FOUND)
	var vs: R = await fetch_versions(pkg)
	if vs.e != null:
		return vs
	var list: PackedStringArray = vs.v
	var version := pick(list, range_txt)
	if version.is_empty():
		return R.err("%s: no version matches \"%s\"." % [who, range_txt], Err.NOT_FOUND)
	return R.ok(version)


# Resolve every package the project reaches, breadth first from gd.json, into an ordered list
# of nodes {owner, alias, spec, kind, version, id, deps}. Two packages may pin different
# versions of one pure package; a native extension loads once, so it gets one version.
# force maps a project alias to the version it must take, for gd update.
func resolve_graph(cfg: Dictionary, cached_only: bool, frozen: bool, force: Dictionary = {}) -> R:
	var pending: Array = [{}] # Unexplored singleton assignments, without a graph-size ceiling.
	var failure: R = R.err("no compatible native dependency graph", Err.INVALID_DATA)
	while not pending.is_empty():
		var bindings: Dictionary = pending.pop_back()
		var result: R = await walk_graph(cfg, cached_only, frozen, force, bindings)
		if result.e != null:
			failure = result
			continue
		if result.v is Array:
			return result
		var branch: Dictionary = result.v
		var versions: PackedStringArray = branch["versions"]
		versions.reverse()
		for version: String in versions:
			var next: Dictionary = bindings.duplicate()
			next[str(branch["package"])] = version
			pending.append(next)
	return failure


# Expand a graph for one set of singleton choices, or request the next choice.
func walk_graph(cfg: Dictionary, cached_only: bool, frozen: bool, force: Dictionary, bindings: Dictionary) -> R:
	var imports: Dictionary = cfg.get("imports", {})
	var data: Dictionary = lock()
	var packages: Dictionary = data.get("packages", {})
	var locked: Dictionary = data.get("imports", {})
	if data.has("registry") and str(data["registry"]) != registry():
		return R.err("registry differs from gd.lock; migrate the lock explicitly before using another source", Err.INVALID_DATA)
	for entry_id: Variant in packages:
		if str(entry_id).begins_with("@"):
			var entry: Dictionary = packages[entry_id]
			if not str(entry.get("url", "")).begins_with(registry() + "/"):
				return R.err("package source differs from gd.lock: %s" % entry_id, Err.INVALID_DATA)
	var chosen := {} # Package name to the versions selected so far.
	var native := {} # Native package name to {"version", "by"} for the one-version rule.
	var first := {} # Package id to the node that expanded it.
	var nodes: Array = []
	var queue: Array = []
	var aliases: Array = imports.keys()
	aliases.sort()
	for raw_alias: Variant in aliases:
		queue.append({"owner": "", "alias": str(raw_alias), "spec": str(imports[raw_alias]), "deps": {}})
	while not queue.is_empty():
		var node: Dictionary = queue.pop_front()
		var owner := str(node["owner"])
		var alias := str(node["alias"])
		var spec := str(node["spec"])
		var who := alias if owner.is_empty() else "%s (for %s)" % [alias, owner]
		if not safe_segment(alias) or alias.begins_with("@"):
			return R.err("%s: package name must be one safe path segment" % who, Err.INVALID_DATA)
		var sp := parse_spec(spec)
		var kind := str(sp["kind"])
		node["kind"] = kind
		node["version"] = ""
		node["id"] = ""
		if kind == "bad":
			return R.err("%s: cannot read \"%s\". Use gd:@scope/name@range, ext:@scope/name@range, https://... or ./path" % [who, spec], Err.INVALID_DATA)
		if kind == "local" or kind == "url":
			if not owner.is_empty():
				return R.err("%s imports %s from \"%s\"; packages may only import registry packages" % [owner, alias, spec], Err.INVALID_DATA)
			if kind == "local":
				var local_manifest := GD.file.join([local_source(spec), "gd.json"])
				var local_cfg: Dictionary = load_json(local_manifest, {})
				if not json_ok(local_manifest):
					return R.err("cannot read local package manifest: %s" % spec, Err.INVALID_DATA)
				node["id"] = "local:" + alias
				var local_imports: Dictionary = local_cfg.get("imports", {})
				var local_names: Array = local_imports.keys()
				local_names.sort()
				for child: Variant in local_names:
					queue.append({"owner": node["id"], "alias": str(child), "spec": str(local_imports[child]), "deps": {}, "parent_deps": node["deps"]})
				nodes.append(node)
				continue
			nodes.append(node)
			continue
		var pkg := str(sp["pkg"])
		var pinned := str(force.get(alias, "")) if owner.is_empty() and force.has(alias) \
				else pinned_version(alias, owner, locked, packages)
		var version := ""
		if kind == "ext" and not frozen:
			if not bindings.has(pkg):
				var candidates := PackedStringArray()
				if not pinned.is_empty() and satisfies_range(pinned, str(sp["range"])):
					var _pin := candidates.append(pinned)
				if not cached_only:
					var available: R = await fetch_versions(pkg)
					if available.e != null:
						return available
					var offered: PackedStringArray = available.v
					for candidate: String in ordered_versions(offered, str(sp["range"])):
						if candidate != pinned:
							var _candidate := candidates.append(candidate)
				if candidates.is_empty():
					return R.err("%s: no compatible native version" % who, Err.NOT_FOUND)
				return R.ok({"package": pkg, "versions": candidates})
			version = str(bindings[pkg])
			if not satisfies_range(version, str(sp["range"])):
				return R.err("%s requires %s@%s, incompatible with native %s" % [who, pkg, sp["range"], version], Err.INVALID_DATA)
		else:
			var picked: R = await choose(pkg, str(sp["range"]), pinned, chosen, cached_only, frozen, who)
			if picked.e != null:
				return picked
			version = str(picked.v)
		if owner.is_empty() and force.has(alias) and version != str(force[alias]):
			return R.err("%s cannot use requested version %s with the other native constraints" % [alias, force[alias]], Err.INVALID_DATA)
		if kind == "ext":
			if native.has(pkg) and str(native[pkg]["version"]) != version:
				return R.err("%s is needed at %s by %s and at %s by %s; a native extension loads once, so pick one range" % [
						pkg, native[pkg]["version"], native[pkg]["by"], version, who], Err.INVALID_DATA)
			native[pkg] = {"version": version, "by": who}
		var taken: PackedStringArray = chosen.get(pkg, PackedStringArray())
		if not taken.has(version):
			var _took := taken.append(version)
		chosen[pkg] = taken
		var id := pkg + "@" + version
		node["version"] = version
		node["id"] = id
		if not owner.is_empty():
			var parent_deps: Dictionary = node["parent_deps"]
			parent_deps[alias] = kind + ":" + id
		if first.has(id):
			if str(first[id]["kind"]) != kind:
				return R.err("%s is imported as both gd and ext" % id, Err.INVALID_DATA)
			node["deps"] = first[id]["deps"] # One id resolves once; later mentions share it.
			node["dup"] = true
			nodes.append(node)
			continue
		first[id] = node
		nodes.append(node)
		# Expand the package's own imports: the lockfile's resolutions when it has them,
		# else the ranges the registry recorded at publication.
		var children := {}
		if packages.has(id):
			var entry: Dictionary = packages[id]
			children = entry.get("requires", entry.get("imports", {}))
		else:
			var meta_got: R = await fetch_meta(pkg) if not cached_only else R.err("%s is not in gd.lock. Drop --cached-only to resolve it." % who, Err.NOT_FOUND)
			if meta_got.e != null:
				return meta_got
			var versions: Dictionary = meta_got.v["versions"]
			if not versions.has(version):
				return R.err("%s@%s is not in the registry" % [pkg, version], Err.NOT_FOUND)
			children = versions[version]["imports"]
		var child_names: Array = children.keys()
		child_names.sort()
		for raw_child: Variant in child_names:
			queue.append({"owner": id, "alias": str(raw_child), "spec": str(children[raw_child]), "deps": {}, "parent_deps": node["deps"]})
	return R.ok(nodes)


# Report whether a resolved node is already installed exactly as the lockfile says, so that
# gd add, remove, and update need not re-verify every package the way gd install does.
func fresh(node: Dictionary, locked: Dictionary, packages: Dictionary) -> bool:
	var id := str(node["id"])
	if id.is_empty() or not packages.has(id):
		return false
	var entry: Dictionary = packages[id]
	if entry.get("imports", {}) != node["deps"]:
		return false
	var owner := str(node["owner"])
	var alias := str(node["alias"])
	if owner.is_empty() and str(locked.get(alias, "")) != str(node["version"]):
		return false
	var kind := str(node["kind"])
	var leaf := GD.file.basename(str(entry.get("url", "")), "")
	if kind == "ext" or place_project():
		var dir := alias if owner.is_empty() else alias_of(id, config(), locked)
		if dir.is_empty():
			dir = id
		return GD.file.exists(GD.file.under(GD.file.under(pkg_dir(), dir), leaf))
	var source_hash := GD.data.hex_encode(GD.data.sha256(registry().to_utf8_buffer())).substr(0, 32)
	return GD.file.exists(GD.file.join([cache_dir("_registry", source_hash), pkg_of_id(id), str(node["version"]), leaf]))


# Remove a placed package directory, and its native registration, in one transaction.
func drop_dir(path: String, manifest: String) -> R:
	if not GD.file.exists(path):
		return R.ok()
	var backup := path + "." + Crypto.new().generate_random_bytes(16).hex_encode() + ".old"
	var before_fingerprint: R = path_fingerprint(path, true)
	if before_fingerprint.e != null:
		return before_fingerprint
	var text_snaps: Array = []
	if not manifest.is_empty():
		var ext_snap: R = extension_change(manifest, false)
		if ext_snap.e != null:
			return ext_snap
		text_snaps.append(ext_snap.v)
	var started: R = begin_txn({
		"final": path, "backup": backup, "stage": "",
		"tree": true,
		"had_final": true, "before": before_fingerprint.v, "after": "", "text": text_snaps,
	})
	if started.e != null:
		return started
	var moved: R = GD.file.rename(path, backup)
	if moved.e != null:
		return fail_txn(moved)
	for raw_snap: Variant in text_snaps:
		var snap: Dictionary = raw_snap
		var changed: R = apply_text(snap)
		if changed.e != null:
			return fail_txn(changed)
	var finished: R = end_txn()
	if finished.e != null:
		return finished
	if graph_active:
		return R.ok()
	var removed: R = GD.file.remove_all(backup)
	if removed.e != null:
		note("cannot remove old package: %s" % backup, "warn")
	# An id copy sits under its scope directory; drop the scope once it holds nothing else.
	var scope := GD.file.dirname(path)
	if GD.file.basename(scope, "").begins_with("@"):
		var left := DirAccess.get_directories_at(scope).size() + DirAccess.get_files_at(scope).size()
		if left == 0:
			var _scope_gone: R = GD.file.remove_all(scope)
	return R.ok()


# Drop lockfile entries and canonical copies that no resolved node reaches, and canonical
# copies of packages that a project alias now places under pkg/<alias>/.
func prune(cfg: Dictionary, nodes: Array, initial: Dictionary) -> R:
	var reached := {}
	for raw_node: Variant in nodes:
		var node: Dictionary = raw_node
		var id := str(node["id"])
		reached[id if not id.is_empty() else str(node["alias"])] = true
	var data: Dictionary = lock()
	if not json_ok(lock_path()):
		return R.err("cannot read gd.lock", Err.INVALID_DATA)
	var packages: Dictionary = data.get("packages", {})
	var locked: Dictionary = data.get("imports", {})
	var imports: Dictionary = cfg.get("imports", {})
	var changed := false
	# Remove superseded alias copies using the pre-install ownership snapshot.
	var dirs := {}
	for raw_node: Variant in nodes:
		var node: Dictionary = raw_node
		var kind := str(node["kind"])
		var id := str(node["id"])
		if kind == "local" or kind == "url":
			if kind == "local" or place_project():
				dirs[str(node["alias"])] = true
		elif kind == "ext" or place_project():
			var alias := alias_of(id, cfg, locked)
			dirs[alias if not alias.is_empty() else id] = true
	var old_requests: Dictionary = initial.get("requests", {})
	var old_imports: Dictionary = initial.get("imports", {})
	var old_packages: Dictionary = initial.get("packages", {})
	for raw_alias: Variant in old_requests:
		var alias := str(raw_alias)
		if dirs.has(alias):
			continue
		if not safe_segment(alias) or alias.begins_with("@"):
			return R.err("unsafe package alias in gd.lock", Err.INVALID_DATA)
		var spec := parse_spec(str(old_requests[alias]))
		var id := str(spec["pkg"]) + "@" + str(old_imports.get(alias, "")) if spec["kind"] in ["gd", "ext"] else alias
		var entry: Dictionary = old_packages.get(id, {})
		var leaf := str(entry.get("url", "")).get_file()
		var path := GD.file.under(pkg_dir(), alias)
		var dropped: R = drop_dir(path, path.path_join(leaf) if leaf.ends_with(".gdextension") else "")
		if dropped.e != null:
			return dropped
	for raw_key: Variant in packages.keys():
		var key := str(raw_key)
		var entry: Dictionary = packages[key]
		var leaf := GD.file.basename(str(entry.get("url", "")), "")
		var manifest_leaf := leaf if leaf.ends_with(".gdextension") else ""
		var canonical := GD.file.under(pkg_dir(), key) if key.begins_with("@") else ""
		if reached.has(key):
			# A copy under the id is stale once a project alias places the same package.
			if not canonical.is_empty() and not alias_of(key, cfg, locked).is_empty():
				var dropped_dup: R = drop_dir(canonical, GD.file.under(canonical, manifest_leaf) if not manifest_leaf.is_empty() else "")
				if dropped_dup.e != null:
					return dropped_dup
			continue
		if not canonical.is_empty():
			var dropped: R = drop_dir(canonical, GD.file.under(canonical, manifest_leaf) if not manifest_leaf.is_empty() else "")
			if dropped.e != null:
				return dropped
		var _gone: bool = packages.erase(key)
		changed = true
	for raw_alias: Variant in locked.keys():
		if not imports.has(raw_alias):
			var _unpinned: bool = locked.erase(raw_alias)
			changed = true
	if not changed:
		return R.ok()
	data["packages"] = packages
	data["imports"] = locked
	var lock_snap: R = json_change(lock_path(), data)
	if lock_snap.e != null:
		return lock_snap
	var started: R = begin_txn({"final": "", "backup": "", "stage": "", "tree": true,
			"had_final": false, "before": "", "after": "", "text": [lock_snap.v]})
	if started.e != null:
		return started
	var snap: Dictionary = lock_snap.v
	var applied: R = apply_text(snap)
	if applied.e != null:
		return fail_txn(applied)
	return end_txn()


# Resolve the graph, then fetch and place every node in order and prune what nothing reaches.
# verify re-checks packages the lockfile already covers, as gd install does.
func install_graph(cfg: Dictionary, cached_only: bool, frozen: bool, verify: bool, force: Dictionary = {}) -> R:
	var resolved: R = await resolve_graph(cfg, cached_only, frozen, force)
	if resolved.e != null:
		return resolved
	if frozen:
		var pinned: Dictionary = lock()
		if pinned.get("requests", {}) != cfg.get("imports", {}):
			return R.err("package requests differ from gd.lock; remove --frozen to resolve them", Err.INVALID_DATA)
		var expected_imports := {}
		var expected_packages := {}
		var entries: Dictionary = pinned.get("packages", {})
		for raw_node: Variant in resolved.v:
			var node: Dictionary = raw_node
			var id := str(node["id"])
			if id.is_empty():
				id = str(node["alias"])
			expected_packages[id] = true
			var entry: Dictionary = entries.get(id, {})
			if str(node["kind"]) == "local" and entry.get("source", "") != node["spec"]:
				return R.err("local package source differs from gd.lock", Err.INVALID_DATA)
			if not entries.has(id) or entry.get("imports", {}) != node["deps"]:
				return R.err("dependency graph differs from gd.lock; remove --frozen to resolve it", Err.INVALID_DATA)
			if str(node["owner"]).is_empty() and not str(node["version"]).is_empty():
				expected_imports[str(node["alias"])] = node["version"]
		if expected_imports != pinned.get("imports", {}) or expected_packages.size() != entries.size() or not pinned.has("registry"):
			return R.err("dependency graph differs from gd.lock; remove --frozen to resolve it", Err.INVALID_DATA)
	graph_active = true
	graph_frozen = frozen
	graph_steps = []
	global_names = {}
	var nodes: Array = resolved.v
	var installed: R = await place_graph(cfg, nodes, cached_only, frozen, verify)
	if installed.e == null and cfg != config():
		var snap: R = json_change(config_path(), cfg)
		if snap.e != null:
			installed = snap
		else:
			installed = begin_txn({"final": "", "backup": "", "stage": "", "tree": true,
					"had_final": false, "before": "", "after": "", "text": [snap.v]})
			if installed.e == null:
				var change: Dictionary = snap.v
				installed = apply_text(change)
	graph_active = false
	graph_frozen = false
	if installed.e != null:
		return fail_txn(installed)
	if GD.file.exists(txn_path()):
		var committed: R = end_txn()
		if committed.e != null:
			return fail_txn(committed)
	for raw_step: Variant in graph_steps:
		var step: Dictionary = raw_step
		var backup := str(step["backup"])
		if not backup.is_empty() and GD.file.exists(backup):
			var dropped: R = GD.file.remove_all(backup)
			if dropped.e != null:
				note("cannot remove old package: %s" % backup, "warn")
		var scope := GD.file.dirname(str(step["final"]))
		if GD.file.basename(scope, "").begins_with("@") and GD.file.exists(scope):
			if DirAccess.get_directories_at(scope).is_empty() and DirAccess.get_files_at(scope).is_empty():
				var _empty_scope: R = GD.file.remove_all(scope)
	return R.ok()


# Install every resolved node while retaining undo records for the graph owner.
func place_graph(cfg: Dictionary, nodes: Array, cached_only: bool, frozen: bool, verify: bool) -> R:
	var initial := lock()
	for raw_node: Variant in nodes:
		var node: Dictionary = raw_node
		if node.get("dup", false):
			continue
		if str(node["kind"]) == "local":
			var synced: R = sync_local(str(node["alias"]), str(node["spec"]))
			if synced.e != null:
				return synced
			continue
		if not verify:
			var prior: Dictionary = lock()
			var prior_imports: Dictionary = prior.get("imports", {})
			var prior_packages: Dictionary = prior.get("packages", {})
			if fresh(node, prior_imports, prior_packages):
				if str(node["kind"]) == "ext":
					var id := str(node["id"])
					var dir := alias_of(id, cfg, prior_imports)
					var root := pkg_dir().path_join(dir if not dir.is_empty() else id)
					var reserved: R = check_native(root.path_join(str(prior_packages[id]["url"]).get_file()), id)
					if reserved.e != null:
						return reserved
				if str(node["kind"]) == "gd":
					var key := str(node["id"])
					var source_hash := registry().sha256_text().substr(0, 32)
					var root := GD.file.join([cache_dir("_registry", source_hash), pkg_of_id(key), version_of_id(key)])
					var files: Dictionary = prior_packages[key]["files"]
					var globals: R = check_globals(root, key, files)
					if globals.e != null:
						return globals
				continue
		var deps: Dictionary = node["deps"]
		var got: R = await fetch_one(str(node["alias"]), str(node["spec"]), cached_only, frozen,
				str(node["version"]), "", {}, str(node["owner"]), deps)
		if got.e != null:
			return got
	var data: Dictionary = lock()
	var locked: Dictionary = data.get("imports", {})
	for raw_node: Variant in nodes:
		var node: Dictionary = raw_node
		if str(node["owner"]).is_empty() and not str(node["version"]).is_empty():
			locked[str(node["alias"])] = str(node["version"])
	data["imports"] = locked
	var packages: Dictionary = data.get("packages", {})
	for raw_node: Variant in nodes:
		var node: Dictionary = raw_node
		if str(node["kind"]) == "local":
			packages[str(node["id"])] = {"imports": node["deps"], "source": node["spec"]}
	data["packages"] = packages
	data["registry"] = registry()
	var requests: Dictionary = cfg.get("imports", {})
	data["requests"] = requests.duplicate()
	var snap: R = json_change(lock_path(), data)
	if snap.e != null:
		return snap
	var started: R = begin_txn({"final": "", "backup": "", "stage": "", "tree": true,
			"had_final": false, "before": "", "after": "", "text": [snap.v]})
	if started.e != null:
		return started
	var change: Dictionary = snap.v
	var applied: R = apply_text(change)
	if applied.e != null:
		return applied
	return prune(cfg, nodes, initial)


# ---------------- Subcommands ----------------

func cmd_install(args: PackedStringArray) -> int:
	var pkg_guard: R = package_lock()
	if pkg_guard.e != null:
		note(pkg_guard.e.msg)
		return 1
	var cached_only: bool = args.has("--cached-only")
	var frozen: bool = args.has("--frozen")
	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	if not place_ok():
		note('gd.json の place は "cache" か "project" にすること')
		return 1
	var _lock_data: Dictionary = lock()
	if not json_ok(lock_path()):
		return 1
	var imports: Dictionary = cfg.get("imports", {})
	if imports.is_empty() and _lock_data.is_empty():
		print("no imports in gd.json")
		return 0
	# --sync is what a run uses to catch up: fetch what is missing and copy local packages that changed.
	var r: R = await install_graph(cfg, cached_only, frozen, not args.has("--sync"))
	if r.e != null:
		note(r.e.msg)
		return 1
	return 0


func cmd_add(args: PackedStringArray) -> int:
	var pkg_guard: R = package_lock()
	if pkg_guard.e != null:
		note(pkg_guard.e.msg)
		return 1
	var rest: PackedStringArray = plain(args)
	if rest.is_empty():
		print("usage: gd add <@scope/name[@range]> | gd add <name> <url>")
		return 1
	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	if not place_ok():
		note('gd.json の place は "cache" か "project" にすること')
		return 1
	var _lock_data: Dictionary = lock()
	if not json_ok(lock_path()):
		return 1
	var imports: Dictionary = cfg.get("imports", {})

	var key: String = ""
	var spec: String = ""
	if rest.size() >= 2:
		key = rest[0] # Accept separately supplied alias and URL.
		spec = rest[1]
	else:
		var given: Dictionary = parse_spec(rest[0])
		if str(given["kind"]) == "bad":
			print("usage: gd add <@scope/name[@range]> | gd add <name> <url|path>")
			return 1
		if str(given["kind"]) == "local":
			# Name the checkout the way its own gd.json does, or after its directory.
			var local_cfg: Dictionary = load_json(GD.file.join([local_source(str(given["url"])), "gd.json"]), {})
			var declared := str(local_cfg.get("name", ""))
			key = short_name(declared if safe_package(declared) else str(given["url"]).trim_suffix("/").get_file())
		else:
			key = short_name(str(given["pkg"]))
		spec = spec_text(given)
	if not safe_segment(key):
		note("package name must be one safe path segment")
		return 1
	var unusable := alias_usable(key)
	if not unusable.is_empty():
		note(unusable)
		return 1

	var was_pinned: Dictionary = lock().get("imports", {})
	imports[key] = spec
	cfg["imports"] = imports
	var kind := str(parse_spec(spec)["kind"])
	# Take a fresh version for a changed range rather than the alias's old pin.
	var force := {}
	var sp := parse_spec(spec)
	var old := str(was_pinned.get(key, ""))
	if kind != "local" and not old.is_empty() and not str(sp["range"]).is_empty() and not satisfies_range(old, str(sp["range"])):
		var picked: R = await choose(str(sp["pkg"]), str(sp["range"]), "", {}, false, false, key)
		if picked.e != null:
			note(picked.e.msg)
			return 1
		force[key] = str(picked.v)
	var r: R = await install_graph(cfg, false, false, false, force)
	if r.e != null:
		note(r.e.msg)
		return 1
	return 0


func cmd_remove(args: PackedStringArray) -> int:
	var pkg_guard: R = package_lock()
	if pkg_guard.e != null:
		note(pkg_guard.e.msg)
		return 1
	var rest: PackedStringArray = plain(args)
	if rest.is_empty():
		print("usage: gd remove <name>")
		return 1
	var target: String = rest[0]
	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	var imports: Dictionary = cfg.get("imports", {})

	# Accept either an alias or a package name.
	var key: String = target
	if not imports.has(key):
		for k: Variant in imports.keys():
			var sp: Dictionary = parse_spec(str(imports[k]))
			if str(sp["pkg"]) == target:
				key = str(k)
				break
	if not imports.has(key):
		print("%s is not in gd.json" % target)
		return 1
	if not safe_segment(key):
		note("package name must be one safe path segment")
		return 1

	var sp2: Dictionary = parse_spec(str(imports[key]))
	var data: Dictionary = lock()
	if not json_ok(lock_path()):
		return 1
	var locked: Dictionary = data.get("imports", {})
	var packages: Dictionary = data.get("packages", {})
	var version: String = str(locked.get(key, ""))

	# Back up published files and update configuration with rollback available.
	var is_ext := str(sp2["kind"]) == "ext"
	var out_path := GD.file.under(pkg_dir(), key)
	var package_key := key if version.is_empty() else str(sp2["pkg"]) + "@" + version
	var manifest := ""
	if is_ext:
		var checked_version: R = registry_version(packages.get(package_key))
		if checked_version.e != null:
			note("%s has broken version metadata in gd.lock" % key)
			return 1
		manifest = GD.file.under(out_path, str(checked_version.v["leaf"]))
	if out_path.is_empty() or (is_ext and manifest.is_empty()):
		note("package output path leaves pkg")
		return 1
	var backup := out_path + "." + Crypto.new().generate_random_bytes(16).hex_encode() + ".old"
	var had_file := GD.file.exists(out_path)
	var before_fingerprint: R = path_fingerprint(out_path, true)
	if before_fingerprint.e != null:
		note(before_fingerprint.e.msg)
		return 1
	var _e1: bool = imports.erase(key)
	cfg["imports"] = imports
	var _e2: bool = locked.erase(key)
	if version.is_empty():
		var _e3: bool = packages.erase(package_key) # Url packages belong to this alias alone.
	data["imports"] = locked
	data["packages"] = packages
	var cfg_snap: R = json_change(config_path(), cfg)
	var lock_snap: R = json_change(lock_path(), data)
	if cfg_snap.e != null or lock_snap.e != null:
		note((cfg_snap if cfg_snap.e != null else lock_snap).e.msg)
		return 1
	var text_snaps: Array = [cfg_snap.v, lock_snap.v]
	if is_ext:
		var ext_snap: R = extension_change(manifest, false)
		if ext_snap.e != null:
			note(ext_snap.e.msg)
			return 1
		text_snaps.append(ext_snap.v)
	var started: R = begin_txn({
		"final": out_path, "backup": backup, "stage": "",
		"tree": true,
		"had_final": had_file, "before": before_fingerprint.v, "after": "", "text": text_snaps,
	})
	if started.e != null:
		note(started.e.msg)
		return 1
	if had_file:
		var moved: R = GD.file.rename(out_path, backup)
		if moved.e != null:
			note(moved.e.msg)
			var _recover_move: R = fail_txn(moved)
			return 1
	for raw_snap: Variant in text_snaps:
		var snap: Dictionary = raw_snap
		var changed: R = apply_text(snap)
		if changed.e != null:
			note(changed.e.msg)
			var _recover_text: R = fail_txn(changed)
			return 1

	var finished: R = end_txn()
	if finished.e != null:
		note(finished.e.msg)
		return 1
	# Remove backed-up files after configuration and lockfile commit.
	if had_file:
		var removed: R = GD.file.remove_all(backup)
		if removed.e != null:
			note("cannot remove old package: %s" % backup, "warn")
	print("removed %s" % key)
	# Other packages may still need what the alias named; place it under its id and drop the rest.
	var kept: R = await install_graph(cfg, false, false, false)
	if kept.e != null:
		note(kept.e.msg)
		return 1
	return 0


func cmd_outdated(_args: PackedStringArray) -> int:
	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	var imports: Dictionary = cfg.get("imports", {})
	var lock_data := lock()
	if not json_ok(lock_path()):
		return 1
	var locked: Dictionary = lock_data.get("imports", {})
	var behind: int = 0
	for k: Variant in imports.keys():
		var name: String = str(k)
		var sp: Dictionary = parse_spec(str(imports[k]))
		var kind: String = str(sp["kind"])
		if kind != "gd" and kind != "ext":
			continue # Dependencies without versions cannot be compared.
		var vs: R = await fetch_versions(str(sp["pkg"]))
		if vs.e != null:
			print("  %s  ? (%s)" % [name, vs.e.msg])
			continue
		var now: String = str(locked.get(name, "-"))
		var list: PackedStringArray = vs.v
		var in_range: String = pick(list, str(sp["range"]))
		var newest: String = pick(list, "*")
		if now != in_range or now != newest:
			behind += 1
			print("  %s  %s -> %s (latest %s)" % [name, now, in_range if not in_range.is_empty() else "-", newest])
	if behind == 0:
		print("all up to date")
	return 0


func cmd_update(args: PackedStringArray) -> int:
	var pkg_guard: R = package_lock()
	if pkg_guard.e != null:
		note(pkg_guard.e.msg)
		return 1
	var latest: bool = args.has("--latest")
	var only: String = ""
	var rest: PackedStringArray = plain(args)
	if not rest.is_empty():
		only = rest[0]

	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	if not place_ok():
		note('gd.json の place は "cache" か "project" にすること')
		return 1
	var imports: Dictionary = cfg.get("imports", {})
	var data: Dictionary = lock()
	if not json_ok(lock_path()):
		return 1
	var locked: Dictionary = data.get("imports", {})
	var next_cfg: Dictionary = cfg.duplicate(true)
	var next_imports: Dictionary = next_cfg.get("imports", {})
	var force := {} # Alias to the version it moves to.
	var failed: int = 0
	for k: Variant in imports.keys():
		var name: String = str(k)
		if not only.is_empty() and name != only:
			continue
		var sp: Dictionary = parse_spec(str(imports[k]))
		var kind: String = str(sp["kind"])
		if kind != "gd" and kind != "ext":
			continue
		var vs: R = await fetch_versions(str(sp["pkg"]))
		if vs.e != null:
			print("  %s  ? (%s)" % [name, vs.e.msg])
			failed += 1
			continue
		var list: PackedStringArray = vs.v
		var want: String = pick(list, "*") if latest else pick(list, str(sp["range"]))
		if want.is_empty() or want == str(locked.get(name, "")):
			continue
		if latest:
			sp["range"] = "^" + want
			next_imports[name] = spec_text(sp)
		force[name] = want
	if force.is_empty():
		if failed == 0:
			print("nothing to update")
		return 1 if failed > 0 else 0
	next_cfg["imports"] = next_imports
	# Move the chosen aliases and re-resolve what the new versions import, in one pass.
	var r: R = await install_graph(next_cfg, false, false, false, force)
	if r.e != null:
		note(r.e.msg)
		return 1
	return 1 if failed > 0 else 0


# Rank search results deterministically from matches for every query term.
func search_rank(item: Dictionary, query: String, words: PackedStringArray) -> int:
	var pkg := str(item.get("pkg", "")).to_lower()
	var short := pkg.get_slice("/", 1)
	var desc := str(item.get("description", "")).to_lower()
	for word: String in words:
		if not pkg.contains(word) and not desc.contains(word):
			return -1
	if pkg == query:
		return 500
	if short == query:
		return 450
	if pkg.begins_with(query) or short.begins_with(query):
		return 400
	if pkg.contains(query):
		return 300
	return 200


# Validate a single-line description safe for search-result display.
func safe_description(text: String) -> bool:
	for i: int in text.length():
		var c := text.unicode_at(i)
		var bidi := c == 0x061C or c == 0x200E or c == 0x200F or c == 0xFEFF \
				or (c >= 0x202A and c <= 0x202E) or (c >= 0x2066 and c <= 0x2069)
		if c < 32 or (c >= 127 and c <= 159) or c == 0x2028 or c == 0x2029 or bidi:
			return false
	return true


# Load the separate search catalog and perform multiword matching locally.
func cmd_search(args: PackedStringArray) -> int:
	var rest: PackedStringArray = plain(args)
	if rest.is_empty():
		print("usage: gd search <words...>")
		return 1
	var registry_url := registry()
	if not secure_url(registry_url):
		note("remote registries must use HTTPS")
		return 1
	var url: String = "%s/-/catalog.json" % registry_url
	var res, _res_err := await GD.http.fetch(url)
	if not (_res_err == null and res.ok()):
		note(_res_err.text() if _res_err else "cannot reach the registry: %s" % registry_url)
		return 1
	var decoded: R = res.json()
	if decoded.e != null or not decoded.v is Dictionary:
		note("broken answer from the registry")
		return 1
	var doc: Dictionary = decoded.v
	var raw_hits: Variant = doc.get("packages", [])
	if not raw_hits is Array:
		note("broken package catalog")
		return 1
	var catalog: Array = raw_hits
	var query := " ".join(rest).strip_edges().to_lower()
	var words := PackedStringArray(query.split(" ", false))
	var hits: Array = []
	for raw_hit: Variant in catalog:
		if not raw_hit is Dictionary:
			note("broken package catalog")
			return 1
		var item: Dictionary = raw_hit
		var kind := str(item.get("kind", ""))
		var pkg := str(item.get("pkg", ""))
		var latest := str(item.get("latest", ""))
		var raw_description: Variant = item.get("description", "")
		if not raw_description is String:
			note("broken package catalog")
			return 1
		var description: String = raw_description
		if not safe_description(description) or (kind != "gd" and kind != "ext") \
				or not safe_package(pkg) or not package_version(latest):
			note("broken package catalog")
			return 1
		var rank := search_rank(item, query, words)
		if rank >= 0:
			var hit := item.duplicate()
			hit["rank"] = rank
			hits.append(hit)
	hits.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if a["rank"] != b["rank"]:
			return a["rank"] > b["rank"]
		return str(a.get("pkg", "")) < str(b.get("pkg", ""))
	)
	var shown := 0
	for h: Variant in hits:
		var it: Dictionary = h
		var kind := str(it.get("kind", ""))
		var pkg := str(it.get("pkg", ""))
		var latest := str(it.get("latest", ""))
		var mark := "  [godot]" if it.get("godot", false) == true else ""
		print("  %s:%s@^%s  %s%s" % [kind, pkg, latest, it.get("description", ""), mark])
		shown += 1
	if shown == 0:
		print("no match")
	return 0


# Check each path component for symlinks from the trusted root onward.
func no_links(root: String, path: String) -> bool:
	var rel := GD.file.relative(root.simplify_path(), path.simplify_path())
	if rel.is_empty() or rel == ".." or rel.begins_with("../"):
		return false
	var current := root.simplify_path()
	for part: String in rel.split("/", false):
		var dir := DirAccess.open(current)
		if dir == null or dir.is_link(part):
			return false
		current = GD.file.join([current, part])
	return true


# Add a regular file by its package-root-relative name.
func add_pure_file(root: String, path: String, files: Dictionary, portable: Dictionary) -> R:
	var rel := GD.file.relative(root, path)
	if not no_links(base_dir(), path) or not FileAccess.file_exists(path):
		return R.err("includeは通常fileだけにすること: %s" % rel, Err.PERMISSION_DENIED)
	if not safe_file_path(rel) or not add_portable_path(rel, portable):
		return R.err("include pathがportableでない: %s" % rel, Err.INVALID_DATA)
	files[rel] = path
	return R.ok()


# Collect regular directory files without following symlinks.
func walk_pure(root: String, path: String, files: Dictionary, portable: Dictionary) -> R:
	var dir := DirAccess.open(path)
	if dir == null:
		return R.err("include directoryを読めない: %s" % path, Err.PERMISSION_DENIED)
	# Include ordinary dotfiles as package contents.
	dir.include_hidden = true
	if dir.list_dir_begin() != OK:
		return R.err("include directoryを列挙できない: %s" % path, Err.PERMISSION_DENIED)
	var items: Array = []
	var name := dir.get_next()
	while not name.is_empty():
		var is_dir := dir.current_is_dir()
		if not is_dir or not PackedStringArray([".bzr", ".git", ".hg", ".svn"]).has(name):
			items.append({"name": name, "dir": is_dir, "link": dir.is_link(name)})
		name = dir.get_next()
	dir.list_dir_end()
	items.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return a["name"] < b["name"])
	for raw_item: Variant in items:
		var item: Dictionary = raw_item
		var child := GD.file.join([path, str(item["name"])])
		if item["link"] == true:
			return R.err("includeにsymlinkは使えない: %s" % child, Err.PERMISSION_DENIED)
		var added: R = walk_pure(root, child, files, portable) if item["dir"] == true \
				else add_pure_file(root, child, files, portable)
		if added.e != null:
			return added
	return R.ok()


# Resolve explicit script-package includes into a tree relative to the main directory.
func pure_files(cfg: Dictionary, entry_path: String) -> R:
	if GD.file.basename(entry_path, "") != "mod.gd":
		return R.err("純GDScript packageのmainはmod.gdにすること", Err.INVALID_DATA)
	if not no_links(base_dir(), entry_path) or not FileAccess.file_exists(entry_path):
		return R.err("mainはsymlinkでない通常fileにすること", Err.PERMISSION_DENIED)
	var raw_include: Variant = cfg.get("include", [])
	if not raw_include is Array:
		return R.err("gd.json の include はpath配列にすること", Err.INVALID_DATA)
	var root := GD.file.dirname(entry_path.simplify_path())
	var files := {"mod.gd": entry_path.simplify_path()}
	var portable := {"mod.gd": {"path": "mod.gd", "file": true}}
	for raw_item: Variant in raw_include:
		if not raw_item is String or not safe_file_path(str(raw_item)):
			return R.err("includeに安全な相対pathを指定すること", Err.INVALID_DATA)
		var item := GD.file.join([base_dir(), str(raw_item)]).simplify_path()
		if not GD.file.exists(item):
			return R.err("includeが無い: %s" % raw_item, Err.NOT_FOUND)
		if not no_links(base_dir(), item):
			return R.err("includeにsymlinkは使えない: %s" % raw_item, Err.PERMISSION_DENIED)
		var from_root := GD.file.relative(root, item)
		var same_root := item.trim_suffix("/") == root.trim_suffix("/")
		if (from_root.is_empty() and not same_root) or from_root.begins_with("../") or from_root == "..":
			return R.err("includeはmain directory内にすること", Err.PERMISSION_DENIED)
		var dir := DirAccess.open(item)
		var added: R = walk_pure(root, item, files, portable) if dir != null \
				else add_pure_file(root, item, files, portable)
		if added.e != null:
			return added
	return R.ok(files)


# Publish the local entry point and native libraries to the registry.
func cmd_publish(args: PackedStringArray) -> int:
	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	var pkg: String = str(cfg.get("name", ""))
	var version: String = str(cfg.get("version", ""))
	var registry_url := registry()
	if not secure_url(registry_url):
		note("remote registries must use HTTPS")
		return 1
	if not safe_package(pkg):
		note("gd.json の name を @scope/name の形にすること")
		return 1
	if not package_version(version):
		note("gd.json の version はbuild metadataの無い完全なSemantic Versionにすること")
		return 1

	var entry: String = str(cfg.get("main", "mod.gd"))
	var entry_path := GD.file.under(base_dir(), entry)
	if entry_path.is_empty() or not GD.file.exists(entry_path):
		note("入口が無い: %s" % entry)
		return 1
	var native := entry.ends_with(".gdextension")
	if not native and not entry.ends_with(".gd"):
		note("入口は.gdまたは.gdextensionにすること")
		return 1
	if not no_links(base_dir(), entry_path) or not FileAccess.file_exists(entry_path):
		note("入口はsymlinkでない通常fileにすること")
		return 1
	var entry_size: R = GD.file.size_of(entry_path)
	if entry_size.e != null:
		note(entry_size.e.msg)
		return 1
	var measured := str(entry_size.v).to_int()
	if measured > PACKAGE_MAX:
		note("packageは500 MiBまで")
		return 1
	if native and measured > MANIFEST_MAX:
		note("入口は16 MiBまで")
		return 1
	var body := ""
	var entry_leaf := GD.file.basename(entry, "")
	var files: Dictionary = {entry_leaf: entry_path}
	var total: int = measured
	var measured_files := {entry_leaf: measured} # Pre-read sizes used to detect replacement after reading.
	var classes: Array = []
	if native:
		if cfg.has("include"):
			note("includeは純GDScript packageだけで使える")
			return 1
		var got: R = GD.file.read_text(entry_path)
		if got.e != null:
			note(got.e.msg)
			return 1
		body = str(got.v)
		var manifest_bytes := body.to_utf8_buffer()
		total += manifest_bytes.size() - measured
		if total > MANIFEST_MAX:
			note("読み込み中に入口の大きさが変わった")
			return 1
		files[entry_leaf] = manifest_bytes
		var _erased_entry: bool = measured_files.erase(entry_leaf)
		var parsed := GD.data.ini(body)
		if parsed.e != null or not parsed.v is Dictionary:
			note(".gdextension の [libraries] が壊れている")
			return 1
		var manifest: Dictionary = parsed.v
		if not manifest.has("libraries") or not manifest["libraries"] is Dictionary:
			note(".gdextension の [libraries] が壊れている")
			return 1
		if manifest.get("classes") is Dictionary:
			var declared: Dictionary = manifest["classes"]
			classes = declared.keys()
		var libraries: Dictionary = manifest["libraries"]
		var native_paths := PackedStringArray()
		for raw: Variant in libraries.values():
			var _lib := native_paths.append(str(raw).strip_edges().trim_prefix('"').trim_suffix('"'))
		if manifest.get("dependencies") is Dictionary:
			var dependencies: Dictionary = manifest["dependencies"]
			for raw_group: Variant in dependencies.values():
				var group := dependency_group(raw_group)
				if not group.is_empty():
					for raw: Variant in group:
						var rel := str(raw).strip_edges().trim_prefix('"').trim_suffix('"')
						if not native_paths.has(rel):
							var _dep := native_paths.append(rel)
		var native_folded := {}
		for rel: String in native_paths:
			if not safe_file_path(rel) or not add_portable_path(rel, native_folded):
				note("unsafe library path: %s" % rel)
				return 1
			var path := GD.file.under(GD.file.dirname(entry_path), rel)
			if path.is_empty() or not no_links(base_dir(), path) or not FileAccess.file_exists(path):
				note("library が無い: %s" % rel)
				return 1
			var lib_size: R = GD.file.size_of(path)
			if lib_size.e != null:
				note(lib_size.e.msg)
				return 1
			var lib_bytes := str(lib_size.v).to_int()
			total += lib_bytes
			if total > PACKAGE_MAX:
				note("packageは500 MiBまで")
				return 1
			files[rel] = path
			measured_files[rel] = lib_bytes
	else:
		var included: R = pure_files(cfg, entry_path)
		if included.e != null:
			note(included.e.msg)
			return 1
		files = included.v
		for raw_leaf: Variant in files:
			var leaf := str(raw_leaf)
			if leaf == entry_leaf:
				continue
			var file_size: R = GD.file.size_of(str(files[raw_leaf]))
			if file_size.e != null:
				note(file_size.e.msg)
				return 1
			var bytes := str(file_size.v).to_int()
			total += bytes
			if total > PACKAGE_MAX:
				note("packageは500 MiBまで")
				return 1
			measured_files[leaf] = bytes

	# Publish the package's own imports so installers can resolve them; only registry packages travel.
	var raw_imports: Variant = cfg.get("imports", {})
	if not raw_imports is Dictionary:
		note("gd.json の imports は辞書にすること")
		return 1
	var declared_imports: Dictionary = raw_imports
	var imports: Dictionary = declared_imports.duplicate()
	for raw_alias: Variant in imports:
		var alias := str(raw_alias)
		var sp := parse_spec(str(imports[raw_alias]))
		var kind := str(sp["kind"])
		if kind == "local":
			# A checkout that is itself a named package publishes as its registry range.
			var local_cfg: Dictionary = load_json(GD.file.join([local_source(str(sp["url"])), "gd.json"]), {})
			var local_name := str(local_cfg.get("name", ""))
			var local_version := str(local_cfg.get("version", ""))
			if not safe_package(local_name) or not package_version(local_version):
				note("%s は %s を指すが、そこに name と version のある gd.json が無いので公開できない" % [alias, sp["url"]])
				return 1
			var head := "ext:" if str(local_cfg.get("main", "mod.gd")).ends_with(".gdextension") else "gd:"
			imports[raw_alias] = head + local_name + "@^" + local_version
			kind = "gd"
		if not safe_segment(alias) or alias.begins_with("@") or (kind != "gd" and kind != "ext"):
			note("packageの imports は gd:@scope/name か ext:@scope/name だけにすること: %s" % alias)
			return 1

	if args.has("--dry-run"):
		print("would publish %s@%s (%d bytes, %d files)" % [pkg, version, total, files.size()])
		return 0

	# Read publishing secrets only from the process environment, never distributable gd.json.
	var token: String = OS.get_environment("GD_TOKEN", "")
	if token.is_empty():
		note("GD_TOKEN が要る。登録所で作った合言葉を入れること")
		return 1

	# Send binary data in 8 MiB chunks to avoid repeated large JSON and Base64 copies.
	var upload_id := GD.id.uuid()
	var scope := pkg.trim_prefix("@").get_slice("/", 0)
	var file_info := {}
	var headers := {"Content-Type": "application/octet-stream", "Authorization": "Bearer " + token}
	for raw_leaf: Variant in files:
		var leaf := str(raw_leaf)
		var file_data := PackedByteArray()
		var file_path := ""
		var file_size := 0
		if files[raw_leaf] is PackedByteArray:
			file_data = files[raw_leaf]
			file_size = file_data.size()
		else:
			file_path = str(files[raw_leaf])
			var actual: R = GD.file.size_of(file_path)
			if actual.e != null:
				note(actual.e.msg)
				return 1
			file_size = str(actual.v).to_int()
			total += file_size - str(measured_files.get(leaf, 0)).to_int()
			if total > PACKAGE_MAX:
				note("libraryの大きさが上限を越えた")
				return 1
		var digest_ctx := HashingContext.new()
		if digest_ctx.start(HashingContext.HASH_SHA256) != OK:
			note("SHA-256を始められない")
			return 1
		var part_count := maxi(1, (file_size + PUBLISH_CHUNK - 1) / PUBLISH_CHUNK)
		for part: int in part_count:
			var at := part * PUBLISH_CHUNK
			var chunk := file_data.slice(at, mini(at + PUBLISH_CHUNK, file_size))
			if not file_path.is_empty():
				var loaded: R = GD.file.read_bytes(file_path, at, mini(PUBLISH_CHUNK, file_size - at))
				if loaded.e != null:
					note(loaded.e.msg)
					return 1
				chunk = loaded.v
			if digest_ctx.update(chunk) != OK:
				note("libraryをSHA-256で読めない")
				return 1
			var upload_url := "%s/-/upload/%s/%s/%s?part=%d" % [
					registry_url, scope, upload_id, leaf.uri_encode(), part,
			]
			var sent, _sent_err := await GD.http.fetch(upload_url, {
				"method": "POST",
				"headers": headers,
				"body": chunk,
			})
			if not (_sent_err == null and sent.ok()):
				note("upload failed: %s" % (_sent_err.text() if _sent_err else sent.text()))
				return 1
		file_info[leaf] = {
			"parts": part_count,
			"size": file_size,
			"sha256": digest_ctx.finish().hex_encode(),
		}

	var payload: Dictionary = {
		"pkg": pkg,
		"version": version,
		"description": str(cfg.get("description", "")),
		"leaf": entry_leaf,
		"upload": upload_id,
		"files": file_info,
		"sha256": str(file_info[entry_leaf]["sha256"]),
		"classes": classes, # Type names obtained from the manifest.
		"imports": imports,
		"godot": cfg.get("godot", false) == true, # Runs on the upstream engine without the local API.
	}
	var encoded_payload: R = encode_json(payload, 0)
	if not encoded_payload.ok:
		note("cannot encode publish metadata: %s" % encoded_payload.e)
		return 1
	var payload_text: String = encoded_payload.v

	var res, _res_err := await GD.http.fetch(registry_url + "/-/publish", {
		"method": "POST",
		"headers": {"Content-Type": "application/json", "Authorization": "Bearer " + token},
		"body": payload_text,
	})
	if not (_res_err == null and res.ok()):
		note("publish failed: %s" % (_res_err.text() if _res_err else res.text()))
		return 1
	print("published %s@%s" % [pkg, version])
	return 0


# Extract only non-flag arguments.
func plain(args: PackedStringArray) -> PackedStringArray:
	var out: PackedStringArray = []
	for a: String in args:
		if not a.begins_with("-"):
			var _a: bool = out.append(a)
	return out


# Combine subcommand and diagnostic-output results into the exit status.
func main(argv: Array) -> int:
	log_failed = false
	var code: int = await dispatch(argv)
	return 1 if log_failed else code


# Execute the subcommand selected by the arguments.
func dispatch(argv: Array) -> int:
	if argv.is_empty():
		print("usage: gd <install|add|remove|outdated|update|search|publish> ...")
		return 1
	var cmd: String = str(argv[0])
	var args: PackedStringArray = []
	for i: int in range(1, argv.size()):
		var _a: bool = args.append(str(argv[i]))

	if cmd == "install":
		return await cmd_install(args)
	if cmd == "add":
		return await cmd_add(args)
	if cmd == "remove":
		return await cmd_remove(args)
	if cmd == "outdated":
		return await cmd_outdated(args)
	if cmd == "update":
		return await cmd_update(args)
	if cmd == "search":
		return await cmd_search(args)
	if cmd == "publish":
		return await cmd_publish(args)
	print("unknown: %s" % cmd)
	return 1
