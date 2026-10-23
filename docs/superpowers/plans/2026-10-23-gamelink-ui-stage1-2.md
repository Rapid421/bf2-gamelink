# gamelink UI - Stages 1-2 Implementation Plan (menu shell, Camera/World/Diagnostics/Advanced, Bots)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An F1 in-game menu in FrostyGameLink with working Camera, World, Diagnostics, Advanced and Bots (64v64) tabs, usable with mouse and keyboard while the game gets no input.

**Architecture:** New UI code lives in `src/ui/*` and talks to the rest of gamelink only through `src/gamelink_api.h` (implemented in `main.cpp`), so its logic is unit-testable against a fake. ImGui widgets turn into text commands posted to a background `CommandQueue` that calls gamelink's existing `HandleCommand`, so no menu action can stall a frame. "Menu open" is one flag read by gamelink's existing raw-input and cursor hooks (block game input, free the cursor) and by a WndProc subclass that feeds ImGui.

**Tech Stack:** C++17, MSVC (`build.bat`), Dear ImGui 1.78 (DX11 + Win32 backends, already vendored in `src/imgui`), no test framework (a ~40-line CHECK runner in `tests/`).

**Spec:** `docs/superpowers/specs/2026-10-23-gamelink-ui-design.md` (stages 1-2 of "Build order"; stages 3-6 get their own plans).

## Global Constraints

