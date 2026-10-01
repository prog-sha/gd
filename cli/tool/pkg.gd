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
const RELEASES_FALLBACK := "https://github.com/prog-sha/gd/releases" # Release page holding archives and their checksum list.
const REDIRECT_MAX := 6 # Redirect hops followed for one download.
const REPLY_MIN := 64 * 1024 # Body allowance for redirect and error replies around small release files.
const UPGRADE_TIMEOUT := 30.0 # Seconds allowed for each small release request.
const EXECUTABLE_MODE := 493 # Owner-writable, world-runnable file mode (rwxr-xr-x).
const ASSET_OFFICIAL: String = "https://store.godotengine.org/api/v1" # Public asset catalog. Stays first so detail requests keep the publisher.
const ASSET_HOME: String = "https://gd.progsha.com/asset/v1" # Reviewed asset catalog. Matches tools/store.gd.
const PUBLISH_CHUNK: int = 8 * 1024 * 1024 # Bytes read or downloaded per step when hashing and fetching package files.
const PACKAGE_MAX: int = 500 * 1024 * 1024 # Maximum combined bytes of package files.

var seen: Dictionary = {} # Original file text used to detect concurrent changes after reading.
var broken: Dictionary = {} # Paths with invalid JSON that must not be overwritten as empty configuration.
var global_names: Dictionary = {} # Global script class names already present in this graph.
var graph_frozen := false # Preserve the exact lockfile bytes during frozen installation.
var graph_active := false # Retain all package undo records until the whole graph commits.
var graph_steps: Array = [] # Durable ordered undo records, including the currently active step.
var graph_cfg: Dictionary = {} # Intended manifest used to place nodes before its atomic commit.

var meta_cache: Dictionary = {} # Registry version indexes already read in this run, by package.
var global_dir := "" # Private install root for one command, separate from the caller's project.
var global_info: Dictionary = {} # Installer metadata independent of the entry's runtime permissions.


# Continue failure cleanup and record diagnostic-output failures for the final exit status.
func note(msg: String, level: String = "error") -> void:
	printerr(msg if msg.begins_with("usage:") else "%s: %s" % [level, msg])


# Decode UTF-8 JSON with strict standard-data semantics.
func decode_json(text: String) -> Variant, Err:
	return GD.data.json_decode(text.to_utf8_buffer())


# Encode JSON as UTF-8 within the existing byte budget.
func encode_json(value: Variant, max_bytes: int, newline: bool = false) -> Variant, Err:
	var spare := 1 if newline else 0
	var encoded_v, encoded_e := GD.data.json_encode(value, {"max_bytes": max_bytes - spare if max_bytes > 0 else 0})
	if not (encoded_e == null):
		return encoded_v, encoded_e
	var raw: PackedByteArray = encoded_v
	var text := raw.get_string_from_utf8()
	return text + ("\n" if newline else "")


# ---------------- Configuration and locks ----------------

# Read gd.json only at the jail root; parent traversal is not permitted.
func find_config() -> String:
	return "res://gd.json" if GD.file.exists("res://gd.json") else ""


func base_dir() -> String:
	return global_dir if not global_dir.is_empty() else "res://"


func load_json(path: String, fallback: Dictionary) -> Dictionary:
	if not GD.file.exists(path):
		if not seen.has(path):
			seen[path] = null
		var _clear_missing: bool = broken.erase(path)
		return fallback
	var r_v, r_e := GD.file.read_text(path)
	if r_e != null:
		broken[path] = true
		note("cannot read %s" % path)
		return fallback
	if not seen.has(path):
		seen[path] = r_v
	var parsed_v, parsed_e := decode_json(str(r_v))
	if not (parsed_e == null) or not parsed_v is Dictionary:
		broken[path] = true
		note("cannot parse %s" % path)
		return fallback
	var _clear_valid: bool = broken.erase(path)
	return parsed_v


func save_json(path: String, data: Dictionary) -> bool:
	if broken.has(path):
		note("cannot overwrite unreadable %s" % path)
		return false
	var encoded_v, encoded_e := encode_json(data, 0, true)
	if not (encoded_e == null):
		note("cannot encode %s: %s" % [path, encoded_e])
		return false
	var body: String = encoded_v
	var _w_v, w_e := GD.file.replace_text(path, seen.get(path, null), body)
	if (w_e == null):
		seen[path] = body
	return (w_e == null)


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
func text_change(path: String, after_exists: bool, after: String) -> Variant, Err:
	var before_exists := GD.file.exists(path)
	var before := ""
	if before_exists:
		var got_v, got_e := GD.file.read_text(path)
		if got_e != null:
			return got_v, got_e
		before = str(got_v)
	return {
		"path": path, "before_exists": before_exists, "before": before,
		"after_exists": after_exists, "after": after,
	}


# Record JSON before and after updates with consistent formatting.
func json_change(path: String, data: Dictionary) -> Variant, Err:
	if graph_frozen and path == lock_path():
		var original_v, original_e := GD.file.read_text(path)
		if original_e != null:
			return original_v, original_e
		var parsed_v, parsed_e := decode_json(str(original_v))
		if parsed_e != null or parsed_v != data:
			return null, Err.from("resolved metadata differs from gd.lock; remove --frozen to update it", Err.INVALID_DATA)
		return text_change(path, true, str(original_v))
	var encoded_v, encoded_e := encode_json(data, 0, true)
	var text: String = encoded_v if (encoded_e == null) else ""
	if encoded_e != null:
		return encoded_v, encoded_e
	return text_change(path, true, text)


# Apply a text update only to its original starting contents.
func apply_text(snap: Dictionary) -> Variant, Err:
	var old: Variant = null
	if snap["before_exists"] == true:
		old = str(snap["before"])
	if snap["after_exists"] == true:
		return GD.file.replace_text(str(snap["path"]), old, str(snap["after"]))
	if snap["before_exists"] == true:
		return GD.file._remove_text(str(snap["path"]), str(snap["before"]))
	return null


# Distinguish external edits from transaction-written contents before recovery.
func can_restore_text(snap: Dictionary) -> Variant, Err:
	var path := str(snap["path"])
	var exists := GD.file.exists(path)
	if not exists:
		if snap["before_exists"] != true:
			return false
		if snap["after_exists"] != true:
			return true
		return null, Err.from("%s changed after the package transaction" % path, Err.ALREADY_EXISTS)
	var got_v, got_e := GD.file.read_text(path)
	if got_e != null:
		return got_v, got_e
	var body := str(got_v)
	if snap["before_exists"] == true and body == str(snap["before"]):
		return false
	if snap["after_exists"] == true and body == str(snap["after"]):
		return true
	return null, Err.from("%s changed after the package transaction" % path, Err.ALREADY_EXISTS)


# Restore only text written by the transaction to its starting state.
func restore_text(snap: Dictionary) -> Variant, Err:
	var check_v, check_e := can_restore_text(snap)
	if check_e != null or check_v != true:
		return check_v, check_e
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
	if str(txn.get("final", "")).begins_with(base_dir().path_join("addons") + "/"):
		root = base_dir().path_join("addons")
	var prefix := root.trim_suffix("/") + "/"
	var final_path := str(txn.get("final", ""))
	var backup_path := str(txn.get("backup", ""))
	var stage_path := str(txn.get("stage", ""))
	if final_path.is_empty():
		# A cache-only install changes text files only.
		if txn["had_final"] == true or not backup_path.is_empty() or not stage_path.is_empty():
			return false
	else:
		if not final_path.begins_with(prefix) or not no_links(base_dir(), final_path, true):
			return false
		var rel := final_path.substr(prefix.length())
		if rel.is_empty() or GD.file.under(root, rel) != final_path:
			return false
		if (root == pkg_dir() and (not safe_file_path(rel) or (rel.contains("/") and not (rel.begins_with("@") and rel.count("/") == 1)))) or (root != pkg_dir() and (not asset_name(rel) or rel.begins_with("."))):
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
func recover_txn() -> Variant, Err:
	var path := txn_path()
	if not GD.file.exists(path):
		return null
	var size_v, size_e := GD.file.size_of(path)
	if size_e != null:
		return size_v, size_e
	var got_v, got_e := GD.file.read_text(path)
	if got_e != null:
		return got_v, got_e
	var decoded_v, decoded_e := decode_json(str(got_v))
	if not (decoded_e == null) or not decoded_v is Dictionary:
		return null, Err.from("broken package transaction", Err.INVALID_DATA)
	var record: Dictionary = decoded_v
	if record.has("steps"):
		if not record["steps"] is Array:
			return null, Err.from("broken graph transaction", Err.INVALID_DATA)
		var steps: Array = record["steps"]
		for raw_step: Variant in steps:
			if not raw_step is Dictionary:
				return null, Err.from("unsafe graph transaction", Err.PERMISSION_DENIED)
			var step: Dictionary = raw_step
			if not valid_txn(step):
				return null, Err.from("unsafe graph transaction", Err.PERMISSION_DENIED)
		while not steps.is_empty():
			var step: Dictionary = steps.back()
			var restored_v, restored_e := restore_txn(step)
			if restored_e != null:
				return restored_v, restored_e
			var _step: Variant = steps.pop_back()
			var remaining_v, remaining_e := encode_json({"steps": steps}, 0, true)
			if remaining_e != null:
				return remaining_v, remaining_e
			var updated_v, updated_e := GD.file.replace_text(path, str(got_v), str(remaining_v))
			if updated_e != null:
				return updated_v, updated_e
			got_v = remaining_v
	else:
		if not valid_txn(record):
			return null, Err.from("unsafe package transaction", Err.PERMISSION_DENIED)
		var restored_v, restored_e := restore_txn(record)
		if restored_e != null:
			return restored_v, restored_e
	return GD.file.remove(path)


# Restore one journaled step; repeating a completed restoration is harmless.
func restore_txn(txn: Dictionary) -> Variant, Err:
	var final_path := str(txn.get("final", ""))
	var backup_path := str(txn.get("backup", ""))
	var stage_path := str(txn.get("stage", ""))
	# Stop before restoring any file if external edits are present.
	for raw_snap: Variant in txn.get("text", []):
		var snap: Dictionary = raw_snap
		var checked_v, checked_e := can_restore_text(snap)
		if checked_e != null:
			return checked_v, checked_e
	# Do not remove contents replaced by something other than this transaction.
	if not final_path.is_empty():
		var tree: bool = txn["tree"]
		var bounded := not final_path.begins_with(base_dir().path_join("addons") + "/")
		var current_v, current_e := path_fingerprint(final_path, tree, bounded)
		if current_e != null:
			return current_v, current_e
		var before := str(txn["before"])
		var after := str(txn["after"])
		var has_backup := GD.file.exists(backup_path)
		if has_backup:
			var backup_fingerprint_v, backup_fingerprint_e := path_fingerprint(backup_path, tree, bounded)
			if backup_fingerprint_e != null:
				return backup_fingerprint_v, backup_fingerprint_e
			if str(backup_fingerprint_v) != before:
				return null, Err.from("package backup changed after the transaction", Err.ALREADY_EXISTS)
		if has_backup and not str(current_v).is_empty() and str(current_v) != after:
			return null, Err.from("package output changed after the transaction", Err.ALREADY_EXISTS)
		if not has_backup and txn["had_final"] == true and str(current_v) != before:
			return null, Err.from("package output changed after the transaction", Err.ALREADY_EXISTS)
		if not has_backup and txn["had_final"] != true and not str(current_v).is_empty() and str(current_v) != after:
			return null, Err.from("package output changed after the transaction", Err.ALREADY_EXISTS)
		if has_backup:
			if GD.file.exists(final_path):
				var dropped_v, dropped_e := GD.file.remove_all(final_path)
				if dropped_e != null:
					return dropped_v, dropped_e
			var restored_v, restored_e := GD.file.rename(backup_path, final_path)
			if restored_e != null:
				return restored_v, restored_e
		elif txn.get("had_final", false) != true and GD.file.exists(final_path):
			var dropped_new_v, dropped_new_e := GD.file.remove_all(final_path)
			if dropped_new_e != null:
				return dropped_new_v, dropped_new_e
	if not stage_path.is_empty() and GD.file.exists(stage_path):
		var dropped_stage_v, dropped_stage_e := GD.file.remove_all(stage_path)
		if dropped_stage_e != null:
			return dropped_stage_v, dropped_stage_e
	for raw_snap: Variant in txn.get("text", []):
		var snap: Dictionary = raw_snap
		var restored_text_v, restored_text_e := restore_text(snap)
		if restored_text_e != null:
			return restored_text_v, restored_text_e
	return null


# Acquire the project lock and recover interrupted operations first.
func package_lock() -> Variant, Err:
	var dir := GD.file.join([base_dir(), ".godot"])
	var made_v, made_e := GD.file.make_dir(dir)
	if made_e != null:
		return made_v, made_e
	var guard_v, guard_e := GD.file._lock(GD.file.join([dir, "gd-package.lock"]))
	if guard_e != null:
		return guard_v, guard_e
	var recovered_v, recovered_e := recover_txn()
	if recovered_e != null:
		return recovered_v, recovered_e
	return guard_v, guard_e
# Commit recovery information before beginning multi-file updates.
func begin_txn(data: Dictionary) -> Variant, Err:
	if not valid_txn(data):
		return null, Err.from("unsafe package transaction", Err.PERMISSION_DENIED)
	if graph_active:
		var before: Variant = null
		if GD.file.exists(txn_path()):
			var got_v, got_e := GD.file.read_text(txn_path())
			if got_e != null:
				return got_v, got_e
			before = got_v
		graph_steps.append(data)
		var journal_v, journal_e := encode_json({"steps": graph_steps}, 0, true)
		if journal_e != null:
			return journal_v, journal_e
		return GD.file.replace_text(txn_path(), before, str(journal_v))
	var encoded_v, encoded_e := encode_json(data, 0, true)
	var text: String = encoded_v if (encoded_e == null) else ""
	if encoded_e != null:
		return encoded_v, encoded_e
	return GD.file.replace_text(txn_path(), null, text)


# Remove the journal after all transaction updates commit.
func end_txn() -> Variant, Err:
	if graph_active:
		return null
	return GD.file.remove(txn_path())


# Roll back a failed transaction and prioritize recovery failures.
func fail_txn(value: Variant, error: Err) -> Variant, Err:
	if graph_active:
		return value, error
	var _recovered_v, recovered_e := recover_txn()
	return value, error if recovered_e == null else recovered_e


# Select the registry from gd.json, then environment, then the default.
# Read gd.json first because environment access requires --allow-env.
func registry() -> String:
	var from_cfg: String = str(config().get("registry", ""))
	if not json_ok(config_path()):
		return ""
	if not from_cfg.is_empty():
		return from_cfg.trim_suffix("/")
	if not global_info.is_empty():
		var selected := str(global_info["registry"])
		return REGISTRY_FALLBACK if selected.is_empty() else selected.trim_suffix("/")
	if OS.has_environment("GD_REGISTRY"):
		var env: String = OS.get_environment("GD_REGISTRY", "")
		if not env.is_empty():
			return env.trim_suffix("/")
	return REGISTRY_FALLBACK


# Require verified remote transport or a local test server.
func secure_url(url: String) -> bool:
	var parts, parse_error := GD.http.parse_url(url)
	if parse_error != null:
		return false
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
func file_sha256(path: String, size: int) -> Variant, Err:
	var digest_ctx := HashingContext.new()
	if digest_ctx.start(HashingContext.HASH_SHA256) != OK:
		return null, Err.from("cannot start SHA-256", Err.INVALID_DATA)
	var at := 0
	while at < size:
		var got_v, got_e := GD.file.read_bytes(path, at, mini(PUBLISH_CHUNK, size - at))
		if got_e != null:
			return got_v, got_e
		var chunk: PackedByteArray = got_v
		if chunk.is_empty():
			return null, Err.from("file ended while hashing: %s" % path, Err.INVALID_DATA)
		if digest_ctx.update(chunk) != OK:
			return null, Err.from("cannot hash %s" % path, Err.INVALID_DATA)
		at += chunk.size()
	return digest_ctx.finish().hex_encode()


# Validate registry fingerprints and byte counts.
func registry_mark(raw: Variant, max_size: int) -> Variant, Err:
	if not raw is Dictionary:
		return null, Err.from("registry has broken file metadata", Err.INVALID_DATA)
	var mark: Dictionary = raw
	var size := str(mark.get("size", -1)).to_int()
	var sha := str(mark.get("sha256", ""))
	if size < 0 or size > max_size or not sha256_text(sha):
		return null, Err.from("registry has invalid file metadata", Err.INVALID_DATA)
	return {"size": size, "sha256": sha}


# Read the entry point and file fingerprints as a platform-independent version contract.
func registry_version(raw: Variant) -> Variant, Err:
	var main_checked_v, main_checked_e := registry_mark(raw, PACKAGE_MAX)
	if main_checked_e != null:
		return main_checked_v, main_checked_e
	var doc: Dictionary = raw
	var raw_files: Variant = doc.get("files")
	if not raw_files is Dictionary:
		return null, Err.from("registry has broken version metadata", Err.INVALID_DATA)
	var files: Dictionary = raw_files
	var clean := {}
	var paths := {}
	var total := 0
	var entries := PackedStringArray()
	var main_mark: Dictionary = main_checked_v
	for raw_path: Variant in files:
		var path := str(raw_path)
		if not safe_file_path(path) or not add_portable_path(path, paths):
			return null, Err.from("registry has unsafe file metadata", Err.INVALID_DATA)
		var checked_v, checked_e := registry_mark(files[raw_path], PACKAGE_MAX - total)
		if checked_e != null:
			return checked_v, checked_e
		var mark: Dictionary = checked_v
		total += str(mark["size"]).to_int()
		clean[path] = mark
		if mark == main_mark and (path == "mod.gd" or (safe_segment(path) and path.ends_with(".gdextension"))):
			var _entry := entries.append(path)
	if entries.size() != 1:
		return null, Err.from("registry entry file is not unique", Err.INVALID_DATA)
	var leaf := entries[0]
	# Files live at the publisher's release address, named by their SHA-256.
	var files_url := str(doc.get("files_url", "")).trim_suffix("/")
	if not secure_url(files_url) or files_url.contains("?") or files_url.contains("#"):
		return null, Err.from("registry has no valid release address", Err.INVALID_DATA)
	# Registry metadata declares a package's imports as ranges; gd.lock records them resolved.
	var raw_imports: Variant = doc.get("requires", doc.get("imports", {}))
	if not raw_imports is Dictionary:
		return null, Err.from("registry has broken import metadata", Err.INVALID_DATA)
	var imports: Dictionary = raw_imports
	var clean_imports := {}
	for raw_alias: Variant in imports:
		var alias := str(raw_alias)
		var spec := str(imports[raw_alias])
		var kind := str(parse_spec(spec)["kind"])
		if not safe_segment(alias) or alias.begins_with("@") or (kind != "gd" and kind != "ext"):
			return null, Err.from("registry has unsafe import metadata", Err.INVALID_DATA)
		clean_imports[alias] = spec
	return {"sha256": main_mark["sha256"], "size": main_mark["size"], "leaf": leaf, "files": clean, "files_url": files_url, "imports": clean_imports}


