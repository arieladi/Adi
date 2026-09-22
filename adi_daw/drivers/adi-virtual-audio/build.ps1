<#
.SYNOPSIS
    Builds the ADI virtual audio device for Windows from Microsoft's sysvad sample.

.DESCRIPTION
    Nothing from the sample is kept in this repository. The script fetches
    microsoft/Windows-driver-samples at a PINNED commit into .build/, rewrites the
    INF strings so the endpoints carry our names, builds the EndpointsCommon
    library and the TabletAudioSample driver with the WDK toolset, creates the
    catalogue with Inf2Cat, and collects an UNSIGNED package under out/:

        out/adi-virtual-audio-<platform>-<configuration>/
            TabletAudioSample.sys           the driver
            ComponentizedAudioSample.inf    the stamped INF, with our strings
            adi-virtual-audio.cat           the unsigned catalogue
            LICENSE-MS-PL.txt               the sample's licence (Microsoft Public License)
            PROVENANCE.txt                  repo, commit, configuration, date

    Signing is never done here (ADR-0118, ADR-0119): the package is what a CI
    release workflow hands to SignPath. The sample's licence is MS-PL, which
    OPEN_SOURCE_POLICY.md does not pre-authorise for copying; that is why the
    source is fetched at build time and not vendored (ADR-0120).

.PARAMETER Configuration
    Release (default) or Debug.
.PARAMETER Platform
    x64 only for now.
.PARAMETER SysvadCommit
    The commit of microsoft/Windows-driver-samples to build from. Change it here,
    deliberately, and say so in the log.
.PARAMETER SkipFetch
    Reuse an existing .build/ checkout (local iteration).

.EXAMPLE
    pwsh ./build.ps1 -Configuration Release
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [ValidateSet('x64')] [string]$Platform = 'x64',
    [string]$SysvadRepo = 'https://github.com/microsoft/Windows-driver-samples.git',
    [string]$SysvadCommit = '3c3fb49073c047c4cc8e6c203c6331f62b426507',
    [string]$WorkDir = (Join-Path $PSScriptRoot '.build'),
    [string]$OutDir = (Join-Path $PSScriptRoot 'out'),
    [switch]$SkipFetch
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Run($exe, [string[]]$argv) {
    Write-Host "    $exe $($argv -join ' ')"
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "'$exe' exited with $LASTEXITCODE" }
}

# The INF strings we own. Every key MUST exist exactly once in the sample's .inx,
# or the build fails: a renamed key upstream must be noticed, not silently kept.
# Speaker and MicArray1 are the sample's two always-present internal endpoints;
# the others are jack-detected and stay with the sample's names until the real
# driver exposes exactly two endpoints (ADR-0120).
$InfStrings = [ordered]@{
    'ProviderName'                                = 'ADI DAW'
    'MfgName'                                     = 'ADI DAW'
    'MsCopyRight'                                 = 'Copyright (c) 2026 ADI DAW contributors. Derived from Microsoft sysvad (MS-PL).'
    'SYSVAD_SA.DeviceDesc'                        = 'ADI Virtual Audio Device'
    'SYSVAD_ComponentizedAudioSample.SvcDesc'     = 'ADI Virtual Audio Device Driver'
    'SYSVAD.WaveSpeaker.szPname'                  = 'ADI DAW Stream Output'
    'SYSVAD.TopologySpeaker.szPname'              = 'ADI DAW Stream Output'
    'SYSVAD.WaveMicArray1.szPname'                = 'ADI DAW Stream Input'
    'SYSVAD.TopologyMicArray1.szPname'            = 'ADI DAW Stream Input'
    'MicArray1CustomName'                         = 'ADI DAW Stream Input'
}
$CatalogName = 'adi-virtual-audio.cat'

$src    = Join-Path $WorkDir 'Windows-driver-samples'
$sysvad = Join-Path $src 'audio\sysvad'
$inx    = Join-Path $sysvad 'TabletAudioSample\ComponentizedAudioSample.inx'

# ------------------------------------------------------------------ 1. fetch
Step "sysvad at $SysvadCommit"
if (-not $SkipFetch) {
    if (-not (Test-Path (Join-Path $src '.git'))) {
        New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
        Run git @('clone', '--filter=blob:none', '--no-checkout', '--sparse', $SysvadRepo, $src)
        Run git @('-C', $src, 'sparse-checkout', 'set', 'audio/sysvad')
    }
    Run git @('-C', $src, 'fetch', '--depth', '1', 'origin', $SysvadCommit)
    Run git @('-C', $src, 'checkout', '--force', '--detach', $SysvadCommit)
}
$head = (& git -C $src rev-parse HEAD).Trim()
if ($head -ne $SysvadCommit) { throw "checked out $head, expected $SysvadCommit" }
if (-not (Test-Path $inx)) { throw "sample layout changed: $inx not found" }
$licenseFile = Join-Path $src 'LICENSE'
if (-not (Test-Path $licenseFile)) { throw "the sample's LICENSE is missing from the sparse checkout" }
if ((Get-Content $licenseFile -Raw) -notmatch 'Microsoft Public License') {
    throw "the sample's LICENSE is no longer MS-PL; ADR-0120's licence statement needs revisiting before building"
}

