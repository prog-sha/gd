# Changelog

English | [日本語](CHANGELOG.ja.md)

This file records changes affecting users of public releases. Versions follow Semantic Versioning.

## 0.8.0

- Removed `R`. Functions return the success value and `Err` in separate slots. Use `return value` for success, and `return Err("reason")` or `return value, err` for failure.
- Standard modules report failures through `Err`, with cause lookup and support for multiple causes.
- Web responses use `GDWebResponse`; sending uses the body's bytes.
- Added online game synchronization and dedicated servers to `gd-godot`, with guest login and external account linking. These are in development.
- `tools/migrate_to_0.8.0.py` converts unambiguous syntax and reports `R` uses that require review.
- Renamed `gd remove` to `gd uninstall`. It drops a dependency, or removes a command with `-g`.
- `gd upgrade` replaces the running `gd` with the latest release after checking it against the release's SHA-256.
- `gd --help`, `gd -h`, `gd <command> --help`, and `gd help <command>` print usage.
- Unscoped names work in `gd install -g tool` and `gd add tool`. When two or more packages share the name, gd asks which one to use.
- `gd run`, `gd serve`, and `gd eval` without a target print usage and exit. `gd completions` needs a shell name (`bash` or `zsh`).
- Commands and their arguments are validated from one definition. `gd bench` and the `++` delimiter are removed; script arguments follow `--`. `gd eval` receives arguments after `--` as `args`.
- `user://` now lives in the OS user data folder, one per script directory, and survives restarts. It used to be a temporary folder per working directory.
- SQLite databases can live in any location mounted writable with `--mount`, as well as `user://`. `res://` also works without `--strict`.
- After `bytes()`, `text()`, or `json()` has read a request body, every later call returns the same content. `save()` after them writes that body, and whole-body reads after `save()` report that the body went to a file instead of returning nothing.
- Package files are distributed from the publisher's GitHub Release; the registry keeps only each version's release address and file fingerprints. `gd publish` writes the files named by their SHA-256, and `gd publish <release-url>` checks the uploaded files and lists the version.
- `*_async()` and the waits of `GD.async` return a `GDTask` instead of a Signal. The task keeps the result, so it can be awaited after other waits; a Signal forgot a result nobody was listening for. Use `task.cancel()` and `task.finished.connect()`; `GD.async.all()`, `race()`, `with_timeout()`, and `with_context()` accept tasks.
- A GDExtension method declared in `[await]` together with its `*_async` twin waits automatically under its plain name, like standard modules, while the `*_async` name returns a `GDTask`.
- An operation that finishes while its script is preempted in a long computation keeps its result until the script reaches `await` or `GD.async.all()`. The result used to be lost.
- Release executables publish printed output before waiting and before printing errors, so pipes and logs show output in order while a script runs.
- The `gd-godot` editor highlights GDScript and can export scripts as tokens again.
- Native extensions added with `gd add` load their release library in release executables instead of the debug one.
- A reserved mount name is reported with the full list of reserved names and a suggested alternative.

## 0.7.5

- Added a CLI-specific entitlement so macOS distributions can load ad-hoc-signed GDExtensions and their dependencies. Hardened Runtime and notarization remain enabled. macOS users should download the new archive.

## 0.7.4

### Error handling

- Format conversion, decoding, URL/query processing, child processes, and directory listing retain processed values on failure, returning the value with Err.
- Hex, Base64, and Base32 return completed blocks as partial values and distinguish standard and raw input formats.
- Redis pipelines always return arrays, retain known results on failure, and put Err in unread positions. WATCH interruption and missing values are distinct.
- Single-row SQLite queries report `NOT_FOUND` when no row exists.
- Nonzero child-process exits return Err while preserving exit status and captured output.

### Permissions

- `--strict` grants implicit read access only to the initial working directory. Outside files require named `--mount` paths.

### Installation

- Added checksum-verified installation through `curl | sh` and the PowerShell installer.
- Added Homebrew tap and apt installation (glibc 2.38+). The official winget catalog entry is not registered.

### Guides

- Introductory Web examples use normal `gd serve` execution. The permissions section explains `--strict`.
- Documented handler failures using `return value, failure` and the boundary that converts them to HTTP responses.
- Added runnable Web, `@import`, and package samples.

## 0.7.3

Scripts from v0.7.2 do not run unchanged. Use the migration tool below and review the language and API changes.

