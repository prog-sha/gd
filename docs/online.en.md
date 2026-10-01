English | [日本語](online.md)

# Godot Online 4.7.2

Build authoritative online games with ordinary Godot scripts. You do not write RPC calls, numeric identifiers, or connection code. Annotated values and functions are synchronized automatically.

For an introduction, read the [short guide](online-guide.en.md).

## World structure

```text
Node2D
└── Online          ← Authoritative world; attach its script here
    ├── Secret      ← Server-only logic, login, and keys
    └── Paddle_7    ← An owned Node created by spawn()
```

Online and Secret are built-in Nodes available in the editor's Add Node dialog. A scene rooted at Online is the world. Keep one world anywhere in the project; its name is unrestricted. The script belongs on that root. The `samples/online/` directory is only an organizational choice.

- All creation and values below Online, except Secret, synchronize to everyone.
- Only the authoritative side can create objects. Spawned objects become Online descendants; pass any suitable scene.
- Only objects spawned with an owner accept that owner's reports.
- Secret is excluded from Client exports; see Distribution boundaries.

## Names to know

| Category | Names |
|---|---|
| Annotations | `@online`, `@online_my`, `@online_save`, `@online_save_my`, `@online_input` |
| Types | `Online`, `Secret` |
| Function available on all Nodes | `spawn(scene, my, properties)`; optional arguments are interpreted by type |
| World reference | `world`: the containing Online, at any depth |
| Signals on all Nodes | `disconnected`, `online_changed(field, value, previous)` |
| Player identity | `my`, with `my.id`, `my.guest`, `my.name` |
| External accounts | `Online.login_with("provider")`, `Online.logout()` |
| Online signals | `player_joining(my)`, `player_left(my)`, `logged_in(my)`, `login_failed(reason)`, `spawned(node)`; `_on_<name>` connects automatically |
| Secret settings | The 19 inspector settings listed below |

`my` is the single player reference. In owned objects it always identifies the owner, on both the authoritative side and the owner's Client; it is null on other Clients. In unowned objects, including Online and platforms, it identifies the caller inside an online function. For player_joining, player_left, and logged_in it identifies the signal's player. Pass the same reference to spawn to assign ownership.

`world.playing` accesses a world value without walking parent Nodes. Cameras inside owned objects are enabled only on the owner's Client and disabled for other players and dedicated servers; no manual camera ownership check is needed.

## Annotations

There are five variable annotations. `_my` restricts distribution to the owner; `_save` persists the value. Input travels in the opposite direction, from the owner's Client to the authoritative side.

| Annotation | Distribution | Persistence |
|---|---|---|
| `@online var hp` | Everyone | No |
| `@online_my var quiz_answer` | Owner and authoritative side | No |
| `@online_save var level` | Everyone | Yes |
| `@online_save_my var robux` | Owner and authoritative side | Yes |
| `@online_input var axis` | Owner writes; authoritative side reads; not broadcast | No |

Input variables accept bool, int, float, Vector2, and Vector3. Changed values are sent up to 20 times per second. Writes on other Clients have no effect. Disconnection restores the declared initial value so held input cannot remain active indefinitely. Validate reported input on the authoritative side, for example with clampf.

```gdscript
# Send the owner's input and apply validated movement on the authoritative side.
@online_input var axis := 0.0 # Owner's horizontal input.

func _process(_delta):
    axis = Input.get_axis("ui_left", "ui_right")

@online
func _physics_process(delta):
    velocity.x = clampf(axis, -1.0, 1.0) * 220.0
```

Use online functions for events such as jumping and attacking; use online_input for continuous states such as held movement.

The same types can be synchronized and saved: bool, int, float, String, StringName, Vector2/3/4, Color, Transform2D/3D, Array, Dictionary, Packed arrays, and other binary-serializable Variants. Object values, including Nodes, Resources, and custom classes, plus RID, Callable, and Signal, cannot be saved or distributed. Invalid statically typed declarations fail immediately. Invalid dynamic values are skipped and reported once on the authoritative side. Arrays and dictionaries follow this rule recursively. Use a dictionary to persist custom-class data.

