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
| Reserve: one N row. `notes::prepareBatch(snapshot, ops)`: one prepared `note.applyDiff` whose effects are the legacy note ops (insert, delete, move, resize, setVelocity, setMute) applied in turn with the overlap rule each applies | engine (`src/adi/note_diff*`, MI-01) | MI-01: a note edit of any size writes the stream once, one op in the history | the editor sends one op per note, one undo step per edit, as today |
| Reserve: one N row. The snapshot's track fields for recording: `TrackNode::recordArmed`, `monitorMode` and `inputRef`, from `tracks.record_armed`, `monitor_mode` and `input_ref` | engine (`src/adi/engine/snapshot.*`) | MX-09: the strips' Arm and the monitoring switch read the frame's snapshot as every other control does (ADR-0201) | the application hands the window the session's rows (`Session::model()`) once a frame, and an action reads them fresh |
| Reserve: one N row. The input-monitor node of MX-09: a live input per monitored track, from its input (channels 1/2 until `input_ref` has its grammar, MX-25), heard per `monitor_mode` (In always; Auto while armed and no clip plays; Off never) | engine (ADR-0248 d6, item 4) | MX-09: the director hears what he records | Arm and the switch store their choice; nothing is heard of the input; Record records |
| Reserve: one N row. A failed capture's audio kept up to the failure (a locate or an overflow after minutes of good audio is lost whole today: `recording::prepareCommit` takes only a clean result) | engine (`src/adi/recording_commands.*`) | MX-09: a take is not lost to one click on the timeline | the take is dropped and the Status Bar says why, at once |
| Reserve: one N row. Live's monitoring default for a new track: Auto, where New and `track.create` store 0 (Off) | win (ruled: Auto, N67) and the engine (`track.create`'s default) | MX-09: a new audio track should monitor as Live's does once the input-monitor node exists | New's tracks and the window's new tracks are written Auto (`window-fn/monitor-auto`); a bare `track.create` stores Off |
| Reserve: one N row. A device parameter whose lane goes (its last point deleted, the lane deleted or disabled) should return to its stored value, as a mixer control does. Today the plug-in keeps the last automated value until it is touched or re-resolved | engine (`Session::rebuild`, `bindDeviceAutomation`) | DF-W37: deleting a device lane's last point in Automation Mode leaves the device playing the envelope's last value | nothing; the window deletes the lane as it does a track's |
| Reserve: one N row. After a discovery job, an ordinary project open should still probe a plug-in's saved path when the catalogue lacks it, as it does before any job. Today `managedDiscovery_` is set by the first `startDiscovery` and never cleared, so `makeVst3` and `makeClap` answer "needs discovery" for every miss for the rest of the run (and `hydrateCatalogue` returns at once). A proposal: suppress the hint only while a job is in flight (`discoveryJob_`) | engine-win (`src/juce/juce_device_loader.cpp`, N58) | DF-W34: a project that names a plug-in installed since the last scan loads it on open, as before the window started discovery at launch | an open while a job runs waits for its adoption (engine-win's handoff); an open with no job running loads catalogue hits only |

---

## 2026-11-22, window-fn — MX-04: Options > Delay Compensation (PR requested)

M1's MX-04, its open half (the PLAN M1 rows, after win's three items). The registry row
`audio.delayCompensation` was on main and nothing read it.

- The Options menu gains **Delay Compensation**, ticked from the application setting (on by default), in
  its own group before Solo in Place. Its place among Live's other Options entries is to be read on Live 12
  (PROVISIONAL). Choosing it writes the setting, with no undo step, and the open project's session follows
  through `Session::setDelayCompensation`. So does every project open, and a change to the Settings' Audio
  row; an open Settings window's row follows the menu.
- While it is off, the mixer's Track Delay box is unavailable, keeps its stored value and says why where
  the pointer is, and its "ms" box dims (manual 18.7).
- A session that refuses keeps its reason in the Status Bar. The setting is in force once set, as every
  Settings change is, and a file that could not be written is said.

