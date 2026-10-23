# Launch the game SUSPENDED, inject before the first instruction runs, then resume.
#
# Why this exists: every previous approach injected into a settled frontend and
# then tried to force a level transition from outside the engine's state machine.
# KYBER never does that. KYBER is loaded before the engine initialises, hooks the
# initialisation path, sets what it wants while the engine is still constructing
# itself, and lets the engine drive the transition. This puts us in the same
# position it occupies.
#
# Because we create the process ourselves we own its handles outright, so this
# does not depend on opening a foreign process - though the game may still need
# an elevated parent depending on how it was installed.

param(
    [string]$Module  = 'F:\Frosty\gamelink\bin\FrostyGameLink.dll',
    [string]$GameExe = 'F:\STAR WARS Battlefront II\starwarsbattlefrontii.exe',
    [string]$GameArgs = '',
    [string]$Level = 'Levels/MP/Naboo_01/Naboo_01',
    [string]$Mode = 'Blast',
    [switch]$Viewer,
    [switch]$Spectate,
    [switch]$NoWait,
    [int]$Port = 31500
)

$ErrorActionPreference = 'Stop'
$ScriptLog = 'F:\Frosty\gamelink\launch-hooked.log'

function Say($m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $m
    Write-Host $line
    try { Add-Content -Path $ScriptLog -Value $line -Encoding utf8 } catch { }
}

function Wait-Close {
    if ($NoWait) { return }
    Read-Host "press Enter to close"
}

trap {
    Say ("UNHANDLED: " + $_.Exception.Message)
    Say ("  at " + $_.InvocationInfo.PositionMessage)
    Wait-Close
    exit 1
}

Add-Type -Namespace LH -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct STARTUPINFO {
    public int cb; public IntPtr lpReserved; public IntPtr lpDesktop; public IntPtr lpTitle;
    public int dwX; public int dwY; public int dwXSize; public int dwYSize;
    public int dwXCountChars; public int dwYCountChars; public int dwFillAttribute;
    public int dwFlags; public short wShowWindow; public short cbReserved2;
    public IntPtr lpReserved2; public IntPtr hStdInput; public IntPtr hStdOutput; public IntPtr hStdError;
}

[StructLayout(LayoutKind.Sequential)]
public struct PROCESS_INFORMATION {
    public IntPtr hProcess; public IntPtr hThread; public int dwProcessId; public int dwThreadId;
}