Persistence depends on where the declaration lives:

| Script location | online_save | online_save_my |
|---|---|---|
| Online world root | World store, `Online.<field>` | Not allowed: the world has no owner |
| Fixed scene Node | World store, `<path>.<field>` | Not distributed without an owner; same storage |
| Owned object from `spawn(scene, my)` | Account store, `<kind>.<field>`, distributed to everyone | Same account storage, distributed only to owner |
| Named object from `spawn(scene, {"name": "Chest"})` | World store, `<path>.<field>` | Not distributed without an owner; same storage |
| Unnamed object from `spawn(scene)` | Not saved; reported once on authoritative side | Same |
| Secret | World store, `<path>.<field>`; never sent to Clients | Not allowed |

On Secret functions, online marks an entry callable from the world rather than a distribution rule. The script's extends declaration determines its meaning.

Signals accept online and online_my. Save annotations on Signals fail with `Annotation "@online_save" cannot be applied to a signal.`

```gdscript
# Send events from the authoritative side to the intended recipients.
@online signal shout(word) # Broadcast to everyone.
@online_my signal whisper(word) # Send only to the owner.
```

Functions also accept online and online_my. Online functions provide the Client-to-authority boundary that replaces RPC. Online_my functions run only on the owner's Client; use them for owner-only UI, markers, and cameras. They do nothing on other Clients or dedicated servers. Engine callbacks such as ready can coexist with an unannotated callback of the same name; the owner-only callback runs after the ordinary callback.

```gdscript
# Show the marker only on the owner's Client.
@online_my
func _ready():
    $Mine.visible = true
```

```gdscript
# Read local input and request an authoritative jump.
func _process(_delta):
    if Input.is_action_just_pressed("jump"):
        jump()

@online
func jump():
    velocity.y = -400.0
```

Use await to receive an online function's answer:

```gdscript
# Apply damage on the authoritative side and return the remaining health.
@online
func hit(amount):
    hp -= amount
    return hp

func _process(_delta):
    if Input.is_action_just_pressed("attack"):
        print("remaining", await hit(3))
```

Omit await when the answer is unused. If the function itself awaits, its answer arrives after that wait completes. Online functions can call spawn directly:

```gdscript
# Spawn an owned projectile from the authoritative side.
const TAMA := preload("res://Tama.tscn") # Projectile scene.

@online
func shot():
    spawn(TAMA, my, {"position": position, "angle": angle, "speed": 200.0})
```

Spawn arguments are interpreted by type, not position: a player reference or their object supplies ownership; a dictionary supplies initial properties. Use `spawn(scene)` for unowned objects. The scene root name defines the kind, generated names such as Paddle_7, and saved field names. Different scenes cannot share the same root name; Secret-rooted scenes cannot be spawned. Rejected spawns, including count/depth overflow, return null and report their reason once on the authoritative side.

To create one object for each joining player, set Secret.player_scene. The engine creates it before player_joining; no handler is required, as shown in shobon_jump. For custom spawning, use player_joining, as in pong.

A name such as `{"name": "Chest"}` gives an object a stable world path for persistence. Names use letters, digits, and underscores and must be unique under the parent. Unnamed objects receive per-run numbers and cannot retain saved values; this is reported once.

### Annotation forms