**Run.** The window's 27 suites (100%); `adi_ui_shell_tests` 2919 checks, 0 failures: the tick, the command
writing the setting and telling the session with no undo step, Track Delay unavailable and still 10.00 ms,
a refusal kept, the menu's order and the command ids' tail. Review: two dimensions with a skeptic on each
finding. Five were confirmed and fixed (the refusal cleared, the stale Settings row, a save failure leaving
the session behind, the tooltip, the "ms" box); two were refuted.

**PR requested** — `window-fn/delay-compensation` at `ea67a671`, on main (merged today): "adi_daw: MX-04: Options >
Delay Compensation, Live's switch, on the session". It depends on nothing.

## 2026-11-22, window-fn — N67 and N65's window halves: Auto monitoring, the inputs heard (PR requested)

Win's third item.

- **N67** (ruled by win under ADR-0203 d1). New's audio and MIDI tracks monitor on Auto, as Live's: set
  right after the store's seed, as part of the new file, with no op and no undo step. WTS-01 step 1 still
  reads 0 ops. The window's own new tracks (Insert Audio/MIDI Track, a file dropped below the tracks) carry
  `track.setMonitorMode` Auto in their creating batch, the same undo step. A bare `track.create` still
  stores Off until the engine's default changes.
- **N65.** Nothing called `Session::setAudioMonitorInputs`. The document now tells the session which
  inputs feed each audio track: 1/2, or 1 alone on a one-input device, as recording takes them, until a
  track's input has its grammar (MX-25). It does so whenever the tracks or the device's open inputs change;
  the application gives the input count when audio attaches and when the device changes. The session then
  hears each track as its Arm and monitoring say.

**Run.** The window's 27 suites (100%); `adi_ui_shell_tests` 2911 checks and `adi_ui_seams_tests` 553, both
0 failures, with the default checks moved from Off to Auto. WTS-01 with Pd: every step as before. 2a
records with monitoring Auto, and 2b still fails only on the engine's read-ahead at Play. Its check now
reads "not In", since Auto keeps the input out while the take plays. The harness gives the document the
device's two inputs as the app does. A probe (not committed) set track 4 to In: the fed input was heard at
its peak (0.4000), and with no inputs given there was silence. No review workflow ran.

**PR requested** — `window-fn/monitor-auto` at `5ac33b36`, on main `0ea10b8e`: "adi_daw: N67 and N65's window
halves: new tracks monitor on Auto; the session hears the inputs". It depends on nothing; it touches WTS-01's
harness and step 2 (win's).

## 2026-11-22, window-fn — DF-W37: Live's automation lane chooser (PR requested)

Win's second item: WTS-01's 6b.

- **The chooser.** The track header's menu gains Show Automation (Live 12 manual 25.2). It lists the
  Mixer's Volume, Pan and each send that has its row, then each device on the track with its automatable
  parameters. A device knob's menu gains Show Automation too, which shows that parameter and turns
  Automation Mode on. A track's lane shows its chosen target, its Volume until another is chosen; a choice
  whose device or send is gone reads as Volume again.
- **Editing.** Breakpoints are added, moved and deleted on the chosen target's lane as on Volume's, each
  one undo step. A first point makes the lane: a track's in real units, a send's on its routing row, a
  device parameter's normalized and named as the engine binds it (`engine::automationParamRef`). Only refs
  the engine plays are offered. A disabled lane of the target is shown and edited, never doubled. While
  Automation Mode is on, the window demands every device's parameters from the feed, so any track's chooser
  lists them; with the mode off, it keeps no demand. A parameter the panel does not show reads as the
  plug-in's declared default.
- **Look.** The menus use JUCE's popup, as the header's other items do; Live's two choosers in the track
  header are the window lane's when their sheet comes.

**Run.** The window's 27 suites (100%). `adi_ui_shell_tests` 2919 checks, 0 failures: the chooser's list, Show
Automation from a knob, and add, move (held to 0 to 1) and delete on a device parameter's existing lane,
then Volume again. **WTS-01 6b PASS** (with Pd): Utility's Gain offered in the chooser and its envelope drawn
point by point with the window's calls; the gain x1.995262 then x1.000000, max |d| 2.2e-08. Review: two
dimensions with a skeptic each. Confirmed and fixed: the default for a parameter off the panel, a demand
left after the header menu, a duplicate lane beside a disabled one, and Show Automation offered where it
fails. Six were refuted; one belongs to the engine (the needs table: a device lane that goes leaves its last
value).

**PR requested** — `window-fn/lane-chooser` at `305e9b3a`, on main `0ea10b8e`: "adi_daw: DF-W37: Live's
automation lane chooser: the track's mixer and every device's parameters". It depends on nothing; it
touches WTS-01's step 6 (win's).

