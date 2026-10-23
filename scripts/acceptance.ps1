[CmdletBinding()]
param(
    [int]$Port = 31500,
    [int]$TimeoutMs = 60000,
    [int]$ScanTimeoutMs = 480000,
    [string]$Out = 'F:\Frosty\gamelink\acceptance-latest.txt'
)

$ErrorActionPreference = 'Stop'
$results = New-Object System.Collections.Generic.List[object]
$transcript = New-Object Text.StringBuilder

function Send-GameLink([string]$Command, [int]$DeadlineMs) {
    $client = New-Object System.Net.Sockets.TcpClient
    $started = Get-Date
    try {
        if (-not $client.ConnectAsync('127.0.0.1', $Port).Wait(2000)) {
            throw "nothing is listening on 127.0.0.1:$Port"
        }
        $client.ReceiveTimeout = $DeadlineMs
        $stream = $client.GetStream()
        $payload = [Text.Encoding]::ASCII.GetBytes($Command + "`n")
        $stream.Write($payload, 0, $payload.Length)

        $reply = New-Object Text.StringBuilder
        $buffer = New-Object byte[] 65536
        while ($true) {
            try { $read = $stream.Read($buffer, 0, $buffer.Length) }
            catch { throw "reply timeout/error: $($_.Exception.Message)" }
            if ($read -le 0) { break }
            [void]$reply.Append([Text.Encoding]::ASCII.GetString($buffer, 0, $read))
            if (-not $stream.DataAvailable) {
                Start-Sleep -Milliseconds 550
                if (-not $stream.DataAvailable) { break }
            }
        }
        $text = $reply.ToString().Trim()
        if ([string]::IsNullOrWhiteSpace($text)) { throw 'empty reply' }
        [void]$transcript.AppendLine("")
        [void]$transcript.AppendLine("=== $Command ===")
        [void]$transcript.AppendLine($text)
        return $text
    }
    catch {
        $text = "TRANSPORT ERROR: $($_.Exception.Message)"
        [void]$transcript.AppendLine("")
        [void]$transcript.AppendLine("=== $Command ===")
        [void]$transcript.AppendLine($text)
        return $text
    }
    finally {
        $client.Close()
        $elapsed = [int]((Get-Date) - $started).TotalMilliseconds
        [void]$transcript.AppendLine("[$elapsed ms]")
    }
}

function Add-Result([string]$Name, [bool]$Passed, [string]$Expected, [string]$Observed) {
    $row = [pscustomobject]@{
        Name = $Name
        Passed = $Passed
        Expected = $Expected
        Observed = $Observed
    }
    [void]$results.Add($row)
    $word = if ($Passed) { 'PASS' } else { 'FAIL' }
    Write-Host ("[{0}] {1} | expected: {2} | observed: {3}" -f $word, $Name, $Expected, $Observed)
}

function Number([string]$Text) {
    $value = 0.0
    if (-not [double]::TryParse($Text, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$value)) {
        throw "not a number: $Text"
    }
    return $value
}

Write-Host "FrostyGameLink self-judging acceptance $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

$level = Send-GameLink 'levelname' $TimeoutMs
$levelOk = $level -match '^loaded level:\s+Levels/'
Add-Result 'levelname' $levelOk 'loaded level path beginning Levels/' (($level -split "`n")[0])

$world = Send-GameLink 'worldinfo' $TimeoutMs
$worldOk = $world -match 'gameWorld\[client\]=0x(?!0\b)[0-9A-Fa-f]+' -and
           $world -match 'clientLevel=0x(?!0\b)[0-9A-Fa-f]+' -and
           $world -match 'renderView=0x(?!0\b)[0-9A-Fa-f]+'
Add-Result 'worldinfo' $worldOk 'non-zero client world, client level, and render view' (($world -split "`n")[0])

$sublevels = Send-GameLink 'sublevels' $ScanTimeoutMs
$requiredLayers = @('Naboo_01','GameModes','TeamDeathmatch','TeamDeathmatch_Online',
                    'Lobby','Naboo_012','HighEnd','Sunset')
$missingLayers = @($requiredLayers | Where-Object { $sublevels.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -lt 0 })
$sublevelOk = $missingLayers.Count -eq 0
$sublevelObserved = if ($sublevelOk) { 'all 8 required names present' } else { 'missing ' + ($missingLayers -join ', ') }
Add-Result 'sublevels' $sublevelOk 'Naboo serialized/dump hierarchy names all present' $sublevelObserved

$entities = Send-GameLink 'entities' $ScanTimeoutMs
$entityMatch = [regex]::Match($entities, '(?m)^(\d+) entities, (\d+) shown')
$entityTotal = if ($entityMatch.Success) { [int]$entityMatch.Groups[1].Value } else { 0 }
$entityShown = if ($entityMatch.Success) { [int]$entityMatch.Groups[2].Value } else { 0 }
$entityOk = $entityTotal -ge 500 -and $entityTotal -le 10000 -and
            $entityShown -eq $entityTotal -and
            $entities -match 'ClientStaticModelEntity' -and $entities -match 'Levels/MP/Naboo_01'
Add-Result 'entities' $entityOk '500..10000, all rows shown, Naboo owner paths, ClientStaticModelEntity present' ("total=$entityTotal shown=$entityShown")

$pickrender = Send-GameLink 'pickrender 20000' $ScanTimeoutMs
$pickCountMatch = [regex]::Match($pickrender, '(?m)^(\d+) placed objects cached')
$pickAverageMatch = [regex]::Match($pickrender,
    'average position\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)')