# Name a locked package's entry file; a registry release names files by fingerprint, so the lock keeps the name.
func lock_leaf(entry: Dictionary) -> String:
	return str(entry["leaf"]) if entry.has("leaf") else GD.file.basename(str(entry.get("url", "")), "")


# Download a release file and expose it in the cache only after complete fingerprint verification.
func fetch_registry_file(url: String, path: String, raw_mark: Variant, max_size: int) -> Variant, Err:
	var checked_v, checked_e := registry_mark(raw_mark, max_size)
	if checked_e != null:
		return checked_v, checked_e
	var mark: Dictionary = checked_v
	var size := str(mark["size"]).to_int()
	var next := "%s.%s.next" % [path, Crypto.new().generate_random_bytes(16).hex_encode()]
	# Stream straight to disk; release hosts redirect, and the body never sits in memory.
	# Redirect and error replies have small bodies of their own; the saved size is checked below.
	var _fetched, fetch_e := await fetch_follow(url, {"save": next, "max_body": maxi(size, REPLY_MIN)})
	if fetch_e != null:
		if GD.file.exists(next):
			var _drop_fetch_v, _drop_fetch_e := GD.file.remove(next)
		return null, Err.from(fetch_e.note("fetch package file"))
	var digest_v, digest_e := file_sha256(next, size)
	var got_v, got_e := GD.file.size_of(next)
	if digest_e != null or str(digest_v) != str(mark["sha256"]) or got_e != null or int(got_v) != size:
		if GD.file.exists(next):
			var _drop_bad_v, _drop_bad_e := GD.file.remove(next)
		return null, Err.from("registry file fingerprint differs", Err.INVALID_DATA)
	var moved_v, moved_e := GD.file.rename(next, path)
	if moved_e != null:
		return moved_v, moved_e
	return null


# Fingerprint a published file or native-extension tree.
func path_fingerprint(path: String, tree: bool, bounded: bool = true) -> Variant, Err:
	if not GD.file.exists(path):
		return ""
	var files := PackedStringArray([path])
	if tree:
		var walked_v: Variant
		var walked_e: Err
		if bounded:
			var value, error := GD.file.walk(path, false, true)
			walked_v = value
			walked_e = error
		else:
			var value, error := asset_files(path)
			walked_v = value
			walked_e = error
		if walked_e != null:
			return walked_v, walked_e
		files = walked_v
		files.sort()
	var total := 0
	var marks := PackedStringArray()
	for file_path: String in files:
		var size_v, size_e := GD.file.size_of(file_path)
		if size_e != null:
			return size_v, size_e
		var count := str(size_v).to_int()
		total += count
		if bounded and total > PACKAGE_MAX:
			return null, Err.from("package tree exceeds the byte limit", Err.LIMITED)
		var digest_v, digest_e := file_sha256(file_path, count)
		if digest_e != null:
			return digest_v, digest_e
		var _marked: bool = marks.append("%s\t%d\t%s" % [file_path.trim_prefix(path).trim_prefix("/"), count, digest_v])
	return GD.data.hex_encode(GD.data.sha256("\n".join(marks).to_utf8_buffer()))


# Read the registry version index over a verified connection.
func fetch_meta(pkg: String) -> Variant, Err:
	if meta_cache.has(pkg):
		return meta_cache[pkg]
	var url: String = registry() + "/" + pkg + "/meta.json"
	if not secure_url(url):
		return null, Err.from("registry must use HTTPS", Err.PERMISSION_DENIED)
	var res, _res_err := await GD.http.fetch(url)
	if not (_res_err == null and res.ok()):
		if _res_err: return null, Err.from(_res_err.note("fetch package metadata"))
		return null, Err.from("%s: HTTP %d" % [pkg, res.status], Err.NOT_FOUND)
	var decoded, decode_error := res.json()
	if decode_error != null or not decoded is Dictionary:
		return null, Err.from("%s: broken meta.json" % pkg, Err.INVALID_DATA)
	var meta: Dictionary = decoded
	if not meta.has("versions") or not meta["versions"] is Dictionary:
		return null, Err.from("%s: broken meta.json" % pkg, Err.INVALID_DATA)
	var versions: Dictionary = meta["versions"]
	for raw: Variant in versions:
		var checked_v, checked_e := registry_version(versions[raw])
		if not package_version(str(raw)) or checked_e != null:
			return null, Err.from("%s: broken meta.json" % pkg, Err.INVALID_DATA)
		versions[raw] = checked_v
	meta["versions"] = versions
	meta_cache[pkg] = meta
	return meta


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


# Place package copies under addons/ for an explicit editor-project installation.
func for_godot() -> bool:
	if not global_info.is_empty():
		return false
	return OS.get_environment("GD_FOR_GODOT") == "1"


# Locate package copies beside gd.json.
func pkg_dir() -> String:
	return GD.file.join([base_dir(), "addons" if for_godot() else "pkg"])


# Name the resource root of a placed copy.
func placed_prefix(dir: String) -> String:
	return ("res://addons/" if for_godot() else "res://pkg/") + dir


# Select package placement when gd.json omits it: an editor project reads only res://, so the project.
func default_place() -> String:
	return "project" if GD.file.exists(GD.file.join([base_dir(), "project.godot"])) else "cache"


# Report whether gd.json names a known package placement: the shared cache or the project.
func place_ok() -> bool:
	return str(config().get("place", default_place())) in ["cache", "project"]


# Copy pure script packages under the project only when packages are placed there.
func place_project() -> bool:
	if for_godot():
		return true
	var cfg := graph_cfg if graph_active else config()
	return str(cfg.get("place", default_place())) == "project"


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
func check_names(names: Array, owner: String) -> Variant, Err:
	for raw_name: Variant in names:
		var name := str(raw_name)
		if not name.is_valid_unicode_identifier() or ClassDB.class_exists(name) or (global_names.has(name) and str(global_names[name]) != owner):
			return null, Err.from("global class %s conflicts between %s and %s" % [name, global_names.get(name, "engine"), owner], Err.INVALID_DATA)
		global_names[name] = owner
	return null


# Check declared native names without loading the package binary.
func check_native(path: String, key: String) -> Variant, Err:
	var text_v, text_e := GD.file.read_text(path)
	if text_e != null:
		return text_v, text_e
	var parsed_v, parsed_e := GD.data.ini(str(text_v))
	if parsed_e != null:
		return parsed_v, parsed_e
	var doc: Dictionary = parsed_v
	var classes: Variant = doc.get("classes", {})
	if not classes is Dictionary:
		return null, Err.from("invalid native class declarations: %s" % key, Err.INVALID_DATA)
	var declared: Dictionary = classes
	return check_names(declared.keys(), key)


# Check global class declarations in verified source files before exposing a package.
func check_globals(root: String, key: String, files: Dictionary) -> Variant, Err:
	for raw_rel: Variant in files:
		var rel := str(raw_rel)
		if not rel.ends_with(".gd"):
			continue
		var source_text_v, source_text_e := GD.file.read_text(GD.file.under(root, rel))
		if source_text_e != null:
			return source_text_v, source_text_e
		var reserved_v, reserved_e := check_names(Array(source_classes(str(source_text_v))), key + "/" + rel)
		if reserved_e != null:
			return reserved_v, reserved_e
	return null


# Inspect copied local sources using the same global-name table as registry packages.
func check_local_globals(root: String, owner: String) -> Variant, Err:
	var files := {}
	for name: String in DirAccess.get_files_at(root):
		files[name] = true
	var checked_v, checked_e := check_globals(root, owner, files)
	if checked_e != null:
		return checked_v, checked_e
	for sub: String in DirAccess.get_directories_at(root):
		var nested_v, nested_e := check_local_globals(root.path_join(sub), owner + "/" + sub)
		if nested_e != null:
			return nested_v, nested_e
	return null


# Return the cache directory holding the loadable form of a stored package directory.
func ready_dir(cache_path: String) -> String:
	return "cache://pkg/_ready/" + cache_path.trim_prefix("cache://pkg/")


# Write the loadable form of verified cache files: scripts relocated to root, everything else as is.
func make_ready(cache_root: String, rels: PackedStringArray, root: String) -> Variant, Err:
	var out_root := ready_dir(cache_root)
	for rel: String in rels:
		var src := GD.file.under(cache_root, rel)
		var dst := GD.file.under(out_root, rel)
		if src.is_empty() or dst.is_empty():
			return null, Err.from("unsafe package path: %s" % rel, Err.PERMISSION_DENIED)
		var made_v, made_e := GD.file.make_dir(GD.file.dirname(dst))
		if made_e != null:
			return made_v, made_e
		var put_v: Variant
		var put_e: Err
		if rel.ends_with(".gd"):
			var got_v, got_e := GD.file.read_text(src)
			if got_e != null:
				return got_v, got_e
			var value, error := GD.file.write_text(dst, relocate_text(str(got_v), root))
			put_v = value
			put_e = error
		else:
			var value, error := GD.file.copy(src, dst)
			put_v = value
			put_e = error
		if put_e != null:
			return put_v, put_e
	return null


# Relocate every script below a placed copy to the root it will be read from.
func relocate_tree(dir: String, root: String) -> Variant, Err:
	var listing := DirAccess.open(dir)
	if listing == null:
		return null, Err.from("cannot read %s" % dir, Err.PERMISSION_DENIED)
	listing.include_hidden = true
	if listing.list_dir_begin() != OK:
		return null, Err.from("cannot list %s" % dir, Err.PERMISSION_DENIED)
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
		var got_v, got_e := GD.file.read_text(path)
		if got_e != null:
			return got_v, got_e
		var put_v, put_e := GD.file.write_text(path, relocate_text(str(got_v), root))
		if put_e != null:
			return put_v, put_e
	for sub: String in dirs:
		var below_v, below_e := relocate_tree(GD.file.join([dir, sub]), root)
		if below_e != null:
			return below_v, below_e
	return null


# ---------------- Registry metadata ----------------

# Require complete versions without build metadata for package selection.
func package_version(raw: String) -> bool:
	if not GD.version.is_canonical(raw):
		return false
	var version, parse_error := GD.version.parse(raw)
	# Confirm the parsed value is a Dictionary before checking build metadata.
	if parse_error != null or not version is Dictionary:
		return false
	return str(version.get("build", "")).is_empty()


# Read available versions shaped as {"versions": {"1.2.0": {}}, "latest": "1.2.0"}.
func fetch_versions(pkg: String) -> Variant, Err:
	var got_v, got_e := await fetch_meta(pkg)
	if got_e != null:
		return got_v, got_e
	var meta: Dictionary = got_v
	var vs: Dictionary = meta["versions"]
	var out: PackedStringArray = []
	for k: Variant in vs.keys():
		var _a: bool = out.append(str(k))
	return out


# Sort compatible versions once, preferring stable versions for an unrestricted query.
func ordered_versions(list: PackedStringArray, range_txt: String) -> PackedStringArray:
	var query := range_txt if not range_txt.is_empty() else "*"
	var matches: Array = []
	for raw: String in list:
		if not package_version(raw):
			continue
		var version, parse_error := GD.version.parse(raw)
		if parse_error != null:
			continue
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
func fetch_one(name: String, spec: String, cached_only: bool, frozen: bool, forced_version: String = "", replace_key: String = "", next_cfg: Dictionary = {}, owner: String = "", deps: Dictionary = {}) -> Variant, Err:
	if not safe_segment(name) or name.begins_with("@"):
		return null, Err.from("package name must be one safe path segment", Err.INVALID_DATA)
	var sp: Dictionary = parse_spec(spec)
	var kind: String = str(sp["kind"])
	if kind == "bad":
		return null, Err.from("%s: cannot read \"%s\". Use gd:@scope/name@range, ext:@scope/name@range, https://... or ./path" % [name, spec], Err.INVALID_DATA)
	if not owner.is_empty() and kind != "gd" and kind != "ext":
		return null, Err.from("%s imports %s from \"%s\"; packages may only import registry packages" % [owner, name, spec], Err.INVALID_DATA)
	if kind == "local":
		return name # Local dependencies need no download.

	var data: Dictionary = lock()
	if not json_ok(lock_path()):
		return null, Err.from("cannot read gd.lock", Err.INVALID_DATA)
	var packages: Dictionary = data.get("packages", {})
	var locked: Dictionary = data.get("imports", {})

	# Honor an existing locked version when selecting what to install.
	var version: String = ""
	var url: String = str(sp["url"])
	var published: Dictionary = {}
	var entry_leaf := "" # File name of a registry package's entry, which its release names by fingerprint.
	var contract: Dictionary = {}
	var key: String = name
	if kind == "gd" or kind == "ext":
		version = forced_version if not forced_version.is_empty() else pinned_version(name, owner, locked, packages)
		if version.is_empty():
			if frozen:
				return null, Err.from("%s is not in gd.lock. Remove --frozen to add it." % name, Err.INVALID_DATA)
			if cached_only:
				return null, Err.from("%s has no cached version in gd.lock" % name, Err.NOT_FOUND)
		# The lockfile is the contract once it holds the version; read the registry only to learn one.
		if not cached_only and (version.is_empty() or not packages.has(str(sp["pkg"]) + "@" + version)):
			var meta_got_v, meta_got_e := await fetch_meta(str(sp["pkg"]))
			if meta_got_e != null:
				return meta_got_v, meta_got_e
			var meta: Dictionary = meta_got_v
			var versions: Dictionary = meta["versions"]
			if version.is_empty():
				var list := PackedStringArray()
				for raw: Variant in versions:
					var _added: bool = list.append(str(raw))
				version = pick(list, str(sp["range"]))
				if version.is_empty():
					return null, Err.from("%s: no version matches \"%s\"." % [name, sp["range"]], Err.NOT_FOUND)
			var checked_meta_v, checked_meta_e := registry_version(versions.get(version, {}))
			if checked_meta_e != null:
				return null, Err.from("%s@%s has broken registry metadata" % [sp["pkg"], version], Err.INVALID_DATA)
			published = checked_meta_v
		key = str(sp["pkg"]) + "@" + version
		var locked_version := {}
		if packages.has(key):
			var checked_lock_v, checked_lock_e := registry_version(packages[key])
			if checked_lock_e != null:
				return null, Err.from("%s has broken version metadata in gd.lock" % name, Err.INVALID_DATA)
			locked_version = checked_lock_v
		if not published.is_empty() and not locked_version.is_empty() and published != locked_version:
			return null, Err.from("%s registry version differs from gd.lock" % name, Err.INVALID_DATA)
		contract = published if not published.is_empty() else locked_version
		if contract.is_empty():
			return null, Err.from("%s has no verified version in gd.lock" % name, Err.INVALID_DATA)
		var leaf := str(contract["leaf"])
		if (kind == "gd" and leaf != "mod.gd") or (kind == "ext" and not leaf.ends_with(".gdextension")):
			return null, Err.from("%s has the wrong package kind" % name, Err.INVALID_DATA)
		url = str(contract["files_url"]) + "/" + str(contract["sha256"])
		entry_leaf = leaf
	elif frozen:
		var frozen_mark: Dictionary = packages.get(key, {})
		if not sha256_text(str(frozen_mark.get("sha256", ""))):
			return null, Err.from("%s has no verified fingerprint in gd.lock" % name, Err.INVALID_DATA)

	# Separate cache entries by source so different registries cannot collide.
	var source := registry() if not version.is_empty() else url
	var source_hash := GD.data.hex_encode(GD.data.sha256(source.to_utf8_buffer())).substr(0, 32)
	var store: String
	if version.is_empty():
		store = GD.file.join([cache_dir("_url", source_hash), name, "mod.gd"])
	else:
		store = GD.file.join([cache_dir("_registry", source_hash), str(sp["pkg"]), version, entry_leaf])

	var body: String = ""
	var registry_files: Dictionary = {}
	var expected_digest := ""
	var expected_size := -1
	if not contract.is_empty():
		expected_digest = str(contract["sha256"])
		expected_size = str(contract["size"]).to_int()
		registry_files = contract["files"]
	if not version.is_empty() and not sha256_text(expected_digest):
		return null, Err.from("%s has no valid registry fingerprint" % name, Err.INVALID_DATA)
	if GD.file.exists(store):
		var stored_size_v, stored_size_e := GD.file.size_of(store)
		if stored_size_e != null or str(stored_size_v).to_int() > PACKAGE_MAX:
			return null, Err.from("cached package exceeds the byte limit", Err.LIMITED)
	else:
		if cached_only:
			return null, Err.from("%s is not in the cache. Drop --cached-only to fetch it." % name, Err.NOT_FOUND)
		if not secure_url(url):
			return null, Err.from("remote packages must use HTTPS", Err.PERMISSION_DENIED)
		var fetch_max := PACKAGE_MAX
		var _mk_store_v, _mk_store_e := GD.file.make_dir(GD.file.dirname(store))
		if version.is_empty():
			var fetch_opts := {"max_body": fetch_max, "save": store}
			if not expected_digest.is_empty():
				fetch_opts["sha256"] = expected_digest
			var res, _res_err := await GD.http.fetch(url, fetch_opts)
			if not (_res_err == null and res.ok()):
				if _res_err: return null, Err.from(_res_err.note("fetch package"))
				return null, Err.from("%s: cannot fetch %s" % [name, url], Err.NOT_FOUND)
		else:
			var fetched_v, fetched_e := await fetch_registry_file(url, store, contract, fetch_max)
			if fetched_e != null:
				return fetched_v, fetched_e
	var body_max := PACKAGE_MAX
	var actual_size_v, actual_size_e := GD.file.size_of(store)
	if actual_size_e != null or str(actual_size_v).to_int() > body_max:
		var _drop_large_v, _drop_large_e := GD.file.remove(store)
		return null, Err.from("package entry exceeds the byte limit", Err.LIMITED)
	if expected_size >= 0 and str(actual_size_v).to_int() != expected_size:
		var _drop_size_v, _drop_size_e := GD.file.remove(store)
		return null, Err.from("package entry size differs from the registry", Err.INVALID_DATA)
	if kind == "ext":
		var got_v, got_e := GD.file.read_text(store)
		if got_e != null:
			return got_v, got_e
		body = str(got_v)

	# Reject fingerprint mismatches.
	var entry_hash_v, entry_hash_e := file_sha256(store, str(actual_size_v).to_int())
	if entry_hash_e != null:
		return entry_hash_v, entry_hash_e
	var digest: String = str(entry_hash_v)
	if not expected_digest.is_empty() and expected_digest != digest:
		var _drop_bad_v, _drop_bad_e := GD.file.remove(store)
		return null, Err.from("%s does not match its verified fingerprint" % name, Err.INVALID_DATA)
	if packages.has(key):
		var entry: Dictionary = packages[key]
		var want: String = str(entry.get("sha256", ""))
		if not want.is_empty() and want != digest:
			return null, Err.from("%s does not match gd.lock. Remove the entry to accept the new content." % name, Err.INVALID_DATA)
	elif frozen:
		return null, Err.from("%s is not in gd.lock. Remove --frozen to add it." % name, Err.INVALID_DATA)
	# Keep pure script packages in the shared cache; copy under pkg/ for native
	# extensions, whose loader needs real files, and when gd.json places packages in the project.
	# The project's own imports are copied as pkg/<name>/; packages only other packages need
	# take their canonical id, pkg/@scope/name@version/, so versions never collide.
	var place := kind == "ext" or place_project()
	if kind == "ext":
		var reserved_v, reserved_e := check_native(store, key)
		if reserved_e != null:
			return reserved_v, reserved_e
	var dir := name if owner.is_empty() else alias_of(key, graph_cfg if graph_active else config(), locked)
	if dir.is_empty():
		dir = key
	var out_path := GD.file.under(GD.file.under(pkg_dir(), dir), entry_leaf if kind == "ext" else "mod.gd")
	if out_path.is_empty():
		return null, Err.from("package output path leaves its directory", Err.PERMISSION_DENIED)
	var nonce := Crypto.new().generate_random_bytes(16).hex_encode()
	var stage_root := GD.file.join([pkg_dir(), ".stage-" + nonce]) if place else ""
	var next_path := GD.file.join([stage_root, GD.file.basename(out_path, "")]) if place else ""
	if place:
		var _mk_stage_v, _mk_stage_e := GD.file.make_dir(stage_root)
		var put_v: Variant
		var put_e: Err
		if kind == "ext":
			var value, error := GD.file.write_text(next_path, body)
			put_v = value
			put_e = error
		else:
			var value, error := GD.file.copy(store, next_path)
			put_v = value
			put_e = error
		if put_e != null:
			var _drop_put_v, _drop_put_e := GD.file.remove_all(stage_root)
			return put_v, put_e
		# Revalidate the installed snapshot to keep concurrent cache edits out of the lockfile.
		var placed_size_v, placed_size_e := GD.file.size_of(next_path)
		if placed_size_e != null or str(placed_size_v).to_int() != str(actual_size_v).to_int():
			var _drop_size_v, _drop_size_e := GD.file.remove_all(stage_root)
			return null, Err.from("placed package changed while it was being verified", Err.INVALID_DATA)
		var placed_hash_v, placed_hash_e := file_sha256(next_path, str(placed_size_v).to_int())
		if placed_hash_e != null or str(placed_hash_v) != digest:
			var _drop_hash_v, _drop_hash_e := GD.file.remove_all(stage_root)
			return null, Err.from("placed package changed while it was being verified", Err.INVALID_DATA)

	# Verify every script-package file in the cache, staging copies only when placing.
	if kind == "gd":
		var rels := PackedStringArray()
		for raw_rel: Variant in registry_files:
			var rel := str(raw_rel)
			if rel != str(contract["leaf"]):
				var _rel := rels.append(rel)
		var placed_files_v, placed_files_e := await fetch_files(
				stage_root, str(contract["files_url"]), GD.file.dirname(store), registry_files,
				rels, cached_only, str(actual_size_v).to_int()
		)
		if placed_files_e != null:
			if place:
				var _drop_files_v, _drop_files_e := GD.file.remove_all(stage_root)
			return placed_files_v, placed_files_e
	if kind == "ext":
		var libs_v, libs_e := await fetch_libs(next_path, str(contract["files_url"]), GD.file.dirname(store), registry_files, cached_only)
		if libs_e != null:
			var _drop_libs_v, _drop_libs_e := GD.file.remove_all(stage_root)
			return libs_v, libs_e
	if kind == "gd":
		var globals_v, globals_e := check_globals(GD.file.dirname(store), key, registry_files)
		if globals_e != null:
			if place:
				var _drop_globals_v, _drop_globals_e := GD.file.remove_all(stage_root)
			return globals_v, globals_e
	# Scripts keep meaning their own root: relocate the loadable cache form to the canonical id,
	# and a placed copy to where it sits under pkg/.
	if kind == "gd" or kind == "url":
		var rels := PackedStringArray([GD.file.basename(store, "")])
		for raw_rel: Variant in registry_files:
			if str(raw_rel) != rels[0]:
				var _r := rels.append(str(raw_rel))
		var ready_v, ready_e := make_ready(GD.file.dirname(store), rels, "pkg://" + key)
		if ready_e != null:
			if place:
				var _drop_ready_v, _drop_ready_e := GD.file.remove_all(stage_root)
			return ready_v, ready_e
		if place:
			var relocated_v, relocated_e := relocate_tree(stage_root, placed_prefix(dir))
			if relocated_e != null:
				var _drop_relocated_v, _drop_relocated_e := GD.file.remove_all(stage_root)
				return relocated_v, relocated_e
	var final_path := GD.file.dirname(out_path) if place else ""
	if place:
		var final_dir_v, final_dir_e := GD.file.make_dir(GD.file.dirname(final_path))
		if final_dir_e != null:
			var _drop_dir_v, _drop_dir_e := GD.file.remove_all(stage_root)
			return final_dir_v, final_dir_e
	# Publish the verified snapshot and lockfile in one project transaction.
	if not version.is_empty() and owner.is_empty():
		locked[name] = version
	var new_entry := {"sha256": digest, "url": url}
	if not version.is_empty():
		new_entry["size"] = str(actual_size_v).to_int()
		new_entry["files"] = registry_files
		new_entry["files_url"] = contract["files_url"]
		new_entry["leaf"] = entry_leaf
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
		var before_fingerprint_v, before_fingerprint_e := path_fingerprint(final_path, true)
		var after_fingerprint_v, after_fingerprint_e := path_fingerprint(stage_root, true)
		if before_fingerprint_e != null or after_fingerprint_e != null:
			var _drop_fingerprint_v, _drop_fingerprint_e := GD.file.remove_all(stage_root)
			if before_fingerprint_e != null:
				return before_fingerprint_v, before_fingerprint_e
			return after_fingerprint_v, after_fingerprint_e
		before = str(before_fingerprint_v)
		after = str(after_fingerprint_v)
	var lock_snap_v, lock_snap_e := json_change(lock_path(), data)
	if lock_snap_e != null:
		if place:
			var _drop_snap_v, _drop_snap_e := GD.file.remove_all(stage_root)
		return lock_snap_v, lock_snap_e
	var text_snaps: Array = [lock_snap_v]
	if not next_cfg.is_empty():
		var cfg_snap_v, cfg_snap_e := json_change(config_path(), next_cfg)
		if cfg_snap_e != null:
			if place:
				var _drop_cfg_snap_v, _drop_cfg_snap_e := GD.file.remove_all(stage_root)
			return cfg_snap_v, cfg_snap_e
		text_snaps.append(cfg_snap_v)
	if kind == "ext":
		var ext_snap_v, ext_snap_e := extension_change(out_path, true)
		if ext_snap_e != null:
			var _drop_ext_snap_v, _drop_ext_snap_e := GD.file.remove_all(stage_root)
			return ext_snap_v, ext_snap_e
		text_snaps.append(ext_snap_v)
	var started_v, started_e := begin_txn({
		"final": final_path, "backup": backup_path, "stage": stage_root,
		"tree": true,
		"had_final": had_final, "before": before, "after": after,
		"text": text_snaps,
	})
	if started_e != null:
		if place:
			var _drop_txn_v, _drop_txn_e := GD.file.remove_all(stage_root)
		return started_v, started_e
	if had_final:
		var backed_v, backed_e := GD.file.rename(final_path, backup_path)
		if backed_e != null:
			return fail_txn(backed_v, backed_e)
	if place:
		var placed_v, placed_e := GD.file.rename(stage_root, final_path)
		if placed_e != null:
			return fail_txn(placed_v, placed_e)
	for raw_snap: Variant in text_snaps:
		var snap: Dictionary = raw_snap
		var changed_v, changed_e := apply_text(snap)
		if changed_e != null:
			return fail_txn(changed_v, changed_e)
	var finished_v, finished_e := end_txn()
	if finished_e != null:
		return finished_v, finished_e
	if had_final and not graph_active:
		var _drop_old_v, drop_old_e := GD.file.remove_all(backup_path)
		if drop_old_e != null:
			note("cannot remove old package: %s" % backup_path, "warning")

	print("%s %s (%d bytes, sha256 %s)" % [name, version if not version.is_empty() else url, str(actual_size_v).to_int(), digest.substr(0, 12)])
	return version


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
	var parsed_v, parsed_e := decode_json(str(raw))
	return parsed_v if (parsed_e == null) and parsed_v is Dictionary else {}


