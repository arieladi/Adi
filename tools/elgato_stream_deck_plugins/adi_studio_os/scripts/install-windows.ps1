<#
    Studio OS installer - Windows.

    Installs the plugin, installs the backend service, registers a scheduled task
    so the service starts at logon, and generates the 36-key + 6-dial profile.

    Nothing is downloaded: the MIDI natives are committed prebuilds (win32-x64
    and win32-arm64) and the service runs on the Stream Deck app's own bundled
    Node, so this is a copy plus two config writes.

    STATUS: written but NOT YET RUN ON WINDOWS. Adi is doing the Windows pass in a
    later session. Two things are known to need attention there and are flagged
    inline: (1) setting the ACTIVE profile - on macOS that is a preferences plist
    key, and the Windows equivalent has not been located yet, so the profile is
    created and you pick it in the app; (2) virtual MIDI - RtMidi cannot create
    ports on Windows, so loopMIDI must supply them.

        .\scripts\install-windows.ps1
        .\scripts\install-windows.ps1 -Yes
        .\scripts\install-windows.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [switch]$Yes,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$Here      = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root      = Split-Path -Parent $Here
$Support   = Join-Path $env:APPDATA 'Elgato\StreamDeck'
$Plugins   = Join-Path $Support 'Plugins'
$PluginSrc = Join-Path $Root 'com.adiariel.studioos.sdPlugin'
$PluginDst = Join-Path $Plugins 'com.adiariel.studioos.sdPlugin'
$ServiceDst= Join-Path $Plugins 'com.adiariel.studioos.service'
$TaskName  = 'StudioOS Service'
$LogDir    = Join-Path $env:LOCALAPPDATA 'StudioOS'

function Say  { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Warn { param($m) Write-Host " !  $m" -ForegroundColor Yellow }
function Die  { param($m) Write-Host " X  $m" -ForegroundColor Red; exit 1 }

function Ask {
    param($m)
    if ($Yes) { return $true }
    $r = Read-Host " ?  $m [y/N]"
    return $r -match '^[Yy]'
}

function Stop-StreamDeck {
    $p = Get-Process -Name 'StreamDeck' -ErrorAction SilentlyContinue
    if ($p) {
        Say 'closing the Stream Deck app (it rewrites its profile store on exit)'
        $p | ForEach-Object { $_.CloseMainWindow() | Out-Null }
        for ($i = 0; $i -lt 30; $i++) {
            Start-Sleep -Milliseconds 500
            if (-not (Get-Process -Name 'StreamDeck' -ErrorAction SilentlyContinue)) { return }
        }
        Die 'the Stream Deck app would not close - close it by hand and re-run'
    }
}

# Prefer the Node the Stream Deck app already ships so nothing has to be installed.
function Find-Node {
    $nodeRoot = Join-Path $Support 'NodeJS'
    if (Test-Path $nodeRoot) {
        $newest = Get-ChildItem $nodeRoot -Directory |
                  Sort-Object { [version]($_.Name -replace '[^0-9.].*$', '') } |
                  Select-Object -Last 1
        if ($newest) {
            $exe = Join-Path $newest.FullName 'node.exe'
            if (Test-Path $exe) { return $exe }
        }
    }
    $sys = Get-Command node -ErrorAction SilentlyContinue
    if ($sys) { return $sys.Source }
    return $null
}

# ------------------------------------------------------------------ uninstall
if ($Uninstall) {
    Say 'uninstalling Studio OS'
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Say 'removed the scheduled task'
    }
    Get-Process -Name 'node' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.CommandLine -like '*studioos*' } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Stop-StreamDeck
    Remove-Item -Recurse -Force $PluginDst, $ServiceDst -ErrorAction SilentlyContinue
    Say 'removed the plugin and service'
    Warn 'the Studio OS profile was left in place - remove it in the Stream Deck app.'
    exit 0
}