### Migration

- `gd tools/migrate_0_7_3.gd <path>` migrates v0.7.2 public types, namespaces, and method names to v0.7.3. `-- --check <path>` checks without editing. `R.of` and `is_kind` arguments not starting with `Err.` require manual review.

### Language and API names

- Comma returns are limited to a value and failure. The final value must be Err, or `null` on success; wrap message strings with `Err.err(reason)`. Combine multiple data items in one array or dictionary.
- Return annotations use one success type and Err, such as `-> int, Err`. Success types are inferred from return values; mismatched values such as `R.ok("a")` fail compilation. Dynamic values are checked at runtime.
- `R` destructuring uses exactly two names, as in `var value, e := call()`. Removed variable-length result APIs such as `r[0]`, `R.at()`, and `R.of()`.
- Multiple variables can be declared with expressions such as `var a, b, c := 1, "a", 0.0`. With at least one new name, declarations may also assign existing variables in the function, including outer blocks. All right-hand expressions are evaluated first. Constants, arguments, and lambda captures cannot be assigned this way.
- Failed `call()!` displays the reason and stops the program with exit code 1; the caller does not continue with a default value. Under `gd serve`, only that handler fails.
- Functions returning a value and failure require a return on every path. Functions that only propagate with `?` end with `return null, null`. Comma returns from a function annotated with one type, such as `-> int`, fail compilation.
- Unannotated success types are inferred from every return, including forwarded calls, match branches, and recursion, independently of definition order. Received success values are not treated as constants.
- Functions annotated `-> R` can return results only. Returning an ordinary Array to `-> Array[int], Err` converts it like a built-in typed return. An R stored in Variant is forwarded without rewrapping.
- Out-of-range kinds passed to `Err.err()` produce an unclassified error. `R.err()` accepts a failed R as its reason and can replace its kind.
- Renamed `Err.make()` to `Err.err()` and `e.is_kind()` to `e.is()`.
- Nine public types now start with `GD`, including `AsyncContext` → `GDAsyncContext`, `CLIFlags` → `GDCLIFlags`, and `TestCheck` → `GDTestCheck`.
- Moved in-memory formats (CSV, TOML, YAML, JSONL, etc.) from `GD.file` to `GD.data`; `GD.text.html_*` to `GD.html`; terminal colors and `tty` from `GD.text` to `GD.cli`; `GD.postgres` and `GD.redis` to `GD.database.postgres` and `GD.database.redis`; and `GD.database.sqlite_sync()` to `GD.database.sqlite.open()`.
- Propagation with `?` discards a partial value if it does not match the caller's success type, preserving the error reason and kind.
- Unannotated functions forwarding `R` across scripts infer types through `:=`, value/Err returns, and `?`. Ordinary unannotated returns remain Variant.

### Packages

- Packages use `pkg://<alias>/`. Pure GDScript packages resolve to a shared cache rather than project copies. Missing `gd.json` dependencies are downloaded on first execution. `vendor/` is no longer used.
- `"place": "project"` installs packages under `pkg/<alias>/`. This is the default for projects with `project.godot` and does not write `.gitignore`. Native extensions always use `pkg/<alias>/`.
- Canonical registry paths use `pkg://@scope/name@version/`. `pkg://<alias>/` resolves through the owning package's imports; the same version always identifies one script.
- Packages can depend on registry packages through their own `gd.json` imports. `gd install` resolves the graph and pins it in `gd.lock`; `gd info` lists it. Pure GDScript versions coexist; incompatible native-extension ranges are rejected before download.
- Failed install, add, and update operations restore project placement and the lockfile. Remove and update discard unused packages.
- Installation rewrites static `res://` references in preload, load, and extends to their installed location, leaving ordinary strings and comments unchanged.
- `gd add <name> <path>` and `gd add ../path` add local packages, copying them into `pkg/<alias>/` and refreshing newer checkouts on subsequent execution. Publish converts local imports to registry ranges using their name and version.
- `gd init @scope/name` creates a package starter. The registry stores the `godot` property and search displays it.
- Scripts can name native-extension classes only when imported by their package through `ext:`. Packages may export `class_name`; installation checks for conflicting declarations.
- Added `@import name [as alias]` as shorthand for a preload constant. Unquoted names resolve only declared imports; quoted relative paths start with `./` or `../`.
- Imports work when a local package's main script is in a subdirectory. Quoted paths can identify subdirectories containing `gd.json`; specs ending in `.gd` identify one script.
- Package scripts loaded through `pkg://<alias>/` and `res://pkg/<alias>/` identify the same class, preserving static state and type identity.
- Annotations immediately before imports no longer attach to the next member. Annotations unsuitable for constants fail. Reserved identifiers require `as`; `"./mod.gd"` takes the directory's name.
- Errors in preloaded dependencies now include the first dependency error and its line number in the caller's diagnostic.

