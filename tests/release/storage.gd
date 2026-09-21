# Verify isolated file I/O and embedded database transactions.
extends RefCounted


# Commit one bound statement through the transaction-specific client.
func commit_row(tx: GDDatabaseTx) -> R:
	return await tx.query("INSERT INTO item VALUES($1,$2)", [2, "committed"])


# Return a failure after writing so the transaction must roll back.
func rollback_row(tx: GDDatabaseTx) -> R:
	await tx.query("INSERT INTO item VALUES($1,$2)", [3, "discarded"])?
	return R.err("rollback", Err.INVALID_DATA)


# Keep all file writes inside the runner's disposable project and the database in memory.
func main() -> int:
	var ck := GD.test.check()
	var path := "user://sample.txt"
	ck.fails(GD.file.write_text("res://denied.txt", "blocked"), Err.PERMISSION_DENIED, "strict source mount is read-only")
	ck.succeeds(GD.file.write_text(path, "日本😀"), "write file")
	ck.eq(GD.file.read_text(path).v, "日本😀", "read file")
	ck.succeeds(GD.file.remove(path), "remove file")
	ck.fails(GD.file.read_text(path), Err.NOT_FOUND, "missing file error")
	var db := GD.database.client()
	var opened: R = await db.open({"driver": "sqlite", "path": ":memory:"})
	ck.succeeds(opened, "open SQLite")
	if opened.ok:
		ck.succeeds(await db.query("CREATE TABLE item(id INTEGER PRIMARY KEY, name TEXT)"), "create table")
		ck.succeeds(await db.query("INSERT INTO item VALUES($1,$2)", [1, "日本"]), "bind parameters")
		ck.succeeds(await db.transaction(commit_row), "commit transaction")
		ck.fails(await db.transaction(rollback_row), Err.INVALID_DATA, "rollback transaction")
		var selected: R = await db.query("SELECT id,name FROM item ORDER BY id")
		ck.succeeds(selected, "select rows")
		if selected.ok:
			ck.eq(selected.v.rows, [{"id": 1, "name": "日本"}, {"id": 2, "name": "committed"}], "rollback preserved committed rows")
		db.close()
		ck.no(db.is_open(), "close database")
	print("release:storage:%d" % ck.code())
	return ck.code()