# Copy version-manifest-verified files from cache into the package tree.
# Each file is fetched from the release address under its SHA-256 name.
func fetch_files(out_root: String, files_url: String, cache_root: String, published: Dictionary,
		rels: PackedStringArray, cached_only: bool, used: int) -> Variant, Err:
	var total := used
	for rel: String in rels:
		var out_path := GD.file.under(out_root, rel) if not out_root.is_empty() else ""
		var cached_path := GD.file.under(cache_root, rel)
		if not safe_file_path(rel) or cached_path.is_empty() or (not out_root.is_empty() and out_path.is_empty()):
			return null, Err.from("unsafe package path: %s" % rel, Err.PERMISSION_DENIED)
		var checked_v, checked_e := registry_mark(published.get(rel), PACKAGE_MAX - total)
		if checked_e != null:
			return null, Err.from("registry has no valid metadata for file: %s" % rel, Err.INVALID_DATA)
		var mark: Dictionary = checked_v
		var want_size := str(mark["size"]).to_int()
		var want_sha := str(mark["sha256"])
		var cached_ok := false
		if GD.file.exists(cached_path):
			var cached_size_v, cached_size_e := GD.file.size_of(cached_path)
			if cached_size_e != null or str(cached_size_v).to_int() > PACKAGE_MAX - total:
				return null, Err.from("package exceeds the total byte limit", Err.LIMITED)
			if str(cached_size_v).to_int() == want_size:
				var cached_hash_v, cached_hash_e := file_sha256(cached_path, want_size)
				if cached_hash_e != null:
					return cached_hash_v, cached_hash_e
				cached_ok = str(cached_hash_v) == want_sha
		if cached_only and not cached_ok:
			if GD.file.exists(cached_path):
				return null, Err.from("cached file does not match gd.lock: %s" % rel, Err.INVALID_DATA)
			return null, Err.from("%s is not in the cache. Drop --cached-only to fetch it." % rel, Err.NOT_FOUND)
		if not cached_ok:
			var made_cache_v, made_cache_e := GD.file.make_dir(GD.file.dirname(cached_path))
			if made_cache_e != null:
				return made_cache_v, made_cache_e
			var fetched_v, fetched_e := await fetch_registry_file(files_url + "/" + want_sha, cached_path, mark, PACKAGE_MAX - total)
			if fetched_e != null:
				return fetched_v, fetched_e
		total += want_size
		if total > PACKAGE_MAX:
			return null, Err.from("package exceeds the total byte limit", Err.LIMITED)
		if out_root.is_empty():
			continue
		var made_out_v, made_out_e := GD.file.make_dir(GD.file.dirname(out_path))
		if made_out_e != null:
			return made_out_v, made_out_e
		var copied_v, copied_e := GD.file.copy(cached_path, out_path)
		if copied_e != null:
			return copied_v, copied_e
		var placed_size_v, placed_size_e := GD.file.size_of(out_path)
		if placed_size_e != null or str(placed_size_v).to_int() != want_size:
			return null, Err.from("cached file changed while it was copied: %s" % rel, Err.INVALID_DATA)
		var placed_hash_v, placed_hash_e := file_sha256(out_path, want_size)
		if placed_hash_e != null or str(placed_hash_v) != want_sha:
			return null, Err.from("cached file changed while it was copied: %s" % rel, Err.INVALID_DATA)
	return total


# Download the current-platform extension binary and its dependent libraries.
func fetch_libs(manifest_path: String, files_url: String, cache_root: String, published: Dictionary, cached_only: bool) -> Variant, Err:
	var txt_v, txt_e := GD.file.read_text(manifest_path)
	if txt_e != null:
		return txt_v, txt_e
	var ini_v, ini_e := GD.data.ini(str(txt_v))
	if ini_e != null or not ini_v is Dictionary:
		return null, Err.from("broken .gdextension", Err.INVALID_DATA)
	var doc: Dictionary = ini_v
	if not doc.get("libraries") is Dictionary:
		return null, Err.from("%s has no [libraries]" % GD.file.basename(manifest_path, ""), Err.INVALID_DATA)

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
		return null, Err.from("no library for %s in %s" % [platform_tag(), GD.file.basename(manifest_path, "")], Err.NOT_FOUND)

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
			return null, Err.from("unsafe library path: %s" % rel, Err.PERMISSION_DENIED)
		if not add_portable_path(rel, folded):
			return null, Err.from("colliding library path: %s" % rel, Err.INVALID_DATA)

	# Verify current-platform files against the shared cross-platform version contract.
	return await fetch_files(
			GD.file.dirname(manifest_path), files_url, cache_root,
			published, rels, cached_only, str(txt_v).to_utf8_buffer().size()
	)


# Build before and after snapshots of the native-extension registry.
func extension_change(manifest_path: String, add: bool) -> Variant, Err:
	var dir: String = GD.file.join([base_dir(), ".godot"])
	var _mk_v, _mk_e := GD.file.make_dir(dir)
	var list_path: String = GD.file.join([dir, "extension_list.cfg"])
	var line: String = "res://" + manifest_path.trim_prefix(base_dir()).trim_prefix("/")
	var body: String = ""
	if GD.file.exists(list_path):
		var got_v, got_e := GD.file.read_text(list_path)
		if got_e != null:
			return got_v, got_e
		body = str(got_v)
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
	if not global_info.is_empty():
		return str(global_info["platform"]) + "." + str(global_info["arch"])
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
func copy_local(src: String, dst: String, root: String, top: bool) -> Variant, Err:
	var listing := DirAccess.open(src)
	if listing == null:
		return null, Err.from("cannot read local package: %s" % src, Err.NOT_FOUND)
	listing.include_hidden = true
	if listing.list_dir_begin() != OK:
		return null, Err.from("cannot list local package: %s" % src, Err.PERMISSION_DENIED)
	var made_v, made_e := GD.file.make_dir(dst)
	if made_e != null:
		return made_v, made_e
	var name := listing.get_next()
	while not name.is_empty():
		var is_dir := listing.current_is_dir()
		var from := src.path_join(name)
		var to := GD.file.join([dst, name])
		if not listing.is_link(name) and not local_skips(name, is_dir, from):
			if is_dir:
				var below_v, below_e := copy_local(from, to, root, false)
				if below_e != null:
					return below_v, below_e
			elif name.ends_with(".gd") or (top and name == "gd.json"):
				var got_v, got_e := GD.file.read_text(from)
				if got_e != null:
					return got_v, got_e
				var text := str(got_v)
				if name == "gd.json":
					var doc_v, doc_e := decode_json(text)
					if doc_e != null or not doc_v is Dictionary:
						return null, Err.from("local package has a broken gd.json: %s" % from, Err.INVALID_DATA)
					var cfg: Dictionary = doc_v
					var _token: bool = cfg.erase("token")
					var encoded_v, encoded_e := encode_json(cfg, 0, true)
					if not (encoded_e == null):
						return null, Err.from("cannot encode gd.json of %s" % src, Err.INVALID_DATA)
					text = str(encoded_v)
				else:
					text = relocate_text(text, root)
				var put_v, put_e := GD.file.write_text(to, text)
				if put_e != null:
					return put_v, put_e
			else:
				var copied_v, copied_e := GD.file.copy(from, to)
				if copied_e != null:
					return copied_v, copied_e
		name = listing.get_next()
	listing.list_dir_end()
	return null


# Place a local package under pkg/<alias>/ in one transaction, replacing an older copy.
func sync_local(alias: String, spec: String) -> Variant, Err:
	var src := local_source(spec)
	if not GD.file.exists(src):
		return null, Err.from("%s: no such directory: %s" % [alias, spec], Err.NOT_FOUND)
	var final_path := GD.file.under(pkg_dir(), alias)
	if final_path.is_empty():
		return null, Err.from("package output path leaves its directory", Err.PERMISSION_DENIED)
	var nonce := Crypto.new().generate_random_bytes(16).hex_encode()
	var stage_root := GD.file.join([pkg_dir(), ".stage-" + nonce])
	var stamp := GD.file._local_stamp(src)
	if stamp.is_empty():
		return null, Err.from("cannot fingerprint local package: %s" % spec, Err.INVALID_DATA)
	var copied_v, copied_e := copy_local(src, stage_root, placed_prefix(alias), true)
	if copied_e != null:
		var _drop_copy_v, _drop_copy_e := GD.file.remove_all(stage_root)
		return copied_v, copied_e
	if GD.file._local_stamp(src) != stamp:
		var _drop_changed_v, _drop_changed_e := GD.file.remove_all(stage_root)
		return null, Err.from("local package changed while copying: %s" % spec, Err.INVALID_DATA)
	var stamped_v, stamped_e := GD.file.write_text(stage_root.path_join(".gd-source"), stamp)
	if stamped_e != null:
		var _drop_stamp_v, _drop_stamp_e := GD.file.remove_all(stage_root)
		return stamped_v, stamped_e
	var globals_v, globals_e := check_local_globals(stage_root, "local:" + alias)
	if globals_e != null:
		var _drop_globals_v, _drop_globals_e := GD.file.remove_all(stage_root)
		return globals_v, globals_e
	var backup_path := final_path + "." + nonce + ".old"
	var had_final := GD.file.exists(final_path)
	var before_fingerprint_v, before_fingerprint_e := path_fingerprint(final_path, true)
	var after_fingerprint_v, after_fingerprint_e := path_fingerprint(stage_root, true)
	if before_fingerprint_e != null or after_fingerprint_e != null:
		var _drop_fingerprint_v, _drop_fingerprint_e := GD.file.remove_all(stage_root)
		if before_fingerprint_e != null:
			return before_fingerprint_v, before_fingerprint_e
		return after_fingerprint_v, after_fingerprint_e
	var _parent_v, _parent_e := GD.file.make_dir(GD.file.dirname(final_path))
	var started_v, started_e := begin_txn({
		"final": final_path, "backup": backup_path, "stage": stage_root,
		"tree": true,
		"had_final": had_final, "before": before_fingerprint_v, "after": after_fingerprint_v,
		"text": [],
	})
	if started_e != null:
		var _drop_txn_v, _drop_txn_e := GD.file.remove_all(stage_root)
		return started_v, started_e
	if had_final:
		var backed_v, backed_e := GD.file.rename(final_path, backup_path)
		if backed_e != null:
			return fail_txn(backed_v, backed_e)
	var placed_v, placed_e := GD.file.rename(stage_root, final_path)
	if placed_e != null:
		return fail_txn(placed_v, placed_e)
	var finished_v, finished_e := end_txn()
	if finished_e != null:
		return finished_v, finished_e
	if had_final and not graph_active:
		var _drop_old_v, drop_old_e := GD.file.remove_all(backup_path)
		if drop_old_e != null:
			note("cannot remove old package: %s" % backup_path, "warning")
	print("%s %s (local copy)" % [alias, spec])
	return alias


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
	var version, parse_error := GD.version.parse(v)
	if parse_error != null or not version is Dictionary:
		return false
	return GD.version.satisfies(version, range_txt)


