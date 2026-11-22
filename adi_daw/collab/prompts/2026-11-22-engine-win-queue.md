# engine-win — standing queue, 22 November 2026

This is your whole queue: work it until your usage limit ends the session. It replaces every earlier list win gave you.

## How to work

- Fetch main first (`git fetch origin`), then read the top entry of `collab/win.md` on main and every entry
  addressed to you in `collab/engine.md` on `engine/log`.
- Work the items top to bottom. One branch per item, `engine-win/<topic>`, cut from origin/main unless the item
  says to stack.
- Test first where the item names a test. Build and test on MSVC (/W4 /WX).
- Push, write the entry in `collab/engine-win.md` on `engine-win/log`, end it with "PR requested", and start the
  next item in the same turn.
- An item blocked on win, another seat or a decision: park it with one line in your log and take the next.
- A question for the director goes under "FOR THE DIRECTOR" at the top of your log, with your recommendation. Then
  continue.
- Stay in your own paths (`collab/README.md`). A need in another seat's path goes in your log's needs table, and
  you build the rest.
- When this list is empty, take the next unbuilt row of your lane in `docs/PLAN.md`, in milestone order. Work meant
  for after 3 December goes on branches marked "lands after 3 December".
- Never stop because "nothing names this lane". Stop only at your usage limit or a broken environment.

## Queue

Items 1 to 6 are before 3 December (ADR-0248 d2). Items 7 to 16 land after 3 December.

1. **DF-E64: rework play-first-block (WTS-01 2b)**
   Do the engine's requested changes on `engine-win/play-first-block` (c7f3d9cc): the stopped read-ahead serves
   the driver's Play path only, so strict `ClipPlayback::primeAt` keeps its contract; keep the pending-Play clock,
   Stop and Locate; add a deliberately delayed reader to the focused driver test.
   Files: `src/adi/engine/clip_playback.cpp/.hpp` (lent for DF-E64), `tests/test_clip_candidate_ready.cpp`.
   Test: `test_clip_candidate_ready` 90/90, including "a stopped captured transport needs no pages"; the
   delayed-reader test holds Play silent, then plays exact samples; WTS-01 2b PASS. Then "PR requested" again.
   (Partly blocked: the Play-path hunk in `src/adi/engine/session.cpp` is not lent. Ask the engine for the hunk
   or win for a loan, park that part and build the rest. The `window_state.hpp` and `project_document.cpp`
   hunks go to window-fn for owner review. `tests/acceptance/*` is win's: run WTS-01, do not edit it; ask win
   for a harness change.)

2. **N66: rebuild recording-failed-prefix on DF-E44**
   Rebuild `engine-win/recording-failed-prefix` (0747821d) on `engine-win/recorder-failure-recovery` (0ed2c355).
   Resolve the conflicts in the recorder, its header and its tests. `finalizedPrefix_` is set only after the
   write loop and every sink close succeed. Keep DF-E44's close-all, reset-before-discard and error_code discard.
   Files: `src/adi/engine/audio_recording.*`, `src/adi/recording_commands.cpp`, `tests/test_audio_recording.cpp`.
   Test: the joined head passes DF-E44's 170 recorder checks and N66's recording, recording-commands and
   document suites; a close that fails after a discontinuity keeps no prefix; WTS-01 2a PASS. Request one
   stack, DF-E44 then N66.
   The N66 hunk in `src/adi/ui/project_document.cpp` and `tests/test_ui_document.cpp` collides with window-fn's
   DFE44-DISCARD at the discard sites. Say in your log which way it goes: window-fn stacks on your joined head,
   or window-fn takes that hunk itself.

