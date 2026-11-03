# Interoperability with the storytold ArtCraft suite

**Status:** design, nothing built; recorded as ADR-0206 (2026-10-25). This file is the design the
entry points at: the facts verified in the ArtCraft repositories, the four subsystems of the bridge
corrected against those facts, what ADI offers the other way, and the priorities. It contains no
code; interfaces are named so the later PRs can be written from it. Where it and ADR-0206 disagree,
the entry wins and this file is corrected. Every claim with a path and line was read at the heads
named in §1; everything marked *unverified* was not read and must be read before code depends on it.

## 1. What was read, and what was not

| Repository | Read at | Licence | What it is |
|---|---|---|---|
| `github.com/storytold/filmcraft` | `a504f3c5adc4cd60d9c1120916cddb50c9b24b86` (2026-10-25; workspace 0.12.0) | MIT OR Apache-2.0 (`Cargo.toml:9`, README:455) | a Premiere Pro clean-room clone in pure Rust (eframe/egui, cpal); "Ready for real work: ~56%" (README:294-297); no plug-in hosting (README:303) |
| `github.com/storytold/soundcraft` | `541096d2cb5f96a13d5b58935e45711530471480` (main, 2026-10-25) | MIT OR Apache-2.0 (README "License and credits") | a Pro Tools clean-room clone in pure Rust, "roughly 40 %" (README); the suite's only DAW; plug-in hosting not read |
| `github.com/storytold/effectcraft` | `991904888a20b7da72b2a4880b21c7d912154736` (2026-10-24; v0.16.0) | MIT OR Apache-2.0 (`Cargo.toml:9`) | an After Effects-class compositor; the same tick base as FilmCraft (`crates/time/src/lib.rs:17`, "shared design with FilmCraft"); a loopback control channel **without authentication** (`docs/control-protocol.md:19, 26`) |
| `github.com/storytold/photocraft` | main, 2026-10-25 (v0.15.1) | MIT OR Apache-2.0 | a Photoshop-class raster editor; no audio or time role; a control channel and an MCP server; no tick base |
| `storytold/craft-libs` | main | — | shared crates (ai-core, atomic, cms, docpath, fonts-build, i18n, jpeg, keymap, media, pdf, plate-test, platform, prepress, print, quit, settings, ui, window): **no shared control, time or link crate** |
| lightcraft, vectorcraft, designcraft, deckcraft, wordcraft, gridcraft, cadcraft, pdfcraft, figcraft | not read | mostly Apache-2.0 (API listing) | no audio or timeline role expected; **unverified** |

### 1.1 The Gemini prompt against the repositories

