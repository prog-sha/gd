[English](online-guide.en.md) | **日本語**

# Godot Online 最初に読む手引き

作り始めるのに要る分だけです。全部の説明は[ONLINE.md](online.md)にあります。

## 世界の形

```
Node2D
└── Online          ← Online側の世界。ここのgdにOnline側の処理を書く
    ├── Secret      ← Online側だけで動く。設定・login処理・鍵はここ
    └── Paddle_7    ← spawn()で持ち主を付けたNode
```

- **根が`Online`のsceneが世界です。**projectのどこに置いても、名前も自由で、1つだけ置きます。
- `Online`の下は`Secret`以外すべて、生成も値も全員へ同期します。物を置けるのはOnline側だけです。
- `Secret`はClientへ渡りません。設定は`Secret`のインスペクタにしかありません。

## 作る

**1. 世界。** 根が`Online`のsceneを1つ作り、本文を付け、子に`Secret`を置きます。
プレイヤーのsceneを`Secret`の`player_scene`に入れると、入ってきた人に1つ置かれます。

```gdscript
extends Online

func _on_player_joining(my):   # _on_<合図> は _ready と同じく自分で繋がる
	print(my.name, " joined")
```

**2. プレイヤー。** 根は好きな型。`@online`が1つでもあれば、`Node2D`/`Node3D`の位置と向きは書かなくても配られます。

```gdscript
extends CharacterBody2D

@online var color := Color.WHITE    # 全員へ配る
@online_input var axis := 0.0       # 本人の手元が書き、Online側が読む

func _process(_delta):              # 印が無い本文は全員の手元で走る
	axis = Input.get_axis("ui_left", "ui_right")

@online                             # Online側だけで走る
func _physics_process(_delta):
	velocity.x = clampf(axis, -1.0, 1.0) * 220.0
	move_and_slide()
```

**3. Online側を1回動かす。** 鍵を作り、証明書の場所を表示します。表示された`certificate.pem`を`res://online-ca.pem`として置きます。

```sh
godot --headless --path . -- --online-server
```

**4. 遊ぶ。** Online側を動かしたまま、projectを普通に実行するとClientとして繋がります。

## 覚える名前

| 種類 | 名前 |
| --- | --- |
| 印 | `@online` `@online_my` `@online_save` `@online_save_my` `@online_input`、位置を配らない`@online("auto_off")` |
| 型 | `Online` `Secret` |
| 関数 | `spawn(scene, my, properties)`（後ろは型で読む。どれも省ける） |
| 語 | `my`（本人。`my.id` `my.guest` `my.name`）、`world`（その物が居る`Online`） |
| `Online`の合図 | `player_joining(my)` `player_left(my)` `logged_in(my)` `login_failed(reason)` `spawned(node)`。`_on_<名前>`を書けば繋がる |
| 全Nodeの合図 | `disconnected` |
| 外部口座 | `Online.login_with("提供者名")` `Online.logout()`。使う場合はSecretに提供者を設定 |

| 印 | 誰に配るか | 保存 |
| --- | --- | --- |
| `@online var hp` | 全員 | しない |
| `@online_my var hand` | 本人とOnline側だけ | しない |
| `@online_save var level` | 全員 | する（Redis） |
| `@online_save_my var coins` | 本人とOnline側だけ | する |
| `@online_input var axis` | 本人の手元が書き、Online側が読む | しない |

- `@online func`はClientからOnline側へ頼む唯一の道です。`await buy("potion")`で答えが戻ります。
- `@online_my func`は本人の手元だけで走ります。自分の目印やカメラはここに書きます。
- 印の無い`_process`は全員の手元で、`@online func _physics_process`はOnline側で走ります。進める式は片方にだけ書きます。

## したいこと

| したいこと | 書きかた |
| --- | --- |
| 世界に物を置く | Online側で`spawn(BULLET)`。誰かの物なら`spawn(BULLET, my)` |
| 跳ぶ・撃つ（出来事） | `@online func jump()`を手元から呼ぶ |
| 押している間の入力（状態） | `@online_input var axis`を`_process`で書く |
| Online側の値をLabelへ | `@online($Score.text) var score := 0` |
| 再起動しても残す | `@online_save var level := 1`。`Secret`の`redis_host` |
| 部屋の人数、切れた人を待つ秒数 | `Secret`の`room_max` `keep_sec` |
| 別のPCのサーバへ | `Secret`の`room_host` |
| 手元で試す | Project ▸ Tools ▸「Online: 手元の試験用コンテナを起動」（Dockerが要る） |

## 困ったとき

| 出たもの | どうする |
| --- | --- |
| `Cannot read the CA certificate` | サーバが表示した場所から証明書を写す |
| `Cannot reach the room` | Online側を先に動かす |
| `The room is full` | `room_max` を増やす。異常ではない。手元は5秒ごとに入り直し、席が空けば入る |
| `Too many new players from this address` | 新しい口座は同じアドレスから10分に8つまで。在る口座はそのまま入れる |
| `... is decided by the Online side` | Clientで `@online` 値を書いた。その行を `@online func` へ移す |
| `Signed in from another device` | 同じ口座で別の端末が入ったので、この端末は止まった。起動し直せばこちらに戻る |
| `The save store is not reachable` | Online側のRedisが落ちている。手元は勝手に入り直す |
| `this is a client build` | `--online-server` はエディターかサーバ用のbuildで |
| 相手が急に切れる | 同じプロジェクト名の写しが2つある。片方の名前を変える |

## 次に読む

| 読む場所（[ONLINE.md](online.md)） | 分かること |
| --- | --- |
| 印 | 配れる型、保存の置き場、signalと関数の印 |
| どこで何が走るか | 印の無い本文とある本文、出来事だけを配る書き方 |
| Secret | Online側だけの本文と、インスペクタの19個の設定 |
| Login | ゲストの口座、任意の外部口座との紐付け |
| サーバ実行 / サーバを選ばない / ホストが世界を持つ | 専用サーバ、マッチング、プレイヤーが部屋を持つ形 |
| 細かい調整 | 回数、寄せかた、先読み。書かなくても動く |
| 配布境界 | ClientのPCKに入らない物 |

サンプルは [samples/](../samples/) の `pong` と `shobon_jump` です。
