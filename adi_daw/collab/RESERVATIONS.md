# Reservations: the numbers in flight

Win keeps this file (ADR-0248 d5). A seat reserves here before it numbers a schema minor, an ADR or a
register row: it asks in its log, win writes the row on main, and only then does the number go into a
branch. A PLAN row lent to a seat other than its owner is reserved here the same way (ADR-0252 d4).
On 19 November two seats that numbered alone both took schema 1.22, and four pull requests wrote the
same "212 ops" headline.

**The rule.**
- Ask in your log ("Reserve: schema minor for …", "Reserve: two DF-E rows for …"). Win writes the row
  here on main within the day and answers in `collab/win.md`.
- Use the number only after its row is on main. A number given up is marked `burned` and never reused.
- When the work lands, win marks the row `landed` with the pull request.
- Whoever lands second recounts what is counted (OPS.md's headline, the README's entry count) against
  current main.

## Schema minors (`src/adi/store.hpp`, `kSchemaMinor`)

Main is at 1.23. PLAN PD-10's register of pending minors is this table. Minors land in numeric order with no gap: check 9 of `tools/validate_schema.py` needs a frozen `docs/format/history/schema-1.N.sql` for every lower minor, so a minor is never burned. If work lands before a lower reserved minor, it takes that number, and the rows it passes move up by one. Win records the move here and in `collab/win.md`.

| Minor | Seat | What | Pull request | Status |
|---|---|---|---|---|
| 1.22 | mixview | the device key taps, `routing_key_taps` (DEV-05, MX-24, DF-M11) | #651 (#674 carried the same) | landed (#651) |
| 1.23 | engine | the send taps, `routing_send_taps` (DF-E22, ADR-0245 d10) | #677 (#665 closed, superseded) | landed (#677) |
| 1.24 | engine | the rack macro side tables: the saved chain selector and its settings, macro settings, enabled mappings, mapping breakpoint curves and authored target caps, keyed by (device, parameter id) (DEV-06, ADR-0060 d3-d5, ADR-0203 d15) | none yet: `engine/dev06-rack-macros` plays the macros without it | reserved; lands after DEV-03 and 3 December (ADR-0249 d3); the saved selector waits for DEV-02 |
| 1.25 | engine | the modulation route side tables (source, target device and parameter, depth, curve points, mode, bipolar, enable), persisted and undoable; outputs never persist (DEV-07, ADR-0230 d4, XC-16) | none yet: engine queue item 12 | reserved; lands after DEV-06 (ADR-0249 d3) |
| 1.26 | the session seat (engine, lent) | Session View's data: `scenes`, `clip_slots`, `clip_launch` and the essential extension `org.adidaw.session` (TM-SV-02, ADR-0101 d2, d4); slot clips stay out of the Arrangement's accessor and its SQL readers | none yet: `seats/session-chain` | reserved 27 November |
| 1.27 | the pitch seat (engine, lent) | the Pitch Editor's clip-owned edit rows: stable note identities against the source's hash and the analysis version, authored edits in the project because ADIA is evictable (`docs/CLIP-VIEW-ADDITIONS.md` C1; plan row TM-PE-01, owed under DF-R3); with them, in the same minor, the marker `clip_pitch_extractions` owned by an extraction's output clip, which outlives the audio source | none yet: `seats/pitch-chain` | reserved 27 November |
| 1.28 | the basics routing session (engine, lent) | SC-404, a MIDI track's own output: one optional row per track holding the endpoint's identifier, its saved name and a MIDI channel 1 to 16; it goes with the track on delete, Undo and duplicate | none yet: the routing chain | reserved 27 November |
| 1.29 | — | the next minor, by reservation here | — | free |

**The order of the minors.** The file on main is at 1.23. Minors 1.24 and 1.25 belong to engine rows whose branches have not landed (DEV-06, DEV-07). A later minor is staged under its number and switched on only when every minor below it is on main: 1.26, 1.27 and 1.28 wait for 1.24 and 1.25, which win lands or renumbers first.

Known to want a minor and not yet reserved: DF-W6, the `tracks.role` column (win); chain volume and pan, a side table (DF-E73, ADR-0253 d8); AU-06's Arrangement clip-envelope modulation streams (ADR-0230 d4, M6); engine-win queue item 8, only if the device ref must store the VST3 class ID. From ADR-0254, each reserved when its row starts: MX-39's per-device gain-match flag and fixed gain; TM-FC-05's lane kind that plays along with the main lane; AGT-12, only if a request's scope needs a new object beside `agent_requests`.

## ADR numbers

The reservation table stays in `collab/README.md` ("Reserved ADR numbers"), where check 8 of
`tools/validate_schema.py` reads it. #210, win's earlier fix that makes check 8 catch stray and missing
rows, is kept open and folded into this work.

