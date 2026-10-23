# One-time setup: registers the "FrostyAutoTest" scheduled task that runs autotest.ps1 with
# highest privileges in your interactive session. Run this ONCE from an elevated PowerShell:
#
#   powershell -ExecutionPolicy Bypass -File F:\Frosty\gamelink\register-autotest.ps1
#
# Afterwards a non-elevated session can trigger a test with:  Start-ScheduledTask FrostyAutoTest
# (arguments come from F:\Frosty\gamelink\autotest-args.json, the verdict lands in autotest-result.json).
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -File F:\Frosty\gamelink\autotest.ps1'
$p = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 20) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName 'FrostyAutoTest' -Action $a -Principal $p -Settings $s -Force | Out-Null
Get-ScheduledTask -TaskName 'FrostyAutoTest' | Select-Object TaskName, State
Write-Host "registered. You can close this window."
