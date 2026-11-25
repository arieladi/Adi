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

- **Insert Shape's lower row (AU-02, L-AUTO-8 unresolved): which values do Live's ramps and ADSR join?** The
  window's rule now: a rise runs from the lower of the envelope's two edge values to the higher, a fall from the
  higher to the lower; on a flat envelope a rise goes to the parameter's top and a fall to its foot; the ADSR
  rises from the lower edge value to the top. **Recommendation:** keep this rule until a Live 12 reading
  (L-AUTO-8) says otherwise. It joins both edges whenever the envelope's direction matches the shape, and on a
  flat lane it gives a full-range ramp, which is the useful case. A reading changes three lines of
  `insertAutomationShape`.

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
| Reserve: one N row. `Session::refresh` should re-read the project's identity (`recordingProject_`, read once in `Session::load` from `PRAGMA database_list`). After a Save As every take is refused, "recording belongs to a different project", until the project is reopened | engine (`src/adi/engine/session.cpp`) | PD06-SAVEAS-GUARD: recording after a Save As keeps its take | nothing: the take is refused and the Status Bar says why |
| Reserve: one N row. A dropped audio file should clear what it lands on, as a move, Paste and Duplicate do (DF-W28): `clip_edit::prepareImportAudio` placed through the planner (`PlacementBatch::clear`), or a planner entry that clears a track span in the same prepared edit as the window's `clip.create`/`clip.attachAudio` batch | engine (`src/adi/material_commands.cpp`, `src/adi/edit_plan*`) | DF-W28: a file dropped onto a clip replaces the covered part in one step | the drop lands over the clip, as before |
| Reserve: one N row. A loop recording keeps the passes it has when it ends early: today a tempo change (Tap Tempo included), a loop edit or running out of passes during a loop recording ends the capture ("musical loop retiming ends this job") and every pass recorded so far is lost | engine (`src/adi/engine/audio_recording.*`, `src/adi/recording_commands.*`) | CT-02: a loop take survives a tempo tap or a loop nudge | nothing: the take is lost and the Status Bar says why |
| Reserve: one N row, or a ruling. Live's undo for a loop recording: one step for the whole recording, or one per pass as `prepareLoopCommit` commits today (the window queue's test asked for one step) | engine and win | CT-02: Undo after a loop recording | the window follows the engine: a step per pass |
| `adi_play_render_tests` fails "same canonical module projects saved alias spelling without rewriting its exact VST3 UID" (`tests/test_play_render.cpp:138`) on Linux, GCC 13: the VST3 fixture loaded through a symlinked `alias.vst3` is not found as the same module (JUCE assertion `juce_VST3PluginFormatImpl.h:1117`). Hidden until now because the test did not compile under GCC 13 (DF-E71) | engine-win (the device loader's canonical module identity) | `ctest` on a GCC 13 build is red on this test | nothing; the window's suites do not use it |
| AU-08's lane-scoped range commands (cut, copy, paste, delete, duplicate and insert time on one automation lane's selection) through the time edits' automation splice | engine seat (the range producers' automation half) | AU-08: Live's commands on an automation lane's selection | the lanes' menu has Insert Shape, Simplify Envelope and Delete Automation only |
| Insert Shape's guards through one shared splice (PLAN AU-02's engine half): a guard or split at a selection's edge that keeps a bent segment's bend (ADR-0159's curve slices), written by the planner, so a shape leaves every curve outside the selection exact (AT-1) | engine seat (`automation_shapes.cpp`) | AU-02: the window writes its own guards, exact for holds and straight segments only | the window's guards (`insertAutomationShape`) |
| A GCC 12 build of `src/adi/mix/limiter/Dsp/FloatRealTransform.h:61`: `bit < std::bit_width(length) - 1` compares an `int` with `std::size_t` under GCC 12.4's libstdc++ (its `bit_width` returns the argument's type; GCC 13's returns `int`), which `-Wsign-compare` with warnings as errors refuses. `static_cast<int>(std::bit_width(length)) - 1` builds on both | the mix seat (the limiter core) | DF-W39's test is a GCC 12 build; every suite that links the limiter stops there | DF-W39 was tested with that cast made locally and not committed |
| Reserve: one N row (MX-07's registry half). `mixing.soloInFront`, `mixing.soloInFrontDb` and `mixing.soloBus` labelled "not built" in their titles or descriptions, as the PLAN row asks, and Live's Options rows `preFollowsSolo` and `keepAllReturns` (they change only on readings, AWAITING 5) | engine-win (`src/adi/settings/registry.cpp`) | MX-07: the Settings say plainly which solo options do nothing yet | the window shows the rows as the registry gives them |

---

## 2026-11-25, window-fn — win's list of 25 November: DF-E76, DF-E67 (b), DF-E12, DF-E78, the parity sheets (PR requested)

Read first: win's entries of 24 and 25 November. Items 1 to 4 are rows lent from the Codex work seat (out until
29 November), each on its own branch, each touching only that row's paths. Every proof below is with
`-DADI_WERROR=ON`; each names its compiler.

**1. DF-E76 → `window-fn/df-e76` d8b9a43d** (two commits) (`src/juce/juce_device_loader.cpp`, `src/juce/vst3_host.*`).
- The loader already matched a symlinked bundle's saved path to the catalogue's module by its canonical path,
  and kept the project's exact uid. It then instantiated the plug-in through the alias. On Linux, JUCE opens
  `<bundle>/Contents/<arch>-linux/<bundle name>.so`, and the alias's own name names no binary there, so the
  factory was null (JUCE's assertion) and the device became a placeholder.
- It now keeps two descriptions: the projection onto the saved spelling for the device's identity (its uid
  unchanged), and the catalogue's own description to load from (`Vst3Host::makeDevice`'s new overload).
- The review found one more branch: a bundle outside the catalogue, found by the path-hint scan, keeps the saved
  alias's spelling, and so do later loads in that session. The loader now derives the module to load from the
  canonical path of whatever it found, whenever that differs from the spelling. No test reaches the path-hint
  branch through an alias (the render suite has no scanner helper); it is covered by reading.
- The check at `tests/test_play_render.cpp:138` is kept as it was; its `load` also requires the instance's uid to
  equal the saved one.
- **Run, GCC 13.3, JUCE on:**
  - Pd off, at d8b9a43d: the whole tree builds; the play, VST3, scan, device, plug-in, CLAP and catalogue suites
    pass, and so do the 27 window and mix suites and the shell suite (3147 checks, 0 failures).
  - Pd and acceptance on, at b8ab5ce0: WTS-01's VST3 steps 7d (a project holding a VST3 exports its 192000
    frames with the saved note) and 7e pass, `vst3: 0 FAIL`. d8b9a43d changes only paths whose canonical form
    differs from their spelling, which that fixture's does not. 7d and 7e need Pd's built-ins, so a Pd-off build
    reports them failed whatever the loader does.
- PR requested: `window-fn/df-e76` → main, "DF-E76: a VST3 reached through a symlinked bundle loads from the real
  module". Depends on nothing beyond main.

