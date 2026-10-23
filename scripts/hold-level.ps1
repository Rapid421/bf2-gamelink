# Level-override test, with Frosty out of the loop.
#
# Launches the game, injects FrostyGameLink, waits for the frontend, then tells the
# module to HOLD a level name in both places a level string is known to live:
# LoadSinglePlayerLevelEntityData.LevelName and GameSettings.Level.
#
# Then you start a match from the game's own menu and see which map loads.
#
# Run elevated.

param(
    [string]$Level   = 'Levels/MP/Naboo_01/Naboo_01',
    [string]$Mode    = '',
    [string]$Module  = 'F:\Frosty\gamelink\bin\FrostyGameLink.dll',
    [string]$GameExe = 'F:\STAR WARS Battlefront II\starwarsbattlefrontii.exe',
    [int]$Port = 31500
)

$ErrorActionPreference = 'Stop'
function Say($m) { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

function Send-GameLink([string]$command, [int]$timeoutMs = 60000) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        if (-not $client.ConnectAsync('127.0.0.1', $Port).Wait(2000)) { return $null }
        $client.ReceiveTimeout = $timeoutMs
        $stream = $client.GetStream()
        $payload = [Text.Encoding]::ASCII.GetBytes($command + "`n")
        $stream.Write($payload, 0, $payload.Length)
        $buffer = New-Object byte[] 8192
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -le 0) { return '' }
        return [Text.Encoding]::ASCII.GetString($buffer, 0, $read).Trim()
    } catch { return $null } finally { $client.Close() }
}

Add-Type -Namespace HL -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool i, int p);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr a, IntPtr s, uint t, uint p);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr s, out IntPtr w);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetModuleHandle(string n);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr m, string n);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr a, IntPtr s, IntPtr f, IntPtr p, uint fl, out uint id);
[DllImport("kernel32.dll", SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
'@

function Inject([int]$processId, [string]$dllPath) {
    $handle = [HL.Native]::OpenProcess(0x043A, $false, $processId)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess failed - run elevated" }
    $bytes = [Text.Encoding]::Unicode.GetBytes($dllPath + "`0")
    $remote = [HL.Native]::VirtualAllocEx($handle, [IntPtr]::Zero, [IntPtr]$bytes.Length, 0x3000, 4)
    [void][HL.Native]::WriteProcessMemory($handle, $remote, $bytes, [IntPtr]$bytes.Length, [ref]([IntPtr]::Zero))
    $loadLibrary = [HL.Native]::GetProcAddress([HL.Native]::GetModuleHandle('kernel32.dll'), 'LoadLibraryW')
    $thread = [HL.Native]::CreateRemoteThread($handle, [IntPtr]::Zero, [IntPtr]::Zero, $loadLibrary, $remote, 0, [ref]([uint32]0))
    if ($thread -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed" }
    [void][HL.Native]::WaitForSingleObject($thread, 15000)
    [void][HL.Native]::CloseHandle($thread); [void][HL.Native]::CloseHandle($handle)
}

# --- game up -----------------------------------------------------------------
$game = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue
if (-not $game) {
    Say "starting the game"
    Start-Process $GameExe -WorkingDirectory (Split-Path $GameExe) | Out-Null
    $deadline = (Get-Date).AddMinutes(3); $stable = 0; $candidate = 0
    while ((Get-Date) -lt $deadline -and $stable -lt 6) {
        Start-Sleep -Seconds 2
        $running = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue
        if ($running) {
            $pidNow = ($running | Select-Object -Last 1).Id
            if ($pidNow -eq $candidate) { $stable++ } else { $candidate = $pidNow; $stable = 0 }
        }
    }
    $game = Get-Process -Id $candidate -ErrorAction SilentlyContinue
    if (-not $game) { Say "no stable game process"; exit 1 }
}
$gamePid = ($game | Select-Object -Last 1).Id
Say "game pid $gamePid"

if (-not (Send-GameLink 'ping' 3000)) { Say "injecting"; Inject $gamePid $Module; Start-Sleep -Seconds 2 }
if (-not (Send-GameLink 'ping' 3000)) { Say "module not answering"; exit 1 }
Say "module up"

# --- wait for the frontend ---------------------------------------------------
Say "waiting for the frontend"
$ready = $false
for ($i = 0; $i -lt 90 -and -not $ready; $i++) {
    $info = Send-GameLink 'worldinfo' 5000
    if ($info -and $info -notmatch 'gameWorld\[client\]=0x0 ' -and $info -notmatch 'clientLevel=0x0 ') {
        Say $info; $ready = $true
    } else { Start-Sleep -Seconds 2 }
}
if (-not $ready) { Say "frontend never came up"; exit 1 }
Start-Sleep -Seconds 5

# --- what the loaders hold before we touch anything --------------------------
Say "loaders before:"
Send-GameLink 'spl' 60000

# --- hold the override -------------------------------------------------------
$command = if ($Mode) { "splhold $Level $Mode" } else { "splhold $Level" }
Say "-> $command"
Say (Send-GameLink $command 60000)

Say ""
Say "NOW: in the game, start a match from the menu - Instant Action, pick any map, play."
Say "Watch which map loads. The module keeps re-writing the override every 100 ms."
Say "Its log is C:\ProgramData\FrostyGameLink\gamelink.log - look for 'hold:' lines."
