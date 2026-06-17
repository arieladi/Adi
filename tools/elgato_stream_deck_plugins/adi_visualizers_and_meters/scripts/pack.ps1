# =============================================================================
# Build a distributable .streamDeckPlugin with Elgato's DistributionTool (Windows).
#
#   powershell -ExecutionPolicy Bypass -File scripts\pack.ps1
#
# Requires DistributionTool.exe on PATH, or pass -Tool C:\path\to\DistributionTool.exe
# Download from https://docs.elgato.com/streamdeck/sdk/ . Output lands in .\release\.
# =============================================================================
param(
  [string]$Tool = $env:DISTRIBUTION_TOOL
)
$ErrorActionPreference = 'Stop'

$uuid = 'com.adi.visualizers-and-meters.sdPlugin'
$repo = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $repo 'release'

python "$repo\scripts\validate.py"
if ($LASTEXITCODE -ne 0) { throw 'Validation failed.' }

New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not $Tool) { $Tool = 'DistributionTool' }
$resolved = Get-Command $Tool -ErrorAction SilentlyContinue
if (-not $resolved) {
  Write-Error "DistributionTool not found. Download from https://docs.elgato.com/streamdeck/sdk/ and add to PATH, or pass -Tool <path>."
  exit 1
}

& $resolved.Source -b -i (Join-Path $repo $uuid) -o $out
Write-Host "Built .streamDeckPlugin into: $out"
