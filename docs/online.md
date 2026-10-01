[English](online.en.md) | 日本語

最初に読む[手引き](online-guide.md)もあります。

# Godot Online 4.7.2

サーバ権威のオンラインゲームを、通常のGodotの書き方のまま作るためのフォークです。
`rpc`も番号も接続コードも書きません。印を付けた値と関数だけが同期します。

## 世界の形

```
Node2D
└── Online          ← Online側の世界。ここのgdにOnline側の処理を書く
    ├── Secret      ← Online側だけで動く。ログイン処理や鍵はここ
    └── Paddle_7    ← spawn()で持ち主を付けたNode
```

`Online`と`Secret`は組み込みNodeです。エディタの「ノードを追加」から置けます。
**根が`Online`のsceneが世界です。**projectのどこに置いても、名前も自由で、1つだけ置きます。本文はその根に付けます。
置き場の決まりは無く、`samples/`の`online/`フォルダはただの整理です。

- `Online`の下は`Secret`以外すべて、生成も値も全員へ同期します。
- 物を置けるのはOnline側だけです。`spawn()`が作る物は`Online`の下に入ります。どのsceneでも渡せます。
- 持ち主を付けて置いたNodeだけは、その人からの申告を信じます。
- `Secret`はClientへ渡りません（「配布境界」）。

## 覚える名前

本文に書く名前はこれで全部です。細かい意味は下の「印」から先にあります。サーバの実行と`Secret`の設定は「サーバ実行」にあります。

| 種類 | 名前 |
| --- | --- |
| 印 | `@online` `@online_my` `@online_save` `@online_save_my` `@online_input` |
| 型 | `Online` `Secret` |
| 全Nodeで使える関数 | `spawn(scene, my, properties)`（後ろは型で読む。どれも省ける） |
| 世界を指す語 | `world`（その物が居る`Online`。どの深さからでも同じ物） |
| 全Nodeのsignal | `disconnected` `online_changed(field, value, previous)` |
| 本人を指す語 | `my`（`my.id` `my.guest` `my.name`） |
| 外部口座 | `Online.login_with("提供者名")` `Online.logout()`（「Login」を参照） |
| `Online`のsignal | `player_joining(my)` `player_left(my)` `logged_in(my)` `login_failed(reason)` `spawned(node)`。`_on_<名前>`を書けば繋がる |
| `Secret`のインスペクタ | 「Secretのインスペクタ」の19個。設定はここにしかない |

本人を指す語は`my`だけです。`@online_my`・`@online_save_my`・`my`が同じ綴りで揃います。
持ち主のいる物では、`my`はどの関数でもその持ち主です。`@online_my`の値と同じく、Online側と本人の手元にあり、他人の手元では`null`です。
持ち主のいない物（`Online`そのもの、足場）では、`@online func`の中で呼んできた人を指します。
`player_joining`・`player_left`・`logged_in`の`my`はその合図の人です。持ち主を決めるときも`my`をそのまま渡します。
世界そのものは`world`です。`world.playing`のように、親を辿らずに世界の値へ届きます。
持ち主の物の中の`Camera2D`/`Camera3D`は、本人の手元でだけ生きます。他人の物と専用サーバでは切れるので、
「自分のカメラだけ有効にする」を書く場所はありません。

## 印

値に付ける印は5つです。`_my`は配る相手を本人だけに絞り、`_save`はデータベースへ残します。
`_input`だけは向きが逆で、本人の手元が書いた値がOnline側へ届きます。

| 印 | 誰に配るか | 保存 |
| --- | --- | --- |
| `@online var hp` | 全員 | しない |
| `@online_my var quiz_answer` | 本人とOnline側だけ | しない |
| `@online_save var level` | 全員 | する |
| `@online_save_my var robux` | 本人とOnline側だけ | する |
| `@online_input var axis` | 本人の手元が書き、Online側が読む。配らない | しない |

`@online_input`は入力のための値です。`bool` `int` `float` `Vector2` `Vector3`だけで、本人の手元が書くと20回/秒で
変わった分だけOnline側へ届きます。他人の手元で書いても何も起きません。通信が切れると宣言の初期値へ戻るので、
押しっぱなしのまま走り続けません。申告は本人の言い分なので、信じる範囲はOnline側で決めます（`clampf`など）。

```gdscript
@online_input var axis := 0.0       # 本人の左右入力

func _process(_delta):
	axis = Input.get_axis("ui_left", "ui_right")

@online
func _physics_process(delta):
	velocity.x = clampf(axis, -1.0, 1.0) * 220.0
```

跳びや攻撃のような**出来事**は`@online func`で頼みます。押している間ずっと続く**状態**が`@online_input`です。

配れる値と残せる値は同じです。`bool` `int` `float` `String` `StringName` `Vector2/3/4` `Color` `Transform2D/3D`
`Array` `Dictionary` `Packed*Array`など、Variantの二進形にできる物です。`Object`（`Node`、`Resource`、自作class）、`RID`、`Callable`、`Signal`は配れず残せません。
型を書いた変数はその場で止まります。型を書かない変数にそれらが実行時に入ったときは、その値だけ配らず残さず、Online側に1度知らせます。
`Array`と`Dictionary`は中身まで同じ規則です。自作classの中身を残したいときは`Dictionary`にします。

どこに書いたかで、残る場所が決まります。

| 書いた本文 | `@online_save` | `@online_save_my` |
| --- | --- | --- |
| `Online`（世界そのもの） | 世界の箱に`Online.<名前>` | 書けない。世界に持ち主はいない |
| sceneに置いた固定のNode | 世界の箱に`<場所>.<名前>` | 持ち主がいないので配られない。残る場所は左と同じ |
| `spawn(scene, my)`した持ち主のいる物 | 口座に`<種類>.<名前>`。全員へ配る | 口座に同じ。本人にだけ配る |
| `spawn(scene, {"name": "Chest"})`した名前のある物 | 世界の箱に`<場所>.<名前>` | 持ち主がいないので配られない。残る場所は左と同じ |
| `spawn(scene)`した名前の無い物 | 残らない。Online側に1度知らせる | 同じ |
| `Secret` | 世界の箱に`<場所>.<名前>`。Clientへは出ない | 書けない |