# ------------------------------------------------------------------ preflight
if (-not (Test-Path $Support))   { Die 'no Stream Deck data folder - launch the app once first' }
if (-not (Test-Path $PluginSrc)) { Die "plugin source missing at $PluginSrc" }

$NodeBin = Find-Node
if (-not $NodeBin) { Die "no Node runtime found (expected one under $Support\NodeJS)" }
Say "using node: $NodeBin"

Write-Host @"

Studio OS will:
  1. install the plugin   -> $PluginDst
  2. install the service  -> $ServiceDst
  3. register a scheduled task so the service starts at logon  ("$TaskName")
  4. generate a "Studio OS" profile (36 keys + 6 dials)

Nothing is downloaded. The Stream Deck app will be closed and relaunched.

VIRTUAL MIDI ON WINDOWS: RtMidi cannot create virtual ports here, so the service
attaches to existing loopMIDI ports instead. Install loopMIDI and create ports
named exactly:
    Adi RekordBox Controller
    Adi Studio OS MIDI
Set loopMIDI to autostart. Start order matters for rekordbox: Stream Deck first,
then rekordbox - it reads its MIDI device list once at startup.

"@

if (-not (Ask 'proceed?')) { Die 'cancelled' }

# --------------------------------------------------------------------- install
Stop-StreamDeck

Say 'installing plugin'
Remove-Item -Recurse -Force $PluginDst -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Plugins | Out-Null
Copy-Item -Recurse -Force $PluginSrc $PluginDst

Say 'installing service'
Remove-Item -Recurse -Force $ServiceDst -ErrorAction SilentlyContinue
Copy-Item -Recurse -Force (Join-Path $Root 'service') $ServiceDst

if (Ask 'register the service to start at logon?') {
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    $entry  = Join-Path $ServiceDst 'index.js'
    # Redirect through cmd so the service log lands in a file the same way the
    # macOS LaunchAgent captures stderr.
    $action = New-ScheduledTaskAction -Execute 'cmd.exe' `
        -Argument ("/c `"`"$NodeBin`" `"$entry`" >> `"$LogDir\service.log`" 2>&1`"")
    $trigger  = New-ScheduledTaskTrigger -AtLogOn
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
        -ExecutionTimeLimit ([TimeSpan]::Zero) -Hidden
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
        -Settings $settings -Force | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    Say "service registered - logs at $LogDir\service.log"
} else {
    Warn 'skipped. Start it by hand with:'
    Warn "  & `"$NodeBin`" `"$ServiceDst\index.js`""
}

if (Ask 'generate the Studio OS profile?') {
    $py = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $py) {
        Warn 'Python not found - skipping profile generation.'
        Warn 'Create a profile by hand: add "Studio OS Cell" to all 36 keys and'
        Warn '"Studio OS Dial" to all 6 dials.'
    } else {
        Push-Location $Root
        # NOTE (unverified on Windows): make_profile.py writes the profile bundle,
        # but the "make it the active profile" step is macOS-only - it edits a
        # preferences plist that does not exist here. Select "Studio OS" from the
        # profile dropdown in the app after this.
        & $py.Source 'scripts/make_profile.py'
        Pop-Location
        Warn 'select the "Studio OS" profile in the Stream Deck app to activate it.'
    }
}

Say 'relaunching the Stream Deck app'
$exe = Join-Path ${env:ProgramFiles} 'Elgato\StreamDeck\StreamDeck.exe'
if (Test-Path $exe) { Start-Process $exe } else { Warn 'could not find StreamDeck.exe - start it yourself' }

Start-Sleep -Seconds 12
$pluginLog = Join-Path $env:APPDATA 'Elgato\StreamDeck\logs\com.adiariel.studioos0.log'
if (Test-Path $pluginLog) {
    Write-Host ''
    Select-String -Path $pluginLog -Pattern 'surface (COMPLETE|INCOMPLETE)|service (online|offline)' |
        Select-Object -Last 3 | ForEach-Object { $_.Line }
}

Write-Host ''
Say 'done.'
