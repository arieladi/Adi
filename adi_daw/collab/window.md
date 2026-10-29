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
| N19 | The transport's metronome and loop: `transport.setMetronome` and `transport.setLoop` are in the catalogue but apply nothing (`transportNoop`); the snapshot carries neither the metronome's state nor the loop region, and nothing clicks or loops | engine | the control bar's Metronome and Loop switches, Live's O key (§42.20) and Ctrl/Cmd+L, the loop brace (§6.6, §42.16) | the switches are drawn and say they are not built; nothing is journaled for them |
| N20 | A home for the channel configuration: `audio.channelConfig` is Device scope, which `AppSettings::set` refuses ("belongs to a device"), and its text has no format; the app also opens the device with no inputs (`initialise(0, 2, ...)`) | win (the store and the registry); engine for recording | Live's Input and Output Config dialogs on the Audio page: which channels are used, mono or stereo pairs, their names per device (online §17.2, Routing and I/O; parity-settings row 7) | nothing built; the dialogs are a new window, so their pictures would go to the director first |
| N21 | WITHDRAWN 2026-10-28: both settings exist in the registry (`lookfeel.wrapTabNavigation`, `editing.arrowKeysMoveClips`); the window follows them itself (master-reference-audit.md) | — | — | — |
| N22 | A track's delay in the snapshot (`mixer.setDelay` exists) | engine | Track Delay in ms on every strip (MR 7, 8.1) | nothing shown |
| N23 | Solo in Place (a setting or a project field, and the engine following it) and the cue path: Solo/Cue, Cue Out, Cue Volume | engine; win for the registry row | the Solo button's menu and the Options menu (MR 8.1) | Solo is plain solo |
| N24 | Live's Delay Compensation switch: a setting and the engine following it, Track Delay off while it is off | engine; win for the row | the Options menu (MR 7, 8.4) | nothing shown |
| N25 | Markers and locators: ops, and the snapshot carrying them, a marker holding a note and a prompt | engine | locators, the Set button, markers with prompts (MR 3.3) | the Set button is drawn and says it is not built |
| N26 | Automation data in the snapshot (lanes, points, the overridden state) | engine | automation lanes, Re-Enable Automation lit (MR 6.1) | the two buttons are drawn |
| N27 | Remarks in the snapshot | engine | the remark pip and the Info View's split (MR 3.5) | nothing shown |
| N28 | A device's sample slot the window can fill (an op), and the missing-file mark in the snapshot | engine, devices | Color Cab's drop tile "with step 7" (MR 9.8, 9.3) | nothing shown |
| N29 | History's list of undo steps (labels, newest first, with branches and snapshots) | engine (`src/adi/history`) | Live's Undo History window, View menu, Ctrl+Alt+Z (MR V.4 §4) | Undo and Redo only |
| N30 | Save a Copy, Collect All and Save, and Collect and Export (the ZIP) in the store | engine (N3's family) | the File menu (MR 1.6, 11.3) | not listed yet |
| N31 | Per-track ops for Rec-Q / Play-Q and Keep Monitoring Latency | engine | the MIDI track header and the monitor button's right-click (MR V.2 §9) | nothing shown |
| N32 | A clip's notes, with each note's identity as the note ops address it, and the clip's own properties (kind, name, colour, start and end markers, loop window, signature, scale) where the window can read them: in the snapshot, or a read the UI may call on the message thread when the journal moves, as `MidiClips` reads `rows::Model`; the engine lane's choice | engine | the Clip View's title bar and Main panel, and every note the Note Editor draws or edits (STEP-7-PLAN §7.7) | the view's frame and the note geometry are built without it; the editor draws no notes |
| N33 | Ops for a clip's start and end markers, its loop window and loop switch, its signature and its scale (Live's Main Clip Properties) | engine | the Main panel's fields and Set buttons, the loop brace, Fit to Scale (§7.7) | the fields show the snapshot's position and length, read-only |
| N34 | An op for a note's active flag (the `flags` column exists) | engine | 0 deactivates a note (Live manual §10.5.7) | nothing |
| N35 | The device tests write their renders (`*-pd-render.wav`, 16 files) into `adi_daw/` itself, wherever `test_all.sh` is started from, so every full run leaves untracked files in the tree: a `.gitignore` line, or the renders written to a temporary folder | devices (`tests/test_*_pd.cpp`) | a clean tree after the gate the window runs before each push | the window deletes them after each run |
| N6 | The snapshot to carry what the arrangement draws: a track's `kind` (to place Main last, as Live's arrangement does), a track's `parent` and `color` (WIDENED 2026-10-28: the director's Bitwig project view lists the groups as a tree with their colours, and a group's tracks show beneath it), a clip's `name`, and the time-signature map (bars in the ruler). #199's `3feee62` adds all three to `engine/snapshot.*` | engine | the ruler and the clip labels | the ruler counts bars in 4/4 and says so in its tooltip; clips are drawn unnamed; Main is listed where the snapshot puts it |

---

## 2026-10-29, cloud — Bitwig's project view [#295](https://github.com/arieladi/Adi/pull/295) `window/view-filter`

The director said "continue" without choosing how the window gets the groups. So the window built
everything that does not need them, ready for N6.

**#295, stacked on #294:**
- `TrackTree` (headless): groups inside groups with their depth, and what a view of a group shows (the
  group, what is inside it at any depth, and Main). A parent that loops back ends the walk.
- `ProjectViewButton` at the top of the track names, lit while a group is chosen. Its menu: Project,
  then the groups indented, each with a folder in its colour.
- `ArrangementGeometry::hidden`: a hidden track's height is 0. Every walk passes over it: the lanes,
  the names, the track controls, the Mixer View's columns, the Overview, the hit tests, the screen
  reader's clips, up and down, and Neighbor.
- `WindowState::viewGroup` keeps the view per window. A vanished group falls back to the whole
  project.
- The frame audit ("generation-change frame allocates zero") caught the first version. The tree is now
  built only while a group is chosen or the menu is open, in buffers kept between frames.
- Without N6 the menu shows Project alone and says the groups appear once the engine reports them.
  The test gives the window a tree.

The picture `pictures/2026-10-29-project-view.png` was sent to the director: the real menu, and the
test's render with a nested group chosen.

Checks: shell 413, seams 103, window state 54; `test_all.sh` 8278 PASS; zero warnings; the shadow
sweep is clean. The rows check fails with the mixer's skip removed.

`window/view-filter` is merged into `window/clip-view`.