## 2026-11-22, window-fn — N60's window half: the keyboard into the session (PR requested)

Win's first item of the new week: WTS-01's 3b and 3d. The keyboard reached nothing; it now reaches the
armed track's instrument.

- **The consumer.** `MidiInputFeed` (`adi/ui/midi_input_feed.hpp`, no JUCE) is the port rings' one
  consumer. It runs on the audio thread, in the device callback, before the document processes the block:
  the application wraps the document in `LiveMidiProcessor`, which the `DeviceBridge` drives. It drains
  every port's ring and reads each message with the engine's own `MpeParser`, the port's MPE switch
  choosing its zone. It then hands the events, in time order across ports, to `Session::enqueueMidiInput`,
  the session's one serialized producer. Only a port whose Track switch is on reaches the tracks; a port
  open for Remote only is drained and goes to nothing until a control surface consumes it. Note ids are
  made unique across ports, nonzero, and channels zero-based, as the session's contract asks.
- **Time.** Each event is stamped in stream samples from its port time. At each block the ports' clock
  is anchored to the block's first sample, and what arrived during the last block's span plays one block
  later at the same offset, so the gaps between notes are kept. Nothing is put before an earlier event.
  The age is taken modulo the 2^32 ms wrap of the counter some backends use. A device restart re-anchors
  the feed and resets the session's input.
- **Lifetimes.** The application owns the feed, declared before the port host and the bridge, so it
  outlives both. The host publishes its open inputs on each refresh; `publish` waits for the audio thread
  to let go of the list it replaces, and only then are closed inputs' rings freed. A change of inputs or
  switches resets the session's live input, so no key held on a closed port sounds on. A change of the MPE
  switch changes the zone only. No allocation or lock on the audio thread; the parsers are on the heap.
  The MIDI Indicators read the inputs' activity count instead of draining the rings.
- **Known limit.** One parser per port: a UMP-native device's groups share its channels. On ALSA and
  CoreMIDI each jack is a port of its own.

**Run.** The window's 27 suites (100%). `adi_ui_shell_tests` 2922 checks, 0 failures, including the feed:
time order across ports, stamps and clamping, zero-based channels, note ids per port, Remote-only ports,
refusals, restart, unpublished rings, the MPE zone and its change under a held key, and fairness.
WTS-01 with `-DADI_ACCEPTANCE=ON`: the harness's driver now pumps the feed as `LiveMidiProcessor` does,
and 3b and 3d play their keys into a port ring.
- **3b PASS** (a build with Pd, and so OneShot): first sound at +1 frame from the note's sample, peak
  0.1256, nothing refused.
- **3d PASS** (the VST3 fixture): onset at +1 from the block start, as the fixture plays events at
  offset 0, 440.0 Hz, peak 0.0712.

The session's remaining FAIL is 2b, the engine's read-ahead at Play, and 7c and 7e fail only without Pd.
Review: two dimensions, a skeptic on each finding. Confirmed and fixed: the handoff's race (two publishes
could free a ring being read), `std::stable_sort` allocating on the audio thread, the feed's 4.7 MB on a
test's stack, a busy port starving the others, held notes sticking when a port closed, and the MPE switch
dropping releases. Taken as a known limit: the UMP groups. Two findings were refuted.