`Secret`の中の関数に付けた`@online`だけは配る印ではありません。
「世界の側から呼べる唯一の入口」を示します（「Secret」を参照）。
どちらの意味かは1行目の`extends`が決めるので、別の綴りはありません。

signalに付けられるのは`@online`と`@online_my`の2つです。
`@online_save`と`@online_save_my`をsignalに付けると
`Annotation "@online_save" cannot be applied to a signal.`で止まります。

```gdscript
@online signal shout(word)      # Online側が鳴らすと全員に届く
@online_my signal whisper(word) # Online側が鳴らすと本人にだけ届く
```

関数に付けられるのは`@online`と`@online_my`です。`@online func`はClientからOnline側へ渡る唯一の道で、`rpc`の代わりです。
`@online_my func`は**本人の手元だけで走る本文**です。自分の物だけに要る処理（自分の目印、カメラ、画面への登録）をここに書きます。
ほかの人の手元と専用サーバでは何も起きません。本人かどうかを尋ねる関数はありません。この場所で書きます。
`_ready`のようなengineが呼ぶ本文なら、印の無い同じ名前と並べて書けます。全員の本文の後に本人の分が走ります。

```gdscript
@online_my
func _ready():
	$Mine.visible = true        # 自分の物の手元だけ
```

```gdscript
## 手元で入力を見て、Online側へ渡す
func _process(_delta):
	if Input.is_action_just_pressed("jump"):
		jump()

@online
func jump():                    # ここから先はOnline側で走る
	velocity.y = -400.0
```

答えが要るときは`await`で受け取ります。`@online`系の関数はすべて同じ書き方です。

```gdscript
@online
func hit(amount):               # Online側で走り、答えが返る
	hp -= amount
	return hp

func _process(_delta):
	if Input.is_action_just_pressed("attack"):
		print("のこり", await hit(3))
```

答えを使わないときは`await`を書かなくてもそのまま呼べます。
`@online func`の本文が中で`await`しても、答えは待ち終わってから返ります。

`@online func`はホストが動かしているので、中では`spawn()`がそのまま使えます。
中継の関数は要りません。置く物は**sceneをそのまま渡します**。

```gdscript
const TAMA := preload("res://Tama.tscn")

@online
func shot():
	spawn(TAMA, my, {"position": position, "angle": angle, "speed": 200.0})
```

`spawn(scene, ...)`の後ろは、印と同じく順ではなく**型**で読みます。`my`かその人の持ち物なら持ち主、辞書なら初期値です。
上の弾は撃った本人の物になります。持ち主のいない物（標的や足場）は`spawn(scene)`です。
sceneの根の名前がその種類の名前になり（`Paddle.tscn`の根`Paddle`なら`Paddle_7`）、保存値もその名前で残ります。
同じ根の名前の別のsceneは置けません。根が`Secret`のsceneも置けません。
置けない物（数や深さの上限を越えた物）は`null`を返し、理由をOnline側に1度出します。

**入ってきた人に1つ置くだけなら、`Secret`のインスペクタの`player_scene`にそのsceneを入れます。**
engineが`player_joining`の前にその人の物として置くので、`_on_player_joining`は要りません（`samples/shobon_jump`）。
置きかたを自分で決めたいときだけ`_on_player_joining`で`spawn()`します（`samples/pong`）。
`{"name": "Chest"}`のように名前を付けると、その名前が世界での場所になり、`@online_save`の値がそこに残ります。
名前は英数字と`_`で、同じ親の下に1つです。付けない物の名前は起動ごとの番号なので、保存値は残りません（Online側に1度知らせます）。

### 印の書き方

- `@online`が1つでもある本文の`Node2D`と`Node3D`は、位置と向きを**何も書かなくても配ります。**寄せかたも自動で決まります。動く物はこれで済みます。
  配らない物にだけ`@online("auto_off")`を1行書きます（手元だけで動かす演出など。書かないと手元の`position`は読み取り専用です）。
  `@online`の無い本文はただのGodotのNodeで、何も配りません。
- `@online(position)`はNodeプロパティを名指しで同期します。回数や寄せかたを自分で決めたいときに使います。
- `@online($Name.text)`は子Nodeプロパティを直接同期します。
- `@online var 角度 := $体.rotation`と初期値に場所を書いても、同じ場所へ映ります。型はその初期値で決まります
  （`$Score.text`なら`String`）。数のまま持ちたいときは引数の形です。
- 表示先は別のsceneでも構いません。`@online_my($"/root/Main/ui/得点".text) var 得点 := "0"`のように
  絶対pathで書けば、Online側が決めた値がそのまま画面のUIへ入ります。`_my`を付ければ本人の画面だけです。
  その場所が無い側（専用サーバ、他人の手元）では映さないだけで、値は同じに持ちます。
- 表示先は見せるための場所なので、型は自動で合わせます。`@online($Score.text) var score := 0`は
  数のまま持ったままLabelへ映るので、`str()`を書く場所はありません。
- 印の付いた変数はインスペクタにも出ます（`@export`を書いたのと同じ）。sceneに書いた値が初期値です。
  初期値に場所を書いた変数はその場所の値が初期値なので出ません。`@export_range`などを自分で書けばその形で出ます。
