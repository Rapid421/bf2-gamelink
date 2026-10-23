# Game-window-only screenshot. Refuses (exit 2, no file written) unless the foreground window belongs to
# starwarsbattlefrontii.exe, and captures only that window's rectangle -- never the rest of the desktop, so a user working
# on the machine is never recorded.
param([string]$Out = 'F:\Frosty\gamelink\shot.png', [int]$Width = 1280)
Add-Type -AssemblyName System.Drawing
Add-Type -Name U -Namespace W -MemberDefinition @'
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int Left, Top, Right, Bottom; }
'@
$fg = [W.U]::GetForegroundWindow()
$fgPid = [uint32]0
[void][W.U]::GetWindowThreadProcessId($fg, [ref]$fgPid)
$game = Get-Process -Name starwarsbattlefrontii -ErrorAction SilentlyContinue
if (-not $game -or ($game.Id -notcontains [int]$fgPid)) {
    $name = (Get-Process -Id $fgPid -ErrorAction SilentlyContinue).ProcessName
    Write-Output ("SKIPPED: foreground is '{0}', not the game" -f $name)
    exit 2
}
$r = New-Object W.U+RECT
[void][W.U]::GetWindowRect($fg, [ref]$r)
$w0 = $r.Right - $r.Left; $h0 = $r.Bottom - $r.Top
if ($w0 -le 0 -or $h0 -le 0) { Write-Output "SKIPPED: empty game window rect"; exit 2 }
$full = New-Object Drawing.Bitmap $w0, $h0
$g = [Drawing.Graphics]::FromImage($full)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $full.Size)
$g.Dispose()
$save = $full
if ($Width -gt 0 -and $full.Width -gt $Width) {
    $h = [int]($full.Height * $Width / $full.Width)
    $save = New-Object Drawing.Bitmap $Width, $h
    $g2 = [Drawing.Graphics]::FromImage($save)
    $g2.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.DrawImage($full, 0, 0, $Width, $h)
    $g2.Dispose()
}
$save.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
Write-Output ("{0} {1}x{2}" -f $Out, $save.Width, $save.Height)
if ($save -ne $full) { $save.Dispose() }
$full.Dispose()
