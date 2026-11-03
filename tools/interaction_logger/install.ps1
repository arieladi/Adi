# One-time setup on the machine you record on:
#   - private .venv next to the scripts, with requirements.txt
#   - config.local.json pointing sessions at -LogDir (keep it off OneDrive and off a small C:)
#   - the ADI_Logger remote script copied into Live's User Library
#   - desktop shortcuts: ADI Log - Start Simple / Start Deep / Stop
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 -LogDir E:\InteractionLogs
#
# Re-run it after updating the scripts; it only replaces what it owns.

param(
    [string]$LogDir,
    [string]$UserLibrary,
    [string]$Python,
    [switch]$NoDesktop
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Json($object, $path) {
    # Set-Content -Encoding utf8 adds a BOM on Windows PowerShell 5.1
    [IO.File]::WriteAllText($path, ($object | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
}

function Find-Python {
    if ($Python) { return $Python }
    $onPath = Get-Command python -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notlike "*\WindowsApps\*" } | Select-Object -First 1
    if ($onPath) { return $onPath.Source }
    $local = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($local) { return $local.FullName }
    throw "No Python found. Install it (winget install Python.Python.3.14 --scope user) or pass -Python."
}

# ---------------------------------------------------------------- venv
$venvPython = Join-Path $here ".venv\Scripts\python.exe"
if (-not (Test-Path $venvPython)) {
    $base = Find-Python
    Write-Host "creating .venv with $base"
    & $base -m venv (Join-Path $here ".venv")
    if ($LASTEXITCODE) { throw "venv creation failed" }
}
& $venvPython -m pip install --quiet --upgrade pip
& $venvPython -m pip install --quiet -r (Join-Path $here "requirements.txt")
if ($LASTEXITCODE) { throw "pip install failed" }

# ---------------------------------------------------------------- config
$localConfig = Join-Path $here "config.local.json"
if ($LogDir) {
    New-Item -ItemType Directory -Force $LogDir | Out-Null
    Write-Json @{ base_output_dir = $LogDir } $localConfig
    Write-Host "sessions -> $LogDir"
}
$effectiveLogDir = (& $venvPython -c "import sys; sys.path.insert(0, r'$here'); from logger_daemon import load_settings; from pathlib import Path; print(Path(load_settings([])['base_output_dir']).expanduser())").Trim()
# ADI_Logger only heartbeats into an existing control dir; make it now so the
# first recording already sees Live as connected.
New-Item -ItemType Directory -Force (Join-Path $effectiveLogDir ".live") | Out-Null
if ($effectiveLogDir -match "OneDrive") {
    Write-Warning "sessions would be written inside OneDrive ($effectiveLogDir) and uploaded. Re-run with -LogDir."
}

# ---------------------------------------------------------------- Live remote script
if (-not $UserLibrary) {
    $UserLibrary = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Ableton\User Library"
}
if (Test-Path $UserLibrary) {
    $dest = Join-Path $UserLibrary "Remote Scripts\ADI_Logger"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item (Join-Path $here "live_remote_script\ADI_Logger\*.py") $dest -Force
    Remove-Item (Join-Path $dest "__pycache__") -Recurse -Force -ErrorAction SilentlyContinue
    Write-Json @{ control_dir = (Join-Path $effectiveLogDir ".live") } (Join-Path $dest "settings.json")
    Write-Host "Live remote script -> $dest"
} else {
    Write-Warning "Live User Library not found at $UserLibrary; pass -UserLibrary to install ADI_Logger"
}

# ---------------------------------------------------------------- desktop
if (-not $NoDesktop) {
    $iconDir = Join-Path $here "icons"
    & $venvPython (Join-Path $here "make_icons.py") $iconDir
    $desktop = [Environment]::GetFolderPath("Desktop")
    $shell = New-Object -ComObject WScript.Shell
    $ctl = Join-Path $here "logger_ctl.py"
    $links = @(
        @{ Name = "ADI Log - Start Simple"; Args = "start simple"; Icon = "simple.ico";
           Desc = "Long, light recording while you work in Live. Asks what you are about to do." },
        @{ Name = "ADI Log - Start Deep"; Args = "start deep"; Icon = "deep.ico";
           Desc = "Full-detail recording of one task. Asks what you are about to do." },
        @{ Name = "ADI Log - Stop"; Args = "stop"; Icon = "stop.ico";
           Desc = "Stop and save any running ADI log." }
    )
    foreach ($l in $links) {
        $lnk = $shell.CreateShortcut((Join-Path $desktop "$($l.Name).lnk"))
        $lnk.TargetPath = $venvPython
        $lnk.Arguments = "`"$ctl`" $($l.Args)"
        $lnk.WorkingDirectory = $here
        $lnk.IconLocation = (Join-Path $iconDir $l.Icon)
        $lnk.Description = $l.Desc
        $lnk.Save()
        Write-Host "desktop -> $($l.Name)"
    }
}

Write-Host ""
Write-Host "done. In Live: Preferences > Link, Tempo & MIDI > Control Surface: ADI_Logger (Input/Output: None)."
Write-Host "Restart Live first if it was open while this ran."