- `@online func`の引数にはNodeを渡せます。世界の物には両側に同じ通し番号があるので、
  番号にして渡し、受け取る側で同じ物へ戻します。世界の外の物は渡せません。
  1回の呼びの引数は16KBまでで、越えた呼びは答えが`null`で戻ります。

### 位置と向きは書かなくても配る

**`@online`が1つでもある本文のNodeは、その型に合う持ち物をエンジンが配ります。**作者はどれを配るか決めません。

| Nodeの型 | 配るもの |
| --- | --- |
| `Node2D` / `Node3D` | `position` と `rotation` |
| うち`CharacterBody2D` / `CharacterBody3D` | 上に加えて、床の上かと、そのときの`velocity` |
| `Control`とそれ以外 | 何も配らない。要るなら`@online(position)`と名指しする |

配らない`Node2D`/`Node3D`には`@online("auto_off")`を1行書きます。位置の無いNodeに書くと、何もしないことを1度知らせます。

変わらない物は最初の1回で止まるので、向きを使わない世界でも通信は増えません。

`CharacterBody`では、手元の見せかたが横と縦で分かれます。
**跳んだ場所も二段ジャンプも着地点も数える必要はありません。**どれも速度の変化から出ます。

| | 手元の見せかた |
| --- | --- |
| 横 | 届いた位置へ寄せるだけ。他人の空中制動は当てにいきません |
| 縦 | 床の上では寄せるだけ。空中では重力で手元が進めます |

横を当てにいかないのは、他人の入力が手元に無いからです。当てられない物は当てず、
届いた位置へ寄せます。横のずれは目に付きませんが、縦の弧は途中で折れると目に付きます。

落下加速度はゲームの定数なのでエンジンからは読めませんが、空中のsample同士の速度差が
それそのものなので測っています。上向きは`up_direction`から読むので運びません。

空中を進めるのは床の上でない間だけです。地上を歩いている物まで長く先読みすると、
届かなくなったときに滑って見えます。

## Onlineのsignal

サーバからの指令はここに集まります。`_ready`と同じように、`_on_<名前>`という関数を書けば繋がります。
インスペクタで繋いでも同じで、二重には鳴りません。engine本来の合図（`pressed`など）はいつもどおりインスペクタです。

| signal | いつ | どこで鳴るか |
| --- | --- | --- |
| `player_joining(my)` | 誰かが入ってきた。この人を持ち主にして`spawn()`する番 | Online側 |
| `player_left(my)` | 誰かが抜けた | Online側 |
| `logged_in(my)` | 自分のloginが済んだ。ここまで待ってから遊び始める。外部loginで口座が結ばれたときも、もう1度鳴る | 自分のClientだけ |
| `login_failed(reason)` | 外部loginが済まなかった | 自分のClientだけ |
| `spawned(node)` | 配りでNodeが現れた | 全Client |

どのNodeにもある`disconnected`は、持ち主の通信が切れたときにOnline側と本人のClientで鳴ります。
`_on_disconnected`を書けば繋がります。`@online`を付ければOnline側で、付けなければ本人の手元で受けます。

```gdscript
## player_joiningを受ける形。合図が鳴る側(Online側)で走る
extends Online

func _on_player_joining(my):
	var paddle := spawn(PADDLE, my)
	paddle.display_name = "GUEST" if my.guest else "MEMBER"
	paddles[my] = paddle       # myはその人を指す手掛かり。辞書の鍵にも使えます
```

```gdscript
## loginが済むまで開始させない。ゲストかどうかもここで分かる
func _on_logged_in(my):
	$"/root/main/title".ready_to_start(my.guest)
```

全員へ配る合図は`Online`本文の`@online signal`をそのまま使います。専用の名前はありません。

通らない命令はOnline側が捨て、理由をサーバの側に警告で出します。
本文へ知らせる合図はありません。直せるのは作者ではなく、送ってきた側だからです。

## Secret

`Secret`はOnline側だけで動きます。外へ出したくない鍵、外部APIの呼び出し、抽選の元データはここへ置きます。

Online側の本文から触れるのは、`@online`を付けた関数だけです（`Online`からでも、持ち主のいる物からでも同じです）。
中の値も、印の無い関数も、子Nodeも、世界の側からは見えません。
答えは通信で返るので`await`で受け取ります。

```gdscript
# secret.gd
extends Secret

@online_save var draws := 0
var api_key := "..."          # 外からは読めない

@online
func draw_one():   # ここだけが世界からの入口
	draws += 1
	return draws

func _mix():       # 印が無いので世界からは呼べない
	return randi()
```

```gdscript
# online.gd
extends Online

func _on_player_joining(my):
	var n = await $Vault.draw_one()
	print("これで", n, "回目")
```

`$Vault.draws`のように中の値を読もうとすると、その場で理由が返ります。
ホストがOnline側になっても同じ形なので、書き方も届く範囲も変わりません。

`Secret`は**スポーン同期と値同期の外**です。中のNodeは生まれた知らせも出ませんし、
`@online`や`@online_my`の値がClientへ届くこともありません。`@online_save`だけは効き、
残りますがClientへは出ません。

`Secret`はどこへ置いても構いません。`Online`の直下でも、その孫でも、spawnするsceneの中でも同じです。

### Secretのインスペクタ

**Onlineの設定は`Secret`のインスペクタにしかありません。**設定fileは持ちません。Clientが繋ぐのに要る設定は
`Online.tscn`の一部として書き出したものにも入るので、配ったあとで設定fileを添える必要はありません。
Clientが読まない設定は配布物から落ちます（「配布境界」）。
Clientの実行引数`-- --room-host=...`のような指定（`--`の後ろ）だけが、その場かぎりで優先します。
Serverはインスペクタの値と、Clientが読まない設定に限って環境変数`ONLINE_<大文字の名前>`（例 `ONLINE_REDIS_PASSWORD`）を読みます。
合鍵や合言葉をsceneに書くとrepoに残るので、置き場で渡したいときはこちらです。

