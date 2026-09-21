# Windows Package Manager manifests

These manifests target the published 0.7.4-stable archive. The official community catalog entry is not registered yet. Use the [PowerShell installer](https://gd.progsha.com/#en-install) until registration is accepted.

On Windows with winget installed, validate the version directory:

```powershell
winget validate --manifest .\packaging\winget\manifests\p\prog-sha\gd\0.7.4
```

To submit, copy `manifests/p/prog-sha/gd/0.7.4/` to the same path in a fork of [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) and follow its contribution checks. Approval is required before the package appears in the public catalog.

公式カタログへの登録は未完了です。登録までは[PowerShellインストーラー](https://gd.progsha.com/#導入)を利用してください。このディレクトリは公式schemaに合わせた申請用ファイルで、上記の`winget validate`で検証できます。
