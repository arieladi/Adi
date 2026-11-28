# Export basics report

**Chain** `basics/export-chain` (from origin/main, 28 November). One commit per item, SC id in the
message. No pull request: win merges the chain into a landing stack.

## Round 1 of the export stream (28 November)

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-479 | `basics/export-chain` `29f699ea3` | An export renders one copy of the song taken when Export is pressed (`render::PinnedProject`, a hidden `.adi-export-*.adi` beside the project, removed when the export ends), so an edit made while it runs reaches the song and never the files. A plug-in that plays but cannot be loaded a second time stops the export and is named with its reason (`Job::playingDevices`); out of memory while loading the second set names the device. New suite `adi_export_while_playing_tests`. Engine (`queue.*`, `session.cpp` resolveOne), window-fn (`ui_main.cpp` startRender). | Slot check (round 2) at `29f699ea`, core, MSVC Debug /WX: build OK, `adi_export_while_playing`, `adi_render_queue`, `adi_session*` 3/3 pass. | PR requested (with the round 2 rows) |

## Round 2 of the export stream (28 November, evening)

The round 1 session stopped before its slot check; round 2 ran it (above), then closed points 4 and 5
and the leak round 1 named.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-479 (stale copy) | `basics/export-chain` `123f6d22c` | The export's copy of the song is held under a shared project-file lease while it lives. Each capture first removes, with its journal, every `.adi-export-*.adi` in the project's folder that nobody holds (an exclusive lease succeeds): a copy left when the app was killed or the power went. A running export's copy is never removed by another's start; other files are left alone. Engine (`render/queue.*`). | `staleCopies` fails without it (the stale copy stays). Slot check at `5820b534`, core, filter `adi_export_while_playing\|adi_render_queue\|adi_freeze_capture\|adi_session`: `staleCopies` passes; see the next row for the one failure of that run. | PR requested |
| SC-479 (point 5, a third engine) | `basics/export-chain` `5820b5341`, `e7623bee3` | Test only. `freezeDuringExport`: a Freeze capture on its own worker and connection while an export renders its pinned copy; the file and the frozen audio are each identical to the ones made alone, each with its own plug-in loaded and freed. | First run failed on the fixture (its media row had a placeholder hash, which Freeze verifies and the export does not); `e7623bee3` gives it the real hash. Slot check at `e7623bee` (filter `adi_export_while_playing`): waiting for a build place when this line was written. | in progress |

### What round 2 decided, and why

- **Point 4, what only real time can render.** Nothing in ADI today is real-time-only in Live's sense.
  Live renders in real time only for External Instrument and External Audio Effect (manual 5.1.3.5,
  `collab/window/parity-export.md` rows 32 to 35), and ADI has neither device yet. A MIDI track sent to a
  hardware port exports without that sound in Live as in ADI, and ADI's export sends no MIDI to the
  hardware (already proved: `adi_midi_clips_tests`, "export callbacks emit no hardware MIDI"). An offline
  export takes no live input in either. So no test fails today, and there is nothing to refuse the Ableton
  way. Naming those tracks in the done message is an addition: it stays choice B below. When External
  Instrument or External Audio Effect is built, real-time rendering (rows 32 to 35) comes with it.
- **Point 5, a second export.** Read in `ui_main.cpp`: `openExport` brings the page forward while a render
  runs, and `chooseExportFile` returns at once while one runs (`render_.joinable()`); the page and the Status
  Bar show the running one. No other path starts an app export (`render::Queue` is used by `startRender`
  only; the control queue `control/render_jobs.cpp` is not wired to the app). Not a headless test: the
  guard lives in the app class, which has no seam, and adding one for a guard that already holds is not a
  fix. Left as read.
- **Point 5, Freeze or Bounce during an export.** Round 1 wrote "Freeze/Bounce waits for a running export".
  Round 2 did not make it wait: that would be a refusal a user sees, and Live has nothing to copy (its export
  is modal). Instead it proved the third engine harms neither (`freezeDuringExport`). Memory is then held a
  third time while both run; that is choice A's question again.

### What a stranger needs next

- If the `e7623bee` check above passed, SC-479 is done on the chain except choices A and B. Run one full
  check (mode all) on the chain head before handing it to win, if this round did not.
- The pinned copy costs one read of the project file on the message thread at the press, and its size on
  disk while the export runs. In a synced folder (OneDrive, Dropbox: `Store::synchronizedLocation()`)
  the sync client may upload the copy while the export runs. Not fixed: rendering from a
  copy elsewhere breaks the media paths, which resolve beside the project. A fix would render that
  case from a snapshot transaction instead, but in a synced folder the journal is not WAL, and a long
  read would block the owner's edits. Noted, not a row.

### SC-479, what each point of the row now has

1. **An edit made during the export does not reach the file.** Finding confirmed: each job of a batch
   (All Individual Tracks, Selected Tracks Only) read the project when its own render started, so an edit
   made during the first file reached the later ones; an edit between the press and the worker's start
   reached the first. The control check in `editDuringExport` keeps that behaviour of an unpinned queue
   visible. Fixed by the pinned copy; the test commits a mute before the run and another during the first
   file, and both files keep both tracks, with the same journal head.