インスペクタでは見出しごとに並びます。空欄は「書いていない」の意味で、既定値が効きます。見出しと設定の名前は訳しません。

| 名前 | 何を決めるか | 書かないとき |
| --- | --- | --- |
| `room_max` | 1部屋へ同時に入れる人数 | 8人 |
| `player_scene` | 入ってきた人に1つ置くscene。`player_joining`の前にengineが置く | 空 |
| `keep_sec` | 通信が切れた人の物を残しておく秒数 | 10秒 |
| `secret_name` | 証明書に書かれるサーバの名前 | `localhost` |
| `ca` | Clientが確かめる証明書の場所 | `res://online-ca.pem` |

**Display（表示）**

| 名前 | 何を決めるか | 書かないとき |
| --- | --- | --- |
| `teleport_px` | これを越える1回の移動は寄せずに飛ばす | 200px |

**Client Address（Clientの繋ぎ先）**

| 名前 | 何を決めるか | 書かないとき |
| --- | --- | --- |
| `room_host` | Clientが繋ぐ先 | `127.0.0.1` |
| `room_port` | 繋ぐport | `4433` |

**Server Listen（Serverの待受）**

| 名前 | 何を決めるか | 書かないとき |
| --- | --- | --- |
| `listen_host` | 外向きのbind先 | `0.0.0.0` |
| `listen_port` | 待ち受けるport | `room_port`、それも空なら`4433` |

**Matchmaking（相手探し）**（「サーバを選ばない」を参照）

| 名前 | 何を決めるか | 書かないとき |
| --- | --- | --- |
| `match_url` | 部屋を聞く／名乗り出る先 | 空。直結する |
| `match_key` | マッチングサーバと共有する合鍵 | 空 |
| `match_room` | この部屋の呼び名 | 空。乱数で付く |

**Save（保存）**（「サーバ実行」を参照）

| 名前 | 何を決めるか | 書かないとき |
| --- | --- | --- |
| `redis_host` | 預け先 | 空。**書かなければ保存しない** |
| `redis_port` | 預け先のport | 6379 |
| `redis_password` | 預け先の合言葉 | 空 |
| `redis_prefix` | 鍵の頭に付ける名前 | `online` |
| `connect_key` | 端末鍵を導く鍵（64桁の16進） | 空。`user://online_connect_key`に1つ作る。**その1台だけの鍵**なので、立て直しや2台目でも口座を届かせるなら書く |

**Accounts**（「Login」を参照）

| 名前 | 何を決めるか | 書かないとき |
| --- | --- | --- |
| `account_providers` | 外部口座の認可先とサーバ側の資格情報を名前ごとに保持する辞書 | 空。外部口座を使わない |

`room_max`を超える参加はOnline側が断り、同じ数をマッチングサーバへも知らせます。
「The room is full」は断りの理由であって異常ではないので、赤字にはなりません。
断られた人の画面には理由が1行出て、手元は5秒ごとに入り直します。席が空けばそのまま入ります。
マッチングサーバがあるときは聞き直して別の部屋へ行き、無ければその人が新しい部屋のホストになります。

切れたかどうかは1秒ごとの生存確認で決めます。5秒届かなければ切れたとみなします。身元の線も世界の線も、ホストとSecretの間も同じです。
`keep_sec`の間は、通信が切れてもその人の物を世界に残します。`my`もそのままなので、鍵にして覚えた物が続きます。強制終了で消えた人も同じ扱いです。
この間に戻ってくれば続きから遊べ、`player_left`は鳴りません。過ぎたら`player_left`が鳴って世界から外れます。

`Online`の下に`Secret`が無いと、エディターの`Online`に⚠が出ます。無くても遊べます。

## Login

何も書かなければゲストです。端末ごとに口座ができ、`@online_save`はその口座に残ります。
端末の鍵は`user://`に置くので、アプリを入れ直さないかぎり同じ口座に入ります。
預け先（Redis）のある世界では、新しい口座は同じアドレスから10分に8つまでです。
9つ目は「Too many new players from this address. Try again later」で待たされます（在る口座は数えません）。

外部口座の紐付けは任意です。標準では提供者が登録されていないため、ゲストログインだけが動きます。
外部口座を使う場合は、`Secret.account_providers` に提供者名をキーとした辞書を設定します。

| どこ | 書く物 |
| --- | --- |
| 提供者の設定画面 | Redirect URIに `http://127.0.0.1:4435/` |
| `Secret.account_providers` | `authorize_url` `token_url` `user_url` `client_id` `client_secret`。必要なら `scope` `id_field` `name_field` |
| Clientの本文（ボタンなど） | `Online.login_with("提供者名")` |

提供者の辞書は環境変数 `ONLINE_ACCOUNT_PROVIDERS` にJSONで渡すこともできます。
`id_field` は本人を一意に識別する応答項目で、既定は `id` です。`name_field` の既定は `name` です。
外部のURLはHTTPSを使います。ローカル試験の `http://127.0.0.1:<port>` も使えます。
資格情報と提供者の設定はClientへ書き出されません。

ブラウザが開き、済むと`logged_in(my)`がもう1度鳴ります。`my.guest`が偽になり、`my.name`に相手の名前が入ります。
済まなかったときは`login_failed(reason)`が鳴ります。時間切れ、portが塞がっている、相手に断られた、のどれかです。
認可はPKCEで結び、他の端末で取った認可codeをこの接続へ投げ込んでも通りません。
URLも合鍵の照合もSecretがするので、Clientに設定はありません。
ブラウザが自動で開かない環境では、Clientの出力に出る`Online: sign in at ...`の行を開きます。