| Claim | FilmCraft | SoundCraft |
|---|---|---|
| `--control <port>`, JSON lines over TCP | true: `apps/filmcraft/src/args.rs:82-88` (also `FILMCRAFT_CONTROL_PORT`), `apps/filmcraft/src/control_server.rs:1` "Localhost JSON-lines control server (one request per line, one reply per line). Loopback only."; binds `127.0.0.1` (`:42`), 16 connections (`:26`), 120 s idle (`:28`), 4 MiB line (`:24`) | true: `apps/soundcraft/src/main.rs` usage; `control_server.rs` binds `127.0.0.1`; `--control 0` picks a free port and prints `SOUNDCRAFT_CONTROL_PORT=…`; 16 connections; 90 s reply timeout |
| authentication | **omitted by the prompt**: a 64-hex `FILMCRAFT_CONTROL_TOKEN` in the environment or "control server disabled" (`control_server.rs:39-42`); a `token` field in every request (`docs/control-protocol.md:17-19`); constant-time compare | **none** |
| push / events | **request/reply only**: "Only requests are read" (`docs/control-protocol.md:30`); expiry closes the connection "without an unsolicited reply" (`:22`); `events.list {level?, since?}` is a polled engine query (`crates/engine/src/panels.rs:380`) | request/reply only; no events method |
| wire methods | `engine.execute` / `ui.menu.invoke` {command, params}, `engine.commands`, `ui.menu.list`, `ui.inspect`, `ui.elements`, `ui.set`, `ui.panel.show/close`, `ui.click/move/drag/scroll/key/type`, `ui.timeline.hit/locate` (locating a clip on screen, not the transport), `ui.playback {action: play\|stop\|toggle, speed?}`, `ui.screenshot`, `ui.resize`, `ui.focus`, `perf.stats`, `app.quit` (`crates/ui-egui/src/control.rs:121-492`; `docs/control-protocol.md:47-64`) | `engine.execute` (alias `command`), `engine.commands`, `engine.parity`, `session.inspect` (alias `document.inspect`) {detail: summary\|full}, `ui.*`, `app.quit` (`crates/ui-egui/src/control.rs`) |
| the dispatch point | `Session::execute(&mut self, id: &str, params: Value) -> Result<Value>` (`crates/engine/src/lib.rs:742-743`); undo `:1034`, redo `:1044`; more than 650 command ids (README:233) | `Engine::execute(&mut self, id, &Value)`; `Arc<Session>` undo snapshots, history limit 200; **not** `Session::execute` |
| `doc_inspect`, `project_inspect`, `sequence_inspect`, `command_run`, `command_batch`, `media_import` as CLI commands | **false as stated**: they are MCP tools of `filmcraft-cli mcp` (`crates/automation/src/server.rs:320-422`); the CLI subcommands are probe, bench-decode, commands, describe, exec, run, inspect, import, export, render, mcp (`apps/filmcraft-cli/src/main.rs:319-514`); `command_batch` is not one undo step ("each edit is its own undo step", `server.rs:356`) | false: its MCP tools are `list_commands`, `execute`, `batch` (max 1000), `inspect_session`, `new/open/save_session`, `import_audio`, `bounce_mix`, `parity` and app-only `ui_*` (`crates/automation/src/mcp.rs`, `docs/mcp.md`) |
| inspection shape | `sequence.inspect` (`commands.rs:3517`; `inspect_sequence` `:4418-4452`): `id, name, settings, duration, durationFrames, playhead, in, out, markers[…], video[], audio[]`; per track `items[{clip,item,name,start,duration,sourceIn,sourceOut,end,gainDb,speed,reverse,enabled,link,label,effects}]`; tick values are raw `Tick.0` | `session.inspect` (`crates/engine/src/inspect.rs`), snake_case, samples: tracks, clips `{id,name,start,end,length,start_time,gain_db,muted,fade_in,fade_out,locked,source,offset}`, markers, selection, `transport{playing,recording,position}` |
| `file.exportInterchange` | true except one token: `{"format":"edl\|xml\|fcpxml\|otio\|aaf\|omf"=xml,"path":str,"sequence":id?}` (`crates/engine/src/commands.rs:1488-1496`; `interchange.rs:164-174`); "fcp7" is accepted nowhere; EDL omitted by the prompt | false: no interchange command; AAF and OMF are ❌/❌ and among its listed alpha blockers (`docs/file-format-parity.md`, ROADMAP, `docs/gaps.md` G-03) |
| 254,016,000,000 ticks per second | true: `pub const TICKS_PER_SECOND: i64 = 254_016_000_000;` (`crates/time/src/lib.rs:20`), `Tick(pub i64)` (`:26`), `Tick::MAX = i64::MAX / 4`, `from_units`, `to_units_floor`, `from_rational` (`:40-75`); "Never use f64 seconds for edit math" (CLAUDE.md:18). EffectCraft shares it (`crates/time/src/lib.rs:17`) | false: `pub type Samples = i64;`, an absolute sample index at the session rate (`crates/time/src/lib.rs:3, 20`), `MAX_POSITION = 1 << 40`; `TICKS_PER_QUARTER: i64 = 960` for musical time (`crates/time/src/tempo.rs:6`) |
| a Dynamic-Link-style live link | **false**. What exists: `edit.editOriginal` opens media in the system default app (`crates/engine/src/project_tools.rs:16, 1104-1118`); Settings › Media › "Automatically refresh growing files" re-reads a media file another app wrote over, naming "a render from EffectCraft or After Effects" (`crates/engine/src/media_watch.rs:1-4`); `media.refreshChanged` for agents (`crates/engine/src/relink.rs:671`); a settings row `audio.editInAuditionLocation` with no command (`settings.rs:1087`). No .dawproject, Ableton Link, OSC or MIDI feature | false: no inter-app protocol; a View › Transport "Ableton Link" menu flag exists with nothing behind it (a Pro Tools parity item); no MTC/LTC chase or generation (G-11) |

