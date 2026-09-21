# 貢献方法 / Contributing

このリポジトリは公開用の製品ソースと利用文書を収録します。開発用ツール、内部テスト、作業記録は別の開発リポジトリで管理します。

同梱の[公開契約テスト](tests/release/README.md)は、公開ソースと実行体だけで実行できます。修正案の動作確認にも使ってください。

不具合や提案は [Issues](https://github.com/prog-sha/gd/issues) へ、版・OS・最小の再現コード・期待した動作を添えて報告してください。修正案は公開ソースに対する差分で示せます。取り込み前に開発側で検証します。

脆弱性は公開 issue へ書かず、[SECURITY.md](SECURITY.md) に従ってください。ビルド手順は [README](README.ja.md#ソースからのビルド) にあります。

This repository contains public product source and user documentation. Development tools, internal tests, and work records are maintained separately.

The bundled [public acceptance tests](tests/release/README.md) run with only the public source and executable, and can also validate proposed fixes.

Use [Issues](https://github.com/prog-sha/gd/issues) for bugs and suggestions. Include the version, OS, a minimal reproducer, and expected behavior. Proposed fixes can be submitted as source diffs; maintainers validate them in the development repository before integration.

Follow [SECURITY.md](SECURITY.md) for vulnerability reports. See [README](README.md#build-from-source) for build instructions.

安定版は`0.7`のようなmajor.minor branchで管理し、`0.7.4-stable`のようなpatch tagはそのbranch上のcommitを指します。公開済みtagは移動しません。公開Actionsは手動の実機検査のみです。

Stable releases use a major.minor branch such as `0.7`. Patch tags such as `0.7.4-stable` point to commits on that branch and remain immutable. Public Actions run native acceptance checks manually.

PRは次minor開発用の`master`へ送ってください。安定版の取得・patch修正は`0.7`を使います。`master`は未公開の0.8開発版です。patch枝は作りません。

Target pull requests at `master`, the unstable development branch for 0.8. Use `0.7` for stable source and maintenance. Patch versions share that branch; tags use `number-status`, without a `v` prefix.
