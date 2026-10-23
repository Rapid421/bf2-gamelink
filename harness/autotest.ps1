# autotest.ps1 - unattended "does this mod load?" test.
#
# 1. puts ONE .fbmod alone into a dedicated Frosty Mod Manager pack ("AutoTest"),
# 2. runs FrostyModManager -launch AutoTest (Frosty applies the pack into ModData and starts the game),
# 3. injects FrostyGameLink into every starwarsbattlefrontii process that appears (the game re-execs
#    itself, so the replacement is the one that matters - that injection needs an ELEVATED shell),
# 4. arms the spawn hook so the engine's own first spawn loads <Level>/<Mode> instead of the frontend,
# 5. watches: a world comes up -> LOADED; the process disappears -> CRASH; nothing within Timeout -> HANG,
# 6. writes the verdict to -Result (JSON) and closes the game and the mod manager.
#
# Runs from the scheduled task "FrostyAutoTest" (highest privileges, see register-autotest.ps1);
# arguments come from autotest-args.json next to this script when the task fires (that is how
# `frostycli mod test` drives it), or from the command line. Needs the FrostyGameLink module
# (F:\Frosty\gamelink\bin\FrostyGameLink.dll by default; override with -Module or "module" in the args file).
param(
    [string]$Mod = '',
    [string]$Level = 'S2/Levels/CloudCity_01/CloudCity_01',
    [string]$Mode = 'SkirmishBlast',
    [int]$Timeout = 180,
    [string]$Result = '',
    [string]$ArgsFile = '',
    [string]$Module = 'F:\Frosty\gamelink\bin\FrostyGameLink.dll',
    [string]$ModManager = 'F:\Frosty\FrostyModManager\FrostyModManager\FrostyModManager.exe',
    [string]$ModsDir = 'F:\Frosty\FrostyModManager\FrostyModManager\Mods\starwarsbattlefrontii',
    [string]$ConfigPath = "$env:LOCALAPPDATA\Frosty\manager_config.json",
    [string]$Pack = 'AutoTest',
    [switch]$LaunchOnly,
    [switch]$Menu,     # with launch-only: stay for $Hold s and execute menu.request input batches (elevated)
    [switch]$SkipFirst,
    [int]$Port = 31500,
    [switch]$Keep,     # leave the game running after LOADED so a human can look around
    [switch]$Deploy,   # with -Keep: press through the class-select screen into the world
    [int]$Settle = 60, # seconds the world must stay up before LOADED is declared
    [int]$Hold = 0     # >0: after LOADED (and the deploy input) watch the world this many seconds, then close the game
)

$ErrorActionPreference = 'Stop'
if (-not $ArgsFile) { $ArgsFile = Join-Path $PSScriptRoot 'autotest-args.json' }
if (-not $Result)   { $Result   = Join-Path $PSScriptRoot 'autotest-result.json' }
$Log = Join-Path $PSScriptRoot 'autotest.log'
# Hide our console: a stray click in it (quick-edit selection) freezes every console write, and
# the run then never gets past its first log line. The log file is the interface, not the window.
# Hiding the window is not enough on its own: if quick-edit selection was ever armed on this
# console, every subsequent console write blocks forever and the run dies after its first log
# line with the scheduled task stuck in Running. Turn quick-edit off explicitly, and do not
# write to the console at all -- the log file is the only interface this script needs.
try {
    Add-Type -Namespace AT -Name Con -MemberDefinition '[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow(); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c); [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int n); [DllImport("kernel32.dll")] public static extern bool GetConsoleMode(IntPtr h, out uint m); [DllImport("kernel32.dll")] public static extern bool SetConsoleMode(IntPtr h, uint m);'
    [void][AT.Con]::ShowWindow([AT.Con]::GetConsoleWindow(), 0)
    $hIn = [AT.Con]::GetStdHandle(-10)
    $m = 0
    if ([AT.Con]::GetConsoleMode($hIn, [ref]$m)) {
        [void][AT.Con]::SetConsoleMode($hIn, (($m -band -bnot 0x0040) -bor 0x0080))
    }
} catch { }
function Say($m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $m
    # autotest.log can be held open by a reader (a `tail -F` since 10:13 on 2026-09-15 swallowed every line after it);
    # fall back to a second file rather than losing the run's only record.
    try { Add-Content -Path $Log -Value $line -Encoding utf8 } catch { try { Add-Content -Path ($Log -replace '\.log$', '-2.log') -Value $line -Encoding utf8 } catch { } }
}

