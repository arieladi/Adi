<#
.SYNOPSIS
  Strips every non-English audio and subtitle track from MKV files, losslessly.

.DESCRIPTION
  Walks a media library, and for each .mkv keeps the English audio and English
  subtitles and drops everything else. Video is remuxed bit-for-bit (no
  re-encoding, no quality loss). Chapters, attachments and file timestamps are
  preserved.

  Safety: each file is remuxed to a temp file and verified (track counts, video
  present, duration within 1%) BEFORE the original is replaced. A file that has
  no English audio at all is left completely untouched.

.PARAMETER Root
  Folder to scan recursively. Default D:\PLEX

.PARAMETER DryRun
  Report what would be removed without changing anything.

.PARAMETER Filter
  Only process files whose full path contains this substring.

.PARAMETER Limit
  Stop after this many files (0 = no limit).

.EXAMPLE
  .\clean.ps1 -DryRun
.EXAMPLE
  .\clean.ps1 -Root "E:\Media" -Filter "Interstellar"
#>
param(
  [string]$Root      = "D:\PLEX",
  [switch]$DryRun,
  [string]$Filter    = "",
  [int]   $Limit     = 0,
  [string]$MkvMerge  = "",
  [string]$FFprobe   = "",
  [string]$ReportDir = ""
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- tool lookup
function Find-Tool {
  param([string]$Explicit, [string]$Exe, [string[]]$Candidates)
  if ($Explicit) {
    if (Test-Path $Explicit) { return $Explicit }
    Write-Host "ERROR: $Exe not found at the path you gave: $Explicit" -ForegroundColor Red
    exit 1
  }
  $c = Get-Command $Exe -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($p in $Candidates) { if (Test-Path $p) { return $p } }
  return $null
}

$mkvmerge = Find-Tool -Explicit $MkvMerge -Exe 'mkvmerge.exe' -Candidates @(
  "$env:ProgramFiles\MKVToolNix\mkvmerge.exe"
  "${env:ProgramFiles(x86)}\MKVToolNix\mkvmerge.exe"
  "D:\_tools\mkv\mkvtoolnix\mkvmerge.exe"
  "$PSScriptRoot\tools\mkvtoolnix\mkvmerge.exe"
)
$ffprobe = Find-Tool -Explicit $FFprobe -Exe 'ffprobe.exe' -Candidates @(
  "$env:ProgramFiles\ffmpeg\bin\ffprobe.exe"
  "D:\_tools\ffprobe.exe"
  "$PSScriptRoot\tools\ffprobe.exe"
)

if (-not $mkvmerge) {
  Write-Host "ERROR: mkvmerge.exe not found." -ForegroundColor Red
  Write-Host "  Install MKVToolNix, or pass -MkvMerge <path>. See README.md."
  exit 1
}
if (-not $ffprobe) {
  Write-Host "NOTE: ffprobe.exe not found - per-language size estimates will show 0." -ForegroundColor Yellow
  Write-Host "      Track removal itself is unaffected. Pass -FFprobe <path> to enable them."
}
if (-not (Test-Path $Root)) { Write-Host "ERROR: Root folder not found: $Root" -ForegroundColor Red; exit 1 }

if (-not $ReportDir) { $ReportDir = Join-Path $env:LOCALAPPDATA "movie_lang_remover" }
if (-not (Test-Path $ReportDir)) { New-Item -ItemType Directory $ReportDir -Force | Out-Null }

# Audio tracks whose name matches this are treated as extras, not the main track.
$commRx = '(?i)comment|director|cast|crew|design team|production|audio descri|^description$'

function Fmt([double]$b) {
  if ($b -ge 1GB)     { return ("{0:N2} GB" -f ($b / 1GB)) }
  elseif ($b -ge 1MB) { return ("{0:N0} MB" -f ($b / 1MB)) }
  elseif ($b -gt 0)   { return ("{0:N0} KB" -f ($b / 1KB)) }
  else                { return "0" }
}

Write-Host "mkvmerge : $mkvmerge"
if ($ffprobe) { Write-Host "ffprobe  : $ffprobe" }
Write-Host "scanning : $Root"
Write-Host ""

$files = Get-ChildItem -Path $Root -Recurse -Filter *.mkv -File | Sort-Object FullName
if ($Filter) { $files = $files | Where-Object { $_.FullName -like "*$Filter*" } }
if ($Limit -gt 0) { $files = $files | Select-Object -First $Limit }

$log      = New-Object System.Collections.ArrayList
$detail   = New-Object System.Collections.ArrayList
$report   = New-Object System.Collections.ArrayList
$langStat = @{}
$n = 0; $skipped = 0; $done = 0; $failed = 0; $noEng = 0
$saved = 0L; $totBefore = 0L; $totAfter = 0L

foreach ($f in $files) {
  $n++
  $tag = "[$n/$($files.Count)] $($f.Name)"
  try { $info = (& $mkvmerge -J "$($f.FullName)" | Out-String) | ConvertFrom-Json }
  catch { Write-Host "$tag -> IDENTIFY FAILED"; $failed++; continue }

  $audio = @($info.tracks | Where-Object { $_.type -eq 'audio' })
  $subs  = @($info.tracks | Where-Object { $_.type -eq 'subtitles' })

  # Preferred: real English audio (not commentary) with >= 6 channels.
  # Falls back progressively so a file is never left without audio.
  $keepA = @($audio | Where-Object {
      $_.properties.language -eq 'eng' -and
      $_.properties.track_name -notmatch $commRx -and
      [int]$_.properties.audio_channels -ge 6 })
  if ($keepA.Count -eq 0) { $keepA = @($audio | Where-Object { $_.properties.language -eq 'eng' -and $_.properties.track_name -notmatch $commRx }) }
  if ($keepA.Count -eq 0) { $keepA = @($audio | Where-Object { $_.properties.language -eq 'eng' }) }
  if ($keepA.Count -eq 0) { Write-Host "$tag -> NO ENGLISH AUDIO, SKIPPING"; $noEng++; continue }
  $keepS = @($subs | Where-Object { $_.properties.language -eq 'eng' })

  $keepAIds = @($keepA | ForEach-Object { $_.id })
  $keepSIds = @($keepS | ForEach-Object { $_.id })
  $remA = @($audio | Where-Object { $keepAIds -notcontains $_.id })
  $remS = @($subs  | Where-Object { $keepSIds -notcontains $_.id })
  if ($remA.Count -eq 0 -and $remS.Count -eq 0) { Write-Host "$tag -> already clean"; $skipped++; continue }

  # Approximate per-track sizes, read from Matroska statistics tags when present.
  $bytesById = @{}
  if ($ffprobe) {
    try {
      $pd = (& $ffprobe -v quiet -print_format json -show_streams "$($f.FullName)" | Out-String) | ConvertFrom-Json
      foreach ($st in $pd.streams) {
        $bv = $null
        foreach ($p in $st.tags.PSObject.Properties) {
          if ($p.Name -like 'NUMBER_OF_BYTES*') { $bv = [int64]$p.Value; break }
        }
        if ($null -ne $bv) { $bytesById[[int]$st.index] = $bv }
      }
    } catch { }
  }

  foreach ($tr in ($remA + $remS)) {
    $lg = $tr.properties.language
    if (-not $lg) { $lg = 'und' }
    $bv = 0L
    if ($bytesById.ContainsKey([int]$tr.id)) { $bv = $bytesById[[int]$tr.id] }
    if (-not $langStat.ContainsKey($lg)) { $langStat[$lg] = @{ Audio = 0; Subs = 0; Bytes = 0L } }
    if ($tr.type -eq 'audio') { $langStat[$lg].Audio++ } else { $langStat[$lg].Subs++ }
    $langStat[$lg].Bytes += $bv
    $null = $detail.Add([PSCustomObject]@{
      File = $f.Name; Type = $tr.type; Lang = $lg; Codec = $tr.codec
      Name = $tr.properties.track_name; ApproxBytes = $bv })
  }

  $desc = @()
  if ($remA.Count) { $desc += "audio: " + (($remA | Group-Object { $_.properties.language } | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', ') }
  if ($remS.Count) { $desc += "subs: "  + (($remS | Group-Object { $_.properties.language } | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', ') }
  $descTxt = $desc -join ' | '

  if ($DryRun) {
    Write-Host "$tag"
    Write-Host "    would remove -> $descTxt"
    continue
  }

  $tmp = Join-Path $f.DirectoryName ($f.BaseName + ".__tmp__.mkv")
  if (Test-Path $tmp) { Remove-Item $tmp -Force }
  $before = $f.Length
  $stamp  = $f.LastWriteTime

  $margs = @('-o', $tmp, '--audio-tracks', ($keepAIds -join ','))
  if ($keepS.Count -gt 0) { $margs += @('--subtitle-tracks', ($keepSIds -join ',')) } else { $margs += '-S' }
  for ($k = 0; $k -lt $keepA.Count; $k++) {
    $v = '0'; if ($k -eq 0) { $v = '1' }   # first kept audio becomes default
    $margs += @('--default-track-flag', "$($keepA[$k].id):$v")
  }
  $margs += "$($f.FullName)"

  & $mkvmerge @margs > $null 2>&1
  $code = $LASTEXITCODE
  if ($code -gt 1 -or -not (Test-Path $tmp)) {
    Write-Host "$tag -> MKVMERGE FAILED (exit $code)"
    if (Test-Path $tmp) { Remove-Item $tmp -Force }
    $failed++
    $null = $log.Add([PSCustomObject]@{ File = $f.FullName; Before = $before; After = ''; Saved = ''; Removed = $descTxt; Status = "FAIL exit=$code" })
    continue
  }

  # ---- verify the new file before destroying the original ----
  try { $vi = (& $mkvmerge -J "$tmp" | Out-String) | ConvertFrom-Json } catch { $vi = $null }
  $vA = @($vi.tracks | Where-Object { $_.type -eq 'audio' }).Count
  $vV = @($vi.tracks | Where-Object { $_.type -eq 'video' }).Count
  $after = (Get-Item $tmp).Length
  $okDur = $true
  if ($info.container.properties.duration -and $vi.container.properties.duration) {
    $d1 = [double]$info.container.properties.duration
    $d2 = [double]$vi.container.properties.duration
    if ($d1 -gt 0) { $okDur = ([math]::Abs($d1 - $d2) / $d1) -lt 0.01 }
  }
  if (-not $vi -or $vA -ne $keepA.Count -or $vV -lt 1 -or $after -lt 1MB -or -not $okDur) {
    Write-Host "$tag -> VERIFY FAILED (audio $vA/$($keepA.Count) video $vV dur_ok=$okDur) - original kept"
    Remove-Item $tmp -Force
    $failed++
    $null = $log.Add([PSCustomObject]@{ File = $f.FullName; Before = $before; After = $after; Saved = ''; Removed = $descTxt; Status = 'VERIFY_FAIL' })
    continue
  }

  # ---- swap; Plex often holds a lock on a freshly added file ----
  $swapped = $false
  for ($r = 1; $r -le 60; $r++) {
    try { Remove-Item $f.FullName -Force -ErrorAction Stop; $swapped = $true; break }
    catch { if ($r -eq 1) { Write-Host "$tag -> locked (Plex?), waiting..." }; Start-Sleep -Seconds 10 }
  }
  if (-not $swapped) {
    Write-Host "$tag -> LOCKED after 10 min - original kept, cleaned file left as .__tmp__.mkv"
    $failed++
    $null = $log.Add([PSCustomObject]@{ File = $f.FullName; Before = $before; After = $after; Saved = ''; Removed = $descTxt; Status = 'LOCKED' })
    continue
  }
  Rename-Item $tmp $f.Name
  $final = Join-Path $f.DirectoryName $f.Name
  (Get-Item $final).LastWriteTime = $stamp

  $diff = $before - $after
  $saved += $diff; $totBefore += $before; $totAfter += $after; $done++
  Write-Host "$tag"
  Write-Host ("    saved {0}   ({1} -> {2})   removed {3}" -f (Fmt $diff), (Fmt $before), (Fmt $after), $descTxt)
  $null = $log.Add([PSCustomObject]@{ File = $f.FullName; Before = $before; After = $after; Saved = $diff; Removed = $descTxt; Status = 'OK' })
  $null = $report.Add([PSCustomObject]@{ Name = $f.Name; SavedRaw = $diff; SavedTxt = (Fmt $diff); BeforeTxt = (Fmt $before); AfterTxt = (Fmt $after); Removed = $descTxt })
}

if (-not $DryRun) {
  $log    | Export-Csv (Join-Path $ReportDir 'clean-log.csv')      -NoTypeInformation -Encoding UTF8
  $detail | Export-Csv (Join-Path $ReportDir 'removed-tracks.csv') -NoTypeInformation -Encoding UTF8
}

Write-Host ""
Write-Host "==================== CLEANUP REPORT ===================="

if ($report.Count -gt 0) {
  Write-Host ""
  Write-Host "--- Files cleaned (largest saving first) ---"
  foreach ($r in ($report | Sort-Object SavedRaw -Descending)) {
    Write-Host ("  {0,10}  {1}" -f $r.SavedTxt, $r.Name)
    Write-Host ("              {0} -> {1} | removed {2}" -f $r.BeforeTxt, $r.AfterTxt, $r.Removed)
  }
}

if ($langStat.Count -gt 0) {
  Write-Host ""
  Write-Host "--- Tracks removed by language ---"
  Write-Host ("  {0,-6} {1,7} {2,7} {3,12}" -f 'Lang', 'Audio', 'Subs', 'Approx size')
  foreach ($e in ($langStat.GetEnumerator() | Sort-Object { $_.Value.Bytes } -Descending)) {
    Write-Host ("  {0,-6} {1,7} {2,7} {3,12}" -f $e.Key, $e.Value.Audio, $e.Value.Subs, (Fmt $e.Value.Bytes))
  }
  $ta = ($langStat.Values | ForEach-Object { $_.Audio } | Measure-Object -Sum).Sum
  $ts = ($langStat.Values | ForEach-Object { $_.Subs }  | Measure-Object -Sum).Sum
  Write-Host ("  {0,-6} {1,7} {2,7}" -f 'TOTAL', $ta, $ts)
}

Write-Host ""
Write-Host "--- Totals ---"
if ($DryRun) {
  Write-Host "  DRY RUN - nothing was changed."
  Write-Host ("  Files that would be cleaned : {0}" -f ($n - $skipped - $failed - $noEng))
  Write-Host ("  Already English-only        : {0}" -f $skipped)
  if ($noEng -gt 0) { Write-Host ("  No English audio (skipped)  : {0}" -f $noEng) }
} else {
  Write-Host ("  Files cleaned : {0}" -f $done)
  Write-Host ("  Skipped       : {0}  (already English-only)" -f $skipped)
  if ($noEng -gt 0) { Write-Host ("  No Eng audio  : {0}  (left untouched)" -f $noEng) }
  Write-Host ("  Failed        : {0}" -f $failed)
  Write-Host ("  Size before   : {0}" -f (Fmt $totBefore))
  Write-Host ("  Size after    : {0}" -f (Fmt $totAfter))
  Write-Host ("  SPACE SAVED   : {0}" -f (Fmt $saved))
  try {
    $drive = Get-PSDrive ((Split-Path -Qualifier $Root).TrimEnd(':')) -ErrorAction Stop
    Write-Host ("  Free on {0}:   {1}" -f $drive.Name, (Fmt $drive.Free))
  } catch { }
  Write-Host ""
  Write-Host "  Details: $ReportDir"
}
Write-Host "========================================================"
