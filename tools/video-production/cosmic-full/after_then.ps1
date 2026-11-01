# Run a fix test only after every running freeze/fix test has finished (never two tests at once).
# usage: powershell -File after_then.ps1 <variants.json> <log name>
param([string]$Variants, [string]$Log)
$ErrorActionPreference = 'Stop'
if ($env:COMPUTERNAME -ne 'ADI-PC') { throw "wrong PC: $env:COMPUTERNAME" }
$R = 'D:\AvasthaAI\CosmicRainFull-20261027'
$me = Get-CimInstance Win32_Process -Filter "ProcessId = $PID"
# Older waiters go first, so queued tests run one after another in the order they were queued.
while (Get-CimInstance Win32_Process | Where-Object {
        ($_.Name -eq 'python.exe' -and $_.CommandLine -match 'freeze_test\.py|fix_test\.py') -or
        ($_.Name -eq 'powershell.exe' -and $_.CommandLine -match 'after_then\.ps1' -and $_.ProcessId -ne $PID -and $_.CreationDate -lt $me.CreationDate) }) {
    Start-Sleep 30
}
& D:\AvasthaAI\venv\Scripts\python.exe -u "$R\fix_test.py" $R "$R\$Variants" *> "$R\$Log"
