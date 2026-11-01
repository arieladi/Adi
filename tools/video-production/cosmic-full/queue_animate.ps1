# Start animate_full.py on a plan only after every running/queued freeze or fix test has finished (one GPU job at a time).
# usage: powershell -File queue_animate.ps1 <plan file> <log name>
param([string]$Plan, [string]$Log)
$ErrorActionPreference = 'Stop'
if ($env:COMPUTERNAME -ne 'ADI-PC') { throw "wrong PC: $env:COMPUTERNAME" }
$R = 'D:\AvasthaAI\CosmicRainFull-20261027'
$me = Get-CimInstance Win32_Process -Filter "ProcessId = $PID"
while (Get-CimInstance Win32_Process | Where-Object {
        ($_.Name -eq 'python.exe' -and $_.CommandLine -match 'freeze_test\.py|fix_test\.py|animate_full\.py') -or
        ($_.Name -eq 'powershell.exe' -and $_.CommandLine -match 'after_then\.ps1|queue_animate\.ps1' -and $_.ProcessId -ne $PID -and $_.CreationDate -lt $me.CreationDate) }) {
    Start-Sleep 30
}
& D:\AvasthaAI\venv\Scripts\python.exe -u "$R\animate_full.py" $R http://127.0.0.1:8192 a2000 D:/AvasthaAI/VideoEditFour-20261025 $Plan *> "$R\$Log"
