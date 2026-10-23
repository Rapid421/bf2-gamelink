# Spawn a server directly on a level - the supported route into a map.
#
# Posting a load-level message from the frontend is not supported and kills the
# process (the handler dereferences a null m_level ~0.6 s later). Spawning a server
# is what the game itself does, and it initialises that state properly.
#
# Sequence: launch, inject, wait for the frontend, locate GameSimulation, spawn.
# No menu clicking at any point.
#
# Run elevated.

param(
    [string]$Level   = 'Levels/MP/Naboo_01/Naboo_01',
    [string]$Mode    = '',
    [string]$Module  = 'F:\Frosty\gamelink\bin\FrostyGameLink.dll',
    [string]$GameExe = 'F:\STAR WARS Battlefront II\starwarsbattlefrontii.exe',
    [switch]$NoWait,
    [int]$Port = 31500
)

$ErrorActionPreference = 'Stop'

# Everything this script says also goes to a file. A run that ends with nothing
# on screen - a closed window, a lost console, a failure before the first line -
# is otherwise unreadable after the fact.
$ScriptLog = 'F:\Frosty\gamelink\spawn-level.log'
function Say($m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    try { Add-Content -Path $ScriptLog -Value $line -Encoding utf8 } catch { }
}

# A failure anywhere must still leave a reason behind, and the window must stay
# open long enough to read it.
function Wait-Close {
    # Unattended runs must not block on a prompt nobody is there to answer.
    if ($NoWait) { return }
    Read-Host "press Enter to close"
}

trap {
    Say ("UNHANDLED: " + $_.Exception.Message)
    Say ("  at " + $_.InvocationInfo.PositionMessage)
    Wait-Close
    exit 1
}

Say "=== spawn-level starting (level '$Level', mode '$Mode') ==="

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say "NOT ELEVATED - injection will fail. Start this from an administrator shell."
    Wait-Close
    exit 1
}
if (-not (Test-Path $Module)) { Say "module missing: $Module"; Wait-Close; exit 1 }
if (-not (Test-Path $GameExe)) { Say "game missing: $GameExe"; Wait-Close; exit 1 }
Say ("module {0} bytes, built {1}" -f (Get-Item $Module).Length, (Get-Item $Module).LastWriteTime)

function Send-GameLink([string]$command, [int]$timeoutMs = 120000) {
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

Add-Type -Namespace SP -Name Native -MemberDefinition @'
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
    $handle = [SP.Native]::OpenProcess(0x043A, $false, $processId)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess failed - run elevated" }
    $bytes = [Text.Encoding]::Unicode.GetBytes($dllPath + "`0")
    $remote = [SP.Native]::VirtualAllocEx($handle, [IntPtr]::Zero, [IntPtr]$bytes.Length, 0x3000, 4)
    [void][SP.Native]::WriteProcessMemory($handle, $remote, $bytes, [IntPtr]$bytes.Length, [ref]([IntPtr]::Zero))
    $loadLibrary = [SP.Native]::GetProcAddress([SP.Native]::GetModuleHandle('kernel32.dll'), 'LoadLibraryW')
    $thread = [SP.Native]::CreateRemoteThread($handle, [IntPtr]::Zero, [IntPtr]::Zero, $loadLibrary, $remote, 0, [ref]([uint32]0))
    if ($thread -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed" }
    [void][SP.Native]::WaitForSingleObject($thread, 15000)
    [void][SP.Native]::CloseHandle($thread); [void][SP.Native]::CloseHandle($handle)
}

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
Say "game pid $(($game | Select-Object -Last 1).Id)"

if (-not (Send-GameLink 'ping' 3000)) { Say "injecting"; Inject ($game | Select-Object -Last 1).Id $Module; Start-Sleep -Seconds 2 }
if (-not (Send-GameLink 'ping' 3000)) { Say "module not answering"; exit 1 }
Say "module up"

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

# Locate the instance before calling into it - a failure here is harmless, a bad
# call is not.
Say "locating GameSimulation"
$found = Send-GameLink 'gamesim' 30000
Say $found
if (-not $found -or $found -notmatch 'GameSimulation at') {
    Say "cannot find GameSimulation - not spawning"
    exit 1
}

$command = if ($Mode) { "spawn $Level $Mode" } else { "spawn $Level" }
Say "-> $command"
Say (Send-GameLink $command 120000)

Start-Sleep -Seconds 8
Say "world state after the spawn:"
Say (Send-GameLink 'worldinfo' 10000)

Say ""
Say "watch trace from the module:"
Say (Send-GameLink 'log 40' 10000)
Say ""
Say "Full module log: C:\ProgramData\FrostyGameLink\gamelink.log"
Say "This script's log: $ScriptLog"
Wait-Close