**For win.** `tests/acceptance/*` are yours. Step 3b, 3d and the harness's `Driver` changed as above, and
step 3's `doc` and `driver` are now marked used in a build without Pd, which did not compile before.

**PR requested** — `window-fn/midi-live` at `fe24a5b0`, on main `0ea10b8e`: "adi_daw: N60's window half: the
keyboard into the session, from the device callback". It depends on nothing.

## 2026-11-20, window-fn — DF-W36: `MidiPortHost::dropped()` safe from any thread (PR requested)

Win's evening entry gave DF-W36 to this seat. The header said `activity` and `dropped` may be read from
any thread, but `dropped()` walked `inputs_`, which `refresh()` rebuilds on the message thread. The input
callbacks now count an event lost to a full ring in one host-level atomic, beside `activity_`, so the
promise holds. The count runs from the host's start, and an input closed by `refresh()` keeps its share.

**Run.** The window's 27 suites (100%). A fixed-port test host opens no inputs, so the shell's check stays
at "nothing played, nothing dropped". No review workflow ran for a seven-line change.

**PR requested** — `window-fn/midi-dropped` at `9bcd9e72`, on main `7510acd2`: "adi_daw: DF-W36:
MidiPortHost::dropped() is one atomic, safe from any thread". It depends on nothing.

## 2026-11-20, window-fn — AU-04: Re-Enable Automation in a control's context menu (PR requested)

M1's AU-04, its open half. The Control Bar's switch (#472) re-enables every lane at once. Live's
parameter menu re-enables one (manual 25.4), and `Session::reenableAutomation(laneId)` was on main with
no caller.

- A device knob's right-click (on the knob or its labels), and the mixer's pan and send menus, offer
  **Re-Enable Automation** while the control has a lane. It is available while that lane is overridden.
  Choosing it makes that one lane follow its envelope again; the others stay overridden. It is session
  state and no undo step, as the switch is (ADR-0162). `ProjectDocument::reenableAutomation(laneId)`
  refreshes the snapshot and the feed as the global one does, and refuses a lane that is not overridden,
  with the reason.
- The lane behind a control (`adi/ui/automation_override.hpp`) is the one the engine plays. As its
  binders do, the lookup skips a disabled lane or one in another value domain, and the first of two
  plays. A device's lane is named as the engine names it (`engine::automationParamRef`, a CLAP id in
  decimal), so `DevicePanelData` now carries its plug-in format.
- **Not here.** The vertical volume fader: a JUCE linear slider moves on a right-click before a menu can
  open, so its item waits for a fader that ignores the right button (the look's or a follow-up). The
  AU-01 lanes' own item comes once `window-fn/automation-lanes` lands. Split pans' items come with MX-02.

**Run.** The window's 27 suites (100%). `adi_ui_shell_tests` (2798 checks): Cutoff overridden, its
menu's item available; chosen, it follows its lane, the switch is unlit, and no undo step is added; a lane
not overridden is refused; a control with no lane gets no item. `adi_ui_seams_tests` (537 checks): the
played lane among disabled, wrong-domain and second lanes, and a CLAP id. Review: three dimensions with a
skeptic on each finding. One was confirmed (the lookup took the first lane, not the played one) and fixed;
three were refuted.

**PR requested** — `window-fn/reenable-param` at `f2988fab`, on main `909ba114`: "adi_daw: AU-04: Live's
per-parameter Re-Enable Automation in a control's context menu". It depends on nothing.

## 2026-11-20, window-fn — DF-W9's load half brought onto N58 (PR requested)

`window-fn/plugin-load` was built on the first scan branch's gate. It now merges
`window-fn/plugin-scan-n58`, with no conflict. The insert goes straight to the loader: the Browser lists
the catalogue, and the loader serves a catalogue hit while a discovery runs. Every listed plug-in carries
its kind, so the guessed-kind path is only a fallback, and its Status Bar line no longer cites N58.

**Run.** The window's 27 suites (100%); `adi_ui_shell_tests` 2841 checks, 0 failures.

**PR requested** — `window-fn/plugin-load` at `ee8bd592`: "adi_daw: DF-W9 load on N58: the Browser inserts
from the loader's catalogue". It depends on `window-fn/plugin-scan-n58` (land that first; this branch
contains it) and on N57, which is on main.