**2. DF-E67 (b) → `window-fn/df-e67b` 0271bf12** (two commits) (`src/juce/pd_loader_fix.cmake` only: overlay patches, the
fetched tree untouched).
- LSan on `adi_pd_engine_tests` (GCC 13.3, ASan, Pd on, JUCE off), against main's overlay: 960 bytes in 276
  allocations (the row's figure). With these patches: **0 leaks**, exit 0, 701 checks pass. Four leaks, each
  fixed where it is made:
  - `g_canvas.c`: an instance's pending canvas arguments (`canvas_setargs`'s copy, one byte even for none, left
    by every abstraction load) are freed in `g_canvas_freepdinstance` (the row's leak, 225 of the 276
    allocations);
  - `s_path.c`: `namelist_append` dropped a duplicate path's string but not its node (an engine adds its patch's
    folder, and a caller may add it again);
  - `x_vexp.c`: `[expr]` kept the copy of each name that turned out to be a function or a variable; it is freed
    once looked up, as a table's already was;
  - `g_scalar.c`: `scalar_new` took a counted gpointer on its glist and never let it go, though Pd's own comment
    there says the copy is thrown away. Every `[table]`'s graph stub leaked. It is let go after `word_init`, as
    `template_conformglist` and `scalar_select` already do;
  - `g_template.c` (the review's finding): one caller relied on that leaked reference. `[struct]`'s `add` made
    its scalar on the `[struct]`'s own canvas and put it on another, so the scalar's arrays named a glist that
    does not hold them. Once that canvas closed, a `[pointer]` walk could reach its freed stub.
    `gtemplate_add` now makes the scalar on the canvas that holds it.
- Run, GCC 13.3, Pd on, at 0271bf12: LSan on `adi_pd_engine_tests`, 0 leaks; the 29 Pd suites pass (`ctest -R pd`), warnings as errors.
- PR requested: `window-fn/df-e67b` → main, "DF-E67 (b): the Pd tier leaks nothing a test can see". Depends on
  nothing beyond main.

**3. DF-E12 (a)(b)(c) → `window-fn/df-e12` 8f62b45b** (two commits) (`src/adi/media/publish_file.cpp`,
`tests/test_project_replace.cpp`, `tests/test_project_files.cpp`).
- (a) A file just made with `O_EXCL` is a new inode, so no lease on it can be rightful yet. A lock refused there
  belongs to the freed file whose number it took, so the create waits for it, for up to two seconds, then
  re-checks the identity as before. The row's other remedy, a descriptor held on the target, is not taken:
  closing one releases SQLite's POSIX record locks (the comment above `identityAt` says so).
- A replace now lets go of the old target's lease right after its rename has freed that inode. So does the copy
  fallback, for its staging source once removed. Neither guards anything once its inode is gone, and the folder
  sync after them has no fixed length (the review's finding: the two-second wait did not bound it).
- (b) A create whose lock failed for any reason but a holder removes the empty file it made, while the name still
  names it. A lock still held after the wait is no stale lease now: it is another process's replace that has
  adopted the new file, and the file is left to it. Unlinking there could remove what that replace had just
  published (the review's finding). So the row's (b) is done for every failure but contention, and contention no
  longer comes from a stale lease.
- (c) `test_project_files` checks `Store::create`'s result before using it.
- Tests (Linux) recycle an inode on purpose. A file's number is freed while its lease is held exclusively, as a
  replace holds it, and the next project file takes that number. Three checks:
  - it opens once the lease is let go;
  - while the lease is held past the wait, it is refused and the file is left to the holder;
  - a project created during a replace's folder sync, on the number its rename freed, opens at once.

  An attempt whose number went elsewhere is tried again. A part that never landed on the number says so in a
  note, and a refused create is never skipped.
- **Run, GCC 13.3, ext4:**
  - `adi_project_replace_tests` 53 checks and `adi_project_files_tests` 54 pass.
  - With main's `publish_file.cpp`, the first and third checks fail.
  - The row's test: in 40 runs of `adi_project_files_tests` beside `adi_project_replace_tests` and
    `adi_settings_tests` in parallel, 0 failures of either.
  - One run of `adi_settings_tests` failed "portable replacement preserves bytes changed after the destination
    lease was released". It did so in 0 of 60 further parallel runs with the fix and 0 of 60 with main's code: not
    reproduced, and not tied to this change.
- PR requested: `window-fn/df-e12` → main, "DF-E12 (a)(b)(c): a project created on a recycled inode number waits
  out the stale lease". Depends on nothing beyond main.

**4. DF-E78 → `window-fn/df-e78`**, two commits on `engine-win/export-profile-validation` 2b3b7e5c, that stack
only.
- b34a2722 calls `explicitDirectTaps` and `directStemBatch` from the suite's `main`. They are tests of the
  stack's direct stems, so they run rather than go.
- 8f44a215, separate so it can be dropped: under libstdc++ 13, `std::bit_width` returns `int`, and
  `aiff_writer.cpp:40` (`bit_width(rate) - 1U`) failed `-Wsign-conversion` under GCC 13.3 and Clang 18.1. The
  stack built with neither, and GCC 12.4 stops at the limiter line main has since fixed. The width is cast first,
  as main's limiter now does.
- **Run:** `adi_render_queue_tests` 600 checks, 0 failures, under GCC 13.3 and under Clang 18.1.
- PR requested: `window-fn/df-e78` → `engine-win/export-profile-validation` (or folded into it), "DF-E78: run the
  two direct-stem tests; the AIFF exponent's width as unsigned". Lands with the export stack, after 3 December.

**5. The parity sheets of #742 → `window-fn/parity-review` d38cc1c9.**
- Each row's ADI column was checked against main's code, and its N, DF and ADR citations against DEFERRED,
  RESERVATIONS and DECISIONS. Each claimed error was read again by a skeptical second reader. Three are
  corrected:
  - recording row 32: the 1/16 Rec-Q default is ADR-0203 d12 and ADR-0132 d8, not d7;
  - export row 2: the page draws only the first four reasons Export is held back;
  - export row 7: the exact range is kept only while both fields are unedited.
- Three claims were refuted and stand (rows 28, 39 and 47 of the recording sheet). The Live column was not judged
  here (no manual on this side). `tools/validate_parity.py`: 11 sheets, clean.
- PR requested: `window-fn/parity-review` → main, "parity-recording and parity-export: three corrections".
  Depends on nothing beyond main.

**RE-01 (`window-fn/remarks`), as ruled:** RE-01 is an M1 row. The branch merges cleanly into main 61676617, and
the merge, built and run locally (GCC 13.3, JUCE on, Pd off), passes the 27 window and mix suites and the shell
suite (3149 checks, 0 failures). PR requested: `window-fn/remarks` → main, "RE-01: remarks from the window (add,
edit, resolve, delete)". The window lane reviews its shell lines.

**5 (P2 window rows):** as logged on 23 November, none in the lane's prefixes has a window half that needs
nothing unbuilt:
- AU-09 comes after AU-03 (the engine's pass coordinator), and its MIDI Learn needs SRF-05's Remote route, whose
  bindings do nothing until SRF-04;
- MI-05 needs the Groove Pool's table;
- MI-07 comes after MI-03;
- MI-09 needs its side table;
- MX-15 and MX-19 wait on Session View and on the engine;
- MX-16 comes after MX-25;
- MX-20 waits on the director's list;
- MX-21 and MX-31 wait on MX-30;
- the SRF P2 rows wait on SRF-04 or SRF-07.

Nothing was started under "lands after 3 December".

## 2026-11-23, window-fn — DF-E71 too, at the director's request; main now builds under GCC 13 (PR requested)

On the same branch as the `adi_play` fix: `tests/scanner_session_reopen.hpp:208` binds the seek pairs by reference
(`const auto &[position,count]`), as DF-E71's row says. The temporary array lives for the whole loop, so nothing
else changes.

**Run** (main dbf98b0c with both fixes, JUCE on, Pd off, warnings as errors, GCC 13, `ninja -k 0`): **the whole tree
builds with no error.** `adi_play_quantize_tests` passes. The 27 window and mix suites pass, and so does the shell
suite (3083 checks, 0 failures).

**`adi_play_render_tests` now builds, and fails one check** that GCC 13 had kept from compiling:
- the check: "same canonical module projects saved alias spelling without rewriting its exact VST3 UID"
  (`tests/test_play_render.cpp:138`);
- what happens: on Linux the test reaches the VST3 fixture through a symlinked `alias.vst3`, and loading by that
  alias fails, with a JUCE assertion at `juce_VST3PluginFormatImpl.h:1117`;
- not from these fixes: it fails the same way built from main's own loop (the copy kept, the warning silenced by
  a pragma, freshly built and run);
- left to its owner, in the needs table.

PR requested: `window-fn/play-indent` (now two commits) → main, "adi_play and DF-E71: GCC 13 builds with warnings as
errors". Depends on nothing beyond main. Closes DEFERRED DF-E71. Outside the lane's files, done because the
director asked.

## 2026-11-23, window-fn — main's GCC 13 build break in adi_play, at the director's request (PR requested)

593bf21f ("Capture driver and callback evidence for hosted graph workers") put a statement on the same line
after an unbraced `if` (`src/juce/play.cpp:233`) and an unbraced `while` (`tests/test_play_render.cpp:269`).
GCC 13 reads both as misleading indentation, so a build with warnings as errors stopped at `adi_play` and its
test. Each statement is now on its own line; nothing else changes.

**Run** (main dbf98b0c with the fix, JUCE on, Pd off, warnings as errors, GCC 13, `ninja -k 0`): the build's only
error left is DF-E71 (`tests/scanner_session_reopen.hpp:208`, engine-win's, left alone). `adi_play_render_tests`
and `adi_play_quantize_tests` pass, as do the 27 window and mix suites and the shell suite (3083 checks, 0
failures). The needs-table row for it is removed.

PR requested: `window-fn/play-indent` → main, "adi_play: one statement per line after an unbraced if and while
(GCC 13)". Depends on nothing beyond main. Outside the lane's files, done because the director asked; the seat of
593bf21f may prefer to take it.

## 2026-11-23, window-fn — five branches that no longer merged into main: three merged with main, two superseded

Main moved past several of the lane's requests. Each branch below was checked against main c0a8f9bb with
`git merge-tree`. The other twenty window-fn branches merge cleanly.
- **`window-fn/tap-quiet` → 96ad25fc** (HI-01). Main's DF-W29 settles held knob edits in the same two places.
  `restoreStep` now ends the tap run, then settles; a tap settles before the run's gesture or its step.
- **`window-fn/split-pan` → 5c583341** (MX-02). Main made the pan and fader `PrimaryButtonSlider`s (AU-04: a
  right press opens the menu and moves nothing), so the split halves are too. The pan menu keeps its source, and
  the volume menu comes from main.
- **`window-fn/send-pan` → 1a9e2dab** (MX-06). The send's menu keeps the Send Pan submenu and main's three send
  taps (DF-W33).
- **Superseded, nothing to land:**
  - `window-fn/play-first-block-main`: DF-E64 landed through engine-win's own reconcile (a7983164).
  - `window-fn/lane-ref-canonical`: an earlier DF-W38, which landed as `window-fn/df-w38-param-ref`.
  - `window-fn/function-keys` and `window-fn/shortcut-overrides`: withdrawn before.

**Run** (each merge, JUCE on, Pd off, warnings as errors, `ninja -k 0`): the 27 window and mix suites pass, and the
shell suite from a scratch directory passes with 0 failures (3083, 3093 and 3083 checks).

**Main does not build under GCC 13 with warnings as errors** (`-Wmisleading-indentation`, 593bf21f "Capture driver
and callback evidence for hosted graph workers"): `src/juce/play.cpp:233`
(`if(!text)return false;const auto length=...`) and `tests/test_play_render.cpp:269`
(`while(std::getline(csv,line))++lines;if(lines!=3)return 1;`). Each wants the statement after the `if` or `while`
on its own line. It is in the needs table. The suites above do not use those targets.

PR requested as before for the three: `window-fn/tap-quiet`, `window-fn/split-pan`, `window-fn/send-pan`.

## 2026-11-23, window-fn — what is left in the lane's prefixes, and what each row waits on

The queue of 22 Nov is done, and so is every unbuilt window row of M1 to M3 in MX, SRF, AU, ED, MI and EX that
waits on nothing. What remains, row by row (main 4836cb61):
- **Waiting on the engine:**
  - SRF-06, the Focus Dial: SRF-04's resolver and takeover gate are not built.
  - SRF-05's Remote route, the binding half: a bound control needs SRF-04 before it can become an op.
  - ED-05, Slip: no slip intent or op.
  - ED-06, Consolidate: no `clip.consolidate`.
  - ED-07 and ED-08: the planner's ripple and its frozen and grouped policy.
  - AU-03's pass coordinator: guards, latch, loop and punch.
  - AU-08's lane-scoped range commands: the time edits' automation splice.
  - AU-07: AU-06's composition of clip gain, its modulation and the curve.
  - EX-07, and EX-02 to EX-09.
- **Waiting on the window lane's placement:** MX-11's summing controls (ADR-0243); the drive's gesture is on
  `window-fn/summing-drive`.
- **M4's MX rows:** each waits on MX-22's or MX-33's measurement, or on MX-30's taps, except MX-24, which is
  built.
- **Built on main since the PLAN's audit:** MI-04 (Split, Chop, Join, Reverse, Invert, Legato, Fit to Scale and the
  rest are in `note_edits.hpp`), MX-24 (the device's Sidechain Input menu on `prepareDeviceKey`), and AU-03's
  window half (Automation Arm).

Until a row frees up, the lane takes the next PLAN rows of P2 (AU-09, MI-05, MI-07, MI-09, MX-15, MX-16, MX-19,
MX-20, MX-21, MX-31, SRF-07 to SRF-10, SRF-13 to SRF-15) whose window half needs nothing unbuilt.

## 2026-11-23, window-fn — PLAN AU-08 (window half): Simplify Envelope and Delete Automation (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/automation_mode.hpp` (`simplifyAutomation`,
`simplifyTolerance`, `deleteAutomation`; `envelopeOf`'s disabled-lane match now uses `sameParam`),
`src/juce/ui_shell.*` (`simplifyAutomation`, `deleteAutomation`; Delete Automation added to
`addReenableAutomation`, and so to every automation menu) and `src/juce/ui_arrangement.cpp`
(`automationLaneAt`).

- **Simplify Envelope** sits on the lanes' menu beside Insert Shape. Over the time selection, the breakpoints that
  a straight line between their kept neighbours already draws, within half a percent of the parameter's travel
  (0.38 dB on a fader), go: Ramer-Douglas-Peucker on the value, with an explicit stack.
  - The line is measured where the engine plays it, in seconds through the tempo map, so a point on the line in
    ticks but not in time stays.
  - The selection's first and last breakpoints stay, so the curve outside is untouched, and so do both points of
    a jump.
  - It is refused, with the reason, on a switch's lane, on a selection holding a hold or a bent segment, and
    where there is nothing to remove.
  - It is one undo step, "Simplify Envelope".
- **Delete Automation** sits beside Re-Enable Automation in every automation menu (the lanes', a fader's, a pan's,
  a send's, a device parameter's). Every lane of that parameter goes, its clips' envelopes with it; a device
  parameter's ref is matched as the engine binds it. It is one undo step.
- **The lanes' menu** now acts on the lane the track shows (DF-W37's chooser), not always its Volume. Re-Enable
  Automation there follows too.
- **Not here:** AU-08's lane-scoped range commands reuse the time edits' automation splice, which is the
  engine's (needs table).

**Test** (`tests/test_ui_shell.cpp`, after Insert Shape):
- on the ramp, the two breakpoints lying on it go and the ramp plays the same;
- a Sine simplified keeps more than ten points, every old one within the 0.38 dB pinned in the test, the outside
  unchanged; Undo gives them all back;
- a Square's holds are refused;
- a 0.2 dB jump inside the selection keeps both its points;
- across a tempo change from 120 to 60 BPM, a point on the line in ticks stays;
- Delete Automation from the lane's menu: one step, and Undo with its points;
- the fader's menu now holds both items.

**Review** (three dimensions, each finding checked by a skeptic): five findings, all fixed in the branch's one
commit:
- collinearity was measured in ticks (two reviewers);
- Delete Automation matched a device ref by its spelling;
- the sine check could pass at any tolerance;
- the jump rule was never exercised.

Five were refuted, among them that Undo of Delete Automation loses the lane's override (the session's override
is runtime state, as Live's).

**Run:** JUCE on, Pd off, warnings as errors (GCC 13, Debug, `ninja -k 0`): the 27 window and mix suites pass, the
shell suite 3048 checks from a scratch directory, 0 failures.

PR requested: `window-fn/simplify-automation` → main, "AU-08: Simplify Envelope and Delete Automation". **Depends
on `window-fn/insert-shape`** (AU-02; one commit on top of it).

## 2026-11-23, window-fn — PLAN AU-02 (window half): Insert Shape on a lane's time selection (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/automation_mode.hpp` (`playedEnvelope`,
`kAutomationShapes`, `insertAutomationShape`), `src/juce/ui_shell.*` (`insertAutomationShape`,
`addInsertShape`) and `src/juce/ui_arrangement.*` (the lanes' menu; a right-click inside the time selection keeps
it, `keepsSelectionAt`; `menuTrack`).

- **The menu.** In Automation Mode, with a time selection holding the track right-clicked, the lanes' menu has
  Insert Shape with Live's ten shapes in two rows: Sine, Triangle, Sawtooth, Inverse Sawtooth and Square; then
  Exponential and Logarithmic Rise and Fall, and ADSR. A right-click inside the time selection now keeps it
  (before, it collapsed the selection, or took a clip's span, before the menu was built).
- **The edit.** The engine's `automation::generateShape` makes the points. The window removes the breakpoints
  inside the selection (edges included), writes the shape, and makes the lane and its data first where the
  envelope has none. It is one batch and one undo step, "Insert Shape", with ids from `RowIds`.
  - The top row spans the parameter's travel (the fader's foot written as silence).
  - Ramps join the envelope's values at the edges: a rise from the lower to the higher, a fall from the higher
    to the lower. On a flat envelope, a rise goes to the top and a fall to the foot.
  - The ADSR rises from the lower edge value to the top.
- **Guards.** The curve outside stays as the engine played it: a breakpoint at each edge holds the value
  `engine::valueAt`'s rule gives there (the left point's curve, a hold kept, the last point at a time winning),
  as stored (a ramp from silence keeps its foot). The end's guard has the curve and tension of the segment it
  played from, unless the shape already ends on it. This is exact where the segment crossing an edge holds or is
  straight; a bent one keeps its ends and its kind, not its exact bend.
- **Asked of the engine (needs table):** the PLAN's guards "through one shared splice" that keep a bent
  segment's bend (ADR-0159's slices) would let the window drop its guards.
