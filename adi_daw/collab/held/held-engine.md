# held-engine — the engine seat's unlanded work, gathered into one chain

**This chain lands only when the director says so.** Nobody merges `held/engine-chain` before his word.

- Chain: `held/engine-chain`. Log: this file, on `held/held-engine-log`. No pull request is open.
- Base: `origin/land/basics-1128c` while it is not on main; origin/main once it is. The base is merged into
  the chain before each push.
- Proof: the build PC's slot 15 (MSVC Debug, warnings as errors, `ctest -LE performance` with the
  acceptance suite; then the window suites and the app).
- A stranger continues from here: read "Where it stands" first, then the step you are on.

## Where it stands

Round 1 (28 November): step 1 (the survey) done. Step 2: 22 of the 28 stack tops are merged into
`held/engine-chain` (head `b9716587`, with three fix commits); one (TM-IC-02) turned out to be on the
base already; five are **not merged yet**, each a port onto code the base rewrote since 22 November,
not a plain merge (the list and why, under step 2). Step 3: the chain **builds** under MSVC Debug /WX;
294 of 300 core suites pass; three failures are likely the merges' (clap, link playback, MIDI capture),
one is the base's own (ui_document), two look like parallel-load flakes (scan cache, library). Details
and the next steps under step 3. The `all` check has not run yet.

Round 2 (28 November, evening): `land/basics-1128c` is on main now, so **BASE is origin/main**; merged
into the chain (`cd7eb8bb`). The round-1 failures in clap and MIDI capture were read: main's tests
written while a CLAP wire carried the engine's source note ID and the parser's IDs were 1, 2, 3; DEV-17
assigns per-instance wire IDs and per-source prefixes. The checks now compare against the note's own
wire ID or the source's own full identity (`7f09d582`). **DEV-06 (racks and macros) is ported and merged** (`6e115393`); how, under
step 2. Not yet built: the slot queue held the first round-2 check ("waiting for a build place") for
over two hours.

**The next session starts here:** step 3's list (round 2). Merge the base into the chain first
(`git fetch`, then the BASE rule of the mission). The local branch names `held/engine-chain` and
`held/held-engine-log` may be held by an older worktree; work on any local name and push to the
origin names (`git push origin <local>:held/engine-chain`).

## For the director

(Completed at step 4. The engine seat's own open questions stay in its log, `collab/engine.md` on
`engine/log`; the ones you already answered are in `collab/win.md`, d7 of the 25 November rulings.)

Gathered so far, for when the chain is proved:
- **When does this chain land?** Every branch in it was marked by its seat "lands after 3 December";
  your mission says it lands only on your word. Recommendation: land it after the 3 December hold ends,
  on one clean core and window check of its head, as one pull request through win.
- **DEV-28 replaces Main's loudness meter** with the shared one. The silent-block shortcut that cut
  factory New's callback cost (empty-project-cost, landed) lived in the replaced code. Recommendation:
  accept the shared meter (one meter for Main and the analysers) and have the engine seat re-measure
  factory New's callback after landing; if it regressed, put the shortcut into the shared meter.

## Step 1 — the survey (28 November)

`git branch -r --no-merged origin/main`, those under `origin/engine/`: 67 branches beside `engine/log`
(which is itself on main). Read against the seat's log (`collab/engine.md` on `engine/log`, entries of
19 to 24 November) and win's closed landing #723 (`land/engine-1123`, 23 November).

How each was judged:
- **contained in**: the branch's head is an ancestor of the named stack top (`git merge-base --is-ancestor`).
- **already on the base**: `git merge-tree` of the branch onto the base gives the base's own tree, or
  `git cherry` finds every commit on the base under another hash.
- **superseded**: the seat replaced it with a `-current` branch that is on main, or carried its parts
  in another branch that landed (the seat's log, and #723's "Superseded" list).
- **carried by another seat**: the row is lent to another seat (`collab/RESERVATIONS.md`, "PLAN rows on
  loan") and that seat's chain already holds the same files or the same commits.

No branch has a pull request of its own: the seat wrote "PR requested" in its log and landing was held
("lands after 3 December"). On 23 November win built `land/engine-1123` (#723) with 22 of these
branches; #723 was closed the same day, "superseded by #729 (rebuilt without the after-3-December
rows, ADR-0248 d2)". #729 landed only the three of them that are now on main (empty-project-cost,
mx23-track-delay-preview, df-e17-edit-invariants-current). The other 22 never landed. #723's
collision notes are used below as the reference for the same collisions.