2. **Playback does not crackle while a heavy song exports.** Proved headless only as far as it can be:
   `twoEngines` plays a live Session block by block on the message thread while the export runs on a
   worker through the host mailbox, as the app does. The playing engine's output is bit-identical with and
   without the export, the file is bit-identical to one exported alone, and each engine's plug-ins process
   only on their own thread. CPU contention on a real driver is not measured here: that needs the real
   window (the walk), and is left for it.
3. **A plug-in loaded a second time.** Finding confirmed: a VST3/CLAP that refuses a second instance
   became a bypassed placeholder and the export completed without it, naming nothing. Now refused with its
   name and reason. Memory: `secondSetMemory` measures that the export holds every device state a second
   time while it runs (2 x 4 MB states: 8 MB playing, 16 MB during) and frees it at its end.
4. **What can only be rendered in real time** (hardware MIDI outputs, an audio track monitoring an
   external input): not started. Read in the code: both export silent today (`MidiHardwareNode` returns
   early when exporting; input monitors are not prepared offline). See "For the director", choice B.
5. **A second export asked while one runs**: read in the code, the page disables Export and
   `chooseExportFile` returns while a render runs; not yet a test. Freeze/Bounce is not excluded while an
   export runs (a third engine): next round.

## Not now

- SC-394 (A, To run): the Loop-off fallback needs one look at Live 12; an earlier session keeps its branch
  `basics/sc-394-export-range` local, never pushed.
- SC-397 (B, Late): stems through Main and Returns wait for the engine's ViaMain route (EX-02).
- SC-398 (B, Late): FLAC and MP3 wait for the encoder pins; the director granted both on 28 November
  (ADR-0266 d1). AIFF is on main.
- SC-471 (C, To draw): the Export page's look is settled from a picture first (ADR-0243).
- SC-469, SC-470: worked by the director's-test session (`basics/test-chain-2` at `1ec5604c0`, in
  `land/basics-1128d`).
- SC-401, SC-402: on main already (the earlier session's `basics/sc-401-export-messages` and
  `basics/sc-402-export-sample-rates` are ancestors of origin/main); only the State cell is unmarked.
- SC-395, SC-396, SC-399, SC-400: on main (#822).

## For the director

- **A. When the second set of plug-ins does not fit (SC-479, your question of 28 November).** Today, after
  this round, ADI stops the export and names the plug-in that could not be loaded a second time, or that
  ran out of memory. Your remedy is to export instead with playback stopped, on the playing engine's own
  plug-ins, as Live does. I recommend it as the fallback, offered in the same message ("Stop playback and
  export"), not done silently: it needs the playing engine to render offline and then resume, which is
  engine work with its own row. Reply: "A yes" to add the row, "A no" to keep the refusal.
- **B. Hardware MIDI outputs and tracks monitoring an external input (SC-479 point 4).** Live renders in
  real time only when the song holds an External Instrument or External Audio Effect; a plain MIDI track
  sent to a hardware port exports without that sound. ADI exports offline only. I recommend: export goes
  on and the done message names each track whose sound is not in the file ("Bass Synth sends to a MIDI
  port; its sound is not in the file"), rather than refusing, because refusing does what Live does not.
  Reply: "B warn" or "B refuse".
- **C. The libFLAC and LAME grants (SC-398):** answered 28 November (ADR-0266 d1); the row still says Late.

## Earlier session (all on main)

The first export session worked SC-394 to SC-402 as one branch per item. Everything but SC-394 is now on
main. Its table, for the record:

| SC | Branch and head | What changed | State |
|---|---|---|---|
| SC-394 | `basics/sc-394-export-range` `0670165d` (local) | The Loop-off fallback as a named choice. | blocked on the Live 12 R36 observation |
| SC-395 | `basics/sc-395-missing-assets` `d348169c` | Missing clips and plug-ins export as playback plays them. | on main |
| SC-396 | `basics/sc-396-export-settings` `d315609f` | The page opens from Settings and remembers the session's choices. | on main |
| SC-397 | `basics/sc-397-stem-export` `5a0495a5` | All or selected tracks as post-fader files; Returns and Main wait for ViaMain. | on main (rest Late) |
| SC-398 | `basics/sc-398-export-formats` `40322435` | AIFF and AIFF-C; FLAC and MP3 wait for the encoders. | on main (rest Late) |
| SC-399 | `basics/sc-399-export-guard` `51932405` | New, Open and Quit ask before stopping a render. | on main |
| SC-400 | `basics/sc-400-export-progress` `ad53f1dc` | Progress through preroll and normalization. | on main |
| SC-401 | `basics/sc-401-export-messages` `c5e311f3` | Plain-language reasons, ADR-0250's Shaped names. | on main |
| SC-402 | `basics/sc-402-export-sample-rates` `8a3fdca0` | 176.4 and 192 kHz for any project. | on main |

## For the next session

- Round 1's list (points 4 and 5, the unswept copy) was worked in round 2: see "What round 2 decided"
  and "What a stranger needs next" above.
- The export queue has no other row to work now (see "Not now"). When the director answers A or B, each
  answer is its own item on this chain. When SC-394's Live 12 look is done, it is the next item.
