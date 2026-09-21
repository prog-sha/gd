# gd 公式マニュアル

[オンラインのマニュアル・APIリファレンス](https://gd.progsha.com/)はブラウザーの優先言語に合わせて英語・日本語を表示し、対象外の言語では英語を表示します。
言語ボタンまたは`?lang=ja`で日本語へ切り替えられ、選択は端末内に保存されます。

## gdは何のための道具か

gdは、GDScriptで端末の道具、Webサイト、Web API、定期処理、データ処理を書くための単体コマンドです。
Godotを画面なしで小さく組んであり、`project.godot`を用意せずに`.gd` fileを一枚書いて実行できます。

PythonやNode.jsでscriptを書く感覚で始められ、必要になった時点で型検査、test、package、database、
Web server、単一実行体へ同じGDScriptのまま進めます。ゲームの画面や描画を作る用途にはGodot本家を使ってください。

Godot本家とあわせれば、アプリもフロントもserverも端末ツールも、一つの言語GDScriptで書けます。
同じscriptがmacOS、Linux、Windowsで動き、通信とdatabaseの待ちはほかの処理を止めません。
GDExtensionでC++と直接つながります。AI agentが書いて動かすことを前提に設計しています。

HTTP/2の多重化とノンブロッキング通信を内蔵。UTF-8・JSON・hex処理にはSIMD/SWARを活用し、接続・バッファ・解析結果の再利用で不要な処理と割当を抑えています。

## 導入

対応環境はmacOS arm64/x86_64、Linux arm64/x86_64、Windows x86_64です。
[Releases](https://github.com/prog-sha/gd/releases/latest)からOSに合うarchiveを取得し、
中の`gd`をPATHの通ったdirectoryへ置きます。`gd --version`が版を表示すれば導入は完了です。
配布物のSHA-256は同梱の`SHA256SUMS`で照合できます。macOS版はDeveloper ID署名とAppleの公証を通しています。

### macOS / Linux

```sh
curl -fsSL https://gd.progsha.com/install.sh | sh
```

チェックサムを照合して`~/.local/bin`へ導入します。案内が出た場合は、このdirectoryをPATHへ追加してください。Linux配布バイナリにはarm64またはx86_64とglibc 2.38以降が必要です。

### Windows（PowerShell）

```powershell
Invoke-WebRequest -UseBasicParsing https://gd.progsha.com/install.ps1 -OutFile install-gd.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-gd.ps1
```

PowerShellで実行してください。管理者権限は不要です。SHA-256を照合し、`%LOCALAPPDATA%\gd\bin`へ`gd.exe`を置き、ユーザーのPATHへ追加します。新しいターミナルを開き、`gd --version`で確認します。

手動の場合はReleasesから`gd-windows-x86_64.zip`を取得して展開し、`gd.exe`を置いたdirectoryをユーザーのPATHへ追加します。PowerShellでUnix用の`curl | sh`は実行しません。

### Homebrew（macOS）

```sh
brew tap prog-sha/gd https://github.com/prog-sha/gd
brew install prog-sha/gd/gd
```

製品リポジトリをtapとして使い、署名済みUniversal実行体を導入します。更新は`brew update && brew upgrade prog-sha/gd/gd`、削除は`brew uninstall gd`です。

### apt（Linux amd64 / arm64）

Ubuntu 24.04、Debian 13など、glibc 2.38以降のapt対応環境で利用できます。Ubuntu 22.04やDebian 12ではsourceからbuildしてください。専用の署名鍵を、この配布元だけに適用します。

```sh
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://gd.progsha.com/apt/gd.asc | sudo tee /etc/apt/keyrings/gd.asc >/dev/null
sudo chmod 0644 /etc/apt/keyrings/gd.asc
echo 'deb [arch=amd64,arm64 signed-by=/etc/apt/keyrings/gd.asc] https://gd.progsha.com/apt stable main' | sudo tee /etc/apt/sources.list.d/gd.list
sudo apt update
sudo apt install gd
```

以降は通常のapt更新で新しい版を取得できます。削除は`sudo apt remove gd`です。HTTPSに加え、aptが配布メタデータの署名を検証します。

### winget（Windows）

公式カタログへの登録申請・審査は未完了です。登録までは上記のPowerShellインストーラーを使ってください。管理者向けの[申請用マニフェスト](https://github.com/prog-sha/gd/tree/0.7/packaging/winget)を用意していますが、`winget install prog-sha.gd`はまだ利用できません。

### sourceからbuild

sourceからbuildする場合はPython、uv、SCons、C/C++ compilerを用意し、`bin/`にできる`gd.*.template_release.*`を使います。
TLSは内蔵しているため、別のTLSライブラリは要りません。

```sh
git clone --branch 0.7 https://github.com/prog-sha/gd.git
cd gd
scons platform=macos target=template_release -j8
# Linux ARM64: platform=linuxbsd arch=arm64
# Linux x86-64: platform=linuxbsd arch=x86_64
# Windows: platform=windows windows_subsystem=console
```

## クイックスタート

`hello.gd`を一枚作ります。設定fileやpackageは要りません。`main()`が入口で、返した整数がprocessの終了codeになります。

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

## scriptの読み込みと共通定数

ローカルfileは`@import "./settings"`、導入済みpackageは`@import hello`。読み込んだ名前をそのまま使います。`.gd`や`as settings`は省略できます。

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

`settings.TITLE`のように使えば、値ごとの`const`宣言を利用側へコピーする必要はありません。1つのscriptでしか使わない値や処理は、そのscriptに置けば十分です。`as`は名前の衝突や読み分けが必要なときだけ使います。組み込みの`GD.web`などにはimportは不要です。

packageの導入とversion固定は[packageと配布](#packageと配布)を参照してください。

## 用途から選ぶ

標準APIの入口は`GD`一つで、用途ごとの子を持ちます。scriptからはこの名前をそのまま書きます。

| やりたいこと | 入口 | 例 |
|---|---|---|
| file、文字、日時、HTTP client、非同期処理 | `GD` | `GD.file.read_text("a.txt")` |
| WebサイトとWeb API | `GD.web` | `GD.web.app()` |
| SQLiteまたはPostgreSQL | `GD.database` | `GD.database.client()` |

`GD.database.postgres`と`GD.database.redis`は、接続先固有の機能が必要なときに使う高度な入口です。

## APIの調べ方

`gd doc`に、scriptへ書く綴りをそのまま渡します。署名は実行体から作るため、実装と一致します。

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

戻り型の`R`、`Err`、`GDWebRequest`は名前だけで引きます。Node、SceneTree、TimerなどGodot由来の型は
[Godotのclass reference](https://docs.godotengine.org/en/stable/classes/)も参照してください。
手引きの言語は`LC_ALL`または`LANG`が`ja`で始まるとき日本語、それ以外は英語です。
Web版は[gd.progsha.com](https://gd.progsha.com/)にあり、日本語と英語を切り替えられます。

## GDScriptの基本

掲載例では型名を繰り返しません。`:=`で代入する変数と、`return 値, 失敗`で返す成功値は型を推論します。
注釈を省いた引数は動的型です。型で境界を固定したい箇所だけ注釈を足せます。

### 引数とflag

script名の後ろに置いた引数は`main(argv)`で受け取ります。`--name=gd`のようにgd自身のflagと紛らわしい引数は、
`--`の後ろへ置くとscriptへ渡ります。

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

flagとして解釈したいときは`GD.cli.flags()`を使います。`--name gd`、`--name=gd`、`-name=gd`のどの綴りも受けます。

```gdscript
func main(argv):
	var flags := GD.cli.flags()
	flags.flag_str("name", "world", "挨拶する相手")
	var parsed := flags.parse(argv)
	if not parsed.ok:
		print(flags.usage())
		return 1
	print("Hello, " + flags.get_str("name"))
	return 0
```

### 外のcommandを呼ぶ

外の道具は`GD.cli.run()`で呼びます。待つのは呼び出したGDScriptだけなので、`gd serve`のhandlerの中から呼んでも他のrequestは進みます。
`--strict`では`--allow-run`が要ります。`--allow-run=/usr/bin/git`のように相手を絞れます。

```gdscript
func main():
	var got := GD.cli.run("git", ["rev-parse", "HEAD"])
	if not got.ok:
		return 1
	print("code=", got.v["code"], " out=", got.v["output"])
	return 0
```

第3引数の`opts`で挙動を変えられます。

子の非ゼロ終了はErrです。終了コードは`value.code`と`e.info.code`、出力は`value.output`で確認できます。期限や取消でも取得済み出力は残ります。

| 名前 | 既定 | 意味 |
|---|---|---|
| `timeout` | `0` | 諦めるまでの秒数。0は無期限。越えると子を畳んで`Err.TIMED_OUT`を返す |
| `output` | `true` | 出力を集める。`false`なら親の標準入出力へ直結し、集めない |

### 開発中のcommand

```sh
gd check main.gd        # Check syntax and types without execution.
gd fmt main.gd          # Format the source consistently.
gd test                 # Discover and run *_test.gd files.
gd --watch main.gd      # Restart after each source save.
gd eval 'print(1 + 1)'  # Evaluate one expression.
gd repl                 # Start an interactive session.
```

## 値と失敗

失敗しうる関数は、例外を投げる代わりに「成功値と失敗」の二つの値を返します。
受け取る側は`var 値, e :=`の形で両方を受け、`e`が`null`でなければ失敗です。

```gdscript
func main():
	var text, e := GD.file.read_text("note.txt")
	if e:
		print(e.text())
		return 1
	print(text)
	return 0
```

### 失敗を短く扱う

毎回`if e`を書く代わりに、呼出しの末尾へ`?`を付けると、失敗をそのまま呼出し元へ返して成功値だけが残ります。
`?`を使う関数は、自分も`return 値, 失敗`で成功値と失敗を返します。

```gdscript
func title(path):
	var text := GD.file.read_text(path)?
	return text.strip_edges(), null

func main():
	var text, e := title("note.txt")
	if e:
		print(e.note("題名を読む").text())
		return 1
	print(text)
	return 0
```

| 書き方 | 意味 |
|---|---|
| `var value, e := call()` | 成功値と失敗を分けて受ける |
| `return 値, null` / `return null, 失敗` | 成功または失敗を返す。成功値の型は`値`から推論する |
| `call()?` | 失敗なら呼出し元へそのまま返す |
| `call()!` | 失敗なら理由を表示して、programをその場で止める（終了codeは1）。試作やtest向き。`gd serve`ではそのhandlerだけが失敗する |
| `e.note("目的")` | 失敗に作業の文脈を足す。表示は「目的: 元の理由」の形になる |
| `e.kind` | `Err.NOT_FOUND`、`Err.INVALID_DATA`などの種類。分岐に使う |
| `Err.err("理由", Err.NOT_FOUND)` | 自分で失敗を作る |

失敗を呼出し元へ渡さない`main()`では`var 値, e :=`か`!`で受けます。

file操作の失敗では`e.info`に`op`、`path`、`source`、`source_code`が入ります。
renameは`path`の代わりに`old`と`new`を持ちます。`source`は`posix`、`win32`、`engine`のいずれかです。
NotFoundなど意味が確定した場合だけ`kind`が付き、未知のI/O失敗は`Err.NONE`のまま元情報を保ちます。

失敗しても処理済みの値が残るAPIがあります。復号、クエリ解析、子プロセス、Redisの一括実行では、`var value, e := call()`で両方を受け取ってください。元の原因は`e.cause`、分類の検査は`e.is(kind)`で辿れます。

`GD.data`のMessagePack・CBOR・TAR・TOML・YAML・XML・dotenv・front matter生成とPBKDF2は、同期・asyncとも値とErrを返します。変換できない項目を黙って省略しません。

`base64_decode(text)`と`base32_decode(text)`は標準padding付きで復号します。paddingなしは第2引数に`true`を渡します。Base32は大文字を使います。`base64url_decode(text)`は既定がpaddingなしで、付きの入力は第2引数を`false`にします。同期・asyncとも失敗時の部分byte列を保持します。

`GD.http.decode_query(raw)`はキーごとの全値をArrayで保持し、最初のErrと返します。`req.query`は先頭値、`req.query_all`は全値です。NULや不正UTF-8を含むキー・値は`PackedByteArray`のまま保持します。`encode_query()`は値のArrayを同じキーで繰り返し、バイト列も扱います。`parse_url()`の`raw_query`は元の表記です。`query`を編集してURLを組み直す場合は、先に`url.erase("raw_query")`で元の表記を外してください。

### 戻り値の規則

型を書かなくても動きます。型を書く場合と細部の規則は次の通りです。

- 戻り型は`-> int, Err`のように成功型一つと`Err`の二つです。実行時の型は`R`で、`-> R`や省略もできます。
- カンマ戻りは必ず二値です。末尾は`Err`型の値か成功時の`null`に限ります。文字列は末尾に直接返せないので`Err.err(reason)`で包みます。
- 失敗を入れる変数は`var e: Err = ...`か`var e := Err.err(...)`で型を固定します。型が変わりうる`var e = ...`は末尾に使えません。
- 複数のdataは`return [1, 0.0, ""], null`のように配列や辞書一つへまとめます。`return null, null`はnullを成功値として返します。
- `R`の分解は`var value, e := call()`の二つの名前に固定です。`var a, b, c := 1, "a", 0.0`のように式を並べる宣言は別物で、個数の制限はなく、各名前を対応する式から推論します。新しい名前を一つ以上含めば、その関数で見えている変数（外側のblockの変数を含む）にも代入でき、右辺は代入の前に全て評価します。定数、引数、lambdaが外から取り込んだ変数には代入できません。
- 型注釈した戻り値に`, Err`が無い関数で`?`を書くと`The "?" operator needs a function returning "R" or "Err".`になります。
- `-> int, Err`で`return R.ok("a")`と書くとcompile errorです。型が動的なら実行時に検査します。
- 型付きのArrayやDictionaryを成功値にするときは、元のcontainerにも同じ要素型を付けます。
- 成功値と失敗を返す関数は、全ての経路で`return`します。`?`で伝播するだけの関数も最後に`return null, null`を書きます。
- 戻り型を`-> int`のように一つだけ書いた関数には、カンマ戻りを書けません。`-> int, Err`と書きます。
- lambdaにはカンマ戻りを書けません。`return R.ok(値)`と`return R.err(理由)`を使います。

### Rで持ち運ぶ

値と失敗を一つの値として持ち運びたいときは`R`を使います。`ok`で成否、`v`で成功値、`e`で失敗を読みます。
`R.ok(値)`と`R.err(理由, 種類, 部分値)`で作ります。Webのhandlerやdatabaseのtransactionは、この`R`を返す形でも書けます。

```gdscript
func find(items, want):
	for item in items:
		if item == want:
			return R.ok(item)
	return R.err("not found: " + want, Err.NOT_FOUND)

func main():
	var got = find(["a", "b"], "c")
	if not got.ok:
		print(got.e.text())
		return 1
	print(got.v)
	return 0
```

`R.ok()`の成功値はnullで、intの0にはなりません。途中まで進むI/O APIは、失敗したときも完了した量を部分値として`v`に残します。
`note()`は部分値を保ち、`v_or(代替値)`は失敗なら代替値を返します。
`?`で伝播するとき、部分値が呼出し元の成功型に合わなければ部分値だけを捨て、失敗の理由と種類は保ちます。

### 待つ処理と同時実行

HTTP、database、`GD.net`、fileなどの待つmethodは、普通の関数呼出しとして書けます。
待つのは呼び出したGDScriptだけで、ほかの通信やtimerは進みます。

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
	for result in got:
		var res, e := result as R
		if e:
			print(e.text())
			return 1
		print(res.status)
	return 0
```

| 入口 | 用途 |
|---|---|
| `名前_async()` | 処理を始めてSignalを返す。`await`すると通常名と同じ結果になる |
| `GD.async.all(list)` | CallableとSignalを受け取り、全部の結果を入力順に返す。無効な入力は対応する欄がエラーになる |
| `GD.async.spawn(fn)` | GDScriptの関数を裏で走らせる。`main()`が返った後も動く |
| `GD.async.sleep(sec)` | 指定秒だけ待つ |

`all()`へはSignalよりCallableを渡してください。開始前に完了を購読するため、先に終わった結果を取りこぼしません。

`:=`で保存したSignalは完了時の型も保持します。異なる型や型不明のSignalへの再代入は拒否します。
実行時に型を決める場合は受け側を`Signal`と明示し、完了値にも必要な型を付けます（例: `var result: R = await pending`）。
`spawn()`はCPU処理を別threadへ移す機能ではありません。長いGDScriptは自動的にほかの処理へ実行権を譲りますが、
native methodの内部は中断しないため、大きな入力を標準moduleへ渡すときは`_async`の版を使います。
待つmethodを呼べるのは、GDScriptから呼ばれた関数の中だけです。`Array.map()`のcallback、`_init()`、member変数の初期化、`_to_string()`などのnative callbackは完了を待てません。callback内に`await`を加えても、nativeの呼出し元は結果を待ちません。非同期の初期化は`main()`へ移し、非同期の`map()`は非同期関数内の`for`、または上の`GD.async.all()`へ渡すCallableで書きます。

## チュートリアル: SQLiteを使うメモAPI

ここまでの知識で、JSONを受けてSQLiteへ保存する小さなAPIを一枚のscriptで作ります。
できあがるのは、入力検査とSQLのparameter bindを備え、権限を絞って起動する開発用serverです。

### 1. 作業directoryを作る

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
func list_notes(_req):
	var got := db.query("SELECT id, title FROM notes ORDER BY id DESC")?
	return GD.web.json(got.rows)


# Save a validated title and return the created row.
func add_note(req):
	var body := req.valid("body")
	var made := db.query(
		"INSERT INTO notes(title) VALUES($1) RETURNING id, title",
		[body.title]
	)?
	return GD.web.json(made.rows[0], 201)


# Prepare the database and routes, then listen on loopback.
func main():
	db.open({"driver": "sqlite", "path": DB_PATH})?
	db.query("CREATE TABLE IF NOT EXISTS notes(id INTEGER PRIMARY KEY, title TEXT NOT NULL)")?
	app.route("GET", "/notes", list_notes)
	app.route("POST", "/notes", add_note, [GD.web.json_body(GD.web.object_rule({
		"title": GD.web.text_rule({"min": 1, "max": 120}),
	}))])
	app.listen(PORT, "127.0.0.1")?
	print("listening on http://127.0.0.1:%d" % PORT)
	return 0, null
```

上から順に読みます。

- `app`はrouter、`db`はdatabase接続です。`main()`が返った後もserverが動き続けられるよう、両方ともscriptの変数として持ちます。
- `main()`はまずSQLiteを開き、表を作ります。`DB_PATH`の`user://`は、gdが利用者ごとに用意する書込み領域です。
- `app.route()`に、HTTP method、path、そのときに呼ぶ関数（handler）を登録します。
- handlerは`GDWebRequest`を受け取り、`GD.web.json()`で返事を作ります。途中の`?`は失敗をserverへ返し、状態番号500などになります。
- POSTには`GD.web.json_body()`を付けています。本文がruleに合うときだけhandlerが呼ばれ、通った値が`req.valid("body")`に入ります。
- SQLの値は`$1`へbindします。文字列連結でSQLを組み立てません。

### 3. 権限を絞って起動する

未確認のscriptや外へ公開するserverは、権限を既定で拒否する`--strict`で実行します。ここでは待受先をloopbackの一つのportに絞ります。
`serve`は`main()`が返った後もprocessを残すcommandで、serverにはこれを使います。

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

最初はstatus `201`と作成した一件、次は保存済みの配列が返ります。空の題名、120文字を超える題名、
JSONでない本文は`400`で拒否されます。止めるときは起動した端末でCtrl-Cを押します。

公開環境ではこのprocessをloopbackのままTLS reverse proxyの後ろへ置き、異常終了耐性が必要な保存先は
PostgreSQLへ切り替えます。接続情報はsourceへ書かず、許可した環境変数から読みます。

## 権限

最初は通常実行を使います。`--strict`は、必要な権限を自分で設計・設定する上級者向けの実行方式です。

| 方式 | 向く場面 | 制限 |
|---|---|---|
| 通常実行 | 信頼したsourceを開発中に動かす | fileもnetworkも制限しない |
| `--strict` | 未確認のscript、公開server | 起動directory配下はread-onlyで、外部の絶対pathは拒否。network、環境変数、子process、native extension、system情報を既定で拒否 |

v0.7.3のmacOS/Linuxでは、`--strict`でも起動directory外の絶対pathを読めました。

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
| `--mount name=path:r` / `--mount name=path:rw` | 名前付きdirectoryのreadまたはread/write |
| `--allow-net=host:port,...` | 接続と待受。値を省くと全て |
| `--allow-env=name,...` | 環境変数 |
| `--allow-run=command,...` | 子process |
| `--allow-ext=path,...` | scriptが実行中に読むnative extension |
| `--allow-sys=item,...` | 機種とsystem情報 |
| `--deny-*` | 対応するallowより優先する拒否 |
| `-A` | file以外を全て許す。開発中の一時的な利用向け |

### fileの置き場

scriptから見えるfileの置き場は次の4種類です。置き場の名前をpathの先頭に書くか、絶対pathをそのまま書きます。

| 書き方 | 指す場所 | strictでの扱い |
|---|---|---|
| `res://a.txt` | scriptを起動したdirectory | read-only |
| `user://a.txt` | gdが利用者ごとに用意する書込み領域 | read/write |
| `store://a.txt` | `--mount store=/srv/app:rw`で付けた名前 | 指定した権限 |
| `/etc/hosts` | 機械上のその場所 | 起動directoryの外なら拒否 |

- `res://`より上へ遡る相対pathは、どちらの方式でも拒否します。
- `--strict`の暗黙read範囲は起動時の作業directoryです。entry scriptが子directoryにあっても基準は変わりません。配下の絶対pathは`res://`と同じread-only検査を通し、親・兄弟directoryは拒否します。リンクでも外へ出られません。外部は明示的にmountし、`store://`などの名前で使います。`-A`でもfileの範囲は広がりません。
- `--mount`と絶対pathはLinuxとmacOS用です。Windowsでは拒否するので、fileは`res://`か`user://`へ置いてください。
- mount名に使えるのは小文字の英数字と`-`です。`res`、`user`、`uid`、`pipe`、`local`、`libgodot`、`tcp`、`unix`、`http`、`https`、`file`、`data`、`cache`は予約済みで選べません。

### networkとextensionの許可

- `--allow-net`の`localhost:8080`は、同じportのIPv4 loopback `127.0.0.0/8`とIPv6 `::1`も表します。
- `*.example.com:443`はその下位hostを許します。
- native extensionは同じprocessで動くため、信頼できるものに限ってください。

### serveとSceneTree

`gd serve`はSceneTreeを作らない常駐用の実行方式です。通信、timer、`await`、自作Signal、`GD.async.sleep()`、
ツリー外Nodeの`queue_free()`は動きます。仕事が無ければ次の期限か通信の通知まで眠るため、周期の調整は要りません。

| やりたいこと | 方法 |
|---|---|
| Web serverや定期処理を常駐させる | `gd serve main.gd` |
| Nodeの`_process()`、`_physics_process()`、`process_frame`、SceneTreeTimer、高水準multiplayerを使う | `serve`を付けない通常実行 |
| SceneTreeやMainLoopを継承したscriptを動かす | `serve`を付けない通常実行 |
| SceneTreeが紛れ込んでいないか開発中に調べる | `gd --no-scene-tree --allow-net serve app.gd` |
| 複数processで待ち受ける | `--workers=<n>`または`--workers=auto`。`n`は1以上の整数 |

`serve`では`Node`を継承しただけのscriptはツリーへ追加されません。
`--no-scene-tree`はSceneTreeが作られた時点で診断を出し、終了code 1で止まります。`--watch`と`--workers`の子にも引き継がれます。
通常実行は暗黙にSceneTreeを作るため、この旗を付けると失敗します。

## TCPとUDP

低水準の通信には`GD.net`を使います。Godot本家の低水準型も互換用に残っていますが、新しいcodeは`GD.net`で書きます。

```gdscript
func echo():
	var listener := GD.net.listen_tcp("127.0.0.1", 8080)?
	var conn := listener.accept()?
	var data := conn.read(65536)?
	conn.write(data)?
	conn.close()
	return 0, null
```

- `GDTCPConn`のreadとwriteは別々の列なので、複数のGDScriptから同時に呼べます。
- 期限methodは今からの秒数を設定し、0で解除します。
- `close()`は未完了のread/writeを`Err.INTERRUPTED`で起こします。listenerの受付待ちも同じです。
- 接続は名前解決で得たIPv4とIPv6の候補を順に試し、成功した一本だけを残します。全体の`timeout`は延びません。

### TLS

TLSは`GD.net.dial_tls(host, port, opts)`で開きます。既定で証明書の鎖とhost名を検証し、失敗しても平文へ戻りません。
戻り値はTCPと同じ`GDTCPConn`です。

| `opts` | 意味 |
|---|---|
| `timeout` | 接続と握手を合わせた期限の秒 |
| `ca_file` | 私設CA。環境変数の設定より優先する |
| `cert_file`、`key_file` | client認証。許可されたmount内のfileを対で指定する |
| `server_name` | 証明書を照合する宛名を接続先と別にする |
| `next_protos` | ALPN名の配列。1名は1–255 byte、全体で65535 byteまで |
| `insecure_skip_verify` | 検証を省く。検証不要と判断できる試験時だけ使う |

交渉結果は`connection_state()`の`negotiated_protocol`と`version`で読めます。TLS1.2は771、TLS1.3は772です。

信頼するCAは、未指定ならmacOSとWindowsではOSの信頼設定、Linuxではsystem CA bundleです。
起動前に`SSL_CERT_FILE`または`SSL_CERT_DIR`を設定すると、どのOSでも指定したCAを使います。
directoryの区切りはUnixで`:`、Windowsで`;`です。

serverがclient証明書を求めるときは、`app.listen_tls(port, cert, key, host, opts)`の`opts`へ`client_ca`（信頼CA bundle）と`client_auth`を渡します。
`client_ca`が未指定ならsystemの信頼設定を使います。

| `client_auth` | 動作 |
|---|---|
| `none` | 証明書を要求しない |
| `request` | 任意提示。検証しない |
| `require` | 提示だけ必須。検証しない |
| `verify_if_given` | 提示された場合だけ検証する |
| `require_and_verify` | 検証済みの証明書を必須にする |

### UDPと名前解決

`GD.net.listen_udp()`は`GDUDPPacketConn`を返します。`read_from()`は`data`、`host`、`port`、`truncated`を持つ辞書を返します。
`write_to()`のhostには`GD.net.resolve()`で解決したIP addressを渡します。packetは結合されません。
`buffer=0`（既定）はOSの受信bufferをそのまま使い、正の値を指定した場合だけ変更を要求します。

`GD.net.resolve()`はOSが選んだ先頭のaddressを一つ返します。名前のcacheは持ちません。
`GD.net.local_addresses()`は機械のaddress一覧を返し、空の一覧とOSの失敗を区別します。
失敗の`e.info`には`syscall`、`source`、`source_code`が入ります。

## fileとdata

`GD.file`でfileの読み書きとpath操作をします。起動したdirectoryが`res://`で、絶対pathも書けます。
strictで外部directoryへ書くときは`--mount store=/srv/app:rw`で付けた名前を`store://users.csv`のように書きます。

```gdscript
func main():
	var rows := GD.data.csv_objects(GD.file.read_text("store://users.csv")?)?
	GD.file.write_text("store://users.json", JSON.stringify(rows))?
	return 0, null
```

fileの操作は、通常名でも呼び出したGDScriptだけを待たせます。Web serverのhandlerから読んでも他のrequestは進みます。
複数の操作を同時に始めるときだけ、末尾が`_async`の版を使います。

```gdscript
func handler(_req):
	var body := GD.file.read_text("store://big.json")
	if not body.ok:
		return GD.web.text("読めません", 500)
	return GD.web.text(body.v)
```

`compile`で同梱したfileも、同じAPIで読取、列挙、static配信ができます。

### 大きなfileを読む

全量をmemoryへ置かず読むときは`GD.file.open(path, mode)`で`GDFileStream`を開きます。
modeは`read`、`write`、`append`、`read_write`で、使い終えたら`close()`を呼びます。

| method | 動作 |
|---|---|
| `read(max)` | 最大`max` byteを返す。少なく返ることがある。空の成功値がEOF |
| `write(bytes)` | 全て書いてbyte数を返す。途中で失敗しても`R.v`に書込み済みbyte数が残る |

同じstreamの操作は受付順、別のstreamは並列に進みます。appendはseekの後も常に末尾へ書きます。
`read_bytes()`も途中で失敗したときは取得済みのbyte列を`R.v`に残します。
`read_text()`はStringに収まらない大きさの入力を切り詰めずエラーにするので、大きなfileはbyte列かstreamで扱います。

### 同時に更新されるfile

複数のprocessが同じfileを更新するときは`GD.file.replace_text(path, old, body)`を使います。
読み取った`old`と現在の内容が同じときだけ置き換えるため、並行編集を黙って上書きしません。新規作成では`old`に`null`を渡します。

### data形式の入口

| 用途 | 入口 |
|---|---|
| CSV、TOML、YAML、JSONL、JSONC、XML、INI、TAR、front matter、`.env`のfileを読む | `GD.file.read_csv(path)`など |
| 同じ形式のmemory上の変換、JSON、codec、hash、HMAC、PBKDF2、HKDF、byte列 | `GD.data` |
| UUIDとULID | `GD.id` |
| 日時の変換と計算 | `GD.time` |
| 文字の整形と比較 | `GD.text` |
| HTML entity、tag、gdhtml（Mustache構文のマイクロテンプレート） | `GD.html` |
| flagと環境変数 | `GD.cli` |
| 配列と辞書の操作 | `GD.collection` |
| 数学の特殊値とbit演算 | `GD.math` |
| versionの比較 | `GD.version` |
| 端末とfileへのlog | `GD.log` |
| testの検査 | `GD.test` |

環境変数と`.env`は、読む対象で入口が分かれます。

| 読む対象 | 入口 |
|---|---|
| processの環境変数 | `GD.cli.env(name, fallback)`、`GD.cli.require_env(name)`。strictでは`--allow-env`が要る |
| `.env` file | `GD.file.read_env(path)`。fileを読んで辞書にする |
| dotenv形式の文字列 | `GD.data.env(src)`と`GD.data.to_env(data)`。memory上で辞書と変換する |

memory上の変換は通常名がその場で計算し、`_async`の版は別のthreadで計算します。大きな入力には`_async`を使います。
正確な一覧は`gd doc GD.file`と`gd doc GD.data`で引けます。形式ごとの検査と上限は、APIリファレンスの各入口の説明にあります。

`GD.collection`のCallableを使う操作は、約1 msごとにほかの処理へ実行権を譲ります。
`GD.log`の各呼出しは書込み完了まで待ち、本文を切り捨てません。失敗は戻り値の`R`で確認でき、`GD.log.flush()`でそれ以前の出力完了を待てます。

### JSONの規則

値は`GD.data.json_encode(value)`でJSON byte列にし、外から受けたbyte列は`GD.data.json_decode(bytes)`で読みます。
どちらも成功値と`Err`を返します。`GDWebRequest.json()`、`GDHTTPResponse.json()`、JSONLの各行も同じ規則です。

- 不正UTF-8、重複名、非有限数、非対応型、循環参照は、曖昧な値へ変えず失敗にします。
- signed 64-bitに収まる整数は`int`のまま戻り、小数、指数、範囲外だけが`float`になります。文字列とキーの`\u0000`は保持します。
- 署名やcache keyのように同じ値から同じbyte列が必要なときは`{"deterministic": true}`を指定します。
- 設定は`deterministic`と`escape_html`が`bool`、`max_bytes`と`max_depth`が`int`です。不正な型は`Err.INVALID_DATA`、上限超過は`Err.LIMITED`です。
- `json_encode_async()`が終わるまで、入力のArray・Dictionaryとその子要素を変更しないでください。設定の辞書は開始時に複製されます。

### hashと鍵導出

`GD.data`はSHA-1、SHA-224/256/384/512、SHA3-224/256/384/512を返します。HMAC、PBKDF2、HKDFでは
`sha1`、`sha224`、`sha256`、`sha384`、`sha512`、`sha3-224`、`sha3-256`、`sha3-384`、`sha3-512`から方式を選べます。
PBKDF2とHKDFは出力長を指定でき、不正な方式や表現範囲外の設定は`R`の失敗として返します。PBKDF2の反復回数が1以下なら1回として計算します。

### threadの上限

`GD.async.set_max_threads(max)`はgdが管理するOS threadの上限を設定し、以前の値を返します。既定は10000です。
上限を越えるとprocessが終了します。現在数より小さい値への変更も終了します。外部libraryが直接作るthreadは数えません。

## Webフレームワーク

同じディレクトリに`main.gd`と`index.html`を作ります。一方のrouteでHTMLテンプレートに挨拶を埋め込み、もう一方で同じデータをJSONとして返します。

`main.gd`:

```gdscript
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

`serve`は`main()`が返ったあとも待受けを続けます。終了はCtrl+Cです。

### routeと返事

`route(method, pattern, handler)`でHTTP methodとpathをhandlerへ結びます。patternの`:name`は`req.params["name"]`に入ります。
handlerは`GDWebRequest`を受け取ります。本文は`req.read()`、`bytes()`、`text()`、`json()`、`save()`で必要な分だけ読みます。
HTMLのformから届く本文は`GD.http.decode_query(req.text()?)`で辞書にします。

`req`は受信した要求です。失敗する操作は`var value, e := call()`で確認して`return null, e`を返すか、`call()?`で伝播します。`return 応答, null`は、すべての失敗を確認した成功経路だけで使います。

`GD.web.view()`と`GD.web.json()`は`応答, Err`を返します。handlerでは`return GD.web.json(data)`のように結果をそのまま返せます。テンプレートの読込み・描画やJSON変換の失敗は、元のErrを保持して`app.on_error(req, e)`へ届きます。middleware、after、自動JSON変換も同じ経路です。エラーハンドラ自身が失敗した場合は500で終了し、呼び直しません。streamの送信開始後はHTTP状態を変更できないため、writerのErrを処理します。

handlerが返した値が返事になります。

| 返した値 | 返事 |
|---|---|
| `GD.web.html(body)`、`GD.web.view(path, data)` | HTML |
| `GD.web.json(data)` | JSON |
| `GD.web.text(body)`、`GD.web.bytes(body, type)` | text、任意の媒体型 |
| `GD.web.stream(producer)` | 少しずつ書く本文。「Webの運用と高度な機能」を参照 |
| `GD.web.redirect(to)` | 302。`to`は同じsite内のpathに限り、他所へ送るときは`away`を`true`にする |
| `GD.web.not_found()` | 404 |
| 文字列 | text/plainの200 |
| `body`を持たない辞書 | JSONの200 |
| `null` | 204 |
| 失敗の`R`または`Err` | 種類に応じた状態番号。`INVALID_DATA` → 400、`UNAUTHENTICATED` → 401、`PERMISSION_DENIED` → 403、`NOT_FOUND` → 404、`LIMITED` → 429、`UNSUPPORTED` → 501、`TIMED_OUT` → 504、他は500（いずれも`Err`の定数） |

ハンドラとmiddlewareは、`await`の後も含めてSignalを返せます。完了時の引数が0個なら`null`、1個ならその値、複数ならArrayとして処理を再開します。利用できないSignalはエラーハンドラへ渡します。要求終了やapp停止時には待機中の購読を解除します。

- text/htmlは第2引数、bytesは媒体型の後の引数で状態番号を変えられます。
- `GD.web.header(reply, name, value)`で返事にheaderを足します。
- `GD.web.guard(reply)`で`X-Content-Type-Options`、`X-Frame-Options`、`Content-Security-Policy`などの防御headerをまとめて足します。
- 失敗の理由は既定では本文に出しません。開発中に`app.show_errors(true)`とした間だけ出します。
- `req.path`は各segmentを一度だけ復号したpath、`req.target`はpercent escapeとqueryを保った原文です。`%2F`は経路の区切りになりません。
- pathのsegmentを復号した結果が正しいUTF-8でない、または制御文字を含むと400を返します。queryの任意byteは`PackedByteArray`として保持するため、文字列として使う前に型を検査してください。`req.query`は壊れた組を省略します。正常な組と`Err`を両方受け取るには`GD.http.decode_query()`を使います。
- `GD.web.json()`や`view()`へ渡した値は、返事を送り終えるまで変更しないでください。

routerには次も登録できます。

| 登録 | 用途 |
|---|---|
| `app.static("/assets", "res://public")` | prefix以下のGETをdirectoryのfileで返す。媒体型は拡張子から決め、directoryの外は返さない。`/`のindexは`route`で書く |
| `app.group("/api", [middleware])` | 共通prefixとmiddlewareを持つroute group。返り値に`route()`と`use()`がある |
| `app.fallback(handler)` | どのrouteにも一致しない要求。404頁をここで返す |
| `app.on_error(handler)` | handlerが失敗を返したときの返事 |
| `app.after(handler)` | 返事を送る前の加工。`func(req, reply)`で受け、headerを足して返す |

### middleware

middlewareは、handlerの前に呼ばれる関数です。`GDWebRequest`を受け取り、`null`を返すと次へ進み、返事を返すとそこで止まります。
`handle(req)`を持つobjectも使えます。後段へ渡す値は`req.keep(name, value)`で置き、`req.kept(name)`で読みます。

| 登録 | 掛かる範囲 |
|---|---|
| `app.pre(mw)` | route選択の前。全要求 |
| `app.use(mw)` | route選択の後。全route。`req.params`を読める |
| `group.use(mw)` | そのgroupのroute |
| `app.route(method, pattern, handler, [mw])` | そのrouteだけ |

入力検査もmiddlewareです。`GD.web.json_body(rule)`、`GD.web.query(rule)`、`GD.web.params(rule)`が本文、query、pathの値を検査し、
通った値を`req.valid("body")`、`req.valid("query")`、`req.valid("params")`に入れます。
ruleは`GD.web.text_rule()`、`int_rule()`、`number_rule()`、`bool_rule()`、`list_rule()`、`object_rule()`で組み、
`GD.web.optional()`と`GD.web.one_of()`で省略と選択肢を表します。queryとparamsの値は文字列なので`text_rule()`で検査し、必要なら`to_int()`で変換します。

```gdscript
var app := GD.web.app()

func show(req):
	var params := req.valid("params")
	return GD.web.json({"id": params.id})

func main():
	app.route("GET", "/posts/:id", show, [GD.web.params(GD.web.object_rule({"id": GD.web.text_rule({"min": 1, "max": 20})}))])
	app.listen(8080)!
	return 0
```

組込みのmiddlewareは`GD.web.sessions()`、`GD.web.csrf()`、`GD.web.jwt()`、`GD.web.rate()`です。認証の節で使います。

### HTML雛形

頁が増えてきたらHTMLを雛形fileへ出し、`GD.web.view(path, data)`で描画します。
雛形はgdhtml（Mustache構文のマイクロテンプレート）で、`{{name}}`、`{{{html}}}`、`#if`、`#unless`、`#each`、
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
func page(_req):
	return GD.web.view("views/page.html", {"title": "Top"})
```

二重括弧の値は、置かれた位置から文脈を判定してescapeします。雛形の作者を信頼し、差し込む値を信頼しない前提です。

| 文脈 | 扱い |
|---|---|
| HTML本文、引用・未引用属性、属性名 | HTML escape |
| `href="{{url}}"` | 相対URLと`http`、`https`、`mailto`を通す。`data-href`も同じ |
| `href="/work/{{path}}"`、`href="/?q={{query}}"` | pathは区切りを保って正規化、queryはpercent escape |
| `onclick`、`script`本文 | JSON化し、`application/json`でも`</script>`が構造を壊さない形にする |
| `style` | 安全な単独CSS値とCSS文字列・URLを通す |
| 危険なURL、srcset、CSS値、属性名 | 画面全体を失敗させず、`#ZgdunsafeZ`または`ZgdunsafeZ`へ置き換える |

- 三重括弧`{{{html}}}`はescapeしない唯一の入口で、HTML本文以外では使えません。固定HTMLか十分に検査済みの値だけを渡してください。
- 「検査済み」の印を付けて二重括弧のescapeを省く方法はありません。
- 分岐の両側や`each`の反復が異なる文脈で終わる雛形、閉じていないtag、曖昧なURLやJavaScript文脈は描画の失敗になります。
- 雛形の大きさに固定上限はありません。再帰する部品の深さだけは100000までです。
- 描画が終わるまで、渡した辞書を変更しないでください。

同じ雛形を何度も描画するserverでは、起動時に`GD.html.template(source, partials)?`で一度だけ解析し、
返った値の`execute(data)?`を各要求から呼びます。解析結果は不変で、複数の要求から同時に使えます。
`execute_bytes(data)?`はUTF-8のbyte列を直接作るので、`GD.web.bytes(body, "text/html; charset=utf-8")`でそのまま返せます。

### 認証とCSRF

loginの状態は`GD.web.sessions()`で持ちます。`issue(value)`でsession IDを作り、`cookie(id)`の値を`Set-Cookie`で返します。
同じstoreをmiddlewareとして付けたrouteでは、cookieのIDに対応する値が`req.kept("user")`に入り、無ければ401になります。

```gdscript
var app := GD.web.app()
var sessions := GD.web.sessions()

func login(req):
	var form := GD.http.decode_query(req.text()?)?
	var user := str(form.get("user", [""])[0])
	if user.is_empty():
		return GD.web.text("user is required", 400), null
	var reply := GD.web.redirect("/me")
	return GD.web.header(reply, "Set-Cookie", sessions.cookie(sessions.issue(user)?)), null

func me(req):
	return GD.web.text("hello, " + str(req.kept("user"))), null

func main():
	app.route("POST", "/login", login)
	app.route("GET", "/me", me, [sessions])
	app.listen(8080)!
	return 0
```

`cookie(id)`は`Secure`と`HttpOnly`付きで作ります。TLSなしの開発中に届かない場合は`cookie(id, false)`にします。
logoutは`drop(id)`と`clear_cookie()`で行います。sessionはprocess内で持つため、`--workers`で複数processにするときはJWTか外部の保存先を使います。

cookieで認証する書き込み経路には`GD.web.csrf()`を付けます。GET、HEAD、OPTIONS以外はBrowserの
`Sec-Fetch-Site: same-origin`が必要です。古いBrowserやBrowser以外のclientも受ける場合に
`GD.web.csrf({"allow_missing": true})`を選び、別のtoken検証を組み合わせてください。

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

JWTをlogin sessionに使う場合は、password変更やlogoutで既発行tokenを失効させます。
`check`は署名と標準claimの検証後に呼ばれ、`true`を返したときだけ認証を通します。
例えばtokenへ利用者の`ver`を入れ、password変更時に保存済みversionを増やします。
複数workerでは各processの辞書でなく、共有DBから同期したcacheなどで照合します。

```gdscript
func token_auth(key, versions):
	return GD.web.jwt(key, {"check": func(claims):
		return versions.get(claims.get("sub", ""), -1) == claims.get("ver", -2)
	})
```

reverse proxyの後ろでIP単位に制限するときは、そのproxyのIPまたはCIDRを`trusted_proxies`へ明示します。
gdは`X-Forwarded-For`の右端から信頼済みproxyを除き、最初の未信頼IPをkeyにします。
未指定のときと未信頼の接続元からの`X-Forwarded-For`は無視するため、client自身によるIP偽装を許しません。
IPv4とIPv4-mapped IPv6は別物として照合するので、mapped addressを信頼する場合はIPv6のCIDRを指定します。zone付きのproxy設定は拒否します。

```gdscript
var per_ip := GD.web.rate({"limit": 60, "trusted_proxies": ["127.0.0.1", "172.18.0.0/16"]})
```

### 停止

終了待ちは`app.shutdown(context)`を使います。新規受付とkeep-aliveを止め、処理中requestの完了を待ちます。
期限を越えた場合は`Err.TIMED_OUT`を返しますが、処理中requestは強制終了しません。
直ちに全接続を閉じる必要がある場合に`app.stop()`を使います。

```gdscript
func close(app):
	var context := GD.async.context().with_timeout(10.0)
	var stopped := app.shutdown(context)
	if not stopped.ok:
		app.stop()
```

handlerでは`req.context`からrequestの完了と切断を受け取れます。
`with_cancel()`と`with_timeout()`は親を変更せず子のcontextを返し、親の打ち切りは子へ伝わります。
HTTP、database、processなどの待ちを打ち切れるようにするには、contextを先頭に渡して`with_context()`で包みます。
処理が先に終わればその結果を返し、contextが先に終われば処理を取り消します。

```gdscript
func load(req, db):
	var result = await GD.async.with_context(req.context, db.query_async("SELECT * FROM posts"))
	return result
```

### Webの運用と高度な機能

#### 上限と大きなupload

大きな本文や長いhandlerを扱うときは、待受前に`limits()`で上限を明示します。

```gdscript
func main():
	var limited_app := GD.web.app()
	limited_app.limits({"header_bytes": 1048576, "header_values": 500, "header_timeout": 15.0, "body_timeout": 10.0, "job_timeout": 30.0, "jobs": 128})
	return 0
```

1 GBのZIPを受ける場合は要求ごとの上限を置き、書込み可能なmountへ逐次保存します。

```gdscript
func main():
	var app := GD.web.app()
	app.limits({"body_timeout": 600.0})
	app.route("POST", "/upload", func(req):
		req.limit(1000 * 1000 * 1000)?
		req.save("uploads://package.zip")?
		return GD.web.text("saved")
	)
	return 0 if app.listen(8080, "127.0.0.1").ok else 1
```

```sh
gd --strict --allow-net=127.0.0.1:8080 --mount=uploads=/srv/uploads:rw serve main.gd
```

本文とmemoryの扱いは次の通りです。

| 対象 | 扱い |
|---|---|
| request body | 既定上限なし。見出しの後ですぐhandlerを呼び、本文はhandlerが読んだ分だけ接続から読む |
| `read()`、`save()` | 本文を逐次読む。`read()`の空の成功値はEOF。`save()`は本文全体をmemoryへ置かない |
| `bytes()`、`text()`、`json()` | 残りの本文全体をmemoryへ読む。大容量には`save()`を使う。`text()`はStringに収まる大きさまで |
| `req.limit(bytes)` | 要求ごとの本文上限。超過は本文を読んだ操作へ失敗として返る |
| request header | 既定1 MiB。行数は`header_values`を指定した場合だけ制限。trailerは4096 byte |
| HTTP clientのresponse header | 10 MiBまで |
| 遅い接続 | その接続だけを待たせ、別の接続を巻き込まない |
| 返事の追加header | 件数と全体量の固定上限なし。不正な名前と値だけを落とす |
| sessionとrate limit | process内で共有し、`--workers`間では共有しない。共有が必要ならDBなど外部の保存先を使う |
| session値 | 文字列と整数の識別子。保持件数は`total`と`per_user`で設定 |
| HS256 JWT | keyは32 byte以上。JSONと署名の妥当性を検査 |
| rate limitのkey | 保持件数は`keys`で設定 |
| HTTP状態番号 | 100..999。範囲外は500として送る |
| port | 待受と`GD.net.free_port()`の探索開始は0を許し、接続先と`is_free()`は1..65535 |
| 問い合わせ文字列 | `GD.http.decode_query()`は素のsemicolonと壊れたpercent escapeを失敗として返す |

#### 少しずつ返す本文

`GD.web.stream(producer, length=-1, type="application/octet-stream", status=200)`は、`producer(writer)`が`GDWebWriter`へ書いた分だけ送ります。
全量をmemoryに結合しません。producerの中で`await`でき、voidまたは`R`を返して終わります。

| `GDWebWriter` | 動作 |
|---|---|
| `write(data, offset=0, count=-1)` | byte列の範囲を送り、受け付けたbyte数を返す。送信が詰まっていれば進むまで待つ |
| `write_text(text, offset=0, count=-1)` | 文字列の範囲をUTF-8で送る。offsetとcountの単位は文字、結果の単位はbyte |
| `flush()` | それまでのwriteの送信完了を待つ。切断はここのエラーと`req.context`の取消でわかる |

- streamは一回限りです。応答ごとに新しく作ります。受信本文はstreamを返す前に読み終えてください。
- `length`は送るbyte数です。宣言と実際が合わないと接続を閉じます。不明長（-1）はHTTP/2でDATA frame、HTTP/1.1でchunked、HTTP/1.0で接続終了が終端になります。
- HEADと本文を持てない状態番号ではproducerを呼びません。
- 長く待つproducerでは`req.context`の取消を確認してください。
- 1回のwriteが1つのchunkに対応するとは限りません。空の文字列や空のbyte列は終端になりません。

#### HTTPSとHTTP/2

HTTPSは`app.listen_tls(8443, "cert://chain.pem", "cert://key.pem", "127.0.0.1")`で開始し、結果の`R`を確認します。
証明書のdirectoryは`--mount cert=/path/to/certs:r`で読取専用にします。PEMの鎖と暗号化されていない秘密鍵を渡します。
鍵の検証に失敗したときはportを開きません。

TLS 1.2と1.3に対応し、ALPNでHTTP/2とHTTP/1.1を選びます。HTTP/2の各streamは独立に進み、一つの取消は他のstreamを閉じません。
`header_timeout`は未完了の握手にも適用されます。client証明書の要求は「TCPとUDP」のTLSの表を参照してください。

#### gzip圧縮

`GD.data.gzip_writer(writer, level=-1)`は、書いたbyte列をgzipにして下のwriterへ渡す`GDGzipWriter`を作ります。
下のwriterには`GDFileStream`、TCP接続、`GDWebWriter`を使えます。全量をmemoryに貯めません。

| 項目 | 内容 |
|---|---|
| method | `write(bytes)`、`flush()`、`close()`、`reset(writer)`。どれも`R`を返す |
| `level` | -2（Huffmanのみ）、-1（既定）、0..9 |
| `close()` | gzipの末尾を完成する。下のwriterは閉じない |
| `reset(writer)` | エラーを消し、同じlevelで使い回す |
| `header` | `name`、`comment`（NULを含まないLatin-1）、`extra`（65535 byteまで）、`mod_time`（Unix秒）、`os`（既定255）。最初の書込みより前に設定する |

HTTPで返すときは`GD.web.header(GD.web.stream(producer), "Content-Encoding", "gzip")`を返し、producerの中で圧縮器を作って書き、`close()`の結果を返します。
`Accept-Encoding`の確認と`Vary`の設定は呼出側で行います。秘密情報と外部入力を一緒に圧縮せず、圧縮済みの本文や部分応答には使わないでください。

#### 待受addressとport

IPv6のlocalhostだけで待ち受けるには`app.listen(8080, "::1")!`を指定します。strictでは`--allow-net=[::1]:8080`、接続先は`http://[::1]:8080/`です。
`::1`と`127.0.0.1`は別の待受で、全interfaceを示す`::`とも異なります。

空きportをOSに選ばせる場合は`app.listen(0)`の直後に`app.port()`を読みます。待受けを保持したまま番号を得るので、他のprocessに取られません。
strictでは選ばれるportを事前に限定できないため、`--allow-net=127.0.0.1`のようにhost全体を許可します。
`GD.net.free_port()`と`is_free()`は診断用の瞬間的な確認で、その番号を確保する機能ではありません。

#### HTTP clientの接続

- HTTPSではHTTP/2を使い、同じ宛先への並行要求は一本の接続を共有します。非対応の相手と平文HTTPにはHTTP/1.1を使います。
- HTTP/1.1の接続は、本文を末尾まで読むと同じ宛先へ再利用します。空き接続は全体100本、宛先ごと2本、90秒まで保持します。
- 再利用した直後に閉じられた場合、安全に再送できるmethodだけ1度開き直します。
- HTTP/2では、相手が未処理と明示した要求だけを最大7回、間隔を延ばしながら再送します。要求の期限と取消は守ります。

#### Webの設定一覧

`GD.http.fetch()`と`GD.web`の各関数に辞書で渡す設定と、その既定値です。時間は秒、大きさはbyteです。

| 入口 | 設定と既定 | 意味 |
|---|---|---|
| `GD.http.fetch` | `method="GET"`, `headers={}`, `body=null` | HTTP method、送信header、送信body |
| 同上 | `timeout=30.0`, `max_body=0` | 要求全体の秒と応答bodyのbyte。0は上限なし |
| 同上 | `save=""`, `sha256=""` | 2xx bodyを`save`へ逐次保存し、返却bodyは空。`sha256`は`save`必須の64桁hexで、一致した完了fileだけを置く |
| 同上 | `authority="host:port"` | CONNECTだけのrequest target |
| `GD.cli.run` | `timeout=0.0`, `output=true` | 子processを諦める秒と、出力を集めるか |
| `GDWebApp.limits` | `jobs=0`, `job_timeout=0.0` | 保持する非同期handler数と秒。0は無制限 |
| 同上 | `header_timeout=0.0`, `body_timeout=0.0` | request header/bodyを受け終える秒。0は無期限 |
| 同上 | `header_bytes=1048576`, `header_values=2147483647` | request lineを含むheader byteと、header行数 |
| `GD.web.jwt_sign` | `ttl=900` | `iat`/`exp`を補う秒。0は自動付与しない |
| `GD.web.jwt` / `jwt_verify` | `leeway=0.0`, `require_exp=true` | 時刻許容秒と`exp`必須化 |
| 同上 | `iss=""`, `aud=""`, `keep="jwt"` | 空でない場合のissuer/audience一致と保持名 |
| 同上 | `check=Callable()` | 署名検証後にclaimを受け取る失効判定。指定時は真だけを許可 |
| `GD.web.sessions` | `total=1024`, `per_user=3` | process内の全session数と同一user数 |
| 同上 | `idle=1800`, `life=43200` | 無操作と最大生存の秒 |
| 同上 | `cookie="sid"`, `keep="user"` | Cookie名とrequest内の保持名。Cookie名はASCIIのtoken文字 |
| `GD.web.rate` | `limit=60`, `window=60.0` | keyごとの回数と固定窓の秒 |
| 同上 | `keys=10000`, `key=Callable()` | process内で保持するkey数とkey選択関数 |
| 同上 | `trusted_proxies=PackedStringArray()` | 転送元IPを信頼するproxyのIPまたはCIDR |
| `GD.web.csrf` | `allow_missing=false` | 状態変更でFetch Metadataが無いclientを許すか |
| `GD.web.text_rule` | `min=0`, `max=4096` | textの文字数 |
| `GD.web.int_rule` | `min=-9223372036854775808`, `max=9223372036854775807` | 64 bit整数の範囲 |
| `GD.web.number_rule` | `min=-1e308`, `max=1e308` | 有限浮動小数の範囲 |
| `GD.web.list_rule` | `min=0`, `max=1024` | 要素数 |
| `GD.web.object_rule` | `extra=false` | 未定義fieldを残すか |

`GDWebApp.limits`は表にある6つの設定名だけを受け、綴り違いや`body_limit`を誤りとして拒否します。

数値の設定が受け付ける範囲です。範囲外の値は設定時に失敗します。

| 設定 | 受理範囲 |
|---|---|
| `jobs` | 0..2147483647。0は無制限 |
| `header_values`, sessionの`total/per_user`, rateの`limit/keys` | 1..2147483647 |
| `job_timeout`, `header_timeout`, `body_timeout` | 有限の0..9223372036.854776秒。0は無期限 |
| sessionの`idle/life` | 1..9223372036秒 |
| `header_bytes` | 1..2147479551 byte。本文とは別 |
| `req.limit`、`GD.http.fetch.max_body` | 0..9223372036854775807 byte。`max_body`の0は上限なし |
| `ttl` | 0以上 |
| `leeway` | 有限の0以上 |

## database

`GD.database.client()`が返すclientは、SQLiteとPostgreSQLを同じ書き方で扱います。
local開発は組込みSQLite、本番はPostgreSQLという切り替えは、`open()`に渡す`driver`で行います。

```gdscript
func main():
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
	return 0, null
```

表の作成もINSERTもSELECTも`query()`一つで送ります。受け取るのは`columns`、`rows`、`tag`を持つ辞書で、
`rows`は列名を鍵にした辞書の配列です。上の例なら`out.rows[0].name`が`ada`になります。
SQLの値は`$1`、`$2`の順でbindし、両driverで同じ書き方です。SQLは変換しないため、両方で通るSQLを使います。

| method | 用途 |
|---|---|
| `query(sql, args)` | 結果を全部集めて返す |
| `query_row(sql, args)` | 先頭1行だけ返す。行が無ければ`Err.NOT_FOUND` |
| `query_rows(sql, args)` | `GDDatabaseRows`を開き、1行ずつ読む。大量の結果向き |
| `stats()` | 接続数、使用中、空き、待ち回数、待ち時間、接続を閉じた理由別の累積数 |

`query_rows()`は`while rows.next()`で進め、`scan()`で列名付きの辞書、`values()`で列順の配列を得ます。
`next()`がfalseになったら`err()`を調べます。途中で止める場合は`close()`を呼びます。

```gdscript
func list_users(db):
	var rows := db.query_rows("SELECT id, name FROM users ORDER BY id")?
	while rows.next():
		var user := rows.scan()?
		print(user.id, " ", user.name)
	if rows.err() != null:
		return R.err(rows.err())
	return R.ok()
```

制約違反では`result.e.info`に機械判定用の情報が入ります。`violation`は`duplicate`、`not_null`、`foreign_key`のいずれか、
`columns`は関係する列名です。PostgreSQLでは`code`、`table`、`constraint`もserverが返した場合に入ります。
値そのものは`info`へ残しません。SQLite自身が報告した失敗では`source="sqlite"`と拡張`source_code`を保ちます。
SQLiteのforeign key文面には列名が無いため、その場合の`columns`は空です。

```gdscript
func save(db):
	var saved := db.query(
		"INSERT INTO users(id,name) VALUES($1,$2)",
		[1, "ada"])
	if not saved.ok and saved.e.info.get("violation") == "duplicate":
		var columns := saved.e.info.get("columns", PackedStringArray())
		print("重複した列: ", columns)
```

### transactionとmigration

複数の更新を一つの成否にするときは`transaction()`を使います。callbackには同じ接続へ固定された
`GDDatabaseTx`が渡ります。callbackが成功の`R`を返すとcommitし、失敗の`R`を返すとrollbackします。

```gdscript
func save(db, id, title):
	return db.transaction(func(tx):
		tx.query("INSERT INTO posts(id,title) VALUES($1,$2)", [id, title])?
		tx.query("UPDATE counters SET value=value+1 WHERE name='posts'")?
		return R.ok(id)
	)
```

- callbackでは渡された`tx`を使い、必ず`R`を返してください。transaction中は元のclientの`query()`と二重transactionを拒否します。
- commitの失敗はそのまま失敗として返ります。
- closeや取消はCOMMITの開始前ならrollbackし、開始後なら結果が確定してから接続を閉じます。
- callbackが終わった後は、保存しておいた`tx`も新しいSQLを受け付けません。

schemaを順番に適用するときは、SQLをsemicolonで分割せず、statementの配列を`migrate()`へ渡します。
途中の一文が失敗すると全体をrollbackし、成功時は適用した文の数を返します。
versionとchecksumはapplication側で管理します。

```gdscript
func migrate(db):
	return db.migrate([
		"CREATE TABLE posts(id INTEGER PRIMARY KEY, title TEXT NOT NULL)",
		"CREATE INDEX posts_title ON posts(title)",
	])
```

### databaseの高度な機能

#### driverの違い

| 項目 | SQLite | PostgreSQL |
|---|---|---|
| 向く用途 | local開発、単一process | 本番、異常終了耐性、複数worker |
| 接続 | clientごとに一つ。journalと一時表はmemoryに置く | 既定`max(4, CPU数)`までのpool。`pool=25`のように最大数を指定できる |
| 追加の入口 | 短い処理をその場で行う`GD.database.sqlite.open()` | まとめ送り、配列、JSONBを使う`GD.database.postgres` |
| 注意 | 既存の`-journal`、`-wal`、`-shm`があるdatabaseは、通常のSQLiteで回復またはcheckpointしてから開く | loopback以外のhostではTLS証明書とhost名を既定で検証。loopbackはTLSなしが既定 |

`GD.database.postgres.client()`と`GD.database.redis.client()`の`open()`は`open(host, port, opts)`の形で、接続先を引数に取ります。

#### SQLiteの並行

同じclientの`query()`は受付順に実行します。別のclientは並行に進みますが、同じdatabase fileへの書込みはSQLiteのlockに従います。
`GD.database.sqlite.open()`が返す`GDSQLiteDB`と`GDSQLiteStatement`は、呼出し元でそのまま実行する同期APIです。
短い処理だけに使い、同時利用はしないでください。並行処理には`GDDatabaseClient`を使います。

#### PostgreSQLの接続と型

- poolは最初の問い合わせまで接続を作らず、需要の分だけ最大数まで増やします。上限に達した後の問い合わせは受付順に待ちます。
- 通常の`query()`は使用中の接続へも続けて送ります（pipeline）。同じ接続では送った順に結果が返ります。
- transactionと`query_rows()`は接続を一本専有します。接続固有の状態を使う処理は、`BEGIN`を単発で送らずtransaction APIを使ってください。
- 同じ接続へ複数のSQLをまとめて送るときは`query_many`、`fetch_many`、`exec_many`を使います。
- 取消と期限超過は呼出し元へすぐ通知しますが、server上のSQL停止までは保証しません。他のqueryは中断しません。
- `stats()`の`wait_count`は接続の取得待ちの回数で、pipeline内の応答待ちは含みません。
- 認証はSCRAM-SHA-256とMD5をserverの要求に合わせます。`auth="scram"`または`auth="md5"`で固定できます。MD5は旧server用です。平文passwordは明示した許可が要ります。
- JSON・JSONB列は「fileとdata」のJSONと同じ規則で読み、64-bit整数を保ちます。重複名など曖昧な値は元のJSON文字列を返します。
- `bool[]`、`int[]`、`bigint[]`、`text[]`は要素の型、null、多次元構造を保ちます。下限を明示した配列は元の文字列を返します。
- 接続はUTF8を指定します。serverが別のclient encodingへの変更を通知した場合はエラーで接続を閉じます。変更のSQL自体は実行済みの場合があります。

#### Redisの接続

`GET`の値なしは`Err.NOT_FOUND`と`e.info.code == "nil"`で返します。`MGET`の配列内nullはそのままです。`pipeline()`は1件でもArrayを返し、通信失敗時も取得済みの返答を残して未受信位置にErrを置きます。WATCH中断は`Err.INTERRUPTED`と`e.info.code == "tx_failed"`で識別できます。

- TLSの選び方はPostgreSQLと同じです。`open()`の`timeout`で接続と応答の期限を秒指定できます。
- poolの`open()`は接続先を設定するだけで、通信は最初の`query()`から始まります。
- 使用中の接続は返却まで専有し、空きが無ければ受付順に待ちます。待機の取消は他の呼出しへ影響せず、実行中の取消はその接続を閉じます。
- `size()`は確立中を含む接続数、`in_flight()`は取得待ちを含む未完了数です。

#### databaseの設定一覧

`open()`に辞書で渡す設定と、その既定値です。

| 入口 | 設定と既定 | 意味 |
|---|---|---|
| `GDDatabaseClient.open` | `driver="postgres"`, `path=""` | driverとSQLite path。SQLite時は`user://...`または`:memory:`が必要 |
| 同上 | `host="127.0.0.1"`, `port=5432` | PostgreSQLの接続先 |
| 同上 | `pool=0` | PostgreSQL最大接続数。0は`max(4, CPU数)`、SQLiteでは使わない |
| 同上 | `max_rows=0`, `max_bytes=0` | `query()`が集める1結果の行数とbyte。0は無制限。`query_rows()`には適用しない |
| `GDPostgresClient.open` | `user="postgres"`, `database="postgres"`, `password=""` | 認証とDB名 |
| 同上 | `connect_timeout=15.0`, `timeout=0.0` | 接続と問い合わせの秒。poolの接続待ちも問い合わせ時間に含む。0は無期限 |
| 同上 | `auth="any"`, `allow_cleartext_password=false` | `auth="scram"`/`"md5"`で方式固定。平文password応答は明示時のみ |
| 同上 | `tls=<hostで決定>`, `ca=""` | 外部hostは`verify-full`、loopbackは`disable`。CA fileは明示時だけ |
| `GD.database.sqlite.open` | `busy_ms=5000`, `max_ms=0` | lock待ちミリ秒と実行期限ミリ秒。0は無期限 |
| 同上 | `max_rows=0`, `max_bytes=0` | 1結果の行数とbyte。0は無制限 |
| `GDRedisClient.open` | `password=""`, `timeout=10.0` | passwordと接続・応答期限の秒。0は無期限 |
| 同上 | `tls=<hostで決定>`, `ca=""` | PostgreSQLと同じTLS選択 |
| `GD.database.postgres.pool` | size既定0、0または1..2147483647 | 0は`max(4, CPU数)` |
| `GD.database.redis.pool` | size既定0、0..2147483647 | 最大接続数。0は無制限。同時に作る接続はCPU数の10倍まで、最大数の指定時はその数まで |
| `GDRedisPool.open` | `pool_timeout=timeout+1.0`（timeoutが0なら30秒） | 接続の空きを待つ期限。明示0は無期限 |

`max_rows`または`max_bytes`を越えた`query()`は、その問い合わせだけを失敗にします。
期限切れや壊れた応答で順序を失った場合は接続全体を閉じます。

| 設定 | 受理範囲 |
|---|---|
| `max_rows`, `max_bytes`, `busy_ms`, `max_ms` | 0..2147483647 |
| bind値 | PostgreSQLは65535個、SQLiteはengineの変数上限まで。`query_many`の件数に固定上限はない |
| PostgreSQLの1送信 | SQLとbind文字列をUTF-8のbyteで数え、約1 GiBまで |
| Redisの1送信 | server側の設定に従う |
| PostgreSQLとRedisのport | 1..65535 |
| 秒指定 | 有限の0..9223372036.854776秒。0は無期限 |

## 定期処理

決まった時刻に一度だけ動かす仕事は、普通のscriptとして書き、OSのcronやsystemd timerから呼びます。
gd側に常駐の仕組みは要りません。

```gdscript
func collect():
	var now := GD.time.to_iso(GD.time.now())
	GD.file.append_text("store://log.txt", now + "\n")?
	return 0, null

func main():
	collect()?
	return 0, null
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

止めるときはprocessを終わらせます。Web serverと同じ常駐なので、ここでも`serve`が必要です。

## 公式拡張モジュール

本体を小さく保ち、外部service固有の機能は必要なprojectだけへGDScript packageまたはGDExtensionとして加えます。

| 入口 | 用途 | APIと導入方法 |
|---|---|---|
| `Discord` | DiscordのGatewayとRESTを使う純GDScript文字Bot | [Discord Bot](https://gd.progsha.com/pkg/) |
| `GDMemcached` | TCP接続を再利用するcache client | [Memcached](https://gd.progsha.com/pkg/) |
| `GDSupabase` | DatabaseとAuthのclient | [Supabase](https://gd.progsha.com/pkg/) |

各文書に公開class、method、戻り値、制限値、strict実行例をまとめています。任意導入のため、
本体だけから生成するAPIリファレンスには含まれません。

- `gd add`で入れた拡張は起動時に信頼して読み込むため、旗は要りません。接続先の`--allow-net`は必要です。
- `--allow-ext`と`--deny-ext`が効くのは、scriptが実行中に`GDExtensionManager.load_extension()`で読む場合です。
- 入れた拡張はprocessと同じ権限で動くので、信頼する版を`gd.lock`で固定してcommitしてください。

## packageと配布

scriptが増えたり他のpackageを使ったりする段階で、`gd init`で`gd.json`を作ります。依存は`gd.json`と`gd.lock`で固定します。

```sh
gd init
gd search discord bot
gd add gd:@scope/script-package@^1.0.0
gd add ext:@scope/name@^1.0.0
gd add short-name https://example.com/module.gd
gd install --frozen
gd task test
```

### packageを使う

入れたpackageは、利用側が決めた呼び名を使って`pkg://<呼び名>/`から読みます。

```gdscript
@import hello
```

- `pkg://`は利用者ごとの共有cacheを指し、projectへは何も複製しません。
- `gd.json`に書いた依存がcacheに無ければ、最初の実行で取得します。`--strict`では登録所への`--allow-net`が要ります。
- `gd add`の既定の呼び名は、package名の`-`と`.`を`_`にした識別子です。engine classやkeywordと同じ呼び名は断ります。
- commitするのは`gd.json`と`gd.lock`です。`gd init`は`pkg/`を`.gitignore`へ書きます。
- `--frozen`はlockを変更しません。offlineの配布先では、networkのある環境で先に取得し、`--cached-only`を併用します。
- install、add、updateが途中で失敗したときは、projectの配置とlockを元へ戻します。
- lockは登録所に結び付いています。別の登録所へ切り替えるには明示的なlock移行が必要です。

### importの短い書き方

`@import`は`const 名 = preload(...)`の短い書き方です。

外部依存がなくても使えます。実際の登録所の依存とlockfileを含む[samples/packages](https://github.com/prog-sha/gd/tree/0.7/samples/packages)を用意しています。

```gdscript
@import greet
@import "./util"
@import greet/style as theme
```

- 引用符の無い名前は、`gd.json`の`imports`に宣言した呼び名だけを解決します。同名のfileを探しに行きません。
- 相対fileは引用符で`./`または`../`から書きます。 `.gd`は省略できます。同名のfileとdirectoryが両方あるときは`"./util.gd"`のように指定します。
- 識別子は`as`が無ければ最後の要素そのままで、`mod.gd`を持つdirectoryはdirectory名です。
- `gd fmt`は`@import`をそのまま残します。
- 本家Godotは`@import`を知らないので、Godotと共有するfileでは`const`と`preload`を書いてください。

### packageを作る

packageは`gd.json`を根に持つ一つのprojectです。`gd init @scope/name`が`mod.gd`とtestの雛形を作り、
`gd test`で回し、`gd publish`で公開します。

```json
{"name":"@scope/hello","version":"1.0.0","main":"src/mod.gd","include":["src"]}
```

```sh
gd publish
gd add hello gd:@scope/hello@^1.0.0
```

- 入口は`mod.gd`です。複数fileなら`include`へfileまたはdirectoryを明示します。
- mainのdirectoryがpackageの根になるので、package内の相対preloadはそのまま動きます。
- packageは自分の`gd.json`の`imports`で他の登録所packageを使えます。`gd publish`がその`imports`を登録所へ載せます。
- `class_name`は公開できます。installは同名classの衝突を検査し、衝突すれば全体を元へ戻します。
- `gd.json`の`godot`を`true`にすると、gd固有のAPIを使わず本家Godotでも動くという作者の宣言になり、`gd search`が`[godot]`と示します。

開発中のpackageは`gd add ../path`でlocalから足します。呼び名は先の`gd.json`の`name`から取ります。
checkoutを`pkg/<呼び名>/`へ複製し、内容の指紋が変われば次の実行で複製し直します。
`.`で始まるfile、`pkg/`、`tmp/`、`gd.json`を持つ下位directory、`token`は複製しません。
`gd publish`は、local importの先に`name`と`version`のある`gd.json`があれば登録所の範囲に変換し、無ければ拒みます。

### 依存の解決

`gd install`は依存graph全体を解決します。版は、`gd.lock`が固定した版、今回すでに選んだ版のうち範囲を満たすもの、
登録所の最新一致の順で選びます。

`gd.lock`は解決した`imports`の設定も保持します。設定が変わった実行では同じresolverで再解決し、要求外の古い版を使いません。
`--frozen`は設定の不一致を拒否します。同じpackageに複数の別名がある場合、辞書順で最初の別名を配置先に使います。

- 純GDScript packageは、版ごとに別のものとして共存できます。
- native拡張はprocessに一つしか読めないため、一つの版に揃えます。範囲が両立しなければ取得前に止まります。
- 同じhost instanceを共有するpluginの仕組み（peer依存）はありません。
- 登録所packageの正式なpathは`pkg://@scope/name@版/`です。`pkg://<呼び名>/`は、書いたscriptが属するpackageの`imports`で正式pathへ展開されます。同じ呼び名でもpackageごとに違う版を指せ、同じ版はどこから辿っても一つのscriptです。
- `gd.lock`には各packageの`imports`の解決先も記録され、`gd info`が一覧します。
- `gd remove`と`gd update`は、どのpackageも使わなくなったものを`gd.lock`と`pkg/`から外します。
- 検索の順位が変わっても、既知のpackageのinstallとlockの検証には影響しません。

### Godotと共有する置き場

本家Godotなど`res://`しか読めない環境と共有するときは、`gd.json`へ`"place": "project"`を書きます。
packageを`pkg/<呼び名>/`へ複製し、`pkg://`も`res://pkg/`もそこを指します。
他のpackageだけが使うものは`pkg/@scope/name@版/`へ置きます。

- `project.godot`のあるdirectoryでは`place`の既定が`project`になり、`.gitignore`は書きません。gdの無い同僚が開けるよう`pkg/`をcommitします。
- installは`preload`、`load`、`extends`に書かれた`res://`参照を配置先へ書き換えます。文字列、コメント、実行時に組み立てるpathは書き換えません。
- `place`はfileの置き場を決めるだけで、gd固有のAPIや構文をGodot向けに変換する機能ではありません。共有するsourceは標準構文と相対preloadで書きます。

### native拡張のpackage

- native拡張は読込みに実fileが要るため、`place`に関わらず`pkg/<呼び名>/`へ置きます。
- scriptから名指せるのは、自分のpackageが`ext:`で取り込んだ拡張のclassだけです。projectのscriptなら`gd.json`、packageのscriptならそのpackageの`imports`が基準です。
- 登録所の外にある拡張はprojectのscriptだけが使えます。
- 登録所のpackageの拡張が、manifestの`[classes]`に無いclassを登録すると起動時に止まります。
- 配布先のOSで取得するか、`gd compile`を配布先のOSで実行してください。

### 設定と環境変数

`gd.json`の設定は次の10件です。

| 名前 | `gd init`の生成値 / 未指定時 | 意味 |
|---|---|---|
| `name` | `my-tool` / 必須 | project名。publishは`@scope/name`が必要 |
| `version` | `0.1.0` / 必須 | packageのversion |
| `tasks` | run/testの2件 / 無し | `gd task`から呼ぶcommand |
| `imports` | `{}` / `{}` | 呼び名と依存先。publishするpackageでは登録所packageだけ |
| `registry` | 未指定 / 環境または公開登録所 | project固定の登録所URL |
| `main` | 未指定 / `mod.gd` | publishする`mod.gd`または`.gdextension`入口 |
| `include` | 未指定 / mainだけ | 純GDScript packageへ含めるmain directory内のfileまたはdirectory |
| `place` | 未指定 / `cache`（`project.godot`があれば`project`） | packageの置き場。`project`で`pkg/`へ複製する |
| `godot` | 未指定 / `false` | gd固有のAPIを使わず本家Godotでも動くpackageの宣言 |
| `description` | 未指定 / 空 | 登録所に出す説明 |

gdが読む環境変数は次の通りです。scriptから環境を読む実行では`--allow-env`で名前を許可します。

| 環境変数 | 用途 |
|---|---|
| `GD_CACHE_HOME` | packageのcache根。未指定はWindowsのLocalAppData内`gd`。macOS/Linuxは絶対pathの`XDG_CACHE_HOME/gd`、それがなければhome内`.gd`。`HOME`未設定時はOSの利用者情報を使う |
| `GD_REGISTRY` | 登録所。未指定は`https://gd.progsha.com/pkg`。`gd.json`の`registry`が優先 |
| `GD_TOKEN` | publishのtoken。設定fileへ書かず、publishするprocessだけへ渡す |
| `LC_ALL`、`LANG` | `gd doc`の手引きの言語 |
| `GD_WORKER` | `--workers`が作る内部印。利用者が設定する値ではない |

遠隔packageと登録所はHTTPSを使います。loopbackの開発用登録所に限りHTTPも使えます。
取得したpackageとnative libraryは登録所索引のSHA-256と照合します。
`.gdextension` manifestは16 MiB、packageの全file合計は500 MiBまでです。

### 単一実行体で配布する

`compile`で、script、view、静的file、migration、依存package、対象OSのGDExtensionを一つの実行体へまとめます。配布先にcacheは要りません。

```sh
gd compile -o app main.gd
./app
```

- `gd.json`が名指すpackageと、それらが取り込むpackageを全部同梱します。
- 同梱したWebアプリも`./app serve --no-scene-tree --allow-net`で常駐できます。
- local pathのpackageからは、`.env`など`.`で始まるfileと`gd.json`の`token`を除きます。
- secretをsourceへ埋め込まないでください。compileは`.env`を除外しますが、sourceに書いた値は実行体へ残ります。

## 対応範囲と報告

gdはAPIが固まる前の公開版です。後方互換は前提にしないでください。変更した点と基準にしたGodotの版は
[CHANGELOG](https://github.com/prog-sha/gd/blob/0.7/CHANGELOG.md)に書きます。
gdはGodot FoundationまたはGodot Engine projectの公式製品ではありません。

不具合は[Issues](https://github.com/prog-sha/gd/issues)へ、公開すべきでない脆弱性は
[GitHubの非公開報告](https://github.com/prog-sha/gd/security/advisories/new)から知らせてください。