**口座の結びかたは1つです。**外部口座でloginすると、この端末のゲストの鍵と外部口座の鍵が同じ口座を指します。
外部口座に保存済みの口座があればそちら、無ければ今のゲストの口座です。値は合成しません。

| 状況 | 結果 |
| --- | --- |
| ゲストで遊んだ端末で初めて紐付け | ゲストの口座が外部口座に結ばれる。値はそのまま |
| 別の端末で、外部口座に保存がある | その端末は外部口座へ向く。以後ゲスト起動でも同じ口座 |
| 両方に口座がある | 外部口座側。置いてある本人の物へその値が戻る |

外部口座の認可は端末ごとに1回です。次回からはいつものゲストの自動loginで同じ口座に入ります。
同じ口座で別の端末が入ると、前の端末は`Signed in from another device`で止まり、つなぎ直しません。
`Online.logout()`は、この端末と口座の結びを切って止まります。次の起動は新しいゲストです。
外部口座はそのまま残り、また`login_with`で入れます。共有の端末で、次に触る人が自分の口座に入らないための出口です。
`my`は同じ物のまま中身が替わるので、`my`を鍵にした辞書もそのまま続きます。
Online側でloginの状態を使うときは`my.guest`と`my.name`をその場で読みます。

## どこで何が走るか

**印が効くのは、engineが呼ぶ本文と、手元からOnline側への境界だけです。** 普通の呼び出しは呼んだ側で走ります。
印の無い`goal()`をOnline側の本文から呼べばOnline側で走り、手元から呼べば手元で走ります。

| 書きかた | 走るところ |
| --- | --- |
| `func _ready()` `func _process(delta)` `func _input(event)` … | 手元。**プレイヤー全員**が、世界に在る**全てのNode**で走らせる |
| `@online_my func _ready()` … | **本人の手元だけ**。自分の物にだけ要る処理 |
| `@online func _ready()` `@online func _process(delta)` … | **ホストだけ**。そのNodeから見たOnline側の処理 |
| 手元から呼んだ`@online func` | ホスト。手元は本人の物の分だけ送る |
| 合図 → 印の無い関数 | その合図が鳴る側 |
| 合図 → `@online func` | ホストだけ |

engineが木を歩いて呼ぶ本文は、どれも同じ規則です。名前による例外はありません。

| engineが呼ぶ本文 | 印が無いとき | `@online`を付けたとき |
| --- | --- | --- |
| `_enter_tree` `_ready` `_exit_tree` `_process` `_physics_process` `_input` `_shortcut_input` `_unhandled_input` `_unhandled_key_input` `_draw` `_notification` | プレイヤー全員の手元で走る | ホストだけで走る |

同じ名前で両方書けます。手元の写しからはホストの本文が外れるので、engineはいつもどおり
片方だけを呼びます。

ホストはOnline側であると同時に普通のプレイヤーでもあるので、**どのcallbackも両方が順に走ります**。
**印の無いほうが先、`@online`のほうが後**です。みんなと同じ支度や毎frameの処理が済んでから、
ホストがOnline側の値を重ねる、という順番です。`_ready`だけの話ではありません。

```gdscript
func _ready():
	$Anim.play("idle")          # 全員の支度。ホストでもまずこれが走る

@online
func _ready():
	hp = hp_max                 # そのあとでホストが世界の初期値を入れる
```

`@online func _physics_process`だけは、engineのframeではなく**世界の時計(20回/秒)**で呼ばれます。
走る側の話ではなく、世界が進む速さの話です。Online側（サーバでも、ホストになった端末でも）は
engineの物理の刻みも20回/秒にするので、`move_and_slide()`も同じ刻みで進みます。

`Online`に書いた`@online func`（callbackでないもの）は、プレイヤーから呼べる口になります。
`Secret`に書いた`@online func`は、世界から呼べる入口になります。engineが呼ぶ本文はどちらでもなく、
どこに書いても「ホストだけで走る」意味です。

- 印の無い`_process`は全員の手元で走ります。他人の物を動かしても、Online側は自分の物の申告しか
  受け取りません。ホストの世界の写しでも同じで、印の無い本文から呼んだ`@online func`は本人の物でなければ何も起こしません。
  この性質を使うと、位置を配らずに済みます（「出来事だけを配る」）。
- Online側の本文から`@online func`を直に呼ぶと、その場で走って答えが返ります。`my`はその物の持ち主です。
- `online_changed`は変化のときだけ鳴ります。最初に値が届いたときは鳴りません（前の値がありません）。
- インスペクタから繋いだ合図も同じ規則です。同じ合図を印の無い関数と`@online func`の
  両方へ繋げば、片方は手元だけ、もう片方はOnline側だけで鳴ります。
  自分のNodeは手元で見えたとおりを`@online func`へ渡し、持ち主のいない物はOnline側で
  当たりを決める、という分けかたが1つのsceneで書けます。

```gdscript
## 自分の当たりは自分の画面が正しい。印が無いので手元だけで鳴る
func _on_atari_area_entered(tama):
	kurau(tama.get_parent())      # 見えた弾をOnline側へ渡す

## 同じ合図を@onlineへも繋いである。こちらはOnline側だけで鳴る
@online
func kizuku(_tama):
	print("Online側からも見えている")
```

- どちらの`_ready`でも、走る前に値がもう入っています。
  Online側は`spawn()`へ渡した値と`@online_save`の保存値、Clientは最初の配りで届いた`@online`値です。

`@online`値は配列・辞書の中までClientでは読取専用です。`Online`配下のNodeはClientから追加・移動・削除できません。

### 出来事だけを配る

位置を毎回配らずに済む物があります。**動きが式で決まっていて、変わるのが出来事のときだけ**の物です。
印の無い`_process`は全員の手元で走るので、同じ式を全員が回せます。配るのは式の入力だけです。

