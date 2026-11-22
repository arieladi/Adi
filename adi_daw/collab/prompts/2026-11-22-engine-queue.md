# engine — standing queue, 22 November 2026

This is your whole queue from win; work it until your usage limit. It replaces every earlier list win gave you.

## How to work

- Fetch first: `git fetch origin`, then read the top entry of `collab/win.md` on main and the Engine table of `docs/DEFERRED.md`.
- Work the items top to bottom. Use one branch per item, `engine/<topic>`, cut from origin/main unless the item says to stack.
- Write the test first where the item names one. Build and test with GCC and Clang, warnings as errors.
- Push the branch, write the entry in `collab/engine.md` on `engine/log`, end it with "PR requested", and start the next item in the same turn.
- If an item is blocked on win, another seat or a decision, park it with one line in your log and take the next one.
- A question for the director goes under "FOR THE DIRECTOR" at the top of your log, with your recommendation. Then continue.
- Stay in your own paths (`collab/README.md`). Put anything you need in another seat's paths in your log's needs table.
- When this list is empty, take the next unbuilt engine row of `docs/PLAN.md` in milestone order. Work meant for after 3 December goes on branches marked "lands after 3 December".
- Never stop because "nothing names this lane". Stop only at your usage limit or when the environment is broken.

## Queue

Before 3 December (ADR-0248 d2 and d6)

1. **E23-FU: the #677 follow-ups, stacked on 76c8aaa1**
   #677's head is now 76c8aaa1. It carries win's reconciliation (79c424de) and the EQ Eight macOS fix. Win lands #677, so never push to `engine/df-e23-plugin-workers`.
   Review that head: the 1.22 to 1.23 migration, and the combined VST3 adapter with the Mix View's `inputBuses_`. Then cut `engine/df-e23-followups` from 76c8aaa1. Port onto it DF-E30(a-c) with one-click Post (167d863e), DF-E55(b) (096edc93) and DF-E24(b,c) (72dfdc77). Keep #696's 512-frame MIDI clock fixture.
   Files: `ops_catalog.cpp`, `engine/plan.*`, `graph.*`, `return_send_tap.*`, `track_image.*`, `src/juce/vst3_host.*`, and their tests. Hunks in the window's files (`ui_mixer.cpp`, `tests/test_ui_seams.cpp`) go in your needs table for the window lane to review.
   Test: Return Send Tap, Plan, Catalog, Strip Lane Bindings, MIDI Clock Continuity, UI Seams, Track Delete, Replay and Ops Schema all pass. WTS-01 5b passes once the window's grouping rule exists.

2. **MI08-MPE: MI-08 over DF-E56's fix**
   Rebase `engine/mi08-expression-units-current` and `engine/mi08-expression-playback-current` over gemini's DF-E56 head: da6d1629 today, or the newer head gemini logs for its DF-E56-ATTACK item (if that comes after your rebase, rebase once more). Both touch `mpe_output.cpp`.
   Add the joined host regression your 22 November review named: 128 releases through the real CLAP and VST3 adapters, with both SDK floors at 256 and none left pending. A CLAP release that crosses a segment keeps its original dialect.
   Test: 126 MPE output checks, and the 120-note/104-slot probe through both adapters.
   The refused-attack residual (reserve room for the reset and the attack together) is gemini's DF-E56-ATTACK item while `mpe_output.cpp` is on loan; review its commit when logged. If the loan comes back without that fix, take it here.

3. **empty-project-cost: the cost of an empty project**
   An empty New project costs 90-150 us per 64-frame callback (11-14 % of the deadline). Find the fixed per-callback work in GraphHost, the plan, the strips, the meters and Main loudness, and remove the waste without changing the output.
   Files: `engine/graph.*`, `plan.*`, the callback path in `session.cpp`, `main_loudness.cpp`, `tools/benchmark_blocks.cpp`. Keep your `session.cpp` hunks small: engine-win's DF-E64 Play path and PD-06 also touch it.
   Test: a Release benchmark at 64 and 128 frames gives the empty-project p99 before and after, with the hot spots named. Output stays byte-identical and WTS-01 is unchanged. Add the breakdown to `adi_builtin_realtime_tests` once `devices/perf-pass` lands; until then, measure with `tools/benchmark_blocks.cpp`.

4. **PD-03: ParamOps::snapshotAll**
   Give it a definite result that tells "unchanged" apart from "could not capture". It writes one `device.loadState` per attached device whose chunk changed, before Take Snapshot, close, Save As, Save a Copy and Collect. This is the contract engine-win asked for. The window's call sites come after it.
   Files: `engine/param_ops.*`, `tests/test_param_ops.cpp`.
   Test (PLAN PD-03): a fake device with stateEpoch 0 and a changed chunk gets exactly one loadState at Take Snapshot and one at close. An unchanged chunk writes nothing, and a refused capture is reported.

