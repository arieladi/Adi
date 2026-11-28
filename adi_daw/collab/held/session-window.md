# Session View, the window side (held)

**This chain lands only when the director says so.** Chain: `held/session-window-chain`. Log: this file, on
`held/session-window-log`. No pull request. Asked for by the director by name ("the Ableton Live view").

Mission (round 1, 28 November): the Session View's window side as far as it goes before its picture is agreed
(ADR-0243): (1) the view's model, free of drawing; (2) Live's commands and keys through the session seat's ops;
(3) headless tests for both; (4) the list of what the picture must settle.

## How to continue (for the next session, with no memory of this one)

- BASE is `origin/land/basics-1128c` until it is an ancestor of `origin/main`, then `origin/main`. The chain was
  made from BASE with `origin/seats/session-chain` and `origin/wincheck/seats/session-chain` merged in (one
  conflict, `src/adi/store.cpp` at the end of `Store::open`: both sides kept, the project UUID write first, then
  `lastError_ = essentialExtensionProblem()`). Merge BASE and both session branches again before each push.
- The work is three files and one suite, all new, no shared file touched but `CMakeLists.txt` (five lines after
  the session seat's suites):
  - `src/adi/ui/session_view_model.hpp/.cpp` — `adi::ui::session_view::Model`.
  - `src/adi/ui/session_view_commands.hpp/.cpp` — keys, commands, `Outcome`.
  - `tests/test_session_view.cpp` — `adi_session_view_tests` (core leg, no JUCE).
- Build on the build PC slot 18 with `/d/adibuild-tools/slot.sh` (see the mission prompt). Quick turn:
  `run 18 origin/held/session-window-chain core 'adi_session_view_tests'`.
- Nothing is wired into `src/juce/ui_shell.cpp` yet, on purpose: there is no view to switch to until the
  picture is agreed. The wiring is listed under "Next".

## Steps

| Step | Head | What changed | How it was proved | State |
|---|---|---|---|---|
| merge | `5151798de` | BASE `origin/land/basics-1128c` + `origin/seats/session-chain` + `origin/wincheck/seats/session-chain`. | the full check below | pushed |
| 1-3 | `ec19b7e56`, `bbb025515` | The model, the commands and keys, the suite (`bbb025515`: two portability spellings in the suite). | Build PC slot 18, MSVC Debug, warnings as errors, at `bbb025515` ("all"): core built, 312 of 314 suites passed, `adi_session_view_tests` among them; ui built, the four window suites passed; five validators passed. The two failures, `adi_agent_log_tests` (three child processes appending) and `adi_link_playback_tests` (4 of 139), touch nothing of this chain and both passed when rerun alone on the same head: load on the shared PC. Linux and macOS not yet run (CI, later). | pushed |

## What was built

**The model** (`Model`). `rebuild(ModelInput)` takes the project on the message thread after a load and after
every commit, undo and redo; `modelInput(rows::Model, LaunchData)` makes the input from the same read the
engine publishes from (tracks in the Arrangement's order, a group before what it holds, the returns after,
Main left out as the scene column). Every frame `applyStatus(LaunchTrackStatus rows, LaunchStatusHeader,
pendingScene)` takes the launcher's completed frame (`Session::readLaunchStatus`) and `applyRecording(state,
slots)` the slot recorder's; both write only into storage sized by `rebuild` (double-buffered, compared before
publishing) and allocate nothing — the suite counts every `operator new` over 200 frames: zero.

- A slot: its clip, name, colour, Deactivate, Follow Actions, launch mode, Stop button; its state, most urgent
  first: Recording, QueuedRecord, QueuedStart, QueuedStop, Playing, Stopped (a clip at rest), Empty.
- A group slot stands for the clips of the tracks it holds at any depth (manual 7.1): shaded in the left-most
  held clip's colour, a Stop button where none holds a clip, playing or queued as any held slot is.
- A scene: number by place, name, colour, tempo and signature each with its enable, Follow Actions, how many
  clip tracks hold a clip in it and how many play it, and whether its launch is queued.
- A column's Track Status (7.3): Arrangement, Session or Silence; the clip and scene playing; a loop's count
  and length; a one-shot's samples left; its Clip Stop button waiting; Back to Arrangement waiting.
- The header: Back to Arrangement lit, transport rolling, the status incarnation.
- The selection, by identity, so a rebuild keeps it: a slot rectangle from anchor to focus, a run of scenes or
  a run of track titles. Selection is session state (ADR-0021), never an op.

**The commands** (`run(Command, Model, Context)` and the mouse paths `launchSlot`, `releaseSlot`,
`launchScene`, `stopColumn`, `renameTo`, `nextRenameTarget`). An `Outcome` is data: `edits` for
`OpSubmitter::submit` as one batch (one Undo step), `launches` for `engine::dispatchLaunchOperations` as one
gesture (outside Undo, as Live's launches are), `capture` for the engine's
`prepareCaptureAndInsertScene`/`commitCaptureAndInsertScene`, and `view`, `selection`, `rename`, `recordInto`
for the window. Keys (`commandForKey`, `commandForKeyRelease`), from manual 41.1 and 41.15:

| Key | Command | Through |
|---|---|---|
| Tab (Use Tab to Move Focus off), F3 | Toggle Arrangement/Session | window state |
| Enter / Enter let go | Launch selected clip, slot or scene / release edge | `session.launchClip`, `.stopTrack`, `.launchScene`; release `session.launchClip` with `release` |
| Ctrl+Enter | Stop Clips in Track with Slot Selection | `session.stopTrack` per track |
| (buttons) | Stop All Clips, Back to Arrangement, Cancel Scene Launch | `session.stopAll`, `.backToArrangement`, `.cancelSceneLaunch` |
| arrows, Shift+arrows | select / extend, Right from the last track to the scene, Left back | selection |
| Page Up/Down, Home, End, Esc, Ctrl+A | eight scenes, first/last track, End from the last track to the scene, the track's title, all | selection |
| Ctrl+I | Insert Scene below the selection, selected | `scene.create` (+ `scene.reorder` unless at the end), one Undo step |
| Ctrl+Shift+I | Capture and Insert Scene below the selection | the engine's capture, one Undo step |
| Ctrl+E | Add/Remove Stop Button | `session.setStopButton` per slot, one Undo step |
| Ctrl+R, then typing; Tab/Shift+Tab | Rename clip, scene or track; next/previous scene or track | `clip.setName`, `scene.rename`, `track.rename`, one Undo step each |
| Delete, Backspace | Delete clips (a group slot: every clip it refers to) or scenes | `session.clearSlot` / `scene.delete`, one Undo step |
| 0 | Deactivate/Activate selected clips | `clip.setMute`, one Undo step |
| Ctrl+Up/Down | Move scenes without collapsing | `scene.reorder`, one Undo step |

An empty slot on an armed track asks for Session Record into it (`recordInto`); the window's recorder path
(`SlotInputRecorder`, `prepareSlotCommit`) is the session seat's.

**The suite** `adi_session_view_tests`: the layout and every slot, scene and track state change; the frame's
zero allocations; the selection; every key of the table; every command's outcome, each launch payload checked
against the launch operations' own descriptors and each edit against the op registry; and on a real project
(`Store::create` with the staged 1.26 Session tables) Insert Scene, Delete clips, Add/Remove Stop Button,
Deactivate, moving scenes, Delete Scene and the three renames, each committed as one batch where ONE Undo puts
the scenes, slots, launch settings, clips and tracks back exactly.

## Decisions taken (and why)

- **Files of its own, under `src/adi/ui/`**, beside the other drawing-free models (`stack_model`,
  `locator_edits`): nothing in `ui_shell.cpp`, `ui_arrangement.cpp` or the window suites, so it cannot collide
  with the basics chains, and nothing draws.
- **The model reads the engine's publications, never the store, per frame** (TM-SV-05: "reading the per-slot
  taps and run edges, never the store"). The project side is read once per commit, as the window's
  `ProjectView` refresh already is (ADR-0201).
- **Launches are not undoable; every edit command is one batch.** Live keeps launching, stopping and Back to
  Arrangement out of its Undo history; its edits are one step each.
- **Insert Scene allocates the next scene id and ordinal itself** (one past the largest), as `addLocator` does
  for markers; `scene.create` takes a literal id and `scene.reorder` needs every scene.
- **Enter launches the focused slot only** when several are selected (named constant
  `enterLaunchesFocusOnly`; reading owed below).
- **F3 toggles the view beside Tab** because TM-SV-05 names "(F3, Tab)" and the shell holds F3 free for it
  (`ui_shell.cpp`, next to F4 for the Mix View). Named constant `f3TogglesSessionView`.

## Engine gaps (for the session seat; not mine to change)

- **The status frame has no queued scene.** `LaunchTrackStatus.queued.scene` names the slot's scene for a clip
  launch and for a scene launch alike, so the window cannot tell which scene button should blink. The model takes
  it as an argument (`pendingScene`). Wanted: the launcher's pending user or follow scene in
  `LaunchStatusHeader` (one `std::int64_t`, zero for none).
- **Capture and Insert Scene's owner entry needs a `SceneCaptureSnapshot`** (status rows, the publication
  sequence and head). The window will need a helper on `Session` that fills it from its own last published
  generation; today only the tests build it.

## Readings owed (Live 12.4)

- Whether Live refuses to delete the last remaining scene (`keepOneScene`, ADI keeps one).
- Whether a group slot's launch stops held tracks that have no clip in that scene
  (`groupLaunchStopsEmptyHeldTracks`, ADI stops them where the empty slot keeps its Stop).
- Enter with several slots selected (`enterLaunchesFocusOnly`).
- Whether a scene's launch button shows "playing" (all, any, or the last launched scene) and in which colour;
  the model offers `SceneLive.playing` against `SceneFacts.occupied` and `triggered`.
- Enter's release edge for a Gate or Repeat clip launched from the computer keyboard.
- What Right from a scene reaches (Live: the Scene Tempo and Signature fields, 7.2.1) once they are shown.

## Reserve

Nothing: no op name, schema minor, ADR number or register id was needed. The chain uses only the session
seat's ops (`scene.*`, `session.*`, the six launch ops) and existing ones (`clip.setName`, `clip.setMute`,
`track.rename`).

## For the director

A. **F3 beside Tab for the Session View.** Live switches with Tab only; the plan row names F3 too, mirroring F4
   for the Mix View. Recommendation: keep F3 as ADI's addition beside Tab. Reply "A yes" or "A Tab only".

## What the picture must settle (item 4: the sheet's list)

Every measure and behaviour of Live's Session View the sheet has to show before anything is drawn. Live 12
manual chapters 7, 16 and 19; the numbers marked "measure" are to be read from Live 12.4 at 100 % zoom.

**Layout**
1. Where the Session View sits: the whole main area in place of the Arrangement (Tab), with the Browser on the
   left, the Detail View (Clip/Device) below, the Mixer section below the grid; the second-window option
   (Ctrl+Shift+W) shows it beside the Arrangement.
2. Column width (measure): default track width, the narrowest (launch buttons and essential controls only,
   7.2), the widest; the drag on the title bar's edge; Alt resizes all.
3. Slot height (measure) and whether it follows the Arrangement's zoom (it does not in Live; fixed with an
   option for size in the View menu).
4. The track title bar: height, name, colour, the fold of a group, its context menu.
5. The Main track column at the right: scene launch buttons, scene names, the scene numbers column, and the
   hidden Scene Tempo and Scene Time Signature fields revealed by dragging its left edge (7.2.1).
6. The Track Status row below the grid and the Clip Stop buttons in it; the Main track's Stop All Clips button.
7. Return tracks in the grid: columns with no slots, before Main.
8. How many scenes show; vertical scrolling; the empty space below the last scene.

**A slot** (what each state shows)
9. A clip at rest: its colour fill, its name, the triangular launch button at the left edge (7.1).
10. Playing: the launch button green; whether and how the slot shows the clip's progress (measure).
11. Queued to start: the launch button blinking, its blink rate (measure) and colour.
12. Queued to stop: the playing clip's button and the track's Clip Stop button blinking.
13. Recording and queued to record: the red record button in empty slots of an armed track, blinking while
    waiting, the slot red while it records (19).
14. An empty slot: the square Clip Stop button; without one (7.4.2): nothing; on an armed track: a record
    button.
15. A deactivated clip (0): grey.
16. Follow Actions on: the striped launch button (16); a launch mode or Legato mark, if any.
17. A group slot: the shading in the left-most clip's colour, its launch button, a Stop button where empty.
18. Selected slot, slot range, multi-selection: the highlight colour and frame; the focus outline.
19. Drag and drop: the insertion mark when moving clips and the rubber band from an empty slot.

**A scene**
20. The scene launch button: plain, coloured when the scene changes tempo or signature (7.2.1), striped with
    Follow Actions, blinking when queued, green when playing (reading owed).
21. The scene's name field, its colour, its number; the tempo and signature fields when shown (their format:
    "128.00 BPM", "7/8"?), and their disabled look.
22. The selected scene: the highlight that runs across the row; Select Next Scene on Launch's next selection.

**Track Status (7.3)**
23. A looping Session clip: the pie with the loop count at the left and the loop length in beats at the right.
24. A one-shot: the progress bar with minutes:seconds left.
25. Monitoring: microphone (audio) or keyboard (MIDI) icon.
26. Playing the Arrangement: the miniature of the Arrangement clips; a group: a pie without numbers.
27. Silence after a Stop: what the field shows.

**Controls around it**
28. The Back to Arrangement button: in the Session View (above the Main column) and in the Arrangement View,
    lit while what plays differs from the Arrangement (7.5).
29. The control bar's Session Record button, Launch Quantization chooser, and Follow Actions switch (now
    placeholders, `ui_controlbar.cpp`).
30. The crossfader and the A/B assign switches in the mixer section (MX-15), when it comes.
31. The Scene View (7.2.2) in the Detail View: tempo and signature sliders, the scene's Follow Action controls,
    its title bar for one or several scenes.
32. The Clip View's launch panel (mode, quantization, Legato, velocity, Follow Actions) for a Session clip.

**Colours** (each to be read from Live's theme and set beside ADI's palette): playing green, queued blink,
record red, selection, the empty slot's grid colour, the Stop button grey, the group shading's tint, the
deactivated grey.

## Next

1. Merge BASE and both session branches again; rerun "all" on slot 18. (Round 1 ended with the chain at
   `bbb025515`, checked as above.)
2. Wire the shell once the picture is agreed: a `SessionViewPanel` that owns a `Model`, rebuilds on the
   `ProjectView` generation and polls `Session::readLaunchStatus` on its timer; the keys through
   `commandForKey`; `Outcome.edits` through the window's `OpSubmitter`, `launches` through
   `dispatchLaunchOperations`, `capture` through the engine's capture.
3. Commands left for a later round: Insert MIDI Clip (Ctrl+Shift+M: `clip.create` then
   `session.setSlotClip`), Toggle Follow Actions (Shift+Enter: `session.setFollowAction`), Create Follow Action
   Chain (Ctrl+Shift+Enter), Duplicate (Ctrl+D), copy/paste and drag of clips and scenes, Ctrl+click picks,
   track moves (Ctrl+Left/Right, the track commands'), Edit Info Text, colour.
