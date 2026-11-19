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
| Reserve: one N row. The snapshot's track fields for recording: `TrackNode::recordArmed`, `monitorMode` and `inputRef`, from `tracks.record_armed`, `monitor_mode` and `input_ref` | engine (`src/adi/engine/snapshot.*`) | MX-09: the strips' Arm and the monitoring switch read the frame's snapshot as every other control does (ADR-0201) | the application hands the window the session's rows (`Session::model()`) once a frame, and an action reads them fresh |
| Reserve: one N row. The input-monitor node of MX-09: a live input per monitored track, from its input (channels 1/2 until `input_ref` has its grammar, MX-25), heard per `monitor_mode` (In always; Auto while armed and no clip plays; Off never) | engine (ADR-0248 d6, item 4) | MX-09: the director hears what he records | Arm and the switch store their choice; nothing is heard of the input; Record records |
| Reserve: one N row. A failed capture's audio kept up to the failure (a locate or an overflow after minutes of good audio is lost whole today: `recording::prepareCommit` takes only a clean result) | engine (`src/adi/recording_commands.*`) | MX-09: a take is not lost to one click on the timeline | the take is dropped and the Status Bar says why, at once |
| Reserve: one N row. Live's monitoring default for a new track: Auto, where New and `track.create` store 0 (Off) | win (AWAITING 20) and the engine | MX-09: a new audio track should monitor as Live's does once the input-monitor node exists | the switch shows Off on New's tracks, as stored |

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