弾は生まれた場所と角度と速さで決まります。あとは配りません。

```gdscript
@online var angle := 0.0         # 生まれたときに一度だけ届く
@online var speed := 0.0

## 進めるのは全員の_process。ホストもプレイヤーなのでここで動く
func _process(delta):
	position += Vector2(cos(angle), sin(angle)) * speed * delta

## Online側はそれに加えて寿命と場外を見る
@online
func _physics_process(delta):
	life -= delta
	if life <= 0.0:
		queue_free()
```

決まりは1つです。**進める式は1か所だけに書く。** 印の無い`_process`はホストでも走るので、`@online func
_physics_process`にも同じ式を書くと、ホストの世界だけが2倍の速さで進みます。

Pongのボールのように何度も向きが変わる物は、この書き方にしません。Online側が進めた位置をそのまま配るほうが
短く、跳ね返りを数える仕掛けも要りません（`samples/pong`）。手元は1sample(50ms)遅れて見えますが、線が届いている間は
先を読まないので、跳ね返りをすり抜ける絵は出ません。

## サンプル

`samples/`に2つあります。それぞれ違う物を見せています。

| | 何を見せているか |
| --- | --- |
| `pong` | 位置を何も書かずに動くボールとPaddle。`@online_my`で本人にだけ見せる勝敗 |
| `shobon_jump` | CharacterBodyの横と縦（床と速度も書かない）、タックルで飛ばす相互作用、面の進行 |

## 本文の決まり

世界の本文は、`@online`が1つでもある本文と、`Online`か`Secret`を継ぐ本文です。置き場は問いません。
説明書きを除いた最初の行が`extends`で（`class_name`は置けません）、継ぐ先はsceneの根に使っている型か`Online`か`Secret`です。
名前の頭に`__`は使えません。`@online`の1つめに書く`"auto_off"`と、`my` `world`は予約語です。

## 細かい調整

書かなくても動きます。回数、寄せかた、先読みを自分で決めたいときだけ読んでください。

- `@online(10)`と`@online(position, 10)`は毎秒回数の上限です。省略時は用途と実測変化量から自動で決めます。
- 寄せかたは名前で選べます。書かなければNodeとプロパティから自動で決まるので、既定を指す綴りはありません。
  `@online(position, 20, "arc", 3)`のように、印の後ろは順番でなく**型**で読みます。数は毎秒回数と過去frame数、文字は寄せかたの名前です。
  `@online(position, "arc")`のように要る物だけ書けます。

| 寄せかた | Clientの表示 | 向いているもの |
| --- | --- | --- |
| `"snap"` | 寄せない。届いた値をそのまま置く | マス目移動、瞬間移動、当たり判定用の物 |
| `"ease"` | 届いた値の間を寄せる。先は読まない | 遅れてよい物。ずれない代わりに1sample遅れる |
| `"predict"` | 寄せて、届かない間は等速で先を読む | 弾、車、一定速度で動く物 |
| `"arc"` | 寄せて、重力を含む加速度で先を読む | ジャンプアクション、投げた物 |

  届かない間に先を読む長さは、どの寄せかたでも0.15秒です。長く読むほど外れたときのずれが
  大きくなるので、それ以上は読まずに止めて、次に届いた値へ寄せ直します。
  ジャンプの見た目は、書かなくても配られる位置が床と速度から縦を進めて持たせるので、
  ここを伸ばす必要はありません（「位置と向きは書かなくても配る」を参照）。
- 1回で大きく飛んだ位置は瞬間移動とみなして寄せません。境目は`Secret`の`teleport_px`(既定200)です。
  1回の刻みでこれ以上動く世界なら、大きくしてください。
- 通信を減らしたいときは回数を下げます。`@online(position, 5, "arc")`なら毎秒5回だけ配り、
  間は手元が弧を読んで進めます。

  最後の数は速さを読むときに混ぜる過去frame数です（1〜7、既定1）。増やすと速さが均されて
  揺れに強くなり、代わりに向きの変化への反応が遅くなります。

## サーバ実行

初回起動でP-256の鍵と自己署名証明書を`user://online_secret/`へ作ります。POSIX系では保存先を`0700`、秘密鍵を`0600`へ固定します。サーバを直接動かすときは`GD_USER_HOME`に永続保存先の絶対パスを指定します。

待受と名前は`Secret`のインスペクタから読みます。`listen_host`・`listen_port`・`secret_name`です。
描画しないOnline側はframeの上限を60にします（projectの`max_fps`を書いていればそちら）。世界の刻みは20Hzなので足ります。

```sh
# 下の「ビルド」のサーバ用 gd-godot
./gd-godot.linuxbsd.template_release.x86_64.online_server --headless --main-pack game.pck -- --online-server
```

サーバのpresetで書き出すと、出力先の隣に`Dockerfile`と`compose.yaml`が出ます。PCKだけの書き出しでも、presetが指すサーバの実行体を隣へ写します。
出力先で`docker compose up -d --build`と打てば、Redisと一緒に動きます。鍵と接続鍵は`online_data`、Redisの保存データは`redis_data`という名前付きvolumeに残ります。
公開証明書だけを`docker compose cp online:/online_data/online_secret/certificate.pem ./online-ca.pem`で取り出し、Clientの`ca`へ指定します。待受は`listen_port`をそのままTCPとUDPで開けます。

手元で試すだけなら、エディターの Project ▸ Tools ▸「Online: 手元の試験用コンテナを起動」です。
サーバのpresetを`res://.godot/online_container/`へ書き出し、コンテナを立て、出来た証明書をprojectの`ca`へ写すところまでやるので、
そのまま「実行」で繋がります。`ca`の場所にもう証明書があれば書き換えず、取り出した公開証明書の場所を出力に出します。止めるのは同じ場所の「停止」です。Dockerが入っていれば、他に用意する物はありません。