Next free: **ADR-0270**.

## Register ids (`docs/DEFERRED.md`)

| Prefix | Section | Who numbers in it | Last used | Next free |
|---|---|---|---|---|
| DF-C | Control | the control seat, through win | DF-C9 | DF-C10 |
| DF-M | Mix View | the Mix View seat, through win | DF-M79 | DF-M80 |
| DF-W | Window | the window lane and win | DF-W65 | DF-W66 |
| DF-E | Engine | the engine seat, engine-win and the devices lane; the row's owner column says which | DF-E92 | DF-E93 |
| DF-R | Repository | win | DF-R3 | DF-R4 |

Three seats share DF-E, so each DF-E id is reserved here first.

## Op names (`docs/OPS.md`)

Claimed by open pull requests, or reserved for a named PLAN row before its pull request opens. A landed op keeps its row, marked.

| Op | Pull request(s) | Seat |
|---|---|---|
| `routing.setKeyTap` | #651 (landed), #674 (landed, carried the same) | mixview |
| `routing.setSendTap` | #677 (landed); #665 closed | engine |
| `lane.restore` | #662 (landed) | engine-win |
| `noteExpression.write` | none yet: MI-08, engine queue item 19; inside `note.applyDiff`, exact Undo over the `note_expression` AEXP rows; DF-W12's gain (3) and pan (4) are existing dimensions | engine |
| `scene.create`, `scene.delete`, `scene.reorder`, `scene.rename`, `scene.setTempo`, `session.setSlotClip`, `session.clearSlot`, `session.setLaunchMode`, `session.setLaunchQuant`, `session.setFollowAction` (edits); `session.launchClip`, `session.launchScene`, `session.stopTrack`, `session.stopAll` (ephemeral); `transport.setLaunchQuantization` (TM-SV-01) and `capture.sessionPerformance` (TM-SV-04) | none yet: `seats/session-chain`. Further names (a scene's signature and enable, the Stop button, clip settings, a scene's cancel, slot record) keep the `scene.` and `session.` prefixes and are listed in the seat's report as they are added | the session seat |
| `view.applyFilter`, `view.clearFilter` | none yet: `control/agent-chain`. The agent's view operations of ADR-0257 d6, through one `view_filter` tool at Propose or Apply: they change what the session shows (track order, clip highlighting), are written to the agent's log with actor and source, and touch neither the Store, routing, Undo nor the user's saved filters; no schema minor. The plan rows for ADR-0257 d4 to d6 are owed under DF-R2 | the agent seat |
| `clip.setPitchEdits` | none yet: `seats/pitch-chain`; one batch replaces a clip's edit rows and captures the previous rows for Undo | the pitch seat |
| `clip.splitPitchSegment`, `clip.mergePitchSegments` | none yet: `seats/pitch-chain`; each carries the resolved source-bound edit image, so Undo, Redo and replay use captured bytes | the pitch seat |
| `scene.setSignature`, `scene.setEnabled`, `session.setStopButton`, `session.setLaunchSettings`, `session.cancelSceneLaunch`, `session.backToArrangement`, `session.setFollowActionsEnabled` | none yet: `seats/session-chain` | the session seat |
| `noteExpression.clear` | none yet: MI-08, engine queue item 19; inside `note.applyDiff`, exact Undo over the same rows | engine |

## PLAN rows on loan (ADR-0252)

A row of `docs/PLAN.md` taken by a seat that is not its owner. The owner lends the row's paths, named
below, for that row only, and reviews the branch before "PR requested". Each row's paths, test and order
are in the seat's queue (`collab/prompts/2026-11-22-gemini-queue.md` for gemini).

