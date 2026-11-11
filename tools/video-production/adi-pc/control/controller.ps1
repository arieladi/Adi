# AI controller for ADI-PC (Adi 2026-11-11): the desktop "Stop video AI" / "Continue video AI" buttons.
# Runs as avastha-remote (scheduled task "AvasthaAI Controller", at startup), polls F:\AvasthaAI\control\requests.
#  stop     - let the running video take finish (up to 8 min), stop the job, interrupt what is left in ComfyUI,
#             park the interrupted take so it is rendered again, then end ComfyUI, Deforum and every AI job:
#             GPUs and RAM are free for Ableton. What was running is written to stopped.json.
#  continue - start the services and jobs from stopped.json again; the jobs resume where they stopped.
# State for Claude, and later HA: state.json (state, time, GPU memory, services, jobs) and events.log.
if ($env:COMPUTERNAME -ne 'ADI-PC') { throw "wrong PC" }
$ErrorActionPreference = 'Stop'
$C = 'F:\AvasthaAI\control'
$Q = "$C\requests"
$Comfy = 'http://127.0.0.1:8192'
$Services = @(
    @{ name = 'comfy'; match = 'VideoEditFour-20261025/launch_server\.py'; port = 8192
       start = 'D:\AvasthaAI\venv\Scripts\python.exe -u D:/AvasthaAI/VideoEditFour-20261025/launch_server.py'; cwd = 'D:\AvasthaAI\VideoEditFour-20261025' },
    @{ name = 'deforum'; match = 'ActualDeforum/launch_actual\.py'; port = 7865
       start = 'D:\AvasthaAI\venv\Scripts\python.exe -u D:/AvasthaAI/ActualDeforum/launch_actual.py'; cwd = 'D:\AvasthaAI\ActualDeforum' }
)

