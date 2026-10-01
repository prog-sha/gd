# gd-godot マニュアル

[English](gd-godot.en.md) | 日本語

`gd-godot` は Godot のエディタ、画面表示、ゲーム実行を担う実行体です。CLI用の `gd` とは別に配布されます。

この文書の機能は開発中です。使い方や仕様は予告なく変わります。

## 導入と起動

```sh
gd install -g gd-godot
gd editor
gd editor path/to/project
gd run-game path/to/project
```

`gd install -g gd-godot` は、CLIと同じ版の公式パッケージをOS・アーキテクチャに合わせて導入します。`gd editor` と `gd run-game` も必要なときにこのグローバル導入処理を使い、以後は同じインストールを再利用します。保存先は通常のグローバルコマンドと同じで、`GD_INSTALL_ROOT` で変更できます。プロジェクトの依存設定には影響しません。ローカル開発でビルド名付きの実行体を使う場合は、隣にある同じビルドの `gd-godot` を優先します。

`gd run-game` はゲームの終了を待ち、その終了コードを返します。ゲームへ渡す引数は `--` の後ろに書きます。

## 企画とアドオン

`gd editor <企画ディレクトリ>` でその企画を開きます。企画ディレクトリで `gd install --godot` を実行すると、`gd.json` の `imports` にあるパッケージを `addons/<呼び名>/` へ複製します。`add`、`update`、`uninstall`、`info` にも `--godot` を指定できます。本家の Godot エディタからも `res://addons/` として読めます。配置先の変更だけで、gd固有のAPIや構文の変換は行いません。

Godotアセットチャンネルから取得するアドオンは `assets` に記録され、ZIPの `addons/` 以下をプロジェクトへ配置します。通常のパッケージは、内部に `addons/` があってもパッケージ全体を配置します。

ソースから画面付き実行体を作る場合は、同じ platform・target・architecture の `gd` と `gd-godot` を並べてください。`scons view=yes` が画面付きビルドを選びます。

## オンライン機能

ゲームの同期とゲスト認証には `Online` と `Secret` を使います。マッチング先の管理には `gd` の `GD.online.match()` を使います。[Online の手引き](online-guide.md)と[詳細](online.md)に構成と設定をまとめています。外部アカウントの紐付けには `Secret` に提供者の設定が必要です。

Godot 標準の高水準マルチプレイを使う場合は、`serve` を付けない通常実行で `ENetMultiplayerPeer` を作り、`get_tree().get_multiplayer().multiplayer_peer` へ設定します。サーバーは既定でクライアント同士の通信を中継します。

## 関連資料

- [gd マニュアル](manual.md): CLI用言語、標準 API、パッケージ管理
- [Online の手引き](online-guide.md): ゲーム通信の構成
