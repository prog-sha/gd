# Changelog

公開版の利用者に影響する変更を記録します。版はSemantic Versioningに従います。

## 0.7.5

- macOS配布版でad-hoc署名のGDExtensionとその依存ライブラリを読み込めるよう、CLI専用entitlementを付与します。Hardened Runtimeと公証は維持します。macOSの利用者は新しいarchiveを取得してください。

## 0.7.4

### 失敗の返し方

- 形式変換、復号、URL/query、子process、directory列挙は、失敗してもここまで得た値を捨てません。値とErrを一緒に返します。
- Hex / Base64 / Base32は完了したブロックを部分値として返します。標準とrawの受理形式を区別します。
- Redisのpipelineは常に配列です。途中失敗でも既知の結果を残し、未受信の位置へErrを置きます。WATCH中断と値なしを区別します。
- SQLiteの1行取得は、行が無いとき`NOT_FOUND`です。
- 子processの非ゼロ終了はErrです。終了codeと取得済み出力を失敗時にも返します。

### 権限

- `--strict`では起動directoryだけを暗黙の読取り範囲にします。外のfileは名前付き`--mount`からだけ読めます。

### 導入

- `curl | sh`とPowerShell installerでchecksum照合付きの導入ができます。
- Homebrew tapとapt（glibc 2.38以降）から導入できます。公式winget catalogは未登録です。

### 手引き

- 入門のWeb例は通常実行の`gd serve`です。`--strict`は権限の節で説明します。
- handlerの失敗は`return 値, 失敗`で渡し、HTTP応答への変換境界を手引きに書きました。
- Web、`@import`、packageの実行できるsamplesを追加しました。

## 0.7.3

v0.7.2のscriptはそのままでは動きません。下の「移行」のtoolで名前を移し、「言語とAPI名」の変更を確認してください。

### 移行

- `gd tools/migrate_0_7_3.gd <path>`でv0.7.2の公開型・名前空間・method名をv0.7.3へ移せます。`-- --check <path>`で無変更検査でき、自動変換できない`R.of`と、引数が`Err.`で始まらない`is_kind`は手作業対象として報告します。

### 言語とAPI名

- カンマ戻りを`return 値, 失敗`の二値に固定しました。末尾は`Err`型の値か成功時の`null`に限り、文字列は`Err.err(reason)`で包みます。複数のdataは配列か辞書一つにまとめます。
- 戻り型は`-> int, Err`のように成功型一つと`Err`で書きます。成功値の型は`return`の値から推論し、`R.ok("a")`のような型の合わない成功値はcompile errorになります。型が動的な値は実行時に検査します。
- `R`の分解を`var value, e := call()`の二つの名前に固定しました。`r[0]`、`R.at()`、`R.of()`など可変長の結果APIは廃止です。
- `var a, b, c := 1, "a", 0.0`のように、式を並べて複数の変数を一度に宣言できます。新しい名前を一つ以上含めば、その関数で見えている変数（外側のblockを含む）にも代入でき、右辺は代入の前に全て評価します。定数、引数、lambdaが取り込んだ変数には代入できません。
- `call()!`の失敗は、理由を表示してprogramをその場で止めます（終了code 1）。呼出し元が既定値で続行することはありません。`gd serve`ではそのhandlerだけが失敗します。
- 成功値と失敗を返す関数は全ての経路で`return`が必要です。`?`で伝播するだけの関数も`return null, null`で終えます。`-> int`のように戻り型を一つだけ宣言した関数のカンマ戻りはcompile errorです。
- 未注釈の関数の成功型は、`return other()`だけで転送する関数や`match`の枝、再帰呼出しを含めて全てのreturnから決まり、関数の定義順で変わりません。受け取った成功値は定数扱いになりません。
- `-> R`と宣言した関数は結果だけを返せます。`-> Array[int], Err`へ素のArrayを返すと本家の型付きreturnと同じく変換し、Variantに入ったRは包み直さず転送します。
- `Err.err()`に範囲外の種類を渡すと分類なしになり、`R.err()`は失敗したRを理由に受け、種類を指定すれば付け替えます。
- `Err.make()`を`Err.err()`、`e.is_kind()`を`e.is()`へ改名しました。
- 公開型の名前を`GD`で始めました（`AsyncContext`→`GDAsyncContext`、`CLIFlags`→`GDCLIFlags`、`TestCheck`→`GDTestCheck`など9種）。
- 入口を移しました。memory上の形式変換（CSV、TOML、YAML、JSONLなど）は`GD.file`から`GD.data`へ、`GD.text.html_*`は`GD.html`へ、端末の色と`tty`は`GD.text`から`GD.cli`へ、`GD.postgres`と`GD.redis`は`GD.database.postgres`と`GD.database.redis`へ、`GD.database.sqlite_sync()`は`GD.database.sqlite.open()`へ移りました。
- `?`で失敗を伝播するとき、部分値が呼出し元の成功型に合わなければ部分値を捨てます。失敗の理由と種類は保ちます。
- 型注釈のない関数が別scriptを介して`R`を転送しても、`:=`、`値, Err`、`?`から型を推論するようにしました。通常の未注釈戻り値は`Variant`のままです。

