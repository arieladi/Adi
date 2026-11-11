# Installs the AI controller on ADI-PC: scheduled task (at startup, as this account, S4U), request folder the
# Studio user can write, and the two desktop buttons on the Studio desktop. Run as avastha-remote (admin).
if ($env:COMPUTERNAME -ne 'ADI-PC') { throw "wrong PC" }
$ErrorActionPreference = 'Stop'
$C = 'F:\AvasthaAI\control'
New-Item -ItemType Directory -Force "$C\requests" | Out-Null
& icacls "$C\requests" /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null   # Users: modify (flag files)
& icacls $C /grant '*S-1-5-32-545:(OI)(CI)RX' | Out-Null             # Users: read state.json and events.log

$name = 'AvasthaAI Controller'
if (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $name -Confirm:$false
}
$act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -File $C\controller.ps1" -WorkingDirectory $C
$trg = New-ScheduledTaskTrigger -AtStartup
$pri = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\$env:USERNAME" -LogonType S4U -RunLevel Highest
$set = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $name -Action $act -Trigger $trg -Principal $pri -Settings $set | Out-Null
Start-ScheduledTask -TaskName $name

$sh = New-Object -ComObject WScript.Shell
foreach ($b in @(@{ n = 'Stop video AI'; a = 'stop'; i = 'imageres.dll,100' }, @{ n = 'Continue video AI'; a = 'continue'; i = 'imageres.dll,101' })) {
    $l = $sh.CreateShortcut("C:\Users\Studio\Desktop\$($b.n).lnk")
    $l.TargetPath = 'powershell.exe'
    $l.Arguments = "-NoProfile -ExecutionPolicy Bypass -File $C\request.ps1 -Action $($b.a)"
    $l.WorkingDirectory = $C
    $l.IconLocation = "C:\Windows\System32\$($b.i)"
    $l.Save()
}
"installed; task state: $((Get-ScheduledTask -TaskName $name).State)"
