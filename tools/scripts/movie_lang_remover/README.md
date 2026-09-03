# MOVIE LANG REMOVER

Strips every non-English audio and subtitle track out of a Plex library, so a
file that shipped with 20 Russian dubs keeps only the English one.

Lossless — the video is remuxed bit-for-bit, never re-encoded. On my own
library it reclaimed **266 GB** out of a 124-file library, with zero quality
loss:

| Area | Files cleaned | Before | After | Saved |
|------|---------------|--------|-------|-------|
| Movies | 12 | 903 GB | 668 GB | **235 GB** |
| Spartacus | 39 | 234 GB | 215 GB | **20 GB** |
| Game of Thrones | 62 | 556 GB | 545 GB | **11 GB** |

Most of the win is in movies — remuxes carry many dub tracks, while a 2160p
WEB-DL episode is ~98% video and barely shrinks.

## What it keeps

- **All video**, plus chapters, attachments and the original file timestamp
- **The main English audio.** Preferred pick is English, not commentary, ≥6
  channels. Where a release ships a lossless 7.1 track *and* an AC-3 5.1
  downmix, both are kept — the AC-3 is what lets weak Plex clients direct-play
  instead of forcing a server-side transcode.
- **Every English subtitle track** (Full, SDH, Forced, …)

## What it removes

- Every non-English audio and subtitle track
- Commentary and audio-description tracks, matched on track name
  (`comment`, `director`, `cast`, `crew`, `design team`, `production`,
  `audio descri`, `description`)

The first surviving audio track is re-flagged as **default**, because releases
with foreign dubs usually default to the dub rather than to English.

## Safety

The original is only deleted once its replacement has been verified. Each file
is remuxed to `<name>.__tmp__.mkv`, then checked for: parseable output, the
expected audio track count, at least one video track, non-trivial size, and a
duration within 1% of the source. Any failure leaves the original untouched.

Three cases are handled explicitly:

- **No English audio at all** → file skipped entirely, never modified. Nothing
  can end up mute.
- **Already English-only** → skipped, so re-running over a whole library is
  cheap and safe.
- **File locked** (Plex analysing a fresh import) → retries the swap for up to
  10 minutes, then keeps the original and leaves the cleaned copy as
  `.__tmp__.mkv` rather than losing either.

> **Replacement is in-place and irreversible.** Removed tracks are gone.
> Run `-DryRun` first on anything you're unsure about.

## Requirements

**[MKVToolNix](https://mkvtoolnix.download/downloads.html)** — required, does
the actual remux. **[ffmpeg](https://www.gyan.dev/ffmpeg/builds/)** — optional,
only used to estimate per-language sizes in the report.

Both are found automatically if they're on `PATH`, in `%ProgramFiles%`, or in
`D:\_tools\`. Otherwise point at them directly:

```powershell
.\clean.ps1 -MkvMerge "C:\Program Files\MKVToolNix\mkvmerge.exe" -FFprobe "C:\ffmpeg\bin\ffprobe.exe"
```

### Portable install, no admin

```powershell
# MKVToolNix (portable .7z - needs 7zr.exe, also a plain download)
Invoke-WebRequest "https://www.7-zip.org/a/7zr.exe" -OutFile "D:\_tools\7zr.exe"
Invoke-WebRequest "https://mkvtoolnix.download/windows/releases/101.0/mkvtoolnix-64-bit-101.0.7z" -OutFile "D:\_tools\mkvtoolnix.7z"
& "D:\_tools\7zr.exe" x "D:\_tools\mkvtoolnix.7z" "-oD:\_tools\mkv" -y

# ffmpeg (zip, extracts natively)
Invoke-WebRequest "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip" -OutFile "D:\_tools\ffmpeg.zip"
Expand-Archive "D:\_tools\ffmpeg.zip" -DestinationPath "D:\_tools\ff" -Force
Copy-Item (Join-Path (Get-ChildItem "D:\_tools\ff" -Recurse -Filter ffmpeg.exe | Select-Object -First 1).Directory.FullName "*.exe") "D:\_tools"
```

## Usage

Double-click **`run_clean.bat`**, or:

```powershell
.\clean.ps1                          # clean D:\PLEX
.\clean.ps1 -DryRun                  # show what would change, touch nothing
.\clean.ps1 -Root "E:\Media"         # different library
.\clean.ps1 -Filter "Interstellar"   # only paths containing this
.\clean.ps1 -Limit 3                 # stop after 3 files
```

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `-Root` | `D:\PLEX` | Folder scanned recursively for `.mkv` |
| `-DryRun` | off | Report only, change nothing |
| `-Filter` | — | Substring match on full path |
| `-Limit` | `0` | Stop after N files (0 = all) |
| `-MkvMerge` / `-FFprobe` | auto | Explicit tool paths |
| `-ReportDir` | `%LOCALAPPDATA%\movie_lang_remover` | Where CSVs are written |

`run_clean.bat` forwards its arguments, so `run_clean.bat -DryRun` works too.

## Output

```
[7/124] PULP FICTION (1994).mkv
    saved 38.37 GB   (115.00 GB -> 76.63 GB)   removed audio: rus x20 | subs: rus x2

==================== CLEANUP REPORT ====================

--- Files cleaned (largest saving first) ---
    56.60 GB  The Lord of the Rings The Return of the King.mkv
              183.69 GB -> 127.09 GB | removed audio: rus x8, eng x4 | subs: rus x3

--- Tracks removed by language ---
  Lang     Audio    Subs  Approx size
  rus        212     347     227.60 GB
  ukr          4       6       2.70 GB
  TOTAL      229     368

--- Totals ---
  Files cleaned : 112
  Skipped       : 11  (already English-only)
  Failed        : 0
  SPACE SAVED   : 243.20 GB
```

Two CSVs land in `-ReportDir`: `clean-log.csv` (per file — before/after bytes,
what was removed, status) and `removed-tracks.csv` (every individual track
removed, with codec and original track name).

## Caveats

- **English is assumed to be the language you want.** A film whose original
  audio is French or Japanese would be stripped down to its English dub. Run
  `-DryRun` on non-English-language titles.
- **Commentaries are dropped.** To keep them, delete the `track_name -notmatch
  $commRx` clause from the `$keepA` filter in `clean.ps1`.
- Only `.mkv` is touched. `.mp4` / `.avi` are ignored.

## Files

- `clean.ps1` — the script; scan, select, remux, verify, swap, report
- `run_clean.bat` — double-click launcher, forwards arguments
