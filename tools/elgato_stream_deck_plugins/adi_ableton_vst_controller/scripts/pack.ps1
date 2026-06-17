# Build a distributable .streamDeckPlugin with Elgato's DistributionTool (Windows).
#   powershell -ExecutionPolicy Bypass -File scripts\pack.ps1
# Requires DistributionTool.exe on PATH, or pass -Tool C:\path\to\DistributionTool.exe
param([string]$Tool = $env:DISTRIBUTION_TOOL)
$ErrorActionPreference = 'Stop'
$uuid = 'com.adiariel.ableton-vst.sdPlugin'
$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo 'release'
python "$repo\scripts\validate.py"; if ($LASTEXITCODE -ne 0) { throw 'Validation failed.' }
New-Item -ItemType Directory -Force -Path $out | Out-Null
if (-not $Tool) { $Tool = 'DistributionTool' }
$r = Get-Command $Tool -ErrorAction SilentlyContinue
if (-not $r) { Write-Error 'DistributionTool not found. https://docs.elgato.com/streamdeck/sdk/'; exit 1 }
& $r.Source -b -i (Join-Path $repo $uuid) -o $out
Write-Host "Built .streamDeckPlugin into: $out"
