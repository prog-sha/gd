# gd

[日本語](README.ja.md) | English

A single command for writing command-line tools, websites, Web APIs, scheduled jobs, and data processing in GDScript.
It is Godot built without a display, and it runs a `.gd` file directly without a `project.godot`.

```gdscript
# hello.gd
func main():
	print("Hello, world")
	return 0
```

```sh
gd hello.gd
```

## Highlights

- Start from one script, then move on to type checking, tests, packages, databases, a Web server, and a standalone executable in the same GDScript.
- The standard API has one entry, `GD`. Pick by purpose: `GD.file`, `GD.web`, `GD.database`.
- Waiting APIs are ordinary function calls. Only the calling GDScript waits; everything else continues.
- Failures are not exceptions. Functions return `return value, failure`, and `?` passes a failure to the caller.
- The same script runs on macOS, Linux, and Windows, and links with C++ through GDExtension.
- Together with Godot itself, apps, servers, and CLI tools can all be written in GDScript. It is designed for AI agents to write and run code.

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

## Install

Supported platforms are macOS arm64/x86_64, Linux x86_64, and Windows x86_64.
Download the archive from [Releases](https://github.com/prog-sha/gd/releases/latest) and put `gd` on your PATH.
To build from source, see [Build from source](#build-from-source).

```sh
curl -fsSL https://gd.progsha.com/install.sh | sh
```

On Windows, run in PowerShell without administrator access:

```powershell
Invoke-WebRequest -UseBasicParsing https://gd.progsha.com/install.ps1 -OutFile install-gd.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-gd.ps1
```

Open a new terminal and run `gd --version`. See the [manual](docs/manual.en.md#install) for Homebrew and apt.

Use `@import` to shorten module declarations and access shared constants. See the [runnable samples](samples/README.md).

## Standard features

| Entry | Contents |
|---|---|
| `GD.file`, `GD.data` | Files, formats such as CSV, TOML, and YAML, JSON, hashes |
| `GD.http`, `GD.net` | HTTP client, TCP, UDP, TLS |
| `GD.web` | HTTP/HTTPS server, router, middleware, input validation, authentication, HTML templates |
| `GD.database` | SQLite, PostgreSQL, Redis |
| `GD.async` | Concurrency, timeouts, cancellation |
| `GD.cli`, `GD.log`, `GD.time`, `GD.text` | Flags, child processes, logs, dates, text |

Godot's Node and SceneTree are available too.

## Main commands

```text
gd script.gd [args...]          run
gd serve script.gd              keep running after main() returns
gd --watch script.gd            run again on every save
gd check [path]                 check types and syntax without running
gd fmt [--check] path           format
gd test [path]                  run *_test.gd
gd init                         write gd.json and main.gd
gd task [name]                  run a task from gd.json
gd compile -o app main.gd       build a standalone executable
gd add / install / remove       manage dependencies
gd doc [name|manual|all]        read the manual and the API
```

`gd --help` lists the remaining commands.

## Permissions

Start with normal execution. The advanced `--strict` mode denies permissions by default and lets you configure access explicitly.
Start them by listing the directories, network targets, and environment variables they use.

```sh
gd --strict \
  --mount store=/srv/app:rw \
  --allow-net=db.example.com:5432 \
  --allow-env=DATABASE_URL \
  main.gd
```

## Documentation

- [Manual](docs/manual.en.md) ([日本語](docs/manual.md)): [Quick start](docs/manual.en.md#quick-start), [Tutorial](docs/manual.en.md#tutorial-a-notes-api-on-sqlite)
- [Web manual and API reference](https://gd.progsha.com/)
- In the terminal: `gd doc`, `gd doc manual`, `gd doc GD.file`
- [Official extensions](https://gd.progsha.com/pkg/): Discord Bot, Memcached, Supabase
- [Changelog](CHANGELOG.md), [Security policy](SECURITY.md), [Contributing](CONTRIBUTING.md)

## Build from source

Install Python, SCons, and a C/C++ compiler.

```sh
git clone --branch 0.7 https://github.com/prog-sha/gd.git
cd gd
scons platform=macos target=template_release -j12
```

Use `platform=linuxbsd` on Linux. For a Windows MinGW build, use
`platform=windows arch=x86_64 use_mingw=yes windows_subsystem=console`.
Executables are written to `bin/`. This public repository contains product source and user documentation.

Run the bundled [public acceptance tests](tests/release/README.md) to check the executable:

```sh
uv run --no-project python tests/release/run.py --gd bin/gd.macos.template_release.arm64
```

## Origin and license

gd is authored by **Omochi and Yuumayay**.

gd is an MIT License project derived from [Godot Engine](https://godotengine.org/).
Godot's copyright notice and third-party library terms are in [LICENSE.txt](LICENSE.txt), [AUTHORS.md](AUTHORS.md),
and [COPYRIGHT.txt](COPYRIGHT.txt).

gd is not an official product of the Godot Foundation or the Godot Engine project.
The Godot name and logo belong to their respective owners.
