# Restart the Cosmic Rain ComfyUI (port 8192, A2000) only when its queue is empty.
# Stops exactly the launch_server.py chain and relaunches it detached (survives the SSH session).
# Never touches the Deforum/Forge servers (7865/7866) or any other process.
$ErrorActionPreference = 'Stop'
if ($env:COMPUTERNAME -ne 'ADI-PC') { throw "wrong PC: $env:COMPUTERNAME" }
$api = 'http://127.0.0.1:8192'
$q = Invoke-RestMethod "$api/queue"
if ($q.queue_running.Count -or $q.queue_pending.Count) { throw 'ComfyUI queue is not empty; not restarting' }
$all = Get-CimInstance Win32_Process
$chain = $all | Where-Object {
    $_.CommandLine -match 'VideoEditFour-20261025/launch_server\.py' -or
    ($_.CommandLine -match 'ComfyUI/main\.py' -and $_.CommandLine -match '--port 8192')
}
$chain | ForEach-Object { "stopping $($_.ProcessId): $($_.CommandLine.Substring(0, [Math]::Min(120, $_.CommandLine.Length)))" }
$chain | Sort-Object ProcessId -Descending | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
$deadline = (Get-Date).AddSeconds(60)
while ((Get-NetTCPConnection -LocalPort 8192 -State Listen -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep 2 }
if (Get-NetTCPConnection -LocalPort 8192 -State Listen -ErrorAction SilentlyContinue) { throw 'port 8192 still listening' }
$cmd = 'D:\AvasthaAI\venv\Scripts\python.exe -u D:/AvasthaAI/VideoEditFour-20261025/launch_server.py'
$r = Invoke-CimMethod Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = 'D:\AvasthaAI\VideoEditFour-20261025' }
"launched pid $($r.ProcessId) (return $($r.ReturnValue))"
$deadline = (Get-Date).AddSeconds(240)
while ((Get-Date) -lt $deadline) {
    try { $q = Invoke-RestMethod "$api/queue" -TimeoutSec 5; "ComfyUI answers: running $($q.queue_running.Count) pending $($q.queue_pending.Count)"; exit 0 } catch { Start-Sleep 5 }
}
throw 'ComfyUI did not answer within 240 s; read comfy.log'