### package

- packageを`pkg://<呼び名>/`で読むようにしました。純GDScript packageは共有cacheへ解決してprojectへ複製せず、`gd.json`の依存がcacheに無ければ最初の実行で取得します。`vendor/`は使いません。
- `"place": "project"`でpackageを`pkg/<呼び名>/`へ置けます。`project.godot`のあるprojectではこれが既定で、`.gitignore`を書きません。native拡張は置き場に関わらず`pkg/<呼び名>/`へ置きます。
- 登録所packageの正式なpathを`pkg://@scope/name@版/`にしました。`pkg://<呼び名>/`はscriptの属するpackageの`imports`で展開され、同じ版はどこから辿っても一つのscriptです。
- packageが自分の`gd.json`の`imports`で他の登録所packageを使えるようにしました。`gd install`は依存graphを解決して`gd.lock`へ固定し、`gd info`が一覧します。純GDScript packageは版ごとに共存し、native拡張は範囲が両立しない版を取得前に拒みます。
- install、add、updateは途中で失敗するとprojectの配置とlockを元へ戻します。`gd remove`と`gd update`は使われなくなったpackageを外します。
- installは静的な`preload`・`load`・`extends`の`res://`参照を配置先へ移します。通常の文字列やコメントは変更しません。
- `gd add <名前> <path>`と`gd add ../path`でlocal packageを足せます。`pkg/<呼び名>/`へ複製し、checkoutが新しければ次の実行で複製し直します。`gd publish`はlocal importを先の`name`と`version`から登録所の範囲に変換します。
- `gd init @scope/name`がpackageの雛形を作ります。`gd.json`の`godot`属性を登録所へ載せ、`gd search`が示します。
- native拡張のclassを、自分のpackageが`ext:`で取り込んだものだけscriptから名指せるようにしました。`class_name`を公開でき、導入時にpackage間の同名宣言を検査します。
- `@import 名前 [as 別名]`を追加しました。`const 名 = preload(...)`の短い形です。引用符の無い名前は`imports`に宣言した呼び名だけを解決し、相対fileは引用符で`./`または`../`から書きます。
- `main`が下位directoryにあるlocal packageも`@import 呼び名`で読めます。引用符付きのpathは`gd.json`を持つ下位directoryも指せ、`.gd`を書いたspecは一つのscriptだけを探します。
- 同じpackageのscriptは、`pkg://<呼び名>/`で読んでも`res://pkg/<呼び名>/`で読んでも一つのclassです。static変数と`is`の判定が二重になりません。
- `@import`の直前に書いたannotationが次のmemberへ付くことがなくなりました。定数に付けられないものはerrorです。予約語になる識別子は`as`を求め、`"./mod.gd"`はそのdirectory名になります。
- preloadした依存scriptが壊れているとき、その最初のerrorを行番号付きで呼び手側のerrorに含めるようにしました。

### compile

- 入口のscriptが`gd.json`より下のdirectoryにあっても、projectの根とpackageを同梱します。
- `gd.json`の名指すpackageと、それらが取り込むpackageを実行体へ同梱します。local pathのpackageからは`.`で始まるfileと`gd.json`の`token`を除きます。
- `.gdextension`の宣言するnative libraryを実行体へ同梱します。同梱できないときは壊れた実行体を作らず、その場で失敗します。

### 非同期

- nativeから呼び返される関数（`Array.map()`のcallback、`_init()`、member初期化、`_to_string()`など）の中で待つmethodを呼ぶとerrorになります。以前は中断状態のobjectが値として返り、誤った値や無限loopの原因になっていました。
- `await`の途中でinstanceが解放された関数は、nullの結果で呼出し元を再開します。以前は呼出し元が永久に待ちました。
- 同じ期限のtimerは作成順に完了し、通常実行でも期限の来たtimerを一度に配ります。
- `GD.async.all()`と`race()`は複数の値を持つsignalの結果を配列で返し、同じsignalを二度渡しても両方の欄を埋め、`race([])`は`-1`で完了します。`with_context()`はsleepなどの待ちも取り消せ、contextの理由をそのまま結果に返します。取り消したprocessは`Err.INTERRUPTED`です。
- 同期形のAPIを`Callable.call()`やCallableを受け取るbuiltin経由で呼んでも`R`が返ります。signalのhandlerとdeferred callの中では従来どおり待てます。
- 呼出しの深さの上限を1024にし、超えるとcrashでなく`Stack overflow`のerrorで止まります。