3. **Register audit: your own rows met on main**
   In your log only, list for each row the landed PR or commit (#646, #666, #686, #694) and the test that
   proves it: DF-E19 (a)-(d), DF-E20 (c), DF-E25 (a)(b), DF-E28, DF-E31, DF-E32, DF-E33, DF-E34 (a)-(d),
   DF-E35 (b), DF-E58. Name what stays open: DF-E25 (c), DF-E34's same-shape edits, DF-E35 (a).
   Your rows only; the gemini seat audits the rest.
   Done when win can move each row to Done with a PR number.

4. **MX-09: place a recorded take for latency**
   Place a take earlier by the driver's reported input and output latency plus
   `audio.driverErrorCompensationMs`, as Live does. Nothing in recording reads latency today. The placement API
   takes an explicit offset in samples.
   Files: `src/adi/recording_commands.*`, `src/adi/engine/audio_recording.*`, `tests/test_recording_commands.cpp`.
   Test: a simulated driver reporting 256 in, 384 out and 1 ms error compensation starts the take exactly that
   many samples earlier, PCM equal to the input; zero latency unchanged; WTS-01 2a still exact.
   The driver figures come from Session (engine's): build against the explicit offset and ask the engine to
   expose them. The PLAN row's lane column says "engine, window, win": ask win to confirm the recording half is
   yours.

5. **CT-01: edit producers take their lane from EditScope**
   Move, resize-end, trim and delete act on take-lane clips within their own lane (`edit_plan` admits only main
   lanes today) and never clear main-lane material.
   Files: `src/adi/edit_plan.*` (the engine's tree, ADR-0248 d7 item 6 gives you the row: the engine reviews the
   hunks) and its tests. Line up with the engine's DF-E17 `clip.move` lane rule before you write.
   Test: a take-lane clip moves and trims inside its lane in one undo step; main-lane clips under it are
   untouched; main-lane editing unchanged; ops and schema validators pass.

6. **CT-03: MIDI loop takes**
   One take clip per pass, through `notes::captureNotesForNewClip` plus `prepareDiff`, in the same transaction
   as the clip's creation; held notes close at the pass end on the take while the main copy carries them; record
   quantize is its own undo step.
   Files: `src/adi/recording_commands.*` (MIDI take materializer), `tests/test_recording_commands.cpp`.
   Test: 3.5 MIDI passes give four take clips and a main copy, one undo per pass; a held note across the loop
   boundary matches the director's recorded reading; the old Rec-Q undo is separate (AT-14, A-H02).
   Stack on `engine/midi-notes-new-clip` (254d7d1c, PR requested). If it changes before landing, rebase.

7. **PD-06: Save As rebinding for interrupted recording**
   The engine's requested change on `engine-win/interrupted-audio-recording` (db039e65): `Session::startRecording`
   checks `recordingProject_`, which keeps the old path after `ProjectDocument::saveAs`. Bind it to the accepted
   successor connection. Then rebase PD-06 and PD-05 (`recording-launch-discovery`, 879a5d15) onto the DF-E44 and
   N66 head.
   Files: `src/adi/recording_commands.*`, `src/adi/engine/recording_recovery_*`, their tests.
   Test: Save As, then Record with fresh and with stale recovery options: fresh ones accepted on the new file,
   stale ones refused; the 572 PD-06 and 434 PD-05 checks pass on the restacked tips.
   (Partly blocked: `Session::startRecording` is in `session.cpp`, the engine's and not lent. Ask the engine for
   the hunk or a loan; do the restack and the test meanwhile.)

8. **DF-E25 (c): match a saved VST3 by its class ID**
   As ruled in ADR-0248 d7, as Live does. JUCE's identifier embeds a hash of the file path, so a moved or
   reinstalled plug-in fails to resolve. Old identifier strings stay readable; new catalogue records carry the
   exact class ID beside them.
   Files: `src/juce/juce_device_loader.*` (catalogue lookup, `makeVst3`), `src/adi/plugin_registry.*`, the scan
   packet.
   Test: a project saved with the VST3 fixture at path A loads after the module moves to path B (catalogue hit, no
   NeedsDiscovery); an old uid still resolves; two classes in one module stay distinct; scanner, catalogue and
   `adi_play_render` suites pass.
   If the project's device ref must store the class ID, ask win to reserve schema 1.24 first (free today).

9. **DF-E35 (a): lane_id in ClipNode** (blocked: #677 must land and the engine's loan of `snapshot.cpp`'s clip
   projection start; skip to the next)
   `ClipNode` carries an optional `lane_id` and compares it in `same()`, so the snapshot stops handing take clips
   to main-row readers.
   Test: a take overlapping a main clip, where `hit()` returns the main clip; `clip.setLane` changes the node;
   projects without lanes unchanged.

10. **DEV-25: Clean discovery mode**
    Live's Alt+Rescan and Alt at launch, in the catalogue API: ignore the scan cache and probe every candidate,
    keeping manual blocks; one job, never on the message thread. The window wires the keys.
    Files: `src/juce/juce_device_loader.*` (N58 service), `src/adi/plugin_scanner.*`, `plugin_registry.*`,
    `tests/scanner_discovery.hpp`.
    Test: after Clean, every candidate is probed exactly once, K=4 matching K=1; the cache is rebuilt; manual
    blocks survive; Incremental and CacheOnly unchanged.

11. **EX-05: export brickwall stage**
    `Normalization.brickwall` (off by default) calls `LookaheadLimiter::processOffline` in `finish()` before
    dither; Peak mode never acts; at most 4 refine trials, steps clamped to ±3 dB; measure with
    `dsp::LoudnessMeter`.
    Files: `src/adi/render/normalization.*`, `src/adi/render/queue.cpp`, the render normalization tests.
    `normalization.*` is not in your path list: ask win to confirm it goes with the export queue.
    Test: on equals off, byte for byte, for a signal under the ceiling; an over stays under the ceiling at 4x;
    length unchanged (the devices lane's handoff tests).
    Stack on `engine-win/export-quantizer` (7a9fe563) and `devices/limiter-offline` (#706). If #706 has not
    landed, build on it and say so.

12. **EX-04: FLAC 16/24 writer**
    On the shared seeded quantizer.
    Files: a new writer in `src/adi/render/`, `queue.cpp`, the dependency pin in the fetch script.
    Test: FLAC 16 and 24, decoded, equal the PCM WAV of the same capture; the quantizer runs once.
    Stack on export-quantizer. A workflow change for the libFLAC fetch is win's: ask for the hunk.

13. **EX-04: AIFF writer**
    16, 24 and 32-bit float on the same quantizer API.
    Files: a new writer in `src/adi/render/`, `queue.cpp`.
    Test: AIFF decoded equals the WAV of the same capture; the refusal matrix covers unsupported pairs.
    Stack on export-quantizer.

14. **TM-MD-03: size and modification-time memo (DF-E34 (a)'s rest)**
    A memo for hash verification, so a same-shape edit of a compressed source becomes a mismatch (clip Offline)
    whether or not it is warped, with no whole-file digests on warm opens.
    Files: `src/adi/audio/progressive_decode.cpp`, `decode.cpp`, the relink resolve.
    `src/adi/audio/**` is not in your path list: ask win in your needs table who owns it and for a loan before you
    write there; build the memo and its test on your side meanwhile.
    Test: re-encode a source at the same length and reopen it twice with H.wav evicted between: the same mismatch
    both times; 50 warm sources open with zero digests (the A-M3 cases).

15. **EX-03: Render as Loop and tails**
    Live's two passes (the first unwritten, latency compensated) and `Job.tail` Off, Fixed or UntilSilent;
    refused inside a gapless batch.
    Files: `src/adi/render/queue.*`.
    Test: an impulse before the range and a decaying effect past it render the same as a continuous pass; a
    looped render's start wraps the tail.
    Where the first pass starts waits on the director (below): build the rest and park that case.

16. **CT-04: engine comp producer**
    A range copied from a take lane, erased on the main lane and pasted, as one splice step; provenance is
    derived, never stored.
    Files: `src/adi/edit_plan.*` (engine reviews) and the take-lane commands.
    Test: comping a range from lane 2 onto the main lane is one undo step and plays that take's PCM; an unfaded
    comp nulls against its takes (CT-05's null test).
    Depends on item 5. Edge cases wait on the director's readings: build the plain case.

Not queued, because it is on main: the graph-workers setting of ADR-0248 d7 item 6 (`audio.graphWorkers`,
`settings/registry.cpp`). Do not add it again.

## Waiting on win

Do not redo these; they wait on win's landing.

- `engine-win/recorder-failure-recovery` 0ed2c355 (DF-E44, engine review passed; item 2 stacks on it)
- `engine-win/export-host-owner` 6b78d94c (N62, WTS-01 7d)
- `engine-win/scanner-cache-ownership` c6bdb514, then `scanner-cache-paths` 1d6dd030 (DF-E62)
- `engine-win/project-plugin-saved-path` 43787b7b (N68)
- `engine-win/export-quantizer` 7a9fe563 (EX-06)
- #642 `engine-win/acceptance-ci` 68c92bbc (its macOS GUI job was cancelled; win re-runs it), then
  `engine-win/worker-measurement-kit` 3ac1b3ff
- #663 `engine-win/clap-transport-gate`; #665 `engine-win/dfe23-hardware-diagnostics` (draft, after #677)
- Withdrawn, will not land (win closes or deletes): `engine-win/mpe-held-notes`, `midi-capture-clock`,
  `project-check-journal-targets`
- Held to after 3 December by ADR-0248 d7 (EX-08, EX-09, PD-11, PD-12) and other requests of 19 November:
  the settings-safety-integration family, project-wal-benchmark/observation, project-sqlite-defensive,
  project-removable-sync, project-salvage, project-quick-check, project-copy-lock-neutral,
  project-snapshot-backup, wav-bwf-v2, render-job-json(-full-bytes), render-preset-groups,
  project-saved-render-jobs, user-render-presets, export-preset-integration, publication-safety-integration,
  collection-safety-integration and collect-*/archive-*, extract-cli-warning, media-cache-probe-integration,
  ffmpeg-pcm-stream, streamed-process-output, decode-cache-runtime-limits/folder, analysis-files-switch,
  scanner-windows-shortcuts, scanner-helper-self-test, acceptance-assets

## Needs from other seats

- **engine:** the Play-path hunk of `session.cpp` for DF-E64, or a loan through win; the `Session::startRecording`
  hunk for PD-06, or a loan; land `engine/midi-notes-new-clip` 254d7d1c (CT-03); land #677 and start the
  `snapshot.cpp` loan (DF-E35 (a), and DEV-23's hosted-worker remeasurement); expose the driver's input and
  output latency to Session for recording placement (MX-09); review the `edit_plan` hunks (CT-01, CT-04) and
  agree them with DF-E17's `clip.move` lane rule.
- **window-fn:** owner review of DF-E64's `window_state.hpp` and `project_document.cpp` hunks; N66's
  `project_document.cpp` and `test_ui_document` hunks (stack on the joined head or take the hunk; it collides
  with DFE44-DISCARD); surface `discard()`'s cleanup refusal and the retained directory in the Record path;
  guard Save As during active and pending placement; call `recording::configureRecovery` from F9, loop record
  included; PD-05's startup hook and the named reopen question; map pow-r1/2/3 to Shaped 1, 2 and 3 on the
  Export page once EX-06 lands; wire EX-11 to N62's owner servicing.
- **devices:** land `devices/limiter-offline` (#706) before EX-05's brickwall stage.
- **win:** a loan or a ruling for DF-E64's `session.cpp` part if the engine prefers to lend; re-run #642's
  cancelled macOS GUI-shell job; any WTS-01 harness change in `tests/acceptance`; reserve schema 1.24 if
  DF-E25 (c) stores the class ID in a project ref; confirm the recording half of MX-09 and
  `render/normalization.*` (EX-05) are yours; who owns `src/adi/audio/**` and a loan for TM-MD-03; the libFLAC
  workflow hunk (EX-04); the media ADR before TM-MD-01;
  a loan or ruling on `store.*` for PD-05's session_lock heartbeat and stale-owner handling (the engine's path).

## For the director

- CT-01 (M6-A6H-029), for the single Live list of ADR-0248 d7: does auditioning a take lane silence the main lane;
  is the audition saved; is it undoable; what is a new lane named.
- CT-02 (M6-A6H-027): where pre-roll goes in a loop recording, and the punch-in/out policy.
- CT-04 (M6-A6H-030): comping across empty lanes or silence, and an edge past a take's end.
- EX-03: where the first pass of Live's Render as Loop starts.
- EX-05 (the devices lane's question; engine-win builds the stage): should the export brickwall detect finer than
  BS.1770's 4x (say 8x), or does the -1 dBTP ceiling cover the gap measured at up to +1.6 dB on noise?