if (-not $Mod -and (Test-Path $ArgsFile)) {
    $a = Get-Content $ArgsFile -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($a.mod)     { $Mod = $a.mod }
    if ($a.level)   { $Level = $a.level }
    if ($a.mode)    { $Mode = $a.mode }
    if ($a.timeout) { $Timeout = [int]$a.timeout }
    if ($a.module)  { $Module = $a.module }
    if ($a.modmanager) { $ModManager = $a.modmanager }
    if ($a.modsdir) { $ModsDir = $a.modsdir }
    if ($a.pack)    { $Pack = $a.pack }
    if ($a.launchonly) { $LaunchOnly = $true }
    if ($a.menu)    { $Menu = $true; $LaunchOnly = $true }
    if ($a.skipfirst) { $SkipFirst = $true }
    if ($a.keep)    { $Keep = [bool]$a.keep }
    if ($a.deploy)  { $Deploy = [bool]$a.deploy }
    if ($a.settle)  { $Settle = [int]$a.settle }
    if ($a.hold)    { $Hold = [int]$a.hold }
    # gameinput: deploy through gamelink's in-process window messages (winfocus/winkey/winclick) and never front the
    # game window - no SendInput, so the user's own mouse and keyboard are left alone (2026-09-27)
    if ($a.gameinput) { $GameInput = [bool]$a.gameinput }
    if ($a.feeds) { $Feeds = [bool]$a.feeds; $FeedsEarly = ("$($a.feeds)" -eq "early") }
}

$verdict = @{ mod = $Mod; level = $Level; mode = $Mode; status = 'ERROR'; detail = ''; started = (Get-Date).ToString('s') }
function Finish($status, $detail) {
    $verdict.status = $status; $verdict.detail = $detail; $verdict.finished = (Get-Date).ToString('s')
    ($verdict | ConvertTo-Json) | Set-Content -Path $Result -Encoding UTF8
    Say "RESULT: $status - $detail"
}
function Stop-Game {
    Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | ForEach-Object { try { Stop-Process -Id $_.Id -Force } catch { } }
    Get-Process FrostyModManager -ErrorAction SilentlyContinue | ForEach-Object { try { Stop-Process -Id $_.Id -Force } catch { } }
    # a killed mod manager keeps its handle on the pack's .fbmod for a moment: the copy right after failed with "being
    # used by another process" (RG1T, 2026-09-27 01:15). Wait (<= 30 s) until both are really gone.
    $until = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $until -and @(Get-Process starwarsbattlefrontii, FrostyModManager -ErrorAction SilentlyContinue).Count -gt 0) {
        Start-Sleep -Milliseconds 500
    }
}

trap {
    Say ("UNHANDLED: " + $_.Exception.Message + " at " + $_.InvocationInfo.PositionMessage)
    Finish 'ERROR' $_.Exception.Message
    Stop-Game
    exit 1
}

# ---- injection helpers (from launch-hooked.ps1) --------------------------------------------------
Add-Type -Namespace AT -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool i, int p);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr a, IntPtr s, uint t, uint p);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr s, out IntPtr w);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr GetModuleHandleW(string n);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr m, string n);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr a, IntPtr s, IntPtr f, IntPtr p, uint fl, out uint id);
[DllImport("kernel32.dll", SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
'@
function Inject([int]$processId, [string]$dllPath) {
    $handle = [AT.Native]::OpenProcess(0x043A, $false, $processId)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess failed (win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())) - run elevated" }
    $bytes = [Text.Encoding]::Unicode.GetBytes($dllPath + "`0")
    $remote = [AT.Native]::VirtualAllocEx($handle, [IntPtr]::Zero, [IntPtr]$bytes.Length, 0x3000, 4)
    if ($remote -eq [IntPtr]::Zero) { throw "VirtualAllocEx failed" }
    [void][AT.Native]::WriteProcessMemory($handle, $remote, $bytes, [IntPtr]$bytes.Length, [ref]([IntPtr]::Zero))
    $loadLibrary = [AT.Native]::GetProcAddress([AT.Native]::GetModuleHandleW('kernel32.dll'), 'LoadLibraryW')
    $thread = [AT.Native]::CreateRemoteThread($handle, [IntPtr]::Zero, [IntPtr]::Zero, $loadLibrary, $remote, 0, [ref]([uint32]0))
    if ($thread -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed" }
    [void][AT.Native]::WaitForSingleObject($thread, 20000)
    [void][AT.Native]::CloseHandle($thread)
    [void][AT.Native]::CloseHandle($handle)
}
function Send-GameLink([string]$command, [int]$timeoutMs = 10000) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        if (-not $client.ConnectAsync('127.0.0.1', $Port).Wait(2000)) { return $null }
        $client.ReceiveTimeout = $timeoutMs
        $stream = $client.GetStream()
        $payload = [Text.Encoding]::ASCII.GetBytes($command + "`n")
        $stream.Write($payload, 0, $payload.Length)
        $out = New-Object Text.StringBuilder
        $buffer = New-Object byte[] 65536
        while ($true) {
            try { $read = $stream.Read($buffer, 0, $buffer.Length) } catch { break }
            if ($read -le 0) { break }
            [void]$out.Append([Text.Encoding]::ASCII.GetString($buffer, 0, $read))
            if (-not $stream.DataAvailable) {
                Start-Sleep -Milliseconds 200
                if (-not $stream.DataAvailable) { break }
            }
        }
        return $out.ToString().TrimEnd()
    } catch { return $null } finally { $client.Close() }
}