# ------------------------------------------------------------------ 2. overlay
Step "INF strings"
$bytes = [System.IO.File]::ReadAllBytes($inx)
$utf16 = ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE)
$enc = if ($utf16) { [System.Text.Encoding]::Unicode } else { New-Object System.Text.UTF8Encoding($false) }
$text = $enc.GetString($bytes)
foreach ($key in $InfStrings.Keys) {
    $pattern = '(?m)^(' + [regex]::Escape($key) + '\s*=\s*)"[^"\r\n]*"'
    $count = ([regex]::Matches($text, $pattern)).Count
    if ($count -ne 1) { throw "INF key '$key' found $count times in $inx (expected exactly 1)" }
    $text = [regex]::Replace($text, $pattern, ('${1}"' + $InfStrings[$key] + '"'))
    Write-Host ("    {0,-45} = ""{1}""" -f $key, $InfStrings[$key])
}
$catPattern = '(?m)^(CatalogFile\s*=\s*)\S+'
if (([regex]::Matches($text, $catPattern)).Count -ne 1) { throw "CatalogFile line not found exactly once in $inx" }
$text = [regex]::Replace($text, $catPattern, ('${1}' + $CatalogName))
[System.IO.File]::WriteAllBytes($inx, $enc.GetBytes($text))

# ------------------------------------------------------------------ 3. build
Step "MSBuild"
$msbuild = (Get-Command msbuild.exe -ErrorAction SilentlyContinue).Source
if (-not $msbuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    }
}
if (-not $msbuild) { throw "MSBuild not found; install Visual Studio 2022 with the C++ workload and the WDK" }
$common = @('/m', '/nologo', '/v:minimal', "/p:Configuration=$Configuration", "/p:Platform=$Platform", '/p:SignMode=Off')
Run $msbuild (@((Join-Path $sysvad 'EndpointsCommon\EndpointsCommon.vcxproj')) + $common)
Run $msbuild (@((Join-Path $sysvad 'TabletAudioSample\TabletAudioSample.vcxproj')) + $common)

$projOut = Join-Path $sysvad "TabletAudioSample\$Platform\$Configuration"
$sys = Get-ChildItem -Path $projOut -Filter 'TabletAudioSample.sys' -Recurse | Select-Object -First 1
$inf = Get-ChildItem -Path $projOut -Filter 'ComponentizedAudioSample.inf' -Recurse | Select-Object -First 1
if (-not $sys) { throw "TabletAudioSample.sys not produced under $projOut" }
if (-not $inf) { throw "ComponentizedAudioSample.inf not produced under $projOut (StampInf did not run?)" }

# ------------------------------------------------------------------ 4. package + catalogue
Step "package"
$pkg = Join-Path $OutDir "adi-virtual-audio-$Platform-$Configuration"
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Force -Path $pkg | Out-Null
Copy-Item $sys.FullName $pkg
Copy-Item $inf.FullName $pkg
Copy-Item $licenseFile (Join-Path $pkg 'LICENSE-MS-PL.txt')

$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$inf2cat = Get-ChildItem -Path $kits -Filter 'Inf2Cat.exe' -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x86\\Inf2Cat\.exe$' } |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $inf2cat) { throw "Inf2Cat.exe not found under $kits; the WDK is required" }
Run $inf2cat.FullName @("/driver:$pkg", '/os:10_X64', '/verbose')
if (-not (Test-Path (Join-Path $pkg $CatalogName))) { throw "Inf2Cat did not produce $CatalogName" }

$ourCommit = try { (& git -C $PSScriptRoot rev-parse HEAD 2>$null).Trim() } catch { 'unknown' }
@"
ADI virtual audio device -- UNSIGNED build package
Built:            $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC
Configuration:    $Configuration / $Platform
Driver source:    $SysvadRepo @ $SysvadCommit (audio/sysvad; licence: MS-PL, see LICENSE-MS-PL.txt)
Build script:     adi_daw/drivers/adi-virtual-audio/build.ps1 @ $ourCommit (github.com/arieladi/Adi)
Endpoints:        "ADI DAW Stream Output" (render), "ADI DAW Stream Input" (capture)
Signing:          none. See adi_daw/drivers/SIGNING.md.
"@ | Set-Content -Path (Join-Path $pkg 'PROVENANCE.txt') -Encoding utf8

Step "done"
Get-ChildItem $pkg | ForEach-Object { Write-Host ("    {0,-32} {1,10:N0} bytes" -f $_.Name, $_.Length) }
Write-Host "    -> $pkg"