- **For the director (L-AUTO-8, unresolved):** which ends Live's ramps and ADSR join. The window's rule is the
  one above; a reading of Live 12 decides it.

**Test** (`tests/test_ui_shell.cpp`, the lane chooser's block), on a Volume ramp with old breakpoints on the
selection's start and inside it:
- Sine: one undo step, 131 points inside (129 and two guards), the old ones gone, top +6 dB, foot silence, the
  ramp outside unchanged at both edges; Undo gives the four breakpoints back;
- Exponential Rise: two points joining the edge values, no guard;
- a Sine inside a Square's hold leaves the hold after it as played;
- a shape after a ramp from silence leaves that ramp as it was;
- a too-short selection is refused with the reason;
- the submenu's ten items and one separator; none for a track outside the selection or outside Automation
  Mode;
- the right-click rule inside and outside the selection.

**Review** (three dimensions, each finding checked by a skeptic): six findings, all fixed in the branch's one
commit:
- a right-click wiped the selection;
- the guards read a straight line where the segment held;
- a point on the end edge lost its curve kind;
- the guards ignored the segment's curve;
- a guard below the travel was pulled to the foot;
- the deletion of old points was untested.

Five were refuted.

**Run:** JUCE on, Pd off, warnings as errors (GCC 13, Debug, `ninja -k 0`): the 27 window and mix suites pass, the
shell suite 3035 checks from a scratch directory, 0 failures.

PR requested: `window-fn/insert-shape` → main, "AU-02: Insert Shape on an automation lane's time selection".
Depends on nothing beyond main. With `window-fn/automation-curves` landed too, both touch
`automation_mode.hpp`'s end (a textual conflict; keep both).

## 2026-11-23, window-fn — PLAN ED-04 (window half): a clip's start moved, its material anchored in time (PR requested)

**Window lane, read first:** this item touches `src/juce/ui_arrangement.*` (the clip bar's left edge, the resize
drag, release, keys and paint; `clipEdit` removed, no caller left) and `src/juce/ui_shell.*` (a new
`resizeClip`).

- **The left edge.** The clip bar's left 4 px (on a clip at least 16 px wide; the right edge wins on a narrower
  one) now drag the clip's start, its end fixed, to the grid while snapping, never shorter than a grid step (or
  than the clip, when it is shorter). The release goes through the engine's planner, `edit_plan::prepareTrim`:
  the content offset moves with the edge, so the material stays where it is in time. Audio stops at its
  source's first frame; MIDI does not. It is one undo step, "Resize Clip". A start moved before the audio's
  first frame is refused: "The clip's audio begins there; its start cannot move earlier."
- **Enter at a clip's start.** It no longer says "waits for a content offset". It begins the same resize from the
  keyboard: Left and Right move the start by the grid, Enter keeps it, Esc cancels. The insert marker lands on
  the start as it landed. Where one clip ends and the next begins, Enter takes the one that ends there (as
  before) and Shift+Enter the one that begins there.
- **While an edge moves,** the clip's outline follows it and its material is drawn where it is in time.
- **A resize moves no envelope,** so the planner is asked with automation-follow off, as a recording's placement
  is. With the default setting it otherwise refused every trim on a track that has an automation lane.
- **The right edge** now goes through `edit_plan::prepareResizeEnd`, so a clip lengthened over the next one
  clears what it now covers, as Live (the next clip begins where the first now ends, the same undo step). Where
  the planner will not place (automation that follows its clips, a frozen track, an editing setting off Live's
  rule), the end moves by `clip.resize` as before and the Status Bar says why. The start has no such fallback:
  without the planner it would slide the material. Nor does a clip on the seconds clock (a bounce, a freeze):
  `clip.resize` writes ticks, so on main its right edge wrote a tick span onto a seconds row; it is now left as
  it is, with the reason. Its scope is padded one tick, since its span shows floored to ticks.
- **Not here (ED-04's other half):** Crop of a clip in the Arrangement (Live's rule: unlooped start to end,
  looped to the loop) waits for a planner intent; `edit_plan.hpp` names it as separate and has none yet. The
  Clip View's note Crop is on main already.

