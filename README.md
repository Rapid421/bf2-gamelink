# gamelink for Star Wars Battlefront II (2017)

gamelink is a DLL you load into a running **offline** Battlefront II. It can read and change almost anything in the
game while you play, through an in-game menu (press **F1**) or through simple text commands on a local port, for
scripts and AI tools.

> **Work in progress.** This repository is updated as features land. What works today is listed below. What is
> still being built is listed under [Roadmap](#roadmap).
>
> **Offline only.** gamelink is for offline play (Instant Action, Arcade, your own test levels). Do not use it
> online: it is not made for that, and EA's terms forbid modified clients there.

## What works today

**The F1 menu.** Press F1 in game to open it. While it is open, the game gets no keyboard or mouse input and the
cursor is free. F1 or clicking another window closes it.

| Tab | What you can do |
| --- | --- |
| **Camera** | Free camera (WASD + mouse, adjustable speed), field of view, hide the HUD, photo mode (free camera + no HUD + time stopped), screenshots to `Pictures\gamelink`, and 8 named camera spots you can save and fly back to |
| **World** | Level and mode names, time scale (pause, slow motion, normal), render view modes |
| **Diagnostics** | Live frame-time graph, FPS, p99, server tick rate, the gamelink log, "Copy diagnostics" |
| **Advanced** | A console for any gamelink command, with history and a searchable list of every command |
| Inspector, Logic, Bots | Not finished yet - see the roadmap |

The first time you change an engine setting (time scale, view mode, HUD, FOV) in a match, it can take up to about
30 seconds: gamelink has to find the setting in memory once. After that, changes are instant.

**The command port.** gamelink listens on `127.0.0.1:31500`. Send one line, get one reply:

```
ping              -> pong gamelink
levelname         -> loaded level: S8/Felucia/Levels/MP/Felucia_01/Felucia_01
help              -> every command with a one-line description
ui open           -> opens the F1 menu
```

`client/gl.py` and `client/gl.ps1` are small clients. There are about 130 commands: camera control, reflection
(read and write any field of any engine type), entity lists, picking, blueprint event tracing and firing, frame
capture, and more. `help` lists them all.

## Supported game version

The PC build of Battlefront II as last updated by EA (2020), module size `0xE45C000`. gamelink checks
this when it loads. On any other build the corner badge reads "unsupported game version" and the build-specific
hooks stay off.

## Building

Needs Visual Studio 2019 or newer with the C++ desktop workload.

```bat
build.bat          :: builds bin\FrostyGameLink.dll
build.bat test     :: builds and runs the unit tests (bin\ui_tests.exe)
```

## Loading it into the game

For now, inject `bin\FrostyGameLink.dll` into `starwarsbattlefrontii.exe` with any DLL injector, at the main menu or
in a match. A one-click launcher is on the roadmap. Its log is `C:\ProgramData\FrostyGameLink\gamelink.log`.

## Repository layout

| Folder | What is in it |
| --- | --- |
| `src/main.cpp` | gamelink itself: hooks, the command server, every command |
| `src/ui/` | the F1 menu: tabs, command queue, settings, actions (unit-tested pieces have no game dependency) |
| `src/imgui/` | Dear ImGui 1.78 (MIT) |
| `tests/` | unit tests for the menu logic (`build.bat test`) |
| `client/` | command-line clients for the port |
| `harness/`, `scripts/` | the automated in-game test harness used during development. These scripts still assume the developer's folder layout (`F:\Frosty\...`); they are being made portable |
| `docs/` | design and implementation plans |

## Roadmap

1. **Bots tab** - team sizes up to 64 v 64 (the engine work is done and proven; the tab is being built now)
2. **Inspector** - click anything in the world and see what it is: asset path, mesh, class, position; edit its
   properties
3. **Logic debugger** - pick one object and watch its logic fire as a readable graph and timeline, with pause,
   slow motion and the ability to fire events and change values by hand
4. **One-click launcher**, player download, and full developer and AI documentation of every command

## Licence

MIT - see [LICENSE](LICENSE). Dear ImGui is MIT, Copyright Omar Cornut.
