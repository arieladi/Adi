# window-fn — log

The window's function seat (ADR-0248 d14): a second Claude seat in the window lane. It makes the window's
existing surfaces work: commands, keys, gestures, undo steps, and the wiring from the window to the
engine. The window seat (`collab/window.md` on `window/log`) keeps the look, built from the sheets the
director agreed (ADR-0243 d1); this seat changes no look, and where an item needs a control that has
none yet it uses the shared painters of `src/juce/ui_controls*` as they are. It works in the window's
paths (`src/adi/ui/**`, `src/juce/ui_*`, `src/juce/app*`) and in nobody else's: the engine's and
engine-win's needs go in the table below, and win tells their logs.

Branches `window-fn/<topic>`, one item each, cut from `origin/main`; this log on `window-fn/log`. Only
the window-fn seat writes to this file. Newest entry first. Each entry ends with "PR requested": the
branch, its title, what was run, and what it depends on.

**Where it runs.** A cloud session (Linux, GCC 13, Ninja). Built as `ui.yml` builds the window:
`-DCMAKE_BUILD_TYPE=Debug -DADI_WITH_JUCE=ON -DADI_WITH_PD=OFF -DADI_WERROR=ON`, the window's suites run
headless. MSVC `/WX` and macOS are not run here; the `Step 7 UI` workflow runs them on each pull request.

---

## FOR THE DIRECTOR

None open.

---

## Needs from other seats (open)

Numbers are asked for in `collab/RESERVATIONS.md` through win before they are used ("Reserve:" below);
until then a need is named by its item.

| Need | Owner | Why the window needs it | What the window does meanwhile |
|---|---|---|---|
| Reserve: one N row. A distinct `scan::State` (or a shared constant) for "the scanner helper could not start", apart from the file it was asked to read | engine-win (`src/adi/plugin_scanner.*`) | DF-W34 (b): the Status Bar names the scanner, not the plug-in, when the helper is missing or blocked | the window matches `Scanner::probe`'s words ("scanner helper unavailable or scan cancelled") on an `Unavailable` failure |
| Reserve: one N row. Live MIDI input to the armed track's instrument, taking the window's per-port rings (`MidiEventRing` of `MidiPortEvent`: MIDI 1.0 bytes, the UMP group, the arrival time in ms of JUCE's high-resolution clock) through a published list the callback reads | engine (ADR-0248 d6 item 4) | SRF-05: the director's keyboard into OneShot or his VST3 | the rings fill and are drained on the message thread; the MIDI Indicators light |
| Reserve: one N row. A `midi.port.name` row, so a port's record can be found by name when its identifier changes (the registry's comment promises the healing) | engine-win (`src/adi/settings/registry.cpp`) | SRF-05: a keyboard on another USB socket keeps its switches | records are keyed by identifier and direction only |
| Reserve: one N row. The render worker's contract for plug-ins (instances made and given state on the message thread, processing on the worker, `serviceHost` marshalled), so Export renders a project with a VST3 or CLAP | engine-win (EX-11's engine half, the export pipeline) | EX-11: one of the director's VST3s in the exported WAV | the page refuses such a project, saying why |
| Reserve: one N row. The snapshot's track fields for recording: `TrackNode::recordArmed`, `monitorMode` and `inputRef`, from `tracks.record_armed`, `monitor_mode` and `input_ref` | engine (`src/adi/engine/snapshot.*`) | MX-09: the strips' Arm and the monitoring switch read the frame's snapshot as every other control does (ADR-0201) | the application hands the window the session's rows (`Session::model()`) once a frame, and an action reads them fresh |
| Reserve: one N row. The input-monitor node of MX-09: a live input per monitored track, from its input (channels 1/2 until `input_ref` has its grammar, MX-25), heard per `monitor_mode` (In always; Auto while armed and no clip plays; Off never) | engine (ADR-0248 d6, item 4) | MX-09: the director hears what he records | Arm and the switch store their choice; nothing is heard of the input; Record records |
| Reserve: one N row. A failed capture's audio kept up to the failure (a locate or an overflow after minutes of good audio is lost whole today: `recording::prepareCommit` takes only a clean result) | engine (`src/adi/recording_commands.*`) | MX-09: a take is not lost to one click on the timeline | the take is dropped and the Status Bar says why, at once |
| Reserve: one N row. Live's monitoring default for a new track: Auto, where New and `track.create` store 0 (Off) | win (AWAITING 20) and the engine | MX-09: a new audio track should monitor as Live's does once the input-monitor node exists | the switch shows Off on New's tracks, as stored |

---

## 2026-11-19, window-fn — EX-11: the Export page, a WAV of the song (PR requested)

