# gd

日本語 | [English](README.md)

GDScriptで端末の道具、Webサイト、Web API、定期処理、データ処理を書くための単体コマンドです。
Godotを画面なしで組んであり、`project.godot`なしで`.gd` fileを直接実行します。

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

- 一枚のscriptから始め、型検査、test、package、database、Web server、単一実行体へ同じGDScriptのまま進めます。
- 標準APIの入口は`GD`一つです。`GD.file`、`GD.web`、`GD.database`のように用途で選びます。
- 待つAPIも普通の関数呼出しで書けます。待つのは呼び出したGDScriptだけで、ほかの処理は進みます。
- 失敗は例外でなく`return 値, 失敗`の二値で返し、`?`で呼出し元へ渡せます。
- macOS、Linux、Windowsで同じscriptが動き、GDExtensionでC++とつながります。
- Godot本家とあわせれば、アプリもserverも端末ツールもGDScript一つで書けます。AI agentが書いて動かすことを前提に設計しています。

```gdscript
var app := GD.web.app()

func home(_req):
	return GD.web.html("<h1>gd</h1>"), null

func hello(req):
	return GD.web.json({"message": "hello", "ip": req.ip})

func main():
	app.route("GET", "/", home)
	app.route("GET", "/api/hello", hello)
	app.listen(8080, "127.0.0.1")!
	return 0
```

```sh
gd serve main.gd
```

## 導入

対応環境はmacOS arm64/x86_64、Linux x86_64、Windows x86_64です。
[Releases](https://github.com/prog-sha/gd/releases/latest)からarchiveを取得し、`gd`をPATHの通ったdirectoryへ置いてください。
sourceからのbuildは[ソースからのビルド](#ソースからのビルド)を参照してください。

```sh
curl -fsSL https://gd.progsha.com/install.sh | sh
```

WindowsはPowerShellで実行します。管理者権限は不要です。

```powershell
Invoke-WebRequest -UseBasicParsing https://gd.progsha.com/install.ps1 -OutFile install-gd.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-gd.ps1
```

新しいターミナルで`gd --version`を確認してください。Homebrew・aptの導入方法は[マニュアル](docs/manual.md#導入)を参照してください。

`@import`でモジュールの読み込みと共通定数の利用を短く書けます。[実行できるサンプル](samples/README.md)を参照してください。

## 標準機能

| 入口 | 内容 |
|---|---|
| `GD.file`、`GD.data` | file、CSV・TOML・YAMLなどの形式、JSON、hash |
| `GD.http`、`GD.net` | HTTP client、TCP、UDP、TLS |
| `GD.web` | HTTP/HTTPS server、router、middleware、入力検査、認証、HTML雛形 |
| `GD.database` | SQLite、PostgreSQL、Redis |
| `GD.async` | 同時実行、timeout、取消 |
| `GD.cli`、`GD.log`、`GD.time`、`GD.text` | flag、子process、log、日時、文字 |

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

`gd --help`で残りのcommandを一覧できます。

## 権限

最初は通常実行を使います。権限を細かく設定する上級者向けに、既定拒否の`--strict`があります。
使うdirectory、接続先、環境変数を挙げて起動します。

```sh
gd --strict \
  --mount store=/srv/app:rw \
  --allow-net=db.example.com:5432 \
  --allow-env=DATABASE_URL \
  main.gd
```

## 文書

- [公式マニュアル](docs/manual.md)（[English](docs/manual.en.md)）: [クイックスタート](docs/manual.md#クイックスタート)、[チュートリアル](docs/manual.md#チュートリアル-sqliteを使うメモapi)
- [Web版マニュアル・APIリファレンス](https://gd.progsha.com/)
- 端末では`gd doc`、`gd doc manual`、`gd doc GD.file`で読めます
- [公式拡張](https://gd.progsha.com/pkg/): Discord Bot、Memcached、Supabase
- [変更点](CHANGELOG.md)、[安全な利用と脆弱性報告](SECURITY.md)、[貢献方法](CONTRIBUTING.md)

## ソースからのビルド

Python、SCons、C/C++ compilerを用意してください。

```sh
git clone --branch 0.7 https://github.com/prog-sha/gd.git
cd gd
scons platform=macos target=template_release -j12
```

Linuxは`platform=linuxbsd`、WindowsのMinGWビルドは`platform=windows arch=x86_64 use_mingw=yes windows_subsystem=console`です。
実行体は`bin/`へ生成されます。この公開リポジトリには製品ソースと利用文書を収録しています。

同梱の[公開契約テスト](tests/release/README.md)で動作を確認できます。

```sh
uv run --no-project python tests/release/run.py --gd bin/gd.macos.template_release.arm64
```

## 来歴とライセンス

gdの著作者は **Omochi, Yuumayay** です。

gdは[Godot Engine](https://godotengine.org/)から派生したMIT Licenseのprojectです。
Godotの著作権表示と第三者libraryの条件は[LICENSE.txt](LICENSE.txt)、[AUTHORS.md](AUTHORS.md)、
[COPYRIGHT.txt](COPYRIGHT.txt)にあります。

gdはGodot FoundationまたはGodot Engine projectの公式製品ではありません。
Godotの名称とlogoは各権利者に帰属します。