- The TCP command interface keeps every existing command and reply unchanged.
- Hotkey F1 opens/closes the menu; F9/F10 keep their current overlay meaning.
- While open: cursor shown and free, game receives no input, game keeps running.
- Settings persist in `%LOCALAPPDATA%\gamelink\ui.ini` (ImGui layout) and `%LOCALAPPDATA%\gamelink\settings.ini` (options); missing or corrupt files mean defaults, never a failure.
- Colours: events orange `#F0A040`, properties blue `#62B4FF`, selection green `#5CD67A`, warnings amber `#FFC247`.
- No hex addresses outside the Advanced tab.
- Nothing may take the game down: every UI frame stays inside the existing `__try` guard in `OverlayFrameGuarded`; any exception turns the UI off for the session and logs why.
- Build: `build.bat` from any folder; outputs in `bin\`. Tests: `build.bat test` builds and runs `bin\ui_tests.exe`, exit code 0 = pass.

## Review Focus

- Menu opened before a level is loaded (front end, loading screen): tabs show "Not in a level yet" and greyed controls; no command is sent that needs a level. Test in Task 5.
- Alt-Tab or game losing focus while the menu is open: input to the game and the real cursor are restored on focus loss, the menu closes. Test in Task 4.
- A slow command (registry walk on first `viewmode`, 20 s memory scans): the UI keeps drawing at full rate and shows "working…" on that control. Test in Task 1.
- Missing/corrupt `settings.ini` (empty file, garbage, out-of-range numbers): defaults, clamped values. Test in Task 2.
- Window resize / resolution change while open: menu re-lays out, no crash (render target view is created per frame already). Checked in Task 5's in-game check.

---

### Task 1: API seam, test runner, CommandQueue

**Files:**
- Create: `src/gamelink_api.h`, `src/ui/command_queue.h`, `src/ui/command_queue.cpp`, `tests/test_main.cpp`, `tests/fake_gamelink.cpp`, `tests/test_command_queue.cpp`
- Modify: `src/main.cpp` (add `gl::Command` wrapper near `HandleCommand`, ~line 11939), `build.bat` (compile `src/ui/*.cpp` into the DLL; add `test` target)

**Interfaces:**
- Produces:
  - `namespace gl { std::string Command(const std::string& line); }` - thread-safe call into `HandleCommand`.
  - `class CommandQueue { public: explicit CommandQueue(std::function<std::string(const std::string&)> exec); ~CommandQueue(); uint64_t Post(const std::string& line); bool Busy(uint64_t id) const; bool Reply(uint64_t id, std::string* out) const; std::string LastReply(const std::string& prefix) const; };` - one worker thread, FIFO, keeps the last 256 replies; `LastReply("timescale")` returns the latest reply to a command starting with that verb.
  - Test runner macros `CHECK(cond)` and `TEST(name)` in `tests/test_main.cpp`; fake: `tests/fake_gamelink.cpp` records every line in `std::vector<std::string> g_fakeLog` and answers from `std::map<std::string,std::string> g_fakeReplies` (default reply `"ok"`), with optional delay `g_fakeDelayMs`.

- [ ] **Step 1: Write the failing tests** (`tests/test_command_queue.cpp`)
  - `queue_runs_in_order`: post `"a"`,`"b"`,`"c"`; wait until not busy; `g_fakeLog == {"a","b","c"}`.
  - `queue_does_not_block_caller`: `g_fakeDelayMs = 500`; `Post("slow")` returns in < 20 ms; `Busy(id)` true right after.
  - `last_reply_by_verb`: replies for `"timescale 0.5"` = `"time scaled"`; `LastReply("timescale") == "time scaled"`.
- [ ] **Step 2: Run** `build.bat test` → FAIL (CommandQueue undefined).
- [ ] **Step 3: Implement** `CommandQueue` (std::thread + std::mutex + std::condition_variable; destructor joins) and `gl::Command` (calls `HandleCommand` under a mutex shared with the TCP listener's dispatch so commands never run concurrently with each other).
- [ ] **Step 4: Run** `build.bat test` → `3 passed`, exit 0; `build.bat` → `bin\FrostyGameLink.dll` builds.
- [ ] **Step 5: Commit** `feat(ui): command queue and API seam with unit tests`

### Task 2: Settings persistence

**Files:** Create `src/ui/settings.h`, `src/ui/settings.cpp`, `tests/test_settings.cpp`

**Interfaces:**
- Produces: `struct UiSettings { float flySpeed = 40.f; float fov = 0.f /*0 = game default*/; float timeScale = 1.f; int teamSize1 = 64; int teamSize2 = 64; bool openOnStart = false; std::string camSpots[8]; /* "name x y z yaw pitch" or empty */ };`
  `bool LoadSettings(const std::wstring& path, UiSettings* out);` (false + defaults on missing/corrupt) and `bool SaveSettings(const std::wstring& path, const UiSettings& s);`
  `std::wstring SettingsPath();` → `%LOCALAPPDATA%\gamelink\settings.ini` (creates the folder).
- Clamps on load: flySpeed 1..500, fov 0 or 30..120, timeScale 0..3, teamSize 1..64.

- [ ] **Step 1: Failing tests:** `roundtrip_all_fields`; `missing_file_gives_defaults` (returns false, defaults); `garbage_file_gives_defaults` (file content `"\x00\xFFzzz"`); `out_of_range_is_clamped` (`flySpeed=99999` → 500, `teamSize1=0` → 1, `fov=10` → 30).
- [ ] **Step 2: Run** `build.bat test` → FAIL.
- [ ] **Step 3: Implement** with `GetPrivateProfile*W`/`WritePrivateProfileStringW`, section `[gamelink]`.
- [ ] **Step 4: Run** → all pass.
- [ ] **Step 5: Commit** `feat(ui): settings load/save with clamping`

### Task 3: Actions (what each control sends)

**Files:** Create `src/ui/actions.h`, `src/ui/actions.cpp`, `tests/test_actions.cpp`

**Interfaces:**
- Produces (each returns the exact command lines, in order):
  `std::vector<std::string> ActFreeCam(bool on, float speed);` → on: `{"camtake 1", "fly 1 <speed>", "inputoff 1"}`; off: `{"fly 0", "camtake 0", "inputoff 0"}`
  `ActHud(bool show)` → `{"hud 1"|"hud 0"}`; `ActFov(float deg)` → `{"forcefov <deg>"}` or `{"forcefov -1"}` for 0;
  `ActTimeScale(float s)` → `{"timescale <s>"}` (s clamped 0..3, formatted `%.2f`); `ActViewMode(int m)` → `{"viewmode <m>"}`;
  `ActScreenshot(const std::wstring& dir)` → `{"grabframe <dir>\\gamelink_<yyyymmdd_hhmmss>.bmp"}` (timestamp injected via a `std::function<std::string()>` clock parameter for tests);
  `ActCamSave()` → `{"camera"}` (reply parsed by `bool ParseCamera(const std::string& reply, float pos[3], float* yaw, float* pitch)`), `ActCamGoto(const float pos[3], float yaw, float pitch)` → `{"camwarp <x> <y> <z> <yaw> <pitch>"}` (check `camwarp` argument order in `main.cpp` and match it).
  `bool ParseFrameTimes(const std::string& reply, FrameStats* out);` with `struct FrameStats { int frames; float fps, avg, p50, p95, p99, max; int over33, over50, over100; };` (reply format is the `frametimes` command's existing output).
- [ ] **Step 1: Failing tests:** one per function above with the exact strings listed; `ParseFrameTimes` against the literal reply `"frametimes over 12.0 s: 1421 frames, 118.4 fps, avg 8.44 ms, p50 8.19, p95 10.72, p99 12.82, max 31.86 ms; >33ms 0, >50ms 0, >100ms 0"` → fps 118.4, p99 12.82, over33 0; `ParseCamera` against a real `camera` reply copied from a live session (capture it in Task 5's check and paste it into the test then; until then use `"camera pos 1 2 3 left ... forward 0 0 1 fovx 1.2"` built from the reply format in `main.cpp`).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → pass. **Step 5: Commit** `feat(ui): control-to-command actions and reply parsers`

### Task 4: Menu state and input capture

**Files:** Create `src/ui/menu_state.h`, `src/ui/menu_state.cpp`, `tests/test_menu_state.cpp`; Modify `src/main.cpp` (raw-input/cursor hooks ~line 8358-8600, `OverlayKeys` ~line 10565, `HandleCommand` new `ui` verb)

**Interfaces:**
- Produces: `class MenuState { public: void Toggle(); void Open(); void Close(); bool IsOpen() const; void OnFocusLost(); int Tab() const; void SetTab(int); };` with tabs `enum Tab { Camera, World, Inspector, Logic, Bots, Diagnostics, Advanced };` (Inspector/Logic show "Coming soon" until stages 3-4).
  `bool gl::MenuCapturesInput();` read by the hooks.
  TCP: `ui open|close|toggle|status|tab <name>` → status reply `"ui open tab Camera input blocked"` / `"ui closed"`.
- Behaviour in `main.cpp`: when `MenuCapturesInput()`: the GetRawInputData/GetRawInputBuffer hooks drop real keyboard/mouse records (synthetic ones still pass); ClipCursor/SetCursorPos hooks do nothing; `ShowCursor` count raised to visible on open and restored on close; the game window's WndProc is subclassed once (on first open) to pass messages to `ImGui_ImplWin32_WndProcHandler` and swallow mouse/keyboard messages while open. F1 edge-triggered in `OverlayKeys` (game window in front) toggles; `WM_ACTIVATEAPP` with FALSE → `OnFocusLost()` closes the menu. The F-key arming (`g_overlayKeysArmed`) is set on attach for F1 (F9/F10 stay armed only by `overlay on`).
- [ ] **Step 1: Failing tests** (`MenuState`): `toggle_opens_and_closes`; `focus_lost_closes`; `tab_persists_across_toggle`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement** `MenuState` and the `main.cpp` wiring above. **Step 4: Run** unit tests → pass; `build.bat` → DLL builds.
- [ ] **Step 5: In-game check** (harness, Felucia Mode8): `ui open` → `ui status` = `ui open tab Camera input blocked`; `inputoff` not needed for the soldier to stand still while open (send `winkey 0x57` = W for 2 s; soldier position from `where` unchanged); `ui close` → W moves the soldier again; `winfocus` then post `WM_ACTIVATEAPP FALSE` (add `ui test-focuslost` hidden verb) → `ui closed`. Log has no `fault:` lines.
- [ ] **Step 6: Commit** `feat(ui): F1 menu state, game input blocking, cursor release`

### Task 5: Menu window, Camera and World tabs, status badge

**Files:** Create `src/ui/menu.h`, `src/ui/menu.cpp`, `src/ui/style.cpp`; Modify `src/main.cpp` (`OverlayFrame` ~line 10463: draw the menu when open, the badge always once the Present hook is in)

**Interfaces:**
- Consumes: `CommandQueue` (T1), `UiSettings`/`LoadSettings`/`SaveSettings` (T2), `Act*` (T3), `MenuState` (T4).
- Produces: `void Menu_Init(); void Menu_Frame(); void Menu_Shutdown();` (called from `OverlayFrame` on the present thread); `void Style_Apply(float screenHeight);`
- Layout: one window "gamelink" (default 720x520, centred first time, ImGui ini at `%LOCALAPPDATA%\gamelink\ui.ini`), tab bar with the seven tabs.
  - **Camera:** "Free camera" toggle (`ActFreeCam`), speed slider 1-500, FOV slider 30-120 + "Default" button, "Hide HUD" checkbox, 8 named spots (Save here / Go / name field), "Photo mode" (free cam on + HUD off + time scale 0) and "Screenshot" (to `%USERPROFILE%\Pictures\gamelink`, folder created) showing the saved path.
  - **World:** level + mode in words (from `levelname` reply, refreshed every 2 s while the tab is visible), time-scale slider 0-3 with Pause/Normal buttons, view-mode dropdown (values and names copied from the `viewmode` comments in `main.cpp`; if none are documented, list 0-9 as "Mode n").
  - Disabled-with-reason state when not in a level ("Not in a level yet"), detected from `worldinfo` (no client world).
  - **Badge:** bottom-right "gamelink · F1" (green) or "gamelink · unsupported game version" (amber) - the version check reuses the module size check gamelink already does on attach.
  - Every control shows "working…" while its command is in the queue and the reply as a tooltip after.
- [ ] **Step 1:** Implement (UI drawing - verified in game, not unit-tested).
- [ ] **Step 2: In-game check** (harness): `ui open`, `grabframe` → screenshot shows the window over the game; toggle free cam via `ui do camera.freecam 1` (add hidden `ui do <id> <value>` that calls the same handler as the widget) → `camera` position changes after `winkey` W; `ui do world.timescale 0.25` → `timescale` reply; front-end check: `ui open` before `hookspawn` fires → grabframe shows "Not in a level yet"; resize the game window (`winkey` Alt+Enter) while open → still drawn, no fault. Paste a real `camera` reply into T3's `ParseCamera` test.
- [ ] **Step 3: Commit** `feat(ui): menu window with Camera and World tabs, status badge`

### Task 6: Diagnostics and Advanced tabs  - end of Stage 1 (owner play-test)

**Files:** Create `src/ui/tab_diagnostics.cpp`, `src/ui/tab_advanced.cpp`; Modify `src/ui/menu.cpp`

**Interfaces:**
- Consumes: `ParseFrameTimes` (T3), `CommandQueue` (T1).
- **Diagnostics:** frame-time line graph (last 10 s, from `frametimes 1` polled each second while visible; installs the Present hook once), FPS / p99 / >33 ms count, server tick rate (counter at server context +0x30 → +0x0, sampled 1 s apart - add `ticks` TCP verb returning `ticks <n>`), gamelink status lines (hooks installed, version check), last 30 log lines (`log 30`), "Copy diagnostics" button (clipboard).
- **Advanced:** command line with history (up/down), reply panel, a "Commands" list from `help` (add a `help` verb that returns verb names + their one-line comment, generated at build time is out of scope: hand-written table in `main.cpp`, also used by `docs/COMMANDS.md` later).
- [ ] **Step 1:** Implement. **Step 2: In-game check:** Diagnostics shows FPS within ±10 % of `frametimes 10`; `ticks` sampled twice 1 s apart differs by 28-32 on a vanilla Felucia round; Advanced: `levelname` typed in returns the same reply as TCP.
- [ ] **Step 3: Commit** `feat(ui): diagnostics and advanced console tabs`
- [ ] **Step 4: Owner play-test** of Stage 1 (launch via harness play mode, owner uses F1 in a match); fold feedback into fixes before Task 7.

### Task 7: Bots tab with race-free 64v64  - Stage 2

**Files:** Modify `src/main.cpp` (new `capfix` and `bots` verbs next to `poolgrow`), create `src/ui/tab_bots.cpp`, modify `src/ui/menu.cpp`

**Interfaces:**
- Produces TCP verbs:
  - `capfix <n>` - patch both player-manager factory reads (`game+0x182FF3` and `game+0x183C2B`, bytes `44 8B 41 20 48 8B 51 40`) to load `n` (port `InstallCapPatch` from `D:\bf2_64v64\release\bf2-64v64\src\bf2_64v64.cpp`); reply `capfix 132 (2 of 2 sites)`; refuses with reason if bytes differ. Must run before a level's ServerPlayerManager is built; reply says `takes effect next match` if a level is up.
  - `bots set <team1> <team2>` - pins ForceFill (AutoPlayerSettings +0x2C/+0x30, apply +0x2C4) with the standalone DLL's settle-then-step top-up so both teams end at the target including humans; runs on gamelink's own thread every 500 ms while set; `bots off` stops pinning; `bots status` → `bots 64v64 players 64/64 alive 62/63 cap 132 pool overflow 63 refused 0`.
  - Spawn tune is part of `bots set` (MaxSpawnsPerUpdate ≥ 8, SpawnRate ×2), reverted by `bots off`.
- Tab: team sliders 1-64, Apply / Stop, live players/alive per team (refresh 1 s), cap and pool status lines; if the current ServerPlayerManager's cap < 2×team+2, an amber note "Cap applies from the next match - inject before starting a match (the launcher does this)".
- `poolgrow` is installed automatically by `bots set` if not on.
- [ ] **Step 1:** Implement verbs + tab. **Step 2: In-game check** (harness, gamelink injected early, `capfix 132` sent at attach via the harness args): Felucia Mode8, `ui do bots.set 64 64` → after 60 s `bots status` = players 64/64, alive ≥ 60/60, cap 132, refused 0; game still running 3 min later; `frametimes 10` p99 < 33 ms.
- [ ] **Step 3: Commit** `feat(ui): bots tab and race-free 64v64 (capfix, bots verbs)`

---

Stages 3-6 (Inspector, Logic Debugger, Launcher, Docs + release) each get their own plan after Stage 2's play-test.