### Compilation

- Includes the project root and packages even when the entry script is below the directory containing `gd.json`.
- Bundles named packages and their transitive imports. Local packages exclude dot-prefixed files and `gd.json` tokens.
- Bundles native libraries declared by `.gdextension`. Missing libraries fail compilation instead of producing a broken executable.

### Async

- Waiting calls inside callbacks invoked from native code, including Array.map callbacks, initializers, member initialization, and `_to_string()`, now fail instead of returning suspended objects that could cause invalid values or infinite loops.
- Awaiting callers resume with null when the awaited instance is freed, instead of waiting indefinitely.
- Timers with equal deadlines complete in creation order; normal execution dispatches due timers together.
- All and race return arrays for multi-value Signals, fill both positions for duplicate Signals, and complete `race([])` with `-1`. Context cancellation also interrupts sleep and retains the context reason. Cancelled processes report `Err.INTERRUPTED`.
- Synchronous APIs invoked through Callable.call or built-ins accepting Callables return R. Signal handlers and deferred calls retain waiting support.
- Calls exceeding depth 1024 stop with `Stack overflow` instead of crashing.

### Web and networking

- `GDWebWriter.write_text(text, offset, count)` directly encodes and sends character ranges as UTF-8.
- Unrecoverable OS failures in the I/O waiting infrastructure report the operation and error number and exit immediately.

### Performance

- Batched small HTTP writes and removed redundant empty reads on plain connections, repeated fixed-key work, body references, and path splitting.
- Accelerated ASCII UTF-8 conversion, ChaCha20, and SHA-224/256 with x86_64 and ARM64 CPU instructions, retaining generic paths on unsupported CPUs.

## 0.7.2

- Selects x86_64 AES-NI/PCLMUL and ARM64 AES/PMULL according to CPU capabilities, retaining arithmetic paths elsewhere.
- Batches TCP monitoring changes; Linux updates interested events without removing epoll registrations.
- Removed intermediate conversion when building ASCII UTF-8 buffers.
- Added round selection, resumption, CPU profiling, and performance-ratio summaries for official HTTP comparisons.

## 0.7.0

- Removed custom file, TCP, database, format, and package count/capacity limits and silent child-output truncation. SQLite uses its own capacity settings; result limits apply only when explicitly requested.
- Uses OS append operations and correctly handles delayed write errors, synchronous I/O during object stringification, and owner destruction.
- Automatically drives continuous SceneTree processing and high-level multiplayer, removing fixed network polling intervals.
- Long synchronous scripts started with spawn yield at VM safe points to return control to the event loop.
- Added dial_tls with default certificate and hostname verification and the same stream contract as TCP.
- Added `GD.math` and `GD.math.bits`, including special math values and uint64 operations.
- Added SHA-224/384/512, SHA-3, general HMAC, variable-length PBKDF2, and HKDF to `GD.data`.
- Preserves JSON integer types and 64-bit precision while reducing allocations in strict encoding.
- Added query_row, pool stats, and Go-style sequential query_rows that retain a connection.
- Added GDFileStream for ordered file reads and writes without retaining the complete file.
- Consolidated JSON APIs in `GD.data.json_encode/json_decode`; internal tools use the same strict JSON implementation.
- Separated HTML partial reads from CPU parsing; fixed dictionary mutation, missing deep input, and lost cancellation notifications.
- Removed fixed log truncation and dropping. Formatting runs separately from ordered I/O. Ordinary calls wait and return R; `*_async` starts concurrent work.
- Removed inconsistent JWT length restrictions and fixed validator handling of cyclic/shared input and session expiration/order retention.
- Applied the OS-thread safety limit runtime-wide. set_max_threads can adjust it; the default 10000 and process exit on overflow match Go.
- Reduced linear worker-completion searches and queue copying; fixed large static files and Content-Length with leading zeros.
- Returns worker-side object stringification to the main thread and releases waiting CPU slots to other work. Fixed shutdown crashes with unfinished work.
- Redis pools use lazy connections and exclusive checkout according to go-redis's maximum-connection settings, with acquisition waiting, deadlines, and cancellation. Open returns connection configuration results as R.

