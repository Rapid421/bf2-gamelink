# One command: boot straight into a level, then set it up for viewing.
#
#   powershell -ExecutionPolicy Bypass -File F:\Frosty\gamelink\viewer.ps1
#   powershell -ExecutionPolicy Bypass -File F:\Frosty\gamelink\viewer.ps1 -Mode SkirmishOnslaught
#
# Needs an elevated shell: the game re-execs itself once and the replacement
# process has to be injected, which means opening a process we did not create.
#
# After the level is up it suppresses the local player's input and takes the
# camera by writing Camera::m_transform - the member the engine's own per-frame
# feed reads. That is the supported route: getTransformWithBase is an inline
# non-virtual member read, so there is nothing to hook and nothing to race.
#
# The earlier render-view writer is retired. It sat downstream of the engine's
# own writer, which is what produced the flickering, mirrored view.

param(
    [string]$Level = 'Levels/MP/Naboo_01/Naboo_01',
    [string]$Mode = 'SkirmishBlast',
    [switch]$KeepInput,
    [switch]$NoWait
)

$ErrorActionPreference = 'Stop'
$gamelink = 'F:\Frosty\gamelink'

$ScriptLog = 'F:\Frosty\gamelink\viewer.log'
function Say($m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    try { Add-Content -Path $ScriptLog -Value $line -Encoding utf8 } catch { }
}

function Send-GameLink([string]$command, [int]$timeoutMs = 60000) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        if (-not $client.ConnectAsync('127.0.0.1', 31500).Wait(2000)) { return $null }
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
                Start-Sleep -Milliseconds 250
                if (-not $stream.DataAvailable) { break }
            }
        }
        return $out.ToString().TrimEnd()
    } catch { return $null } finally { $client.Close() }
}

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say "NOT ELEVATED - the replacement process cannot be injected. Run this from an administrator shell."
    if (-not $NoWait) { Read-Host "press Enter to close" }
    exit 1
}

Say "launching '$Level' with GameMode '$Mode'"
$launcher = Join-Path $gamelink 'launch-hooked.ps1'
& powershell -NoProfile -ExecutionPolicy Bypass -File $launcher -Level $Level -Mode $Mode -Spectate -NoWait

$loaded = Send-GameLink 'levelname' 10000
Say ("levelname: " + $loaded)

if (-not $loaded -or $loaded -notmatch 'loaded level') {
    Say "no level is loaded - stopping here rather than writing into a process that is not ready"
    if (-not $NoWait) { Read-Host "press Enter to close" }
    exit 1
}

Say ""
Say "--- taking the camera ---"
# This first, and quickly. The spectator and registry steps that used to run
# before it took half a minute, so pressing W during the launch did nothing -
# the camera had not been taken yet. The spectator route is also dead: those
# objects are only built by a network join, which this route never performs.
Say (Send-GameLink 'watchdrive 1' 30000)

Say ""
Say "--- suppressing player input ---"
Say (Send-GameLink 'inputoff 1' 15000)

Say ""
Say "--- editor view settings ---"
# These field names come from OpenGameCamera, a working free-camera tool for
# this exact game (ArmchairDevelopers, GPL, pinned in plugin-corpus). We reach
# them by name through the reflection registry rather than by its
# signature-scanned static offsets.
Say ""
Say "--- moving to where the level actually is ---"
# Averaged from the placed entities rather than guessed coordinates. The deploy
# camera sits about three kilometres below the map.
Say (Send-GameLink 'camtoassets' 60000)

Say ""
Say "--- selection ---"
Say (Send-GameLink 'showsel 1' 20000)

Say ""
Say "--- view settings (slow: these scan the heap for live instances) ---"
# These two write settings by name through the reflection registry, which means
# scanning committed memory for live instances. With a level loaded that took 73
# seconds for hud and over thirteen minutes for fov on the 01:40 run, and because
# the module served one client at a time back then, everything queued behind it
# was lost - including camtoassets and showsel, which is why they now run first.
# The module threads connections now, but this script is still sequential, so
# order still decides what you get if you stop waiting.
# Fire and forget. Both write settings by name through the reflection registry,
# which scans all committed memory for live instances - measured at 73 seconds
# for hud and over thirteen minutes for forcefov. Waiting on them made every
# launch feel like a five-minute job when everything essential was already done.
# The module threads connections, so these run on their own worker and apply
# whenever they finish.
foreach ($slow in @('hud 0', 'forcefov 80')) {
    Start-Job -ScriptBlock {
        param($cmd)
        try {
            $c = New-Object System.Net.Sockets.TcpClient
            if ($c.ConnectAsync('127.0.0.1', 31500).Wait(2000)) {
                $s = $c.GetStream()
                $b = [Text.Encoding]::ASCII.GetBytes($cmd + [char]10)
                $s.Write($b, 0, $b.Length)
                $c.ReceiveTimeout = 900000
                $buf = New-Object byte[] 4096
                try { $s.Read($buf, 0, $buf.Length) | Out-Null } catch { }
            }
            $c.Close()
        } catch { }
    } -ArgumentList $slow | Out-Null
    Say ("queued in background: " + $slow)
}

Say ""
Say "READY - click the game window."
Say "  LEFT CLICK     select whatever is in the middle of the view"
Say "  W A S D        move"
Say "  gl.ps1 invert 0 0     if look is still the wrong way round"
Say "  Space / C      up / down"
Say "  right mouse    look (hold)"
Say "  Shift / Alt    fast x5 / slow x0.2"
Say ""
Say "  the deploy camera sits ~3 km under the map, so start with:"
Say "    gl.ps1 camto 0 60 0"
Say "  gl.ps1 speed 80        change the base speed"
Say "  gl.ps1 look 0.2        change mouse sensitivity"
Say "  gl.ps1 camwhere        where you are now"
Say "  gl.ps1 hud 1           put the HUD back"
Say "  gl.ps1 timescale 0     freeze the world"
Say "  gl.ps1 forcefov 90     change the field of view"
Say "  gl.ps1 watchdrive 0    hand the camera back"

Say ""
Say "The game is still running. Ask it anything with:"
Say "  powershell -ExecutionPolicy Bypass -File $gamelink\gl.ps1 <command>"
if (-not $NoWait) { Read-Host "press Enter to close" }
