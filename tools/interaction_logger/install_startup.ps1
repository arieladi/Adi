# Adds (or removes) a Startup shortcut so the logger daemon is always listening.
# Run it yourself when you want it; nothing here runs automatically.
#
#   powershell -ExecutionPolicy Bypass -File install_startup.ps1
#   powershell -ExecutionPolicy Bypass -File install_startup.ps1 -Remove

param([switch]$Remove)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$startup = [Environment]::GetFolderPath("Startup")
$link = Join-Path $startup "Interaction Logger.lnk"

if ($Remove) {
    if (Test-Path $link) {
        Remove-Item $link
        Write-Host "removed $link"
    } else {
        Write-Host "nothing to remove"
    }
    exit 0
}

$target = Join-Path $here "start_logger.cmd"
if (-not (Test-Path $target)) { throw "start_logger.cmd not found next to this script" }

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($link)
$shortcut.TargetPath = $target
$shortcut.WorkingDirectory = $here
$shortcut.WindowStyle = 7          # start minimised: the console is captured otherwise
$shortcut.Description = "Hotkey screen + input recorder"
$shortcut.Save()

Write-Host "created $link"
Write-Host "it starts minimised at login; press the hotkey to record."
