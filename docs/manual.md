# gd 公式マニュアル

[English](manual.en.md) | 日本語

## gdは何のための道具か

gdは、GDScriptでCLIツール、Webサイト、Web API、定期処理、データ処理を書くための単体コマンドです。
画面表示を使わない小さな実行環境をGodotから作っています。`project.godot`を用意せず、1つの`.gd`ファイルを書くだけで実行できます。

PythonやNode.jsでスクリプトを書く感覚で始められます。必要に応じて型検査やテストを加え、パッケージ、データベース、Webサーバーも使えます。完成したスクリプトは、1つの実行ファイルにまとめて配布できます。ゲームの画面や描画を作る用途にはGodot本家を使ってください。

Godot本家とあわせれば、アプリもフロントもサーバーもCLIツールも、一つの言語GDScriptで書けます。
同じスクリプトがmacOS、Linux、Windowsで動き、通信とデータベースの待ちはほかの処理を止めません。
GDExtensionでC++と直接つながります。

速度も重視しています。HTTP/2に対応し、通信を待つ間もほかの処理が進みます。文字列とJSONの処理も高速です。

## 導入

配布バイナリはmacOS arm64/x86_64、Linux x86_64、Windows x86_64向けです。Linux arm64はソースからビルドできます。
[Releases](https://github.com/prog-sha/gd/releases/latest)からOSに合うアーカイブを取得し、`gd`をPATHの通ったディレクトリへ置く方法も使えます。

### macOS / Linux

```sh
curl -fsSL https://gd.progsha.com/install.sh | sh
```

`~/.local/bin`へ導入します。PATHに未設定の場合だけ`y/N`で追加を確認します。`y`を選ぶと利用中のシェルに合う設定へ保存し、新しいターミナルから使えます。Linux配布バイナリにはx86_64とglibc 2.38以降が必要です。

### Windows（PowerShell）

```powershell
Invoke-WebRequest -UseBasicParsing https://gd.progsha.com/install.ps1 -OutFile install-gd.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-gd.ps1
```

導入先がPATHに未設定の場合だけ`y/N`で確認し、`y`を選ぶとユーザーのPATHへ追加します。

### Homebrew（macOS）

```sh
brew tap prog-sha/gd https://github.com/prog-sha/gd
brew install prog-sha/gd/gd
```

### apt（Linux amd64）

Ubuntu 24.04、Debian 13など、glibc 2.38以降のapt対応環境で利用できます。

```sh
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://gd.progsha.com/apt/gd.asc | sudo tee /etc/apt/keyrings/gd.asc >/dev/null
sudo chmod 0644 /etc/apt/keyrings/gd.asc
echo 'deb [arch=amd64 signed-by=/etc/apt/keyrings/gd.asc] https://gd.progsha.com/apt stable main' | sudo tee /etc/apt/sources.list.d/gd.list
sudo apt update
sudo apt install gd
```

### 更新

```sh
gd upgrade            # Move to the latest release.
gd upgrade 0.8.0      # Move to a chosen version.
gd upgrade --dry-run  # Show the version without installing it.
```

配布元のSHA-256と照合してから、実行中の`gd`を置き換えます。Homebrewやaptで入れた`gd`は、それぞれの`brew upgrade gd`、`apt upgrade gd`で更新してください。

### ソースからビルド

Python、uv、SCons、C/C++コンパイラーを用意します。実行ファイルは`bin/`に生成されます。

```sh
git clone --branch master https://github.com/prog-sha/gd.git
cd gd
scons platform=macos target=template_release -j8
# Linux ARM64: platform=linuxbsd arch=arm64
# Linux x86-64: platform=linuxbsd arch=x86_64
# Windows: platform=windows arch=x86_64 use_mingw=yes windows_subsystem=console
```

## クイックスタート

`hello.gd`を1つ作ります。設定ファイルやパッケージは要りません。`main()`が入口で、返した整数がプロセスの終了コードになります。

```gdscript
func main():
	print("Hello, world")
	return 0
```

```sh
gd hello.gd
```

実行せずに型と構文を調べるには`check`を使います。

```sh
gd check hello.gd
```

## スクリプトの読み込みと共通定数

ローカルファイルは`@import "./settings"`、導入済みパッケージは`@import hello`。読み込んだ名前をそのまま使います。`.gd`や`as settings`は省略できます。

共有する値を`settings.gd`に置きます。

```gdscript
const TITLE = "Hello"
const USER = "world"
```

`main.gd`から使います。

```gdscript
@import "./settings"

func main():
	print(settings.TITLE, ", ", settings.USER, "!")
	return 0
```

これで`Hello, world!`と表示します。共有が必要になったときの書き方です。

- 1つのスクリプトでしか使わない値や処理は、そのスクリプトに置けば十分です。
- `as`は、名前がぶつかるときだけ使います。
- 組み込みの`GD.web`などは、importなしで使えます。

パッケージの導入とバージョン固定は[パッケージと配布](#パッケージと配布)を参照してください。

## 用途から選ぶ

標準APIは`GD`にまとまっています。ファイル操作は`GD.file`、Webサービスは`GD.web`のように、用途に合わせて選びます。

| やりたいこと | 入口 | 例 |
|---|---|---|
| ファイル、文字、日時、HTTP クライアント、非同期処理 | `GD` | `GD.file.read_text("a.txt")` |
| WebサイトとWeb API | `GD.web` | `GD.web.app()` |
| SQLiteまたはPostgreSQL | `GD.database` | `GD.database.client()` |
| Redis | `GD.database.redis` | `GD.database.redis.client()` |
| TCP、UDP、TLS | `GD.net` | `GD.net.listen_tcp("127.0.0.1", 8080)` |
| メールの解析とSMTP送信 | `GD.mail` | `GD.mail.parse_address("me@example.com")` |

## APIの調べ方

コマンドの使い方は`gd --help`、コマンドごとの引数とオプションは`gd install --help`のように調べます。

APIは`gd doc GD.file.read_text`のように、調べたい関数や型の名前を渡します。関数名・引数・戻り値の情報は、実行プログラムから取得しています。

```sh
gd doc                     # Show a short guide and entry points.
gd doc manual              # Read the complete manual.
gd doc GD                  # List the standard modules.
gd doc GD.file             # file API
gd doc GD.http.fetch       # Inspect the returned HTTP response.
gd doc GD.web.app          # Web application
gd doc SceneTree           # Inspect a public engine class.
gd doc all                 # List public types.
```

戻り値に使われる型の`Err`、`GDWebRequest`は名前だけで引きます。Node、SceneTree、TimerなどGodot由来の型は
[Godotのクラスリファレンス](https://docs.godotengine.org/en/stable/classes/)も参照してください。
手引きの言語は`LC_ALL`または`LANG`が`ja`で始まるとき日本語、それ以外は英語です。
Web版は[gd.progsha.com](https://gd.progsha.com/)にあり、日本語と英語を切り替えられます。

## GDScriptの基本

掲載例では型名を繰り返しません。`:=`で代入する変数は右辺から型を推論します。
注釈を省いた引数は動的型です。受け取る値や返す値の型を限定したい箇所には、型名を書けます。

### 引数とオプション

スクリプト名の後ろに置いた引数は`main(argv)`で受け取ります。`--name=gd`のようにgd自身のオプションと紛らわしい引数は、
`--`の後ろへ置くとスクリプトへ渡ります。

```gdscript
func main(argv):
	for arg in argv:
		print(arg)
	return 0
```

```sh
gd main.gd apple orange
gd main.gd -- --name=gd
```

オプションとして解釈したいときは`GD.cli.flags()`を使います。`--name gd`、`--name=gd`、`-name=gd`のどの綴りも受けます。

```gdscript
func main(argv):
	var flags := GD.cli.flags()
	flags.flag_str("name", "world", "挨拶する相手")
	var _parsed, parse_err := flags.parse(argv)
	if parse_err:
		print(flags.usage())
		return 1
	print("Hello, " + flags.get_str("name"))
	return 0
```

### 外のコマンドを呼ぶ

外の道具は`GD.cli.run()`で呼びます。待つのは呼び出したGDScriptだけなので、`gd serve`のハンドラーの中から呼んでも他のリクエストは進みます。
`--strict`では`--allow-run`が要ります。`--allow-run=/usr/bin/git`のように相手を絞れます。

```gdscript
func main():
	var got, err := await GD.cli.run("git", ["rev-parse", "HEAD"])
	if err:
		return 1
	print("code=", got["code"], " out=", got["output"])
	return 0
```

子プロセスの終了コードが0以外なら`Err`を返します。終了コードは`value.code`、出力は`value.output`で読めます。時間切れやキャンセルのときも、そこまでの出力は残ります。

第3引数の`opts`で挙動を変えられます。

| 名前 | 既定 | 意味 |
|---|---|---|
| `timeout` | `0` | 完了を待つ秒数。0は無期限。時間を超えると子プロセスを終了し、`Err.TIMED_OUT`を返す |
| `output` | `true` | 出力を集める。`false`なら親の標準入出力へ直結し、集めない |

## 処理結果とエラー

失敗する可能性がある関数は、値と`Err`の2つを返します。
`var 値, e :=`で両方を受け取れます。型は関数の宣言から推論され、`e`が`null`でなければ失敗です。
既存の変数には`値, e = call()`で代入できます。先に型なしで宣言した変数は`Variant`のままなので、静的に型を検査するなら`var 値: T`と`var e: Err`を宣言します。

```gdscript
func main():
	var text, e := GD.file.read_text("note.txt")
	if e:
		print(e.text())
		return 1
	print(text)
	return 0
```

### エラー処理を短く書く

関数呼び出しの末尾に`?`を付けると、失敗時は値と`Err`を呼び出し元へ返します。成功時は値だけを使えます。
`?`を使う関数は値と`Err`を返します。返却型を省くと`return`の値から第一結果を推論し、必要なら`-> String, Err`のように明示できます。正常時の`return 値`は、第二結果の`null`を省略した形です。

```gdscript
func title(path) -> String, Err:
	var text := GD.file.read_text(path)?
	return text.strip_edges()

func main():
	var text, e := title("note.txt")
	if e:
		print(e.note("題名を読む").text())
		return 1
	print(text)
	return 0
```

### 自分でエラーを返す

ファイルを読めても、内容が空なら失敗にしたい場合は、自分で`Err`を作って返します。`Err("説明", 種類, 追加情報)`の形で、説明文、`Err.INVALID_DATA`などの種類、必要な情報を指定します。次を`read_message.gd`に保存してください。

```gdscript
# Read a message and reject empty contents.
func read_message(path: String) -> String, Err:
    var text := GD.file.read_text(path)?
    if text.strip_edges().is_empty():
        return "", Err("Message is empty", Err.INVALID_DATA, {"path": path})
    return text, null

# Print the message or report the failure to the caller.
func main() -> int:
    var text, err := read_message("message.txt")
    if err != null:
        printerr(err)
        return 1
    print(text)
    return 0
```

```sh
printf 'Hello!\n' > message.txt
gd read_message.gd
printf '' > message.txt
gd read_message.gd
```

最初の実行は`Hello!`を表示し、空ファイルにした後の実行は`Message is empty`というエラーを表示して終了コード1を返します。

- `-> String, Err`は、文字列とエラーの2つを返すという宣言です。
- 成功したら`return text, null`、失敗したら`return "", Err(...)`を返します。
- 呼び出し側は`var text, err := read_message(...)`で受け取り、`err`を確かめてから`text`を使います。
- ファイルを読めなかったときは、`?`がそのエラーをそのまま返します。

### 待つ処理と同時実行

HTTP、データベース、`GD.net`、ファイルなどの待つメソッドは、普通の関数呼び出しとして書けます。
待つのは呼び出したGDScriptだけで、ほかの通信やタイマーは進みます。

```gdscript
func main():
	var res := GD.http.fetch("https://example.com/")!
	print(res.status)
	return 0
```

複数の処理を同時に始めたいときは、末尾が`_async`の版と`GD.async.all()`を使います。

```gdscript
func main():
	var got = await GD.async.all([
		GD.http.fetch_async.bind("https://example.com/a"),
		GD.http.fetch_async.bind("https://example.com/b"),
	])
	for result: Array in got:
		var res: GDHTTPResponse = result[0]
		var e: Err = result[1]
		if e:
			print(e.text())
			return 1
		print(res.status)
	return 0
```

| 入口 | 用途 |
|---|---|
| `名前_async()` | 処理を始め、結果を保持する`GDTask`をすぐ返す。ほかの処理を待った後でも、`await`すると通常名と同じ結果を受け取れる。`cancel()`で取り消せる |
| `GD.async.all(list)` | 複数の処理を同時に進め、結果を渡した順に返す |
| `GD.async.spawn(fn)` | 関数を裏で動かす。`main()`が返った後も動く |
| `GD.async.sleep(sec)` | 指定秒だけ待つ |

1つだけ待つときは`var value, e := await GD.http.fetch_async(url)`のように、通常名と同じ形で受け取れます。

先に始めた処理を、あとから順に受け取ることもできます。`first`を待つ間に`second`が終わっても、結果は`second`が保持しています。

```gdscript
func main():
	var first := GD.http.fetch_async("https://example.com/a")
	var second := GD.http.fetch_async("https://example.com/b")
	var a, a_err := await first
	var b, b_err := await second
	if a_err or b_err:
		return 1
	print(a.status, " ", b.status)
	return 0
```

気を付ける点は3つです。

- `all()`には、`_async()`が返したGDTaskか、上の例のように`.bind()`で作ったCallableを渡します。どちらも、先に終わった処理の結果を取りこぼしません。
- `spawn()`は処理を別スレッドへ移しません。大きなデータを標準モジュールで処理するときは`_async`の版を使います。
- 待つメソッドは`main()`や自分で書いた関数の中で呼びます。`Array.map()`へ渡す関数、`_init()`、メンバー変数の初期化の中では待てないので、`for`か`GD.async.all()`で書きます。

## チュートリアル: SQLiteを使うメモAPI

ここまでの知識で、JSONを受けてSQLiteへ保存する小さなAPIを1つのスクリプトで作ります。
できあがるのは、入力を検査し、SQL文と値を分けて渡す、権限を絞って起動する開発用サーバーです。

### 1. 作業ディレクトリを作る

```sh
mkdir notes-api
cd notes-api
```

### 2. APIを書く

次を`main.gd`として保存します。

```gdscript
# Store notes in an embedded database and expose a JSON API.
extends RefCounted


const PORT := 18080 # Development listener port on loopback.
const DB_PATH := "user://notes.sqlite3" # Writable storage isolated per user.

var app := GD.web.app()
var db := GD.database.client()


# Return notes as JSON in newest-first order.
func list_notes(req: GDWebRequest) -> GDWebResponse, Err:
	var got, query_err := await GD.async.with_context_pair(req.context, db.query_async("SELECT id, title FROM notes ORDER BY id DESC"))
	if query_err:
		return query_err
	return GD.web.json(got.rows)


# Save a validated title and return the created row.
func add_note(req: GDWebRequest) -> GDWebResponse, Err:
	var body := req.valid("body")
	var made, query_err := await GD.async.with_context_pair(req.context, db.query_async(
		"INSERT INTO notes(title) VALUES($1) RETURNING id, title",
		[body.title]
	))
	if query_err:
		return query_err
	return GD.web.json(made.rows[0], 201)


# Prepare the database and routes, then listen on loopback.
func main() -> int, Err:
	db.open({"driver": "sqlite", "path": DB_PATH})?
	db.query("CREATE TABLE IF NOT EXISTS notes(id INTEGER PRIMARY KEY, title TEXT NOT NULL)")?
	app.route("GET", "/notes", list_notes)
	app.route("POST", "/notes", add_note, [GD.web.json_body(GD.web.object_rule({
		"title": GD.web.text_rule({"min": 1, "max": 120}),
	}))])
	app.listen(PORT, "127.0.0.1")?
	print("listening on http://127.0.0.1:%d" % PORT)
	return 0
```

上から順に読みます。

- `app`はルーター、`db`はデータベース接続です。`main()`が返った後もサーバーが動き続けられるよう、両方ともスクリプトの変数として持ちます。
- `main()`はまずSQLiteを開き、表を作ります。`DB_PATH`の`user://`は、gdが利用者ごとに用意する書き込み領域です。
- `app.route()`に、HTTP メソッド、パス、そのときに呼ぶ関数（ハンドラー）を登録します。
- ハンドラーは`GDWebRequest`を受け取り、`GD.web.json()`で返事を作ります。途中の`?`は失敗をサーバーへ返し、ステータスコード500などになります。
- POSTには`GD.web.json_body()`を付けています。本文が検証ルールに合うときだけハンドラーが呼ばれ、通った値が`req.valid("body")`に入ります。
- SQLの`$1`に入れる値は、別の引数で渡します。文字列連結でSQLを組み立てません。

### 3. 権限を絞って起動する

未確認のスクリプトや外へ公開するサーバーは、権限を既定で拒否する`--strict`で実行します。ここでは待ち受け先をループバックの一つのポートに絞ります。
`serve`は`main()`が返った後もプロセスを残すコマンドで、サーバーにはこれを使います。

```sh
gd check main.gd
gd --strict --allow-net=127.0.0.1:18080 serve main.gd
```

### 4. 別の端末から使う

```sh
curl -s -X POST http://127.0.0.1:18080/notes \
  -H 'Content-Type: application/json' \
  -d '{"title":"gdを試す"}'
curl -s http://127.0.0.1:18080/notes
```

最初はステータス `201`と作成した一件、次は保存済みの配列が返ります。空の題名、120文字を超える題名、
JSONでない本文は`400`で拒否されます。止めるときは起動したターミナルでCtrl-Cを押します。

公開環境ではこのプロセスをループバックのままTLS リバースプロキシの後ろへ置き、異常終了耐性が必要な保存先は
PostgreSQLへ切り替えます。接続情報はソースへ書かず、許可した環境変数から読みます。

## 権限

最初は通常実行を使います。`--strict`は、必要な権限を自分で設計・設定する上級者向けの実行方式です。

| 方式 | 向く場面 | 制限 |
|---|---|---|
| 通常実行 | 信頼できるソースを開発中に動かす | ファイルもネットワークも制限しない |
| `--strict` | 未確認のスクリプト、公開サーバー | 起動ディレクトリ配下は読み取り専用で、外部の絶対パスは拒否。ネットワーク、環境変数、子プロセス、ネイティブ拡張機能、システム情報を既定で拒否 |

`--strict`では、使うものを挙げて起動します。

```sh
gd --strict \
  --mount store=/srv/app:rw \
  --allow-net=db.example.com:5432 \
  --allow-env=DATABASE_URL \
  main.gd
```

| 指定 | 許すもの |
|---|---|
| `--mount name=path:r` / `--mount name=path:rw` | 名前を付けたディレクトリの読み取り、または読み書き |
| `--allow-net=host:port,...` | 接続と待ち受け。値を省くと全て |
| `--allow-env=name,...` | 環境変数 |
| `--allow-run=command,...` | 子プロセス |
| `--allow-ext=path,...` | スクリプトが実行中に読むネイティブ拡張機能 |
| `--allow-sys=item,...` | 機種とシステム情報 |
| `--deny-*` | 対応するallowより優先する拒否 |
| `-A` | ファイル以外を全て許す。開発中の一時的な利用向け |

`--strict`が効くのは、スクリプトを実行する`run`、`serve`、`test`、`task`、`eval`、`repl`です。`check`や`fmt`、パッケージ操作のようにスクリプトを実行しないコマンドには影響しません。

### ファイルの置き場

スクリプトから見えるファイルの置き場は次の4種類です。置き場の名前をパスの先頭に書くか、絶対パスをそのまま書きます。

| 書き方 | 指す場所 | strictでの扱い |
|---|---|---|
| `res://a.txt` | スクリプトを起動したディレクトリ | 読み取り専用 |
| `user://a.txt` | 実行したスクリプトごとに、利用者のデータ領域に用意する書き込み領域。再起動しても残る | 読み書き可能 |
| `store://a.txt` | `--mount store=/srv/app:rw`で付けた名前 | 指定した権限 |
| `/etc/hosts` | このコンピューター上の指定した場所 | 起動ディレクトリの外なら拒否 |

迷ったら、読むだけのファイルは`res://`、保存するファイルは`user://`へ置きます。

- strictで起動ディレクトリの外を使うときは`--mount`で名前を付けます。`-A`を付けてもファイルの範囲は広がりません。
- マウント名は`store`や`uploads`のように、小文字の英数字と`-`で付けます。gdが使っている次の名前は選べません: `res`、`user`、`uid`、`pipe`、`local`、`libgodot`、`tcp`、`unix`、`http`、`https`、`file`、`data`、`cache`、`pkg`、`global`。
- Windowsでは`--mount`と絶対パスを使えません。`res://`か`user://`へ置いてください。

### ネットワークと拡張機能の許可

- `--allow-net`の`localhost:8080`は、同じポートのIPv4 ループバック `127.0.0.0/8`とIPv6 `::1`も表します。
- `*.example.com:443`はその下位ホストを許します。
- ネイティブ拡張機能は同じプロセスで動くため、信頼できるものに限ってください。

### serveとSceneTree

`gd serve`はSceneTreeを作らない常駐用の実行方式です。通信、タイマー、`await`、自作Signal、`GD.async.sleep()`、
ツリー外Nodeの`queue_free()`は動きます。仕事が無ければ次の期限か通信の通知まで眠るため、周期の調整は要りません。

| やりたいこと | 方法 |
|---|---|
| Webサーバーや定期処理を常駐させる | `gd serve main.gd` |
| Nodeの`_process()`、`_physics_process()`、`process_frame`、SceneTreeTimer、高水準マルチプレイを使う | `serve`を付けない通常実行 |
| SceneTreeやMainLoopを継承したスクリプトを動かす | `serve`を付けない通常実行 |
| SceneTreeが紛れ込んでいないか開発中に調べる | `gd --no-scene-tree --allow-net serve app.gd` |
| 複数プロセスで待ち受ける | `--workers=<n>`または`--workers=auto`。`n`は1以上の整数 |

`serve`では`Node`を継承しただけのスクリプトはツリーへ追加されません。
`--no-scene-tree`はSceneTreeが作られた時点で診断を出し、終了コード 1で止まります。`--watch`と`--workers`の子にも引き継がれます。

## TCPとUDP

低水準の通信には`GD.net`を使います。Godot本家の低水準の型も使えますが、新しいコードは`GD.net`で書きます。

```gdscript
func echo() -> int, Err:
	var listener := GD.net.listen_tcp("127.0.0.1", 8080)?
	var conn := listener.accept()?
	var data := conn.read(65536)?
	conn.write(data)?
	conn.close()
	return 0
```

- `GDTCPConn`は読み取りと書き込みを別々の順番待ちで処理するため、複数のGDScriptから同時に呼べます。
- 期限メソッドは今からの秒数を設定し、0で解除します。
- `close()`を呼ぶと、待っている読み書きは`Err.INTERRUPTED`で終わります。リスナーの受付待ちも同じです。
- 接続は名前解決で得たIPv4とIPv6の候補を順に試し、成功した一本だけを残します。全体の`timeout`は延びません。

### TLS

TLSは`GD.net.dial_tls(host, port, opts)`で開きます。既定で証明書チェーンと接続先のホスト名を検証し、失敗しても平文へ戻りません。
戻り値はTCPと同じ`GDTCPConn`です。

| `opts` | 意味 |
|---|---|
| `timeout` | 接続とTLSの初期通信が完了するまでの制限時間（秒） |
| `ca_file` | 私設CA。環境変数の設定より優先する |
| `cert_file`、`key_file` | クライアント認証。許可されたマウント内のファイルを対で指定する |
| `server_name` | 証明書を照合する宛名を接続先と別にする |
| `next_protos` | ALPN名の配列。1名は1–255 バイト、全体で65535バイトまで |
| `insecure_skip_verify` | 検証を省く。検証不要と判断できる試験時だけ使う |

交渉結果は`connection_state()`の`negotiated_protocol`と`version`で読めます。TLS1.2は771、TLS1.3は772です。

信頼するCAは、未指定ならmacOSとWindowsではOSの信頼設定、Linuxではシステム CA証明書の一覧です。
起動前に`SSL_CERT_FILE`または`SSL_CERT_DIR`を設定すると、どのOSでも指定したCAを使います。
ディレクトリの区切りはUnixで`:`、Windowsで`;`です。

サーバーがクライアント証明書を求めるときは、`app.listen_tls(port, cert, key, host, opts)`の`opts`へ`client_ca`（信頼するCA証明書の一覧）と`client_auth`を渡します。
`client_ca`が未指定ならシステムの信頼設定を使います。

| `client_auth` | 動作 |
|---|---|
| `none` | 証明書を要求しない |
| `request` | 任意提示。検証しない |
| `require` | 提示だけ必須。検証しない |
| `verify_if_given` | 提示された場合だけ検証する |
| `require_and_verify` | 検証済みの証明書を必須にする |

### UDPと名前解決

UDPは`GD.net.listen_udp()`で開きます。届いたパケットには送り主の`host`と`port`が入っているので、そのまま返信できます。

```gdscript
func echo_udp() -> int, Err:
	var conn := GD.net.listen_udp("127.0.0.1", 9000)?
	var packet := conn.read_from()?
	conn.write_to(packet.data, packet.host, packet.port)?
	conn.close()
	return 0
```

- `write_to()`の宛先はIPアドレスで渡します。ホスト名は先に`GD.net.resolve(name)?`でIPアドレスにします。
- 1回の`read_from()`が1つのパケットです。受け取る大きさを超えた分は捨てられ、`packet.truncated`が`true`になります。
- このコンピューターのアドレスは`GD.net.local_addresses()`で調べます。

## メール

`GD.mail`はメールアドレスやヘッダーを解析し、SMTPサーバーへメールを送ります。

- `send_mail`には、ヘッダーと本文を含むメール全体をバイト列で渡します。
- 成功は「SMTPサーバーが受け取った」という意味で、相手に届いたことまでは保証しません。
- `--strict`では、接続先に`--allow-net`が要ります。

```gdscript
func main() -> int, Err:
	var message := "From: me@example.com\r\nTo: you@example.com\r\nSubject: Hello\r\n\r\nHello\r\n".to_utf8_buffer()
	GD.mail.send_mail("smtp.example.com", 587, "me@example.com", PackedStringArray(["you@example.com"]), message, {
		"tls": "starttls", "timeout": 10.0,
		"auth": GD.mail.plain_auth("me@example.com", "secret"),
	})?
	return 0
```

## ファイルとデータ

`GD.file`でファイルの読み書きとパス操作をします。起動したディレクトリが`res://`で、絶対パスも書けます。
strictで外部ディレクトリへ書くときは`--mount store=/srv/app:rw`で付けた名前を`store://users.csv`のように書きます。

```gdscript
func main() -> int, Err:
	var rows := GD.data.csv_objects(GD.file.read_text("store://users.csv")?)?
	GD.file.write_text("store://users.json", JSON.stringify(rows))?
	return 0
```

ファイルの操作は、通常名でも呼び出したGDScriptだけを待たせます。Webサーバーのハンドラーから読んでも他のリクエストは進みます。
複数の操作を同時に始めるときだけ、末尾が`_async`の版を使います。

```gdscript
func handler(_req) -> GDWebResponse:
	var body, read_err := GD.file.read_text("store://big.json")
	if read_err:
		return GD.web.text("読めません", 500)
	return GD.web.text(body)
```

`compile`で同梱したファイルも、同じAPIで読み取り、列挙、static配信ができます。

### 大きなファイルを読む

全体をメモリへ置かず読むときは`GD.file.open(path, mode)`で`GDFileStream`を開きます。
modeは`read`、`write`、`append`、`read_write`で、使い終えたら`close()`を呼びます。

| メソッド | 動作 |
|---|---|
| `read(max)` | 最大`max` バイトを返す。少なく返ることがある。maxが正のとき、エラーがなく空ならファイルの末尾まで読み取り済み |
| `write(bytes)` | 全て書いてバイト数を返す。途中で失敗しても第一結果に書き込み済みバイト数が残る |

同じストリームの操作は受付順、別のストリームは並列に進みます。appendはseekの後も常に末尾へ書きます。
`read_bytes()`も途中で失敗したときは取得済みのバイト列を第一結果に残します。
`read_text()`はStringに収まらない大きさの入力を切り詰めずエラーにするので、大きなファイルはバイト列かストリームで扱います。

### 同時に更新されるファイル

複数のプロセスが同じファイルを更新するときは`GD.file.replace_text(path, old, body)`を使います。
読み取った`old`と現在の内容が同じときだけ置き換えるため、並行編集を黙って上書きしません。新規作成では`old`に`null`を渡します。

### データ形式の入口

| 用途 | 入口 |
|---|---|
| CSV、TOML、YAML、JSONL、JSONC、XML、INI、TAR、front matter、`.env`のファイルを読む | `GD.file.read_csv(path)`など |
| 同じ形式のメモリ上の変換、JSON、エンコード・デコード、ハッシュ、HMAC、PBKDF2、HKDF、バイト列 | `GD.data` |
| UUIDとULID | `GD.id` |
| 日時の変換と計算 | `GD.time` |
| 文字の整形と比較 | `GD.text` |
| HTML エンティティ、タグ、gdhtml（Mustache構文のマイクロテンプレート） | `GD.html` |
| オプションと環境変数 | `GD.cli` |
| 配列と辞書の操作 | `GD.collection` |
| 数学の特殊値とビット演算 | `GD.math` |
| バージョンの比較 | `GD.version` |
| ターミナルとファイルへのログ | `GD.log` |
| テストの検査 | `GD.test` |

環境変数と`.env`は、読む対象で入口が分かれます。

| 読む対象 | 入口 |
|---|---|
| プロセスの環境変数 | `GD.cli.env(name, fallback)`、`GD.cli.require_env(name)`。strictでは`--allow-env`が要る |
| `.env` ファイル | `GD.file.read_env(path)`。ファイルを読んで辞書にする |
| dotenv形式の文字列 | `GD.data.env(src)`と`GD.data.to_env(data)`。メモリ上で辞書と変換する |

メモリ上の変換は通常名がその場で計算し、`_async`の版は別のスレッドで計算します。大きな入力には`_async`を使います。
正確な一覧は`gd doc GD.file`と`gd doc GD.data`で引けます。形式ごとの検査と上限は、APIリファレンスの各入口の説明にあります。

`GD.collection`のCallableを使う操作は、約1 msごとにほかの処理へ実行権を譲ります。
`GD.log`の各呼び出しは書き込み完了まで待ち、本文を切り捨てません。失敗は第二結果の`Err`で確認でき、`GD.log.flush()`でそれ以前の出力完了を待てます。

### JSONの規則

値は`GD.data.json_encode(value)`でJSONのバイト列にし、受け取ったバイト列は`GD.data.json_decode(bytes)`で読みます。
`GDWebRequest.json()`、`GDHTTPResponse.json()`、JSONLの各行も同じ規則です。

```gdscript
func main() -> int, Err:
	var bytes := GD.data.json_encode({"id": 1, "tags": ["a", "b"]})?
	var value := GD.data.json_decode(bytes)?
	print(value.id)
	return 0
```

- 整数は`int`のまま戻ります。小数と指数表記は`float`になります。
- 壊れた文字コードや同じ名前の重複など、あいまいなJSONは読まずに失敗にします。
- 署名やキャッシュのキーのように、同じ値から毎回同じバイト列が要るときは`{"deterministic": true}`を渡します。

### ハッシュと鍵導出

`GD.data`はSHA-1、SHA-224/256/384/512、SHA3-224/256/384/512を返します。HMAC、PBKDF2、HKDFでは
`sha1`、`sha224`、`sha256`、`sha384`、`sha512`、`sha3-224`、`sha3-256`、`sha3-384`、`sha3-512`から方式を選べます。
PBKDF2とHKDFは出力長を指定でき、不正な方式や表現範囲外の設定は`Err`として返します。PBKDF2の繰り返す回数が1以下なら1回として計算します。

## Webフレームワーク

同じディレクトリに`main.gd`と`index.html`を作ります。一方のルートでHTMLテンプレートに挨拶を埋め込み、もう一方で同じデータをJSONとして返します。

`main.gd`:

```gdscript
# Serve a greeting as an HTML page and a JSON response.
var app := GD.web.app()

# Build the shared response data from the requested name.
func greeting(req: GDWebRequest) -> Dictionary, Err:
	var name := req.query.get("name", "world")
	if not name is String:
		return {}, Err("name must be text", Err.INVALID_DATA)
	return {"name": name, "message": "Hello, " + name + "!"}

# Render the page with escaped template values.
func home(req: GDWebRequest) -> GDWebResponse, Err:
	return GD.web.view("index.html", greeting(req)?)

# Return the same data as JSON.
func hello(req: GDWebRequest) -> GDWebResponse, Err:
	return GD.web.json(greeting(req)?)

# Listen locally, using an optional port argument.
func main(args):
	var port := 8080 if args.is_empty() else int(args[0])
	app.route("GET", "/", home)
	app.route("GET", "/api/hello", hello)
	app.listen(port, "127.0.0.1")!
	print("http://127.0.0.1:", app.port())
	return 0
```

`index.html`:

```html
<!doctype html>
<!-- Display a greeting and let visitors choose the name. -->
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{message}}</title>
  <style>
    body { max-width: 36rem; margin: 4rem auto; padding: 0 1rem; font-family: system-ui, sans-serif; line-height: 1.6; }
    input, button { padding: .6rem; font: inherit; max-width: 100%; box-sizing: border-box; }
  </style>
</head>
<body>
  <h1>{{message}}</h1>
  <form action="/" method="get">
    <label>Your name <input name="name" value="{{name}}"></label>
    <button type="submit">Say hello</button>
  </form>
  <p><a href="/api/hello?name={{name}}">View the JSON response</a></p>
</body>
</html>
```

そのディレクトリで実行します。

```sh
gd serve main.gd
```

`http://127.0.0.1:8080/?name=Alice`を開くと、`{{message}}`へ「Hello, Alice!」を埋め込みます。`/api/hello?name=Alice`は`{"name":"Alice","message":"Hello, Alice!"}`を返します。テンプレートの値はHTMLの文脈に応じてエスケープされます。

`serve`は`main()`が返ったあとも待ち受けを続けます。終了はCtrl+Cです。

### ルートと返事

`route(method, pattern, handler)`でHTTP メソッドとパスをハンドラーへ結びます。patternの`:name`は`req.params["name"]`に入ります。
HEADの要求にはGETのルートで応えます。パスは合うのにメソッドが違う要求には405を返します。
ハンドラーは`GDWebRequest`を受け取ります。本文は`req.read()`、`bytes()`、`text()`、`json()`、`save()`で必要な分だけ読みます。
HTMLのフォームから届く本文は`GD.http.decode_query(req.text()?)`で辞書にします。

ハンドラーが返した値が返事になります。`return GD.web.json(data)`のように、下の表の値をそのまま返します。

途中の操作が失敗したら、`call()?`でその`Err`を返します。`Err`は表の最後の行のステータスコードになります。返事を自分で決めたいときは`app.on_error(handler)`を登録します。

| 返した値 | 返事 |
|---|---|
| `GD.web.html(body)`、`GD.web.view(path, data)` | HTML |
| `GD.web.json(data)` | JSON |
| `GD.web.text(body)`、`GD.web.bytes(body, type)` | テキスト、任意のメディアタイプ |
| `GD.web.stream(producer)` | 本文を少しずつ送信する。「Webの運用と高度な機能」を参照 |
| `GD.web.redirect(to)` | 302。`to`は同じサイト内のパスに限り、他所へ送るときは`away`を`true`にする |
| `GD.web.not_found()` | 404 |
| 文字列 | テキスト/plainの200 |
| `body`を持つ辞書 | 手書きの返事。`status`、`type`、`headers`も指定できる |
| `body`を持たない辞書 | JSONの200 |
| `null` | 204 |
| 失敗の`Err` | 種類に応じたステータスコード。`INVALID_DATA` → 400、`UNAUTHENTICATED` → 401、`PERMISSION_DENIED` → 403、`NOT_FOUND` → 404、`LIMITED` → 429、`UNSUPPORTED` → 501、`TIMED_OUT` → 504、他は500（いずれも`Err`の定数） |

返事は次のように調整できます。

- ステータスコードは`GD.web.text("見つかりません", 404)`のように第2引数で変えます。`bytes`はメディアタイプの後ろに書きます。
- `GD.web.header(reply, name, value)`で返事にヘッダーを足します。
- `GD.web.guard(reply)`で`Content-Security-Policy`などの防御ヘッダーをまとめて足します。
- 失敗の理由は既定では本文に出しません。開発中は`app.show_errors(true)`で表示できます。
- `req.query`の値は文字列とは限りません。上の例のように`is String`で確かめてから使います。
- `req.path`は`%20`などを元の文字へ戻したパスです。届いたままの文字列は`req.target`で読めます。

ルーターには次も登録できます。

| 登録 | 用途 |
|---|---|
| `app.static("/assets", "res://public")` | 接頭辞以下のGETとHEADをディレクトリのファイルで返す。メディアタイプは拡張子から決め、ディレクトリの外は返さない。`/`のindexは`route`で書く |
| `app.group("/api", [middleware])` | 共通接頭辞とミドルウェアを持つルートグループ。戻り値に`route()`と`use()`がある |
| `app.fallback(handler)` | どのルートにも一致しない要求。404ページをここで返す |
| `app.on_error(handler)` | ハンドラーが失敗を返したときの返事 |
| `app.after(handler)` | 返事を送る前の加工。`func(req, reply)`で受け、ヘッダーを足して返す |

### ミドルウェア

ミドルウェアは、ハンドラーの前に呼ばれる関数です。`GDWebRequest`を受け取り、`null`を返すと次へ進み、返事を返すとそこで止まります。
`handle(req)`を持つオブジェクトも使えます。後段へ渡す値は`req.keep(name, value)`で置き、`req.kept(name)`で読みます。

| 登録 | 掛かる範囲 |
|---|---|
| `app.pre(mw)` | ルート選択の前。全要求 |
| `app.use(mw)` | ルート選択の後。全ルート。`req.params`を読める |
| `group.use(mw)` | そのグループのルート |
| `app.route(method, pattern, handler, [mw])` | そのルートだけ |

入力検査もミドルウェアです。`GD.web.json_body(rule)`、`GD.web.query(rule)`、`GD.web.params(rule)`が本文、クエリ、パスの値を検査し、
通った値を`req.valid("body")`、`req.valid("query")`、`req.valid("params")`に入れます。
検証ルールは`GD.web.text_rule()`、`int_rule()`、`number_rule()`、`bool_rule()`、`list_rule()`、`object_rule()`で組み、
`GD.web.optional()`と`GD.web.one_of()`で省略と選択肢を表します。クエリとparamsの値は文字列なので`text_rule()`で検査し、必要なら`to_int()`で変換します。

```gdscript
var app := GD.web.app()

func show(req) -> GDWebResponse, Err:
	var params := req.valid("params")
	return GD.web.json({"id": params.id})

func main():
	app.route("GET", "/posts/:id", show, [GD.web.params(GD.web.object_rule({"id": GD.web.text_rule({"min": 1, "max": 20})}))])
	app.listen(8080)!
	return 0
```

組み込みのミドルウェアは`GD.web.sessions()`、`GD.web.csrf()`、`GD.web.jwt()`、`GD.web.rate()`です。認証の節で使います。

### HTMLテンプレート

ページが増えてきたらHTMLをテンプレートファイルへ出し、`GD.web.view(path, data)`で描画します。
テンプレートはgdhtml（Mustache構文のマイクロテンプレート）で、`{{name}}`、`{{{html}}}`、`#if`、`#unless`、`#each`、
`#with`、`else`、`{{> header}}`を扱います。`views/page.html`から`{{> header}}`を使うと、
同じ階層の`views/partials/header.html`を読みます。

```html
<!-- views/page.html -->
{{> header}}
<main><h1>{{title}}</h1></main>
```

```html
<!-- views/partials/header.html -->
<header><a href="/">gd app</a></header>
```

```gdscript
func page(_req) -> GDWebResponse, Err:
	return GD.web.view("views/page.html", {"title": "Top"})
```

二重括弧の値は、置かれた位置から文脈を判定してエスケープします。テンプレートの作者を信頼し、差し込む値を信頼しない前提です。

| 文脈 | 扱い |
|---|---|
| HTML本文、引用・未引用属性、属性名 | HTML エスケープ |
| `href="{{url}}"` | 相対URLと`http`、`https`、`mailto`を通す。`data-href`も同じ |
| `href="/work/{{path}}"`、`href="/?q={{query}}"` | パスは区切りを保って正規化、クエリはpercent エスケープ |
| `onclick`、`script`本文 | JSON化し、`application/json`でも`</script>`が構造を壊さない形にする |
| `style` | 安全な単独CSS値とCSS文字列・URLを通す |
| 危険なURL、srcset、CSS値、属性名 | 画面全体を失敗させず、`#ZgdunsafeZ`または`ZgdunsafeZ`へ置き換える |

- 三重括弧`{{{html}}}`はエスケープしない唯一の入口で、HTML本文以外では使えません。固定HTMLか十分に検査済みの値だけを渡してください。
- 「検査済み」の印を付けて二重括弧のエスケープを省く方法はありません。
- 分岐の両側や`each`の反復が異なる文脈で終わるテンプレート、閉じていないタグ、曖昧なURLやJavaScript文脈は描画の失敗になります。
- テンプレートの大きさに固定上限はありません。再帰する部品の深さだけは100000までです。
- 描画が終わるまで、渡した辞書を変更しないでください。

同じテンプレートを何度も描画するサーバーでは、起動時に`GD.html.template(source, partials)?`で一度だけ解析し、
返った値の`execute(data)?`を各要求から呼びます。解析結果は不変で、複数の要求から同時に使えます。
`execute_bytes(data)?`はUTF-8のバイト列を直接作るので、`GD.web.bytes(body, "text/html; charset=utf-8")`でそのまま返せます。

### 認証とCSRF

ログインの状態は`GD.web.sessions()`で持ちます。`issue(value)`でセッション IDを作り、`cookie(id)`の値を`Set-Cookie`で返します。
同じstoreをミドルウェアとして付けたルートでは、CookieのIDに対応する値が`req.kept("user")`に入り、無ければ401になります。
ミドルウェアの設定が不正なら生成関数は`null`を返し、`app.use()`やルートに渡した場合は`app.listen()`が設定エラーを返します。

```gdscript
var app := GD.web.app()
var sessions := GD.web.sessions()

func login(req: GDWebRequest) -> GDWebResponse, Err:
	var form := GD.http.decode_query(req.text()?)?
	var user := str(form.get("user", [""])[0])
	if user.is_empty():
		return GD.web.text("user is required", 400)
	var reply := GD.web.redirect("/me")
	return GD.web.header(reply, "Set-Cookie", sessions.cookie(sessions.issue(user)?))

func me(req) -> GDWebResponse, Err:
	return GD.web.text("hello, " + str(req.kept("user")))

func main():
	app.route("POST", "/login", login)
	app.route("GET", "/me", me, [sessions])
	app.listen(8080)!
	return 0
```

`cookie(id)`は`Secure`と`HttpOnly`付きで作ります。TLSなしの開発中に届かない場合は`cookie(id, false)`にします。
ログアウトは`drop(id)`と`clear_cookie()`で行います。セッションはプロセス内で持つため、`--workers`で複数プロセスにするときはJWTか外部の保存先を使います。

Cookieで認証する書き込み経路には`GD.web.csrf()`を付けます。GET、HEAD、OPTIONS以外はブラウザーの
`Sec-Fetch-Site: same-origin`が必要です。古いブラウザーやブラウザー以外のクライアントも受ける場合に
`GD.web.csrf({"allow_missing": true})`を選び、別のトークン検証を組み合わせてください。

```gdscript
var app := GD.web.app()
var sessions := GD.web.sessions()

func save_email(_r):
	return "saved"

func main():
	app.route("POST", "/account/email", save_email, [GD.web.csrf(), sessions])
	app.listen(8080)!
	return 0
```

JWTをログインセッションに使う場合は、パスワード変更やログアウトで既発行トークンを失効させます。
`check`は署名と標準クレームの検証後に呼ばれ、`true`を返したときだけ認証を通します。
例えばトークンへ利用者の`ver`を入れ、パスワード変更時に保存済みバージョンを増やします。
複数ワーカーでは各プロセスの辞書でなく、共有DBから同期したキャッシュなどで照合します。

```gdscript
func token_auth(key, versions):
	return GD.web.jwt(key, {"check": func(claims):
		return versions.get(claims.get("sub", ""), -1) == claims.get("ver", -2)
	})
```

リバースプロキシの後ろでIP単位に制限するときは、そのプロキシのIPまたはCIDRを`trusted_proxies`へ明示します。
gdは`X-Forwarded-For`の右端から信頼済みプロキシを除き、最初の未信頼IPをkeyにします。
未指定のときと未信頼の接続元からの`X-Forwarded-For`は無視するため、クライアント自身によるIP偽装を許しません。
IPv4とIPv4-mapped IPv6は別物として照合するので、mapped アドレスを信頼する場合はIPv6のCIDRを指定します。zone付きのプロキシ設定は拒否します。

```gdscript
var per_ip := GD.web.rate({"limit": 60, "trusted_proxies": ["127.0.0.1", "172.18.0.0/16"]})
```

制限を通過した応答と429応答には`X-RateLimit-Limit`と`X-RateLimit-Remaining`が付きます。429応答には、再試行までの秒数を示す`Retry-After`も付きます。

### 停止

終了待ちは`app.shutdown(context)`を使います。新規受付とkeep-aliveを止め、処理中リクエストの完了を待ちます。
期限を越えた場合は`Err.TIMED_OUT`を返しますが、処理中リクエストは強制終了しません。
直ちに全接続を閉じる必要がある場合に`app.stop()`を使います。

```gdscript
func close(app: GDWebApp):
	var context := GD.async.context().with_timeout(10.0)
	var _stopped, stop_err := app.shutdown(context)
	if stop_err:
		app.stop()
```

ハンドラーでは`req.context`からリクエストの完了と切断を受け取れます。
`with_cancel()`と`with_timeout()`は親を変更せず子のコンテキストを返し、親の打ち切りは子へ伝わります。
HTTP、データベース、プロセスなどの二値の待ちを打ち切れるようにするには、コンテキストを先頭に渡して`with_context_pair()`で包みます。一値の処理には`with_context()`を使います。
処理が先に終わればその結果を返し、コンテキストが先に終われば処理を取り消します。

```gdscript
func load(req: GDWebRequest, db: GDDatabaseClient) -> Variant, Err:
	return await GD.async.with_context_pair(req.context, db.query_async("SELECT * FROM posts"))
```

### Webの運用と高度な機能

#### 上限と大きなアップロード

大きな本文や長いハンドラーを扱うときは、待ち受け前に`limits()`で上限を明示します。

```gdscript
func main():
	var limited_app := GD.web.app()
	limited_app.limits({"header_bytes": 1048576, "header_values": 500, "header_timeout": 15.0, "body_timeout": 10.0, "job_timeout": 30.0, "jobs": 128})
	return 0
```

1 GBのZIPを受ける場合は要求ごとの上限を置き、書き込み可能なマウントへ逐次保存します。

```gdscript
func main():
	var app := GD.web.app()
	app.limits({"body_timeout": 600.0})
	app.route("POST", "/upload", func(req: GDWebRequest) -> GDWebResponse, Err:
		req.limit(1000 * 1000 * 1000)?
		req.save("uploads://package.zip")?
		return GD.web.text("saved")
	)
	var _server, listen_err := app.listen(8080, "127.0.0.1")
	return 0 if listen_err == null else 1
```

```sh
gd --strict --allow-net=127.0.0.1:8080 --mount=uploads=/srv/uploads:rw serve main.gd
```

本文の読み方で、使うメモリが変わります。

| 読み方 | メモリの使い方 |
|---|---|
| `read()`、`save()` | 少しずつ読む。`save()`は全体をメモリへ置かない |
| `bytes()`、`text()`、`json()` | 残りの本文全体をメモリへ読む。一度読んだ本文は保持するので、何度呼んでも同じ内容を返す。大きな本文には`save()`を使う |
| `req.limit(bytes)` | その要求の本文の上限。超えると、読んだ操作が失敗する |

- 本文の大きさに既定の上限はありません。外へ公開するサーバーでは`req.limit()`を置きます。
- リクエストヘッダーは既定で1 MiBまでです。`limits()`の`header_bytes`で変えられます。
- 遅い接続が待たせるのはその接続だけです。

#### 本文を少しずつ送信する

`GD.web.stream(producer, length=-1, type="application/octet-stream", status=200)`は、`producer(writer)`が`GDWebWriter`へ書いた分だけ送ります。
全体をメモリに結合しません。本文を生成する関数の中で`await`でき、voidまたは二値を返して終わります。

| `GDWebWriter` | 動作 |
|---|---|
| `write(data, offset=0, count=-1)` | バイト列の範囲を送り、受け付けたバイト数を返す。送信が詰まっていれば進むまで待つ |
| `write_text(text, offset=0, count=-1)` | 文字列の範囲をUTF-8で送る。offsetとcountの単位は文字、結果の単位はバイト |
| `flush()` | それまでのwriteの送信完了を待つ。切断はここのエラーと`req.context`のキャンセルでわかる |

- ストリームは一回限りです。応答ごとに新しく作ります。受信本文はストリームを返す前に読み終えてください。
- `length`は送るバイト数です。宣言と実際が合わないと接続を閉じます。不明長（-1）はHTTP/2でDATA フレーム、HTTP/1.1でchunked、HTTP/1.0で接続終了が終端になります。
- HEADと本文を持てないステータスコードでは本文を生成する関数を呼びません。
- 長く待つ本文を生成する関数では`req.context`のキャンセルを確認してください。
- 1回のwriteが1つのデータ片に対応するとは限りません。空の文字列や空のバイト列は終端になりません。

#### HTTPSとHTTP/2

HTTPSは`app.listen_tls(8443, "cert://chain.pem", "cert://key.pem", "127.0.0.1")`で開始し、第二結果の`Err`を確認します。
証明書のディレクトリは`--mount cert=/path/to/certs:r`で読み取り専用にします。PEM形式の証明書チェーンと暗号化されていない秘密鍵を渡します。
鍵の検証に失敗したときはポートを開きません。

TLS 1.2と1.3に対応し、ALPNでHTTP/2とHTTP/1.1を選びます。HTTP/2の各ストリームは独立に進み、一つのキャンセルは他のストリームを閉じません。
`header_timeout`は完了していないTLSの初期通信にも適用されます。クライアント証明書の要求は「TCPとUDP」のTLSの表を参照してください。

#### gzip圧縮

`GD.data.gzip_writer(writer, level=-1)`は、書いたバイト列をgzipにして出力先へ渡す`GDGzipWriter`を作ります。
出力先には`GDFileStream`、TCP接続、`GDWebWriter`を使えます。全体をメモリに貯めません。

| 項目 | 内容 |
|---|---|
| メソッド | `write(bytes)`、`flush()`、`close()`、`reset(writer)`。どれも値と`Err`を返す |
| `level` | -2（Huffmanのみ）、-1（既定）、0..9 |
| `close()` | gzipの末尾を完成する。出力先は閉じない |
| `reset(writer)` | エラーを消し、同じlevelで使い回す |
| `header` | `name`、`comment`（NULを含まないLatin-1）、`extra`（65535 バイトまで）、`mod_time`（0〜4294967295のUnix秒）、`os`（既定255）。最初の書き込みより前に設定する |

HTTPで返すときは`GD.web.header(GD.web.stream(producer), "Content-Encoding", "gzip")`を返し、本文を生成する関数の中で圧縮器を作って書き、`close()`の結果を返します。
`Accept-Encoding`の確認と`Vary`の設定は呼び出し側で行います。秘密情報と外部入力を一緒に圧縮せず、圧縮済みの本文や部分応答には使わないでください。

#### 待ち受けアドレスとポート

IPv6のlocalhostだけで待ち受けるには`app.listen(8080, "::1")!`を指定します。strictでは`--allow-net=[::1]:8080`、接続先は`http://[::1]:8080/`です。
`::1`と`127.0.0.1`は別の待ち受けで、全インターフェースを示す`::`とも異なります。

空きポートをOSに選ばせる場合は`app.listen(0)`の直後に`app.port()`を読みます。待ち受けを保持したまま番号を得るので、他のプロセスに取られません。
待受け開始後のソケット障害は`app.serve_error()`で確認できます。一時的な資源不足では短く待って再試行し、継続できない障害では待受けを閉じます。
strictでは選ばれるポートを事前に限定できないため、`--allow-net=127.0.0.1`のようにホスト全体を許可します。
`GD.net.free_port()`と`is_free()`は診断用の瞬間的な確認で、その番号を確保する機能ではありません。

#### HTTP クライアントの接続

- HTTPSではHTTP/2を使い、同じ宛先への並行要求は一本の接続を共有します。非対応の相手と平文HTTPにはHTTP/1.1を使います。
- HTTP/1.1の接続は、本文を末尾まで読むと同じ宛先へ再利用します。空き接続は全体100本、宛先ごと2本、90秒まで保持します。
- 再利用した直後に閉じられた場合、安全に再送できるメソッドだけ1度開き直します。
- HTTP/2では、相手が未処理と明示した要求だけを最大7回、間隔を延ばしながら再送します。要求の期限とキャンセルは守ります。

#### Webの設定一覧

`GD.http.fetch()`と`GD.web`の各関数に辞書で渡す設定と、その既定値です。時間は秒、大きさはバイトです。

| 入口 | 設定と既定 | 意味 |
|---|---|---|
| `GD.http.fetch` | `method="GET"`, `headers={}`, `body=null` | HTTP メソッド、送信ヘッダー、送信本文 |
| 同上 | `timeout=0.0`, `max_body=0` | 要求全体の秒と応答本文のバイト。0は上限なし。新規TCP接続は30秒、TLS初期通信は10秒が既定 |
| 同上 | `save=""`, `sha256=""` | 2xx 本文を`save`へ逐次保存し、返却本文は空。`sha256`は`save`必須の64桁16進数で、一致した完了ファイルだけを置く |
| 同上 | `authority="host:port"` | CONNECTだけのリクエストの送信先 |
| `GDWebApp.limits` | `jobs=0`, `job_timeout=0.0` | 保持する非同期ハンドラー数と秒。0は無制限 |
| 同上 | `header_timeout=0.0`, `body_timeout=0.0` | リクエストヘッダー/bodyを受け終える秒。0は無期限 |
| 同上 | `header_bytes=1048576`, `header_values=2147483647` | リクエスト lineを含むヘッダー バイトと、ヘッダー行数 |
| `GD.web.jwt_sign` | `ttl=900` | `iat`/`exp`を補う秒。0は自動付与しない |
| `GD.web.jwt` / `jwt_verify` | `leeway=0.0`, `require_exp=true` | 時刻許容秒と`exp`必須化 |
| 同上 | `iss=""`, `aud=""`, `keep="jwt"` | 空でない場合のissuer/audience一致と保持名 |
| 同上 | `check=Callable()` | 署名検証後にクレームを受け取る失効判定。指定時は真だけを許可 |
| `GD.web.sessions` | `total=1024`, `per_user=3` | プロセス内の全セッション数と同一ユーザー数 |
| 同上 | `idle=1800`, `life=43200` | 操作がない場合の有効期間と、セッション全体の有効期間（秒） |
| 同上 | `cookie="sid"`, `keep="user"` | Cookie名とリクエスト内の保持名。Cookie名はASCIIのトークン文字 |
| `GD.web.rate` | `limit=60`, `window=60.0` | keyごとの最大連続回数と、その回数分を補充する時間（秒） |
| 同上 | `expires=180.0` | 最後の要求からkeyを解放するまでの秒数 |
| 同上 | `keys=0`, `key=Callable()` | プロセス内で保持するkey数とkey選択関数。0は無制限。明示した保持枠が満杯なら期限が最も早いkeyを追い出す |
| 同上 | `trusted_proxies=PackedStringArray()` | 転送元IPを信頼するプロキシのIPまたはCIDR |
| `GD.web.csrf` | `allow_missing=false` | 状態変更でFetch Metadataが無いクライアントを許すか |
| `GD.web.text_rule` | `min=0`, `max=4096` | テキストの文字数 |
| `GD.web.int_rule` | `min=-9223372036854775808`, `max=9223372036854775807` | 64ビット整数の範囲 |
| `GD.web.number_rule` | `min=-1e308`, `max=1e308` | 有限浮動小数の範囲 |
| `GD.web.list_rule` | `min=0`, `max=1024` | 要素数 |
| `GD.web.object_rule` | `extra=false` | 未定義フィールドを残すか |

`GDWebApp.limits`は表にある6つの設定名だけを受け、綴り違いや`body_limit`を誤りとして拒否します。

範囲外の値は設定時に失敗します。受け付ける範囲は、APIリファレンスの各関数の説明にあります。

## データベース

`GD.database.client()`が返すクライアントは、SQLiteとPostgreSQLを同じ書き方で扱います。
ローカル開発は組み込みSQLite、本番はPostgreSQLという切り替えは、`open()`に渡す`driver`で行います。

```gdscript
func main() -> int, Err:
	var local := GD.cli.env("DB_DRIVER", "sqlite") == "sqlite"
	var db := GD.database.client()
	db.open({
		"driver": "sqlite" if local else "postgres",
		"path": "user://app.sqlite3",
		"host": "127.0.0.1",
		"database": "app",
		"user": "app",
		"password": GD.cli.env("PGPASSWORD", ""),
	})?
	db.query("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)")?
	db.query("INSERT INTO users (id, name) VALUES ($1, $2) ON CONFLICT (id) DO NOTHING", [1, "ada"])?
	var out := db.query("SELECT id, name FROM users WHERE id=$1", [1])?
	print(out.rows[0].name)
	db.close()
	return 0
```

表の作成もINSERTもSELECTも`query()`一つで送ります。受け取るのは`columns`、`rows`、`tag`を持つ辞書で、
`rows`は列名を鍵にした辞書の配列です。上の例なら`out.rows[0].name`が`ada`になります。
SQLに入れる値は、`$1`、`$2`に対応する順番で渡します。両ドライバーで同じ書き方です。SQLは変換しないため、両方で通るSQLを使います。

| メソッド | 用途 |
|---|---|
| `query(sql, args)` | 結果を全部集めて返す |
| `query_row(sql, args)` | 先頭1行だけ返す。行が無ければ`Err.NOT_FOUND` |
| `query_rows(sql, args)` | `GDDatabaseRows`を開き、1行ずつ読む。大量の結果向き |
| `stats()` | 接続数、使用中、空き、待ち回数、待ち時間、接続を閉じた理由別の累積数 |

`query_rows()`は`while rows.next()`で進め、`scan()`で列名付きの辞書、`values()`で列順の配列を得ます。
`next()`がfalseになったら`err()`を調べます。途中で止める場合は`close()`を呼びます。

```gdscript
func list_users(db: GDDatabaseClient) -> Variant, Err:
	var rows := db.query_rows("SELECT id, name FROM users ORDER BY id")?
	while rows.next():
		var user := rows.scan()?
		print(user.id, " ", user.name)
	if rows.err() != null:
		return null, rows.err()
	return null
```

制約違反では第二結果の`e.info`にプログラムでの判定用の情報が入ります。`violation`は`duplicate`、`not_null`、`foreign_key`のいずれか、
`columns`は関係する列名です。PostgreSQLでは`code`、`table`、`constraint`もサーバーが返した場合に入ります。
値そのものは`info`へ残しません。SQLite自身が報告した失敗では`source="sqlite"`と拡張`source_code`を保ちます。
SQLiteのforeign keyエラーメッセージには列名が無いため、その場合の`columns`は空です。

```gdscript
func save(db: GDDatabaseClient) -> void:
	var _saved, e := db.query(
		"INSERT INTO users(id,name) VALUES($1,$2)",
		[1, "ada"])
	if e and e.info.get("violation") == "duplicate":
		var columns := e.info.get("columns", PackedStringArray())
		print("重複した列: ", columns)
```

### トランザクションとマイグレーション

複数の更新をまとめて確定または取り消したいときは`transaction()`を使います。渡した関数には、専用の接続を使う
`GDDatabaseTx`が渡ります。コールバックの第二結果が`null`ならコミットし、`Err`ならロールバックします。

```gdscript
func save(db: GDDatabaseClient, id: int, title: String) -> Variant, Err:
	return db.transaction(func(tx: GDDatabaseTx) -> int, Err:
		tx.query("INSERT INTO posts(id,title) VALUES($1,$2)", [id, title])?
		tx.query("UPDATE counters SET value=value+1 WHERE name='posts'")?
		return id
	)
```

- コールバックの中では、渡された`tx`だけでSQLを実行します。元の`db.query()`はこのトランザクションに入りません。
- コミットの失敗はそのまま失敗として返ります。
- close()やキャンセルはCOMMITの開始前ならロールバックし、開始後なら結果が確定してから接続を閉じます。
- コールバックが終わった後は、保存しておいた`tx`も新しいSQLを受け付けません。

PostgreSQLで、同時に走る別のトランザクションとぶつかって失敗する場合（SQLSTATE `40001`）は、`serialize()`でやり直せます。

- 渡す関数は`transaction()`と同じ形で、やり直すたびに新しいトランザクションで呼ばれます。
- やり直しは最大10回です。別のエラーやキャンセルではそこで止まります。
- メール送信のように取り消せない処理は、関数の外で行ってください。

```gdscript
func increment(db: GDDatabaseClient, id: int) -> int, Err:
	var saved, e := db.serialize(func(tx: GDDatabaseTx) -> int, Err:
		tx.query("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE")?
		tx.query("UPDATE counters SET value=value+1 WHERE id=$1", [id])?
		return id
	)
	return saved, e
```

テーブル構成を順番に適用するときは、SQLをセミコロンで分割せず、SQL文の配列を`migrate()`へ渡します。
途中の一文が失敗すると全体をロールバックし、成功時は適用した文の数を返します。
バージョンとチェックサムはアプリケーション側で管理します。

```gdscript
func migrate(db):
	return db.migrate([
		"CREATE TABLE posts(id INTEGER PRIMARY KEY, title TEXT NOT NULL)",
		"CREATE INDEX posts_title ON posts(title)",
	])
```

### データベースの高度な機能

#### ドライバーの違い

| 項目 | SQLite | PostgreSQL |
|---|---|---|
| 向く用途 | ローカル開発、単一プロセス | 本番、異常終了耐性、複数ワーカー |
| 接続 | クライアントごとに一つ。変更履歴と一時表はメモリに置く | 必要な接続だけ作るプール。`pool=25`のように最大数を指定できる |
| 追加の入口 | 短い処理をその場で行う`GD.database.sqlite.open()` | まとめ送り、配列、JSONBを使う`GD.database.postgres` |
| 注意 | 永続DBはrollback journalで未完了の書込みを回復する。既存の`-wal`、`-shm`は通常のSQLiteでチェックポイントしてから開く | ループバック以外のホストではTLS証明書とホスト名を既定で検証。ループバックはTLSなしが既定 |

`GD.database.postgres.client()`と`GD.database.redis.client()`の`open()`は`open(host, port, opts)`の形で、接続先を引数に取ります。

#### SQLiteの並行

同じクライアントの`query()`は受付順に実行します。別のクライアントは並行に進みますが、同じデータベースファイルへの書き込みはSQLiteのロックに従います。
`GD.database.sqlite.open()`が返す`GDSQLiteDB`と`GDSQLiteStatement`は、呼び出し元でそのまま実行する同期APIです。
短い処理だけに使い、同時利用はしないでください。並行処理には`GDDatabaseClient`を使います。

#### PostgreSQLの接続と型

接続はプールが必要な分だけ作り、使い終わると戻します。ふだんは接続を意識せず`query()`を呼ぶだけです。空きが無いときは順番に待ちます。

| やりたいこと | 方法 |
|---|---|
| 複数のSQLを1つのまとまりで実行する | `BEGIN`を自分で送らず`transaction()`を使う |
| 複数のSQLをまとめて送る | `query_many`、`fetch_many`、`exec_many` |
| 重複登録を見分ける | `Err.ALREADY_EXISTS`。細かい原因は`e.info.code`のSQLSTATEで調べる |
| 接続が足りているか調べる | `stats()`の`wait_count`（接続待ちの回数） |
| 認証方式を固定する | `auth="scram"`。`auth="md5"`は古いサーバー用 |

列の値は次の型で届きます。

| 列の型 | 受け取る値 |
|---|---|
| `real`、`double precision` | `float` |
| `numeric` | 桁を失わないよう文字列 |
| `json`、`jsonb` | 辞書や配列。整数は`int`のまま |
| `int[]`、`text[]`などの配列 | 同じ要素型のArray。nullと入れ子も保つ |

キャンセルや期限切れになった問い合わせは接続ごと閉じます。サーバー側のSQLがすぐ止まるとは限りません。文字コードはUTF-8です。

#### Redisの接続

Redisは`GD.database.redis.client()`で使います。コマンド名と引数を分けて`query()`へ渡します。

```gdscript
func main() -> int, Err:
	var redis := GD.database.redis.client()
	redis.open("127.0.0.1", 6379)?
	redis.query("SET", ["greeting", "hello"])?
	print(redis.query("GET", ["greeting"])?)
	redis.close()
	return 0
```

| やりたいこと | 方法 |
|---|---|
| 値が無いことを見分ける | `GET`は`Err.NOT_FOUND`を返す |
| 複数のコマンドをまとめて送る | `pipeline(cmds)`。結果は送った順のArray |
| 多くの要求から同時に使う | `GD.database.redis.pool()`。接続は最初の`query()`で作る |
| `MULTI`、`WATCH`、`SELECT`、購読を使う | プールではなく専用の`GDRedisClient`を使う |
| 期限を決める | `open()`の`timeout`（秒） |

TLSの選び方はPostgreSQLと同じです。

#### データベースの設定一覧

`open()`に辞書で渡す設定と、その既定値です。

| 入口 | 設定と既定 | 意味 |
|---|---|---|
| `GDDatabaseClient.open` | `driver="postgres"`, `path=""` | ドライバーとSQLiteのパス。`user://`、`--mount`で書き込みを許可した名前、`:memory:`のどれか。`--strict`でなければ`res://`も使える |
| 同上 | `host="127.0.0.1"`, `port=5432` | PostgreSQLの接続先 |
| 同上 | `pool=0` | PostgreSQL最大接続数。0は上限なし、SQLiteでは使わない |
| 同上 | `max_rows=0`, `max_bytes=0` | `query()`が集める1結果の行数とバイト。0は無制限。`query_rows()`には適用しない |
| `GDPostgresClient.open` | `user="postgres"`, `database="postgres"`, `password=""` | 認証とDB名 |
| 同上 | `connect_timeout=0.0`, `timeout=0` | 接続と問い合わせの秒。プールの接続待ちも問い合わせ時間に含む。0は無期限。必要なら各期限を設定する |
| 同上 | `auth="any"`, `allow_cleartext_password=false` | `auth="scram"`/`"md5"`で方式固定。平文パスワード応答は明示時のみ |
| 同上 | `tls=<hostで決定>`, `ca=""` | 外部ホストは`verify-full`、ループバックは`disable`。CA ファイルは明示時だけ |
| `GD.database.sqlite.open` | `busy_ms=0`, `max_ms=0`, `foreign_keys=false` | ロック待ちと実行期限のミリ秒。`busy_ms=0`は競合を即時通知し、`max_ms=0`は実行期限なし。外部キー検査は明示して有効にする |
| 同上 | `max_rows=0`, `max_bytes=0` | 1結果の行数とバイト。0は無制限 |
| `GDRedisClient.open` | `password=""`, `dial_timeout=5`, `read_timeout=5`, `write_timeout=5` | パスワードと接続・読取り・書込み期限の秒。各期限の明示0は無期限。`timeout`を指定すると三つをまとめて設定でき、個別指定が優先される |
| 同上 | `tls=<hostで決定>`, `ca=""` | PostgreSQLと同じTLS選択 |
| `GD.database.postgres.pool` | size既定0、0または1..2147483647 | 0は上限なし。接続は必要になった分だけ作る |
| `GDPostgresPool.open` | `max_idle=2` | 仕事を終えた後に保持する接続数。0は保持せず、最大接続数を超えない |
| `GD.database.redis.pool` | size既定0、0..2147483647 | 最大接続数。0はCPU数の10倍。同時に作る接続は最大接続数まで |
| `GDRedisPool.open` | `pool_timeout=read_timeout+1.0`（読取り期限が0なら30秒）、`conn_max_idle_time=1800` | 接続の空きを待つ期限と、返却後に再利用できる期間。各設定の明示0は無期限。期限を過ぎたidle接続は次の貸出時に閉じる |

長いRedis問い合わせが必要な場合は、`read_timeout`を明示して期限を延ばせます。

`max_rows`または`max_bytes`を越えた`query()`は、その問い合わせだけを失敗にします。
期限切れや壊れた応答で順序を失った場合は接続全体を閉じます。

範囲外の値は設定時に失敗します。受け付ける範囲は、APIリファレンスの各関数の説明にあります。

## 定期処理

決まった時刻に一度だけ動かす仕事は、普通のスクリプトとして書き、OSのcronやsystemd タイマーから呼びます。
gd側に常駐の仕組みは要りません。

```gdscript
func collect() -> int, Err:
	var now := GD.time.to_iso(GD.time.now())
	GD.file.append_text("store://log.txt", now + "\n")?
	return 0

func main() -> int, Err:
	collect()?
	return 0
```

```sh
gd --strict --mount store=/var/lib/app:rw collect.gd
```

自分で間隔を持って回り続ける仕事は、`GD.async.spawn()`へ渡して`gd serve`で常駐させます。
`spawn()`へ渡した処理は`main()`が返った後も動き続けます。

```gdscript
func every(sec, fn):
	while true:
		await GD.async.sleep(sec)
		fn.call()

func collect():
	print(GD.time.to_iso(GD.time.now()))

func main():
	var _job := GD.async.spawn(every.bind(60.0, collect))
	return 0
```

```sh
gd serve schedule.gd
```

止めるときはプロセスを終わらせます。Webサーバーと同じ常駐なので、ここでも`serve`が必要です。

## 公式拡張モジュール

本体を小さく保ち、外部サービス固有の機能は必要なプロジェクトだけへGDScript パッケージまたはGDExtensionとして加えます。

| 入口 | 用途 | 導入 |
|---|---|---|
| `Discord` | DiscordのGatewayとRESTを使う、GDScriptだけで書かれたテキストBot | `gd add @gd/discord` |
| `GDSupabase` | DatabaseとAuthのクライアント | `gd add ext:@gd/supabase` |

公開中のパッケージは[パッケージ一覧](https://gd.progsha.com/pkg/)で探せます。任意で導入するため、本体から生成するAPIリファレンスには含まれません。

- `gd add`で入れた拡張は起動時に信頼して読み込むため、読み込み許可のオプションは要りません。`--strict`では接続先の`--allow-net`が必要です。
- `--allow-ext`と`--deny-ext`が効くのは、スクリプトが実行中に`GDExtensionManager.load_extension()`で読む場合です。
- 入れた拡張はプロセスと同じ権限で動くので、信頼する版を`gd.lock`で固定してコミットしてください。

## パッケージと配布

他の人が公開したパッケージを使うときは、`gd init`で`gd.json`を作ってから追加します。公開中のパッケージは[パッケージ一覧](https://gd.progsha.com/pkg/)か`gd search`で探せます。

```sh
gd init
gd add hello
```

| やりたいこと | コマンド |
|---|---|
| 探す | `gd search discord` |
| 追加する | `gd add hello`（`gd install hello`も同じ） |
| スコープや版の範囲を指定する | `gd add @gd/hello@^0.8.0` |
| 呼び名を決めて追加する | `gd add greet @gd/hello` |
| ネイティブ拡張を追加する | `gd add ext:@gd/supabase` |
| URLや手元のディレクトリから追加する | `gd add util https://example.com/util.gd`、`gd add ../mylib` |
| 別の環境で同じ版を復元する | `gd install --frozen` |
| 新しい版を調べる、上げる | `gd outdated`、`gd update` |
| 外す | `gd uninstall hello` |
| 入れたものを一覧する | `gd info` |

依存は`gd.json`（何を使うか）と`gd.lock`（どの版を使うか）に記録されます。両方をコミットしてください。引数なしの`gd install`は`gd.json`の依存を復元します。`--frozen`・`--cached-only`・`--sync`はこの復元で使います。

### パッケージを使う

入れたパッケージは、スクリプトの先頭で`@import 呼び名`と書いて読み込みます。

```gdscript
@import hello
```

- パッケージの実体は利用者ごとの共有キャッシュ（`pkg://<呼び名>/`）に置かれ、プロジェクトへは複製しません。
- `gd.json`に書いた依存がキャッシュに無ければ、最初の実行で取得します。`--strict`では登録所への`--allow-net`が要ります。
- `gd add hello`のような短い名前は登録所で探します。同じ名前が2つ以上あるときは、どれにするか尋ねます。
- `gd add`の既定の呼び名は、パッケージ名の`-`と`.`を`_`にした識別子です。エンジンクラスやキーワードと同じ呼び名は断ります。
- コミットするのは`gd.json`と`gd.lock`です。`gd init`は`pkg/`を`.gitignore`へ書きます。
- `--frozen`はロックを変更しません。オフラインの配布先では、ネットワークのある環境で先に取得し、`--cached-only`を併用します。
- install、add、updateが途中で失敗したときは、プロジェクトの配置とロックを元へ戻します。
- ロックは登録所に結び付いています。別の登録所へ切り替えるには明示的なロック移行が必要です。

### コマンドとして導入する

`gd install -g`は、パッケージやスクリプトを`my-tool`のようなコマンドとして導入します。作業中のプロジェクトの`gd.json`と`gd.lock`は変更しません。

```sh
gd install -g tool                      # Install a published package by its short name.
gd install -g @scope/tool@1.0.0         # Choose the scope and version.
gd install -g --name my-tool ./tool.gd  # Install a local script.
gd uninstall -g my-tool
```

| 指定 | 意味 |
|---|---|
| `-n` / `--name` | コマンド名。省略するとパッケージ名かスクリプト名（`main.gd`や`mod.gd`なら親ディレクトリ名） |
| `--root <dir>` | 導入先。省略すると`GD_INSTALL_ROOT`、それも無ければ既定のキャッシュ。その中の`bin/`をPATHへ追加する |
| `-f` / `--force` | 導入済みのコマンドを更新する |
| `-- 引数` | 起動のたびに先頭へ渡す固定引数 |

- `tool`のような短い名前は登録所で探します。同じ名前のパッケージが2つ以上あるときは、候補を並べてどれにするか尋ねます。`@scope/tool`と書けば尋ねません。
- 入口は`main(args)`を持つスクリプトです。公開パッケージは`mod.gd`、ローカルのディレクトリは`gd.json`の`main`を使います。
- 導入時に付けた`--allow-*`と`--mount`は、起動のたびに同じ設定で使われます。
- 更新に失敗したときは、導入済みのコマンドをそのまま残します。
- `--root`を付けて導入したコマンドは、同じ`--root`を付けて削除します。

### importの短い書き方

`@import`は`const 名 = preload(...)`の短い書き方です。

外部依存がなくても使えます。実際の登録所の依存とロックファイルを含む[samples/packages](https://github.com/prog-sha/gd/tree/master/samples/packages)を用意しています。

```gdscript
@import greet
@import "./util"
@import greet/style as theme
```

- 引用符の無い名前は、`gd.json`の`imports`に宣言した呼び名だけを解決します。同名のファイルを探しに行きません。
- 相対ファイルは引用符で`./`または`../`から書きます。 `.gd`は省略できます。同名のファイルとディレクトリが両方あるときは`"./util.gd"`のように指定します。
- 識別子は`as`が無ければ最後の要素そのままで、`mod.gd`を持つディレクトリはディレクトリ名です。
- `gd fmt`は`@import`をそのまま残します。
- 本家Godotは`@import`を知らないので、Godotと共有するファイルでは`const`と`preload`を書いてください。

### パッケージを作る

パッケージは`gd.json`を根に持つ一つのプロジェクトです。`gd init @scope/name`が`mod.gd`とテストのテンプレートを作り、
`gd test`で回し、`gd publish`で公開します。

```json
{"name":"@scope/hello","version":"1.0.0","main":"src/mod.gd","include":["src"]}
```

パッケージの本体は、作者のGitHub Releaseから配ります。登録所が持つのは、ファイルの場所と指紋（SHA-256）だけです。

```sh
gd publish                         # Write the release files to tmp/release/scope-hello-1.0.0/.
gh release create 1.0.0 tmp/release/scope-hello-1.0.0/*
gd publish https://github.com/OWNER/REPO/releases/download/1.0.0
gd add hello gd:@scope/hello@^1.0.0
```

1. 引数なしの`gd publish`は、公開するファイルを`tmp/release/<scope>-<name>-<版>/`へ書き出します。ファイル名は中身のSHA-256です。
2. それらをGitHub Releaseのアセットとして上げます。同じReleaseに複数のパッケージや版のファイルを置いても、名前は衝突しません。
3. ReleaseのダウンロードURLを付けて`gd publish`を実行します。gdはURLから各ファイルを取得し、手元と一致することを確かめてから登録所へ登録します。

公式の登録所へ投稿したパッケージは、審査で承認されると検索・インストールできます。`gd publish`が審査番号を表示した場合は、承認されるまで公開されません。承認の直前にも、登録所がReleaseのファイルを照合します。

- 利用者のgdは、ファイルをGitHubから直接取得し、登録所の指紋と照合します。Releaseのファイルを差し替えると、照合に失敗してインストールできなくなります。

- 入口は`mod.gd`です。複数ファイルなら`include`へファイルまたはディレクトリを明示します。
- mainのディレクトリがパッケージの根になるので、パッケージ内の相対preloadはそのまま動きます。
- パッケージは自分の`gd.json`の`imports`で他の登録所パッケージを使えます。`gd publish`がその`imports`を登録所へ載せます。
- `class_name`は公開できます。installは同名クラスの衝突を検査し、衝突すれば全体を元へ戻します。
- `gd.json`の`godot`を`true`にすると、gd固有のAPIを使わず本家Godotでも動くという作者の宣言になり、`gd search`が`[godot]`と示します。

開発中のパッケージは`gd add ../path`でローカルから足します。呼び名は先の`gd.json`の`name`から取ります。
チェックアウトを`pkg/<呼び名>/`へ複製し、内容を比較するハッシュ値が変われば次の実行で複製し直します。
`.`で始まるファイル、`pkg/`、`tmp/`、`gd.json`を持つ下位ディレクトリ、`token`は複製しません。
`gd publish`は、ローカル importの先に`name`と`version`のある`gd.json`があれば登録所の範囲に変換し、無ければ拒みます。

### ローカル開発チャンネル

リポジトリで、実際の登録所をループバックで起動できます。登録所の情報は`tmp/dev-channel/data`へ保存されます。

```sh
export GD_TOKEN='開発用のトークン'
uv run --no-project python -B devtools/channel.py serve --scope dev --port 8787
```

別のターミナルで`GD_REGISTRY=http://127.0.0.1:8787`と同じ`GD_TOKEN`を設定し、`@dev/name`のパッケージから`gd publish --dry-run`、`gd publish`を実行します。書き出したファイルは、`python3 -m http.server 8788 --bind 127.0.0.1 --directory tmp/release`などでループバックから配り、`gd publish http://127.0.0.1:8788/dev-name-1.0.0`で登録します。開発用の登録所に限り、ループバックのHTTPを受け付けます。利用側も`GD_REGISTRY`を設定すれば、`gd search`、`gd add`、`gd install`、`gd install -g`、`gd compile`がその登録所を使います。プロジェクトの`gd.json`に`registry`がある場合は、そのURLが優先します。

### 依存の解決

`gd install`は依存グラフ全体を解決します。版は、`gd.lock`が固定した版、今回すでに選んだ版のうち範囲を満たすもの、
登録所の最新一致の順で選びます。

`gd.lock`は解決した`imports`の設定も保持します。設定が変わった実行では同じ依存関係の解決処理で再解決し、要求外の古い版を使いません。
`--frozen`は設定の不一致を拒否します。同じパッケージに複数の別名がある場合、辞書順で最初の別名を配置先に使います。

- GDScriptだけで書かれたパッケージは、版ごとに別のものとして共存できます。
- ネイティブ拡張はプロセスに一つしか読めないため、一つの版に揃えます。範囲が両立しなければ取得前に止まります。
- 複数のプラグインで同じ依存パッケージを共有するためのpeer依存には対応していません。
- 登録所パッケージの正式なパスは`pkg://@scope/name@版/`です。`pkg://<呼び名>/`は、書いたスクリプトが属するパッケージの`imports`で正式パスへ展開されます。同じ呼び名でもパッケージごとに違う版を指せ、同じ版はどこから辿っても一つのスクリプトです。
- `gd.lock`には各パッケージの`imports`の解決先も記録され、`gd info`が一覧します。
- `gd uninstall`と`gd update`は、どのパッケージも使わなくなったものを`gd.lock`と`pkg/`から外します。
- 検索の順位が変わっても、既知のパッケージのinstallとロックの検証には影響しません。

### Godotと共有する置き場

本家Godotなど`res://`しか読めない環境と共有するときは、`gd.json`へ`"place": "project"`を書きます。
パッケージを`pkg/<呼び名>/`へ複製し、`pkg://`も`res://pkg/`もそこを指します。
他のパッケージだけが使うものは`pkg/@scope/name@版/`へ置きます。

- `gd install --godot`は、パッケージを`addons/<呼び名>/`へ複製します。`add`、`update`、`uninstall`、`info`でも`--godot`を指定します。パッケージ内に`addons/`があっても、その部分だけを取り出すことはありません。
- `project.godot`のあるディレクトリでは`place`の既定が`project`になり、`.gitignore`は書きません。gdの無い同僚が開けるよう`pkg/`をコミットします。
- installは`preload`、`load`、`extends`に書かれた`res://`参照を配置先へ書き換えます。文字列、コメント、実行時に組み立てるパスは書き換えません。
- `place`はファイルの置き場を決めるだけで、gd固有のAPIや構文をGodot向けに変換する機能ではありません。共有するソースは標準構文と相対preloadで書きます。

### ネイティブ拡張のパッケージ

- ネイティブ拡張は読み込みに実ファイルが要るため、`place`に関わらず`pkg/<呼び名>/`へ置きます。
- スクリプトから名指せるのは、自分のパッケージが`ext:`で取り込んだ拡張のクラスだけです。プロジェクトのスクリプトなら`gd.json`、パッケージのスクリプトならそのパッケージの`imports`が基準です。
- 登録所の外にある拡張はプロジェクトのスクリプトだけが使えます。
- 登録所のパッケージの拡張が、設定ファイルの`[classes]`に無いクラスを登録すると起動時に止まります。
- 配布先のOSで取得するか、`gd compile`を配布先のOSで実行してください。

### 設定と環境変数

`gd.json`の設定は次の11件です。

| 名前 | `gd init`の生成値 / 未指定時 | 意味 |
|---|---|---|
| `name` | `my-tool` / 必須 | プロジェクト名。publishは`@scope/name`が必要 |
| `version` | `0.1.0` / 必須 | パッケージのバージョン |
| `tasks` | run/testの2件 / 無し | `gd task`から呼ぶコマンド |
| `imports` | `{}` / `{}` | 呼び名と依存先。publishするパッケージでは登録所パッケージだけ |
| `registry` | 未指定 / 環境または公開登録所 | プロジェクト固定の登録所URL |
| `main` | 未指定 / `mod.gd` | publishする`mod.gd`または`.gdextension`入口 |
| `include` | 未指定 / mainだけ | GDScriptだけで書かれたパッケージへ含めるmain ディレクトリ内のファイルまたはディレクトリ |
| `place` | 未指定 / `cache`（`project.godot`があれば`project`） | パッケージの置き場。`project`で`pkg/`へ複製する |
| `godot` | 未指定 / `false` | gd固有のAPIを使わず本家Godotでも動くパッケージの宣言 |
| `description` | 未指定 / 空 | 登録所に出す説明 |
| `assets` | 未指定 / 無し | Godotアセットの管理用の呼び名と`チャンネル:作者/名前[@版]`。`addons/`へ配置する |

gdが読む環境変数は次の通りです。スクリプトから環境を読む実行では`--allow-env`で名前を許可します。

| 環境変数 | 用途 |
|---|---|
| `GD_CACHE_HOME` | パッケージのキャッシュ根。未指定はWindowsのLocalAppData内`gd`。macOS/Linuxは絶対パスの`XDG_CACHE_HOME/gd`、それがなければホームディレクトリ内`.gd`。`HOME`未設定時はOSの利用者情報を使う |
| `GD_INSTALL_ROOT` | `gd install -g`で入れるコマンドの置き場。`--root`が優先 |
| `GD_USER_HOME` | `user://`の保存先。絶対パスを指定する。未指定ではOSの利用者データ領域（macOSは`~/Library/Application Support/gd/user`、Linuxは`~/.local/share/gd/user`、Windowsは`%APPDATA%/gd/user`）に、実行したスクリプトのディレクトリごとに作る |
| `GD_REGISTRY` | 登録所。未指定は`https://gd.progsha.com/pkg`。`gd.json`の`registry`が優先 |
| `GD_RELEASES` | `gd upgrade`が読む配布元。未指定は`https://github.com/prog-sha/gd/releases` |
| `GD_TOKEN` | publishのトークン。設定ファイルへ書かず、publishするプロセスだけへ渡す |
| `LC_ALL`、`LANG` | `gd doc`の手引きの言語 |
| `GD_WORKER` | `--workers`が作る内部印。利用者が設定する値ではない |

遠隔パッケージと登録所はHTTPSを使います。ループバックの開発用登録所に限りHTTPも使えます。
取得したパッケージとネイティブライブラリは登録所索引のSHA-256と照合します。
パッケージの全ファイル合計は500 MiBまでです。

### 1つの実行ファイルにまとめて配布する

`compile`で、スクリプト、HTMLテンプレート、静的ファイル、マイグレーション、依存パッケージ、対象OSのGDExtensionを1つの実行ファイルへまとめます。配布先にキャッシュは要りません。

```sh
gd compile -o app main.gd
./app
```

- `gd.json`が名指すパッケージと、それらが取り込むパッケージを全部同梱します。
- 同梱したWebアプリも`./app serve --no-scene-tree --allow-net`で常駐できます。
- ローカルパスのパッケージからは、`.env`など`.`で始まるファイルと`gd.json`の`token`を除きます。
- 秘密情報をソースへ埋め込まないでください。compileは`.env`を除外しますが、ソースに書いた値は実行ファイルに残ります。

## アセットチャンネル

`gd search`はパッケージに加え、Godotの公開アセットカタログとgdの審査済みカタログを検索します。独自の登録所を設定している場合は、その登録所だけを検索します。

| チャンネル | 用途 |
|---|---|
| `official` | [Godot Asset Store](https://store.godotengine.org/)のアセット |
| `gd` | gdの審査済みアセット |

検索結果の`asset:チャンネル:作者/名前`を、そのまま`gd add`に渡します。

```sh
gd search dialog
gd add asset:official:publisher/dialog
gd add ui asset:gd:publisher/dialog@1.2.0
gd install --frozen --cached-only
gd update ui
gd uninstall ui
```

- バージョン名が重複する場合は、版の一覧にあるIDを`asset:official:publisher/dialog@#123`のように指定します。名前に空白を含む場合は引数全体を引用符で囲みます。
- チャンネルを省略すると`official`になります。取得に失敗しても別のチャンネルへ切り替えません。
- `gd.json`の`assets`に依存先を記録し、`gd.lock`に取得元・選んだ版・SHA-256を固定します。両方をコミットしてください。
- `asset:`で指定するGodotアセットは、ZIP内の`addons/`以下をプロジェクトの`addons/`へ配置します。`assets`の呼び名は管理用です。通常のパッケージは`imports`に記録し、`@import`で読み込みます。
- 最初の取得では対応するエンジンのバージョンを確認し、安定版を優先します。以後のinstallはロックした版を使い、updateで再選択します。`@1.2.0`のように指定した版はupdateでも変わりません。
- `--frozen`ではロックを変更せず、`--cached-only`では通信しません。キャッシュのない版をオフラインで取得することはできません。
- 手作業で置いたアドオンや利用者が編集したファイルは上書きしません。処理が失敗したら、パッケージ・アドオン・設定・ロックを元へ戻します。
- gdチャンネルはアセットの説明、配布URL、SHA-256を審査して登録します。アーカイブは作者の配布先から取得し、gdがSHA-256を照合します。Godotエディタはこの追加のハッシュ値を使わないため、同じ照合は行いません。

アセットの詳細や版の一覧を調べるには、ソース配布に含むクライアントも使えます。

```sh
gd tools/store.gd channels
gd tools/store.gd search official 4.7 dialog
gd tools/store.gd show gd publisher dialog
gd tools/store.gd releases gd publisher dialog
```

## 対応範囲と報告

gdはAPIが固まる前の公開版です。後方互換は前提にしないでください。変更した点と基準にしたGodotの版は
[CHANGELOG](https://github.com/prog-sha/gd/blob/master/CHANGELOG.md)に書きます。
gdはGodot FoundationまたはGodot Engine プロジェクトの公式製品ではありません。

不具合は[Issues](https://github.com/prog-sha/gd/issues)へ、公開すべきでない脆弱性は
[GitHubの非公開報告](https://github.com/prog-sha/gd/security/advisories/new)から知らせてください。

## 開発中の機能（参考）

ここにある機能は開発中です。使い方や仕様は予告なく変わります。参考程度にご覧ください。

### エディタ（gd-godot）

`gd editor`でエディタを開きます。初回は画面用の実行ファイル`gd-godot`を自動で導入します。

| やりたいこと | コマンド |
|---|---|
| エディタを開く | `gd editor` |
| プロジェクトを指定して開く | `gd editor <プロジェクトディレクトリ>` |
| プロジェクトを画面付きで実行する | `gd run-game <プロジェクトディレクトリ>` |

詳しくは[gd-godot マニュアル](gd-godot.md)を参照してください。

### オンラインゲーム

`GD.online.match()`は、オンラインゲームの対戦相手を探すサーバーを作ります。部屋は自分のアドレスと人数を知らせ、
参加する側は空きのある部屋を1つ受け取ります。状態はプロセスのメモリ、または複数のプロセスで共有するキー値サーバーに置きます。
変数に`@online`を付けてサーバーとクライアントで状態を同期する仕組みも準備中です。
