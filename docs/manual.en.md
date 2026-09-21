# gd Manual

The [online manual and API reference](https://gd.progsha.com/) uses your browser’s preferred English or Japanese language, with English as the fallback.
Use the language button or `?lang=ja` for Japanese; an explicit selection is remembered locally.

## What gd is for

gd is a single command for writing command-line tools, websites, Web APIs, scheduled jobs, and data processing in GDScript.
It is Godot built small without a display, and it runs a single `.gd` file without a `project.godot`.

You start the way you would with a Python or Node.js script. When you need them, type checking, tests, packages, databases,
a Web server, and a standalone executable are available in the same GDScript. For game screens and rendering, use upstream Godot.

Together with Godot itself, apps, frontends, servers, and CLI tools can all be written in one language, GDScript.
The same script runs on macOS, Linux, and Windows, and waits on networking and databases do not stop other work.
It links directly with C++ through GDExtension. It is designed for AI agents to write and run code.

Built-in HTTP/2 multiplexing and nonblocking networking keep concurrent connections moving. SIMD/SWAR paths accelerate UTF-8, JSON, and hex processing, while connection, buffer, and parsed-template reuse reduce repeated work and allocations.

## Install

Supported platforms are macOS arm64/x86_64, Linux arm64/x86_64, and Windows x86_64.
Download the archive for your OS from [Releases](https://github.com/prog-sha/gd/releases/latest)
and put the `gd` inside on your PATH. When `gd --version` prints a version, the install is done.
The SHA-256 of each archive can be checked against the bundled `SHA256SUMS`. The macOS build is signed with a Developer ID and notarized by Apple.

### macOS / Linux

```sh
curl -fsSL https://gd.progsha.com/install.sh | sh
```

The installer checks the archive checksum and installs to `~/.local/bin`. Add that directory to your PATH if prompted. Linux binaries require arm64 or x86_64 and glibc 2.38 or newer.

### Windows (PowerShell)

```powershell
Invoke-WebRequest -UseBasicParsing https://gd.progsha.com/install.ps1 -OutFile install-gd.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-gd.ps1
```

Run these commands in PowerShell. Administrator access is not required. The installer verifies the SHA-256 checksum, installs `gd.exe` into `%LOCALAPPDATA%\gd\bin`, and adds that directory to your user PATH. Open a new terminal and run `gd --version`.

For a manual install, download `gd-windows-x86_64.zip` from Releases, extract it, and add the directory containing `gd.exe` to your user PATH. Do not run the Unix `curl | sh` command in PowerShell.

### Homebrew (macOS)

```sh
brew tap prog-sha/gd https://github.com/prog-sha/gd
brew install prog-sha/gd/gd
```

This tap lives in the product repository and installs the signed Universal binary. Update with `brew update && brew upgrade prog-sha/gd/gd`; remove with `brew uninstall gd`.

### apt (Linux amd64 / arm64)

Use an apt-based distribution with glibc 2.38 or newer, such as Ubuntu 24.04 or Debian 13. Ubuntu 22.04 and Debian 12 need a source build. The dedicated repository uses a signing key restricted to this source:

```sh
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://gd.progsha.com/apt/gd.asc | sudo tee /etc/apt/keyrings/gd.asc >/dev/null
sudo chmod 0644 /etc/apt/keyrings/gd.asc
echo 'deb [arch=amd64,arm64 signed-by=/etc/apt/keyrings/gd.asc] https://gd.progsha.com/apt stable main' | sudo tee /etc/apt/sources.list.d/gd.list
sudo apt update
sudo apt install gd
```

Subsequent versions arrive through normal apt updates. Remove with `sudo apt remove gd`. Repository metadata and packages use HTTPS; apt also verifies the signed metadata.

### winget (Windows)

The official winget catalog entry is pending submission and review. Until it is accepted, use the PowerShell installer above. Maintainers can validate or submit the [prepared manifests](https://github.com/prog-sha/gd/tree/0.7/packaging/winget); `winget install prog-sha.gd` is not yet an available installation route.

### Build from source

Building from source needs Python, uv, SCons, and a C/C++ compiler. The executable is `gd.*.template_release.*` under `bin/`.
TLS is built in, so no separate TLS library is needed.

```sh
git clone --branch 0.7 https://github.com/prog-sha/gd.git
cd gd
scons platform=macos target=template_release -j8
# Linux ARM64: platform=linuxbsd arch=arm64
# Linux x86-64: platform=linuxbsd arch=x86_64
# Windows: platform=windows windows_subsystem=console
```

## Quick start

Create one file, `hello.gd`. No config file or package is needed. `main()` is the entry point, and the integer it returns becomes the process exit code.

```gdscript
func main():
	print("Hello, world")
	return 0
```

```sh
gd hello.gd
```

To check types and syntax without running, use `check`.

```sh
gd check hello.gd
```

## Import scripts and shared constants

Use `@import "./settings"` for a local file and `@import hello` for an installed package. Use the imported name directly; `.gd` and `as settings` can be omitted.

Put shared values in `settings.gd`:

```gdscript
const TITLE = "Hello"
const USER = "world"
```

Use them from `main.gd`:

```gdscript
@import "./settings"

func main():
	print(settings.TITLE, ", ", settings.USER, "!")
	return 0
```

This prints `Hello, world!`. Use this pattern when values need to be shared.

Access values as `settings.TITLE` instead of copying a `const` declaration for each value into the caller. Keep values and functions used by only one script in that script. Use `as` when a name conflicts or needs clarification. Built-in APIs such as `GD.web` need no import.

See [Packages and distribution](#en-packages-and-distribution) for installation and version locking.

## Pick by purpose

The standard API has one entry point, `GD`, with a child per purpose. Scripts write these names as they are.

| What you want | Entry | Example |
|---|---|---|
| Files, text, time, HTTP client, async | `GD` | `GD.file.read_text("a.txt")` |
| Websites and Web APIs | `GD.web` | `GD.web.app()` |
| SQLite or PostgreSQL | `GD.database` | `GD.database.client()` |

`GD.database.postgres` and `GD.database.redis` are advanced entries for features specific to one backend.

## Looking up the API

Pass the same spelling you write in a script to `gd doc`. Signatures come from the executable, so they match the implementation.

```sh
gd doc                     # short guide and entry points
gd doc manual              # the full manual
gd doc GD                  # the children of GD
gd doc GD.file             # file API
gd doc GD.http.fetch       # the returned HTTP response
gd doc GD.web.app          # Web application
gd doc SceneTree           # Inspect a public engine class.
gd doc all                 # every public class
```

Return types such as `R`, `Err`, and `GDWebRequest` are looked up by name alone. For Godot classes such as Node, SceneTree, and Timer,
see the [Godot class reference](https://docs.godotengine.org/en/stable/classes/) as well.
The manual is shown in Japanese when `LC_ALL` or `LANG` starts with `ja`, and in English otherwise.
The web version at [gd.progsha.com](https://gd.progsha.com/) switches between Japanese and English.

## GDScript basics

The examples avoid repeating type names. Variables assigned with `:=` and success values returned as `return value, failure` infer their types.
Parameters without annotations remain dynamic. Add annotations only where you want to fix a type boundary.

### Arguments and flags

Arguments placed after the script name arrive in `main(argv)`. An argument that could be mistaken for gd's own flag, such as `--name=gd`,
reaches the script when placed after `--`.

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

To interpret them as flags, use `GD.cli.flags()`. It accepts `--name gd`, `--name=gd`, and `-name=gd` alike.

```gdscript
func main(argv):
	var flags := GD.cli.flags()
	flags.flag_str("name", "world", "who to greet")
	var parsed := flags.parse(argv)
	if not parsed.ok:
		print(flags.usage())
		return 1
	print("Hello, " + flags.get_str("name"))
	return 0
```

### Calling external commands

Call external tools with `GD.cli.run()`. Only the calling GDScript waits, so other requests proceed even when it is called from a `gd serve` handler.
Under `--strict` it needs `--allow-run`. You can narrow the target, as in `--allow-run=/usr/bin/git`.

```gdscript
func main():
	var got := GD.cli.run("git", ["rev-parse", "HEAD"])
	if not got.ok:
		return 1
	print("code=", got.v["code"], " out=", got.v["output"])
	return 0
```

A nonzero child exit returns Err. Inspect `value.code` or `e.info.code` for its status and `value.output` for captured output. Timeout and cancellation retain captured output too.

The third argument, `opts`, changes the behavior.

| Name | Default | Meaning |
|---|---|---|
| `timeout` | `0` | Seconds before giving up. 0 is unlimited. Past it, the child is shut down and `Err.TIMED_OUT` is returned |
| `output` | `true` | Collect the output. With `false`, the child uses the parent's standard I/O directly and nothing is collected |

### Commands during development

```sh
gd check main.gd        # check types and syntax without running
gd fmt main.gd          # normalize formatting
gd test                 # collect and run *_test.gd
gd --watch main.gd      # run again on every save
gd eval 'print(1 + 1)'  # try one line
gd repl                 # try interactively
```

## Values and failures

A function that can fail returns two values, the success value and the failure, instead of throwing.
The caller receives both with `var value, e :=`, and a non-`null` `e` means failure.

```gdscript
func main():
	var text, e := GD.file.read_text("note.txt")
	if e:
		print(e.text())
		return 1
	print(text)
	return 0
```

### Shorter failure handling

Instead of writing `if e` every time, append `?` to the call. The failure is returned to the caller as is, and only the success value remains.
A function that uses `?` also returns a success value and a failure itself, with `return value, failure`.

```gdscript
func title(path):
	var text := GD.file.read_text(path)?
	return text.strip_edges(), null

func main():
	var text, e := title("note.txt")
	if e:
		print(e.note("read the title").text())
		return 1
	print(text)
	return 0
```

| Form | Meaning |
|---|---|
| `var value, e := call()` | Receive the success value and the failure separately |
| `return value, null` / `return null, failure` | Return success or failure. The success type is inferred from `value` |
| `call()?` | On failure, return it to the caller as is |
| `call()!` | On failure, print the reason and stop the program there with exit code 1. For prototypes and tests. Under `gd serve`, only that handler fails |
| `e.note("purpose")` | Add working context to a failure. It prints as "purpose: original reason" |
| `e.kind` | The kind, such as `Err.NOT_FOUND` or `Err.INVALID_DATA`. Use it to branch |
| `Err.err("reason", Err.NOT_FOUND)` | Create a failure yourself |

In `main()`, which does not pass failures up, receive them with `var value, e :=` or `!`.

File-operation failures put `op`, `path`, `source`, and `source_code` in `e.info`.
A rename has `old` and `new` instead of `path`. `source` is one of `posix`, `win32`, and `engine`.
A `kind` is set only when its meaning, such as NotFound, is known; an unknown I/O failure keeps its source details with `Err.NONE`.

Some APIs retain processed values on failure. Receive both with `var value, e := call()` when decoding, parsing queries, running child processes, or executing Redis batches. Causes remain accessible through `e.cause`; use `e.is(kind)` to inspect their categories.

The MessagePack, CBOR, TAR, TOML, YAML, XML, dotenv, front-matter encoding, and PBKDF2 APIs in `GD.data` return a value and Err in both synchronous and async forms. Unrepresentable entries are not silently omitted.

`base64_decode(text)` and `base32_decode(text)` require standard padding; pass `true` as the second argument for raw input. Base32 uses uppercase letters. `base64url_decode(text)` uses raw input by default; pass `false` for padded input. Both synchronous and async decoders retain partial bytes on failure.

`GD.http.decode_query(raw)` returns Arrays of all values per key and the first Err. `req.query` provides the first value; `req.query_all` retains every value. Keys and values containing NUL or invalid UTF-8 remain `PackedByteArray`, preserving their exact bytes. `encode_query()` repeats Array values and accepts byte arrays. `parse_url()` preserves the original spelling in `raw_query`. After editing `query`, call `url.erase("raw_query")` before rebuilding the URL to use the edited dictionary.

### Return value rules

Code works without written types. When you do write types, these are the detailed rules.

- A result signature has two types, such as `-> int, Err`: one success type and `Err`. The runtime type is `R`; `-> R` or no return annotation is also allowed.
- A comma return always has two values. The last must have type `Err` or be `null` for success. A string cannot be returned there directly; wrap it with `Err.err(reason)`.
- Fix a failure variable's type with `var e: Err = ...` or `var e := Err.err(...)`. A variable declared with `var e = ...` can change type and cannot occupy the last slot.
- To return several items, put them in one array or dictionary, as in `return [1, 0.0, ""], null`. `return null, null` returns null as the success value.
- Unpack an `R` with exactly two names: `var value, e := call()`. A declaration that lists expressions, such as `var a, b, c := 1, "a", 0.0`, is a different thing: it has no count limit and infers each name from its expression. It may also assign variables visible in the function, including those of enclosing blocks, when it introduces at least one new name, and every right-hand expression is evaluated before any assignment. Constants, parameters, and variables a lambda captured cannot be assigned.
- Using `?` in a function whose annotated return type lacks `, Err` gives `The "?" operator needs a function returning "R" or "Err".`
- Writing `return R.ok("a")` under `-> int, Err` is a compile error. When the type is dynamic, it is checked at runtime.
- When a typed Array or Dictionary is the success value, give the original container the same element type.
- A function returning a value and a failure returns on every path. One that only propagates with `?` still ends with `return null, null`.
- A function declaring a single return type such as `-> int` cannot use a comma return. Declare `-> int, Err`.
- A lambda cannot use a comma return. Use `return R.ok(value)` and `return R.err(reason)`.

### Carrying results in R

To carry the value and the failure around as one value, use `R`. Read `ok` for the outcome, `v` for the success value, and `e` for the failure.
Build one with `R.ok(value)` or `R.err(reason, kind, partial_value)`. Web handlers and database transactions can also be written to return this `R`.

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

`R.ok()` carries null as its success value, not integer 0. I/O APIs that make partial progress keep the completed amount in `v` as a partial value even on failure.
`note()` preserves the partial value, and `v_or(fallback)` returns the fallback on failure.
When `?` propagates a failure whose partial value does not fit the caller's success type, only the partial value is dropped; the reason and kind are kept.

### Waiting and concurrency

Waiting methods in HTTP, databases, `GD.net`, files, and others are written as ordinary function calls.
Only the calling GDScript waits; other networking and timers keep running.

```gdscript
func main():
	var res := GD.http.fetch("https://example.com/")!
	print(res.status)
	return 0
```

To start several operations together, use the variants ending in `_async` with `GD.async.all()`.

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

| Entry | Purpose |
|---|---|
| `name_async()` | Start the operation and return a Signal. `await` gives the same result as the regular name |
| `GD.async.all(list)` | Accept Callables and Signals and return all results in input order. An invalid input becomes an error in its result slot |
| `GD.async.spawn(fn)` | Run a GDScript function in the background. It keeps running after `main()` returns |
| `GD.async.sleep(sec)` | Wait the given number of seconds |

Pass Callables rather than Signals to `all()`. It subscribes to completion before starting each one, so it cannot lose a result that finishes early.

A signal saved with `:=` retains its completion type. Reassignment from a different or unknown completion type is rejected.
For dynamic completions, declare the receiver as `Signal` and annotate the awaited value as needed, for example `var result: R = await pending`.

`spawn()` does not move CPU work to another thread. Long GDScript yields to other work automatically, but
the inside of a native method is not interrupted, so use the `_async` variant when passing large input to a standard module.
A waiting method may be called only from a function that GDScript itself called. Native callbacks such as `Array.map()`, `_init()`, member initializers, and `_to_string()` cannot wait for it. Adding `await` inside such a callback does not make the native caller await the result. Move asynchronous initialization into `main()`; replace asynchronous `map()` callbacks with a `for` loop in an asynchronous function or the Callables passed to `GD.async.all()` above.

## Tutorial: a notes API on SQLite

With what you have read so far, this builds a small API that accepts JSON and stores it in SQLite, in one script.
The result is a development server with input validation and SQL parameter binding, started with narrowed permissions.

### 1. Create a working directory

```sh
mkdir notes-api
cd notes-api
```

### 2. Write the API

Save the following as `main.gd`.

```gdscript
# Store notes in an embedded database and expose a JSON API.
extends RefCounted


const PORT := 18080 # development port listening on loopback
const DB_PATH := "user://notes.sqlite3" # per-user writable area provided by gd

var app := GD.web.app()
var db := GD.database.client()


# Return notes as JSON, newest first.
func list_notes(_req):
	var got := db.query("SELECT id, title FROM notes ORDER BY id DESC")?
	return GD.web.json(got.rows)


# Store a validated title and return the created row.
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

Read it from the top.

- `app` is the router and `db` is the database connection. Both are script variables so the server can keep running after `main()` returns.
- `main()` first opens SQLite and creates the table. `user://` in `DB_PATH` is a per-user writable area provided by gd.
- `app.route()` registers an HTTP method, a path, and the function to call for it (the handler).
- A handler receives a `GDWebRequest` and builds the reply with `GD.web.json()`. A `?` in the middle returns the failure to the server, which becomes a status such as 500.
- The POST route carries `GD.web.json_body()`. The handler is called only when the body matches the rule, and the value that passed arrives in `req.valid("body")`.
- SQL values are bound to `$1`. SQL is never built by string concatenation.

### 3. Start with narrowed permissions

Run unverified scripts and servers exposed to the outside with `--strict`, which denies permissions by default. Here the listener is limited to one loopback port.
`serve` keeps the process alive after `main()` returns, and it is the command to use for servers.

```sh
gd check main.gd
gd --strict --allow-net=127.0.0.1:18080 serve main.gd
```

### 4. Use it from another terminal

```sh
curl -s -X POST http://127.0.0.1:18080/notes \
  -H 'Content-Type: application/json' \
  -d '{"title":"try gd"}'
curl -s http://127.0.0.1:18080/notes
```

The first call returns status `201` with the created row, the second the stored array. An empty title, a title over 120 characters,
or a non-JSON body is rejected with `400`. Press Ctrl-C in the terminal that started it to stop.

In production, keep this process on loopback behind a TLS reverse proxy, and switch storage that must survive crashes
to PostgreSQL. Keep connection details out of the source and read them from allowed environment variables.

## Permissions

Start with normal execution. `--strict` is an advanced mode for explicitly designing and configuring the permissions a script needs.

| Mode | Suited to | Restrictions |
|---|---|---|
| Normal execution | Running trusted source during development | Neither files nor the network are restricted |
| `--strict` | Unverified scripts, public servers | The startup directory is read-only; absolute paths outside it are denied. Network, environment variables, child processes, native extensions, and system information are denied by default |

On macOS/Linux, v0.7.3 `--strict` still permitted absolute reads outside the startup directory.

Under `--strict`, start by listing what the script uses.

```sh
gd --strict \
  --mount store=/srv/app:rw \
  --allow-net=db.example.com:5432 \
  --allow-env=DATABASE_URL \
  main.gd
```

| Flag | Grants |
|---|---|
| `--mount name=path:r` / `--mount name=path:rw` | Read or read/write on a named directory |
| `--allow-net=host:port,...` | Connecting and listening. Without a value, everything |
| `--allow-env=name,...` | Environment variables |
| `--allow-run=command,...` | Child processes |
| `--allow-ext=path,...` | Native extensions a script loads while running |
| `--allow-sys=item,...` | Machine and system information |
| `--deny-*` | A denial that wins over the matching allow |
| `-A` | Allow everything except files. For temporary use during development |

### File locations

A script sees files through four kinds of location. Write the name of the location at the start of the path, or write an absolute path as it is.

| Spelling | Location | Under strict |
|---|---|---|
| `res://a.txt` | The directory the script was started from | Read-only |
| `user://a.txt` | Per-user writable area provided by gd | Read/write |
| `store://a.txt` | The name given by `--mount store=/srv/app:rw` | As specified |
| `/etc/hosts` | That location on the machine | Denied unless it is inside the startup directory |

- A relative path that climbs above `res://` is refused in either mode.
- Under `--strict`, the startup working directory is the implicit read boundary, even when the entry script is in a subdirectory. Absolute paths inside it use the same read-only checks as `res://`; parent and sibling directories are denied. Links cannot bypass this boundary. To access an outside directory, explicitly mount it and use its named scheme, such as `store://`. `-A` does not expand file access.
- `--mount` and absolute paths are for Linux and macOS. Windows rejects them, so put files under `res://` or `user://` there.
- A mount name uses lowercase letters, digits, and `-`. `res`, `user`, `uid`, `pipe`, `local`, `libgodot`, `tcp`, `unix`, `http`, `https`, `file`, `data`, and `cache` are reserved and cannot be chosen.

### Network and extension permissions

- In `--allow-net`, `localhost:8080` also covers IPv4 loopback `127.0.0.0/8` and IPv6 `::1` on the same port.
- `*.example.com:443` allows its subdomains.
- Native extensions run in the same process, so allow only ones you trust.

### serve and SceneTree

`gd serve` is the resident way to run, and it creates no SceneTree. Networking, timers, `await`, custom Signals, `GD.async.sleep()`,
and `queue_free()` on nodes outside a tree all work. With no work to do, it sleeps until the next deadline or network notification, so there is no cycle to tune.

| What you want | How |
|---|---|
| Keep a Web server or scheduled job resident | `gd serve main.gd` |
| Use Node `_process()`, `_physics_process()`, `process_frame`, SceneTreeTimer, or high-level multiplayer | Normal execution without `serve` |
| Run a script extending SceneTree or MainLoop | Normal execution without `serve` |
| Check during development that no SceneTree slips in | `gd --no-scene-tree --allow-net serve app.gd` |
| Listen with several processes | `--workers=<n>` or `--workers=auto`. `n` is an integer of 1 or more |

Under `serve`, a script that only extends `Node` is not added to a tree.
`--no-scene-tree` reports a diagnostic as soon as a SceneTree is created and exits with code 1. It is inherited by `--watch` and `--workers` children.
Normal execution creates an implicit SceneTree, so it fails with this flag.

## TCP and UDP

Use `GD.net` for low-level networking. Godot's low-level types remain for compatibility, but new code should use `GD.net`.

```gdscript
func echo():
	var listener := GD.net.listen_tcp("127.0.0.1", 8080)?
	var conn := listener.accept()?
	var data := conn.read(65536)?
	conn.write(data)?
	conn.close()
	return 0, null
```

- `GDTCPConn` keeps reads and writes in separate queues, so several GDScripts may call it concurrently.
- Deadline methods set durations from now; zero clears them.
- `close()` releases pending reads and writes with `Err.INTERRUPTED`. Listener accepts behave the same way.
- A connection tries the IPv4 and IPv6 candidates from name resolution in order and keeps only the one that succeeds. The overall `timeout` is never extended.

### TLS

Open TLS with `GD.net.dial_tls(host, port, opts)`. It verifies the certificate chain and host name by default, and a failure never falls back to plaintext.
The result is the same `GDTCPConn` used for TCP.

| `opts` | Meaning |
|---|---|
| `timeout` | One deadline in seconds covering both connect and handshake |
| `ca_file` | A private CA. It takes precedence over the environment settings |
| `cert_file`, `key_file` | Client authentication. Provide both, as files inside permitted mounts |
| `server_name` | Check the certificate against a name different from the dial address |
| `next_protos` | An array of ALPN names. One name is 1–255 bytes, and the whole list is up to 65535 bytes |
| `insecure_skip_verify` | Skip verification. Use only for tests where verification is deliberately unnecessary |

Read the negotiated result from `negotiated_protocol` and `version` in `connection_state()`. TLS 1.2 is 771 and TLS 1.3 is 772.

Without other settings, the trusted CAs are the OS trust settings on macOS and Windows, and the system CA bundle on Linux.
Set `SSL_CERT_FILE` or `SSL_CERT_DIR` before starting the process to use the given CAs on any OS.
Directory lists use `:` on Unix and `;` on Windows.

When a server requires client certificates, pass `client_ca` (a trusted CA bundle) and `client_auth` in the `opts` of `app.listen_tls(port, cert, key, host, opts)`.
A missing `client_ca` uses the system trust settings.

| `client_auth` | Behavior |
|---|---|
| `none` | Does not request a certificate |
| `request` | A certificate is optional. It is not verified |
| `require` | A certificate must be presented. It is not verified |
| `verify_if_given` | Verifies a certificate only when one is presented |
| `require_and_verify` | Requires a verified certificate |

### UDP and name resolution

`GD.net.listen_udp()` returns a `GDUDPPacketConn`. `read_from()` returns a dictionary containing `data`, `host`, `port`, and `truncated`.
Pass an IP address resolved by `GD.net.resolve()` as the host of `write_to()`. Packets are never merged.
The default `buffer=0` keeps the OS receive buffer as it is; only a positive value requests a change.

`GD.net.resolve()` returns the first address selected by the OS. It keeps no name cache.
`GD.net.local_addresses()` returns the machine's address list and distinguishes an empty list from an OS failure.
The failure's `e.info` carries `syscall`, `source`, and `source_code`.

## Files and data

`GD.file` reads and writes files and handles paths. The directory you started from is `res://`, and absolute paths work as written.
To write to an outside directory under strict, write the name given by `--mount store=/srv/app:rw` as in `store://users.csv`.

```gdscript
func main():
	var rows := GD.data.csv_objects(GD.file.read_text("store://users.csv")?)?
	GD.file.write_text("store://users.json", JSON.stringify(rows))?
	return 0, null
```

File operations suspend only the calling GDScript, even under their regular names. Other requests proceed while a Web server handler reads a file.
Use the variants ending in `_async` only to start several operations together.

```gdscript
func handler(_req):
	var body := GD.file.read_text("store://big.json")
	if not body.ok:
		return GD.web.text("cannot read", 500)
	return GD.web.text(body.v)
```

Files embedded by `compile` can be read, listed, and served statically through the same API.

### Reading large files

To read without holding the whole file in memory, open a `GDFileStream` with `GD.file.open(path, mode)`.
Modes are `read`, `write`, `append`, and `read_write`. Call `close()` when done.

| Method | Behavior |
|---|---|
| `read(max)` | Returns up to `max` bytes. It may return fewer. An empty successful value is EOF |
| `write(bytes)` | Writes all bytes and returns the count. On a failure partway, `R.v` retains the number already written |

Operations on one stream run in arrival order, and separate streams proceed in parallel. Append always writes at the end, even after a seek.
`read_bytes()` also retains the bytes already read in `R.v` when it fails partway.
`read_text()` rejects input too large for a String instead of truncating it, so handle large files as bytes or a stream.

### Files updated concurrently

When several processes update the same file, use `GD.file.replace_text(path, old, body)`.
It replaces the content only when the `old` you read still matches the current content, so a concurrent edit is never silently overwritten. Pass `null` as `old` to create a new file.

### Entry points by data format

| Purpose | Entry |
|---|---|
| Reading CSV, TOML, YAML, JSONL, JSONC, XML, INI, TAR, front matter, and `.env` files | `GD.file.read_csv(path)` and similar |
| In-memory conversion of the same formats, JSON, codecs, hashes, HMAC, PBKDF2, HKDF, byte sequences | `GD.data` |
| UUID and ULID | `GD.id` |
| Time conversion and arithmetic | `GD.time` |
| Text formatting and comparison | `GD.text` |
| HTML entities, tags, and gdhtml (a micro template with Mustache syntax) | `GD.html` |
| Flags and environment variables | `GD.cli` |
| Array and dictionary operations | `GD.collection` |
| Special math values and bit operations | `GD.math` |
| Version comparison | `GD.version` |
| Logging to the terminal and files | `GD.log` |
| Test assertions | `GD.test` |

Environment variables and `.env` have separate entries by what you read.

| What you read | Entry |
|---|---|
| Process environment variables | `GD.cli.env(name, fallback)` and `GD.cli.require_env(name)`. Strict mode needs `--allow-env` |
| A `.env` file | `GD.file.read_env(path)`. Reads the file into a dictionary |
| A dotenv string | `GD.data.env(src)` and `GD.data.to_env(data)`. Convert to and from a dictionary in memory |

In-memory conversions compute in place under their regular names, and their `_async` variants compute on another thread. Use `_async` for large inputs.
Run `gd doc GD.file` and `gd doc GD.data` for the exact lists. Checks and limits per format are in the description of each entry in the API reference.

`GD.collection` operations that take a Callable yield to other work about every 1 ms.
Each `GD.log` call waits until the write completes and never truncates the message. Check failures through the returned `R`; `GD.log.flush()` waits for all earlier output.

### JSON rules

Use `GD.data.json_encode(value)` to produce JSON bytes and `GD.data.json_decode(bytes)` to read bytes received from outside.
Both return a success value and an `Err`. `GDWebRequest.json()`, `GDHTTPResponse.json()`, and each JSONL line follow the same rules.

- Invalid UTF-8, duplicate names, non-finite numbers, unsupported types, and cycles fail instead of being turned into ambiguous values.
- An integer within the signed 64-bit range returns as `int`; only fractions, exponents, and out-of-range values become `float`. Strings and keys preserve `\u0000`.
- Pass `{"deterministic": true}` when the same value must give the same bytes, as for signatures or cache keys.
- The options `deterministic` and `escape_html` are `bool`; `max_bytes` and `max_depth` are `int`. An invalid type returns `Err.INVALID_DATA`, and exceeding a limit returns `Err.LIMITED`.
- Keep the input Arrays, Dictionaries, and their children unchanged until `json_encode_async()` finishes. The options dictionary is copied at the start.

### Hashes and key derivation

`GD.data` returns SHA-1, SHA-224/256/384/512, and SHA3-224/256/384/512 digests. HMAC, PBKDF2, and HKDF accept
`sha1`, `sha224`, `sha256`, `sha384`, `sha512`, `sha3-224`, `sha3-256`, `sha3-384`, or `sha3-512` as the hash name.
PBKDF2 and HKDF accept an output length and return an `R` failure for an invalid hash or settings outside the supported range. PBKDF2 iteration counts of one or less perform one iteration.

### Thread limit

`GD.async.set_max_threads(max)` sets the limit on OS threads managed by gd and returns the previous value. The default is 10000.
Exceeding the limit terminates the process. Lowering it below the current count also terminates it. Threads created directly by external libraries are not counted.

## Web framework

Create `main.gd` and `index.html` in the same directory. One route renders a greeting through an HTML template; another returns the same data as JSON.

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

Run from that directory:

```sh
gd serve main.gd
```

Open `http://127.0.0.1:8080/?name=Alice` to insert “Hello, Alice!” into `{{message}}`. `/api/hello?name=Alice` returns `{"name":"Alice","message":"Hello, Alice!"}`. Template values are escaped for their HTML context.

`serve` keeps listening after `main()` returns. Stop with Ctrl+C.

### Routes and replies

`route(method, pattern, handler)` binds an HTTP method and a path to a handler. `:name` in the pattern arrives in `req.params["name"]`.
A handler receives a `GDWebRequest`. It reads only the needed body through `req.read()`, `bytes()`, `text()`, `json()`, or `save()`.
A body sent by an HTML form becomes a dictionary with `GD.http.decode_query(req.text()?)`.

`req` is the incoming request. Inspect fallible operations with `var value, e := call()` and return `null, e`, or propagate with `call()?`. Use `return response, null` only on a successful path after handling every possible failure.

`GD.web.view()` and `GD.web.json()` return `response, Err`. A handler can forward the result directly: `return GD.web.json(data)`. Template loading/rendering and JSON encoding preserve the original Err for `app.on_error(req, e)`. Middleware, postprocessors, and automatic JSON encoding share that boundary. If the error handler itself fails, the server returns 500 without invoking it again. Once streaming starts, the HTTP status is committed; handle errors returned by the writer.

The value a handler returns becomes the reply.

| Returned value | Reply |
|---|---|
| `GD.web.html(body)`, `GD.web.view(path, data)` | HTML |
| `GD.web.json(data)` | JSON |
| `GD.web.text(body)`, `GD.web.bytes(body, type)` | Text, or any media type |
| `GD.web.stream(producer)` | A body written a little at a time. See "Web operations and advanced features" |
| `GD.web.redirect(to)` | 302. `to` is limited to a path on the same site. Set `away` to `true` to send elsewhere |
| `GD.web.not_found()` | 404 |
| A string | 200 as text/plain |
| A dictionary without `body` | 200 as JSON |
| `null` | 204 |
| A failed `R` or an `Err` | Status by kind. `INVALID_DATA` → 400, `UNAUTHENTICATED` → 401, `PERMISSION_DENIED` → 403, `NOT_FOUND` → 404, `LIMITED` → 429, `UNSUPPORTED` → 501, `TIMED_OUT` → 504; other kinds → 500 (all are `Err` constants) |

Route handlers and middleware may return a Signal, including after `await`. Processing resumes when it completes: no arguments become `null`, one argument becomes that value, and multiple arguments become an Array. An unavailable Signal goes through the error handler. Pending subscriptions are removed when the request ends or the app stops.

- text and html take the status as the second argument; bytes takes it after the media type.
- `GD.web.header(reply, name, value)` adds a header to a reply.
- `GD.web.guard(reply)` adds the defensive headers such as `X-Content-Type-Options`, `X-Frame-Options`, and `Content-Security-Policy` at once.
- The reason for a failure is not written to the body by default. It is shown only while `app.show_errors(true)` is set during development.
- `req.path` is the path with each segment decoded once. `req.target` is the original text, keeping percent escapes and the query. `%2F` does not become a path separator.
- A path whose percent-decoded segments contain invalid UTF-8 or control characters gets a 400. Query values preserve arbitrary bytes as `PackedByteArray`; validate their type before using them as text. `req.query` skips malformed pairs; use `GD.http.decode_query()` to receive both the valid pairs and an `Err`.
- Do not modify values passed to `GD.web.json()` or `view()` until the reply has been sent.

The router also accepts the following.

| Registration | Purpose |
|---|---|
| `app.static("/assets", "res://public")` | Answer GET under the prefix with files from the directory. The media type comes from the extension, and nothing outside the directory is served. Write the index of `/` as a `route` |
| `app.group("/api", [middleware])` | A route group with a shared prefix and middleware. The result has `route()` and `use()` |
| `app.fallback(handler)` | Requests matching no route. Return the 404 page here |
| `app.on_error(handler)` | The reply when a handler returns a failure |
| `app.after(handler)` | Reshape the reply before sending. Receives `func(req, reply)` and returns it with headers added |

### Middleware

Middleware is a function called before the handler. It receives a `GDWebRequest`, returns `null` to continue, or returns a reply to stop there.
An object with `handle(req)` also works. Pass values to later stages with `req.keep(name, value)` and read them with `req.kept(name)`.

| Registration | Scope |
|---|---|
| `app.pre(mw)` | Before route selection. Every request |
| `app.use(mw)` | After route selection. Every route. Can read `req.params` |
| `group.use(mw)` | Routes in that group |
| `app.route(method, pattern, handler, [mw])` | That route only |

Input validation is middleware too. `GD.web.json_body(rule)`, `GD.web.query(rule)`, and `GD.web.params(rule)` check the body, query, and path values,
and put the values that pass into `req.valid("body")`, `req.valid("query")`, and `req.valid("params")`.
Rules are built from `GD.web.text_rule()`, `int_rule()`, `number_rule()`, `bool_rule()`, `list_rule()`, and `object_rule()`,
with `GD.web.optional()` and `GD.web.one_of()` for omission and choices. Query and path values are strings, so check them with `text_rule()` and convert with `to_int()` when needed.

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

The built-in middleware are `GD.web.sessions()`, `GD.web.csrf()`, `GD.web.jwt()`, and `GD.web.rate()`. The authentication section uses them.

### HTML templates

As pages grow, move the HTML into template files and render them with `GD.web.view(path, data)`.
The template language is gdhtml, a micro template with Mustache syntax. It handles `{{name}}`, `{{{html}}}`, `#if`, `#unless`, `#each`,
`#with`, `else`, and `{{> header}}`. Using `{{> header}}` from `views/page.html` reads
`views/partials/header.html` at the same level.

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

Double-brace values are escaped by the context they appear in. The template author is trusted, the values inserted are not.

| Context | Handling |
|---|---|
| HTML body, quoted and unquoted attributes, attribute names | HTML escape |
| `href="{{url}}"` | Relative URLs and `http`, `https`, `mailto` pass. `data-href` is treated the same |
| `href="/work/{{path}}"`, `href="/?q={{query}}"` | Paths are normalized keeping separators, query values are percent-escaped |
| `onclick`, `script` body | Encoded as JSON in a form where `</script>` cannot break the structure, even for `application/json` |
| `style` | Safe single CSS values and CSS strings and URLs pass |
| Dangerous URLs, srcset, CSS values, attribute names | Replaced with `#ZgdunsafeZ` or `ZgdunsafeZ` without failing the whole page |

- Triple braces `{{{html}}}` are the only unescaped entry, and they work only in the HTML body. Pass only fixed HTML or a sufficiently checked value.
- A double brace cannot be marked "checked" to skip escaping.
- A template whose branches or `each` iterations end in different contexts, an unclosed tag, or an ambiguous URL or JavaScript context fails to render.
- Template size has no fixed limit. Only the depth of recursive partials is limited, to 100000.
- Do not modify the dictionary you passed until rendering finishes.

For a server that renders the same template repeatedly, parse it once at startup with `GD.html.template(source, partials)?`,
then call `execute(data)?` on the returned value from each request. The parsed value is immutable and can be used by several requests at once.
`execute_bytes(data)?` produces UTF-8 bytes directly, so they can be returned as is with `GD.web.bytes(body, "text/html; charset=utf-8")`.

### Authentication and CSRF

Login state is held by `GD.web.sessions()`. `issue(value)` creates a session ID, and the value of `cookie(id)` is returned as `Set-Cookie`.
On routes that carry the same store as middleware, the value behind the cookie's ID arrives in `req.kept("user")`, and a missing session is a 401.

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

`cookie(id)` sets `Secure` and `HttpOnly`. If the cookie does not arrive during development without TLS, use `cookie(id, false)`.
Log out with `drop(id)` and `clear_cookie()`. Sessions live in one process, so with several processes under `--workers` use JWT or an external store.

Attach `GD.web.csrf()` to write paths that authenticate with cookies. Requests other than GET, HEAD, and OPTIONS need the browser's
`Sec-Fetch-Site: same-origin`. When old browsers or non-browser clients must be accepted, choose
`GD.web.csrf({"allow_missing": true})` and combine it with separate token verification.

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

When JWT is used as a login session, revoke issued tokens on password change and logout.
`check` is called after the signature and standard claims are verified, and authentication passes only when it returns `true`.
For example, put the user's `ver` in the token and increment the stored version on password change.
With several workers, compare against something like a cache synced from a shared DB, not a per-process dictionary.

```gdscript
func token_auth(key, versions):
	return GD.web.jwt(key, {"check": func(claims):
		return versions.get(claims.get("sub", ""), -1) == claims.get("ver", -2)
	})
```

To limit per IP behind a reverse proxy, list the proxy's IPs or CIDRs in `trusted_proxies`.
gd strips trusted proxies from the right end of `X-Forwarded-For` and uses the first untrusted IP as the key.
`X-Forwarded-For` is ignored when `trusted_proxies` is unset and when it comes from an untrusted peer, so a client cannot forge its own IP.
IPv4 and IPv4-mapped IPv6 are matched as different things, so use an IPv6 CIDR to trust mapped addresses. Proxy settings with zones are rejected.

```gdscript
var per_ip := GD.web.rate({"limit": 60, "trusted_proxies": ["127.0.0.1", "172.18.0.0/16"]})
```

### Shutdown

Wait for shutdown with `app.shutdown(context)`. It stops accepting new connections and keep-alive, then waits for in-flight requests.
Past the deadline it returns `Err.TIMED_OUT` but does not kill in-flight requests.
Use `app.stop()` when every connection must close immediately.

```gdscript
func close(app):
	var context := GD.async.context().with_timeout(10.0)
	var stopped := app.shutdown(context)
	if not stopped.ok:
		app.stop()
```

A handler can observe request completion and disconnection through `req.context`.
`with_cancel()` and `with_timeout()` return a child context without changing the parent, and the parent's cancellation reaches the child.
To make HTTP, database, process, and other waits cancelable, wrap them with `with_context()`, passing the context first.
The operation result is returned when it finishes first; when the context finishes first, the operation is canceled.

```gdscript
func load(req, db):
	var result = await GD.async.with_context(req.context, db.query_async("SELECT * FROM posts"))
	return result
```

### Web operations and advanced features

#### Limits and large uploads

When handling large bodies or long handlers, set the limits explicitly with `limits()` before listening.

```gdscript
func main():
	var limited_app := GD.web.app()
	limited_app.limits({"header_bytes": 1048576, "header_values": 500, "header_timeout": 15.0, "body_timeout": 10.0, "job_timeout": 30.0, "jobs": 128})
	return 0
```

To accept a 1 GB ZIP, put a per-request limit on it and stream it to a writable mount.

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

Bodies and memory are handled as follows.

| Target | Handling |
|---|---|
| Request body | No default size limit. The handler starts right after the header, and the body is read from the connection only as the handler reads it |
| `read()`, `save()` | Stream the body. An empty successful `read()` is EOF. `save()` never holds the complete body in memory |
| `bytes()`, `text()`, `json()` | Read the whole remaining body into memory. Use `save()` for large bodies. `text()` is limited to what fits in a String |
| `req.limit(bytes)` | Per-request body limit. Overflow is returned as a failure to the body-reading operation |
| Request header | Default 1 MiB. The line count is limited only when `header_values` is set. Trailers 4096 bytes |
| HTTP client response header | Up to 10 MiB |
| Slow connections | Only that connection waits. Other connections are not affected |
| Extra reply headers | No fixed count or aggregate limit. Only invalid names and values are dropped |
| Sessions and rate limits | Shared within a process, not across `--workers`. Use an external store such as a DB when sharing is needed |
| Session values | String and integer identifiers. Retention counts are set with `total` and `per_user` |
| HS256 JWT | Key at least 32 bytes. JSON and signature validity are checked |
| Rate limit key | Retention count is set with `keys` |
| HTTP status | 100..999. Out of range is sent as 500 |
| Port | 0 is allowed for listening and as the search start of `GD.net.free_port()`. Targets and `is_free()` take 1..65535 |
| Query string | `GD.http.decode_query()` reports a bare semicolon and a broken percent escape as failures |

#### Streaming bodies

`GD.web.stream(producer, length=-1, type="application/octet-stream", status=200)` sends only what `producer(writer)` writes to the `GDWebWriter`.
It never joins the whole body in memory. The producer may `await`, and it finishes by returning void or an `R`.

| `GDWebWriter` | Behavior |
|---|---|
| `write(data, offset=0, count=-1)` | Sends a range of a byte array and returns the accepted bytes. When sending is backed up, it waits until it progresses |
| `write_text(text, offset=0, count=-1)` | Sends a range of a string as UTF-8. Offset and count are in characters; the result is in bytes |
| `flush()` | Waits for preceding writes to be sent. A disconnect shows up as an error here and as `req.context` cancellation |

- A stream is single-use. Create a new one for each response. Finish reading the incoming body before returning the stream.
- `length` is the number of bytes to send. If the declared and actual lengths differ, the connection is closed. Unknown length (-1) uses DATA frames on HTTP/2, chunked framing on HTTP/1.1, and connection close as the end on HTTP/1.0.
- HEAD and statuses that cannot carry a body never call the producer.
- Long-waiting producers should observe `req.context` cancellation.
- One write does not necessarily correspond to one chunk. An empty string or empty byte array does not end the body.

#### HTTPS and HTTP/2

Start HTTPS with `app.listen_tls(8443, "cert://chain.pem", "cert://key.pem", "127.0.0.1")` and check the returned `R`.
Mount the certificate directory read-only with `--mount cert=/path/to/certs:r`. Pass a PEM chain and an unencrypted private key.
When key validation fails, no port is opened.

TLS 1.2 and 1.3 are supported, and ALPN selects HTTP/2 or HTTP/1.1. Each HTTP/2 stream proceeds independently, and canceling one does not close the others.
`header_timeout` also applies to an incomplete handshake. For requiring client certificates, see the TLS tables in "TCP and UDP".

#### gzip compression

`GD.data.gzip_writer(writer, level=-1)` creates a `GDGzipWriter` that gzips the bytes written to it and passes them to the writer below.
The writer below can be a `GDFileStream`, a TCP connection, or a `GDWebWriter`. The whole body is never held in memory.

| Item | Details |
|---|---|
| Methods | `write(bytes)`, `flush()`, `close()`, and `reset(writer)`. Each returns `R` |
| `level` | -2 (Huffman only), -1 (default), and 0..9 |
| `close()` | Finishes the gzip trailer. It does not close the writer below |
| `reset(writer)` | Clears errors and reuses the compressor at the same level |
| `header` | `name` and `comment` (non-NUL Latin-1), `extra` (up to 65535 bytes), `mod_time` (Unix seconds), and `os` (default 255). Set it before the first write |

For HTTP, return `GD.web.header(GD.web.stream(producer), "Content-Encoding", "gzip")`; the producer creates the compressor, writes, and returns the result of `close()`.
Checking `Accept-Encoding` and setting `Vary` are up to the caller. Do not compress secrets together with external input, and do not apply it to an already compressed body or a partial response.

#### Listen address and port

For an IPv6-only localhost listener, use `app.listen(8080, "::1")!`. Under strict use `--allow-net=[::1]:8080`, and connect to `http://[::1]:8080/`.
`::1` and `127.0.0.1` are separate listeners, and both differ from `::`, which means every interface.

To let the OS pick a free port, read `app.port()` right after `app.listen(0)`. The number is obtained while holding the listener, so no other process can take it.
Under strict the chosen port cannot be limited ahead of time, so allow the whole host, as in `--allow-net=127.0.0.1`.
`GD.net.free_port()` and `is_free()` are momentary diagnostics, not a way to reserve that number.

#### HTTP client connections

- HTTPS uses HTTP/2, and concurrent requests to the same origin share one connection. Peers without HTTP/2 and plain HTTP use HTTP/1.1.
- An HTTP/1.1 connection is reused for the same origin after its body is read to the end. Idle connections are kept up to 100 overall, 2 per origin, for 90 seconds.
- If a reused connection closes just after reuse, only a safely replayable method is retried once on a fresh connection.
- On HTTP/2, only requests the peer marks as unprocessed are replayed, up to seven times with growing intervals. The request deadline and cancellation still apply.

#### Web settings

The settings passed as a dictionary to `GD.http.fetch()` and the `GD.web` functions, with their defaults. Times are seconds and sizes are bytes.

| Entry | Setting and default | Meaning |
|---|---|---|
| `GD.http.fetch` | `method="GET"`, `headers={}`, `body=null` | HTTP method, request headers, request body |
| same | `timeout=30.0`, `max_body=0` | Seconds for the whole request and bytes of the response body. 0 is unlimited |
| same | `save=""`, `sha256=""` | Stream a 2xx body to `save`, returning an empty body. `sha256` requires `save`, is 64 hex digits, and only a matching completed file is placed |
| same | `authority="host:port"` | Request target for CONNECT only |
| `GD.cli.run` | `timeout=0.0`, `output=true` | Seconds before giving up on the child process, and whether to collect output |
| `GDWebApp.limits` | `jobs=0`, `job_timeout=0.0` | Number of async handlers kept and seconds. 0 is unlimited |
| same | `header_timeout=0.0`, `body_timeout=0.0` | Seconds to finish receiving request header/body. 0 is unlimited |
| same | `header_bytes=1048576`, `header_values=2147483647` | Header bytes including the request line, and the header line count |
| `GD.web.jwt_sign` | `ttl=900` | Seconds used to fill `iat`/`exp`. 0 does not add them |
| `GD.web.jwt` / `jwt_verify` | `leeway=0.0`, `require_exp=true` | Clock tolerance in seconds, and whether `exp` is required |
| same | `iss=""`, `aud=""`, `keep="jwt"` | Issuer/audience match when non-empty, and the name kept on the request |
| same | `check=Callable()` | Revocation check receiving claims after signature verification. When set, only true passes |
| `GD.web.sessions` | `total=1024`, `per_user=3` | Sessions per process, and per user |
| same | `idle=1800`, `life=43200` | Idle and maximum lifetime in seconds |
| same | `cookie="sid"`, `keep="user"` | Cookie name and the name kept on the request. The cookie name uses ASCII token characters |
| `GD.web.rate` | `limit=60`, `window=60.0` | Count per key and the fixed window in seconds |
| same | `keys=10000`, `key=Callable()` | Keys kept per process and the key selector |
| same | `trusted_proxies=PackedStringArray()` | IPs or CIDRs of proxies whose forwarded IP is trusted |
| `GD.web.csrf` | `allow_missing=false` | Whether to allow state changes from clients without Fetch Metadata |
| `GD.web.text_rule` | `min=0`, `max=4096` | Text length in characters |
| `GD.web.int_rule` | `min=-9223372036854775808`, `max=9223372036854775807` | 64-bit integer range |
| `GD.web.number_rule` | `min=-1e308`, `max=1e308` | Finite float range |
| `GD.web.list_rule` | `min=0`, `max=1024` | Element count |
| `GD.web.object_rule` | `extra=false` | Whether to keep undeclared fields |

`GDWebApp.limits` accepts only the six listed setting names and rejects misspellings and `body_limit`.

Numeric settings accept the following ranges. A value outside the range fails when set.

| Setting | Accepted range |
|---|---|
| `jobs` | 0..2147483647. 0 is unlimited |
| `header_values`, session `total/per_user`, rate `limit/keys` | 1..2147483647 |
| `job_timeout`, `header_timeout`, `body_timeout` | Finite 0..9223372036.854776 seconds. 0 is unlimited |
| session `idle/life` | 1..9223372036 seconds |
| `header_bytes` | 1..2147479551 bytes. Separate from the body |
| `req.limit`, `GD.http.fetch.max_body` | 0..9223372036854775807 bytes. 0 for `max_body` is unlimited |
| `ttl` | 0 or more |
| `leeway` | Finite, 0 or more |

## Database

The client returned by `GD.database.client()` handles SQLite and PostgreSQL with the same code.
Switching from the embedded SQLite in local development to PostgreSQL in production is done through the `driver` passed to `open()`.

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

Table creation, INSERT, and SELECT all go through the one `query()`. It yields a dictionary with `columns`, `rows`, and `tag`,
where `rows` is an array of dictionaries keyed by column name. In the example, `out.rows[0].name` is `ada`.
SQL values are bound in order as `$1`, `$2`, and the spelling is the same on both drivers. SQL is not translated, so use SQL that works on both.

| Method | Purpose |
|---|---|
| `query(sql, args)` | Collect and return the whole result |
| `query_row(sql, args)` | Return only the first row. `Err.NOT_FOUND` when there is no row |
| `query_rows(sql, args)` | Open `GDDatabaseRows` and read one row at a time. For large results |
| `stats()` | Connection count, in use, idle, wait count, wait duration, and cumulative close counts by reason |

Advance `query_rows()` with `while rows.next()`. `scan()` returns a dictionary keyed by column name and `values()` returns an array in column order.
After `next()` returns false, inspect `err()`. Call `close()` when stopping early.

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

On a constraint violation, `result.e.info` carries machine-readable details. `violation` is one of `duplicate`, `not_null`, or `foreign_key`,
and `columns` lists the related column names. On PostgreSQL, `code`, `table`, and `constraint` are included when the server returns them.
Values themselves are never kept in `info`. A failure reported by SQLite itself keeps `source="sqlite"` and its extended `source_code`.
SQLite's foreign key message has no column names, so `columns` is empty there.

```gdscript
func save(db):
	var saved := db.query(
		"INSERT INTO users(id,name) VALUES($1,$2)",
		[1, "ada"])
	if not saved.ok and saved.e.info.get("violation") == "duplicate":
		var columns := saved.e.info.get("columns", PackedStringArray())
		print("duplicate columns: ", columns)
```

### Transactions and migrations

To make several updates one success or failure, use `transaction()`. The callback receives a `GDDatabaseTx`
pinned to one connection. Returning a successful `R` commits, returning a failed `R` rolls back.

```gdscript
func save(db, id, title):
	return db.transaction(func(tx):
		tx.query("INSERT INTO posts(id,title) VALUES($1,$2)", [id, title])?
		tx.query("UPDATE counters SET value=value+1 WHERE name='posts'")?
		return R.ok(id)
	)
```

- Use the given `tx` in the callback and always return an `R`. During a transaction, `query()` on the original client and a nested transaction are rejected.
- A commit failure is returned as a failure.
- Close or cancel before COMMIT begins rolls back; after it begins, the connection closes once the result is settled.
- After the callback finishes, a retained `tx` no longer accepts new SQL.

To apply a schema in order, pass an array of statements to `migrate()` instead of splitting SQL on semicolons.
If one statement fails, everything rolls back. On success it returns the number of statements applied.
Versions and checksums are managed by the application.

```gdscript
func migrate(db):
	return db.migrate([
		"CREATE TABLE posts(id INTEGER PRIMARY KEY, title TEXT NOT NULL)",
		"CREATE INDEX posts_title ON posts(title)",
	])
```

### Advanced database features

#### Driver differences

| Item | SQLite | PostgreSQL |
|---|---|---|
| Suited to | Local development, a single process | Production, crash resilience, several workers |
| Connection | One per client. Journal and temporary tables live in memory | A pool of up to `max(4, CPU count)` by default. Set a maximum as in `pool=25` |
| Extra entries | `GD.database.sqlite.open()` for short work done in place | `GD.database.postgres` for batched sends, arrays, and JSONB |
| Notes | A database with existing `-journal`, `-wal`, or `-shm` files must be recovered or checkpointed with regular SQLite before opening | Hosts other than loopback verify the TLS certificate and host name by default. Loopback defaults to no TLS |

`open()` on `GD.database.postgres.client()` and `GD.database.redis.client()` takes the target as arguments, in the form `open(host, port, opts)`.

#### SQLite concurrency

`query()` calls on one client run in arrival order. Separate clients proceed concurrently, while writes to the same database file follow SQLite locking.
The `GDSQLiteDB` and `GDSQLiteStatement` returned by `GD.database.sqlite.open()` are a synchronous API that runs directly on the caller.
Use them only for short work and never concurrently. Use `GDDatabaseClient` for concurrent work.

#### PostgreSQL connections and types

- The pool creates no connection until the first query and grows only for demand up to the maximum. Queries after the maximum is reached wait in arrival order.
- An ordinary `query()` is also sent onto a busy connection (pipelining). Results on one connection return in the order sent.
- Transactions and `query_rows()` reserve one connection. For work that uses connection-local state, use the transaction API instead of sending a standalone `BEGIN`.
- Use `query_many`, `fetch_many`, or `exec_many` to send several SQL operations together on one connection.
- Cancellation and deadline expiry notify the caller at once, but do not guarantee that the SQL stopped on the server. Other queries are not interrupted.
- `wait_count` in `stats()` counts waits to acquire a connection and excludes response waits inside a pipeline.
- Authentication follows the server's request with SCRAM-SHA-256 or MD5. Pin the method with `auth="scram"` or `auth="md5"`. MD5 is for older servers. A cleartext password needs explicit permission.
- JSON and JSONB columns are read by the same rules as JSON in "Files and data" and preserve 64-bit integers. Ambiguous values such as duplicate names return the original JSON string.
- `bool[]`, `int[]`, `bigint[]`, and `text[]` preserve element types, nulls, and nested dimensions. Arrays with explicit lower bounds return the original text.
- Connections request UTF8. A server-reported change to another client encoding closes the connection with an error. The SQL that made the change may already have executed.

#### Redis connections

A missing `GET` returns `Err.NOT_FOUND` with `e.info.code == "nil"`; null items inside an `MGET` Array remain null. `pipeline()` always returns an Array, including a single command. On a transport failure, known replies remain available and unknown positions contain Err. A WATCH abort returns `Err.INTERRUPTED` with `e.info.code == "tx_failed"`.

- The TLS choice is the same as PostgreSQL. `timeout` on `open()` sets the connect and response deadline in seconds.
- Pool `open()` only configures the destination; network activity begins with the first `query()`.
- A connection in use is held exclusively until it is returned, and callers wait in arrival order when none is free. Canceling a waiter does not affect other calls; canceling an active call closes its connection.
- `size()` counts connections, including those connecting; `in_flight()` counts unfinished calls, including acquisition waiters.

#### Database settings

The settings passed as a dictionary to `open()`, with their defaults.

| Entry | Setting and default | Meaning |
|---|---|---|
| `GDDatabaseClient.open` | `driver="postgres"`, `path=""` | Driver and SQLite path. SQLite needs `user://...` or `:memory:` |
| same | `host="127.0.0.1"`, `port=5432` | PostgreSQL target |
| same | `pool=0` | PostgreSQL maximum connections. 0 means `max(4, CPU count)`. Unused by SQLite |
| same | `max_rows=0`, `max_bytes=0` | Rows and bytes per result collected by `query()`. 0 is unlimited. Not applied to `query_rows()` |
| `GDPostgresClient.open` | `user="postgres"`, `database="postgres"`, `password=""` | Credentials and database name |
| same | `connect_timeout=15.0`, `timeout=0.0` | Connect and query seconds. Waiting for a pool connection counts toward the query time. 0 is unlimited |
| same | `auth="any"`, `allow_cleartext_password=false` | Pin the method with `auth="scram"`/`"md5"`. A cleartext password reply only when explicit |
| same | `tls=<decided by host>`, `ca=""` | External hosts use `verify-full`, loopback `disable`. A CA file only when explicit |
| `GD.database.sqlite.open` | `busy_ms=5000`, `max_ms=0` | Lock wait and execution deadline in milliseconds. 0 is unlimited |
| same | `max_rows=0`, `max_bytes=0` | Rows and bytes per result. 0 is unlimited |
| `GDRedisClient.open` | `password=""`, `timeout=10.0` | Password, and connect and response deadline in seconds. 0 is unlimited |
| same | `tls=<decided by host>`, `ca=""` | The same TLS choice as PostgreSQL |
| `GD.database.postgres.pool` | size default 0; 0 or 1..2147483647 | 0 means `max(4, CPU count)` |
| `GD.database.redis.pool` | size default 0; 0..2147483647 | Maximum connections. 0 is unlimited. Connections being opened at once are capped at ten times the CPU count, or at the maximum when one is set |
| `GDRedisPool.open` | `pool_timeout=timeout+1.0` (30 seconds when timeout is 0) | Deadline for waiting for a free connection. An explicit 0 is unlimited |

A `query()` over `max_rows` or `max_bytes` fails only that query.
The whole connection is closed when ordering is lost through a deadline or a corrupt reply.

| Setting | Accepted range |
|---|---|
| `max_rows`, `max_bytes`, `busy_ms`, `max_ms` | 0..2147483647 |
| Bound values | 65535 for PostgreSQL, and the engine's variable limit for SQLite. `query_many` has no fixed item count |
| One PostgreSQL send | SQL and bound strings are counted as UTF-8 bytes, up to roughly 1 GiB |
| One Redis send | Server-configured limits apply |
| PostgreSQL and Redis port | 1..65535 |
| Seconds | Finite 0..9223372036.854776 seconds. 0 is unlimited |

## Scheduled jobs

A job that runs once at a fixed time is an ordinary script, called from the OS's cron or a systemd timer.
gd needs no resident scheduler for it.

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

A job that loops on its own interval is passed to `GD.async.spawn()` and kept resident with `gd serve`.
Work passed to `spawn()` keeps running after `main()` returns.

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

Stop it by ending the process. It is resident like a Web server, so `serve` is needed here too.

## Official extension modules

The core stays small. Features specific to an external service are added as GDScript packages or GDExtensions only to projects that need them.

| Entry | Purpose | API and setup |
|---|---|---|
| `Discord` | Pure-GDScript text bots on the Discord Gateway and REST | [Discord Bot](https://gd.progsha.com/pkg/) |
| `GDMemcached` | Cache client reusing TCP connections | [Memcached](https://gd.progsha.com/pkg/) |
| `GDSupabase` | Database and Auth client | [Supabase](https://gd.progsha.com/pkg/) |

Each document lists public classes, methods, return values, limits, and strict-mode examples. Because they are optional,
they are not part of the API reference generated from the core alone.

- Extensions added with `gd add` are trusted and loaded at startup, so no flag is needed. `--allow-net` for their target is still needed.
- `--allow-ext` and `--deny-ext` apply when a script loads one while running with `GDExtensionManager.load_extension()`.
- An added extension runs with the same privileges as the process, so pin the versions you trust in `gd.lock` and commit it.

## Packages and distribution

When scripts multiply or you start using other packages, create `gd.json` with `gd init`. Dependencies are pinned with `gd.json` and `gd.lock`.

```sh
gd init
gd search discord bot
gd add gd:@scope/script-package@^1.0.0
gd add ext:@scope/name@^1.0.0
gd add short-name https://example.com/module.gd
gd install --frozen
gd task test
```

### Using packages

An installed package is read from `pkg://<alias>/`, using the alias chosen by the consumer.

```gdscript
@import hello
```

- `pkg://` points into the per-user shared cache and copies nothing into the project.
- A dependency named in `gd.json` but absent from the cache is fetched on the first run. Under `--strict` the registry needs `--allow-net`.
- The default alias of `gd add` is the package name with `-` and `.` turned into `_`, so it is an identifier. Aliases that are engine classes or keywords are refused.
- Commit `gd.json` and `gd.lock`. `gd init` writes `pkg/` into `.gitignore`.
- `--frozen` does not change the lock. For an offline target, fetch first where a network is available, and add `--cached-only`.
- When install, add, or update fails partway, project files and the lock are restored.
- The lock is bound to its registry. Switching to another registry requires explicit lock migration.

### Short import syntax

`@import` is the short form of `const Name = preload(...)`.

It is useful even without external dependencies: see [samples/packages](https://github.com/prog-sha/gd/tree/0.7/samples/packages) for a real registry dependency and its lockfile.

```gdscript
@import greet
@import "./util"
@import greet/style as theme
```

- Unquoted names resolve only aliases declared in the `imports` of `gd.json`. Files with the same name are not searched.
- Quote relative files, starting with `./` or `../`. The `.gd` extension is optional. If a file and directory share a name, specify the file as `"./util.gd"`.
- Without `as`, the identifier is the last segment as written, and a directory holding `mod.gd` binds its directory name.
- `gd fmt` keeps `@import` as it is.
- Upstream Godot does not know `@import`, so files shared with Godot should spell out `const` and `preload`.

### Creating a package

A package is one project rooted at its `gd.json`. `gd init @scope/name` seeds `mod.gd` and a test,
`gd test` runs it, and `gd publish` releases it.

```json
{"name":"@scope/hello","version":"1.0.0","main":"src/mod.gd","include":["src"]}
```

```sh
gd publish
gd add hello gd:@scope/hello@^1.0.0
```

- The entry is `mod.gd`. For multiple files, list files or directories in `include`.
- The main file's directory becomes the package root, so relative preloads inside the package keep working.
- A package may use other registry packages through the `imports` of its own `gd.json`. `gd publish` records those `imports` in the registry.
- `class_name` may be published. Installation checks for conflicts between classes of the same name and rolls everything back on a conflict.
- Setting `godot` to `true` in `gd.json` is the author's declaration that the package runs on upstream Godot without gd's own API, and `gd search` marks it `[godot]`.

A package under development is added from a local path with `gd add ../path`. Its alias comes from the `name` in its `gd.json`.
The checkout is copied under `pkg/<alias>/` and copied again on the next run whenever its content fingerprint changes.
Files starting with `.`, `pkg/`, `tmp/`, subdirectories holding a `gd.json`, and `token` are not copied.
`gd publish` turns a local import into its registry range when the target's `gd.json` has `name` and `version`, and refuses it otherwise.

### Dependency resolution

`gd install` resolves the whole dependency graph. It picks a version in this order: the version `gd.lock` pins, a version already chosen this time that satisfies the range,
then the newest match in the registry.

`gd.lock` also records the resolved `imports` configuration. A run after a configuration change uses the same resolver instead of silently loading an outdated version.
`--frozen` rejects mismatched requests. When multiple aliases name one package, the lexicographically first alias selects its copy directory.

- Pure GDScript packages coexist as distinct versions.
- A native extension loads only once per process, so it is unified to one version. If the ranges cannot agree, it stops before anything is fetched.
- There is no mechanism for plugins that share one host instance (peer dependencies).
- The canonical path of a registry package is `pkg://@scope/name@version/`. `pkg://<alias>/` expands to it through the `imports` of the package the script belongs to. The same alias may name different versions in different packages, and one version is one script however it is reached.
- `gd.lock` also records each package's resolved `imports`, and `gd info` lists them.
- `gd remove` and `gd update` drop what no package uses any more from `gd.lock` and `pkg/`.
- A change in search ranking does not affect installation or lock verification for a known package.

### Sharing a location with Godot

To share a project with tools that only read `res://`, such as upstream Godot, set `"place": "project"` in `gd.json`.
Packages are copied under `pkg/<alias>/`, and both `pkg://` and `res://pkg/` point there.
A package that only other packages use goes under `pkg/@scope/name@version/`.

- Where `project.godot` exists, `place` defaults to `project` and no `.gitignore` is written. Commit `pkg/` so teammates without gd can open the project.
- Installation rewrites `res://` references written in `preload`, `load`, and `extends` to the placement. Strings, comments, and paths built at run time are not rewritten.
- `place` only selects where files live. It does not convert gd's own API or syntax for Godot. Shared source should use standard syntax and relative preloads.

### Native extension packages

- A native extension needs real files to load, so it lives under `pkg/<alias>/` whatever `place` is.
- A script may only name classes of the extensions its own package imports with `ext:`: the project's `gd.json` for project scripts, the package's own `imports` for package scripts.
- An extension outside the registry is usable by project scripts only.
- A registry package's extension that registers a class missing from its manifest's `[classes]` stops startup.
- Fetch on the target OS, or run `gd compile` on the target OS.

### Settings and environment variables

`gd.json` has these ten settings.

| Name | Written by `gd init` / when omitted | Meaning |
|---|---|---|
| `name` | `my-tool` / required | Project name. Publishing needs `@scope/name` |
| `version` | `0.1.0` / required | Package version |
| `tasks` | run and test / none | Commands invoked by `gd task` |
| `imports` | `{}` / `{}` | Alias and dependency source. A published package may name registry packages only |
| `registry` | omitted / environment or the public registry | Registry URL pinned to the project |
| `main` | omitted / `mod.gd` | `mod.gd` or `.gdextension` entry published |
| `include` | omitted / main only | Files or directories inside the main directory included in a pure GDScript package |
| `place` | omitted / `cache`, or `project` beside `project.godot` | Where packages live. `project` copies them under `pkg/` |
| `godot` | omitted / `false` | Declares a package that runs on upstream Godot without gd's own API |
| `description` | omitted / empty | Description shown in the registry |

gd reads these environment variables. A script that reads the environment needs the names allowed with `--allow-env`.

| Variable | Purpose |
|---|---|
| `GD_CACHE_HOME` | Package cache root. Defaults to `gd` in Windows LocalAppData. On macOS/Linux, uses an absolute `XDG_CACHE_HOME` plus `/gd`, or `.gd` under the home directory. The OS account directory is used when `HOME` is unset |
| `GD_REGISTRY` | Registry. Defaults to `https://gd.progsha.com/pkg`. `registry` in `gd.json` wins |
| `GD_TOKEN` | Publish token. Keep it out of config files and pass it only to the publishing process |
| `LC_ALL`, `LANG` | Language of the manual shown by `gd doc` |
| `GD_WORKER` | Internal mark set by `--workers`. Not a user setting |

Remote packages and registries use HTTPS. A development registry on loopback may also use HTTP.
Fetched packages and native libraries are checked against the SHA-256 in the registry index.
A `.gdextension` manifest is limited to 16 MiB, and all files in a package to 500 MiB in total.

### Distributing a single executable

`compile` collects scripts, views, static files, migrations, dependency packages, and the target OS's GDExtensions into one executable. The target needs no cache.

```sh
gd compile -o app main.gd
./app
```

- Every package `gd.json` names, and every package those import, is embedded.
- An embedded Web app can also stay resident with `./app serve --no-scene-tree --allow-net`.
- From a local path package, files starting with `.` such as `.env` and the `token` in `gd.json` are left out.
- Do not embed secrets in source. compile excludes `.env`, but values written in source remain in the executable.

## Scope and reporting

gd is a public release before the API has settled. Do not assume backward compatibility. Changes and the Godot version used as the base
are recorded in the [CHANGELOG](https://github.com/prog-sha/gd/blob/0.7/CHANGELOG.md).
gd is not an official product of the Godot Foundation or the Godot Engine project.

Report bugs in [Issues](https://github.com/prog-sha/gd/issues). Report vulnerabilities that should not be public through
[GitHub private reporting](https://github.com/prog-sha/gd/security/advisories/new).