`@online_save`値はRedisへ預けます。預け先は`Secret`のインスペクタの「保存」に書きます
（表は「Secretのインスペクタ」）。`redis_host`が空なら保存しません。

置き場はhashが2種類です。持ち主のいるNodeは口座ごとの`<prefix>:<accountID>`に`<種類>.<変数名>`の項目で、
持ち主のいないNodeと`Secret`は1つの`<prefix>:world`に`<場所>.<変数名>`の項目で置きます。
値はVariantの二進形で、`Vector2`も`Color`も型のまま戻ります。書くのは変わった項目だけで、消えた物の項目は消します。
`<場所>.@kind`には種類の名前が入り、同じ場所に別の種類が置かれたときは戻しません。
`redis-cli`なら`HKEYS <prefix>:world`で何が残っているか見られます。
`@online_save`も`@online_save_my`も同じ経路で残ります。違うのは配る相手だけです。
login時に本人の保存値を読み、その人の物を置いた直後に戻します。世界での名前が前回と変わっても同じ値が戻ります。
Redisの読み書きで世界は止まりません。世界の開始とloginだけは、保存値が戻ってから進みます。
1回の保存は1つの取引（MULTI/EXEC）で書くので、途中で切れても半分だけ残ることはありません。
sceneに置いた固定の物と、`spawn()`で名前を付けた物は場所で覚え、`queue_free`まで残ります。
持ち主のいる物は口座に**種類ごとに1つ**残り、切断しても消えません。同じ種類を2つ持たせると後の方だけが残り、Online側に1度知らせます。
Redisが読めない間は、新しいloginを`The save store is not reachable`で断ります。手元は少し待って入り直します。
居る人と世界はそのまま続き、切れている間に変わった値も繋ぎ直したときに預け直します。
起動時にRedisが読めなければ、読めるまで世界を始めません。初期値で始めると次の保存で本物を消すからです。

端末とaccountの間は`<prefix>:device:<端末鍵>`の1本で結びます。端末鍵は`connect_key`（無ければ`user://online_connect_key`）で
導くHMACなので、Redisを読んでも元の端末IDは戻せません。accountIDは端末と無関係な乱数です。

起動時に表示されるCA証明書だけをClientへ同梱します。秘密鍵はサーバ外へ出しません。GuestのインストールID、session、再接続token、heartbeat、命令頻度、型、所有者はC++で検査します。

## サーバを選ばない

`match_url`を書くと、Clientは行き先を自分で決めません。空いている部屋を1つもらって入ります。
部屋は動いているOnline側のプロセスそのもので、自分から名乗り出ます。

Online側の`Secret`。名乗り出る先と、この部屋の呼び名・行き先を書きます。

| 設定 | 例 |
|---|---|
| `listen_port` | `4433` |
| `match_url` | `https://match.example.com/pong` |
| `match_key` | マッチングサーバと共有する合鍵 |
| `match_room` | `tokyo-1` |
| `room_host` | `tokyo-1.example.com` |

Client側の`Secret`。hostもportも書きません。

| 設定 | 例 |
|---|---|
| `secret_name` | `game.example.com` |
| `ca` | `res://online-ca.pem` |
| `match_url` | `https://match.example.com/pong` |

`match_url`は両側で同じ1つです。部屋の名乗りか、行き先を聞きに来たかは中身で分かれます。
URLの道（`/pong`）がゲームの名前で、同じマッチングサーバでいくつものゲームを分けて持てます。
互換の無い更新は`/pong/2`のように道を変えれば、古いClientが新しい部屋に会いません。

マッチングサーバは gd の標準モジュール`GD.online.match`です。空きのある部屋のうち**いちばん埋まっている部屋**を返すので、
人が散らばらず対戦相手が見つかります。`samples/online/main.gd`がそのまま動くサーバで、設定は環境変数`MATCH_*`で渡します。

```sh
MATCH_KEY=<Online側と共有する合鍵> MATCH_REDIS=127.0.0.1:6379 gd serve samples/online/main.gd
```

APIは[マニュアル](manual.md)の`GD.online`にあります。

## ホストが世界を持つ

世界を動かすサーバを置かずに遊ばせることもできます。身元だけを持つ`--online-secret`を1つ立てておくと、
**空いている部屋が無いときは、聞きに来た端末そのものが部屋になります**。誰が部屋を持つかを決めるのは
マッチングで、作者もプレイヤーも選びません。常時動かすのは身元の1つとマッチングだけです。

```sh
# 身元だけ。世界は動かさない
./gd-godot.linuxbsd.template_release.x86_64.online_server --headless --main-pack game.pck -- --online-secret
```

身元側の`Secret`。Clientが最初に来る場所です。
`listen_port`・`secret_name`・`match_url`・`match_key`・`redis_host`を書きます。

Client側の設定は「サーバを選ばない」と同じです。
ホストは元から信用しない作りです。合鍵はホストへ渡りません。
部屋を持てるのは、マッチングが「あなたが持つ」と言った端末だけです。その返事に付く札をSecretが同じ合鍵で検めます。
1本の接続が持てる部屋は1つで、続けて3人を断った（5秒答えなかったのも数える）部屋は畳みます。

通り道はこうなります。

```
Client --TLS--> Secret   login・再接続
Client --DTLS-> ホスト   状態と@online funcの命令
ホスト --TLS--> Secret   部屋の名乗り・認証の受け取り・保存・Secret本文への依頼
```

**ホストは世界を動かすだけで、身元も保存も`Secret`の中身も持ちません。**
`$Vault.数える()`のような`Secret`の`@online func`は、ホストからSecretへ頼んで答えだけが返ります。
本人の`@online_save`の値もSecretがRedisへ書きます。ホストになった端末のPCKには`Secret`の本文が入っていないので、
書き換えても中身は出てきません。作者が書く本文は1プロセスのときと1文字も変わりません。