# ---- attach-coords (autotest-args.json "attachcoords": true) --------------------------------------
# The user plays the game launched THEIR way; this injects FrostyGameLink into the running game and turns on the coords
# overlay (camera position + heading drawn by the game's own debug text, camera untouched) once a level is up, so they
# can read off where something is. No pack, no launch, no input, no verdict hold, never closes the game. 2026-10-04.
if ($a -and $a.attachcoords) {
    $gp = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $gp) { Finish 'ERROR' 'attachcoords: the game is not running'; exit 1 }
    if (-not (Send-GameLink 'ping' 3000)) {
        try { Inject $gp.Id $Module; Say ("attachcoords: injected into pid {0}" -f $gp.Id) }
        catch { Finish 'ERROR' ("attachcoords: inject failed: {0}" -f $_.Exception.Message); exit 1 }
        Start-Sleep -Seconds 3
    } else { Say "attachcoords: gamelink already in the game" }
    # a level may not be up yet (menu, loading): retry for up to 15 min
    $until = (Get-Date).AddMinutes(15)
    while ((Get-Date) -lt $until) {
        if (@(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue).Count -eq 0) { Finish 'ERROR' 'attachcoords: the game closed'; exit 0 }
        $r = Send-GameLink 'coords 1' 8000
        if ($r -and $r -match '^coords on') { Finish 'COORDS' $r; exit 0 }
        Say ("attachcoords: not yet ({0})" -f $r)
        Start-Sleep -Seconds 10
    }
    Finish 'ERROR' 'attachcoords: no level within 15 min'
    exit 0
}

# ---- 0. preconditions ----------------------------------------------------------------------------
# No mod, or "vanilla": the AutoTest pack is emptied and Mod Manager launches the plain game,
# still with gamelink injected and the level hook armed (2026-10-07).
$Vanilla = (-not $Mod) -or ($Mod -eq 'vanilla')
if ($Vanilla) { $Mod = 'vanilla' }
elseif (@($Mod -split ';' | Where-Object { $_ -and -not (Test-Path $_) }).Count) { Finish 'ERROR' "mod not found: $Mod"; exit 1 }   # "a.fbmod;b.fbmod": each one
if (-not (Test-Path $Module))  { Finish 'ERROR' "module missing: $Module"; exit 1 }
if (-not (Test-Path $ModManager)) { Finish 'ERROR' "mod manager missing: $ModManager"; exit 1 }
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Say "autotest: mod=$Mod level=$Level mode=$Mode timeout=$Timeout elevated=$elevated"
if (-not $elevated) { Say "WARNING: not elevated - injecting into the re-exec'd game process will fail" }
# deployonly (autotest-args.json "deployonly": true): the game is already running on the class-select screen and the
# earlier deploy presses missed; re-send the deploy input from this elevated script until the local player controls
# a soldier, then exit WITHOUT touching the game. Added 2026-09-17 (Mustafar M20 lightmap check).
if ($a -and $a.deployonly) {
    $gp = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $gp) { Finish 'ERROR' 'deployonly: no game window'; exit 1 }
    $b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
    $sx = [int](65535 * 0.079); $sy = [int](65535 * 0.934)
    $ok = $false
    for ($attempt = 0; $attempt -lt 8 -and -not $ok; $attempt++) {
        [void][AT2]::ShowWindowW($gp.MainWindowHandle, 9); [void][AT2]::SetForegroundWindowW($gp.MainWindowHandle); [AT2]::SwitchToThisWindow($gp.MainWindowHandle, $true)
        Start-Sleep -Seconds 2
        [AT2]::Key(0x1C); Start-Sleep -Milliseconds 900
        [AT2]::Key(0x39); Start-Sleep -Milliseconds 900
        [AT2]::Click($sx, $sy); Start-Sleep -Seconds 2
        [AT2]::Click($sx, $sy); Start-Sleep -Seconds 2
        [AT2]::Key(0x1C); Start-Sleep -Seconds 6
        $pl = Send-GameLink 'players' 8000
        $ok = ($pl -match "'RAPID421'[^
]*
\s*controlled 0x[0-9A-F]*[1-9A-F]")
        Say ("deployonly: attempt {0} -> {1}" -f ($attempt + 1), $(if ($ok) { 'deployed' } else { 'not yet' }))
    }
    if ($ok) { Finish 'DEPLOYED' 'deployonly: local player controls a soldier' } else { Finish 'NODEPLOY' 'deployonly: gave up after 8 attempts' }
    exit 0
}
Stop-Game
Start-Sleep -Seconds 2

# ---- 1. the pack: exactly this one mod ----------------------------------------------------------
$GameModData = 'F:\STAR WARS Battlefront II\ModData'
if ($Vanilla) { $modName = '' }
else {
    # several mods: "a.fbmod;b.fbmod" in load order (the later ones win a shared asset)
    $modNames = @()
    foreach ($one in ($Mod -split ';' | Where-Object { $_ })) {
        $leaf = Split-Path $one -Leaf
        Copy-Item -Path $one -Destination (Join-Path $ModsDir $leaf) -Force
        $modNames += $leaf
    }
    $modName = ($modNames -join '|')
}
$cfgText = Get-Content $ConfigPath -Raw -Encoding UTF8
$cfg = $cfgText | ConvertFrom-Json
$game = $cfg.Games.starwarsbattlefrontii
if (-not $game.Packs) { $game | Add-Member -NotePropertyName 'Packs' -NotePropertyValue ([pscustomobject]@{}) -Force }
if (-not $game.Packs.PSObject.Properties[$Pack]) {
    $game.Packs | Add-Member -NotePropertyName $Pack -NotePropertyValue "" -Force
}
$game.Packs.$Pack = $(if ($Vanilla) { "" } else { (($modName -split '\|') | ForEach-Object { "$($_):True" }) -join '|' })
$game.Options.SelectedPack = $Pack
Copy-Item $ConfigPath "$ConfigPath.autotest-bak" -Force
($cfg | ConvertTo-Json -Depth 20) | Set-Content -Path $ConfigPath -Encoding UTF8
Say $(if ($Vanilla) { "pack '$Pack' = (empty - vanilla)" } else { "pack '$Pack' = $modName" })