[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern bool CreateProcessW(
    string lpApplicationName, string lpCommandLine,
    IntPtr lpProcessAttributes, IntPtr lpThreadAttributes, bool bInheritHandles,
    uint dwCreationFlags, IntPtr lpEnvironment, string lpCurrentDirectory,
    ref STARTUPINFO lpStartupInfo, out PROCESS_INFORMATION lpProcessInformation);

[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool i, int p);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr a, IntPtr s, uint t, uint p);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr s, out IntPtr w);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr GetModuleHandleW(string n);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr m, string n);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr a, IntPtr s, IntPtr f, IntPtr p, uint fl, out uint id);
[DllImport("kernel32.dll", SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetExitCodeThread(IntPtr h, out uint code);
[DllImport("kernel32.dll", SetLastError=true)] public static extern uint ResumeThread(IntPtr h);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
'@

function Inject([int]$processId, [string]$dllPath) {
    # PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
    # PROCESS_VM_WRITE | PROCESS_VM_READ
    $handle = [LH.Native]::OpenProcess(0x043A, $false, $processId)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess failed - run elevated" }

    $bytes = [Text.Encoding]::Unicode.GetBytes($dllPath + "`0")
    $remote = [LH.Native]::VirtualAllocEx($handle, [IntPtr]::Zero, [IntPtr]$bytes.Length, 0x3000, 4)
    if ($remote -eq [IntPtr]::Zero) { throw "VirtualAllocEx failed" }

    [void][LH.Native]::WriteProcessMemory($handle, $remote, $bytes, [IntPtr]$bytes.Length, [ref]([IntPtr]::Zero))

    $loadLibrary = [LH.Native]::GetProcAddress([LH.Native]::GetModuleHandleW('kernel32.dll'), 'LoadLibraryW')
    $thread = [LH.Native]::CreateRemoteThread($handle, [IntPtr]::Zero, [IntPtr]::Zero, $loadLibrary, $remote, 0, [ref]([uint32]0))
    if ($thread -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed" }

    [void][LH.Native]::WaitForSingleObject($thread, 20000)
    [void][LH.Native]::CloseHandle($thread)
    [void][LH.Native]::CloseHandle($handle)
}

function Send-GameLink([string]$command, [int]$timeoutMs = 30000) {
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

Say "=== suspended launch ==="

if (-not (Test-Path $Module))  { Say "module missing: $Module";  Wait-Close; exit 1 }
if (-not (Test-Path $GameExe)) { Say "game missing: $GameExe";   Wait-Close; exit 1 }
Say ("module {0} bytes, built {1}" -f (Get-Item $Module).Length, (Get-Item $Module).LastWriteTime)

$existing = Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue
if ($existing) {
    Say "a game process is already running - stopping it so the DLL is free and the launch is clean"
    $existing | Stop-Process -Force
    Start-Sleep -Seconds 2
}

$startupInfo = New-Object LH.Native+STARTUPINFO
$startupInfo.cb = [Runtime.InteropServices.Marshal]::SizeOf($startupInfo)
$processInfo = New-Object LH.Native+PROCESS_INFORMATION

$commandLine = '"' + $GameExe + '"'
if ($GameArgs) { $commandLine += ' ' + $GameArgs }

$CREATE_SUSPENDED = 0x00000004
$workingDirectory = Split-Path $GameExe

Say "CreateProcess (suspended): $commandLine"
$created = [LH.Native]::CreateProcessW(
    $GameExe, $commandLine, [IntPtr]::Zero, [IntPtr]::Zero, $false,
    $CREATE_SUSPENDED, [IntPtr]::Zero, $workingDirectory,
    [ref]$startupInfo, [ref]$processInfo)

if (-not $created) {
    $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Say "CreateProcessW failed, win32 error $code"
    if ($code -eq 740) { Say "  740 = elevation required. Run this from an administrator shell." }
    Wait-Close
    exit 1
}

Say ("created pid {0}, main thread suspended" -f $processInfo.dwProcessId)

# Inject while nothing in the process has executed yet. ntdll is mapped and
# kernel32 is loaded by the loader before the entry point, so LoadLibraryW is
# already resolvable at the same address in both processes.
$bytes = [Text.Encoding]::Unicode.GetBytes($Module + "`0")
$remote = [LH.Native]::VirtualAllocEx($processInfo.hProcess, [IntPtr]::Zero, [IntPtr]$bytes.Length, 0x3000, 4)
if ($remote -eq [IntPtr]::Zero) { Say "VirtualAllocEx failed"; Wait-Close; exit 1 }

$written = [IntPtr]::Zero
if (-not [LH.Native]::WriteProcessMemory($processInfo.hProcess, $remote, $bytes, [IntPtr]$bytes.Length, [ref]$written)) {
    Say "WriteProcessMemory failed"; Wait-Close; exit 1
}

$loadLibrary = [LH.Native]::GetProcAddress([LH.Native]::GetModuleHandleW('kernel32.dll'), 'LoadLibraryW')
if ($loadLibrary -eq [IntPtr]::Zero) { Say "LoadLibraryW not found"; Wait-Close; exit 1 }

$thread = [LH.Native]::CreateRemoteThread($processInfo.hProcess, [IntPtr]::Zero, [IntPtr]::Zero, $loadLibrary, $remote, 0, [ref]([uint32]0))
if ($thread -eq [IntPtr]::Zero) {
    Say ("CreateRemoteThread failed, win32 error {0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    Wait-Close; exit 1
}

[void][LH.Native]::WaitForSingleObject($thread, 20000)
$moduleHandle = [uint32]0
[void][LH.Native]::GetExitCodeThread($thread, [ref]$moduleHandle)
Say ("injected before entry point (LoadLibraryW returned 0x{0:X})" -f $moduleHandle)
[void][LH.Native]::CloseHandle($thread)

Say "resuming the main thread"
[void][LH.Native]::ResumeThread($processInfo.hThread)
[void][LH.Native]::CloseHandle($processInfo.hThread)
[void][LH.Native]::CloseHandle($processInfo.hProcess)

# Watch for the re-exec and take the replacement process.
Say "watching for a relaunch (this game re-execs itself)"
$firstPid = $processInfo.dwProcessId
$targetPid = $firstPid
$deadline = (Get-Date).AddSeconds(30)

while ((Get-Date) -lt $deadline) {
    $running = @(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue)

    $replacement = $running | Where-Object { $_.Id -ne $firstPid } | Select-Object -First 1
    if ($replacement) {
        $targetPid = $replacement.Id
        Say ("relaunched as pid {0} - injecting into the replacement" -f $targetPid)
        try {
            Inject $targetPid $Module
            Say "injected into the replacement"
        } catch {
            Say ("injection into the replacement failed: " + $_.Exception.Message)
            Say "  this step needs an elevated shell - the child is not our own process"
        }
        break
    }

    # Do NOT bail when the first process disappears. Observed 15:55: the original
    # exits and there is a gap of a second or more before the replacement shows
    # up, so treating "no process right now" as failure gives up just before the
    # thing we are waiting for appears.
    Start-Sleep -Milliseconds 25
}

if ($targetPid -eq $firstPid) {
    $stillHere = @(Get-Process starwarsbattlefrontii -ErrorAction SilentlyContinue) |
                 Where-Object { $_.Id -eq $firstPid }
    if ($stillHere) {
        Say "no relaunch seen within 30 s - the original process is still the target"
    } else {
        Say "the original exited and no replacement appeared within 30 s"
        Wait-Close
        exit 1
    }
}

Say "waiting for the module's command port"
$up = $false
for ($i = 0; $i -lt 60 -and -not $up; $i++) {
    if (Send-GameLink 'ping' 2000) { $up = $true } else { Start-Sleep -Seconds 1 }
}
if (-not $up) { Say "the module never opened its port"; Wait-Close; exit 1 }
Say "module up"

# Watch the engine build itself. This is the whole point of the exercise: we are
# now present for initialisation instead of arriving after it.
# Arm the hook immediately. The init trace puts the engine's own spawn at about
# 9 seconds in, and the port is answering within a second of the injection, so
# there is a wide margin - but this is the one step that must not be late.
# Arm the viewer before the level exists, so the camera is ours from the first
# frame rather than after the match has pointed it somewhere.
if ($Viewer) {
    Say ("-> viewer 1 : " + (Send-GameLink 'viewer 1' 10000))
}

# Arm spectator mode before anything joins. Kyber sets its equivalent flag ahead
# of the client state change, and BFII's own spectator mode is where the shipped
# free camera lives - ClientSettings::IsSpectator +0x8C, confirmed present in
# this build's reflection registry.
if ($Spectate) {
    Say ("-> spectate 1 : " + (Send-GameLink 'spectate 1' 15000))
}

$armCommand = "hookspawn $Level"
if ($Mode) { $armCommand += " $Mode" }
Say "-> $armCommand"
$armed = Send-GameLink $armCommand 10000
Say ("   " + $armed)
if (-not $armed -or $armed -notmatch 'armed') {
    Say "the hook did not arm - the engine will load its own level"
}

Say "watching engine initialisation:"
for ($i = 0; $i -lt 90; $i++) {
    $info = Send-GameLink 'worldinfo' 5000
    if ($info) { Say ("  " + $info) }
    if ($info -and $info -notmatch 'clientLevel=0x0 ') { Say "  a level is up"; break }
    Start-Sleep -Seconds 2
}

Say ("hook status: " + (Send-GameLink 'hookstatus' 5000))
Say ("LEVEL LOADED: " + (Send-GameLink 'levelname' 10000))
if ($Viewer) { Say ("fly: " + (Send-GameLink 'fly 1' 10000)) }
Say "module trace:"
Say (Send-GameLink 'log 40' 15000)

Say ""
Say "This script's log: $ScriptLog"
Say "Module log: C:\ProgramData\FrostyGameLink\gamelink.log"
Wait-Close
