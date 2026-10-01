# Windows Package Manager マニフェスト

[English](README.md) | 日本語

このマニフェストは公開済みの0.7.4アーカイブ向けです。公式コミュニティカタログへの登録は未完了です。承認までは[PowerShellインストーラー](https://gd.progsha.com/#導入)を利用してください。

wingetを導入したWindowsで、バージョンのディレクトリを検証します。

```powershell
winget validate --manifest .\packaging\winget\manifests\p\prog-sha\gd\0.7.4
```

申請する場合は`manifests/p/prog-sha/gd/0.7.4/`を[microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)のフォーク内の同じ場所へコピーし、貢献ガイドに沿って検査してください。承認されると公開カタログに表示されます。