## 2026-11-20, window-fn — DF-W34 on N58: the Browser's plug-ins through the loader's discovery jobs (PR requested)

N58 (engine-win's catalogue API) and the Settings' separate VST3 and CLAP search roots are on main, so
the scan moves onto them, as engine-win's log asked. This branch replaces `window-fn/plugin-scan`: it is
that branch merged with main and reworked. The window's own worker, its gate in front of the loader
(`Hold`) and its folder plumbing are gone.

- **The service.** `PluginCatalogueService` (`adi/ui/plugin_scan.hpp`, no JUCE) runs on the message
  thread, over a `DiscoveryPort` that `ui_main.cpp` adapts to `startDiscovery`, `discoveryProgress`,
  `adoptDiscovery`, `cancelDiscovery` and `catalogue()`. The app's 20 ms timer polls it. It never scans
  and never waits.
- **The jobs.** At launch the first project's open reads the stored catalogue (the loader's read-only
  hydration, already on main), and the Browser lists it at once. Then an Incremental finds what was
  installed or removed since. Engine-win suggested CacheOnly first; a separate CacheOnly job would only
  make the first open wait. Rescan runs a Refresh, and stops a running Incremental first, since the
  Refresh covers it. A change to the Settings' plug-in folders runs an Incremental; a running job left
  stale runs again.
- **What is shown.** The Browser's list comes from `catalogue()`. Each VST3 has its kind from the
  catalogue's instrument flag (N58), named as `device::identityOf` names it. The status line ("Scanning
  plug-ins: 12 of 150 files…") heads the list for the whole job. A count changes only that line, so the
  selection and scroll stay. A failed or user-cancelled job keeps the list and names why it stopped; a job
  the loader would not start is named too, on the Settings page as well.
- **Opens.** Once a job has run, the loader resolves plug-ins from the catalogue alone (see the needs
  table). So a project opened while a job runs waits for that job's adoption, then finds what it found.
  The Status Bar says "Opening X once the plug-in scan has finished…". A stale job's opens wait for its
  rerun; those of a job a Rescan stopped wait for the Refresh. Opens run between jobs, before the next job
  starts. A refused open clears its line.

**Run.** The window's 27 suites (100%). `adi_ui_shell_tests` 2836 checks, 0 failures: the service over a
stand-in port (startup listing, progress, adoption, Rescan stopping an Incremental, a stop refused by a
finished job, Stale, opens waiting and running between jobs, a failed job, a refused start, Quit), and the
Browser's status line keeping the selection. The new Rescan test fails on the previous rule. The app ran
headless under a virtual display, with a CLAP fixture folder set in the Settings: the Incremental started,
was adopted Published, and the catalogue held the fixture's two plug-ins. There were two review rounds,
each finding verified by a skeptic. The first confirmed 13 findings (one root cause: stopping a job for an
open gained nothing; plus the Browser's selection, the progress line, Rescan's refusal and the waiting
line), all fixed. The second confirmed one (the Rescan case above, reported twice) and a test gap, both
fixed.

**For engine-win, through win.** The needs-table row above: the loader's saved-path probe stays off for
the whole run after the first discovery job.

**For win.** `adi_play_render_tests` does not build on GCC 13 with warnings as errors:
`tests/scanner_session_reopen.hpp:208`, "loop variable '<structured bindings>' creates a copy"
(`-Werror=range-loop-construct`; `const auto& [position,count]` is the fix). It came from main (`3bfd0d5c`),
and it is not this seat's file.

