# gd-godot manual

English | [日本語](gd-godot.md)

`gd-godot` provides the Godot editor, display, and game runtime. It is distributed separately from the terminal oriented `gd` executable.

The features in this document are in development. Their usage and behavior may change without notice.

## Install and open

```sh
gd install -g gd-godot
gd editor
gd editor path/to/project
gd run-game path/to/project
```

`gd install -g gd-godot` installs the official package matching the CLI version, operating system, and architecture. `gd editor` and `gd run-game` use this global installation process when needed and reuse the same installation afterward. Storage follows the normal global command settings; use `GD_INSTALL_ROOT` to change it. Project dependencies are unaffected. For local development with a build-suffixed executable, the matching `gd-godot` beside it takes priority.

`gd run-game` waits for the game and returns its exit status. Pass game arguments after `--`.

## Projects and addons

`gd editor <project directory>` opens that project. Run `gd install --godot` there to copy packages listed under `imports` in `gd.json` into `addons/<alias>/`. The `add`, `update`, `uninstall`, and `info` commands also accept `--godot`. The upstream Godot editor can read these files through `res://addons/`. This selects the destination; it does not translate gd-specific APIs or syntax.

Addons fetched from Godot asset channels are recorded under `assets`, and their ZIP's `addons/` tree is placed in the project. Ordinary packages are copied as a whole, even when they contain an `addons/` directory.

When building the display executable from source, keep `gd` and `gd-godot` for the same platform, target, and architecture together. `scons view=yes` selects the display build.

## Online features

Use `Online` and `Secret` for game synchronization and guest authentication. Manage matchmaking destinations with `GD.online.match()` in `gd`. The [Online guide](online-guide.en.md) and [details](online.en.md) describe the setup. External account linking requires a provider configured in `Secret`.

For Godot's built-in high-level multiplayer, create an `ENetMultiplayerPeer` in normal execution without `serve` and assign it to `get_tree().get_multiplayer().multiplayer_peer`. The server relays traffic between clients by default.

## Related guides

- [gd manual](manual.en.md): terminal language, standard API, and package management
- [Online guide](online-guide.en.md): game networking
