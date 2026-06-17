# =============================================================================
# Install on Windows: the Stream Deck plugin AND the AdiVST Ableton Remote Script.
#
#   powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1 -Mode copy
#
# Symlinks need an elevated shell or Developer Mode; falls back to copy.
# Then in Ableton: Settings > Link/Tempo/MIDI > Control Surface > "AdiVST".
# =============================================================================
param([ValidateSet('symlink', 'copy')][string]$Mode = 'symlink')
$ErrorActionPreference = 'Stop'

$uuid = 'com.adiariel.ableton-vst.sdPlugin'
$repo = Split-Path -Parent $PSScriptRoot
$srcPlugin = Join-Path $repo $uuid
$srcRS = Join-Path $repo 'ableton\remote_script\AdiVST'

$sdDir = Join-Path $env:APPDATA 'Elgato\StreamDeck\Plugins'
$rsDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Ableton\User Library\Remote Scripts'

Write-Host 'Validating...'; python "$repo\scripts\validate.py"
if ($LASTEXITCODE -ne 0) { throw 'Validation failed.' }

function Install-Item($src, $dest) {
  if (Test-Path $dest) {
    $it = Get-Item $dest -Force
    if ($it.Attributes -band [IO.FileAttributes]::ReparsePoint) { $it.Delete() } else { Remove-Item -Recurse -Force $dest }
  }
  if ($Mode -eq 'copy') { Copy-Item -Recurse -Force $src $dest }
  else {
    try { New-Item -ItemType SymbolicLink -Path $dest -Target $src | Out-Null }
    catch { Write-Warning "Symlink failed ($($_.Exception.Message)); copying."; Copy-Item -Recurse -Force $src $dest }
  }
}

Write-Host 'Stopping Stream Deck...'
Get-Process 'StreamDeck' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

New-Item -ItemType Directory -Force -Path $sdDir | Out-Null
Write-Host "Installing plugin -> $sdDir\$uuid"; Install-Item $srcPlugin (Join-Path $sdDir $uuid)

New-Item -ItemType Directory -Force -Path $rsDir | Out-Null
Write-Host "Installing Remote Script -> $rsDir\AdiVST"; Install-Item $srcRS (Join-Path $rsDir 'AdiVST')

$exe = Join-Path ${env:ProgramFiles} 'Elgato\StreamDeck\StreamDeck.exe'
if (Test-Path $exe) { Start-Process $exe }

Write-Host ''
Write-Host 'Done. In Ableton Live: Settings > Link/Tempo/MIDI > Control Surface -> "AdiVST" (restart Live first).'