$pickCount = if ($pickCountMatch.Success) { [int]$pickCountMatch.Groups[1].Value } else { 0 }
$px = 0.0; $py = 0.0; $pz = 0.0
if ($pickAverageMatch.Success) {
    $px = Number $pickAverageMatch.Groups[1].Value
    $py = Number $pickAverageMatch.Groups[2].Value
    $pz = Number $pickAverageMatch.Groups[3].Value
}
$pickOk = $pickCount -ge 1500 -and $pickCount -le 2200 -and
          $px -ge 500 -and $px -le 850 -and
          $py -ge 350 -and $py -le 550 -and
          $pz -ge -450 -and $pz -le 100
Add-Result 'pickrender' $pickOk '1500..2200; average X 500..850, Y 350..550, Z -450..100' ("count=$pickCount average=$px,$py,$pz")

$watch = Send-GameLink 'watchdrive 1' $TimeoutMs
$watchOk = $watch -notmatch 'failed|could not|no render|TRANSPORT ERROR'
Add-Result 'watchdrive' $watchOk 'frame-thread hook armed without an error' (($watch -split "`n")[0])

$renderer = Send-GameLink 'debugrenderer' $TimeoutMs
$defaultPair = $renderer -match '(?m)^\s*\[19\].*game\+0x14D9060\s*$' -and
               $renderer -match '(?m)^\s*\[20\].*game\+0x14DF110\s*$'
$fallbackPair = $renderer -match '(?m)^\s*\[18\].*game\+0x14D9060\s*$' -and
                $renderer -match '(?m)^\s*\[19\].*game\+0x14DF110\s*$'
$chosenPair = ''
if ($defaultPair) {
    $chosenPair = '19 20'
} elseif ($fallbackPair) {
    # Automatic second pass is read-only gated: never call 18/19 unless the live
    # table proves those slots contain the known begin/end function RVAs.
    $chosenPair = '18 19'
}
$rendererOk = $chosenPair.Length -gt 0
$rendererObserved = if ($rendererOk) { "safe pair $chosenPair" } else { 'neither known RVA pair appears in the live frame-thread table' }
Add-Result 'renderer-layout' $rendererOk 'automatic 19/20 pass, then read-only-gated 18/19 second pass' $rendererObserved

if ($rendererOk) {
    $slotReply = Send-GameLink ("vertexslots " + $chosenPair) $TimeoutMs
    $slotOk = $slotReply -match ("beginVertices slot " + ($chosenPair -split ' ')[0] +
        ", endVertices slot " + ($chosenPair -split ' ')[1])
    Add-Result 'vertexslots' $slotOk ("selected safe live-RVA pair $chosenPair") (($slotReply -split "`n")[0])

    $frame = Send-GameLink 'framedraw 1' $TimeoutMs
    $frameMatch = [regex]::Match($frame,
        'handler submitted\s+(\d+)\s+time\(s\).*?(\d+)\s+lines',
        [Text.RegularExpressions.RegexOptions]::Singleline)
    $submits = if ($frameMatch.Success) { [int]$frameMatch.Groups[1].Value } else { 0 }
    $lines = if ($frameMatch.Success) { [int]$frameMatch.Groups[2].Value } else { 0 }
    $frameOk = $submits -ge 1 -and $lines -ge 12
    Add-Result 'framedraw' $frameOk 'at least 1 frame-thread submission and 12 box lines' ("submits=$submits lines=$lines")
} else {
    Add-Result 'vertexslots' $false 'known begin/end RVAs before any call' 'submission skipped safely'
    Add-Result 'framedraw' $false 'renderer layout compatible' 'submission skipped safely'
}

$nested = Send-GameLink 'get FiringFunctionData FireLogic.RateOfFire 4' $ScanTimeoutMs
$nestedMatch = [regex]::Match($nested,
    'FiringFunctionData::FireLogic\.RateOfFire \+0x[0-9A-Fa-f]+ across (\d+) instance\(s\)')
$nestedInstances = if ($nestedMatch.Success) { [int]$nestedMatch.Groups[1].Value } else { 0 }
Add-Result 'nested-field-read' ($nestedInstances -gt 0) 'dotted RateOfFire resolves on at least one live FiringFunctionData' ("instances=$nestedInstances")

$passed = @($results | Where-Object { $_.Passed }).Count
$failed = $results.Count - $passed
$verdict = if ($failed -eq 0) { 'PASS' } else { 'FAIL' }

$summary = New-Object Text.StringBuilder
[void]$summary.AppendLine("FrostyGameLink acceptance $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
foreach ($row in $results) {
    $word = if ($row.Passed) { 'PASS' } else { 'FAIL' }
    [void]$summary.AppendLine(("[{0}] {1} | expected: {2} | observed: {3}" -f
        $word, $row.Name, $row.Expected, $row.Observed))
}
[void]$summary.AppendLine("")
[void]$summary.AppendLine("VERDICT $verdict - $passed passed, $failed failed")
[void]$summary.AppendLine("")
[void]$summary.Append($transcript.ToString())

Set-Content -LiteralPath $Out -Value $summary.ToString() -Encoding UTF8
Write-Host ""
Write-Host "VERDICT $verdict - $passed passed, $failed failed"
Write-Host "report: $Out"
if ($failed -ne 0) { exit 1 }
exit 0
