# Send a command to the running module and print the reply.
#
#   powershell -ExecutionPolicy Bypass -File F:\Frosty\gamelink\gl.ps1 drawhere 2 30
#   powershell -ExecutionPolicy Bypass -File F:\Frosty\gamelink\gl.ps1 levelname
#
# Does not need an elevated shell - it is just a socket to a module that is
# already running. Only the launcher needs elevation.

# PositionalBinding=$false matters: without it PowerShell handed the second word
# of a command ("get ClientSettings IsSpectator 1") to -Port and failed on the
# type conversion. Only $Words is positional now; -Port and -TimeoutMs must be
# named, which is what anyone typing them would do anyway.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Words,
    [int]$Port = 31500,
    [int]$TimeoutMs = 60000
)

if (-not $Words -or $Words.Count -eq 0) {
    Write-Host "usage: gl.ps1 <command> [args...]"
    Write-Host ""
    Write-Host "  levelname            which level is loaded"
    Write-Host "  worldinfo            engine pointers"
    Write-Host "  hookstatus           did the level hook fire"
    Write-Host "  camlock 1            take the camera"
    Write-Host "  fly 1 20             keyboard fly, 20 m/s"
    Write-Host "  drawhere 2 30        white box + label 5 m ahead, for 30 s"
    Write-Host "  entities             what is in the world"
    Write-Host "  log 40               the module's own recent lines"
    exit 1
}

$command = $Words -join ' '

$client = New-Object System.Net.Sockets.TcpClient
try {
    if (-not $client.ConnectAsync('127.0.0.1', $Port).Wait(2000)) {
        Write-Host "nothing is listening on 127.0.0.1:$Port - is the game running with the module injected?"
        exit 1
    }

    $client.ReceiveTimeout = $TimeoutMs
    $stream = $client.GetStream()
    $payload = [Text.Encoding]::ASCII.GetBytes($command + "`n")
    $stream.Write($payload, 0, $payload.Length)

    # Read until the sender pauses - field tables and scans come back in pieces.
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

    Write-Host ("-> " + $command)
    Write-Host $out.ToString().TrimEnd()
} finally {
    $client.Close()
}