# Prefer a valid lock, then an existing compatible version, then a registry candidate.
func choose(pkg: String, range_txt: String, pinned: String, chosen: Dictionary, cached_only: bool, frozen: bool, who: String) -> Variant, Err:
	if not pinned.is_empty() and (range_txt.is_empty() or satisfies_range(pinned, range_txt)):
		return pinned
	if frozen:
		return null, Err.from("%s is not in gd.lock or its range changed. Remove --frozen to resolve it." % who, Err.INVALID_DATA)
	var taken: PackedStringArray = chosen.get(pkg, PackedStringArray())
	for v: String in taken:
		if range_txt.is_empty() or satisfies_range(v, range_txt):
			return v
	if cached_only:
		return null, Err.from("%s has no cached version in gd.lock" % who, Err.NOT_FOUND)
	var vs_v, vs_e := await fetch_versions(pkg)
	if vs_e != null:
		return vs_v, vs_e
	var list: PackedStringArray = vs_v
	var version := pick(list, range_txt)
	if version.is_empty():
		return null, Err.from("%s: no version matches \"%s\"." % [who, range_txt], Err.NOT_FOUND)
	return version


# Resolve every package the project reaches, breadth first from gd.json, into an ordered list
# of nodes {owner, alias, spec, kind, version, id, deps}. Two packages may pin different
# versions of one pure package; a native extension loads once, so it gets one version.
# force maps a project alias to the version it must take, for gd update.
func resolve_graph(cfg: Dictionary, cached_only: bool, frozen: bool, force: Dictionary = {}) -> Variant, Err:
	var pending: Array = [{}] # Unexplored singleton assignments, without a graph-size ceiling.
	var failure_v: Variant
	var failure_e := Err.from("no compatible native dependency graph", Err.INVALID_DATA)
	while not pending.is_empty():
		var bindings: Dictionary = pending.pop_back()
		var result_v, result_e := await walk_graph(cfg, cached_only, frozen, force, bindings)
		if result_e != null:
			failure_v = result_v
			failure_e = result_e
			continue
		if result_v is Array:
			return result_v, result_e
		var branch: Dictionary = result_v
		var versions: PackedStringArray = branch["versions"]
		versions.reverse()
		for version: String in versions:
			var next: Dictionary = bindings.duplicate()
			next[str(branch["package"])] = version
			pending.append(next)
	return failure_v, failure_e
