# window — log

The window lane (ADR-0208, `docs/LANES.md` §3b): a Claude work seat of its own, building step 7,
the first window. It owns `src/adi/ui/**`, `src/juce/ui_*`, `src/juce/app*`, their tests,
`docs/STEP-7-PLAN.md` and `docs/UI-ARCHITECTURE.md`. Standing prompt:
`collab/prompts/2026-10-27-window-lane.md`. Branches `window/<topic>`, one per item, cut from
`origin/main`; the six rebased PRs stack in the plan's order, each on the one before. Only the
window lane writes to this file. Newest entry at the top.

**Why this log has its own branch and PR (`window/log`).** As the engine lane's: item PRs never
touch this file, so no merge of one turns the next red. Each entry names its PR.

**Where it runs.** The director's Mac (Apple Clang, macOS 26, 8 cores, 8 GB), in a worktree
outside the OneDrive-synced folder (`~/adi-window`). Built as `ci.yml` builds on macOS: the
default generator (Makefiles; there is no Ninja on this Mac and the workflow's macOS legs do not
use it), `-DCMAKE_BUILD_TYPE=Debug -DADI_WERROR=ON`, and a second tree with `-DADI_WITH_JUCE=ON`.
MSVC `/WX` and Linux are not run here; the `Step 7 UI` workflow runs all three.

**The Live 12 manual.** `reference/DOCS/_text/Ableton_live12/` is not on this Mac. The parity
checklists are drafted from `adi_daw/References/live12-manual-en.pdf` in the director's main
checkout, read to text locally; pages cited are the PDF's. Where the A2000's rows exist, they win.
The checklists live in `collab/window/parity-*.md`, one per item, in that item's PR.

---

## Needs from other lanes (open)

