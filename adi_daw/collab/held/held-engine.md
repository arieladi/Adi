# held-engine — the engine seat's unlanded work, gathered into one chain

**This chain lands only when the director says so.** Nobody merges `held/engine-chain` before his word.

- Chain: `held/engine-chain`. Log: this file, on `held/held-engine-log`. No pull request is open.
- Base: `origin/land/basics-1128c` while it is not on main; origin/main once it is. The base is merged into
  the chain before each push.
- Proof: the build PC's slot 15 (MSVC Debug, warnings as errors, `ctest -LE performance` with the
  acceptance suite; then the window suites and the app).
- A stranger continues from here: read "Where it stands" first, then the step you are on.

## Where it stands

Round 1 (28 November): step 1 (the survey) done, below. Step 2 (the chain) in progress.

## For the director

(Filled at step 4. The engine seat's own open questions stay in its log, `collab/engine.md` on
`engine/log`; the ones you already answered are in `collab/win.md`, d7 of the 25 November rulings.)

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
| **engine/tm-ic02-smf-codec** | **ec515123** | itself (TM-IC-02) | **to take** (was in #723). The base has its own `tests/test_smf.cpp` (an add/add collision): see step 2 |
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

| # | Merged | Chain head after | Collisions and how they were resolved |
|---|---|---|---|

## Step 3 — the proof

| Check | Chain head | Result |
|---|---|---|

## Step 4 — the account

(Filled when the proof is done.)