### 1.2 Facts the bridge stands on

- FilmCraft's audio is planar f32; the sequence sample rate defaults to 48,000 and is validated 1 to
  384,000 (`crates/project/src/lib.rs:847, 875, 958`); cpal is "the playback master clock" and "while
  playing, the samples played by the sound card drive the playhead and video follows"
  (`apps/filmcraft/src/audio.rs:1-2`; `docs/architecture.md:359-361`); sound is mixed 200 ms ahead
  of the device (`perf.stats`).
- FilmCraft's AAF export: `file.exportAaf {path, sequence?, mixdownVideo?, mixdownFormat: mov|mxf?,
  breakoutToMono?, audio: embedded|separate|linked?, audioFormat: wav|aiff|mxf?, sampleRate?, bitDepth:
  16|24?, trimAudio?, handles: frames?, renderAudioEffects?, smallSectors?}` (`commands.rs:1517-1526`);
  OMF export `:1527-1534`; `file.importAaf {path}` `:1535`; `file.import` auto-detects interchange
  documents by content (`:1422-1423`); OTIO round-trips "every FilmCraft detail through
  `metadata.filmcraft`" (README:220-225); `markers.exportCsv` writes Start Ticks and Duration Ticks
  (`docs/agents.md:235-243`). Caveat from FilmCraft itself: its AAF was "never opened in Media
  Composer / Pro Tools" and its OMF "never opened in Pro Tools" (`docs/file-format-parity.md:20-21`);
  OMF read is listed "no" although `omf::import` exists (`crates/interchange/src/omf/mod.rs:57`).
- FilmCraft's media export: WAV/AIFF 16, 24, 32-bit integer or 32-bit float, RF64 past 4 GiB
  (`crates/export/src/pcm.rs:2-3, 38`), AAC, FLAC, MP4/MOV (H.264, HEVC, ProRes, DNxHR…), MXF
  OP1a/OP-Atom; an export queue (`export.queue.add/list/start`, `jobs.list`, `jobs.cancel`,
  `crates/engine/src/export_tools.rs:1158-1166`). Media import: WAV/BWF, AIFF, MP3, AAC, FLAC,
  Ogg/Opus and the video containers.
- FilmCraft's transport over the channel: `ui.playback {action}` (`control.rs:445`) and
  `engine.execute` → `playhead.set` taking ticks, a frame, seconds or a timecode (the general rule
  for FilmCraft's time parameters); `ui.timeline.locate` scrolls a clip into view and is not a
  transport command.
- FilmCraft's project file `.fcproj` is a whole-document JSON snapshot with a schema version (15 at
  the head read; `docs/project-files.md` points at the constant), media by path never embedded;
  undo lives in the Session, not the file (`docs/project-files.md:1-55`).
- SoundCraft: positions are absolute samples at the session rate (8 kHz to 768 kHz accepted, default
  48,000); `file.import_audio {path | paths, track?, at?, new_tracks?}`, `file.bounce_mix {path,
  format?: wav|aiff|flac, bit_depth?: 16|24|32f, start?, end?, source?, normalize?, dither?,
  fold_down?}`, `file.bounce_stems {dir, …}`, `session.inspect`; `.scraft` is plain JSON beside an
  `Audio Files/` folder; the desktop app imports audio, MIDI and `.scraft` paths given on its command
  line (`apps/soundcraft/src/main.rs`). WAV/BWF/RF64/BW64 read and write; AAF and OMF neither.

## 2. The shape: a converter plus a client

README commitment 7 ("interop is a converter, not a shared file") and ADR-0181 d2 ("the control API
is the RPC layer … there is no second server") already decide the shape. The Gemini prompt implied a
bespoke engine-to-engine socket; ADI does not get one. What the prompt called the engine
prerequisite is already designed in ADI, and is the richer half: the loopback RPC boundary
(ADR-0039: one entry point, the op registry; JSON on the wire; off by default, loopback, token), the
surface client class (ADR-0181 d2: WebSocket on loopback, pairing once, token in the OS keychain, an
`Origin` allow-list, an allow-list of ops applied as the user's own), and the one parameter feed,
which **pushes** (ADR-0181 d3). FilmCraft's and SoundCraft's channels are the same idea with one
difference that decides everything about "live": **they only answer**.

| | ADI side | Craft side |
|---|---|---|
| transport | WebSocket on `127.0.0.1`, JSON in the registry's schema | TCP JSON lines on `127.0.0.1` |
| authentication | pairing once, token in the keychain, `Origin` check (ADR-0181 d2) | FilmCraft: a 64-hex token in the environment and in every request; SoundCraft and EffectCraft: none (loopback is the only guard; the bridge says so in its UI) |
| direction | request/reply **and** push (the feed; the visual array streams of ADR-0184; a transport subscription named by ADR-0206) | request/reply only; the bridge polls `sequence.inspect`, `events.list`, `perf.stats` |
| what the bridge may do | an allow-list on its grant, approved by the user at pairing (ADR-0181 d2: "a wider scope is the user's explicit grant, per surface"): transport, the four link ops, media relink/refresh requests, enqueueing export jobs (the user presses render, ADR-0071 d3) | anything `engine.execute` accepts; the bridge uses a short list (§5) |
| attribution | `ops.actor = 'user'`, `actor_detail = bridge:<app>` for transport and link ops; `actor = 'import'` for the structural ops ADI's own importer writes | FilmCraft's History holds each edit as its own undo step |

Nothing in ADI's core knows FilmCraft exists, exactly as nothing knows the Stream Deck exists
(ADR-0181 d4). Whether the bridge is a module in the application or a process beside the agent's
sidecar (ADR-0168) is decided when it is built; the contract is the same either way.

## 3. Subsystem 1: the time base

### 3.1 The constant

FilmCraft and EffectCraft: 254,016,000,000 ticks per second. 254,016,000,000 = 254,016 × 10⁶ =
(2⁶ · 3⁴ · 7²) × (2⁶ · 5⁶) = **2¹² · 3⁴ · 5⁶ · 7²**. Check: 4,096 × 81 = 331,776; × 15,625 =
5,184,000,000; × 49 = 254,016,000,000. It has no factor 11 or 13, which is what makes ADI's
musical tick inexact in it (§3.4) while every sample rate is exact (§3.2).

ADI has two domains (SPEC §4.1): musical ticks at `ADI_PPQ = 5,765,760` per quarter (= 2⁷ · 3² · 5 ·
7 · 11 · 13) in `pos_ticks`, and linear **nanoseconds** (`i64`) in `pos_ns`, with the read position
inside a source file in frames at that file's own rate (`audio_clips.src_start_frames`, SPEC §4.3).
SoundCraft has one domain: absolute samples at the session rate, plus 960 ticks per quarter for MIDI.

### 3.2 Sample frames ↔ FilmCraft ticks: exact

Ticks per sample is 254,016,000,000 ÷ rate, an integer for every rate ADI offers (ADR-0157: 44.1 kHz
to 768 kHz) and for the low rates a file may carry:

| Rate (Hz) | ticks per sample | Rate (Hz) | ticks per sample |
|---|---|---|---|
| 8,000 | 31,752,000 | 88,200 | 2,880,000 |
| 11,025 | 23,040,000 | 96,000 | 2,646,000 |
| 16,000 | 15,876,000 | 176,400 | 1,440,000 |
| 22,050 | 11,520,000 | 192,000 | 1,323,000 |
| 32,000 | 7,938,000 | 352,800 | 720,000 |
| 44,100 | 5,760,000 | 384,000 | 661,500 |
| 48,000 | 5,292,000 | 705,600 | 360,000 |
| | | 768,000 | 330,750 |

- frames → ticks: multiply. Exact.
- ticks → frames: divide; the remainder says whether the position is on the sample grid. A FilmCraft
  position between samples (a picture-frame cut: one NTSC frame is 1001/30000 s = 8,475,667,200
  ticks = 1,601.6 samples at 48 kHz) is placed at the floor sample, as FilmCraft's own
  `to_units_floor` does, and the **verbatim tick value is kept in `external_links`** so the round trip
  back is exact whatever ADI rounded.

### 3.3 Nanoseconds ↔ ticks: exact on a shared grid

gcd(254,016,000,000, 1,000,000,000) = 2⁹ · 5⁶ = 8,000,000, so

- ticks = ns × 31,752 ÷ 125
- ns = ticks × 125 ÷ 31,752

The shared grid is **125 ns = 31,752 ticks**. Positions on it round-trip exactly; others round to
the nearest nanosecond with an error below 0.5 ns, under a fortieth of a sample at 768 kHz (1,302 ns
per sample). Where exactness matters, ADI keeps ticks (the link row) and frame indices
(`project.frame_rate_num/den`); nanoseconds are the placement. Broadcast frame durations divide the
tick rate for 23.976, 24, 25, 29.97, 30, 48, 50, 59.94, 60 and 120 fps because 254,016,000,000 is
divisible by 24,000, 30,000, 60,000, 25, 48, 50 and 120.

### 3.4 Musical ticks ↔ FilmCraft ticks: through the tempo map, rounded

A musical position has no fixed duration; SPEC §4.4 makes the tick↔ns mapping the runtime's, derived
from `tempo_map`. The converter takes the position as a rational number of seconds from the tempo map
and FilmCraft takes it with `Tick::from_rational`. It is not exact in general: at 120 BPM one ADI
tick is 1/11,531,520 s, which is 22,027.97… FilmCraft ticks (the 11 and 13 in ADI's PPQ never divide
into FilmCraft's base). The rounding error is under half a FilmCraft tick, about 2 ps, far below a
sample. For round trips ADI keeps its own musical ticks and the FilmCraft ticks side by side; it
never recomputes one from the other. SoundCraft's 960 ticks per quarter divides ADI's PPQ exactly
(5,765,760 ÷ 960 = 6,006), as SPEC §4.2 promised for every interchange PPQ.

### 3.5 Range

FilmCraft's `Tick::MAX = i64::MAX / 4` ≈ 2.3 × 10¹⁸ ticks ≈ 9.08 × 10⁶ s ≈ 105 days. ADI's
nanoseconds reach ±292 years. The converter range-checks and refuses a position FilmCraft cannot
hold; it never wraps. SoundCraft's `MAX_POSITION = 1 << 40` samples (about 265 days at 48 kHz) is
the same kind of bound.

### 3.6 Where it lives

One pure component in ADI's time library, beside the tick↔ns mapping, parameterised by
`units_per_second` (254,016,000,000 for FilmCraft and EffectCraft; the session sample rate for
SoundCraft, where units are samples), with: units ↔ frames at a rate, units ↔ ns, a rational-seconds
entry for musical positions, and the range check. Tests from the tables above, the NTSC frame, the
125 ns grid, the `Tick::MAX` refusal. **P1**, because it is cheap and the later converters and the
bridge both need it; nothing calls it until phase F.

## 4. Subsystem 2: the schema reservation

### 4.1 The rule it obeys

ADR-0144: a minor adds whole objects (new tables), never a column on an existing table and never a
widened CHECK; an older reader sees the new tables as empty; the upgrade creates each later minor's
objects in order in one transaction. The two tables below are an additive minor of their own. The
`project.sampleRate` default change that BUILD-BRIEF §8 lists is a different kind of change (an
existing object's default) and takes ADR-0144 d3's own mechanism and entry; the two are not bundled.

### 4.2 The two tables (names provisional until the schema PR)

**`external_sessions`**, one row per linked document:

| Column | Meaning |
|---|---|
| `id` | primary key |
| `app` | which Craft app: `filmcraft`, `soundcraft`, `effectcraft`, … (text, never an enum CHECK, so a new app needs no schema change) |
| `app_version` | as reported (FilmCraft: the `generator` of the `.fcproj`, or the control channel) |
| `doc_rel_path`, `doc_abs_path_hint` | the `.fcproj` or `.scraft`, kept as `media_files` keeps paths |
| `doc_hash_blake3` | the document's hash at the last sync; `''` until read |
| `remote_doc_id` | FilmCraft's sequence `ItemId`, or SoundCraft's session identity |
| `units_per_second` | 254,016,000,000 for FilmCraft and EffectCraft; the session sample rate for SoundCraft (units are samples) |
| `frame_rate_num`, `frame_rate_den` | the sequence's picture rate, rational |
| `sample_rate` | the sequence's or session's audio rate |
| `remote_revision` | FilmCraft's `Session.revision` when the channel reports one; null otherwise |
| `last_sync_utc` | when the last send or return completed |
| `role` | `picture`, `audio_post` or `source` (text) |
| `enabled` | 0 by default, as `remote_hosts.enabled` is: a project never talks to another process because it was opened |

**`external_links`**, one row per linked ADI object:

| Column | Meaning |
|---|---|
| `id` | primary key |
| `session_id` | → `external_sessions`, `ON DELETE CASCADE` |
| `local_kind`, `local_id` | the ADI object: `track`, `clip`, `marker`, `media`, `project`; a polymorphic anchor checked by `adi_tool check`, as the other soft references are |
| `remote_kind`, `remote_id` | FilmCraft's item or clip id (text), SoundCraft's clip id |
| `remote_start_units`, `remote_duration_units`, `remote_source_in_units` | **verbatim**, in the session's units; never recomputed from ADI's placement |
| `remote_revision` | the remote revision the row was last confirmed against |
| `state` | `linked`, `stale` (the remote changed since), `broken` (the remote item is gone) |

Unique on (`session_id`, `remote_id`, `local_kind`, `local_id`). Both tables are project data (they
travel with the `.adi`, appear in the text projection and are checked), written only through ops.

### 4.3 The ops that first write them (P2, with the bridge)

Named now, designed at the schema PR, in OPS.md §9.11's table shape: `external.attach {id, row}`
(inverse: pair `external.detach`), `external.detach {id}` (captures the row), `external.link {id, row}`
(pair `external.unlink`), `external.unlink {id}` (captures). Editing scope, no engine impact. Until
they exist nothing writes a row, so an undo can never lose one (ADR-0146 d1's pattern).

### 4.4 Rejected: an `extensions` blob

`extensions` (`ns`, `key`, opaque `data`, `criticality`) could hold a link today without a minor.
Rejected: a link is relational (one per clip), has to be checked and projected to text, and is
essential to a round trip, which an advisory blob is not. The format's own argument applies:
reserving a table now is nearly free; migrating later costs trust.

## 5. Subsystem 3: the control client (the bridge)

### 5.1 Attach

The bridge pairs with ADI once (ADR-0181 d2) and then finds or launches the Craft app. For FilmCraft
it generates the 64-hex token and launches with `--control <port>` and the token in the environment;
if FilmCraft is already running, the user must have launched it with both, and the bridge says so.
(Whether FilmCraft's `--control 0` picks a free port as SoundCraft's does is **unverified**.) It
reads `sequence.inspect` (FilmCraft) or `session.inspect` (SoundCraft), fills the `external_sessions`
row through `external.attach`, and from then on polls `events.list` (FilmCraft) and the inspection at
a low rate to keep `state` honest (`stale` when the remote revision moved).

Surface: a File menu command ("Link ArtCraft app…") and the Export page (below); the executables'
paths in Settings beside `files.sampleEditor`. Not a Settings page (ADR-0203 d2).

### 5.2 Send to ADI (FilmCraft → ADI)

1. The bridge asks FilmCraft for `file.exportAaf` with `audio: separate`, `audioFormat: wav`,
   `bitDepth: 24`, handles, `renderAudioEffects` and, when wanted, `mixdownVideo` (or `file.exportOtio`
   plus `file.exportMedia` for the WAVs).
2. ADI's converter (§6) imports it: tracks and clips as linear-time objects (`time_base = 1`), media
   by BLAKE3 hash (dedupe as `media_files` already does), markers as markers, clip gain and its
   keyframes as event volume (ADR-0115), the video mixdown as Live's video clip (Live 12 manual
   chapter 27: a movie on an audio track, the floating Video Window). Import follows the no-warp rule
   (ADR-0132 d6). Every imported clip gets an `external_links` row with the verbatim ticks.
3. Structural ops carry `actor = 'import'`; the bridge itself wrote nothing structural.

### 5.3 Return to FilmCraft (ADI → FilmCraft)

An **After Export action** on the Export page (ADR-0205 d9 already lists Cubase's After Export, "open
elsewhere"):

1. The user ticks the sources (the render matrix, ADR-0203 d14; stem jobs, ADR-0205 d5), chooses
   "Return to FilmCraft", and presses render (ADR-0071 d3: files on disk have no undo, so the AI and
   the bridge fill the form and never press the button).
2. The job writes the mix or the stems **over the files FilmCraft's linked clips reference** (the
   paths from `media_files` of the linked rows), or beside them when the user says so.
3. The bridge calls `media.refreshChanged` on FilmCraft, or FilmCraft's Settings › Media › "Automatically
   refresh growing files" watcher notices by itself. The cut updates without a re-import. This is the
   Edit-in-Audition round trip done the way FilmCraft's code already anticipates ("a render from
   EffectCraft or After Effects written over the file a clip uses"). The first upstream contribution
   the director plans (the stale-waveform fix) is the piece that makes this visible in FilmCraft.
4. A **re-conform** (clips moved or split in ADI) cannot be written into FilmCraft's existing sequence
   from outside; the bridge writes an AAF or OTIO and calls `file.importAaf` or `file.import`, which
   makes a new sequence. Merging is FilmCraft's to add (P3).

The same shape serves SoundCraft (`file.import_audio` of the rendered files) if ever wanted; it is
not scheduled.

### 5.4 Transport

**ADI as master (P2).** The bridge subscribes to ADI's transport on the control API (a subscription
named by ADR-0206: position, playing/stopped, tempo, loop; surfaces and Link need it too) and sends
FilmCraft `ui.playback {action}` and, at every locate, `engine.execute` → `playhead.set` (ticks,
frame, seconds or timecode). FilmCraft's own cpal clock then runs and its video follows its audio.
Resolution is a frame, not a sample: two free-running audio clocks drift, and the bridge re-locates
during play to bound it. This is what scoring to picture needs and what Live itself gives (the
picture is a clip; chapter 27).

**FilmCraft as master (P3).** The only way is to poll `sequence.inspect` for `playhead` at perhaps
30 Hz and chase with a flywheel, the class of MTC (Live 12 manual chapter 36: Live can only follow
MIDI Timecode, not generate it, and tempo changes cannot be tracked). It is honest to call this
MTC-class and nothing more. Sample-locked chase needs a push channel or a shared clock, and FilmCraft
has neither; PTP (ADR-0107) and Link (ADR-0185 d7) do not reach it. The one upstream change that
moves this to P2 without redesign is an `events.subscribe` (push) method with a transport state
stream on FilmCraft's control channel; whether to propose it is the director's call, as part of his
contributions (no contact with maintainers before his go).

### 5.5 What the bridge never does

Open a second server in ADI; write structural ops; press render; talk to anything past loopback;
store a token in the `.adi`; keep running because a project was opened (`enabled` is 0 by default).

## 6. Subsystem 4: the OTIO/AAF fallback through the converter path

ADI's converters are one layer: `.dawproject` (P1, ADR-0127 d7), MIDI, stems, and now OTIO and AAF
(P2), OMF read (P3). They are useful without the bridge (an AAF from Media Composer or Pro Tools),
and the bridge is nothing without them.

| Format | FilmCraft | What it carries into ADI | What it loses | ADI priority |
|---|---|---|---|---|
| OTIO (OpenTimelineIO, Apache-2.0) | export `file.exportOtio`; import by `file.import`; "every FilmCraft detail through `metadata.filmcraft`" | structure, tracks, clips with source ranges, markers, FilmCraft's metadata verbatim (kept in `external_links` or passed through) | no audio essence (media by reference); effects and speed only as metadata | P2 |
| AAF Edit Protocol | export `file.exportAaf`: embedded, separate or linked WAV/AIFF, 16/24-bit, handles, mono breakout, rendered clip effects, optional video mixdown; `file.importAaf` | audio essence, clip volume with keyframes → event volume (ADR-0115), markers, source timecode, dissolves and dips | speed changes, frame holds, graphics, video effects other than transitions; clip markers become media markers (FilmCraft's own list) | P2 |
| OMF 2.0 | export `file.exportOmf` (embedded or separate audio); `omf::import` exists in code, parity table says read "no" | audio tracks with crossfades and gain, sample-accurate | video, markers | P3 read |
| FCP7 XML (`xml`), FCPXML, CMX 3600 EDL | export and import | picture-side structure | audio post detail | not planned |
| WAV / AIFF / FLAC | both ways | the stems and the mix | — | P1 (the queue) |
| `.fcproj`, `.scraft` | FilmCraft's and SoundCraft's own JSON | — | — | **never read or written by ADI** (commitment 7) |

Mapping rules, the same for every converter: positions as linear time through §3; media by hash;
a missing file stays a placeholder and relinks by content (SPEC §10.2); gain keyframes to event
volume; markers to `markers`; a clip FilmCraft made from a nested sequence or a graphics template
becomes a plain clip with a remark naming what it was. FilmCraft's own caveat is ADI's test: its AAF
was "never opened in Media Composer / Pro Tools"; ADI's import is one more reader that will find
out, and a corpus of FilmCraft exports joins ADI's round-trip tests when the converter is built.

## 7. What ADI offers the other way

| | What | Status |
|---|---|---|
| Stems for FilmCraft and SoundCraft | the export queue's stem jobs as WAV (ADR-0205 d5, P1); FilmCraft imports them directly or through AAF/OMF with the structure; SoundCraft through `file.import_audio` | with the queue |
| `.adiviz` for EffectCraft and any consumer | ADR-0205's sharded bundle (`00_master_manifest.adiviz.json`, `schema_version` `3.0-sharded`, ~8-second scene files; the `zero_redraw_layered_vfx` conditioning with `compositor_bindings` is the natural input for a layered compositor) published as an open format. EffectCraft's control channel exists (`effectcraft --control <port>`, no authentication); **its command set was not read**; a bridge could drive it once it is | no ADI work beyond ADR-0205; a consumer is storytold's or P3 |
| The external Sample Editor preference (Live's Edit button; `files.sampleEditor`) | Live's rule (manual 8.4.3): the Edit button opens the sample in an external editor, playback stopped, the file edited **in place**; so ADI's default is in place and the "copy" choice is ADI's addition (REAPER's). Today the registry holds the path only; nothing launches an editor until step 7. **SoundCraft is not a sample editor** (a Pro Tools clone that opens files from its command line and writes nothing back in place); it is not the target. A SoundCraft bounce-back over its control channel is P3 if ever wanted | the preference and the Edit button are parity (step 7); SoundCraft P3 |
| ADI's control API to storytold's agents | FilmCraft's MCP and ADI's agent (ADR-0168) are both MCP-shaped; an agent that drives both is a client of two servers and needs nothing from ADI | none |

## 8. Priorities in one table

| Priority | What | Why here |
|---|---|---|
| **P1** | the exact time-base converter (§3) | cheap, pure, needed by every converter and the bridge; expensive to retrofit into placed projects |
| **P1** | `external_sessions` + `external_links` in an additive minor (§4) | format space, nearly free now, a migration later; nothing writes them until P2 |
| **P2** | the bridge: attach, Send to ADI, Return to FilmCraft, ADI-master transport, the four link ops (§5) | rides on the control API (step 9), the export queue (P1) and the converters; a first-class addition |
| **P2** | OTIO and AAF import/export (§6) | the industry handoff; useful without the bridge |
| **P2** | Live's video clips and Video Window (chapter 27) | Live's ability, so Live's way; the picture side of the bridge; win's placement, to strike if wrong |
| **P3** | ADI following FilmCraft (poll, MTC-class); sample-locked sync; a shared clock; frames streamed in; merging into an existing FilmCraft sequence; EffectCraft reading `.adiviz`; OMF read; SoundCraft bounce-back; SoundCraft AAF/OMF | each depends on something storytold has not released, or is long tail |

## 9. Open items

- FilmCraft `--control 0` free-port behaviour; whether `file.importAaf` can target an existing
  sequence; EffectCraft's command set (all unverified; read when phase F starts).
- The table and op names, fixed at the schema PR.
- The transport subscription's exact fields on the control API, designed with the API.
- Whether the director wants ADI to follow FilmCraft at frame resolution at all, or only the master
  direction; whether he proposes `events.subscribe` upstream with his contributions.
