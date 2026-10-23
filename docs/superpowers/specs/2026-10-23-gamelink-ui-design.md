# gamelink UI - design

Date: 2026-10-23. Status: for approval.

## Intent

FrostyGameLink (gamelink) is an in-process DLL for Star Wars Battlefront II (2017) that can read and change almost
anything in a running offline game, but today it is driven by typing text commands into a TCP port - usable by
scripts and AI, not by people. This project gives it a **clean in-game menu so a person can use every feature by
pointing and clicking**, and above all **see and understand the game's logic as it happens**, then releases it
publicly with docs for both players and developers/AI tools.

The owner's words: "all the features easy to use, visual or actually useful, not just AI use"; logic shown "not
just a mess with 100s of events going off with no human understanding but something you could understand and learn
from"; "have something where you can alter stuff as well". Reference point: a Kyber-creator showcase where throwing a
grenade shows that grenade's logic firing in real time.

## Goals

1. An in-game menu (ImGui, already in gamelink) toggled by a hotkey, mouse-driven, covering every human-useful
   gamelink feature.
2. A **Logic Debugger**: pick one thing, see its logic as a graph that lights up live as events fire, with a
   plain-language timeline, pause/slow-mo/replay, and the ability to change values and fire events by hand.
3. An **Inspector**: click anything in the world, see what it is (asset path, mesh, class, position), edit its
   properties in a typed table.
4. Existing power features as controls: free camera, time scale, view modes, bot battles (64v64), diagnostics.
5. A **one-click launcher** so a player never touches injectors or command lines.
6. A **public release**: player download (binaries + launcher + simple README) and source with full developer/AI
   documentation (command protocol, every command, architecture, offsets and how they were found).

## Non-goals

- Online play. gamelink refuses to run when it detects online/multiplayer services in use (see Safety).
- Saving changes back into game files. Live edits last until the level ends. (Frosty remains the tool for
  permanent mods; the Inspector shows asset paths so users can find things in Frosty.)
- A full visual scripting editor. The Logic Debugger shows and pokes existing logic; it does not author new graphs.
- Supporting other games or other BF2 builds. One build, byte-checked.

## Anti-goals (would make it a failure even if it "works")

- An unreadable firehose: hundreds of unrelated events scrolling by. Every logic view is scoped to what the user
  picked and written in readable names.
- Crashing or freezing the user's game because of the tool. Every UI action goes through guarded reads/writes;
  heavy work (memory scans) never runs per frame; hooks that cost frames (event trace) run only while a view needs
  them.
- Hijacking the user's PC: no global input, no desktop screenshots, nothing outside the game window.
- A dev-tool UI full of hex addresses. Addresses appear only in an "advanced" fold-out.

## Constraints

- Lives inside the existing gamelink DLL (`src/main.cpp`, one translation unit) and its ImGui 1.78 DX11/Win32 backend
  (already integrated, currently a non-interactive feed overlay toggled with F9).
- All engine knowledge is build-specific (BF2 final PC build, module size 0xE45C000). Each feature byte-checks
  what it patches and degrades to "unavailable on this game version" instead of failing silently.
- The text/TCP command interface stays exactly as it is (scripts, the Frosty bridge and AI tools depend on it);
  the menu calls the same internal functions.
- Owner's machine: C: is nearly full; work and outputs on D:.

## The menu

