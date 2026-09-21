# 公開契約テスト / Public acceptance tests

配布実行体、または公開ソースからビルドした実行体の基本動作を検査します。Python 3.9 以上の標準ライブラリと、検査対象の `gd` だけを使います。Go、Deno、shell、開発用ツール、外部 DB、外部ネットワークは不要です。

リポジトリまたは展開した配布物のルートから実行します。

```sh
uv run --no-project python tests/release/run.py --gd ./gd
# Source build example:
uv run --no-project python tests/release/run.py --gd bin/gd.macos.template_release.arm64
```

Python を直接使う場合は `python tests/release/run.py --gd ./gd` でも実行できます。Windows では `--gd gd.exe` または実行体の path を指定します。Linux の source build では `bin/gd.linuxbsd.template_release.x86_64` を指定します。

| 対象 | 確認する契約 |
|---|---|
| language | 型推定、typed collection、多値戻り値、Err 伝播 |
| data | Unicode・NUL・JSON、反復キー、不正 JSON/UTF-8、hex/base64、SHA-256 |
| async | 自動待機、明示 await、通常の Signal、複数待機、取消 |
| storage | strict の書込み拒否、一時領域での file 操作、SQLite bind・commit・rollback・close |
| isolation | strict で起動directory外の読取り・metadata取得・親への移動を拒否、`-A`でも境界を維持 |
| scene | Node の tree 参加、SceneTree timer と await |
| web | SceneTree なしの serve、loopback HTTP、通常/async JSON 本文、400/404、停止 |
| compiler/checker | 不正な戻り型の拒否、検査失敗の終了 code |
| package | ローカル依存の install、frozen/cached-only install、import の実行 |

正常 script の型検査も含めて 18 項目です。`bad_type.gd` と `check_failure.gd` は意図的に失敗する fixture なので、単独実行の成功を期待しないでください。合否は `assert` の有効化に依存せず、終了 code と完了 marker を照合します。失敗検出用 fixture 自体も検査します。

各子 process は既定 20 秒で timeout し、停止・回収します。低速な環境では `--timeout 60` のように変更できます。時間切れや loopback の利用不可は失敗とし、skip で成功にはしません。作業 file、cache、log、`results.json` は `tmp/release-check-*` に残します。ソースと利用者の package cache を書き換えません。

この suite は公開 API の基本契約を確認します。全回帰、性能、負荷、TLS、外部 DB、拡張、各 OS の全機能を保証するものではありません。

The suite checks public CLI and runtime contracts using only Python 3.9+ and the executable under test. Run the command above from the source or extracted distribution root. Set `--gd` to the executable path; use `--timeout` to adjust the per-process deadline.

All 18 checks must pass. Expected compiler/checker failures are explicitly verified. Temporary files, package caches, logs, and the executable fingerprint stay under `tmp/release-check-*`. No external services or development repository are required. This is a bounded acceptance suite, not the complete regression or performance suite.


公開ソースのWebサンプルは、同じ実行体で次のように検査できます。HTML・JSON・Unicode・escape・404をHTTP経由で確認し、終了時にserverを停止します。/ Check the source distribution's web sample over HTTP; the runner always stops its server.

```sh
uv run --no-project python tests/release/sample.py --gd ./gd --sample samples/web
```
