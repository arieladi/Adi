# Export basics report

**Chain** `basics/export-chain` (from origin/main, 28 November). One commit per item, SC id in the
message. No pull request: win merges the chain into a landing stack.

## Round 1 of the export stream (28 November)

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-479 | `basics/export-chain` `see below` | An export renders one copy of the song taken when Export is pressed (`render::PinnedProject`, a hidden `.adi-export-*.adi` beside the project, removed when the export ends), so an edit made while it runs reaches the song and never the files. A plug-in that plays but cannot be loaded a second time stops the export and is named with its reason (`Job::playingDevices`); out of memory while loading the second set names the device. New suite `adi_export_while_playing_tests`. Engine (`queue.*`, `session.cpp` resolveOne), window-fn (`ui_main.cpp` startRender). | Pending the slot check; see the proof line below. | in progress |

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

- Continue SC-479 at point 4 (real-time-only parts: a test that fails today, then the fix the director
  picks under B; build the test first, it is wanted either way) and point 5 (a test that a second Export
  is refused while one runs, and that Freeze/Bounce waits for a running export).
- The pinned copy costs one read of the project file on the message thread at the press (a project with
  large sampler states takes longer), and the copy's size on disk while the export runs. A copy left by a
  crash is a hidden `.adi-export-*.adi` beside the project; nothing sweeps it yet.