| # | Need | Owner | Why the window needs it | What the window does meanwhile |
|---|---|---|---|---|
| N1 | `Store::create` writes no `project` row, so `project.setName` updates nothing on a new file and the snapshot falls back to 48 kHz (ADR-0203 d21 says 44.1 kHz) | engine | STEP-7-PLAN §3.0, still open on main `a57d47a` | New makes no project row; the Take Snapshot prompt names the project after its file |
| N2 | A bootstrap for a new project (the project row and the Main track) that is the history's root, not an undo step (Live manual 5.4.2) | engine | #198 wrote both rows with raw SQL from the UI, which the plan forbids | New makes the Main track through `track.create` and the UI's `OpSubmitter::protectRoot` refuses to undo that step |
| N3 | Save As, a true one that writes a new `.adi` and carries on in it (ADR-0203 d6, LANES §3 item 6) | engine | Ctrl/Cmd+Shift+S | the key and the menu item exist; the Status Bar says Save As waits for the engine |
| N4 | An app-scoped home for each window's interface zoom, per window and per display (ADR-0129 d3; ADR-0203 d11 "kept with the application") | win (the settings registry) | Ctrl/Cmd + and - zoom each window, but the zoom is not remembered across launches | held per window for the session |
| N5 | An undoable path from a file and a track to a clip that plays: `importMedia` that captures without committing (so media and clip are one undo step) and fills `sample_rate`/`channels`/`frames`, and an op pair that attaches an `audio_clips` row (STEP-7-PLAN §3.0). #199's commit `3feee62` has a working version (`prepareImport`, `clip.attachAudio`/`clip.detachAudio`) in `media_ops.*` and `ops_catalog.cpp`, for the engine lane to lift | engine | dropping audio onto a track, 7.3's headline gesture | the arrangement accepts the drop and the Status Bar says it waits for the engine; nothing is written |
| N7 | A UI producer into the edit capture: `ParamOps` has one ring per plug-in and none for the window, so a drag in the device strip cannot become an op whose first touch emits its opener (ADR-0124, ADR-0181 d5). #203's `537c56d` has `ParamOps::uiEvent` (its own `ParamEditCapture`, begin/value/end, reusing the opener machinery) for the engine lane to lift | engine | editing a parameter from the strip, 7.4 | the strip shows every value; a gesture is refused and the Status Bar says why |
| N8 | A plug-in's editor from the hosts: VST3 (`IPlugView`, through JUCE's `createEditor`) and CLAP (`clap_plugin_gui`), as a component the floating host can place, and the editor's own size | engine (`src/juce/*host*`) | Live's wrench toggle and Ctrl/Cmd+Alt+P; the floating host's main users (ADR-0076) | the host is built and tested; nothing opens a plug-in window |
| N9 | The scalar meter of UI-ARCHITECTURE §9, one `{peak, rms}` per strip written by the audio thread (#205's `9e4f4af` has a `StripMeter` in `engine/mixer.*`) | engine | metering a hundred strips without reading a tap each | each visible row reads the newest 1024 frames of its track's post-fader tap |
| N10 | A Learn target for a mixer control that has no automation lane yet (#205's `learnControllerTarget` in `controller_maps.*`) | engine | MIDI Learn on lane headers, P2 (ADR-0203 d7) | nothing; Live learns through MIDI Map mode, which comes later |
| N11 | The snapshot to carry the send rows (`routing` with `kind='send'`: source, return, gain, pre/post) | engine | one send knob per return on every row (ADR-0203 d3) | no send knobs; the routing ops exist and are ready to be called |
| N12 | Split Stereo Pan: a pan mode and two pans per strip in the format and the mixer (ADR-0203 d9) | engine | the pan control's context menu | the menu shows the mode disabled |
| N13 | A loudness meter's DSP: momentary, short-term and integrated LUFS and true peak, cheap enough for the Main track at all times (ADR-0203 d14) | engine or devices | the Main track's loudness meter | none yet |
| N6 | The snapshot to carry what the arrangement draws: a track's `kind` (to place Main last, as Live's arrangement does), a clip's `name`, and the time-signature map (bars in the ruler). #199's `3feee62` adds all three to `engine/snapshot.*` | engine | the ruler and the clip labels | the ruler counts bars in 4/4 and says so in its tooltip; clips are drawn unnamed; Main is listed where the snapshot puts it |

---

## 2026-10-27, latest — the Mixer View [#263](https://github.com/arieladi/Adi/pull/263) `window/mixer-view`

**The Mixer View, now #263, stacked on #262.** Live 12's Mixer in the Arrangement: vertical
strips at the arrangement's foot, from Ctrl/Cmd+Alt+M, the View menu's Mixer, or the Status Bar's
new bottom-right toggle.
- It is the same `MixerPanel` in a vertical mode, so edits, meters and Auto Gain are shared.
- Only the columns on screen are realised, and the wheel scrolls it sideways.
- Dragged past 260 units tall, it shows ticks every 6 dB and a numeric volume field.
- The manual settles parity-7.2 row 6: Ctrl/Cmd+Alt+M is the Mixer View's. The side panel of
  track controls keeps a View-menu item, with no key.
- Found by the render: the Track Activator showed the track's id. It now shows its place.

Checks: 244 shell checks, with a new golden; `test_all.sh` 8265 across 97 suites.

**The shadowing sweep earned its keep twice more:** an outer `order` and an `a` hidden by locals
were both caught before push.

**CI at this entry:** #246 and #248 are fully green; #251 to #262 have no failures and are still
running.

**Next in §3b item 2:**
1. The scrub area and its click-to-play (manual 6.1, p. 161).
2. The browser's content.
3. Live's resettable peak indicators on a tall Mixer View.

After those, item 3: the window parts of phase C, the Settings window in Live's shape (ADR-0203
d11), and the EQ/limiter window (ADR-0204 d1).

---

## 2026-10-27, later — Rename [#260](https://github.com/arieladi/Adi/pull/260), selection [#261](https://github.com/arieladi/Adi/pull/261), the canvas's accessibility [#262](https://github.com/arieladi/Adi/pull/262)

**CI.** #246 and #248 are green on all 22 legs. Three failures elsewhere in the stack, all fixed
on the branch that introduced them, with the branches above restacked:
- #256: MSVC C4456. The floating-host test's `host` hid the `ZoomHost host`.
- #257 and #258: GCC `-Wsign-conversion` on `size_t += cond ? 1 : 0`.
- #261: Windows allocated in the frame that presents a new selection, where the device strip
  follows the track. The check now audits the frames after it.

Before each push, a Clang `-Wshadow-all` sweep now runs over the UI sources and the shell test,
through `build-juce/compile_commands.json`. It caught two more shadowed loop variables before
#262 went up. GCC 16 is installed (`/opt/homebrew/bin/g++-16`), but it cannot parse JUCE's
headers on macOS, so GCC-only warnings still surface in CI.

**Rename, now #260, stacked on #258.** Ctrl/Cmd+R, the Edit menu or the header's menu opens an
editor in the track's title bar.
- Enter commits one undo step; Esc cancels; Tab and Shift+Tab move on through a series.
- A leading # shows the track's number and follows it, with a # per digit (manual 18.2). New
  tracks are named "#-Audio" and "#-MIDI".
- Checks: 219.

**Selection, now #261, stacked on #260.** Selection is session state.
- `adi::ui::SessionState` holds the project's one selection in `session_state`. It is never an op.
- Every window presents a copy per frame.
- Edits carry it as `sel_before`, and undo restores it.
- Checks: 224 shell and 48 headless (+10, every ABI). `test_all.sh` 8265 across 97 suites.

