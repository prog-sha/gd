English | [日本語](online-guide.md)

# Godot Online: the short guide

Only what you need to start. The full reference is [full reference](online.en.md).

## The shape of a world

```
Node2D
└── Online          ← the Online-side world. Its script holds the Online-side code
    ├── Secret      ← runs only on the Online side: settings, sign-in, keys
    └── Paddle_7    ← a node placed by spawn() with an owner
```

- **The scene whose root is `Online` is the world.** Put it anywhere in the project, name it as you like, one per project.
- Everything under `Online` except `Secret` is synchronized: nodes and values, to everyone. Only the Online side can place nodes.
- `Secret` never reaches a client. Every setting lives in the `Secret` inspector and nowhere else.

## Build one

**1. The world.** Make one scene whose root is `Online`, attach a script, add a `Secret` child.
Put the player scene into `player_scene` on `Secret` and every player who joins gets one.

```gdscript
extends Online

func _on_player_joining(my):   # _on_<signal> connects itself, like _ready
	print(my.name, " joined")
```

**2. The player.** Any root type. With at least one `@online` in the script, a `Node2D`/`Node3D` shares its position and rotation without a mark.

```gdscript
extends CharacterBody2D

@online var color := Color.WHITE    # shared with everyone
@online_input var axis := 0.0       # written on the owner's device, read on the Online side

func _process(_delta):              # unmarked code runs on every device
	axis = Input.get_axis("ui_left", "ui_right")

@online                             # runs on the Online side only
func _physics_process(_delta):
	velocity.x = clampf(axis, -1.0, 1.0) * 220.0
	move_and_slide()
```

**3. Run the Online side once.** It creates the keys and prints where the certificate is. Copy that `certificate.pem` into the project as `res://online-ca.pem`.

```sh
godot --headless --path . -- --online-server
```

**4. Play.** With the Online side running, run the project as usual and it connects as a client.

## The names

| Kind | Names |
| --- | --- |
| Marks | `@online` `@online_my` `@online_save` `@online_save_my` `@online_input`, and `@online("auto_off")` to stop sharing the position |
| Types | `Online` `Secret` |
| Function | `spawn(scene, my, properties)` (arguments are read by type; all optional) |
| Words | `my` (the owner: `my.id` `my.guest` `my.name`), `world` (the `Online` this node lives in) |
| `Online` signals | `player_joining(my)` `player_left(my)` `logged_in(my)` `login_failed(reason)` `spawned(node)`. Write `_on_<name>` and it connects |
| Signal on every node | `disconnected` |
| External account | `Online.login_with("provider")` `Online.logout()`; configure the provider in Secret before use |

| Mark | Who receives it | Saved |
| --- | --- | --- |
| `@online var hp` | everyone | no |
| `@online_my var hand` | the owner and the Online side | no |
| `@online_save var level` | everyone | yes (Redis) |
| `@online_save_my var coins` | the owner and the Online side | yes |
| `@online_input var axis` | written on the owner's device, read on the Online side | no |

- `@online func` is the one road from a client to the Online side. `await buy("potion")` returns the answer.
- `@online_my func` runs on the owner's device only. Your own marker or camera goes here.
- An unmarked `_process` runs on every device; `@online func _physics_process` runs on the Online side. Write the movement in one of them, not both.

## I want to

| I want to | Write |
| --- | --- |
| Put a node in the world | `spawn(BULLET)` on the Online side, `spawn(BULLET, my)` for a player's own |
| Jump or shoot (an event) | `@online func jump()`, called from the device |
| Hold a key (a state) | `@online_input var axis`, written in `_process` |
| Show an Online-side value in a Label | `@online($Score.text) var score := 0` |
| Keep a value across restarts | `@online_save var level := 1` and `redis_host` on `Secret` |
| Room size, reconnect grace | `room_max` and `keep_sec` on `Secret` |
| Reach a server on another machine | `room_host` on `Secret` |
| Try it locally | Project ▸ Tools ▸ "Online: Start the local test container" (needs Docker) |

## When something goes wrong

| Message | Do |
| --- | --- |
| `Cannot read the CA certificate` | Copy the certificate from the path the server printed |
| `Cannot reach the room` | Start the Online side first |
| `The room is full` | Raise `room_max`. Not an error. The client retries every 5 s and gets in when a seat frees |
| `Too many new players from this address` | 8 new accounts per address per 10 minutes. Existing accounts still sign in |
| `... is decided by the Online side` | You wrote an `@online` value on a client. Move that line into `@online func` |
| `Signed in from another device` | The same account signed in elsewhere, so this copy stopped. Start it again to take the account back |
| `The save store is not reachable` | Redis is down on the Online side. The client retries by itself |
| `this is a client build` | `--online-server` needs the editor or server build |
| The other player drops | Two copies share a project name. Rename one |

## Read next

| Section of [full reference](online.en.md) | What it covers |
| --- | --- |
| Annotations | which types sync, where saves go, annotations on signals and functions |
| Where code runs | ordinary vs annotated code, event-only sync |
| Secret | Server-side-only code and the 19 inspector settings |
| Login | Guest accounts and optional external account linking |
| Running a server / Matchmaking / Player-hosted worlds | dedicated server, matchmaking, players hosting rooms |
| Fine tuning | rates, smoothing, prediction. Works without any of it |
| Distribution boundaries | what never enters a Client PCK |

The examples in [samples/](../samples/) are `pong` and `shobon_jump`.