**PR requested** — `window-fn/plugin-scan-n58` at `423f1027`, on main `909ba114` (merged): "adi_daw:
DF-W34 on N58: the Browser's plug-ins through the loader's discovery jobs". It replaces
`window-fn/plugin-scan` and depends on #676 (`window/plugins-browser`), which it contains. The DF-W9 load
branch (`window-fn/plugin-load`) was built on `window-fn/plugin-scan`'s gate; it is brought onto this
branch next.

## 2026-11-20, window-fn — DF-W35: a send's Enable Send tick and action under one rule (PR requested)

Win's entry of 19 November gave DF-W35 (a) to this seat; (b) and (c) are small and were taken with it.

- **(a)** `enableSend` now reads a send as `sendOn` does: with no row, a track's send is on and a return's
  is off. Unticking Enable Send on a new track's send writes the row fully down, as the knob shows it, and
  switched off: one step, "Disable Send". It is no longer refused as "off already". Ticking it again
  switches the same row on, and undo removes the row, so the send reads on again. Enable All Sends makes
  no row for a send that already reads on.
- **(b)** Enable All Sends counts no id past its last new row, which may be INT64_MAX itself. The review
  found that the first test of this passed on the old code too, because the overflow is undefined and
  invisible on a plain build. So `adi_ui_seams_tests` now builds with signed-overflow trapping (GCC and
  Clang, trap mode, no runtime), and putting the old increment back makes the suite trap. There are cases
  for one row and for two rows ending at INT64_MAX, and for three that do not fit.
- **(c)** `heldBy` and `holdsSoloed` share one walk (`track_edits_detail::insideGroup`). It stops after as
  many steps as there are tracks, as `TrackTree::within` does, so a parent loop cannot hang the mixer. A
  copied snapshot with two tracks made each other's parent checks it.

**Run.** The window's 27 suites (100%); `adi_ui_shell_tests` (2747 checks, 0 failures) with a mixer case:
Disable Send on 3 Audio's send with no row, the tick gone, one step, undo; `adi_ui_seams_tests` (549
checks). The review ran in three dimensions with a skeptic on each finding: one confirmed (the test above,
fixed), two refuted.

**PR requested** — `window-fn/send-enable` at `bb8d28f1`, on main `909ba114`: "adi_daw: one rule for a
send's Enable Send tick and action; bounded group walks (DF-W35)". It depends on nothing. It touches
`CMakeLists.txt` in one place, the seams target's compile options.

## 2026-11-19, window-fn — AU-01: Automation Mode in the Arrangement (PR requested)

**What it does** (`src/adi/ui/automation_mode.hpp`, `src/juce/ui_arrangement.*`, `ui_shell.*`): the bar's
Automation Mode switch (Live 12 manual 25.2) draws each track's Volume envelope across its lane (the
fader's level as a line where it has none). A click on a lane adds a breakpoint there, on the grid as the
view snaps, at the value the height means; the lane's foot is silence, as the fader's. The track's Volume
lane (real dB, the one the mixer plays) is made in the same batch when it has none. A breakpoint dragged
is written once at the release; a double-click on one, or Delete on a chosen one, removes it, and the
last one takes its lane, so the fader plays again. Breakpoints are linear (curve 1). Each edit is one
undo step ("Add/Move/Delete Automation Point"). In the mode a lane's clicks are the envelope's: fade
handles are hidden, a double-click neither makes nor opens a clip; Shift and Ctrl/Cmd still select.

**Not yet:** choosing another parameter (Live's lane choosers in the track header wait for their look;
pan is supported in `automation_mode.hpp` for when they come); lanes under the track (Unfold Track); AU-05,
Lock Envelopes. The envelope is joined with straight lines between linear points; a curved segment drawn
elsewhere shows straight until the drawing reads the curve formulas (SPEC 6.3.2).