# Expand a graph for one set of singleton choices, or request the next choice.
func walk_graph(cfg: Dictionary, cached_only: bool, frozen: bool, force: Dictionary, bindings: Dictionary) -> Variant, Err:
	var imports: Dictionary = cfg.get("imports", {})
	var data: Dictionary = lock()
	var packages: Dictionary = data.get("packages", {})
	var locked: Dictionary = data.get("imports", {})
	if data.has("registry") and str(data["registry"]) != registry():
		return null, Err.from("registry differs from gd.lock; migrate the lock explicitly before using another source", Err.INVALID_DATA)
	for entry_id: Variant in packages:
		if str(entry_id).begins_with("@"):
			var entry: Dictionary = packages[entry_id]
			# A registry package's entry is the release file named by its fingerprint.
			if str(entry.get("url", "")) != str(entry.get("files_url", "")) + "/" + str(entry.get("sha256", "")):
				return null, Err.from("package source differs from gd.lock: %s" % entry_id, Err.INVALID_DATA)
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
			return null, Err.from("%s: package name must be one safe path segment" % who, Err.INVALID_DATA)
		var sp := parse_spec(spec)
		var kind := str(sp["kind"])
		node["kind"] = kind
		node["version"] = ""
		node["id"] = ""
		if kind == "bad":
			return null, Err.from("%s: cannot read \"%s\". Use gd:@scope/name@range, ext:@scope/name@range, https://... or ./path" % [who, spec], Err.INVALID_DATA)
		if kind == "local" or kind == "url":
			if not owner.is_empty():
				return null, Err.from("%s imports %s from \"%s\"; packages may only import registry packages" % [owner, alias, spec], Err.INVALID_DATA)
			if kind == "local":
				var local_manifest := GD.file.join([local_source(spec), "gd.json"])
				var local_cfg: Dictionary = load_json(local_manifest, {})
				if not json_ok(local_manifest):
					return null, Err.from("cannot read local package manifest: %s" % spec, Err.INVALID_DATA)
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
					var available_v, available_e := await fetch_versions(pkg)
					if available_e != null:
						return available_v, available_e
					var offered: PackedStringArray = available_v
					for candidate: String in ordered_versions(offered, str(sp["range"])):
						if candidate != pinned:
							var _candidate := candidates.append(candidate)
				if candidates.is_empty():
					return null, Err.from("%s: no compatible native version" % who, Err.NOT_FOUND)
				return {"package": pkg, "versions": candidates}
			version = str(bindings[pkg])
			if not satisfies_range(version, str(sp["range"])):
				return null, Err.from("%s requires %s@%s, incompatible with native %s" % [who, pkg, sp["range"], version], Err.INVALID_DATA)
		else:
			var picked_v, picked_e := await choose(pkg, str(sp["range"]), pinned, chosen, cached_only, frozen, who)
			if picked_e != null:
				return picked_v, picked_e
			version = str(picked_v)
		if owner.is_empty() and force.has(alias) and version != str(force[alias]):
			return null, Err.from("%s cannot use requested version %s with the other native constraints" % [alias, force[alias]], Err.INVALID_DATA)
		if kind == "ext":
			if native.has(pkg) and str(native[pkg]["version"]) != version:
				return null, Err.from("%s is needed at %s by %s and at %s by %s; a native extension loads once, so pick one range" % [
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
				return null, Err.from("%s is imported as both gd and ext" % id, Err.INVALID_DATA)
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
			var meta_got_v: Variant
			var meta_got_e: Err
			if cached_only:
				meta_got_e = Err("%s is not in gd.lock. Drop --cached-only to resolve it." % who, Err.NOT_FOUND)
			else:
				var value, error := await fetch_meta(pkg)
				meta_got_v = value
				meta_got_e = error
			if meta_got_e != null:
				return meta_got_v, meta_got_e
			var versions: Dictionary = meta_got_v["versions"]
			if not versions.has(version):
				return null, Err.from("%s@%s is not in the registry" % [pkg, version], Err.NOT_FOUND)
			children = versions[version]["imports"]
		var child_names: Array = children.keys()
		child_names.sort()
		for raw_child: Variant in child_names:
			queue.append({"owner": id, "alias": str(raw_child), "spec": str(children[raw_child]), "deps": {}, "parent_deps": node["deps"]})
	return nodes


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
	var leaf := lock_leaf(entry)
	if kind == "ext" or place_project():
		var dir := alias if owner.is_empty() else alias_of(id, graph_cfg if graph_active else config(), locked)
		if dir.is_empty():
			dir = id
		var root := GD.file.under(pkg_dir(), dir)
		if not GD.file.exists(GD.file.under(root, leaf)):
			return false
		if kind == "gd":
			var files: Dictionary = entry.get("files", {})
			for raw_rel: Variant in files:
				if not GD.file.exists(GD.file.under(root, str(raw_rel))):
					return false
		return true
	var source_hash := GD.data.hex_encode(GD.data.sha256(registry().to_utf8_buffer())).substr(0, 32)
	var source := GD.file.join([cache_dir("_registry", source_hash), pkg_of_id(id), str(node["version"])])
	if not GD.file.exists(GD.file.under(source, leaf)):
		return false
	if kind == "gd":
		var ready := ready_dir(source)
		var files: Dictionary = entry.get("files", {})
		for raw_rel: Variant in files:
			if not GD.file.exists(GD.file.under(ready, str(raw_rel))):
				return false
	return true


# Remove a placed package directory, and its native registration, in one transaction.
func drop_dir(path: String, manifest: String) -> Variant, Err:
	if not GD.file.exists(path):
		return null
	var backup := path + "." + Crypto.new().generate_random_bytes(16).hex_encode() + ".old"
	var before_fingerprint_v, before_fingerprint_e := path_fingerprint(path, true)
	if before_fingerprint_e != null:
		return before_fingerprint_v, before_fingerprint_e
	var text_snaps: Array = []
	if not manifest.is_empty():
		var ext_snap_v, ext_snap_e := extension_change(manifest, false)
		if ext_snap_e != null:
			return ext_snap_v, ext_snap_e
		text_snaps.append(ext_snap_v)
	var started_v, started_e := begin_txn({
		"final": path, "backup": backup, "stage": "",
		"tree": true,
		"had_final": true, "before": before_fingerprint_v, "after": "", "text": text_snaps,
	})
	if started_e != null:
		return started_v, started_e
	var moved_v, moved_e := GD.file.rename(path, backup)
	if moved_e != null:
		return fail_txn(moved_v, moved_e)
	for raw_snap: Variant in text_snaps:
		var snap: Dictionary = raw_snap
		var changed_v, changed_e := apply_text(snap)
		if changed_e != null:
			return fail_txn(changed_v, changed_e)
	var finished_v, finished_e := end_txn()
	if finished_e != null:
		return finished_v, finished_e
	if graph_active:
		return null
	var _removed_v, removed_e := GD.file.remove_all(backup)
	if removed_e != null:
		note("cannot remove old package: %s" % backup, "warning")
	# An id copy sits under its scope directory; drop the scope once it holds nothing else.
	var scope := GD.file.dirname(path)
	if GD.file.basename(scope, "").begins_with("@"):
		var left := DirAccess.get_directories_at(scope).size() + DirAccess.get_files_at(scope).size()
		if left == 0:
			var _scope_gone_v, _scope_gone_e := GD.file.remove_all(scope)
	return null


# Drop lockfile entries and canonical copies that no resolved node reaches, and canonical
# copies of packages that a project alias now places under pkg/<alias>/.
func prune(cfg: Dictionary, nodes: Array, initial: Dictionary) -> Variant, Err:
	var reached := {}
	for raw_node: Variant in nodes:
		var node: Dictionary = raw_node
		var id := str(node["id"])
		reached[id if not id.is_empty() else str(node["alias"])] = true
	var data: Dictionary = lock()
	if not json_ok(lock_path()):
		return null, Err.from("cannot read gd.lock", Err.INVALID_DATA)
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
			return null, Err.from("unsafe package alias in gd.lock", Err.INVALID_DATA)
		var spec := parse_spec(str(old_requests[alias]))
		var id := str(spec["pkg"]) + "@" + str(old_imports.get(alias, "")) if spec["kind"] in ["gd", "ext"] else alias
		var entry: Dictionary = old_packages.get(id, {})
		var leaf := lock_leaf(entry)
		var path := GD.file.under(pkg_dir(), alias)
		var dropped_v, dropped_e := drop_dir(path, path.path_join(leaf) if leaf.ends_with(".gdextension") else "")
		if dropped_e != null:
			return dropped_v, dropped_e
	for raw_key: Variant in packages.keys():
		var key := str(raw_key)
		var entry: Dictionary = packages[key]
		var leaf := lock_leaf(entry)
		var manifest_leaf := leaf if leaf.ends_with(".gdextension") else ""
		var canonical := GD.file.under(pkg_dir(), key) if key.begins_with("@") else ""
		if reached.has(key):
			# A copy under the id is stale once a project alias places the same package.
			if not canonical.is_empty() and not alias_of(key, cfg, locked).is_empty():
				var dropped_dup_v, dropped_dup_e := drop_dir(canonical, GD.file.under(canonical, manifest_leaf) if not manifest_leaf.is_empty() else "")
				if dropped_dup_e != null:
					return dropped_dup_v, dropped_dup_e
			continue
		if not canonical.is_empty():
			var dropped_v, dropped_e := drop_dir(canonical, GD.file.under(canonical, manifest_leaf) if not manifest_leaf.is_empty() else "")
			if dropped_e != null:
				return dropped_v, dropped_e
		var _gone: bool = packages.erase(key)
		changed = true
	for raw_alias: Variant in locked.keys():
		if not imports.has(raw_alias):
			var _unpinned: bool = locked.erase(raw_alias)
			changed = true
	if not changed:
		return null
	data["packages"] = packages
	data["imports"] = locked
	var lock_snap_v, lock_snap_e := json_change(lock_path(), data)
	if lock_snap_e != null:
		return lock_snap_v, lock_snap_e
	var started_v, started_e := begin_txn({"final": "", "backup": "", "stage": "", "tree": true,
			"had_final": false, "before": "", "after": "", "text": [lock_snap_v]})
	if started_e != null:
		return started_v, started_e
	var snap: Dictionary = lock_snap_v
	var applied_v, applied_e := apply_text(snap)
	if applied_e != null:
		return fail_txn(applied_v, applied_e)
	return end_txn()


# Resolve the graph, then fetch and place every node in order and prune what nothing reaches.
# verify re-checks packages the lockfile already covers, as gd install does.
func install_graph(cfg: Dictionary, cached_only: bool, frozen: bool, verify: bool, force: Dictionary = {}) -> Variant, Err:
	var baseline_v: Variant
	var baseline_e: Err
	if GD.file.exists(config_path()):
		var value, error := GD.file.read_text(config_path())
		baseline_v = value
		baseline_e = error
	if baseline_e != null:
		return baseline_v, baseline_e
	var resolved_v, resolved_e := await resolve_graph(cfg, cached_only, frozen, force)
	if resolved_e != null:
		return resolved_v, resolved_e
	if frozen:
		var pinned: Dictionary = lock()
		if pinned.get("requests", {}) != cfg.get("imports", {}):
			return null, Err.from("package requests differ from gd.lock; remove --frozen to resolve them", Err.INVALID_DATA)
		var expected_imports := {}
		var expected_packages := {}
		var entries: Dictionary = pinned.get("packages", {})
		for raw_node: Variant in resolved_v:
			var node: Dictionary = raw_node
			var id := str(node["id"])
			if id.is_empty():
				id = str(node["alias"])
			expected_packages[id] = true
			var entry: Dictionary = entries.get(id, {})
			if str(node["kind"]) == "local" and entry.get("source", "") != node["spec"]:
				return null, Err.from("local package source differs from gd.lock", Err.INVALID_DATA)
			if not entries.has(id) or entry.get("imports", {}) != node["deps"]:
				return null, Err.from("dependency graph differs from gd.lock; remove --frozen to resolve it", Err.INVALID_DATA)
			if str(node["owner"]).is_empty() and not str(node["version"]).is_empty():
				expected_imports[str(node["alias"])] = node["version"]
		if expected_imports != pinned.get("imports", {}) or expected_packages.size() != entries.size() or not pinned.has("registry"):
			return null, Err.from("dependency graph differs from gd.lock; remove --frozen to resolve it", Err.INVALID_DATA)
	graph_active = true
	graph_cfg = cfg
	graph_frozen = frozen
	graph_steps = []
	global_names = {}
	var nodes: Array = resolved_v
	var installed_v, installed_e := await place_graph(cfg, nodes, cached_only, frozen, verify)
	if installed_e == null:
		var value, error := await install_assets(cfg, cached_only, frozen, force)
		installed_v = value
		installed_e = error
	if installed_e == null:
		var current_v: Variant
		var current_e: Err
		if GD.file.exists(config_path()):
			var value, error := GD.file.read_text(config_path())
			current_v = value
			current_e = error
		if current_e != null or current_v != baseline_v:
			installed_v = current_v
			installed_e = current_e if current_e != null else Err("gd.json changed during installation", Err.ALREADY_EXISTS)
	if installed_e == null and cfg != config():
		var snap_v, snap_e := json_change(config_path(), cfg)
		if snap_e != null:
			installed_v = snap_v
			installed_e = snap_e
		else:
			var value, error := begin_txn({"final": "", "backup": "", "stage": "", "tree": true,
					"had_final": false, "before": "", "after": "", "text": [snap_v]})
			installed_v = value
			installed_e = error
			if installed_e == null:
				var change: Dictionary = snap_v
				var changed_v, changed_e := apply_text(change)
				installed_v = changed_v
				installed_e = changed_e
	graph_active = false
	graph_cfg = {}
	graph_frozen = false
	if installed_e != null:
		return fail_txn(installed_v, installed_e)
	if GD.file.exists(txn_path()):
		var committed_v, committed_e := end_txn()
		if committed_e != null:
			return fail_txn(committed_v, committed_e)
	for raw_step: Variant in graph_steps:
		var step: Dictionary = raw_step
		var backup := str(step["backup"])
		if not backup.is_empty() and GD.file.exists(backup):
			var _dropped_v, dropped_e := GD.file.remove_all(backup)
			if dropped_e != null:
				note("cannot remove old package: %s" % backup, "warning")
		var scope := GD.file.dirname(str(step["final"]))
		if GD.file.basename(scope, "").begins_with("@") and GD.file.exists(scope):
			if DirAccess.get_directories_at(scope).is_empty() and DirAccess.get_files_at(scope).is_empty():
				var _empty_scope_v, _empty_scope_e := GD.file.remove_all(scope)
	return null


# Install every resolved node while retaining undo records for the graph owner.
func place_graph(cfg: Dictionary, nodes: Array, cached_only: bool, frozen: bool, verify: bool) -> Variant, Err:
	var initial := lock()
	for raw_node: Variant in nodes:
		var node: Dictionary = raw_node
		if node.get("dup", false):
			continue
		if str(node["kind"]) == "local":
			var synced_v, synced_e := sync_local(str(node["alias"]), str(node["spec"]))
			if synced_e != null:
				return synced_v, synced_e
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
					var reserved_v, reserved_e := check_native(root.path_join(lock_leaf(prior_packages[id])), id)
					if reserved_e != null:
						return reserved_v, reserved_e
				if str(node["kind"]) == "gd":
					var key := str(node["id"])
					var source_hash := registry().sha256_text().substr(0, 32)
					var root := GD.file.join([cache_dir("_registry", source_hash), pkg_of_id(key), version_of_id(key)])
					var files: Dictionary = prior_packages[key]["files"]
					var globals_v, globals_e := check_globals(root, key, files)
					if globals_e != null:
						return globals_v, globals_e
				continue
		var deps: Dictionary = node["deps"]
		var got_v, got_e := await fetch_one(str(node["alias"]), str(node["spec"]), cached_only, frozen,
				str(node["version"]), "", {}, str(node["owner"]), deps)
		if got_e != null:
			return got_v, got_e
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
	var snap_v, snap_e := json_change(lock_path(), data)
	if snap_e != null:
		return snap_v, snap_e
	var started_v, started_e := begin_txn({"final": "", "backup": "", "stage": "", "tree": true,
			"had_final": false, "before": "", "after": "", "text": [snap_v]})
	if started_e != null:
		return started_v, started_e
	var change: Dictionary = snap_v
	var applied_v, applied_e := apply_text(change)
	if applied_e != null:
		return applied_v, applied_e
	return prune(cfg, nodes, initial)


# ---------------- Asset catalogs ----------------

# Return the default asset catalogs. The public catalog stays first.
func asset_channels() -> Array:
	return [
		{"id": "official", "url": ASSET_OFFICIAL},
		{"id": "gd", "url": ASSET_HOME},
	]


# Decide whether package search should also read the default asset catalogs.
func official_registry() -> bool:
	var from_cfg := str(config().get("registry", "")).trim_suffix("/")
	if not json_ok(config_path()):
		return false
	if not from_cfg.is_empty():
		return from_cfg == REGISTRY_FALLBACK
	if OS.has_environment("GD_REGISTRY"):
		var env := OS.get_environment("GD_REGISTRY", "").trim_suffix("/")
		if not env.is_empty():
			return env == REGISTRY_FALLBACK
	return true


# Read one catalog document.
func fetch_channel_json(target: String) -> Variant, Err:
	var response, failure := await GD.http.fetch(target, {"headers": {"User-Agent": "gd"}})
	if failure != null:
		return null, Err.from(failure.note("fetch asset catalog"))
	if int(response.status) != 200:
		return null, Err.from("catalog HTTP %d" % int(response.status), Err.NOT_FOUND if response.status == 404 else Err.INVALID_DATA)
	return response.json()


# Bind a catalog explicitly; omitted channels select the public catalog only.
func parse_asset(spec: String) -> Dictionary:
	var text := spec.strip_edges()
	var channel := "official"
	if text.get_slice("/", 0).contains(":"):
		channel = text.get_slice(":", 0)
		text = text.substr(channel.length() + 1)
	if channel not in ["official", "gd"]:
		return {}
	var ver := ""
	var at := text.rfind("@")
	if at >= 0:
		ver = text.substr(at + 1)
		text = text.substr(0, at)
		if not asset_label(ver) or (ver.begins_with("#") and (not ver.substr(1).is_valid_int() or ver.substr(1).to_int() < 1)):
			return {}
	var parts := text.split("/", true)
	if parts.size() != 2 or not safe_segment(parts[0]) or not safe_segment(parts[1]):
		return {}
	return {"channel": channel, "publisher": parts[0], "slug": parts[1], "version": ver}


# Accept display labels without path syntax or terminal control characters.
func asset_label(value: String) -> bool:
	return not value.is_empty() and value == value.strip_edges() and safe_description(value) and not value.contains("/") and not value.contains("\\") and not value.contains("@")


# Match a release ID or an exact display label.
func asset_matches(rel: Dictionary, want: String) -> bool:
	return str(rel.get("id", "")) == want.substr(1) if want.begins_with("#") else str(rel.get("version", "")) == want


# Produce an unambiguous catalog request for the manifest and lockfile.
func asset_spec(spec: Dictionary) -> String:
	var value := "%s:%s/%s" % [spec["channel"], spec["publisher"], spec["slug"]]
	return value if str(spec["version"]).is_empty() else value + "@" + str(spec["version"])


# Find the URL belonging to a catalog name without changing origins on errors.
func asset_channel(id: String) -> String:
	for raw: Variant in asset_channels():
		var channel: Dictionary = raw
		if channel["id"] == id:
			return str(channel["url"])
	return ""


# Split a dotted version into leading numbers so releases can be ordered.
func version_parts(text: String) -> Array:
	var parts: Array = []
	for raw: String in text.split(".", false):
		var digits := ""
		for i: int in raw.length():
			var c := raw.unicode_at(i)
			if c < 48 or c > 57:
				break
			digits += raw.substr(i, 1)
		parts.append(0 if digits.is_empty() else digits.to_int())
	return parts


# Read one numeric version component. Missing components are zero.
func version_at(parts: Array, index: int) -> int:
	if index >= parts.size():
		return 0
	var raw: Variant = parts[index]
	if raw is int:
		var whole: int = raw
		return whole
	if raw is float:
		var numeric: float = raw
		return int(numeric)
	return 0


# Report whether the first version is newer than the second.
func version_newer(left: Array, right: Array) -> bool:
	var count := maxi(left.size(), right.size())
	for i: int in count:
		var a := version_at(left, i)
		var b := version_at(right, i)
		if a != b:
			return a > b
	return false


# Validate dotted engine bounds before comparing their numeric components.
func asset_bound(value: String) -> bool:
	var parts := value.split(".", true)
	if parts.size() < 1 or parts.size() > 3:
		return false
	for part: String in parts:
		if part.is_empty():
			return false
		for digit: String in part:
			if digit < "0" or digit > "9":
				return false
	return true


# Check release compatibility against the engine that will load the addon.
func asset_compatible(rel: Dictionary) -> bool:
	var engine := Engine.get_version_info()
	var current := [engine["major"], engine["minor"], engine["patch"]]
	var minimum: Variant = rel.get("min_godot_version", "0")
	var maximum: Variant = rel.get("max_godot_version")
	if not minimum is String or not asset_bound(str(minimum)) or (maximum != null and (not maximum is String or not asset_bound(str(maximum)))):
		return false
	return not version_newer(version_parts(str(minimum)), current) and (maximum == null or not version_newer(current, version_parts(str(maximum))))


# Select stable releases by publication date and ID, using numeric labels when dates are absent.
# An ambiguous display label requires the caller to select a release ID.
func pick_release(rows: Array, want: String) -> Dictionary:
	var best: Dictionary = {}
	var matched := 0
	for raw: Variant in rows:
		if not raw is Dictionary:
			continue
		var rel: Dictionary = raw
		if not rel.get("version") is String or not rel.get("stable") is bool or not rel.get("created", "") is String or not rel.get("id", 0) is int:
			continue
		var ver := str(rel.get("version", ""))
		if not asset_compatible(rel) or not asset_label(ver):
			continue
		if not want.is_empty():
			if not asset_matches(rel, want):
				continue
			matched += 1
			best = rel
			continue
		var stable: bool = rel.get("stable", false) == true
		var best_stable: bool = best.get("stable", false) == true
		var newer := version_newer(version_parts(ver.trim_prefix("v")), version_parts(str(best.get("version", "")).trim_prefix("v")))
		var created := str(rel.get("created", ""))
		var previous := str(best.get("created", ""))
		if not created.is_empty() and not previous.is_empty():
			var rel_id: float = rel.get("id", 0) # Catalog ids arrive as JSON numbers.
			var best_id: float = best.get("id", 0)
			newer = created > previous or (created == previous and rel_id > best_id)
		if best.is_empty() or (stable and not best_stable) or (stable == best_stable and newer):
			best = rel
	return {} if matched > 1 else best


# Preserve Unicode resource names while rejecting portable filesystem aliases and separators.
func asset_name(value: String) -> bool:
	if value.is_empty() or value in [".", ".."] or value.ends_with(".") or value.ends_with(" ") or windows_reserved(value):
		return false
	for i: int in value.length():
		var code := value.unicode_at(i)
		if code < 32 or code == 127 or '<>:"/\\|?*'.contains(value[i]):
			return false
	return true


# Map an archive member onto addons/, or reject a member that climbs out.
# An empty result is a member that does not belong in the project.
func addon_member(entry: String) -> String:
	var text := entry
	if text.contains("\\") or text.begins_with("/"):
		return ".."
	var parts := text.split("/", false)
	var at := -1
	for i: int in parts.size():
		var part := parts[i]
		if part in [".", ".."] or part.contains(":"):
			return ".."
		if part == "addons" and at < 0:
			at = i
	if at < 0 or text.ends_with("/") or at >= parts.size() - 1:
		return ""
	var tail := PackedStringArray()
	for i: int in range(at, parts.size()):
		var _added := tail.append(parts[i])
	var rel := "/".join(tail)
	if tail.size() < 3 or tail[1].begins_with("."):
		return ".."
	for part: String in tail:
		if not asset_name(part):
			return ".."
	return rel


# Release an archive reader. Closing is cleanup, not the copy result.
func close_archive(reader: ZIPReader) -> void:
	var _closed: int = reader.close()


# Validate the complete archive before writing into a fresh staging directory.
func place_archive(zip_path: String, project: String) -> Variant, Err:
	var reader := ZIPReader.new()
	if reader.open(zip_path) != OK:
		return null, Err.from("cannot open archive", Err.INVALID_DATA)
	var entries := {}
	var paths := {}
	var files := reader.get_files()
	if reader.get_last_error() != OK:
		close_archive(reader)
		return null, Err.from("invalid archive directory", Err.INVALID_DATA)
	for entry: String in files:
		var rel := addon_member(entry)
		if rel == ".." or (not rel.is_empty() and (entries.has(rel) or not add_portable_path(rel, paths))):
			close_archive(reader)
			return null, Err.from("unsafe or conflicting archive path", Err.PERMISSION_DENIED)
		if rel.is_empty():
			continue
		entries[rel] = entry
	if entries.is_empty():
		close_archive(reader)
		return null, Err.from("archive has no addons", Err.INVALID_DATA)
	for rel: String in entries:
		var dest := GD.file.under(project, rel)
		if dest.is_empty() or not no_links(base_dir(), dest, true) or GD.file.exists(dest):
			close_archive(reader)
			return null, Err.from("archive would overwrite existing files", Err.ALREADY_EXISTS)
	for rel: String in entries:
		var dest := GD.file.under(project, rel)
		var made_v, made_e := GD.file.make_dir(GD.file.dirname(dest))
		if made_e != null:
			close_archive(reader)
			return made_v, made_e
		# Detect aliases resolved by the target filesystem, including Unicode normalization.
		if GD.file.exists(dest):
			close_archive(reader)
			return null, Err.from("archive paths refer to the same file", Err.ALREADY_EXISTS)
		var body := reader.read_file(str(entries[rel]))
		if reader.get_last_error() != OK:
			close_archive(reader)
			return null, Err.from("invalid archive member", Err.INVALID_DATA)
		var put_v, put_e := GD.file.write_bytes(dest, body)
		if put_e != null:
			close_archive(reader)
			return put_v, put_e
	close_archive(reader)
	return entries.size()


# Locate immutable downloaded archives separately from package source caches.
func asset_cache() -> String:
	return "cache://asset"


# Select metadata without downloading archives, for install and update checks.
func asset_release(spec: Dictionary) -> Variant, Err:
	var base := asset_channel(str(spec["channel"]))
	if not secure_url(base):
		return null, Err.from("asset catalog must use HTTPS", Err.PERMISSION_DENIED)
	var listed_v, listed_e := await fetch_channel_json("%s/releases/%s/%s/" % [base, str(spec["publisher"]).uri_encode(), str(spec["slug"]).uri_encode()])
	if listed_e != null:
		return listed_v, listed_e
	if not listed_v is Array:
		return null, Err.from("invalid asset releases", Err.INVALID_DATA)
	var releases: Array = listed_v
	var found := pick_release(releases, str(spec["version"]))
	if found.is_empty():
		return null, Err.from("no compatible or unique asset release; use @#id for duplicate labels", Err.NOT_FOUND)
	return {"source": asset_spec(spec), "channel": base, "version": found["version"], "id": found.get("id", 0),
		"url": found.get("download_url", ""), "sha256": found.get("sha256", ""),
		"min_godot_version": found.get("min_godot_version", "0"), "max_godot_version": found.get("max_godot_version")}


# Resolve one catalog release or reuse the exact origin and digest in the lockfile.
func fetch_asset(spec: Dictionary, pinned: Dictionary = {}, cached_only: bool = false) -> Variant, Err:
	var base := asset_channel(str(spec["channel"]))
	if not secure_url(base):
		return null, Err.from("asset catalog must use HTTPS", Err.PERMISSION_DENIED)
	var found := pinned.duplicate(true)
	if found.is_empty():
		if cached_only:
			return null, Err.from("asset is not in gd.lock", Err.NOT_FOUND)
		var selected_v, selected_e := await asset_release(spec)
		if selected_e != null:
			return selected_v, selected_e
		found = selected_v
	if not asset_compatible(found) or not asset_label(str(found.get("version", ""))) or (not str(spec["version"]).is_empty() and not asset_matches(found, str(spec["version"]))):
		return null, Err.from("locked asset version is invalid or incompatible", Err.INVALID_DATA)
	var digest: Variant = found.get("sha256", "")
	if not digest is String or (not str(digest).is_empty() and not sha256_text(str(digest))):
		return null, Err.from("invalid asset fingerprint", Err.INVALID_DATA)
	if not pinned.is_empty() and (not sha256_text(str(digest)) or found.get("channel") != base or found.get("source") != asset_spec(spec)):
		return null, Err.from("asset origin or fingerprint differs from gd.lock", Err.INVALID_DATA)
	if str(spec["channel"]) == "gd" and not sha256_text(str(digest)):
		return null, Err.from("reviewed asset has no fingerprint", Err.INVALID_DATA)
	var download := str(found.get("url", ""))
	if not secure_url(download):
		return null, Err.from("asset download must use HTTPS", Err.PERMISSION_DENIED)
	var path := asset_cache().path_join(str(digest) + ".zip")
	if not str(digest).is_empty() and GD.file.exists(path):
		var stored_size_v, stored_size_e := GD.file.size_of(path)
		if stored_size_e != null:
			return stored_size_v, stored_size_e
		var stored_hash_v, stored_hash_e := file_sha256(path, str(stored_size_v).to_int())
		if stored_hash_e != null:
			return stored_hash_v, stored_hash_e
		if stored_hash_v == digest:
			return found
		if cached_only:
			return null, Err.from("cached asset fingerprint differs", Err.INVALID_DATA)
	if cached_only:
		return null, Err.from("asset archive is not cached", Err.NOT_FOUND)
	var made_v, made_e := GD.file.make_dir(asset_cache())
	if made_e != null:
		return made_v, made_e
	var temporary := asset_cache().path_join(".next-" + Crypto.new().generate_random_bytes(16).hex_encode())
	var opts := {"save": temporary, "headers": {"User-Agent": "gd"}}
	if not str(digest).is_empty():
		opts["sha256"] = digest
	var response, failure := await GD.http.fetch(download, opts)
	if failure != null or response.status != 200:
		if GD.file.exists(temporary):
			var _dropped_v, _dropped_e := GD.file.remove(temporary)
		if failure != null:
			return null, failure.note("fetch asset archive")
		return null, Err("asset download HTTP %d" % response.status, Err.INVALID_DATA)
	var size_v, size_e := GD.file.size_of(temporary)
	if size_e != null:
		var _bad_size_v, _bad_size_e := GD.file.remove(temporary)
		return size_v, size_e
	var hashed_v, hashed_e := file_sha256(temporary, str(size_v).to_int())
	if hashed_e != null:
		var _bad_v, _bad_e := GD.file.remove(temporary)
		return hashed_v, hashed_e
	found["sha256"] = hashed_v
	path = asset_cache().path_join(str(hashed_v) + ".zip")
	var moved_v, moved_e := GD.file.rename(temporary, path)
	if moved_e != null:
		var _failed_v, _failed_e := GD.file.remove(temporary)
		return moved_v, moved_e
	return found


# Enumerate addon files without following user-created symbolic links or cycles.
func asset_files(root: String) -> Variant, Err:
	if not no_links(base_dir(), root):
		return null, Err.from("addon contains a symbolic link", Err.PERMISSION_DENIED)
	var pending := PackedStringArray([root])
	var files := PackedStringArray()
	while not pending.is_empty():
		var path := pending[pending.size() - 1]
		var _resized := pending.resize(pending.size() - 1)
		var dir := DirAccess.open(path)
		if dir == null:
			return null, Err.from("cannot read addon directory", Err.PERMISSION_DENIED)
		dir.include_hidden = true
		if dir.list_dir_begin() != OK:
			return null, Err.from("cannot list addon directory", Err.PERMISSION_DENIED)
		var name := dir.get_next()
		while not name.is_empty():
			if dir.is_link(name):
				dir.list_dir_end()
				return null, Err.from("addon contains a symbolic link", Err.PERMISSION_DENIED)
			if dir.current_is_dir():
				var _directory := pending.append(path.path_join(name))
			else:
				var _file := files.append(path.path_join(name))
			name = dir.get_next()
		dir.list_dir_end()
	return files


# Replace or remove one owned addon directory through the graph recovery journal.
func place_asset_tree(stage: String, name: String, expected: String) -> Variant, Err:
	var root := base_dir().path_join("addons")
	var final_path := root.path_join(name)
	if not asset_name(name) or name.begins_with(".") or not no_links(base_dir(), final_path, true):
		return null, Err.from("unsafe addon directory", Err.PERMISSION_DENIED)
	var before_v, before_e := path_fingerprint(final_path, true, false)
	var after_v: Variant = ""
	var after_e: Err
	if not stage.is_empty():
		var value, error := path_fingerprint(stage, true, false)
		after_v = value
		after_e = error
	if before_e != null or after_e != null:
		if before_e != null:
			return before_v, before_e
		return after_v, after_e
	if not str(before_v).is_empty() and before_v != expected:
		return null, Err.from("addon directory is unowned or locally modified: " + name, Err.ALREADY_EXISTS)
	if before_v == after_v:
		return null
	var made_v, made_e := GD.file.make_dir(root)
	if made_e != null:
		return made_v, made_e
	var nonce := Crypto.new().generate_random_bytes(16).hex_encode()
	var pending := root.path_join(".stage-" + nonce) if not stage.is_empty() else ""
	if not stage.is_empty():
		var moved_v, moved_e := GD.file.rename(stage, pending)
		if moved_e != null:
			return moved_v, moved_e
	var backup := final_path + "." + nonce + ".old"
	var started_v, started_e := begin_txn({"final": final_path, "backup": backup, "stage": pending, "tree": true,
		"had_final": not str(before_v).is_empty(), "before": before_v, "after": after_v, "text": []})
	if started_e != null:
		if not pending.is_empty():
			var _drop_v, _drop_e := GD.file.remove_all(pending)
		return started_v, started_e
	if not str(before_v).is_empty():
		var backed_v, backed_e := GD.file.rename(final_path, backup)
		if backed_e != null:
			return backed_v, backed_e
	if not pending.is_empty():
		return GD.file.rename(pending, final_path)
	return null


# Install, update and prune assets inside the same transaction as ordinary packages.
func install_assets(cfg: Dictionary, cached_only: bool = false, frozen: bool = false, force: Dictionary = {}) -> Variant, Err:
	var data := lock()
	if not json_ok(lock_path()) or not cfg.get("assets", {}) is Dictionary or not data.get("assets", {}) is Dictionary:
		return null, Err.from("assets must be an object", Err.INVALID_DATA)
	var assets: Dictionary = cfg.get("assets", {})
	var old: Dictionary = data.get("assets", {})
	var next := {}
	var owners := {}
	var initial := {}
	for alias: String in old:
		var held: Variant = old[alias]
		if not held is Dictionary:
			return null, Err.from("invalid asset lock", Err.INVALID_DATA)
		var held_lock: Dictionary = held
		var held_roots: Variant = held_lock.get("roots", {})
		if not held_roots is Dictionary:
			return null, Err.from("invalid asset lock", Err.INVALID_DATA)
		var roots: Dictionary = held_roots
		for name: String in roots:
			if not asset_name(name) or name.begins_with(".") or not sha256_text(str(roots[name])) or initial.has(name):
				return null, Err.from("invalid asset ownership", Err.INVALID_DATA)
			initial[name] = roots[name]
	if frozen and old.size() != assets.size():
		return null, Err.from("asset requests differ from gd.lock", Err.INVALID_DATA)
	var imports: Dictionary = cfg.get("imports", {})
	for alias: String in assets:
		if not safe_segment(alias) or imports.has(alias) or not assets[alias] is String:
			return null, Err.from("invalid or duplicate asset alias", Err.INVALID_DATA)
		var spec := parse_asset(str(assets[alias]))
		if spec.is_empty():
			return null, Err.from("asset must be [official:|gd:]publisher/slug[@version]", Err.INVALID_DATA)
		var pinned: Dictionary = old.get(alias, {})
		if pinned.get("source", "") != asset_spec(spec) or force.has(alias):
			pinned = {}
		if frozen and pinned.is_empty():
			return null, Err.from("asset request differs from gd.lock", Err.INVALID_DATA)
		var got_v, got_e := await fetch_asset(spec, pinned, cached_only)
		if got_e != null:
			return got_v, got_e
		var entry: Dictionary = got_v
		# Reuse verified installed trees without extracting an unchanged archive again.
		var pin_roots: Dictionary = pinned.get("roots", {})
		var reused := not pinned.is_empty() and not pin_roots.is_empty()
		if reused:
			var roots: Dictionary = pinned["roots"]
			for name: String in roots:
				var tree := base_dir().path_join("addons").path_join(name)
				var mark_v, mark_e := path_fingerprint(tree, true, false)
				if mark_e != null:
					return mark_v, mark_e
				if str(mark_v).is_empty():
					reused = false
				elif mark_v != roots[name]:
					return null, Err.from("addon directory is locally modified: " + name, Err.ALREADY_EXISTS)
			if reused:
				for name: String in roots:
					if owners.has(name):
						return null, Err.from("assets share addon directory: " + name, Err.ALREADY_EXISTS)
					owners[name] = alias
				next[alias] = entry
				continue
		var stage := base_dir().path_join(".godot/asset-" + Crypto.new().generate_random_bytes(16).hex_encode())
		var made_v, made_e := GD.file.make_dir(stage)
		if made_e != null:
			return made_v, made_e
		var placed_v, placed_e := place_archive(asset_cache().path_join(str(entry["sha256"]) + ".zip"), stage)
		if placed_e == null:
			var roots := {}
			for name: String in DirAccess.get_directories_at(stage.path_join("addons")):
				if owners.has(name):
					placed_e = Err("assets share addon directory: " + name, Err.ALREADY_EXISTS)
					break
				owners[name] = alias
				var tree := stage.path_join("addons").path_join(name)
				var mark_v, mark_e := path_fingerprint(tree, true, false)
				if mark_e != null:
					placed_v = mark_v
					placed_e = mark_e
					break
				roots[name] = mark_v
				var value, error := place_asset_tree(tree, name, str(initial.get(name, "")))
				placed_v = value
				placed_e = error
				if placed_e != null:
					break
			entry["roots"] = roots
		var dropped_v, dropped_e := GD.file.remove_all(stage)
		if placed_e != null or dropped_e != null:
			if placed_e != null:
				return placed_v, placed_e
			return dropped_v, dropped_e
		next[alias] = entry
	for name: String in initial:
		if not owners.has(name):
			var removed_v, removed_e := place_asset_tree("", name, str(initial[name]))
			if removed_e != null:
				return removed_v, removed_e
	if next == old:
		return null
	data["assets"] = next
	var snap_v, snap_e := json_change(lock_path(), data)
	if snap_e != null:
		return snap_v, snap_e
	var started_v, started_e := begin_txn({"final": "", "backup": "", "stage": "", "tree": true,
		"had_final": false, "before": "", "after": "", "text": [snap_v]})
	if started_e != null:
		return started_v, started_e
	return apply_text(snap_v)


# Recognize an asset request written as asset:publisher/slug.
func asset_request(rest: PackedStringArray) -> bool:
	if rest.size() == 1:
		return rest[0].begins_with("asset:")
	if rest.size() == 2:
		return rest[1].begins_with("asset:")
	return false


# Record one asset and copy it into addons/.
func add_asset(rest: PackedStringArray) -> int:
	var _pkg_guard_v, pkg_guard_e := package_lock()
	if pkg_guard_e != null:
		note(pkg_guard_e.msg)
		return 1
	if not asset_request(rest):
		print("usage: gd add asset:<publisher/slug[@version]> | gd add <name> asset:<publisher/slug[@version]>")
		return 1
	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	var key := ""
	var spec := ""
	if rest.size() == 2:
		key = rest[0]
		spec = rest[1].substr(6)
	else:
		spec = rest[0].substr(6)
		var parsed := parse_asset(spec)
		if parsed.is_empty():
			print("usage: gd add asset:<publisher/slug[@version]>")
			return 1
		key = short_name(str(parsed["slug"]))
	if not safe_segment(key) or parse_asset(spec).is_empty():
		note("asset must be publisher/slug")
		return 1
	var raw_assets: Variant = cfg.get("assets", {})
	if not raw_assets is Dictionary:
		note("assets must be an object")
		return 1
	var assets: Dictionary = raw_assets
	assets[key] = asset_spec(parse_asset(spec))
	cfg["assets"] = assets
	var _placed_v, placed_e := await install_graph(cfg, false, false, false)
	if placed_e != null:
		note(placed_e.msg)
		return 1
	return 0


# Print asset hits from the default catalogs. A quiet catalog does not fail package search.
func asset_hits(query: String) -> int:
	var shown := 0
	for raw_channel: Variant in asset_channels():
		var channel: Dictionary = raw_channel
		var url := "%s/search/query/?%s" % [str(channel["url"]), GD.http.encode_query({
			"query": query,
			"require_release": "true",
			"type": "0",
			"sort": "relevance",
			"page": "1",
		})]
		var decoded_v, decoded_e := await fetch_channel_json(url)
		if decoded_e != null or not decoded_v is Dictionary:
			note("asset catalog %s unavailable: %s" % [channel["id"], decoded_e.msg if decoded_e != null else "invalid document"], "warning")
			continue
		var doc: Dictionary = decoded_v
		var raw_rows: Variant = doc.get("hits", [])
		if not raw_rows is Array:
			continue
		for raw_hit: Variant in raw_rows:
			if not raw_hit is Dictionary:
				continue
			var row: Dictionary = raw_hit
			var raw_item: Variant = row.get("asset", {})
			if not raw_item is Dictionary:
				continue
			var item: Dictionary = raw_item
			var raw_publisher: Variant = item.get("publisher", {})
			if not raw_publisher is Dictionary:
				continue
			var who: Dictionary = raw_publisher
			var publisher := str(who.get("slug", ""))
			var slug := str(item.get("slug", ""))
			if not safe_segment(publisher) or not safe_segment(slug) or not safe_description(str(item.get("name", ""))):
				continue
			print("  asset:%s:%s/%s  %s" % [str(channel["id"]), publisher, slug, str(item.get("name", ""))])
			shown += 1
	return shown


# ---------------- Subcommands ----------------

# Quote a fixed argument for a POSIX command launcher.
func shell_arg(value: String) -> String:
	return "'" + value.replace("'", "'\"'\"'") + "'"


# Infer a command from its source, using the parent for generic entry names.
func global_name(source: String, directory: bool) -> String:
	var path := source.get_slice("?", 0).get_slice("#", 0).trim_suffix("/")
	var leaf := path.get_file() if directory else path.get_file().get_basename()
	if not directory and leaf in ["main", "mod", "index", "cli"]:
		leaf = path.get_base_dir().get_file()
	return leaf.get_slice("@", 0)


# Keep command data, locks, and recovery files in disjoint managed directories.
func global_paths(name: String) -> Dictionary:
	return {"final": "global://.gd/" + name, "stage": "global://.gd-stage/" + name, "backup": "global://.gd-old/" + name, "journal": "global://.gd-txn/" + name + ".json", "launcher": "global://" + name + (".cmd" if global_info["platform"] == "windows" else "")}


# Invoke the saved entry without inserting script arguments into shell syntax.
func global_launcher(bin: String, name: String) -> String:
	var cli := str(global_info["cli"])
	if global_info["platform"] != "windows":
		return "#!/bin/sh\nexec %s --installed %s \"$@\"\n" % [shell_arg(cli), shell_arg(bin.path_join(".gd").path_join(name))]
	# Read UTF-8 paths while restoring the caller's console page and exit status.
	return "@echo off\r\nsetlocal DisableDelayedExpansion\r\nfor /f \"tokens=2 delims=:\" %%c in ('chcp') do set \"_gd_cp=%%c\"\r\nchcp 65001 >nul\r\n\"" + cli.replace("%", "%%") + "\" --installed \"%~dp0.gd\\" + name + "\" %*\r\nset \"_gd_exit=%errorlevel%\"\r\nif defined _gd_cp chcp %_gd_cp% >nul\r\nexit /b %_gd_exit%\r\n"


# Restore an interrupted publication or clean its committed backups.
func global_recover(name: String) -> Variant, Err:
	var paths := global_paths(name)
	var journal := str(paths["journal"])
	if not GD.file.exists(journal):
		return null
	var txn := load_json(journal, {})
	if not json_ok(journal) or not txn_nonce(str(txn.get("token", ""))) or not txn.get("body") is String or typeof(txn.get("had")) != TYPE_BOOL or typeof(txn.get("committed")) != TYPE_BOOL or (txn.get("before") != null and not txn.get("before") is String):
		return null, Err.from("unsafe global installation journal", Err.PERMISSION_DENIED)
	var final_path := str(paths["final"])
	var backup := str(paths["backup"])
	var current := load_json(final_path.path_join("gd.json"), {})
	var launcher := str(paths["launcher"])
	var body: Variant = null
	if GD.file.exists(launcher):
		var got_v, got_e := GD.file.read_text(launcher)
		if got_e != null:
			return got_v, got_e
		body = got_v
	var owns := current.get("global_token") == txn["token"] and current.get("global_command") == name
	if txn["committed"] == true:
		if not owns or body != txn["body"]:
			return null, Err.from("global command changed during recovery", Err.INVALID_DATA)
	else:
		# Refuse to overwrite a launcher modified outside the locked installation.
		if body != txn["before"]:
			if body != txn["body"]:
				return null, Err.from("global launcher changed during recovery", Err.INVALID_DATA)
			var restored_v: Variant
			var restored_e: Err
			if txn["before"] == null:
				restored_v, restored_e = GD.file._remove_text(launcher, str(body))
			else:
				restored_v, restored_e = GD.file.replace_text(launcher, body, str(txn["before"]))
			if restored_e != null:
				return restored_v, restored_e
		if owns:
			var dropped_v, dropped_e := GD.file.remove_all(final_path)
			if dropped_e != null:
				return dropped_v, dropped_e
		elif GD.file.exists(backup) and GD.file.exists(final_path):
			return null, Err.from("global package changed during recovery", Err.INVALID_DATA)
		if GD.file.exists(backup):
			if load_json(backup.path_join("gd.json"), {}).get("global_command") != name:
				return null, Err.from("unowned global backup", Err.PERMISSION_DENIED)
			var moved_v, moved_e := GD.file.rename(backup, final_path)
			if moved_e != null:
				return moved_v, moved_e
	for key: String in ["stage", "backup"]:
		var removed_v, removed_e := GD.file.remove_all(str(paths[key]))
		if removed_e != null:
			return removed_v, removed_e
	return GD.file.remove(journal)


# Serialize install and remove before checking command ownership or recovering state.
func global_lock(name: String) -> Variant, Err:
	for dir: String in [".gd", ".gd-stage", ".gd-old", ".gd-txn", ".gd-lock"]:
		var made_v, made_e := GD.file.make_dir("global://" + dir)
		if made_e != null:
			return made_v, made_e
	var guard_v, guard_e := GD.file._lock("global://.gd-lock/" + name)
	if guard_e != null:
		return guard_v, guard_e
	var recovered_v, recovered_e := global_recover(name)
	if recovered_e != null:
		return recovered_v, recovered_e
	var paths := global_paths(name)
	if GD.file.exists(str(paths["stage"])) or GD.file.exists(str(paths["backup"])):
		return null, Err.from("unowned global staging directory", Err.PERMISSION_DENIED)
	return guard_v


# Build and type-check a private graph before replacing the visible command.
func global_stage(paths: Dictionary, cfg: Dictionary, script: String, journal: Dictionary) -> Variant, Err:
	global_dir = str(paths["stage"])
	var guard_v, guard_e := package_lock()
	if guard_e != null:
		return guard_v, guard_e
	var ready_v, ready_e := await install_graph(cfg, false, false, true)
	guard_v = null
	if ready_e != null:
		return ready_v, ready_e
	var checked_v, checked_e := GD.file._check_global(global_dir, script, PackedStringArray(cfg["global_flags"]))
	if checked_e != null:
		return checked_v, checked_e
	var final_path := str(paths["final"])
	if journal["had"] == true:
		var saved_v, saved_e := GD.file.rename(final_path, str(paths["backup"]))
		if saved_e != null:
			return saved_v, saved_e
	var placed_v, placed_e := GD.file.rename(global_dir, final_path)
	if placed_e != null:
		return placed_v, placed_e
	var launcher := str(paths["launcher"])
	var written_v, written_e := GD.file.replace_text(launcher, journal["before"], str(journal["body"]))
	if written_e != null:
		return written_v, written_e
	if global_info["platform"] != "windows" and FileAccess.set_unix_permissions(launcher, 493) != OK:
		return null, Err.from("cannot make global command executable", Err.PERMISSION_DENIED)
	journal["committed"] = true
	if not save_json(str(paths["journal"]), journal):
		return null, Err.from("cannot commit global installation", Err.INVALID_DATA)
	return null


# Install one tool in its own locked directory and publish only its launcher.
func cmd_global_install(args: PackedStringArray) -> int:
	global_info = GD.file._global_info()
	var bin := ""
	var name := ""
	var spec := ""
	var force := false
	var run_flags := PackedStringArray()
	var fixed := PackedStringArray()
	var i := 0
	while i < args.size():
		var arg := args[i]
		if arg.begins_with("--global-bin="):
			bin = arg.trim_prefix("--global-bin=")
		elif arg.begins_with("--global-run="):
			var flag := arg.trim_prefix("--global-run=")
			if flag.begins_with("--mount="):
				var mount := flag.trim_prefix("--mount=")
				var equal := mount.find("=")
				var colon := mount.rfind(":")
				if equal >= 0 and colon > equal:
					var path := mount.substr(equal + 1, colon - equal - 1)
					if not path.is_absolute_path():
						path = ProjectSettings.globalize_path("res://").path_join(path).simplify_path()
					flag = "--mount=" + mount.substr(0, equal + 1) + path + mount.substr(colon)
			var _added := run_flags.append(flag)
		elif arg == "-g" or arg == "--global":
			pass
		elif arg == "--force" or arg == "-f":
			force = true
		elif arg == "--":
			fixed.append_array(args.slice(i + 1))
			break
		elif arg == "--name" and i + 1 < args.size():
			i += 1
			name = args[i]
			if name.is_empty():
				note("global command name must not be empty")
				return 1
		elif not arg.begins_with("-"):
			if spec.is_empty():
				spec = arg
			else:
				fixed.append(arg)
		else:
			note("usage: gd install -g [--name command] [--force] <package|url|directory>")
			return 1
		i += 1
	if spec.is_empty():
		note("usage: gd install -g [--name command] [--force] <package|url|file|directory>")
		return 1
	# Resolve the official tool before considering local files with the same name.
	if spec == "gd-godot":
		spec = editor_package()
		if name.is_empty():
			name = "gd-godot"
	var origin := spec
	var entry := "mod.gd"
	var local := spec if spec.is_absolute_path() else ProjectSettings.globalize_path("res://").path_join(spec).simplify_path()
	var source := local_source(local.get_base_dir()).path_join(local.get_file()) if not spec.contains("://") and not spec.begins_with("gd:") and not spec.begins_with("@") else ""
	var single := not source.is_empty() and FileAccess.file_exists(source)
	# Look up an unscoped name in the registry when no local file or directory has that name.
	if not source.is_empty() and not single and not DirAccess.dir_exists_absolute(source) and safe_segment(spec.get_slice("@", 0)) and not spec.begins_with("@") and not spec.ends_with(".gd"):
		var scoped, scoped_err := await short_package(spec, ["gd"])
		if scoped_err != null:
			note(scoped_err.msg)
			return 1
		origin = scoped
		source = ""
	if single:
		entry = local.get_file()
		origin = local.get_base_dir()
	elif not source.is_empty() and DirAccess.dir_exists_absolute(source):
		origin = local
	var parsed := parse_spec(origin)
	var kind := str(parsed["kind"])
	if not bin.is_absolute_path() or kind not in ["gd", "url", "local"]:
		note("global installation needs a script package, URL, or local package directory")
		return 1
	if kind == "local":
		origin = origin if origin.is_absolute_path() else ProjectSettings.globalize_path("res://").path_join(origin).simplify_path()
		if not DirAccess.dir_exists_absolute(local_source(origin)):
			note("global local source must exist")
			return 1
		if not single:
			var source_cfg := load_json(GD.file.join([local_source(origin), "gd.json"]), {})
			entry = str(source_cfg.get("main", "mod.gd"))
	var leaf := str(parsed["pkg"]).get_file() if kind == "gd" else global_name(local if single else origin, kind == "local" and not single)
	if name.is_empty():
		name = leaf
	if not safe_segment(name):
		note("global command name must be a portable file name")
		return 1
	var paths := global_paths(name)
	var _guard_v, guard_e := global_lock(name)
	if guard_e != null:
		note(guard_e.msg)
		return 1
	# Use installer configuration independently of the invoking project's manifest.
	global_dir = str(paths["stage"])
	var reg := registry()
	var before: Variant = null
	if GD.file.exists(str(paths["launcher"])):
		var old_v, old_e := GD.file.read_text(str(paths["launcher"]))
		if old_e != null:
			note(old_e.msg)
			return 1
		before = old_v
	var previous := load_json(str(paths["final"]).path_join("gd.json"), {})
	if GD.file.exists(str(paths["final"])) or before != null:
		if not force or previous.get("global_command") != name or (before != null and previous.get("global_launcher") != before):
			note("existing command %s; use --force only for an installed GD tool" % name)
			return 1
	if kind == "url":
		if not origin.get_file().get_slice("?", 0).ends_with(".gd"):
			note("global tool needs a .gd entry script")
			return 1
	if not safe_file_path(entry) or not entry.ends_with(".gd"):
		note("global tool needs a .gd entry script")
		return 1
	var script := "res://pkg/tool/" + entry
	var body := global_launcher(bin, name)
	var token := Crypto.new().generate_random_bytes(16).hex_encode()
	var cfg := {"place": "project", "global_command": name, "global_token": token, "global_entry": script, "global_args": Array(fixed), "global_flags": Array(run_flags), "global_launcher": body, "registry": reg, "imports": {"tool": origin}}
	var journal := {"token": token, "before": before, "body": body, "had": GD.file.exists(str(paths["final"])), "committed": false}
	if not save_json(str(paths["journal"]), journal):
		note("cannot prepare global installation")
		return 1
	var _ready_v, ready_e := await global_stage(paths, cfg, script, journal)
	var _recovered_v, recovered_e := global_recover(name)
	if ready_e != null or recovered_e != null:
		note((recovered_e if recovered_e != null else ready_e).msg)
		return 1
	var full := bin.path_join(name + (".cmd" if global_info["platform"] == "windows" else ""))
	print("installed %s: %s" % [name, full])
	print("Add %s to PATH if needed." % bin)
	return 0


# Select a platform package at the version of the invoking CLI.
func editor_package() -> String:
	var platform := str(global_info["platform"])
	var arch := "universal" if platform == "macos" else str(global_info["arch"])
	return "gd:@gd/gd-godot-%s-%s@%s" % [platform, arch, global_info["version"]]


# Open the editor independently or wait for a game while preserving its output and exit status.
func start_display(path: String, args: PackedStringArray, editor: bool) -> int:
	if editor:
		if OS.create_process(path, args) >= 0:
			return 0
		note("cannot start gd-godot")
		return 1
	var result, e := await GD.cli.run(path, args, {"output": false, "timeout": 0})
	if e and result.is_empty():
		note(e.msg)
	return int(result.get("code", 1))


# Resolve the same installed display package for editing and running projects.
func cmd_display(args: PackedStringArray, editor: bool) -> int:
	global_info = GD.file._global_info()
	var bin := ""
	var project := ""
	var user_args := PackedStringArray()
	for i: int in args.size():
		var arg := args[i]
		if arg == "--":
			user_args = args.slice(i + 1)
			break
		if arg.begins_with("--global-bin="):
			bin = arg.trim_prefix("--global-bin=")
		elif project.is_empty() and not arg.begins_with("-"):
			project = arg if arg.is_absolute_path() else ProjectSettings.globalize_path("res://").path_join(arg).simplify_path()
		else:
			note("choose one project path")
			return 1
	if not bin.is_absolute_path():
		return 1
	if not project.is_empty() and not DirAccess.dir_exists_absolute(project):
		note("project directory does not exist: %s" % project)
		return 1
	var cli := str(global_info["cli"])
	var suffix := cli.get_file().trim_prefix("gd")
	var sibling := cli.get_base_dir().path_join("gd-godot" + suffix)
	var launch_args := PackedStringArray(["--editor"]) if editor else PackedStringArray()
	if not project.is_empty():
		launch_args.append_array(PackedStringArray(["--path", project]))
	if not user_args.is_empty():
		launch_args.append("--")
		launch_args.append_array(user_args)
	OS.set_environment("GD_HEADLESS_BIN", cli)
	# Named build outputs share a neighboring display binary; ordinary names use installed packages.
	if suffix not in ["", ".exe"] and FileAccess.file_exists(sibling):
		return await start_display(sibling, launch_args, editor)
	var paths := global_paths("gd-godot")
	var current := load_json(str(paths["final"]).path_join("gd.json"), {})
	var wanted := editor_package()
	if current.get("global_command") != "gd-godot" or current.get("imports", {}).get("tool") != wanted or not GD.file.exists(str(paths["final"]).path_join("pkg/tool/mod.gd")):
		var code := await cmd_global_install(PackedStringArray(["-g", "--force", "--global-bin=" + bin, "gd-godot"]))
		if code != 0:
			return code
	var installed_args := PackedStringArray(["--installed", bin.path_join(".gd/gd-godot")])
	installed_args.append_array(launch_args)
	return await start_display(cli, installed_args, editor)


# Request a URL and follow redirects by hand so that every hop stays on a secure address.
func fetch_follow(url: String, opts: Dictionary = {}) -> GDHTTPResponse, Err:
	var next := url
	for _hop: int in REDIRECT_MAX:
		if not secure_url(next):
			return null, Err.from("release downloads must use HTTPS", Err.PERMISSION_DENIED)
		var res, e := await GD.http.fetch(next, opts)
		if e != null:
			return null, e
		if res.status < 300 or res.status >= 400:
			if not res.ok():
				return null, Err.from("cannot download %s: HTTP %d" % [url, res.status], Err.NOT_FOUND)
			return res
		next = str(res.headers.get("location", ""))
	return null, Err.from("too many redirects: %s" % url)


# Read the newest release tag from the address the release page redirects to.
func latest_release(base: String) -> String, Err:
	if not secure_url(base):
		return "", Err.from("release downloads must use HTTPS", Err.PERMISSION_DENIED)
	var res, e := await GD.http.fetch(base + "/latest", {"timeout": UPGRADE_TIMEOUT})
	if e != null:
		return "", e
	var target := str(res.headers.get("location", ""))
	if res.status < 300 or res.status >= 400 or not target.contains("/tag/"):
		return "", Err.from("cannot find the latest release at %s" % base, Err.NOT_FOUND)
	return target.get_slice("/tag/", 1).get_slice("?", 0)


# Validate a canonical release identifier before it is placed in a URL.
func release_tag(tag: String) -> bool:
	if not package_version(tag):
		return false
	if not tag.contains("-"):
		return true
	var status := tag.get_slice("-", 1).split(".")
	return tag.get_slice_count("-") == 2 and status.size() == 2 and status[0] in ["dev", "beta", "rc"] and status[1].is_valid_int() and status[1].to_int() > 0


# Read the executable out of a verified release archive.
func release_executable(path: String, member: String) -> PackedByteArray, Err:
	if path.ends_with(".zip"):
		var zip := ZIPReader.new()
		if zip.open(path) != OK or not zip.file_exists(member):
			return PackedByteArray(), Err.from("release archive has no %s" % member, Err.INVALID_DATA)
		var body := zip.read_file(member)
		zip.close()
		return body
	var packed, read_e := GD.file.read_bytes(path)
	if read_e != null:
		return PackedByteArray(), read_e
	var entries, tar_e := GD.data.untar(packed.decompress_dynamic(-1, FileAccess.COMPRESSION_GZIP))
	if tar_e != null:
		return PackedByteArray(), tar_e
	for raw: Variant in entries:
		var entry: Dictionary = raw
		if entry["name"] == member and not entry["is_dir"]:
			return entry["body"]
	return PackedByteArray(), Err.from("release archive has no %s" % member, Err.INVALID_DATA)


# Replace the running executable with a checksum-verified release.
func cmd_upgrade(args: PackedStringArray) -> int:
	global_info = GD.file._global_info()
	var bin := ""
	var wanted := ""
	var force := false
	var dry := false
	for arg: String in args:
		if arg.begins_with("--global-bin="):
			bin = arg.trim_prefix("--global-bin=")
		elif arg == "-f" or arg == "--force":
			force = true
		elif arg == "--dry-run":
			dry = true
		elif wanted.is_empty() and not arg.begins_with("-"):
			wanted = arg
		else:
			note("usage: gd upgrade [--dry-run] [--force] [version]")
			return 1
	var cli := str(global_info.get("cli", "")).replace("\\", "/")
	if not bin.is_absolute_path() or cli.get_base_dir() != bin:
		note("cannot locate the running executable")
		return 1
	# Leave package-manager installations to the tool that owns them.
	if cli.contains("/Cellar/") or cli.begins_with("/usr/bin/"):
		note("this gd was installed by a package manager; upgrade it with brew upgrade gd or apt upgrade gd")
		return 1
	var platform := str(global_info["platform"])
	var windows := platform == "windows"
	var archive := "gd-macos-universal.zip" if platform == "macos" else "gd-%s-%s.%s" % [platform, global_info["arch"], "zip" if windows else "tar.gz"]
	var current := str(global_info["version"])
	var base := OS.get_environment("GD_RELEASES").trim_suffix("/")
	if base.is_empty():
		base = RELEASES_FALLBACK
	var tag := wanted
	if tag.is_empty():
		var latest, latest_e := await latest_release(base)
		if latest_e != null:
			note(latest_e.msg)
			return 1
		tag = latest
	if not release_tag(tag):
		note("invalid release version: %s" % tag)
		return 1
	# Without an explicit version, never move to an older or equal release.
	var candidate, candidate_e := GD.version.parse(tag)
	var installed, installed_e := GD.version.parse(current)
	if candidate_e != null or installed_e != null:
		note("cannot compare release versions")
		return 1
	var newer := GD.version.compare(candidate, installed) > 0
	if not force and (tag == current or (wanted.is_empty() and not newer)):
		print("gd %s is up to date" % current)
		return 0
	if dry:
		print("would upgrade gd %s -> %s" % [current, tag])
		return 0

	# Take the checksum from the release's own list, then let the download verify against it.
	var from := "%s/download/%s/" % [base, tag]
	var sums, sums_e := await fetch_follow(from + "SHA256SUMS", {"timeout": UPGRADE_TIMEOUT, "max_body": 65536})
	if sums_e != null:
		note(sums_e.msg)
		return 1
	var digests := PackedStringArray()
	for line: String in sums.text().split("\n", false):
		if line.strip_edges().get_slice("  ", 1) == archive:
			var _added := digests.append(line.get_slice(" ", 0))
	if digests.size() != 1 or not sha256_text(digests[0]):
		note("release %s lists no checksum for %s" % [tag, archive])
		return 1
	var work := bin.path_join(".gd-upgrade")
	var _cleared_v, _cleared_e := GD.file.remove_all(work)
	var _made_v, made_e := GD.file.ensure_dir(work)
	if made_e != null:
		note("cannot write beside %s: %s" % [cli, made_e.text()])
		return 1
	var code := await upgrade_replace(from + archive, digests[0], work, cli, windows)
	var _dropped_v, _dropped_e := GD.file.remove_all(work)
	if code == 0:
		print("upgraded gd %s -> %s" % [current, tag])
	return code


# Download, verify, extract, and move the new executable over the running one.
func upgrade_replace(url: String, digest: String, work: String, cli: String, windows: bool) -> int:
	var packed := work.path_join(url.get_file())
	var _got, got_e := await fetch_follow(url, {"save": packed, "sha256": digest})
	if got_e != null:
		note(got_e.msg)
		return 1
	var body, body_e := release_executable(packed, "gd/gd.exe" if windows else "gd/gd")
	if body_e != null:
		note(body_e.msg)
		return 1
	var staged := work.path_join(cli.get_file())
	var _written_v, written_e := GD.file.write_bytes(staged, body)
	if written_e != null:
		note(written_e.msg)
		return 1
	if not windows and FileAccess.set_unix_permissions(staged, EXECUTABLE_MODE) != OK:
		note("cannot mark the new executable as runnable")
		return 1
	# Run the new executable once before it takes the place of the current one.
	if OS.execute(staged, PackedStringArray(["--version"])) != 0:
		note("the downloaded executable does not run on this machine")
		return 1
	if windows:
		# A running executable cannot be overwritten there, so move it aside first.
		var old := cli + ".old"
		var _gone_v, _gone_e := GD.file.remove(old)
		var _aside_v, aside_e := GD.file.rename(cli, old)
		if aside_e != null:
			note(aside_e.msg)
			return 1
	var _moved_v, moved_e := GD.file.rename(staged, cli)
	if moved_e != null:
		note(moved_e.msg)
		return 1
	return 0


# Remove only a command whose private manifest identifies it as a GD installation.
func cmd_global_remove(args: PackedStringArray) -> int:
	global_info = GD.file._global_info()
	var bin := ""
	var name := ""
	for arg: String in args:
		if arg.begins_with("--global-bin="):
			bin = arg.trim_prefix("--global-bin=")
		elif arg == "-g" or arg == "--global":
			pass
		elif name.is_empty() and not arg.begins_with("-"):
			name = arg
		else:
			note("usage: gd uninstall -g <command>")
			return 1
	if not bin.is_absolute_path() or not safe_segment(name):
		note("usage: gd uninstall -g <command>")
		return 1
	var paths := global_paths(name)
	var _guard_v, guard_e := global_lock(name)
	if guard_e != null:
		note(guard_e.msg)
		return 1
	global_dir = str(paths["final"])
	if config().get("global_command") != name:
		note("no GD global command named %s" % name)
		return 1
	var launcher := str(paths["launcher"])
	if GD.file.exists(launcher):
		var _removed_v, removed_e := GD.file._remove_text(launcher, str(config().get("global_launcher", "")))
		if removed_e != null:
			note(removed_e.msg)
			return 1
	var _dropped_v, dropped_e := GD.file.remove_all(global_dir)
	if dropped_e != null:
		note(dropped_e.msg)
		return 1
	print("removed %s" % name)
	return 0


func cmd_install(args: PackedStringArray) -> int:
	if args.has("-g") or args.has("--global"):
		return await cmd_global_install(args)
	# Install named dependencies through the same transaction as explicit additions.
	if not plain(args).is_empty():
		if args.has("--frozen") or args.has("--cached-only") or args.has("--sync"):
			note("restore options require gd install without package arguments")
			return 1
		return await cmd_add(args)
	var _pkg_guard_v, pkg_guard_e := package_lock()
	if pkg_guard_e != null:
		note(pkg_guard_e.msg)
		return 1
	var cached_only: bool = args.has("--cached-only")
	var frozen: bool = args.has("--frozen")
	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	if not place_ok():
		note('place in gd.json must be "cache" or "project"')
		return 1
	var _lock_data: Dictionary = lock()
	if not json_ok(lock_path()):
		return 1
	if not cfg.get("assets", {}) is Dictionary:
		note("assets must be an object")
		return 1
	var imports: Dictionary = cfg.get("imports", {})
	var assets: Dictionary = cfg.get("assets", {})
	if imports.is_empty() and assets.is_empty() and _lock_data.is_empty():
		print("no imports in gd.json")
		return 0
	var _installed_v, installed_e := await install_graph(cfg, cached_only, frozen, not args.has("--sync"))
	if installed_e != null:
		note(installed_e.msg)
		return 1
	return 0


func cmd_add(args: PackedStringArray) -> int:
	var asset_args: PackedStringArray = plain(args)
	if asset_request(asset_args):
		return await add_asset(asset_args)
	var _pkg_guard_v, pkg_guard_e := package_lock()
	if pkg_guard_e != null:
		note(pkg_guard_e.msg)
		return 1
	var rest: PackedStringArray = plain(args)
	if rest.is_empty() or rest.size() > 2:
		print("usage: gd add <name|@scope/name[@range]> | gd add <name> <url>")
		return 1
	var cfg: Dictionary = config()
	if not json_ok(config_path()):
		return 1
	if not place_ok():
		note('place in gd.json must be "cache" or "project"')
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
		var wanted := rest[0]
		# Look up an unscoped name in the registry.
		if safe_segment(wanted.get_slice("@", 0)) and not wanted.begins_with("@"):
			var scoped, scoped_err := await short_package(wanted, ["gd", "ext"])
			if scoped_err != null:
				note(scoped_err.msg)
				return 1
			wanted = scoped
		var given: Dictionary = parse_spec(wanted)
		if str(given["kind"]) == "bad":
			print("usage: gd add <name|@scope/name[@range]> | gd add <name> <url|path>")
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
		var picked_v, picked_e := await choose(str(sp["pkg"]), str(sp["range"]), "", {}, false, false, key)
		if picked_e != null:
			note(picked_e.msg)
			return 1
		force[key] = str(picked_v)
	var _r_v, r_e := await install_graph(cfg, false, false, false, force)
	if r_e != null:
		note(r_e.msg)
		return 1
	return 0


# Remove a direct request and prune its unreferenced files in one graph transaction.
func cmd_remove(args: PackedStringArray) -> int:
	if args.has("-g") or args.has("--global"):
		return cmd_global_remove(args)
	var _pkg_guard_v, pkg_guard_e := package_lock()
	if pkg_guard_e != null:
		note(pkg_guard_e.msg)
		return 1
	var rest := plain(args)
	if rest.size() != 1:
		note("usage: gd uninstall <name>")
		return 1
	var cfg := config()
	if not json_ok(config_path()) or not cfg.get("assets", {}) is Dictionary:
		return 1
	var imports: Dictionary = cfg.get("imports", {})
	var assets: Dictionary = cfg.get("assets", {})
	var key := rest[0]
	if assets.has(key):
		var _asset: bool = assets.erase(key)
		cfg["assets"] = assets
	else:
		if not imports.has(key):
			for alias: String in imports:
				if parse_spec(str(imports[alias]))["pkg"] == key:
					key = alias
					break
		if not imports.has(key):
			print("%s is not in gd.json" % rest[0])
			return 1
		var _import: bool = imports.erase(key)
		cfg["imports"] = imports
	var _removed_v, removed_e := await install_graph(cfg, false, false, false)
	if removed_e != null:
		note(removed_e.msg)
		return 1
	print("removed %s" % key)
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
	var failed := 0
	for k: Variant in imports.keys():
		var name: String = str(k)
		var sp: Dictionary = parse_spec(str(imports[k]))
		var kind: String = str(sp["kind"])
		if kind != "gd" and kind != "ext":
			continue # Dependencies without versions cannot be compared.
		var vs_v, vs_e := await fetch_versions(str(sp["pkg"]))
		if vs_e != null:
			print("  %s  ? (%s)" % [name, vs_e.msg])
			failed += 1
			continue
		var now: String = str(locked.get(name, "-"))
		var list: PackedStringArray = vs_v
		var in_range: String = pick(list, str(sp["range"]))
		var newest: String = pick(list, "*")
		if now != in_range or now != newest:
			behind += 1
			print("  %s  %s -> %s (latest %s)" % [name, now, in_range if not in_range.is_empty() else "-", newest])
	var assets: Variant = cfg.get("assets", {})
	var pins: Variant = lock_data.get("assets", {})
	if not assets is Dictionary or not pins is Dictionary:
		note("assets must be an object")
		return 1
	var pin_map: Dictionary = pins
	for alias: String in assets:
		var spec := parse_asset(str(assets[alias]))
		var latest_v: Variant
		var latest_e: Err
		if spec.is_empty():
			latest_e = Err("invalid asset request", Err.INVALID_DATA)
		else:
			var value, error := await asset_release(spec)
			latest_v = value
			latest_e = error
		var pin: Variant = pin_map.get(alias, {})
		if latest_e != null or not pin is Dictionary:
			print("  %s  ? (%s)" % [alias, latest_e.msg if latest_e != null else "invalid asset lock"])
			failed += 1
			continue
		var held: Dictionary = pin
		var rel: Dictionary = latest_v
		var now := str(held.get("version", "-"))
		if now != str(rel["version"]) or held.get("id", 0) != rel.get("id", 0):
			behind += 1
			print("  %s  %s -> %s" % [alias, now, rel["version"]])
	if behind == 0 and failed == 0:
		print("all up to date")
	return 1 if failed else 0


func cmd_update(args: PackedStringArray) -> int:
	var _pkg_guard_v, pkg_guard_e := package_lock()
	if pkg_guard_e != null:
		note(pkg_guard_e.msg)
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
		note('place in gd.json must be "cache" or "project"')
		return 1
	var imports: Dictionary = cfg.get("imports", {})
	var addons: Variant = cfg.get("assets", {})
	if not only.is_empty() and not imports.has(only) and not (addons is Dictionary and addons.has(only)):
		note("%s is not in gd.json" % only)
		return 1
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
		var vs_v, vs_e := await fetch_versions(str(sp["pkg"]))
		if vs_e != null:
			print("  %s  ? (%s)" % [name, vs_e.msg])
			failed += 1
			continue
		var list: PackedStringArray = vs_v
		var want: String = pick(list, "*") if latest else pick(list, str(sp["range"]))
		if want.is_empty() or want == str(locked.get(name, "")):
			continue
		if latest:
			sp["range"] = "^" + want
			next_imports[name] = spec_text(sp)
		force[name] = want
	var assets: Variant = cfg.get("assets", {})
	if assets is Dictionary:
		for alias: String in assets:
			if only.is_empty() or only == alias:
				force[alias] = ""
	if force.is_empty():
		if failed == 0:
			print("nothing to update")
		return 1 if failed > 0 else 0
	next_cfg["imports"] = next_imports
	# Move the chosen aliases and re-resolve what the new versions import, in one pass.
	var _r_v, r_e := await install_graph(next_cfg, false, false, false, force)
	if r_e != null:
		note(r_e.msg)
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


# Fetch validated registry packages that contain every query word, best match first.
func search_hits(query: String) -> Array, Err:
	var registry_url := registry()
	if not secure_url(registry_url):
		return [], Err.from("remote registries must use HTTPS", Err.PERMISSION_DENIED)
	var url: String = "%s/-/search?%s" % [registry_url, GD.http.encode_query({"q": query})]
	var res, res_err := await GD.http.fetch(url)
	if res_err != null:
		return [], res_err
	if not res.ok():
		return [], Err.from("cannot reach the registry: %s" % registry_url)
	var decoded, decode_error := res.json()
	if decode_error != null or not decoded is Dictionary:
		return [], Err.from("broken answer from the registry", Err.INVALID_DATA)
	var doc: Dictionary = decoded
	var raw_hits: Variant = doc.get("packages", [])
	var broken := Err.from("broken package catalog", Err.INVALID_DATA)
	if not raw_hits is Array:
		return [], broken
	var catalog: Array = raw_hits
	var words := PackedStringArray(query.split(" ", false))
	var hits: Array = []
	for raw_hit: Variant in catalog:
		if not raw_hit is Dictionary:
			return [], broken
		var item: Dictionary = raw_hit
		var kind := str(item.get("kind", ""))
		var pkg := str(item.get("pkg", ""))
		var latest := str(item.get("latest", ""))
		var raw_description: Variant = item.get("description", "")
		if not raw_description is String:
			return [], broken
		var description: String = raw_description
		if not safe_description(description) or (kind != "gd" and kind != "ext") \
				or not safe_package(pkg) or not package_version(latest):
			return [], broken
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
	return hits


# Expand an unscoped name to a registry package, asking which one when several scopes publish it.
func short_package(spec: String, kinds: PackedStringArray) -> String, Err:
	var name := spec.get_slice("@", 0).to_lower()
	var range_txt := spec.substr(name.length())
	var hits, hits_err := await search_hits(name)
	if hits_err != null:
		return "", hits_err
	var found := PackedStringArray()
	for raw: Variant in hits:
		var item: Dictionary = raw
		var pkg := str(item["pkg"])
		if kinds.has(str(item["kind"])) and pkg.get_slice("/", 1).to_lower() == name:
			var _added := found.append("%s:%s" % [item["kind"], pkg])
	if found.is_empty():
		return "", Err.from("no package named %s; try gd search %s" % [name, name], Err.NOT_FOUND)
	var pick := 0
	if found.size() > 1:
		# Leave the choice to the person; a script must name the scope itself.
		if not GD.cli.stdin_tty():
			return "", Err.from("several packages are named %s: %s; write the one you want with its scope" % [name, ", ".join(found)], Err.INVALID_DATA)
		print("Several packages are named %s:" % name)
		for i: int in found.size():
			print("  %d) %s" % [i + 1, found[i]])
		printraw("Which one? [1-%d] " % found.size())
		pick = OS.read_string_from_stdin().strip_edges().to_int() - 1
		if pick < 0 or pick >= found.size():
			return "", Err.from("no package chosen", Err.INVALID_DATA)
	print("%s -> %s" % [name, found[pick]])
	return found[pick] + range_txt


# Ask the registry query index for packages that contain every word.
func cmd_search(args: PackedStringArray) -> int:
	var rest: PackedStringArray = plain(args)
	if rest.is_empty():
		print("usage: gd search <words...>")
		return 1
	var query := " ".join(rest).strip_edges().to_lower()
	var hits, hits_err := await search_hits(query)
	if hits_err != null:
		note(hits_err.msg)
		return 1
	var shown := 0
	for h: Variant in hits:
		var it: Dictionary = h
		var kind := str(it.get("kind", ""))
		var pkg := str(it.get("pkg", ""))
		var latest := str(it.get("latest", ""))
		var mark := "  [godot]" if it.get("godot", false) == true else ""
		print("  %s:%s@^%s  %s%s" % [kind, pkg, latest, it.get("description", ""), mark])
		shown += 1
	var asset_shown := 0
	if official_registry():
		asset_shown = await asset_hits(query)
	if shown == 0 and asset_shown == 0:
		print("no match")
	return 0


# Check each path component for symlinks from the trusted root onward.
func no_links(root: String, path: String, missing: bool = false) -> bool:
	var root_path := root if root.is_absolute_path() else base_dir().path_join(root)
	var file_path := path if path.is_absolute_path() else base_dir().path_join(path)
	var rel := GD.file.relative(root_path.simplify_path(), file_path.simplify_path())
	if rel.is_empty() or rel == ".." or rel.begins_with("../"):
		return false
	var current := root_path.simplify_path()
	for part: String in rel.split("/", false):
		if missing and not GD.file.exists(current):
			return true
		var dir := DirAccess.open(current)
		if dir == null or dir.is_link(part):
			return false
		current = GD.file.join([current, part])
	return true


# Add a regular file by its package-root-relative name.
func add_pure_file(root: String, path: String, files: Dictionary, portable: Dictionary) -> Variant, Err:
	var rel := GD.file.relative(root, path)
	if not no_links(base_dir(), path) or not FileAccess.file_exists(path):
		return null, Err.from("include accepts regular files only: %s" % rel, Err.PERMISSION_DENIED)
	if not safe_file_path(rel) or not add_portable_path(rel, portable):
		return null, Err.from("include path is not portable: %s" % rel, Err.INVALID_DATA)
	files[rel] = path
	return null


# Collect regular directory files without following symlinks.
func walk_pure(root: String, path: String, files: Dictionary, portable: Dictionary) -> Variant, Err:
	var dir := DirAccess.open(path)
	if dir == null:
		return null, Err.from("cannot read include directory: %s" % path, Err.PERMISSION_DENIED)
	# Include ordinary dotfiles as package contents.
	dir.include_hidden = true
	if dir.list_dir_begin() != OK:
		return null, Err.from("cannot list include directory: %s" % path, Err.PERMISSION_DENIED)
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
			return null, Err.from("include cannot contain a symlink: %s" % child, Err.PERMISSION_DENIED)
		var added_v: Variant
		var added_e: Err
		if item["dir"] == true:
			var value, error := walk_pure(root, child, files, portable)
			added_v = value
			added_e = error
		else:
			var value, error := add_pure_file(root, child, files, portable)
			added_v = value
			added_e = error
		if added_e != null:
			return added_v, added_e
	return null


# Resolve explicit script-package includes into a tree relative to the main directory.
func pure_files(cfg: Dictionary, entry_path: String) -> Variant, Err:
	if GD.file.basename(entry_path, "") != "mod.gd":
		return null, Err.from("main of a script package must be mod.gd", Err.INVALID_DATA)
	if not no_links(base_dir(), entry_path) or not FileAccess.file_exists(entry_path):
		return null, Err.from("main must be a regular file, not a symlink", Err.PERMISSION_DENIED)
	var raw_include: Variant = cfg.get("include", [])
	if not raw_include is Array:
		return null, Err.from("include in gd.json must be an array of paths", Err.INVALID_DATA)
	var root := GD.file.dirname(entry_path.simplify_path())
	var files := {"mod.gd": entry_path.simplify_path()}
	var portable := {"mod.gd": {"path": "mod.gd", "file": true}}
	for raw_item: Variant in raw_include:
		if not raw_item is String or not safe_file_path(str(raw_item)):
			return null, Err.from("include needs safe relative paths", Err.INVALID_DATA)
		var item := GD.file.join([base_dir(), str(raw_item)]).simplify_path()
		if not GD.file.exists(item):
			return null, Err.from("include not found: %s" % raw_item, Err.NOT_FOUND)
		if not no_links(base_dir(), item):
			return null, Err.from("include cannot contain a symlink: %s" % raw_item, Err.PERMISSION_DENIED)
		var from_root := GD.file.relative(root, item)
		var same_root := item.trim_suffix("/") == root.trim_suffix("/")
		if (from_root.is_empty() and not same_root) or from_root.begins_with("../") or from_root == "..":
			return null, Err.from("include must stay inside the main directory", Err.PERMISSION_DENIED)
		var dir := DirAccess.open(item)
		var added_v: Variant
		var added_e: Err
		if dir != null:
			var value, error := walk_pure(root, item, files, portable)
			added_v = value
			added_e = error
		else:
			var value, error := add_pure_file(root, item, files, portable)
			added_v = value
			added_e = error
		if added_e != null:
			return added_v, added_e
	return files


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
		note("name in gd.json must be @scope/name")
		return 1
	if not package_version(version):
		note("version in gd.json must be a full Semantic Version without build metadata")
		return 1

	var entry: String = str(cfg.get("main", "mod.gd"))
	var entry_path := GD.file.under(base_dir(), entry)
	if entry_path.is_empty() or not GD.file.exists(entry_path):
		note("entry not found: %s" % entry)
		return 1
	var native := entry.ends_with(".gdextension")
	if not native and not entry.ends_with(".gd"):
		note("entry must be a .gd or .gdextension file")
		return 1
	if not no_links(base_dir(), entry_path) or not FileAccess.file_exists(entry_path):
		note("entry must be a regular file, not a symlink")
		return 1
	var entry_size_v, entry_size_e := GD.file.size_of(entry_path)
	if entry_size_e != null:
		note(entry_size_e.msg)
		return 1
	var measured := str(entry_size_v).to_int()
	if measured > PACKAGE_MAX:
		note("a package is limited to 500 MiB")
		return 1
	var body := ""
	var entry_leaf := GD.file.basename(entry, "")
	var files: Dictionary = {entry_leaf: entry_path}
	var total: int = measured
	var measured_files := {entry_leaf: measured} # Pre-read sizes used to detect replacement after reading.
	var classes: Array = []
	if native:
		if cfg.has("include"):
			note("include is for script packages only")
			return 1
		var got_v, got_e := GD.file.read_text(entry_path)
		if got_e != null:
			note(got_e.msg)
			return 1
		body = str(got_v)
		var manifest_bytes := body.to_utf8_buffer()
		total += manifest_bytes.size() - measured
		if total > PACKAGE_MAX:
			note("entry size changed while reading")
			return 1
		files[entry_leaf] = manifest_bytes
		var _erased_entry: bool = measured_files.erase(entry_leaf)
		var parsed, parse_error := GD.data.ini(body)
		if parse_error != null or not parsed is Dictionary:
			note("[libraries] in .gdextension is broken")
			return 1
		var manifest: Dictionary = parsed
		if not manifest.has("libraries") or not manifest["libraries"] is Dictionary:
			note("[libraries] in .gdextension is broken")
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
				note("library not found: %s" % rel)
				return 1
			var lib_size_v, lib_size_e := GD.file.size_of(path)
			if lib_size_e != null:
				note(lib_size_e.msg)
				return 1
			var lib_bytes := str(lib_size_v).to_int()
			total += lib_bytes
			if total > PACKAGE_MAX:
				note("a package is limited to 500 MiB")
				return 1
			files[rel] = path
			measured_files[rel] = lib_bytes
	else:
		var included_v, included_e := pure_files(cfg, entry_path)
		if included_e != null:
			note(included_e.msg)
			return 1
		files = included_v
		for raw_leaf: Variant in files:
			var leaf := str(raw_leaf)
			if leaf == entry_leaf:
				continue
			var file_size_v, file_size_e := GD.file.size_of(str(files[raw_leaf]))
			if file_size_e != null:
				note(file_size_e.msg)
				return 1
			var bytes := str(file_size_v).to_int()
			total += bytes
			if total > PACKAGE_MAX:
				note("a package is limited to 500 MiB")
				return 1
			measured_files[leaf] = bytes

	# Publish the package's own imports so installers can resolve them; only registry packages travel.
	var raw_imports: Variant = cfg.get("imports", {})
	if not raw_imports is Dictionary:
		note("imports in gd.json must be a dictionary")
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
				note("%s points to %s, which has no gd.json with name and version, so it cannot be published" % [alias, sp["url"]])
				return 1
			var head := "ext:" if str(local_cfg.get("main", "mod.gd")).ends_with(".gdextension") else "gd:"
			imports[raw_alias] = head + local_name + "@^" + local_version
			kind = "gd"
		if not safe_segment(alias) or alias.begins_with("@") or (kind != "gd" and kind != "ext"):
			note("package imports accept only gd:@scope/name or ext:@scope/name: %s" % alias)
			return 1

	if args.has("--dry-run"):
		print("would publish %s@%s (%d bytes, %d files)" % [pkg, version, total, files.size()])
		return 0

	# Fingerprint every file; a release names each file by its SHA-256, so one release holds them all.
	var marks := {}
	total = 0
	for raw_leaf: Variant in files:
		var mark_v, mark_e := publish_mark(files[raw_leaf])
		if mark_e != null:
			note(mark_e.msg)
			return 1
		var mark: Dictionary = mark_v
		total += int(mark["size"])
		if total > PACKAGE_MAX:
			note("a package is limited to 500 MiB")
			return 1
		marks[str(raw_leaf)] = mark
	var positional := plain(args)
	if positional.is_empty():
		var out_dir := GD.file.join([base_dir(), "tmp", "release", "%s-%s" % [pkg.trim_prefix("@").replace("/", "-"), version]])
		var written_v, written_e := write_release(files, marks, out_dir)
		if written_e != null:
			note(written_e.msg)
			return 1
		print("wrote %d files for %s@%s to %s" % [int(written_v), pkg, version, out_dir])
		print("upload them to one GitHub Release, then run: gd publish https://github.com/OWNER/REPO/releases/download/TAG")
		return 0
	var files_url := positional[0].trim_suffix("/")
	if not secure_url(files_url) or files_url.contains("?") or files_url.contains("#"):
		note("the release address must be an HTTPS URL without a query")
		return 1

	# Read publishing secrets only from the process environment, never distributable gd.json.
	var token: String = OS.get_environment("GD_TOKEN", "")
	if token.is_empty():
		note("GD_TOKEN is required; set the token issued by the registry")
		return 1
	# Confirm the release serves exactly these files before the registry lists them.
	for raw_leaf: Variant in marks:
		var checked_v, checked_e := await check_release_file(files_url, marks[raw_leaf])
		if checked_e != null:
			note("%s: %s" % [str(raw_leaf), checked_e.msg])
			return int(checked_v != null)
	var entry_mark: Dictionary = marks[entry_leaf]

	var payload: Dictionary = {
		"pkg": pkg,
		"version": version,
		"description": str(cfg.get("description", "")),
		"leaf": entry_leaf,
		"files": marks,
		"files_url": files_url,
		"sha256": str(entry_mark["sha256"]),
		"classes": classes, # Type names obtained from the manifest.
		"imports": imports,
		"godot": cfg.get("godot", false) == true, # Runs on the upstream engine without the local API.
	}
	var encoded_payload_v, encoded_payload_e := encode_json(payload, 0)
	if not (encoded_payload_e == null):
		note("cannot encode publish metadata: %s" % encoded_payload_e)
		return 1
	var payload_text: String = encoded_payload_v

	var res, _res_err := await GD.http.fetch(registry_url + "/-/publish", {
		"method": "POST",
		"headers": {"Content-Type": "application/json", "Authorization": "Bearer " + token},
		"body": payload_text,
	})
	if not (_res_err == null and res.ok()):
		note("publish failed: %s" % (_res_err.text() if _res_err else res.text()))
		return 1
	var decoded, decode_error := res.json()
	if decode_error == null and decoded is Dictionary:
		var answer: Dictionary = decoded
		if str(answer.get("status", "")) == "review":
			print("submitted %s@%s for review %s" % [pkg, version, str(answer.get("id", ""))])
			return 0
	print("published %s@%s" % [pkg, version])
	return 0


# Measure one file to publish, given as a path or as bytes already read.
func publish_mark(source: Variant) -> Variant, Err:
	if source is PackedByteArray:
		var bytes: PackedByteArray = source
		return {"size": bytes.size(), "sha256": GD.data.hex_encode(GD.data.sha256(bytes))}
	var size_v, size_e := GD.file.size_of(str(source))
	if size_e != null:
		return size_v, size_e
	var digest_v, digest_e := file_sha256(str(source), int(size_v))
	if digest_e != null:
		return digest_v, digest_e
	return {"size": int(size_v), "sha256": str(digest_v)}


# Write each file under its SHA-256 name, ready to upload as release assets.
func write_release(files: Dictionary, marks: Dictionary, out_dir: String) -> Variant, Err:
	if GD.file.exists(out_dir):
		var _cleared_v, cleared_e := GD.file.remove_all(out_dir)
		if cleared_e != null:
			return null, cleared_e
	var _made_v, made_e := GD.file.make_dir(out_dir)
	if made_e != null:
		return null, made_e
	var names := {}
	for raw_leaf: Variant in files:
		var target := GD.file.join([out_dir, str(marks[raw_leaf]["sha256"])])
		if names.has(target):
			continue # Identical files share one release asset.
		names[target] = true
		var source: Variant = files[raw_leaf]
		var put_e: Err
		if source is PackedByteArray:
			var _put_v, error := GD.file.write_bytes(target, source)
			put_e = error
		else:
			var _put_v, error := GD.file.copy(str(source), target)
			put_e = error
		if put_e != null:
			return null, put_e
	return names.size()


# Download one released file and confirm its size and fingerprint.
func check_release_file(files_url: String, mark: Dictionary) -> Variant, Err:
	var scratch := GD.file.join([base_dir(), "tmp", "release-check", GD.id.uuid()])
	var _made_v, _made_e := GD.file.make_dir(GD.file.dirname(scratch))
	var _res, fetch_e := await fetch_follow(files_url + "/" + str(mark["sha256"]), {"save": scratch, "max_body": maxi(int(mark["size"]), REPLY_MIN), "timeout": 120.0})
	if fetch_e != null:
		if GD.file.exists(scratch):
			var _drop_v, _drop_e := GD.file.remove(scratch)
		return 1, fetch_e
	var digest_v, digest_e := file_sha256(scratch, int(mark["size"]))
	var size_v, _size_e := GD.file.size_of(scratch)
	var _drop_v, _drop_e := GD.file.remove(scratch)
	if digest_e != null or str(digest_v) != str(mark["sha256"]) or int(size_v) != int(mark["size"]):
		return 1, Err.from("the released file differs from the local file", Err.INVALID_DATA)
	return null


# Extract only non-flag arguments.
func plain(args: PackedStringArray) -> PackedStringArray:
	var out: PackedStringArray = []
	for a: String in args:
		if not a.begins_with("-"):
			var _a: bool = out.append(a)
	return out


# Combine subcommand and diagnostic-output results into the exit status.
func main(argv: Array) -> int:
	return await dispatch(argv)


# Execute the subcommand selected by the arguments.
func dispatch(argv: Array) -> int:
	if argv.is_empty():
		note("usage: gd <install|add|uninstall|upgrade|outdated|update|search|publish> ...")
		return 1
	var cmd: String = str(argv[0])
	var args: PackedStringArray = []
	for i: int in range(1, argv.size()):
		var _a: bool = args.append(str(argv[i]))
	# Reject unsupported previews before any command can modify package state.
	if args.has("--dry-run") and cmd != "publish" and cmd != "upgrade":
		note("--dry-run is supported only by gd publish and gd upgrade")
		return 1

	if cmd == "install":
		return await cmd_install(args)
	if cmd == "editor" or cmd == "run-game":
		return await cmd_display(args, cmd == "editor")
	if cmd == "add":
		return await cmd_add(args)
	if cmd == "uninstall":
		return await cmd_remove(args)
	if cmd == "upgrade":
		return await cmd_upgrade(args)
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