**The canvas's accessibility, now #262, stacked on #261.** Each clip on screen gets a transparent
`ClipAccessible`, recycled as the view scrolls.
- Its listItem title is built only when a reader asks; its press action selects the clip.
- Keyboard moves are announced.
- Checks: 229.

**Next in §3b item 2:**
1. The Mixer View of vertical strips (Live 12 18.1, p. 390, 393).
2. The scrub area and its click-to-play.
3. The browser's content.

---

## 2026-10-27, night — LANES §3b item 2 begins: the Overview → [#258](https://github.com/arieladi/Adi/pull/258) `window/overview`

**The Overview, now #258, stacked on #257.** It is a 16-unit strip above the ruler: every track's
clips in miniature over the whole span, and an outline around the part on screen.
- Dragging sideways scrolls by the Overview's own scale; dragging vertically zooms.
- A double-click zooms out to the whole arrangement.
- The lanes, the playhead, the mixer's rows and the strip's ceiling now start below it
  (`kTopHeight`).

Checks: 212 shell checks, with real mouse dispatch through the fake peer; `test_all.sh` 8255
across 97 suites. Parity: row 20 of `parity-7.3-arrangement.md` is built.

**Where to resume** (superseded by the entry above for the branch). The worktree is `~/adi-window`, on `window/overview`. Its trees are
`adi_daw/build` (core, `-DADI_WERROR=ON`) and `adi_daw/build-juce` (`-DADI_WITH_JUCE=ON`). This
log lives in `~/adi-window-log` on `window/log` (#252). The next items of §3b item 2, in order:
1. Rename (Ctrl/Cmd+R) and the double-click on a track's name: `track.rename` exists.
2. The scrub area and its click-to-play: the locators wait for their ops.
3. Selection as session state, which UI-ARCHITECTURE §2 requires. The arrangement still keeps it
   in view geometry, as #199 did.
4. The canvas's accessibility layer, one virtual child per clip (UI-ARCHITECTURE §6).
5. The Mixer View of vertical strips.
6. The browser's content.

Then item 3: the window parts of phase C, the Settings window in Live's shape, and the EQ/limiter
window. Before any of that, check CI on #246, #248, #251, #254, #256, #257 and #258; their reruns
after the restack were pending at this entry.

---

## 2026-10-27, late — #204 → [#256](https://github.com/arieladi/Adi/pull/256) `window/floating-windows`; #205 → [#257](https://github.com/arieladi/Adi/pull/257) `window/mixer`; the six are through

**#204, the floating host, now #256, stacked on #254.**
- **Kept:** the host and the analyser views, with their tests. Undocking reparents; a second view
  shares one model; the monitor clamp; one clock per window; keys reach the app's commands; a
  window closes when its device is deleted and does not reopen on undo.
- **Gone:** the "Analyse" button on every device. Live has none; the analyser's big window is the
  analyser device's, and plug-in editors need the hosts (N8).
- **Added:** tooltips in every floating window.

Checks: 161. Parity: `parity-7.5-floating.md` (11 rows).

**#205, the mixer, now #257, stacked on #256.** Its engine half is left out (N9, N10).
- **Live's Arrangement Track Controls:** one row per track, level with the lane and scrolling with
  it, in the mixer slot. Each row holds the meter (from the track's post-fader tap), volume, a pan
  knob, the numbered Track Activator and Solo.
- **The ADI-Q and ADI-L slots** sit on every row, present and off.
- **Auto Gain Stage** is in the right-click menu.
- **Split Stereo Pan** is shown disabled (N12).
- **Open:** send knobs, return strips and the Main track's loudness meter (N6, N11, N13).
- **Per-strip Learn is gone:** Live learns through MIDI Map mode.

Checks: 208. Parity: `parity-7.6-mixer.md` (15 rows).

**MSVC caught one, and the stack was restacked.** #248's Windows GUI leg failed C4456: the
shell test's second `opCount` hid the first. It was fixed on `window/application`, and a lambda's
local `id` in the device panel's Configure menu was renamed against C4458. The four branches
above were rebased onto that fix and force-pushed with lease: #251, #254, #256 and #257. The tip
passes 208 shell checks.

**The queue moves on to item 2:** what 7.2 to 7.6 lists and no PR covers. That is the Overview;
the scrub area and locators; the Mixer View of vertical strips; Rename (Ctrl/Cmd+R); the
DetailEditor (the Clip View); the browser's content; the Ctrl/Cmd+Alt+M Mixer section's
show/hide options; and the ADR-0129 gate rows not yet built.

---

## 2026-10-27, evening — #199 → [#251](https://github.com/arieladi/Adi/pull/251) `window/arrangement`; #203 → `window/device-strip`

**#199, the arrangement, now #251, stacked on #248.** Its engine half is left out (N5, N6). The
rulings applied:
- **Live's layout:** the headers sit right of the clips.
- **Zoom around the selection:** + - and Ctrl/Cmd+wheel anchor on the selection's middle, else
  the insert marker, else the playhead. The cursor anchor is used only with
  `lookfeel.zoomOnSelection` off.
- **Live's keys and menus:** Z X H W, Home, Shift+Space, Ctrl/Cmd+T and +Shift+T, a Create menu,
  and right-click menus. They replace #199's buttons.
- **Track ids** come from the current cell, not from a raw `MAX(id)` read of the store.

Checks: 131 shell checks, with real mouse dispatch through #199's fake peer;
`test_all.sh` 8255 across 97 suites. Parity: `parity-7.3-arrangement.md` (22 rows).

**#203, the device strip, now [#254](https://github.com/arieladi/Adi/pull/254), stacked on #251.** Its `ParamOps::uiEvent`
is left out (N7), so the strip shows values and refuses edits, saying so in the Status Bar. The
rulings applied:
- **Live's Device View:** devices run left to right, and the controls sit in columns that fit
  the strip's height. #203 stacked them in a column that overflowed the floor.
- **The title bar:** the activator, the name and Configure. A double-click folds the device to a
  narrow bar. A right-click menu offers Fold, Configure and Delete.
- **The floor** comes from `DesktopDefaults`; the top edge drags the strip upward only.
- **Kept from #203:** the document's tick calls `ParamOps::drain`. It is named in the PR because
  STEP-7-PLAN §7 item 5 leaves "who calls drain" open.

Checks: 146 shell checks. Parity: `parity-7.4-devices.md` (14 rows).

**The first window.** The `ADI` app builds and opens its native window on macOS from a real
project file. LANES §5 makes "the first window" the director's gate "when 7.2 is merged". #246
and #248 are that, and they wait on win's review.

**The Mac was shared** with the devices lane's Codex build and Ableton all evening: load averages
of 50 to 190 on 8 cores, 8 GB. `adi_library_tests` (SQLite under fsync) ran for half an hour in
one harness run, but it passed; nothing of ours was implicated.

---

## 2026-10-27 — LANES §3b item 1: #196 → [#246](https://github.com/arieladi/Adi/pull/246) `window/root-window`; #198 → [#248](https://github.com/arieladi/Adi/pull/248) `window/application`

**#196, the root window, now #246.** Cut from `origin/main` (`a57d47a`, 142 commits past #196's
base). The claims-table edit and the win_codex handoff note were dropped. The rulings applied:
- Live's keys, §41:
  - Esc stops nothing.
  - Hide/Show Browser on Ctrl/Cmd+Alt+B and +5, Mixer on +M, Device View on +4.
  - Info View on Shift+?.
  - Zoom In/Out Window on Ctrl/Cmd + and -, each window with its own interface zoom.
- The Status Bar.
- The Info View, bottom-left beside the Device View, app-scoped.
- The AI tab collapsed at the browser's foot.
- The device strip's floor of 169 units, unverified, as a desktop default; it grows upward only.
- Floating tooltips after 600 ms.

Checks: `adi_ui_shell_tests` 82, `adi_ui_window_state_tests` 38; `test_all.sh` 8217 checks across 96
suites. Parity: `collab/window/parity-7.2-shell.md` (17 rows; 4 to measure).

**#198, the application, now #248, stacked on #246.** The rulings applied:
- **No Save.** Every op is committed as it happens (ADR-0203 d13), and quitting asks nothing. #198's
  Save (a WAL checkpoint on Ctrl+S) is gone.
- **Ctrl/Cmd+S takes a named snapshot** through `History::takeSnapshot`. The prompt is pre-filled
  with the project's name and the local time.
- **Ctrl/Cmd+Shift+S is Save As**, which waits for the engine (N3).
- **Ctrl/Cmd+, opens the Settings' Audio page**, the device chooser.
- **New makes no rows of its own.** #198 inserted the project row and the master with raw SQL;
  the Main track is now made through `track.create`, and the history's first step is protected
  (N1, N2).
- **The menus are Live's, as far as they go:** File, Edit, View. On the Mac they are the main menu.
  The Edit menu names the step it would undo.
- **A refused command is handled, not asserted.** JUCE asserted when a command was refused, or
  when commands were re-registered under changed names. A refused command now says why in the
  Status Bar.