- **Hotkey: F1** opens/closes (F9/F10 keep their current overlay meaning). While open: the cursor is shown, mouse
  and keyboard go to the menu, the game receives no input (gamelink's existing `inputoff`), the game keeps running
  unless the user pauses it. A small "gamelink" badge in a corner shows it is active and the version check result.
- Window: one resizable, movable main window with tabs; layout and options persist between sessions in
  `%LOCALAPPDATA%\gamelink\ui.ini` (ImGui ini + a small settings file).
- Look: ImGui dark style, tuned (rounded panels, spacing, readable font size scaled to screen height); colours
  carry meaning consistently (events orange, properties blue, selection green, warnings amber).

### Tabs

1. **Camera** - Free camera on/off (WASD/mouse, speed slider, shift/alt fast/slow as today), FOV slider, hide HUD,
   lock camera, save/restore up to 8 camera spots by name, "Photo mode" (free cam + HUD off + time scale 0),
   screenshot button (game-only render capture, saved to `Pictures\gamelink`).
2. **World** - Level and game mode shown in words; time scale slider (0 / 0.1 .. 3x) with a pause button; view
   modes (lit, unlit, wireframe ... whatever `viewmode` supports) as a dropdown; sub-levels list (read-only).
3. **Inspector** - see below.
4. **Logic** - see below.
5. **Bots** - the 64v64 work: team size per team (1..64), spawn speed, apply; live readout "players / soldiers
   alive" per team; the player-cap and pool fixes are switched on automatically and shown as status lines. Requires
   injection before the match starts (the launcher does this); if the current match was created before injection,
   the tab says so and offers "takes effect next match".
6. **Diagnostics** - FPS and frame-time graph (`frametimes`), server tick rate, gamelink and hook status, game
   version check, log tail, "copy diagnostics to clipboard" for bug reports.
7. **Advanced** - a command console for any text command (same as the TCP interface), and the raw tools (peek,
   types/fields search, trace feed) for modders.

### Inspector

- **Pick:** "Pick in world" button (or hold Alt + click while the menu is open) → click in the game. Uses the
  existing bounding-box picker; because boxes overlap, a **"under the cursor" list** shows every hit (nearest
  first, with asset names) and the user chooses. Also a searchable list of all level objects/entities.
- **Shows:** asset path (e.g. `Objects/Architecture/Naboo/Palace/O_NAB_DoorPalace_03`), runtime class, mesh path
  where found, position/rotation, owning blueprint and sub-level, a highlight box + label in the world.
- **Edit:** reflected fields in a table with typed widgets (checkbox, number drag, vector3, colour, enum dropdown,
  read-only for references with their target names). Changes apply live (existing `entityset`). "Move" gizmo
  (existing) and "teleport camera to it" / "look at it". A per-field "revert" restores the value read on selection.
- **Hand-off:** "Watch its logic" opens the Logic tab focused on it; "Copy asset path" for Frosty.

### Logic Debugger (the centrepiece)

**What to watch** (my call): any of
- the object picked in the Inspector,
- **"What I'm aiming at"** (the object under the crosshair, one key),
- **"Follow my actions"**: whatever the local player's soldier and current weapon/gadget trigger (covers the
  grenade case: throw it and its logic appears),
- or a blueprint chosen from a searchable list.

**The graph:** nodes = the logic entities of the watched blueprint (and, one level out, the prefabs it references,
collapsed until expanded); edges = its event connections (solid, orange) and property connections (dashed, blue),
from gamelink's existing `wires` data. Laid out left-to-right by first-fire order once anything fires, by a
simple layered layout before that. Node title = readable type (e.g. "Timer", "Explosion", "ApplyDamage") + its
name/asset when it has one; ports show event/property names resolved through the hash tables gamelink already
carries. Pan/zoom, click a node to see its properties (same typed table as the Inspector).

**Live:** the event trace hook (existing, only enabled while the Logic tab is watching something) records every
connection that fires; only connections belonging to the watched blueprint(s) are kept. A firing edge pulses
along its length, the target node flashes, a counter on the edge counts fires. Property changes show the new value
on the edge for a moment.

**Timeline (beside the graph):** each step as a sentence with time since the first event, e.g.
`0.00s  Grenade (Thrown) → Fuse timer: Start` / `3.00s  Fuse timer: Finished → Explosion: Detonate`. Repeated
identical steps collapse ("×12"). Clicking a line highlights it in the graph.

**Control:** pause (freezes the game with time scale 0 and stops recording), slow-mo (0.25x), step (advance one
recorded step in the view), replay (re-animates the last recording in the graph without re-running the game),
clear.

