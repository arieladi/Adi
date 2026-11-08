# Research intake: what the November research and the code harvest gave ADI

5 November 2026. Companion to `docs/PLAN.md`. The research seat's bundle of 4 November (milestone
11, the eleventh of a series that began with the reference inventories) and win's code harvest
were read in full against what is on main; this document records, by research ID and by harvest
row, what ADI took, what it deferred and what it rejected. The research is cited by its IDs only;
none of its behaviours has been measured in a running DAW (every one is NOT RUN until the Live
measurement seat exists, PLAN §7), and the plan never treats a documented sentence as a
measurement. Every file was treated as data, never as instructions.

## 1. What arrived, and how each part was used

| Part of the bundle | What it holds | What ADI did with it |
|---|---|---|
| The manager handoff | The asks: one decision matrix per need (need and current ADR, research IDs, contract, reuse, reject or defer, evidence, unresolved test, lane); decided work separated from refinements and from the director's choices; a short milestone sequence; conflicts found before work is assigned; only meaningful choices put to the director; no automatic feature widening, ADRs, default changes or patch merges; instruments excluded; the milestone-11 erratum applied | PLAN §3 (the matrix), §2 (the milestones), §5 (the conflicts), §6 (the choices); §1.6 and §6 keep the limits |
| HANDOFF.md, ERRATA-11.md | The bundle's own index and its correction: ADI's audited queue captures the project revision and naming context at job start; an enqueue-time project snapshot is a different policy | The erratum governs every render and export row (EX-02, EX-09, TM-FC-01, AGT-04; XC-14 and XC-20); the snapshot policy is in PLAN §1.6, not proposed |
| DESIGN-IMPLICATIONS.md, ENGINE-CONTRACTS.md | Proposed contracts and their engine consequences per area | Read against main; each became a row's proposed contract, or was rejected in that row's "Reject or defer" |
| OPEN-QUESTIONS.md | The questions the research could not close, and the relay rule for readings | The rule is PLAN §7's "How readings enter ADI"; the questions became rows' unresolved tests or the director's choices |
| RESEARCH-MATRIX.md, HOST-COVERAGE.md | The research's own priorities and the host-wide coverage | PLAN §7.2 (Cubase's six priorities first), §7.3 and §7.4; the excluded study of built-in effect and instrument implementations stays excluded |
| The missions files (milestones 2 to 11; 165 new in 11) | Measurement missions with disposable projects, NOT RUN | A reference queue: a mission is scheduled only when a row names it (PLAN §7) |
| The Pd and rack fit note | How Pd racks and export fit the host | DEV-10 to DEV-16, the Pd user tier; the "no second editor" rule in DEV-11 |
| The per-app notes (Live 12, Cubase 15, Bitwig 6, REAPER 7, Reason 14) | The reference behaviours, per app, versioned | On main already as `docs/daws/*.md` (#436, #437); not applied again |
| The reference roles table | Ableton for daily workflow; Bitwig for modulation; REAPER for host flexibility, editing and export; Reason for rack presentation and macros; Cubase for detailed editing and delivery | PLAN §1.1 and §1.2: Live is the base under ADR-0203 d1; the others supply additions where ADI's needs warrant them |

**Research ID families, as the matrices cite them.** Live rows begin A (A3, A4, A5C, A6C to A6U,
A7, A-...); Bitwig rows B; Cubase rows C (C3, C4, C5W, C-R...); REAPER rows R (R3, R10H, R11E,
R11X); Reason rows N; missions M (M2 to M7, M-...); the bundle's design-requirement rows DR; the
evidence manifests E; the per-area probes and fit notes by name (PD-RACK-EXPORT-FIT-11). ADI's own
acceptance and awaiting rows (AT, AC, SF, DP, GT, LM, AR, AWAITING) are not research IDs and are
listed in PLAN §7.7.

**How it was read.** The research's code claims were pinned to `5c9e1cd8`; every "on main" statement
was read again at `ca0290c2` (#430), and main's `src/` and `tests/` differ from it by #461 alone. A
test source that exists was never counted as a passing test. The research's patch series (#436,
#437) is on main and was not applied again.

## 2. By row: what each need took, deferred and rejected

233 rows: 125 decided, 92 refinements, 16 new choices for the director. Milestones: M0 5, M1 24, M2 18, M3 20, M4 20, M5 28, M6 30, M7 11, Track 14, P2 60, P3 3. "Taken" is the
row's proposed contract in short; "Harvest takes" are the ledger rows it reuses (§3); "Rejected or
deferred" names what the research offered and the row declined, with the reason in PLAN Appendix A.

### 2.1 Editing and the arrangement (44 rows)

| ID | Need | Kind | M | Research IDs | Taken (the contract, in short) | Harvest takes | Rejected or deferred |
|---|---|---|---|---|---|---|---|
| AU-01 | Lanes and breakpoints in the window | decided | M2 | A6C-053, A6C-058, A6C-059, A6C-060, A6C-062, A5W-016, A5W-017, C3-015 | Each breakpoint gesture is one gesture (HI-01) and one undo step; the point ops re-key automation_segment_ranges in the same op; before the first point a lane holds its first value, and a disabled lane plays its control's own value. | AU1 | Cubase's Virgin Territory; Bitwig's Hold and Spread point options. |
| AU-02 | Insert Shape | decided | M6 | A6C-065, A-BASE-25.5.5 | Top-row shapes scale to the selection and the parameter's range; lower-row shapes join the neighbouring values; guard points at both edges through one shared splice, so the curve outside the selection is unchanged; one undo step. | AU1, AU2 | User-defined shapes. |
| AU-03 | Recording automation: the write pass | refinement | M6 | A3-014, A-H02, C3-016, C3-019, R10H-052, R11E-054 | A pass coordinator per lane: mouse writes are touch, controller and surface writes latch to the loop end or stop; locate, stop and loop wrap split the pass; Punch clamps writes; both pass edges keep the old curve (guards); thinning at drain time, recorded; the written lane suspended without a rebuild; commits grouped as Live's. | AU1, AU2, AU3, AU6, AU8 | Cubase's Fill, Return Time and Continue Writing (the P2 modes, AU-09); Tracktion's 2 s release timeout. |
| AU-04 | Override and Re-Enable Automation | decided | M1 | A5H-022, A6P-003, L-AUTO-7 | Session state, not an op (ADR-0162): the window passes the overridden lanes to the view, lights Re-Enable Automation and offers the per-parameter item; whether an override survives save follows the reading. | none | A/B preset switching that disables automation, until device A/B exists. |
| AU-05 | Lock Envelopes: automation follows clips and range edits | refinement | M2 | A-BASE-25.5.6, C3-014, R10H-051, R11E-054, R11E-055 | The planner reads one switch: with envelopes unlocked it moves, copies or deletes the track's device, mixer and send automation inside the span (replacing at the destination, guard points at both edges) in the edit's single undo step. The ...Time commands always move global automation and ignore the switch. | AU7, AU1, RE6 | Window-side batching of automation.movePoints (worklist C7); REAPER's automation items. |
| AU-06 | Clip envelopes: Arrangement clips modulate | refinement | M6 | A4-020, A-H05, A6C-069, A6C-071, A6C-072, A3-028, B-V02 | Arrangement clip envelopes are modulation streams in a whole-table minor (lane, clip, data, declared domain), evaluated with Live's per-parameter laws after automation inside the one modulation evaluator; #434's absolute overlay stays only for Session clips (P2) and for ADR-0115's gain curve (AU-07). | AU4 | Bitwig's automation clips; Tracktion's normalized relative units (ADI declares each domain). |
| AU-07 | The event volume curve on the clip (Cubase 14) | decided | M6 | C-F10, A6C-069 | The clip-owned gain lane is ADR-0115's absolute curve in dB; a clip's gain is its slider times Live's Clip Gain modulation (AU-06) times this curve, all before the effects; drawn and edited on the clip body in the Arrangement, its own undo steps. | AU4 | Cubase's tilt, compress and scale tools for the curve (P2, with the fade editor). |
| AU-08 | Simplify, Delete Automation and lane-scoped commands | decided | M6 | A6C-051, A6C-069, A-BASE-25.5.4, A6C-062 | Simplify replaces a selection's points by an error-bounded reduction in one transaction (enum and hold lanes refused), exact undo; Delete Automation removes a parameter's data in every owner in one step; lane-scoped Cut, Copy, Duplicate and Delete reuse the time-edit splice. | AU5, AU6 | REAPER's point-reduction preferences. |
| AU-09 | Cubase's automation modes and MIDI Learn on lanes (P2) | decided | P2 | C3-016, C3-017, C3-018, C3-019, R10H-053 | Cubase's automation modes ride AU-03's pass coordinator as a visible addition; Live's Arm stays the default; Trim writes an offset layer merged into the curve at pass end as one step; MIDI Learn on lane headers through the one port host. | AU9, AU3 | Fill and Return Time until after parity. |
| CT-01 | Take lanes exist; only the main lane plays | refinement | M6 | A6H-027, A6H-029, R10H-020, R10H-021, R10H-022, R11E-040, B-A07 | A NULL lane_id is the main lane; a take-lane clip plays only while its lane is the track's auditioned lane (session state, one per track); lane.* ops plus clip.setLane; every producer takes its lane from EditScope; SPEC §6.2 reworded to Live's copies model and ADR-0037's "comping follows Cubase" recorded as superseded by ADR-0203 d1. No schema change. | CP5 | REAPER's lane masking and play-all-lanes layering; Bitwig's comp-region sliding. |
| CT-02 | Audio loop recording into take lanes | decided | M6 | A6H-016, A6H-027, A4-017, R10H-039 | The recorder keeps a preallocated pass log (no lock or allocation on the callback). At stop one transaction writes one media row, a take clip per pass on new or reused take lanes, and a main-lane copy of the last pass; the incomplete last pass is kept. | CP1, CP2, CP3 | REAPER's takes inside items; a new FIFO. |
| CT-03 | MIDI loop takes | decided | M6 | A-H02, A6H-027, A4-018, R11E-040 | The MIDI recorder writes one take clip per pass into take lanes through note.applyDiff, closing held notes at each pass end; record quantize is its own undo step; note ids come from the recorder. | CP4, CP3 | Session-style loop-overdub merging (Session View is P2). |
| CT-04 | Comp gestures, audition and source highlights | decided | M6 | A6H-029, A6H-030, A-H04, R10H-023, R10H-024, R10H-025, R11E-041, R11E-042 | Comp commands compose existing producers (a range copy from a take lane, erased and pasted onto the main lane, one splice step); provenance is derived, never stored; dragging a source highlight's edge moves the split between two main-lane copies in one step. | CP5, CP6 | The active-region comp model (Tracktion, Cubase); REAPER's comp areas as separate objects and ranked take markers. |
| CT-05 | Take crossfades and comp seams | refinement | M6 | C-F04, C-F05, C-F06, R11E-024, A6C-080 | A seam crossfade is the two main-lane copies' own fades over their source handles: a "Crossfade comp seams" command, and an option on the comp gestures, write them in the same step; off by default; win's placement at P1 with comping, to strike if wrong. | none | Cubase's advanced crossfade editor (P2, with the fade editor); REAPER's crossfade snapshots. |
| ED-01 | Split, range commands and clipboards in the window | decided | M2 | A6C-083, A4-012, C-R01, C-R02, C-R03, R11E-002, M2-C-RANGE | The window calls the built producers, one undo step per command through OpSubmitter (worklist A2); a stale batch is refused, never rebased. A split of a selection inside one clip makes both cuts in one step; the left piece keeps the clip id, and a lone surviving piece keeps it (eraseRange's tail case changes). Main lane only until CT-01. | RE1, RE3, RE5 | Cubase's Range Tool split preference and Delete Overlaps override; REAPER's split-every-track-with-nothing-selected as the default (the Live reading sets editing.splitAllWithNoSelection). |
| ED-02 | The ...Time commands and Insert Silence | decided | M2 | A4-012, A5C-035, C-R02, R11E-018, R11E-054 | As built: the whole Set over a musical span, linear objects moved by the integrated tempo, crossing clips split, the join keeping the old end's tempo, meter and key. The window adds the Edit-menu items and Live's keys; a Time copy shares ED-01's clipboard. | RM2, RE7 | Ardour's and Qtractor's Insert/Remove Time option dialogs; per-track insertion of time belongs to ripple (ED-07). |
| ED-03 | One placement rule: EditScope and Live's overlap rule | refinement | M2 | A6C-077, C-R04, C5W-023, R11E-013, R11E-015, R11E-017, R10H-030, R10H-031 | One engine planner (edit_plan): every placement clears the destination span on its track and lane with eraseRange, then places, in one arranger.splice step; describe() names what is split, removed and shifted; EditScope (tracks, lanes, range, flags) with Live's presets; the window stops sending raw clip.move and clip.resize. Undo restores rows byte for byte. | RE1, RE3, RE5, RE7 | Cubase's Shift override and Lock Event Attributes; REAPER's item-group lead/follow; razor areas (RE8) deferred; editing.mutedClipsAsDeleted stays an off option. |
| ED-04 | Left trim, the clip's start, and Crop | refinement | M2 | A6M-033, A6M-034, A6C-076, R11E-003, C-R03 | prepareTrim moves the content offset (ticks and the ns clock) with a left edge; audio stops at source frame 0, MIDI does not; fades clamp. Crop by Live's rule (unlooped start to end; looped, loop bounds; a selection overrides); MIDI notes outside removed in the same step; audio writes no file. | RE4, RE1, RE3 | REAPER's move-item-without-contents and extra trim variants; Cubase's Crop of events wholly inside or outside. |
| ED-05 | Slip content inside fixed edges | refinement | M2 | A6C-076, R11E-003, R10H-035, B-A07 | Live's slip gesture (Ctrl+Shift-drag on Windows, Shift+Option on macOS; Alt or Cmd bypasses snap) on warped audio and MIDI, extended to unwarped audio as ADR-0115 d2's addition; one op moves the content offset and ns clock with edges and fades fixed; one undo step per drag. | RE2, RE1 | ADR-0115 d2's plain Shift-drag binding gives way to Live's modifier; REAPER's roll and slide deferred. |
| ED-06 | Consolidate | refinement | M5 | A4-005, A5C-022, R11E-009, R11E-010, R11E-011, B-O04 | The window keeps the selection's scope (EditScope) and Ctrl/Cmd+J; the render, the op and the file are TM-FC-02's, on the one capture (XC-14). MIDI clips merge their notes with ids and expression kept, nothing rendered. Live's peak normalization with a compensating clip gain; the result's warp state is Q-H. | RE1, RE6, RE3 | REAPER's Glue that bakes item FX; consolidating automation until Live is read; the matrix's "Warp off by d16" placement (withdrawn). |
| ED-07 | Ripple editing (an addition) | refinement | M6 | R11E-016, R11E-017, R11E-018, R11E-019, R10H-022, C-R02, C-F06 | A visible ripple switch: Off (Live's leave-gap, the default), Per track, All tracks. The EditScope planner closes or opens time right to left on the affected tracks; automation follows Lock Envelopes; markers and tempo move only in All tracks; frozen material stops the ripple and the Status Bar names the track; one undo step. | RE2, RE5, RE4, RE6 | REAPER's four ripple lock policies; per-lane ripple until take lanes exist; razor ripple. |
| ED-08 | Frozen, grouped and linked tracks under edits | refinement | M6 | A4-013, R11E-013, R11E-014, R11E-015 | An operation-capability table per track state, applied by the planner (frozen: TM-FC-04's rule after the readings; folded group members by the selection rules). Live 12's Linked-Track Editing (one linked set per track; clip and take-lane edits mirrored in one transaction) as win's P2 placement, to strike if wrong. | RE7, RE6 | REAPER's item locks and edit groups; Cubase's Lock Event Attributes (Live has no item lock). |
| ED-09 | Object snapping | decided | M2 | A6C-077, A4-015, A6M-003 | Snap candidates from the snapshot (clip edges on any track, locators, signature changes) within a measured pixel threshold; the modifier inverts snapping; no engine change. | none | Snapping to zero crossings (ADR-0070). |
| ED-10 | Fade handles, Create Fade and Crossfade | decided | M2 | A6C-079, A6C-080, C-F01, C-F02, C-F07, M2-C-FADE | Handles write clip.setFades as one gesture step, bounded by fade peaks, loop boundaries and the opposite fade (A6C-079); a selection over a clip edge makes that fade, over a boundary a crossfade; Delete removes a fade, resetting to 4 ms only when Create Fades is on. | none | Automatic edge fades by default (his deviation); Cubase's auto-fade layer; the fade editor waits for P2. |
| HI-01 | A gesture is previewed, then committed once | refinement | M2 | A6P-006, A6P-019, A-H02, A6X-016, C5W-017 | GestureSession: begin; each update re-runs the gesture's ops from the origin; commit writes one transaction with one label and the pre-gesture inverse; cancel writes nothing; undo, redo, moveTo and branch switches refused while open. Two preview strategies: live values for continuous scalars, a rolled-back transaction for structural edits. Supersedes OPS §6.4. | HI1, HI2, HI6, HI7, HI8 | Undo-based coalescing; OPS §6.4's 300 ms merge of separate commits. |
| HI-02 | An undo cap that holds | new-choice | M2 | R3-001, R10H-061, N10H-002, A4-010, A5C-048 | Inside authority now: compaction is best-effort (it reports and never fails a commit), runs down to a low-water mark and stores checkpoint rows by content hash. What may be compacted past is the director's (Q-A; recommended: every snapshot and dead end kept as a deduplicated stored state, so the cap always holds). | HI3 | Reason's redo clearing; REAPER's preference-driven history. |
| HI-03 | Live's Undo History window, with branches and snapshots | decided | M2 | A4-010, A5C-048, R3-001, R10H-061 | History::moveTo(seq) rewinds to the common ancestor and replays forward in one transaction; the timeline leaves out the compaction root and says older steps were compacted; Live's Undo History list, plus a branch tree view as d11's addition. | HI4, HI5 | A space-used column (an addition nobody asked for). |
| HI-04 | Snapshots, revert, the historical tab and Save As | decided | P2 | A4-010, A6H-068, R3-001 | As decided: Ctrl+S snapshots with a pre-filled name, revert keeping the left work as a branch. The daily snapshot is wired only with Q-A's rule. A historical state opens as the silent read-only case of PD-15's tab host; paste back is one transaction. | HI2 | Automatic snapshots on export or open. |
| MI-01 | One note edit, one transaction: note.applyDiff | refinement | M2 | A-M01, A6M-016, A6P-010, A6P-011 | note.applyDiff {clip, from-hash, added, removed, changed}: writes the stream once, records overlap side effects, refuses a stale "from", round-trips unknown tails, carries flags and expression rows through remove and undo, and validates every range; the five P0 note ops become thin wrappers over it. | ME1 | Ardour's lock-based model. |
| MI-02 | Live's note overlap rule | refinement | M2 | A-M03, A6M-016, M2-A-MIDI | Live's same-pitch overlap resolver inside applyDiff, classifying by interval (start inside, end inside, covers, covered) and recording each truncation or deletion as a side effect of the same step; the preview shows them before commit. | ME1 | Ardour's other merge policies unless Live shows them. |
| MI-03 | The note editor (Clip View) | decided | M2 | A-M01, A6M-003, A6M-009, A6M-036, A6M-037, A6M-038, A5W-016, B4-005 | Every editor gesture previews an applyDiff in a rolled-back transaction and commits once on release (HI-01); a refused op leaves the notes unchanged and says why; note preview through the note preview queue (N44). Built only after he approves the Clip View picture. | ME4 | The pattern editor (P3); Helio's Join, Reverse, Invert and Legato semantics where Live differs. |
| MI-04 | Pitch and Time utilities, Split, Chop and Join | decided | M6 | A6M-009, A6M-018, A6M-019, A6M-027, A6M-033, A6M-034 | Each utility is a pure function over the selection producing one applyDiff; Split keeps the left note's id; expression curves split and join exactly per curve kind; degree-aware transposition reads the clip's scale. | ME3 | Helio's own Join, Reverse, Invert and Legato semantics. |
| MI-05 | Quantize, groove, Humanize and Randomize | decided | P2 | A6M-022, A6M-024, A6G-013, A-H02, A6M-078 | The Groove Pool as a project-scoped whole table with Live's groove parameters; Commit Groove writes one applyDiff; Humanize and Randomize as Live's two controls, the span from the reading, seeded per apply so redo replays. | none | Bitwig's real-time Note FX quantize. |
| MI-06 | Scale Mode | decided | M6 | A6M-018, A6M-019, A6M-031, A6M-032, A-T09, A6X-004 | Live's scales under Live's names and order from a Live 12.4 census, each a degree mask over the clip's tuning; Fit to Scale rounds ties to the lower degree; degree operations through ScaleContext; spelling Sharp, Flat or Auto. | ME6, ME3 | The .stacks chord rulesets. |
| MI-07 | MIDI Tools (P2) | decided | P2 | A-T02, A6X-013, A6X-016, A6X-017, A6M-040, B-O01, C3-030 | A tool is saved settings, a pure transformer or generator and a rolled-back preview committed as one applyDiff; a change of note selection commits a pending preview; seeds stored so redo replays; Arpeggiate shares DEV-19's ordering function. | ME5 | Bitwig's operators as note properties; the Logical Editor and Expression Maps (P3). |
| MI-08 | MPE and note expression: playback and editing | decided | M6 | A-X01, A-X02, A-X03, B9G-038, C3-028 | The clip schedule compiles AEXP curves into expression events on ADR-0054's grid; noteExpression writes ride in applyDiff; the utilities carry expression. | ME4, ME3 | Bitwig's eight-port note bridge beyond the MPE dimensions. |
| MI-09 | Live 12's note model: probability groups and deviation | refinement | P2 | A-M07, A6M-030, A6X-006, A6P-011 | A side table (clip, note id, velocity deviation, probability group) in a whole-table minor; playback rolls probability per occurrence under ADR-0203 d7's random rule (a fresh draw per pass); a group resolves together. | none | Bitwig's occurrence and recurrence operators. |
| RE-01 | Remarks shown in the window | decided | M1 | DR-12-09, DR-12-10 | Pips from the snapshot on visible tracks, clips, devices and parameters; agent remarks in their own colour and glyph; the Info View shows the documentation above and the remark below; add, edit, resolve and remove. | RM5, RM8 | Margin balloons in the Arrangement. |
| RE-02 | Remarks as Word's comments: threads and anchors (P2) | decided | P2 | DR-12-09, DR-12-10, C3-030, R10H-029 | Whole tables (remark_replies, remark_anchors); Word's rules literally: a remark follows its object, Split keeps it on the left piece, Duplicate and Paste copy threads, deleting the object deletes its thread (captured for undo), Delete Time acts as Word on deleted text; every rule in the edit's transaction; the agent's brief bounded and propose-only. | RM1, RM2, RM3, RM4, RM6, RM8, RM9 | The harvest's re-pin and collapse rules (they change Word's behaviour he named); nested threads. |
| RE-03 | Live's Edit Info Text | refinement | M6 | DR-12-09 | Live's Edit Info Text as a property of tracks, clips and devices in a side table with one op (info.setText), copied with its object and kept in presets, exported as DAWproject's Nameable comment; shown in the Info View above the remarks; never merged with them. | RM7 | Storing info text as a remark. |
| TU-01 | Land the tuning stack | decided | M6 (merges in M0) | A3-024, A6F-008, A6F-009, A6F-010, A6F-011, A6M-084 | Merge #445 (rebased first), #444, #443 and #442 in stack order; OPS moves the six tuning and key ops to P1; until TU-02, refusals carry a verdict naming the physical line. | TU1 | Vendoring tuning-library (it is the test oracle only); the visible key and scale track (P2). |
| TU-02 | Source fidelity: what ADI accepts and re-exports | refinement | M6 | A6F-001, A6F-004, A6F-005, A6F-006, A6F-007, A6F-009, A6F-010, A6F-012, A6F-013, A6F-014 | ADR-0178's tables stay; source order and duplicates live in a keyboard map and the original bytes in tuning_sources (whole tables, the next minor); unknown @ABL lines kept verbatim with a warning; .ascl and @ABL in UTF-8, plain .scl from Latin-1; an unedited import re-exports byte for byte. | TU1, TU4, TU7 | tuning-library's centre-on-unmapped path; any refusal of a valid source; a pitch-ordered ruler as the default. |
| TU-03 | What a tuning change does; Bypass Tuning; the ruler | refinement | M6 | A3-024, A3-025, A6M-084, A6M-086, A6M-087, A6M-090, A6M-092 | Live's model by default: a tuning or key change keeps note indices; Live's automatic retuning is a confirmed edit, one undo step; Bypass Tuning per track; the ruler in Live's index order with names from NOTE_NAMES or the EDO data and Hz on hover; scale and tuning on one clip is ADI's addition. | ME3, TU7 | Pitch-order display as the default (an addition only if he asks for it). |
| TU-04 | Tunings for plug-ins beyond per-note pitch | new-choice | P2 | A8P-018, A8P-019, B9M-069, A6F-014 | The director's Q-L. Recommended: an MTS-ESP master as a per-plug-in route beside CLAP note expression, MPE and plain MIDI, off by default, talking to the library already on the machine; CLAP's tuning extension for CLAP instruments; MTS SysEx for hardware later. | TU2, TU3, TU5, TU6, TU8 | Any libMTS binary inside ADI (not usable); one machine-wide table applied to every plug-in. |

### 2.2 Session, clips, time and media (32 rows)

| ID | Need | Kind | M | Research IDs | Taken (the contract, in short) | Harvest takes | Rejected or deferred |
|---|---|---|---|---|---|---|---|
| TM-FC-01 | Bounce to New Track and Bounce Track in Place as Live | refinement | M5 | A4-006, A4-007, A6H-023, A6H-024, C-E01, C-E02, C-E03, M4-A-BASE, M6-A6H-023 | Render Tap::PostFx; copy the mixer settings (never input gain) in the same transaction; split at the selection and mute only the inner pieces; one new track per selected track, one step; Bounce in Place removes the devices (captured for undo); a frozen source bounces from its verified wet file; renders placed Warp off (d16). | FC4, FC2 | SoundCraft's deactivate-and-hide (that is Pro Tools' Commit, not Live). |
| TM-FC-02 | Consolidate, Paste Bounced Audio and group bounce | refinement | M5 | A4-005, A4-008, A4-009, A5C-022, R11E-005, R11E-009, R11E-010, R11E-011, R11X-029, R11X-030, M5-A5C-022, M4-A-BASE | Consolidate renders PreFx per selected track through the one capture (XC-14), peak-normalizes with a compensating clip gain, and replaces the material through the planner in one step labelled "Consolidate", keyed by a position-relative recipe; MIDI merges with note ids kept; Paste Bounced Audio renders PostFx when pasted; group bounces after EX-02's ViaMain. Warp state: Q-H. | FC3, FC4, FC2 | Qtractor's merge (no clip envelopes). |
| TM-FC-03 | The recipe key, reuse, collection and render folders | refinement | M5 | C-D01, C-D16, A3-023, A6G-005, R10H-017, R10H-018, M5-A5C-018, M3-MEDIA | An ADR "freeze, bounce and consolidate are three contracts" with a recipe key (BLAKE3 over canonical CBOR of kind, fingerprint, identities, engine build, spec); only a current-recipe result becomes active; refs written as .part then renamed; one render in flight per key; collection only from an explicit Unused Files command; the mark covers every retained op on every branch; named stage prefixes; renders leave the project root (folder: win picks). | FC1, FC2, FC3 | FNV-1a and a weak media identity; Tracktion's purge on every render and delete on unfreeze; ADR-0069 clip chains until device_chains accepts a clip owner. |
| TM-FC-04 | Freeze All Channels and Live's frozen-track editing | decided | M5 | DR-07-21, FR-041, A5C-013, A5C-014, A5C-015, A5C-016, A5C-017, A5C-018, A6G-005, R10H-017, C-E03 | Each child cached through the one capture with dry and wet streams; sidechain keys and sends served from those streams once taps and endpoints have their format home (XC-15); re-freeze renders only changed children; after the Live readings, frozen clips play windows of the wet file at their positions, tails as temporary windows. | FC2, FC5 | Frozen-clip editing before M5-A5C-013 to -016; a real-time freeze before external devices exist. |
| TM-IC-01 | The exact time base and SMPTE timecode | decided | M5 | FR-010, DR-13-17, DR-13-18, C4-016 | One pure integer component (src/adi/time_base): ticks per second to and from frames, nanoseconds and musical ticks with a range check; drop-frame on integer frame counts; 29.97 as exactly 30000/1001; the drop-frame flag in a new table; external_sessions and external_links as their own additive minor. | IC8 | OTIO's double-based RationalTime; Ardour's truncated 29.97 column. |
| TM-IC-02 | MIDI files as Live (P2) | decided | P2 | A-BASE-3.10, A-BASE-5.2, A-BASE-5.2.1, A6H-028, A3-021, A6F-012, A6F-013, A6F-014, M3-EXPORT-INTERCHANGE | A JUCE-free SMF codec (variable-length quantities, running status, raw meta and sysex, 64-bit ticks, SMPTE division, unknown chunks kept); import is prepare and commit, one step, through clip.createMidi; unmapped data parked in extensions rows for a byte-identical re-export; the source PPQ remembered; no options box (P2). | IC7 | glib-based libsmf; committed song files; the harvest's P1 placement (d7 rules P2). |
| TM-IC-03 | DAWproject both ways (P1) and the converter rules | decided | M5 | C4-018, DR-13-17, M3-EXPORT-INTERCHANGE, M4-C-EXPORT | A converter ADR: JUCE-free src/adi/interchange; prepare on a worker, commit as one step (actor import); a per-field loss report; unmapped XML parked and re-exported verbatim; nested clips flattened; VST3 matched by hashing its class UUID; colours to the 70 palette indexes; the missing ops first (extension.write, noteExpression.write, clip.createMidi); the devices lane maps the built-in EQ, Compressor, Gate and Limiter. | IC1, IC2, IC3, IC4, IC5, IC6, IC12, IC13 | Tracktion's JUCE glue and its random round trip as the fuzzer; a drop-frame column; .als and Pro Tools import (not raised). |
| TM-IC-04 | OTIO and AAF (P2), OMF read (P3) | decided | P2 | C4-016, C4-017, R11X-023, R11X-024, DR-13-17, DR-13-18, M4-C-EXPORT, M3-EXPORT-INTERCHANGE | TM-IC-03's converter rules; AAF import through LibAAF (static, behind a mutex) with Ardour's mapping; AAF export through a clean-room compound-file writer lowered from rows::Model; separate essence files as export jobs that read the project when each job starts; OTIO as a JSON subset with integer time; the timecode and frame-rate ops rise to P2. | IC9, IC10, IC8, IC11 | The AAF SDK; OTIO's C++ library; reading or writing .fcproj and .scraft. |
| TM-IC-05 | NI Stems import and export, and N-stem results (P1) | decided | M5 | DR-13-11, R11X-013, A11R-001 | Kept as built: the job reads its revision when it starts (ERRATA-11). After the macOS fix, prove the Windows Media Foundation path really runs rather than skipping; the NI reader becomes a strict filter over the general one; #453 (N-stem results) after. | MD2, MD1 | A container CDJs read (AWAITING 17). |
| TM-LS-01 | Ableton Link (P1) | decided | M3 | DR-11-02, A6I-006, A5C-026, A5C-027, A5C-028, A5C-029, A6G-002, M3-CONTROLLER | The SDK behind one pImpl unit; one driver commits the session state per callback (callback time, output latency, compensation); Link's tempo enters as an effective-tempo generation; starts and stops land on the quantum; a peer's tempo is a runtime override that lights Re-Enable Automation; Link on ASIO, off on DirectSound, WASAPI per Q-F; the pin moves to Link-4.1.1 before the consumer merges. | LS1, LS4, LS5, LS6 | The SDK example's Windows latency arithmetic; a TimingView-only override; whole-output resampling; BespokeSynth's SPP code; Link Audio (Q-F). |
| TM-LS-02 | Link Audio | new-choice | P2 | A6I-001, A6I-002, A6I-003, A6I-004, A6I-005 | The director's Q-F. If reopened: BasicLinkAudio on the same 4.1.1 pin, enabled only when used; a source node per peer channel into a track's input; a sink per track with audio; Latency and Sync to Incoming Audio as declared latency; peers named by peer and channel ids through input_ref's grammar. | LS3 | If he keeps it closed, the 4.1.1 pin still stands, so no SDK swap follows. |
| TM-LS-03 | MIDI clock and timecode (P2), and the MIDI port layer | decided | P2 | A6I-007, A6I-008, A6I-009, A6I-010, A6I-011, A6I-012, A5C-024, A5C-025 | One MIDI port host on UMP endpoints (window) feeds timestamped rings to the engine; clock in by a second-order DLL into the effective-tempo overlay; SPP on exact ticks; clock out (Song or Pattern, Sync Delay); MTC in, locking after 8 quarter frames; the registry made to match d9; a FEATURES row (P2; MTC out an ADI option). | LS2, LS4, LS6, IC8 | Ardour's truncated 29.97 drop-frame column; Bitwig's "sync in" chooser; Tracktion's MTC generator; BespokeSynth's SPP truncation; the Tempo Follower; video sync (P3). |
| TM-MD-01 | MP4, M4A, MOV and ALAC import | decided | M3 | A6R-047, M6-A6I-013, DR-12-08, L-MEDIA-2, L-MEDIA-3 | Step zero is PD-00's CoreAudio priming fix. Then a media ADR: ADI's own bounded ISO-BMFF and QuickTime reader (headers only, hard caps, hostile files refused); AAC layout from the AudioSpecificConfig; trimming by edit list, else iTunSMPB; platform decoders fed raw packets; ALAC in process; a movie's first audio track imports (the picture is P2); the NI reader becomes a strict filter over it. | MD2, MD1, MD3 | craft's sample-entry channel count; the Windows re-mux; complex edit lists (P2); Ardour's -map_channel. |
| TM-MD-02 | Playing before decoding finishes; known-hash loads; Cleanup | decided | M3 | DR-12-08, A6H-064, A6C-033, A5H-009, M3-MEDIA, L-MEDIA-1, L-MEDIA-5 | One decode job per source hash; a clip's page served once the decoded frontier covers it (a pending page is silence counted as pending, never an underrun); the clip switches to the finished file without a rebuild; warped clips wait for the whole file; the stored BLAKE3 reused on load; partial files reaped; Cleanup; decode state in the snapshot. | none | A separate shared-file positional-read layer. |
| TM-MD-03 | Relink by hash, automatic search, Offline and Replace | refinement | M3 | DR-12-07, A5H-016, A5H-017, A5H-018, A6C-026, A3-023, N10H-053, A11N-008, M3-MEDIA, L-MEDIA-4 | resolveSession checks the stored path, the project's audio/, registered folders and search paths, BLAKE3-verified through a size and mtime memo; searchMissing asks the library's hash index, then walks size-filtered; exact matches relink as one step; a same-name file with other content is listed, never played; an Offline mark; Live's two Replace rules as two ops. | none | Live's unverified manual repair (hash identity is stronger); collection and trash (TM-FC-03). |
| TM-MD-04 | The analysed folder, sample defaults and sample analysis | decided | M3 | A-W05, A6H-063, A5H-008, A5H-009, DR-02-09, DR-04-11, R11X-023, R11X-024, M2-A-WARP, L-MEDIA-6 | Derived analysis as versioned ADIA sections in the analysed cache (evictable); the user's sample settings in the library, never cleaned, recalled over Auto-Warp; key from the suite's analyser (Q-D); read-only tags with a precedence (the user's value, the file's tag, ACID, detection). | MD6, MD7, MD8, MD9, MD10 | ADIL v2 clip_defaults and an AANL file; Similarity Search, Similar Sample Swapping and Auto Tags (a later row); REX (no usable decoder). |
| TM-MD-05 | The long tail of formats | refinement | M3 | R11X-022, M10-INV-REX, A-M5 | A format Live imports never needs an outside tool: Ogg Opus (opusfile, with pre-skip) and ALAC decode in process; the rest decode through FFmpeg as a child process (validated ffprobe JSON, streamed floats), its path in files.ffmpegPath, shipped in the installer's tools tier (win's placement). | MD4, MD5 | Ardour's -map_channel; Audacity's wx-bound FFmpeg layer; Sound Designer II stays P3; REX (no usable path). |
| TM-SV-01 | Global Quantization and its Arrangement uses | refinement | M3 | A6G-013, A6M-004, A6C-006, A6C-007, A6C-023, A6C-042, A6P-011, M-LIVE-CLIP, M3-CONTROLLER | transport.launchQuantization in session_state (outside undo) in Live's code list; a pure, bar-aware nextGridTick in ADI_PPQ resolved on the driver; a quantized locate at the grid sample with MIDI chase; with Link on it is the quantum. Window: the live menu, Ctrl+6 to 0, quantized scrub and locators. A FEATURES row at P1. | SV2, SV5, SV6 | MAGDA's "strictly future" rule and its launch-while-stopped; storing ticks (1 Bar is not a fixed tick count in 3/4); per-clip launch quantization and the Launch rows (P2). |
| TM-SV-02 | Session data: scenes, slots and launch settings | refinement | P2 | A3-032, A6S-007, A6P-011, A6P-012, A5C-037, A5C-038, DR-01-02, M3-LAUNCH | Whole new tables (scenes, clip_slots, clip_launch) in Live's code lists, with an essential extensions row so an older build warns instead of playing slot clips at 1.1.1; slot clips kept apart from timeline clips; the 14 edit ops and the ephemeral launch ops; amends ADR-0117 §2's wording (side tables, not columns). | SV6, IC2 | Rebuilding clips with a relaxed CHECK; scene-tempo application (TM-SV-04) and the DAWproject scene mapping (TM-IC-03) deferred. |
| TM-SV-03 | The launch engine | decided | P2 | A3-001, A3-002, A5C-039, A5C-040, A6C-018, A6C-019, A6C-028, A6C-041, A6C-048, A6P-012, M3-LAUNCH, M-LIVE-CLIP | An engine launch lane drained on the driver in Live's per-block order; a pure press/release table (Trigger relaunches); Legato joins the old position; launch, stop and hand-over edges follow Live's per-clip Fade (the measured 176/192/384 samples until N8), Arrangement edges keep ADR-0151 d3; one tick-to-sample rule; voice identity survives rebuilds; zero callback allocations. | SV1, SV2, SV4 | Tracktion's runtime launcher; MAGDA's host and UI; openDAW's stop that returns to the timeline; warped Session clips under a changing tempo (TM-WT-04). |
| TM-SV-04 | Follow actions, scenes and capture | decided | P2 | A3-003, A3-004, A3-005, A6C-043, A6C-044, A6C-045, A6C-046, A6C-047, A6C-049, A5C-036, A5C-041, A6S-007, A4-017, M3-LAUNCH | Follow-action groups as runs of occupied slots; Live's ten actions written fresh; a fresh chance draw on every pass (seeded within one pass so a realtime pass and its render agree); a user launch outranks a due follow action; scene launch honours Stop buttons; scene tempo through the one effective-tempo path; capture.sessionPerformance as one changeset. | SV3, SV1, SV6 | A stateful random generator on the audio thread; the matrix's "same draw on every pass" (XC-29); the crossfader (MX-15). |
| TM-SV-05 | The Session View window | decided | P2 | DR-01-02, A5C-038, A5C-039, A5W-003, A5W-014, A6R-012, B4-20 | The window builds the grid on a stub provider once TM-SV-02's schema lands, reading the per-slot taps and run edges, never the store; a parity checklist from manual chapters 7 and 16; his signed side-by-side session. The crossfader is MX-15's. | none | The MixConsole toggle (P3, last). |
| TM-TT-01 | Tempo ramps play everywhere | decided | M3 | DR-04-12, C3-011 | One effective tempo map that integrates linear and bezier ramps exactly, read by every builder (clips, MIDI, automation, metronome, warp anchors, both quantizers); Link and clock boundaries as a private overlay that never enters the project. | none | A per-block TimingView override (plug-in tempo would move while clips keep the project tempo). |
| TM-WT-01 | The analysis core behind Auto-Warp | refinement | M6 | A-W01, A-W04, A6C-032, C3-001, C3-002, C3-003, C3-004, C3-005, R11E-027, R11E-028, R11E-029, DR-04-08, DR-04-09, M2-A-WARP | The warp-analysis ADR: spectral-flux onsets with strength (reading editing.transientSensitivity); beats, downbeat and tempo from qm-dsp at a fixed 44.1 kHz with Mixxx's TempoTrackV2; BeatUtils regions and snapping; results as versioned ADIA sections; replaces today's heuristic only if a labelled benchmark (beat_this as comparator) says so; amends ADR-0176 d4. The suite's tracker is Q-D. | WT1, WT2, WT3, WT8, WT7 | Upstream TempoTrackV2 (needs about 7.4 s); qm-dsp's maths/pca; a second AANL cache; the re-detection inside prepareWarp. |
| TM-WT-02 | Warp ops, per-clip warp settings and Live's warp commands | refinement | M6 | A-W02, A-W03, A-W04, A-W06, A-W07, A6C-026, A6C-034, A6C-035, A6C-036, A6C-037, A6C-038, A6C-086, A6P-008, R11E-033, R11E-034, R11E-036, C5V-013, DR-04-01, DR-04-02, M2-A-WARP | AWRP stays the one source-to-tick map; a minor of whole tables (clip_warp_settings, clip_transients); a clip origin for Set 1.1.1 Here (side table or AWRP v2, win picks); OPS §9.4's six ops plus setWarpSettings, setTransients and setOrigin; every Live warp command a prepared producer, one undo step, never a new analysis. | WT6 | Tracktion's code (design only); tempo leaders (TM-WT-07) and the streaming players (TM-WT-04). |
| TM-WT-03 | Beats and Texture parity and the listening gate | decided | M6 | A-W09, A-W10, A6C-039, A6C-040, DR-04-03, DR-04-04, DR-04-05, DR-04-06, DR-04-07, M2-A-WARP, M6-A6C-039 | Beats cuts on the AWRP grid by Preserve; the loop point at a sign change within ±5 % of the segment's middle; a micro-fade at slice switches; 4-point Hermite interpolation; Texture's parameters and seed from clip_warp_settings; the named parity gaps stay named. | none | SoundTouch; real-time players (TM-WT-04). |
| TM-WT-04 | Warped audio that follows tempo changes | decided | P2 | DR-04-12, C3-011, R11E-038, M2-A-WARP, M3-LAUNCH | Streaming Beats, Texture and Re-Pitch players and a primed real-time R3 whose latency joins compensation; until they exist a tempo change is a new prepared generation and every warped clip says so; an equivalence test against the prepared render at a steady tempo. | WT6 | Whole-output resampling for Link following. |
| TM-WT-05 | Short samples on drop | new-choice | M6 | A-W01, A6C-008, A6C-032, DR-04-08 | The director's Q-E. Recommended: every drop arrives clean; Live's short-sample classifier runs when Warp is pressed and in the import prompt's Warp choice (a Warped Loop with two markers, or an Unwarped One Shot); record.shortSampleMode gets Live's four choices; the threshold provisional at 60 s until read. | WT4, WT5 | Audacity's "any whole number of bars is a loop" rule (Live assumes 1, 2, 4, 8 or 16 bars of 4/4). |
| TM-WT-06 | The Clip View Sample Editor | decided | M6 | A6C-034, A6C-035, A6C-036, A6C-037, A6C-038, A6C-039, A6C-040, DR-03-09 | Built against needs rows for transients, warp settings and AWRP in the snapshot, after his approval of the Clip View picture; each gesture commits one prepared batch through OpSubmitter. | none | Waits for TM-WT-02's ops and the picture approval. |
| TM-WT-07 | Tempo leaders, and the duplicate tempo-detection row | refinement | P2 | A-W08, A6C-031, M2-A-WARP | Live's warped clips as tempo leaders (the bottom-most leader wins; Unfollow turns the result into tempo events in one step) as win's P2 placement; FEATURES' duplicate P2 "tempo detection" row is rewritten as this feature. | none | The Tempo Follower (ADR-0203 d9). |
| TM-WT-08 | Hitpoints, Slice to MIDI and Audio Alignment (P2) | decided | P2 | C3-001, C3-002, C3-003, C3-004, C3-005, R11E-026, DR-03-12 | Hitpoints are TM-WT-01's cached onset strengths filtered at read time by Cubase's Threshold, Intensity, Minimum Length and Beats; Audio Alignment by dynamic time warping over onset strength and MFCCs, thinned to warp markers in one changeset; slicing through OneShot and a Drum Rack container. | WT2, WT1 | Any new instrument work. |
| TM-WT-09 | Convert Harmony, Melody and Drums to New MIDI Track | decided | P2 | none (no bundle row) | Live's three conversions, one MIDI track and clip each in one undo step; Drums on TM-WT-01's cached onsets; Harmony and Melody on the suite's pitch tracker (Q-D); results as versioned analysis sections. | WT1 | A model-based transcription (the AI workflows' audio-to-MIDI is a separate feature); new instrument work. |

### 2.3 Devices and processing (30 rows)

| ID | Need | Kind | M | Research IDs | Taken (the contract, in short) | Harvest takes | Rejected or deferred |
|---|---|---|---|---|---|---|---|
| DEV-01 | Racks play (level 1) | refinement | P2 | A11N-001, A6H-042, A8D-001, A8D-022, N11H-006, N11H-035, N11H-036, N11H-037, B-G01, M6-A6H-042 | Racks spliced into the track's graph (a router, chain gates, serial chains, chain strips and a double rack sum), not a nested Graph; ADR-0060's other decisions stand; ChainKey gains Branch and Join legs; mute, solo and volume ramp 5 ms without a rebuild; an unknown latency is told apart from zero; ADR-0122 d8's skip retired. | RK1, RK2, RK4 | tracktion's shared buses and its audio-thread macro lock; wet/dry pin mapping; N-bus (ADR-0113) deferred. |
| DEV-02 | Zones, chain select and note identity where chains rejoin (level 1) | decided | P2 | A6H-042, A6H-043, A3-026, A3-027, A8D-017, M6-A6H-042, B9M-034 | The zone BLOB is exactly 4 bytes (low, high, low_fade, high_fade; NULL is the full range); a note passes every applicable zone; the router runs in the transform phase; the selector is an automatable rack parameter; per-chain note ids where chains rejoin; both fade laws built, Live's measured law the default. | RK3, RK6, RK2 | sfizz's normalized velocity offset in integer zones; Bitwig's Selector modes. |
| DEV-03 | Rack structure: ops, exact undo, copy and paste (level 1) | decided | P2 | N11H-006, N11H-007, N11H-008, N11H-013, N11H-015, N11H-016, A5H-020 | Every structural edit is one transaction with a captured inverse; device.remove and chain.delete capture the whole subtree (parameters, state, panels, routes, macros, mappings that target it); device.move refuses a cycle; Group, Ungroup and reparent are each one transaction; paste is a graph fragment with fresh ids (lifts #394's refusal). | RK4 | A universal move-modifier rule (ERRATA-11: Reason's chapters disagree); undo-manager command objects. |
| DEV-04 | Drum Rack (level 1) | decided | P2 | M8-A8D-001, A6H-044, A8D-004, A8D-007, A8D-010 | Drum mode of the rack router: Receive, then Play translation; choke as router-tagged events sorted before same-position note-ons, symmetric within a group; return chains as rack branches with post-fader sends to the rack or a Set return; pads show Receive notes; a pad swap rewrites mappings, never clips; a sample drop makes a OneShot chain. | RK2, RK3, RK4 | sfizz's directed off_by; the beatbox drum tab (P3); kit instruments (Impulse). |
| DEV-05 | A sidechain key into a device, with Live's taps (level 1) | decided | M4 | A5H-036, B-G01, N11H-040 | A routing row may end at a device's key input (dst_kind 'device'); the tap sits in a side table keyed by routing id; the planner places a compensated key edge into the device's sidechain bus; Pre FX and Post FX read the source chain, only Post Mixer follows fader, mute and solo; hosts negotiate VST3 aux buses and CLAP audio ports. | none | The Quick Kick helper (P2; its target waits for his list); N-bus (P2). |
| DEV-06 | Macros: Live's takeover plus the multimapper (level 2) | refinement | P2 | A3-028, A3-029, A6H-051, A8D-018, N11H-001, N11H-018, N11H-030, A11N-003, A11N-004, B-M01, B-M08 | macro.1 to .16 and chain select as ordinary rack parameters; a MacroSource emits ParamValue through each target's curve (AAUT points; empty is Live's straight line); one mapping is Live's takeover (win's ruling); several sum in real units clamped to floor and ceiling (d15), or last writer; VST3 sums normalized (a named deviation); keyed by (device, parameter id); side tables only. | RK1, RK5 | tracktion's normalized sum-then-clamp and lock; Bitwig's signed amounts as the macro model; Reason's undocumented many-source equation. |
| DEV-07 | Modulation in the graph: the schema, the modulators, the ring (level 2) | refinement | M7 | B-M01, B-M09, B3-015, B3-020, B9M-045, B9M-046, B9M-048, B9M-050, N11H-004, N11H-005, N11H-018, N11H-030, A7B-088 | The modulation ADR ADR-0046 owes (XC-16): route side tables (source, target, depth, curve, mode, bipolar, enable) persist and undo; modulator nodes on the audio thread emit ParamMod, ordered by event-only edges; one evaluator per block delivers the base and one summed offset per target, the same way to CLAP, VST3, built-ins and Pd; one playing-value slot per target; mono or global scope now. | MF1 | Per-voice modulation (until instruments are in scope); mappings inside a device's state blob; audio-rate host targets inside Pd until [adi.param~]. |
| DEV-08 | Automation and modulation heard on ADI's own devices (level 2) | decided | M1 | A11N-002, A11N-004, B-M01 | Built-in devices (and later patch devices) consume ParamValue (the base, normalized through the declaration's curve) and ParamMod (an offset for that block) from io.events at segment start, sent through the pre-resolved sendParameter before the native core or the Pd tick. | none | Nothing. |
| DEV-09 | Pd runtime safe and 64-bit capable (level 3) | decided | M1 | A11N-009, A11N-010, A11N-011 | Native loading closed at the source (absolute, ../ and dynamic names refused, named on the console); quit, exit, open and path changes fenced; one trylock tick per Pd tick (skip and count, never wait); printing through a queue; SPSC sends; arrays read without the lock-leaking path; the #A overlay; the latency binding unbound on close; a PD_FLOATSIZE=64 build. | PD1, PD2, MF4, PD4 | [pd~] out-of-process patches; a file and network policy for shared patches (noted for later). |
| DEV-10 | User Pd patches as devices (level 3) | refinement | M7 | A11N-002, A11N-003, A11N-008, A11N-009, A11N-010, A11N-011, B9G-002, B9G-006 | plugin_refs internal uids for audio-effect, instrument and midi-effect patches make a PdPatchDevice; state roles patch, abstractions and sample:<id>; loadState parses and walks the allowed objects off the audio thread, reconciles parameters by id, swaps engines under the gate; Save is one device.loadState; a failing patch is a placeholder keeping its bytes; three starter templates. | PD5, PD4 | hvcc (a compiler at run time, a second DSP truth); plugdata's name-keyed parameter pool; a "data" role unless win rules it. |
| DEV-11 | The Pd patch editor's route (level 3) | new-choice | M7 | B9G-003, B9G-004, B9G-005 | The director's Q-K (C-1). Recommended route A: "Edit in Pd" attaches vanilla Pd's editor to the running instance; edits are heard at once; objects ADI cannot run draw dashed; Save re-reads, validates and writes one device.loadState; closing without saving reverts; Save updates every instance that shared the old hash. | PD3, PD1, PD4, PD6 | Route C (plugdata's canvas inside ADI, XL) unless its look is worth months; route B later, as an optional "Open in plugdata". |
| DEV-12 | Opening and editing a built-in device's patch (level 3) | new-choice | M7 | A11N-006, A11N-005, B9G-008 | The director's Q-K (C-3). Recommended: opening a built-in shows its canonical patch read-only; editing makes "<Device> (edited)" on the user-patch path, leaving the built-in native; the copy states which path runs and its delay. | none | Editing in place (it would silently change a familiar device's sound path and timing). |
| DEV-13 | User-patch delay when blocks do not divide by 64 (level 3) | new-choice | M7 | A11N-005, N11H-038 | The director's Q-K (C-2). Recommended: no added delay when the host buffer is a multiple of 64 (whole Pd ticks across a block's splits, each event at the start of its tick); otherwise one reported, compensated 64-sample block shown on the device; a patch's own delay always added. | none | Silencing invalid segments in production; promising zero delay for any patch. |
| DEV-14 | Pd note output, voices and instrument kinds (level 3) | refinement | M7 | A11N-007, B9G-038, B9G-039, N11H-017, N11H-039, N11H-040 | Per-instance libpd MIDI-out hooks fill a fixed array inside the trylock tick, decoded under the device's note prefix (DEV-17); offsets from Pd's logical time; MIDI-effect patches run in the transform phase; Pd instruments consume notes with [clone] voices; a host reset bang on prepare, stop, bypass and panic; MIDI 1 by construction. | MF3, PD1, MF5 | [noteout] alone as completion; claims of Grid-identical polyphony; a high-resolution note bridge until a patch needs it. |
| DEV-15 | Grid-style modular semantics on the Pd tier (level 3) | refinement | M7 | B9G-001, B9G-059, B-G02, B-G06, N11H-038 | A hybrid: the host owns note identity, scheduling, modulation, state, undo and latency; Pd is the authoring and runtime layer; registered native kernels carry the heavy DSP. An ADR answers the gap review's eight decisions before any Grid-style work; acceptance is G1 to G6. | PD1, PD6 | A second native graph runtime or editor; a graph copy beside the Pd copy; Bitwig compatibility and Grid-equivalence claims; [adi.param~] until a patch needs it. |
| DEV-16 | `.amxd` translator and Live Suite's Max for Live devices (level 3, P2) | decided | P2 | none | A pre-flight allowlist scan of the patcher's boxes; a full match converts into a user Pd patch device with Max's right-to-left order explicit; otherwise the AI proposes through the Propose tier and the result must pass the allowed-object walk; Suite's LFO, Envelope Follower and Shaper become native modulators on DEV-07's routes. | PD4 | A Max application path. |
| DEV-17 | One note-identity namespace | refinement | M6 | A7M-128, B9G-038 | One process-wide event-source prefix allocator (a note id is the prefix in the high 32 bits and a serial in the low 32) for clip tracks, live inputs, transform nodes and Pd output; note-offs acknowledged by prefix; per-instance int32 maps in the CLAP and VST3 hosts replace the % 0x7FFFFFFF fold; same-key MIDI 1 handling; a clip's serials derived from the stored note identity so a rebuild keeps held notes. | RK2 | Per-device counters (#207's nextId * 1024); per-event owner pointers. |
| DEV-18 | The event transform phase; generated notes owned and released | refinement | M6 | A7M-128, N11H-039, B9M-001 | transformEvents runs in Graph::forwardEvents after a slot's inputs are gathered and before splits, into a fixed store; transform nodes own their notes by prefix; releaseGenerated on bypass and when a node leaves at a publication swap; stop, seek or a loop jump releases everything; the clock runs while bypassed; prepare is idempotent. | MF1, MF5 | #207's EventType::Control = 5 and its per-event EventOwner*; pull-based scheduling. |
| DEV-19 | Live's fourteen MIDI effects | decided | M6 | A7M-001, A7M-053, A7M-122, A7M-128, A7M-130, A5H-020, B9M-002 | Native midi_effect devices (org.adidaw.builtin.* uids) inserted before the instrument; kernels in src/adi/dsp with fixed state; randomness varies per pass by default, with a keep switch that stores the seed (one setParam, reseeded per note from seed, position and key); Use Current Scale from a published scale context; one ADR and one parity checklist per device; P1 first (Arpeggiator, Chord, Scale, Pitch, Velocity, Note Length, Random). | MF1, MF5 | Microtuner (parked); Expression Control, Envelope MIDI and Shaper MIDI until DEV-07; Groove until the groove pool; tracktion's MidiModifier. |
| DEV-20 | Recording generated notes, Live's way | refinement | M6 | N10H-015, N10H-016, A7M-128 | A MIDI row from track A (Pre FX or Post FX tap) into track B's note input is a graph edge, delayed by ADR-0091's rules; B's recording and Capture take the tapped events (an ADR amends MidiMemory's live-input-only rule); generated ids re-prefixed at the receiver. | MF6 | ADI's own "commit MIDI effects to a clip" (an addition; Live's tap-and-record covers the need). |
| DEV-21 | OneShot and Sampler (the two authorized instruments) | decided | M6 | A7M-058, A7M-131, A7M-127, A7R-008, A7R-009 | Catalogue kind instrument (consumes notes), native dispatch in makeBuiltinDevice, samples through #414's slot ops, idempotent prepare; Sampler's zones published immutably on OneShot's voices; Sample Selector and Offset latch at note-on (A7M-127). Land #278, then port #208 onto it. | MF2 | Live's other instruments (outside scope: Analog to Impulse). |
| DEV-22 | Live's audio effects: measured parity, the queue, Hybrid Reverb | decided | M6 | M7-A7A-001, M7-A7B-001, A7R-001, A5C-021 | Each device a canonical canvas with a native core and no adapter delay, fixed parameter ids, latency reported and kept while bypassed, accepted on a measured Live 12 render, one per PR; the queued effects in the manual's order, Compressor first; Hybrid Reverb's native factory; the DJ Filter on Auto Filter's cores. | LD5 | Parity claims from synthetic oracles; instruments other than OneShot and Sampler. |
| DEV-23 | Multi-threaded graph processing | refinement | M7 | A5C-019, A5C-020, M-CPU, M5-A5C-019, B3-009 | One persistent pool owned by GraphHost and lent to each graph; a dependency-counted, work-conserving scheduler (a chain continues on one thread, the callback co-runs, a node count ends the block); workers inherit FP state and real-time standing (MMCSS Pro Audio, the macOS workgroup, SCHED_FIFO); devices opt in; offline renders on their own normal-priority pool; on by default only after the the director's Windows workstation measurements. | GT1, GT2, GT3, GT4, GT6, GT5, GT8, GT7 | #441's per-graph pool and level barriers as the target (keep its RenderState split, FP propagation and tests); a user-facing multicore switch; plug-in sandboxing. |
| DEV-24 | CPU Load Meter and Performance Impact | refinement | M7 | A5C-019, M5-A5C-019, M-CPU | The callback load from JUCE's AudioDeviceManager (getCpuUsage, getXRunCount) read raw so values above 100 % show; Average smoothed, Current and the overload warning as Live's; per-track Performance Impact from per-slot block timings summed per track at UI rate. | GT4, GT6 | Ranking DAWs by their meters. |
| DEV-25 | Out-of-process plug-in scanner | refinement | M1 | A5H-034, A5H-035, M5-A5H-034, B3-005, B3-009, B3-010 | Land #440, #439 and #438 after D1, D2, D5, D7 and D8 (and SEM_FAILCRITICALERRORS), checked by SHA; registry v3 (a scan cache keyed by format and path with a stamp; two strikes block; a negative cache); a scan service (K workers, Incremental, CacheOnly and Clean modes, a catalogue snapshot, a report, scans at startup only); Settings > Plug-Ins with Rescan, Alt+Rescan, Alt at launch and a blocklist. | SC1, SC2, SC3, SC4, SC5, SC6, SC7 | A long-lived scan worker; any in-process fallback; runtime sandboxing; Re-scan Faulty, Selected and a per-plug-in Skip (additions not proposed). |
| DEV-26 | The director's plug-ins load as ordinary plug-ins | decided | M1 | N11H-028, N11H-033, A5H-021, A11N-009, B3-005, B-G07 | His builds take the ordinary path: scanned in a child process and listed under Plug-ins; inserted in one undo step; edited in their own windows (one op per knob gesture, one per preset); state round-trips through save and reopen; ADI-L's lookahead compensated, also while bypassed; a missing build becomes a placeholder keeping its state. Nothing in ADI depends on their source until his list. | none | Designing the unified feature, the native slot contract or any built-in that hosts the VST3; requiring a CLAP target. |
| DEV-27 | AudioGridder remote devices | decided | M7 | B3-009, B3-010, N11H-032 | A remote device is a DeviceInstance with remote_host_id (format unchanged); the network stays off the audio thread through an SPSC queue; pipelined one block behind with that latency declared; a missed deadline outputs silence and counts; an unreachable server loads the plug-in locally, else a placeholder; the LAN-only authentication posture stated before shipping. | none | A wrapper plug-in (ADR-0053); PTP until a measured need. |
| DEV-28 | One loudness meter for every reading, proved from outside | refinement | M4 | R10H-073, R11X-025 | src/adi/dsp/loudness: 10 ms sub-blocks, gated I, LRA at 10 Hz, max M and S, run and pause, 0.01 LU exact-energy histograms, explicit channel weights, a double true-peak path beyond 8 channels; N13, export normalization, Auto Gain Stage and the Reference rewired onto it; the claim is "passes ADI's synthesised Tech 3341/3342 cases and agrees with libebur128 and ffmpeg", never "EBU Mode compliant". | LD1, LD3, LD4, LD6, LD7, LD2 | The EBU WAV set (not usable); libebur128's LRA (1 Hz hop); per-bus meters; ADI-L's own meter as the base (waits for his list). |
| DEV-29 | Export loudness: NI Stems guard, "together", brickwall, more targets | decided | M5 | R10H-073, R11X-002, R11X-025, R11X-034, R11X-043, R11X-047, A11R-001, M3-EXPORT-INTERCHANGE, A5C-022 | Now: validateNiStems refuses normalization. Then (a) an after-AAC check lifts the refusal; (b) "together" is one gain from the summed, time-aligned capture; (c) an off-by-default true-peak brickwall in ADI's LookaheadLimiter with a refine loop of at most four trials; (d) RMS-I and max M/S targets. WITH and VIA master stay distinct; job values at enqueue, revision and names at job start. | LD5, EX4, LD3 | An enqueue-time snapshot (ERRATA-11); one checkbox for both master modes; normalizing by default; ADI-L as the export limiter. |
| DEV-30 | The Loudness Drive after the 5 November ruling | refinement | M4 | none (no bundle row) | A saved, automatable global control with its own op that adds the same gain to every leaf track's input gain (after the instrument), never to groups, returns or Main; click-free at both precisions; the Reference bypasses it; it engages nothing (the cascade is whatever clippers and limiters the user placed, his own plug-ins included); the engine's "blocked" entry lifts. | none | Switching on per-channel slots (withdrawn); the generic limiter as a stand-in for his clone; VCA fader tracks; automatic stage switching (waits for the unified feature). |

### 2.4 Mixer, routing, monitoring and the Mix View (36 rows)

| ID | Need | Kind | M | Research IDs | Taken (the contract, in short) | Harvest takes | Rejected or deferred |
|---|---|---|---|---|---|---|---|
| MX-01 | Strip meters on the engine's scalar meter | decided | M1 | A5C-001, A5C-004, A-BASE-18.1, A-BASE-18.1.1, M5-A5C-001 | Each visible strip reads {peak, rms} relaxed per frame from the engine's StripMeter; ballistics and the Peak Level hold stay the window's; no ScopeTap per strip; while a track monitors its input the meter shows the input (MX-09). | none | The per-strip 0.1 s tap that never closes; K-meter, PPM and VU scales (additions not asked for). |
| MX-02 | Split Stereo Pan | decided | M1 | A5C-003, A5C-023, A-BASE-18.1, M5-A5C-003 | The pan menu ticks from panMode and writes mixer.setPanMode (one step); a split strip draws Live's two sliders; each drag is one step of setPanLeft or setPanRight; double-click resets; the two pans' lanes come with the automation lanes. | none | Width or phase controls on the strip (Utility keeps them, ADR-0203 d9). |
| MX-03 | Pan law, 64-bit summing and the Processing Precision switch, measured against Live | decided | M4 | A5C-023, A3-009, A6G-006, A6G-007, A6G-010, M-AUDIO, M-AUDIO-PRECISION, M6-A6G-006, A-BASE-38.2.4 | Keep the built pan law and 64-bit summing and close them with records: AWAITING 5's readings for a -6 dBFS sine at C, 25R and 50R; the precision cancellation cases; undithered render neutrality; Live's mono sum; a Live law that differs opens a new ADR, as ADR-0163 says. | MS1, MS5 | Changing ADI's law before the readings; a 64-bit default (ADR-0207 d2). |
| MX-04 | Track Delay in milliseconds, and Live's Delay Compensation switch | decided | M1 | A3-012, A6H-034, R10H-011, N10H-048, A-BASE-18.7, A-BASE-23.6, M6-A6H-034 | A millisecond field writes mixer.setDelay (one step), shown by D and the Track Options items; win adds audio.delayCompensation (on) and its Options item, which drives Session::setDelayCompensation; the field greys while it is off; ±1 s until AWAITING 8 reads Live's range. | none | Sub-sample Track Delay (P2, with Audio Alignment); REAPER's per-chain and per-FX PDC modes. |
| MX-05 | Returns and sends as Live | decided | M1 | A5C-009, A6S-001, A-BASE-18.4, N10H-044, R10H-002 | B1: one Pre/Post switch per return through return.setSendTap, with no refusal when there are no sends; B2: new send ids through the one id helper; New's A Reverb and B Delay in the parity notes; new sends at -inf until AWAITING 7(a); return rings refused until 7(f); Pre sends follow solo until 7(d). | none | Per-send Pre/Post and a Pre-FX send tap (the M10 review corrected R10H-002). |
| MX-06 | ADI's sends into any track, and a pan on a send | decided | M1 | C3-022, N10H-043, N10H-044 | The send slot's "Send to Track..." lists eligible tracks (never Main, never itself unless a return); routing.connect of kind send plus setGain, one step; Pan in a send knob's menu (routing.setPan, one step); a send into a summing group keeps its channel half. | none | Cubase's Direct Routing fan-out (P2, MX-16). |
| MX-07 | Solo as Live, with its options | decided | M1 | A5C-011, A6H-007, A6H-008, A6H-009, A-BASE-18.6, M3-ROUTE, M5-A5C-011 | Worklist B5: Solo in Place from the snapshot, written by mixer.setSoloInPlace outside undo and saved with the project; Live's Options entry maps to the same field until measured; preFollowsSolo and keepAllReturns change only on readings; Solo in front and the solo bus stay registry rows labelled "not built". | none | An engine for Solo in front and the solo bus until the Live base is done. |
| MX-08 | Cue, Cue Out, Cue Volume, the metronome and preview on Cue Out; the output stage | decided | M1 | A5C-012, A-BASE-18.6, C3-023, C3-024, C3-025, C3-027, R10H-010, M-CUBASE-CUE, M3-ROUTE | Worklist B6 (outside undo, the session refreshed after each); the engine's cue bus as built until the Live reading, then the cue tap follows Live's; the engine reads audio.metronomeOutput and audio.autoMute; the browser's preview on Cue Out once the preview path exists. | none | Cubase's four cue mixes, talkback, AFL/PFL choice and Listen Dim (the Control Room, v2); a fallback pair when Cue Out is missing. |
| MX-09 | Input monitoring, Arm on the strip, Keep Monitoring Latency, Reduced Latency When Monitoring, latency calibration | decided | M4 | A3-007, A3-008, A3-012, A5C-001, A5C-024, A6H-010, A-BASE-17.1, A-BASE-18.8, M3-ROUTE, M5-C5W-030, R10H-011 | A live input node per monitored track, fed from its input (MX-25) per monitor_mode (Auto = armed and no clip playing); Reduced Latency skips compensation for input-monitored tracks; Keep Monitoring Latency shifts the recorded placement; Driver Error Compensation offsets every recording; Measure round trip by MTDM on a loopback; the window wires Arm and the toggles (C6). | MS2, MS4 | Cubase's monitoring preferences (comparison only). |
| MX-10 | Loudness meter on Main | decided | M1 | C-U02, R10H-073, R11X-025 | Main's strip draws M, S, I and TP from the engine's readout, loaded relaxed each frame; a click resets; no other strip shows them; the picture goes to him first (an addition); later the Scope, analyser and export readouts read the same meter. | LD7 | A meter on every bus (ADR-0203 d14). |
| MX-11 | Native analog summing on groups and Main: the controls | decided | M2 | none (no bundle row) | Summing on or off, the flavour (eight) and the drive (-12 to +24 dB) on groups and Main through the built group.setSumming ops; the drive dial is one gesture and one undo step. | none | Crosstalk (P2); FEATURES' stale "the master is not built". |
| MX-12 | Small live scope and spectrum in every strip, and the analyser's big view | decided | M4 | none | A visible strip subscribes one 96-point spectrum (every 50 ms, 300 ms smoothing) and one 40-80-column min/max scope; a hidden strip holds none; a click opens the analyser's floating view or the Scope on that track; one tap per visible strip. | none | OpenGL for strips; per-client analysis on the audio thread. |
| MX-13 | The Scope window, and a minimal SCOPE & PHASE | decided | M4 | R11E-026, A7B-086, M6-A6G-011 | One Scope component, shown as a window (Compare in Scope) and as the SCOPE & PHASE tab (two tracks first, eight later); heard-time alignment by stamps; Fix Delay proposes mixer.setDelay on B (one step); polarity is a per-layer view flip; a track polarity fix, if offered, sets Utility's Phase switch, never the strip's stored phase_invert. | none | Sub-sample Track Delay until Audio Alignment (P2); the EXO (P2, inside the analyser device). |
| MX-14 | The Mix View shell and the function keys | decided | M1 | B-H01, C3-021, C5W-001, C5W-002, C6-KEYBOARD | F1 to F8 lose Live's track activators (his deviation); F4 opens and closes the Mix View only; Activate Track 1 to 8 keep their menu entries without keys; the Mix View's switch, detach and re-dock (his remark R3). | WI1, WI2 | A second Mix View window; cloned mixers (ADR-0063, ADR-0180). |
| MX-15 | The crossfader (P2, with Session View) | decided | P2 | A5C-010, A-BASE-18.5, B3-020, M5-A5C-010 | A crossfade gain at each assigned track's gain stage (no routing change); its storage in side tables placed by win when Session View starts; seven curves; the position and each assignment mappable and automatable. | none | Before Session View (P2); the Mixer View option stays dimmed until then. |
| MX-16 | Multi-output instruments, up to 64 buses per node, Direct Routing (P2) | decided | P2 | A6H-012, A-BASE-17.5.2, C3-022, R10H-001, R10H-003, R10H-005, R10H-006, R10H-007, M6-A6H-012 | Buses beyond Main and Sidechain become numbered node ports; Audio From can name "track X, output n"; routing.channel_map carries partial channels; Direct Routing is several enabled outputs from one track. | none | REAPER's 128 channels per track (ADR-0113's 64 stands). |
| MX-17 | System audio as a track input (P1), and the ADI virtual audio device (P2) | decided | M4 | A6H-002, A-BASE-17.2 | The loopback source appears as input channels in Audio From with its resampler latency declared (P1); the virtual audio device is an output choice for Main or a bus (P2); both follow Live's chooser rules. | none | Bundling VB-CABLE; the virtual device before P2. |
| MX-18 | The Control Room (version 2) | decided | P3 | C3-023, C3-024, C3-025, C3-026, C3-027, C5W-013, R10H-010, M-CUBASE-CUE | Deferred to version 2, with recorded constraints: monitor-only processing is workstation state, never in a render or the project; cue mixes are per-channel sends; talkback latch or hold; the Cue bus and the reference route become its first sources. | none | Deferred (decided). |
| MX-19 | Mixer snapshots (P2) | decided | P2 | C3-020 | Deferred; recalling a mixer snapshot is one undoable step in the one journal, never a second history. | none | Before P2. |
| MX-20 | Quick Kick Sidechain (P2): the routing now, the target after the list | decided | P2 | A6H-013, N10H-045, N10H-046 | The routing half is MX-24's sidechain into a device with the Pre FX tap; what the button engages or inserts waits for his list (the unified feature) and is not designed here. | none | The button, pill and drops until his list; any slot-based design. |
| MX-21 | The P2 analysis tabs, the AI Mix Radar and the docks | decided | P2 | none | As the program's §1.24 and §4.3: OVERVIEW (a SPAN-class overlay of pinned tracks with statistics and correlation), SPECTROGRAM (reassigned, OpenGL on that view only), the EXO inside the analyser device, and the AI Mix Radar through analyze.* and a Propose changeset; parts that drew a slot wait for his list. | none | K-meters and PPM, the RGB waveform and chroma (additions not proposed). |
| MX-22 | The Live mixer and routing measurement round | decided | M4 | M3-ROUTE, M5-A5C-001, M5-A5C-009, M5-A5C-011, M5-A5C-012, M6-A6H-007, M6-A6H-008, M6-A6H-034, M-CUBASE-CUE, A6P-003 | Generated Live 12.4 sets per case family (pan law; sends 7(a) to (i); Track Delay; group mute; the crossfader and Post sends; the cue tap; meter ballistics; monitor defaults), rendered and compared through the harness; GUI cases through the logger's case protocol; win relays only records labelled with the Live build. | MS3, MS1, MS5 | Answers from manuals or controller palettes; setting Live's parameters through its API for automation or undo cases. |
| MX-23 | Drags heard while dragging, one undo step, multi-selection | refinement | M2 | A5C-002, A-BASE-18.2, M5-A5C-002, M-LIVE-UNDO, C6-HISTORY | A mixer drag (volume, pan, split pans, send level, a Track Delay drag, the summing drive) runs a GestureSession with live values (nothing journaled per update); End commits one transaction of the final value; Esc reverts; an unchanged end commits nothing; with N selected tracks the gesture is one transaction of N relative ops. | HI1, HI2, HI6, HI7, HI8 | A UI-layer list of coalescable ops; journaling every value. |
| MX-24 | Sidechain keys reaching a device, with Live's three taps | decided | M4 | A6H-008, A6H-009, A6H-013, A-BASE-17.5.1, N10H-045, N10H-046, N10H-047, N11H-035, N11H-036, N11H-037, C-E03, R10H-012 | A sidechain row may end at a device; the realiser connects the source's tap node to that device's sidechain bus, compensated; the tap sits in a side table keyed by routing id (absent means Post Mixer); a key stays unheard; bounces and renders keep keys; Live's sidechain and tap choosers on devices that declare a sidechain input. | RK1 | Reason's compensation gaps as targets; REAPER's blanket PDC disable with feedback. |
| MX-25 | Track-to-track routing and live In/Out choosers | refinement | M4 | A6H-001, A6H-002, A6H-006, A6H-007, A6H-011, A6H-012, A6H-014, A3-009, A-BASE-17.2.1, A-BASE-17.4, A-BASE-17.5, R10H-003, R10H-007, R10H-008 | Win places (i) external inputs (hardware channels, loopback, a Link Audio peer) in tracks.input_ref's grammar and (ii) a whole new table for track-to-track inputs and MIDI From, with real foreign keys and their tap; Sends Only and No Output as an output state in a side table. The engine plays a non-exclusive edge from the source's tap, governed by monitoring; Resampling takes Main without the recording track; one step for all selected tracks. | none | A cable or matrix UI; REAPER's pin mapping (waits with MX-16); a track id stored inside text. |
| MX-26 | Group mute semantics, and the registry default | refinement | M4 | A5C-007, A5C-008, A-BASE-18.3, M5-A5C-007 | Until the reading the engine keeps its structure (a group's mute mutes its output; children keep sending), and mixing.groupMuteMutesSources defaults to Live's measured behaviour; if Live has no such option, the key leaves the registry or stays an ADI option, off. | none | Cubase's default (true) as ADI's default. |
| MX-27 | One loudness meter, tested against independent references | refinement | M4 | C-U02, R10H-073, R11X-021, R11X-025, R11X-047 | The acceptance view of DEV-28's one meter: Main's meter, export normalization, Auto Gain Stage's LUFS mode and the Reference's match all read it, tested against synthesised Tech 3341 and 3342 signals and libebur128. | LD1, LD3, LD4, LD6, LD5, EX4, LD7, LD2 | The EBU WAV set (not usable); libebur128's LRA; Ardour's and lsp's meters; ADI-L's meter until his list. |
| MX-28 | Performance Impact indicators and the CPU Load Meter | refinement | M7 | A5C-019, A6G-003, A-BASE-18.9, A-BASE-37.1.1, R10H-014, N10H-050, N10H-051, M-CPU, M5-A5C-019 | The engine publishes per-callback deadline use (audio work only) and per-track node time; the window draws Live's CPU Load Meter with its options and the six-rectangle Performance Impact indicators; win adds the FEATURES rows. | GT4, GT6 | Ranking DAWs by their meters. |
| MX-29 | The Reference that sounds: role storage, matched loudness, spot play, drops on the strip | refinement | M4 | C3-025, C3-026, R10H-010 | Roles in a side table track_roles (track, role; one Reference) as minor 1.20 on its own; the engine repoints its three readers from the column probe; an essential row makes older readers warn; a matched-loudness gain on the cue path from short-term loudness (Match on by default, nothing written); Alt+hold spot play outside undo; a file dropped on the strip lands on the hidden Reference track, Warp off. | none | A separate reference bus (ADR-0209 Q7); Bluprint's matcher, the four-stem split and phone translation (P2). |
| MX-30 | Taps and the visual worker | refinement | M4 | none | Win's ADR amends ADR-0175 d2 (stamps kept per block in a side ring) and d4 (a tap retired by epoch after its last consumer, never freed under a stalled callback); device input and output tap points; gapless cursor reads; one visual worker per process paced by the displays; one STFT per tap and resolution; a 4 ms budget with a declared degradation order; analysis kernels as pure functions shared with analyze.* and export. | none | A fourth audio-to-UI path without an ADR; analysis on the audio thread for a client. |
| MX-31 | Roles, the CTZ template, the navigator and CASCADE, without slot data | refinement | P2 | none (no bundle row) | Set Role in the track, strip and STACK menus; the CTZ template in the User Library through PD-14's mechanism (Live's default Set stays the default); navigator blocks from group taps; tiers from a small template side table with a name-match fallback; the shaving and breathing bars and CASCADE's stage columns stay empty until the unified feature. | none | Per-rebuild "CTZ group metadata" from the engine; slot-derived shaving. |
| MX-32 | Hosted plug-ins learn their track's name and colour | refinement | M4 | none (no bundle row) | After loading and on every rename or recolour, pass the track's name and colour to hosted plug-ins (VST3 channel context through JUCE; clap.track-info for CLAP). If a fixture shows Live 12 does the same, this is Live's way; if not, it is an addition for his yes. | none | CLAP drafts (gain-adjustment metering, mini curve display). |
| MX-33 | The measurement harness | refinement | M4 | M-AUDIO, M-AUDIO-PRECISION, M-CPU, C6-AUDIO, M6-A6G-011 | Win's ADR "the measurement harness": a record schema, a gate vocabulary declared before each run, the seat table and the relay rule (AWAITING rows struck only from records); tools/measure from the live-probe kit, a Windows Live driver and analyse.py; the engine's src/adi/measure (residual compare, MTDM, sync spikes, probe signals) and adi_measure; audio_probe --loopback. | MS1, MS2, MS3, MS4, MS5, MS6 | DawDreamer, Plugalyzer and pedalboard as gates (block-granular automation); an .als importer. |
| MX-34 | The governing texts follow the 5 November ruling | refinement | M0 | none (no bundle row) | One win docs PR records the 5 November ruling in every text that still names slots: FEATURES §2 rows, LANES §3 and §4, BUILD-BRIEF §5 and Phase D 7 and 7a, UI-ARCHITECTURE §12, the parity checklists, the harvest ledger's §1 and the Mix View program; the stale text of FEATURES (the master "not built", the strip row) fixed; the code stays as drawn. | none | Any design of the unified feature. |
| MX-35 | The Loudness Drive after the ruling (CH-MX-1) | refinement | M4 | none (no bundle row) | The same contract as DEV-30, its owner: the Loudness Drive as one automatable dB control adding the same gain to every leaf track's input gain, with 5 ms ramps, exported as played, engaging nothing; built after the records PR. | none | VCA fader tracks; stage engagement (part of the unified feature). |
| MX-36 | The Mix View and STACK before the unified feature (CH-MX-2) | new-choice | M4 | C3-021, A7A-062 | The director's Q-B. Recommended (a): after the mixer's B1 to B8, the non-slot parts now: the Reference (MX-29), the strip scope and spectrum and the analyser view (MX-12), the Scope window (MX-13); then STACK as an instance list of each track's own devices (his VST3s among them) with the fisheye, slider, navigator, search, pins and row spectra; tiles read-only except ADI's own devices; the drawn slot columns stay off until his list. | HI1, WT-1, WT-6 | The program's slot work; ADI-Q-specific tiles and editors; the phrase that the slot columns become the unified feature's place. |

### 2.5 Project, export and data safety (36 rows)

| ID | Need | Kind | M | Research IDs | Taken (the contract, in short) | Harvest takes | Rejected or deferred |
|---|---|---|---|---|---|---|---|
| CO-01 | The collaboration core: order, identity and rebase | refinement | P2 | DR-12-11, DR-12-13 | Server-ordered: every batch gets a global seq (a put-if-absent claim on the bucket; the host on a LAN); batches carry author, id space, base seq, vector and typed ops; replicas apply in seq order through a remote commit; ids from the one allocator (an id space in the high 32 bits, a counter in the low 32); fractional sibling keys; rebase by rewinding the pending suffix and re-committing; machine-local columns out of sync and digest; code in src/adi/collab. | C1, C2, C3, C4, C8, C9 | Yjs or any CRDT mirror of the project; SQLite's rebaser; validation on a server copy. |
| CO-02 | Undo in a shared project | new-choice | P2 | DR-12-15 | The director's Q-N (his Q4). Recommended: an unsequenced transaction undoes by moving the head; a sequenced one appends a compensating batch of its inverse ops, minus those whose touch set meets a later batch by someone else; redo compensates the compensation. | C1, C2 | Compensating a collaborator's later writes by default. |
| CO-03 | Incoming changes previewed; collisions kept both ways | decided | P2 | DR-12-12, DR-12-13, DR-12-14 | Preview is CO-01's rebase run in a rolled-back transaction, returning the diff and the collisions; Apply commits the same rebase as one transaction; ghosts in a sync_ghosts table the engine never reads; Combine blends in the lane's own domain, then thins; "a clip becomes a take" waits for take lanes. | C1, C2, C9 | Auto-applying incoming batches (against the directive). |
| CO-04 | Live awareness: how presence travels | new-choice | P2 | DR-12-11, DR-12-12 | The director's Q-N (his Q5). Recommended: (a) presence as heartbeats beside the batches in the bucket, no live cursors, for the first collaboration release; (b) an optional LAN-hosted live session later, off by default, on the control API's one server stack. | C5, C6 | A relay service of ADI's (against "ADI runs no service"). |
| CO-05 | Shared media and the bucket client | decided | P2 | DR-12-11 | Objects media/<blake3> in the owner's bucket; ADR-0182 d4's selection (all, none, per file); an S3 client with SigV4 and a conditional-write probe; a keychain adapter per platform; a resumable upload queue that survives a crash. | C4, C7 | Git of the text projection as the carrier. |
| EX-01 | One consistent revision per job, and media identity | decided | M3 | A11R-001, A11R-004, A11R-005, A11R-007, R10H-079, R11X-013, R11X-028, C4-008, DR-13-05, DR-13-06 | Keep ERRATA-11's four ownership moments (job values at enqueue, membership at Queue::run, one revision and the names at each job's start, publication last); add a run manifest with the journal head each job read; every referenced media file's BLAKE3 checked at job start and again before publication. | EX3, A4 | An enqueue-time project snapshot (R10H-079); MAGDA's revision policy as a second job system. |
| EX-02 | Sources, routes and taps | decided | M5 | A4-001, A4-002, A4-008, R11X-001, R11X-002, R11X-003, R11X-005, R11X-044, A11R-002, C4-003, C4-005, C4-006, DR-13-01, DR-13-02 | Source {track, tap (PreFx, PostFx, PostFader, PostMixer), route (Direct, ViaMain, MainFxOnly at P2)}; ViaMain as an offline audibility override that keeps keys and sidechains live and never uses solo; batch expansion with a cardinality preview and unique names; the dependency closure as a preview, not a filter; impossible combinations refused at add. | EX3 | Tracktion's useMasterPlugins read as Live's mode; stems by soloing each track; REAPER's parent-channel projection and automatic mono (P2). |
| EX-03 | Render as Loop, tails and gapless region batches | decided | M5 | A4-002, A11R-003, R11X-006, R11X-007, R11X-036, R11X-046, C4-001, C4-005, DR-13-09 | Render as Loop is Live's two passes (one unwritten pass, latency-compensated); Job.tail is Off, Fixed or UntilSilent, its bounds stored apart; refused inside a gapless batch; the warm-up from zero stays and is not a tail; the gapless cycle-marker batch is P2 (EX-12). | EX3 | Tracktion's wrapRemainder fold as Render as Loop; REAPER's per-region take-FX reset (a comparison). |
| EX-04 | Deliveries, formats, Live's rate rule and mono | decided | M5 | A4-003, A4-004, A6G-010, R11X-019, R11X-020, R11X-041, A11R-009, C4-002, C4-012, C4-013, DR-13-07 | Job.deliveries (container, sample format, dither, codec, suffix); the source captured once in double at the graph rate, then each delivery quantized or encoded, decoded, checked and published; a capability matrix refuses impossible combinations; FLAC equals the PCM WAV bit for bit; LAME gets the undithered double; below the project rate process, then convert with soxr; Float64; AIFF-C fl32; Convert to Mono; Create Analysis File into the analysed folder. | EX1, EX7, EX9 | JUCE's format writers and its LAME format (render/ is JUCE-free); an ffmpeg command-line pipe; libsndfile as the writer; FilmCraft's Rust encoders. |
| EX-05 | Normalization, statistics, the brickwall and the stage order | decided | M5 | R10H-073, R11X-021, R11X-025, R11X-027, R11X-034, R11X-035, R11X-043, R11X-047, A4-003, DR-13-07, DR-13-08 | One documented stage order, each adjacent pair tested: capture, loop and tail windows, SRC, channel conversion, analysis, normalization gain, the optional true-peak brickwall, then per delivery dither, quantize, encode, decode and check, metadata, publish. Statistics from the one meter; "together" measures the summed capture; an analysis-only run never processes; the brickwall is off by default and never acts in Peak mode. | LD1, LD2, LD3, LD4, LD5, EX4 | libebur128's LRA (superseded EX2); ADI-L's detectors as the brickwall until his list; the EBU WAV set (not usable). |
| EX-06 | Dither for Live's three Pow-r slots | new-choice | M5 | A4-004, A6G-010, R11X-034, A11R-009, DR-13-07 | The director's Q-J. One quantizer for WAV, AIFF and FLAC (seeded, partition-independent, applied once, never at 32-bit float); shaped modes reserve headroom so a 0 dBFS peak cannot clip; rates without a table use a stated fallback; the shapes and names of Live's three Pow-r slots are his (recommended: SoX's open tables now). | EX5 | POW-r (not usable); the Airwindows dithers (none is F-weighted). |
| EX-07 | Names, collisions, partial outputs and the run manifest | refinement | M5 | A11R-006, A11R-007, A11R-008, A6D-012, R11X-011, R11X-012, R11X-016, R11X-017, R11X-018, R11X-038, R11X-040, R11X-041, C4-007, C4-009, DR-13-04, DR-13-05 | Job.collision is Fail, Increment or Replace; a single export asks through the save dialog (Live's way); batches follow Live once read; collisions resolved once per delivery at preflight (case-folded) and checked again at publication; Replace through replaceFile; a JSON run manifest; cleanup failures reported; the length check on the final name; Live 12.4.3's default naming. | EX3, R1 | Directory templates until escaping rules exist; REAPER's keep-partial (P3); Cubase's cancel deleting files already exported. |
| EX-08 | Export metadata | decided | M5 | R11X-023, R11X-024, R11X-042, C4-014, DR-13-07 | A typed Job.metadata; bext v2 with the loudness fields from the checked output (0.01 LU), an R99 originator reference seeded by the job, origination time and coding history; cue markers on the time map shared with trim and pad; Vorbis, ID3v2 and MP4 tags; header fields patched after the check. | EX6, EX8, MD8 | An ACID tempo chunk by default (opt-in only); libsndfile's adtl handling. |
| EX-09 | Job persistence, presets and the agent's path | refinement | M5 | R11X-013, R11X-014, R11X-015, C4-008, C4-015, DR-13-05, DR-13-07 | render::Job to and from JSON under a strict schema; user presets in ADI's data folder; project presets and saved jobs in session_state (no schema change); field-group masks; saving, enqueueing and rendering are three actions; export.queue.* on the control API at the Propose tier; the agent's job manager wraps render::Queue. | EX3, CA1, OS1, A4 | Cubase's 20-job cap; REAPER's opaque sink blobs. |
| EX-10 | NI Stems: normalization and macOS | decided | M5 | DR-13-11 | Refuse Peak and Loudness normalization on an NI Stems job now (one check, one test); later one gain measured on the premix for all five streams, checked after AAC; the macOS fix as PD-00. | none | Writing a Pioneer stem container (none exists; AWAITING 17). |
| EX-11 | The Export page, and where renders run | decided | M5 | A4-001, A4-002, A4-003, A4-004, A3-031, A6H-073, A11R-005, DR-13-01, DR-13-06 | Live's Export Audio/Video dialog first (Rendered Track, Start and Length, Include Return and Main Effects, Render as Loop, Convert to Mono, Normalize, Create Analysis File, Sample Rate, PCM and MP3, Video), ADR-0205 d9's additions below a divider; renders in process: instances created and given state on the message thread, processing on a render worker, serviceHost marshalled; LANES §3b gains the Export item. | none | Two dialogs; the AI pressing render; a render child process (struck by ADR-0203 d8). |
| EX-12 | P2 export abilities | decided | P2 | R11X-022, R11X-026, R11X-029, R11X-030, R11X-031, R11X-032, R11X-033, C4-010, C4-011, DR-13-03, DR-13-10, DR-13-15, DR-13-16 | Matrix cells add ordinary jobs; After Export is a separate undoable import once the files exist; the batch converter feeds file sources through the same post pipeline; live capture is a separate session-owned writer; the .adiviz telemetry shares the export pass. | EX3 | REAPER's keep-partial (P3); a dry run that edits the project outside ordinary ops. |
| IC-01 | The converter framework and DAWproject both ways | decided | M5 | C4-017, C4-018, DR-13-17, M3-EXPORT-INTERCHANGE | A reference to TM-IC-03, the owner: converters JUCE-free in src/adi/interchange, prepare on a worker and commit one step (actor import), a per-field loss report, extension rows for unmapped data, VST3 class-id matching, the neutral clip.createMidi. | IC1, IC2, IC3, IC4, IC5, IC6, IC12 | Tracktion's JUCE glue; .als import (not proposed); DawVert as code. |
| IC-02 | MIDI files, Live's way | decided | P2 | A6H-028 | A reference to TM-IC-02, the owner: Live's Import MIDI File and Export MIDI Clip with no options box (P2); the source PPQ remembered; unknown chunks parked. | IC7 | The harvest's P1 placement; libsmf (glib). |
| IC-03 | OTIO and AAF (P2), OMF read (P3) and the ArtCraft bridge | decided | P2 | C4-016, C4-017, DR-13-17, DR-13-18 | A reference to TM-IC-04, the owner: the integer time base first; AAF import through LibAAF; a clean-room AAF and compound-file export with essence from the queue; an OTIO subset; drop-frame as a new table; .fcproj and .scraft never read or written. | IC8, IC9, IC10, IC11 | OTIO's C++ library; the AMWA AAF SDK; ptformat; an FFI to FilmCraft. |
| PD-00 | Main green on macOS for Save As, the ZIP and NI Stems | decided | M0 | DR-12-05, DR-13-11 | Relative media paths computed between folders spelled as SQLite spells them, so a rebased path resolves from the reopened copy (Save a Copy, Save As, Collect and the ZIP); CoreAudio's AAC priming read from its packet table; then a full run on main's head by workflow_dispatch; ui.yml's cancel-in-progress taken off for main. | none | Skipping or loosening the failing checks; striking N3 or N30 before macOS passes. |
| PD-01 | Save As and Save a Copy | decided | M3 | A6H-068, A6H-069, DR-12-05 | The built semantics stand (a SQLite backup into a stage, media paths rebased on every branch, the WAL folded, no-clobber publication, saveAs switching the same Store). Added: every device snapshotted first (PD-03); the copy's session lock cleared (after PD-05); Live's "replace?" asked by the save dialog, then an atomic replace from the stage. Window: worklist A1 and A4. | R1 | Audacity's SafetyFileName and MoveProject; REAPER's copy-media-on-Save-As options. |
| PD-02 | Collect All and Save; Collect and Export ZIP | decided | M3 | A5C-049, A6H-068, DR-12-05, DR-12-07 | The built semantics stand; each staged copy and the ZIP flushed and their folder synced before publication; Collect and Export on a worker with a read-only connection, progress and cancel; Live's Collect dialog choices on a location filter the store gains; Live's Unused Files with freeze-cache's collector. | R1 | Collect and Embed and .adibundle (superseded); any file copy of the live database. |
| PD-03 | Ctrl+S Take Snapshot, the daily snapshot, plug-in state written | decided | M3 | A4-010, A5C-048, R3-001, R10H-061, DR-12-02, DR-12-03 | ParamOps::snapshotAll writes one device.loadState per attached device whose chunk changed, before Take Snapshot, close, Save As, Save a Copy, Collect and any backup. DailyHistorySnapshots is built and polled only together with Q-A's rule. The silent plug-ins on his machine are counted for ADR-0038. | none | An autosave timer; a periodic quiet snapshot of silent plug-ins until the count shows a need. |
| PD-04 | Atomic, durable writes for every file beside the project | decided | M3 | A11R-006, R11X-011, R11X-040, A5C-049 | One primitive pair in publish_file: publishFile (no-clobber) and replaceFile (rename-over); both flush the staged file before the rename (F_FULLFSYNC on Apple) and sync the folder after; Windows MoveFileExW with capped retries on sharing and lock errors; stages named .adi-<kind>-<session>-<n>, listed in the session marker and swept only after taking a dead session's lock; the settings bundle never truncates the user's file. | R1 | JUCE's TemporaryFile and ReplaceFileW; unflushed .tmp-then-rename (VCV Rack, Ardour); Tracktion's temp edit copy. |
| PD-05 | Crash detection, the recovery notice and reopening | decided | M5 | A5C-048, A4-010, A5H-034, C5W-029, DR-12-01 | An OS-locked session marker per running instance (session.json: project path and UUID, stages, recordings in flight, clean exit); the session_lock row with a 20-30 s heartbeat; Store::open returns an OpenReport (unclean, ops since a clean close, WAL bytes, last op, writer); a live lock opens read-only naming host and PID; copies clear the lock; modified_by on every write; one launch question; holding Alt at launch skips the scan. | R2 | FilmCraft's snapshot journal (every op is already in the file); Live's version-specific replay. |
| PD-06 | Recover a recording that was running at a crash | new-choice | M5 | A5C-049, DR-12-01 | The director's Q-I. If yes: a take.json plan written at record start; a RIFF or RF64 header repair only for the recorder's own files in a record folder; recovery is Stop's prepared batch, one undo step, carrying the recording id so it never places twice; refused on a read-only project; the launch question names the recovered takes. | R3 | "Idempotent because media is content-addressed" (the recording id is what makes it idempotent). |
| PD-07 | Live's Backup folder | new-choice | M5 | A5H-014, DR-12-16 | The director's Q-A (with HI-02). If yes: from the second Take Snapshot a worker writes a consistent copy to <project>/Backup/<name> [date time].adi; ten kept, ordered by a UTC time stored in each copy; the eleventh to the Recycle Bin; copies clear the session lock; media stay referenced, as Live's. | R7 | Deleting the oldest (Live uses the trash); any timer-driven copy (his no-timer directive). |
| PD-08 | Back up project to cloud, and a project kept in a sync folder | decided | P2 | DR-12-16, A5H-014 | File > Back Up Project... asks for the folder each time, runs Collect and Export on a worker with progress and cancel, and writes "<name> [date time].zip" without replacing anything; a project kept inside a known sync folder gets one Status Bar line. | R1 | A scheduler or stored credentials (A6-08); S3 sync (that is collaboration). |
| PD-09 | File too new, and the next major | refinement | M5 | A6H-069 | A newer-major .adi opens read-only with a banner naming the writer and offering Save a Copy; 1.x to 2.0 follows Live (opens read-only, Save As writes the 2.x file); plugins.sqlite and library.sqlite adopt major.minor; every refusal names the version found, the version supported and the writer. | R4 | Mixxx's release-string upgrade chain. |
| PD-10 | Schema minors stay whole tables | refinement | M0 | none (no bundle row) | Every format change is a whole new table in a minor, classified for the digest and the text projection; anything that changes an existing object needs ADR-0144 d3's own mechanism and ADR; pending minors kept in one register by win, each number given when its engine PR opens. | none | A column added in a minor without that mechanism (the role column, a drop-frame column). |
| PD-11 | Settings file safety and migration | refinement | M3 | C5W-029, C4-015, DR-12-18 | Saves through replaceFile with a .bak of the last good file, a re-read before writing (a hand edit is adopted), a 500 ms debounce and backoff on failure; reset() erases the key; a step migration runner; the bundle import checks its manifest; a newer schema shown but never written over (after reading how Live carries its Preferences). | R4 | toml_edit's comment preservation; Mixxx's upgrade chain. |
| PD-12 | Durability under power loss | refinement | M5 | DR-12-01 | A BENCHMARKS row first: commit p50 and p99 on the owner thread with NORMAL and FULL and a near-full WAL, on the director's Windows workstation. FULL everywhere if it fits the frame budget; otherwise NORMAL stays and SPEC states the window. FULL on removable and network volumes now; checkpoint_fullfsync on Apple; DEFENSIVE on Store connections; a checkpoint thread only if p99 demands it. | R6 | Audacity's checkpoint thread (R6) before the measurement. |
| PD-13 | Salvage a damaged `.adi` | refinement | M5 | DR-12-01 | adi_tool salvage <in> <out>: open read-only, recover into a private stage, run checkProject, report the rows sent to lost_and_found, publish without replacing anything; a quickCheck after an unclean open; DEFENSIVE on ordinary connections; no window offer until he asks. | R5 | Any repair in place (ADR-0033); a Salvage button now. |
| PD-14 | Templates and the default Set | decided | M5 | none | A template is an .adi written by saveCopy into the User Library's Templates folder with a flag row in adi_meta (no schema change); New from a template starts at its state as the root, not as an undo step; Save as Default Set sets files.startTemplate. | none | REAPER's track and project templates as a separate system. |
| PD-15 | Project tabs and paste between projects | refinement | P2 | R3-002, A4-011, A6H-067, DR-12-04, DR-12-06 | As ADR-0068 d1 to d5: one Store per project, one active, paste between them as one transaction with remapped ids, blobs by hash; Live's import of tracks from another Set through the browser is the parity part; the historical read-only tab is this host's read-only case; a FEATURES row at P2 (win's placement). | none | Keeping inactive tabs' engines warm (ADR-0111 d1); REAPER's subprojects. |

### 2.6 Control, AI, surfaces and testing (55 rows)

| ID | Need | Kind | M | Research IDs | Taken (the contract, in short) | Harvest takes | Rejected or deferred |
|---|---|---|---|---|---|---|---|
| AGT-01 | The changeset as a service, with the decided caps | decided | Track C | DR-14-02, DR-14-12 | maxOps defaults to 6000 and follows agent.maxOpsPerRequest; agent.maxOpsPerMinute enforced on Apply commits; each changeset gets an id, a client and a request id (one queue entry per request id); an app-scoped ChangesetQueue, never in the .adi; Propose previews and queues; Apply applies unless an always-confirm rule fires; a stale changeset is refused, never rebased. | A4 | Silent rebase; per-op editing (open). |
| AGT-02 | Observe tier, the levelled projection and a stable project identity | refinement | Track C | DR-14-04, DR-14-03, A6P-004 | project_read {level 0 to 3, scope} returns {text, level, head} on a second, read-only connection (bars, beats and note names; level 3 is AGT-07's analysis); the Store writes a random project_uuid at create and at the first writable open of an older file; Save As keeps it, and so do Save a Copy and Collect (win's ruling: the same project for consent). | none | Dumping SQL rows or the whole canonical text at a model. |
| AGT-03 | The consent ledger | refinement | P2 | DR-14-05, DR-14-15, DR-14-02 | A disclosure names kinds and sizes, never contents (parameters alone still count); records keyed by app, project_uuid, connection, destination and capability, with terms (this time, an hour, a day, until revoked); a new destination or capability asks again; a fail-closed 256 KiB file; loopback never asks (judged by the parsed address); an external MCP host's read is recorded per project; refusal is ConsentRequired. | A3 | The harvested Disclosure that skips consent for parameter-only requests; a quit-scoped term. |
| AGT-04 | Jobs a client can watch, cancel and trust after the project moves | refinement | M5 | DR-14-12, DR-14-06, A11R-001, R10H-079 | One legal state machine (progress monotone, terminal states final) wrapping render::Queue and the other workers (analysis, stem separation, remote services); owner-scoped cancel; retention 128 jobs or 10 minutes; a job that commits into the project declares its read set and is refused as stale only when those inputs changed (refused, never rebased); renders keep ERRATA-11's four moments. | A4 | Enqueue-time snapshots; float cost fields; staleness on any movement of the head. |
| AGT-05 | The user's side: review, consent, chat and entry points | decided | P2 | DR-14-02, DR-14-03, DR-12-10 | A review panel (a unified diff and a per-op before and after list; Apply, Reject, a stale banner offering a fresh preview); the consent prompt; chat in the collapsed tab at the foot of the browser; entry points (the palette per Q-G, a marker's prompt run on the user's action, model-backed buttons producing ordinary changesets); agent remarks read as context only; no panel commits on its own. | none | Per-op editing (later); a chat box as the only interface. |
| AGT-06 | The sidecar `adi-agent`, its budgets and its providers | decided | P2 | DR-14-05, DR-14-01, DR-14-13 | pi-agent-core, pi-ai and pi-mcp at exact pins; started on demand as a child process; JSON lines on stdio; tools from adi-mcp, run sequentially; a budget guard (8 steps, 32,000 tokens, 8 mutations, 2 minutes, at most 2 identical calls); a run that read remark text is tainted and queues even at Apply; local, cloud-key and LAN providers, all behind consent; credentials in the keychain. | A5, A6, A7 | Forking OpenClaw; Docker as the default; MAGDA's in-process models and its one-tool-per-op MCP surface. |
| AGT-07 | Analysis tools the agent can call | decided | P2 | DR-14-04, DR-14-08, DR-14-09 | analyze.loudness, spectrum, transients, key, tempo, harmony, groove, masking and headroom as jobs against a WAL snapshot, never blocking edits; per-file analyses read from ADIA (computed on a miss by the same analyser); project-level analyses on MX-30's kernels; output schemas described as ops are. | LD1, LD2, WT1, WT3, MD6 | Raw audio to a model by default (only under the leaves-the-machine confirmation). |
| AGT-08 | The evaluation corpus | refinement | Track C | DR-14-01, DR-14-02 | Fixture projects with tasks whose outcomes are checkable as digest or projection assertions; adi_tool propose and the sidecar with the faux provider in CI, a real model by hand; adversarial fixtures (a track named as an instruction, an agent-addressed remark) must end refused or queued. | none | Judging taste. |
| AGT-09 | Tiers at the boundary: their scope and the always-confirm rules | refinement | M0 | DR-14-02, DR-14-15, DR-12-10 | agent.tier stays the app default; a per-project override in session state may only lower it unless the user raises it; no client raises its own tier; a client's grant is capped at the effective tier; always confirm: a changeset touching more than 20 % of the clips, any egress, any run tainted by remark text. | none | Executing because a remark asked (never). |
| CTL-01 | One control stack: dispatcher, listener, wire, actors and owners | refinement | M0 | R10D-021, DR-14-01, DR-14-02, DR-11-04, DR-11-06, A6P-001, A6P-002, A6P-003 | Win's ADR: one listener (ws://127.0.0.1:<port>/rpc, JSON-RPC 2.0, off by default); /mcp only on the same server and port, adi-mcp over stdio; every project method names its project; request meta (expectHead, deadlineMs, idempotencyKey); actors by who writes the ops; network threads never touch the model; durable integer ids; owners: the engine (src/adi/control, tools/adi-mcp) and the window (src/juce/app_control.*). | CA1, A1, CA4 | MAGDA's two listeners and the src/adi/rpc split; IXWebSocket; MCP and control servers without authentication; anything past loopback as a default. |
| CTL-02 | The registry describes itself (JSON Schema export) | decided | Track C | DR-14-01, DR-14-04 | Field gains optional description, enum values, bounds and item limits (existing initialisers stay valid); opInputSchema() emits closed objects with MAGDA's default bounds; validateSubmission enforces the same metadata at submission only (replay untouched); adi_tool ops --schema; descriptions first for the ops the agent may emit. | OS1 | A second validator (MAGDA's validateJson); hand-written tool schemas. |
| CTL-03 | The control core (JUCE-free) | refinement | Track C | DR-14-02, DR-14-15, DR-11-04 | JSON-RPC errors; constant-time token checks with one refusal text; grants keyed by the paired credential, a new client at Propose or lower, Apply only by an explicit grant under maxOpsPerMinute; surface allow-lists; idempotency by credential and request id; PENDING, TAKEN, ABANDONED deadlines; an expectHead mismatch writes nothing; a 512-event audit ring with secrets redacted. | CA1, A1, CA4 | MAGDA's name-keyed grants, connection-keyed dedupe, dispatch lock across the handler and inline execution. |
| CTL-04 | Transport: one loopback WebSocket | refinement | Track C | DR-11-04, DR-11-06 | cpp-httplib v0.64.0 behind one pImpl unit; 127.0.0.1 only; 401 and 403 before the protocol switch; the Origin checked when present (the allow-list empty by default); caps of 8 connections, 240 KiB frames, 8 requests in flight, 50 a second and a 10 s deadline; one writer thread and outbox per connection; an HTTP POST never dispatched; a clean shutdown. | CA2, CA1 | cpp-httplib v0.52.0 (ten WebSocket fixes missing); IXWebSocket; civetweb; CHOC, websocketpp and uWebSockets. |
| CTL-05 | Where control-API secrets live (CH-2) | new-choice | Track C | DR-11-04, DR-14-15 | Win's placement (was the director's Q-O): a 256-bit session token, shown masked, in an owner-only, non-roaming discovery record inside PD-05's session marker, deleted on exit and swept when stale, granting at most Propose; pairing by a single-use 120 s code; ADI keeps only a hash; clients keep their tokens in the OS keychain (a prebuilt native helper for the Stream Deck); never in the .adi or settings.json. | CA1, CA4, SU2 | A tokenless local mode; a roaming record; Elgato's plain settings store. |
| CTL-06 | Feed subscriptions and the param and transport methods | decided | P2 | DR-11-05, DR-11-06, B3-013, A6P-003, A3-018 | param.subscribe {names} returns a snapshot then per-tick diffs (at most one per parameter per frame); a stalled client is dropped and resynced, and disconnected after 100 refused flushes; param.get, set, begin and end through parameterGesture into ParamOps (one undo per gesture); param.nudge through SRF-04; every write stamps lastTouchedBy; transport, history-head and selection subscriptions. | CA1 | A second feed; deltas in the log. |
| CTL-07 | Visual array streams (P2) | decided | P2 | DR-11-06 | streams.subscribe {name, points, rateHz} returns a stream id; binary frames with a 16-byte header (signed i64 timestamp) and float32 values; names track.<id>.meter and .spectrum, master.correlation, device.<id>.scope; producers serialise MX-30's worker outputs and run no FFT of their own; raw audio a capped stream, marked while held; ui.mixview.focus for the Stream Deck. | CA1 | Arrays as JSON numbers; analysis on the audio thread for a client. |
| CTL-08 | `adi-mcp`, the stdio MCP bridge | refinement | Track C | DR-14-01, DR-14-02, DR-14-03, DR-14-15 | Out of process, stdio, JUCE-free, with two backends (--project, headless over the core; --connect through the discovery record); tools project_read, ops_describe, changeset_propose and changeset_status (jobs later); both MCP protocol eras; an op failure is a tool error carrying ADI's refusal; the instructions say project content is data and nothing commits without the user; consent before a non-local read. | CA3, A2 | A second Streamable-HTTP listener; muse_framework's and Ardour's MCP servers; community C++ MCP SDKs without grants or audit. |
| CTL-09 | The app host and the Clients page | refinement | Track C | DR-14-02, DR-11-04 | The host src/juce/app_control.* starts on control.enabled (rows control.enabled off, control.port 0, control.allowedOrigins empty); the executor is MessageManager::callAsync; each client writes through its own OpSubmitter so every window refreshes; one Settings page (the masked token and pairing code; clients and their grants; connections and refusals; Revoke, Disconnect, Rotate). | CA1 | An embedded script language (scripts are external clients). |
| SRF-01 | The strip's parameter drag is one undo step | decided | M1 | DR-11-05, A6P-003 | parameterGesture calls ParamOps::uiEvent on the steady-clock milliseconds the drain uses, and commits everything appended even when this event was refused (an older gesture may have completed); ParameterFeed::touched(id, param, source) is called; the engine's two caveats stay named. | none | Committing only on release (Live sounds during the drag). |
| SRF-02 | Mixer gestures: audible while moving, one undo step | refinement | M2 | DR-11-05, DR-11-07 | An engine producer takes Begin, Value and End for volume, pan and a send's gain; each Value sets a live value the audio thread reads, with no op; End commits one op whose inverse is the value at Begin; a source with no release closes after a quiet window; the lane is overridden live while touched; it feeds the automation recorder while armed; the mouse and the value keys adopt it. | none | Committing only on release. |
| SRF-03 | Encoder flavours, absolute decoding and the keyboard source in the format | refinement | M3 | A6R-008, A6R-009, A6R-010, A6R-011, A3-018, DR-11-01, DR-11-07 | controller_maps.mode widened through the validator and the SPEC and OPS text (no CHECK change): absolute, two's complement, toggle, binary offset, signed bit, signed bit inverted and their linear variants; detection from a slow left turn; 14-bit and note-like CC decoding; Key Map rows as protocol key; midi.focusDialMode widened; Live's type names attached after SF-22d. | SU1, SU9 | A separate encoding column; MAGDA's router write path. |
| SRF-04 | One resolver for nudge, acceleration, conserved ticks and takeover | decided | M3 | A6R-004, A3-018, DR-11-05, DR-11-11 | resolveNudge returns an absolute normalized value, or nothing when clamped; switch and menu parameters move a step per tick; Fine divides by 10; ADR-0188 d4's acceleration only for unit-tick sources; ticks conserved in an atomic per-binding counter; a TakeoverGate (None, Pick-Up, a provisional Value Scaling); relative bindings bypass takeover; one setParam per settle. | SU7, SU8, SU9 | MAGDA's router (it overwrites pending ticks); Mixxx's threshold and time override; helgoboss-learn's Value Scaling (not usable; a hypothesis only). |
| SRF-05 | MIDI ports, the Remote route and feedback | decided | M3 | A3-010, A6R-001, A6R-002, A6R-003, A6U-013, DR-11-01, DR-11-07 | One port host (window, src/juce/app_midi_ports.*) on JUCE 9's UMP endpoints with Windows MIDI Services on and the backend named in Settings; hot-plug, identity healed by identifier then name, per-port Track, Sync, Remote and MPE; the Remote route to the message thread (the Focus Dial first); the Track and Sync route as timestamped lock-free rings into the engine; mapped messages consumed first; win gives midi.port.* a home. | SU10, SU1 | libremidi; the classic MIDI 1.0 device API (ADR-0188 d7 rules UMP). |
| SRF-06 | The Master Focus Dial (P1) | decided | M3 | DR-11-11 | Hover targets native controls, touch targets plug-in parameters; push locks, Shift divides the step by 10; a HUD reads [Device] → [Parameter] → [value], cyan while following, amber while locked; a turn goes through SRF-04 and the capture, one op per settle. | none | A second dial (ADR-0130's open item). |
| SRF-07 | The feed's surface half | refinement | P2 | DR-11-06, DR-11-05, B3-013 | Strip records (id, name, palette index, volume, pan and sends with their text, mute, solo, arm, selected, a meter tap id); transport and selection records; ui.mixview.focus; DevicePanelData publishes the plug-in's identity uid; playing values from the one lock-free slot per target; driven and lastTouchedBy tell modulation, automation and manual input apart. | CA1 | A surface reading the model directly (ADR-0181 d5). |
| SRF-08 | The surface host: six slots, MCU, HUI, profiles, follow, lock and pin | decided | P2 | A6R-002, A3-017, A6R-012, DR-11-03, DR-11-05, DR-11-07, B3-011, B3-012, M3-CONTROLLER | Six App-scoped surface slots (Live's Mackie entries; HUI as ADI's first-class addition; declarative JSON profiles); auto-add; instant mappings as driver state, never rows (ADR-0181 d4's wording amended); Lock to... per surface; MCU and HUI feedback rules; output diffed per tick and sent off the message thread; button meanings measured (SF-22a); no script language now; grid controllers at P3. | SU3, SU5, SU9, SU1, SU7 | Ardour's mackie_control_protocol bindings; Live's MIDI Remote Scripts (not usable); MAGDA's Lua (a later ADR); Push (Q-M). |
| SRF-09 | OSC (P2) | decided | P2 | DR-11-07, DR-11-08 | Off by default, on the interface the user picks, with an optional client list; ADI owns the UDP read loop (juce_osc only encodes); a fixed namespace (/adi/transport, /adi/track/{n}, /adi/master, /adi/focused); writes as user and osc:<client>; feedback on change, never echoed into the flush its value arrived in. | SU1, SU11 | Ardour's liblo surface; liblo, oscpack and tinyosc. |
| SRF-10 | OSCQuery v1 (P2) | decided | P2 | DR-11-08 | HTTP GET only (/, a path, ?HOST_INFO); the tree built from the feed with stable ids, float types, 0 to 1 ranges and ACCESS 3; DNS-SD advertises only while on; values travel as OSC under SRF-09's rules; version 2 (LISTEN and the rest) waits for a transport ruling; FEATURES' TouchOSC and Lemur wording corrected. | SU6 | organicui's CORS * and file endpoints; Servus; a read-only mode; version 2 now. |
| SRF-11 | MIDI-CI property exchange (P3) | refinement | P3 | DR-11-09 | Strike the OS precondition (JUCE runs MIDI-CI over MIDI 1.0 SysEx on plain ports); a ci::Device per Remote port pair on the message thread; a PropertyDelegate serving ResourceList, DeviceInfo, ChannelList and the focused device's parameters from the feed; writes as midici:<muid>; P3 on priority alone. | SU4 | Waiting for the OS MIDI stacks. |
| SRF-12 | TUIO and touch (P3) | decided | P3 | DR-11-10 | Windows touch through JUCE's native multi-touch, one source per finger through the mouse's gesture paths; TUIO 1.1 over OSC on UDP 3333 on the same path, off until opened. | none | Before P3. |
| SRF-13 | The ADI Stream Deck + XL plug-in (P2) | decided | P2 | DR-11-04, DR-11-05, DR-11-06 | A Node plug-in (SDK v2, exact pins, device type 13) carrying ADI's name; pairs once and sends a bearer token; follows the selection or pins; in the Mix View follows the hovered row, the focused row or a CTZ group; dial turns as param.nudge, touch or push as one gesture; the touch strip shows the parameter's text; [REF], solo, page and bank keys; his 13 controllers re-pointed at the API, keyed on plug-in identity, driving his builds as ordinary plug-ins. | SU2 | A browser-page plug-in (no Authorization header; throttled timers); Studio OS's ws-server (no Origin check); per-channel ADI-Q and ADI-L pages (withdrawn). |
| SRF-14 | Where a surface's page comes from | refinement | P2 | B3-011, B3-012, A6R-002, DR-11-03, DR-11-05, M3-CONTROLLER | Live's default (a plug-in's Configure list in banks of eight; a native device's fixed banks); his predefined controllers take precedence for their plug-ins; ADI's addition: Bitwig-style named, editable pages per instance (a side table) and per plug-in identity (app data), never replacing Live's banks (win's placement); one page model for every surface. | none | Bitwig's ninth slot until a surface needs it. |
| SRF-15 | Push 2 and Push 3 (CH-1) | new-choice | P2 | A6U-001, A6U-002, A6U-013, A6R-014, DR-11-03 | The director's Q-M. If yes: a Push 2 and Push 3 controller-mode driver behind SRF-08's host, built from Ardour's display transport and DrivenByMoss's behaviour and checked against Live 12.4, at P2 after the surface host and Session View; Push's User Mode works as a generic MIDI surface either way. | none | Ableton's push-interface document and Live's Remote Scripts (not usable). |
| SRF-16 | The Windows MIDI Services backend (MIDI 2.0 on Windows) | decided | P2 | DR-11-01 | ADR-0188 d7's P2 item as its own row: the service on behind SRF-05's port-host seam, with the SDK pin, the package in a local repository on the Windows CI legs, the installer prerequisite and a Windows 11 seat; nothing claimed from a Windows 10 or Win32 run. | none | Claiming MIDI 2.0 on Windows from a Win32 run; JUCE's older WinRT backend as the default. |
| WIN-01 | The function keys do functions (a defect) | decided | M1 | DR-01-01, A5W-010 | F1 to F8 lose the activator defaults (the eight commands stay, unbound, for the palette and the Shortcuts page); F4 is the Mix View; F3 and F7 are held for Session View and offline processing; parity-7.6 row 6 amended; a golden of every preset's default map with a no-clash check. | WI4 | Live's F1 to F8 defaults, unless he asks for them back. |
| WIN-02 | Every window reopens where it was | refinement | M1 | DR-01-09, C5W-008, C5W-011, M3-UI-WORKSPACE | One placement rule (sanitize; the monitor by area shown; a reachable title bar; clamp; restore proportionally when the monitor changed, on the main display when it is gone; cascade); monitor identity by id, then area, origin, a unique same-size-and-DPI monitor, then the main display; the main window and Settings in an App record window.places, floating windows in ui_view; saved on move and checked again on display changes. | WI1 | Free docking, named workspaces and display profiles (ADR-0203 d5); a "gather windows" command. |
| WIN-03 | Window kinds, owner windows and the plug-in windows | refinement | M1 | DR-01-10, A5W-001, A5W-002 | Owned windows (plug-in editors, device views) stay above the main window only, minimise with it, have no taskbar button; normal windows (the Mix View's, the Second Window); editorsAlwaysOnTop chooses; Esc closes unless a plug-in takes the key; Live's plug-in window commands (the wrench, Ctrl+Alt+P, Multiple, Auto-Open and Auto-Hide, the track's name, cascading); ADI never scales a plug-in window; a generic device-view window kind. | WI2 | Always-on-top as the Linux fallback; placing the unified feature's window now. |
| WIN-04 | Stable command identity and user shortcuts that survive (first half) | refinement | M1 | C5W-010, C5W-015, C5W-016, A5W-009 | Stable text command ids with Live's menu categories (the list only grows); a shortcuts.overrides record mapping ids to canonical chords, re-applied after every reload and compared before writing; a Shortcuts page on JUCE's key-mapping editor in ADI's own look. | WI3, WI7 | Persisting JUCE's numeric ids or per-OS key descriptions. |
| WIN-05 | Shortcut editing, reference presets, non-Latin layouts and key dispatch (second half) | decided | M3 | A5W-015, C5W-015, C5W-016, C5W-017, C5W-018, DR-01-11 | Canonical chord text; foreign key files read; Steal, Keep or Refuse; clash told apart from shadow; reserved system chords refused per OS; named sets that move across OSes; search by name or pressed key; Cubase and Bitwig presets from their documentation; a native physical-key source for non-Latin layouts; a provisional dispatch order; keys typed into a native plug-in editor never reach ADI. | WI4 | Macros (one key running several commands is not one journal op); craft's egui UI. |
| WIN-06 | The command palette (Ctrl/Cmd+P) | decided | M3 | DR-14-03, DR-02-01 | A popup over the focused window from any window; fzy scoring on UTF-32 (Hebrew matches as it is); prefix-routed providers (commands with key hints, disabled ones listed with their reason; V/ for ADI's devices then plug-ins, inserted on the selected track in one step; S/ and P/ once the store exists); free text per Q-G; closes on focus loss. | WI5, WI6 | Helio's lib_fts matcher; setlocale. |
| WIN-07 | Key Map Mode, MIDI Map Mode and the Mapping Browser (P2) | decided | P2 | A6R-005, A6R-006, A6R-007, A6R-011, A5W-012, A5W-013, A5W-014, DR-11-01 | Ctrl/Cmd+K and Ctrl/Cmd+M overlay the assignable controls; one message or key binds through learnControllerTarget; the Mapping Browser lists manual mappings for the active mode (never instant mappings); notes act by target kind; Key Map rows travel in the .adi; Clip View mappings act on the selection. | SU1, WI4 | Per-strip Learn buttons (Live learns through Map Mode). |
| WIN-08 | Live's Second Window (P2) | decided | P2 | A5W-001, A5W-002, A5W-003, A5W-004, DR-01-02, DR-01-10 | Ctrl/Cmd+Shift+W opens it; Tab swaps Session and Arrangement between the windows; Shift+Tab or F12 moves the Detail View; the Browser and the Info View stay in the main window; window.zoom keeps second@<display>; a normal window kind; a FEATURES row at P2. | WI1, WI2 | Before Session View. |
| WIN-09 | The Help menu and Live's menu search | refinement | M1 | A-BASE-2.2.1, DR-01-01, DR-01-12 | The Help menu shown with at least the manual and the keyboard-reference entries, so the Mac's Cmd+? menu search works; the Help View joins later. | none | A lessons engine (later). |
| WIN-10 | Palette free text: commands first, or straight to the agent (CH-4) | new-choice | P2 | DR-14-03 | The director's Q-G. Recommended (a): free text ranks local matches first (commands, devices, later plug-ins, samples and presets) and Enter runs the top one offline; the last row is always "Ask the agent: <text>", acting only when chosen, at the agent's tier under consent; inert, and saying so, with the agent off. | WI5, WI6 | Sending every unmatched string to a model. |
| WTS-01 | Never on the director's desktop: launch isolation and the testing ADR | refinement | M1 | DR-01-12 | Test launches (--smoke-output, --control-test) are multi-instance, require ADI_HOME, never activate their window, report to stderr and script their file choosers; tests run on the CI legs, in process, on the Linux box under Xvfb, and in a Windows Sandbox on the director's Windows workstation only with his OK, never in his session; ADR-0180's two settled items struck. | WT-5, WT-4 | ADI.exe on his live desktop; pywinauto for ADI's own UI (it stays for driving Live). |
| WTS-02 | Test hygiene: the input clock, assertions, repaints and counting | refinement | M1 | DR-01-01 | An InputClock seam (key state and milliseconds) for every key read (keyIsDown, AppCommands::key, the Mix View's [REF], Tap Tempo); a fixed virtual display set; AssertionWatch with JUCE_LOG_ASSERTIONS on the test targets only; windows_no_dialogs in every Windows test binary; repaint invariance, an extreme-zoom paint and a paint census; the JUCE suite counted by test_all.sh. | WT-5, WT-3 | A PUBLIC JUCE_LOG_ASSERTIONS (it keeps jassert live in the release ADI.exe). |
| WTS-03 | Goldens that check text: comparator, glyph masks and the font experiment | refinement | Track C | DR-01-11, DR-01-12 | The font experiment first (one pinned Latin and Hebrew face; each scene recorded per OS as identical, anti-alias only or offset); then a RecordingRenderer storing glyph boxes beside each golden; the ignore mask the union of the boxes; EXACT by memcmp, pixelmatch for the diff image; bless modes; the eight goldens recorded again with their text. | WT-1, WT-6 | pixelmatch at threshold 0 as EXACT; hand-placed masks. |
| WTS-04 | The render review job | refinement | Track C | DR-01-12 | Every leg uploads its renders and a manifest; a PR is compared per OS against its base's renders (found by the first parent; built when missing); reference, current and diff images with a report; the job fails until the PR carries ui-renders-changed and names the scenes; actions: read, no cancel-in-progress on main, the labeled trigger. | WT-2 | Comparing against the base branch's tip; automatic acceptance. |
| WTS-05 | `UiControl`: one description of the window, in process first | refinement | Track C | DR-01-12, DR-01-01 | src/juce/ui_control.* holds the one melatonin copy: ui.windows, inspect, elements and lint; input only through the peer's handlers (click, drag, move, scroll, key, text, drop); ui.invoke, command and focus; frames and screenshots; arrangement hit and locate; replies after the input applied and one frame run; componentID ids; the fake peer records dirty rectangles; the runner inside the dispatch loop; in process first. | CA5, WT-3, WT-4 | Calling mouseDown directly; promising never to take the keyboard on a shared desktop. |
| WTS-06 | The headless script runner | refinement | Track C | DR-01-01 | adi_ui_script (--project or --new) script.jsonl over the fake peer, with no display or audio device and ADI_HOME under its output; requests, shots, frames, expectations and notes; PNGs and a transcript; exit 1 on a failed expectation; ui.yml runs it on three legs; [P:...] parity tags in the scripts. | WT-4 | A queue with synchronous input (a per-request step cap stands in). |
| WTS-07 | Sweeps, and the director's remarks as regressions | refinement | P2 | DR-01-03, DR-01-11 | A dead-control sweep (every click yields a new history head, a view change, a popup, a transport command or a Status Bar line; undoing every step returns the first projection); round-trip and dialog sweeps; layout invariants at three sizes and three zooms; the Mix View's transitions as a filmstrip (R3); a Hebrew-named track right to left; scripts for his remarks R1 to R6. | WT-5 | Building on add5102 (revert #372 on a scratch branch instead). |
| WTS-08 | Accessibility lint and the Windows UIA scan | refinement | P2 | DR-01-11, DR-01-12 | ui.lint rules (unnamed, obscured, zero-size, outside the parent, duplicate id, clipped text); axe-windows' UIA rules ported where they map, and its CLI scanning ADI.exe on windows-latest; a floor that only falls; Linux runs the in-process rules only. | WT-3 | Out-of-process UIA as the main path. |
| WTS-09 | Parity bookkeeping that cannot drift | refinement | M1 | none | tools/validate_parity.py in ci.yml's validator step: row ids, normalised status words, every [P:file#row] tag resolves, the count of untagged built rows only falls, a file's built count never drops without a floor change; optionally a menu catalogue compared with Live's captures. | WT-4 | Parity claimed from manual text. |
| WTS-10 | The running app over the API, smokes everywhere, other desktops | refinement | Track C | DR-01-09 | The ui grant exists only under --control-test (multi-instance, ADI_HOME, a banner), never for a paired surface; synthetic ops carry ui-test:<client>; screenshots and drops confined to declared roots; native smokes on Linux (xvfb-run) and macOS too; an the Linux box runbook driven over loopback from SSH. | CA5, CA4 | xdotool and ImageMagick sessions. |
| WTS-11 | The Live side: L01-L10 captured, never read from the manual | decided | M4 | M-LIVE-UI | The measurement seat captures L01 to L10 in Live 12.4 (Mac-only rows on his Mac); each L-row gets an ADI script; behaviour rows answered from ui.inspect; picture rows on side-by-side sheets at one scale with a 50 % overlay; win relays dated results into the window's "Measure on Live" lines and AWAITING by the relay rule. | none | Striking a row from a manual or a static controller palette. |

## 3. By harvest row: the code ADI takes, and where

The harvest read 28 areas of open code and recorded 208 takes, each verified by its area's
verifier. Code is recorded as usable or not usable, nothing more. "How" is the port mode (port C++,
copy with attribution, vendor as a library, tests only, design only, in tree); "M" is the milestone
where the take's work happens (PLAN §4). Sources are named by project; the ledger holds the exact
files and commits.

### 3.1 M1 (15 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| LD7 | loudness | Readings snapshot with validity flags + seqlock publish; test ideas (block-size invariance, histogram vs exact list within 0.05 LU, concurrent polling, reset) | tracktion_engine | design-only | engine | S | DEV-28, MX-10, MX-27 | M1 |
| MF4 | midi-fx | libpd unbind leak fix | in-house | re-apply by hand | win (or engine per LANES 1) | S | DEV-09 | M1 |
| PD1 | pd-tier | libpd/Pd internals already pinned: per-instance MIDI-out hooks with logical-time offsets (pd_systime per clock callback), lock-free queued console (z_queued.c + ... | libpd | vendor-as-library (already) + copy-with-attribution (PROCESS macro port) + ... | devices (after R3) | M | DEV-09, DEV-14, DEV-15, DEV-11 | M1 |
| PD2 | pd-tier | #A dangling-binding fix + test scenarios (dangling binding, seeded patch-text fuzz, state round trip) | plugdata-pd | copy fix with attribution (overlay on pinned m_pd.c) + port test scenarios | devices | S | DEV-09 | M1 |
| PD4 | pd-tier | Allowed-object marking: recursive compatibility walk through subpatches/abstractions + class enumeration from the running build (vanilla m_imp.h, not fork-only API) + ... | plugdata | port C++ (walk + enumeration onto ADI's tokeniser and own list) | devices | S-M | DEV-09, DEV-16, DEV-10, DEV-11 | M1 |
| RM5 | remarks | Pin clustering (greedy around running centre), open pin stands alone, count bubble, stable author colour | figcraft | port C++ (1-D sweep O(n); author colour into Live's 70 palette indices; agent glyph) | window | S | RE-01 | M1 |
| RM8 | remarks | Typing a remark is one undo step (merged execution) | soundcraft | design-only | window | S | RE-01, RE-02 | M1 |
| WI1 | window-infra | Window placement rule: sanitize, monitorOf by area shown, reachable title bar, keptReachable, clampOnto (min/max), restore (same monitor / proportional on resize / main ... | craft-libs | port C++ (JUCE-free; monitor identity by id, area, origin, then same-size-and-DPI unique ... | window (win: window.places row) | S | MX-14, WIN-08, WIN-02 | M1 |
| WI2 | window-infra | Owner windows for floating views (palette kind above the main window only, minimise with it, activation handed back on close, detach before owner dies) | magda-core-head | copy with attribution (Mac half in ui_owner_mac.mm) | window | S | MX-14, WIN-08, WIN-03 | M1 |
| WI3 | window-infra | Key mapping store lifecycle (store only differences from defaults; restore = defaults then saved diffs) + own-look shortcut panel (categories, reset-to-preset buttons ... | magda-core-head | copy with attribution (overrides keyed by stable text ids in canonical chord text, App ... | window | S | WIN-04 | M1 |
| WI4 | window-infra | Keymap model: canonical chord text (Cmd/Ctrl split folding off macOS), foreign text reader, Steal/Keep/Refuse, conflicts (clash vs shadow, in defaults), overrides kept ... | craft-libs | port C++ in part (not egui ui/, not editor Cancel) | window | M (L with import) | WIN-01, WIN-05, WIN-07 | M1 |
| WI7 | window-infra | Docking concepts only (whole-result checks, palette vs normal kinds, modal drops to normal); keybindings by name corroborate stable ids | craft-libs | design-only | win | S | WIN-04 | M1 |
| WT-3 | window-testing | Accessibility/structure helpers + lint rules: component tree walk, DN role table, unnamed widgets, obscured check; axe-windows UIA rule library (verifier add: ~110 rules ... | melatonin_inspector (the same copy as control-api CA5) ... | copy with attribution (helpers S, UiControl M); axe-windows rules ported as ui.lint rules ... | window | S/M | WTS-02, WTS-05, WTS-08 | M1 |
| WT-4 | window-testing | Control protocol + runners for scripted UI: element ids, synthetic input, screenshots, JSONL shot logs, parity floor, scripted driver that never steals the keyboard | gridcraft | design-only | window | M | WTS-01, WTS-05, WTS-06, WTS-09 | M1 |
| WT-5 | window-testing | Click-through/coverage/dialog sweeps + repaint invariance + assertion watch + no-dialog guard + extreme-zoom paint | plugdata | port + copy with attribution (JUCE_LOG_ASSERTIONS=1 on the TEST targets only - adi_app ... | window | S | WTS-01, WTS-02, WTS-07 | M1 |

### 3.2 M2 (18 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| AU1 | automation | Guard-point policy + write-pass lifecycle (evaluate old curve at boundary, held value moves last point, locate/stop/reverse split the pass, toggles latch, one undo per ... | ardour | port C++ (policy only, pure functions over sorted vector; no RWLock/watcher thread) | engine | M | AU-01, AU-02, AU-03, AU-05 | M2 |
| AU7 | automation | Automation follows clips (Lock Envelopes off) | tracktion_engine | port C++ (design on ADI's time-edit split) | engine | M | AU-05 | M2 |
| HI1 | history | Gesture dispatcher contract: submit makes then updates, different target commits the stale one first, commit pushes without second apply, rollback, rollback_without_undo ... | ossia-score | port C++ (contract + edge cases onto ADR-0148 preview; hard refusal is ADI's) | engine | M | HI-01, MX-23, MX-36 | M2 |
| HI2 | history | Preview-from-origin rule + version-history behaviour tests | figcraft | port tests C++; design for preview | engine | S | HI-01, HI-04, MX-23 | M2 |
| HI3 | history | Undo groups across calls + sequence fuzzer (undo-all = start, redo-all = end, shrinking) | gridcraft | port tests + fuzz harness; design-only group API (pending batch, never a held SQLite txn) | engine | M | HI-02 | M2 |
| HI4 | history | Undo History list model (rows, current index, undoRedoToIndex, redo rows dimmed) | audacity | design-only | window | M | HI-03 | M2 |
| HI5 | history | Revision tree view (Buchheim layout, connectors, head mark), checkout any node, cherry-pick from past revision | helio-sequencer | port C++ (already JUCE) | window | M | HI-03 | M2 |
| HI6 | history | Merged gesture at a full history costs one step | soundcraft | port tests | engine | S | HI-01, MX-23 | M2 |
| HI7 | history | Editing-step rules: one log owned by outermost step, cancel reverts own changes only, empty step commits nothing, undo off while open, idle debounce for ... | openDAW | design-only | win | S | HI-01, MX-23 | M2 |
| HI8 | history | Drag framework Esc contract (abort, finished(movement_occurred)) | ardour | design-only | window | S | HI-01, MX-23 | M2 |
| ME1 | midi-editing | NoteDiffCommand shape (field changes old/new, added, removed, side-effect removals, undo order) + same-key overlap classifier and merge policies + MidiOperator pattern ... | ardour | port C++ (shape; ADI records every side effect, classifies by intervals, key-only ... | engine | M | MI-01, MI-02 | M2 |
| ME4 | midi-editing | JUCE piano roll mechanics: grid drawing, move/resize, ghosts on copy-drag, rubber band, velocity lane + ramps, CC lane, pitch fold map, MPE pitch-glide overlay with ... | magda-core | design-only (not its snap rules, not its slice curve copy) | window | L | MI-03, MI-08 | M2 |
| RE1 | range-edit | DAW edit algebra over sorted non-overlapping clips: place = clear then insert, put_clips clears all destinations first, shuffle ripple bounded by locked clips with ... | soundcraft | port C++ into a pure planner (not its MIDI-drop-on-split policy) | engine | M | ED-01, ED-03, ED-04, ED-05, ED-06 | M2 |
| RE2 | range-edit | Timeline edit algebra: overwrite, insert, razor, lift/extract, ripple delete with sync-lock conflicts naming the track, handle-clamped trims, roll/slip/slide ... | filmcraft | port C++ into the same planner (conflict model, not Premiere defaults) | engine | M (L with roll/slide) | ED-05, ED-07 | M2 |
| RE3 | range-edit | Live-model region overlap resolver (delete/separate/trim start/end right to left over joined masks), trimStart keeping loop offset anchored, cuts keeping loop offset ... | openDAW | port C++ | engine | S | ED-01, ED-03, ED-04, ED-06 | M2 |
| RE4 | range-edit | Option sets: ripple scope (Selected/All/Interview), Insert/Remove Time options (intersected Leave/Move/Split, all playlists, markers, locked markers, tempos), left-trim ... | ardour | design-only | engine | S | ED-04, ED-07 | M2 |
| RE5 | range-edit | Ripple/paste taxonomy (DeleteBehavior, CloseGapBehavior, PasteBehavior) + trimOrDeleteOverlapping 4 cases + mixed-rate sample-boundary tests | audacity | design-only + tests (Live's leave-gap stays default) | engine | S | ED-01, ED-03, ED-07 | M2 |
| RE7 | range-edit | Affected-object scope as a bit mask (Clips, Automation, Loop, Punch, Markers, TempoMap) + per-track insert/remove range | qtractor | design-only | engine | S | ED-02, ED-03, ED-08 | M2 |

### 3.3 M3 (24 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| R1 | atomic-recovery | Atomic replace primitive: stage, sync (F_FULLFSYNC on Apple), rename with Windows retry (5/32/33, 7 retries 10 ms doubling cap 200 ms), dir fsync, fault-injection trait | craft-libs | port C++ | engine | S (M with call sites) | PD-01, PD-02, PD-04, PD-08, EX-07 | M3 |
| R4 | atomic-recovery | Settings store policy (.bak, re-read before save, debounce 500 ms, backoff retry, newer schema read-only) + step migration runner + TooNew{found,supported,generator} | craft-libs | port C++ | engine (+window Wd4) | M | PD-09, PD-11 | M3 |
| LS1 | link-sync | Ableton Link core: BasicLink (enable, numPeers, tempo/start-stop callbacks, realtime-safe capture/commitAudioSessionState, SessionState ... | Ableton/link Link-4.1.1 | vendor as library (third_party pin; one TU, _WIN32_WINNT=0x0A00 ... | engine | L (vendor S, adapter M ... | TM-LS-01 | M3 |
| LS4 | link-sync | Link chase policy (half-bar jump threshold with 250 ms inhibit, varispeed bound), cycle-aligned start; MTC reader chase policy | tracktion_engine | design-only (not its MTC generator bug, not getTempoInRange) | engine | S | TM-LS-01, TM-LS-03 | M3 |
| LS5 | link-sync | Minimal per-block Link adapter shape (hylia_process → {bpb, bpm, beat, phase, playing}, set_output_latency) | Carla | design-only (int64 sample time) | engine | S | TM-LS-01 | M3 |
| LS6 | link-sync | JUCE-side Link + MIDI clock module ideas (offset ms, +/- beat buttons, 48-pulse warm-up) | BespokeSynth | design-only (SPP bug: do not copy) | window | S | TM-LS-01, TM-LS-03 | M3 |
| MD1 | media | ISO BMFF / QuickTime demuxer: header-only open (moov/moof, never mdat), trak/stbl tables, edit offsets, fragments (tfhd/tfdt/trun), sample entries v0/v1/v2 incl. ... | craft-libs | port C++ (ASC wins for AAC channels/rate; add iTunSMPB with FFmpeg mov.c 5851-5872 bounds ... | engine | L | TM-MD-01, TM-IC-05 | M3 |
| MD2 | media | Decoder acceptance suite + fixtures (one audio in every format; first-sound frame; seek/skip/read-beyond-end; wrong suffix; free-mode garbage) | mixxx | copy with attribution (fixtures into tests/fixtures/decode/mixxx/ with README; compare ... | engine | S | TM-MD-01, TM-IC-05 | M3 |
| MD3 | media | In-process ALAC decoder (bit-exact on every leg) | macosforge/alac | vendor as library (or port) | engine | S-M | TM-MD-01 | M3 |
| MD4 | media | Ogg Opus in process (pre-skip) | xiph/opusfile | vendor as library (shared with export's test-only opusfile) | engine | S | TM-MD-05 | M3 |
| MD5 | media | Long tail via external ffmpeg/ffprobe (probe JSON fields, f32le streamed from a child process) | ardour | design-only (own ffmpeg_tool + scan_process streaming mode) | engine (win: settings row + ruling on ... | M | TM-MD-05 | M3 |
| MD6 | media | Key detection: constant-Q chromagram (36 bins/octave) correlated with major/minor profiles; Mixxx global-key reduction + downmix helper | qm-dsp | vendor as library (adi_qmdsp shared with warp-tempo) + port Mixxx wrapper (~150 lines) | engine | S (after warp-tempo step B) / ... | TM-MD-04, AGT-07 | M3 |
| MD7 | media | Embedded tag reading (ID3v1/v2, APE, MP4 ilst incl. freeform, Xiph comments, RIFF INFO, WAV id3, AIFF, Matroska) + Mixxx field map + key-text parser ... | TagLib | vendor TagLib (read-only, options trimmed, static) + port Mixxx rules (std::regex) | engine | M | TM-MD-04 | M3 |
| MD8 | media | ACID/smpl/inst/cue/bext/INFO metadata from already-vendored decoders; Vorbis comments; Xing frame count | dr_libs (wav-0.14.5) | use (already vendored; separate dr_wav metadata open since WavReader path skips dr_wav) | engine | S | TM-MD-04, EX-08 | M3 |
| MD9 | media | Apple Loops basc chunk (beats, root, meter, loop type) | libsndfile | port C++ | engine | S | TM-MD-04 | M3 |
| MD10 | media | Sound Similarity / Auto Tags / CLAP encoders + file-name BPM/key rules + user-over-detected | magda-core | design-only (reference for Live 12 3.4 Similarity Search, 41.23 Similar Sample Swapping ... | engine | L later | TM-MD-04 | M3 |
| SV2 | session-view | Launch quantization table + next position through bars/beats + clip source range mapper (loop wrap split, progress) + audible launcher test suite | tracktion_engine HEAD | port C++ (table + mapper) + tests restated vs Live; runtime design-only | engine | M | TM-SV-01, TM-SV-03 | M3 |
| SV5 | session-view | Musical grid rules across meter changes (whole-bar grids counted from last signature change; sub-bar restart at bar lines; half-open intervals; one tick-to-sample map ... | libossia | port C++ (rules ~80 lines) | engine | S | TM-SV-01 | M3 |
| SV6 | session-view | Live's API code lists + state vocabulary (fired vs playing, triggered scene, has_stop_button, back_to_arranger) | AbletonOSC | design-only (copy code tables with attribution into ADR + OPS) | win | S | TM-SV-01, TM-SV-02, TM-SV-04 | M3 |
| SU7 | surfaces | Pick-Up crossing rule (+ host move re-arms) + driver interface checklist | tracktion_engine | copy with attribution (rule) + design (interface) | engine | S | SRF-04, SRF-08 | M3 |
| SU8 | surfaces | Soft-takeover truth table + test matrix | mixxx | tests only (crossing vectors) | engine | S | SRF-04 | M3 |
| SU10 | surfaces | MIDI device hot-plug notifications | JUCE 9.0.2 (in tree) MidiDeviceListConnection::make | use (already vendored) | window | S | SRF-05 | M3 |
| WI5 | window-infra | Command palette: prefix-routed providers, root list advertising prefixes, popup (Enter runs, Esc dismisses, height fits), commands provider with key hints | helio-sequencer | copy with attribution (string prefixes, longest wins; all commands listed incl. disabled ... | window | M | WIN-06, WIN-10 | M3 |
| WI6 | window-infra | Optimal fuzzy matcher (two-row DP, positions for underlining, fzy scores incl. slash/word/capital bonuses) | fzy | port C++ to UTF-32 (JUCE-free lowercase table; Hebrew matches as is) | window | S | WIN-06, WIN-10 | M3 |

### 3.4 M4 (11 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| LD1 | loudness | Streaming BS.1770-4/R128 meter: M/S at 100 ms (move to 10 ms grid), gated I, LRA at 10 Hz (Tech 3342), max M/S, 0.01 LU exact-energy histograms (-70..+20), fixed memory ... | filmcraft | port C++ (10 ms sub-blocks; -inf before warm-up; explicit channel weights; clamp before ... | devices | M | DEV-28, MX-27, EX-05, AGT-07 | M4 |
| LD2 | loudness | Director's C++ loudness meter: M/S/I, LRA 10 Hz, max M/S, recording flag freezing I/LRA/maxima (EBU stand-by), explicit LFE index, 0.01 LU energy histograms with running ... | ADI Audio | port/merge (C++ base candidate; must be committed by the clones lane first; add TP, 10 ms ... | devices (clones lane commits source) | M | DEV-28, MX-27, EX-05, AGT-07 | M4 |
| LD3 | loudness | Independent test-only oracle (I, max M/S, TP; _multiple for common-gain/one-long-file semantics) | libebur128 v1.2.6 | vendor as library, TEST-ONLY (never shipped; warnings off for that TU) | devices (win: row) | S | DEV-29, DEV-28, MX-27, EX-05 | M4 |
| LD4 | loudness | Tech 3341 v4 + Tech 3342 v4 minimum-requirement signals SYNTHESISED in C++ (cases 1-6, 9-23; LRA 1-4) | EBU Tech | design-only (synthesise) | devices | M | DEV-28, MX-27, EX-05 | M4 |
| LD6 | loudness | 10 Hz LRA oracle via CLI (ebur128 filter metadata print) | ffmpeg | design-only + optional Linux CI oracle (ADI_REQUIRE_ORACLES=1) | devices | S | DEV-28, MX-27 | M4 |
| MS1 | measurement | Sweeps, impulse/sine/seeded noise, sub-sample IR delay (Tamim-Ghani), regularised deconvolution (RegularizedSpectrumInversion), fractional time_shift, windows ... | pyfar | vendor as library (pinned in tools/measure/requirements.txt; Python >= 3.11) | win | S | MX-03, MX-22, MX-33 | M4 |
| MS2 | measurement | Realtime-safe sub-sample round-trip latency meter (13 phase-locked tones, polarity, error figure) + caller logic (no signal, peak > 0.707 refused, err > 0.3 invert and ... | ardour | copy with attribution (~150 lines; fix both peak slips) | engine | S | MX-09, MX-33 | M4 |
| MS3 | measurement | Live 12 driver on Windows (app start/connect by pid, native menus incl. check states, UIA Save dialog, keys) | pywinauto | vendor as library (pinned commit; Windows only) | win (GUI seat) | M | MX-22, MX-33 | M4 |
| MS4 | measurement | Sparse spike-pattern alignment + LatencyTester (device loopback) | tracktion_engine | copy with attribution as port (seed spikes AND noise bed; wait-free handoff instead of ... | engine | S-M (needs a fake loopback ... | MX-09, MX-33 | M4 |
| MS5 | measurement | Distances for non-nullable comparisons: ESR, DC, SI-SDR, SD-SDR, multi-resolution STFT, sum-and-difference | auraloss | port formulas to numpy (~60 lines) + ESR/SI-SDR into C++ residual core | win + engine | S | MX-03, MX-22, MX-33 | M4 |
| MS6 | measurement | Synchronised swept-sine nonlinear identification (Hammerstein kernels), reverberation times | lsp-dsp-units | design-only now | devices | L later | MX-33 | M4 |

### 3.5 M5 (30 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| A4 | agent | Job manager (bounded legal state machine, revision policy StableUntilCompletion/CheckAtStartOnly, owner-scoped cancel, retention 128/10 min) | magda-core head | port C++ | engine | M | EX-01, EX-09, AGT-01, AGT-04 | M5 |
| R2 | atomic-recovery | OS-locked session marker (dead vs live session) + SPEC 3.6 session_lock heartbeat + OpenReport{unclean, opsSinceCleanClose, walBytesAtOpen, generator} | filmcraft | port C++ (marker only; .adi is the journal) | engine | M | PD-05 | M5 |
| R3 | atomic-recovery | Recover a recording running at a crash: take plan json + header repair + place once as one undo step | soundcraft | port C++ | engine | M | PD-06 | M5 |
| R5 | atomic-recovery | SQLite recover extension: `adi_tool salvage` | sqlite version-3.49.2 (1b571af1) | vendor as library (third_party/sqlite-recover, SQLITE_ENABLE_DBPAGE_VTAB ... | engine | S | PD-13 | M5 |
| R6 | atomic-recovery | Checkpoint thread off the committing thread, "disk full" once | audacity | port C++ only if BENCHMARKS commit p99 row demands | engine | M | PD-12 | M5 |
| R7 | atomic-recovery | Backup rotation with DST-safe names | filmcraft | port C++ | window | S | PD-07 | M5 |
| EX1 | export | FLAC + MP3 (then Ogg Vorbis, Opus, single-file AAC) encoders behind an audio::Encoder seam | xiph/flac | vendor codecs as libraries (third_party, pinned, --build-only) + port adapters (~150 ... | engine (rows: win, grant: director) | M (FLAC S, MP3 S, Ogg S, Opus ... | EX-04 | M5 |
| EX3 | export | Export job as plain data (JSON schema), stem dependency closure (sidechain, aux-send returns, rack co-instances, cycle-safe), per-track batch with unique names, tail ... | tracktion_engine | port C++ (algorithms; ADI adds unknown-key rejection + case-folded names); design-only ... | engine | M | EX-01, EX-02, EX-03, EX-12, EX-07, EX-09 | M5 |
| EX4 | export | True-peak brickwall: stage order + upsample-and-hold idea with latency trim/flush | ardour | port C++ (extend ADI's dsp::LookaheadLimiter to double + true-peak target); design-only ... | devices (+engine wiring) | M | DEV-29, MX-27, EX-05 | M5 |
| EX5 | export | Noise-shaped dither for Live's Pow-r slots | sox | copy tables with attribution + port loop (reserve headroom) | engine | S | EX-06 | M5 |
| EX6 | export | BWF v2 (loudness fields, EBU R99 originator ref, coding history), cue markers, time reference split | libsndfile | port C++ (layouts + fill rules; own LIST/adtl/labl writer; seeded random + injected clock) | engine | S | EX-08 | M5 |
| EX7 | export | Export sample-rate conversion as Live does it (SoX Resampler, Live Audio Fact Sheet 38.3.2) | soxr | vendor as library | engine (grant: director) | S-M | EX-04 | M5 |
| EX8 | export | Tag read/write oracle (ID3v2, FLAC/Xiph comments, MP4 ilst + NI stem atom, WAV INFO) | TagLib | vendor for tests (oracle) | engine | S | EX-08 | M5 |
| EX9 | export | AIFF-C fl32 writer pieces | soundcraft | port C++ | engine | S | EX-04 | M5 |
| FC1 | freeze-cache | Recipe-named render cache lifecycle: renders named by recipe hash, nothing evicted except explicit delete so undo finds the old key, .part then rename, hash version ... | filmcraft | design-only (ADI's own ~150 lines, BLAKE3 over canonical CBOR; reject FNV-1a + weak media ... | engine | S (design) / M (impl) | TM-FC-03 | M5 |
| FC2 | freeze-cache | Prefix-hash render chain (stage i hash folds 0..i; skip stages with existing files), render single-flight per key, freeze point (prefix freeze) | tracktion_engine | port C++ (chain + single-flight, BLAKE3 keys, stage-0 key folds source media hash) ... | engine | M (L with ADR-0069 schema) | TM-FC-01, TM-FC-02, TM-FC-03, TM-FC-04 | M5 |
| FC3 | freeze-cache | Three contracts in code: Consolidate Range (no processing) vs with processing vs Bounce vs Freeze; freeze stops at first processor it cannot reproduce; two-phase "dead" ... | ardour | design-only (never its _history.clear()) | engine | S | TM-FC-02, TM-FC-03 | M5 |
| FC4 | freeze-cache | Consolidate + commit reference: render_clips (clip FX, no mixer, skip muted), MIDI consolidate merges as it sounds, commit_track pre-fader with mixer copy | soundcraft | design-only + tests | engine | M | TM-FC-01, TM-FC-02 | M5 |
| FC5 | freeze-cache | Real-time capture pass for external hardware inside an offline render | magda-core | design-only | engine | M later | TM-FC-04 | M5 |
| IC1 | interchange | DAWproject reader/writer (element walk, mapping helpers: velocity/pan/gain/controller normalisation, IDGenerator, IDRefResolver; ZIP audio/ + plugins/) | tracktion_engine | port C++ off JUCE onto pugixml + miniz; emit OpRequests (stem_import shape); fix listed ... | engine | L | TM-IC-03, IC-01 | M5 |
| IC2 | interchange | Second DAWproject codec with declared-loss table per feature + in-process XSD validation; VST3 state as .vstpreset; Sends, automation Points, Warps, built-in ... | magda-core | port C++ beside IC1 (loss table = C4-017 loss report) | engine | (in L) | TM-SV-02, TM-IC-03, IC-01 | M5 |
| IC3 | interchange | Nested-timeline flattener (recursive Clips, trim to parent window, loop unrolling, timeUnit inheritance) + Vst3Preset + CLAP state | git-moss/ProjectConverter | port C++ | engine | M | TM-IC-03, IC-01 | M5 |
| IC4 | interchange | Conformance oracle: Project.xsd + MetaData.xsd, DOM semantics, DawProjectTest scenarios (14 audio scenarios, CC1/pitch bend on track and in clips, all-features) ... | bitwig/dawproject | copy XSDs + fixtures with attribution (fetched test data, not committed) | engine (win: rows) | M | TM-IC-03, IC-01 | M5 |
| IC5 | interchange | XML parser | pugixml (release tag) | vendor as library (third_party, pinned) | engine (win: pin row) | S | TM-IC-03, IC-01 | M5 |
| IC6 | interchange | VST3 .vstpreset container (chunk IDs + 32-char class ID) | VST3 SDK in JUCE | port C++ (container subset) | engine | S | TM-IC-03, IC-01 | M5 |
| IC8 | interchange | SMPTE timecode DF/NDF + OTIO JSON subset + sample data | OpenTimelineIO | copy vectors + design (integer reimplementation; OTIO subset over nlohmann) | engine | M | TM-LS-03, TM-IC-01, TM-IC-04, IC-03 | M5 |
| IC12 | interchange | openDAW DawProjectImporter heuristics (primary output detection, Bitwig sends without enable) + DawProjectTempo.test.ts | openDAW | design-only | engine | S | TM-IC-03, IC-01 | M5 |
| IC13 | interchange | Live's 70 palette colours as RGB | DawVert | reference data (cross-check vs window lane's measurement; ADR-0211 d3) | win/window | S | TM-IC-03 | M5 |
| LD5 | loudness | Export loudness refine loop (measure, gain, TP limiter, re-measure, <= 4 trials, +/-3 dB steps, 0.02 LU plateau) + true-peak detector for the look-ahead limiter | filmcraft | port C++ (TP detector path into ADI's own LookaheadLimiter; refine loop into ... | devices + engine | M | DEV-22, DEV-29, MX-27, EX-05 | M5 |
| RE6 | range-edit | Scope expansion (EditSpaceTargets: clip tracks + all sub-tracks + their automatables), close-gap parameter, trimAwayOverlap, mergeMidiClips (MIDI half of Consolidate) ... | tracktion_engine | design-only (not auto-unfreeze on delete) | engine | S | ED-06, ED-07, ED-08, AU-05 | M5 |

### 3.6 M6 (31 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| AU2 | automation | Splice shape: mergeCurve (leaveOpenAtStart/leaveOpenEnded/fadeLength → GuardPolicy) | tracktion_engine | port C++ (not its fade maths) | engine | (in AU1) | AU-02, AU-03 | M6 |
| AU3 | automation | Recording lifecycle: gesture vs value source, punch in/out, loop-wrap split, flush | tracktion_engine | port C++ | engine | M | AU-03, AU-09 | M6 |
| AU4 | automation | Clip modulation maths + timing: absolute/relative/scale, linked/unlinked loops | tracktion_engine | port C++ (maths+timing) + design (Live pan law, domains) | win (ADR) + engine | L | AU-06, AU-07 | M6 |
| AU5 | automation | RDP simplifier (Simplify Envelope core) | magda-core | copy with attribution (RDP) + own curve fit | engine | M (RDP S) | AU-08 | M6 |
| AU6 | automation | Single-pass error-bounded thinning at drain time + write_range anchors | soundcraft | port C++ (keep coincident pairs as anchors) | engine | S | AU-03, AU-08 | M6 |
| AU8 | automation | Recorder test vectors: latch until stop, loop wrap close/reopen, suspend on write, RDP never drops steps | openDAW | tests only (as vectors) | engine | S | AU-03 | M6 |
| CP1 | comping | Capture passes over one source: CaptureInfo per loop pass (start, samples, loop_offset), punch clipping by 5 overlap cases, one region per pass over the same file | ardour | port C++ (bookkeeping only; preallocated pass log, no lock/alloc on callback) | engine | M | CT-02 | M6 |
| CP2 | comping | Take-window arithmetic (closed-form pass windows, partial last pass) | qtractor | port C++ as oracle (adi::recording::loopPassWindows); drop zero-length last take | engine | S | CT-02 | M6 |
| CP3 | comping | Loop-take splitting at the engine's wraps + stopping cases + MIDI late keys | openDAW | design-only + test scenarios | engine | S | CT-02, CT-03 | M6 |
| CP4 | comping | MIDI pass boundaries: held notes closed at pass end, lane per discontinuity, one Record macro | zrythm | design-only + tests | engine | S | CT-03 | M6 |
| CP5 | comping | Comp-section span algebra (assignCompSections, remapCompSpansAfterDelete) as planner/test oracle | magda-core | port C++ to int64 ticks (oracle, never the model) | engine | S | CT-01, CT-04 | M6 |
| CP6 | comping | Comp-edge drag rule (clamp, covered sections removed, beyond-edge section keeps take) | tracktion_engine | design-only | engine | S | CT-04 | M6 |
| ME3 | midi-editing | Note-operation mechanics (Split identity rule, scale-degree stepping with directional rounding, rescale between scales, chord inversion, tuplets) + EDO name data + knife ... | helio-sequencer | port mechanics under Live's rules (not Helio's Join/Reverse/Invert/Legato semantics) + ... | engine | M | MI-04, MI-06, MI-08, TU-03 | M6 |
| ME5 | midi-editing | Shared arpeggio ordering for device + Arpeggiate tool: openDAW walk_index (18 modes, 15 match Live's styles, pure integer, expected sequences) + in-house #207 styles ... | openDAW | port C++ (one pure allocation-free function, 18 styles) | devices (+engine tool) | S | MI-07 | M6 |
| ME6 | midi-editing | Scale presets: Push/Live scale set under Live's display names incl. Pelog Selisir/Tembung + Messiaen 3-7 (DrivenByMoss); zrythm 60+ 12-TET masks as cross-check | DrivenByMoss | copy data with attribution (Live 12.4 census decides order/members) | engine (census: measurement seat) | S | MI-06 | M6 |
| MF1 | midi-fx | MIDI effect kernels + transform phase (Pitch, Chord, Random, Scale, Velocity, Note Length, Note Echo, Monitor, CC Control, Arpeggiator, MPE Control) with ... | in-house | port C++ onto the chain with the 10+ §1.4 fixes (prefix IDs, idempotent prepare, bypass ... | engine + devices | L | DEV-19, DEV-07, DEV-18 | M6 |
| MF2 | midi-fx | OneShot instrument (+ Pd bridge, MIDI identity fold) and Sampler (32 zones via SnapshotPublisher) | in-house | land #278 + port #208 (fix idempotent prepare; macOS assertion) | devices (+engine dispatch) | M | DEV-21 | M6 |
| MF5 | midi-fx | MIDI-effect lifecycle contracts (release_leavers at chain swap; release_all on discontinuity) + arpeggio walk_index (16 Live styles incl. Pinky/Thumb/Con&Diverge) + ... | openDAW | design-only (lifecycle) + port (walk_index, reseed rule) | engine + devices | S | DEV-19, DEV-14, DEV-18 | M6 |
| MF6 | midi-fx | MIDI bounce with processing up to an endpoint + note tracker closing open notes at range end | ardour | design-only (ADI: offline prefix pass → capture::prepareMidi; capture.createMidi + ... | engine | M | DEV-20 | M6 |
| RM7 | remarks | Nameable.comment on tracks/channels/devices/parameters/clips/markers/scenes = Live's info text one to one | bitwig/dawproject | design (DAWproject converter mapping for info text) | engine | S | RE-03 | M6 |
| TU1 | tuning | KBM semantics + key-table maths as TEST ORACLE + parser shapes (readKBMStream with 4 Scala fixes; readASCLStream as @ABL starting point) + own-authored tests | surge-synthesizer/tuning-library | port tests + write own compileKeyTable/parsers (design from its shapes) | engine | M | TU-01, TU-02 | M6 |
| TU4 | tuning | .ascl / .scl / .kbm writers + MTS bulk dump; MTS byte maths | xenharmonic-devs/scale-workshop | port C++ (~290 lines; own MMA checksum, not its 0xff-seeded XOR; octave convention from ... | engine | S | TU-02 | M6 |
| TU7 | tuning | Multi-channel keyboard layouts (index[2048][16]) + Scala name_1..16.kbm convention + trailing unmapped keys | helio-sequencer | design-only (layout half: Live's MIDI Controller Layouts; not its index note model) | engine/window | S | TU-02, TU-03 | M6 |
| WT1 | warp-tempo | Beat tracker (comb filterbank + Viterbi tempo, DP beat placement), DownBeat (bar phase from beat-synchronous spectral difference), onset detection function | c4dm/qm-dsp tag qm-vamp-plugins-v1.8.0 | vendor as library | engine | M | TM-WT-01, TM-WT-08, AGT-07, TM-WT-09 | M6 |
| WT2 | warp-tempo | Hitpoint detector: vibrato-suppressed spectral flux on log bands, local-max + mean threshold + 30 ms refractory, sample-accurate attack start, fast-release rejection ... | soundcraft | port C++ (~200 lines, onto adi::dsp::RealFft; add strength + peakDb ... | engine | S | TM-WT-01, TM-WT-08 | M6 |
| WT3 | warp-tempo | Constant-tempo regions (ironing +/-12 ms jitter), metronome tempo across regions, BPM snapping in error band (integer, 1/2, 2/3, 1/3, 1/12), phase refinement | mixxx | port C++ (Qt containers → std) | engine | S | TM-WT-01, AGT-07 | M6 |
| WT4 | warp-tempo | Loop vs one-shot decision (tatum quantization fit, IsSingleEvent) on PFFFT | audacity-3.7 tag Audacity-3.7.9 | port C++ (loop-or-one-shot only; bar count follows Live's 1/2/4/8/16-bar 4/4 rule) | engine | M | TM-WT-05 | M6 |
| WT5 | warp-tempo | Tempo estimate rounded to whole beats + bar count snapped up to 1/2/4/8/16 in 4/4 (= Live's even-length rule); probably_oneshot design echo | ardour | port C++ | engine | S | TM-WT-05 | M6 |
| WT6 | warp-tempo | Warp-marker edit rules (endpoints snap back, adjacent ratio clamp) + streaming WarpReader priming (pre-roll, discard latency) + real-time Rubber Band wrapper primed with ... | tracktion_engine | design-only (clamps measured from Live, intersected with engine limits) | engine | S (rules) / L (streaming) | TM-WT-02, TM-WT-04 | M6 |
| WT7 | warp-tempo | Multi-feature beat tracker + LoopBpmConfidence (closed-form) | essentia | design-only (port only if benchmark shows qm-dsp clearly worse, or director keeps one ... | engine | L | TM-WT-01 | M6 |
| WT8 | warp-tempo | Joint beat + downbeat tracker as benchmark comparator / downbeat fallback | CPJKU beat_this | benchmark comparator (needs a model runtime ADI lacks) | win/engine | S | TM-WT-01 | M6 |

### 3.7 M7 (19 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| GT1 | graph-threads | Lock-free multithreaded node player: per-node inputs-remaining counts, MPMC ready queue, chain continuation (RETURN_MID_NODES), pool persists across graph swaps with ... | tracktion_engine | port C++ (logic only, JUCE-free; not its creator-thread RT upgrade, not -1 dropped ... | engine | M | DEV-23 | M7 |
| GT2 | graph-threads | Activation-count kernel: items reset own count, first ready successor runs on same thread, node count reaching 0 ends the tick (any number of sinks), callback co-runs ... | supercollider | port C++ (replace boost::lockfree with third_party/lockfree MPMC or single-use per-block ... | engine | S (kernel) | DEV-23 | M7 |
| GT3 | graph-threads | Persistent process-wide pool, dependency counts with acquire-release, caller co-runs until batch count 0 (no quiescence step), owner-only queue, fork_n parallel-for ... | libossia | port C++ (design + kernel; not notify_all per submit, not moodycamel implicit producers) | engine | (in S1/S2) | DEV-23 | M7 |
| GT4 | graph-threads | Platform RT setup + wake rule + RT task list | ardour | port C++ (MMCSS Pro Audio at AVRT_PRIORITY_NORMAL; never REALTIME_PRIORITY_CLASS; not the ... | engine (driver side; win to confirm ... | S | DEV-24, DEV-23, MX-28 | M7 |
| GT5 | graph-threads | CLAP thread-pool host (request_exec served by pool, submitter helps, nested refused) + thread-check answering on any worker | zrythm | port C++ (on ADI's own pool) | engine | M | DEV-23 | M7 |
| GT6 | graph-threads | Device audio workgroup for workers | JUCE 9.0.2 (in tree) | use (vendor-as-library, already vendored) | engine | S | DEV-24, DEV-23, MX-28 | M7 |
| GT7 | graph-threads | Claim cursor + HybridBarrier (work-conserving within a level) | vcvrack | design-only (stopgap if S2 slips; fix Windows semaphore max count 1 hazard) | engine | S | DEV-23 | M7 |
| GT8 | graph-threads | Workers count as the audio thread only while rendering for it | soundcraft | tests only | engine | S | DEV-23 | M7 |
| MF3 | midi-fx | Pd MIDI egress: per-instance libpd MIDI-out hooks (noteon, controlchange, pitchbend, aftertouch, polyaftertouch) decoded by ADI's MpeParser into prefix IDs | libpd 0.16.1 | use (already vendored) + own wiring | devices (+engine for pd_engine.cpp per ... | M | DEV-14 | M7 |
| PD3 | pd-tier | "Edit in Pd" live attach lifecycle precedent (libpd_set_instance + start_gui, 8 ms poll, stop on close) + installed-Pd locator; static port scan idea | ossia-score | port C++ (logic, not Qt; not its audio-thread vector push_back or CC+8192 bug) | engine (+window menus) | M (route A real cost: overlay ... | DEV-11 | M7 |
| PD5 | pd-tier | Patch-owned data state protocol (bang save → lists via [s camomile] → restore to load), reloadPatch order (suspend, save, reopen, restore, prepare), patch-side ... | Camomile | design-only | engine/window | S | DEV-10 | M7 |
| PD6 | pd-tier | cyclone (compiled-in externals to widen the palette) / ELSE DSP objects | pd-cyclone | vendor as compiled-in externals (ADR-0188 d8 allows) | devices | M | DEV-15, DEV-11 | M7 |
| SC1 | scanner | Thread pool of per-plug-in child registrations with 15 s timeout + cancel, out-file results, main thread sole registry writer, interrupted run revalidated at next ... | muse_framework (Audacity 4's pin | port C++ | engine | M | DEV-25 | M7 |
| SC2 | scanner | Progress-reset deadline (30 s reset on every message), mtime-keyed hash, "nothing yielded" negative cache until mtime changes (= reinstall clears) | Carla | port C++ (policy) | engine | S | DEV-25 | M7 |
| SC3 | scanner | Scan-state taxonomy (New, Updated, Error, Incompatible, TimeOut, Blacklisted, Faulty), module-binary path to stamp (module_path_vst3), timeout/cancel is not a crash ... | ardour | port C++ (taxonomy + policy, ~150 lines; not per-file XML caches or text blacklist) | engine | M | DEV-25 | M7 |
| SC4 | scanner | Retry once in a fresh child before blaming the plug-in (mechanical half of Live's two-strike rule); thread-count setting | tracktion_engine | port C++ (~30 lines; not its in-process fallback) | engine | S | DEV-25 | M7 |
| SC5 | scanner | PluginDescription XML as VST3 cache payload (loadFromXml with no child), moduleinfo.json fast path, KnownPluginList round trip | JUCE 9.0.2 (in tree) | use (already vendored; stamp module binary + moduleinfo.json, not bundle dir mtime; key ... | engine | S | DEV-25 | M7 |
| SC6 | scanner | Worker pool with deadline per worker, plain-text scan report, SQLite plugin store with seen-set sweep | magda-core | design-only (SC1 preferred for code) | engine | S | DEV-25 | M7 |
| SC7 | scanner | Helper self-test before each batch (ADI's own design; Audacity runs it only in CI) | audacity | design-only | engine | S | DEV-25 | M7 |

### 3.8 Track C (11 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| A1 | agent | Remote dispatcher: scopes/ScopeSet, client grants + first-use prompt, audit ring 512 w/ secret redaction, constant-time token/Origin check, discovery record ... | magda-core head | port C++ ONCE with control-api CA1 (one dispatcher; the agent's checks below join CA1's ... | engine | in CA1 (L) | CTL-01, CTL-03 | C |
| A2 | agent | MCP endpoint (tools+resources projection, error rules, _meta, both protocol eras) + Streamable HTTP server options + stdio bridge | magda-core head | port C++ ONCE with control-api CA3: stdio adi-mcp, no second listener; /mcp over HTTP ... | engine | in CA3 (M) | CTL-08 | C |
| CA1 | control-api | Remote API core: WebSocket transport options (loopback, token required, Origin, 8 connections exact, 240 KiB frames, in-flight 8, reply/event budgets, 50 rps token ... | magda-core head | port C++ JUCE-free (nlohmann::json, injected Executor, undo head instead of Revision, two ... | engine | L | EX-09, CTL-06, CTL-07, CTL-01, CTL-03, CTL-04, CTL-09, SRF-07, CTL-05 | C |
| CA2 | control-api | WebSocket/HTTP transport library | cpp-httplib v0.64.0 (24e58ce5) | vendor as library (third_party pin in tools/fetch_external.sh; one TU behind pimpl ... | engine (pin: win) | S | CTL-04 | C |
| CA3 | control-api | MCP projection + stdio bridge (protocol eras table, legacy initialize + stateless server/discover, tool-error rules, discovery-record endpoint resolution) (= agent A2) | magda-core head | port C++ | engine | M | CTL-08 | C |
| CA4 | control-api | Control-channel rules: auth before anything, one refusal text, bounded pending auth, abandon timed-out requests (PENDING/TAKEN/ABANDONED), synthetic input + screenshot ... | gridcraft | design only | engine + window | M | CTL-01, CTL-03, WTS-10, CTL-05 | C |
| CA5 | control-api | Component + accessibility description for ui.elements/ui.inspect (role table, type demangling, font/LnF strings) | melatonin_inspector | copy with attribution | window | S (surface M) | WTS-05, WTS-10 | C |
| OS1 | control-api | op_schema: JSON Schema export from the op registry with MAGDA's default bounds (shared by agent and control-api) | magda-core head | port C++ (small); teach validateSubmission the same constraints | engine | M | EX-09, CTL-02 | C |
| WT-1 | window-testing | Pixel diff with anti-alias detection, diff image, ignore mask, windowed diff | mapbox pixelmatch-cpp v8.0.0 | vendor header-only via fetch_external.sh as SYSTEM include (verifier: NOT bit-exact at ... | window | S | MX-36, WTS-03 | C |
| WT-2 | window-testing | Visual-test review job: base renders found by SHA, compare tool, label on change | musescore | design-only (verifier: MuseScore builds pull_request.base.ref - a branch tip - and ... | window (win for the workflow) | M | WTS-04 | C |
| WT-6 | window-testing | Golden testkit: diff metrics, EXACT and RENDER tolerances, bless, failures directory, size cap; snapshot update modes | filmcraft | port C++ | window | S | MX-36, WTS-03 | C |

### 3.9 P2 (48 takes)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| A3 | agent | Consent ledger (Disclosure kinds+sizes, ConsentRecord per app/connection/destination/capability/kinds, 4 terms, fail-closed 256 KiB atomic file) | craft-libs | port C++ | engine (+window) | S (+M UI) | AGT-03 | P2 |
| A5 | agent | Sidecar agent loop, multi-provider LLM API, MCP client | pi-mono | vendor npm, exact pins + integrity | sidecar seat (win names) | M | AGT-06 | P2 |
| A6 | agent | Run budget + termination reasons (steps 8, tokens 32000, mutations 8, identical calls 2, wall 2 min) | magda-core head | design only | sidecar seat | S | AGT-06 | P2 |
| A7 | agent | Sidecar argument validation, and an MCP client fallback if pi-mcp lags the 2026-07-28 protocol | pi-mono | npm dependency, exact pins + integrity (only if needed) | sidecar seat | S | AGT-06 | P2 |
| AU9 | automation | Freeze Trim data model (trim lane coalesced into volume) | soundcraft | port C++ (densify curves first) | engine | M | AU-09 | P2 |
| C1 | collab | Collaboration core: sequencer (seq per batch, base_seq, dedupe, bounded catch-up log, id-space check), resume/catch-up protocol, session persistence + journal replay ... | figcraft | port C++ (ADI syncs typed ops; sequencer orders only; replicas validate) | engine | L | CO-03, CO-01, CO-02 | P2 |
| C2 | collab | Touch sets (table, PK, columns incl. FK cascades) for collision detection + filtered undo | SQLite 3.49.2 session extension | vendor-as-library: target_compile_definitions(sqlite3 PUBLIC SQLITE_ENABLE_SESSION ... | engine | S enable / M use | CO-03, CO-01, CO-02 | P2 |
| C3 | collab | Fractional sibling keys + key reuse (longest increasing run) + respace | figcraft | port C++ (take figcraft's refusal of swapped bounds) | engine | S | CO-01 | P2 |
| C4 | collab | Sequencer on a plain bucket: put-if-absent claim loop, 20-digit keys, S3 conditional-write quirks (R2 412 vs 304, If-Match 409 retry), SigV4 signing | delta-rs | port C++ | engine | M | CO-05, CO-01 | P2 |
| C5 | collab | Presence/signals/spotlight, throttle 33 ms latest-wins, LAN hub threading + NetLimits, join codes (Crockford base32, local 20-symbol code with 44-bit secret), lan_ip | figcraft | port C++ | engine | M | CO-04 | P2 |
| C6 | collab | Follow a collaborator's playhead: clock offset from 4 requests, advancePosition, resolveChain, transport lock | openDAW | port C++ | engine (+window U3) | S | CO-04 | P2 |
| C7 | collab | Crash-safe resumable upload queue (block hashes, pending snapshots, missing-blocks uploader) | audacity | port C++ (design) | engine | M | CO-05 | P2 |
| C8 | collab | Rebase by replaying named mutators (pending mutations rebased; throwing mutator leaves basis; lastMutationID per client) | rocicorp-mono | design-only (complements C1 for G5/G12) | engine | (in E4) | CO-01 | P2 |
| C9 | collab | Protocol vocabulary and reconcile rules: sequenced-message fields; never revert locally plus the overlap rule; vector-only fallback; durable-before-confirm; room ... | FluidFramework protocol-definitions ... | design-only | engine | S | CO-03, CO-01 | P2 |
| IC7 | interchange | SMF reader/writer (VLQ, running status, sysex/meta raw bytes) + DryWetMIDI corpus (63 valid, 666 invalid) + 16 reading policies as named anomalies + JUCE RMID ... | libremidi | copy with attribution (parser/writer, fix: SMPTE division, 64-bit ticks, preserve unknown ... | engine | M | TM-IC-02, IC-02 | P2 |
| IC9 | interchange | AAF reader (tracks, clips with essence offset, gain/automation, fades/crossfades, markers, timecode, embedded essence extraction) + Ardour mapping + 65-file corpus with ... | LibAAF | vendor as library (static adi_libaaf, mutex: not reentrant) + port mapping | engine | M | TM-IC-04, IC-03 | P2 |
| IC10 | interchange | Clean-room AAF Edit Protocol + MS-CFB writer (SMPTE UMIDs, deterministic timestamps, weak refs, handles, mono breakout, 512-byte sectors) | filmcraft | port C++ (lowering from rows::Model is new) | engine | L | TM-IC-04, IC-03 | P2 |
| IC11 | interchange | OMF 2.0 read | filmcraft | port C++ | engine | M | TM-IC-04, IC-03 | P2 |
| LS2 | link-sync | MIDI clock in (2nd-order DLL, SPP, stop rewinds to last MIDI beat), clock out ticker (sample offsets, Start/SPP+Continue, 0x3FFF clamp), MTC in (quarter-frame assembly ... | ardour | port C++ (algorithms against ADI's sample transport, 240,240 ticks per clock, SPSC rings) | engine | L | TM-LS-03 | P2 |
| LS3 | link-sync | Link Audio: BasicLinkAudio, LinkAudioSink (realtime-safe commit with same session snapshot), LinkAudioSource, renderer send/receive with beat-mapped SPSC queue | Link-4.1.1 | vendor (same pin) | engine | L | TM-LS-02 | P2 |
| RK1 | racks | One graph with the player doing PDC for rack sub-graphs; macro modifier sources (remap, bezier depth); wrap selected plug-ins in a rack (Group) | tracktion_engine | design-only (verifier: tracktion builds each RackType once on shared buses and cannot ... | engine | S | DEV-01, DEV-06, MX-24 | P2 |
| RK2 | racks | Composite as a flattening host (not a nested engine): persistent per-child reconcile, choke by router-tagged EVENT_CHOKE (off/choke sorted before on, 5 ms ... | openDAW | design-only + tests | engine | S | DEV-02, DEV-04, DEV-01, DEV-17 | P2 |
| RK3 | racks | Zone admission (conjunction of key/velocity/controller ranges) + crossfade gains (gain and power laws) + choke arbitration tests | sfizz | copy with attribution (fade maths) + tests | engine | S | DEV-02, DEV-04 | P2 |
| RK4 | racks | Live-shaped rack model (ChainElement variant of device or nested rack; ChainInfo volume/pan/mute/solo/bypass; macros 16 in pages of 8); chain routing policy ... | magda-core | design-only + tests | engine | S | DEV-03, DEV-04, DEV-01 | P2 |
| RK5 | racks | Multi-point curve editor model (power per segment, smoothing, presets, flips, add-middle-point, 100-point cap) for per-target macro mapping curves | Vital | design-only (evaluation stays ADR-0159 curves.hpp; storage AAUT points) | window | S | DEV-06 | P2 |
| RK6 | racks | Live Sampler zone layout (Min/Max/CrossfadeMin/CrossfadeMax) supports the positional 4-byte zone BLOB | DawVert | cross-check data | engine | S | DEV-02 | P2 |
| RE8 | range-edit | Razor area format: start, end, envelope GUID, fixed-lane top/bottom, so a TrackSpan can name automation or take lanes | reaper-sdk | design-only | engine | S | - | P2 |
| RM1 | remarks | Thread closure (cycle-safe BFS over a children map), reply needs existing parent, reading-order listing with unanchored last, next/previous with wrap, resolve toggle | wordcraft | port C++ (algorithm + tests; flat threads per verifier; closure kept only for hostile ... | engine (+window) | S | RE-02 | P2 |
| RM2 | remarks | Six-way time-edit relation table (before/after/surrounds/within/begins-in/ends-in) with point/region edge rules for insert/delete/copy/paste | audacity | port C++ (~120 lines; non-retain branch fixed; i64 ticks/ns half-open) | engine | S | ED-02, RE-02 | P2 |
| RM3 | remarks | Re-pin to nearest surviving parent on delete (undo pins back) + panel Filter/Sort/Query + bounded untrusted agent brief (256 KiB, 200 threads, 4,096 chars/message ... | figcraft | design-only (re-pin rule as explicit remark.setAnchor in the same txn) + port C++ ... | engine + window | M | RE-02 | P2 |
| RM4 | remarks | Balloon cards: cascade layout, author stripe, 10-line clamp until selected, natural reading direction per paragraph, edit in place with Reply/Resolve/Delete | wordcraft | design-only (cache layouts by id/text hash/width/scale; no margin balloons in the ... | window | M | RE-02 | P2 |
| RM6 | remarks | Marker range relative to its item; slice copies markers to both pieces (evidence); converter mapping | OpenTimelineIO | design-only (content-relative clip ranges; audio anchored in source time per verifier) | engine | S | RE-02 | P2 |
| RM9 | remarks | Per-endpoint side (before/after) for anchor boundary cases | FluidFramework SequencePlace Side | design-only | engine | S | RE-02 | P2 |
| SV1 | session-view | Audio-thread launch engine: SlotKey, SyncRange, LaunchHandle (play/stop/playSynced/advance/releaseSection/scheduleBeat), LaunchRequestQueue (1024-slot ring ... | magda-core-head | port C++ onto integer ADI_PPQ ticks + samples + SnapshotPublisher + BlockScope pin ... | engine | L | TM-SV-03, TM-SV-04 | P2 |
| SV3 | session-view | Follow-action groups (contiguous occupied slots split by empty) + deterministic target resolver | tracktion_engine HEAD | port C++ + Live's ten actions written fresh (Next wraps = TE RoundRobin; Other alone ... | engine | M | TM-SV-04 | P2 |
| SV4 | session-view | Per-slot press/release state table for 5 launch styles (Live's Trigger = Ardour ReTrigger), atomic press counters, WaitingToSwitch state | ardour | design-only + port table as pure onEdge(phase, mode, edge) (not its quantizer or legato ... | engine | S | TM-SV-03 | P2 |
| SU1 | surfaces | Encoders with inverses (applyMode 4 flavours, curves, ranges, toggle edge, normalized OSC), binding model, port identity (identifier then name, heal key), JSON ... | magda-core-head | port C++ JUCE-free (std::string/nlohmann; NOT ControllerRouter write path which drops ... | engine (+window sockets/ports) | L | SRF-05, SRF-08, SRF-09, WIN-07, SRF-03 | P2 (its device-identity match in M3) |
| SU2 | surfaces | Stream Deck + XL client: Elgato Node SDK (dial ticks/pressed, setFeedback, layouts) + director's VST controller (one-module bridge, 13 predefined controllers ... | elgato-streamdeck | copy with attribution; @elgato/streamdeck 1.4.x as npm dependency; wire re-pointed at ADI ... | seat win names | M | SRF-13, CTL-05 | P2 |
| SU3 | surfaces | Mackie Control protocol layer: SysEx headers/handshake/Logic Control challenge, fader pitch bend + servo echo, in-use guard, V-Pot decode, rings, LCD, meters, timecode ... | ardour | port C++ (protocol layer as pure functions; device files to JSON; NOT ... | engine | L | SRF-08 | P2 |
| SU5 | surfaces | HUI codec (zones/ports, 14-bit faders + touch, V-Pots with HUI polarity, rings, aftertouch meters, scribble + timecode SysEx, 1 s ping) | DrivenByMoss | port C++ (~600 lines codec + tests) | engine | M | SRF-08 | P2 |
| SU6 | surfaces | OSCQuery: HOST_INFO keys, tree JSON shapes, ACCESS 3, LISTEN/IGNORE/PATH_\* | OSCQueryProposal | design-only (in-process HTTP GET responder on nlohmann + per-OS DNS-SD; NOT organicui's ... | engine + window | M | SRF-10 | P2 |
| SU9 | surfaces | Generic MIDI surface: enc-l/enc-r/enc-2/enc-b polarities, absolute threshold-in-sync rule (3rd Pick-Up oracle), 58 device maps (bank size, motorised, threshold) as ... | ardour | port data + names/polarities (not its extra-step code); native drivers P3 | engine | M | SRF-04, SRF-08, SRF-03 | P2 |
| SU11 | surfaces | Plain-OSC listen pattern (/get, /set, /start_listen, /stop_listen) | AbletonOSC | design-only | engine | S | SRF-09 | P2 |
| TU2 | tuning | MTS-ESP master wrapper (lazy dlopen of the USER's installed libMTS; 21 entry points; registration protocol) + client parseMIDIData as SysEx decoder oracle + freqToNote ... | ODDSound/MTS-ESP | copy with attribution (static-init load → lazy; injectable function table) | engine (after director R2) | M | TU-04 | P2 |
| TU3 | tuning | Open re-implementation of libMTS middleware (24-EDO master/client test programs) as the CI fake for an end-to-end master test through the real wrapper | baconpaul/mts-dylib-reference | test double (not installed) | engine | S | TU-04 | P2 |
| TU5 | tuning | Byte-exact MTS encoders: single-note change (7F/08/02, 08/07 with bank), scale/octave 1- and 2-byte (08/08, 08/09), tuning program/bank RPNs; +/-50 cent normalisation ... | Woyten/tune | port test expectations (not its KBM model) | engine | S | TU-04 | P2 |
| TU6 | tuning | Host sending MTS to a plug-in or a track (bulk + real-time single-note; immediate event to PluginInsert or MidiTrack) | ardour | design-only (its checksum includes 0xF0 - not copied) | engine | S | TU-04 | P2 |
| TU8 | tuning | CLAP tuning draft (get_relative per tuning_id/channel/key/sample_offset, should_play, clap_event_tuning) + octave-number/1 (note 60 = C3) | third_party/clap 1.2.10 | weigh as the CLAP-side alternative to MTS-ESP (ruling R2) | engine | M | TU-04 | P2 |

### 3.10 P3 (1 take)

| Take | Area | What | From | How | Lane | Effort | Rows | M |
|---|---|---|---|---|---|---|---|---|
| SU4 | surfaces | MIDI-CI (discovery, property exchange, subscriptions) over plain MIDI 1.0 SysEx | JUCE 9.0.2 (in tree) modules/juce_midi_ci | vendor-as-library (link juce::juce_midi_ci) | window | M (P3) | SRF-11 | P3 |

## 4. Not taken

**Not usable:** POW-r dither; the EBU loudness test-sequence WAV set (the signals are synthesised
instead); the libMTS binaries (the MTS-ESP sources are usable); the REX2 decoding SDK; Ableton's
push-interface documentation; Live's MIDI Remote Scripts in any copy; the
Ableton-authored `.ascl` and `.kbm` fixtures in tuning-library's test data; h1data's M4L MIDI tool
examples; helgoboss-learn; the OSCQueryProposal repository (read as a protocol description only);
bitwig-docked-plugins; DawVert's (its source file) folder; pd-else's Live Electronics
Tutorial; Plugin Doctor and REW.

**Merged or superseded:** export EX2 (libebur128's LRA, whose 1 Hz hop is below Tech 3342's 10 Hz)
gives way to the one meter; midi-editing ME2 (vendoring tuning-library) gives way to TU1 (a test
oracle only); agent A1 and A2 are built once, inside CA1 and CA3, with no `src/adi/rpc` and no second
listener; IXWebSocket and cpp-httplib v0.52.0 are rejected; a new AANL analysis format duplicates
#449's ADIA sections; pywinauto never drives ADI's own UI.

**Rejected per area:** each matrix's "Reject or defer" lines (PLAN Appendix A) and the ledger's per-area lists keep the reasons.

One take is recorded and used by no row: RE8 (it stays in the ledger for the row that may need it).

**Rejected at the bundle's level** (PLAN §1.6): the study of built-in effect and instrument
implementations (excluded by the director); instruments other than OneShot and Sampler; a second
graph runtime or patch editor; other programs' preset formats; Bitwig project compatibility;
plug-in sandboxing at runtime and a render child process (ADR-0203 d8); an enqueue-time project
snapshot for export jobs (the erratum); the additions nobody asked for; VariAudio-class editing,
notation, video sync and hardware CV/Gate at P3 or later. Reading Live Sets is not rejected: it is
the director's Q-P.

## 5. Research ID index

Every research ID a row cites, with the rows that cite it. A row that cites an ID took it as
evidence for its contract, its rejection or its unresolved test; the row's card says which.

- A-BASE-17.1: MX-09
- A-BASE-17.2: MX-17
- A-BASE-17.2.1: MX-25
- A-BASE-17.4: MX-25
- A-BASE-17.5: MX-25
- A-BASE-17.5.1: MX-24
- A-BASE-17.5.2: MX-16
- A-BASE-18.1: MX-01, MX-02
- A-BASE-18.1.1: MX-01
- A-BASE-18.2: MX-23
- A-BASE-18.3: MX-26
- A-BASE-18.4: MX-05
- A-BASE-18.5: MX-15
- A-BASE-18.6: MX-07, MX-08
- A-BASE-18.7: MX-04
- A-BASE-18.8: MX-09
- A-BASE-18.9: MX-28
- A-BASE-2.2.1: WIN-09
- A-BASE-23.6: MX-04
- A-BASE-25.5.4: AU-08
- A-BASE-25.5.5: AU-02
- A-BASE-25.5.6: AU-05
- A-BASE-3.10: TM-IC-02
- A-BASE-37.1.1: MX-28
- A-BASE-38.2.4: MX-03
- A-BASE-5.2: TM-IC-02
- A-BASE-5.2.1: TM-IC-02
- A-H02: AU-03, CT-03, HI-01, MI-05
- A-H04: CT-04
- A-H05: AU-06
- A-M01: MI-01, MI-03
- A-M03: MI-02
- A-M07: MI-09
- A-M5: TM-MD-05
- A-T02: MI-07
- A-T09: MI-06
- A-W01: TM-WT-01, TM-WT-05
- A-W02: TM-WT-02
- A-W03: TM-WT-02
- A-W04: TM-WT-01, TM-WT-02
- A-W05: TM-MD-04
- A-W06: TM-WT-02
- A-W07: TM-WT-02
- A-W08: TM-WT-07
- A-W09: TM-WT-03
- A-W10: TM-WT-03
- A-X01: MI-08
- A-X02: MI-08
- A-X03: MI-08
- A11N-001: DEV-01
- A11N-002: DEV-08, DEV-10
- A11N-003: DEV-06, DEV-10
- A11N-004: DEV-06, DEV-08
- A11N-005: DEV-12, DEV-13
- A11N-006: DEV-12
- A11N-007: DEV-14
- A11N-008: DEV-10, TM-MD-03
- A11N-009: DEV-09, DEV-10, DEV-26
- A11N-010: DEV-09, DEV-10
- A11N-011: DEV-09, DEV-10
- A11R-001: AGT-04, DEV-29, EX-01, TM-IC-05
- A11R-002: EX-02
- A11R-003: EX-03
- A11R-004: EX-01
- A11R-005: EX-01, EX-11
- A11R-006: EX-07, PD-04
- A11R-007: EX-01, EX-07
- A11R-008: EX-07
- A11R-009: EX-04, EX-06
- A3-001: TM-SV-03
- A3-002: TM-SV-03
- A3-003: TM-SV-04
- A3-004: TM-SV-04
- A3-005: TM-SV-04
- A3-007: MX-09
- A3-008: MX-09
- A3-009: MX-03, MX-25
- A3-010: SRF-05
- A3-012: MX-04, MX-09
- A3-014: AU-03
- A3-017: SRF-08
- A3-018: CTL-06, SRF-03, SRF-04
- A3-021: TM-IC-02
- A3-023: TM-FC-03, TM-MD-03
- A3-024: TU-01, TU-03
- A3-025: TU-03
- A3-026: DEV-02
- A3-027: DEV-02
- A3-028: AU-06, DEV-06
- A3-029: DEV-06
- A3-031: EX-11
- A3-032: TM-SV-02
- A4-001: EX-02, EX-11
- A4-002: EX-02, EX-03, EX-11
- A4-003: EX-04, EX-05, EX-11
- A4-004: EX-04, EX-06, EX-11
- A4-005: ED-06, TM-FC-02
- A4-006: TM-FC-01
- A4-007: TM-FC-01
- A4-008: EX-02, TM-FC-02
- A4-009: TM-FC-02
- A4-010: HI-02, HI-03, HI-04, PD-03, PD-05
- A4-011: PD-15
- A4-012: ED-01, ED-02
- A4-013: ED-08
- A4-015: ED-09
- A4-017: CT-02, TM-SV-04
- A4-018: CT-03
- A4-020: AU-06
- A5C-001: MX-01, MX-09
- A5C-002: MX-23
- A5C-003: MX-02
- A5C-004: MX-01
- A5C-007: MX-26
- A5C-008: MX-26
- A5C-009: MX-05
- A5C-010: MX-15
- A5C-011: MX-07
- A5C-012: MX-08
- A5C-013: TM-FC-04
- A5C-014: TM-FC-04
- A5C-015: TM-FC-04
- A5C-016: TM-FC-04
- A5C-017: TM-FC-04
- A5C-018: TM-FC-04
- A5C-019: DEV-23, DEV-24, MX-28
- A5C-020: DEV-23
- A5C-021: DEV-22
- A5C-022: DEV-29, ED-06, TM-FC-02
- A5C-023: MX-02, MX-03
- A5C-024: MX-09, TM-LS-03
- A5C-025: TM-LS-03
- A5C-026: TM-LS-01
- A5C-027: TM-LS-01
- A5C-028: TM-LS-01
- A5C-029: TM-LS-01
- A5C-035: ED-02
- A5C-036: TM-SV-04
- A5C-037: TM-SV-02
- A5C-038: TM-SV-02, TM-SV-05
- A5C-039: TM-SV-03, TM-SV-05
- A5C-040: TM-SV-03
- A5C-041: TM-SV-04
- A5C-048: HI-02, HI-03, PD-03, PD-05
- A5C-049: PD-02, PD-04, PD-06
- A5H-008: TM-MD-04
- A5H-009: TM-MD-02, TM-MD-04
- A5H-014: PD-07, PD-08
- A5H-016: TM-MD-03
- A5H-017: TM-MD-03
- A5H-018: TM-MD-03
- A5H-020: DEV-03, DEV-19
- A5H-021: DEV-26
- A5H-022: AU-04
- A5H-034: DEV-25, PD-05
- A5H-035: DEV-25
- A5H-036: DEV-05
- A5W-001: WIN-03, WIN-08
- A5W-002: WIN-03, WIN-08
- A5W-003: TM-SV-05, WIN-08
- A5W-004: WIN-08
- A5W-009: WIN-04
- A5W-010: WIN-01
- A5W-012: WIN-07
- A5W-013: WIN-07
- A5W-014: TM-SV-05, WIN-07
- A5W-015: WIN-05
- A5W-016: AU-01, MI-03
- A5W-017: AU-01
- A6C-006: TM-SV-01
- A6C-007: TM-SV-01
- A6C-008: TM-WT-05
- A6C-018: TM-SV-03
- A6C-019: TM-SV-03
- A6C-023: TM-SV-01
- A6C-026: TM-MD-03, TM-WT-02
- A6C-028: TM-SV-03
- A6C-031: TM-WT-07
- A6C-032: TM-WT-01, TM-WT-05
- A6C-033: TM-MD-02
- A6C-034: TM-WT-02, TM-WT-06
- A6C-035: TM-WT-02, TM-WT-06
- A6C-036: TM-WT-02, TM-WT-06
- A6C-037: TM-WT-02, TM-WT-06
- A6C-038: TM-WT-02, TM-WT-06
- A6C-039: TM-WT-03, TM-WT-06
- A6C-040: TM-WT-03, TM-WT-06
- A6C-041: TM-SV-03
- A6C-042: TM-SV-01
- A6C-043: TM-SV-04
- A6C-044: TM-SV-04
- A6C-045: TM-SV-04
- A6C-046: TM-SV-04
- A6C-047: TM-SV-04
- A6C-048: TM-SV-03
- A6C-049: TM-SV-04
- A6C-051: AU-08
- A6C-053: AU-01
- A6C-058: AU-01
- A6C-059: AU-01
- A6C-060: AU-01
- A6C-062: AU-01, AU-08
- A6C-065: AU-02
- A6C-069: AU-06, AU-07, AU-08
- A6C-071: AU-06
- A6C-072: AU-06
- A6C-076: ED-04, ED-05
- A6C-077: ED-03, ED-09
- A6C-079: ED-10
- A6C-080: CT-05, ED-10
- A6C-083: ED-01
- A6C-086: TM-WT-02
- A6D-012: EX-07
- A6F-001: TU-02
- A6F-004: TU-02
- A6F-005: TU-02
- A6F-006: TU-02
- A6F-007: TU-02
- A6F-008: TU-01
- A6F-009: TU-01, TU-02
- A6F-010: TU-01, TU-02
- A6F-011: TU-01
- A6F-012: TM-IC-02, TU-02
- A6F-013: TM-IC-02, TU-02
- A6F-014: TM-IC-02, TU-02, TU-04
- A6G-002: TM-LS-01
- A6G-003: MX-28
- A6G-005: TM-FC-03, TM-FC-04
- A6G-006: MX-03
- A6G-007: MX-03
- A6G-010: EX-04, EX-06, MX-03
- A6G-013: MI-05, TM-SV-01
- A6H-001: MX-25
- A6H-002: MX-17, MX-25
- A6H-006: MX-25
- A6H-007: MX-07, MX-25
- A6H-008: MX-07, MX-24
- A6H-009: MX-07, MX-24
- A6H-010: MX-09
- A6H-011: MX-25
- A6H-012: MX-16, MX-25
- A6H-013: MX-20, MX-24
- A6H-014: MX-25
- A6H-016: CT-02
- A6H-023: TM-FC-01
- A6H-024: TM-FC-01
- A6H-027: CT-01, CT-02, CT-03
- A6H-028: IC-02, TM-IC-02
- A6H-029: CT-01, CT-04
- A6H-030: CT-04
- A6H-034: MX-04
- A6H-042: DEV-01, DEV-02
- A6H-043: DEV-02
- A6H-044: DEV-04
- A6H-051: DEV-06
- A6H-063: TM-MD-04
- A6H-064: TM-MD-02
- A6H-067: PD-15
- A6H-068: HI-04, PD-01, PD-02
- A6H-069: PD-01, PD-09
- A6H-073: EX-11
- A6I-001: TM-LS-02
- A6I-002: TM-LS-02
- A6I-003: TM-LS-02
- A6I-004: TM-LS-02
- A6I-005: TM-LS-02
- A6I-006: TM-LS-01
- A6I-007: TM-LS-03
- A6I-008: TM-LS-03
- A6I-009: TM-LS-03
- A6I-010: TM-LS-03
- A6I-011: TM-LS-03
- A6I-012: TM-LS-03
- A6M-003: ED-09, MI-03
- A6M-004: TM-SV-01
- A6M-009: MI-03, MI-04
- A6M-016: MI-01, MI-02
- A6M-018: MI-04, MI-06
- A6M-019: MI-04, MI-06
- A6M-022: MI-05
- A6M-024: MI-05
- A6M-027: MI-04
- A6M-030: MI-09
- A6M-031: MI-06
- A6M-032: MI-06
- A6M-033: ED-04, MI-04
- A6M-034: ED-04, MI-04
- A6M-036: MI-03
- A6M-037: MI-03
- A6M-038: MI-03
- A6M-040: MI-07
- A6M-078: MI-05
- A6M-084: TU-01, TU-03
- A6M-086: TU-03
- A6M-087: TU-03
- A6M-090: TU-03
- A6M-092: TU-03
- A6P-001: CTL-01
- A6P-002: CTL-01
- A6P-003: AU-04, CTL-01, CTL-06, MX-22, SRF-01
- A6P-004: AGT-02
- A6P-006: HI-01
- A6P-008: TM-WT-02
- A6P-010: MI-01
- A6P-011: MI-01, MI-09, TM-SV-01, TM-SV-02
- A6P-012: TM-SV-02, TM-SV-03
- A6P-019: HI-01
- A6R-001: SRF-05
- A6R-002: SRF-05, SRF-08, SRF-14
- A6R-003: SRF-05
- A6R-004: SRF-04
- A6R-005: WIN-07
- A6R-006: WIN-07
- A6R-007: WIN-07
- A6R-008: SRF-03
- A6R-009: SRF-03
- A6R-010: SRF-03
- A6R-011: SRF-03, WIN-07
- A6R-012: SRF-08, TM-SV-05
- A6R-014: SRF-15
- A6R-047: TM-MD-01
- A6S-001: MX-05
- A6S-007: TM-SV-02, TM-SV-04
- A6U-001: SRF-15
- A6U-002: SRF-15
- A6U-013: SRF-05, SRF-15
- A6X-004: MI-06
- A6X-006: MI-09
- A6X-013: MI-07
- A6X-016: HI-01, MI-07
- A6X-017: MI-07
- A7A-062: MX-36
- A7B-086: MX-13
- A7B-088: DEV-07
- A7M-001: DEV-19
- A7M-053: DEV-19
- A7M-058: DEV-21
- A7M-122: DEV-19
- A7M-127: DEV-21
- A7M-128: DEV-17, DEV-18, DEV-19, DEV-20
- A7M-130: DEV-19
- A7M-131: DEV-21
- A7R-001: DEV-22
- A7R-008: DEV-21
- A7R-009: DEV-21
- A8D-001: DEV-01
- A8D-004: DEV-04
- A8D-007: DEV-04
- A8D-010: DEV-04
- A8D-017: DEV-02
- A8D-018: DEV-06
- A8D-022: DEV-01
- A8P-018: TU-04
- A8P-019: TU-04
- B-A07: CT-01, ED-05
- B-G01: DEV-01, DEV-05
- B-G02: DEV-15
- B-G06: DEV-15
- B-G07: DEV-26
- B-H01: MX-14
- B-M01: DEV-06, DEV-07, DEV-08
- B-M08: DEV-06
- B-M09: DEV-07
- B-O01: MI-07
- B-O04: ED-06
- B-V02: AU-06
- B3-005: DEV-25, DEV-26
- B3-009: DEV-23, DEV-25, DEV-27
- B3-010: DEV-25, DEV-27
- B3-011: SRF-08, SRF-14
- B3-012: SRF-08, SRF-14
- B3-013: CTL-06, SRF-07
- B3-015: DEV-07
- B3-020: DEV-07, MX-15
- B4-005: MI-03
- B4-20: TM-SV-05
- B9G-001: DEV-15
- B9G-002: DEV-10
- B9G-003: DEV-11
- B9G-004: DEV-11
- B9G-005: DEV-11
- B9G-006: DEV-10
- B9G-008: DEV-12
- B9G-038: DEV-14, DEV-17, MI-08
- B9G-039: DEV-14
- B9G-059: DEV-15
- B9M-001: DEV-18
- B9M-002: DEV-19
- B9M-034: DEV-02
- B9M-045: DEV-07
- B9M-046: DEV-07
- B9M-048: DEV-07
- B9M-050: DEV-07
- B9M-069: TU-04
- C-D01: TM-FC-03
- C-D16: TM-FC-03
- C-E01: TM-FC-01
- C-E02: TM-FC-01
- C-E03: MX-24, TM-FC-01, TM-FC-04
- C-F01: ED-10
- C-F02: ED-10
- C-F04: CT-05
- C-F05: CT-05
- C-F06: CT-05, ED-07
- C-F07: ED-10
- C-F10: AU-07
- C-R01: ED-01
- C-R02: ED-01, ED-02, ED-07
- C-R03: ED-01, ED-04
- C-R04: ED-03
- C-U02: MX-10, MX-27
- C3-001: TM-WT-01, TM-WT-08
- C3-002: TM-WT-01, TM-WT-08
- C3-003: TM-WT-01, TM-WT-08
- C3-004: TM-WT-01, TM-WT-08
- C3-005: TM-WT-01, TM-WT-08
- C3-011: TM-TT-01, TM-WT-04
- C3-014: AU-05
- C3-015: AU-01
- C3-016: AU-03, AU-09
- C3-017: AU-09
- C3-018: AU-09
- C3-019: AU-03, AU-09
- C3-020: MX-19
- C3-021: MX-14, MX-36
- C3-022: MX-06, MX-16
- C3-023: MX-08, MX-18
- C3-024: MX-08, MX-18
- C3-025: MX-08, MX-18, MX-29
- C3-026: MX-18, MX-29
- C3-027: MX-08, MX-18
- C3-028: MI-08
- C3-030: MI-07, RE-02
- C4-001: EX-03
- C4-002: EX-04
- C4-003: EX-02
- C4-005: EX-02, EX-03
- C4-006: EX-02
- C4-007: EX-07
- C4-008: EX-01, EX-09
- C4-009: EX-07
- C4-010: EX-12
- C4-011: EX-12
- C4-012: EX-04
- C4-013: EX-04
- C4-014: EX-08
- C4-015: EX-09, PD-11
- C4-016: IC-03, TM-IC-01, TM-IC-04
- C4-017: IC-01, IC-03, TM-IC-04
- C4-018: IC-01, TM-IC-03
- C5V-013: TM-WT-02
- C5W-001: MX-14
- C5W-002: MX-14
- C5W-008: WIN-02
- C5W-010: WIN-04
- C5W-011: WIN-02
- C5W-013: MX-18
- C5W-015: WIN-04, WIN-05
- C5W-016: WIN-04, WIN-05
- C5W-017: HI-01, WIN-05
- C5W-018: WIN-05
- C5W-023: ED-03
- C5W-029: PD-05, PD-11
- C6-AUDIO: MX-33
- C6-HISTORY: MX-23
- C6-KEYBOARD: MX-14
- DR-01-01: WIN-01, WIN-09, WTS-02, WTS-05, WTS-06
- DR-01-02: TM-SV-02, TM-SV-05, WIN-08
- DR-01-03: WTS-07
- DR-01-09: WIN-02, WTS-10
- DR-01-10: WIN-03, WIN-08
- DR-01-11: WIN-05, WTS-03, WTS-07, WTS-08
- DR-01-12: WIN-09, WTS-01, WTS-03, WTS-04, WTS-05, WTS-08
- DR-02-01: WIN-06
- DR-02-09: TM-MD-04
- DR-03-09: TM-WT-06
- DR-03-12: TM-WT-08
- DR-04-01: TM-WT-02
- DR-04-02: TM-WT-02
- DR-04-03: TM-WT-03
- DR-04-04: TM-WT-03
- DR-04-05: TM-WT-03
- DR-04-06: TM-WT-03
- DR-04-07: TM-WT-03
- DR-04-08: TM-WT-01, TM-WT-05
- DR-04-09: TM-WT-01
- DR-04-11: TM-MD-04
- DR-04-12: TM-TT-01, TM-WT-04
- DR-07-21: TM-FC-04
- DR-11-01: SRF-03, SRF-05, SRF-16, WIN-07
- DR-11-02: TM-LS-01
- DR-11-03: SRF-08, SRF-14, SRF-15
- DR-11-04: CTL-01, CTL-03, CTL-04, CTL-05, CTL-09, SRF-13
- DR-11-05: CTL-06, SRF-01, SRF-02, SRF-04, SRF-07, SRF-08, SRF-13, SRF-14
- DR-11-06: CTL-01, CTL-04, CTL-06, CTL-07, SRF-07, SRF-13
- DR-11-07: SRF-02, SRF-03, SRF-05, SRF-08, SRF-09
- DR-11-08: SRF-09, SRF-10
- DR-11-09: SRF-11
- DR-11-10: SRF-12
- DR-11-11: SRF-04, SRF-06
- DR-12-01: PD-05, PD-06, PD-12, PD-13
- DR-12-02: PD-03
- DR-12-03: PD-03
- DR-12-04: PD-15
- DR-12-05: PD-00, PD-01, PD-02
- DR-12-06: PD-15
- DR-12-07: PD-02, TM-MD-03
- DR-12-08: TM-MD-01, TM-MD-02
- DR-12-09: RE-01, RE-02, RE-03
- DR-12-10: AGT-05, AGT-09, RE-01, RE-02
- DR-12-11: CO-01, CO-04, CO-05
- DR-12-12: CO-03, CO-04
- DR-12-13: CO-01, CO-03
- DR-12-14: CO-03
- DR-12-15: CO-02
- DR-12-16: PD-07, PD-08
- DR-12-18: PD-11
- DR-13-01: EX-02, EX-11
- DR-13-02: EX-02
- DR-13-03: EX-12
- DR-13-04: EX-07
- DR-13-05: EX-01, EX-07, EX-09
- DR-13-06: EX-01, EX-11
- DR-13-07: EX-04, EX-05, EX-06, EX-08, EX-09
- DR-13-08: EX-05
- DR-13-09: EX-03
- DR-13-10: EX-12
- DR-13-11: EX-10, PD-00, TM-IC-05
- DR-13-15: EX-12
- DR-13-16: EX-12
- DR-13-17: IC-01, IC-03, TM-IC-01, TM-IC-03, TM-IC-04
- DR-13-18: IC-03, TM-IC-01, TM-IC-04
- DR-14-01: AGT-06, AGT-08, CTL-01, CTL-02, CTL-08
- DR-14-02: AGT-01, AGT-03, AGT-05, AGT-08, AGT-09, CTL-01, CTL-03, CTL-08, CTL-09
- DR-14-03: AGT-02, AGT-05, CTL-08, WIN-06, WIN-10
- DR-14-04: AGT-02, AGT-07, CTL-02
- DR-14-05: AGT-03, AGT-06
- DR-14-06: AGT-04
- DR-14-08: AGT-07
- DR-14-09: AGT-07
- DR-14-12: AGT-01, AGT-04
- DR-14-13: AGT-06
- DR-14-15: AGT-03, AGT-09, CTL-03, CTL-05, CTL-08
- FR-010: TM-IC-01
- FR-041: TM-FC-04
- L-AUTO-7: AU-04
- L-MEDIA-1: TM-MD-02
- L-MEDIA-2: TM-MD-01
- L-MEDIA-3: TM-MD-01
- L-MEDIA-4: TM-MD-03
- L-MEDIA-5: TM-MD-02
- L-MEDIA-6: TM-MD-04
- M-AUDIO: MX-03, MX-33
- M-AUDIO-PRECISION: MX-03, MX-33
- M-CPU: DEV-23, DEV-24, MX-28, MX-33
- M-CUBASE-CUE: MX-08, MX-18, MX-22
- M-LIVE-CLIP: TM-SV-01, TM-SV-03
- M-LIVE-UI: WTS-11
- M-LIVE-UNDO: MX-23
- M10-INV-REX: TM-MD-05
- M2-A-MIDI: MI-02
- M2-A-WARP: TM-MD-04, TM-WT-01, TM-WT-02, TM-WT-03, TM-WT-04, TM-WT-07
- M2-C-FADE: ED-10
- M2-C-RANGE: ED-01
- M3-CONTROLLER: SRF-08, SRF-14, TM-LS-01, TM-SV-01
- M3-EXPORT-INTERCHANGE: DEV-29, IC-01, TM-IC-02, TM-IC-03, TM-IC-04
- M3-LAUNCH: TM-SV-02, TM-SV-03, TM-SV-04, TM-WT-04
- M3-MEDIA: TM-FC-03, TM-MD-02, TM-MD-03
- M3-ROUTE: MX-07, MX-08, MX-09, MX-22
- M3-UI-WORKSPACE: WIN-02
- M4-A-BASE: TM-FC-01, TM-FC-02
- M4-C-EXPORT: TM-IC-03, TM-IC-04
- M5-A5C-001: MX-01, MX-22
- M5-A5C-002: MX-23
- M5-A5C-003: MX-02
- M5-A5C-007: MX-26
- M5-A5C-009: MX-22
- M5-A5C-010: MX-15
- M5-A5C-011: MX-07, MX-22
- M5-A5C-012: MX-22
- M5-A5C-018: TM-FC-03
- M5-A5C-019: DEV-23, DEV-24, MX-28
- M5-A5C-022: TM-FC-02
- M5-A5H-034: DEV-25
- M5-C5W-030: MX-09
- M6-A6C-039: TM-WT-03
- M6-A6G-006: MX-03
- M6-A6G-011: MX-13, MX-33
- M6-A6H-007: MX-22
- M6-A6H-008: MX-22
- M6-A6H-012: MX-16
- M6-A6H-023: TM-FC-01
- M6-A6H-034: MX-04, MX-22
- M6-A6H-042: DEV-01, DEV-02
- M6-A6I-013: TM-MD-01
- M7-A7A-001: DEV-22
- M7-A7B-001: DEV-22
- M8-A8D-001: DEV-04
- N10H-002: HI-02
- N10H-015: DEV-20
- N10H-016: DEV-20
- N10H-043: MX-06
- N10H-044: MX-05, MX-06
- N10H-045: MX-20, MX-24
- N10H-046: MX-20, MX-24
- N10H-047: MX-24
- N10H-048: MX-04
- N10H-050: MX-28
- N10H-051: MX-28
- N10H-053: TM-MD-03
- N11H-001: DEV-06
- N11H-004: DEV-07
- N11H-005: DEV-07
- N11H-006: DEV-01, DEV-03
- N11H-007: DEV-03
- N11H-008: DEV-03
- N11H-013: DEV-03
- N11H-015: DEV-03
- N11H-016: DEV-03
- N11H-017: DEV-14
- N11H-018: DEV-06, DEV-07
- N11H-028: DEV-26
- N11H-030: DEV-06, DEV-07
- N11H-032: DEV-27
- N11H-033: DEV-26
- N11H-035: DEV-01, MX-24
- N11H-036: DEV-01, MX-24
- N11H-037: DEV-01, MX-24
- N11H-038: DEV-13, DEV-15
- N11H-039: DEV-14, DEV-18
- N11H-040: DEV-05, DEV-14
- R10D-021: CTL-01
- R10H-001: MX-16
- R10H-002: MX-05
- R10H-003: MX-16, MX-25
- R10H-005: MX-16
- R10H-006: MX-16
- R10H-007: MX-16, MX-25
- R10H-008: MX-25
- R10H-010: MX-08, MX-18, MX-29
- R10H-011: MX-04, MX-09
- R10H-012: MX-24
- R10H-014: MX-28
- R10H-017: TM-FC-03, TM-FC-04
- R10H-018: TM-FC-03
- R10H-020: CT-01
- R10H-021: CT-01
- R10H-022: CT-01, ED-07
- R10H-023: CT-04
- R10H-024: CT-04
- R10H-025: CT-04
- R10H-029: RE-02
- R10H-030: ED-03
- R10H-031: ED-03
- R10H-035: ED-05
- R10H-039: CT-02
- R10H-051: AU-05
- R10H-052: AU-03
- R10H-053: AU-09
- R10H-061: HI-02, HI-03, PD-03
- R10H-073: DEV-28, DEV-29, EX-05, MX-10, MX-27
- R10H-079: AGT-04, EX-01
- R11E-002: ED-01
- R11E-003: ED-04, ED-05
- R11E-005: TM-FC-02
- R11E-009: ED-06, TM-FC-02
- R11E-010: ED-06, TM-FC-02
- R11E-011: ED-06, TM-FC-02
- R11E-013: ED-03, ED-08
- R11E-014: ED-08
- R11E-015: ED-03, ED-08
- R11E-016: ED-07
- R11E-017: ED-03, ED-07
- R11E-018: ED-02, ED-07
- R11E-019: ED-07
- R11E-024: CT-05
- R11E-026: MX-13, TM-WT-08
- R11E-027: TM-WT-01
- R11E-028: TM-WT-01
- R11E-029: TM-WT-01
- R11E-033: TM-WT-02
- R11E-034: TM-WT-02
- R11E-036: TM-WT-02
- R11E-038: TM-WT-04
- R11E-040: CT-01, CT-03
- R11E-041: CT-04
- R11E-042: CT-04
- R11E-054: AU-03, AU-05, ED-02
- R11E-055: AU-05
- R11X-001: EX-02
- R11X-002: DEV-29, EX-02
- R11X-003: EX-02
- R11X-005: EX-02
- R11X-006: EX-03
- R11X-007: EX-03
- R11X-011: EX-07, PD-04
- R11X-012: EX-07
- R11X-013: EX-01, EX-09, TM-IC-05
- R11X-014: EX-09
- R11X-015: EX-09
- R11X-016: EX-07
- R11X-017: EX-07
- R11X-018: EX-07
- R11X-019: EX-04
- R11X-020: EX-04
- R11X-021: EX-05, MX-27
- R11X-022: EX-12, TM-MD-05
- R11X-023: EX-08, TM-IC-04, TM-MD-04
- R11X-024: EX-08, TM-IC-04, TM-MD-04
- R11X-025: DEV-28, DEV-29, EX-05, MX-10, MX-27
- R11X-026: EX-12
- R11X-027: EX-05
- R11X-028: EX-01
- R11X-029: EX-12, TM-FC-02
- R11X-030: EX-12, TM-FC-02
- R11X-031: EX-12
- R11X-032: EX-12
- R11X-033: EX-12
- R11X-034: DEV-29, EX-05, EX-06
- R11X-035: EX-05
- R11X-036: EX-03
- R11X-038: EX-07
- R11X-040: EX-07, PD-04
- R11X-041: EX-04, EX-07
- R11X-042: EX-08
- R11X-043: DEV-29, EX-05
- R11X-044: EX-02
- R11X-046: EX-03
- R11X-047: DEV-29, EX-05, MX-27
- R3-001: HI-02, HI-03, HI-04, PD-03
- R3-002: PD-15

731 research IDs cited by 219 rows.
