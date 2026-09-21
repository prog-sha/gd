# 安全な利用と脆弱性報告

## 対応版

修正は最新のreleaseへ入れます。APIが固まる前の版では、互換性より安全な修正を優先します。

## 脆弱性の報告

公開issueには書かず、GitHubの
[Private vulnerability reporting](https://github.com/prog-sha/gd/security/advisories/new)を使ってください。
利用できない場合は、機密情報を除いた連絡用issueを作り、非公開の連絡方法を相談してください。

次の情報があると確認が速くなります。

- 影響する版、OS、architecture
- 再現に必要な最小のscriptとcommand
- 必要な権限flagと攻撃者が持つ前提
- 読取、書込、実行、通信、停止など想定される影響
- 公開希望日がある場合はその日

受領後7日以内に確認し、影響と修正方針を返します。修正と公開時期は報告者と調整し、
修正版、advisory、creditを同時に公開します。

## 安全な利用

- 未確認のscriptは`--strict`で実行し、必要なmountとallowだけを渡してください。Windowsではmountが未対応なので、fileを`res://`か`user://`へ置いてください。
- `--allow-ext`で許したnative extensionはgdと同じ権限で動きます。
- secretはsource、`gd.json`、command lineへ書かず、許可した環境変数から読んでください。
- 公開serverはloopbackで動作確認してから、必要なaddressだけを`--allow-net`へ指定してください。
- release archiveは`SHA256SUMS`を検証してください。