- Node2D and Node3D scripts with any online annotation automatically distribute position and rotation, with automatic interpolation. Use `@online("auto_off")` for local-only movement; otherwise local position is read-only. Scripts without online annotations are ordinary unsynchronized Godot Nodes.
- `@online(position)` synchronizes a named Node property and permits explicit rate/interpolation settings.
- `@online($Name.text)` synchronizes a child Node's property directly.
- An initializer pointing to a property, such as `@online var angle := $Body.rotation`, also maps the variable to that property and infers its type. Use the argument form when the stored variable should retain a different type.
- Targets may be in another scene. For example, `@online_my($"/root/Main/ui/Score".text) var score := "0"` maps the value to owner-only UI. Missing targets on dedicated servers or other Clients simply skip display; the value is retained.
- Display targets convert types automatically. `@online($Score.text) var score := 0` retains an integer and displays it in a Label without an explicit str call.
- Annotated variables appear in the inspector like exported values; scene values supply their initial values. Property-backed initializers do not appear unless explicitly exported, for example with export_range.
- Online function arguments can contain world Nodes. Their shared identifiers are transmitted and resolved to the corresponding Node. Nodes outside the world cannot be passed. Arguments are limited to 16 KB per call; oversized calls return null.

### Automatic position and rotation

| Node type | Automatically distributed properties |
|---|---|
| Node2D / Node3D | position and rotation |
| CharacterBody2D / CharacterBody3D | The above, plus floor state and velocity |
| Control and other types | None; explicitly name a property if needed |

Use auto_off on Node2D/Node3D to disable automatic distribution. On Nodes without position it reports once that it has no effect. Unchanged values stop after their initial transmission, so unused rotation causes no ongoing traffic.

CharacterBody display handles horizontal and vertical movement differently. Jumps, double jumps, and landings follow velocity changes without explicit tracking:

| Direction | Client display |
|---|---|
| Horizontal | Interpolates toward received position; does not predict other players' airborne steering |
| Vertical | Interpolates on the floor; advances with gravity while airborne |

Other players' input is unavailable, so horizontal steering cannot be predicted reliably. Vertical arcs are more visually sensitive to discontinuities. Gravity is measured from velocity differences between airborne samples; up_direction is read locally. Airborne prediction applies only off the floor so grounded objects do not slide during missing updates.

## Online signals

Define `_on_<name>` to connect automatically, as with ready. Inspector connections work too without duplicate delivery. Ordinary engine Signals such as pressed still use normal inspector connections.

| Signal | Trigger | Delivery |
|---|---|---|
| player_joining(my) | A player joins; custom owned-object spawning can occur here | Authoritative side |
| player_left(my) | A player leaves | Authoritative side |
| logged_in(my) | Local login completes; also emitted after external account linking | Own Client |
| login_failed(reason) | External login fails | Own Client |
| spawned(node) | A synchronized Node appears | All Clients |

Every Node has disconnected, emitted on the authoritative side and owner's Client when the owner's connection ends. Define `_on_disconnected` to connect automatically; online annotations select the authoritative handler, while an unannotated handler runs locally.

```gdscript
# Create the joining player's object and retain its identity.
extends Online

func _on_player_joining(my):
    var paddle := spawn(PADDLE, my)
    paddle.display_name = "GUEST" if my.guest else "MEMBER"
    paddles[my] = paddle
```

```gdscript
# Start local play only after login completes.
func _on_logged_in(my):
    $"/root/main/title".ready_to_start(my.guest)
```

Use an online Signal on the Online script to broadcast events. Invalid incoming commands are discarded and warned about on the authoritative side; there is no script Signal for them because the sender must correct them.

## Secret

Secret runs only on the server side. Put private keys, external API calls, and private random-selection data here. World scripts can call only online-annotated functions, whether invoked from Online or an owned object. Fields, unannotated functions, and children are inaccessible. Await the response:

```gdscript
# Keep private state and expose one callable world operation.
extends Secret

@online_save var draws := 0 # Persisted draw count.
var api_key := "..." # Private credential placeholder.

@online
func draw_one():
    draws += 1
    return draws

func _mix():
    return randi()
```

```gdscript
# Request a private operation when a player joins.
extends Online

func _on_player_joining(my):
    var n = await $Vault.draw_one()
    print("draw number", n)
```