### Webと通信

- `GDWebWriter.write_text(text, offset, count)`で文字範囲を直接UTF-8へ変換して送信できます。
- I/O待機基盤の回復不能なOSエラーでは、操作名とエラー番号を出してすぐ異常終了します。

### 性能

- HTTPの小さい分割出力をまとめて送り、平文接続の重複する空読みと、固定キー・本文参照・path分割の無駄を省きました。
- ASCII文字列のUTF-8変換、ChaCha20、SHA-224/256をx86_64とARM64のCPU命令で高速化しました。非対応CPUでは汎用処理を使います。

## 0.7.2

- x86_64のAES-NI/PCLMULとARM64のAES/PMULLをCPU機能に応じて選び、非対応環境の算術経路を維持しました。
- TCPの監視変更をまとめ、Linuxではepollの登録を維持したまま関心イベントを更新するようにしました。
- ASCII文字列のUTF-8バッファ作成で中間変換を省きました。
- 公式HTTP比較の巡回数指定、途中再開、CPUプロファイル採取と性能比集計を追加しました。

## 0.7.0

- file・TCP・DB・形式変換・package管理の独自件数／容量制限と子process出力の無通知切捨てを廃止しました。SQLiteは本来の容量設定を使い、結果の制限は明示指定時だけ適用します。
- appendをOSの追記処理へ変更し、遅延書込エラーと、Object文字列化中の同期I/O・所有者解放を正しく処理するようにしました。
- SceneTreeの連続処理と高水準multiplayerを自動駆動し、通信用の固定周期設定を廃止しました。
- `GD.async.spawn()`で起動した長い同期GDScriptをVMの安全点で中断し、event loopへ制御を戻す実行基盤を追加しました。
- `GD.net.dial_tls()`を追加し、証明書と宛名を既定で検証するTLS接続をTCPと同じstream契約で扱えるようにしました。
- `GD.math`と`GD.math.bits`を追加し、標準mathの特殊値とuint64演算を公開しました。
- `GD.data`へSHA-224/384/512、SHA-3、汎用HMAC、可変長PBKDF2、HKDFを追加しました。
- JSON整数の型と64-bit精度を保ち、厳密な符号化の値ごとの余分な確保を削減しました。
- databaseへ`query_row`、接続poolの`stats`、接続を保持して1行ずつ読むGo準拠の`query_rows`を追加しました。
- 通常fileを全量保持せず受付順に読み書きする`GDFileStream`を追加しました。
- JSONの公開入口を`GD.data.json_encode/json_decode`へまとめました。内部toolも同じ厳密JSONを使います。
- HTML templateのpartial読取とCPU解析を分離し、入力辞書の破壊、深い入力の欠落、取消通知の漏れを修正しました。
- logの固定切詰め・破棄をなくし、CPU整形と順序付きI/Oへ分離しました。通常呼出しは完了を待って`R`を返し、並行開始には`*_async`を使います。
- JWTの不整合な長さ制限をなくし、validatorの循環・共有入力処理、sessionの失効判定と保持順序を修正しました。
- OS threadの安全上限をruntime全体へ適用しました。`GD.async.set_max_threads`で変更でき、既定10000・超過時process終了はGoと同じです。
- worker完了の線形探索とqueueコピーを削減し、大きな静的fileと先頭ゼロ付きContent-Lengthを正しく扱うようにしました。
- native worker内で必要になったObject文字列化をmainへ戻し、待機中のCPU枠を他の仕事へ渡すようにしました。未完了仕事を残した終了時のクラッシュも修正しました。
- Redis poolをgo-redisの最大接続数設定に沿う遅延接続と排他貸出しへ変更しました。接続取得の待機・期限・取消を備え、`open()`は接続先の設定結果を`R`で返します。

## 0.2.7

- 標準moduleの大きなI/Oと計算に待てる入口を揃え、Node、timer、通信を止めずにworkerへ渡せるようにしました。
- SQLite、PostgreSQL、Redisは同じ接続の順序を保ち、別接続とevent loopを並列に進めるようにしました。
- Linuxの待受けが無通信時にCPUを使い切る問題を直し、kernelの`epoll`で用件まで眠るようにしました。

## 0.2.6

- 常駐中の大きなfile操作に待てる版（`read_text_async`など）を足し、別threadへ渡してevent loopを止めないようにしました。
- `GD.cli.run()`で外の道具を呼び、終わるまで待っている間も他のrequestが進むようにしました。

## 0.2.4

- 登録所のpackage名判定をclientと同じ安全規則に揃え、Windows予約名、末尾dot、1段の長さ上限を拒否するようにしました。
- package取得の失敗にHTTPの理由を含めるようにしました。