# FrostyModExecutor decides whether to rebuild ModData by comparing patch\mods.json entry by
# entry on Name/Version/Category/FileName only -- never the mod's contents or timestamp. Rebuilding
# the same .fbmod under the same mod name is therefore a silent no-op: the game keeps running the
# previous build while this log says the new mod was applied. Deleting just mods.json sets
# needsModding and leaves the .cas symlinks in place; deleting the whole patch tree instead makes
# Frosty put up a modal "New patch detected" box that nobody may be at the machine to click.
$packMods = Join-Path $GameModData "$Pack\patch\mods.json"
if (Test-Path $packMods) { Remove-Item -Force $packMods; Say "removed $packMods so Frosty re-applies the pack" }


# Frosty puts up informational message boxes (symlink regeneration, missing ModData) whose result it
# ignores. Unattended they wedge the run, and a non-elevated shell cannot click an elevated window --
# this script IS the elevated context, so it can. WM_CLOSE dismisses a one-button box exactly like OK.
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class AT2 {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint="ShowWindow")] public static extern bool ShowWindowW(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT {
        [FieldOffset(0)]  public uint type;
        [FieldOffset(8)]  public MOUSEINPUT mi;
        [FieldOffset(8)]  public KEYBDINPUT ki;
    }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] a, int cb);
    public static void Key(ushort scan) {
        INPUT[] a = new INPUT[2];
        a[0].type = 1; a[0].ki.wScan = scan; a[0].ki.dwFlags = 0x0008;              // KEYEVENTF_SCANCODE
        a[1].type = 1; a[1].ki.wScan = scan; a[1].ki.dwFlags = 0x0008 | 0x0002;     // + KEYUP
        SendInput(2, a, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Click(int ax, int ay) {
        INPUT[] a = new INPUT[3];
        a[0].type = 0; a[0].mi.dx = ax; a[0].mi.dy = ay; a[0].mi.dwFlags = 0x8000 | 0x0001;  // ABSOLUTE|MOVE
        a[1].type = 0; a[1].mi.dx = ax; a[1].mi.dy = ay; a[1].mi.dwFlags = 0x8000 | 0x0002;  // LEFTDOWN
        a[2].type = 0; a[2].mi.dx = ax; a[2].mi.dy = ay; a[2].mi.dwFlags = 0x8000 | 0x0004;  // LEFTUP
        SendInput(3, a, Marshal.SizeOf(typeof(INPUT)));
    }
    [DllImport("user32.dll", EntryPoint="SetForegroundWindow")] public static extern bool SetForegroundWindowW(IntPtr h);
}
"@
# Only windows of the Mod Manager this run started ($fmm): the titles alone also matched the user's own
# Frosty Editor window, and 2026-10-08 it was gone after a run (2,441 closes logged across the day's runs).
function Dismiss-FrostyDialogs {
    $titles = @('Frosty Toolsuite', 'Frosty Editor')
    if ($null -eq $fmm) { return }
    $ownPid = [uint32]$fmm.Id
    $cb = [AT2+EnumProc]{
        param($h, $p)
        $owner = [uint32]0
        [void][AT2]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $ownPid -and [AT2]::IsWindowVisible($h)) {
            $sb = New-Object Text.StringBuilder 512
            [void][AT2]::GetWindowTextW($h, $sb, $sb.Capacity)
            $t = $sb.ToString()
            if ($titles -contains $t) {
                Say "dismissing Frosty dialog '$t'"
                [void][AT2]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
            }
        }
        return $true
    }
    [void][AT2]::EnumWindows($cb, [IntPtr]::Zero)
}