Attempting to read a private field such as Vault.draws returns a reason immediately. The same access boundary applies when a player hosts the world.

Secret is outside spawn and value synchronization. Its child creation is not announced, and online/online_my values are never sent to Clients. Online_save persists values without exposing them. Secret can be a direct child, a deeper descendant, or part of a spawned scene.

### Secret inspector

Settings are defined only here. Clients can read only secret_name, ca, teleport_px, room_host, room_port, and match_url; room_max is also retained in Client exports for hosted-room capacity. Server-only settings are stripped during export.

Client connection settings are exported in Online.tscn, so no separate settings file is needed. Client arguments after `--`, such as `-- --room-host=...`, temporarily override them.

Unset inspector settings can be supplied through `ONLINE_<UPPERCASE_NAME>` environment variables, for example ONLINE_REDIS_PASSWORD, keeping credentials out of scenes and repositories. Empty fields select defaults. Inspector group and property names are not translated.

| Setting | Purpose | Default |
|---|---|---|
| room_max | Simultaneous players per room | 8 |
| player_scene | Scene spawned for each player before player_joining | Empty |
| keep_sec | Seconds to retain disconnected players' objects | 10 |
| secret_name | Server name in the certificate | localhost |
| ca | Certificate path verified by Clients | res://online-ca.pem |

**Display**

| Setting | Purpose | Default |
|---|---|---|
| teleport_px | Movement per update above this threshold snaps | 200 px |

**Client Address**

| Setting | Purpose | Default |
|---|---|---|
| room_host | Client destination | 127.0.0.1 |
| room_port | Destination port | 4433 |

**Server Listen**

| Setting | Purpose | Default |
|---|---|---|
| listen_host | Bind address | 0.0.0.0 |
| listen_port | Listening port | room_port, or 4433 if unset |

**Matchmaking**

| Setting | Purpose | Default |
|---|---|---|
| match_url | Room registration/discovery endpoint | Empty; direct connection |
| match_key | Shared key with matchmaking server | Empty |
| match_room | Room identifier | Empty; randomly generated |

**Save**

| Setting | Purpose | Default |
|---|---|---|
| redis_host | Save-store host | Empty; persistence disabled |
| redis_port | Save-store port | 6379 |
| redis_password | Save-store password | Empty |
| redis_prefix | Storage key prefix | online |
| connect_key | Key deriving device identities, 64 hex digits | Generated at user://online_connect_key; supply a persistent shared key to retain accounts after rebuilds or across servers |

**Accounts**

| Setting | Purpose | Default |
|---|---|---|
| account_providers | Dictionary of authorization endpoints and server credentials by provider name | Empty; external accounts disabled |

The authoritative side rejects players beyond room_max and reports the same capacity to matchmaking. `The room is full` is an expected rejection, not a red error. Clients display it and retry every five seconds. With matchmaking they ask for another room; without it they become hosts of new rooms.

Heartbeat checks occur every second; five seconds without a response means disconnection, on identity/world connections and between host and Secret. Objects and my remain for keep_sec, including after abrupt termination. Returning within that period resumes without player_left; after it expires, player_left is emitted and the player leaves the world.

The editor warns when Online has no Secret child, though play remains possible.

## Login

With no configuration, each device receives a guest account, and online_save values are stored under it. The device key lives under user://, so the account persists until the application is reinstalled. Worlds with Redis allow eight new accounts per address per ten minutes. A ninth receives `Too many new players from this address. Try again later`; existing accounts do not count.

External account linking is optional. Without registered providers only guest login is available. Configure Secret.account_providers:

| Location | Required configuration |
|---|---|
| Provider settings | Redirect URI `http://127.0.0.1:4435/` |
| Secret.account_providers | authorize_url, token_url, user_url, client_id, client_secret; optional scope, id_field, name_field |
| Client button/script | `Online.login_with("provider")` |