**Where to resume:**
- the director's choice on the groups' data: N6 from the engine lane (recommended), or the window
  reading the store's track rows (STEP-7-PLAN §2.2);
- the Clip View's approval;
- CI on #293 to #295.

---

## 2026-10-28, cloud, continued — the flip arrows, Live's strip, Use Large Track Height [#293](https://github.com/arieladi/Adi/pull/293) [#294](https://github.com/arieladi/Adi/pull/294)

**The director, on screenshots (28 October).**
- Yellow mark: small arrows that say "Flip sides" on hovering, under the strip's two round switches.
- Green mark: in flip mode, the column the strip took between the track controls and the lanes
  belongs to the track controls.
- He still doesn't see Bitwig's project view: the button where he decides to see the whole project
  or a group, with the groups' layers shown as a tree (his Bitwig screenshot).
- He wants Bitwig's Use Large Track Height button.

**#293's third commit:**
- Live's strip becomes its own component, `SideStrip`.
- `FlipButton` sits under the switches. It shows "Flip sides" in the tooltip and the Info View, is lit
  while flipped, and takes no keyboard focus.
- Flipped, the strip is at the window's left edge: strip, names, track controls, lanes.
- The band's Swap button is gone. The View menu item is now Flip Sides.
- The arrangement now ends where the right-hand panel begins. With the browser on the left and the
  track controls hidden, the old formula gave it zero width. That bug predated #293; a new check fails
  on it.
- `root-swapped` is re-recorded on purpose.

**#294, `window/track-height`, on #293:**
- `ArrangementGeometry::largeTracks`: off, every track shows at half its height. Heights are kept large;
  a resize while small is stored at twice the size shown, and Optimize Height uses the same path.
- A Bitwig-icon button under the flip arrows, and the View menu's ticked Use Large Track Height.
- Kept per window in `WindowState::largeTracks`.
- Checks: shell 405, window state 53; `test_all.sh` 8270 PASS. The halving check fails without the
  halving.

**The project view: what blocks it.** The snapshot still carries no track parent, kind or colour.
N6 is widened to say so. The design follows his Bitwig screenshot, replacing the funnel picture:
- a tree icon with ▾ at the top of the track names;
- its menu: Project, then the nested groups with folder icons in their colours;
- the button lit while a group is chosen. It is the warning light, so no separate funnel is needed.

He is asked to choose:
- the engine lane fills N6 (recommended);
- or the window reads the track rows from the store on the message thread after each edit, as the
  engine's `MidiClips` does. This changes STEP-7-PLAN §2.2's single read path, so it needs his ruling.

Picture branches merged forward, with trailers, and `window/clip-view`'s plan corrected for the arrows.

**Where to resume:**
- his ruling on the project view's data;
- then build the button, the menu and the filter (Main always shown);
- the Clip View's approval.

---

## 2026-10-28, cloud, continued — Swap moved above the track controls [#293](https://github.com/arieladi/Adi/pull/293)

**The director, on a screenshot of the Clip View picture.** He marked in green two empty areas as
room for buttons like the swap:
- the band above the track controls, level with the ruler;
- the strip down the right-hand edge.

He also answered the view filter's questions:
- Main is always shown in every view: yes.
- The funnel as the un-filter toggle: not sure; he asked for more explanation. The answer went to
  him with the options and a recommendation; his ruling is awaited.

**#293's second commit.** Swap leaves the Status Bar and becomes the track controls' own button:
- it sits in the band, travelling with the controls, and is lit while they are on the left;
- the Mixer View has none;
- it takes no keyboard focus, and the View menu is its keyboard path.

Why no focus: as a focusable child at the panel's top, it took the focus whenever a row holding it
was hidden on a scroll, and Next/Previous Neighbor stopped there. The existing check caught that.

New check: the button's bounds in the band, on either side. No golden moves, because the band's
buttons and the Status Bar are masked as typography. The PR body and the swap picture are updated.
- Checks: shell 401, window state 52, document 38, seams 96; `test_all.sh` 8269 PASS; zero warnings;
  the shadow sweep is clean.

Merged into the stacked picture branches:
- `window/swap-panels` into `window/view-filter`. Its merge commit was pushed with git's default
  message and so lacks the trailers; it stays as pushed (no force-push).
- `window/view-filter` into `window/clip-view`.

`window/clip-view`'s plan, parity row 10 and picture now say the Status Bar's second field is free for
the Clip View's toggle and selector.

New Need N35: the device tests leave 16 render files in `adi_daw/` on every full run.

**Where to resume:**
- the director's ruling on the funnel;
- his approval of the filter and Clip View pictures;
- then build the view filter (Main always shown) and the Clip View's frame.

---

## 2026-10-28, cloud, continued — the Clip View and the Note Editor: plan and picture `window/clip-view`

Branch `window/clip-view`, cut from `window/view-filter`. It holds no code. No PR until the build
starts.

**Read for it:** Live 12 manual ch. 8 "Clip View" and ch. 10 "Editing MIDI" (the online edition,
fetched 28 October), with the manual's own figures of the MIDI and audio Clip View; Master Reference
4.1 and 5.1.

**What the branch holds:**
- **The plan:** STEP-7-PLAN §7.7.
  - The view is Live 12's: a title bar; panels on the left (Main, then Pitch and Time, Transform and
    Generate for MIDI); the editor on the right with its tabs.
  - It sits in the detail row beside the Info View, stacked above the Device View.
  - The build order: the frame; the note geometry, headless; notes; the Pitch and Time tools,
    composed from the five note ops that exist; what comes later.
- **The checklist:** `parity-7.7-clip-view.md`, 37 rows.
- **The picture,** sent to the director: `pictures/2026-10-28-clip-view.png`.
  - Drawn over the real window: a MIDI clip with the Note Editor and the Velocity lane; an audio clip
    with the Sample Editor.

**What the window cannot read.** The snapshot gives a clip only its id, position, length, mute and
gain. The notes and the clip's properties are in the store (`rows::Model`), which the UI may not read
by raw SQL. New Needs:
- N32: a clip's notes and its own properties where the window can read them;
- N33: ops for the Main panel's properties;
- N34: an op for a note's active flag.