# ---- 2. apply + launch through Frosty ------------------------------------------------------------
$before = @(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
Say "starting Frosty Mod Manager -launch $Pack"
$fmm = Start-Process -FilePath $ModManager -ArgumentList @('-launch', $Pack) -WorkingDirectory (Split-Path $ModManager) -PassThru
if ($LaunchOnly) {
    # launch-only: no game-link injection, no verdict; the game comes up on its own (main menu) and stays running
    $deadline = (Get-Date).AddSeconds(300)
    while ((Get-Date) -lt $deadline) {
        # the mod manager's "Frosty Editor" notice blocks the launch until closed (the injected path closes it too);
        # left open, the manager sat idle and no game started (HOTH_RG1, 2026-09-26 23:45)
        Dismiss-FrostyDialogs
        $procs = @(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id })
        if ($procs.Count -gt 0) {
            Say ("launch-only: game started (pid {0}), leaving it alone" -f $procs[0].Id); Finish 'LAUNCHED' 'launch-only mode: no injection, no verdict'
            if (-not $Menu) { exit 0 }
            # menu mode: drive the frontend with real input for $Hold s. A client writes a JSON array of steps to
            # menu.request, e.g. [{"op":"click","x":0.08,"y":0.93},{"op":"key","scan":28},{"op":"wait","s":2}]
            # (x/y are fractions of the primary screen; scan is a keyboard scan code), then waits for menu.done.
            $menuFile = Join-Path $PSScriptRoot 'menu.request'; $doneFile = Join-Path $PSScriptRoot 'menu.done'
            if (Test-Path $menuFile) { Remove-Item $menuFile -Force }
            $menuEnd = (Get-Date).AddSeconds([Math]::Max($Hold, 60)); $n = 0; $goneSince = $null
            Say ("menu: driving input until {0}" -f $menuEnd.ToString('HH:mm:ss'))
            while ((Get-Date) -lt $menuEnd) {
                Start-Sleep -Milliseconds 500
                # the game re-execs itself ~3 s after start, so a short gap with no process is normal
                if (@(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue).Count -eq 0) {
                    if (-not $goneSince) { $goneSince = Get-Date }
                    elseif (((Get-Date) - $goneSince).TotalSeconds -gt 60) { Say 'menu: game exited'; break }
                    continue
                } else { $goneSince = $null }
                if (-not (Test-Path $menuFile)) { continue }
                try {
                    $steps = Get-Content $menuFile -Raw -Encoding UTF8 | ConvertFrom-Json
                    Remove-Item $menuFile -Force
                    $gpm = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                    if ($gpm) { [void][AT2]::ShowWindowW($gpm.MainWindowHandle, 9); [void][AT2]::SetForegroundWindowW($gpm.MainWindowHandle); [AT2]::SwitchToThisWindow($gpm.MainWindowHandle, $true); Start-Sleep -Milliseconds 400 }
                    foreach ($st in @($steps)) {
                        switch ($st.op) {
                            'click' { [AT2]::Click([int](65535 * [double]$st.x), [int](65535 * [double]$st.y)); Start-Sleep -Milliseconds 300 }
                            'key'   { [AT2]::Key([uint16]$st.scan); Start-Sleep -Milliseconds 300 }
                            'wait'  { Start-Sleep -Milliseconds ([int](1000 * [double]$st.s)) }
                        }
                    }
                    $n++; Set-Content -Path $doneFile -Value ("{0} {1}" -f $n, (Get-Date).ToString('HH:mm:ss.fff'))
                    Say ("menu: batch {0} done ({1} step(s))" -f $n, @($steps).Count)
                } catch { Say ("menu: bad request: {0}" -f $_.Exception.Message) }
            }
            Stop-Game
            exit 0
        }
        if ($fmm.HasExited) { Start-Sleep -Seconds 5; if (@(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue).Count -eq 0) { Finish 'ERROR' 'mod manager exited without starting the game'; exit 1 } }
        Start-Sleep -Milliseconds 200
    }
    Finish 'ERROR' 'launch-only: no game process within 300 s'; exit 1
}

