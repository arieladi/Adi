# Settings — the store behind the Settings window

The core library every Settings page reads and writes: `src/adi/settings/**`,
tested by `tests/test_settings.cpp`. It has no UI. The rulings are ADR-0125
(R-01 to R-07, d1 to d3), ADR-0127 d5, ADR-0145, ADR-0149 and, for the
defaults, ADR-0203 (§8). The decisions this library adds are ADR-0152.

## 1. The registry (`registry.hpp`)

One typed table of every setting:

- **key**, e.g. `audio.bufferSize`. Permanent, like an op name.
- **type**: bool, int, real, text, choice, path or path list.
- **scope** (R-03 badge): App, Project or Device.
- **page**: the left-hand list (R-01).
- **label** and **help**, which the Find box searches (R-02). Every word of the
  query must match somewhere in the label, help, key or page, ignoring case.
- **default**, the legal **choices** or **range**, the **op** for a Project
  setting, and whether the **agent** may change it.

| Scope | Where the value lives | Who writes it |
|---|---|---|
| App | `settings.json` of this application (§2) | the store |
| Project | a row in the `.adi` | only its op, named in the registry (OPS.md §9); the store refuses it and names the op |
| Device | the device's own state | the device |

A setting that is not in the registry does not exist: the store refuses an
unknown key from a caller.

### The settings of ADR-0145

| Key | Page | Default | Ruling |
|---|---|---|---|
| `lookfeel.zoomOnSelection` | Look & Feel | on | d2 as amended by ADR-0203: Ctrl/Cmd + wheel zooms around the selection, as Live does; off zooms around the cursor |
| `audio.silentResampling` | Audio | off | d3: resample without the mismatch bar |
| `audio.bufferSize` | Audio | 512 | d5: 64, 128, 256, 512, 1024, 2048 or 4096. No 32 and no automatic switching |
| `audio.driverType` | Audio | system default | d5: ASIO is one choice, opened through JUCE's wrapper. No raw bypass is offered |
| `audio.linuxBackend` | Audio | PipeWire/JACK | d4: ALSA or PipeWire/JACK |
| `audio.jackTransportSync` | Audio | off | d4 |
| `plugins.clapFolders`, `plugins.lv2Folders` | Plug-ins | none | d4: custom folders on Linux |

ADR-0145 names the sizes offered, not the default. The default is 512, a
ruling (ADR-0203); it was 256 before. The tracking sizes (64 and 128) are two
clicks away.

## 2. The file (`store.hpp`)

```
<appdata::pathsFor(app).config>/settings.json
{"schema": 1, "app": "ADI", "values": {"audio.bufferSize": 512, ...}}
```

- **One file per application.** ADI and ADiJ each have their own
  folder (ADR-0149), and the file names its application. A file naming another
  application is refused and never overwritten (ADR-0145 d10).
- **Unknown keys survive.** A key a newer build wrote is kept and written back
  exactly. A newer schema number is never lowered.
- **A corrupt file is kept.** It moves aside to `settings.json.corrupt-N`, the
  application starts from defaults, and the next save writes a fresh file.
  Nothing is overwritten silently.
- **Saves are atomic:** a temporary file beside it, then a rename.
- **A stored value this build finds illegal** (a 32-sample buffer from an older
  build) reads as the default, and stays in the file until the setting changes.

## 3. The change log

`settings-changes.jsonl`, beside the file: one JSON line per change, with
`time`, `actor` (`user` or `agent`), `key`, `before`, `after`, and `detail` for
the agent's model. The time is passed in by the caller. **An agent's change is
always logged.** A person's is logged too, by default: ADR-0125 left that open
and recommended it, and ADR-0152 d3 decides it. The switch is a setting,
`settings.logChanges` on the Privacy page, on (A6-20, ADR-0203); the store
reads it before each write, so the change that turns the log off is the log's
last line. There is no undo (ADR-0125 d3); recovery is a bundle or the page's
Defaults button (`resetPage`).

## 4. The agent's pipeline

`agentSet(store, tier, key, value, model, time)` is the only path by which the
agent changes an application setting (ADR-0125 d1):

- **Apply tier only.**
- **A whitelist:** `agentMayChange` in the registry, off for every setting
  unless someone turned it on for that setting. Today: theme, knob mode,
  tooltip delay, follow playhead, Info View, meter peak hold, Zoom on
  Selection, modifier keys, snap.
- **A structural "never" that no mark overrides** (`agentForbidden`): any path,
  anything on the Audio, Plug-ins, Privacy or AI pages, and anything not App
  scope. The test checks that no whitelisted setting is one of these.
- Every accepted change is logged (§3).