**Review:** two dimensions, each finding checked by a skeptic: 12 confirmed, 0 refuted, all fixed in the
branch's last commit. Planted fault fires (no grid snap on a click).

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app` and all 27 of `ui.yml`'s
suites on main merged; `adi_ui_shell_tests` 2762 checks, 0 failures.

**PR requested.** Branch `window-fn/automation-lanes` at `ecd6797b`. Title: **adi_daw: window-fn: AU-01,
Automation Mode in the Arrangement: a track's Volume envelope drawn on its lane, breakpoints added,
dragged and deleted, each one undo step**. Depends on nothing unlanded. **For the window lane:** the
envelope's drawing (a line in `look::record`, squares on the points) is a stand-in until the Arrangement
sheet draws it.

---

## 2026-11-19, window-fn — item 10 parked: the recording and export parity checklists need the Live 12 manual's text

The checklists are drafted row by row from the manual (chapter 21 for recording, 5.2 for export), each row
citing its section. This seat has no copy of the manual, and the manual's public pages are on a host this
cloud environment does not reach. Drafting from memory would put section numbers in the repository that
nobody checked, so the item waits. **Two ways on, for win:** the window lane drafts both from the copy it
works from (it wrote `parity-7.6-mixer.md` and the rest that way), with this seat's branches as the
behaviour to check (MX-09 for recording, EX-11 for export, whose rows the log entries above list); or the
public manual's host is allowed for this seat's environment and it drafts them.

---

## 2026-11-19, window-fn — DF-W28 and ED-03: clip moves placed by the engine's planner (PR requested)

**What it does** (`src/juce/ui_shell.*`: `placeClips`; `ui_arrangement.cpp`): a clip dragged, several
picked clips dragged together (across tracks too) and a clip moved by the arrow keys are each one
placement by `edit_plan::prepareMove` under the user's edit policy (`capturePolicy`), where the window sent
`clip.move`, which left the two clips stacked. A clip the move covers whole goes; one it covers in part
keeps the rest (Live 12 manual 6.7); one undo step, named as before. Where the planner does not place yet
(a track with an automation lane, AU-05; a frozen track; Delete Overlaps off or muted clips as deleted;
clips that already overlap), the clips move as before, keeping what they land on, and the Status Bar says
why, as a range edit already does.

**Not in this branch:** a dropped file and a duplicate landing on a clip (DF-W28's second sentence; the
drop goes through the import's own batch, the duplicate through the clipboard's prepares); the parity
rows from a Live measurement the row asks for.

**Review:** two dimensions, each finding checked by a skeptic: 5 confirmed, 3 refuted, all fixed in the
branch's last commit. One shell test's expectation changed: a clip moved onto one it covers whole now
removes it, as Live does (it had encoded the stacked result).

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app` and all 27 of `ui.yml`'s
suites on main `88e2d94e` merged; `adi_ui_shell_tests` 2752 checks, 0 failures.

**PR requested.** Branch `window-fn/clip-overlap` at `b40ceb93`. Title: **adi_daw: window-fn: DF-W28 and
ED-03, clip moves placed by the engine's planner: the covered part goes, as Live**. Depends on nothing
unlanded.

---

## 2026-11-19, window-fn — MI-01 parked: the window's switch to note.applyDiff waits for one engine helper

Tried and taken back: the Note Editor's batches turned into one `note.applyDiff` from the clip's stream as
stored and the batch's final values (a translator over note.insert, delete, move, resize, setVelocity and
setMute). Transpose, velocity, mute and Add Chord passed, but 13 of the shell suite's note checks
failed: each legacy note op applies the engine's overlap rule as it lands (a note moved into a longer note
of its key cuts it short; a chop, a stretch, Fit to Time Range and Set Length build on what the op before
left), and a diff of final values skips those steps. Copying the overlap rule into the window would make
two owners of it. Nothing of this is pushed.

**Asked of the engine (need below):** `notes::prepareBatch(snapshot, ops)`, one prepared `note.applyDiff`
whose effects are exactly the legacy ops applied in turn (it already plans each one: `legacyApply`). The
window then routes every note batch through it in `AdiRootComponent::submitEdits`, about twenty lines.

---

## 2026-11-19, window-fn — DF-W9's load half on N57: a plug-in from the Browser onto a track (PR requested)