**The Status Bar.** Its second field is where Live 12 puts the Clip View toggle and selector. When the
view is built, the Swap button (#293) moves just left of that field. This is in the plan and in the
picture.

**Where to resume:** the director's answers on the two pictures (the view filter; the Clip View). If
the Clip View is approved, its first step (the frame) and the note geometry can be built before N32.

---

## 2026-10-28, cloud, continued — the view filter's picture, for approval `window/view-filter`

Branch `window/view-filter`, cut from `window/swap-panels` (#293). It holds only the picture so far
(`collab/window/pictures/2026-10-28-view-filter.png`), which was sent to the director. No PR until
it is built.

The picture is drawn over the real app, with a project of three groups made through `adi_tool
propose --apply`. It has four panels:
1. A chooser at the top of the track names, level with the beat ruler, opens the filter. Its Project
   tab lists, in order:
   - the groups, as Figure 32;
   - the saved views;
   - Show Only Selected Tracks, Name Contains and Save View As.
2. The Manual tab: tick the tracks to show. Main is always shown.
3. "Body" chosen: only the group and its tracks, in the lanes and the track controls alike.
4. The funnel beside the chooser is the un-filter toggle:
   - grey when there is no view;
   - lit while a view applies;
   - unlit with every track shown and the view kept.

Bitwig's online user guide does not describe this menu, so Figure 32 and ADR-0112 d2 are the spec.

Wait on N6:
- the Groups list;
- the tracks' own colours.

Built once approved:
- the chooser;
- the Manual tab;
- Show Only Selected Tracks;
- Name Contains;
- saved views (per project, as view state);
- the funnel.

Seen while making the picture, for later:
- With the Device View hidden and the Info View shown, the detail row stays the device floor's height
  and the arrangement does not grow beside the Info View (parity-7.2 row 3 has it as "measure").
- The H and W buttons don't fit the arrangement yet.
- Shift+click doesn't select several tracks (the audit's queue item 7).

**Where to resume:** the director's answer on the filter picture. Meanwhile, the clip view and the note
editor: plan and pictures first.

---

## 2026-10-28, cloud, continued — the side swap shown; swapped, the names lead [#293](https://github.com/arieladi/Adi/pull/293) `window/swap-panels`

**The director's rulings on the audit's four questions (28 October).**
1. The swapped layout is C: the track names, the track controls, the strip, then the lanes; the
   browser on the right.
2. The swap's control sits in the Status Bar.
3. Yes to the view filter, Figure 32 style, its picture first.
4. The clip view and the note editor come next.

He also clarified drops, as in Ableton:
- files, synths and plug-ins drop on an audio, MIDI or synth track, or on the empty area;
- a device or plug-in dropped on a track goes to its device chain below, never into the lanes.

This waits on N5 and N15 and is recorded on the audit's drop row.

He asked where the switch-sides button was in the earlier pictures. I answered that there was none:
- picture A came from View › Swap side panels;
- B and C were mock-ups.

**#293, stacked on #292**; the audit's item 1.
- A Swap button in the Status Bar beside Mixer, lit while swapped. It calls the same command as View
  › Swap side panels.
- When swapped, `ArrangementView::setNamesFirst` leaves a gap between the names and the lanes, and the
  root puts the track controls in it. The ruler, the scrub area, the Overview and the Mixer View start
  at the lanes.
- Live's sides are unchanged.
- The `root-swapped` golden is re-recorded on purpose; no other golden changed.
- A real-app picture, before and after a click on Swap, is
  `collab/window/pictures/2026-10-28-swap-button.png`. It was sent to the director.

Rows updated:
- parity-7.2-shell row 25 (new, built);
- the audit's swap row, its questions (marked ruled), its queue (renumbered) and its drop row;
- UI-ARCHITECTURE's layout section.

Checks:
- shell 400, window state 52, document 38, seams 96;
- `test_all.sh` 8269 across 97 suites PASS;
- zero warnings for `adi_app` and the UI tests; the shadow sweep is clean;
- the new order checks fail with names-first disabled.

Not caused by this branch: in a local `-Werror` build of everything, GCC rejects `adi_audio_probe` and
`adi_vst3_probe` on JUCE's own `juce_UMPUtils.h`. CI builds the probes only on macOS and Windows, and
without `-Werror`.

**Where to resume:** `window/swap-panels` (#293) is the tip. Next, as ruled:
1. the view filter's picture for the director: a menu at the top of the track-header column with the
   group tree, "Only these tracks", and an un-filter toggle;
2. then the clip view and the note editor: plan and pictures first, from Live 12 manual chapters 8 and
   10 and Master Reference 4.1 and 5.1.

The audit's later items follow.

---

## 2026-10-28, cloud, continued — Wrap Tab Navigation, Move Clips with Arrow Keys [#292](https://github.com/arieladi/Adi/pull/292) `window/settings-nav`

**#292, stacked on #291**; the audit's item 2. Both settings were already in the registry, so N21 is
withdrawn (#291's entry).
- **Wrap Tab Navigation** is the Navigate menu's second ticked switch (`lookfeel.wrapTabNavigation`,
  on, as Live's). On, Tab from the window's last control goes to its first; off, Tab stops at the last
  and Shift+Tab at the first. The window's content is now a keyboard focus container.
- **Move Clips with Arrow Keys** off (`editing.arrowKeysMoveClips`): left and right collapse a
  selection to its start or end and move nothing.

Rows updated: parity-7.2-shell row 21 (built; measure what Live counts as an area) and
parity-7.3-arrangement row 30.

Checks: shell 397, window state 52, document 38, seams 96; `test_all.sh` 8269 across 97 suites PASS;
zero warnings; the shadow sweep is clean (one shadowed `why` in the test, renamed). The Wrap check was
shown to fail with the stop disabled.

**Where to resume:** `window/settings-nav` (#292) is the tip. Next, the audit's item 3: the Options
menu's Chase MIDI Notes, MIDI Envelope Auto-Reset and Reduced Latency When Monitoring, from their
settings; then Edit MIDI Map and Edit Key Map; then Exclusive Solo followed. The swap and the filters
wait for the director's answers to the audit's questions 1 to 3.

---

## 2026-10-28, cloud, continued — the Master Reference against the window [#291](https://github.com/arieladi/Adi/pull/291) `window/mref-audit`

**The director's message.** The swap of browser and channels is not to be seen, nor a Bitwig-style
filter that shows one group or folder; "go over my document and make sure all we describe is there".
He uploaded the Master Reference v1.2 to main (`adi_daw/5_Adi-Daw Master Reference (claude draft
v1.2) preview.pdf`, 190 pages).

**Read in full** (text and figures), and every interface item checked against the code:
`collab/window/master-reference-audit.md`, one row per item, built / partial / hidden / missing / waits /
later.
- **The swap is built but hidden.** It is View › "Swap side panels" only: no key, nothing on screen.
  After a swap the track names stay right of the clips while their controls go left.
- **The filters and views are missing:** the named view filters (MR 10, figure 32, Bitwig's group
  menu), the three view states, the current view state in the control bar, and project tabs.
  Groups wait for N6.
- **Other gaps:**
  - the palette;
  - the strip's scope and the Scope window;
  - the Parameter List and the parameter context menu;
  - a scrolling browser sidebar (Places is cut off on a short window);
  - the Options-menu items whose settings exist;
  - Settings presets and profiles, related-word search, and Restore "Don't Show Again";
  - Safe Mode and the sample-rate bar;
  - the clip view and the note editor (P0, and not in step 7's plan).
- **N21 was wrong and is withdrawn:** both settings exist. New Needs N22 to N31, from the audit.
- **Pictures for the director:** `collab/window/pictures/2026-10-28-swap-track-names.png`.
  A is today; B is Live mirrored; C is names first, as Bitwig and Cubase.

**For the director:** four questions, at the audit's end:
1. Which side the track names go on after a swap (B or C).
2. The visible swap control, proposed in the Status Bar beside Mixer.
3. The view filter's control.
4. Where the clip view and the note editor go in the queue.

**Parked:** `window/track-move` (dragging tracks by their title bars, Insert Track renumbering) is
stashed locally, unfinished, and resumes after the audit's first items.

**Where to resume** (superseded by the entry above): `window/mref-audit` (#291) was the tip.

---

## 2026-10-28, cloud, continued — Live's Arrangement keys [#290](https://github.com/arieladi/Adi/pull/290) `window/arrangement-nav`

**#290, stacked on #289** (parity-7.3-arrangement row 30, new). ADI's left and right arrows
stepped from clip to clip; that was this lane's own choice (2026-10-27). Live 12 spells out its
keys in online §41.4.1.3, and those are now built:
- **Left and right** move the insert marker by the grid in use.
- **Ctrl (Option on the Mac) with them** go to the previous or next clip edge on the track.
- **Shift with them** selects. A selection that is exactly a clip selects the clip.
- **With a clip selected** they move it a grid line: one `clip.move` per press, "Move Clip".
  This is Live's Move Clips with Arrow Keys, on by default (§41.1.1).
- **With a time selection** it moves by the grid when it holds no clip. When it holds part of a
  clip it stays, and the Status Bar says that waits for split (N17).
- **Enter at a clip's end**, then left and right, changes its length. Enter keeps it as one
  `clip.resize`, "Resize Clip"; Esc, a click or any other key cancels.
- **Enter at a clip's start** says moving the start waits for a content offset.
- A screen reader hears where each key landed.

New need **N21** (win's registry): Display & Input's Wrap Tab Navigation and Move Clips with Arrow
Keys. Until then the arrows always move clips, as Live's default does.

**For the director:**
- Shift with Ctrl (Option) selects to the next clip edge, by analogy with the MIDI editor's note
  boundaries (online §10.5.2). On Windows that editor takes Alt for it, so this is marked
  "measure".
- What Live says to a screen reader is also to be measured.

Checks: shell 393, window state 52, document 38, seams 96; `test_all.sh` 8269 across 97 suites
PASS; zero warnings; the shadow sweep is clean (it caught one shadowed `sel`, now fixed). Under
Xvfb, a click and Shift+Right twice select 1.4.1 to 2.2.1 on the quarter grid.

**Where to resume** (superseded by the entry above): `window/arrangement-nav` (#290) was the tip.

---

## 2026-10-28, cloud, continued — #288's Windows golden; the grid's named widths [#289](https://github.com/arieladi/Adi/pull/289) `window/grid-widths`

**CI.** On #274 to #288: one failure, on #288, `GUI shell / windows-latest`, which failed
`live-parity` by 310 differing pixels. Every other finished run passed, and none of these PRs has
a comment or review. The only pending runs are macOS legs, queued since about 07:00Z.
- **Cause.** #288 lengthened the canvas's description. The parity scene hovers the canvas, so the
  Info View shows it. With Windows' fonts the body wraps one line more, and `TextLayout::draw`
  does not clip, so the text spilled past the masked area.
- **Fix, on #288 (`e386a6e`).** The Info View's body is clipped to its panel. A new check hovers
  a description far longer than the panel and finds no pixel changed outside it; it fails
  without the clip. The Linux golden is unchanged. #288 is the tip, so nothing above it needed
  the fix.

**#289, stacked on #288** (parity-7.3-arrangement row 26, now built with measures). The lanes'
menu names the grid's widths (online §6.10):
- **Adaptive Grid ▸** Widest, Wide, Medium, Narrow, Narrowest.
- **Fixed Grid ▸** 8 Bars to 1/32.
- The width in use is ticked, followed by Triplet Grid and Snap to Grid. The Options menu keeps
  its five commands.
- Ctrl/Cmd+1 and 2 stay within those names: adaptive ±2 steps (was ±3), fixed down to 1/32 (was
  1/128). A saved width beyond them loads as the nearest.
- **For the director:** the online manual names neither list. The names are as the window
  recalls Live, and which adaptive name is Live's default (Medium, ADI's default) is to be
  measured.

Checks: shell 380, window state 52, document 38, seams 96; `test_all.sh` 8269 across 97 suites
PASS; zero warnings; the shadow sweep is clean. Under Xvfb, Fixed Grid ▸ 1/8 sets the ruler's
spacing to "1/8" and the lanes draw eighths.

**Where to resume** (superseded by the entry above): `window/grid-widths` (#289) was the tip.

---

## 2026-10-28, cloud, continued — a clip's right edge [#288](https://github.com/arieladi/Adi/pull/288) `window/clip-resize`

**#288, stacked on #287** (parity-7.3-arrangement row 27). §6.7's edge drag, its right half:
- over the last 4 units of a clip's bar the cursor is a bracket;
- a drag lengthens or shortens the clip, its end on the nearest grid line (the bypass key as
  for moves), never shorter than one grid step, drawn as it goes;
- on release it is one `clip.resize` with the same `pos`, "Resize Clip".
- One hit test, `barAt`, serves the move and the edge.
- The left edge waits for a content offset in `clip.resize`; without one it would slide the
  audio.

Checks: shell 370, document 38, window state 51; `test_all.sh` 8268 across 97 suites PASS; zero
warnings; the shadow sweep is clean.

**CI** at 08:41Z on #274 to #287: no failure, no comment, no review; only macOS legs were
pending, plus #286's and #287's runs in progress.

**The session hit a usage limit and a worker restart after #288 went up; nothing was lost.**

**Where to resume** (superseded by the entry above): `window/clip-resize` (#288) was the tip.

---

## 2026-10-28, cloud, continued — Live's clip editing [#287](https://github.com/arieladi/Adi/pull/287) `window/clip-edits`

**#287, stacked on #286** (parity-7.3-arrangement rows 27 to 29, new). The catalogue has had
`clip.move`, `clip.delete` and `clip.setMute` all along; the window had wired none of them.
- **Move (§6.7).** A drag on a clip's bar moves it. While dragging it is drawn where it would
  land, on the nearest grid line (Alt, Cmd on the Mac, bypasses). On release it is one
  `clip.move`, "Move Clip", and the selection follows. A drag on the body still selects time.
- **Delete (§6.9).** Delete or Backspace with a clip selected: one `clip.delete`, "Delete Clip".
- **0 (§6.9).** It deactivates the selected clip, and again activates it (`clip.setMute`). A
  deactivated clip is drawn grey.
- **Open:**
  - moves across tracks wait for the clip's and the track's kind in the snapshot (N6);
    `clip.move` does not check them either;
  - snapping to clip edges and locators;
  - edge drags: `clip.resize` takes no content offset, so a left-edge drag would slide the audio.

Checks: shell 368, document 38, window state 51; `test_all.sh` 8268 across 97 suites PASS; zero
warnings; the shadow sweep is clean. The tests drag clip 1's bar through the fake peer with a
real engine behind it, and undo each step.

**Where to resume** (superseded by the entry above): `window/clip-edits` (#287) was the tip.

---

## 2026-10-28, cloud, continued — Live's editing grid [#286](https://github.com/arieladi/Adi/pull/286) `window/grid`

**#286, stacked on #285** (parity-7.3-arrangement row 26, new). Live's grid (online §6.10,
§42.13): the lanes drew a zoom-adaptive grid, but nothing snapped and no command existed.
- **The model.** `ArrangementGeometry::Grid` (snap, fixed, triplet, narrow, the fixed step),
  with `gridQuarters()`, `snap()` and `gridLabel()`. Kept per window in `ui_view`. The default
  step is the one always drawn, so default pixels are unchanged.
- **Snapping.** A click snaps the insert marker to the line at or before it; a drag covers
  whole cells. Alt (Cmd on the Mac) bypasses snapping, or snaps while the grid is off.
- **The commands.** Narrow, Widen, Triplet, Snap and Fixed on Ctrl/Cmd+1 to 5, in the Options
  menu and the lanes' menu, with ticks.
- **Drawing.** The spacing shows at the time ruler's right end. Triplets keep the lanes'
  shading on the bars.
- **Goldens.** "arrangement" and "devices" are re-recorded on purpose. Their test drag now ends
  on a grid line (11 pixels on two rows); the region was compared old beside new first.
- **Seen in the app under Xvfb:** Ctrl+1, Ctrl+3 give the denser triplet grid and "1/8T". The
  first capture showed the label clipping the last time stamp, which is fixed.
- **Open:** Live's named widths in the lanes' menu (Widest to Narrowest; 8 Bars to 1/32), and
  clip moves, which do not exist yet.
- **For win to measure:** Live's adaptive steps per zoom, and nearest versus previous line on a
  click.

Checks: shell 362, window state 51, document 38; `test_all.sh` 8268 across 97 suites PASS;
zero warnings; the shadow sweep is clean.

**Learned:** JUCE builds a mouse event's keys from `ModifierKeys::currentModifiers` (the
keyboard's state), not from the modifiers handed to the peer's `handleMouseEvent`. A test of a
modified gesture sets that state and clears it after.

**Where to resume** (superseded by the entry above): `window/grid` (#286) was the tip.

---

## 2026-10-28, cloud, continued — a mixer neighbour in view [#284](https://github.com/arieladi/Adi/pull/284); the View menu's toggles [#285](https://github.com/arieladi/Adi/pull/285)

**Resumed on "continue".** CI on #265 to #283 had no failure, no comment and no review. Only
macOS legs, and two Windows legs on #283, were still queued or running.

**#284 `window/mixer-scroll`, stacked on #283** (parity-7.6 row 16). The Neighbor keys now go on
to a track out of view and bring it into view first (`MixerPanel::reveal`):
- the Mixer View scrolls sideways;
- the track controls scroll the arrangement, keeping a tall row's top in view.
Move Focus to Mixer does the same. Checks: shell 346.

**#285 `window/view-toggles`, stacked on #284** (parity-7.2-shell rows 22 to 24, new). §42.1:
- **Full Screen** (F11; Ctrl+Cmd+F on the Mac).
- **Hide/Show Overview** (Ctrl/Cmd+Alt+O): the arrangement's top is now `topHeight()`, used by
  the mixer, the root and the playhead.
- **Hide/Show In/Out** (Ctrl/Cmd+Alt+I): the track controls narrow by the column and the
  arrangement widens.
- Both kept per window in `ui_view`.
- The View menu lists Full Screen, then §42.1's toggles in order.
- Seen in the app under Xvfb: F11 fills the screen; the toggles move and narrow; both are kept
  in the project, so a second run on the same file brought them back.
- **For win to measure:** where Live puts H and W when the Overview is hidden.

Checks: shell 352, window state 50, document 38; `test_all.sh` 8267 across 97 suites PASS;
zero warnings; the shadow sweep is clean.

**Caught before push:**
- `ResizableWindow::setFullScreen` on a window that is not on the desktop reads the monitor's
  area, which a peerless test has not, and crashed. The command now refuses off the screen and
  says why.
- The first app run left the In/Out column's space empty; Live narrows the panel instead, and so
  does ADI now.

**N20 (new): the Audio page's Channel Configuration is blocked.**
- `audio.channelConfig` is Device scope, which the store refuses to write, and its text has no
  format.
- The app opens the device with no inputs, and recording is not built.
- The Input and Output Config dialogs wait for a home for their values. Once they have one, they
  are a new window: pictures to the director first.

**Where to resume** (superseded by the entry above): `window/view-toggles` (#285) was the tip.

---

## 2026-10-28, cloud, last — the Mixer from the keyboard [#283](https://github.com/arieladi/Adi/pull/283) `window/mixer-keys`

**#283, stacked on #282** (parity-7.6-mixer row 16, new; parity-7.2-shell row 21).
- **Move Focus to Mixer, Alt/Option+Shift+M (§42.16):** the selected track's Track Activator. It
  uses the Mixer View when that is open, else the track controls, shown first if hidden.
- **Next and Previous Neighbor (§41.4.1.2):** in the Navigate menu, on Ctrl+Tab and
  Ctrl+Shift+Tab (Option on the Mac). They move to the same switch on the next or previous
  track: Activator, Solo, Arm, ADI-Q or ADI-L.
- A click on a mixer switch no longer takes the keyboard focus. So a focused switch is always
  the keyboard's, and the Live look rings it (`live.focusRing`).
- **Open:** volume and pan take no keyboard focus. Value keys wait for win's ruling on how a
  non-parameter edit coalesces (parity-7.6 row 3). A neighbour scrolled out of the track
  controls is not brought into view.

Checks: shell 345, document 38, window state 49; `test_all.sh` 8266 across 97 suites PASS; zero
warnings.

**Caught before push:**
- The shadow sweep found a lambda's `b` hiding the test's second root `b`.
- A segfault in the test itself: changing the selected track rebuilt the device strip under a
  later check's reference to `panels[0]->controls`. The mixer checks now leave the selection
  alone. **Lesson:** in `test_ui_shell.cpp`, the integrated root's selection is load-bearing for
  the device checks after it.

**Where to resume** (superseded by the entry above): `window/mixer-keys` (#283) was the tip.

---

## 2026-10-28, cloud, night, later — Live's arrangement keys [#282](https://github.com/arieladi/Adi/pull/282) `window/arrangement-keys`

**#282, stacked on #281** (parity-7.3-arrangement rows 24 and 25, new). Two rows of §42.16 that
need no op:
- **Move Insert Marker to Playhead Position, Ctrl/Cmd+Shift+Space.** The insert marker becomes a
  point at the playhead, with no clip selected, so Space plays from there.
- **Adjust Height of Selected Tracks, Alt/Option + and -.** As Alt+wheel: the selected track, or
  every track with selected content. A 16-unit step, provisional; win to measure Live's.

Checks: shell 340, document 38, window state 49; `test_all.sh` 8266 across 97 suites PASS;
zero warnings; the shadow sweep is clean.

**CI at 07:29Z on #274 to #282:** no failure, no cancellation. Every finished leg passed: all
the Linux legs, Windows (MSVC and its GUI shell), and the macOS GUI shell on #274 to #277. The
macOS core legs wait 30 to 50 minutes for runners; #281 and #282 had just started.

**The stack from this session**, each on the one before and on #273:

| PR | Branch | What |
|---|---|---|
| #274 | `window/menu-bar` | Live's menus in Live's order, only commands that exist; Settings in Options off the Mac and in the Mac's application menu; Delete Track in Edit |
| #275 | `window/navigate-menu` | Navigate's focus moves: Control Bar, Arrangement, Device View, Browser; arrows between devices and sidebar labels |
| #276 | `window/menu-keys` | Alt and a menu's letter opens it, off the Mac |
| #277 | `window/browser-search` | Ctrl/Cmd+F to the browser's search |
| #279 | `window/settings-apply` | a fix: Settings changes reach the open windows at once |
| #280 | `window/tab-focus` | Use Tab Key to Move Focus, off by default |
| #281 | `window/follow` | Live's Follow |
| #282 | `window/arrangement-keys` | insert marker to playhead; Alt +/- track heights |
| #283 | `window/mixer-keys` | Move Focus to Mixer; Next and Previous Neighbor (added after this entry) |

**Where to resume** (superseded by the entry above): `window/arrangement-keys` (#282) was the tip.

---

## 2026-10-28, cloud, night — Tab's switch [#280](https://github.com/arieladi/Adi/pull/280); Follow [#281](https://github.com/arieladi/Adi/pull/281)

**#280 `window/tab-focus`, stacked on #279** (parity-7.2-shell rows 13 and 21).
- Live 12's Use Tab Key to Move Focus (online §41.1.1) closes the Navigate menu, ticked from
  `lookfeel.tabMovesFocus` and writing it.
- Off, Live's default, the root takes plain Tab and Shift+Tab and moves nothing. Live uses the
  key for Session View, which is P2. On, JUCE's focus order moves on, as before.
- The Settings window's own Tab keys work either way.
- Wrap Tab Navigation has no setting yet (win's registry). Next and Previous Neighbor come with
  the Mixer's keyboard.

**#281 `window/follow`, stacked on #280** (parity-7.3-arrangement row 23, new). Live's Follow
(§6.2), which the control bar drew but answered "not built yet".
- The switch is lit when on. It is also a command in the Options menu, so the Mac has an
  Options menu now. It is kept per window in `ui_view`, off by default as the bar was drawn.
- While playing, the arrangement follows by `lookfeel.followMode`. By page, it jumps to put the
  playhead at the left once it leaves the lanes; by scroll, the playhead stays in the middle.
- **Pauses:** an edit through the window (arrangement, mixer, track commands, Undo and Redo), a
  sideways wheel scroll, an Overview drag, a click on the ruler.
- **Resumes:** playback starting or stopping, a click in the lanes or the scrub area.
- **Keys:** Option+Shift+F on the Mac. Off the Mac both Ctrl+Shift+F and Alt+Shift+F, since the
  manual gives Windows both.
- A device's edit does not pause it yet.
- **For win to measure:** Live's default for Follow in a new Set, which Windows key Live 12
  answers, and how Live shows Follow paused.

Checks: shell 325 (#280) and 337 (#281), window state 49 on #281, document 38; `test_all.sh`
8265 and then 8266 across 97 suites PASS; zero warnings; the shadow sweep is clean. Follow's
checks run with a real engine playing, with real clicks through the fake peer on the ruler and
the scrub area.

**CI** at 07:14Z: no failure on #274 to #280; macOS legs queued.

**N19 (new):** the transport's metronome and loop. `transport.setMetronome` and
`transport.setLoop` are in the catalogue but apply nothing, and the snapshot carries neither. So
the Metronome and Loop switches, the O key, Ctrl/Cmd+L and the loop brace wait for the engine.

**Where to resume** (superseded by the entry above): `window/follow` (#281) was the tip.

---

## 2026-10-28, cloud, evening — Ctrl/Cmd+F [#277](https://github.com/arieladi/Adi/pull/277); Settings that apply at once [#279](https://github.com/arieladi/Adi/pull/279)

**#277 `window/browser-search`, stacked on #276** (parity-7.2-shell row 20, new). Online §41.4.1.6
and §42.22: Ctrl/Cmd+F brings the focus to the browser's search from anywhere.
- It shows a hidden browser, then focuses the field with its words selected to type over.
- The field's hint now names the platform's key (Ctrl off the Mac).
- Partial: Live also selects the sidebar's All label, which ADI's Live 11 sidebar has not. The
  results wait for N15.

**#279 `window/settings-apply`, stacked on #277: a fix to #267.** The Settings window set and
wrote each change at once, but `AppCommands` read its five settings only at launch. So the Info
View, tooltips and their delay, Zoom on Selection and the shortcut preset did nothing until a
relaunch.
- `SettingsPanel::onChange` now runs after each value set and after Restore Defaults. The
  application answers it with `AppCommands::reload`.
- Every native window keeps its tooltips to the commands at each frame (`followTooltips`).
- Parity-settings row 6 now says so.
- #278 is not this lane's; the number was taken between the two.

Checks: shell 317 (#277) and 322 (#279), document 38, window state 48; `test_all.sh` 8265 across
97 suites PASS on each; zero warnings; the shadow sweep is clean.

**CI:** #274 to #277 had no failure at 07:01Z; macOS legs were still queued behind macOS runners.

**Findings in the settings registry (win's), for the record. No ruling is asked; the window
builds on what is there.**
- `lookfeel.tabMovesFocus` (default off) is Live 12's "Use Tab Key to Move Focus", an entry of
  Live's Navigate menu (§41.1.1). Today the main window moves the focus on Tab whatever it says,
  because JUCE moves it on any unused Tab. That is the next item.
- `lookfeel.focusOutline` (default on) says "Outline the view that has keyboard focus, with the
  view indicator of ADR-0112". ADR-0112 does not describe that indicator. The rings of #275 and
  of the Settings window are per control and do not read it.
- `lookfeel.followMode` (page or scroll) is Live 12's Follow Behavior. Whether Follow is on is
  not a setting.
- **A conflict in the manual itself:** Follow's key on Windows is Alt+Shift+F in §42.9 but
  Ctrl+Shift+F in §42.11, §42.12 and §42.16. Option+Shift+F on the Mac in all four.

**Where to resume** (superseded by the entry above): `window/settings-apply` (#279) was the tip.

---

## 2026-10-28, cloud, later still — Alt and a menu's letter [#276](https://github.com/arieladi/Adi/pull/276) `window/menu-keys`

**#276, stacked on #275** (parity-7.2-application row 13, new). Online §42.28: on Windows, Alt and
a menu's first letter opens that menu.
- Off the Mac, the window's key listener asks `menuForKey` which menu the key names, and opens
  it in the menu bar. JUCE's menus already take the arrows (through a menu and on to its
  neighbours) and Enter.
- Only Alt alone counts. Navigate's Alt+digit keys are untouched.
- On the Mac, Live's equivalent is Cmd+? menu search. macOS puts it in an application's Help
  menu, which ADI does not have yet, so row 13 is open there.

**Seen in the app itself**, the first time this lane has driven the app on Linux. ADI ran under
`Xvfb` with keys sent by `xdotool` and captures by ImageMagick's `import`, with `HOME` in the
scratchpad and the project from `adi_ui_document_tests --fixture`:
- Alt+E opens Edit (Undo, Redo, Delete Track, Rename); then Right opens Create.
- Alt+N shows Navigate's four entries with their keys.
- Alt+0 rings Link; Alt+5 then Down twice rings the sidebar and selects Sounds.

This is a cheap way to check, without the Mac, what a peerless test cannot (popups, real
focus). The captures stay in the session's scratchpad; nothing is committed.

Checks: shell 316, document 38, window state 48; `test_all.sh` 8265 across 97 suites PASS; zero
warnings; the shadow sweep is clean.

**CI:** on #274 every Linux leg and all three GUI-shell legs passed, the Mac's included, so the
Mac-only menu expectations hold. Its MSVC and macOS core legs were still running, and #275's
had just started.

**Where to resume** (superseded by the entries above): `window/menu-keys` (#276) was the tip.

---

## 2026-10-28, cloud, later — Live's Navigate menu [#275](https://github.com/arieladi/Adi/pull/275) `window/navigate-menu`

**#275, stacked on #274** (parity-7.2-shell rows 18 and 19, parity-7.4-devices row 15). Live 12's
Navigate menu (online §41.4.1, §42.2), with the four views ADI has. Option replaces Alt on the
Mac.
- **Control Bar (Alt+0).** The focus returns to the control that last had it, Link until one
  has (§41.4.1.1).
- **Arrangement View (Alt+2).** The canvas, whose arrows already move.
- **Device View (Alt+4).** The first device. The left and right arrows then move between
  devices (§41.4.1.5).
- **Browser (Alt+5).** The browser now takes the keyboard. Up and down move between the
  sidebar's labels, as a click selects (§41.4.1.6).
- A hidden Device View or browser is shown first.
- Where the keyboard put the focus, a control bar item or the browser's sidebar is ringed in
  the accent, as the Settings window's controls; a click leaves none.
- Session View, Clip View, the Groove Pool and the Learn View come with those views. The Mixer
  (Alt+Shift+M) comes when its controls take the keyboard.
- **For win to measure:** does Live show a hidden view on Alt+4 or Alt+5, and what colour is
  its focus ring?

Checks: shell 314, document 38, window state 48; `test_all.sh` 8265 across 97 suites PASS;
zero warnings; the shadow sweep is clean (it caught a shadowed `alt` in the new test).

**Found on the way:** JUCE calls `focusOfChildComponentChanged` only when the focus enters or
leaves a parent, not when it moves between the parent's children. So the control bar learns
which control had the focus from each item, not from its own callback.

**Where to resume** (superseded by the entry above): `window/navigate-menu` (#275) was the tip.

---

## 2026-10-28, cloud — Live's menu bar [#274](https://github.com/arieladi/Adi/pull/274) `window/menu-bar`

**CI on #265 to #273, read first.** Nothing is red. Every Linux, Windows, s390x, i386, fuzz,
validator and GUI-shell leg passed on all nine. At 06:20Z only macOS legs were still queued or
running, waiting on macOS runners. No fix was needed, so nothing was pushed to the stack.

**#274, stacked on #273** (parity-7.2-application row 11). Live 12's menus in Live's order, each
listing only the commands ADI has. They are one table in `ui_shell.cpp` (`menuBar`).
- **Settings moves to Options off the Mac.** The online manual §2.3 says "Settings can be found
  in the Options menu on Windows and in the Live menu on macOS". ADI had it in the File menu.
- **Settings on the Mac.** The Mac listed Settings in no menu; only Cmd+, reached it. It is now
  the application menu's (`applicationMenu`, through `setMacMainMenu`).
- **The Edit menu gains Delete Track before Rename** (§18.2: "Tracks are deleted using the Edit
  menu's Delete command"). It has no Delete key: Live's Delete acts on whatever is selected,
  clips included.
- **Menus with nothing to list are left out:** Options on the Mac; Navigate until its focus moves
  exist (§41.4.1, Alt+0 to Alt+8); Help until there is a Learn View (§2.2.1).

Checks: shell 303 (296 on #273 here), document 38, window state 48; `test_all.sh` 8265 across 97
suites PASS; zero warnings; the clang `-Wshadow-all` sweep is clean. The Mac-only lines compile
only in CI's macOS legs.

**How the cloud builds.**
- Ubuntu 24.04, GCC 13.3, 4 cores, Ninja, with the apt packages `ui.yml` installs.
- `fetch_external.sh --with-juce`, then one tree, `adi_daw/build-juce`, configured
  `-DCMAKE_BUILD_TYPE=Debug -DADI_WITH_JUCE=ON -DADI_WERROR=ON`.
- The UI tests run with `DISPLAY` unset, as `ui.yml` does. JUCE asserts in
  `juce_Messaging_linux.cpp:87` about 2000 times there, a full message queue in a peerless run;
  the checks pass.
- The tip's numbers match the Mac's: 8265 checks across 97 suites.

**Found, for the engine lane (not the window's to fix).** With `-DADI_WERROR=ON` and GCC 13,
four targets fail inside JUCE's own sources: `adi_audio_probe`, `adi_vst3_probe`, `adi_play` and
`adi_play_render_tests`.
- They compile JUCE's module sources with the project's warning flags, and GCC's `-Wconversion`
  fires in JUCE's headers (`juce_UMPUtils.h:78` and others).
- No CI leg builds them that way: the JUCE legs run on macOS and Windows without `-Werror`.
- The UI targets are unaffected. `test_all.sh` counts the same 97 suites: it globs the build
  directory, and the four would land in their own `*_artefacts` folders.

**The Live 12 manual here.** Ableton's online edition (2026-10-01). Its keyboard shortcuts are
chapter 42, the PDF's 41; rows written here cite "online §". The text is read locally with curl,
never committed.

**Where to resume** (superseded by the entry above): `window/menu-bar` (#274) was the tip.
- Next: Navigate's focus moves for the views ADI has, Live's Alt/Option+0 (Control Bar), +2
  (Arrangement), +4 (Device View), +5 (Browser) and Alt/Option+Shift+M (Mixer). Then Live's
  Alt+letter that opens a menu on Windows (§42.28).
- Phase C's window parts still wait for N17.

---

## 2026-10-28, late — the lane moves to a cloud session

The director is moving the window lane off the Mac to a Claude Code cloud session (environment
"Adi", this repository). Everything is pushed: the stack #246 to #273 and this log. Nothing was left
uncommitted in `~/adi-window` or `~/adi-window-log`.

**What the cloud does not have, and what follows from it:**
- **Live 11 on the director's Mac.** No new captures of Live, so a new window's pictures beside
  Live wait for the Mac. Pictures of ADI alone render headless through `ADI_UI_PARITY`.
- **The Live 12 manual's PDF.** It is in the director's main checkout, not in the repository, and
  the repository is public, so it stays out. Use Ableton's online manual
  (ableton.com/en/live-manual/12/); the checklists cite the PDF's page numbers.
- **The native macOS app.** `--smoke-settings` and the Mac's own menu run on the Mac. Everything
  else builds and tests on Linux as `ui.yml`'s Linux leg does, and CI runs all three platforms.

**Where to resume:** unchanged from the floating-zoom entry. First read CI on #265 to #273, where
the fix for the Linux and Windows failures was pushed. Then the menu bar's other menus (7.2 row 11).

---

## 2026-10-28, night — the stack's CI on Linux and Windows (#265 to #273)

CI showed what the Mac cannot:
- **Windows, #265 and #267 to #272.** MSVC's C4996 refused `std::getenv` in the parity render's
  hook, with `/WX`. It now reads `ADI_UI_PARITY` through `juce::SystemStats`.
- **Linux, #265 only.** The browser's last sidebar row ("Add Folder") was drawn under the AI tab,
  outside the text masks, so the fonts differed by 32 to 217 pixels in seven goldens. #266 already
  clips the rows to the sidebar; #265 now does too, and its goldens are re-recorded.

**How it went up the stack.** One fix commit on `window/live-look`. Each branch above got the same
`getenv` replacement, then a merge of the branch below with `-s ours`: each already carries the
clip and its own goldens, so its tree is kept. Nothing was force-pushed. Each branch's change
against what was pushed is only the `getenv` lines; each passes its shell checks (#266 265, #267
277, #268 281, #269 284, #270 285, #271 291, #272 291, #273 295). The tip passes `test_all.sh` and
the shadow sweep. #246 to #264 were green already.

**The lesson:** the Mac's Clang does not warn on `getenv`. In window tests, read the environment
through `juce::SystemStats`.

---

## 2026-10-28, night — a floating window's own zoom [#273](https://github.com/arieladi/Adi/pull/273) `window/floating-zoom`

**#273, stacked on #272** (ADR-0129 d3, parity-7.5 row 10). Ctrl/Cmd + and - pressed in the
Channel EQ window zoomed the main window, because a floating window passed every key to the main
one.
- Now an ADI view's window zooms itself, on the main window's steps (50 % to 200 %). Every other
  key there is still the main window's.
- The view lays out in logical units under a scale, as `ZoomHost` holds the main root. The steps
  are one function, `nextZoom`, which both use.
- The zoom is held for the session, as the main window's is (N4).

Checks: 295 shell checks; `test_all.sh` 8265 across 97 suites PASS; the shadow sweep is clean (it
caught a `window` shadow in the new test before push).

**Where to resume:** `~/adi-window` on `window/floating-zoom`, the tip of the stack. Item 3's phase
C parts still wait for N17. The next piece the window can finish alone is the menu bar's other
menus (7.2 row 11), as far as there are commands to list. Also: the native Settings smoke in
`ui.yml` (win's file) once win agrees.

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

Resumed the same evening with a floating window's zoom (#273, above).

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
