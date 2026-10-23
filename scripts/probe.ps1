# Read-only probe. Launches, injects, and asks the module questions - it never
# calls into the engine, so the game stays up and can be probed repeatedly.
#
# This exists because every spawn attempt kills the process within ~3 seconds,
# which makes interactive investigation impossible. Everything here is a read.
#
# Run elevated.

param(
    [string]$Module  = 'F:\Frosty\gamelink\bin\FrostyGameLink.dll',
    [string]$GameExe = 'F:\STAR WARS Battlefront II\starwarsbattlefrontii.exe',
    [string[]]$Commands = @(),
    [switch]$NoWait,
    [int]$Port = 31500
)

$ErrorActionPreference = 'Stop'
$ScriptLog = 'F:\Frosty\gamelink\probe.log'

function Say($m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    try { Add-Content -Path $ScriptLog -Value $line -Encoding utf8 } catch { }
}

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

function Send-GameLink([string]$command, [int]$timeoutMs = 60000) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        if (-not $client.ConnectAsync('127.0.0.1', $Port).Wait(2000)) { return $null }
        $client.ReceiveTimeout = $timeoutMs
        $stream = $client.GetStream()
        $payload = [Text.Encoding]::ASCII.GetBytes($command + "`n")
        $stream.Write($payload, 0, $payload.Length)

        # Replies can be large - a field table or a scan result is many lines - so
        # read until the sender pauses rather than taking the first packet.
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

Add-Type -Namespace PB -Name Native -MemberDefinition @'
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
    $handle = [PB.Native]::OpenProcess(0x043A, $false, $processId)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess failed - run elevated" }
    $bytes = [Text.Encoding]::Unicode.GetBytes($dllPath + "`0")
    $remote = [PB.Native]::VirtualAllocEx($handle, [IntPtr]::Zero, [IntPtr]$bytes.Length, 0x3000, 4)
    [void][PB.Native]::WriteProcessMemory($handle, $remote, $bytes, [IntPtr]$bytes.Length, [ref]([IntPtr]::Zero))
    $loadLibrary = [PB.Native]::GetProcAddress([PB.Native]::GetModuleHandle('kernel32.dll'), 'LoadLibraryW')
    $thread = [PB.Native]::CreateRemoteThread($handle, [IntPtr]::Zero, [IntPtr]::Zero, $loadLibrary, $remote, 0, [ref]([uint32]0))
    if ($thread -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed" }
    [void][PB.Native]::WaitForSingleObject($thread, 15000)
    [void][PB.Native]::CloseHandle($thread); [void][PB.Native]::CloseHandle($handle)
}

Say "=== probe starting (read-only) ==="

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say "NOT ELEVATED - injection will fail. Start this from an administrator shell."
    Wait-Close
    exit 1
}
if (-not (Test-Path $Module))  { Say "module missing: $Module";  Wait-Close; exit 1 }
if (-not (Test-Path $GameExe)) { Say "game missing: $GameExe";   Wait-Close; exit 1 }
Say ("module {0} bytes, built {1}" -f (Get-Item $Module).Length, (Get-Item $Module).LastWriteTime)

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
    if (-not $game) { Say "no stable game process"; Wait-Close; exit 1 }
}
Say "game pid $(($game | Select-Object -Last 1).Id)"

if (-not (Send-GameLink 'ping' 3000)) { Say "injecting"; Inject ($game | Select-Object -Last 1).Id $Module; Start-Sleep -Seconds 2 }
if (-not (Send-GameLink 'ping' 3000)) { Say "module not answering"; Wait-Close; exit 1 }
Say "module up"

Say "waiting for the frontend"
$ready = $false
for ($i = 0; $i -lt 90 -and -not $ready; $i++) {
    $info = Send-GameLink 'worldinfo' 5000
    if ($info -and $info -notmatch 'gameWorld\[client\]=0x0 ' -and $info -notmatch 'clientLevel=0x0 ') {
        Say $info; $ready = $true
    } else { Start-Sleep -Seconds 2 }
}
if (-not $ready) { Say "frontend never came up"; Wait-Close; exit 1 }

if ($Commands.Count -eq 0) {
    $Commands = @(
        'refs "Levels/FrontEnd/FrontEnd" 40',
        'refs "Levels/Frontend/Frontend" 40',
        'refs "Levels/MP/Naboo_01/Naboo_01" 40'
    )
}

foreach ($command in $Commands) {
    Say ""
    Say "----- $command -----"
    $reply = Send-GameLink $command 60000
    if ($null -eq $reply) { Say "(no reply)" } else { Say $reply }
}

Say ""
Say "The game is still running - re-run with -Commands to ask more."
Say "This script's log: $ScriptLog"
Wait-Close