Alternatively pass the provider dictionary as JSON through ONLINE_ACCOUNT_PROVIDERS. Id_field defaults to id and identifies the account uniquely; name_field defaults to name. External endpoints require HTTPS, with localhost HTTP allowed for tests. Provider settings and credentials are excluded from Client exports.

Login opens a browser. Success emits logged_in again, sets my.guest to false, and fills my.name. Failure emits login_failed for timeout, an occupied callback port, or provider rejection. PKCE binds authorization to the connection, rejecting codes obtained on another device. Secret verifies endpoints and credentials; Clients need no provider settings. If a browser does not open automatically, follow the Client output line `Online: sign in at ...`.

Linking points the guest device key and external identity key to one account. An existing external account takes precedence; otherwise the current guest account is retained. Values are not merged.

| Situation | Result |
|---|---|
| First link after playing as guest | Guest account becomes linked; values remain |
| Another device with an existing external account | Device uses that account, including subsequent automatic guest startup |
| Both guest and external accounts exist | External account wins and restores values onto the player's objects |

Authorize once per device. Subsequent automatic guest login reaches the same account. Another device logging into that account stops the previous device with `Signed in from another device`, without reconnecting.

Logout removes the device-account link and stops the session. The next startup creates a new guest; the external account remains available through login_with. This prevents subsequent users of a shared device from entering the same account. The my object retains its identity while its fields change, preserving dictionary keys. Read guest/name when needed on the authoritative side.

## Where code runs

Annotations affect engine callbacks and the Client-to-authority boundary. Ordinary function calls execute on the calling side. An unannotated goal function therefore executes wherever it is called.

| Form | Execution |
|---|---|
| Ordinary ready/process/input callbacks | Every player's Client, on every world Node |
| online_my engine callbacks | Only the owner's Client |
| online engine callbacks | Only the host/authoritative side |
| online function called locally | Host; Clients send only for their own objects |
| Signal to unannotated function | Side emitting the Signal |
| Signal to online function | Host only |

This applies to enter_tree, ready, exit_tree, process, physics_process, input, shortcut_input, unhandled_input, unhandled_key_input, draw, and notification, with no name-specific exceptions. Annotated and ordinary callbacks of the same name can coexist; the host callback is stripped from Client copies.

A player host is both authority and Client, so both callbacks run in sequence: ordinary first, online second. This applies to every callback, not only ready.

```gdscript
# Prepare local presentation before authoritative initialization.
func _ready():
    $Anim.play("idle")

@online
func _ready():
    hp = hp_max
```

Online physics_process uses the world clock of 20 ticks per second. Both dedicated servers and player hosts set engine physics to this rate so move_and_slide uses the same interval.

Non-callback online functions on Online expose player-callable entries; those on Secret expose world-callable entries. Engine callbacks always mean authoritative execution regardless of location.

- Ordinary process runs locally for everyone. Authority accepts reports only for the player's own objects. The host's ordinary callbacks also cannot invoke online functions on objects owned by others.
- Direct authoritative calls to online functions execute immediately and return locally; my identifies the object owner.
- Online_changed fires on changes, not the initial value delivery.
- Inspector-connected Signals use the same side-selection rules. Connecting one Signal to ordinary and online handlers lets local owned-object collision reports coexist with authoritative unowned-object collision decisions in one scene.

```gdscript
# Report the locally observed projectile to the authoritative side.
func _on_atari_area_entered(tama):
    kurau(tama.get_parent())

# Observe the same Signal only on the authoritative side.
@online
func kizuku(_tama):
    print("Observed on the authoritative side")
```

Values are populated before either ready callback: spawn properties and saved values on authority, initial synchronized values on Clients. Online values are read-only on Clients, including nested arrays and dictionaries. Clients cannot add, move, or remove Nodes under Online.

### Synchronize events only

Objects whose movement follows a fixed equation and changes only at events can share the equation's inputs instead of repeated positions. Ordinary process runs for everyone, including the host. A projectile needs its spawn position, angle, and speed:

```gdscript
# Share projectile parameters and simulate movement locally.
@online var angle := 0.0 # Initial direction.
@online var speed := 0.0 # Initial speed.

func _process(delta):
    position += Vector2(cos(angle), sin(angle)) * speed * delta

# Enforce projectile lifetime on the authoritative side.
@online
func _physics_process(delta):
    life -= delta
    if life <= 0.0:
        queue_free()
```

Write the movement equation in one callback only. Repeating it in online physics_process doubles movement in the host world. Objects that repeatedly change direction, such as Pong balls, are simpler with authoritative position synchronization. Clients display one sample (50 ms) behind without predicting through rebounds while updates arrive.

## Samples

| Sample | Demonstrates |
|---|---|
| pong | Automatic ball/Paddle position synchronization and owner-only results |
| shobon_jump | CharacterBody horizontal/vertical movement, automatic floor/velocity data, tackle interactions, and stage progression |

## Script rules

World scripts contain an online annotation or extend Online/Secret, regardless of directory. Their first non-comment line must be extends; class_name is not allowed. The base type must match the scene root type, Online, or Secret. Names cannot start with `__`; my, world, and the first-argument string auto_off are reserved.

## Fine tuning

Defaults work without tuning. Specify these only to control rates, interpolation, or prediction.

- `@online(10)` and `@online(position, 10)` set the maximum updates per second. Otherwise the rate is selected from purpose and measured changes.
- Interpolation is chosen by name. The default is inferred from Node/property type and has no explicit name. Arguments are interpreted by type, not order: numbers supply update rate and history length, strings select interpolation. Examples include `@online(position, 20, "arc", 3)` and `@online(position, "arc")`.

| Mode | Client display | Typical use |
|---|---|---|
| snap | Applies received values directly | Grid movement, teleports, collision objects |
| ease | Interpolates without predicting | Objects that may lag one sample |
| predict | Interpolates and predicts constant velocity during missing updates | Projectiles, vehicles, steady motion |
| arc | Interpolates and predicts acceleration including gravity | Jumping and thrown objects |

Prediction stops after 0.15 seconds without updates, then corrects toward the next received value. Longer extrapolation would increase error. Automatic CharacterBody synchronization already advances vertical motion using floor/velocity data, so it does not require extending this interval.

- Single-step jumps above Secret.teleport_px (default 200) snap rather than interpolate. Raise it when legitimate world movement exceeds that distance in one tick.
- Lower update rates to reduce traffic. `@online(position, 5, "arc")` sends five updates per second while Clients advance along the arc between updates.
- The final number selects previous samples used for velocity estimation: 1–7, default 1. More samples smooth noise but slow response to direction changes.

## Running a server

First startup creates a P-256 private key and self-signed certificate under user://online_secret/. POSIX directory/key modes are 0700/0600. For direct server execution, set GD_USER_HOME to an absolute persistent-data path.

Secret.listen_host, listen_port, and secret_name configure listening and identity. Headless authority caps rendering frames at 60 unless the project sets max_fps; the world uses 20 Hz.

```sh
# Run the server export template built below.
./gd-godot.linuxbsd.template_release.x86_64.online_server --headless --main-pack game.pck -- --online-server
```

The server preset creates Dockerfile and compose.yaml beside the PCK. PCK-only exports also copy the selected server executable there. Run `docker compose up -d --build` in that directory to start the server and Redis. GD_USER_HOME is `/online_data`; keys and connection keys persist in the online_data volume, and Redis data in redis_data. Retrieve only the public certificate with `docker compose cp online:/online_data/online_secret/certificate.pem ./online-ca.pem` and set Client.ca to it. Expose listen_port for both TCP and UDP.

For local testing, use Project → Tools → the Online test-container command. It exports the server preset under `res://.godot/online_container/`, starts a container, and copies the certificate to the configured project CA path. Existing certificates are not overwritten; the retrieved certificate's location is reported instead. Stop it from the same menu. Docker is the only additional prerequisite.