| Row | Owner | Seat | Paths lent | Lands | Status |
|---|---|---|---|---|---|
| SRF-09, OSC: the core | engine | gemini | `src/adi/osc_codec.*`, `src/adi/osc_namespace.*`, their tests and the fuzz harness `tests/fuzz_osc_codec.cpp`, their lines in `CMakeLists.txt` | after 3 December | reserved |
| SRF-10, OSCQuery v1: the core | engine | gemini | `src/adi/oscquery.*`, its tests and `tests/fuzz_oscquery.cpp`, their lines in `CMakeLists.txt`; stacked on SRF-09 | after 3 December | reserved |
| PD-15, paste between projects: the engine half | engine | gemini | `src/adi/project_paste.*`, `tests/test_project_paste.cpp`, its lines in `CMakeLists.txt` | after 3 December | reserved |
| CO-01, the collaboration core | engine | gemini | `src/adi/collab/**` (new), `tests/test_collab_*.cpp`, their lines in `CMakeLists.txt`; its schema minor, if a slice needs one, reserved in the table above first | after 3 December | reserved |
| the rows of `docs/SANITY-CHECK.md` (fixes; ADR-0257 d2) | each row's owner seat | the six basics sessions (`collab/prompts/2026-11-27-basics-*.md`) | the files each queue names, for its own rows only | now, in win's stacks | lent 27 November, until the owner seat next runs |
| DF-E89, the Release warnings (a fix) | engine, engine-win and devices | gemini | the 54 units its sweep named, for those warnings only (`collab/prompts/2026-11-27-gemini-basics.md`) | now | lent 27 November |
| TM-WT-04, the stretcher that follows tempo | engine | the stretcher seat | new files for the streaming players and the real-time stretcher, their tests; small changes in the shared session and graph, named in its report | after the basics, as one bundle | lent 27 November by the director's word (`collab/prompts/2026-11-27-engine-seats.md`) |
| TM-SV-02, TM-SV-03, TM-SV-04, Session View's engine | engine | the session seat | new files for the tables, ops and launch engine, their tests; schema minor 1.26 | after the basics, as one bundle | lent 27 November |
| TM-PE-01, the Pitch Editor's engine (row owed, DF-R3); TM-WT-09's note finder | engine | the pitch seat | new files for the analysis, the edits and the render, their tests; schema minor 1.27; the optional ADIA section `PNOT` for a source's pitch notes | after the basics, as one bundle | lent 27 November |
| TM-MD-04, TM-MD-07, TM-WT-01 and TM-WT-05's analysis, and the query service of ADR-0261 d12 | engine | the library seat | the library's analysis worker, descriptors, classifier and query, their tests | after the basics, as one bundle | lent 27 November |
| DF-E92 and DEV-27's engine half, the plug-in server | engine, with win for `third_party/` | the server seat | `docs/ENGINE-PLUGIN-SERVER.md`, the latency probe, the client behind the device contract, their tests | after the basics, as one bundle | lent 27 November |
| N76 and DF-E82, the preview player (with SC-123) | engine | the basics browser session (`collab/prompts/2026-11-27-basics-browser.md`) | the preview source and its cue path, the Browser's preview row, their tests | now, in win's stacks | lent 27 November (ADR-0261 d14) |
| the analyser files (fixes; `collab/prompts/2026-11-27-basics-analysers.md`) | mixview | the basics analysers session | `src/juce/ui_analysis.*`, `src/juce/ui_mix_analysers.cpp`, `src/adi/ui/shared_analysis*`, the loudness meter under `src/adi/mix/`, their tests | now, in win's stacks | lent by the director's word of 27 November, until the Mix View seat next runs |
| the audio device and file rows (SC-376 to SC-391, SC-405, SC-428; `collab/prompts/2026-11-27-basics-device.md`) | each row's owner seat | the Windows 10 walk session | the files its queue names, for its own rows only | now, in win's stacks | lent 27 November |