ホストが預けられるのは、**その部屋に居る人の口座**の、**その種類の`@online_save`項目**だけです。
持ち主のいない物の`@online_save`は、この形では残りません。部屋ごとに世界が別なのに1つの箱へ入れると、互いに上書きするからです。
部屋に居ない口座、知らない項目、世界の保存値を名指しした保存は断り、Secretの出力に`Refused`と理由ごとに1回出ます。
部屋の中の人の値をホストが好きに書けることと、`Secret`の`@online func`をホストが好きな引数で呼べることは残ります。
その人の世界を動かしているのがホストなので、これはホスト方式の代償です。`Secret`の関数が受ける引数は、ホスト方式では信用しない前提で書きます。

どの部屋も満席なら、その人が新しいホストになって次の部屋が立ちます。人が増えるほど部屋も増えるので、
`room_max`は「1部屋の人数」であって「遊べる人数の上限」ではありません。

ホストが抜けるとその部屋は畳まれます。落ちても黙っても、Secretは5秒以内に気付いて居た人へその場で伝え、
居た人は待たずにマッチングへ聞き直して、また誰かがホストになります。落ちてから次の部屋で遊べるまでは数秒です。
そのとき世界は最初からですが、本人の`@online_save`の値はSecretのRedisに残っているので戻ります。

ホストの状態通信は、身元のportの次から空きを探して開き、Secretが作って渡した使い捨ての証明書で名乗ります。
その証明書はSecretが認証の返事に載せてClientへ届けるので、Client側に増える設定はありません。
家庭内の回線でホストになるときは、そのportが外から届く必要があります。

## Client

何も書かなければ`127.0.0.1:4433`・`res://online-ca.pem`で動くので、手元で試すだけなら設定は0行です。

`@online`を1つでも検出すると接続処理を自動生成します。通常のmain scene以外に入口は要りません。
世界の線(DTLS)には項目名でなく番号を乗せ、範囲符号器で縮めます。4人分の位置と速度200tickで、線に乗る量は111.5KB→30.6KB（約1/3.6）です。
Online側の1tickの費用は、動く物200個の世界で約5.5ms（作者の物理3.2ms、配りの集め2.3ms）、2人の世界で0.4msです。

## 配布境界

- エディターは構文解析、検査、サーバ起動を含みます。
- 通常のOnline export templateは同期、補間、命令送信を標準搭載します。`Secret`型は入口として持ちますが、
  認証の金庫（サーバexport template内のC++オブジェクト。作者が触る`Secret`ノードとは別物）は含みません。
- **`Secret`の本文はClientのPCKへ入りません。**書き出しのたびに、`extends Secret`の本文は
  `extends Secret`の1行へ置き換わり、scene内の`Secret`からは子Nodeも本文の参照も外れます。
- **`Secret`の設定も、Clientが読む分しか残りません。**残るのは`secret_name` `ca` `teleport_px`
  `room_host` `room_port` `match_url`と、遊ぶ人がホストになった部屋も同じ人数で満席にするための`room_max`だけです。
  `listen_*` `match_key` `match_room` `redis_*` `account_providers` `keep_sec`はPCKへ入りません。
- `Secret`の下でしか使っていない本文も、同じくClientへ中身が出ません。Clientでも使う本文は
  `Secret`の外にも置いてください。
- `Secret`の本文は`.gd`のfileにします。sceneへ直接書いた組み込みscriptは抜けないので断ります。
- 本文を持ったまま書き出すのは、`online_server`の印を付けたpresetだけです。
  名前に`online_server`を含むtemplate（下の「ビルド」のサーバ用）を指定していれば、印は自動で付きます。
- サーバexport templateはGuest認証、TLS/DTLS、Online側の世界、生成・削除、同期配信、`@online_save`のRedis保存を含みます。
- 世界のsceneと、世界の本文はサーバ実行に必要なためPCKへ残します。
- 世界のsceneは実行時に字面で読むので、書き出し設定に関わらずtextのままPCKへ入ります。

## 試験

```sh
scons view=yes online_secret_enabled=yes dev_build=yes   # 先に実行体を作る
tests/mv/run.sh                         # 全部。戻り値は落ちた本数
tests/mv/run.sh T-SYNC-01               # 1本だけ
tests/mv/run.sh --slow                  # Redisを落とす遅い枠も
```

試験の本体は`tests/mv/cases/`にあります。動かし方と落ちたときの見方は`tests/mv/README.md`にあります。

待ちは全部「出来事」で待ちます。logの行、portのlisten、Redisの鍵、processの終了を0.05秒刻みに見て、上限だけを持ちます。
試験ごとにport帯とRedisの鍵の頭と`user://`を分けて並行に走らせ、login用のport（4435）を使う試験だけ直列です。
sampleは触らず、写しへ試験の本文を差し込みます。`redis-server`と`python3`が要ります。

## 本家に追いつく

本家の版を上げる手順は gd 本体と同じです。`devtools/upstream.sh`でmergeし、`devtools/godot_patches.sh record`で記録し直します。
engine本体へ入れた変更は`patches/godot/online.patch`と`language.patch`に入ります。
`scene/main/node.*`と`scene/main/scene_tree.*`の関所は、表示用の実行体だけが使うので`cli/view/online.patch`にあり、
`scons view=yes`が本家のソースを取り出すときに当てます。

## ビルド

Onlineは表示用の実行体`gd-godot`に入ります。旗の無い`gd`には入りません。

```sh
scons view=yes online_secret_enabled=yes   # エディター
scons view=yes target=template_release     # 配布するClient
scons view=yes target=template_release online_secret_enabled=yes extra_suffix=online_server   # サーバ
```