Online_save uses Redis configured in Secret's Save group. Empty redis_host disables persistence.

Two hashes store data: owned Nodes use `<prefix>:<accountID>` with `<kind>.<field>` entries; unowned Nodes and Secret use `<prefix>:world` with `<path>.<field>`. Values retain binary Variant types, including Vector2 and Color. Only changed fields are written, and removed objects' fields are deleted. `<path>.@kind` prevents restoring values to a different kind at the same path. Inspect world entries with `redis-cli HKEYS <prefix>:world`.

Online_save and online_save_my share storage; only their recipients differ. Login reads account values and restores them immediately after owned-object spawning, even if generated world names have changed. Redis I/O does not stop world processing, but startup and login wait for restoration. Each save uses MULTI/EXEC so partial transactions do not survive a disconnect.

Fixed scene objects and named spawns retain path-based values until queue_free. Owned objects retain one saved object per kind in the account, including after disconnect. If a player owns two of the same kind, the later one wins and authority reports this once.

When Redis is unreachable, new logins receive `The save store is not reachable` and retry. Existing players/world continue; changed values are saved after reconnection. Startup waits for Redis rather than starting with defaults that could overwrite real saved data.

`<prefix>:device:<device-key>` links a device to its account. The key is an HMAC derived with connect_key, or the generated user://online_connect_key, so Redis does not reveal the original device ID. Account IDs are independent random values.

Distribute only the public CA certificate to Clients. Private keys remain on the server. C++ validates guest installation IDs, sessions, reconnect tokens, heartbeats, command frequency, types, and ownership.

## Matchmaking

With match_url, Clients ask for an available room rather than selecting a server. A room is a running authoritative process that registers itself.

Authoritative Secret settings:

| Setting | Example |
|---|---|
| listen_port | 4433 |
| match_url | https://match.example.com/pong |
| match_key | Shared matchmaking key |
| match_room | tokyo-1 |
| room_host | tokyo-1.example.com |

Client Secret settings; do not specify host or port:

| Setting | Example |
|---|---|
| secret_name | game.example.com |
| ca | res://online-ca.pem |
| match_url | https://match.example.com/pong |

Both sides use the same URL. Request contents distinguish registration from discovery. The URL path names the game, allowing several games on one matchmaking server. Use a different path, such as `/pong/2`, for incompatible versions.

The standard gd module GD.online.match returns the fullest room that still has space so players find each other. The runnable server is samples/online/main.gd, configured through MATCH environment variables:

```sh
MATCH_KEY=<shared-key> MATCH_REDIS=127.0.0.1:6379 gd serve samples/online/main.gd
```

See GD.online in the [manual](manual.en.md).

## Player-hosted worlds

A single identity-only server using online-secret allows play without dedicated world servers. When no room is available, the requesting device becomes a room. Matchmaking selects the host automatically; neither author nor player chooses. Only identity and matchmaking services remain permanently running.

```sh
# Run identity services without running a world.
./gd-godot.linuxbsd.template_release.x86_64.online_server --headless --main-pack game.pck -- --online-secret
```

Identity Secret configures listen_port, secret_name, match_url, match_key, and redis_host. Clients use the matchmaking settings above.

Hosts are untrusted and never receive the shared key. Only a device selected by matchmaking may host; Secret verifies the signed room ticket with the shared key. Each connection owns one room. A room closes after rejecting three consecutive players, including failures to answer within five seconds.

```text
Client --TLS--> Secret   Login and reconnection
Client --DTLS-> Host     State and online function commands
Host   --TLS--> Secret   Registration, authentication, saving, private operations
```

Hosts run worlds but do not hold identities, saves, or Secret implementations. Online functions on Secret forward to the identity server and return only their answers. Secret writes account values to Redis. Hosted Client PCKs contain no Secret script contents, even if modified. Authors use the same scripts as in a single-process world.

