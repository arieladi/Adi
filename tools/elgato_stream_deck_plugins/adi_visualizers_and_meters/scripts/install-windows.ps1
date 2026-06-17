# =============================================================================
# Install the plugin into Stream Deck on Windows for local development.
#
#   powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1 -Mode copy
#
# Symlinking needs either an elevated shell or Windows "Developer Mode" enabled;
# the script falls back to copying automatically if the link can't be created.
# =============================================================================
param(
  [ValidateSet('symlink', 'copy')]
  [string]$Mode = 'symlink'
)
$ErrorActionPreference = 'Stop'

$uuid = 'com.adi.visualizers-and-meters.sdPlugin'
$repo = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $repo $uuid
$dir  = Join-Path $env:APPDATA 'Elgato\StreamDeck\Plugins'
$dest = Join-Path $dir $uuid

if (-not (Test-Path $src)) { throw "Plugin folder not found: $src" }

Write-Host 'Validating...'
python "$repo\scripts\validate.py"
if ($LASTEXITCODE -ne 0) { throw 'Validation failed.' }

New-Item -ItemType Directory -Force -Path $dir | Out-Null

# Remove any existing install. For a directory symlink, delete the link only
# (never recurse into the target).
if (Test-Path $dest) {
  $item = Get-Item $dest -Force
  if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { $item.Delete() }
  else { Remove-Item -Recurse -Force $dest }
}

Write-Host 'Stopping Stream Deck...'
Get-Process 'StreamDeck' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

if ($Mode -eq 'copy') {
  Write-Host "Copying -> $dest"
  Copy-Item -Recurse -Force $src $dest
}
else {
  Write-Host "Symlinking -> $dest"
  try {
    New-Item -ItemType SymbolicLink -Path $dest -Target $src | Out-Null
  }
  catch {
    Write-Warning "Symlink failed ($($_.Exception.Message)). Copying instead."
    Copy-Item -Recurse -Force $src $dest
  }
}

$exe = Join-Path ${env:ProgramFiles} 'Elgato\StreamDeck\StreamDeck.exe'
if (Test-Path $exe) { Write-Host 'Relaunching Stream Deck...'; Start-Process $exe }
else { Write-Host 'Start Stream Deck manually to load the plugin.' }

Write-Host 'Done.'
