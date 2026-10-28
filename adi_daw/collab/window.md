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
| N14 | The registry's pages in Live's ten tab names and order, then ADI's own (ADR-0203 d11, Master Reference V.2): the Display & Input / Theme & Colors split of "Look & Feel", Link, Tempo & MIDI, Record, Warp & Launch, Licenses & Updates | win (`src/adi/settings/registry.cpp`) | the Settings window lists `settings::pages()` as they are, by design: one source for the left-hand list | the window shows the registry's pages in the registry's order |
| N15 | ADR-0104's library index (the app-scoped store of samples, presets and devices) and a catalogue of ADI's own devices by category, with a way to insert one (`device.insert` needs a plug-in reference row and the track's chain, neither of which the UI can make) | engine, devices | the browser's list (Live's folders under each category, double-click or drag to load) | the browser shows its sidebar and the list's header; a click on a category says it is not listed yet |
| N16 | The channel slots for ADI-Q and ADI-L on every channel and group (ADR-0204 d1, LANES §3 item 7): present but off, their on/off and ADI-L's mode in the snapshot, an op to switch each, and the engaged EQ's curve for the window to draw | engine (slots), clones (the plug-ins) | the Channel EQ window's switches and its display; the track rows' Q and L | every slot is shown off; a switch says what it waits for |
| N17 | The ops of phase C's window parts: clip fades (handles, Create Fades on Clip Edges off, d7), Insert Silence, and the import and bounce prompts' actions (import needs N5; bounce needs the render path) | engine | range selection's commands, fade handles, the two prompts (LANES §3b item 3) | nothing drawn that cannot act; the parts wait for their ops |
| N18 | A device's copy the window can read: what `device.remove`'s inverse captures (the plug-in reference, the stored params, the state's hashes, the panel and the route), from the snapshot or a read the UI may call, so a paste is a `device.insert` with a new id in front of the selected device (Live manual §23, p. 443) | engine | Cut, Copy, Paste and Duplicate in a device's menu and the Edit menu (§23.2.1) | the menu lists Delete and Rename; the four wait |
| N6 | The snapshot to carry what the arrangement draws: a track's `kind` (to place Main last, as Live's arrangement does), a clip's `name`, and the time-signature map (bars in the ruler). #199's `3feee62` adds all three to `engine/snapshot.*` | engine | the ruler and the clip labels | the ruler counts bars in 4/4 and says so in its tooltip; clips are drawn unnamed; Main is listed where the snapshot puts it |

---

## 2026-10-28, evening — the keyboard and the device menu [#269](https://github.com/arieladi/Adi/pull/269) [#270](https://github.com/arieladi/Adi/pull/270) [#271](https://github.com/arieladi/Adi/pull/271) [#272](https://github.com/arieladi/Adi/pull/272)

**The director approved the Settings and Channel EQ windows (#267, #268):** "The Settings and
Channel EQ windows look right. Continue with the next window items."

The three pieces the last entry named, each its own branch and PR, stacked in this order on #268:
- **#269 `window/settings-arrows`** (manual §40.1.2). With a control focused, the arrows switch a
  switch or step a value box, and Enter switches a switch; each change is applied and written at
  once. A value box now takes the focus, so Tab reaches it. The keyboard's focus is outlined in the
  accent; a click's is not. Settings row 9 is built.
- **#270 `window/quit-key`** (§41.3). A Quit command: Ctrl+Q and the foot of the File menu off the
  Mac; on the Mac Cmd+Q stays the application menu's. Both reach `systemRequestedQuit`, which asks
  nothing. Application row 10 is built.
- **#271 `window/device-rename`** (§23.2.1). The device menu in Live's order (Delete; Rename; Fold,
  Configure). Rename, from the menu or Ctrl/Cmd+R with the device focused, is typed in the title
  bar and commits one `device.rename`. The Edit menu's Rename acts on the focused device, else the
  selected track. Cut, Copy, Paste and Duplicate wait for a device's copy (N18, new).
- **#272 `window/parity-shell-space`**, a checklist correction: 7.2-shell rows 10 and 11 (Space,
  Shift+Space) were built with the arrangement but still read open.

Checks on #271, the tip: 291 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow
sweep is clean. It caught a `host` shadow in #269's new test before push (MSVC C4456).

**Where to resume:** `~/adi-window` on `window/parity-shell-space`, the tip of the stack. Item 3's
phase C parts still wait for N17. The next pieces the window can finish alone:
- a floating window's interface zoom (7.5 row 10), as the main window's Ctrl/Cmd + and -;
- the menu bar's other menus (7.2 row 11), as far as there are commands to list.
Also: the native Settings smoke in `ui.yml` (win's file) once win agrees.

---

## 2026-10-28, later — the Channel EQ window [#268](https://github.com/arieladi/Adi/pull/268) `window/channel-eq`

**#268, stacked on #267.** ADR-0204 d1's one EQ window: every channel's ADI-Q and ADI-L in one
view, FabFilter's "show all instances" made native.
- EQ Eight's display, where each engaged EQ's curve will be drawn in its track's colour.
- One row per channel with its two switches and ADI-L's mode.
- It opens from the View menu's Channel EQ and from a track row's Q or L, in the floating host,
  one per project.
- Its slots are present and off until the engine's channel slots and the clones' plug-ins
  arrive (N16, new).

Checks: 281 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow sweep is clean.

**Item 3's state:**
- The Settings window (#267) and the EQ/limiter window (#268) are done.
- Phase C's window parts wait for their ops: fades, Insert Silence, and the import and bounce
  actions (N17, new). They are not drawn before they can act.

**Where to resume:** `~/adi-window` on `window/channel-eq`, the tip of the stack. With item 3
blocked, the next pieces are the parity rows marked partial that the window alone can finish:
- arrows on a focused Settings switch and value box;
- Ctrl+Q on Windows;
- Cut, Copy and Rename in a device's menu, where ops exist.
Also: the native Settings smoke in `ui.yml` (win's file) once win agrees.

---

## 2026-10-28 — the Settings window in Live's shape [#267](https://github.com/arieladi/Adi/pull/267) `window/settings`

**#267, stacked on #266. LANES §3b item 3 begins.** Measured against Live 11.2.7's Preferences
window, captured on the director's Mac once the macOS prompt that covered Live was answered.
- Live's tabs beside a rounded page panel. Rows are typed from the registry at Live's measures
  (names at 17, controls at 191, 197 and 97 wide, a 21-unit pitch).
- Every change is applied and written at once, with no OK button.
- REAPER's Find box, a help line at the foot, Restore Defaults per page, and the Project and
  Device marks.
- Live's keys: Ctrl+Tab and Option+Tab, the arrows on the tab list, and Esc.
- The Audio page leads with Live's Audio Device, Sample Rate and Latency sections over the
  device manager (`app_audio_page`). This replaces the JUCE selector dialog Cmd+, opened.
- `--smoke-settings` renders that page natively.
- The director has the side-by-side pictures of the Settings window and the Mixer View.

Checks: 277 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow sweep is clean
(it caught an MSVC C4456 in the new test before push). `parity-settings.md` lists what waits:
the registry's tab names and sections (N14).

**Next in item 3:** the EQ/limiter window (ADR-0204 d1). Then the phase C window parts: their
ops (fades, Insert Silence, the import and bounce prompts) are not in the catalogue yet, so they
will be logged as needs where they block.

---

## 2026-10-27, night — the Mixer View in Live's shape and its peak levels [#266](https://github.com/arieladi/Adi/pull/266) `window/peak-indicators`

**#266, stacked on #265.**
- The Mixer View is measured on Live 11's Session mixer (Live 11's Mixer View): 93-unit
  strips, the name in the track's colour, and ADI-Q and ADI-L where Live has the sends.
- The peak level pill (manual 18.1) is resettable by a click and turns red once the track
  clips. That was 7.6's last open row (13b).
- The meter and the fader share Live's measured scale, so the ◁ sits at the meter's dB.
- Checks: 265 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow sweep is clean.

**§3b item 2 is done** as far as nothing blocks it. What remains open there waits on other
lanes:
- the audio drop (N5);
- plug-in windows (N8);
- returns, Main last and the meters' sources (N6, N9, N11);
- the browser's list (N15, new).

**Next: item 3, starting with the Settings window in Live's shape** (ADR-0203 d11, Master
Reference Part V). Its pages are the registry's; their names in Live's ten tabs are win's
(N14, new).

**Driving Live:** a macOS prompt ("claude" would like to access data from other apps) sits over
Live and blocks clean captures of Live's Preferences. It is the director's to answer; nothing
clicks it.

---

## 2026-10-27, last — Live's look [#265](https://github.com/arieladi/Adi/pull/265) `window/live-look`; approved

**Approved by the director, 27 October:** "Approved, the look is close enough to Live. Continue
with the next window items." The hold on new windows is lifted. Each new window is measured
against Live 11 the same way, and the director sees pictures as each one lands.

**The director's ruling (27 October):** the window "looks very far from Ableton". It must look
like Live, plus what the Master Reference adds. He approves from pictures, and no new window is
built until he does. Item 3 and the rest of item 2 are held until then.

**What was done:**
- Measured Live 11.2.7 on his Mac (1440 × 900, default theme) region by region: colours by
  histogram, edges pixel by pixel.
- Restyled the window to those measures, now #265, stacked on #264.
- The numbers and the gaps that remain are in `collab/window/look-live11.md`.
- `liveParity` renders Live's default set as ADI at the same size. It is the golden
  `live-parity.png`. `ADI_UI_PARITY=<folder>` writes a 2x render to lay beside a capture.
- Sent the director side-by-side pictures: the whole window, the track headers, the detail row,
  and the control bar with the browser.

**What the measures changed:**
- The device strip's floor is 196, the whole Device View panel; its body is about 170. The
  director's 169 is the body. Both docs now say so.
- The frame is 8 units between panels, with 4-unit panel edges. The Status Bar is 38.
- A track is 88 tall. New tracks are named "# Audio" and "# MIDI", as Live's.
- The default panel widths are 316 for the browser and 221 for the track controls.

**What is not like Live, and what it waits on:**
- The browser's list: its content model.
- Returns and Main shown last, and MIDI tracks drawn as MIDI: N6 and N11.
- The loop brace: the transport's loop.
- Live's own device displays: ADR-0150.
- The typeface.

**Checks:**
- 259 shell checks; all goldens re-recorded, with masks over every new place that shows words.
- `test_all.sh`: 8265 checks across 97 suites, PASS; window state 48, document 38.
- The Clang `-Wshadow-all` sweep is clean.
- Found on the way: a negative-size fill in the mixer panel when the tracks run past it. It is
  fixed, and the JUCE assertion it raised is gone.

**Where to resume:** `~/adi-window` on `window/live-look`, the tip of the stack. Once the
director rules on the pictures, apply his corrections here first, then carry on with §3b item 2
and item 3.

---

## 2026-10-27, later still — the Mixer View [#263](https://github.com/arieladi/Adi/pull/263) `window/mixer-view`

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

**The scrub area, now [#264](https://github.com/arieladi/Adi/pull/264) `window/scrub`, stacked
on #263.**
- A 14-unit strip under the ruler; a click there plays from that point, or jumps there while
  playing, through `AdiRootComponent::playFrom`.
- Unquantized until the global launch quantization exists; hold-to-loop waits for a transport
  loop.
- Checks: 250 shell checks, a real click through the fake peer; `test_all.sh` 8265 across 97
  suites.

**CI:** no failures on #251 to #264; the queued macOS legs are still running.

**Where to resume:** the worktree is `~/adi-window`, on `window/scrub`, the tip of the stack.

**Next in §3b item 2:**
1. The browser's content.
2. Live's resettable peak indicators on a tall Mixer View.

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