Hosts may save only the declared online_save fields for accounts currently in their room. Unowned world values are not persisted in this mode because rooms have independent worlds. Attempts to save absent accounts, unknown fields, or world data are rejected and logged once per reason with Refused.

Hosts can still choose values for players in their room and call exposed Secret functions with arbitrary arguments. This is a trust tradeoff of player hosting: validate Secret arguments as untrusted input.

When all rooms are full, a new host creates another room. Room_max limits one room, not the total player population. When a host leaves, crashes, or stops responding, Secret detects it within five seconds and informs the other players. They immediately ask matchmaking again; another player becomes host within seconds. The world restarts, but account values restore from Secret's Redis.

Hosts select an available state port starting after the identity port and use a disposable certificate issued by Secret. Secret includes that certificate in Client authentication responses, requiring no additional Client setting. Home-network hosts must make that port reachable from outside.

## Client

Defaults are 127.0.0.1:4433 and res://online-ca.pem, requiring no local-test configuration. Any online annotation triggers automatic connection generation; the ordinary main scene remains the entry point.

World DTLS packets carry numeric field identifiers and use range coding. For 200 ticks of four players' position/velocity data, traffic is 111.5 KB → 30.6 KB, about a 3.6-fold reduction. A world with 200 moving objects costs approximately 5.5 ms per authoritative tick (3.2 ms user physics, 2.3 ms synchronization collection); a two-player world costs 0.4 ms.

## Distribution boundaries

- The editor includes parsing, checking, and server startup.
- Ordinary Online export templates include synchronization, interpolation, and command transmission. They expose the Secret Node type but omit the authentication vault C++ object included in server templates.
- Secret scripts never enter Client PCKs intact. Export replaces each extends Secret script with that declaration alone and removes Secret children and script references from scenes.
- Client exports retain only secret_name, ca, teleport_px, room_host, room_port, match_url, and room_max. Listen settings, match_key, match_room, Redis settings, account_providers, and keep_sec are stripped.
- Scripts used only below Secret are stripped too. Place shared scripts outside Secret if Clients also need them.
- Secret scripts must be external `.gd` files. Embedded scene scripts cannot be stripped and are rejected.
- Only presets marked online_server retain private scripts. Selecting a template whose name contains online_server applies the mark automatically.
- Server templates include guest authentication, TLS/DTLS, authoritative worlds, object creation/deletion, synchronization, and Redis persistence.
- World scenes and world scripts remain in server PCKs because execution requires them.
- World scenes remain text in PCKs regardless of export settings because runtime reads their text.

## Tests

```sh
scons view=yes online_secret_enabled=yes dev_build=yes # Build the executable.
tests/mv/run.sh # All tests; exit status is the failure count.
tests/mv/run.sh T-SYNC-01 # One test.
tests/mv/run.sh --slow # Include slow Redis outage cases.
```

Cases and test instructions live under tests/mv/cases/ and tests/mv/README.md in the development repository.

Tests wait for events—log lines, listening ports, Redis keys, and process exits—with 0.05-second polling and bounded deadlines. Each test isolates port ranges, Redis prefixes, and user directories for parallel execution. Tests using login callback port 4435 run serially. Samples remain unchanged; fixtures modify copies. Redis-server and Python 3 are required.

## Upstream updates

Use the same process as gd: merge with devtools/upstream.sh and record with devtools/godot_patches.sh. Engine changes are in patches/godot/online.patch and language.patch. Node and SceneTree hooks used only by display builds are in cli/view/online.patch and applied when view=yes prepares upstream source.

## Build

Online belongs to the gd-godot display executable, not the default gd CLI.

```sh
scons view=yes online_secret_enabled=yes # Editor.
scons view=yes target=template_release # Client distribution.
scons view=yes target=template_release online_secret_enabled=yes extra_suffix=online_server # Server.
```