## 5. Presets (R-04)

A preset is a name, a set of pages, and the values those pages held.

- `makePreset(store, name, pages)` captures them. A preset holding every page
  is a whole preset; any other is partial.
- `applyPreset` changes the preset's pages only. A key from a page it does not
  hold is skipped and reported, even if the preset file carries it.
- Presets are JSON (`toJson`, `presetFromJson`).

## 6. Settings bundles (R-05, ADR-0127 d5)

```
bundle.zip
  manifest.json    {"kind": "adi-settings-bundle", "schema": 1, "app": "ADI"}
  settings.json    App-scope settings the registry knows
  presets/N.json
```

- **Paths travel as roles.** The roles are the roots a user's material lives
  under: `user library`, and `content folder 1`, `content folder 2`, and so
  on. A path is written as `role:<name>/<rest>`, using the longest root that
  holds it. The record folder under the user library travels as
  `role:user library/Recordings`.
- **A path under no role is left out and reported.** Export then checks the
  whole bundle for any absolute path (POSIX, `C:\`, UNC) and refuses to write
  if one is left.
- **Import resolves each role against the importing machine's own folders.** A
  role that machine lacks is reported and nothing is invented.
- **A bundle from another application is refused.**
- **Unknown keys do not travel:** this build cannot tell whether they hold a
  path.

## 7. The catalogue (ADR-0156)

`docs/SETTINGS-CATALOGUE.md` lists the Settings Reference's 211 rows, and the
registry is filled from it. `catalogue.{hpp,cpp}` puts every row in exactly
one of two lists, and `tests/test_settings.cpp` reads the catalogue and proves
the split.

- **Answered: 145 rows.** Each names the registry keys that answer it. A key
  may answer several rows: Part III's Cubase, Bitwig and REAPER rows mostly
  point back to settings on Live's pages, and `audio.bufferSize` answers both
  Audio and Engine.
- **Absent: 66 rows.** Each has a kind and a reason:

  | Kind | When | Rows |
  |---|---|---|
  | rejected | the catalogue says REJECTED | 10 |
  | note | NOTE | 2 |
  | backlog | BACKLOG: arrives with its feature | 17 |
  | wish | WISH: not planned | 3 |
  | not a setting | decided or proposed, but a behaviour, a command, a display or a fixed rule | 34 |

  The test holds the status to the list. A REJECTED, NOTE, BACKLOG or WISH row
  is never answered, and "not a setting" is only for a DECIDED or DIRECTION
  row.

A row is named `<section> / <setting>`: the section as the catalogue heads it,
and the setting cell up to its first ` (`. Two settings, PTP and AudioGridder
servers, appear in two sections each, so the section is part of the name.

**226 settings on 21 pages:**

| Page | Settings | Notes |
|---|---|---|
| Look & Feel | 48 | Live's Display & Input and Theme & Colors |
| Audio | 27 | 2 Device scope |
| Record | 18 | |
| MIDI | 25 | 8 Device scope (per port) |
| Editing | 20 | |
| Plug-ins | 11 | 2 Device scope (per plug-in) |
| Library | 5 | |
| Privacy | 4 | |
| AI | 6 | |
| File & Folder | 12 | |
| Export | 7 | the export dialog's defaults (A7-03 c, ADR-0203) |
| Mixing | 4 | |
| Transport | 5 | |
| Sync | 13 | |
| Engine | 1 | |
| Updates | 2 | |
| Windows | 3 | |
| Devices | 4 | |
| Music | 5 | |
| Shortcuts | 2 | |
| Project | 4 | |

**The sample rate.** `project.sampleRate` and `audio.sampleRate` are choices
from ADR-0157's ladder: 44.1 / 48, 88.2 / 96, 176.4 / 192, 352.8 / 384 and
705.6 / 768 kHz. Nothing below 44.1 kHz is offered; a media file below it
still plays, converted (ADR-0157 d1). `sampleRateLadder()` is the one list.
The device opens at 44.1 kHz by default (ADR-0203); the project's default
stays 48 kHz because it is the `.adi` schema's column default
(`docs/format/schema.sql`, `project.sample_rate`), which moves only with a
schema bump.

**The Decoding Cache.** `cache.maxSizeMb` (20,480; ADR-0203) and
`cache.minFreeSpaceMb` (2,048) are the keys the decoder reads
(`audio::limitsFromSettings`), and the test checks that their defaults are the
decoder's own.

**Decisions this fill made** (ADR-0156, part two):
- **The theme follows the OS by default**, as the catalogue decides.
  `lookfeel.theme` gains `os`.
- **The agent's tier is Propose by default.** The catalogue said Observe,
  while AI-AGENT §2 and ADR-0152 say Propose. Settled 2026-09-24: the
  catalogue row was win's transcription error, not a ruling (the row cited
  AI-AGENT itself), and now reads Propose.
- **The catalogue adds nothing to the agent's whitelist.** The same nine
  settings as before are marked. The Engine page joins the structural "never".
- **New-track defaults were App scope,** on a *New track* page: the catalogue
  called R-26's page Project scope, but no op sets them in the `.adi`, and a
  Project setting must name its op. ADR-0203 drops the page (§8).
- **Per-port MIDI switches and per-plug-in overrides are Device scope:**
  listed for the badge and stored with the port or device.
- **Where the catalogue names no default, the default is Live's where Live
  has one, and otherwise stated in the help.** For example: the PDC threshold
  when recording is 10 ms (behind a switch since ADR-0203), the rate cap is
  6000 ops a minute (ADR-0203; it was 600), and the freeze tail is 4 s.

## 8. The v1.0 rulings (ADR-0203)

The director's rulings on Master Reference v1.0, applied to the registry.
Where a default moved, the help says so and cites ADR-0203; where Live has
the behaviour, Live's is the default and the other way stays the option.

**Defaults that moved** (old → new): `audio.bufferSize` 256 → 512;
`audio.sampleRate` 48 → 44.1 kHz; `cache.maxSizeMb` 10,240 → 20,480;
`agent.maxOpsPerMinute` 600 → 6000; `agent.maxOpsPerRequest` 256 → 6000 (its
maximum 10,000 → 100,000); `record.warpDetectTempoChanges` off → on (Live's
Auto-Warp); `lookfeel.zoomOnSelection` off → on; `record.discardIncompleteTakes`
on → off; `library.maxResults` 10,000 → 0, which is unlimited (0 is now
legal); `files.copyImportedMedia` ask → never; `lookfeel.wheelChangesParameters`
not in scrollable areas → never. `lookfeel.autoTrackColor` is Live's switch
(on) instead of Cubase's five modes. `agent.asyncServices` gains `builtin`,
the default: stem separation runs built in, in its own process (ADR-0064 d4
amended). `audio.metronomeOutput` empty is Live's Cue Out.

**Removed:** `plugins.scanOutOfProcess` (the scan always runs out of process,
D-03 / A6-37), `devices.pdExternalsFolders` (embedded libpd only, A6-31),
`mixing.defaultSendLevelDb`, `transport.runFxWhenStoppedMs` (effects always
keep running when stopped, as Live) and the five `newtrack.*` keys with their
page (Live's Save as Default Audio/MIDI Track replaces it, a later feature).
`library.recordFolder` is now `files.recordFolder`, on File & Folder; empty
means the project's own folder, as Live.

**Added:** `midi.chaseNotes` (on; amends ADR-0155 d3), `midi.envelopeAutoReset`
(on), `audio.reducedLatencyWhenMonitoring` (off), `audio.recordPdcThresholdEnabled`
(off, as Live: nothing is bypassed while recording; the 10 ms threshold applies
only when it is on), `settings.logChanges` (on, §3), and the Export page:
`export.fileType` (wav), `export.bitDepth` (24), `export.dither` (triangular,
Live's default), `export.nameTemplate`, `export.normalize` (off; peak is Live's
Normalize, loudness aims at `export.loudnessTargetLufs`, -14, under
`export.truePeakCeilingDb`, -1; D-04 a). The destination stays
`files.renderFolder`; freeze render settings stay on Devices.

**ADR-0207 d2, the Processing Precision switch:** `audio.processingPrecision`
on the Audio page, 32 or 64, **32 by default**. At 32 the engine processes in
32-bit float and sums every mix point in 64-bit, as Live's Audio Fact Sheet
describes; at 64 the whole graph runs in double, as Cubase's Processing
Precision, and a plug-in that cannot take 64-bit is processed at 32 between
conversions. In this build every device (VST3, CLAP, the Pd tier and the
built-in devices it runs) is processed at 32 that way; the engine's own nodes
(sums, junctions, strips, input gain, the console halves, clip playback) run
in double. The engine takes it as `SessionSpec::precision`, and changing it
is a rebuild (`Session::setPrecision`); no application layer reads the setting
into the session yet, which is with the window (step 7). Not the agent's
(ADR-0125 d2).

**Registry-only for now:** the engine chases notes unconditionally and reads no
setting for reduced-latency monitoring, the PDC bypass, envelope auto-reset or
the export defaults; wiring each is a follow-up. The switches exist so the
pages, presets and bundles are already right.