**Alter (the owner's "alter stuff"):**
- Edit any node's properties live (e.g. fuse time, damage, radius) from its table.
- **Fire an event by hand** on a node's input port (existing `fire`), e.g. trigger "Detonate" now.
- Toggle a node/connection off for the rest of the level (writes the node's enabled/realm-safe flag where the
  engine has one; otherwise not offered).
- Every change is listed in a "changes" panel with undo.

**Honest limits, shown in the UI, not hidden:** logic that runs in C++ engine code (not in blueprints) appears as
a single "engine" step with no inner detail; client-only and server-only logic are both shown, labelled by realm.

## Launcher (my call: a small Windows app, C++/Win32 + ImGui, same toolchain)

- Finds the game (registry `HKLM\SOFTWARE\(WOW6432Node\)EA Games\STAR WARS Battlefront II\Install Dir`, then Steam
  library paths; manual "browse" fallback), checks the exe version, shows status.
- Options: enable 64v64 bots (team sizes), open menu on start, (later) presets.
- **Play**: starts the game the normal way (through the EA app/Steam - no DRM bypass, no Mod Manager needed) and
  injects gamelink as soon as the game process is up (early, so the player cap and pool fixes are in before any
  match), elevating once via UAC only if the game runs elevated.
- Also works with Frosty Mod Manager: "Inject into running game" button for people who launch through FMM.

## Safety

- Offline only: gamelink checks that the game is not connected to an online/multiplayer session (the client's
  online state / server peer type) and, if it is, disables everything except Diagnostics and shows why. (Exact
  signal to be found during build; if none is reliable, the launcher and README state "offline only" prominently
  and the DLL refuses injection when the EA online multiplayer front end is active.)
- Version check on load: module size + code bytes at every patch site; mismatch → "unsupported game version",
  nothing patched.
- No desktop capture, no global input, no files written outside `%LOCALAPPDATA%\gamelink` and `Pictures\gamelink`.

## Release (public, MIT, repo `Rapid421/bf2-gamelink`)

- **Player download** (GitHub Release zip): `gamelink_launcher.exe`, `FrostyGameLink.dll`, `gamelink_inject.exe`,
  README-PLAYERS (download → run launcher → press F1, with screenshots), licenses (MIT + ImGui MIT).
- **Source:** curated tree already assembled in `D:\bf2-gamelink` (src, imgui, injector, client, scripts, harness),
  personal paths removed, private repo untouched.
- **Docs for developers/AI:** `docs/PROTOCOL.md` (TCP, one command per connection, reply format), `docs/COMMANDS.md`
  (every command - generated from the source comments so it never drifts), `docs/ARCHITECTURE.md` (threads, hooks,
  overlay, feeds, guarded memory), `docs/OFFSETS.md` (every address, what it is, how it was found, how to re-find it
  on a new build), `docs/HARNESS.md`, `docs/case-studies/64v64.md`, plus an `AGENTS.md` telling AI tools how to drive
  gamelink safely.

## Build order (each stage ends with a test in game and is usable on its own)

1. Menu shell: F1 toggle, interactive input, input-blocking, settings persistence, badge; Camera, World,
   Diagnostics tabs; Advanced console.
2. Bots tab with 64v64 integrated (port the race-free cap patch from the standalone DLL into gamelink).
3. Inspector: pick + under-cursor list, info panel, typed property table with edit/revert, highlight.
4. Logic Debugger: scoped trace, graph + live pulses, timeline, pause/slow-mo/replay, alter (edit, fire, undo),
   "follow my actions".
5. Launcher.
6. Docs, release packaging, public repo + GitHub Release.

## How it gets tested

- Each stage: build, launch through the harness on a shipped level (and for 4: an Instant Action match with
  grenades), exercise the UI, check gamelink's log for faults and `frametimes` for cost; screenshots (game-only) of
  each tab for the owner to review.
- The owner plays a build at the end of stages 1, 3, 4 and 5 and says what's confusing.
- Release check: a clean folder, launcher only, normal game start - no harness, no dev paths.

## Left to the builder

- Exact widget layout and wording within each tab; graph layout algorithm details; how many recorded steps to
  keep (default: last 2,000 for the watched scope); font choice (ImGui default scaled, or an embedded open font).
- Which reflected field types get special widgets beyond the ones listed.
- Internal structure of the new UI code (may split `main.cpp` into a few files if it stays one build).

## Calls I made (owner can overrule)

F1 hotkey; game input blocked while the menu is open; launcher as a native app; logic watch modes (pick / aim /
follow my actions / list); "engine" placeholder for C++ logic; live edits are not saved to files; repo name
`bf2-gamelink`, MIT.