| Branch | Head | Top of which stack | State |
|---|---|---|---|
| engine/dev01-rack-playback | cb8b4fb6 | dev06-rack-macros | contained in dev06-rack-macros |
| engine/dev03-rack-structure | 757d934a | dev06-rack-macros | contained in dev06-rack-macros |
| **engine/dev06-rack-macros** | **0a2e3eb9** | itself (DEV-01, DEV-03, DEV-06: racks and macros) | **to take** (no schema minor: the macros play from existing rows; minor 1.24 stays reserved for their side tables) |
| **engine/dev25-plugin-readiness** | **d8995f97** | itself (DEV-25) | **to take** |
| engine/dev17-note-identity | 4af8d018 | dev18-event-transforms | contained in dev18-event-transforms |
| **engine/dev18-event-transforms** | **4e749553** | itself (DEV-17, DEV-18) | **to take** |
| **engine/dev13-node-segment-quantum** | **59453a16** | itself (DEV-13 / DF-E40 (a)) | **to take** |
| **engine/dev28-main-loudness-meter** | **f8308a55** | itself (DEV-28) | **to take** |
| **engine/tm-md04-wave-metadata** | **d59778d7** | itself (TM-MD-04) | **to take** (the row is lent to the library seat, whose chain does not hold this reader) |
| **engine/pd04-analysis-publication** | **da074f14** | itself (PD-04) | **to take** |
| engine/srf03-controller-encoders | 169fd401 | srf04-controller-nudge | contained in srf04-controller-nudge |
| **engine/srf04-controller-nudge** | **45e69a22** | itself (SRF-03, SRF-04) | **to take** |
| engine/mx33-loopback-measurement | 63cad539 | mx33-sync-spikes | contained in mx33-sync-spikes |
| engine/mx33-offline-measure | 5e192277 | mx33-sync-spikes | contained in mx33-sync-spikes |
| engine/mx33-fixture-render | d8c0eeaa | mx33-sync-spikes | contained in mx33-sync-spikes |
| **engine/mx33-sync-spikes** | **53fad1c3** | itself (MX-33, MX-09's probe) | **to take** |
| engine/tm-fc01-bounce-range-mute | d1146559 | tm-fc01-bounce-mixer | contained in tm-fc01-bounce-mixer |
| engine/tm-fc01-remove-bounced-inserts | 6689b97c | tm-fc01-bounce-mixer | contained in tm-fc01-bounce-mixer |
| **engine/tm-fc01-bounce-mixer** | **8bc053ca** | itself (TM-FC-01) | **to take** |
| engine/tm-ic01-integer-timecode | ba6109f0 | tm-ic04-otio-linear-writer and tm-ls03-midi-transport-edges | contained in both |
| engine/tm-ic04-otio-exact-time | 1980d55a | tm-ic04-otio-linear-writer | contained in tm-ic04-otio-linear-writer |
| engine/tm-ic04-otio-linear-timeline | 36713a41 | tm-ic04-otio-linear-writer | contained in tm-ic04-otio-linear-writer |
| **engine/tm-ic04-otio-linear-writer** | **2f414469** | itself (TM-IC-01, TM-IC-04 OTIO) | **to take** |
| engine/tm-ls03-midi-sync-wire | 8fdbe416 | tm-ls03-midi-transport-edges | contained in tm-ls03-midi-transport-edges |
| engine/tm-ls03-midi-clock-follower | 5f83cc24 | tm-ls03-midi-transport-edges | contained in tm-ls03-midi-transport-edges |
| **engine/tm-ls03-midi-transport-edges** | **eb545c1e** | itself (TM-LS-03, on TM-IC-01) | **to take** |
| **engine/pd14-project-templates** | **ca5cd357** | itself (PD-14) | **to take** |
| **engine/ex02-via-main-routing** | **99c36e2e** | itself (EX-02) | **to take** |
| engine/au02-guarded-insert-shape | 8e44fe52 | au08-simplify-envelope and au08-envelope-clipboard | contained in both |
| engine/au03-write-pass-boundaries | c85c9f34 | au08-simplify-envelope | contained in au08-simplify-envelope |
| engine/au03-bounded-write-thinning | 2782fb8a | au08-simplify-envelope | contained in au08-simplify-envelope |
| engine/au03-guarded-arrangement-passes | da8b4adc | au08-simplify-envelope | contained in au08-simplify-envelope |
| **engine/au08-simplify-envelope** | **be846d1b** | itself (AU-02, AU-03, AU-08 Simplify) | **to take** |
| **engine/au08-envelope-clipboard** | **9ad32e5e** | itself (AU-08 Copy/Paste, on AU-02) | **to take** |
| **engine/au08-clear-clip-envelope** | **75ae50ef** | itself (AU-08 Clear Envelope) | **to take** |
| engine/mi06-fit-to-scale | e6d0c93c | mi04-step-scale-degrees | contained in mi04-step-scale-degrees (was in #723) |
| engine/mi04-invert-selected-pitches | c7c7927a | mi04-step-scale-degrees | contained in mi04-step-scale-degrees (was in #723) |
| **engine/mi04-step-scale-degrees** | **c3b86647** | itself (MI-06, MI-04) | **to take** (was in #723) |
| **engine/tm-wt02-interior-warp-markers** | **6fa013a5** | itself (TM-WT-02) | **to take** (was in #723) |
| engine/tm-wt03-native-warp-interpolation | 7745cd4b | tm-wt03-beats-zero-crossing | contained in tm-wt03-beats-zero-crossing (was in #723) |
| **engine/tm-wt03-beats-zero-crossing** | **c52e6ccd** | itself (TM-WT-03) | **to take** (was in #723) |
| engine/dev19-device-scale-timeline | 33880f60 | dev24-track-processing-time | contained in dev24-track-processing-time (was in #723) |
| engine/dev19-clip-local-device-scale | ad26772c | dev24-track-processing-time | contained in dev24-track-processing-time (was in #723) |
| **engine/dev24-track-processing-time** | **a16019c0** | itself (DEV-19, DEV-24 track time) | **to take** (was in #723) |
| **engine/dev24-callback-load-meter** | **72679a12** | itself (DEV-24 callback load) | **to take** (was in #723) |
| engine/dev02-chain-zone-admission | 61e95403 | dev04-drum-pad-routing | contained in dev04-drum-pad-routing (was in #723) |
| **engine/dev04-drum-pad-routing** | **b1b50df2** | itself (DEV-02, DEV-04) | **to take** (was in #723) |
| engine/srf08-mcu-fader-feedback | 52563508 | srf08-mcu-vpot-wire | contained in srf08-mcu-vpot-wire (was in #723) |
| engine/srf08-hui-wire | 34db3323 | srf08-mcu-vpot-wire | contained in srf08-mcu-vpot-wire (was in #723) |
| engine/srf08-mcu-lcd-feedback | 961a0cb4 | srf08-mcu-vpot-wire | contained in srf08-mcu-vpot-wire (was in #723) |
| **engine/srf08-mcu-vpot-wire** | **5f41d14d** | itself (SRF-08) | **to take** (was in #723) |
| engine/tm-ic02-smf-codec | ec515123 | itself (TM-IC-02) | **already on the base by another route** (found at step 2): the base's `617c138f` "Import and export MIDI clips as standard MIDI files" (26 November) carries `src/adi/interchange/smf.*` byte for byte, and its `tests/test_smf.cpp` holds every test of this branch plus 92 lines more |
| **engine/mi05-seeded-note-timing** | **4bc52ffc** | itself (MI-05) | **to take** (was in #723) |
| **engine/tm-ic04-compound-file** | **3ff8ba64** | itself (TM-IC-04 CFB writer) | **to take** (was in #723) |
| **engine/mx32-vst3-context-fixture** | **db99def9** | itself (MX-32's test fixture) | **to take** (was in #723) |
| engine/tm-sv01-launch-quantization | a676ca80 | itself | **carried by another seat**: TM-SV-01..04 are lent to the session seat; `seats/session-chain` holds `launch_quantize.*` and its test with this branch's content (two small later edits). Not taken, so the two chains do not add the same files twice |
| engine/tm-sv03-launch-gestures | 39ed4325 | tm-sv04-follow-targets | **carried by another seat** (as above; `seats/session-chain` holds `launch_gestures.hpp`, `launch_follow.hpp` and the test, one file extended) |
| engine/tm-sv04-follow-targets | 59056abc | itself | **carried by another seat** (as above) |
| engine/tm-tt01-tempo-ramps | 8f0705db | tm-tt01-tempo-ramps-current | **carried by another seat**: both commits are in `seats/stretcher-chain` and `basics/integrate-4` (TM-WT-04, lent to the stretcher seat, is built on it) |
| engine/tm-tt01-tempo-ramps-current | 7c37df25 | itself | **carried by another seat** (as above) |
| engine/mi08-expression-units-current | 922e275d | mi08-expression-playback-current | **already on the base**: the merge onto the base changes nothing |
| engine/mi08-expression-playback-current | b755e07e | itself | **already on the base**: the merge onto the base changes nothing |
| engine/mi08-note-expression-read | e5b350cd | mi08-expression-playback | **superseded** by `engine/mi08-expression-read-current` (on main) |
| engine/mi08-expression-host-units | 0c3d63ea | mi08-expression-playback | **superseded** by `engine/mi08-expression-units-current` (on the base) |
| engine/mi08-expression-playback | cd8d8e69 | itself | **superseded** by `engine/mi08-expression-playback-current` (on the base) |
| engine/df-e17-edit-invariants | 0c30d4ce | itself | **superseded** by `engine/df-e17-edit-invariants-current` (on main) |
| engine/df-e23-followups-staged | 72dfdc77 | itself | **already on the base**: its three commits are on main through #677 (`25e3dfe9`, `7449992a`, `0f2f33c4`) |
| engine/df-e24-vst3-routing | a4023a80 | itself | **superseded**: DF-E24 (a) landed as `engine/df-e24-vst3-aux-sinks`, (b, c) with #677 |
| engine/df-e27-clip-fades-and-end | b9773069 | itself | **superseded**: landed as `engine/df-e27-shortened-fades` and `engine/df-e27-source-end` |
| engine/df-e17a-move-overlap | 98ee01f8 | itself | **not finished, win's**: the director ruled DF-E17 (a) on 25 November (d9, `collab/win.md`): the overlap is kept while Delete Overlaps is off or the planner refuses, and "win finishes `engine/df-e17a-move-overlap` to this". Not taken unfinished |

Also unmerged and the lane's, not under `engine/`: `land/engine-1123` (d5163723), win's closed #723. Not
merged as a whole: it carries TM-SV-03/04 (carried by the session seat now) and two landed merges. Its
22 branches are taken one by one instead, with its resolutions as the reference.

Trial merges of the 28 tops onto the base (`git merge-tree`): clean for dev24-callback-load-meter,
mx33-sync-spikes, mx32-vst3-context-fixture, pd04-analysis-publication, srf04-controller-nudge,
au08-clear-clip-envelope, tm-wt03-beats-zero-crossing; one to thirteen conflicted files for the rest,
the most in dev06-rack-macros (13), tm-fc01-bounce-mixer (6), dev18-event-transforms (6).

## Step 2 — the chain

The order, oldest stack first (by each stack's first commit, all of 22 November). The racks and
macros stack (DEV-06, the row of reserved minor 1.24) is first; no branch here is DEV-07's (1.25).

1. `engine/dev06-rack-macros` 0a2e3eb9
2. `engine/dev25-plugin-readiness` d8995f97
3. `engine/dev18-event-transforms` 4e749553
4. `engine/dev13-node-segment-quantum` 59453a16
5. `engine/dev28-main-loudness-meter` f8308a55
6. `engine/tm-md04-wave-metadata` d59778d7
7. `engine/pd04-analysis-publication` da074f14
8. `engine/srf04-controller-nudge` 45e69a22
9. `engine/mx33-sync-spikes` 53fad1c3
10. `engine/tm-fc01-bounce-mixer` 8bc053ca
11. `engine/tm-ic04-otio-linear-writer` 2f414469
12. `engine/tm-ls03-midi-transport-edges` eb545c1e
13. `engine/pd14-project-templates` ca5cd357
14. `engine/ex02-via-main-routing` 99c36e2e
15. `engine/au08-simplify-envelope` be846d1b
16. `engine/au08-envelope-clipboard` 9ad32e5e
17. `engine/au08-clear-clip-envelope` 75ae50ef
18. `engine/mi04-step-scale-degrees` c3b86647
19. `engine/tm-wt02-interior-warp-markers` 6fa013a5
20. `engine/tm-wt03-beats-zero-crossing` c52e6ccd
21. `engine/dev24-track-processing-time` a16019c0
22. `engine/dev24-callback-load-meter` 72679a12
23. `engine/dev04-drum-pad-routing` b1b50df2
24. `engine/srf08-mcu-vpot-wire` 5f41d14d
25. `engine/tm-ic02-smf-codec` ec515123
26. `engine/mi05-seeded-note-timing` 4bc52ffc
27. `engine/tm-ic04-compound-file` 3ff8ba64
28. `engine/mx32-vst3-context-fixture` db99def9

The order taken: the stacks that merge clean or with one or two hunks first, so a first check could
start, then the ones that need hand work. Merge order does not change what the chain holds.

| # | Merged | Chain head after | Collisions and how they were resolved |
|---|---|---|---|
| 1 | engine/dev24-callback-load-meter 72679a12 (DEV-24) | 96c3e313 | clean |
| 2 | engine/mx33-sync-spikes 53fad1c3 (MX-33, four branches) | e16989d4 | clean |
| 3 | engine/mx32-vst3-context-fixture db99def9 (MX-32) | 83bfb9e9 | clean |
| 4 | engine/pd04-analysis-publication da074f14 (PD-04) | 755cb37e | clean |
| 5 | engine/srf04-controller-nudge 45e69a22 (SRF-03, SRF-04) | 9a3ba74c | clean |
| 6 | engine/au08-clear-clip-envelope 75ae50ef (AU-08 Clear) | 198f268b | clean |
| 7 | engine/tm-wt03-beats-zero-crossing c52e6ccd (TM-WT-03) | 7694de8b | clean |
| 8 | engine/dev13-node-segment-quantum 59453a16 (DEV-13) | e2e3d3c4 | `device_model.hpp`: main's three bypass overrides and the branch's `segmentQuantum` override, both kept |
| 9 | engine/dev28-main-loudness-meter f8308a55 (DEV-28) | 219a2032 | `main_loudness.cpp`: the branch replaces Main's private meter with the shared `dsp::LoudnessMeter`, as the seat wrote ("agree the landing order with empty-project-cost, whose old meter implementation is replaced here"). Taken whole; main's silent-block shortcut (empty-project-cost, in the replaced code) goes with it. Main's tests stay. **Check:** the factory-New callback cost may rise back toward its pre-#708 figure (DF item for the director below) |
| 10 | engine/tm-md04-wave-metadata d59778d7 (TM-MD-04) | d40e25e3 | `decode.hpp`: main's DF-E65 lock declaration and the branch's WAVE metadata reader, both kept |
| 11 | engine/tm-ic04-otio-linear-writer 2f414469 (TM-IC-01, TM-IC-04 OTIO) | 74b81625 | `CMakeLists.txt`: both sides' sources |
| 12 | engine/tm-ls03-midi-transport-edges eb545c1e (TM-LS-03) | 80c11a34 | `CMakeLists.txt`: both sides' sources and tests |
| 13 | engine/tm-ic04-compound-file 3ff8ba64 (TM-IC-04 CFB) | 21d3d66f | `CMakeLists.txt`: both |
| 14 | engine/mi04-step-scale-degrees c3b86647 (MI-06, MI-04) | 8cf04b83 | `test_note_diff.cpp`: the case table is the union, 14 + 3 = 17 (as #723 did) |
| 15 | engine/mi05-seeded-note-timing 4bc52ffc (MI-05) | a1d09946 | `CMakeLists.txt` both sources; `test_note_diff.cpp` both includes, both test functions, the table 18 |
| 16 | engine/tm-wt02-interior-warp-markers 6fa013a5 (TM-WT-02) | 161e46a7 | `warp_map.cpp` includes; `test_warp.cpp` and `test_warp_playback.cpp`: main's `main()` (ADI_HOME of its own, the speed-only mode) with the branch's `markerEditing()` and `editedInteriorMarkerPlayback()` calls added |
| 17 | engine/dev04-drum-pad-routing b1b50df2 (DEV-02, DEV-04) | 6d6b49ba | `test_device_ops.cpp`: main's VCA insert test and the chain-zone and drum-pad tests, all kept |
| 18 | engine/srf08-mcu-vpot-wire 5f41d14d (SRF-08) | cae5f0b5 | `CMakeLists.txt`: both |
| 19 | engine/dev25-plugin-readiness d8995f97 (DEV-25) | 8de854f6 | `scanner_session_reopen.hpp`: main's loop spelling; `test_vst3_key.cpp`: main already loads the fixture's `adiFixtureCompletedProcessCalls`, so the branch's readiness checks call it under main's name |
| 20 | engine/ex02-via-main-routing 99c36e2e (EX-02) | c5574cd4 | `realize.cpp`: EX-02's `connectOutput` (an optional silencing gate for a track outside the isolated mix) now ends in main's `connectTrackRoute`, so every Main route keeps its source track with or without the gate; `session.*`: main's MIDI hardware output node and the branch's `hearingFlags`, both kept; `test_session.cpp`: both sides' calls |
| 21 | engine/dev24-track-processing-time a16019c0 (DEV-19 x2, DEV-24) | 9d87e58d | As win resolved the same in #723: `Graph::addNode(node, track, fixedLatency)`, the track first so a whole number cannot bind to the latency; realize's `add()` passes both; `tests/test_retap.cpp`'s `addNode(fixed, 0)` became `addNode(fixed, std::nullopt, 0)` (it meant a latency, and would have compiled as a track). `midi_clips.cpp` plays through the branch's `projectMidiClipPlacement`, with main's exact `storedNanosecondsToTicks` for linear clips (DF-E63) and main's `secondsAt` and monitor ranges on top; the local `tickLimit` that would hide the file-scope one removed. `graph.cpp`: main's arrival for every compensated node, the scale timing added to the device-clock condition, main's quantum loop kept. `session.cpp`: the two scale problems join main's `collectProblems` as reports, not export blockers |
| 22 | engine/dev18-event-transforms 4e749553 (DEV-17, DEV-18) | 51910ac1 | Main's newer code moved onto DEV-17's shared event-source prefix: MIDI clip controllers and tracks take `allocateEventSourcePrefix` (the old controller allocator and track counter go), main's live-input voices and Memory preview notes take `eventIdentity(prefix_, serial)` in place of the removed `kLiveNoteIdPrefix`. `graph.cpp`: a transforming node keeps the bypass it sampled for its transform, every other node main's per-block bypass. Tests: DEV-13's quantum test and DEV-18's three transform tests, all kept |

| 23 | origin/main (BASE from round 2) | cd7eb8bb | clean (main had only documents beyond the round-1 base) |
| 24 | engine/dev06-rack-macros 0a2e3eb9 (DEV-01, DEV-03, DEV-06; round 2) | 6e115393 | 17 files, 46 hunks against the chain. **Session**: the rack layout (`deviceLayout_`) is read with the rows in main's `readPlayback`, which load and refresh both use now; the staged load (`PendingLoad`) captures the rack-control basis in `finishLoad`; `resolveOne` keeps main's preloaded instance (SC-318) with the branch's layout check. **MIDI hardware output** (main): `graphTrackChain(track, timing, layout*)` and `trackEventPath(track, layout, toHardware)`; a track's notes reach its output before the first top-level stage that stops them, a rack expanded. **Live MIDI input and Memory preview** play every instrument the track's notes reach, inside racks too (main took only the first instrument of a flat chain; for a flat chain this is the same). **Device automation**: one `bindDeviceAutomation(…, steps, activatorAvailable, model, resolve)`; main's `bindResolvedDeviceAutomation` binds macros too, resolving targets through main's stricter `resolvedAutomationDevice`; the branch's `bindPlaybackDevices` is gone; a stepped lane keeps main's hold-and-snap, a macro lane the branch's sum; a parameter a macro maps is not played by its own lane (the Device Activator lane excepted). **Graph**: one latency declaration per node through `Slot::latencyReport()` (main's fixed latency counts as known); main's `fixedDirectFeed` kept. **Realize**: every node a rack places is the track's (`add(node, track)`, DEV-24). **Device paste**: one `preparePaste(copies, requestedIds, …)` holds main's block paste (roles, one instrument, native functions once per channel, one sealed packet) and the branch's rack paste (fragment remap, remark guard); empty `requestedIds` allocates (the branch's `…WithFreshId`); a fragment never takes another pasted root's ID; a staged chain skips every fragment chain ID; a rack's inner native functions join the once-per-channel check. **device.insert with contents**: the branch's placement check, then main's VCA refusal (DF-E55 a) on the restored chain. Tests: both sides' everywhere; the automation test probe answers the test's own declarations first, then the branch's two mapped parameters |

### Not merged yet (the next session's work)

Each of these collides with code the base rewrote after 22 November. Each needs a port of the branch's
behaviour onto the new code, with both sides' tests, not a choice between hunks. Tried and aborted in
round 1; nothing of them is on the chain.

| Stack top | Conflicts on the base | What the port is |
|---|---|---|
| engine/au08-simplify-envelope be846d1b (AU-02, AU-03 x3, AU-08 Simplify) | 3 files, `automation_splice_store.cpp` 9 hunks | Main's splice store gained row guards, a selection and a label argument (SC-351 adapted this branch's Simplify fitter on 27 November, so AU-08 Simplify itself is on the base by another route; its tests here should still be kept beside main's). Port AU-02/AU-03's `EnvelopeSpliceRequest` (data id, label, actor detail, sub-sample flag) onto main's `prepareEnvelopeSplice(…, label, selection)`; merge AU-03 by its top `engine/au03-guarded-arrangement-passes` if Simplify's own hunks are dropped as carried |
| engine/au08-envelope-clipboard 9ad32e5e (AU-08 Copy/Paste) | 5 files | Stacked on AU-02: after the port above |
| engine/tm-fc01-bounce-mixer 8bc053ca (TM-FC-01) | 6 files, `bounce_commands.cpp` 14 hunks | Main's bounce commands were reworked by the basics sessions; port range mute, insert removal and the editable sibling's mixer onto them |
| engine/pd14-project-templates ca5cd357 (PD-14) | 4 files | Main's Save a Copy gained `StoreCopyPolicy` (share, with a fresh Undo baseline and identity), session copies and managed backups. A template is close to a shared copy: port `saveTemplate`/`createFromTemplate` onto `saveCopyImpl(path, collision, policy, stop, kind)` with a template kind, and `initializeStoredOrigin` onto main's `seedShareBaseline` |

## Step 3 — the proof

| Check | Chain head | Result |
|---|---|---|
| core | c5574cd4 | build failed: three merge joins left two functions under one closing brace (test_device_ops, test_note_diff; test_graph had the same); EX-02's `RenderSilence` filled float with an int zero (MSVC C4244 under /WX, the seat's own, fixed plainly). Fixed in e525ec05 |
| core | e525ec05 | build failed: `tests/test_midi_clips.cpp` (main's audition test) used `pluginNoteId`, which DEV-17 removed. It now compares the whole runtime IDs (b9716587) |
| core | **b9716587** | **build OK** (MSVC Debug, /WX); ctest -LE performance: 294 of 300 pass. Failing: `adi_clap_tests` (3: "CLAP volume uses the authored linear amplitude above unity", "the switch sends all 128 original releases before all 16 new-wire attacks", "the next segment first sends the pending release on its original wire, then the new attack"), `adi_link_playback_tests` (10, the native performance generation and restarted Warp tempo checks), `adi_midi_capture_tests` (3: "stored ANOT retains original full uint64 identities…", "native Capture validates binary records, addresses, IDs…"), `adi_scan_cache_tests` (1, cancellation reaps the child), `adi_library_tests` (timeout), `adi_ui_document_tests` (1, "opening yields to the owner between slow plug-in loads") |
| the six alone, on the base | 16064c0a (base) | only `adi_ui_document_tests` fails, the same check: **on the base already, not the chain's**. scan_cache and library pass alone on the base; on the chain they ran in the full parallel run, where the library timeout is a known parallel-only one (win's wincheck). They need a run alone on the chain before they are counted as the chain's |

**Round 2, read before any build** (the queue held the build place):
- `adi_clap_tests` (3) and `adi_midi_capture_tests` (3): **the merge's, fixed in `7f09d582`**, tests
  only. Main's MI-08 volume check wanted `note_id==7`, DF-E56's two switch checks wanted the new CLAP
  attack's ID to be the source ID (1001 + key, 99), Capture wanted the first stored note ID to be
  `UINT64_MAX-1` and made a duplicate by writing 1. DEV-17 maps each source ID to an instance wire ID
  (`PluginNoteIds`, from 1) and gives each source its own prefix (`eventIdentity(prefix, serial)`). The
  checks now compare with the note-on's own wire ID / a fresh positive wire ID / the source's first
  note-on identity / the first record's ID. What each check proves is unchanged.
- `adi_link_playback_tests` (10): not read to a cause yet; the labels are needed (`slot.sh grep 15
  core.ctest 'FAIL' 1` after a run that includes it). Candidates: `Session::rebuild`'s DEV-19
  `compileTrackScaleTimeline(playbackModel_, timing->tempo, …)` during `setExternalTempo`'s refresh,
  or the DEV-24 `addNode(node, track, latency)` argument order in a caller the merge did not see.
- **Round-2 risks to watch in the first check after DEV-06**: `adi_param_automation_tests` (the probe
  now declares two more parameters for main's tests), `adi_device_copy_tests` (paste unified),
  `adi_session_tests`, `adi_device_ops_tests`, `adi_midi_live_*`/preview tests (every rack instrument
  now monitored). Linux/macOS: the branch compares json with string literals in `device_copy.cpp`
  (`entry.at("table")=="devices"`); the project's rule wants `get_ref<const std::string&>()`; MSVC
  accepts both; CI will say.

**What the next session does in step 3** (in this order):
1. Run the three real failures alone on the chain: `slot.sh run 15 origin/held/engine-chain core '^(adi_clap_tests|adi_link_playback_tests|adi_midi_capture_tests|adi_scan_cache_tests|adi_library_tests)$'`, and read the lines with `slot.sh grep 15 core.ctest 'FAIL' 2`.
2. Likely causes, to confirm by reading: clap and midi_capture touch note identity and wire IDs, which DEV-17 changed (shared event-source prefix, per-instance wire maps) while main's MI-08 units, DF-E56 release queue and Capture's full-ID records landed after it was written; link_playback's performance generation is touched by the graph's device clock (DEV-13 quantum, DEV-19 scale timing) or the merged `addNode` arguments. A merge mistake is fixed on the chain; a fault of a seat's branch that its own branch already had is logged with the test name and left for the seat.
3. Then `all` (core and the window suites).

## Step 4 — the account

**What the chain adds to ADI, in plain words** (one line per thing a user would notice; most are the
engine half, waiting for a window binding before a user can reach them):
- Racks' Drum Rack pads: a played key reaches the pad's chains and the "All Notes" chains in order, with choke groups tagged (DEV-04); chain zones (key, velocity, chain select) with their fades are read and obeyed (DEV-02).
- MIDI effects can change notes before the instrument hears them, and notes from clips, live input and MPE never mix up their identities, also inside plug-ins (DEV-17, DEV-18).
- A device can see the song's key and scale, and a clip's own scale on its track (DEV-19).
- The CPU meter: the real callback load against its deadline, and the time each track takes (DEV-24).
- A plug-in reports that it really loaded, took its state and processed audio (DEV-25).
- Pd devices get whole blocks of their own size, so a Pd patch hears exact ticks (DEV-13).
- Main's loudness readout (momentary, short-term, integrated, true peak, range) from the one shared meter (DEV-28).
- Fit to Scale, Invert pitches around an axis, and transpose by scale degrees, on selected notes (MI-06, MI-04).
- Humanize's engine: seeded random timing moves of selected notes (MI-05).
- Warp: insert, move and remove a warp marker inside a clip without disturbing the rest; smoother (four-point) warped playback; Beats loops that cross at a zero crossing (TM-WT-02, TM-WT-03).
- Clear Envelope: clear one clip's envelope, or delete a parameter's automation everywhere (AU-08).
- Export "Via Main": render one track or group through Main's effects, alone (EX-02).
- WAVE files' embedded tempo, root key, loops and text read on import (TM-MD-04).
- Analysis cache files written safely, never half-written (PD-04).
- Controllers: relative encoders of every format, keyboard keys mapped to parameters, nudges that never lose a tick, Pick-Up takeover (SRF-03, SRF-04).
- Mackie Control and HUI surfaces: faders with motor feedback, V-Pots and their rings, the LCD (SRF-08, the wire half).
- MIDI clock in: follow an external clock's tempo and Start/Stop/Song Position (TM-LS-03, the engine half).
- Exact time conversion and SMPTE timecode, drop-frame included (TM-IC-01); OpenTimelineIO read and write of a linear timeline, and the file container AAF needs (TM-IC-04, the codec half).
- Measurement tools for the latency rig: loopback latency, offline nulls, sync spikes (MX-33, MX-09); a test plug-in that records the track name and color a host sends (MX-32).

**What it leaves out, and why:** the five ports under step 2 (racks and macros, the AU-02/AU-03
automation passes and AU-08 Copy/Paste, Bounce's editable sibling, project templates); TM-SV-01/03/04
and TM-TT-01 (another seat carries them); DF-E17 (a) (win finishes it to your ruling); the superseded
and already-landed branches of the survey.