## 0.2.3

- packageの版選択をGoと同じく、canonicalなsemantic versionだけを対象にし、安定版を優先するようにしました。
- 純GDScript packageの公開入口を登録所の`mod.gd`へ正規化し、利用側は`vendor/<呼び名>.gd`から読む形に揃えました。
- packageのfile pathを複数OSで同じ形に展開し、大小文字の衝突やOS固有の名前を拒否するようにしました。

## 0.2.2

- `GDWebApp.limits()`が未知の設定名を拒否するようにしました。
- PostgreSQLの文字列上限をUTF-8の実byteで判定し、送信境界をPostgreSQLと同じ約1 GiBへ揃えました。

## 0.2.1

- 手引きを読者の流れに沿って書き直し、flagの読み方、`--watch`、`eval`、`repl`、`R`の使い方を足しました。
- APIリファレンスで`GD.web`のmethodを用途別の4群に分け、`GD.data`の`json_decode`と`xor_bytes`の分類を直しました。
- HTTP request bodyを接続ごとのstreamとして受信し、全接続共有の64 MiB上限を撤廃しました。
- `read`、`bytes`、`text`、`json`、`save`でbodyを非同期に読み、requestごとに上限を設定できるようにしました。
- 未読bodyの処理、`Expect: 100-continue`、keep-aliveと切断判定をGoのHTTP server設計に揃えました。

## 0.2.0

- 公式マニュアルとREADMEを日本語と英語で用意し、Web版に言語切替を付けました。
- `gd doc`が`LC_ALL`または`LANG`に合わせて手引きとAPI説明の言語を選ぶようにしました。
- 標準入口を`GD`へまとめ、Webとdatabaseは`GD.web`、`GD.database`から使う形へ揃えました。
- 公開型と公式拡張の型・singletonを`GD`で始まる名前へ揃えました。
- 0.2系の旧公開名を当時の現行名へ書き換える移行ツールを追加しました。
- 公式拡張のsourceと試験を`gd-extensions`へ分離し、package登録所から導入する形へ揃えました。
- 通常実行でOSの絶対pathとsymlinkを扱い、`--strict`では名前付きmountへ限定する形へ戻しました。
- `gd task`の標準入出力を子processへ直結し、長時間動くtaskの出力を逐次表示するようにしました。
- JSONの64bit範囲内の整数字面を、小数へ変えず`int`として保持するようにしました。
- 公式HTMLをローカルで生成し、GitHub Pagesは生成済み文書の配置だけを行うようにしました。

## 0.1.3

- Windows x86_64の画面なしCLIとZIP配布へ対応しました。
- Windowsでは通常実行と`--strict`を使えます。名前付き`--mount`は未対応として起動時に拒否します。

## 0.1.2

- PostgreSQLの接続をpool化し、同時queryを1接続へ直列化せず処理できるようにしました。
- PostgreSQL認証でSCRAM-SHA-256に加えてMD5方式へ対応しました。
- 大きなHTTP本文を固定の小さいbufferで逐次受信し、設定した上限まで扱えるようにしました。
- 信頼するproxyを明示したrate limitで、偽装を避けながら元のclient IPを使えるようにしました。
- JWTへ失効確認用の`check`を追加し、password変更やlogout後のtokenを拒否できるようにしました。
- PostgreSQLの行数上限超過で接続を切らず、結果だけを安全に打ち切るようにしました。

## 0.1.1

- `compile`が壊れた`gd.json`を拒否し、tokenを含む設定をそのまま配布しないようにしました。
- 単一実行体を一時fileへ逐次作成し、成功時だけ既存成果物と置き換えるようにしました。
- 日時の加算と差分が64bit整数の端で反対側へ折り返さないようにしました。
- DB制約違反の種別、列、SQLSTATEなどを`Err.info`から判定できるようにしました。
- Discord Bot、Memcached、Supabaseの公式拡張文書をマニュアルから辿れるようにしました。

## 0.1.0

- GDScriptをprojectなしで実行、検査、整形、test、compileできるCLI。
- file、data、非同期処理、日時、ID、log、HTTPの標準API。
- router、middleware、session、rate limit、限定版Handlebarsを持つWeb API。
- 組込みSQLite、PostgreSQL、Redisとdatabase切替API。
- mountとnetwork、環境変数、子process、native extension、system情報の権限分離。
- package管理とnative extensionの取得、固定、publish。
- 端末とWebを共通の正本から作る公式マニュアル・APIリファレンス。

基準にしたGodot Engineは4.7.2です。本家側の変更は
[Godot 4.7.2 changelog](https://godotengine.org/article/maintenance-release-godot-4-7-2/)を参照してください。
