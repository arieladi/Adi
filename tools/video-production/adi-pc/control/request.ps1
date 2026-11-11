# Desktop button: ask the AI controller to stop or continue, and show its progress.
param([ValidateSet('stop', 'continue')][string]$Action)
$C = 'F:\AvasthaAI\control'
$want = @{ stop = 'stopped'; continue = 'running' }[$Action]
$Host.UI.RawUI.WindowTitle = "Video AI - $Action"
Set-Content "$C\requests\$Action.flag" (Get-Date).ToString('s')
Write-Host ("{0} requested. " -f $Action) -NoNewline
if ($Action -eq 'stop') { Write-Host "This can take up to 10 minutes while the current video take finishes." } else { Write-Host "Starting the video AI again." }
$t0 = Get-Date; $last = ''
while (((Get-Date) - $t0).TotalMinutes -lt 15) {
    Start-Sleep 3
    try { $s = Get-Content "$C\state.json" -Raw | ConvertFrom-Json } catch { continue }
    if ($s.note -ne $last) { Write-Host ("{0:HH:mm:ss}  {1}: {2}" -f (Get-Date), $s.state, $s.note); $last = $s.note }
    if ([datetime]$s.time -gt $t0.AddSeconds(-1) -and ($s.state -eq $want -or $s.state -eq 'failed')) {
        if ($s.state -eq 'stopped') { Write-Host "`nDone. GPUs:"; $s.gpu | ForEach-Object { Write-Host "  $_" }; Write-Host "RAM in use: $($s.ram.in_use_gb) of $($s.ram.total_gb) GB (the rest is free). Ableton can use the PC." }
        if ($s.state -eq 'running') { Write-Host "`nDone. The video work continues where it stopped." }
        if ($s.state -eq 'failed') { Write-Host "`nSomething went wrong - Claude will see it in the log." -ForegroundColor Red }
        Write-Host "You can close this window."; Read-Host | Out-Null; exit
    }
}
Write-Host "No answer from the controller after 15 minutes - Claude will see it in the log." -ForegroundColor Red
Read-Host | Out-Null