## 0.2.7

- Standardized waiting APIs for large I/O and computation, allowing worker execution without stopping Nodes, timers, or networking.
- SQLite, PostgreSQL, and Redis preserve same-connection order while other connections and the event loop proceed concurrently.
- Fixed Linux listeners consuming all CPU while idle by waiting in kernel epoll.

## 0.2.6

- Added waiting versions of large file operations, such as read_text_async, that use other threads without blocking the event loop.
- Added external-command execution through GD.cli.run while other requests continue.

## 0.2.4

- Registry package names use the client's safety rules, rejecting Windows reserved names, trailing dots, and excessive segment lengths.
- Package download failures include the HTTP reason.

## 0.2.3

- Package version selection considers canonical semantic versions and prefers stable versions, matching Go.
- Pure GDScript package entry points use registry mod.gd; consumers read vendor aliases.
- Package extraction uses consistent paths across OSes and rejects case collisions and OS-specific names.

## 0.2.2

- GDWebApp.limits rejects unknown settings.
- PostgreSQL string limits use actual UTF-8 byte counts and match its roughly 1 GiB transmission boundary.

## 0.2.1

- Reorganized the manual around readers' workflow and added flags, watch, eval, repl, and R usage.
- Grouped GD.web reference methods into four categories and corrected GD.data json_decode/xor_bytes categories.
- HTTP request bodies stream per connection instead of using a shared 64 MiB limit.
- Read, bytes, text, json, and save read bodies asynchronously with request-specific limits.
- Unread-body handling, Expect: 100-continue, keep-alive, and disconnect detection follow Go's HTTP server design.

## 0.2.0

- Added English/Japanese manuals and READMEs and Web language switching.
- gd doc chooses its manual and API language from LC_ALL or LANG.
- Consolidated standard entry points under GD, including GD.web and GD.database.
- Public and official-extension types and singletons use GD-prefixed names.
- Added migration tools for earlier 0.2 public names.
- Moved official-extension source and tests to gd-extensions for registry installation.
- Normal execution supports OS absolute paths and symlinks; strict execution uses named mounts.
- gd task connects child standard I/O directly, displaying long-running output incrementally.
- JSON integer literals within the 64-bit range remain int without floating-point conversion.
- Generates official HTML locally; GitHub Pages only places generated documents.

## 0.1.3

- Added the Windows x86_64 CLI and ZIP distribution.
- Windows supports normal and strict execution; unsupported named mounts fail at startup.

## 0.1.2

- PostgreSQL queries use connection pools rather than serializing all work on one connection.
- Added MD5 authentication alongside SCRAM-SHA-256.
- Large HTTP bodies stream through small buffers up to the configured limit.
- Explicit trusted proxies protect client-IP-based rate limits from spoofing.
- Added JWT revocation checks to reject tokens after password changes and logout.
- PostgreSQL row-limit overflow safely truncates results without closing the connection.

## 0.1.1

- Compile rejects broken gd.json and avoids distributing configurations containing tokens unchanged.
- Standalone executables are built incrementally into temporary files and replace existing output only on success.
- Date arithmetic no longer wraps at 64-bit integer boundaries.
- Err.info identifies database constraint kinds, columns, and SQLSTATE.
- The manual links to Discord Bot, Memcached, and Supabase extension documentation.

## 0.1.0

- CLI execution, checking, formatting, testing, and compilation without a project.
- Standard file, data, async, time, ID, logging, and HTTP APIs.
- Web APIs with routing, middleware, sessions, rate limiting, and a limited Handlebars implementation.
- Embedded SQLite, PostgreSQL, Redis, and database switching.
- Separate permissions for mounts, networking, environment, child processes, native extensions, and system information.
- Package and native-extension installation, pinning, and publishing.
- Terminal and Web manuals/API references generated from shared sources.

The base Godot Engine version is 4.7.2. For upstream changes, see the
[Godot 4.7.2 changelog](https://godotengine.org/article/maintenance-release-godot-4-7-2/).