function Log($m) { Add-Content "$C\events.log" ("{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $m) }
function Gpu {
    @(nvidia-smi --query-gpu=name,memory.used,utilization.gpu --format=csv,noheader 2>$null) | ForEach-Object { $_.Trim() }
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MemPurge {
    [DllImport("ntdll.dll")] static extern int NtSetSystemInformation(int cls, ref int info, int len);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr h, int acc, out IntPtr tok);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool LookupPrivilegeValue(string s, string n, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr tok, bool all, ref TP np, int len, IntPtr p, IntPtr r);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [StructLayout(LayoutKind.Sequential, Pack = 1)] struct TP { public int Count; public long Luid; public int Attr; }
    // Empties the standby list: the file cache of model files the video AI read, which Windows counts as cached.
    public static int PurgeStandby() {
        IntPtr tok; OpenProcessToken(GetCurrentProcess(), 0x28, out tok);
        TP tp = new TP(); tp.Count = 1; tp.Attr = 2;
        LookupPrivilegeValue(null, "SeProfileSingleProcessPrivilege", out tp.Luid);
        AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
        int cmd = 4; return NtSetSystemInformation(80, ref cmd, 4);
    }
}
'@
function Ram {
    # As Task Manager shows it: in use = total - available (free + standby cache).
    $os = Get-CimInstance Win32_OperatingSystem
    $avail = (Get-Counter '\Memory\Available Bytes').CounterSamples[0].CookedValue / 1GB
    $total = $os.TotalVisibleMemorySize / 1MB
    [ordered]@{ total_gb = [math]::Round($total, 1); in_use_gb = [math]::Round($total - $avail, 1)
                free_gb = [math]::Round($os.FreePhysicalMemory / 1MB, 1); available_gb = [math]::Round($avail, 1) }
}
function Save($state, $note, $extra) {
    $s = [ordered]@{ state = $state; time = (Get-Date).ToString('s'); note = $note; gpu = @(Gpu); ram = (Ram) }
    if ($extra) { foreach ($k in $extra.Keys) { $s[$k] = $extra[$k] } }
    $s | ConvertTo-Json -Depth 5 | Set-Content "$C\state.tmp" -Encoding UTF8
    Move-Item "$C\state.tmp" "$C\state.json" -Force
}
function Procs { @(Get-CimInstance Win32_Process) }
function Tree($all, $id) {
    $out = @($id)
    foreach ($p in $all | Where-Object { $_.ParentProcessId -eq $id }) { $out += Tree $all $p.ProcessId }
    $out
}
function Top($all, $p) {
    # The process that started this job: the cmd.exe wrapper when there is one, else the outermost python.
    $byId = @{}; foreach ($x in $all) { $byId[[int]$x.ProcessId] = $x }
    $cur = $p
    while ($true) {
        $par = $byId[[int]$cur.ParentProcessId]
        if (-not $par -or $par.Name -notmatch '^(python|cmd)\.exe$') { return $cur }
        $cur = $par
        if ($cur.Name -eq 'cmd.exe') { return $cur }
    }
}
function Running {
    $all = Procs
    $svc = @(); $svcIds = @()
    foreach ($s in $Services) {
        $roots = $all | Where-Object { $_.CommandLine -match $s.match -and $_.Name -eq 'python.exe' }
        $root = $roots | Where-Object { $p = $_; -not ($roots | Where-Object { $_.ProcessId -eq $p.ParentProcessId }) } | Select-Object -First 1
        if ($root) {
            $top = Top $all $root
            $svc += $s.name; $svcIds += Tree $all $top.ProcessId
        }
    }
    $jobs = @{}
    foreach ($p in $all | Where-Object { $_.Name -eq 'python.exe' -and $_.CommandLine -match 'AvasthaAI' -and $svcIds -notcontains $_.ProcessId }) {
        $t = Top $all $p
        if ($svcIds -notcontains $t.ProcessId) { $jobs[[int]$t.ProcessId] = $t }
    }
    @{ all = $all; services = $svc; serviceIds = $svcIds; jobs = @($jobs.Values) }
}
function ComfyQueue {
    try { Invoke-RestMethod "$Comfy/queue" -TimeoutSec 10 } catch { $null }
}
function RunningPrompt {
    $q = ComfyQueue
    if ($q -and $q.queue_running.Count) { return [string]$q.queue_running[0][1] }
    ''
}
function Park($ids) {
    # A take whose ComfyUI prompt was interrupted is parked: receipt, submit marker and partial output move to
    # interrupted-<time> folders (nothing is deleted), so the resumed job renders that take again.
    $ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    foreach ($r in Get-ChildItem 'F:\AvasthaAI\*\anim\*\receipt.json' -ErrorAction SilentlyContinue) {
        $d = $r.Directory
        if (Test-Path "$($d.FullName)\generated.json") { continue }
        $pid_ = (Get-Content $r.FullName -Raw | ConvertFrom-Json).prompt_id
        if ($ids -notcontains $pid_) { continue }
        $dst = New-Item -ItemType Directory -Force "$($d.FullName)\interrupted-$ts"
        foreach ($f in 'receipt.json', 'submitting.json', 'workflow.json') {
            if (Test-Path "$($d.FullName)\$f") { Move-Item "$($d.FullName)\$f" $dst.FullName }
        }
        foreach ($o in Get-ChildItem 'D:\AvasthaAI\VideoEditFour-20261025\output' -Recurse -Depth 1 -Directory -Filter $d.Name -ErrorAction SilentlyContinue) {
            Rename-Item $o.FullName "$($o.Name).interrupted-$ts"
        }
        Log "parked interrupted take $($d.Name) ($pid_)"
    }
}

function DoStop {
    $r = Running
    if (-not $r.services.Count -and -not $r.jobs.Count) { Save 'stopped' 'nothing was running'; Log 'stop: nothing running'; return }
    $jobs = @($r.jobs | ForEach-Object { @{ cmd = $_.CommandLine } })
    Save 'stopping' 'finishing the current video take (up to 8 min)' @{ services = $r.services; jobs = $jobs }
    Log "stop requested; services: $($r.services -join ','); jobs: $($jobs.Count)"
    # 1. Let the take on the GPU finish: wait until ComfyUI moves to a new prompt or goes idle.
    $first = RunningPrompt
    $deadline = (Get-Date).AddMinutes(8)
    if ($first) {
        while ((Get-Date) -lt $deadline) {
            Start-Sleep 3
            $now = RunningPrompt
            if ($now -ne $first) { break }
        }
    }
    # 2. Stop the jobs, then whatever ComfyUI still runs or has queued.
    foreach ($j in (Running).jobs) { & taskkill /T /F /PID $j.ProcessId 2>$null | Out-Null }
    $q = ComfyQueue
    $left = @()
    if ($q) { $left = @($q.queue_running | ForEach-Object { [string]$_[1] }) + @($q.queue_pending | ForEach-Object { [string]$_[1] }) }
    if ($left.Count) {
        try { Invoke-RestMethod "$Comfy/queue" -Method Post -Body '{"clear": true}' -ContentType 'application/json' | Out-Null } catch {}
        try { Invoke-RestMethod "$Comfy/interrupt" -Method Post | Out-Null } catch {}
        Start-Sleep 5
        Log "interrupted ComfyUI prompts: $($left -join ',')"
    }
    Park $left
    # 3. End the services and anything left, and check the GPUs are free.
    $r2 = Running
    foreach ($id in $r2.serviceIds) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    foreach ($j in $r2.jobs) { & taskkill /T /F /PID $j.ProcessId 2>$null | Out-Null }
    Start-Sleep 5
    $stray = @(Procs | Where-Object { $_.Name -eq 'python.exe' -and $_.CommandLine -match 'AvasthaAI' })
    foreach ($p in $stray) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
    @{ time = (Get-Date).ToString('s'); services = $r.services; jobs = $jobs; interrupted = $left } |
        ConvertTo-Json -Depth 5 | Set-Content "$C\stopped.json" -Encoding UTF8
    Start-Sleep 3
    $rc = [MemPurge]::PurgeStandby()
    $left = @(Procs | Where-Object { $_.Name -eq 'python.exe' -and $_.CommandLine -match 'AvasthaAI' }).Count
    Save 'stopped' 'video AI is off: GPUs and RAM are free' @{ services = $r.services; jobs = $jobs; ai_processes_left = $left; standby_purge_rc = $rc }
    Log "stopped; AI processes left: $left; standby purge rc=$rc; RAM: $((Ram).in_use_gb) GB in use; GPU: $((Gpu) -join ' | ')"
}

function Launch($cmd, $cwd) {
    $a = Invoke-CimMethod Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $cwd }
    Log "started rc=$($a.ReturnValue) pid=$($a.ProcessId): $cmd"
}
function DoContinue {
    if (-not (Test-Path "$C\stopped.json")) { Save 'running' 'continue: nothing was stopped'; Log 'continue: nothing to resume'; return }
    $st = Get-Content "$C\stopped.json" -Raw | ConvertFrom-Json
    Save 'continuing' 'starting the video AI again'
    Log 'continue requested'
    $r = Running
    foreach ($s in $Services | Where-Object { @($st.services) -contains $_.name }) {
        if ($r.services -notcontains $s.name) { Launch $s.start $s.cwd }
    }
    # Jobs need ComfyUI: wait for it (up to 5 min) before resuming them.
    if (@($st.services) -contains 'comfy') {
        $deadline = (Get-Date).AddMinutes(5)
        while ((Get-Date) -lt $deadline -and -not (ComfyQueue)) { Start-Sleep 5 }
        if (-not (ComfyQueue)) { Save 'failed' 'ComfyUI did not come up; jobs not resumed'; Log 'continue failed: ComfyUI not up'; return }
    }
    foreach ($j in @($st.jobs)) { if ($j.cmd) { Launch $j.cmd 'F:\AvasthaAI' } }
    Rename-Item "$C\stopped.json" ("stopped-{0:yyyyMMdd-HHmmss}.json" -f (Get-Date))
    Start-Sleep 20
    $now = Running
    Save 'running' 'video AI is running again' @{ services = $now.services; jobs = @($now.jobs | ForEach-Object { @{ cmd = $_.CommandLine } }) }
    Log "running; services: $($now.services -join ','); jobs: $($now.jobs.Count)"
}

New-Item -ItemType Directory -Force $Q | Out-Null
Log 'controller started'
if (Test-Path "$C\stopped.json") { Save 'stopped' 'video AI is off (stopped before this controller started)' }
else { $r = Running; Save 'running' 'controller started' @{ services = $r.services; jobs = @($r.jobs | ForEach-Object { @{ cmd = $_.CommandLine } }) } }
while ($true) {
    $req = Get-ChildItem $Q -Filter '*.flag' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($req) {
        Get-ChildItem $Q -Filter '*.flag' | Remove-Item -Force
        try {
            if ($req.BaseName -eq 'stop') { DoStop } elseif ($req.BaseName -eq 'continue') { DoContinue }
        } catch { Save 'failed' "$($req.BaseName) failed: $($_.Exception.Message)"; Log "ERROR $($req.BaseName): $($_.Exception.Message)" }
    }
    Start-Sleep 3
}