5. **landed-parts: check the sub-parts before rows close**
   Your list of landed rows is right for whole rows only. Check these parts on main and report each one as landed or open, with its commit: DF-E45(b), DF-E47(b), DF-E51(c) and (d), DF-E52(b), DF-E54(b), and the window half of DF-E16 (window-fn's).
   Output: a table in your log, so win moves only the landed parts to Done. Build anything that is open and yours as its own item.

6. **DF-E17: rebase only; lands after DF-E35(a)**
   Rebase `engine/df-e17-edit-invariants` (c0269d2d, 0c30d4ce) onto current main, which now has DF-E47's projection and DF-E27(a). Drop the redundant fade backport. Reconcile `snapshot.cpp`, `ops_catalog.cpp` and the clip edit fixture.
   ADR-0248 d6 keeps DF-E17 out until its lane semantics agree with DF-E35. DF-E35(a) is engine-win's, after #677. Agree the cross-track `clip.move` lane rule with engine-win's CT-01 EditScope work in `edit_plan` through your logs. Then mark the PR request "lands after DF-E35(a)".
   Test: clip edit, device ops, automation edit, material commands and engine suites pass. A linear clip move plus Undo keeps `pos_ns` and `length_ns`.

7. **DF-E10: Link assertion diagnostics**
   Make the #548 Link assertion print the stop intention, beat and tempo when it fails, as DF-E12 did. Then the next ARM64 failure can be diagnosed.
   Files: `tests/test_link_session.cpp`.
   Test: `adi_link_session_tests` passes, and a planted failure prints the captured state.

8. **DF-E64: re-review engine-win's first block** (blocked: engine-win has not pushed past c7f3d9cc on `engine-win/play-first-block`; skip to the next)
   Re-review once the stopped prefetch is limited to the driver's Play path and the delayed-reader driver test exists. This is review only (`clip_playback.cpp` is on loan).
   Test: clip candidate readiness 90/90, including "a stopped captured transport needs no pages", and WTS-01 2b with a delayed reader.

After 3 December (branches marked "lands after 3 December"; ADR-0249 d3 order: racks first, then DEV-32, then the rest)

9. **DEV-01: racks play (level 1)**
   Splice racks into the track graph: the router, chain gates, serial chains, chain strips and the double rack sum. Retire ADR-0122 d8's rack skip. Keep unknown latency distinct from zero. Use the RK1, RK2 and RK4 designs.
   Files: `engine/graph.*`, `plan.*`, `realize.cpp`, `session.cpp`, `tests/test_session.cpp`.
   Test: test_session's rack fixture is inverted. Two chains at 0 and 64 samples line up and the rack reports 64, nested too. An empty chain passes the signal dry.
   It cannot land before win's ADR-0060 d1 amendment. Build it anyway.

10. **DEV-03: rack structure ops** (stack on DEV-01)
    Rack structure ops with exact Undo, and rack paste with fresh ids (this lifts #394's refusal). `device.remove` and `chain.delete` capture the whole subtree.
    Files: `device_copy.*`, `ops_catalog.cpp`, `tests/test_device_ops.cpp`, `test_device_copy.cpp`.
    Test: removing a populated rack, then Undo and Redo, gives an identical digest. A self-nest is refused, and a rack paste gets fresh ids. OPS 6.3's `lossy_undo` threshold holds.

11. **DEV-06: macros** (stack on DEV-03)
    `macro.1` to `.16` and chain select become rack parameters. A MacroSource emits ParamValue through each target's curve, with Live's takeover and the summed multimapper.
    Files: `engine/graph.*`, `param_automation.*`, `ops_catalog.cpp`.
    Test: A8D-018's 16 macros, takeover, and d15's sum in real units, clamped.

12. **DEV-07: modulation, engine side** (blocked: the devices lane's LFO and Shaper kernels do not exist yet, and no schema minor is reserved for the route tables; skip to the next)
    Under ADR-0230: route side tables, modulator nodes emitting ParamMod, and one evaluator per block (the base plus one summed offset per target), with mono and global scope. The evaluator must clear a withdrawn or disabled source.
    Test: one source modulating a CLAP, a VST3 and a built-in gives the same curve. The base survives stop, save and reopen. Deleting a source is one undo step.

13. **DEV-32: read his `.adg` rack presets, read-only**
    Build a JUCE-free reader beside TM-IC-03's converter rules. A rack, its chains and macros import as one undo step, with older device versions mapped and a per-slot loss report. Modulation routes go to the loss report until DEV-07 lands.
    Test: hand-written minimal racks first; Live-saved fixtures come through the devices lane's ONE list. A minimal stock-device rack loads with the same chains and macros. An unbuilt device lands in the loss report, and one Undo removes the import.

14. **DEV-25: plug-in readiness states**
    Add owner-side instantiation and state-acceptance transitions to the VST3 and CLAP hosts, plus a callback-safe first-process signal. DEV-25 (engine-win) then shows and exports readiness.
    Files: `src/juce/vst3_host.*`, `clap_host.*`, `tests/test_clap.cpp`, the VST3 fixture test.
    Test: a real VST3 and a real CLAP report Instantiated, StateAccepted, then FirstProcessed, the last only after a real process call. The callback makes no allocations.

15. **DEV-17: one note-identity namespace**
    Build one process-wide event-source prefix allocator: the prefix in the high 32 bits, a serial in the low 32. It covers clip tracks, live inputs, transform nodes and Pd output. Per-instance int32 maps in the CLAP and VST3 hosts replace the `% 0x7FFFFFFF` fold. A clip's serials come from the stored note identity, so a rebuild keeps held notes (XC-25).
    Test (PLAN DEV-17): a clip note and a live note reach one CLAP and one VST3 fixture with distinct ids. 10^6 random live ids never collide while held, and NOTE_END frees the map slot.
    This unblocks DF-E39 and DEV-19 for the devices lane.

16. **DEV-18: the event transform phase** (stack on DEV-17)
    `transformEvents` runs in `Graph::forwardEvents` after a slot's inputs are gathered and before splits. Transform nodes own their notes by prefix. Generated notes are released on bypass, when a node leaves at a swap, and on stop, seek or a loop jump. The clock runs while bypassed, and prepare is idempotent.
    Test (PLAN DEV-18): #207 is not on main, so port its timing tests as ADI's own (identical timing from 32 to 4,096 frames, zero allocations). Also grid9's generated-event ownership: no orphans after removal, seek, stop and undo.

17. **DEV-13 / DF-E40(a): segment quantum**
    When a node's `segmentQuantum()` is above 1, the graph splits that node's block only at multiples of it. Each segment gets the events inside it (ADR-0215 d2).
    Files: `engine/graph.*`, `tests/test_graph.cpp`.
    Test: hosts11's pd-segment-contract (buffers 64 to 511; offsets 1, 63, 64, 65 and 127). AR5: a 256 split as 1 + 255 nulls at the declared delay.
    `segmentQuantum()` is on `devices/pd-block-quantum`, not on main. Until it lands, add the virtual on Node yourself.

18. **DEV-28 / N13: Main's meter on the one loudness meter**
    Move Main's meter onto `dsp::LoudnessMeter`, with the LD7 readings snapshot and validity flags. Then do Auto Gain Stage's LUFS mode.
    Files: `engine/main_loudness.*`, `gain_stage.cpp`.
    Test: Main's readings equal `dsp::LoudnessMeter`'s on the Tech 3341 cases, with zero allocations and Main's CPU cost measured.
    Auto Gain Stage's LUFS mode waits on win's rule for a selection under 400 ms. Do the meter part first.

19. **MI-08: noteExpression ops** (blocked: `noteExpression.write` and `noteExpression.clear` are not reserved in RESERVATIONS.md; skip to the next)
    The two ops ride in `note.applyDiff` with exact Undo over the AEXP table. Add the gain and pan dimensions for DF-W12.
    Files: `note_diff*`, `ops_catalog.cpp`, `docs/OPS.md`, `tests/test_note_diff.cpp`.
    Test: write and clear Undo restore the exact AEXP bytes, and the OPS validator passes. The check with real MPE synths stays win's.

20. **TM-SV-01: Global Quantization, engine half**
    `transport.launchQuantization` in session_state, a pure `nextGridTick`, a quantized locate with MIDI chase, and the Link quantum. The default is 1 Bar until a fresh-install reading says otherwise.
    Files: `engine/transport.*`, `session.cpp`, `ops_catalog.cpp`.
    Test: `test_launch_quantize`, a quantized-locate test, and N4's sub-bar values in 6/8 and 7/8.

21. **DEV-14 / DF-E40(b): Pd reset and MIDI-effect patches** (blocked: win's ADR on kinds, Consume and [clone] voices; skip to the next)
    The engine calls `PdDevice::hostReset()` on stop, bypass and panic. MIDI-effect patches run in the transform phase (item 16).
    Test: AR10, reset within a block.

## Waiting on win

Do not redo these; they wait for win to land or close them.

- `engine/df-e23-plugin-workers` (#677, head 76c8aaa1; win lands it)
- `engine/df-e23-followups-staged`: superseded by item 1's branch once that is pushed; close it then
- `engine/df-e27-finite-open-payloads` d0cf6d3c (DF-E27 f)
- `engine/df-e27-host-telemetry` 0d14bd14 (DF-E27 g)
- `engine/df-e24-vst3-aux-sinks` 2705f6d2 (DF-E24 a)
- `engine/df-e55-vca-device-insert` b157aa00 (DF-E55 a)
- `engine/df-e63-native-tick-readers` c37e20e0 (DF-E63)
- `engine/mi08-expression-read-current` db864e54
- `engine/mi08-expression-units-current` 0273aa63 and `engine/mi08-expression-playback-current` 2f1b9d41 (item 2 moves them over da6d1629; log the new heads)
- `engine/midi-notes-new-clip` 254d7d1c
- `engine/n63-ordered-note-batch` c97f2a96
- `engine/n64-recording-snapshot` 8a32d9e3
- `engine/n67-auto-monitor-default` f2802a00
- `engine/param-replacement-receipt` 4a1b47e7
- `engine/log` 42806c85
- Your DF-E53 review of `gemini/df-e53-canonical-param-ref` 03be0b99 is done (green, log of 22 November). Nothing more is needed.
- Superseded, for win to close: `engine/mi08-expression-playback`, `engine/mi08-expression-host-units`, `engine/mi08-note-expression-read`, `engine/df-e24-vst3-routing`, `engine/df-e27-clip-fades-and-end`.

## Needs from other seats

- **win:** reserve `noteExpression.write` and `noteExpression.clear` in RESERVATIONS.md (item 19; asked since 19 November). Reserve a schema minor for DEV-07's route tables.
- **win:** write the ADR-0060 d1 amendment before DEV-01 lands. Write the DEV-14 ADR on kinds, Consume and [clone] voices. Rule what an Auto Gain Stage selection under 400 ms reads.
- **win:** move to Done only what landed whole: DF-E1, E2/E5 (#530), E3, E4, E7, E9, E11 to E15, E18, E26(a,b), E27(a-d), E29(a-d), E48, E49. For DF-E45, E47, E51, E52, E54 and E16, close only the parts item 5 confirms. DF-E12's cause stays open.
- **win:** land the parts of #677 in order with the other seats' `session.cpp` work (engine-win's DF-E64 Play path and PD-06 Session fix).
- **window:** review the window-file hunks of item 1 (`ui_mixer.cpp`, `test_ui_seams.cpp`).
- **engine-win:** limit DF-E64's stopped prefetch to the driver's Play path and add the delayed-reader test. PD-06 binds the recording path to the Save As successor. N66 keeps DF-E44's close-every-sink behaviour when joined. Agree the `clip.move` lane rule for DF-E17 with CT-01's EditScope work. DF-E35(a), the ClipNode lane field, uses the engine's snapshot loan after #677.
- **window-fn:** DF-W38: the Re-Enable Automation menu must use the binder's resolved lane id. Guard Save As while a recording is active or pending, and show `discard()`'s refusal. Adopt `notes::prepareBatch` (N63) and the N64 snapshot fields. Wire PD-03's call sites once item 4 lands.
- **devices:** land `devices/perf-pass` (`adi_builtin_realtime_tests`) for item 3. Land `segmentQuantum()` from `devices/pd-block-quantum` for item 17. Build the LFO and Shaper kernels for DEV-07. Put DEV-32's minimal rack fixtures, DF-E6, N53, AU05 and Crop on the ONE Live list.
- **gemini:** the DF-E56 refused-attack residual while `mpe_output.cpp` is on loan (its DF-E56-ATTACK item, first in its queue). Its register audit is a second check of your landed-whole list only; your item 5 keeps the sub-parts.

## For the director

- DF-E29(e): with Delay Compensation off, does MIDI keep the upstream plug-in's physical latency, or follow heard time like automation? (The engine recommends physical latency, as an exception to ADR-0245 d8.)
- DF-E6: with more than one beat of render latency, does the transport start later, or does the count-in cue keep clicking until bar one is heard? (This needs a Live 12.4 reading.)
- N53: where does Live 12.4 put a crossfade's overlap when one clip lacks hidden material (full, one-sided and no-handles cases)?
- N54: should material Paste refuse the other kind (MIDI onto an audio track), or keep ADR-0045 d1's hybrid track? (The engine recommends keeping ADR-0045.)
- DF-E38: Live readings for AU05 (automation moved or deleted with material) and Crop (ED-04), from the ONE list in your session.
