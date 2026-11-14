# What takes the memory during a long render (Adi 2026-11-14: "how with 64 GB did we run out?").
# usage: memlog.ps1 <out.csv> [seconds between samples, default 30]   runs until killed
param([string]$Out, [int]$Every = 30)
"time,commit_gb,commit_limit_gb,ram_in_use_gb,ram_available_gb,pagefile_used_gb,comfy_private_gb,python_other_private_gb,a2000_used_mb,rtx4000_used_mb,gpu_shared_total_gb,top_private" | Set-Content $Out
while ($true) {
    try {
        $c = (Get-Counter '\Memory\Committed Bytes', '\Memory\Commit Limit', '\Memory\Available Bytes', '\Paging File(_Total)\% Usage').CounterSamples
        $os = Get-CimInstance Win32_OperatingSystem
        $total = $os.TotalVisibleMemorySize / 1MB
        $procs = Get-Process | Sort-Object PrivateMemorySize64 -Descending
        $comfyIds = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'ComfyUI/main\.py' } | ForEach-Object { $_.ProcessId })
        $comfy = ($procs | Where-Object { $comfyIds -contains $_.Id } | Measure-Object PrivateMemorySize64 -Sum).Sum / 1GB
        $other = ($procs | Where-Object { $_.Name -eq 'python' -and $comfyIds -notcontains $_.Id } | Measure-Object PrivateMemorySize64 -Sum).Sum / 1GB
        $gpu = @(nvidia-smi --query-gpu=name,memory.used --format=csv,noheader,nounits)
        $a2000 = ($gpu | Where-Object { $_ -match 'A2000' }) -replace '.*,\s*', ''
        $rtx = ($gpu | Where-Object { $_ -match 'RTX 4000' }) -replace '.*,\s*', ''
        $shared = ((Get-Counter '\GPU Adapter Memory(*)\Shared Usage' -ErrorAction SilentlyContinue).CounterSamples | Measure-Object CookedValue -Sum).Sum / 1GB
        $pf = (Get-CimInstance Win32_PageFileUsage | Measure-Object CurrentUsage -Sum).Sum / 1KB
        $top = ($procs | Select-Object -First 4 | ForEach-Object { "$($_.Name):$([math]::Round($_.PrivateMemorySize64/1GB,1))" }) -join ' '
        $avail = $c[2].CookedValue / 1GB
        ('{0:s},{1:F1},{2:F1},{3:F1},{4:F1},{5:F1},{6:F1},{7:F1},{8},{9},{10:F1},{11}' -f (Get-Date), ($c[0].CookedValue / 1GB), ($c[1].CookedValue / 1GB),
            ($total - $avail), $avail, $pf, $comfy, $other, $a2000, $rtx, $shared, $top) | Add-Content $Out
    } catch { "$(Get-Date -Format s),error,$($_.Exception.Message)" | Add-Content $Out }
    Start-Sleep $Every
}
