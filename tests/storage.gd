# Verify isolated file I/O and embedded database transactions.
extends RefCounted


# Commit one bound statement through the transaction-specific client.
func commit_row(tx: GDDatabaseTx) -> Dictionary, Err:
	return await tx.query("INSERT INTO item VALUES($1,$2)", [2, "committed"])


# Return a failure after writing so the transaction must roll back.
func rollback_row(tx: GDDatabaseTx) -> Dictionary, Err:
	await tx.query("INSERT INTO item VALUES($1,$2)", [3, "discarded"])?
	return {}, Err("rollback", Err.INVALID_DATA)


# Keep all file writes inside the runner's disposable project and the database in memory.
func main() -> int:
	var ck := GD.test.check()
	var path := "user://sample.txt"
	var _denied, denied_error := GD.file.write_text("res://denied.txt", "blocked")
	ck.fails(denied_error, Err.PERMISSION_DENIED, "strict source mount is read-only")
	var _written, write_error := GD.file.write_text(path, "日本😀")
	ck.succeeds(write_error, "write file")
	ck.eq(GD.file.read_text(path)!, "日本😀", "read file")
	var _removed, remove_error := GD.file.remove(path)
	ck.succeeds(remove_error, "remove file")
	var _missing, missing_error := GD.file.read_text(path)
	ck.fails(missing_error, Err.NOT_FOUND, "missing file error")
	var db := GD.database.client()
	var _opened, open_error := await db.open({"driver": "sqlite", "path": ":memory:"})
	ck.succeeds(open_error, "open SQLite")
	if open_error == null:
		var _made, make_error := await db.query("CREATE TABLE item(id INTEGER PRIMARY KEY, name TEXT)")
		ck.succeeds(make_error, "create table")
		var _inserted, insert_error := await db.query("INSERT INTO item VALUES($1,$2)", [1, "日本"])
		ck.succeeds(insert_error, "bind parameters")
		var _committed, commit_error := await db.transaction(commit_row)
		ck.succeeds(commit_error, "commit transaction")
		var _rolled, rollback_error := await db.transaction(rollback_row)
		ck.fails(rollback_error, Err.INVALID_DATA, "rollback transaction")
		var selected, select_error := await db.query("SELECT id,name FROM item ORDER BY id")
		ck.succeeds(select_error, "select rows")
		if select_error == null:
			ck.eq(selected.rows, [{"id": 1, "name": "日本"}, {"id": 2, "name": "committed"}], "rollback preserved committed rows")
		db.close()
		ck.no(db.is_open(), "close database")
	print("release:storage:%d" % ck.code())
	return ck.code()
