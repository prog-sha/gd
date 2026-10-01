# gd

[English](README.md) | 日本語

GDScriptでCLIツール、Webサイト、Web API、定期処理、データ処理を書くための単体コマンドです。
Godotを画面なしで組んであり、`project.godot`なしで`.gd` ファイルを直接実行します。

```gdscript
# hello.gd
func main():
	print("Hello, world")
	return 0
```

```sh
gd hello.gd
```

## 特徴

- 1つのスクリプトから始められます。型検査やテストを加え、パッケージ、データベース、Webサーバーも使えます。完成したプログラムは1つの実行ファイルにまとめて配布できます。
- 標準APIの入口は`GD`一つです。`GD.file`、`GD.web`、`GD.database`のように用途で選びます。
- 待つAPIも普通の関数呼び出しで書けます。待つのは呼び出したGDScriptだけで、ほかの処理は進みます。
- 失敗は例外でなく`return 値, 失敗`の二値で返し、`?`で呼び出し元へ渡せます。
- macOS、Linux、Windowsで同じスクリプトが動き、GDExtensionでC++とつながります。
- Godot本家とあわせれば、アプリもサーバーもCLIツールもGDScript一つで書けます。

```gdscript
var app := GD.web.app()

func home(_req: GDWebRequest) -> GDWebResponse, Err:
	return GD.web.html("<h1>gd</h1>")

func hello(req: GDWebRequest) -> GDWebResponse, Err:
	return GD.web.json({"message": "hello", "ip": req.ip})

func main() -> int:
	app.route("GET", "/", home)
	app.route("GET", "/api/hello", hello)
	app.listen(8080, "127.0.0.1")!
	return 0
```

```sh
gd serve main.gd
```

値と`Err`は別々の変数で受け取ります。`Err`が`null`なら成功です。失敗時に途中までの値が返る場合もあるので、値の有無だけで判断しません。`message.txt`を用意し、次を`read.gd`に保存して`gd read.gd`で実行できます。

```gdscript
# Read a file and handle its failure separately from its contents.
func main():
	var text, err := GD.file.read_text("message.txt")
	if err:
		printerr(err)
		return 1
	print(text)
	return 0
```

## 導入

対応環境はmacOS arm64/x86_64、Linux arm64/x86_64、Windows x86_64です。
[Releases](https://github.com/prog-sha/gd/releases/latest)からアーカイブを取得し、`gd`をPATHの通ったディレクトリへ置いてください。
ソースからのビルドは[ソースからのビルド](#ソースからのビルド)を参照してください。

```sh
curl -fsSL https://gd.progsha.com/install.sh | sh
```

WindowsはPowerShellで実行します。管理者権限は不要です。

```powershell
Invoke-WebRequest -UseBasicParsing https://gd.progsha.com/install.ps1 -OutFile install-gd.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-gd.ps1
```

新しいターミナルで`gd --version`を確認してください。Homebrew・aptの導入方法は[マニュアル](docs/manual.md#導入)を参照してください。

`@import`でモジュールの読み込みと共通定数の利用を短く書けます。[実行できるサンプル](samples/README.ja.md)を参照してください。

## 標準機能

| 入口 | 内容 |
|---|---|
| `GD.file`、`GD.data` | ファイル、CSV・TOML・YAMLなどの形式、JSON、ハッシュ |
| `GD.http`、`GD.net` | HTTP クライアント、TCP、UDP、TLS |
| `GD.mail` | メールの解析、SMTP送信、MIME |
| `GD.web` | HTTP/HTTPS サーバー、ルーター、ミドルウェア、入力検査、認証、HTMLテンプレート |
| `GD.database` | SQLite、PostgreSQL、Redis |
| `GD.async` | 同時実行、タイムアウト、キャンセル |
| `GD.cli`、`GD.log`、`GD.time`、`GD.text` | オプション、子プロセス、ログ、日時、文字 |

GodotのNodeとSceneTreeも使えます。

## 主なコマンド

```text
gd script.gd [args...]          実行
gd serve script.gd              main()が返っても常駐
gd --watch script.gd            保存のたびに実行し直す
gd check [path]                 実行せずに型と構文を検査
gd fmt [--check] path           整形
gd test [path]                  *_test.gdを実行
gd init                         gd.jsonとmain.gdを作る
gd task [name]                  gd.jsonのtaskを実行
gd compile -o app main.gd       単一実行体を作る
gd add / install / remove       依存を管理
gd doc [name|manual|all]        手引きとAPIを表示
```

`gd help`で残りのコマンドを一覧できます。

## 権限

最初は通常実行を使います。権限を細かく設定する上級者向けに、既定拒否の`--strict`があります。
使うディレクトリ、接続先、環境変数を挙げて起動します。

```sh
gd --strict \
  --mount store=/srv/app:rw \
  --allow-net=db.example.com:5432 \
  --allow-env=DATABASE_URL \
  main.gd
```

## 文書

- [公式マニュアル](docs/manual.md)（[English](docs/manual.en.md)）: [クイックスタート](docs/manual.md#クイックスタート)、[チュートリアル](docs/manual.md#チュートリアル-sqliteを使うメモapi)
- [gd-godot マニュアル](docs/gd-godot.md)（[English](docs/gd-godot.en.md)）: エディタ、企画、オンライン機能（開発中）
- [APIリファレンス・チュートリアル](https://gd.progsha.com/docs/ja/index.html)
- ターミナルでは`gd doc`、`gd doc manual`、`gd doc GD.file`で読めます
- [公式拡張](https://gd.progsha.com/pkg/): Discord Bot、Supabase
- [変更点](CHANGELOG.ja.md)、[安全な利用と脆弱性報告](SECURITY.ja.md)、[貢献方法](CONTRIBUTING.ja.md)、[行動規範](CODE_OF_CONDUCT.ja.md)

## ソースからのビルド

Python、SCons、C/C++ コンパイラーを用意してください。

```sh
git clone --branch master https://github.com/prog-sha/gd.git
cd gd
scons platform=macos target=template_release -j12
```

Linuxは`platform=linuxbsd arch=arm64`または`arch=x86_64`、WindowsのMinGWビルドは`platform=windows arch=x86_64 use_mingw=yes windows_subsystem=console`です。
実行ファイルは`bin/`に生成されます。この公開リポジトリには製品ソースと利用文書を収録しています。

同梱の[動作確認テスト](tests/README.ja.md)で動作を確認できます。

```sh
uv run --no-project python tests/run.py --gd bin/gd.macos.template_release.arm64
# Linux ARM64: --gd bin/gd.linuxbsd.template_release.arm64
```

## 来歴とライセンス

gdの著作者は **Omochi, Yuumayay** です。

gdは[Godot Engine](https://godotengine.org/)から派生したMIT Licenseのプロジェクトです。
Godotの著作権表示と第三者ライブラリの条件は[LICENSE.txt](LICENSE.txt)、[AUTHORS.md](AUTHORS.md)、
[COPYRIGHT.txt](COPYRIGHT.txt)にあります。

gdはGodot FoundationまたはGodot Engine プロジェクトの公式製品ではありません。
Godotの名称とロゴは各権利者に帰属します。