**Test** (`tests/test_ui_shell.cpp`, the Arrangement's clip editing):
- a drag on the left edge moves the start to the grid, the end where it was, and `content_offset_ticks` by
  exactly the ticks trimmed; one undo step; Undo restores the start and the offset;
- Enter at the start, then Right, then Enter: the start one grid step later, the end fixed, the insert marker
  there; Undo;
- with the clip two steps in, a start one step earlier than its first frame is refused, nothing written, the
  Status Bar names it;
- the right edge lengthened two steps over a following clip trims that clip to begin where the first ends, one
  undo step; Undo gives both back; where the two clips now meet, Enter takes the first and Shift+Enter the second;
- a trim on a track with a Volume breakpoint moves the start, one undo step (without the automation-follow fix
  this check fails).

**Review** (three dimensions, each finding checked by a skeptic): five findings, all fixed in the branch's one
commit:
- the drag's preview slid the material with the edge;
- back-to-back clips left Enter no way to reach the second clip's start (Shift+Enter now);
- a seconds-clock clip's scope missed its real end, and its fallback wrote ticks;
- with the default Lock Envelopes setting, a trim on an automated track was always refused;
- a trim stopped at the audio's first frame could still commit an overlap's clearing as "Resize Clip".

Four were refuted.

**Run:** JUCE on, Pd off, warnings as errors (GCC 13, Debug, `ninja -k 0`): the 27 window and mix suites pass,
the shell suite 3029 checks from a scratch directory, 0 failures.

PR requested: `window-fn/left-trim` → main, "ED-04: a clip's left edge and Enter at its start, through the
planner". Depends on nothing beyond main.

## 2026-11-23, window-fn — DF-W39: the seams suite builds with GCC 12 (PR requested)

`CMakeLists.txt` added `-fsanitize-trap=signed-integer-overflow` (DF-W35 (b)'s test) for every compiler but MSVC.
GCC accepts that option only from version 13. The trap now goes where the compiler has it, checked with
`check_cxx_compiler_flag` under `-fsanitize=signed-integer-overflow`:
- `-fsanitize-trap=signed-integer-overflow` where it exists (GCC 13, Clang);
- otherwise `-fsanitize-undefined-trap-on-error` (GCC 12's spelling, the same trap for this one sanitizer);
- otherwise no trap flag.

**Run:** `adi_ui_seams_tests`, warnings as errors, 568 checks, 0 failures, with GCC 12.4 (the check picks
`-fsanitize-undefined-trap-on-error`) and with GCC 13 (it keeps `-fsanitize-trap=signed-integer-overflow`). The
GCC 12 build stops before the suite at a limiter line of the mix lane's (`FloatRealTransform.h:61`, `-Wsign-compare`);
the run made a one-line cast there locally and did not commit it. That line is in the needs table for the mix seat.

PR requested: `window-fn/seams-gcc12` → main, "DF-W39: the seams suite's overflow trap where the compiler has it".
Depends on nothing beyond main. Closes DEFERRED DF-W39.

## 2026-11-23, window-fn — PLAN ED-09: object snapping for a dragged clip (PR requested)

**Window lane, read first:** this item touches the clip drag in `src/juce/ui_arrangement.cpp` (where `movingTo_` is
set from the grid) and adds `src/adi/ui/object_snap.hpp` (no JUCE).

- **What it does** (Live's object snapping; ADR-0129). With snapping on, a dragged clip's start or end lands on a
  nearby object when it comes within a few pixels, ahead of the grid. The modifier that turns the grid off turns
  this off too. Nothing in the engine changes.
- **The candidates** come from the snapshot:
  - the starts and ends of every clip on the tracks the lanes show, except the dragged ones;
  - the locators the Arrangement draws (`locatorsOf`);
  - the time signature changes.
- **A clip in seconds** snaps by its start only: its length in ticks changes with the tempo where it lands.
- **The distance** is 8 pixels, provisional until Live's is measured (M6-A6C-077).
- **Test** (seams):
  - the candidates, with a cycle marker, a locator in seconds and a hidden track left out;
  - the shift, forwards and backwards;
  - the nearer edge, either way, with the start winning a tie.
- **Review** (three dimensions, each finding checked by a skeptic). Fixed:
  - every marker kind was a candidate;
  - clips on hidden tracks were candidates;
  - a clip in seconds snapped its end by a length it does not keep;
  - three test gaps.

  Three findings refuted; one (rebuilding the candidates per mouse move) judged no worse than the handler's existing
  walk.
- **Run:** JUCE on, Pd off, warnings as errors (`ninja -k 0`): seams 575 checks; 27 of 27 suites; shell 3018
  checks; 0 failures.

PR requested: `window-fn/object-snap` → main, "ED-09: object snapping for a dragged clip". Depends on nothing beyond
main.

## 2026-11-23, window-fn — PLAN SRF-01: the feed says which parameter was last moved from ADI's controls (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/parameter_feed.hpp` (`touched` takes a source) and
`src/adi/ui/project_document.cpp` (`parameterGesture`, `parameterPairGestureExpected`).

- SRF-01's other parts were already on main: the steady-clock events, and committing what was appended even when
  an event is refused. What was left was that `ParameterFeed::touched` had no caller, so every parameter's
  `lastTouchedBy` read None.
- A surface event the document takes now marks its parameter as touched by ADI's own control. A paired EQ drag
  marks both of its parameters (review fix). A control surface can pass `Source::Surface`. An automated parameter
  that follows its lane still reads Automation.
- **Test** (shell, the strip drag): after the drag, only Cutoff reads Mouse.
- **Review:** two findings confirmed and fixed (the paired drag; the test naming the parameter), two refuted.
- **Run:** JUCE on, Pd off, warnings as errors (`ninja -k 0`): 27 of 27 suites; shell 2955 checks; 0 failures.

PR requested: `window-fn/param-touched` → main, "SRF-01: the feed says which parameter was last moved from ADI's
controls". Depends on nothing beyond main.

(`window-fn/shortcut-overrides`, withdrawn above, now carries its review fixes at `49bbc68e`, run the same way: 27 of
27 suites; shell 2968 checks; 0 failures. It is for the window lane to take from.)

## 2026-11-23, window-fn — three branches merged with main, run and pushed (PR requested as before)

Each was built alone on its merge with origin/main `4836cb61` (`ninja -k 0`, JUCE on, Pd off, warnings as errors).
The 26 window and mix suites with `ui_seams` (27 of 27) pass, and the shell suite runs from a scratch directory, 0
failures:
- `window-fn/tap-quiet` `b94eb8e8`: shell 3018 checks.
- `window-fn/loop-record` `d1e692f8`: shell 3019 checks.
- `window-fn/automation-curves` `f170fbe3`: shell 3018 checks.

Their "PR requested" lines above stand, at these heads.

## 2026-11-23, window-fn — the split of 22 Nov read again: WIN-04 and WIN-09 withdrawn, RE-01 offered

The window log's split (22 Nov) gives window-fn the MX, SRF, AU, ED, MI and EX rows. The window lane keeps the
shell and the test kit, and lists WIN-01, WIN-09, WTS-02, WIN-04, WIN-02 and WIN-03 as its own. I took three of
them from PLAN after my queue ran out, without reading that list.
- **WIN-04** `window-fn/shortcut-overrides` (`c7beaf38`, pushed): withdrawn; close it unmerged in favour of
  `window/shortcut-ids`. What the window lane may take from it:
  - The overrides re-applied after every reload.
  - `setShortcut` comparing with the defaults.
  - Review fixes, local `49bbc68e`, unpushed:
    - the latest assignment holds (a new key is taken from other overrides);
    - a chord must read back as itself (JUCE reads most unknown text as its last letter);
    - a refused chord leaves the command its own keys;
    - a user's key for Fold to Notes, Fold to Scale, Highlight Scales and Draw Mode works (`AppCommands::key`
      held back every key bound to them, not just their plain letters);
    - problems are said when the Settings change.
- **WIN-09** `window-fn/help-menu` (`4ee1578e`, local, not pushed): withdrawn in favour of `window/help-menu`.
- **WIN-01** as in the entry below.
- **RE-01** is in neither list. `window-fn/remarks` (now pushed at `a1e78539`, with review fixes: a remark's time in microseconds as the format's, no id past INT64_MAX, one line in the menu; 27 of 27 suites, shell 3020 checks, 0 failures) holds its function half: Add, Edit,
  Resolve or Reopen, and Delete from a track's and a clip's menu, each one undo step (`remark_edits.hpp`), with
  seams and shell tests. Its pips and Info View are looks. **For win:** whose row is RE-01? If window-fn's, I push
  and request it; if the window lane's, it can take the branch.
- **From here** I take only MX, SRF, AU, ED, MI and EX rows. Next is ED-09 (object snapping), already written.

## 2026-11-23, window-fn — win's entry of 23 Nov: DF-E64 reconciled with main, three branches merged with main, WIN-01 withdrawn

- **`engine-win/play-first-block` (DF-E64), reconciled** as win asked: `window-fn/play-first-block-main` `848bf6b9`
  is `01b1e723` with origin/main `4836cb61` merged. The one conflict was `TransportMailbox::drain`
  (`window_state.hpp`): #710 stores each Locate's revision for the Mix View's `settledLocateRevision`, and DF-E64
  rewrites the same branches to hold Play. Both are kept: the Locate branch stores the revision, and Record, Stop and
  Play are DF-E64's.
  - **Run:** JUCE on, Pd off, warnings as errors. The 26 window and mix suites with `clip_candidate_ready` pass (27
    of 27), and the shell suite passes (3018 checks, 0 failures).
  - **WTS-01** (Pd on, acceptance on): 2a PASS. 2b now stops on the harness's own check: "pending Play advanced the
    playhead or leaked audio". **Finding for win** (`tests/acceptance/wts01_harness.hpp`, `firstPlayingBlock`, which
    is win's): main's tracks now monitor on Auto (N67, landed), and 2a leaves track 4 armed ("armed 1 (monitor
    Auto)"). While Play is held, the transport is stopped, so an armed track on Auto hears its input, as Live's does.
    The check reads any sample during the hold as a leak. Suggest 2b set track 4's monitoring to Off, or disarm it,
    before its Play: the step already expects "the live input is not heard". I did not verify the fix, since the
    file is win's.
  - PR requested: `window-fn/play-first-block-main` → main, "DF-E64 on current main: keep #710's Locate revision and
    the held Play", in place of `engine-win/play-first-block`'s own PR (or merged into it).
- **Merged with main** (each conflict resolved; runs follow in the next entry): `window-fn/tap-quiet` (`ui_main.cpp`'s
  Quit and project switch now end a Tap Tempo run beside main's settled-take report), `window-fn/loop-record` (main's
  RowIds for linear takes with the loop branch's own; its refusal now goes through `discardTake`), and
  `window-fn/automation-curves` (`envelopeValueAt` beside the lane chooser's `trackEnvelope` and its new
  `editableEnvelope`).
- **WIN-01 withdrawn from my list:** win's split gives it to the window lane (`window/function-keys` `724851f5`).
  Close `window-fn/function-keys` `97036ee9` unmerged. One thing in it the window lane may want: the shell check over
  every preset's default map (`functionKeys`: no key is two commands' default; F1 to F8 activate no track; F4 is the
  Mix View; F3 and F7 are free), and the activator check that ran on F3 now running from its command.
- **DF-W33** is the window lane's, as win says; nothing of mine.

## 2026-11-23, window-fn — queue items 14 to 18 (PR requested; 14, 15, 16 and 17 land after their engine branches)

**Run for the five, built together** (`ninja -k 0`, JUCE on, Pd off, warnings as errors): seams 571 checks; the 26
window and mix suites with `note_diff`, `wav_file`, `render_queue` and `normalization` (30 of 30); shell 2958 checks;
0 failures. **Review** (three dimensions, each finding checked by a skeptic): six findings confirmed and fixed (under
each item), three refuted.

- **Item 18, AU-01 curves** — `window-fn/automation-curves` (from main), "AU-01: draw curved automation segments as
  the engine plays them". **Window lane, read first:** `src/adi/ui/automation_mode.hpp` (`envelopeValueAt`) and the
  Automation Mode painter in `src/juce/ui_arrangement.cpp`.
  - Every segment is traced every two pixels with the engine's own evaluator (SPEC 6.3.2, `curveSliceValue`): the
    outgoing point's curve and tension, and a trimmed source's curve, measured in seconds as the engine places points
    in samples. A linear segment bends where the tempo changes inside it.
  - While a point is dragged, a segment keeps its trimmed source's curve only where `automation.movePoints` will
    keep it.
  - Test (seams): the drawn value equals `engine::valueAt` within 0.002 dB for every curve and three tensions, across
    a tempo change inside the segment.
  - Review fixed: linear segments were straight joins in ticks; the drag preview kept slices the move drops.
- **Item 17, N68** — `window-fn/open-saved-path` on `engine-win/project-plugin-saved-path` `43787b7b` plus main,
  "N68: an open after a plug-in scan loads a plug-in by its saved path".
  - The open rule already runs a project only once a job is adopted, with no job beside it, which is exactly when
    N68's loader probes a saved path. The comment and the shell check now say so. The stand-in discovery port
    cannot load a real plug-in, so the load itself is engine-win's test (`scanner_discovery.hpp`).
  - Needs row "after a discovery job, an ordinary open should still probe a saved path" closes when N68 lands.
  - Lands after N68.
- **Item 16, EX-06** — `window-fn/shaped-dither` on `engine-win/export-quantizer` `7a9fe563` plus main, "EX-06: the
  Export page offers the shaped dithers". **Window lane, read first:** `src/adi/ui/export_choices.hpp` (the list's
  names).
  - Live's three POW-r slots are ADI's three noise-shaped dithers, listed as Shaped 1, 2 and 3 (the engine's are
    SoX shaping tables, not POW-r).
  - They are no longer refused, except at a rate their tables do not cover, for a 16 or 24-bit file. There the
    engine would dither triangular without a word, so the page says so instead (review fix).
  - Test (shell): Shaped 2 renders at a covered rate; the uncovered rate is refused; a 32-bit float file is not.
  - Lands after `engine-win/export-quantizer`.
- **Item 15, MI-01** — `window-fn/note-batch` on `engine/n63-ordered-note-batch` `c97f2a96` plus main, "MI-01:
  every note batch is one note.applyDiff". **Window lane, read first:** `AdiRootComponent::submitEdits` in
  `src/juce/ui_shell.cpp`.
  - A batch of scalar note ops on one clip goes through `notes::prepareBatch`, which applies the legacy ops in turn,
    each with its overlap rule. It commits as one `note.applyDiff`: the stream is written once, one op in the
    history. A clip with no notes yet gets its stream under the next free id. Other batches are as before.
  - The 13 checks that failed the November translator pass all pass: moved into a longer note, chop, stretch, Fit to
    Time Range, Set Length.
  - The shell's note checks read a step through one helper: an applyDiff step must hold one op whose sequence has as
    many steps as the edit's legacy ops. A note added to a new clip must be one `note.applyDiff` (review fixes).
  - Lands after N63.
- **Item 14, N64** — `window-fn/n64-snapshot-arm` on `engine/n64-recording-snapshot` `8a32d9e3` plus main, "N64: Arm
  and monitoring read the snapshot". **Window lane, read first:** `src/adi/ui/arm_edits.hpp` (`recordStatesOf`),
  `src/juce/ui_shell.*`, `src/juce/ui_main.cpp`.
  - The strips' Arm and monitoring, their actions, and the document's armed tracks for Record read the frame's
    snapshot.
  - The once-a-frame handoff of the session's rows and the root's `readRecordStates`/`setRecordStates` go.
  - Lands after N64. WTS-01 2a was not run on this branch: it needs the Pd tree and the engine branch merged, and
    the Record path reads the same states.

## 2026-11-23, window-fn — PLAN WIN-01: the function keys do functions (PR requested)

**Window lane, read first:** this item touches `src/juce/ui_shell.cpp` (`AppCommands::getCommandInfo`) and a shell
check.

F1 to F8 were Live's Activate Track 1 to 8 by default, and F4 was also the Mix View (ADR-0209 d9), so the default
map bound F4 twice. The eight activators keep their commands, for the palette and the Shortcuts page, but lose
their keys. F4 opens the Mix View, and F3 and F7 stay free for Session View and offline processing.

**Test** (`tests/test_ui_shell.cpp`, `functionKeys`), for every preset's default map (Live, Cubase, Bitwig): no key is
two commands' default; F1 to F8 activate no track; F4 is the Mix View; F3 and F7 are free. The activator check that
pressed F3 now finds F3 does nothing and runs Activate Track 3 from its command.

**Run:** JUCE on, Pd off, warnings as errors (`ninja -k 0`): the 26 window and mix suites pass, and the shell suite
from a scratch directory passes (2963 checks, 0 failures).

PR requested: `window-fn/function-keys` → main, "WIN-01: the function keys do functions". Depends on nothing beyond
main. The parity-7.6 row 6 amendment the PLAN row names is the window lane's checklist.

## 2026-11-23, window-fn — PLAN MX-06 (half): a pan on a send (PR requested; Send to Track parked)

**Window lane, read first:** this item touches `src/adi/ui/send_edits.hpp` (a new `setSendPan`) and
`src/juce/ui_mixer.*` (the send knob's menu).

- **A pan on a send** (ADR-0203 d3). The send knob's menu gains Send Pan, with Live's five most read places (Left,
  50L, Center, 50R, Right), the current one ticked.
- A pick is one undo step, "Change Send Panning": the row's pan. A send with no row gets its row first, fully down
  and following its return's switch, so a pan alone never makes a silent send heard. A pan that would not change
  writes nothing.
- **Parked: Send to Track…** (a send into any track). The strip lays out one knob per return; a send into an
  ordinary track needs a knob of its own on the strip, which is the window lane's to place. Once it has one, the
  connect is `setSend`'s with any destination.

**Test** (`tests/test_ui_seams.cpp`): a row's pan, the same pan again, the clamp at Left, the centred rowless send, a
rowless send panned Right (made fully down, then panned, one step), and Undo.

**Run:** seams 559 checks; the 26 window and mix suites; shell 2954 checks; 0 failures.

PR requested: `window-fn/send-pan` → main, "MX-06: a pan on a send, from the send knob's menu". Depends on nothing
beyond main.

## 2026-11-23, window-fn — queue item 12, CT-02's window half: Record with the loop on, a take per pass (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/project_document.cpp` (`startRecording`, `landTake`) and
replaces the shell check that expected the old refusal.

- **Record with the loop on.** It is no longer refused. The window freezes the loop in samples as the engine plays
  it: the engine's own tempo map (the stored events, 120 BPM only for a project with none, as `Session`'s
  TimingView builds it) and `sampleForTick`'s rounding, with a sub-sample loop playing one frame. It passes this to
  the recorder as `AudioRecordingSpec::loop`.
- **A loop take lands** through `recording::prepareLoopCommit`:
  - each pass is a take on a take lane of its track, and the last pass's audio is copied to the main lane;
  - each track gets one media, and lane, clip and media ids come from RowIds;
  - the engine commits one undo step per pass;
  - the clip chosen afterwards is each track's main-lane copy, the one that plays;
  - the Status Bar says "Recorded N passes of the loop, a take each."
- **The queue's wording:** its test says the takes and the main-lane copy land "in one undo step". The engine's
  contract (`prepareLoopCommit`, `recording_commands.hpp`) is one undo per pass. The window follows the engine.
  If Live's is one step for the whole recording, that is the engine's to change; see the needs table.
- **Test** (`tests/test_ui_document.cpp`, `loopRecord`): a one-beat loop at 120 BPM, recorded for 760 blocks of 64
  frames: two whole passes and 640 frames of a third. It expects exactly three takes on three take lanes, main-lane
  audio, the main-lane clip chosen, and three undo steps that take everything back. The shell's Record check now
  expects Record with the loop on to record.
- **Review** (three dimensions, each finding checked by a skeptic). Fixed in `e2164d45`:
  - the frozen loop used the snapshot's tempo map, which puts a 120 at the start of a project whose first stored
    tempo comes later, so such a project's loop never recorded;
  - ids were MAX+1 rather than RowIds;
  - the take-lane clip, not the main-lane clip, was chosen.

  Two findings are the engine's (needs table): a tempo or loop change during a loop recording, Tap Tempo
  included, and running out of passes each end the capture and lose every pass recorded so far.

PR requested: `window-fn/loop-record` → main, "CT-02: record with the loop on, a take per pass". Depends on nothing
beyond main. With `window-fn/tap-quiet` landed too, a Tap Tempo run during a loop recording ends that capture
(the engine's retime rule above).

## 2026-11-23, window-fn — queue item 11, HI-01: Tap Tempo through a quiet gesture (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/project_document.*` (`tapTempo`, `pollTapTempo`,
`endTapTempo`), `src/juce/ui_shell.*` (the root's `tapTempo`, `endTapRun`, two hooks) and `src/juce/ui_main.cpp` (the
hooks, the timer's poll, ending a run before a project goes or a copy is saved).

A run of taps was a commit per tap, each undone and committed again by the next tap, which forked the history at
every tap. The run is now one quiet gesture (`gesture::QuietSession`) on the tempo event in force at the playhead,
or an inserted one where the start's 120 is only the snapshot's.
- Every tap is heard at once (a structural preview: tempo has no live-value path) and nothing is written.
- The application's timer ends the run Tap Tempo's own pause (2 s) after its last tap, as one undo step "Tap Tempo".
- A late tap ends a run and begins another.
- The run is ended first by Undo, Redo, a jump in the Undo History, Save As, Save a Copy, Open, New and Quit (an
  open gesture refuses history moves, and a document closed with it open would cancel the tempo heard).
- A tap that rounds to the same tempo keeps the run going.
- A run whose end is refused says so.

**Test** (`tests/test_ui_document.cpp`, `tapTempoQuiet`): four taps heard with nothing written; still one run short
of the pause; at the pause one step at 103 and one line in the history's timeline; Undo to 120; a late tap's run as
its own step; Undo to the first run's 90.

**Review** (three dimensions, each finding checked by a skeptic): five findings, all fixed in `608e3c02` (the history
moves, the project switch and Quit, same-value taps, the silent refused end, the second run's test).

PR requested: `window-fn/tap-quiet` → main, "Tap Tempo through a quiet gesture: one undo step, no forked history".
Depends on nothing beyond main.

## 2026-11-23, window-fn — queue item 10, MX-23 (drive): a summing group's drive heard while dragging (PR requested, the dial parked)

- **The document half needed no code.** `ProjectDocument::beginMixerDrag` already takes any of the gesture
  producers' scalar kinds, `GroupSummingDrive` included. The branch adds the test the queue asked for
  (`tests/test_ui_document.cpp`, in `mixerDragHeard`): two tones in a summing group. A console's channel and buss
  stages cancel for one source; the drive colours a sum. Dragged to +24 dB, the waveform moves (by up to 0.124) with
  nothing written. Esc gives the clean tone back. The release is one op and one undo step, heard as it was let go,
  and Undo restores both.
- **Parked: the drive dial.** No window control writes the drive. MX-11's summing controls (on/off, the flavour, the
  drive dial) are not built, and where the dial sits on a group's strip is the window lane's (ADR-0243). The dial's
  gesture is a few lines on the strip once it has a place: `beginLive(gesture::ScalarKind::GroupSummingDrive,
  track_, {-12, 24, std::nullopt}, "Change Summing Drive")`, as the fader does. The window reads the drive from
  `Session::model().summing` until the snapshot carries it.

PR requested: `window-fn/summing-drive` → main, "Test a summing group's drive heard while it is dragged (MX-23)".
Depends on nothing beyond main.

**Run for items 10, 11 and 12, built together** (they touch different functions; the one shared test file,
`tests/test_ui_document.cpp`, takes a two-line conflict in `main`, where all three calls stay): JUCE on, Pd off,
warnings as errors, `ninja -k 0`. the 26 window and mix suites pass, the document suite 338 checks and the shell suite 2955 from a scratch directory, 0 failures. WTS-01 (Pd on, acceptance on): 2a PASS; 2b FAILs as on main (DF-E64).

## 2026-11-22, window-fn — review of items 3, 5, 6 and 7: four findings fixed (branches updated, PR requested as before)

A three-way review (correctness, behaviour, test validity), each finding then checked by a skeptic, ran over the
four branches merged on main. Four findings held up; each is fixed by a new commit on its branch. Item 5 had none.
- **`window-fn/saveas-guard` `10ad6d32` (item 3).**
  - The take that Save As landed was told by the timer on its next tick, which replaced Save As's own warning or
    failure in the Status Bar. The app's Save As now takes the outcome itself: a take that was not kept leads the
    message, and a kept take is chosen.
  - "Recording stopped, its take kept" was said even for a take that was dropped. It is now said only when the
    take landed.
  - New document case: Save As after a Record that recorded nothing.
- **`window-fn/midi-port-name` `e699a94a` (item 6).** A record at an identifier that now lists another device was
  stamped with that device's name, so its own device could never find it again. A port's own record is now one that
  names it, or names nothing yet. A record naming another device keeps its name and can be adopted by that device at
  its new identifier. Moves are read from the record as it was, so two devices that swapped sockets both keep their
  switches. New cases: a reused identifier, a return, a swap.
- **`window-fn/reenable-lane` `682d9d3c` (item 7).**
  - The slider filtered each event by its own buttons. On macOS, a left drag released with Ctrl held (a popup
    click) never ended its gesture, which held Undo and Save As. The press now decides for its whole drag and
    release.
  - The fader's menu and the lanes' menu are now built by functions the shell test checks: no item without a
    Volume lane, the item with one, and the lanes' menu ending with it only in Automation Mode.
- **For win:** items 3 and 5 both add a call in `tests/test_ui_document.cpp`'s `main` at the same place. Merging the
  second takes a two-line conflict; keep both calls.

**Run** (the three built together, `ninja -k 0`, JUCE on, Pd off, warnings as errors): the 27 suites pass,
window-state 140 checks, document 322, shell 2966, 0 failures.

## 2026-11-22, window-fn — queue item 9, DF-W38: Re-Enable finds a device lane stored as "001" (PR requested, lands after DF-E53)

**Window lane, read first:** this item touches `src/adi/ui/automation_override.hpp` only.

`laneOf` compared a device lane's `param_ref` as written, so a lane stored as "001" (a hand-made or older file) was
never found for parameter 1. The engine, after DF-E53, binds that lane as parameter 1, and an edit overrides it, so
the knob's Re-Enable Automation went missing. A device lane's ref is now compared by the id that DF-E53's shared
parser (`adi::parseDeviceParamRef`, `src/adi/device_param_ref.hpp`) reads from it. A track's or a send's ref is
compared as written. This replaces `window-fn/lane-ref-canonical` (`996d5b1f`), which carried a parser of its own.
`ParameterControl::showMenu` (`ui_devices.cpp`) goes through `deviceLaneOf` → `laneOf`, so it needed no change.

**Test** (`tests/test_ui_seams.cpp`): a lane stored as "001" is found as parameter 1 by `laneOf` and `deviceLaneOf`,
so the item is offered; parameter 2 finds none; a track's "Pan" is not "pan".

**Run:** JUCE on, Pd off, warnings as errors (GCC 13, Debug, `ninja -k 0`). Seams 555 checks, 0 failures. The 27
window and mix suites, `adi_param_automation_tests` and `adi_automation_lanes_tests` pass (30 of 30). The shell suite
passes from a scratch directory (2911 checks, 0 failures).

PR requested: `window-fn/df-w38-param-ref` → main, "DF-W38: a device lane's ref is the parameter it parses to".
**Lands after DF-E53:** the branch is `gemini/df-e53-canonical-param-ref` `03be0b99` plus one commit (`bd21e064`).
Close `window-fn/lane-ref-canonical` unmerged.

## 2026-11-22, window-fn — items 3 and 5 run again on full builds

Rebuilt with `ninja -k 0`, so every target but main's broken acceptance target was built. On `window-fn/saveas-guard`
`55bdf56e` and on `window-fn/dfe44-discard` `90595272`, the 27 window and mix suites pass, and the shell suite passes
from a scratch directory (2954 checks, 0 failures). The two "PR requested" entries below stand as written.

## 2026-11-22, window-fn — queue item 8, DF-W28 (rest): a Duplicate or Paste onto clips removes what it covers (PR requested, drop half parked)

- **Duplicate and Paste already do this.** Both go through the engine's placement planner
  (`clip_edit::preparePaste` → `edit_plan::detail::prepareCaptured`), so the covered part goes in the same step. The
  branch adds the test the queue asked for (`tests/test_ui_shell.cpp`, `clipClipboard`). Duplicate onto the next
  clip replaces it in one step, "Duplicate Clip". Paste a quarter bar into two clips trims the first and removes the
  second in one step, "Paste Clip". Undo brings both back. No code change was needed.
- **Parked: the drops.** A file dropped onto a clip still lands over it. Both drop paths place the clip without the
  planner:
  - the window's batch (`adi::ui::dropAudio`, `clip.create` + `clip.attachAudio`);
  - the engine's material import (`clip_edit::prepareImportAudio`, a `clip.duplicate` op).

  The planner's clear step (`PlacementBatch::clear`) is engine-internal (`edit_plan::detail`), and the window cannot
  join a planner edit to its own ops in one step (`commitPrepared` takes one appended op). This needs the engine; see
  the needs table.

PR requested: `window-fn/df-w28-rest` → main, "Test that a Duplicate or Paste onto clips removes what it covers
(DF-W28)". Depends on nothing beyond main.

## 2026-11-22, window-fn — queue item 7, AU-04 lane and fader: Re-Enable Automation on the lane and the fader (PR requested)

**Window lane, read first:** this item touches `src/juce/ui_mixer.*` (the volume and pan sliders become
`PrimaryButtonSlider`, a `volumeMenu`) and `src/juce/ui_arrangement.*` (`contextMenu` takes the lane under the click).

- **The lane.** In Automation Mode a right-click on a track's lane ends the lanes' menu with that envelope's
  Re-Enable Automation (AU-04's `laneOf`, so a disabled lane or a lane in another domain offers nothing). It is
  checked only while overridden, as on the controls.
- **The fader.** A right-click on the track volume fader opens a menu with the Volume lane's Re-Enable Automation. A
  track with no Volume lane shows no menu.
- **The fader ignores the right button.** With its own menu off, JUCE's `Slider::mouseDown` starts a drag on any
  button (`juce_Slider.cpp`, `mouseDown`). So a right-click on the fader changed the volume and opened a mixer gesture.
  `PrimaryButtonSlider` passes only left-button presses, drags, releases and double-clicks. Pan gets the same class.
- **Test** (`tests/test_ui_shell.cpp`, the mixer drag case): a right-button press, drag 40 px and release through the
  slider's own handlers leave the value, the gesture and the ops as they were. The menus themselves are asynchronous
  and are not driven in the suite.

PR requested: `window-fn/reenable-lane` → main, "Re-Enable Automation on the lane and the fader; the fader ignores
the right button (AU-04)". Depends on nothing beyond main.

## 2026-11-22, window-fn — queue item 6, N61's window half: a MIDI port's record follows it by name (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/midi_ports.hpp` (`reconcileMidiPorts`, a `name` for
`withMidiPortSwitch`), `src/juce/app_midi_ports.*` (the host takes a writer) and `src/juce/ui_main.cpp` (the writer).
`ui_settings.*` needed nothing: the page already hides the `midi.port*` rows behind the table.

- Each port's record keeps its name (`midi.port.name`). It is written with a switch and stamped when the host
  lists the port.
- A port whose identifier has no record adopts the record of the same direction and name whose port is no longer
  listed. The record is re-keyed to the new identifier and saved, before the switches are read, so the keyboard
  opens with its Track and MPE switches from its first listing.
- Only an unambiguous match moves: one such record and one such port. Two keyboards of one name, or two old records
  of one name, stay apart. A port of another name gets no record. A record that already agrees is not written again.
- **Test:** `tests/test_ui_window_state.cpp` covers the pure function's cases (stamp, move per direction, unrelated
  name, still listed, two ports, two records). `tests/test_ui_shell.cpp` covers a host over fixed ports with a writer:
  the record moves on the first listing, Track and MPE survive, the unrelated port gets nothing, and a second refresh
  writes nothing. It also checks that a click on Track keeps the port's name.

PR requested: `window-fn/midi-port-name` → main, "Keep a MIDI port's name with its record and follow it to a new
identifier (N61)". Depends on nothing beyond main. `window-fn/midi-live` (N60, waiting on win) also changes
`app_midi_ports.*`. The two touch different functions; the second to land merges by hand if git asks.

**Run for items 6, 7 and 8, built together** (they touch different code; the one shared file,
`tests/test_ui_shell.cpp`, merges cleanly): JUCE on, Pd off, warnings as errors (GCC 13, Debug). The 27 window and
mix suites pass, the window-state suite 137 checks, and the shell suite from a scratch directory 2972 checks, 0
failures. Each branch builds alone from main.

**Correction to items 3 and 5:** main's acceptance target fails to compile in a JUCE-on, Pd-off build
(`test_session_wts01.cpp:307`, unused `doc` and `driver`), and it stops `ninja` before every target is built. Items 3
and 5 were built that way, so their shell-suite runs may have used a binary from before their change. Items 6-8 were
built with `ninja -k 0`. Items 3 and 5 are being run again the same way; the result follows in the next entry.

## 2026-11-22, window-fn — queue item 5, DFE44-DISCARD: the Status Bar says when a dropped take's files stay (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/project_document.*` (a new `discardTake`).

Since DF-E44, `AudioRecordingResult::discard()` returns the `std::error_code` of a folder the system would not remove
and keeps the folder for a retry. The window ignored it. Each of the three discard sites now goes through
`discardTake`, which adds the system's words and the folder's name to the Status Bar line:
- a take with no attachable audio: "The take was not kept: …";
- a take the project refused: "The take was recorded but could not be added: …";
- a Record start whose command did not fit the transport queue.

The added line reads: "Its recorded files could not be removed (Permission denied); they stay in
.adi-record-… in the project's folder."

**Test** (`tests/test_ui_document.cpp`, `discardRefusalSaid`): Record, then make the work folder read-only, then stop
with nothing recorded and land. The Status Bar names the refusal and the folder, and the folder stays. POSIX only.
Root removes entries from a read-only folder, so as root the case skips and says why. Here it ran as an
unprivileged user (317 checks, 0 failures) and as root (it skips). CI's runners are not root.

**Run:** JUCE on, Pd off, warnings as errors (GCC 13, Debug): the 27 window and mix suites pass, and the shell suite
from a scratch directory passes (2954 checks, 0 failures).

PR requested: `window-fn/dfe44-discard` → main, "Say in the Status Bar when a dropped take's files stay (DF-E44)".
**Lands after DF-E44 and N66:** the branch is `engine-win/recording-failed-prefix` `0900deeb` (N66, which contains
DF-E44 `0ed2c355`) plus one commit.

## 2026-11-22, window-fn — queue item 4, OWNER-REVIEW-1122: the window's sites in N62, N66, DF-E64 and the loans (for win)

**Tree reviewed:** main `87ea349c` with `engine-win/export-host-owner` `6b78d94c` (N62), then
`engine-win/recording-failed-prefix` `0900deeb` (N66, which contains DF-E44 `0ed2c355`), then
`engine-win/play-first-block` `01b1e723` (DF-E64), merged in that order. N62 and N66 merge cleanly. DF-E64
conflicts with #710 (below).

**WTS-01** (JUCE on, Pd on, `-DADI_ACCEPTANCE=ON`, GCC 13 Debug, warnings as errors): **2b PASS with DF-E64**
("0 of 384000 frames differ … held 1 callbacks before the first song frame"; on main alone it FAILs). **7d PASS with
N62** ("worker export completed, 192000 frames, audible saved note"), 7e PASS. Session 0 FAIL, VST3 part 0 FAIL.
The window-state suite (135 checks) and the document suite (319) pass on the merged tree.

**DF-E64 (`play-first-block`)**
- **Conflict with #710** in `TransportMailbox::drain` (`src/adi/ui/window_state.hpp`): #710 stores the Locate's
  revision (`locateRevision_`, read by the Mix View's `settledLocateRevision`), and DF-E64 rewrites the same branches
  for the held Play. The resolution keeps both. The Locate branch stores `locateRevision_` after `transport.locate`.
  The Record, Stop and Play branches are DF-E64's: Record and Stop clear `pending`, and a Play while stopped sets it.
  With that resolution the suites and 2b pass as above.
- **Finding (important): the held Play has no bound.** `drain` starts the transport only once
  `sourcesReadyAt(position, frames, true)` is true. A page that never comes (slots exhausted, a progressive source
  still pending while its import runs, a reader that stalls) leaves Play lit (`willPlay()` is true) and the song
  silent and still, with nothing in the Status Bar. Suggest a bound: after about 250 ms of callbacks, start anyway
  and count the underrun as before, or have the window say "Waiting for audio files to load".
- Nit: while stopped, each callback asks every clip for read-ahead at the playhead (`clip_playback.cpp`, `play`).
  That costs O(clips) per block on a large set that is stopped. It is bounded, but it could run once per locate.

**N62 (`export-host-owner`), its window site in `ui_main.cpp`**
- Sound: `HostDispatch` carries an action's exception back to the worker (`packaged_task`). `stopRender` services
  the mailbox until the worker has finished, so a cancelled render's Session and plug-ins are destroyed on the
  message thread. `install` and Quit both call `stopRender` before the document goes. The old window refusal and the
  harness's `exportLoader` are gone, as they should be.
- Finding (performance, plausible): `service()` puts `session.tick` and `serviceHost` on the message thread every
  block, a round trip per block through `callAsync`. An export's speed is now bounded by the window's message loop,
  and it slows while the window paints or a menu is open. Suggest servicing the host on a timer (every 10 ms or so,
  as the live session's tick does) rather than every block.
- Note: `Session::load` for the render now runs on the message thread, so a project with heavy plug-ins freezes the
  window for the load's length. It is acceptable (plug-ins need it), but worth a progress line on the Export page.

**N66 (`recording-failed-prefix`), its window site in `project_document.cpp`**
- Sound: `landTake` keeps an attachable prefix as one undo step and names the interruption.
- The three `discard()` calls ignore the `std::error_code` DF-E44 now returns. That is queue item 5,
  `window-fn/dfe44-discard`, stacked on this branch.
- Order note: `landTake`'s ids are still `MAX(id)+1`. `window-fn/row-ids` (queue item 2) changes those lines, so
  whichever lands second takes a small textual conflict there. RowIds' version is the one to keep.

**Mix View loans (on main since #710)**
- `pasteDevice`: the id comes from `RowIds`, past the retained device ids and the copy's own id, and is read in one
  transaction. That is consistent with DF-E16. No finding.
- The `synchronise` retiree loop (`entryFor(deviceId) == &entry`): correct. A retiree that shares its id with a newer
  instance no longer detaches the newer one's parameter ops.
- `settledLocateRevision`: the ordering holds (the revision is stored before `read_`'s release store, and read
  after its acquire). No finding beyond the DF-E64 conflict above.

**Clones loans (#705, on the held ADI-Shaper panel stack, top `clones/shaper-trim` `69422cef`)**: the editor factory in
`ui_main.cpp` and the wrench checks in `ui_devices.cpp`.
- Sound: the panel view opens through the plug-in window host, and `floating_` is reset before the document goes,
  in `install` and at Quit, so the view's `ProjectDocument&` does not outlive it.
- Nits: the uid `org.adidaw.builtin.adi-shaper` is spelled at three sites (two in `ui_devices.cpp`, one in
  `ui_main.cpp`); one predicate ("has a window of its own") would keep them together. The refusals on that path
  still say "plug-in" for a built-in ("The plug-in is not loaded, so it has no window."). Only the two loaned sites
  were reviewed. The rest of the stack waits on ADR-0248 d11 as #708 says.

No PR: review only.

## 2026-11-22, window-fn — queue item 3, PD06-SAVEAS-GUARD: a take lands in its own file before Save As, Open or New (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/project_document.*` (`saveAs`, a new
`landTakeHere`, `settleRecording` now returns whether the take landed) and `src/juce/ui_main.cpp` (`install`
says when a take could not be kept).

- **Save As during Record or a pending take.** Before the file switches, `saveAs` stops Record, waits up to ten
  seconds for the recorder's worker (the device keeps calling back, as in the app) and lands the take as its one
  undo step ("Record Audio") in the file it was recorded in. Then the store switches, so the take is in both files.
  The Status Bar says "Recording stopped, its take kept, before Save As." If the take is still being written after
  ten seconds, Save As is refused ("The take is still being saved; try Save As again in a moment.") and nothing
  switches.
- **Open, New and a project replace** (`install`) already settled the take into the old project after the device
  was detached. A take still being written after ten seconds went with the document without a word; the Status Bar
  now says so. Quit is unchanged.
- **Test** (`tests/test_ui_document.cpp`, `saveAsKeepsTheTake`): Save As during Record with a callback thread
  running: Record stops, one clip lands, the undo step is "Record Audio", the clip is in the old file and the new
  one, the warning names it. Then, in a second project, Save As with the take pending (stopped, device released,
  not yet landed): it lands first in the old file, then saves, as one undo step.

**Found, not fixed here (engine):** a take recorded *after* a Save As is refused: "recording belongs to a different
project". `Session::load` reads the project's identity (`recordingProject_`, from `PRAGMA database_list`) once, and
`Session::refresh`, which the document's `synchronise()` calls after Save As, never reads it again. So every
recording after a Save As fails until the project is reopened. Added to the needs table. The test's second case uses
a project of its own until then.

**Run:** JUCE on, Pd off, warnings as errors (GCC 13, Debug): the 27 window and mix suites pass, the document suite
320 checks, 0 failures, the shell suite from a scratch directory. WTS-01 (`-DADI_ACCEPTANCE=ON`, Pd on): 2a PASS.
2b FAILs as on main (DF-E64, queue item 4).

PR requested: `window-fn/saveas-guard` → main, "Save As, Open and New land a pending take in its own file first
(PD06-SAVEAS-GUARD)". Depends on nothing beyond main.

## 2026-11-22, window-fn — queue item 2, DF-E16's window half: one RowIds per prepared edit (PR requested)

**Window lane, read first:** this item touches `src/adi/ui/track_edits.hpp` (`insertTrack` and `groupTracks`
take an optional id) and `src/adi/ui/audio_drop.hpp` (`newClipId` skips remark-held ids; a new `newTrackId`).

The window gave a new row the highest live id plus one, so a deleted track's or clip's id was given again while
a remark still named the old row. Every allocation in the window now follows `RowIds`' rule: past every live
row and every id a remark holds for the kind.
- **The document's prepared edits** take one `RowIds` each: `prepareImport`'s media row (past what the drop's
  earlier files took), the import job's clip and media, and a landed take's clips and media.
- **The root's** Insert Audio/MIDI Track, Insert Return Track and Group Tracks, and a first automation point's
  lane and data, take their ids from `RowIds`.
- **The snapshot helpers** (`newClipId`, used by Split, Duplicate and the drops, and `newTrackId` for a dropped
  file's track) apply the same rule over the snapshot's remarks.
- `pasteDevice` already used `RowIds`. `ui_main.cpp`'s `loadPlugin` reads the newest device id after an insert
  to find it: a read, left as it is, since the new id is the highest under the same rule.

**Run.** The window's 27 suites (100%). `adi_ui_seams_tests` 566 checks: Kick and a clip deleted with their remarks
kept; the next track is 5, not 4, and the next clip 8, not 1, from `RowIds` and from the snapshot alike; the new
track lands as 5; Undo brings Kick back with its id, and Redo deletes it again. `adi_ui_shell_tests` 2954 checks,
0 failures. **For win:** the shell run ends with JUCE's leak detector naming 50 `SoftwarePixelData` images. This
branch touches no image, and main's shell suite gained 43 checks since this seat's last run. WTS-01 (Pd): 2a and
6a PASS, as before; 6b waits on `window-fn/lane-chooser`, which is not on main. No review workflow ran.

**PR requested** — `window-fn/row-ids` at `3ba97042`, on main `ac07be74`: "adi_daw: DF-E16's window half: one RowIds
per prepared edit". It depends on nothing; it touches `ui_shell.cpp`'s `addAutomationPoint`, as
`window-fn/lane-chooser` does (one hunk each).

## 2026-11-22, window-fn — queue item 1, SHELL-BROWSER-FIXTURE: browserDevices fixed (PR requested)

Reproduced on main `ac07be74` with Pd on: Hybrid Reverb loads now (its native core is ready), so the check
"Hybrid Reverb is dimmed always" failed. Its load put it on 3 Audio, the plug-in subcase then cleared the
selection, the built-in loads after it failed, and the strip's first panel was read from an empty list: a
segfault.

- With Pd on, the test expects every Audio Effect to load, Hybrid Reverb too, as one step, then undone.
- 3 Audio is chosen again after the plug-in subcase, and the strip read is guarded.
- Without Pd, Hybrid Reverb is dimmed as the other Pd devices are. `BrowserPanel::unavailable`'s "Hybrid
  Reverb is not built in ADI yet" was stale, so it is gone and the Browser gives the Pd reason. **Touched for
  the window lane:** that one line of `src/juce/ui_browser.cpp`.

**Run.** `adi_ui_shell_tests` with Pd on (a JUCE + Pd tree, warnings as errors): 2926 checks, 0 failures, no
crash; with Pd off: 2911 checks, 0 failures. The window's 27 suites (100%). Fresh profile: the suite's own temp
settings.

**PR requested** — `window-fn/browser-fixture` at `fa167f5c`, on main `ac07be74`: "adi_daw: SHELL-BROWSER-FIXTURE:
browserDevices with Hybrid Reverb ready". It depends on nothing.

## 2026-11-22, window-fn — the standing queue of 22 November taken; MX-02: Split Stereo Pan (PR requested, lands after 3 December)

Win's standing queue (`collab/prompts/2026-11-22-window-fn-queue.md`) replaces every earlier list. MX-02, its
item 13, was nearly done when the queue arrived; it is finished here and marked **lands after 3 December**.
`window-fn/lane-ref-canonical` (DF-W38, logged earlier today) is superseded by the queue's item 9, which stacks
on gemini's parser (`src/adi/device_param_ref.hpp`): that branch will be rebuilt there, and win can drop the
standalone one.

- The pan's menu offers **Stereo Pan** and **Split Stereo Pan**, the one in use ticked; a choice is one undo
  step (`mixer.setPanMode`).
- **Split**, the strip shows two pans, left and right, in the pan's place, made as the pan is; the look is the
  window lane's sheet. Each is dragged live through the gesture session (`TrackPanLeft`, `TrackPanRight`) and
  leaves one undo step of `mixer.setPanLeft` or `setPanRight`. With tracks selected, a drag moves only the
  tracks whose pan is the one dragged, so a track in the other mode keeps its stored values. A typed value or
  a double-click (back to 50L or 50R) commits at once.
- The menu offers **Re-Enable Automation** for the pan, or for the split half right-clicked (`panLeft`,
  `panRight` lanes).

**Run.** The window's 27 suites (100%; one `adi_ui_document_tests` failure under a parallel build's load passed
alone, 305 checks, and in the rerun of all 27). `adi_ui_shell_tests` 2921 checks, 0 failures: the mode one
step; the two pans at 50L and 50R; each half dragged live, nothing written mid-drag, one step, moving only
itself; typed; a split drag beside a selected stereo track leaving that track alone; Stereo again on Undo.
Review: two dimensions with a skeptic each. Confirmed and fixed: a selected stereo track's hidden halves
written by a split drag, the two unlabelled Re-Enable items, and two test gaps (the live drag, the right
half). Three were refuted.

**PR requested** — `window-fn/split-pan` at `1d8030a5`, on main `0ea10b8e`: "adi_daw: MX-02: Live's Split Stereo
Pan". **Lands after 3 December.** It depends on nothing; it shares `ui_mixer.cpp` with `window-fn/cueing` and
`window-fn/delay-compensation` (separate hunks).

## 2026-11-22, window-fn — MX-08: Live's cueing, the window's half (PR requested)

M1's MX-08 (the window worklist's B6), on the engine's `cue.*` ops, which were on main and had no caller
(Live 12 manual 18.6).

- **Solo Switches and Cue Switches** in the Options menu, before Solo in Place (Live 12's order), one ticked
  from the project's cue mode (`cue.setMode`).
- **The Cue button.** With Cue Switches, each track's Solo button is its Cue button. It sends the track to
  Cue Out without soloing it (`cue.setTrack`) and is lit while the track is cued; S cues the selected track
  the same way. It reads "C" until the window lane draws Live's headphones. A group's button is not half
  coloured from its children's solos, which Cue mode does not play. A Cue button toggles its own track only;
  whether Live's Cue is exclusive is to be read on Live 12 (PROVISIONAL).
- **Cue Volume** on Main: a knob where a track has its Activator, in the Mixer View (placement the window
  lane's), written at release or when typed (`cue.setVolume`), on the fader's -70..+6 dB with silence at
  its foot.
- **Cue Out** on Main, now live: a click on the chooser lists the device's open outputs in stereo pairs, up
  to 63/64 as the op takes them, the one in use ticked (`cue.setOutput`).

Every one is a transport op outside undo, saved with the project, the session re-reading the rows after it.
Not here: the metronome on Cue Out and the preview on Cue Out (the engine's: `audio.metronomeOutput` and the
browser's preview path).

**Run.** The window's 27 suites (100%). `adi_ui_shell_tests` 2924 checks, 0 failures: Solo Switches by default;
Cue Switches ticked with no undo step; the Cue button cueing without soloing, lit, and uncueing; S in Cue
mode; Main's Cue Volume typed with no undo step; Cue Out 3/4 taken and an odd first channel refused; Solo
Switches again. Review: two dimensions with a skeptic on each finding. Four were confirmed and fixed (the
chooser on a right-click only, S writing an unheard solo, a group's half colour, pairs past 63/64); four
were refuted.

**PR requested** — `window-fn/cueing` at `c6a557e6`, on main `ce29e618`: "adi_daw: MX-08: Live's cueing: Solo/Cue
Switches, Cue buttons, Cue Volume and Cue Out". It depends on nothing. It and `window-fn/delay-compensation`
(MX-04) each append to `AppCommands::Id`'s tail and the names table; whichever lands second takes the other's
ids first, a merge of three lines and the shell's tail check.

## 2026-11-22, window-fn — DF-W38: a device lane found by the parameter it names (PR requested)

Win's entry of 22 November gave DF-W38 to this seat, after DF-E53. Its window half does not need to wait:
`laneOf` compared a device lane's `param_ref` as written, so a lane stored as "001" (a hand-made or older
file) was never found for parameter 1, and Re-Enable Automation went missing for it. A device lane's ref is
now compared by the decimal id it parses to, up to 32 bits, as the engine's parser does (the same rule as
`param_automation.cpp`'s `parseParamId`). A track's or a send's ref is compared as written. Once DF-E53
shares its parser, this one can call it.

**Run.** The window's 27 suites (100%); `adi_ui_seams_tests` 555 checks ("001" is parameter 1, parameter 2
has none, a track's ref as written), `adi_ui_shell_tests` 2911, both 0 failures. No review workflow ran for a
twenty-line change.

**PR requested** — `window-fn/lane-ref-canonical` at `996d5b1f`, on main `ce29e618`: "adi_daw: DF-W38: a device
lane's ref is the parameter it parses to". It depends on nothing; it touches the same function as
`window-fn/lane-chooser` (DF-W37), a merge of one hunk.

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