**What it does** (`src/adi/ui/plugin_listing.hpp`, `src/juce/ui_browser.cpp`, `ui_main.cpp`, `ui_shell.hpp`):
a double-click or Enter on a plug-in in the Browser's Plug-Ins loads it onto the selected track, in front
of the device chosen in the strip or where Live places its kind, through the engine's insert
(`ProjectDocument::insertPlugin`, N57): reference, chain, device and route in one undo step. A CLAP's kind
comes from its descriptor. A VST3's is not known until engine-win's catalogue (N58): the window takes it
for an instrument on a MIDI track and an audio effect on an audio track, and says so; a plug-in the
project already holds keeps the kind its one reference stores. A plug-in that does not load stays in the
chain as a bypassed placeholder, and the Status Bar says why. The names are no longer dimmed. A drag of a
plug-in is not built yet.

**Review:** two dimensions, each finding checked by a skeptic: 4 confirmed, 5 refuted, all fixed in the
branch's last commit.

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app`, all 27 of `ui.yml`'s suites
and `adi_builtin_devices_tests` (N57's own); `adi_ui_shell_tests` 2787 checks, 0 failures.

**PR requested.** Branch `window-fn/plugin-load` at `942873bd`. Title: **adi_daw: window-fn: DF-W9's load
on N57, a VST3 or CLAP from the Browser onto the selected track, one undo step**. **Depends on**
`window-fn/plugin-scan` (DF-W34, itself after #676) **and** `engine/n57-plugin-insertion` (N57, the
engine's): land it after both. Its own diff is one commit of six files. **DF-W9 can close with it.**

---

## 2026-11-19, window-fn — MX-23 and SRF-02: faders, pans and sends heard while dragging (PR requested)

**What it does** (`src/adi/ui/project_document.*`, `src/juce/ui_mixer.*`, `ui_shell.hpp`, `ui_main.cpp`):
- A mouse drag of a track's fader, its pan, or a send knob that has its routing row is the document's
  live gesture: the engine's `gesture::Coordinator` over an `EnginePresentation` of the document's view
  and session, with `gesture::relativeScalarProducer` (on main). Each move is heard at once and nothing
  is written; the release commits one transaction of the final values, one undo step named for the
  control ("Change Track Volume", "Change Track Panning", "Change Send Level"); a drag that ends where it
  began, or a click that does not move, writes nothing; Esc during the drag puts every value back.
- With the dragged track among several selected tracks, all of them move by the same amount in the same
  step (Live 12 manual 18.1); a send moves alone.
- The fader drags from where it is, not to where it was clicked; the Mixer View fader's foot is the floor
  (silence), as the track controls' is.
- Typed values, a double-click's reset, a send with no row yet: as before, the clicked control alone,
  committed at once or at the release.

**Review:** three dimensions (the gesture API and its lifetimes, the strip's handlers, the tests), each
finding checked by a skeptic: 11 confirmed, 3 refuted. All fixed in the branch's last commit (listed in
its message). Planted faults fire (the release after Esc committing; the controls not re-read after Esc).

**Run:** Linux, GCC 13, Debug, JUCE on, Pd off, warnings as errors: `adi_app` and all 27 of `ui.yml`'s
suites on main merged; `adi_ui_document_tests` 252 checks (a tone measured while the fader is dragged:
heard at -12 and -24 dB with nothing written, one undo step at the release, Undo and Esc back to 0 dB),
`adi_ui_shell_tests` 2757 checks, 0 failures.

**PR requested.** Branch `window-fn/mixer-gestures` at `1eaa239d`. Title: **adi_daw: window-fn: MX-23
and SRF-02, faders, pans and sends heard while dragging, one undo step per gesture**. Depends on nothing
unlanded.

**Not in this branch:** the Track Delay drag and the group's summing drive (MX-23 names them; the engine's
producer has the drive, `GroupSummingDrive`, and no Delay preview); automation recording while armed
(SRF-02's last clause) waits for AU-03.

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