# ---- 3. inject into every game process that appears ----------------------------------------------
# The game re-execs itself ~3 s after start: the first process exits and a replacement appears.
# Every new pid gets the module, and the hook is armed per pid (the module state lives in the
# process that owns it). A pid that disappears is only a crash if no replacement shows up.
$injected = @{}      # pid -> armed (bool)
$deadline = (Get-Date).AddSeconds(900)   # a forced full re-apply of the pack takes ~5 min
$sawGame = $false
$lastSeen = $null
$armedPid = 0
$armedAt = $null
$skippedFirst = $false
function Try-Arm {
    $pong = Send-GameLink 'ping' 2000
    if (-not $pong) { return $false }
    $cmd = "hookspawn $Level $Mode"
    $r = Send-GameLink $cmd 10000
    Say "-> $cmd : $r"
    # feeds "early": ask for both feeds right after injection. gamelink defers them until a level is
    # loading (client level set) - never at the frontend - so the ring buffers hold the level's own
    # start-up logic (game-mode streaming) for a 'game record' started after LOADED.
    if ($FeedsEarly -and ($r -match 'armed')) {
        Say ("-> trace on (early) : " + (Send-GameLink 'trace on' 10000))
        Say ("-> prop on (early) : " + (Send-GameLink 'prop on' 10000))
    }
    return ($r -match 'armed')
}
while ((Get-Date) -lt $deadline) {
    Dismiss-FrostyDialogs

    $procs = @(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id })
    foreach ($p in $procs) {
        if ($injected.ContainsKey($p.Id)) { continue }
        $sawGame = $true
        if ($SkipFirst -and -not $skippedFirst) { $skippedFirst = $true; $injected[$p.Id] = $true; Say ("skipfirst: not injecting into the first game process (pid {0}); waiting for the re-exec" -f $p.Id); continue }
        # opt-in: F:\Frosty\gamelink\premodules.txt lists DLLs (one per line) loaded into the game BEFORE gamelink, e.g.
        # RenderDoc's renderdoc.dll so it hooks D3D11 before the device exists (JundlandWastes T3 frame capture, 2026-10-17)
        $preFile = Join-Path $PSScriptRoot 'premodules.txt'
        if (Test-Path $preFile) {
            foreach ($pm in @(Get-Content $preFile | Where-Object { $_.Trim() -ne '' -and -not $_.Trim().StartsWith('#') })) {
                $pm = $pm.Trim()
                if ($pm.StartsWith('cmd:')) {
                    # cmd:<exe>|<args with {pid}> -- an external injector (e.g. renderdoccmd inject --PID={pid} --capture-file D:\...),
                    # run hidden, at most 60 s, before gamelink goes in
                    $parts = $pm.Substring(4).Split('|', 2)
                    $exeArgs = $parts[1].Replace('{pid}', [string]$p.Id)
                    try {
                        $ip = Start-Process -FilePath $parts[0] -ArgumentList $exeArgs -WindowStyle Hidden -PassThru
                        if (-not $ip.WaitForExit(60000)) { try { $ip.Kill() } catch {}; Say ("pre-inject cmd timed out (60 s): {0}" -f $parts[0]) }
                        else { Say ("pre-inject cmd exit {0}: {1} {2}" -f $ip.ExitCode, $parts[0], $exeArgs) }
                    } catch { Say ("pre-inject cmd failed: {0}" -f $_.Exception.Message) }
                    continue
                }
                try { Inject $p.Id $pm; Say ("pre-injected {0} into pid {1}" -f $pm, $p.Id) }
                catch { Say ("pre-inject {0} into pid {1} failed: {2}" -f $pm, $p.Id, $_.Exception.Message) }
            }
        }
        try { Inject $p.Id $Module; $injected[$p.Id] = $false; Say ("injected into pid {0}" -f $p.Id) }
        catch { Say ("inject pid {0} failed: {1}" -f $p.Id, $_.Exception.Message); $injected[$p.Id] = $false }
    }
    if ($procs.Count -gt 0) { $lastSeen = Get-Date }
    # arm the hook in the newest live pid that has not been armed yet
    $live = @($procs | Where-Object { -not $injected[$_.Id] })
    if ($live.Count -gt 0) {
        $target = $null
        try { $target = $live | Sort-Object { try { $_.StartTime } catch { [DateTime]::MinValue } } | Select-Object -Last 1 } catch { $target = $live | Select-Object -Last 1 }
        if (-not $target) { Start-Sleep -Milliseconds 100; continue }
        if (Try-Arm) { $injected[$target.Id] = $true; $armedPid = $target.Id; $armedAt = Get-Date; Say ("hook armed in pid {0}" -f $armedPid) }
    }
    # done with this phase once the armed pid has stayed alive for 20 s (a re-exec happens within ~3 s)
    if ($armedPid -and ($procs | Where-Object { $_.Id -eq $armedPid }) -and ((Get-Date) - $armedAt).TotalSeconds -ge 20) { break }
    # EA Desktop's relaunch has taken 25 s (2026-09-15 12:35, 286 MB pack): a 20 s window declared CRASH, then the
    # replacement came up uninjected and stayed running with nobody to stop it.
    if ($sawGame -and $procs.Count -eq 0 -and $lastSeen -and ((Get-Date) - $lastSeen).TotalSeconds -gt 90) {
        Finish 'CRASH' 'game exited before a world came up (no replacement process within 90 s)'; Stop-Game; exit 0
    }
    if (-not $sawGame -and $fmm.HasExited) {
        Start-Sleep -Seconds 5
        if (@(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue).Count -eq 0) { Finish 'ERROR' 'mod manager exited without starting the game'; exit 1 }
    }
    Start-Sleep -Milliseconds 100
}
if (-not $armedPid) { Finish 'ERROR' 'the spawn hook never armed (injection failed or the port never answered)'; Stop-Game; exit 1 }