| DF-M34, a device told each skipped segment's length (a fix, not a PLAN row) | engine | mixview | the device backend's interface in `src/juce/device_model.hpp` (one hook taking each skipped segment's exact frame count, beside `didSuspend`) and the graph's skip site that already calls `Node::silenceTaps` (`src/adi/engine/graph.*`), with a test in the engine's graph suite | before 3 December | lent by win 25 November, the engine seat being out; the engine reviews it when back |
| DF-E76, a symlinked VST3 bundle resolves to its module (a fix) | engine-win | window-fn | `src/juce/juce_device_loader.cpp` (the canonical module identity) and `tests/test_play_render.cpp` | when proved | lent by win 25 November (first to the Codex work seat, which is out until 29 November) |
| DF-E36's tests (code done) | devices | devices (its own row; no loan needed) | the Pd engine's tests (`tests/test_pd_*.cpp`) and their CMake lines | when proved | the devices lane takes it, 25 November |
| DF-E67 (b), the Pd canvas leak | devices | window-fn | the Pd tier's canvas load and close (`src/juce/pd_engine.cpp` and the vendored libpd file the row names, by a patch the vendoring rules allow) and the Pd suites | when proved | lent by win 25 November (first to the Codex work seat, which is out until 29 November) |
| DF-E12, the POSIX project lease | engine-win | window-fn | the project lease (`src/adi/project_lock*` or the file the row names) and its tests | when proved | lent by win 25 November (first to the Codex work seat, which is out until 29 November) |
| DF-E78, two unused test functions | engine-win | window-fn | `tests/test_render_queue.cpp` on `engine-win/export-profile-validation` only | with the export stack, after 3 December | lent by win 25 November (first to the Codex work seat, which is out until 29 November) |
| DF-E77, a sign conversion in a test | devices | devices (its own row; no loan needed) | `tests/test_vinyl_distortion.cpp` on `devices/vinyl-distortion` only | with the DEV-22 stack, after 3 December | the devices lane takes it, 25 November |
None of the four needs a schema minor, an op name or a register id today.

## The window's needs-table numbers (`collab/window.md`)

| N | What | Owner |
|---|---|---|
| N57 | the insert of a third-party plug-in (`preparePluginInsert`, `insertPlugin`), from the Mix View's 668e05c9 core with Live's placement rules | engine |
| N58 | one catalogue API off the message thread: a startup scan service, Rescan, a default worker count, each VST3's kind with the instrument flag | engine-win |
| N59 | a distinct scan state for "the scanner helper could not start" (`HelperCouldNotStart`) | engine-win; **met on main** (#694) |
| N60 | live MIDI input to the armed track's instrument, taking the window's per-port rings (`MidiPortEvent`) on the audio thread | engine's seam on main (#685, `Session::enqueueMidiInput`); the drain from the window's rings into it is the window's (window-fn) |
| N61 | a `midi.port.name` settings row, so a port's record heals by name when its identifier changes | engine-win; **met on main** (`settings/registry.cpp`); the healing by name is the window's |
| N62 | the render worker's contract for plug-ins (instances made and given state on the message thread, processing on the worker, `serviceHost` marshalled), so Export renders a project with plug-ins | engine-win (EX-11's engine half) |
| N63 | `notes::prepareBatch(snapshot, ops)`: one prepared `note.applyDiff` whose effects are the legacy note ops applied in turn, one undo step | engine (`note_diff*`, MI-01) |
| N64 | the snapshot's recording fields `TrackNode::recordArmed`, `monitorMode`, `inputRef` | engine (`engine/snapshot.*`) |
| N65 | the input-monitor node of MX-09, heard per `monitor_mode` | engine; **met on main** (#685, `Session::setAudioMonitorInputs`); wiring it is the window's |
| N66 | a failed capture's audio kept up to the failure (a locate or an overflow after minutes of good audio is lost whole today) | engine-win (recording, ADR-0248 d7) |
| N67 | Live's monitoring default for a new track: Auto, where New and `track.create` store 0 (Off) | **ruled by win (ADR-0203 d1): Auto, as Live**; the engine's `track.create` default and New's tracks |
| N68 | after a discovery job, a project open still probes a plug-in's saved path when the catalogue lacks it, as before any job (`managedDiscovery_`, `juce_device_loader.cpp` 450) | engine-win (N58) |
| N69 | a device parameter whose automation lane goes (its last point deleted, the lane deleted or disabled) returns to its stored value, as a mixer control does (`Session::rebuild`, `bindDeviceAutomation`; DF-W37) | engine |
| N70 | ~~a distinct `scan::State` for "the scanner helper could not start"~~ **withdrawn by win, 25 November: a duplicate of N59, which is met on main (#694, `HelperCouldNotStart`).** The window-fn seat reads that state in place of matching the probe's words (DF-W34 (b)) | — |
| N71 | `Session::refresh` re-reads the project's identity (`recordingProject_`), so recording after a Save As keeps its take (PD06-SAVEAS-GUARD) | engine (`engine/session.cpp`) |
| N72 | a dropped audio file clears what it lands on, as a move, Paste and Duplicate do (DF-W28): the import placed through the planner (`PlacementBatch::clear`), one prepared edit with the window's batch | engine (`material_commands.cpp`, `edit_plan*`) |
| N73 | a loop recording keeps the passes it has when it ends early (a tempo change, Tap Tempo, a loop edit, running out of passes) | engine (`engine/audio_recording.*`, `recording_commands.*`) |
| N74 | Live's undo for a loop recording: one step for the whole recording, or one per pass as `prepareLoopCommit` commits today. A reading of Live 12 first (the devices lane's ONE list); until it, the window follows the engine, a step per pass | engine and win (a ruling after the reading) |
| N75 | MX-07's registry half: `mixing.soloInFront`, `mixing.soloInFrontDb` and `mixing.soloBus` labelled "not built", and Live's Options rows `preFollowsSolo` and `keepAllReturns` (they change only on readings, AWAITING 5) | engine-win (`settings/registry.cpp`) |
| N76 | the engine's preview path: a source that exists with no project open (ADR-0104 d2), heard on Cue Out; the Browser's preview row calls it (MX-08, DF-E82, ADR-0254 d24); lent to the basics browser queue with SC-123 (ADR-0261 d14): one player, built once, with three callers (the Browser's row, the floating search, the map) | engine |

Next free: **N77**. The project key and scale take the next free one if they need an engine field. N69 to N75 were the window-fn seat's unnumbered needs, numbered by win on 24 November in the order of its needs table.
