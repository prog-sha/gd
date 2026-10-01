# Smoke tests

English | [日本語](README.ja.md)

Check the basic behavior of a distributed executable or one built from public source. The suite uses only Python 3.9+ standard libraries and the gd executable under test. Go, Deno, a shell, development tools, external databases, and external network access are unnecessary.

Run from the repository or extracted distribution root:

```sh
uv run --no-project python tests/run.py --gd ./gd
# Source build example:
uv run --no-project python tests/run.py --gd bin/gd.macos.template_release.arm64
```

You can also run `python tests/run.py --gd ./gd` directly. On Windows, set `--gd gd.exe` or the executable path. Linux source builds use `bin/gd.linuxbsd.template_release.arm64` or `bin/gd.linuxbsd.template_release.x86_64` according to the architecture.

| Area | Behavior checked |
|---|---|
| language | Type inference, typed collections, multiple return values, Err propagation |
| data | Unicode, NUL, JSON, duplicate keys, invalid JSON/UTF-8, hex/base64, SHA-256 |
| async | Automatic waiting, explicit await, ordinary Signals, multiple waiters, cancellation |
| storage | Strict write denial, temporary file operations, SQLite binding, commit, rollback, close |
| isolation | Strict denies reads and metadata outside the working directory and parent traversal; `-A` preserves this boundary |
| scene | Node tree membership, SceneTree timers and await |
| web | Serve without SceneTree, loopback HTTP, ordinary/async JSON bodies, 400/404 responses, shutdown |
| compiler/checker | Rejection of invalid return types, failure exit status |
| package | Local dependency installation, frozen/cached-only installation, import execution |

All 18 checks, including type checking of valid scripts, must pass. `bad_type.gd` and `check_failure.gd` intentionally fail; do not expect their standalone execution to succeed. Checks compare exit status and completion markers instead of relying on enabled assertions. Failure-detection fixtures are checked too.

Each child process has a default 20-second timeout and is stopped and reaped afterward. Use `--timeout 60` for a slower environment. Timeouts and unavailable loopback access are failures, never successful skips. Temporary files, caches, logs, and `results.json` remain under `tmp/release-check-*`. The suite does not modify source or the user's package cache.

These checks cover basic public API behavior. They do not guarantee the full regression, performance, load, TLS, external database, extension, or OS feature coverage.

Check the source distribution's Web sample over HTTP with the same executable. This verifies HTML, JSON, Unicode, escaping, and 404 responses and always stops the server afterward:

```sh
uv run --no-project python tests/sample.py --gd ./gd --sample samples/web
```