# ---- 4. watch ------------------------------------------------------------------------------------
$watchEnd = (Get-Date).AddSeconds($Timeout)
$last = ''
while ((Get-Date) -lt $watchEnd) {
    if (@(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue).Count -eq 0) {
        Start-Sleep -Seconds 10
        if (@(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue).Count -eq 0) {
            Finish 'CRASH' ("game process exited; last worldinfo: $last"); Stop-Game; exit 0
        }
    }
    $info = Send-GameLink 'worldinfo' 5000
    if ($info) { $last = ($info -replace "`r?`n", ' | ') }
    if ($info -and $info -match 'clientLevel=0x[0-9A-Fa-f]+' -and $info -notmatch 'clientLevel=0x0\b') {
        # Let the level settle: a crash within -Settle seconds of the world coming up (default 60 s;
        # the player spawn and the first gameplay tick land in that window) is reported as CRASH_LATE.
        $settleEnd = (Get-Date).AddSeconds($Settle); $upAt = Get-Date
        while ((Get-Date) -lt $settleEnd) {
            Start-Sleep -Seconds 2
            if (@(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue).Count -eq 0) {
                # Was it a real fault or did the game just end/quit? Look at the module log since the world came up.
                $fault = ''
                try {
                    $lines = Get-Content 'C:\ProgramData\FrostyGameLink\gamelink.log' -Tail 400 -ErrorAction SilentlyContinue
                    $since = $upAt.ToString('HH:mm:ss')
                    $hit = $lines | Where-Object { $_ -match 'fault: code 0xC0000005' -and $_ -ge "[$since" } | Select-Object -Last 1
                    if ($hit) { $fault = ($hit -replace '^\[[^\]]*\] ', '') }
                } catch { }
                $secs = [int]((Get-Date) - $upAt).TotalSeconds
                if ($fault) { Finish 'CRASH_LATE' ("game faulted $secs s after the world came up: $fault") }
                else { Finish 'EXIT_LATE' ("game exited $secs s after the world came up with no access violation logged (round ended / self-quit?)") }
                Stop-Game; exit 0
            }
        }
        $name = Send-GameLink 'levelname' 10000
        Finish 'LOADED' ("levelname: $name | $last")
        # feeds (args "feeds": true): logic + property feeds on, so a later crash writes the last
        # events and property writes into the gamelink log ('feed:' lines). Only ever AFTER the level
        # is up: patching at injection/frontend stalled or killed the game in all four tries (2026-10-08).
        if ($Feeds) {
            Say ("-> trace on : " + (Send-GameLink 'trace on' 10000))
            Say ("-> prop on : " + (Send-GameLink 'prop on' 10000))
        }
        if ($Keep) {
            Say "keep: leaving the game running"
            Get-Process FrostyModManager -ErrorAction SilentlyContinue | ForEach-Object { try { Stop-Process -Id $_.Id -Force } catch { } }
            # The game window belongs to an elevated process, so nothing outside this script can bring
            # it forward -- and a screenshot of a backgrounded game is a screenshot of the desktop.
            try {
                $gp = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                if ($GameInput) { Say "keep: gameinput - not fronting the game window" }
                elseif ($gp) {
                    [void][AT2]::ShowWindowW($gp.MainWindowHandle, 9)      # SW_RESTORE
                    [void][AT2]::SetForegroundWindowW($gp.MainWindowHandle)
                    [AT2]::SwitchToThisWindow($gp.MainWindowHandle, $true)   # SetForegroundWindow is refused unless the caller owns the foreground
                    Say ("keep: fronted the game window (pid {0})" -f $gp.Id)
                } else { Say "keep: no game window to front yet" }
            } catch { Say ("keep: could not front the game window: {0}" -f $_.Exception.Message) }
            # The hooked spawn lands on the class-select screen, so an unattended screenshot shows the
            # deploy UI rather than the level. UIPI stops anything outside this elevated script from
            # sending the game a keystroke, so press deploy here.
            # The hooked spawn lands on the class-select screen, so an unattended screenshot shows the
            # deploy UI rather than the level. UIPI stops anything outside this elevated script from
            # reaching the game, and SendKeys does not reach a raw-input game at all -- SendInput with
            # hardware scancodes and an absolute mouse click does.
            if ($Deploy -and $GameInput) {
                # The same deploy sequence, delivered by gamelink inside the game process as window messages to the game's
                # own window: nothing reaches the user's desktop, the cursor never moves, focus never changes.
                try {
                    Start-Sleep -Seconds 4
                    for ($attempt = 0; $attempt -lt 12; $attempt++) {
                        if ($attempt -gt 0) { Start-Sleep -Seconds 12 }
                        $r = @()
                        $r += Send-GameLink 'winfocus' 4000
                        $r += Send-GameLink 'winkey 0x0D' 4000; Start-Sleep -Milliseconds 900
                        $r += Send-GameLink 'winkey 0x20' 4000; Start-Sleep -Milliseconds 900
                        $r += Send-GameLink 'winclick 0.079 0.934' 4000; Start-Sleep -Seconds 2
                        $r += Send-GameLink 'winclick 0.079 0.934' 4000; Start-Sleep -Seconds 2
                        $r += Send-GameLink 'winkey 0x0D' 4000
                        Say ("keep: gameinput deploy attempt {0}: {1}" -f ($attempt + 1), (($r | Select-Object -First 1) -replace "`r?`n", ' '))
                        Start-Sleep -Seconds 4
                        $pl = Send-GameLink 'players' 8000
                        if ($pl -and ($pl -match "'RAPID421'[^
]*
\s*controlled 0x[0-9A-F]*[1-9A-F]")) { Say "keep: local player deployed (gameinput)"; break }
                    }
                } catch { Say ("keep: gameinput deploy failed: {0}" -f $_.Exception.Message) }
            }
            elseif ($Deploy) {
                try {
                    Start-Sleep -Seconds 4
                    $b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
                    # START sits at the bottom-left of the class-select screen, ~8% across and ~93% down
                    $sx = [int](65535 * (0.079 * $b.Width) / $b.Width)
                    $sy = [int](65535 * (0.934 * $b.Height) / $b.Height)
                    # The class-select screen does not always accept the first presses (it can still be
                    # animating in), so send the sequence three times; once deployed, the extra input only
                    # makes the soldier jump or fire.
                    # 2026-09-17: three blind attempts missed whenever the class-select screen came up late (Mustafar M20nm:
                    # all three fired before START existed). Now up to 12 attempts, stopping once gamelink `players`
                    # shows the local player controlling a soldier.
                    for ($attempt = 0; $attempt -lt 12; $attempt++) {
                        if ($attempt -gt 0) {
                            Start-Sleep -Seconds 12
                            $gp2 = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                            if ($gp2) { [void][AT2]::SetForegroundWindowW($gp2.MainWindowHandle); [AT2]::SwitchToThisWindow($gp2.MainWindowHandle, $true) }
                        }
                        [AT2]::Key(0x1C); Start-Sleep -Milliseconds 900       # Enter
                        [AT2]::Key(0x39); Start-Sleep -Milliseconds 900       # Space
                        [AT2]::Click($sx, $sy); Start-Sleep -Seconds 2
                        [AT2]::Click($sx, $sy); Start-Sleep -Seconds 2
                        [AT2]::Key(0x1C)
                        Say ("keep: sent deploy input (enter, space, click START), attempt {0}" -f ($attempt + 1))
                        Start-Sleep -Seconds 4
                        $pl = Send-GameLink 'players' 8000
                        if ($pl -and ($pl -match "'RAPID421'[^
]*
\s*controlled 0x[0-9A-F]*[1-9A-F]")) { Say "keep: local player deployed"; break }
                    }
                } catch { Say ("keep: deploy input failed: {0}" -f $_.Exception.Message) }
            }
            if ($Hold -gt 0) {
                # hold: keep watching the running world for $Hold s after it came up, then close the game.
                # A client that wants a screenshot later writes new text into front.request; the game window
                # is re-fronted from here because only this elevated script can do it (UIPI).
                $holdEnd = $upAt.AddSeconds($Hold)
                $frontFile = Join-Path $PSScriptRoot 'front.request'
                $frontSeen = ''
                if (Test-Path $frontFile) { $frontSeen = (Get-Content $frontFile -Raw -ErrorAction SilentlyContinue) }
                Say ("hold: watching the world until {0}" -f $holdEnd.ToString('HH:mm:ss'))
                $holdStatus = 'HELD'; $holdDetail = "alive $Hold s after the world came up"
                $releaseFile = Join-Path $PSScriptRoot 'hold.release'
                while ((Get-Date) -lt $holdEnd) {
                    Start-Sleep -Seconds 2
                    # A client that has its shots writes hold.release: end the hold now instead of idling to $holdEnd,
                    # so the next queued test starts (2026-09-26, user: close it once you have what you need).
                    if ((Test-Path $releaseFile) -and ((Get-Item $releaseFile).LastWriteTime -gt $upAt)) {
                        Remove-Item $releaseFile -ErrorAction SilentlyContinue
                        $holdStatus = 'RELEASED'; $holdDetail = ("client done; released {0} s after the world came up" -f [int]((Get-Date) - $upAt).TotalSeconds)
                        break
                    }
                    if (@(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue).Count -eq 0) {
                        Start-Sleep -Seconds 3
                        $fault = ''
                        try {
                            $lines = Get-Content 'C:\ProgramData\FrostyGameLink\gamelink.log' -Tail 600 -ErrorAction SilentlyContinue
                            $since = $upAt.ToString('HH:mm:ss')
                            $hit = $lines | Where-Object { $_ -match 'fault: code 0xC0000005' -and $_ -ge "[$since" } | Select-Object -Last 1
                            if ($hit) { $fault = ($hit -replace '^\[[^\]]*\] ', '') }
                        } catch { }
                        $secs = [int]((Get-Date) - $upAt).TotalSeconds
                        $holdStatus = if ($fault) { 'HOLD_CRASH' } else { 'HOLD_EXIT' }
                        $holdDetail = "game gone $secs s after the world came up: $fault"
                        break
                    }
                    try {
                        $req = (Get-Content $frontFile -Raw -ErrorAction SilentlyContinue)
                        if ($req -and $req -ne $frontSeen -and -not $GameInput) {
                            $frontSeen = $req
                            Get-Process EADesktop -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | ForEach-Object { [void][AT2]::ShowWindowW($_.MainWindowHandle, 6) }
                            $gp3 = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                            if ($gp3) {
                                [void][AT2]::ShowWindowW($gp3.MainWindowHandle, 9)
                                [void][AT2]::SetForegroundWindowW($gp3.MainWindowHandle)
                                [AT2]::SwitchToThisWindow($gp3.MainWindowHandle, $true)
                            }
                        }
                    } catch { }
                }
                $verdict.hold = $holdStatus; $verdict.hold_detail = $holdDetail; $verdict.hold_finished = (Get-Date).ToString('s')
                ($verdict | ConvertTo-Json) | Set-Content -Path $Result -Encoding UTF8
                Say "HOLD: $holdStatus - $holdDetail"
                Stop-Game
            }
            exit 0
        }
        Stop-Game; exit 0
    }
    Start-Sleep -Seconds 2
}
Finish 'HANG' ("no world within $Timeout s; last worldinfo: $last | hookstatus: " + (Send-GameLink 'hookstatus' 5000))
Stop-Game
exit 0