**What it does** (`src/adi/ui/export_choices.hpp`, `src/juce/ui_export.*`, `ui_main.cpp`, `ui_shell.*`):
- **File > Export Audio/Video** (Ctrl/Cmd+Shift+R; Live 12 manual 5.2.1; in the File menu after Collect and
  Export) opens the Export page with Live's choices in Live's order: Rendered Track (Main, or a track in
  the lanes' order), Render Start and Render Length (bar positions in the project's meter; the time
  selection, else the loop brace whether the loop is on or not, as Live's), Include Return and Main
  Effects, Render as Loop, Convert to Mono, Normalize, Create Analysis File, Sample Rate (the project's
  own rate always offered), Encode PCM with File Type, Bit Depth and Dither Options, Encode MP3, Export
  Video.
- **Export** asks for a file (".wav" added, never swapped; a file the user chose to replace goes to the
  trash first, so a failed render loses nothing) and renders with the engine's `render::renderWav` on a
  worker: the project as committed, on the renderer's own read-only connection; the range on the tempo
  map at the chosen rate; 16, 24 or 32-bit float; no, rectangular or triangular dither; Normalize as
  Live's (peak to 0 dB). Devices are made on the message thread and handed to the worker. Progress and
  the outcome show on the page and in the Status Bar; Cancel and Quit stop the render; opening another
  project stops it and closes the page.
- **Refused, each with its reason on the page:** a track through the returns and Main (the engine's
  stems, EX-07; with the switch off the track's own post-fader signal renders), Render as Loop, Convert
  to Mono and analysis files (EX-07), AIFF, FLAC and MP3 (EX-08), the shaped dithers (EX-06), video
  (P2), and a project with a VST3 or CLAP device (the engine's render worker; until N57 the window cannot
  put one on a track anyway).

**For the director's session:** New's Set has Reverb and Delay on its returns, which only a Pd build
loads; the renderer renders only a project whose every device loads, so a Pd-off build refuses New's
Set ("export requires a fully resolved project"). The Release build is Pd on.

**Review:** three dimensions (the worker and its lifetimes, behaviour against Live's dialog and the
renderer, the tests), each finding checked by a skeptic: 23 confirmed, 2 refuted. All fixed in
`250caf4b` (listed in its message). Planted faults fire (the exact range; MP3's refusal).

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app` and all 27 of `ui.yml`'s
suites on main merged; `adi_ui_shell_tests` 2762 checks, 0 failures, a real render of a project to the
chosen WAV over the whole range among them.

**PR requested.** Branch `window-fn/export-page` at `250caf4b`. Title: **adi_daw: window-fn: EX-11, the
Export page: Live's Export Audio/Video choices, one WAV of the song through the engine's renderer**.
Depends on nothing unlanded. `AppCommands` gains `ExportAudio` after `FitToTimeRange`, as MX-09 gains
`ArrangementRecord`: whichever lands second puts its id after the other's (one line, and the tail check).

---

## 2026-11-19, window-fn — SRF-05, the window's half: the MIDI port host, the MIDI Ports table, the MIDI Indicators (PR requested)

**What it does** (`src/juce/app_midi_ports.*`, `src/adi/ui/midi_ports.hpp`, `ui_settings.*`, `ui_main.cpp`,
`ui_controlbar.cpp`):
- **The port host** (`MidiPortHost`), the application's one owner of the MIDI ports, on JUCE 9's
  Universal MIDI Packet API with Windows MIDI Services off (JUCE's default; the Win32 backend). It lists
  every input and output by name, follows hot-plug, and opens each input whose Track or Remote switch is
  on, in the MIDI 1.0 protocol. What a port sends goes, timestamped in milliseconds and with its UMP group,
  into that port's own lock-free ring (one producer, the port's input thread; one consumer; no lock, no
  allocation) and is drained on the message thread.
- **Live's MIDI Ports table** at the head of the Tempo & MIDI Settings page, in place of the `midi.ports`
  record's text and the per-port rows: a row per port and direction, inputs first, Track, Sync, Remote and
  MPE. A switch is in force at once, saved, and opens or closes the input; a record per direction
  ("in:<identifier>", "out:<identifier>"), since a two-way port has one identifier on ALSA, Android and
  WinRT. With no port: "No MIDI port is connected. A port connected now appears here."
- **The MIDI Indicators** on the Control Bar light for 150 ms when MIDI comes in.

**Not yet:** MIDI Arm into the armed track's instrument. The rings wait for their consumer, the engine's
live MIDI input (ADR-0248 d6 item 4); until it is on main the events light the Indicators and are let go.
The table's look is the Settings' own controls until the Settings sheet is agreed.

**Review:** three dimensions (threads and the UMP API, behaviour against SRF-05 and Live's table, the
tests), each finding checked by a skeptic: 13 confirmed, 4 refuted. Twelve fixed in the branch's last
commit (listed in its message); the thirteenth, healing a record by name, is a need below. Planted
faults fire (the per-port rows shown; the inputs-first order).

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app` and all 27 of `ui.yml`'s
suites on main merged; `adi_ui_shell_tests` 2754 checks, `adi_ui_window_state_tests` 109 checks, 0
failures. This machine has no MIDI (no ALSA sequencer): the host lists none and says so; the table's
switches are tested over a fixed list of ports.

**PR requested.** Branch `window-fn/midi-ports` at `76508b13`, main merged. Title: **adi_daw: window-fn:
SRF-05, the window's half: the MIDI port host on JUCE 9's UMP API, Live's MIDI Ports table, the MIDI
Indicators**. Depends on nothing unlanded. Adds `src/juce/app_midi_ports.cpp` to `adi_app` and the shell
tests in `src/juce/ui_shell.cmake`, and links `adi_settings` into `adi_ui_window_state_tests`.

**For win (the default):** `midi.port.track` defaults to off, so a keyboard plugged in reaches no track
until Track is ticked. Live's default for a new port is to be read (one more line for the devices lane's
list of Live readings); if it is on, the registry row changes.

---

## 2026-11-19, window-fn — MX-09: Arm, the monitoring switch, Arrangement Record and Overdub (PR requested)

**What it does** (`src/adi/ui/arm_edits.hpp`, `project_document.*`, `window_state.hpp`, `src/juce/ui_shell.*`,
`ui_mixer.*`, `ui_controlbar.cpp`, `ui_main.cpp`):
- **Arm** on a track's strip (the Mixer and the track controls) writes `track.setArm`: Exclusive Arm
  (`record.exclusiveArm`, on as Live's) unless Ctrl/Cmd is held; the selected tracks together when the
  clicked one is among them; Main, returns and groups have none. One batch, one undo step.
- **The monitoring switch** In, Auto, Off: a click writes `track.setMonitorMode` (the selected tracks
  together); the switch lights the track's own choice, where it always lit Auto before. The snapshot
  carries neither arm nor monitoring (need below), so the application hands the window the session's rows
  once a frame (only when they change) and an action reads them fresh.
- **Arrangement Record** (F9, the Control Bar's Record; manual 21.1): every armed audio track records from
  the device's inputs 1/2 (1 on a one-input device; `input_ref` has no grammar yet, MX-25). The recorder is
  prepared on the message thread, then the transport mailbox's new Record command starts the driver
  through `RenderBlock::startPlayback(insert marker, recording)`, which applies the saved song start and
  Count-In. Pressed while playing, it records from the playhead. Record again stops the recording and the
  transport plays on; Stop or Space ends both. On the timer the take lands through
  `recording::prepareCommit`/`commit`: one undo step for every armed track, each clip named after its
  track, the first selected, "Recorded 2 takes." in the Status Bar. A capture that ends on its own (a
  locate, an overflow, a device change) ends the recording at once and says why; its private files go
  with it. A take still recording or landing when a project is replaced or the app quits is settled into
  its project first. A Count-In reads as playing, so Space cancels it.
- **Refused, with the reason in the Status Bar:** nothing armed; only MIDI armed (the engine's live MIDI
  input, ADR-0248 d6 item 4); a device with no inputs; the loop on (a linear capture fails at the loop's
  end and loop passes are not published as takes yet, CT-02).
- **MIDI Arrangement Overdub** is the Control Bar's own switch (the window's state, off in a new Set); it
  decides what a MIDI recording does once MIDI records (SRF-05).

**Not heard yet:** the input. The engine's input-monitor node (need below) is what makes In and Auto
audible; this branch stores the choice and records.

**Review:** three dimensions (the callback and threads, behaviour against Live and MX-09, the tests),
each finding checked by a skeptic: 10 confirmed, 5 refuted. All ten fixed in `d815201d` (listed in its
message). Planted faults fire: Exclusive Arm off; the Record command not starting the transport; polling
only while landing; the Count-In not read as playing.

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app` and all 27 of `ui.yml`'s
suites; `adi_ui_shell_tests` 2782 checks, 0 failures; `adi_ui_document_tests` 255 checks, 0 failures (a real
recording through `ProjectDocument::process`, one undo step, a locate mid-take, a device stopped while
landing).

**PR requested.** Branch `window-fn/arm-record` at `81fe15df`, main merged. Title: **adi_daw: window-fn:
MX-09, Arm and the monitoring switch, Arrangement Record (F9) and MIDI Arrangement Overdub; a take lands as
one undo step**. Depends on nothing unlanded. `AppCommands` gains `ArrangementRecord` after
`FitToTimeRange`: whichever of this and a window-lane branch adding commands lands second merges the list.

**For the window lane:** the monitoring switch now lights the stored choice (Off on New's tracks) where
it always lit Auto; no golden changed (outside the application the window shows Live's default, Auto).

---

## 2026-11-19, window-fn — DF-W34: the plug-in scan off the message thread (PR requested)

**Taken over from the window lane, with win's word in ADR-0248 d14:** the function list, starting with
DF-W34. **Update, later the same evening:** the window lane requested #676 (`window/plugins-browser`,
DF-W9's listing with its review fixes, `06698a2c`), so this branch now merges #676's head and main and
lands **after #676**, as its follow-up: it replaces #676's first scan on the message thread (after the
first paint) with the worker, and #676's stopped-scan wording with the report's. The window lane leaves
DF-W34 to this seat.

**What it does** (`src/adi/ui/plugin_scan.hpp`, `plugin_listing.hpp`, `src/juce/ui_main.cpp`,
`ui_browser.*`, `ui_settings.*`):
- (a) Choosing Plug-Ins never scans on the message thread. `PluginScanService` runs the app's one
  `JuceDeviceLoader` on a worker; the list's line and the Status Bar say "Scanning plug-ins: N files…"
  (the VST3 files and CLAP bundles it will probe, counted first, filesystem only) and the list fills when
  the scan's end reaches the message thread. An arrow key or a click never waits. The session's loader
  goes through a gate (`gatedLoader`): a VST3 or CLAP load waits for a running scan and holds the loader
  until it is made, so the loader never runs on two threads and no second scanner meets the registry.
  Quit cancels through the helper's cancel flag. The lazy scan inside a project open runs only with no
  worker beside it, and what it lists is adopted.
- (b) A scan that stopped early says why, from the loader's report: the scanner itself (the registry, or
  the helper missing or blocked), or the file that stopped it; "Nothing could be listed." or "The list
  may be incomplete.", and "Rescan from the Settings' Plug-Ins page." It never says "the rest are
  listed", which is kept for a complete scan's per-file failures (now "1 plug-in file", singular).
  **Rescan** is on the Settings' Plug-Ins page, as Live's (the Restore Defaults button's look, beside
  it). A change of the plug-in folders rescans once Plug-Ins has been scanned; choosing Plug-Ins after a
  stopped scan does not rescan by itself, so a missing helper is not probed on every click.
- (c) One folder per vendor ignoring case, named as the first of its plug-ins spells it; no vendor and
  "other" share one.
- (d) The load still waits for N57; its Status Bar line cites "N57".

**Kept from the row:** one loader, one registry, the format and vendor folders, the Status Bar lines,
the shell test's layout. **Not done, and why:** a folder taken out of the Settings stays searched until
restart (the loader adds folders and removes none: engine-win's catalogue settings, N58); a VST3's kind
stays unknown until N58's typed catalogue. When N58 lands, the service wraps its `startDiscovery`,
`discoveryProgress` and `adoptDiscovery` in place of `loader_.scan()`, with per-file progress.

**Review:** an independent review in three dimensions (threads and lifetimes, the row's behaviour, the
tests), each finding checked by a skeptic: 5 confirmed, 8 refuted. All five fixed in `0155622d`: the
loader held for the whole load (a plug-in running its own message loop while it is made could let a
posted restart start a worker mid-load), a missing helper worded as the scanner's, the test scan polled
for its cancel (it could hang), and two checks that could not fail made real. Planted faults fire (the
vendor key; the hold; the generation guard).

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app` and all 27 of `ui.yml`'s
suites build and pass on the merge of #676 and main `bedee5ab`; `adi_ui_shell_tests` 2782 checks, 0
failures.

**PR requested.** Branch `window-fn/plugin-scan` at `73ec8d80`: #676's head and main merged. Title: **adi_daw:
window-fn: DF-W34, the plug-in scan off the message thread: a worker that owns the loader, a gate for
loads, Rescan, a stopped scan named, one folder per vendor**. **Depends on #676**; land it after #676 (its
diff against #676 is DF-W34 alone).

**Asked of engine-win (through win):** a distinct state or a shared constant for "the helper could not
start" in `scan::Result` (`Scanner::probe` reports it as `Unavailable` with "scanner helper unavailable or
scan cancelled" and the file's path; the window matches those words today).

---

