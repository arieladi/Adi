# Ableton Live 12 — research v0.6, 2026-11-04

Baseline: `5c9e1cd8ebbb5dd940815aa733d49c1217c9aa19`. Researcher: Codex on SALON-TV. Installed: Suite executable file version 12.4; existing Live log reports `Live 12.4 Build: 2026-04-24_d85a94ab5e`, revision `d85a94ab5eb1299391a7ff9400bdb8c0dcd7cc75`. This is not a fresh-default profile. PE identity and hashes: E01; log identity: E02.

Authority: current `docs/FEATURES.md`, `docs/DECISIONS.md`, `docs/LANES.md` and window parity checklists. Dream Reference v0.2 is director-facing context, not a superseding specification (director clarification, 2026-11-04). No product changes or live GUI tests were made.

Evidence keys resolve under private `D:\ADI-Research\2026-11-04-daws\EVIDENCE-INDEX.md`. Proprietary binaries, code extracts, projects and raw results stay there, outside this repository. Manual page numbers below are the printed pages in the local extracted chapters under git-ignored `reference/DOCS/`.

Statuses distinguish static facts, measured behavior, supported hypotheses and unknowns. A manual claim is labelled a supported hypothesis about the installed build. No row in this milestone is confirmed by DAW measurement. A string, import, symbol or pseudocode result does not prove a feature executes, its GUI behavior, its DSP precision or its CPU cost.

## Window lane: ten normalized questions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| L01: unresolved — Select All: earliest clip or 1.1.1; inclusion of empty tracks and returns | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | parity-7.3-arrangement row 36; window/select-all | Unknown / not tested |
| L02: unresolved — Return Pre/Post: which state is lit, including a return with no incoming sends | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | N42; parity-7.6-mixer row 17 | Unknown / not tested |
| L03: unresolved — New return: generated name, letter and colour; reorder and undo effects | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | window/insert-return; ADR-0203 d3; ADR-0211 | Unknown / not tested |
| L04: unresolved — Drop audio/MIDI on a track that cannot accept it: reject, convert, insert or reroute | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | window/clip-to-track; FEATURES §3; hybrid-track deviation retained | Unknown / not tested |
| L05: unresolved — Grouped track output chooser: literal Group or actual group name, including nesting | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | parity-7.6-mixer row 20 | Unknown / not tested |
| L06: unresolved — MIDI track Arm glyph without/with instrument, inactive/armed/implicit arm | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | two separate Measure on Live lines in window.md; parity-7.6-mixer row 18 | Unknown / not tested |
| L07: unresolved — All 70 palette colours and row/column order in a precisely recorded theme | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | ADR-0211 d3; parity-7.3-arrangement row 34 | Unknown / not tested |
| L08: unresolved — Colour chooser position in track/group/clip context menus | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | window/colour-chooser; parity-7.3-arrangement row 34 | Unknown / not tested |
| L09: unresolved — Ctrl+Alt+I on Windows versus Option+Cmd+I on macOS: Mixer or Arrangement Track Controls | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | parity-7.2-shell row 24; parity-7.6-mixer row 19 | Unknown / not tested |
| L10: unresolved — Independent Sends visibility in Mixer and Arrangement Track Controls | Existing window log Measure on Live lines; Claude mission M-LIVE-UI | window/sends-section; current ADI shared toggle requires verification | Unknown / not tested |

## Automation and undo

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Lock Envelopes fixes envelopes to arrangement time when clips move; single-lane edits affect that envelope. This is a documented rule, not an observed drag/undo result. | Live manual §25.5.6–25.5.7, p.502 | N26; FEATURES §7; preserve ADI typed operation/transaction boundaries | Supported hypothesis — documented, not measured on installed build |
| Re-Enable Automation restores overridden automated controls. Which per-control/global overrides clear, lane order, and the unfold triangle by track kind remain unmeasured. | §25.2, p.494; §25.5; M-LIVE-AUTO | N26; parity-7.3 rows 38–39; ADR-0110 | Supported hypothesis — documented, not measured on installed build |
| Per-gesture undo granularity, intermediate states, menu labels, cancellation and redo are unmeasured. No existing InteractionLogs directory was found at the logger README default path. | tools/interaction_logger/README.md; M-LIVE-UNDO | ADR-0003, ADR-0110, ADR-0124; do not infer journal grouping from mouse events alone | Unknown / not tested |

## Info View, Status Bar, palette and defaults

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Exact context-dependent text for every control, all Settings factory defaults, and the factory default Set remain unmeasured. Existing user preferences cannot establish factory defaults. | M-LIVE-TEXT and M-LIVE-DEFAULTS; AWAITING rows 14 and 16 | ADR-0131; SETTINGS-CATALOGUE; parity-settings; preserve approved deviations | Unknown / not tested |
| The installed Akai controller colour module contains 70 7-bit RGB triples. Push/Move also contain 70-entry controller index mappings. These are not a measured 8-bit on-screen palette and must not replace the UI palette. | E05; Akai_Force_MPC/live_colors.pyc, COLOR_INDICES_TO_RGB_COLORS; read as data, never executed | ADR-0211 d3: keep displayed-RGB measurement open; do not multiply controller values by two and call them measured | Confirmed by static analysis |

## Clip View, warp and Session launch

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Clip controls and warp operations are documented, but marker insertion/move/delete, transient conversion, loop-boundary edits, modifier behavior and undo need a build-bound session. | §8 pp.195–228; §9 pp.229–246; M-LIVE-CLIP | FEATURES §3; AWRP; ADR-0061; imported clips remain Warp off by ADI decision | Supported hypothesis — documented, not measured on installed build |
| The manual distinguishes neutral unstretched Beats/Tones/Texture/Re-Pitch playback from Complex/Complex Pro; no equality claim is made here for 12.4. | §38.2.3 pp.960–961; E06; M-AUDIO | ADR-0207; mode-specific numerical tests; do not promise Rubber Band is Live-identical | Supported hypothesis — documented, not measured on installed build |
| Global versus clip launch quantization, legato/retrigger and queued launch changes remain to be measured. | §16 Launching Clips pp.355–366; M-LIVE-CLIP | FEATURES §5; ADR-0117; Session remains P2 under ADR-0203 d10 | Unknown / not tested |

## Browser hierarchy, search and tags

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Shipped browser defaults declare filter order beginning Content, Type, Sounds, Drums, Character, Devices, Function, Format, MIDI Tools, Clips, Genres, Key, Grooves, Tunings, Creator. This is resource order, not proof of every visible category or search result. | E04; Resources/Builtin/Ableton Folder Info/filter-order-defaults.json | N15; ADR-0104; use resource inventory as a test oracle for browser missions | Confirmed by static analysis |
| Collections, Library and Places, search, filtering and preview behavior are documented. Search ranking, persistence and tag-edit undo have not been observed. | §4 pp.60–108; M-LIVE-BROWSER | N15; app-scoped library; Dream §2 is context | Supported hypothesis — documented, not measured on installed build |

## Audio engine and interfaces

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| The shipped Live Engine DLL exposes 16 RWDEF entry points. Targeted disassembly/decompilation includes DriveAudio, SetAudioInfo and GetDeviceNameAndVersion. This does not establish that the standalone mixer runs through this DLL. | E07; RWDEFDriveAudio VA 0x180e9dd30, SetAudioInfo 0x180e9ea10; exact DLL hash in E01 | A candidate integration boundary only; locate actual standalone callback before specifying engine scheduling | Confirmed by static analysis |
| The manual describes float32 processing with float64 accumulation at individual mix points. Installed-build stage boundaries and numerical results are still unmeasured. | §38.2.4 p.961; E06; M-AUDIO-PRECISION | ADR-0207 d2 already specifies ADI behavior; research verifies assumptions, not a new ADR | Supported hypothesis — documented, not measured on installed build |
| Audio scheduling, pan law, compensation, silence/bypass/tails, warp engines and CPU efficiency are not established by this milestone. Existing log reports 44.1 kHz / 4096 output frames; these are historical user-session values. | E02; M-AUDIO and M-CPU; AWAITING rows 5,7–9 | Engine validation queue; no unmeasured performance claims | Unknown / not tested |

## Expanded host research — 2026-11-04

Scope expanded by the director: all host functions are in the research inventory; built-in instrument and effect implementations are excluded. Editing utilities, MIDI transformations, modulation, routing, hosting and modular-system semantics remain in scope. Rows below are behavior contracts and ADI design implications, not new ADRs or permission to implement. An unknown is not a negative finding. Private evidence: E17–E20; precise validation missions: DEEP-CLAUDE-MISSIONS.md.

### Warp: time domains, markers and nonlocal effects

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A-W01: Warp off plays at the sample’s original tempo independently of Set tempo. Import short/long policies and the default Warp Mode are separate preferences. | Live 12 local manual, §9.2, pp.234–235; exact manual build not established | ADR-0061 / ADR-0132 / ADR-0203 d7: ADI imports Warp off by decision; retain source time, beat placement and enabled state separately. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W02: A warp marker anchors a source position to musical time. Shift-drag adjusts the waveform beneath an anchor; moving the marker changes the timeline anchor. | Live 12 local manual, §9.2.3, pp.235–236; exact manual build not established | AWRP: expose source-seconds and beat coordinates distinctly; undo must restore both and any affected neighboring segment. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W03: Transients and pseudo-markers are not warp markers. Promoting a pseudo-marker with Ctrl/Cmd can anchor adjacent transients; inserting across a range without transients creates boundary markers. | Live 12 local manual, §9.2.3.1, p.236; exact manual build not established | ADR-0176: keep analysis candidates separate from user constraints; test promotion, manual transient reset and no-transient selection. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W04: Creating a marker with no later warp marker can also change clip tempo. Resetting transients removes manual changes, not necessarily the warp map. | Live 12 local manual, p.236; exact manual build not established | AWRP / M2-A-WARP: test rightmost-marker edits and detector reset separately; do not assume all drags are local. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W05: Markers save with the Set; explicit sample-default Save also preserves them for later imports, overriding automatic warp unless Warp From is invoked. | Live 12 local manual, §9.2.3.2, p.237; exact manual build not established | ADI: distinguish project state from library analysis/default state; editing one clip must not silently change later imports. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W06: Multi-clip warp edits propagate for same-length selected clips. Warp From variants reset the right side and leave earlier markers unchanged; Straight and fixed-BPM modes differ. | Live 12 local manual, §9.2.4.4, p.239; §9.2.5.1, p.242; exact manual build not established | ADR-0176: add multi-mic phase/coherence tests, unequal lengths, trimmed copies and locked anchors; no equivalence claim from shared marker positions alone. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W07: Quantize moves transients toward the chosen grid, with an Amount control for partial movement. | Live 12 local manual, §9.2.7, pp.243–244; exact manual build not established | ADI: materialize resulting anchors as an undoable edit; record selection and rounding, including triplets and tempo ramps. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W08: Arrangement tempo leaders derive tempo from warp markers. Among simultaneous leaders the bottom-most track takes precedence; generated tempo automation becomes editable through Unfollow. | Live 12 local manual, §9.1.4, pp.231–233; exact manual build not established | FEATURES §1: this is an optional reference contract, not an ADI priority change. Model precedence and derived tempo separately from user tempo points. | Supported hypothesis — documented; installed behavior unmeasured |

### Warp modes: control contracts, not reconstructed algorithms

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A-W09: Beats preserves transients or fixed beat divisions; Loop Off leaves gaps, Forward repeats a segment, and Back-and-Forth reverses repeat direction. Transient Envelope controls decay between slices. | Live 12 local manual, §9.3.1, pp.244–245; exact manual build not established | AWRP: separate onset partitioning, segment looping and amplitude envelope; use isolated transients and long decays for M2-A-WARP. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W10: Tones adapts grain size to the source’s pitch characteristics; Texture exposes grain size and randomness without that pitch dependence. | Live 12 local manual, §9.3.2–3, p.245; exact manual build not established | ADR-0061: controls alone cannot identify synthesis kernels. Test periodic, noisy and mixed inputs; preserve random state in reproducibility notes. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W11: Re-Pitch couples speed and pitch and disables separate transposition. Doubling speed gives an octave rise. | Live 12 local manual, §9.3.4, p.246; exact manual build not established | ADI: test resampling and boundary continuity separately from independent time/pitch stretch. | Supported hypothesis — documented; installed behavior unmeasured |
| A-W12: Complex Pro exposes formant preservation and Envelope; the manual gives Envelope 128 as default and says formants have no effect without transposition. | Live 12 local manual, §9.3.5, p.246; exact manual build not established | AWRP: use transposed and untransposed controls; never promise Live-identical output from an unrelated algorithm. | Supported hypothesis — documented; installed behavior unmeasured |

### MIDI editor: selection, collisions and expression ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A-M01: Time selection and note selection are distinct; Enter switches between them. Pitch/time utilities can act on the whole clip when no notes or time are selected. | Live 12 local manual, §10.5.2 pp.254–255; §10.5.9 p.263; exact manual build not established | FEATURES §4 / window parity: bind command targets to editor focus and selection type; an empty selection is not necessarily a no-op. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M02: Find/Select filters combine pitch, time, chance, condition, count, duration, scale and velocity. Each supports inversion/reselection; clicking the editor clears active filters and selection. | Live 12 local manual, §10.5.3 pp.255–257; exact manual build not established | ADI: selection queries can feed normal typed edits; define combination semantics and avoid logging selection as note mutation. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M03: A new overlapping note can replace an existing note’s beginning or shorten its end. Note stretch can cross its other boundary and reverse order. | Live 12 local manual, §10.5.4 p.257; §10.5.6 p.259; exact manual build not established | ADI MIDI: test same-pitch collisions, reversal and stable IDs; preview must show deletions and truncations before commit. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M04: Split divides notes; Chop partitions selected notes on the grid; Join merges same-pitch notes and preserves/join their MPE envelopes. | Live 12 local manual, §10.5.8 pp.260–262; exact manual build not established | FEATURES §4 / ADR-0054: notes and expression are one edit payload; test envelope continuity and overlap during join. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M05: Stretch adjusts linked clip-envelope timing; unlinked envelopes are unaffected. Moving notes moves their expression, and stretching scales per-note expression. | Live 12 local manual, p.259; §12.2 p.330; exact manual build not established | ADI: model clip envelope time base separately from note-relative expression. Test loop boundaries and note identity remapping. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M06: Randomize changes stored velocity once; Velocity Deviation supplies a fresh playback value within a signed range. These are distinct controls. | Live 12 local manual, §10.5.12 pp.274–275; exact manual build not established | ADI humanize/probability: distinguish materialized random edits from runtime randomness; redo should restore edited notes exactly. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M07: Probability groups can play all members together or choose one. A note moves out of its previous group when assigned to another; group type can persist as the last-used grouping action. | Live 12 local manual, §10.5.13.1 pp.278–279; exact manual build not established | ADI: retain group identity and grouped probability semantics; independent Bernoulli trials cannot reproduce Play One. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M08: Fold to Notes, scale highlighting, Fold to Scale and pitch fitting are separate. Fold to Scale keeps rows containing existing out-of-scale notes visible. | Live 12 local manual, §10.6 pp.279–282; exact manual build not established | ADR-0178 / FEATURES §4: never delete or retune hidden notes merely because the display is folded. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M09: MIDI Time commands shift note content without changing clip start/end or loop brace. Cropping MIDI removes data without making a new media file. | Live 12 local manual, §10.7 pp.282–284; exact manual build not established | ADI: separate content extent, loop extent and arrangement placement; compare with audio consolidation, which has different media effects. | Supported hypothesis — documented; installed behavior unmeasured |
| A-M10: Multi-clip Focus restricts editing to the active clip; without it notes from several clips can be edited, but Velocity/Chance lanes still operate on one foreground clip. | Live 12 local manual, §10.8 pp.284–286; exact manual build not established | ADR-0047: multi-clip selection does not imply every editor lane is multi-target. Test changes in focus while a gesture is active. | Supported hypothesis — documented; installed behavior unmeasured |

### MIDI Tools: draft/apply semantics and full editor-tool families

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A-T01: Auto Apply previews note changes; disabling it restores original notes. Apply commits the current transform. Undo/Redo affects notes, not tool parameters; Reset changes tool settings, not notes. | Live 12 local manual, §11.1 pp.288–291; exact manual build not established | ADR-0110: keep tool draft, input snapshot and resulting events separate; exact undo grouping is M2-A-MIDI, not established by the manual. | Supported hypothesis — documented; installed behavior unmeasured |
| A-T02: Transform targets selection or loop and replaces input notes. The introduction says generators replace existing notes, while the detailed workflow says nonoverlapping notes remain and overlapping ones are replaced. | Live 12 local manual, §11 introduction p.288 versus §11.1 p.290; exact manual build not established | Do not resolve this documentary ambiguity by guessing. M2-A-MIDI tests same/different pitch, partial overlap, selected/unselected notes and repeated Apply. | Supported hypothesis — documented; installed behavior unmeasured |
| A-T03: Pitch parameters use scale degrees when a clip scale is active. Tools include Arpeggiate (style, rate, gate, steps) and Chop (2–64 pieces, gaps, emphasis and variation). | Live 12 local manual, pp.290,292–294; exact manual build not established | FEATURES §4: editor transformations remain in scope even though instrument/MIDI-effect implementations are excluded. Store resulting notes, not a hidden playback effect. | Supported hypothesis — documented; installed behavior unmeasured |
| A-T04: Connect adds randomized gap-filling notes with spread/density/rate/tie. Ornament adds flam or grace notes and can accumulate extra notes on repeat application. | Live 12 local manual, pp.295–296,301–304; exact manual build not established | ADI: define repeated Apply as a new operation, distinguish selection baseline from previous output, and test notes preceding zero. | Supported hypothesis — documented; installed behavior unmeasured |
| A-T05: Quantize independently targets starts and ends with partial amount. Recombine permutes one dimension in the order Shuffle, Mirror, then Rotate. | Live 12 local manual, pp.304–307; exact manual build not established | ADI: preserve untouched dimensions, specify deterministic redo and test same-time notes/ties in permutation ordering. | Supported hypothesis — documented; installed behavior unmeasured |
| A-T06: Span changes articulation; Strum offsets note timing; Time Warp applies a speed curve with optional quantization, preserved total range and inclusion of note ends. | Live 12 local manual, pp.308–313; exact manual build not established | ADI: these are different timing operations. Record transformation domain, endpoint policy and preservation of MPE for M2-A-MIDI. | Supported hypothesis — documented; installed behavior unmeasured |
| A-T07: Glissando joins successive note pitches through an MPE bend; the LFO transformation writes pitch, slide or pressure curves; Velocity Shaper writes velocities from a curve. | Live 12 local manual, pp.297–301,314–315; exact manual build not established | ADI expression editing: generated curves belong to notes; this study covers their editing contract, not bundled device internals. | Supported hypothesis — documented; installed behavior unmeasured |
| A-T08: Rhythm generates step/density patterns; Seed constrains random pitch/duration/velocity/polyphony; Shape maps a drawn contour to notes; Stacks generates chords; Euclidean supports four voices. | Live 12 local manual, §11.3 pp.316–324; exact manual build not established | FEATURES §4 / future editor tools: reserve parameterized edit proposals and deterministic committed output; exact RNGs and every numerical range remain unverified. | Supported hypothesis — documented; installed behavior unmeasured |
| A-T09: Stacks supports JSON chord-bank files with a .stacks extension; individual chords keep their own root, inversion, duration and offset. | Live 12 local manual, pp.321–322; exact manual build not established | ADI: keep reusable generator settings separate from resulting note content; custom chord-bank compatibility is a possible later item, not an adopted format. | Supported hypothesis — documented; installed behavior unmeasured |

### MPE and host export boundaries

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A-X01: Live edits per-note pitch, slide, pressure, velocity and release velocity. Input MPE locks the track channel chooser to All Channels; expression can also be added to notes created without an MPE controller. | Live 12 local manual, §12 pp.325–326; exact manual build not established | FEATURES §4 / ADR-0054: expression is model data, not merely raw incoming MIDI channel messages. | Supported hypothesis — documented; installed behavior unmeasured |
| A-X02: Expression grid state is independent and saved per clip; it starts disabled. Moving a segment inserts selection-edge points, while dragging over a neighboring point can remove it. | Live 12 local manual, §12.2 pp.328–330; exact manual build not established | ADI: editor grids and gesture coalescing must be scoped correctly; test boundary point creation, collision and undo. | Supported hypothesis — documented; installed behavior unmeasured |
| A-X03: One track outputs one MPE zone. Upper/lower zone and note-channel count are configurable; the manual states zones apply to MPE output. | Live 12 local manual, §12.6 pp.331–334; exact manual build not established | ADR-0054 / host adapters: serialize routing configuration independently of expression; test voice exhaustion and pitch-bend range after restore. | Supported hypothesis — documented; installed behavior unmeasured |

### Installed serialization: direct evidence useful for ADI format tests

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A-S01: Twenty-five shipped .als files were parsed read-only as XML data. In the Suite demo, WarpMarker attributes include SecTime, BeatTime and Id; AudioClip also carries IsWarped, Onsets, Loop and WarpMode. | E17: evidence/deep/live-shipped-project-structure.json; Suite demo SHA-256 90ff59cdf8b83eaa33c54392595f968aa8ed41188a3eedd930368bc2207c9a28 | AWRP: independent time coordinates and analysis/edit layers are concrete persistence targets. This is one resource schema sample, not complete .als compatibility. | Confirmed by static analysis — scope limited to the named artifact |
| A-S02: The same demo stores NoteId on note events and PerNoteEventList, with separate time offsets and curve control points. Clip envelopes include their own LoopSlot. | E17; the demo declares Creator Ableton Live 12.4d1, which is not the running build identity | ADI format: test expression ownership and independent loop timing. Names do not prove interpolation math, ID lifetime or runtime evaluation order. | Confirmed by static analysis — scope limited to the named artifact |
| A-S03: Shipped Default Audio Track.als declares Creator 12.4d1; a resource named default is not proof of the user’s new-track behavior or a clean factory Set. | E17; SHA-256 22114f48f58b439336955768a841af56f088e9022688ebe799df02da8634d3a3 | Keep M-LIVE-DEFAULTS open; user templates and preferences can override shipped resources. | Confirmed by static analysis — scope limited to the named artifact |

### Recording, comping, grooves and envelope evaluation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A-H01: Groove timing applies to MIDI and warped audio. Quantize precedes groove timing; the global amount scales timing/random/velocity. Commit writes notes or warp markers and removes the groove assignment. | Live 12 local manual, §14 pp.340–344; exact manual build not established | FEATURES §3 groove pool: retain a non-destructive groove reference until Commit; redo must restore the committed result, including random offsets. | Supported hypothesis — documented; installed behavior unmeasured |
| A-H02: Arrangement recording quantization is separately undoable, but loop-overdub quantization changes are documented as not separately undoable. | Live 12 local manual, §19.5 p.412; exact manual build not established | ADR-0110: add a concrete exception to the undo measurement matrix; do not infer a universal recording transaction boundary. | Supported hypothesis — documented; installed behavior unmeasured |
| A-H03: Capture listens to armed/monitored MIDI tracks. In an empty stopped Set it creates clips in the focused view, detects tempo and starts playback; earlier played material can remain before the clip start. | Live 12 local manual, §19.10 pp.415–416; exact manual build not established | ADR-0132 / FEATURES §4: capture buffer, inferred phrase, clip start and all retained input are distinct. Existing-Set behavior still needs a separate test. | Supported hypothesis — documented; installed behavior unmeasured |
| A-H04: Comping copies selected take material to the main lane as independent clips. One take per track can be auditioned; source highlights require matching positions/properties. | Live 12 local manual, §21 pp.427–429; exact manual build not established | FEATURES §3: edits to the comp must not silently alter a source take. Test highlights after slip, crop and note-expression edits. | Supported hypothesis — documented; installed behavior unmeasured |
| A-H05: Clip pitch modulation adds to transpose; clip gain/send modulation is relative and cannot exceed its base setting. Track volume modulation is post-effects. | Live 12 local manual, §26 pp.507–511; exact manual build not established | ADR-0046 / ADR-0115: one generic additive modulation formula is insufficient for Live parity; declare composition rules per parameter domain. | Supported hypothesis — documented; installed behavior unmeasured |
| A-H06: Arrangement Clip View offers modulation envelopes; absolute automation resides on track lanes. Session clips expose both automation and modulation. | Live 12 local manual, §26.1/26.3 pp.505,508–509; exact manual build not established | N26: distinguish envelope owner, time base and composition; UI colour alone is not a semantic model. | Supported hypothesis — documented; installed behavior unmeasured |


## Host-wide coverage queue

The whole host is in scope; these rows keep unclosed areas visible. They are research questions, not claims about the app or new ADI commitments. A feature named below is not established absent merely because it is not covered above.

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| To establish: Transport, tempo, sync, launch and scenes | Source inventory: Ch.6–7,9,16,36 | Quantization boundaries, queued/retrigger/legato/follow actions, tempo/meter changes, Link disconnect/reconnect. FEATURES §1/5; ADR-0117. | Unknown / not tested — remaining scope |
| To establish: Recording, comping, capture and monitoring | Source inventory: Ch.17,19–21 | Loop/punch/arm interactions, capture state machine, clock changes, monitoring delay, take identity. FEATURES §8; ADR-0132; N23. | Unknown / not tested — remaining scope |
| To establish: Automation and undo | Source inventory: Ch.25–26; ten original window questions | Every gesture and its exact journal grouping, override/re-enable, linked/unlinked envelopes and lane order. N26; ADR-0110. | Unknown / not tested — remaining scope |
| To establish: Browser, tags, search, files and project recovery | Source inventory: Ch.4–5 | Ranking, AND/OR filters, hot-swap cancel, missing media, collecting external files, Save As/Copy, crash recovery. N15; FEATURES §8/9. | Unknown / not tested — remaining scope |
| To establish: Routing, mixer, cue, host devices and racks | Source inventory: Ch.17–18,23–24 | Tap placement, sends/returns, feedback, pan laws, plug-in state/latency, macros and nesting; processor algorithms excluded. N23/N42/N43; ADR-0060. | Unknown / not tested — remaining scope |
| To establish: Audio conversion, tuning, video and stem separation | Source inventory: Ch.13,15,22,27 | Conversion placement and undo, custom tuning import/output, video sync/export, separation job/source handling. FEATURES §3/4/8; ADR-0178/0206. | Unknown / not tested — remaining scope |
| To establish: Default Set, all settings, text, accessibility and control surfaces | Source inventory: Ch.3,33–35,40–41 plus installed UI | All defaults and context text, keyboard focus, screen reader, scaling, MIDI takeover/relative controls, hardware-specific workflows. ADR-0131/0181; parity-settings. | Unknown / not tested — remaining scope |
| To establish: Engine, performance and numerical behavior | Source inventory: Ch.37–39; E07; original core-engine matrix | Precision per stage, block scheduling, denormals, latency, restore and offline/playback differences. ADR-0207; M-AUDIO/M-CPU. No measured results yet. | Unknown / not tested — remaining scope |



## Whole-Dream research expansion — 2026-11-04

Coverage is driven by [the whole-feature register](../DAW-RESEARCH-COVERAGE.md), not just the initial examples. Manual claims remain supported hypotheses about the installed build; all live behavior, undo and performance checks remain pending. REAPER is included only where Dream/current ADI explicitly references it. E25/E26 identify new static artifacts. See whole-host test missions in the private research root.

### Session playback and musical clocks

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A3-001: Trigger reacts to press; Gate also stops on release; Toggle alternates on successive presses; Repeat retriggers while held at clip quantization. | Live 12 local manual, §16.2 p.356; exact manual build unknown | Dream §1/5; FEATURES §5: separate input edges, pending launch and playing state; test press/release straddling a quantization boundary. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-002: Legato takes the previous clip’s play position, including with launch quantization off. The manual warns that unprepared disk positions can cause dropouts. | Live 12 local manual, §16.3 p.357; exact manual build unknown | Session: phase inheritance and preload policy are independent; test unequal loop lengths, nonzero clip start and RAM/disk modes. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-003: Follow groups are contiguous occupied slots separated by empty slots. Linked timing follows clip end/loop count; Unlinked timing uses elapsed bars/beats/sixteenths. | Live 12 local manual, §16.7 pp.361–362; exact manual build unknown | Session: group membership is not track-folder membership; preserve scheduled action identity when slots move. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-004: The detailed Follow Action text says Global quantization is bypassed but explicit clip quantization is honored. The general quantization paragraph is less specific. | Live 12 local manual, §16.4 p.358 versus §16.7 p.362; exact manual build unknown | Resolve this precedence on the installed build; do not implement a second global quantization delay from the general sentence alone. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-005: Scene Follow Actions take precedence when triggered; clip Follow Actions continue while a scene action is merely scheduled. There is also a global enable switch. | Live 12 local manual, §16.7 p.363; exact manual build unknown | Session: deterministic simultaneous-event ordering and global suspension need explicit tests, including recording the performance. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-006: Link synchronizes beat/tempo/phase; peers can independently start/stop. Start Stop Sync applies only among peers enabling it. | Live 12 local manual, §36.1 pp.944–946; exact manual build unknown | ADR-0185: separate musical synchronization from sample clock, audio transport and project collaboration; test join/leave during playback. | Supported hypothesis — documented; installed behavior unmeasured |

### Monitoring, routing and compensation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A3-007: Auto monitors armed input except while clips play; In suppresses clip output; Off disables input monitoring. The page separately documents reset defaults of Off for audio and Auto for MIDI. | Live 12 local manual, §17.1 p.369; exact manual build unknown | Dream §7; N23: model arm, monitor mode and clip playback jointly. The introductory “default Auto” phrase does not establish every track’s factory default. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-008: Keep Monitoring Latency in Recorded Audio changes recording placement to match monitored timing; it can be switched independently on In/Auto. | Live 12 local manual, §17.1 p.369; exact manual build unknown | Recording: distinguish captured sample time, monitor latency and final clip placement; measure acoustic/direct versus software monitoring. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-009: Mono input records mono; track device chains are documented as stereo. Mono is duplicated to L/R; mono output sums L/R with 6 dB attenuation. | Live 12 local manual, §17.2.1 p.370; exact manual build unknown | ADR-0207/channel layouts: file channel count, processing width and output conversion are separate contracts; test asymmetric and inverted stereo. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-010: MIDI port Track, Sync and Remote switches have different purposes. Remote output carries controller feedback; mapped keys are removed from instrument input. | Live 12 local manual, §17.3 pp.371–373; §33 p.836; exact manual build unknown | ADR-0181: classify control versus performance input before routing and logging; test collisions between controller maps and note recording. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-011: Clock Song mode sends SPP/Continue on location changes; Pattern mode uses Start at the next bar. Resync can send Stop+Start, Start only or nothing. | Live 12 local manual, §17.3.1 pp.372–373; exact manual build unknown | Transport/controller compatibility: test locate, loop, reconnect and note cleanup independently of GUI clock display. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-012: Reduced Latency When Monitoring exempts input-monitored tracks from compensation and can desynchronize them from compensated returns. Track Delay is unavailable with compensation disabled. | Live 12 local manual, §23.6 pp.469–470; exact manual build unknown | ADR-0203 d4/N23: define the low-latency graph cut and visible tradeoff; test monitored source through a latent return and sidechain. | Supported hypothesis — documented; installed behavior unmeasured |

### Automation ownership and controller gestures

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A3-013: Session automation becomes track automation when copied or recorded into Arrangement; recording Session playback always transfers its automation. | Live 12 local manual, §25.1–25.2 pp.491–494; exact manual build unknown | N26: conversion must bind destination parameter IDs/time coordinates and preserve source clip data on undo. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-014: Session automation recording with mouse ends on release; MIDI controller recording continues until the clip loop ends after movement stops. | Live 12 local manual, §25.2.1 p.494; exact manual build unknown | ADR-0110: one universal release rule is insufficient; test touch versus latch through loops, stopped transport and controller disconnect. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-015: A manual override suspends automation without deleting it. Re-enable can affect all controls, one parameter, or occur by relaunching a Session clip containing automation. | Live 12 local manual, §25.4 pp.494–495; exact manual build unknown | Keep stored envelope, override state and effective value separate; do not close actual lane/undo measurements from this manual description. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-016: Hiding an automation lane does not disable its envelope; additional lanes can be folded together. Discrete controls use discrete values. | Live 12 local manual, §25.5 pp.495–496; exact manual build unknown | N26: view visibility cannot gate playback. Test switches, lane reorder/fold and hidden automated devices. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-017: Manual mappings attach to Session slots, not their clips. Instant mappings follow device focus and may be locked to a device; some surfaces lack lock support. | Live 12 local manual, §33 pp.836–838; exact manual build unknown | ADR-0181: target identity, selection-follow and pin state must remain distinct; test clip replacement and deleted pinned devices. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-018: Absolute-controller takeover offers None, Pick-Up and Value Scaling; the Mapping Browser stores ranges and supports inversion. Instant mappings are not listed there. | Live 12 local manual, §33.1.3–33.2 pp.839–840; exact manual build unknown | ADR-0188: map hardware range, target domain and ownership separately; test bank switch, external automation and two competing surfaces. | Supported hypothesis — documented; installed behavior unmeasured |

### Browser, media ownership and tuning

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A3-019: Browser history includes searches and navigation. Enabled filters are recalled per Library label; filtered results can be saved as a custom label. | Live 12 local manual, §4.2–4.4 pp.67–72; exact manual build unknown | Dream §2/N15: persist query/view state separately from content membership; boolean tag-combination semantics still need measurement. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-020: Factory tags cannot be removed, but additional tags can be assigned. The manual describes periodic analysis tagging user samples up to 60 seconds. | Live 12 local manual, §4.4.2 p.72; exact manual build unknown | ADI app library: preserve provenance of factory, user and inferred tags. Analysis limits are reference behavior, not ADI requirements. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-021: Imported MIDI is incorporated into the Set and loses the original-file reference, unlike sample media. Insertion follows Arrangement marker or selected Session slot. | Live 12 local manual, §5.2 p.128; exact manual build unknown | Import: distinguish embedded event data from external media references and place the whole operation in one reversible changeset. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-022: Collect Files on Export controls dependent-file copying for dragged clips/tracks/presets; Always, Ask and Never are distinct from the musical export dialog. | Live 12 local manual, §5.7.1 pp.147–148; exact manual build unknown | ADR-0127: distinguish library export, project collect, Save As and rendered audio; preserve references on cancel and path changes. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-023: Unused-file analysis is project-local: a file can be considered unused here even when another project uses it. | Live 12 local manual, §5.8 p.151; exact manual build unknown | History/media retention: current clips alone are insufficient for ADI cleanup; snapshots, branches and other project references need a declared reachability policy. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-024: SCL/ASCL tuning is loaded into and saved with the Live Set; piano-roll pitches then represent that tuning. It is not documented here as independent clip tuning. | Live 12 local manual, §15 pp.346–350; exact manual build unknown | Dream §5 versus ADR-0178: ADI per-key-change/degree tuning is an addition. Correct the reference attribution without changing ADI scope. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-025: Changing tuning normally preserves note positions, potentially changing sounding pitch. Retune Set instead tries to preserve pitches and can modify MIDI clips. MPE targets need the documented bend-range setup. | Live 12 local manual, §15 pp.348–350; exact manual build unknown | Test note degree versus absolute pitch, per-note expression, retuning undo and unsupported instruments. Do not promise universal tuning output. | Supported hypothesis — documented; installed behavior unmeasured |

### Rack graph, export and stored structure

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A3-026: Chain select zones filter notes by default, not all MIDI controllers. A separate setting includes controllers. Fade zones attenuate MIDI velocity or audio output depending on Rack type. | Live 12 local manual, §24.5.4 p.479; exact manual build unknown | Dream §9: separate event gating from audio gain. Test note-off/sustain delivery and tails while crossing zones. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-027: Leaving a chain zone without a fade can preserve a sounding tail; leaving through a fade attenuates the chain output to zero. | Live 12 local manual, §24.5.4 p.479; exact manual build unknown | Hosting: inactivity, inaudibility and safe suspension are different; test long tails and re-entry with delay compensation. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-028: A mapped parameter hands control to its Macro; multiple targets can change the Macro display to a generic range. Mapping ranges can invert. | Live 12 local manual, §24.7.1 p.485; exact manual build unknown | ADR-0114: Live min/max mapping is a baseline, not proof of ADI multi-source breakpoint semantics; define composition and units explicitly. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-029: Randomization and variation recall have separate per-Macro exclusions. Variations save Macro states, can be recalled or overwritten, and are not whole-device snapshots. | Live 12 local manual, §24.7.2–24.7.3 pp.486–488; exact manual build unknown | Snapshot/undo: record the resulting values and exclusion policy; redo must not rerandomize. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-030: Drum-chain extraction from the Session mixer transfers its notes and devices; extraction from the chain list transfers devices only. Rack mixer chains have no clip slots. | Live 12 local manual, §24.8 pp.488–490; exact manual build unknown | Model a compound extraction transaction, including note/expression ownership, routing and selection; do not infer identical drag semantics across views. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-031: External hardware in a render path forces real-time rendering. Live traces the path; unaffected individual tracks can still render offline. Cancel deletes the partial render. | Live 12 local manual, §5.1.3.5 pp.126–127; exact manual build unknown | Export jobs need graph-derived execution mode and explicit partial-file policy; test external sends, long tails and dropout restart. | Supported hypothesis — documented; installed behavior unmeasured |
| A3-032: Static observation: the shipped Intro demo stores routing Target separately from display strings, AutomationTarget IDs, ClipSlot HasStop, and independent FollowAction timing/chance fields. | E25: evidence/wide/live-host-structure.json; Intro demo SHA-256 17329b3109a77b31c9c25cd9a59d224a9e73c5d83d2af2d7c95f7b634d57e4ff | Independent persistence tests should preserve stable targets, slot-stop state and action state separately from labels. Artifact fields are not a complete schema or runtime proof. | Confirmed by static analysis — artifact/API documentation only |

## Base-reference audit and targeted expansion — 2026-11-04

Research scope now explicitly requires complete Live host coverage as the ADI base, Cubase export capabilities, and current Bitwig UI behavior. These entries add documented contracts; installed behavior remains unmeasured. See E30–E32 in the private evidence index.

### Export, consolidate, bounce and project history are separate contracts

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A4-001: Export uses post-fader Main, individual or selected-track signals. Individual files include returns/instrument tracks and share length; current Session/Arrangement playback ownership affects rendered content. | Live 12 local manual, §5.1.3.1 pp.122–123; exact manual revision unconfirmed | ADR-0071: snapshot actual playback ownership and range. Test Session overrides while Arrangement is visible and silent/empty selected tracks. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-002: Include Return and Main Effects renders each selected track with its applicable returns and Main effects. Render as Loop runs an unwritten pass before writing the next pass. | Live 12 local manual, §5.1.3.2 p.124; exact manual revision unconfirmed | Distinguish isolated stems, shared-effect context and loop warmup. Test nonlinearity, long tails and random/stateful processors; do not promise exact stem recombination. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-003: Export at or above project rate runs at the chosen rate; lower-rate export first renders at project rate then downsamples. Normalize, mono and analysis-file creation are independent options. | Live 12 local manual, pp.124–125; exact manual revision unconfirmed | Compare execution rate with output rate; test rate-dependent plug-ins, tail length and metadata. SRC quality is still unmeasured. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-004: PCM export offers WAV/AIFF/FLAC; PCM and fixed 320 kbps CBR MP3 can be produced together. Export is disabled when neither encoding is selected. Dither choices are separate from encoding. | Live 12 local manual, §5.1.3.3 p.125; exact manual revision unconfirmed | Model one render with multiple deliveries and their metadata/encoding constraints. Test cancellation, name conflicts and no double dithering. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-005: Consolidate creates per-track audio before track effects/mixer, including clip processing and envelopes. Files go to Samples/Processed/Consolidate or temporary storage before the Set is saved. | Live 12 local manual, §6.13 p.177; exact manual revision unconfirmed | Keep source derivation distinct from post-device bounce. Test source/clip gain, warp, loop seams and relocation on first save. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-006: Individual Bounce Track in Place replaces the source; Bounce to New Track retains and mutes selected source material. Individual bounce is post-device/pre-mixer and transfers mixer settings. | Live 12 local manual, §20.1 pp.418–420; exact manual revision unconfirmed | Specify retained source state and transaction boundaries. Test mixer automation, partial selection, muted clips and undo without double-applying gain. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-007: In-place bounce affects clips in both Session and Arrangement; new-track bounce processes only selected material in the initiating view, leaving the counterpart new track empty. | Live 12 local manual, p.420; exact manual revision unconfirmed | A track is shared across views; content operations still have view scope. Test unselected scenes and playback ownership after undo/reopen. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-008: Group bounce is post-mixer and excludes Main processing; children routed outside the group are excluded, while return inclusion depends on the path reaching Main. | Live 12 local manual, §20.2 pp.420–422; exact manual revision unconfirmed | Render graph selection must follow actual routing. Test external group routes and shared returns rather than summing visible children. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-009: Paste Bounced Audio renders copied source at paste time, so intervening source edits affect it. Deleting the source disables the command; pasting into an empty MIDI track converts it to audio. | Live 12 local manual, §20.3 pp.422–423; exact manual revision unconfirmed | Clipboard references are not immutable audio snapshots. Test source deletion/undo, device edits and type conversion while preserving stable identity. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-010: Undo History supports jumping through actions from this open session; it is discarded on closing the Set. Creating/opening is the first non-undoable entry. | Live 12 local manual, §5.4.2 pp.130–131; exact manual revision unconfirmed | ADR-0110: separate reference session history from ADI persistent history. Actual gesture grouping and branch truncation still require measurement. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-011: Merging a Set reconstructs tracks, clips in both views, devices and automation but excludes return tracks. Session clip selections can also be exported as a new Set through the browser. | Live 12 local manual, §5.4.3–4 pp.131–134; exact manual revision unconfirmed | Transfer must state dependencies and remapping. Test send targets, missing media and return-dependent sound rather than assuming complete graph portability. | Supported hypothesis — documented; installed behavior unmeasured |

### Selection, linked editing and recording state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A4-012: Time-edit commands insert/delete time across all Arrangement tracks and affect time-signature markers; ordinary edit commands act on the selection. | Live 12 local manual, §6.11 p.176; exact manual revision unconfirmed | Specify affected-object sets for hidden/linked/locked tracks, tempo, locators and automation separately; manual wording does not settle every combination. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-013: A track belongs to one linked-edit set. Shared edits include clip operations, arm state and take-lane operations even where lanes are hidden; simultaneous fades must start at the same time. | Live 12 local manual, §6.14 pp.178–180; exact manual revision unconfirmed | Distinguish linked-edit membership from grouping/routing. Test mismatched lane counts, phase coherence, partial selection and one-step undo. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-014: Arrangement mixer and track controls share values but have separate visibility controls; some features appear only in the mixer. | Live 12 local manual, §6.15 pp.180–181; exact manual revision unconfirmed | Keep L09 shortcut-target and L10 independent-visibility measurements open. Shared data is not evidence that both panels share visibility settings. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-015: The 0 key deactivates selected material or a focused track header. Temporary snap inversion applies both when the grid is on and when it is off. | Live 12 local manual, §6.9–10 pp.175–176; exact manual revision unconfirmed | Key routing depends on focus and selection type. Test text editing, mixed selections and fixed/adaptive/triplet grids. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-016: Arrangement Record behavior depends on Start Playback with Record; Shift temporarily reverses it. Punch boundaries use the Arrangement loop endpoints. | Live 12 local manual, §19.3.1 p.407; exact manual revision unconfirmed | Separate armed, pending-record, transport and punch state; test negative/preroll time and session launch starting recording. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-017: Session Record can create clips in the selected scene across armed tracks, then switch them into loop playback. The mappable New action stops armed-track clips and finds/creates a recording scene. | Live 12 local manual, §19.3.2 pp.408–409; exact manual revision unconfirmed | Test scene selection, existing clips, quantization and multi-track atomicity; distinguish stop recording from stop playback. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-018: Step recording uses held notes and grid-based cursor advancement with arm and note preview enabled; continued holds extend notes and backward movement can remove entered notes. | Live 12 local manual, §19.3.4 pp.409–410; exact manual revision unconfirmed | Test chord entry, repeated pitches, expression and cursor/undo behavior separately from realtime capture. | Supported hypothesis — documented; installed behavior unmeasured |

### Clip envelopes and keyboard accessibility

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A4-019: Session Clip View offers automation and modulation; Arrangement Clip View offers modulation while absolute automation lives on track lanes. | Live 12 local manual, §26.1 pp.504–505; §26.3 pp.508–509; exact manual revision unconfirmed | Keep absolute, relative and manually overridden values distinct. Test clip transfer between views and target-device movement. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-020: Clip pitch modulation adds to transpose and clips to its range; gain and send modulation instead scale their base values downward. Pan modulation shrinks toward either hard-pan extreme. | Live 12 local manual, §26.2–3 pp.507,510–512; exact manual revision unconfirmed | Parameter-specific composition rules are required. Measure value-domain mapping and smoothing rather than using one generic modulation formula. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-021: An unlinked envelope has independent start/loop settings; a looping sample can use a one-shot envelope. Linked envelopes stretch with warp-marker changes. | Live 12 local manual, §26.5 pp.513–516; exact manual revision unconfirmed | Store separate time domains and link state. Test relaunch, offset, groove, tempo, split/duplicate and link/unlink round trips. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-022: Replacing a sample through Clip View preserves clip settings and envelopes; clip modulation itself leaves source audio unchanged. | Live 12 local manual, §26.2.1 p.505; §26.2.5 p.508; exact manual revision unconfirmed | Source replacement must invalidate derived analysis while preserving intended controls. Test different source lengths and missing dependencies. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-023: Tab can switch views or navigate controls depending on Use Tab Key to Move Focus. Tab navigation within Settings works regardless; wrap navigation and arrow-key clip movement are separate settings. | Live 12 local manual, §40.1 pp.973–974; exact manual revision unconfirmed | Accessibility is part of baseline parity. Test focus restoration, shortcut collisions, disabled controls and screen-reader names without treating manual defaults as measured defaults. | Supported hypothesis — documented; installed behavior unmeasured |
| A4-024: The guide identifies screen-reader gaps including browser tags/filters and MPE editing. Info View help can be announced; Control Bar navigation remembers its last focused control. | Live 12 local manual, §40 pp.973–976; exact manual revision unconfirmed | ADI can improve these gaps. Require actual accessibility-tree/help-text evidence on the installed build, alongside visual parity. | Supported hypothesis — documented; installed behavior unmeasured |

### Complete-base coverage rule

Live is the default behavioral reference for every applicable ADI host feature, not just warp/MIDI. Review every menu, command, context menu, shortcut/modifier, setting, default, help/status string, focus/selection rule, gesture, undo boundary and persistence/failure path. Platform/edition/build differences must be explicit. Current ADI decisions can intentionally diverge, with a documented reason.

The table below inventories every host/integration entry in the supplied manual's contents, including controller workflows. It is an omission guard, not evidence that each section has been read or tested. Body-only subheadings, release changes, undocumented behavior and installed controls need additional rows. Device DSP chapters 28–30/32 and credits 42 are excluded; host devices/racks/Max integration remain included. Push hardware-specific tests are conditional on available hardware, never substituted for software behavior.

Each topic needs the same completion dimensions: source/build; objects and ownership; units/defaults; trigger and state transition; applicable platforms; gesture/undo/redo; save/reopen/transfer; cancel/failure; audio timing/quality where relevant; ADI adoption or deliberate difference. **No topic below is closed.** Existing A-W/A-M/A-T/A-X/A-S/A-H/A3/A4 entries provide selected findings, not whole-topic completion.

The manual does not settle all ten original Measure on Live questions, the 70 displayed palette RGB values, every Settings default, all Info/Status wording or every gesture's undo granularity. Those remain explicit measurement work. Public/manual defaults are not user-profile defaults; a default Set can be customized. Newer installed behavior must be reconciled with this unversioned manual and version-specific release notes.

| Base-reference obligation beyond the contents | Evidence required | What ADI does with it | Status |
|---|---|---|---|
| Every installed menu/control and conditional entry, including disabled/hidden states | Build-bound UI inventory; edition, platform, language and focus recorded | Window parity and exact command availability | Unknown / not tested |
| Settings, default Set, themes, palette and help/status corpus | Clean-profile versus user-profile comparison; exact text/RGB with source and operator | Preserve ADI decisions while identifying deliberate differences | Unknown / not tested |
| Every gesture's journal boundary and state transitions | Action log plus before/after/one-undo/redo/reopen state | ADR-0110; cover mouse, keyboard, controller and async operations | Unknown / not tested |
| Recovery, missing media/devices, failures and cancellation | Disposable fixtures; restored dependencies and safe fault cases | No data loss or partially committed operations | Unknown / not tested |
| Playback/export equivalence, precision, scheduling and CPU | Controlled raw audio and same-machine timing; exact binary hashes | No numeric or performance claim from labels/static clues | Unknown / not tested |
| Release/build and platform deltas, Push 3 and Live Object Model integration | Version-specific primary docs and installed capability evidence | Keep base contract separate from optional hardware/service integration | Unknown / not tested |

### Ableton topic-by-topic base inventory

610 host/integration contents entries retained from 871 total entries. Counts include parent sections and examples; they are not counts of independent features. Source headings/pages were mechanically paired and checked for uniqueness. Raw manual remains in the ignored reference folder.

#### Chapter 1 — Welcome to Live

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-1: Welcome to Live | Live 12 manual §1, p.22 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-1.1: The Ableton Team Says: Thank You | Live 12 manual §1.1, p.22 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 2 — First Steps

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-2: First Steps | Live 12 manual §2, p.23 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.1: Installation and Authorization | Live 12 manual §2.1, p.23 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.2: Learning About Live | Live 12 manual §2.2, p.23 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.2.1: Learn View | Live 12 manual §2.2.1, p.23 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-060, A6R-061. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.2.2: Info View | Live 12 manual §2.2.2, p.28 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.2.3: Other Learning Resources | Live 12 manual §2.2.3, p.29 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3: Live’s Settings | Live 12 manual §2.3, p.30 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.1: Display & Input | Live 12 manual §2.3.1, p.30 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-062. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.2: Theme & Colors | Live 12 manual §2.3.2, p.30 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-062, A6T-001, A6T-002, A6T-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.3: Audio | Live 12 manual §2.3.3, p.31 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-062. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.4: Link | Live 12 manual §2.3.4, p.31 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-013. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.5: Tempo & MIDI | Live 12 manual §2.3.5, p.31 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-013. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.6: File & Folder | Live 12 manual §2.3.6, p.32 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-063. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.7: Library | Live 12 manual §2.3.7, p.32 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-063. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.8: Plug-Ins | Live 12 manual §2.3.8, p.32 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-063. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.9: Record, Warp & Launch | Live 12 manual §2.3.9, p.32 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-063. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-2.3.10: Licenses & Updates | Live 12 manual §2.3.10, p.32 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-063. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 3 — Live Concepts

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-3: Live Concepts | Live 12 manual §3, p.33 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.1: The Control Bar | Live 12 manual §3.1, p.33 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.2: The Status Bar | Live 12 manual §3.2, p.34 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.3: The Browser | Live 12 manual §3.3, p.35 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.4: Sound Similarity | Live 12 manual §3.4, p.36 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.5: Live Sets | Live 12 manual §3.5, p.38 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.6: Arrangement and Session | Live 12 manual §3.6, p.38 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.7: Tracks | Live 12 manual §3.7, p.40 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.8: Audio and MIDI | Live 12 manual §3.8, p.43 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.9: Audio Clips and Samples | Live 12 manual §3.9, p.44 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.10: MIDI Clips and MIDI Files | Live 12 manual §3.10, p.45 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.11: Devices | Live 12 manual §3.11, p.46 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.12: Clip and Device View | Live 12 manual §3.12, p.48 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.13: Scale Awareness | Live 12 manual §3.13, p.49 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.14: The Mixer | Live 12 manual §3.14, p.51 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.15: Presets and Racks | Live 12 manual §3.15, p.54 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.16: Routing | Live 12 manual §3.16, p.54 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.17: Recording New Clips | Live 12 manual §3.17, p.55 (contents p.2) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.18: Automation Envelopes | Live 12 manual §3.18, p.57 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.19: Clip Envelopes | Live 12 manual §3.19, p.57 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.20: MIDI and Key Remote | Live 12 manual §3.20, p.58 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-3.21: Saving and Exporting | Live 12 manual §3.21, p.58 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 4 — Working with the Browser

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-4: Working with the Browser | Live 12 manual §4, p.60 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-053, A6D-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.1: Content Pane | Live 12 manual §4.1, p.62 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.2: Search Bar | Live 12 manual §4.2, p.63 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.2.1: Saving Search Results as Custom Labels | Live 12 manual §4.2.1, p.66 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-054. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.3: Browser History | Live 12 manual §4.3, p.69 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.4: Filters and Tags | Live 12 manual §4.4, p.70 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.4.1: Filter Groups | Live 12 manual §4.4.1, p.71 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.4.2: Tags | Live 12 manual §4.4.2, p.72 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.4.3: Tag Editor | Live 12 manual §4.4.3, p.73 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.4.4: Quick Tags | Live 12 manual §4.4.4, p.77 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.5: Collections | Live 12 manual §4.5, p.79 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.6: Library | Live 12 manual §4.6, p.81 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-054. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7: Places | Live 12 manual §4.7, p.83 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-054. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7.1: Downloading and Installing Packs in the Browser | Live 12 manual §4.7.1, p.84 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-055. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7.2: Pack Info | Live 12 manual §4.7.2, p.88 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-055. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7.3: Splice | Live 12 manual §4.7.3, p.89 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-058. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7.4: Using Ableton Cloud | Live 12 manual §4.7.4, p.103 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-056. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7.5: Transferring Files from Push 3 in Standalone Mode | Live 12 manual §4.7.5, p.105 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-057. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7.6: User Library | Live 12 manual §4.7.6, p.108 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-062. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7.7: Current Project | Live 12 manual §4.7.7, p.111 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-067. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.7.8: User Folders | Live 12 manual §4.7.8, p.113 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.8: Navigating in the Browser | Live 12 manual §4.8, p.115 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.9: Previewing Files | Live 12 manual §4.9, p.115 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-4.10: Adding Content from the Browser to a Live Set | Live 12 manual §4.10, p.118 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 5 — Managing Files and Sets

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-5: Managing Files and Sets | Live 12 manual §5, p.120 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.1: Sample Files | Live 12 manual §5.1, p.120 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-064. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.1.1: The Decoding Cache | Live 12 manual §5.1.1, p.120 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.1.2: Analysis Files (.asd) | Live 12 manual §5.1.2, p.121 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-033, A6H-063, A6H-064. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.1.3: Exporting Audio and Video | Live 12 manual §5.1.3, p.122 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6D-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.2: MIDI Files | Live 12 manual §5.2, p.128 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6Z-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.2.1: Exporting MIDI Files | Live 12 manual §5.2.1, p.128 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.3: Live Clips | Live 12 manual §5.3, p.129 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-065. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.4: Live Sets | Live 12 manual §5.4, p.130 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6Z-001, A6Z-010. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.4.1: Creating, Opening and Saving Sets | Live 12 manual §5.4.1, p.130 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.4.2: Accessing a Set’s Undo History | Live 12 manual §5.4.2, p.130 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6P-006. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.4.3: Merging Sets | Live 12 manual §5.4.3, p.131 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-067. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.4.4: Exporting Session Clips as New Sets | Live 12 manual §5.4.4, p.134 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-071. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.4.5: Template Sets | Live 12 manual §5.4.5, p.134 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-066, A6S-001, A6S-002, A6S-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.4.6: Viewing and Changing a Live Set’s File References | Live 12 manual §5.4.6, p.135 (contents p.3) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.5: Live Projects | Live 12 manual §5.5, p.137 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.5.1: Projects and Live Sets | Live 12 manual §5.5.1, p.137 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-068, A6H-069. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.5.2: Projects and Presets | Live 12 manual §5.5.2, p.141 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-036. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.5.3: Managing Files in a Project | Live 12 manual §5.5.3, p.142 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.6: Locating Missing Files | Live 12 manual §5.6, p.142 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.6.1: Manual Repair | Live 12 manual §5.6.1, p.143 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.6.2: Automatic Repair | Live 12 manual §5.6.2, p.143 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.7: Collecting External Files | Live 12 manual §5.7, p.145 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-070, A6Z-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.7.1: Collect Files on Export | Live 12 manual §5.7.1, p.147 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-071. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.7.2: Aggregated Locating and Collecting | Live 12 manual §5.7.2, p.148 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-070. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.8: Finding Unused Files | Live 12 manual §5.8, p.150 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6Z-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.9: Packing Projects into Packs | Live 12 manual §5.9, p.154 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.10: File Management FAQs | Live 12 manual §5.10, p.158 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.10.1: How Do I Create a Project? | Live 12 manual §5.10.1, p.158 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-068. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.10.2: How Can I Save Presets Into My Current Project? | Live 12 manual §5.10.2, p.158 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.10.3: Can I Work On Multiple Versions of a Set? | Live 12 manual §5.10.3, p.158 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-068. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.10.4: Where Should I Save My Live Sets? | Live 12 manual §5.10.4, p.158 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-068. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-5.10.5: Can I Use My Own Folder Structure Within a Project Folder? | Live 12 manual §5.10.5, p.158 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-068. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 6 — Arrangement View

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-6: Arrangement View | Live 12 manual §6, p.160 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-006, A6D-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.1: Layout | Live 12 manual §6.1, p.160 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.2: Navigation and Zooming | Live 12 manual §6.2, p.163 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-085. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.3: Transport and Playback | Live 12 manual §6.3, p.163 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.4: Launching the Arrangement with Locators | Live 12 manual §6.4, p.165 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.5: Time Signature Changes | Live 12 manual §6.5, p.167 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.6: The Arrangement Loop | Live 12 manual §6.6, p.169 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.7: Moving and Resizing Clips | Live 12 manual §6.7, p.170 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-076, A6C-077. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.8: Audio Clip Fades and Crossfades | Live 12 manual §6.8, p.172 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-018, A6C-078, A6C-079, A6C-080, A6C-081. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.9: Selecting Clips and Time | Live 12 manual §6.9, p.174 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-016, A6C-082. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.10: Using the Editing Grid | Live 12 manual §6.10, p.175 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-077. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.11: Using the …Time Commands | Live 12 manual §6.11, p.176 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.12: Splitting Clips | Live 12 manual §6.12, p.176 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-083. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.13: Consolidating Clips | Live 12 manual §6.13, p.177 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.14: Linked-Track Editing | Live 12 manual §6.14, p.178 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.14.1: Linking and Unlinking Tracks | Live 12 manual §6.14.1, p.178 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-084. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.14.2: Editing Linked Tracks | Live 12 manual §6.14.2, p.179 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-6.15: The Mixer in Arrangement View | Live 12 manual §6.15, p.180 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 7 — Session View

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-7: Session View | Live 12 manual §7, p.182 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-006, A6S-007, A6P-004. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.1: Session View Clips | Live 12 manual §7.1, p.183 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.2: Tracks and Scenes | Live 12 manual §7.2, p.184 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.2.1: Editing Scene Tempo and Time Signature Values | Live 12 manual §7.2.1, p.186 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.2.2: Scene View | Live 12 manual §7.2.2, p.187 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.3: The Track Status Fields | Live 12 manual §7.3, p.189 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.4: Setting Up the Session View Grid | Live 12 manual §7.4, p.190 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.4.1: Select on Launch | Live 12 manual §7.4.1, p.191 (contents p.4) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.4.2: Removing Clip Stop Buttons | Live 12 manual §7.4.2, p.191 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.4.3: Editing Scenes | Live 12 manual §7.4.3, p.191 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-7.5: Recording Sessions into the Arrangement | Live 12 manual §7.5, p.192 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 8 — Clip View

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-8: Clip View | Live 12 manual §8, p.195 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-001, A6C-002, A6P-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.1: Clip View Layout | Live 12 manual §8.1, p.196 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-001. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.1.1: Clip Title Bar | Live 12 manual §8.1.1, p.197 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.1.2: Clip Panels | Live 12 manual §8.1.2, p.199 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-002. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.1.3: Editor View Modes | Live 12 manual §8.1.3, p.201 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.2: Main Clip Properties Panel | Live 12 manual §8.2, p.202 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.2.1: Clip and Loop Region Settings | Live 12 manual §8.2.1, p.203 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-006, A6C-007, A6C-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.2.2: Clip Time Signature | Live 12 manual §8.2.2, p.204 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.2.3: Clip Groove | Live 12 manual §8.2.3, p.205 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-010, A6C-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.2.4: Clip Scale | Live 12 manual §8.2.4, p.206 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.3: Extended Clip Properties | Live 12 manual §8.3, p.207 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.3.1: Follow Action and Launch Controls | Live 12 manual §8.3.1, p.208 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.3.2: MIDI Clip Bank and Program Change Controls | Live 12 manual §8.3.2, p.208 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.4: Audio Utilities Panel | Live 12 manual §8.4, p.209 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.4.1: Warp Controls | Live 12 manual §8.4.1, p.210 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.4.2: Reversing Samples | Live 12 manual §8.4.2, p.211 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-013, A6C-014, A6C-015, A6C-016. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.4.3: Destructive Sample Editing | Live 12 manual §8.4.3, p.213 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-017. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.4.4: Clip Start and End Fades | Live 12 manual §8.4.4, p.213 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-018. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.4.5: Clip RAM Mode | Live 12 manual §8.4.5, p.214 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-019. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.4.6: High Quality Interpolation | Live 12 manual §8.4.6, p.215 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-020. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.4.7: Clip Gain and Pitch | Live 12 manual §8.4.7, p.216 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-021. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.5: Pitch and Time Utilities Panel | Live 12 manual §8.5, p.217 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.5.1: Pitch Tools | Live 12 manual §8.5.1, p.218 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.5.2: Time Tools | Live 12 manual §8.5.2, p.219 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-022. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.6: Transform and Generate Panels | Live 12 manual §8.6, p.220 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-038. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.7: Zooming and Scrolling in the Clip View’s Editor | Live 12 manual §8.7, p.221 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-085. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.8: Playing and Scrubbing Clips | Live 12 manual §8.8, p.222 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-023. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.9: Looping Clips | Live 12 manual §8.9, p.223 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-008, A6C-024. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.10: Clip View Sample Details | Live 12 manual §8.10, p.225 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-022. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.11: Cropping Clips | Live 12 manual §8.11, p.226 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-033, A6C-025. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.12: Replacing and Editing the Sample | Live 12 manual §8.12, p.226 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-026, A6C-027. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.13: Editing Clip Properties for Multiple Clips | Live 12 manual §8.13, p.227 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-021, A6C-022. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-8.14: Clip Defaults and Update Rate | Live 12 manual §8.14, p.227 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-028. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 9 — Audio Clips, Tempo, and Warping

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-9: Audio Clips, Tempo, and Warping | Live 12 manual §9, p.229 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6D-002, A6P-007, A6P-008, A6P-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.1: Tempo | Live 12 manual §9.1, p.229 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.1.1: Setting the Tempo | Live 12 manual §9.1.1, p.229 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-029. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.1.2: Tapping the Tempo | Live 12 manual §9.1.2, p.230 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-029, A6C-030. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.1.3: Nudging the Tempo | Live 12 manual §9.1.3, p.231 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-029. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.1.4: Clip Tempo Followers and Leaders | Live 12 manual §9.1.4, p.231 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-031. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.2: Warping | Live 12 manual §9.2, p.233 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.2.1: Warping Options in Settings | Live 12 manual §9.2.1, p.234 (contents p.5) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-032. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.2.2: Importing Samples | Live 12 manual §9.2.2, p.235 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-033. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.2.3: Warp Markers | Live 12 manual §9.2.3, p.235 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-034, A6Z-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.2.4: Warping Short Samples | Live 12 manual §9.2.4, p.237 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.2.5: Auto-Warping Long Samples | Live 12 manual §9.2.5, p.239 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.2.6: Manipulating Grooves | Live 12 manual §9.2.6, p.243 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.2.7: Quantizing Audio | Live 12 manual §9.2.7, p.243 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-038. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.3: Warp Modes | Live 12 manual §9.3, p.244 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.3.1: Beats Mode | Live 12 manual §9.3.1, p.244 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-039, A6C-071. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.3.2: Tones Mode | Live 12 manual §9.3.2, p.245 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-040. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.3.3: Texture Mode | Live 12 manual §9.3.3, p.245 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-040. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.3.4: Re-Pitch Mode | Live 12 manual §9.3.4, p.246 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-9.3.5: Complex and Complex Pro Mode | Live 12 manual §9.3.5, p.246 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-040. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 10 — Editing MIDI

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-10: Editing MIDI | Live 12 manual §10, p.247 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6D-006, A6P-010, A6P-011, A6Z-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.1: The MIDI Note Editor Layout | Live 12 manual §10.1, p.247 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-001. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.2: Zooming and Navigating in the MIDI Note Editor | Live 12 manual §10.2, p.249 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-002. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.2.1: Grid Snapping | Live 12 manual §10.2.1, p.251 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.2.2: Playback Options | Live 12 manual §10.2.2, p.251 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-004. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.3: Creating a MIDI Clip | Live 12 manual §10.3, p.252 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.4: Adding MIDI Notes | Live 12 manual §10.4, p.252 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-006. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.4.1: Draw Mode | Live 12 manual §10.4.1, p.252 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-006. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.4.2: Previewing Notes | Live 12 manual §10.4.2, p.253 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5: Editing MIDI Notes | Live 12 manual §10.5, p.254 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.1: Non-Destructive Editing | Live 12 manual §10.5.1, p.254 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.2: Selecting Notes and Timespan | Live 12 manual §10.5.2, p.254 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-009, A6M-010, A6G-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.3: Find and Select Notes | Live 12 manual §10.5.3, p.255 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-011, A6M-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.4: Moving Notes | Live 12 manual §10.5.4, p.257 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-003, A6M-013. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.5: Changing Note Length | Live 12 manual §10.5.5, p.257 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-003, A6M-014. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.6: MIDI Note Stretch | Live 12 manual §10.5.6, p.259 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-015. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.7: Deactivating Notes | Live 12 manual §10.5.7, p.259 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-016. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.8: Note Operations | Live 12 manual §10.5.8, p.260 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.9: Pitch and Time Utilities | Live 12 manual §10.5.9, p.263 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6X-018. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.10: MIDI Tools | Live 12 manual §10.5.10, p.271 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.11: Quantizing Notes | Live 12 manual §10.5.11, p.271 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-024. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.12: Editing Velocities | Live 12 manual §10.5.12, p.272 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-025, A6M-026, A6Z-002, A6X-018. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.5.13: Editing Probabilities | Live 12 manual §10.5.13, p.277 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-029, A6Z-002. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.6: Folding and Scales | Live 12 manual §10.6, p.279 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-031, A6M-032. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.7: Editing MIDI Clips | Live 12 manual §10.7, p.282 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.7.1: Cropping MIDI Clips | Live 12 manual §10.7.1, p.282 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-033. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.7.2: The …Time Commands in the MIDI Note Editor | Live 12 manual §10.7.2, p.283 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-034. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.7.3: Looping | Live 12 manual §10.7.3, p.283 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-034. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.8: Multi-Clip Editing | Live 12 manual §10.8, p.284 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-035. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.8.1: Focus Mode | Live 12 manual §10.8.1, p.285 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-035, A6M-036. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.8.2: Multi-Clip Editing in the Session View | Live 12 manual §10.8.2, p.287 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-037. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-10.8.3: Multi-Clip Editing in the Arrangement View | Live 12 manual §10.8.3, p.287 (contents p.6) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-037, A6M-038. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 11 — MIDI Tools

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-11: MIDI Tools | Live 12 manual §11, p.288 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-039, A6S-009, A6D-004, A6D-006. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.1: Using MIDI Tools | Live 12 manual §11.1, p.288 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-039, A6X-011, A6X-013, A6X-014, A6X-015, A6X-016, A6X-017, A6X-018. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.1.1: Using Max for Live MIDI Tools | Live 12 manual §11.1.1, p.291 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-039, A6X-006, A6X-007, A6X-008, A6X-009, A6X-010, A6X-011, A6X-012, A6X-013, A6X-015, A6X-019. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2: Transformation Tools | Live 12 manual §11.2, p.292 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.1: Arpeggiate | Live 12 manual §11.2.1, p.292 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-040. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.2: Chop | Live 12 manual §11.2.2, p.293 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-041, A6M-042. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.3: Connect | Live 12 manual §11.2.3, p.295 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-043. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.4: Glissando | Live 12 manual §11.2.4, p.297 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-044. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.5: LFO | Live 12 manual §11.2.5, p.299 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-045, A6M-046. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.6: Ornament | Live 12 manual §11.2.6, p.301 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-047, A6M-048. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.7: Quantize | Live 12 manual §11.2.7, p.304 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-024, A6M-049. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.8: Recombine | Live 12 manual §11.2.8, p.305 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-050. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.9: Span | Live 12 manual §11.2.9, p.308 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-051, A6M-052. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.10: Strum | Live 12 manual §11.2.10, p.310 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-053. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.11: Time Warp | Live 12 manual §11.2.11, p.312 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-054. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.2.12: Velocity Shaper | Live 12 manual §11.2.12, p.314 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-055. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.3: Generative Tools | Live 12 manual §11.3, p.316 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.3.1: Rhythm | Live 12 manual §11.3.1, p.316 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-056, A6M-057. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.3.2: Seed | Live 12 manual §11.3.2, p.318 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-058. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.3.3: Shape | Live 12 manual §11.3.3, p.319 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-059. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.3.4: Stacks | Live 12 manual §11.3.4, p.321 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-060, A6M-061, A6X-001, A6X-002, A6X-003, A6X-004, A6X-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-11.3.5: Euclidean | Live 12 manual §11.3.5, p.322 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-062, A6M-063. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 12 — Editing MPE

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-12: Editing MPE | Live 12 manual §12, p.325 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6Z-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-12.1: Viewing MPE Data | Live 12 manual §12.1, p.326 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-064, A6G-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-12.2: Editing MPE Data | Live 12 manual §12.2, p.327 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-065, A6M-066, A6M-067, A6M-068. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-12.3: Drawing Envelopes | Live 12 manual §12.3, p.330 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-066, A6M-069. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-12.4: MPE in Live’s Devices and on Push 2 | Live 12 manual §12.4, p.331 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-12.5: MPE in External Plug-ins | Live 12 manual §12.5, p.331 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-070. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-12.6: MPE/Multi-channel Settings | Live 12 manual §12.6, p.331 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-070. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-12.6.1: Accessing the MPE/Multi-channel Settings Dialog | Live 12 manual §12.6.1, p.332 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-070. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-12.6.2: The MPE/Multi-Channel Settings Dialog | Live 12 manual §12.6.2, p.334 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-071. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 13 — Converting Audio to MIDI

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-13: Converting Audio to MIDI | Live 12 manual §13, p.335 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-072. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-13.1: Slice to New MIDI Track | Live 12 manual §13.1, p.335 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-072, A6M-073. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-13.1.1: Resequencing Slices | Live 12 manual §13.1.1, p.337 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-074. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-13.1.2: Using Effects on Slices | Live 12 manual §13.1.2, p.337 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-074. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-13.2: Convert Harmony to New MIDI Track | Live 12 manual §13.2, p.338 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-075. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-13.3: Convert Melody to New MIDI Track | Live 12 manual §13.3, p.338 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-075. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-13.4: Convert Drums to New MIDI Track | Live 12 manual §13.4, p.339 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-076. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-13.5: Optimizing for Better Conversion Quality | Live 12 manual §13.5, p.339 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-076, A6M-077. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 14 — Using Grooves

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-14: Using Grooves | Live 12 manual §14, p.340 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-078, A6D-002. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.1: Groove Pool | Live 12 manual §14.1, p.341 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-078. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.1.1: Adjusting Groove Parameters | Live 12 manual §14.1.1, p.342 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-079, A6M-080, A6M-081. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.1.2: Committing Grooves | Live 12 manual §14.1.2, p.343 (contents p.7) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.2: Editing Grooves | Live 12 manual §14.2, p.344 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-082. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.2.1: Extracting Grooves | Live 12 manual §14.2.1, p.344 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-082. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.3: Groove Tips | Live 12 manual §14.3, p.345 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-083. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.3.1: Grooving a Single Voice | Live 12 manual §14.3.1, p.345 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-083. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.3.2: Non-Destructive Quantization | Live 12 manual §14.3.2, p.345 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-083. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-14.3.3: Creating Texture With Randomization | Live 12 manual §14.3.3, p.345 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-080. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 15 — Using Tuning Systems

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-15: Using Tuning Systems | Live 12 manual §15, p.346 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-084, A6Z-006, A6X-002, A6X-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-15.1: Loading a Tuning System | Live 12 manual §15.1, p.348 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-084, A6M-085, A6M-086, A6M-087. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-15.2: The Tuning Section | Live 12 manual §15.2, p.351 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-088, A6M-089, A6M-092. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-15.3: MIDI Track Options for Tuning Systems | Live 12 manual §15.3, p.352 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-091. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-15.3.1: Bypass Tuning | Live 12 manual §15.3.1, p.352 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-090. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-15.3.2: MIDI Controller Layouts | Live 12 manual §15.3.2, p.353 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-091. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-15.4: Learn More About Tuning Systems | Live 12 manual §15.4, p.354 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6M-092, A6F-001, A6F-002, A6F-003, A6F-004, A6F-005, A6F-006, A6F-007, A6F-008, A6F-009, A6F-010, A6F-011, A6F-012, A6F-013, A6F-014. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 16 — Launching Clips

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-16: Launching Clips | Live 12 manual §16, p.355 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-007, A6P-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.1: The Launch Controls | Live 12 manual §16.1, p.355 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.2: Launch Modes | Live 12 manual §16.2, p.356 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-048. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.3: Legato Mode | Live 12 manual §16.3, p.357 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-019. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.4: Clip Launch Quantization | Live 12 manual §16.4, p.358 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-048. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.5: Velocity | Live 12 manual §16.5, p.359 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-041. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.6: Clip Offset and Nudging | Live 12 manual §16.6, p.359 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-042. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.7: Follow Actions | Live 12 manual §16.7, p.361 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-043, A6C-044, A6C-045, A6C-046, A6C-048. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.7.1: Looping Parts of a Clip | Live 12 manual §16.7.1, p.364 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.7.2: Creating Cycles | Live 12 manual §16.7.2, p.365 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-047. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.7.3: Temporarily Looping Clips | Live 12 manual §16.7.3, p.365 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-044. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.7.4: Adding Variations in Sync | Live 12 manual §16.7.4, p.365 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.7.5: Mixing up Melodies and Beats | Live 12 manual §16.7.5, p.366 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-16.7.6: Creating Nonrepetitive Structures | Live 12 manual §16.7.6, p.366 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 17 — Routing and I/O

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-17: Routing and I/O | Live 12 manual §17, p.367 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-001, A6S-004, A6D-005, A6P-016. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.1: Monitoring | Live 12 manual §17.1, p.369 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.2: External Audio In/Out | Live 12 manual §17.2, p.370 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-002. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.2.1: Mono/Stereo Conversions | Live 12 manual §17.2.1, p.370 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.3: External MIDI In/Out | Live 12 manual §17.3, p.370 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.3.1: MIDI Port Inputs and Outputs | Live 12 manual §17.3.1, p.371 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.3.2: Playing MIDI With the Computer Keyboard | Live 12 manual §17.3.2, p.373 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.3.3: Connecting External Synthesizers | Live 12 manual §17.3.3, p.374 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.3.4: MIDI In/Out Indicators | Live 12 manual §17.3.4, p.374 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-004. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.4: Resampling | Live 12 manual §17.4, p.375 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-006. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.5: Internal Routings | Live 12 manual §17.5, p.375 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-001, A6H-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.5.1: Internal Routing Points | Live 12 manual §17.5.1, p.376 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-17.5.2: Making Use of Internal Routing | Live 12 manual §17.5.2, p.378 (contents p.8) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 18 — Mixing

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-18: Mixing | Live 12 manual §18, p.389 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-005, A6S-006, A6D-001, A6P-017, A6P-018. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.1: The Live Mixer | Live 12 manual §18.1, p.389 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.1.1: Additional Mixer Features | Live 12 manual §18.1.1, p.393 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.2: Audio and MIDI Tracks | Live 12 manual §18.2, p.394 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.3: Group Tracks | Live 12 manual §18.3, p.395 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.4: Return Tracks and the Main track | Live 12 manual §18.4, p.397 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.5: Using Live’s Crossfader | Live 12 manual §18.5, p.398 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.6: Soloing and Cueing | Live 12 manual §18.6, p.401 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.7: Track Delays | Live 12 manual §18.7, p.403 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.8: Keep Monitoring Latency in Recording Track Toggles | Live 12 manual §18.8, p.403 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-18.9: Performance Impact Track Indicators | Live 12 manual §18.9, p.404 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 19 — Recording New Clips

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-19: Recording New Clips | Live 12 manual §19, p.405 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-008, A6P-004. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.1: Choosing an Input | Live 12 manual §19.1, p.405 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.2: Arming (Record-Enabling) Tracks | Live 12 manual §19.2, p.406 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-015. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.3: Recording | Live 12 manual §19.3, p.406 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.3.1: Recording Into the Arrangement | Live 12 manual §19.3.1, p.407 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-016. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.3.2: Recording Into Session Slots | Live 12 manual §19.3.2, p.407 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.3.3: Overdub Recording MIDI Patterns | Live 12 manual §19.3.3, p.409 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-017. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.3.4: MIDI Step Recording | Live 12 manual §19.3.4, p.409 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.4: Recording in Sync | Live 12 manual §19.4, p.410 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.4.1: Metronome Settings | Live 12 manual §19.4.1, p.412 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-018, A6H-019. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.5: Recording Quantized MIDI Notes | Live 12 manual §19.5, p.412 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.6: Recording with Count-in | Live 12 manual §19.6, p.413 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-020. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.7: Setting up File Types | Live 12 manual §19.7, p.413 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-021. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.8: Where are the Recorded Samples? | Live 12 manual §19.8, p.413 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-021. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.9: Using Remote Control for Recording | Live 12 manual §19.9, p.414 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.10: Capturing MIDI | Live 12 manual §19.10, p.415 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-022. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.10.1: Starting a New Live Set | Live 12 manual §19.10.1, p.416 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-022. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-19.10.2: Adding Material to an Existing Live Set | Live 12 manual §19.10.2, p.417 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-022. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 20 — Bounce to Audio

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-20: Bounce to Audio | Live 12 manual §20, p.418 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-20.1: Bouncing Individual Tracks | Live 12 manual §20.1, p.418 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-023, A6H-024. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-20.2: Bouncing Group Tracks | Live 12 manual §20.2, p.420 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-20.3: Pasting Bounced Audio | Live 12 manual §20.3, p.422 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-024. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 21 — Comping

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-21: Comping | Live 12 manual §21, p.424 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6P-019, A6P-020, A6Z-008, A6Z-010. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-21.1: Take Lanes | Live 12 manual §21.1, p.424 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-025. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-21.2: Inserting and Managing Take Lanes | Live 12 manual §21.2, p.426 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-026. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-21.3: Recording Takes | Live 12 manual §21.3, p.426 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-027. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-21.4: Inserting Samples | Live 12 manual §21.4, p.427 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-028. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-21.5: Auditioning Take Lanes | Live 12 manual §21.5, p.427 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-029. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-21.6: Creating a Comp | Live 12 manual §21.6, p.428 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-080, A6H-030. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-21.7: Source Highlights | Live 12 manual §21.7, p.429 (contents p.9) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-030. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 22 — Stem Separation

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-22: Stem Separation | Live 12 manual §22, p.430 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-22.1: How Stem Separation Works in Live | Live 12 manual §22.1, p.430 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-22.2: Separating Audio Files and Clips | Live 12 manual §22.2, p.430 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-001. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-22.2.1: Separation Speed vs. Quality | Live 12 manual §22.2.1, p.436 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 23 — Working with Instruments and Effects

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-23: Working with Instruments and Effects | Live 12 manual §23, p.438 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6D-005, A6P-005, A6P-013, A6P-014, A6P-019. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.1: Device View | Live 12 manual §23.1, p.438 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.2: Using Devices | Live 12 manual §23.2, p.440 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-031. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.2.1: Device Title Bar | Live 12 manual §23.2.1, p.443 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.2.2: Device A/B Comparison | Live 12 manual §23.2.2, p.447 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.2.3: Live Device Presets | Live 12 manual §23.2.3, p.451 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.2.4: Hot-Swapping Presets | Live 12 manual §23.2.4, p.452 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.2.5: Saving Presets | Live 12 manual §23.2.5, p.455 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-036. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.2.6: Default Presets | Live 12 manual §23.2.6, p.456 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-035. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.3: Using Plug-Ins | Live 12 manual §23.3, p.458 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.3.1: Plug-Ins in the Device View | Live 12 manual §23.3.1, p.460 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-032. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.3.2: Sidechain Parameters | Live 12 manual §23.3.2, p.463 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.4: VST Plug-Ins | Live 12 manual §23.4, p.464 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.4.1: The VST Plug-In Folder | Live 12 manual §23.4.1, p.464 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.4.2: VST Presets and Banks | Live 12 manual §23.4.2, p.467 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-033. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.5: Audio Units Plug-Ins | Live 12 manual §23.5, p.468 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-23.6: Device Delay Compensation | Live 12 manual §23.6, p.469 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-034. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 24 — Instrument, Drum and Effect Racks

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-24: Instrument, Drum and Effect Racks | Live 12 manual §24, p.471 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.1: An Overview of Racks | Live 12 manual §24.1, p.471 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-037. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.1.1: Signal Flow and Parallel Device Chains | Live 12 manual §24.1.1, p.471 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-037. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.1.2: Macro Controls | Live 12 manual §24.1.2, p.472 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.2: Creating Racks | Live 12 manual §24.2, p.473 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-038. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.3: Looking at Racks | Live 12 manual §24.3, p.474 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-039. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.4: Chain List | Live 12 manual §24.4, p.475 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-039, A6H-040. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.4.1: Auto Select | Live 12 manual §24.4.1, p.476 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-041. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.5: Zones | Live 12 manual §24.5, p.477 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-042. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.5.1: Signal Flow through Zones | Live 12 manual §24.5.1, p.477 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-042. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.5.2: Key Zones | Live 12 manual §24.5.2, p.478 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-042. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.5.3: Velocity Zones | Live 12 manual §24.5.3, p.478 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-042. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.5.4: Chain Select Zones | Live 12 manual §24.5.4, p.479 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-043. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.6: Drum Racks | Live 12 manual §24.6, p.481 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-044, A6H-045. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.6.1: Pad View | Live 12 manual §24.6.1, p.482 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-046, A6H-047, A6H-048, A6H-049. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.7: Using the Macro Controls | Live 12 manual §24.7, p.484 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-050. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.7.1: Map Mode | Live 12 manual §24.7.1, p.484 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-051. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.7.2: Randomizing Macro Controls | Live 12 manual §24.7.2, p.485 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-050. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.7.3: Macro Control Variations | Live 12 manual §24.7.3, p.487 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-050. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.8: Mixing With Racks | Live 12 manual §24.8, p.488 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-052. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-24.8.1: Extracting Chains | Live 12 manual §24.8.1, p.489 (contents p.10) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6H-052. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 25 — Automation and Editing Envelopes

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-25: Automation and Editing Envelopes | Live 12 manual §25, p.491 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-005, A6D-007, A6P-013, A6P-015, A6Z-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.1: Recording Automation in Arrangement View | Live 12 manual §25.1, p.491 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-049. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.2: Recording Automation in Session View | Live 12 manual §25.2, p.492 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-050. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.2.1: Session Automation Recording Modes | Live 12 manual §25.2.1, p.494 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.3: Deleting Automation | Live 12 manual §25.3, p.494 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-051, A6C-069. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.4: Overriding Automation | Live 12 manual §25.4, p.494 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5: Drawing and Editing Automation | Live 12 manual §25.5, p.495 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-052, A6C-053. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5.1: Drawing Envelopes | Live 12 manual §25.5.1, p.496 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-054. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5.2: Editing Breakpoints | Live 12 manual §25.5.2, p.497 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-055, A6C-056, A6C-057, A6C-058, A6C-059, A6C-060. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5.3: Stretching and Skewing Envelopes | Live 12 manual §25.5.3, p.499 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-061, A6C-062, A6C-063. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5.4: Simplifying Envelopes | Live 12 manual §25.5.4, p.500 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-064. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5.5: Inserting Automation Shapes | Live 12 manual §25.5.5, p.501 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-065. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5.6: Locking Envelopes | Live 12 manual §25.5.6, p.502 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-066. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5.7: Edit Menu Commands | Live 12 manual §25.5.7, p.502 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-066. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-25.5.8: Editing the Tempo Automation | Live 12 manual §25.5.8, p.502 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-067. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 26 — Clip Envelopes

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-26: Clip Envelopes | Live 12 manual §26, p.504 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-005, A6Z-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.1: The Clip Envelope Editor | Live 12 manual §26.1, p.504 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-068, A6C-069, A6C-073. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.2: Audio Clip Envelopes | Live 12 manual §26.2, p.505 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.2.1: Clip Envelopes are Non-Destructive | Live 12 manual §26.2.1, p.505 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.2.2: Changing Pitch and Tuning per Note | Live 12 manual §26.2.2, p.506 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-071. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.2.3: Muting or Attenuating Notes in a Sample | Live 12 manual §26.2.3, p.507 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.2.4: Scrambling Beats | Live 12 manual §26.2.4, p.507 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-072. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.2.5: Using Clips as Templates | Live 12 manual §26.2.5, p.508 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-026. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.3: Mixer and Device Clip Envelopes | Live 12 manual §26.3, p.508 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-070. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.3.1: Modulating Mixer Volumes and Sends | Live 12 manual §26.3.1, p.510 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-070. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.3.2: Modulating Pan | Live 12 manual §26.3.2, p.512 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.3.3: Modulating Device Controls | Live 12 manual §26.3.3, p.512 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.4: MIDI Controller Clip Envelopes | Live 12 manual §26.4, p.513 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-073. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.5: Unlinking Clip Envelopes From Clips | Live 12 manual §26.5, p.513 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-074, A6C-075. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.5.1: Programming a Fade-Out for a Live Set | Live 12 manual §26.5.1, p.514 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-074. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.5.2: Creating Long Loops from Short Loops | Live 12 manual §26.5.2, p.514 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-074. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.5.3: Imposing Rhythm Patterns onto Samples | Live 12 manual §26.5.3, p.515 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-074. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.5.4: Clip Envelopes as LFOs | Live 12 manual §26.5.4, p.515 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-074. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-26.5.5: Warping Linked Envelopes | Live 12 manual §26.5.5, p.515 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6C-075. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 27 — Working with Video

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-27: Working with Video | Live 12 manual §27, p.517 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6D-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-27.1: Importing Video | Live 12 manual §27.1, p.517 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-047, A6I-013, A6I-014. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-27.2: The Appearance of Video in Live | Live 12 manual §27.2, p.517 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-27.2.1: Video Clips in the Arrangement View | Live 12 manual §27.2.1, p.517 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-048. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-27.2.2: The Video Window | Live 12 manual §27.2.2, p.518 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-049, A6S-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-27.2.3: Clip View | Live 12 manual §27.2.3, p.519 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-051. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-27.3: Matching Sound to Video | Live 12 manual §27.3, p.520 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-053. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-27.4: Video Trimming Tricks | Live 12 manual §27.4, p.520 (contents p.11) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-053. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 31 — Max for Live

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-31: Max for Live | Live 12 manual §31, p.795 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6D-008, A6P-001, A6P-002, A6P-003, A6P-004, A6P-005, A6P-006, A6P-007, A6P-008, A6P-009, A6P-010, A6P-011, A6P-012, A6P-013, A6P-014, A6P-015, A6P-016, A6P-017, A6P-018, A6P-019, A6P-020, A6Z-001, A6Z-002, A6Z-003, A6Z-010, A6U-010. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-31.1: Setting Up Max for Live | Live 12 manual §31.1, p.795 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-31.2: Using Max for Live Devices | Live 12 manual §31.2, p.796 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6U-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-31.3: Editing Max for Live Devices | Live 12 manual §31.3, p.796 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-31.4: Building Max for Live MIDI Tools | Live 12 manual §31.4, p.799 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-31.5: Max Dependencies | Live 12 manual §31.5, p.800 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-31.6: Learning Max Programming | Live 12 manual §31.6, p.800 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 33 — MIDI and Key Remote Control

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-33: MIDI and Key Remote Control | Live 12 manual §33, p.836 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-001, A6X-015. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.1: MIDI Remote Control | Live 12 manual §33.1, p.836 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-001, A6R-013. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.1.1: Natively Supported Control Surfaces | Live 12 manual §33.1.1, p.837 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-002, A6U-001, A6U-003, A6U-010. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.1.2: Manual Control Surface Setup | Live 12 manual §33.1.2, p.838 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-003, A6U-013. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.1.3: Takeover Mode | Live 12 manual §33.1.3, p.839 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-004. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.2: The Mapping Browser | Live 12 manual §33.2, p.840 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.2.1: Assigning MIDI Remote Control | Live 12 manual §33.2.1, p.841 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6U-015. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.2.2: Mapping to MIDI Notes | Live 12 manual §33.2.2, p.841 (contents p.17) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-006, A6R-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.2.3: Mapping to Absolute MIDI Controllers | Live 12 manual §33.2.3, p.841 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-008, A6R-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.2.4: Mapping to Relative MIDI Controllers | Live 12 manual §33.2.4, p.842 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-010, A6R-011, A6U-014. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-33.2.5: Computer Keyboard Remote Control | Live 12 manual §33.2.5, p.844 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 34 — Using Push 1

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-34: Using Push 1 | Live 12 manual §34, p.845 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-010. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.1: Setup | Live 12 manual §34.1, p.845 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.2: Browsing and Loading Sounds | Live 12 manual §34.2, p.846 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-014. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.3: Playing and Programming Beats | Live 12 manual §34.3, p.847 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.3.1: Loop Selector | Live 12 manual §34.3.1, p.847 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.3.2: 16 Velocities Mode | Live 12 manual §34.3.2, p.849 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.3.3: 64-Pad Mode | Live 12 manual §34.3.3, p.849 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.3.4: Loading Individual Drums | Live 12 manual §34.3.4, p.849 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.3.5: Step Sequencing Beats | Live 12 manual §34.3.5, p.850 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-016. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.3.6: Real-time Recording | Live 12 manual §34.3.6, p.852 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-018. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.3.7: Fixed Length Recording | Live 12 manual §34.3.7, p.854 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-019. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.4: Additional Recording Options | Live 12 manual §34.4, p.855 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.4.1: Recording with Repeat | Live 12 manual §34.4.1, p.855 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-020. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.4.2: Quantizing | Live 12 manual §34.4.2, p.856 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.5: Playing Melodies and Harmonies | Live 12 manual §34.5, p.857 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-024. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.5.1: Playing in Other Keys | Live 12 manual §34.5.1, p.859 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-025. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.6: Step Sequencing Melodies and Harmonies | Live 12 manual §34.6, p.860 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-026. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.6.1: Adjusting the Loop Length | Live 12 manual §34.6.1, p.862 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-027, A6R-028. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.7: Melodic Sequencer + 32 Notes | Live 12 manual §34.7, p.863 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.7.1: 32 Notes | Live 12 manual §34.7.1, p.863 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.7.2: Sequencer | Live 12 manual §34.7.2, p.864 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-028. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.8: Navigating in Note Mode | Live 12 manual §34.8, p.864 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-029. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.9: Controlling Live’s Instruments and Effects | Live 12 manual §34.9, p.865 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.10: Mixing with Push 1 | Live 12 manual §34.10, p.866 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-030. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.11: Recording Automation | Live 12 manual §34.11, p.868 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-031. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.12: Step Sequencing Automation | Live 12 manual §34.12, p.869 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.12.1: Note-Specific Parameters | Live 12 manual §34.12.1, p.869 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-032. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.12.2: Per-Step Automation | Live 12 manual §34.12.2, p.870 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-033. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.13: Controlling Live’s Session View | Live 12 manual §34.13, p.870 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-034. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.13.1: Session Overview | Live 12 manual §34.13.1, p.871 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-035. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.14: Setting User Preferences | Live 12 manual §34.14, p.872 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-036. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-34.15: Push 1 Control Reference | Live 12 manual §34.15, p.874 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 35 — Using Push 2

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-35: Using Push 2 | Live 12 manual §35, p.885 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-010, A6D-010. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.1: Setup | Live 12 manual §35.1, p.886 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6U-001, A6U-002, A6U-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.2: Browsing and Loading Sounds | Live 12 manual §35.2, p.886 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-014, A6U-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.3: Playing and Programming Beats | Live 12 manual §35.3, p.889 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.3.1: Loop Selector | Live 12 manual §35.3.1, p.889 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.3.2: 16 Velocities Mode | Live 12 manual §35.3.2, p.891 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.3.3: 64-Pad Mode | Live 12 manual §35.3.3, p.891 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.3.4: Loading Individual Drums | Live 12 manual §35.3.4, p.892 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-014, A6U-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.3.5: Step Sequencing Beats | Live 12 manual §35.3.5, p.895 (contents p.18) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-016, A6R-017. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.3.6: Real-time Recording | Live 12 manual §35.3.6, p.898 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-018, A6R-021. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.3.7: Fixed Length Recording | Live 12 manual §35.3.7, p.900 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-019. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.4: Additional Recording Options | Live 12 manual §35.4, p.901 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.4.1: Recording with Repeat | Live 12 manual §35.4.1, p.901 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-020. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.4.2: Quantizing | Live 12 manual §35.4.2, p.902 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-022. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.4.3: Arrangement Recording | Live 12 manual §35.4.3, p.903 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-023. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.5: Playing Melodies and Harmonies | Live 12 manual §35.5, p.903 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-024. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.5.1: Playing in Other Keys | Live 12 manual §35.5.1, p.906 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-025. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.6: Step Sequencing Melodies and Harmonies | Live 12 manual §35.6, p.907 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-026, A6U-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.6.1: Adjusting the Loop Length | Live 12 manual §35.6.1, p.909 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-027, A6R-028. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.7: Melodic Sequencer + 32 Notes | Live 12 manual §35.7, p.910 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6U-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.7.1: 32 Notes | Live 12 manual §35.7.1, p.911 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.7.2: Sequencer | Live 12 manual §35.7.2, p.911 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-028. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.8: Working with Samples | Live 12 manual §35.8, p.913 (contents p.19) | Excluded from this research pass: this section operates the built-in Simpler instrument. Stable inventory ID retained for traceability. | Excluded — built-in instrument operation; no completion claim |
| A-BASE-35.8.1: Classic Playback Mode | Live 12 manual §35.8.1, p.914 (contents p.19) | Excluded from this research pass: this section operates the built-in Simpler instrument. Stable inventory ID retained for traceability. | Excluded — built-in instrument operation; no completion claim |
| A-BASE-35.8.2: One-Shot Mode | Live 12 manual §35.8.2, p.915 (contents p.19) | Excluded from this research pass: this section operates the built-in Simpler instrument. Stable inventory ID retained for traceability. | Excluded — built-in instrument operation; no completion claim |
| A-BASE-35.8.3: Slicing Mode | Live 12 manual §35.8.3, p.917 (contents p.19) | Excluded from this research pass: this section operates the built-in Simpler instrument. Stable inventory ID retained for traceability. | Excluded — built-in instrument operation; no completion claim |
| A-BASE-35.9: Navigating in Note Mode | Live 12 manual §35.9, p.919 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-029, A6U-004, A6U-005, A6U-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.10: Working With Instruments and Effects | Live 12 manual §35.10, p.920 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6U-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.10.1: Adding, Deleting, and Reordering Devices | Live 12 manual §35.10.1, p.922 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-041, A6U-006. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.10.2: Working with Racks | Live 12 manual §35.10.2, p.923 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-042, A6U-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.11: Track Control And Mixing | Live 12 manual §35.11, p.925 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-030. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.11.1: Rack and Group Track Mixing | Live 12 manual §35.11.1, p.927 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-042. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.12: Recording Automation | Live 12 manual §35.12, p.928 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-031. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.13: Step Sequencing Automation | Live 12 manual §35.13, p.929 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-033. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.14: Clip Mode | Live 12 manual §35.14, p.930 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-043, A6U-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.14.1: Using MIDI Tracks in Clip Mode | Live 12 manual §35.14.1, p.932 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-044, A6U-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.14.2: Real-Time Playing Layouts | Live 12 manual §35.14.2, p.932 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-044. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.14.3: Sequencing Layouts | Live 12 manual §35.14.3, p.932 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-044. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.14.4: Note-Specific Parameters | Live 12 manual §35.14.4, p.934 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-032. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.15: Controlling Live’s Session View | Live 12 manual §35.15, p.934 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-034, A6U-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.15.1: Session Overview | Live 12 manual §35.15.1, p.936 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-035. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.16: Setup Menu | Live 12 manual §35.16, p.937 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-036, A6R-045, A6U-002, A6U-004. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-35.17: Push 2 Control Reference | Live 12 manual §35.17, p.938 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-017, A6R-022, A6R-023, A6R-037, A6R-038, A6R-046, A6U-006, A6U-007, A6U-008, A6U-010, A6U-013, A6U-014, A6U-015. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 36 — Synchronizing with Link, Tempo Follower, and MIDI

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-36: Synchronizing with Link, Tempo Follower, and MIDI | Live 12 manual §36, p.944 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6D-009, A6G-002. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.1: Synchronizing via Link | Live 12 manual §36.1, p.944 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-002. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.1.1: Setting up Link and Link Audio | Live 12 manual §36.1.1, p.944 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6I-003, A6I-004, A6I-006. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.1.2: Using Link | Live 12 manual §36.1.2, p.947 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-002. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.1.3: Using Link Audio | Live 12 manual §36.1.3, p.947 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6I-001, A6I-002, A6I-003, A6I-004, A6I-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.2: Synchronizing via Tempo Follower | Live 12 manual §36.2, p.949 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.2.1: Setting Up Tempo Follower | Live 12 manual §36.2.1, p.949 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.3: Synchronizing via MIDI | Live 12 manual §36.3, p.950 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.3.1: Synchronizing External MIDI Devices to Live | Live 12 manual §36.3.1, p.950 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.3.2: Synchronizing Live to External MIDI Devices | Live 12 manual §36.3.2, p.951 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-36.3.3: Sync Delay | Live 12 manual §36.3.3, p.952 (contents p.19) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 37 — Computer Audio Resources and Strategies

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-37: Computer Audio Resources and Strategies | Live 12 manual §37, p.953 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-008, A6P-018. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-37.1: Managing the CPU Load | Live 12 manual §37.1, p.953 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-37.1.1: The CPU Load Meter | Live 12 manual §37.1.1, p.953 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-37.1.2: CPU Load from Multichannel Audio | Live 12 manual §37.1.2, p.955 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-37.1.3: CPU Load from Tracks and Devices | Live 12 manual §37.1.3, p.955 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-003. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-37.1.4: Track Freeze | Live 12 manual §37.1.4, p.956 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-005. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-37.2: Managing the Disk Load | Live 12 manual §37.2, p.957 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-004. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 38 — Audio Fact Sheet

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-38: Audio Fact Sheet | Live 12 manual §38, p.959 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.1: Testing and Methodology | Live 12 manual §38.1, p.959 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2: Neutral Operations | Live 12 manual §38.2, p.959 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.1: Undithered Rendering | Live 12 manual §38.2.1, p.960 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.2: Matching Sample Rate/No Transposition | Live 12 manual §38.2.2, p.960 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-007. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.3: Unstretched Beats/Tones/Texture/Re-Pitch Warping | Live 12 manual §38.2.3, p.960 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.4: Summing at Single Mix Points | Live 12 manual §38.2.4, p.961 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-006. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.5: Recording External Signals (Bit Depth >/= A/D Converter) | Live 12 manual §38.2.5, p.961 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.6: Recording Internal Sources at 32 Bit | Live 12 manual §38.2.6, p.961 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.7: Freezing Tracks | Live 12 manual §38.2.7, p.962 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.8: Bypassed Effects | Live 12 manual §38.2.8, p.962 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.9: Routing | Live 12 manual §38.2.9, p.963 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.2.10: Splitting Clips | Live 12 manual §38.2.10, p.963 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-009. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3: Non-Neutral Operations | Live 12 manual §38.3, p.963 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.1: Playback in Complex and Complex Pro Mode | Live 12 manual §38.3.1, p.963 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.2: Sample Rate Conversion/Transposition | Live 12 manual §38.3.2, p.964 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-010. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.3: Volume Automation | Live 12 manual §38.3.3, p.964 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.4: Dithering | Live 12 manual §38.3.4, p.964 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-010. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.5: Recording External Signals (Bit Depth < A/D Converter) | Live 12 manual §38.3.5, p.964 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.6: Recording Internal Sources Below 32 Bit | Live 12 manual §38.3.6, p.965 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-008. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.7: Consolidate | Live 12 manual §38.3.7, p.965 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.8: Clip Fades | Live 12 manual §38.3.8, p.965 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.9: Panning | Live 12 manual §38.3.9, p.965 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.3.10: Grooves | Live 12 manual §38.3.10, p.965 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.4: Tips for Achieving Optimal Sound Quality in Live | Live 12 manual §38.4, p.966 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-38.5: Conclusion | Live 12 manual §38.5, p.966 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 39 — MIDI Fact Sheet

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-39: MIDI Fact Sheet | Live 12 manual §39, p.967 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-39.1: Ideal MIDI Behavior | Live 12 manual §39.1, p.967 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-39.2: MIDI Timing Problems | Live 12 manual §39.2, p.968 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-39.3: Live’s MIDI Solutions | Live 12 manual §39.3, p.968 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6I-007, A6I-008, A6I-009, A6I-010, A6I-011, A6I-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-39.4: Variables Outside of Live’s Control | Live 12 manual §39.4, p.969 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-011, A6I-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-39.5: Tips for Achieving Optimal MIDI Performance | Live 12 manual §39.5, p.971 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-39.6: Summary and Conclusions | Live 12 manual §39.6, p.971 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 40 — Accessibility and Keyboard Navigation

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-40: Accessibility and Keyboard Navigation | Live 12 manual §40, p.973 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6S-011, A6T-001, A6T-002, A6T-003, A6D-011. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.1: Menu and Keyboard Navigation Settings | Live 12 manual §40.1, p.973 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.1.1: Using Tab for Navigation | Live 12 manual §40.1.1, p.973 (contents p.20) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-054. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.1.2: Settings Menu | Live 12 manual §40.1.2, p.974 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-055. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.1.3: Options Menu | Live 12 manual §40.1.3, p.975 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-056. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.1.4: Speak Help Text | Live 12 manual §40.1.4, p.975 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6R-056. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.2: Audio Setup | Live 12 manual §40.2, p.975 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.3: Connecting MIDI Devices | Live 12 manual §40.3, p.975 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.4: Navigating in Live | Live 12 manual §40.4, p.976 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.4.1: Navigate Menu | Live 12 manual §40.4.1, p.976 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.5: Editing Automation and Modulation Envelopes | Live 12 manual §40.5, p.981 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.5.1: Navigating Between Breakpoints | Live 12 manual §40.5.1, p.981 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.5.2: Selecting and Editing Breakpoints | Live 12 manual §40.5.2, p.982 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-40.5.3: Switching Between Automation Envelopes in Arrangement View | Live 12 manual §40.5.3, p.983 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

#### Chapter 41 — Live Keyboard Shortcuts

| What to investigate in Live | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| A-BASE-41: Live Keyboard Shortcuts | Live 12 manual §41, p.984 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.1: Showing and Hiding Views | Live 12 manual §41.1, p.984 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.2: Keyboard Focus and Navigation | Live 12 manual §41.2, p.985 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.3: Working with Sets and the Program | Live 12 manual §41.3, p.986 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.4: Working with Devices and Plug-Ins | Live 12 manual §41.4, p.987 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.5: Editing | Live 12 manual §41.5, p.987 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.6: Adjusting Values | Live 12 manual §41.6, p.988 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.7: Commands for Breakpoint Envelopes | Live 12 manual §41.7, p.989 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.8: Loop Brace and Start/End Markers | Live 12 manual §41.8, p.990 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.9: Zooming, Display and Selections | Live 12 manual §41.9, p.990 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.10: Clip View Editor View Modes | Live 12 manual §41.10, p.991 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.11: Clip View Sample Editor | Live 12 manual §41.11, p.992 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.12: Clip View MIDI Note Editor | Live 12 manual §41.12, p.992 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-012. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.13: Grid Snapping and Drawing | Live 12 manual §41.13, p.995 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-013. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.14: Global Quantization | Live 12 manual §41.14, p.995 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. M6 related contracts: A6G-013. Exact topic linkage only; completion dimensions remain open. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.15: Session View | Live 12 manual §41.15, p.996 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.16: Arrangement View | Live 12 manual §41.16, p.997 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.17: Comping | Live 12 manual §41.17, p.999 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.18: Bounce to Audio | Live 12 manual §41.18, p.999 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.19: Commands for Tracks | Live 12 manual §41.19, p.1000 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.20: Transport | Live 12 manual §41.20, p.1001 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.21: Audio Engine | Live 12 manual §41.21, p.1002 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.22: Browser | Live 12 manual §41.22, p.1002 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.23: Similar Sample Swapping | Live 12 manual §41.23, p.1003 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.24: Key/MIDI Map Mode and the Computer MIDI Keyboard | Live 12 manual §41.24, p.1003 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.25: Momentary Latching Shortcuts | Live 12 manual §41.25, p.1004 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.26: General Keyboard Navigation and Workflow | Live 12 manual §41.26, p.1004 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.26.1: Using Tab for Navigation | Live 12 manual §41.26.1, p.1005 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.26.2: Navigating Between Controls in the Settings Menu | Live 12 manual §41.26.2, p.1005 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.27: Editing Automation and Modulation Envelopes with the Keyboard | Live 12 manual §41.27, p.1006 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.28: Accessing Menus | Live 12 manual §41.28, p.1006 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |
| A-BASE-41.29: Using Live’s Context Menu | Live 12 manual §41.29, p.1006 (contents p.21) | Baseline parity; apply the completion dimensions above and map the resulting contract to current FEATURES/ADR. | Unknown / not tested — inventory; selected findings may exist above |

## Deep host research — VariAudio, workspaces and Live priority — 2026-11-04

This pass prioritizes Live as the base reference and expands Cubase VariAudio, multi-display/workspace behavior, key commands and preferences. All new entries are documentary or explicitly unresolved; none is a new installed-app measurement. Source hashes and per-row cases are in private E34–E36 and DEEP-HOST-MISSIONS-5.md. Existing ADI priorities/deviations remain unchanged.

The A-BASE inventory above remains open. These findings deepen selected contracts; neither a topic count nor this pass closes the full host. The source manual revision is unconfirmed against installed Live 12.4.

### Mixer, freeze, clocks, Session, engine hypotheses and recovery — Mixer, selection, groups and cue paths

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5C-001: Track meters change from output to input metering while monitoring; peak and RMS are both shown. | Live 12 local manual, §18.1 p.391 | FEATURES §6: meter tap must follow monitoring state explicitly. Measurement case M5-A5C-001. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-002: Multi-track continuous-control edits preserve differing values; solo/arm/activation can act on the entire selection. Exclusive preferences and modifiers change the affected set. | Live 12 local manual, pp.392,395 | Window parity: distinguish relative continuous edits from grouped toggles. Measurement case M5-A5C-002. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-003: Stereo Pan and Split Stereo Pan have different semantics: the latter positions left/right independently. Double-click resets pan. | Live 12 local manual, p.392 | FEATURES §6: store pan mode and channels separately. Measurement case M5-A5C-003. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-004: Meter tick marks, numeric volume and peak reset become visible with mixer height; greater track width reveals a decibel scale. | Live 12 local manual, p.393 | UI parity: resizing changes affordances without changing audio state. Measurement case M5-A5C-004. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-005: Dragging clips into a new-track drop area copies the source devices; dragging nonadjacent track headers gathers them, while Ctrl+arrow preserves spacing. | Live 12 local manual, p.394 | FEATURES §2/9: distinguish clip move, track construction and reorder. Measurement case M5-A5C-005. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-006: A leading # in track names generates an order-dependent number; additional # characters add zero padding. | Live 12 local manual, p.394 | Naming requires stored template versus displayed label. Measurement case M5-A5C-006. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-007: Grouping reroutes ordinary Main-bound children to the group but preserves existing custom routes. Groups can therefore act as folders without summing every child. | Live 12 local manual, §18.3 p.395 | FEATURES §2/6: containment is not audio routing. Measurement case M5-A5C-007. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-008: Deleting a group deletes contents; ungrouping preserves children. The local manual describes Assign Track Color to Grouped Tracks and Clips as changing clips only in the initiating Session or Arrangement view. A later 12.4.3 release change is recorded at A6D-001; do not generalize this older scope to all Live 12 builds. | Live 12 local manual, p.396 | Track lifecycle and view-scoped batch edits need different operations. Measurement case M5-A5C-008. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-009: Return-to-return/self sends start disabled. A return Pre/Post switch selects a tap before/after pan, volume and track-active controls. | Live 12 local manual, §18.4 p.398 | N42/N43: keep tap semantics separate from the unresolved lit-state glyph. Measurement case M5-A5C-009. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-010: Crossfader assignments change gain without changing routing; returns can be assigned. Crossfader position and per-track assignment have separate automation targets. | Live 12 local manual, §18.5 pp.398–401 | FEATURES §6: model gain-stage control, routing and assignments independently. Measurement case M5-A5C-010. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-011: Soloing a return keeps its incoming sends audible while muting other main outputs. Solo in Place governs return audibility when soloing a clip track. | Live 12 local manual, §18.6 p.401 | N23: build the route-aware solo truth table rather than a global mute loop. Measurement case M5-A5C-011. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-012: Cue mode replaces Solo controls, uses a separate hardware output and leaves Track Activator controlling Main audibility; browser preview uses Cue Out when cueing is active. | Live 12 local manual, pp.401–402 | N23: preview, cue and Main are distinct paths. Measurement case M5-A5C-012. | Supported hypothesis — documented; installed behavior unmeasured |

### Mixer, freeze, clocks, Session, engine hypotheses and recovery — Freeze ownership, tails and processing state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5C-013: Freeze generates one file per Session clip plus one for Arrangement; group tracks cannot be frozen under the documented command. External hardware forces realtime freezing. | Live 12 local manual, §37.1.4 p.956 | FEATURES §2: frozen representation has view-dependent granularity. Measurement case M5-A5C-013. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-014: Frozen tracks retain mixer edits and some clip operations; editing frozen material can produce different results after unfreeze because effects are recalculated. | Live 12 local manual, pp.956–957 | Keep source/edit/derived state separately; edits do not imply equivalent rerender. Measurement case M5-A5C-014. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-015: Arrangement freeze tails appear as separate temporary clips, disappear on unfreeze and may need to be selected with the parent when moving. | Live 12 local manual, p.957 | Tail ownership and edit selection need explicit invariants. Measurement case M5-A5C-015. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-016: Frozen Session clips contain two loop cycles; longer independent envelope cycles or tails can change. Frozen tails stop with playback rather than continuing as live effects. | Live 12 local manual, pp.957,962 | Freeze is not universally playback-equivalent. Measurement case M5-A5C-016. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-017: The fact sheet describes Session freeze taking parameter values at Arrangement 1.1.1, while Arrangement freeze incorporates parameter automation. Frozen randomness becomes rendered audio. | Live 12 local manual, §38.2.7 p.962 | Document state origin and deterministic derivative playback. Measurement case M5-A5C-017. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-018: Freeze files live in Samples/Processed/Freeze or temporary storage before save; external-device freeze files are immediately discarded on unfreeze. | Live 12 local manual, p.957 | Media retention must include undo/history dependencies under ADI policy. Measurement case M5-A5C-018. | Supported hypothesis — documented; installed behavior unmeasured |

### Mixer, freeze, clocks, Session, engine hypotheses and recovery — Engine hypotheses and numerical test boundaries

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5C-019: CPU percentage is described as audio-buffer processing time divided by buffer playback time; UI work is excluded. This is not operating-system total CPU. | Live 12 local manual, §37.1.1 pp.953–954 | ADR-0207: separate deadline utilization, total CPU and UI cost. Measurement case M5-A5C-019. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-020: Unused hardware channels are not automatically disabled; changing hardware configuration can interrupt audio. Silence-related device suspension is described as waiting for effect tails. | Live 12 local manual, §37.1.2–3 p.955 | Engine scheduling needs explicit activation and tail rules. Measurement case M5-A5C-020. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-021: The fact sheet describes bypass retaining reported latency despite removing effect processing from the audible path. | Live 12 local manual, §38.2.8 p.962 | Bypass and latency/graph lifetime are separate states. Measurement case M5-A5C-021. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-022: Consolidation is described as normalizing the derived audio and compensating its clip volume; output rate/depth come from settings. | Live 12 local manual, §38.3.7 p.965 | A4 consolidation requires both file and clip-state comparison. Measurement case M5-A5C-022. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-023: The fact sheet specifies per-sample volume automation updates and constant-power sinusoidal panning, but does not establish all plugin parameter update rates or Split Stereo behavior. | Live 12 local manual, §38.3.3/9 pp.964–965 | ADR-0207: numerical claims remain specific hypotheses. Measurement case M5-A5C-023. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-024: MIDI input uses driver timestamps; the guide describes monitoring-related timestamp delay and internally scheduled output for Windows MME. | Live 12 local manual, §39.3 pp.968–969 | ADR-0132: distinguish input timestamp, audible time and saved note time. Measurement case M5-A5C-024. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-025: The guide says incoming MIDI can still be captured during audio overload, while monitored audio may distort or mistime. Its published jitter results belong to its unnamed test hardware. | Live 12 local manual, §39.3–4 pp.969–971 | Retain event delivery independently of audio deadline success. Measurement case M5-A5C-025. | Supported hypothesis — documented; installed behavior unmeasured |

### Mixer, freeze, clocks, Session, engine hypotheses and recovery — Clock ownership, network audio and transport

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5C-026: Link peer tempo changes override Set tempo automation; the last simultaneous change wins in the documented description. Recording count-in is unavailable with Link enabled. | Live 12 local manual, §36.1.2 p.947 | ADR-0185: external clock authority must be visible and recoverable. Measurement case M5-A5C-026. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-027: Link Audio requires Link plus its Audio switch. Input latency buffers peers; Sync to Incoming Audio delays Live playback and is documented for one participant only. | Live 12 local manual, §36.1.1 pp.946–947 | Separate network buffering from device latency and beat synchronization. Measurement case M5-A5C-027. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-028: Link Audio sources are chosen by peer then source track; all tracks with audio output can be streamed, while some peers may only send. | Live 12 local manual, §36.1.3 p.948 | Network endpoints require stable identity/capabilities distinct from displayed names. Measurement case M5-A5C-028. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-029: Tempo Follower stops when its toggle is hidden and becomes unavailable if the selected audio input cannot connect. It excludes incoming External Sync but can still send MIDI clock. | Live 12 local manual, §36.2 pp.949–950 | Settings visibility can have operational meaning; document authority transitions. Measurement case M5-A5C-029. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-030: Live can send/receive MIDI Clock but only receive MIDI Timecode; MTC provides position without tempo. Song position pointers wrap into an active Arrangement loop. | Live 12 local manual, §36.3 pp.950–951 | Keep clock, song position, frame time and tempo separate. Measurement case M5-A5C-030. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-031: MTC frame-rate detection/offset and MIDI Sync Delay are configured per device; displayed time ruler format is separate from synchronization interpretation. | Live 12 local manual, pp.951–952 | Persist device-specific timing and display settings independently. Measurement case M5-A5C-031. | Supported hypothesis — documented; installed behavior unmeasured |

### Mixer, freeze, clocks, Session, engine hypotheses and recovery — Arrangement navigation, scene state and view ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5C-032: Arrangement waveform vertical zoom is global display scaling, including newly recorded clips; it does not adjust clip gain. | Live 12 local manual, §6.1 p.162 | Keep view amplitude scaling out of signal state and musical undo. Measurement case M5-A5C-032. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-033: Z zooms to selection and repeated X walks back through prior zoom states. Alt-scroll zooms selected tracks, including all tracks with selected time content. | Live 12 local manual, §6.2 p.163 | View-history and selection-driven track height need explicit scope. Measurement case M5-A5C-033. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-034: Follow pauses after editing, horizontal scrolling or ruler clicks, then resumes on specified transport/scrub actions. | Live 12 local manual, p.163 | Distinguish enabled preference from temporarily suspended following. Measurement case M5-A5C-034. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-035: Meter changes may create fragmentary bars; completing/deleting a fragment inserts/deletes time across every track. | Live 12 local manual, §6.5 pp.167–169 | Tempo/meter editing can be a project-wide structural operation. Measurement case M5-A5C-035. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-036: Scene launch can be canceled from Main context menu; selecting the next scene is a separate preference from launching it. | Live 12 local manual, §7.2 p.185 | Scheduled action, selection and active scene are separate identities. Measurement case M5-A5C-036. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-037: Scene tempo/signature controls have enabled/disabled state distinct from their values. Enter first selects the associated scene and only a subsequent Enter launches it. | Live 12 local manual, §7.2.1 p.186 | Focus/activation state matters for keyboard command routing. Measurement case M5-A5C-037. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-038: Selecting a group slot selects referenced child clips; its visual color is described as the leftmost child clip color. Empty group slots carry stop controls. | Live 12 local manual, §7.1 p.184 | Group slots are aggregate views rather than independent audio clips. Measurement case M5-A5C-038. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-039: Track status distinguishes looping count/length, one-shot remaining time, monitored input and Arrangement playback. A group uses an aggregate looping indicator. | Live 12 local manual, §7.3 pp.189–190 | UI state must derive from transport/source ownership, not selected clip. Measurement case M5-A5C-039. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-040: Stopping a Session clip does not restore Arrangement playback on that track. Session and Arrangement clips are independent, with Back to Arrangement explicitly restoring ownership. | Live 12 local manual, §7.5 pp.192–193 | Model silent Session override separately from Arrangement ownership. Measurement case M5-A5C-040. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-041: Removing a slot stop button makes scene launch leave that track untouched. Capture and Insert Scene copies running clips and launches the resulting scene. | Live 12 local manual, §7.4 pp.191–192 | Scene capture and scene launch need full affected-track/phase contracts. Measurement case M5-A5C-041. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-042: Consolidate Time to New Scene creates one clip per selected track and new files for qualifying audio tracks; second-window drag also transfers between Session and Arrangement. | Live 12 local manual, §7.5 p.194 | Distinguish content transfer from derivation and from window state. Measurement case M5-A5C-042. | Supported hypothesis — documented; installed behavior unmeasured |

### Mixer, freeze, clocks, Session, engine hypotheses and recovery — Stem separation as an editor job

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5C-043: Separation accepts mono/stereo audio; merging selected outputs is available for two or three categories, not one or all four. Starting separation stops playback. | Live 12 local manual, §22 pp.430–434 | ADR-0205: analysis jobs have eligibility, transport and result-structure contracts. Measurement case M5-A5C-043. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-044: Clip start/end and loop settings limit processed material; separation of a time selection splits the source at its boundaries. | Live 12 local manual, pp.434–435 | Define source coordinate range before asynchronous work starts. Measurement case M5-A5C-044. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-045: For grouped stem results, track effects and automation move/copy to the new group; clip envelopes go to stem clips and the source clip is deactivated after success. | Live 12 local manual, pp.435–436 | Result application must preserve graph ownership and avoid doubled playback. Measurement case M5-A5C-045. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-046: Merged outputs also retain individual stem files. High-quality Others requires other components to be processed even when those clips are not added to the Set. | Live 12 local manual, p.436 | Visible clips are not the entire generated-file dependency set. Measurement case M5-A5C-046. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-047: The guide describes separated files at 44.1 kHz and source bit depth, using the recording file-type preference. Platform processing resources differ in the documented implementation. | Live 12 local manual, pp.436–437 | Keep model execution format, output format and device capability separate. Measurement case M5-A5C-047. | Supported hypothesis — documented; installed behavior unmeasured |

### Mixer, freeze, clocks, Session, engine hypotheses and recovery — Recovery versus ordinary Undo History

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5C-048: The recovery guide identifies version-specific Undo, BaseFiles and recovery-info artifacts; recovery must use the same Live version that produced them. Declining recovery can move its inputs into a Crash folder. | [Official recovery guide](https://help.ableton.com/hc/en-us/articles/115001878844-Recovering-a-Set-manually-after-a-crash), accessed 2026-11-04; not a 12.4 runtime observation | ADR-0110: session-only visible Undo History does not prove there is no disk recovery journal. Distinguish recovery inputs from user-accessible persistent history. Measurement case M5-A5C-048. | Supported hypothesis — documented; installed behavior unmeasured |
| A5C-049: The guide explicitly leaves recovery of unsaved recorded audio uncertain; recovering edit state alone does not guarantee its media survived. | [Official recovery guide](https://help.ableton.com/hc/en-us/articles/115001878844-Recovering-a-Set-manually-after-a-crash), accessed 2026-11-04 | ADI backup/recovery must cover project transactions and media publication together. Measurement case M5-A5C-049. | Supported hypothesis — documented; installed behavior unmeasured |

### Browser, media, defaults, device comparison and hosting — Browser query, navigation and tags

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5H-001: Text search requires all entered terms; Ctrl+F switches to All and focuses search. A # prefix searches tags with completion. | Live 12 local manual, §4.2 pp.63–66; exact revision unconfirmed; installed 12.4 | ADR-0104/N15: retain text query, tag predicates and search scope as separate fields; substring/token and case rules still need measurement. Measurement case M5-A5H-001. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-002: The search-field x clears text without resetting filters; Results Clear removes text and tags; Esc in the documented keyboard search workflow clears both. | Live 12 local manual, §4.2 pp.65–66; exact revision unconfirmed; installed 12.4 | ADR-0104: distinct reset commands need explicit semantics and focus-aware shortcuts. Measurement case M5-A5H-002. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-003: Browser history includes previous searches and navigation states. Columns other than Name can be shown, reordered and used for ascending/descending sorting; extensions can be hidden. | Live 12 local manual, §4.1 pp.62–63; §4.3 pp.69–70; exact revision unconfirmed; installed 12.4 | N15: view/history state is not media mutation; determine app versus window ownership. Measurement case M5-A5H-003. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-004: Custom tags may have subtags; Creator tags cannot. A new custom group becomes visible after assigning one of its tags. Default groups/tags cannot be renamed or deleted. | Live 12 local manual, §4.4.3 pp.75–77; exact revision unconfirmed; installed 12.4 | ADR-0104: distinguish taxonomy definitions, item assignments and presentation; user additions must preserve factory provenance. Measurement case M5-A5H-004. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-005: Quick Tags shows the union for multiple selected items, marking tags not shared by every item with an asterisk. Assigned tags appear alphabetically; selecting one shows its group in Status Bar. | Live 12 local manual, §4.4.4 pp.77–79; exact revision unconfirmed; installed 12.4 | N15: multiselection needs explicit mixed state; one bulk action must resolve its affected items before commit. Measurement case M5-A5H-005. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-006: Automatic sample tags may be disabled, removed or converted to user tags. VST3 subcategory metadata can supply tags when it maps to a Live category. | Live 12 local manual, §4.4.2–3 pp.72–73; exact revision unconfirmed; installed 12.4 | ADR-0104: preserve automatic versus user authority and plugin-declared metadata separately. Measurement case M5-A5H-006. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-007: Collections accept heterogeneous items and folders, with multiple colors per item but at most three color squares displayed. Assigning a hidden unused color reveals its label; removing all assignments does not hide it. | Live 12 local manual, §4.5 pp.79–81; exact revision unconfirmed; installed 12.4 | N15: membership, displayed markers and sidebar visibility are separate states; do not truncate stored tags to visible capacity. Measurement case M5-A5H-007. | Supported hypothesis — documented; installed behavior unmeasured |

### Browser, media, defaults, device comparison and hosting — Similarity, preview and insertion

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5H-008: Similarity Search compares against Core/User Library content, supports samples up to 60 seconds plus instrument/drum presets, and orders results by similarity. Saved similarity labels retain the reference at the top. | Live 12 local manual, §3.4 pp.36–37; exact revision unconfirmed; installed 12.4 | ADR-0104: store reference identity and query type, not a materialized ranking assumed permanent; model/corpus changes can change results. Measurement case M5-A5H-008. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-009: New user audio is analyzed in the background; analysis status and Pause are exposed, while Core Library content is pre-analyzed. | Live 12 local manual, §3.4 p.37; exact revision unconfirmed; installed 12.4 | FEATURES §9: async index state needs cancellation/pause, revision tracking and stale-result handling outside the audio thread. Measurement case M5-A5H-009. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-010: Preview can be invoked with Shift+Enter or Right Arrow even if automatic preview is off. Scrubbing is unavailable for saved clips with Warp off. | Live 12 local manual, §4.9 pp.115–116; exact revision unconfirmed; installed 12.4 | ADR-0104: audition activation and per-item capability differ; do not silently modify a clip to preview it. Measurement case M5-A5H-010. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-011: Non-Raw previews loop and can begin at the next bar while transport runs. Raw plays at original tempo without looping or scrubbing; Preview/Cue Volume and cue outputs control audition. | Live 12 local manual, §4.9 pp.117–118; exact revision unconfirmed; installed 12.4 | ADR-0104/N43: audition clock, source-time playback and monitor destination are independent of project insertion. Measurement case M5-A5H-011. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-012: Opening a browser folder normally closes previously open folders; Ctrl/Cmd preserves them. Session sample loading targets a clip slot on the selected audio track but creates Simpler on MIDI tracks. Drop areas create a new track. | Live 12 local manual, §4.8 pp.115; §4.10 pp.118–119; exact revision unconfirmed; installed 12.4 | FEATURES §3/9: focus, destination type and insertion method must resolve one typed operation; ADI hybrid behavior is a deliberate difference. Measurement case M5-A5H-012. | Supported hypothesis — documented; installed behavior unmeasured |

### Browser, media, defaults, device comparison and hosting — Library, dependencies and file recovery

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5H-013: Added Places folders are indexed in place, not moved. Missing folders remain greyed; Locate repairs the path and Remove from Sidebar forgets registration without deleting the content. | Live 12 local manual, §4.7.8 pp.113–114; exact revision unconfirmed; installed 12.4 | ADR-0104: mount availability, path registration and content identity must be separate; removal is not file deletion. Measurement case M5-A5H-013. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-014: A Backup folder appears after at least two saves and retains the last ten saved Set versions; older backups are moved to trash. An unsaved Set uses temporary project storage with no Backup folder. | Live 12 local manual, §4.7.7 pp.112–113; exact revision unconfirmed; installed 12.4 | ADR-0110/FEATURES §10: backup snapshots differ from session undo or ADI persistent history; media reachability needs independent verification. Measurement case M5-A5H-014. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-015: Compressed sample playback uses decoded temporary audio; the cache limits total size and minimum free disk space. Cleanup removes files not used by the current Set. | Live 12 local manual, §5.1–5.1.1 p.120; exact revision unconfirmed; installed 12.4 | FEATURES §3/10: source media is not its regenerable decode cache; eviction must account for active reads and project switches. Measurement case M5-A5H-015. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-016: File Manager replacement changes every clip referencing the file. Clip properties remain, but warp markers survive only when the replacement is at least as long as the old source. | Live 12 local manual, §5.4.6 p.136; exact revision unconfirmed; installed 12.4 | ADR-0127/0061: global source replacement differs from replacing one clip; validate affected references and derived constraints before commit. Measurement case M5-A5H-016. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-017: External sample editing holds the sample offline while its Edit switch remains engaged; existing warp markers survive only when the sample length remains unchanged. | Live 12 local manual, §5.4.6 p.136; exact revision unconfirmed; installed 12.4 | ADR-0061/0127: external edits need a declared handoff/reload transaction and content revision, not blind path reuse. Measurement case M5-A5H-017. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-018: Missing samples cause silence and Offline marking. Manual repair accepts the supplied replacement without verifying that it is the original; automatic repair accepts a single candidate and asks the user when multiple candidates exist. | Live 12 local manual, §5.6 pp.142–144; exact revision unconfirmed; installed 12.4 | ADR-0127: ADI hash identity is a stronger policy; expose intentional substitution separately from verified relink. Measurement case M5-A5H-018. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-019: Project-based Packs restore the complete project folder and Sets, but external files are not automatically included. Packing leaves the original project intact. | Live 12 local manual, §5.9 pp.154–157; exact revision unconfirmed; installed 12.4 | ADR-0127: archive creation and dependency collection are distinct steps; ADI export must report missing/excluded dependencies. Measurement case M5-A5H-019. | Supported hypothesis — documented; installed behavior unmeasured |

### Browser, media, defaults, device comparison and hosting — Device insertion, comparison and presets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5H-020: Device paste inserts before the selected device; selecting the space after the chain pastes at its end. Device ordering follows MIDI effects, instrument, then audio effects. | Live 12 local manual, §23.2 pp.441–443; exact revision unconfirmed; installed 12.4 | FEATURES §6/parity-7.4: graph insertion must validate type and destination before mutation; stream-continuity claims remain unmeasured. Measurement case M5-A5H-020. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-021: Built-in device A/B starts with equal defaults; edits change the selected state. Comparison commands do not apply to Racks, Max devices or third-party plug-ins. | Live 12 local manual, §23.2.2 pp.447–449; exact revision unconfirmed; installed 12.4 | FEATURES §6: A/B is host state with device capability limits; it is not a substitute for a processor’s own A/B feature. Measurement case M5-A5H-021. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-022: A/B switching disables parameter automation; switching back does not automatically restore it. Re-enable is explicit and switching again disables it again. | Live 12 local manual, §23.2.2 pp.449–450; exact revision unconfirmed; installed 12.4 | ADR-0110/N26: bank selection, stored envelopes and automation override require separate state transitions. Measurement case M5-A5H-022. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-023: Copying/duplicating a built-in device or saving its preset uses only the selected A/B state, initializing both states of the copy from it. | Live 12 local manual, §23.2.2 p.450; exact revision unconfirmed; installed 12.4 | FEATURES §6/10: distinguish cloning complete runtime state from exporting a selected sound state; preserve the chosen transfer contract. Measurement case M5-A5H-023. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-024: Hot-Swap uses the selected preset or, with none selected, the first audio effect on audio tracks or instrument on MIDI tracks. Replacement must keep the device type, although it may change the specific device. | Live 12 local manual, §23.2.4 p.452; exact revision unconfirmed; installed 12.4 | FEATURES §6/9: capture an explicit target identity before browsing; selection changes must not retarget a pending replacement accidentally. Measurement case M5-A5H-024. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-025: Ordinary Hot-Swap loads on Enter/double-click. An Options.txt switch enables replacement on selection; leaving browser focus, Esc or Q exits Hot-Swap. The text does not promise rollback on exit. | Live 12 local manual, §23.2.4 pp.453–455; exact revision unconfirmed; installed 12.4 | ADR-0110: exit is not assumed to mean cancel/restore; distinguish preview audition from committed replacement and coalesced browsing. Measurement case M5-A5H-025. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-026: Save Preset retains custom info text; Esc cancels naming. A device can be dragged to a chosen Places folder for project-specific storage. | Live 12 local manual, §23.2.5 pp.455–456; exact revision unconfirmed; installed 12.4 | ADR-0127/FEATURES §9: preset metadata and dependencies belong in the saved artifact; naming/cancel and path ownership need tests. Measurement case M5-A5H-026. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-027: Plug-in default configuration saves which parameters appear in Live’s panel, not their values; VST and AU variants can have separate configurations. Track defaults instead may contain devices with parameter settings. | Live 12 local manual, §23.2.6 pp.456–457; exact revision unconfirmed; installed 12.4 | ADR-0150: panel layout, device sound state and track template are different payloads; do not treat Default as one universal operation. Measurement case M5-A5H-027. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-028: Project Defaults folders override matching User Library defaults for Sets in that project. Context-menu Save as Default writes to User Library, not the project-specific folder. | Live 12 local manual, §23.2.6 p.458; exact revision unconfirmed; installed 12.4 | FEATURES §6/10: hierarchical defaults need scope and provenance indicators; portable project defaults must not silently rewrite workstation preferences. Measurement case M5-A5H-028. | Supported hypothesis — documented; installed behavior unmeasured |

### Browser, media, defaults, device comparison and hosting — Plug-in panel, windows and scanning

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5H-029: Plug-ins with at most 64 modifiable parameters receive a complete panel; larger ones start empty. Native editor windows are independent of folding the host panel. | Live 12 local manual, §23.3.1 p.460; exact revision unconfirmed; installed 12.4 | ADR-0150/parity-7.4: already-decided rule remains reference-backed; capability count and configured panel are separate from editor availability. Measurement case M5-A5H-029. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-030: Auto-Open, Multiple Windows and Auto-Hide are independent. Ctrl/Cmd-opening preserves prior editors even with Multiple Windows disabled; global hide/show operates on previously opened editors. | Live 12 local manual, §23.3.1.1 p.461; exact revision unconfirmed; installed 12.4 | ADR-0076/parity-7.5: track selection, editor existence and visibility are distinct. Retain stable ownership when devices/tracks move. Measurement case M5-A5H-030. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-031: Configure adds parameters by touch/change in the native editor; undeclared parameters cannot be added. Removing a configured parameter warns if automation, clip envelopes or mappings depend on it. | Live 12 local manual, §23.3.1.2 p.462; exact revision unconfirmed; installed 12.4 | ADR-0150/parity-7.4: deletion needs dependency inspection and an explicit consequence; exact warning and rollback behavior remain unknown. Measurement case M5-A5H-031. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-032: Panel configuration is per instance and saved in the Set. For plugins without a native window, Configure permits reordering but forbids parameter deletion. | Live 12 local manual, §23.3.1.2 p.462; exact revision unconfirmed; installed 12.4 | ADR-0150: editable panel capability depends on editor fallback, while target identity must survive reorder. Measurement case M5-A5H-032. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-033: An editor touch creates temporary parameter entries in automation/envelope/X-Y choosers; editing/selecting the entry can make it permanent. Recorded automation adds parameters when recording stops; mapping mode adds and selects them immediately. | Live 12 local manual, §23.3.1.2 p.463; exact revision unconfirmed; installed 12.4 | ADR-0150/0181: temporary observation, persistent panel membership and gesture recording have different commit boundaries. Measurement case M5-A5H-033. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-034: Alt/Option during launch can skip scanning; manual Rescan refreshes changes, while Alt/Option-Rescan deletes the plugin database and performs a clean scan. | Live 12 local manual, §23.3 pp.458–460; exact revision unconfirmed; installed 12.4 | FEATURES §6: cache rebuild and runtime instance state must be separate; ADI’s scanner isolation decision is unchanged. Measurement case M5-A5H-034. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-035: The guide describes diagnosing a scan-crashing plug-in on restart, optional retry, and making it unavailable after a second crash. It does not establish the exact installed scanner process model. | Live 12 local manual, §23.4.1 pp.466–467; exact revision unconfirmed; installed 12.4 | ADR-0203 d8: capture quarantine identity and recovery rules independently of in-process runtime hosting. Measurement case M5-A5H-035. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-036: Plugin sidechain controls select internal routing points, scale the trigger and mix it with the original trigger; sidechain gain does not raise the source’s audible mix level. | Live 12 local manual, §23.3.2 pp.463–464; exact revision unconfirmed; installed 12.4 | FEATURES §6/N23: separate detector input graph, source gain and program audio. The sidechain Mute description needs an exact tap observation. Measurement case M5-A5H-036. | Supported hypothesis — documented; installed behavior unmeasured |

### Browser, media, defaults, device comparison and hosting — Max integration and portable state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5H-037: Changing the external Max application path requires restarting Live; the configured editor runtime is distinct from Max device instances in a Set. | Live 12 local manual, §31.1 p.795; exact revision unconfirmed; installed 12.4 | FEATURES §6/9: editor-tool version and embedded/runtime device identity need distinct capability records and migration behavior. Measurement case M5-A5H-037. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-038: Saving an edited Max device updates all its instances; Save As asks whether to apply to the edited preset or all instances. AMXD files remain separate dependencies and moving/renaming them can break links. | Live 12 local manual, §31.3 p.798; exact revision unconfirmed; installed 12.4 | ADR-0127: shared definition versus per-instance state is a separate ownership layer; invalidation must enumerate all dependent instances. Measurement case M5-A5H-038. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-039: Max MIDI Tools are discoverable in default folders or indexed Places. Files outside those locations do not appear in Clip View’s transformation/generator menus. | Live 12 local manual, §31.4 pp.799–800; exact revision unconfirmed; installed 12.4 | FEATURES §4/9: filesystem presence is not discoverability; menus require index/capability state and refresh policy. Measurement case M5-A5H-039. | Supported hypothesis — documented; installed behavior unmeasured |
| A5H-040: Freezing a Max device packages its external dependencies, including samples, pictures and other patches; this is distinct from Freeze Track. | Live 12 local manual, §31.5 p.800; exact revision unconfirmed; installed 12.4 | ADR-0127/FEATURES §6: dependency closure and render caching need separate operations and manifests. Measurement case M5-A5H-040. | Supported hypothesis — documented; installed behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Two-window topology is constrained

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-001: The second window can be used even on one display. Browser and Info View remain in the main window. Clip and Device Views cannot each appear in both windows simultaneously; Shift+Tab swaps them. | [Ableton Dual Monitor Support](https://help.ableton.com/hc/en-us/articles/209071749-Dual-Monitor-Support), retrieved 2026-11-04 | ADR-0047/0112: distinguish a second coordinated window from arbitrary per-panel detachment. ADI multi-window choices must be explicit deviations where broader than Live. Measurement case M5-A5W-001. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Window-local commands and close behavior

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-002: Show/Hide Detail View targets the focused window. Close Window is unavailable from the main window but closes the focused second window. The article describes Ctrl/Cmd+W as inactive with only the main window open. | [Ableton Dual Monitor Support](https://help.ableton.com/hc/en-us/articles/209071749-Dual-Monitor-Support), retrieved 2026-11-04 | Route commands through explicit window/focus ownership, separate from project close. Test unsaved changes and independent window lifecycle before assigning ADI defaults. Measurement case M5-A5W-002. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — View switching does not change playback ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-003: With two windows, Tab swaps Session and Arrangement between windows when Tab is not assigned to focus navigation. Switching views during playback or recording changes UI only, not the clips currently playing. | Live 12 local manual §3.6 p.39; §41.1 p.984 | Window selection is not a transport or clip-launch operation. Preserve playing-source identity while moving views; keep focus navigation policy independent. Measurement case M5-A5W-003. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Display preferences and per-window scale

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-004: Display & Input includes separate zoom amounts for main and second windows, focus outline, scrollbar display, follow behavior, UI labels, pen mode and persistent scrub areas. | Live 12 local manual §2.3.1 p.30 | Keep window scale/layout, input behavior and musical state separate. Defaults and monitor-DPI transitions remain measured questions under window parity. Measurement case M5-A5W-004. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Theme and automatic color assignment are independent

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-005: Theme settings include OS light/dark following, palette tone and high contrast; customization includes grid opacity, brightness, color intensity/hue. Track auto-color/default color and new-clip random-versus-track color are separate policies. | Live 12 local manual §2.3.2 p.30 | ADR-0211: distinguish semantic color identity from theme-rendered RGB and assignment policy. This does not close the 70-color measurement question. Measurement case M5-A5W-005. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Suppressed warning state is recoverable

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-006: Application Options can restore warning dialogs previously suppressed with Don’t Show Again. | Live 12 local manual §2.3.1 p.30 | Preference persistence includes confirmation suppression; reset must have a defined scope independent of project content and general settings reset. Measurement case M5-A5W-006. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Object Info Text is authored Set data

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-007: Tracks, clips, devices and other eligible objects can carry user-entered Info Text saved with the Live Set; the same Info View also displays hover help. | Live 12 local manual §2.2.2 pp.28–29 | Keep authored object annotations separate from localized application help. ADI remarks/import must retain text as data and never execute embedded instructions. Measurement case M5-A5W-007. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Status Bar has contextual and asynchronous roles

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-008: Status Bar content includes errors, update information when automatic updates are enabled, MIDI-note location/pitch/velocity/probability and precise insert-marker positions. Background similarity analysis exposes progress and a pause action there. | Live 12 local manual §3.2 pp.34–35; §3.4 p.37 | Define status-message precedence and accessibility separately from the exact wording corpus. Hover/selection information must coexist with long-running job progress and failures. Measurement case M5-A5W-008. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Context menus can change global preferences

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-009: Some context-menu entries are application settings rather than settings of the selected item. Context-only commands also exist, and keyboard context-menu invocation is supported. | Live 12 local manual §41.29 p.1006 | Command placement does not determine ownership. ADI should make global-versus-object scope reviewable and apply the same operation validation from menu and keyboard paths. Measurement case M5-A5W-009. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Momentary keyboard latching has a time threshold

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-010: Holding supported shortcuts for about 500 ms temporarily switches state and restores it on release. Listed cases include automation, draw mode, solo, zoom-to-selection, first-eight track activators and Session/Arrangement switching; Options.txt can disable latching. | Live 12 local manual §41.25 p.1004 | Model press, threshold, release, lost-focus and repeat events explicitly. This is a different interaction from ordinary toggles, not a universal feature of every shortcut. Measurement case M5-A5W-010. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Numeric entry is a provisional edit

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-011: Controls support direct number entry, Enter confirmation, Esc cancellation, Delete reset, Shift-drag finer resolution and period/comma navigation across bar/beat/sixteenth fields. | Live 12 local manual §41.6 pp.988–989 | Separate text buffer, parsed candidate and committed parameter value; time-field navigation and locale punctuation need explicit rules. Measurement case M5-A5W-011. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Key Map is a control-assignment mode

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-012: Key Map exposes eligible controls with red overlays and a Mapping Browser. Mapped clip slots obey launch mode, switches toggle, and radio groups cycle. This is distinct from the Computer MIDI Keyboard. | Live 12 local manual §33.2.5 p.844; §41.24 pp.1003–1004 | Do not equate control mappings with a complete command-remapping system. Map eligible target identity, event behavior, conflicts and saved scope explicitly. Measurement case M5-A5W-012. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Mappings to Clip View follow the selected content

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-013: Remote mappings to Clip View controls can affect whichever clip or multiple clips are currently selected; they are not necessarily bound to the clip present when mapping was created. | Live 12 local manual §33.2.4.2 pp.843–844 | Distinguish selection-relative UI targets from fixed object targets. ADI controllers must expose target scope and avoid accidental jumps between unlike clips. Measurement case M5-A5W-013. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Relative Session navigation is an explicit mapping surface

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-014: Map modes expose scene up/down, scene-number navigation, launch highlighted scene, cancel pending scene launch and per-track launch at the highlighted scene. Select Next Scene on Launch changes progression; the highlighted scene is kept centered. | Live 12 local manual §33.2.4.1 p.843 | Represent scene selection, pending launch, launched scene and viewport independently; map-driven navigation must use stable slot/scene identities. Measurement case M5-A5W-014. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Computer MIDI Keyboard changes shortcut resolution

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-015: With Computer MIDI Keyboard active, ordinary single-letter shortcuts remain reachable by adding Shift; octave and velocity range changes use their own keys. | Live 12 local manual §41.24 pp.1003–1004 | Keyboard event routing requires explicit modes, modifier priority and text-focus escape. Do not broadcast one keystroke as both musical input and UI command. Measurement case M5-A5W-015. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Keyboard breakpoint editing has commit/cancel state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-016: Inserting/selecting breakpoints uses Enter; keyboard navigation can move the candidate, Enter commits/deselects, and Esc cancels/deselects. Current-position values and focused/selected/undo-affected points have different visual feedback. | Live 12 local manual §40.5 pp.981–983 | ADR-0110: editing preview, selection and committed envelope state must be distinct. Accessibility feedback should identify affected points without introducing extra undo operations. Measurement case M5-A5W-016. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Envelope navigation distinguishes existing and possible targets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-017: Alt/Option plus vertical arrows cycles device parameters with automation; adding Shift includes parameters without envelopes. | Live 12 local manual §40.5.3 p.983 | Keep parameter registry, existing-envelope filter and visible lane selection separate. Navigating to an unused target should not imply creating automation data. Measurement case M5-A5W-017. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

### Keyboard, settings and multiple-window behavior — Manual shortcut conflicts require installed verification

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A5W-018: The accessibility chapter gives Ctrl/Cmd+R for audio export, while the shortcut tables give Ctrl/Cmd+Shift+R for export and Ctrl/Cmd+R for Rename. Info View is described as ? in one section and Shift+? in the shortcut table. | Live 12 local manual p.978 versus pp.986,988; p.28 versus p.984 | Do not copy inconsistent prose into the ADI shortcut catalogue. Distinguish physical key, produced character, layout and modifier notation, then retain observed build-specific bindings. Measurement case M5-A5W-018. | Unknown / not tested — inconsistent manual shortcut descriptions |

## Live base reference — detailed host contracts — 2026-11-04

This pass adds 445 detailed contracts and matching unexecuted cases. Read them as documentary claims, narrowly bounded static observations, or explicit unknowns. Private E37–E42 contain sources, review notes, source hashes, the section-linkage audit and LIVE-BASE-MISSIONS-6.md. No new DAW GUI, audio or CPU measurement was made. The current FEATURES, decisions and parity documents remain the product authority.

Source boundaries: the local manual revision remains unconfirmed against installed Live Suite 12.4/build 2026-04-24_d85a94ab5e. Public API documentation labels its reference 12.4.5; the ASCL specification identifies a 12.1 baseline. Release deltas are explicitly versioned and are not evidence about the installed 12.4 build. The shipped default Set and theme declare older creator versions; they do not establish current factory defaults. Push 1/2 chapter behavior is documentary, and the later Push 2 integration change requires version qualification. No hardware was exercised.

Scope: built-in instrument/effect implementation and operation remain excluded. Typed device hosting, Rack routing, controller navigation, warp controls and editor MIDI transformations remain included. Four historical A-BASE §35.8 entries are now explicitly excluded because they cover Simpler operation; their IDs remain stable. The 610-entry contents inventory includes parents and examples, and is neither a feature count nor a completion claim. Exact section references below improve navigation without silently closing any runtime question.

### MIDI editor, transformations, expression, grooves and tuning

#### Editor lanes and viewport ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-001: The Notes editor initially shows Velocity but not Chance. The lane selector enables lanes, the common toggle hides/shows enabled lanes, Swap Lane replaces the visible lane, and individual lane dividers resize independently. The vertical ruler normally spans C-2–C8, but tuning and Drum Rack contexts change its contents. | Live 12 local manual, §10.1, pp.247–249; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d1/d7; FEATURES §4: represent enabled-lane state separately from overall lane visibility and pitch-row contents. Measurement case M6-A6M-001. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Two-axis selection zoom

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-002: Double-clicking the note ruler fits selected pitches, while double-clicking the time ruler fits selected time. With nothing selected these fit lowest-to-highest pitch and first-to-last note respectively; an offscreen selection is brought back into view. The overview always represents the complete clip. | Live 12 local manual, §10.2, pp.250–251; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 and window parity: maintain separate horizontal and vertical selection-fit operations. Measurement case M6-A6M-002. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Magnetic grid preserves offsets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-003: A moved or resized note first travels freely until the next grid point, then snaps. Notes can also snap to the original offset from the grid. The temporary modifier reverses the current grid state rather than always disabling it. | Live 12 local manual, §10.2.1, §10.5.4, §10.5.5, pp.251,257; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4 / ADR-0203 d7: preserve a gesture baseline and off-grid phase; snapping is not unconditional rounding. Measurement case M6-A6M-003. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### MIDI follow, scrub and chase are different states

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-004: Editing pauses Follow. Follow resumes via its switch, stopping/restarting playback, or stopping then clicking a clip/Arrangement scrub area. Scrub starts are rounded by global quantization; Chase MIDI Notes permits sounding a note whose start lies before playback begins. | Live 12 local manual, §10.2.2, pp.251–252; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7; MIDI playback/transport: distinguish viewport following, launch position quantization and chase reconstruction. Measurement case M6-A6M-004. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Empty MIDI clip creation has context-dependent targets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-005: Empty clips can be inserted from a selected Session slot or Arrangement time selection via Insert Empty MIDI Clip(s); double-clicking an empty MIDI slot or MIDI Arrangement track display also creates a clip. Chapter 10 does not state exact default length for every creation path. | Live 12 local manual, §10.3, pp.252; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7; window parity: route one command to focused Session slots or Arrangement time spans. Measurement case M6-A6M-005. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Drawing mode preference reverses the modifier

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-006: Draw Mode with Pitch Lock determines whether ordinary drawing stays on one pitch or draws melodies; Alt/Option temporarily selects the other behavior. Melodic drawing over an existing note erases it; pitch-locked drawing erases newly drawn notes when retracing toward the first note. Outside Draw Mode double-click deletes a note. | Live 12 local manual, §10.4, §10.4.1, pp.252–253; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7; FEATURES §4: implement the two stroke behaviors and preference-sensitive modifier, with a stable stroke snapshot. Measurement case M6-A6M-006. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Preview is shared Set state and gates step input

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-007: Preview auditions additions, selections and moves when the MIDI chain contains an instrument. Its on/off state applies to all MIDI tracks in the Set. On an armed track, Preview also permits step recording. | Live 12 local manual, §10.4.2, pp.253–254; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 step input P2; FEATURES §4: separate Set-wide audition preference from track arm and target selection. Measurement case M6-A6M-007. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Imported MIDI becomes owned Set content

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-008: Import incorporates MIDI-file contents into the Set: subsequent note edits do not modify the original file. Ordinary Undo restores an earlier clip state, but this chapter does not define transaction size or source-link tracking. | Live 12 local manual, §10.5.1, pp.254; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 MIDI import/export; distinguish imported asset provenance from editable project events. Measurement case M6-A6M-008. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Insert marker and selection determine arrow-key meaning

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-009: An insert marker moves by grid or note boundaries, with Home/End moving to clip limits. Arrow keys collapse/move a time selection, but move or transpose selected notes. Enter exchanges time and contained-note selection; Esc collapses selection. | Live 12 local manual, §10.5.2, pp.254–255; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d1/d7 and window parity: focus plus selection type must choose the command target. Measurement case M6-A6M-009. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Pitch-row and inverse selections are explicit operations

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-010: Shift-clicking a ruler key adds/removes all notes in that pitch row; Shift-click adds/removes individual notes. Keyboard selection can advance by time or within one pitch row. Invert Selection swaps selected and unselected notes without applying pitch inversion. | Live 12 local manual, §10.5.2, pp.255; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4: preserve distinct selection commands and pitch-edit commands; provide accessible traversal. Measurement case M6-A6M-010. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Find filters have temporal and quantized-count semantics

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-011: Time filter uses beat-based Start, Length and Repeat. Count selects every nth note/chord with an Offset; Quantized groups notes within a grid step as one count. Pitch matches in every octave, while Scale can use the clip scale or an independently chosen root/scale. | Live 12 local manual, §10.5.3, pp.256; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4; ADR-0203 d7: selection query data needs repeat windows and grouping units rather than only property ranges. Measurement case M6-A6M-011. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Condition filters identify stored metadata

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-012: Condition can select active notes, notes whose Chance is below 100%, or notes with Velocity Deviation. Multiple filters apply as parameters change; the manual does not formalize Boolean combination or grouped-probability interpretation. Shift-drag can create evenly spaced repeated time selections while Find is active. | Live 12 local manual, §10.5.3, pp.256–257; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4: build typed queries for activity/probability/deviation and leave unproven combination rules open. Measurement case M6-A6M-012. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Copy can be chosen after starting a drag

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-013: Clipboard notes paste at the insert marker. Ctrl/Option-drag copies notes, and the copy modifier may be pressed after a move drag has already begun. | Live 12 local manual, §10.5.4, pp.257; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7; gesture system: resolve move/copy mode during a live gesture without losing its original source snapshot. Measurement case M6-A6M-013. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Fit to Time Range aligns both ends

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-014: Fit to Time Range changes selected note durations so their start and end match the current time or note selection. It is exposed as a menu command and a Set Length duration option. | Live 12 local manual, §10.5.5, §10.5.9.6, pp.258; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4: this can move note starts as well as alter duration; do not model it as only an end-edge edit. Measurement case M6-A6M-014. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Interior note stretch is locally anchored

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-015: The pseudo stretch marker between selection-boundary markers changes the material within the fixed boundaries while leaving outside material alone. Its snapping matches the fixed markers. | Live 12 local manual, §10.5.6, pp.259; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7; FEATURES §4: interior stretch requires a piecewise time mapping with fixed endpoints. Measurement case M6-A6M-015. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Inactive notes remain editable data

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-016: Deactivate Note(s) toggles selected notes gray and prevents playback; the same command reactivates them. This is a note state, not deletion. | Live 12 local manual, §10.5.7, pp.259; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4: preserve note IDs, expression and group membership when muting notes. Measurement case M6-A6M-016. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Split and Chop share a selection-sensitive shortcut

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-017: Without note selection, Split is available only when a note intersects the insert marker or extends outside the time selection. With notes selected, Ctrl/Cmd+E chops on grid. Holding the shortcut modifier permits adjusting the part count; Shift changes counts in powers of two. | Live 12 local manual, §10.5.8.1, §10.5.8.2, §10.5.8.3, pp.260–262; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7; ADR-0054: command eligibility and complete note/expression payloads matter, beyond generic splitting/joining. Measurement case M6-A6M-017. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Scale-fitting ties round downward

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-018: Fit to Scale moves pitches to the nearest degree of the active clip scale; equally distant choices use the lower degree. The button is unavailable without an active clip scale. Transpose uses degrees with a scale and semitones otherwise. | Live 12 local manual, §10.5.9.1, §10.5.9.2, pp.263–264; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178 / ADR-0203 d7: define exact nearest-degree tie behavior for Live parity; ADI custom tuning needs its own degree-distance decision. Measurement case M6-A6M-018. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Pitch inversion uses scale space

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-019: Invert reflects the highest and lowest pitches and the pitches between them; with an active scale, inverse positions are calculated in scale degrees. This differs from Invert Selection. | Live 12 local manual, §10.5.9.3, pp.264–265; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7; ADR-0178: keep pitch-coordinate mapping separate from selection inversion. Measurement case M6-A6M-019. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Interval slider can create notes immediately

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-020: With notes selected, changing Interval Size immediately adds and selects notes at that interval. With no selection it only sets the interval; Add Interval then applies to every existing note. Interval units are semitones or scale degrees. | Live 12 local manual, §10.5.9.4, pp.265–266; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4 / ADR-0203 d7: distinguish previewing a selected edit from setting an action parameter. Measurement case M6-A6M-020. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Stretch Factor and doubling differ on loop bounds

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-021: Stretch Factor changes note length but does not change the loop-region length. The ×2 and /2 controls can double/halve note duration, the time selection, or loop region. Set Length applies its chosen duration to all notes if none are selected. | Live 12 local manual, §10.5.9.5, §10.5.9.6, pp.266–268; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: separate content stretch from loop-boundary resize and context-sensitive duration operations. Measurement case M6-A6M-021. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Humanize range wording leaves total span versus displacement ambiguous

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-022: The detailed editor section bounds start displacement at one quarter-grid before or after the original. The Clip View summary instead says up to half a grid division without direction. These could describe the same total span, but that interpretation is not explicit; no RNG seed, distribution or repeated-application baseline is specified. | Live 12 local manual, §10.5.9.7, §8.5.2, pp.219,268–269; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 expressly adopts Humanize; resolve the bound through measurement and record realized event values so redo is stable. Measurement case M6-A6M-022. | Unknown / not tested — documentary range wording ambiguous; no installed measurement |

#### Reverse and Legato choose different endpoints

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-023: Reverse exchanges positions in a selected note sequence, or reverses the whole clip with no selection. Legato extends or shortens each selected note to the next note start, with the final note extending to loop end. | Live 12 local manual, §10.5.9.8, §10.5.9.9, pp.269–271; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: do not conflate reversal with boundary crossing or Legato with a uniform duration. Measurement case M6-A6M-023. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Quantize shortcut documentation conflicts

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-024: The editor chapter states Ctrl/Cmd+U applies the stored Quantize Tool settings and Ctrl/Cmd+Shift+U opens those settings. The Quantize Tool section instead describes Ctrl/Cmd+Shift+U as applying quantization without opening the panel. This contradiction cannot establish installed shortcut behavior. | Live 12 local manual, §10.5.11, §11.2.7, pp.271–272,305; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d1/d7; add a direct keyboard measurement before adopting either wording. Measurement case M6-A6M-024. | Unknown / not tested — source contradiction; no installed measurement |

#### Velocity editing has relative and equalizing paths

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-025: Selected velocity markers can be edited numerically or together. Dragging them to a common extreme and back gives equal values. Alt/Cmd vertical dragging a selected note edits velocity without opening its lane. The Randomization Amount range is ±the specified amount, and typing Enter triggers randomization. | Live 12 local manual, §10.5.12, pp.272–274; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 Randomize parity: distinct absolute entry, relative batch dragging and materialized randomness need explicit gesture rules. Measurement case M6-A6M-025. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Velocity ramps and deviations have different coordinates

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-026: Ramp Start/End set velocities for first/last selected notes, with other notes distributed evenly between them. Velocity Deviation is clipped per note to available range, can show mixed range values, and double-clicking a deviated marker resets deviation to zero. | Live 12 local manual, §10.5.12, pp.275; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4: keep stored velocity, ramp result and runtime deviation as independent values. Measurement case M6-A6M-026. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Velocity drawing is selection-gated within grid cells

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-027: Drawing velocity affects notes in the grid division, but an existing note selection restricts it to selected notes only. With no note selection all notes in the division are eligible. A modifier permits drawing individual markers; straight and horizontal line gestures are also available. | Live 12 local manual, §10.5.12.1, pp.276; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: velocity stroke targeting must intersect selection and temporal hit testing. Measurement case M6-A6M-027. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Release velocity is a separate editable lane

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-028: Note Off velocity is exposed in Release Velocity Editor through the lane selector; its audibility depends on receiver support. It is not the same value as ordinary note-on velocity. | Live 12 local manual, §10.5.12.2, pp.276–277; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054 / FEATURES §4: preserve release velocity through note copies, transformations and protocol adapters even if a receiver ignores it. Measurement case M6-A6M-028. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Chance editing and randomization commit differently with selection

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-029: Chance spans 0–100%; marker selection supports numerical values and ±10% keyboard changes with finer Shift adjustment. Adjusting Randomization Amount immediately changes selected notes; with no selection it only takes effect after Enter or Randomize, then affects all notes. | Live 12 local manual, §10.5.13, pp.277–278; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4; model Chance values separately from playback draws, and distinguish parameter staging from editing note data. Measurement case M6-A6M-029. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Probability groups expose shared markers and conditional indicators

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-030: A group has one shared Chance marker: diamond for Play All, triangle for Play One. Group notes retain the corner probability indicator even at 100%, whereas ordinary notes show it only below 100% and at sufficient row height. Hover highlights group members; mixed selected group types are marked with an asterisk in status. | Live 12 local manual, §10.5.13.1, pp.278–279; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4: grouping is durable relational data, with UI distinct from independent note probabilities. Measurement case M6-A6M-030. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Fold and scale-highlight switches are global, scale values are clip data

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-031: Folding options apply across MIDI clips in the Set, while each clip supplies its notes/scale. Highlight Scales is global. With Fold off, Drum Rack rows show populated pads; with Fold on only rows containing notes remain. | Live 12 local manual, §10.6, pp.279–281; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 / ADR-0178: separate view preferences from clip scale and instrument-derived row availability. Measurement case M6-A6M-031. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### New clips inherit contextual scale and spelling differs in/out of scale

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-032: A foreground clip among mixed-scale clips influences newly created clip scale. If Scale Mode is off, new clips inherit the most recently selected scale but keep it inactive. Sharp/flat spelling preference applies only out of scale when Scale Mode is on; Auto derives spelling from the root circle of fifths; MIDI-number display is available. | Live 12 local manual, §10.6, pp.281–282; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 note naming and ADR-0178: distinguish inherited scale configuration, enabled state and display spelling. Measurement case M6-A6M-032. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### MIDI crop summary must retain conditional marker rules

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-033: Chapter 10 summarizes cropping around the loop or time selection and confirms no new disk file. Chapter 8 gives the fuller MIDI boundary rule: unlooped start/end; looped earlier of start/loop-start through loop-end; explicit time selection can override. Expression handling for crossing notes is not specified. | Live 12 local manual, §10.7.1, §8.11, pp.226,282; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: implement owned-event cropping using conditional clip markers; preserve the distinction from audio media-file creation. Measurement case M6-A6M-033. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### MIDI time insertion and loop duplication differ

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-034: Insert Time adds the selected duration before the selection; Delete Time closes it; Duplicate Time inserts a copy. These leave clip/loop bounds fixed. Duplicating a selected loop brace instead doubles the loop, duplicates its notes, moves later notes by the added duration and zooms out if needed. MIDI clips loop by default. | Live 12 local manual, §10.7.2, §10.7.3, pp.283–284; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: use separate content-time and loop-extension operations, with explicit later-content movement. Measurement case M6-A6M-034. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Multiclip loop editing and shared properties have separate scopes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-035: Loop bars can be selected and resized together using modifiers, and duplicated. Focus Mode prevents selecting multiple bars and ignores an existing bar multiselection. Loop, time signature, groove and scale properties can otherwise be edited for selected clips; an unnamed foreground clip displays its track name. | Live 12 local manual, §10.8, §10.8.1, pp.284–286; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0047 / ADR-0203 d7: target sets for bars, clip properties and per-note edits must not be conflated. Measurement case M6-A6M-035. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Multiclip clipboard remembers destinations only while context is stable

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-036: Without Focus, copying/cutting notes from multiple clips can paste back into the same clip set only while clip selection and foreground clip remain unchanged; selecting another clip instead pastes into that new clip. Time/note editing can cross loop and clip boundaries. Mixed scales display asterisks. | Live 12 local manual, §10.8.1, pp.286–287; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0047: clipboard payload needs per-clip provenance plus an explicit destination-resolution rule. Measurement case M6-A6M-036. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Session multiclip limits and repeated timeline differ from Arrangement

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-037: Session shows at most eight MIDI clips and requires them all to loop; bars order by track then scene. Different loop lengths repeat until they realign; pre-loop starts shift the depicted loop. Arrangement selects clips from up to eight tracks over time, orders bars by track/time and fits first-clip start through last-clip end. | Live 12 local manual, §10.8.2, §10.8.3, pp.287; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0047 / ADR-0203 d7: keep Session repeating-phase coordinates separate from Arrangement absolute coordinates. Measurement case M6-A6M-037. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Arrangement multiclip drawing can cross clip boundaries

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-038: With Focus disabled, notes can be drawn continuously across Arrangement clip boundaries. A mixed looped/unlooped selection shows a half-colored Loop button. The chapter does not establish ownership of a note spanning two clips or a gap. | Live 12 local manual, §10.8.3, pp.287; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0047 / FEATURES §4: cross-clip drawing requires an explicit destination and fragmentation policy. Measurement case M6-A6M-038. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### MIDI Tool auto-apply default and extension hosting

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-039: Auto Apply starts active for every MIDI Tool. Native tools are fixed; Max tools are AMXD files hosted in Transform/Generate panels. Third-party tools are discovered in the corresponding User Library MIDI Tools folders or any Places folder; authoring/third-party eligibility depends on Live edition and Max add-on. | Live 12 local manual, §11, §11.1, §11.1.1, pp.288–291; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 MIDI Tools P2; record native/custom tool identity independently of committed note results; do not infer bundled DSP implementation scope. Measurement case M6-A6M-039. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Arpeggiate materializes style, transposition and gate

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-040: The editor Arpeggiate tool offers 18 style patterns; Distance supplies transposition in semitones or scale degrees and Steps controls repeated transposed steps. Rate controls spacing and note length; Gate below/above 100% shortens/lengthens notes relative to that rate. | Live 12 local manual, §11.2.1, pp.292–293; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: store the resulting events as an edit; a playback arpeggiator is a different feature. Measurement case M6-A6M-040. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Chop pattern length differs from output part count

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-041: Chop outputs 2–64 parts, but its programmed pattern has at most 16 parts and repeats for larger counts. Positive Gaps inserts a gap after that many notes; negative Gaps gives that many gaps after each note. Manual pattern edits make Gaps display a dash, and changing Gaps overwrites those edits. | Live 12 local manual, §11.2.2, pp.293–295; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: persist explicit pattern and scalar-generated pattern as distinguishable states. Measurement case M6-A6M-041. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Chop emphasis changes relative chunk duration

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-042: Emphasis can select notes or gaps. Stretch Chunk(s) lengthens emphasized chunks by 2–8 times and disables their corresponding Pattern toggles; Variation randomizes note starts and ends. | Live 12 local manual, §11.2.2, pp.294–295; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §4 MIDI Tools: model duration allocation, silence and stochastic displacement separately. Measurement case M6-A6M-042. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Connect density and tie are gap-based

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-043: Connect fills gaps between original notes with randomized notes. Spread bounds pitch displacement relative to originals, Density fills a percentage of each interval (100% fills gaps), Rate sets generated note duration and Tie is the probability of extending a generated note to the next original. | Live 12 local manual, §11.2.3, pp.295–297; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: preserve original/generated event provenance and distinguish density from per-note playback Chance. Measurement case M6-A6M-043. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Glissando requires successive selected notes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-044: At least two selected notes are required. Start positions the bend onset as a percentage of note length, Curve shapes it, and the display supports dragging the onset breakpoint or curve. A pitch lane can expose bends while notes are folded. | Live 12 local manual, §11.2.4, pp.297–298; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054 / ADR-0203 d7: generated pitch expression belongs to notes and needs a clear relation between successive voices. Measurement case M6-A6M-044. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### LFO tool has coupled envelope and reseed controls

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-045: LFO writes pitch-bend, slide or pressure expression. Attack/Decay use percentages of oscillation length and constrain each other (Attack 100% resets Decay to 0%). Shapes are Sine, Square, Triangle and Random; Random exposes Reseed. Rate spans 1 to 1/128, with 1 representing four beats. | Live 12 local manual, §11.2.5, pp.299–300; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054 / ADR-0203 d7: materialize note-relative curves and retain tool draft independently; no oscillator implementation is reconstructed here. Measurement case M6-A6M-045. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### LFO amplitude ranges depend on target and clip bend range

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-046: Pitch Amount spans twice the clip bend range in either direction, while Amplitude Base spans the clip range itself (±120 st and ±60 st for a ±60 st clip). Slide/Pressure Amount and Base span −127 to +127. Positive Time Shift delays the start; negative shifts phase. | Live 12 local manual, §11.2.5, pp.300–301; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054: distinguish a generated curve amplitude from legal output-range clipping and preserve expression precision. Measurement case M6-A6M-046. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Flam placement is grid-relative and can prepend

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-047: Positive Flam Position replaces the original note beginning; negative values prepend. Magnitude 100% occupies one current grid division regardless of original note duration. Flam velocity is relative to the original note. | Live 12 local manual, §11.2.6, pp.301–303; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: retain grid used for a committed edit and define negative-time/pre-roll behavior. Measurement case M6-A6M-047. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Grace notes alternate relative pitch and retain relative dynamics

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-048: Grace notes have equal duration, fill a grid-relative window before or within the original start, and use High/Low alternating semitone or scale-degree displacement or Same pitch. Amount controls count; Velocity and Chance are relative to the original note values. | Live 12 local manual, §11.2.6, pp.303–304; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 / ADR-0054: separate ornament generation chance from runtime chance and keep group metadata intentional. Measurement case M6-A6M-048. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Quantize end editing changes duration independently

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-049: Quantize can use the visible grid or an explicit subdivision including triplets, target starts, ends or both, and apply a partial Amount. End quantization stretches the note to the chosen subdivision. | Live 12 local manual, §11.2.7, pp.304–305; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: quantization settings must be shared by the command/tool but preserve independent start/end switches. Measurement case M6-A6M-049. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Recombine rotation domain depends on dimension

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-050: Recombine permutes one of Position/Pitch/Duration/Velocity. Ordinary rotation offers one fewer step than selected notes; positive steps rotate clockwise and negative counterclockwise. Position additionally offers Rotate on Grid, deriving steps from grid cells rather than note count. Shuffle regenerates a permutation on every Apply. | Live 12 local manual, §11.2.8, pp.305–308; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: permutation domain and event order must be explicit; keep untouched dimensions and stable redo. Measurement case M6-A6M-050. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Span articulations use distinct duration formulas

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-051: Legato extends to next selected note start, then time-selection end or loop end. Tenuto preserves input duration before modifiers. Staccato sets all lengths to half the smallest interval between selected note starts. | Live 12 local manual, §11.2.9, pp.308–310; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: do not implement Span as three arbitrary duration presets. Measurement case M6-A6M-051. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Span Offset and Variation use opposite sign directions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-052: Offset adjusts note end by up to one grid step: positive extends and negative shortens. Variation does the reverse direction: positive shortens and negative extends. A nonzero Variation produces new lengths each reapplication. | Live 12 local manual, §11.2.9, pp.309–310; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: preserve documented sign semantics and record realized random lengths. Measurement case M6-A6M-052. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Strum endpoints are grid-scaled while tension controls spacing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-053: Strum Low/High offset the lowest/highest chord starts by up to one grid step at ±100%; other notes follow the curve. At zero tension spacing is even; positive tension makes intervals large early then smaller, negative does the converse. At least one endpoint offset must be nonzero to have an effect. | Live 12 local manual, §11.2.10, pp.310–312; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: separate pitch ordering, endpoint offsets and curve shape. Measurement case M6-A6M-053. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Time Warp speed curve has a selection coordinate system

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-054: Time Warp enables one to three breakpoints, with time/speed controls reflecting the selected breakpoint. The speed curve spans the time selection; optional Quantize uses the grid, Preserve Time Range fits results to the original range, and Include Note End controls duration changes. | Live 12 local manual, §11.2.11, pp.312–314; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 / ADR-0054: define source-time to result-time mapping and which events/expressions are transformed. Measurement case M6-A6M-054. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Velocity Shaper repeats and offsets an envelope over selection

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-055: Velocity Shaper permits adding/dragging envelope breakpoints, bounds output with Minimum/Maximum Velocity, repeats the envelope by Loop and offsets it by Rotate steps whose size comes from Division (including Grid). | Live 12 local manual, §11.2.12, pp.314–316; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: this Max editor tool remains in scope as a note-value transformation. Measurement case M6-A6M-055. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Rhythm generator parameter bounds depend on one another

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-056: Rhythm uses one pitch/pad and up to 16 Steps, repeating its pattern over the selection. Density cannot exceed Steps; Pattern choices depend on both. Step Duration determines repetitions and constrains maximum Steps. Split is the probability of halving a step; Shift moves notes by signed step counts. | Live 12 local manual, §11.3.1, pp.316–318; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: dependent ranges and regeneration must update atomically without invalid intermediate settings. Measurement case M6-A6M-056. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Rhythm accents count note occurrences

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-057: Velocity and Accent values differ; Accent Frequency ranges from zero through Density and counts the accented note itself, so 1 accents every note. Accent Offset shifts the accent placement. To add another pitch, deselect the previously generated notes before changing parameters. | Live 12 local manual, §11.3.1, pp.317–318; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: preserve generated selection identity and distinguish add-another-voice workflow from replacing a preview. Measurement case M6-A6M-057. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Seed ranges collapse and polyphony is a generation constraint

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-058: Pitch-range handles merge when coincident to produce one pitch, and separate again on a range click/edit. Duration spans 1/128 to one whole note; Velocity spans 1–127. Voices limits simultaneous generated notes, while Density expresses population of the pitch range. | Live 12 local manual, §11.3.2, pp.318–319; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: separate generated voice limit, note count and playback probability; commit concrete events for reproducible history. Measurement case M6-A6M-058. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Shape jitter stays inside pitch bounds

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-059: Shape uses a preset or drawn contour between chosen pitch bounds. Rate sets minimum duration; Tie probabilistically extends to the next note. Density fills a percentage of the shape; Jitter 0 follows the contour exactly and increasing it randomizes pitches while staying within the pitch limits. | Live 12 local manual, §11.3.3, pp.319–320; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: define contour sampling and bounded randomness as editor operations. Measurement case M6-A6M-059. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Stacks roots, inversions and rhythm are per-chord

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-060: A clip scale initially sets Chord Root, and active scale limits selectable roots to its notes. Positive inversions cycle chord voicings; negative values cycle the same inversions an octave lower. Duration and Offset use eighths of original chord length; switching selected chord changes the displayed parameters. | Live 12 local manual, §11.3.4, pp.321–322; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7 / ADR-0178: store each chord proposal independently and distinguish its duration unit from grid divisions. Measurement case M6-A6M-060. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Stacks custom bank loading replaces the default bank

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-061: A .stacks JSON bank placed in a Places folder is loaded by double-clicking and replaces the default bank; browser MIDI Tool/Stacks tags locate it. Chord-pad hover exposes additional chord information in Status Bar. The chapter does not supply the bank schema or error behavior. | Live 12 local manual, §11.3.4, pp.321; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: keep reusable tool-bank resources distinct from generated notes; no format compatibility claim without specification. Measurement case M6-A6M-061. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Euclidean voice controls and rotations are independent

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-062: Euclidean generates up to four voices with individual enable, pitch/pad, velocity and rotation. A center randomize control randomizes voice rotations; pitch arrows move all voice pitches/pads together. Output targets the time selection or loop. | Live 12 local manual, §11.3.5, pp.322–324; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: separate voice configuration from generated events and preserve per-voice identity in edit proposals. Measurement case M6-A6M-062. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Euclidean patterns may wrap across selection boundaries

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-063: Steps controls pattern length; a short pattern repeats and may wrap around the selected time. Density controls repetitions within the selection, and notes wrap if the pattern does not fit; Division sets each step duration. These labels differ from Rhythm’s Density meaning. | Live 12 local manual, §11.3.5, pp.323–324; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0203 d7: retain tool-specific parameter semantics and modular time mapping rather than sharing a generic density formula. Measurement case M6-A6M-063. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Expression view has lane state and a meter indication

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-064: The MPE chapter says Slide and Pressure lanes show by default, while Pitch curves overlay notes. The common lane toggle shows all lanes when none are enabled; Alt/Option-click shows all. A MIDI track meter’s lowest dot lights blue for per-note controller changes. | Live 12 local manual, §12.1, pp.326–327; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054 / ADR-0203 d1: separate incoming expression activity, visible note curves and enabled lanes. Measurement case M6-A6M-064. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Expression editing separates curve points from note selection

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-065: Selecting a note or expression dimension activates its overlay and makes that curve’s points/segments editable. Clicking a segment adds a point; clicking an existing point removes it. Edit Value sets an exact value, but a multi-point selection moves relatively; Add Value can create an exact-valued preview point. | Live 12 local manual, §12.2, pp.327–329; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054: keep note, dimension, time-range and breakpoint selection scopes explicit; numerical editing must preserve relative shape. Measurement case M6-A6M-065. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Expression snapping has time and pitch components

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-066: With expression grid enabled, points/segments snap to neighboring breakpoint times and new points near grid lines snap there. Pitch values can snap to semitones with Alt/Cmd when grid is off, with the same modifier bypassing snapping when grid is on. Drawing obeys the pitch behavior too. | Live 12 local manual, §12.2, §12.3, pp.329–331; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054 / ADR-0178: distinguish time snapping, neighboring-point alignment and pitch-coordinate snapping. Measurement case M6-A6M-066. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Expression scaling excludes pitch and differs from equal translation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-067: Dragging above a deselected note’s Slide/Pressure/Velocity/Release envelope scales it proportionally over its duration; this also works for multiple selected notes. Pitch is excluded. Selecting all breakpoints and dragging instead translates them equally in value or time. | Live 12 local manual, §12.2, pp.329–330; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054: give scale and translation different typed operations; preserve high-resolution values without MIDI-transport quantization. Measurement case M6-A6M-067. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Expression curves, zoom and clearing are view-specific

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-068: Alt/Option-drag curves a segment and modified double-click straightens it. In Note Expression view, Zoom to Clip Selection includes pitch-bend excursions. Clear All Envelopes clears expression on selected notes; the chapter states Fold hides pitch envelopes on the notes. | Live 12 local manual, §12.2, pp.329–330; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054 / ADR-0203 d7: expression geometry must affect fit-to-selection and clear operations must not delete note events. Measurement case M6-A6M-068. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Expression drawing changes resolution with the grid

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-069: Pitch, Slide and Pressure accept freehand drawing. With grid active, a stroke creates steps as wide as the visible grid; the temporary grid modifier reverses the state, and Shift permits finer value adjustment. Holding B temporarily toggles Draw Mode. | Live 12 local manual, §12.3, pp.330–331; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054: store curve edits independent of display sampling and define stroke coalescing explicitly. Measurement case M6-A6M-069. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Plug-in MPE capability participates in default configuration

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-070: A plug-in’s enabled/disabled MPE Mode is saved with its device default configuration. Plug-ins with MIDI outputs can also output MPE when enabled. Specific output settings are available through mixer I/O and MPE-enabled plug-in menus. | Live 12 local manual, §12.5, §12.6, §12.6.1, pp.331–333; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054 / FEATURES §4 host adapters: separate capability, default configuration, per-instance settings and emitted expression. Measurement case M6-A6M-070. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### MPE zones reserve global channels; multichannel mode is distinct

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-071: The lower MPE zone reserves channel 1, upper zone channel 16; other assigned channels determine note-channel capacity. A track outputs one zone, so both zones require two tracks. Multi-channel mode instead selects an arbitrary note-channel range. The text limits zones to output. | Live 12 local manual, §12.6.2, pp.334; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054: encode routing and polyphony limits separately from per-note curves, and distinguish generic multitimbral mode from MPE. Measurement case M6-A6M-071. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Slice is time partitioning with a hard slice-count limit

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-072: Slice to New MIDI Track partitions an audio clip by beat division, transients or Warp Markers, without pitch analysis. More than 128 slices is rejected because each slice requires a Rack chain; a smaller time region or coarser division is needed. | Live 12 local manual, §13, §13.1, pp.335–336; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4 and Dream §5 conversion: distinguish slicing references to source audio from extracted MIDI pitches. Measurement case M6-A6M-072. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Slicing can preserve warped timing and creates a complete object graph

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-073: Preserve warped timing chooses the warped timing versus original raw timing. Confirming creates a new MIDI track/clip with one chromatically sequenced note per slice and one source-audio chain per note; the slicing preset determines mapped controls. | Live 12 local manual, §13.1, pp.336–337; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4: plan a transactional conversion producing track, clip, source references and mappings; ADI hybrid-track placement is a deliberate difference, not Live behavior. Measurement case M6-A6M-073. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Slice resequencing changes notes or their mappings

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-074: Sliced MIDI initially ascends chromatically in source order. Editing notes resequences playback, while swapping pads changes note-to-slice mapping. Each slice has its own chain, and several slice chains can be grouped for shared processing. | Live 12 local manual, §13.1.1, §13.1.2, pp.337; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4: keep note sequence, source segment and routing identity independent; processing devices themselves remain outside this study. Measurement case M6-A6M-074. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Harmony and Melody create new pitched tracks with audition sounds

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-075: Convert Harmony extracts polyphonic pitches, while Convert Melody targets monophonic pitches. Both create a new MIDI clip/track with an audition instrument; unlike slicing, their notes trigger a new sound rather than replaying the source audio. | Live 12 local manual, §13.2, §13.3, pp.338; exact manual revision unknown; not a measurement of installed Live 12.4 | Dream §5 / FEATURES §3/4: distinguish extraction mode, output objects and audition configuration; ADI’s hybrid target policy needs an explicit deviation. Measurement case M6-A6M-075. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Drum conversion classifies attacks and transient edits influence conversion

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-076: Convert Drums extracts rhythms and attempts kick/snare/hi-hat classification into a new MIDI track. All Convert commands use source transient markers to divide notes; adding, moving or deleting those markers can change conversion output. | Live 12 local manual, §13.4, §13.5, pp.339; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4: preserve editable detection markers as input provenance and distinguish rhythmic classes from pitched detection. Measurement case M6-A6M-076. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Conversion quality constraints are source-dependent, not accuracy guarantees

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-077: The manual favors clear attacks, isolated instruments and high-quality uncompressed files; swells can be missed and accompaniment can generate unwanted notes. Harmony on monophonic material may deliberately yield additional notes. No accuracy percentage, deterministic guarantee or failure contract is given. | Live 12 local manual, §13.5, pp.339; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4: define a representative evaluation corpus and editable output rather than promising perfect transcription. Measurement case M6-A6M-077. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Groove resources can be loaded before any clip uses them

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-078: Grooves are .agr resources; dragging onto a clip applies them, while double-click loads into Groove Pool without assigning. Unused pool entries have grayed parameters. Hot-swap can audition successive grooves while the clip plays. | Live 12 local manual, §14, §14.1, pp.340–342; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3 groove pool: distinguish loaded resources, clip references, selected groove and preview replacement. Measurement case M6-A6M-078. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Groove Base defines displacement anchors and quantization happens first

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-079: Base measures groove-note offsets from nearest beat subdivisions and transfers corresponding displacement to assigned clips; groove notes exactly on the grid do not move corresponding clip notes. Quantize first moves source notes toward Base grid, then Timing applies the groove. | Live 12 local manual, §14.1.1, pp.342; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4: preserve separate straight-quantize and groove-offset stages; note timing offsets are not ordinary swing percentages. Measurement case M6-A6M-079. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Groove randomization separates simultaneous voices

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-080: Random applies different timing fluctuations to each voice, so initially simultaneous notes can separate from each other as well as from the grid. This differs from moving an entire chord by one random offset. | Live 12 local manual, §14.1.1, §14.3.3, pp.342,345; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4: decide per-note, per-chord and per-loop stochastic scopes explicitly; do not substitute grouped Chance semantics. Measurement case M6-A6M-080. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Groove velocity is bipolar and global amount can exaggerate

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-081: Groove Velocity ranges −100 to +100, with negative values reversing loud/quiet influence. Global Amount scales Timing, Random and Velocity for every groove, up to 130%; the documentation does not include Quantize in that scaling list. The Control Bar shows global amount when grooves are assigned. | Live 12 local manual, §14.1.1, pp.342–343; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3 groove pool: keep global scaling separate from per-groove settings and pre-quantization. Measurement case M6-A6M-081. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Groove editing materializes MIDI; extraction reads only playing content

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-082: Dragging a groove to a MIDI track produces editable MIDI. Extract Groove from MIDI/audio or dragging a clip into the pool creates a groove from timing and volume, considering only the clip’s playing portion. The edited MIDI can then be extracted again. | Live 12 local manual, §14.2, §14.2.1, pp.344; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4: distinguish resource editing, extraction input bounds and explicit replacement of a shared resource. Measurement case M6-A6M-082. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### A groove affects a whole clip; single-voice variation requires separation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-083: The manual’s single-voice workaround extracts that instrument chain to its own track/clip before assigning a separate groove. Setting Timing, Random and Velocity to zero leaves nondestructive Quantize controlled by Base and Quantize regardless of groove-file content. | Live 12 local manual, §14.3, §14.3.1, §14.3.2, pp.345; exact manual revision unknown; not a measurement of installed Live 12.4 | FEATURES §3/4: Live baseline is clip-scoped groove; any ADI per-note groove assignment is an explicit extension. Measurement case M6-A6M-083. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Tuning resources have explicit file and browser presentation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-084: Live defaults to 12TET. Scala .scl and Ableton .ascl files in Places appear under Tunings/User; Info View shows the tuning description and note-count context for hovered/selected files and the loaded tuning. | Live 12 local manual, §15, §15.1, pp.346–348; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178 / ADR-0203 d7: preserve tuning provenance and descriptions while distinguishing Live’s Set scope from ADI’s key-change scope. Measurement case M6-A6M-084. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Tuning output assumes a matching 48-semitone MPE bend range

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-085: The tuning chapter requires MPE-enabled external plug-ins/Max instruments with pitch bend range set to 48 semitones; other ranges or non-MPE targets may sound out of tune. This is an external-receiver contract, not evidence every instrument accepts tunings automatically. | Live 12 local manual, §15.1, pp.348; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0054 / ADR-0178: negotiate or expose target bend range; preserve intended note pitch separately from transport encoding. Measurement case M6-A6M-085. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Retune-on-load can delete or shorten colliding notes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-086: Automatic retuning seeks nearby pitches when tuning is loaded/changed/removed, with a confirmation dialog. If formerly different simultaneous notes map to the same target pitch, one can be shortened or deleted. This is a musical-content edit, not only a display change. | Live 12 local manual, §15.1, pp.350–351; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178 / ADR-0203 d7: design collision preview and atomic history for retuning across key changes; preserve recovery of lost note/expression data. Measurement case M6-A6M-086. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Custom tuning disables ordinary Scale Mode controls

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-087: When a tuning system is loaded, Scale Mode choosers disappear from Clip View and Control Bar and scale-aware devices lose their Use Current Scale option. The piano roll displays tuning notes, and hover reports pitch/frequency. | Live 12 local manual, §15.1, pp.349–351; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178 / ADR-0203 d7: Live separates these systems; ADI’s combined custom tuning/scale degrees is an addition needing an explicit interaction design. Measurement case M6-A6M-087. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Reference note selection avoids an immediate pitch jump

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-088: Changing reference Octave or Note updates the displayed corresponding frequency but causes no audible change; only editing Ref. Pitch/Freq changes actual tuning. That frequency change raises/lowers all notes in the Set. | Live 12 local manual, §15.2, pp.351; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178: distinguish re-labeling the reference anchor from changing the tuning frequency; do not bind every field to immediate transposition. Measurement case M6-A6M-088. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Tuning keyboard range moves as a fixed-width pair

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-089: Changing Lowest Note also changes Highest Note and vice versa, preserving the number of notes between them. The save control writes the current tuning as .ascl; Delete removes the loaded tuning and returns to 12TET. An optional information/editor link is disabled when no webpage exists. | Live 12 local manual, §15.2, pp.351–352; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178: keep keyboard mapping bounds, tuning degrees and saved resource settings distinct; ADI .kbm/reference mapping remains a separate planned issue. Measurement case M6-A6M-089. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Bypass Tuning belongs to a track and changes its editor coordinates

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-090: Bypass Tuning ignores the Set tuning for one MIDI track and restores 12TET pitch rows in its clips. Tracks containing Drum Racks bypass loaded tuning automatically. | Live 12 local manual, §15.3.1, pp.352–353; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178 d5 explicitly defers this mechanism; preserve it as an adoption decision, not an implemented ADI guarantee. Measurement case M6-A6M-090. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Controller layout is track-level input mapping with saved custom state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-091: Tuning controller layouts offer All Keys, Black Keys Only centered on C#3, White Keys Only centered on C3, Closest in Pitch to Keyboard and Custom. Custom opens Configure MIDI Layout, whose layout is saved/recalled with the Set. This is distinct from tuning the output receiver. | Live 12 local manual, §15.3, §15.3.2, pp.352–354; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178 d5 / ADR-0054: separate physical input key mapping, musical note representation and MPE output encoding. Measurement case M6-A6M-091. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact local manual revision unknown |

#### Tuning format and external editor use separate documentation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6M-092: The chapter points to an ASCL specification and an external interactive tuning editor, without reproducing the format or all custom-layout fields. The accompanying midi-format supplement now records the official specification; actual import/export interoperability remains untested. | Live 12 local manual, §15.2, §15.4, pp.352,354; exact manual revision unknown; not a measurement of installed Live 12.4 | ADR-0178: use the reviewed format supplement and private tuning6 diagnostic corpus to distinguish document-supported syntax, ADI representation policy and measured interoperability. Measurement case M6-A6M-092. | Unknown / not tested — documentary format follow-up completed separately; no interoperability measurement |

### Arrangement, clips, warp, launch, automation and envelopes

#### Clip View — selected-object and window ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-001: Clip View follows the selected clip even when selection occurs in the other window. Opening it targets the focused window; F12 moves it between windows. Clicking a Session Track Status Display opens the currently playing clip, which need not be the previously selected clip. | Live 12 local manual, §8, §8.1 pp.195–196; exact revision unconfirmed; installed 12.4 | ADR-0047/0063/0112: selected object, playing object, focused window and editor host are distinct identities; moving the editor must retain one model owner. Measurement case M6-A6C-001. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip View — stacking, closing and panel layout

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-002: Opening with the Clip View Toggle while Device View is open stacks Clip View above it. Dragging its upper boundary to the window bottom closes it. Panels support horizontal, vertical or automatic layout; Live remembers the arrangement, and double-clicking the title bar folds the panels. | Live 12 local manual, §8, §8.1.2 pp.195,199–201; exact revision unconfirmed; installed 12.4 | ADR-0112/window shell: panel visibility, content selection, panel orientation and height must not be encoded as one open/closed flag. Measurement case M6-A6C-002. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip metadata — name versus referenced media

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-003: Renaming an audio clip does not rename its sample file. With multiple clips selected, the title shows a count; bulk renaming changes the selected clips even though their new name is not shown in that multi-selection title. | Live 12 local manual, §8.1.1.2 pp.197–198; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0131: clip labels and media-library names need separate operations and validation; multi-edit feedback must not imply a shared source rename. Measurement case M6-A6C-003. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip defaults — sample defaults are prospective

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-004: Save Default Clip saves audio clip settings into the sample analysis data and applies them to future imports, not already-created clips. Multi-selection saves defaults for all selected samples. The guide permits this for owned samples, not Core Library or Pack content; a Live Clip additionally saves devices and their settings. | Live 12 local manual, §8.1.1.4, §9.2.3.2 pp.198–199,237; exact revision unconfirmed; installed 12.4 | ADR-0203 d7/d12: preserve ADI’s central library analysis location; distinguish project clip state, sample defaults and reusable Live-Clip-like templates. Saving defaults must not retroactively mutate aliases or old instances. Measurement case M6-A6C-004. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip View — type and view gate the launch panel

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-005: Extended Clip Properties includes launch controls for Session clips. Arrangement audio clips have no such panel; Arrangement MIDI clips retain the bank/program controls, because Arrangement playback follows timeline placement rather than clip launch settings. | Live 12 local manual, §8.3, §16.1 pp.207–209,355; exact revision unconfirmed; installed 12.4 | FEATURES §3/5; ADR-0203 d10: editor capability comes from clip content and view role, not a generic always-visible panel; Session remains P2. Measurement case M6-A6C-005. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip region — units and live Set controls

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-006: Unlooped clips play from start to end or until stopped. Audio region fields use bars/beats/sixteenths when warped and minutes/seconds/milliseconds when unwarped. Set Start and Set End capture the current playhead position quantized by global launch quantization. | Live 12 local manual, §8.2.1 pp.203; exact revision unconfirmed; installed 12.4 | FEATURES §3/AWRP: playback region, source time and beat representation are separate; display-domain changes must not silently move the audible region. Measurement case M6-A6C-006. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip loop — live capture state transitions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-007: Set Loop Position moves the loop start to the quantized current position and enables looping. Set Loop Length then moves the loop end to the playhead. With looping still off, Set Loop Length instead places the loop end at the playhead while retaining the preset length, and enables looping. | Live 12 local manual, §8.2.1 pp.203–204; exact revision unconfirmed; installed 12.4 | FEATURES §3/5: these are state-dependent commands, not simple setters; one gesture can change loop-enabled state and region coordinates. Measurement case M6-A6C-007. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip loop — warp prerequisite and introductory material

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-008: Audio clip looping requires Warp; the Loop control is unavailable for unwarped audio. When looping is enabled, playback still begins at the clip start marker, so material before the loop can play once before the loop repeats. | Live 12 local manual, §8.2.1, §8.9 pp.204,223–225; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0203 d7: ADI’s clean Warp-off import is retained. The loop-enabled state, launch start and repeated interval cannot be collapsed into one region. Measurement case M6-A6C-008. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip metadata — meter and audio scale do not retime audio

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-009: Clip time signature is independent of the Set’s meter and affects display, not playback. Setting a scale on an audio clip likewise does not retune its sample; scale settings are forwarded to scale-aware devices in its chain. | Live 12 local manual, §8.2.2, §8.2.4 pp.204–205,206–207; exact revision unconfirmed; installed 12.4 | FEATURES §1/4: clip meter/scale metadata is distinct from tempo, warp and pitch processing; keep ADI’s broader tuning model an explicit addition. Measurement case M6-A6C-009. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Groove assignment — Hot-Swap mutates the pool entry

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-010: Hot-Swap Groove replaces the loaded groove in the Groove Pool as well as the clip. It is not described as merely assigning an unrelated groove to one selected clip. | Live 12 local manual, §8.2.3 pp.205–206; exact revision unconfirmed; installed 12.4 | FEATURES §3 Groove Pool: shared groove identity and per-clip assignment are separate; pool replacement can have dependents that must be visible to undo and migration. Measurement case M6-A6C-010. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Groove commit — audio volume-envelope overwrite

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-011: Committing a groove with positive velocity values to an audio clip creates a volume clip envelope and overwrites any existing volume clip envelope. Commit writes the groove effect and disables the assigned groove. | Live 12 local manual, §8.2.3 pp.206; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0003/0203 d13: groove commit can replace authored envelope content, not only add warp markers; preserve overwritten data in one reversible edit. Measurement case M6-A6C-011. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### MIDI clip properties — bank/program optionality

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-012: Clip bank/program controls send messages when a clip launches. The guide exposes 128 banks, 128 sub-banks and 128 programs; the dash value means no bank/program change message for that chooser. | Live 12 local manual, §8.3.2 pp.208–209; exact revision unconfirmed; installed 12.4 | FEATURES §4/5: an unset message is distinct from numeric zero; represent optional bank/program fields and transport-triggered emission separately from MIDI note data. Measurement case M6-A6C-012. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Reverse audio — new derived media and storage lifecycle

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-013: Reverse creates a new sample from the referenced sample, substitutes it into the clip, and retains the original as a separate source. Saved results live in Samples/Processed/Reverse; before the Set is saved, results use the configured Temporary Folder. | Live 12 local manual, §8.4.2 pp.211–212; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0069/0127: reverse is a derived-media edit with managed dependencies; it must not destructively reverse the shared original. Measurement case M6-A6C-013. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Reverse audio — source-relative versus timeline-relative data

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-014: Reversal flips sample-relative warp-marker positions and clip loop/region settings. Clip envelopes remain at their existing time positions, so a first-half volume reduction continues to affect the first half after reversal. | Live 12 local manual, §8.4.2 pp.212; exact revision unconfirmed; installed 12.4 | AWRP/ADR-0003/0203 d13: one reverse operation transforms source-linked anchors and region coordinates while preserving envelope-time content; blanket reversal of every time series is incorrect. Measurement case M6-A6C-014. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Reverse audio — asynchronous state is not a performance guarantee

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-015: The guide describes a progress indicator for long reversals, temporarily blocked further actions while existing clips continue, and resumption once new waveform generation begins. Original/reversed version information is cached until Live closes, allowing a subsequent reversal to reuse it. | Live 12 local manual, §8.4.2 pp.212–213; exact revision unconfirmed; installed 12.4 | ADR-0069/background jobs: preparation, playable derived media, waveform availability and reusable cache lifetime need distinct states. This is not evidence of actual latency, CPU cost or glitch-free live operation. Measurement case M6-A6C-015. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Reverse audio — selection restrictions differ by view

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-016: Session View cannot reverse multiple clips together. Arrangement can reverse a time selection spanning multiple audio clips, but a selection containing MIDI clips cannot be reversed by Reverse Clip(s). | Live 12 local manual, §8.4.2, §6.9 pp.212,175; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0045: bulk-command availability must inspect selected content; ADI hybrid tracks should not cause an audio-only command to silently discard MIDI. Measurement case M6-A6C-016. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### External sample editing — shared-media and length gate

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-017: The configured external editor is launched through Edit. Processing requires stopping Live playback. On return, edited media is used; existing warp markers are retained only if sample length is unchanged. Other clips referencing that same sample may be affected. | Live 12 local manual, §8.4.3 pp.213; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0069: externally changed media invalidates shared analysis by source identity and length; outside-file edits need an explicit boundary because ordinary project undo may not restore bytes. Measurement case M6-A6C-017. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip fades — Session microfades differ from Arrangement handles

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-018: Session clips expose a Fade toggle applying signal-dependent edge fades of 0–4 ms. Arrangement uses clip fade envelopes instead. Create Fades on Clip Edges controls whether newly created clips enable fades; it does not establish the current user profile’s default. | Live 12 local manual, §8.4.4, §6.8 pp.213–214,172–174; exact revision unconfirmed; installed 12.4 | ADR-0203 d7: ADI starts this preference off by decision. Preserve separate Session microfade behavior and editable Arrangement fade data; do not claim a universal fixed 4 ms for Session. Measurement case M6-A6C-018. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip RAM mode — placement policy and limits

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-019: RAM Mode loads audio referenced by the clip into memory rather than streaming it in real time. The guide presents it as a remedy for disk or legato access problems but warns that memory swapping can itself cause mutes and timing hiccups. | Live 12 local manual, §8.4.5, §16.3 pp.214–215,357; exact revision unconfirmed; installed 12.4 | FEATURES §3/5: media readiness and preload policy must be separate from warp mode and musical timing. Neither actual memory footprint nor load-unit sharing is established by the manual. Measurement case M6-A6C-019. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip interpolation — Hi-Q capability boundary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-020: Hi-Q selects the guide’s higher-quality sample-rate conversion path for transposition and sample-rate matching. The manual’s approximately ±19-semitone aliasing statement is qualitative guidance, not a hard supported range or measured installed-build guarantee. | Live 12 local manual, §8.4.6 pp.215–216; exact revision unconfirmed; installed 12.4 | ADR-0188/0207: preserve resampling quality as a user option and test signal behavior separately from warp algorithms; do not infer the kernel or promise equivalence from the label. Measurement case M6-A6C-020. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip gain and pitch — mixed-value editing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-021: Clip gain is shown in dB and audio pitch has separate semitone and cent fields. Differing gains in a multi-clip selection appear as split handles. Multi-selection controls show a value range; dragging to an absolute extreme makes the selected values identical thereafter. | Live 12 local manual, §8.4.7, §8.13 pp.216–217,227; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0003/0203 d13: represent mixed values explicitly, preserve relative differences until saturation and distinguish cent precision from semitone entry. The chapter does not specify the gain bounds or exact rounding. Measurement case M6-A6C-021. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip selection — common capabilities only

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-022: Clip View generally displays only properties common to every selected clip. Multiple sample headers show the total sample count and use an asterisk where sample rate, bit depth or channel count differ; clicking a sample name reveals its file in the browser. | Live 12 local manual, §8.10, §8.13 pp.225,227; exact revision unconfirmed; installed 12.4 | ADR-0045/FEATURES §3: a mixed-selection inspector needs a capability intersection plus mixed-value state; absent controls must not be mistaken for absent persisted data. Measurement case M6-A6C-022. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip region navigation — move both markers without changing span

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-023: Start/end markers can be moved by dragging or arrow keys. Alt/Option plus arrows on either marker moves the whole start/end region together. Scrubbing follows global quantization, and holding the scrub area repeats a chunk of that size; Shift permits scrubbing when Permanent Scrub Areas is off. | Live 12 local manual, §8.8 pp.222–223; exact revision unconfirmed; installed 12.4 | ADR-0129/0203 d7: marker selection and region translation are different gestures from audio slip and loop movement; edit-grid snapping and global launch quantization have different owners. Measurement case M6-A6C-023. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip loop keyboard — position, length and content duplication

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-024: With the loop brace selected, horizontal arrows move by the edit grid, vertical arrows move by loop length, Ctrl/Cmd-horizontal changes length by the grid, and Ctrl/Cmd-vertical doubles/halves length. Duplicate Loop duplicates length and content; MIDI notes after the loop move to preserve their distance from its end. | Live 12 local manual, §8.9 pp.223–224; exact revision unconfirmed; installed 12.4 | FEATURES §3/4: loop-brace edits, region moves and destructive content duplication are separate commands; post-loop material must have a specified policy. Measurement case M6-A6C-024. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Crop — region selection and derived-media boundary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-025: Unlooped Crop uses clip start/end. Looped Crop begins at the earlier of clip start and loop start and ends at loop end. A time selection offers Crop to Time Selection using the same shortcut. Audio writes a shorter derived sample under Processed/Crop after save; MIDI crops event content rather than creating audio. | Live 12 local manual, §8.11 pp.226; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0069: crop is not just trimming a timeline instance. Preserve pickup material before the loop and retain original media for reversibility; give selection-dependent command precedence an explicit rule. Measurement case M6-A6C-025. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Replace sample — length-qualified marker preservation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-026: Dropping a sample into Clip View, or loading the selected browser sample there, preserves clip settings such as pitch and gain. Warp markers survive only if replacement length exactly matches. The broad template description in §26.2.5 must be read with this explicit length condition. | Live 12 local manual, §8.12, §26.2.5 pp.226–227,508; exact revision unconfirmed; installed 12.4 | FEATURES §3/AWRP: parameter/envelope templates are independent from source-dependent warp analysis. Do not interpret “all settings” as unconditional validity of an old warp map. Measurement case M6-A6C-026. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Replace sample — clip operation versus file-manager fan-out

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-027: Manage Sample File opens the current Set’s File Manager with the clip’s sample selected. Replacement or destructive editing there can affect all clips and instruments referencing the sample, unlike a clip-local template replacement. | Live 12 local manual, §8.12 pp.226–227; exact revision unconfirmed; installed 12.4 | ADR-0069/0127: expose per-instance replacement separately from shared-asset replacement, enumerate dependents and retain appropriate undo/media recovery information. Measurement case M6-A6C-027. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip update rate — playback-time application differs from launch quantization

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-028: Clip Update Rate in Record, Warp & Launch quantizes when Clip View changes reach a running clip. Launch Mode and Warp Mode also have new-clip defaults in that Settings page. The guide does not enumerate which individual edits obey the update rate. | Live 12 local manual, §8.14 pp.227–228; exact revision unconfirmed; installed 12.4 | ADR-0125/0203: editor commit time, audio-effective update time, launch quantization and new-object defaults are independent state. Pending audio changes must not become extra journal edits. Measurement case M6-A6C-028. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Tempo controls — coarse/fine mapping and temporary nudge

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-029: The tempo field has separately mappable coarse BPM and hundredth-BPM portions. Phase Nudge temporarily speeds/slows playback for alignment, whereas Tap Tempo changes the estimated tempo; these controls can be key/MIDI mapped. | Live 12 local manual, §9.1.1, §9.1.2, §9.1.3 pp.229–231; exact revision unconfirmed; installed 12.4 | FEATURES §1/5: base tempo, mapping precision and temporary phase correction require separate state and automation ownership. Measurement case M6-A6C-029. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Tap tempo — optional start and meter-bound example

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-030: Tap Tempo reacts immediately and refines the estimate with subsequent taps. Start Playback with Tap Tempo is a separate preference; the documented 4/4 example starts after four taps and aligns connected Link applications when used to start transport. | Live 12 local manual, §9.1.2 pp.230; exact revision unconfirmed; installed 12.4 | FEATURES §1/ADR-0185: estimator state, start-count policy and peer synchronization are different contracts. The four-tap example does not establish the count for every meter. Measurement case M6-A6C-030. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Tempo leaders — authority exclusions and commit operation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-031: A leader clip disables direct tempo-field editing; generated automation tracks leader rearrangement until Unfollow Tempo Automation converts all leaders to followers and makes that automation editable. Leader automation overrides audio Tempo Follower; enabling EXT disables the Lead/Follow toggle. | Live 12 local manual, §9.1.4 pp.231–233; exact revision unconfirmed; installed 12.4 | FEATURES §1/A-W08: model tempo-source authority and conversion explicitly. ADI has declined audio Tempo Follower under ADR-0203 d9; this reference does not reopen that decision. Measurement case M6-A6C-031. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Warp import policy — documented defaults and separate branch choices

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-032: Short-sample options are Unwarped One Shot, Warped One Shot, Warped Loop and Auto; Auto is the documented default. Long samples auto-warp by default, placing a marker at each bar’s first beat. Beats is the initial default Warp Mode. The short/long threshold is not stated. | Live 12 local manual, §9.2.1 pp.234–235; exact revision unconfirmed; installed 12.4 | ADR-0203 d7/d12: ADI’s import remains Warp off; pressing Warp performs its approved analysis behavior. Keep detector classification separate from new-clip policy and sample-default overrides. Measurement case M6-A6C-032. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Warp import — analysis readiness

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-033: §9.2.2 broadly says first-import analysis gates playback/editing. §5.1.2 narrows the long-sample playback wait: it does not occur when analysis already exists or Auto-Warp Long Samples is off. The installed readiness and edit-availability sequence remains unmeasured. | Live 12 local manual, §9.2.2 p.235; §5.1.2 pp.121–122; exact revision unconfirmed; installed 12.4 | ADR-0176/0203 d7: distinguish asset discovery, decode/analysis, waveform availability and playability; ADI’s Warp-off import may intentionally have a different readiness path. Measurement case M6-A6C-033. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Warp editing — contextual insertion/deletion and transient movement

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-034: Double-clicking the upper Sample Editor creates a warp marker; Ctrl/Cmd+I inserts at the cursor or promotes all transients in a time selection. Double-click deletes a marker, while deleting selected time removes markers there. Shift-dragging a pseudo-marker moves the transient; Shift-dragging an actual marker adjusts the waveform under its anchor. | Live 12 local manual, §9.2.3, §9.2.3.1 pp.235–236; exact revision unconfirmed; installed 12.4 | AWRP/ADR-0203 d6: one shortcut routes by focus and selection. Analysis transients and authored anchors require distinct hit targets and operations; preserve the already-recorded A-W03/04 propagation caveats. Measurement case M6-A6C-034. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Warp estimation — musical-length assumption and manual correction

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-035: For short prepared loops, the guide assumes lengths of 1, 2, 4, 8 or 16 bars and places start/end markers. BPM can be entered or finely dragged with Shift, and ×2/÷2 correct octave errors. Shift-dragging an Arrangement clip edge stretches/compresses its tempo. | Live 12 local manual, §9.2.4.1 pp.237–238; exact revision unconfirmed; installed 12.4 | ADR-0176/0188/0203 d7: estimated BPM is an editable interpretation, not ground truth; edge stretch must be distinct from ordinary trim. Measurement case M6-A6C-035. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Warp commands — selection-as-loop writes loop bounds

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-036: Warp Sample As proposes a loop length fitting Set tempo. Warp Selection As runs on a selected section, chooses a fitting loop length, places matching loop start/end markers and warps the selection into that loop. | Live 12 local manual, §9.2.5.1 pp.240–242; exact revision unconfirmed; installed 12.4 | FEATURES §3/AWRP: these commands may change both timing interpretation and clip loop state; preserve unaffected source regions and the complete prior region in undo. Measurement case M6-A6C-036. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Warp From variants — distinct reference-tempo inputs

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-037: Warp From Here analyzes audio to the right and adds a starting marker when needed. Start At uses current Set tempo as an analysis baseline; Straight uses estimated original BPM and one starting marker; fixed-BPM From Here assumes the Set tempo is the clip tempo. Earlier markers remain unchanged. | Live 12 local manual, §9.2.5.1 pp.242–243; exact revision unconfirmed; installed 12.4 | AWRP/A-W06: expose analysis-seed tempo versus a fixed tempo constraint as different commands; preserve the left-side anchor set, not just approximate audio output. Measurement case M6-A6C-037. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Audio quantize — focus, grid and tie uncertainty

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-038: Quantize operates from the focused Sample Editor using current quantization settings, moving transients toward nearby grid lines. The panel can follow the current grid or use an explicit division including triplets; Amount controls partial movement. Exact ties and interaction with existing locked anchors are not specified. | Live 12 local manual, §8.6, §9.2.7 pp.220,243–244; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0176: quantize settings and editor grid are distinct; generated markers must become one reproducible authored edit rather than rerunning analysis on redo. Measurement case M6-A6C-038. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Beats warp — loop turnaround and envelope endpoints

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-039: Forward and Back-and-Forth transient loops return to a zero crossing near the segment middle; the latter reverses direction on that path. Transient Envelope 100 applies no fade, whereas 0 yields fast-decaying segments. The guide does not identify the zero-crossing search rule or exact fade function. | Live 12 local manual, §9.3.1 pp.244–245; exact revision unconfirmed; installed 12.4 | ADR-0188 d2: these are behavioral requirements for the native Beats slice player, not proof of Live’s DSP implementation. Keep zero-crossing selection and decay curves as measurement targets. Measurement case M6-A6C-039. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Warp parameter bounds — documented controls versus missing numeric contracts

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-040: The local chapter names Tones Grain Size, Texture Grain Size/Fluctuation, and Complex Pro Formants/Envelope, but does not give all numeric ranges, step sizes, smoothing or automation support. It states full formant preservation at 100 percent and Envelope default 128; an untransposed sample is unaffected by Formants. | Live 12 local manual, §9.3.2, §9.3.3, §9.3.5 pp.245–246; exact revision unconfirmed; installed 12.4 | ADR-0188 d2: retain approved Tones continuous-Grain-Size and Complex-Pro Envelope parity gaps. A matching control name must not be presented as matching behavior or supported continuous range. Measurement case M6-A6C-040. | Unknown / not tested — documentary boundary or unresolved detail |

#### Clip launch velocity — amplitude sensitivity

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-041: Velocity Amount determines how incoming MIDI note velocity influences the launched clip’s volume. At zero there is no influence; at 100 percent the softest input notes make the clip silent. The transfer curve and interaction with existing clip gain are not given. | Live 12 local manual, §16.5 pp.359; exact revision unconfirmed; installed 12.4 | FEATURES §5/ADR-0117: remote launch velocity is a performance control distinct from MIDI notes inside the clip, clip gain and recorded velocity events. Measurement case M6-A6C-041. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip offset — nudge uses global timing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-042: Nudge Backward/Forward jumps within a playing clip in increments of global quantization. MIDI Map Mode exposes an intermediate scrub control for an encoder. Quantization shorter than one bar can offset clip phase from the main clock. | Live 12 local manual, §16.6 pp.359–361; exact revision unconfirmed; installed 12.4 | FEATURES §5: global nudge step, clip-specific launch quantization and play-phase offset are separate. Offsetting playback must not rewrite warp anchors or the source region implicitly. Measurement case M6-A6C-042. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Follow Action defaults — enabled, timing mode and time are independent

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-043: Follow Actions start disabled and can be toggled with Shift+Enter. Clip timing defaults to Linked; its multiplier delays action by a number of loops. Unlinked elapsed time defaults to one bar and has a draggable editor marker. Scenes expose time but not the clip-only Linked/Unlinked switch. | Live 12 local manual, §16.7 pp.361–362; exact revision unconfirmed; installed 12.4 | FEATURES §5/ADR-0117: separate enabled state, timing mode, loop multiplier and elapsed time; inactive controls may still contain values that survive toggling. Measurement case M6-A6C-043. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Follow Action terminal outcomes — no action versus stop versus restart

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-044: Selecting No Action as the outcome ends the opportunity for the clip’s other Follow Action to occur after that trigger; playback may continue normally. Stop halts the clip at the Follow Action time even if loop/region settings would continue. Restart starts the clip again. | Live 12 local manual, §16.7, §16.7.3 pp.362,365; exact revision unconfirmed; installed 12.4 | FEATURES §5: lack of a new launch, termination of the Follow Action sequence and stopping audio are separate scheduler states; No Action is not repeated polling for the other outcome. Measurement case M6-A6C-044. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Follow Action target choices — groups, exclusions and slot jumps

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-045: The next-in-group action wraps the last clip to the first. Random-any may choose the current clip; the nonrepeating alternative excludes consecutive repetition unless the group has only one clip. Jump selects a numeric clip-slot or scene target, which is a different target domain from adjacent group-relative actions. | Live 12 local manual, §16.7 pp.361–362; exact revision unconfirmed; installed 12.4 | FEATURES §5/ADR-0117: preserve action kind and target domain instead of flattening them to a computed next index. Empty-slot groups, absolute slot targets and mutable ordering require distinct resolution rules. Measurement case M6-A6C-045. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Follow Action chance — endpoint evidence and coupled-control uncertainty

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-046: The guide establishes that Chance 0 never occurs and A=100/B=0 always chooses A. Its example then changes B to 90 and describes A occurring about one tenth of the time, without explaining the linked-control update or normalization rule. It does not establish RNG algorithm, seed persistence or deterministic replay. | Live 12 local manual, §16.7 pp.361–362; exact revision unconfirmed; installed 12.4 | FEATURES §5/ADR-0003/0203 d13: do not implement independent weights or complementary percentages solely from this prose. Captured launches must preserve committed results independently of a future random redraw. Measurement case M6-A6C-046. | Unknown / not tested — documentary boundary or unresolved detail |

#### Follow Action chain creation — noncontiguous selection

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-047: Create Follow Action Chain configures a loop through selected clips, and the selection need not be contiguous. This differs from ordinary Follow Action group membership, which depends on consecutive occupied slots. | Live 12 local manual, §16.7.2 pp.365; exact revision unconfirmed; installed 12.4 | FEATURES §5: chain creation is a batch configuration command over an explicit selection, not necessarily Next on every selected slot. Preserve each prior Follow Action configuration for undo. Measurement case M6-A6C-047. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip launch state machine — release and queue details remain open

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-048: The mode descriptions establish press/release roles but do not settle Gate release before a queued launch, Repeat with quantization None, repeated clicks on an already pending launch, or cancellation when a target is moved/deleted. The general and detailed Follow Action quantization paragraphs also differ in specificity, as already recorded in A3-004. | Live 12 local manual, §16.2, §16.4, §16.7 pp.356,358,362–363; exact revision unconfirmed; installed 12.4 | FEATURES §5/ADR-0117: queued launch, input-held state, currently playing clip and Follow Action timer require explicit tests. Existing manual rules cannot close the installed gesture/undo questions. Measurement case M6-A6C-048. | Unknown / not tested — documentary boundary or unresolved detail |

#### Automation recording — Arrangement arm gate

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-049: Direct Arrangement recording writes manual parameter changes when both Automation Arm and Arrangement Record are on. Session-to-Arrangement performance recording always transfers Session clip automation and records manual changes on tracks recorded from Session. | Live 12 local manual, §25.1 pp.491–492; exact revision unconfirmed; installed 12.4 | N26/ADR-0003/0203 d13: captured Session automation and live control writing have different gate conditions; do not use one boolean to suppress both paths. Measurement case M6-A6C-049. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Automation recording — all playing Session clips option

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-050: Session automation normally requires Automation Arm, track arm and Session Record. The Session Automation Recording preference can instead record automation into every playing Session clip regardless of track arm, enabling automation overdub into an existing MIDI clip without also recording notes. | Live 12 local manual, §25.2 pp.492–494; exact revision unconfirmed; installed 12.4 | N26/FEATURES §7: audio/MIDI recording arm and automation capture eligibility are related but not identical; store the policy outside clip event data. Measurement case M6-A6C-050. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Automation deletion — parameter-wide scope across views

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-051: Delete Automation invoked on an automated control removes that control’s automation across the Arrangement and Session clips, after which its value stays constant. This is broader than deleting selected breakpoints in one visible lane and is not a command to delete every parameter’s automation. | Live 12 local manual, §25.3 pp.494; exact revision unconfirmed; installed 12.4 | N26/ADR-0003/0203 d13: expose target-wide deletion separately from range/clip envelope clearing; enumerate all affected owners in a reversible changeset. Measurement case M6-A6C-051. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Automation display — control touch, filters and target existence

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-052: Clicking a mixer/device control shows its envelope on the main clip lane. The Device chooser can hide it with None or filter to automated parameters, and LEDs identify automated devices/controls. Viewing a parameter and possessing automation data are distinct states. | Live 12 local manual, §25.5 pp.495–496; exact revision unconfirmed; installed 12.4 | N26/parity-7.3 rows 38–39: keep the selected target, filtered target list, lane layout and stored envelopes separate; touching a control to navigate must be distinguishable from changing its value. Measurement case M6-A6C-052. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Automation lanes — bulk add/hide scope and keyboard fold

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-053: Alt on Windows or Cmd on macOS while adding a lane exposes all automated envelopes; the same modifier while hiding a lane hides that lane and subsequent lanes. The add control is absent for Device=None. Left from a lane returns to the main track and folds lanes; left/right on the main track folds/unfolds them. | Live 12 local manual, §25.5 pp.496; exact revision unconfirmed; installed 12.4 | N26/parity-7.3: bulk lane display is view state; the original unknown about exact lane order/unfold behavior by track kind remains open. These modifier choices must not be normalized to one cross-platform key. Measurement case M6-A6C-053. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope drawing — step width and freehand mode

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-054: Draw Mode writes steps the width of the visible editing grid. Shift-vertical drag gives finer value resolution. Hiding the grid enables freehand drawing; Alt on Windows or Cmd on macOS temporarily enables freehand while the grid is visible. Holding B temporarily toggles Draw Mode. | Live 12 local manual, §25.5.1 pp.496–497; exact revision unconfirmed; installed 12.4 | N26/ADR-0203 d7: edit grid, drawing mode, precision modifier and launch/update quantization are independent. Coalescing a stroke must include all generated points without assuming one point per mouse event. Measurement case M6-A6C-054. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope points — insertion, deletion and displayed value

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-055: With Draw Mode off, a click on a segment inserts a breakpoint; a double-click away from a segment inserts one there; clicking a breakpoint deletes it. Hovering or dragging a selected segment displays the value of the breakpoint nearest the cursor, not necessarily an interpolated value at that cursor time. | Live 12 local manual, §25.5.2 pp.497–498; exact revision unconfirmed; installed 12.4 | N26/ADR-0131: hit-testing must distinguish point, line and background. The numeric tooltip is not sufficient evidence of the audio interpolation formula. Measurement case M6-A6C-055. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope points — relative multi-edit and exact entry

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-056: Dragging a selected breakpoint moves every breakpoint in the selection. Edit Value accepts an exact typed value and moves other selected breakpoints relatively. Add Value creates a point from a preview breakpoint at an exact value. | Live 12 local manual, §25.5.2 pp.498; exact revision unconfirmed; installed 12.4 | N26/ADR-0003/0203 d13: multi-point edits preserve relative offsets until bounds intervene; typed entry and drag should reach the same validated operation while retaining distinct preview/commit state. Measurement case M6-A6C-056. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope segment drag — selection boundary points

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-057: Clicking near a segment, or Shift-clicking directly on it, selects the segment. Dragging a segment inside the current time selection inserts breakpoints at the selection edges and moves the selected segment together. | Live 12 local manual, §25.5.2 pp.498; exact revision unconfirmed; installed 12.4 | N26: a segment gesture can create boundary anchors in addition to moving existing points; keep the surrounding envelope stable and include generated anchors in the same edit. Measurement case M6-A6C-057. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope collisions — snapping and destructive crossing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-058: Breakpoints snap near grid lines and to neighboring breakpoint times. Continuing a horizontal point/segment drag over a neighbor removes that neighbor. Alt on Windows or Cmd on macOS bypasses grid snapping; the exact collision threshold/order is not specified. | Live 12 local manual, §25.5.2 pp.498; exact revision unconfirmed; installed 12.4 | N26/ADR-0003/0203 d13: collision removal is an authored change, not just display overlap; retain removed points for undo and determine whether snapping bypass also affects point-to-point snapping. Measurement case M6-A6C-058. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope modifiers — axis lock and precision coexist

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-059: Shift while moving points/segments restricts movement to horizontal or vertical; Shift during vertical drag also gives finer value resolution. The guide does not specify how the axis is chosen or how modifier changes mid-drag are resolved. | Live 12 local manual, §25.5.2 pp.498–499; exact revision unconfirmed; installed 12.4 | N26/ADR-0129: gesture recognizers need explicit initial-axis and mid-gesture modifier rules; do not infer them from generic Shift handling. Measurement case M6-A6C-059. | Unknown / not tested — documentary boundary or unresolved detail |

#### Envelope curves — bend and straightening contract

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-060: Alt/Option-dragging a line segment curves it; Alt/Option-double-clicking restores a straight segment. The manual does not define the curve function, its parameter range or whether the same curve is evaluated in display-normalized or real parameter units. | Live 12 local manual, §25.5.2 pp.499; exact revision unconfirmed; installed 12.4 | ADR-0203 d7/ADR-0188: ADI must measure the curve before claiming Live parity; real-unit automation storage does not itself determine the interpolation space. Measurement case M6-A6C-060. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope transform — vertical stretch clips values

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-061: Top/bottom center handles stretch the selected envelope vertically. The transform rectangle snaps at parameter boundaries and when corners intersect; dragging past bounds clips the envelope. Shift enables fine adjustment. | Live 12 local manual, §25.5.3 pp.499–500; exact revision unconfirmed; installed 12.4 | N26: transform preview, clamped result and original data must remain separate until commit; clipping can destroy relative amplitudes and must be fully reversible. Measurement case M6-A6C-061. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope transform — horizontal stretch versus pushing neighbors

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-062: Left/right center handles stretch automation in time. Normally, stretching over points outside the selection removes them; holding Shift moves those outside points proportionally with the handle instead. Alt on Windows or Cmd on macOS bypasses grid snapping. | Live 12 local manual, §25.5.3 pp.500; exact revision unconfirmed; installed 12.4 | N26/ADR-0003/0203 d13: the modifier changes edit scope beyond the selected range; the operation must include displaced/deleted outside anchors and preserve their stable order. Measurement case M6-A6C-062. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope transform — corner skew and mirrored handles

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-063: Corner handles skew selected automation. Shift gives fine skew adjustment. Alt/Option while dragging a handle mirrors the movement in the opposite handle; this is separate from the platform-specific grid-bypass modifier described for horizontal stretching. | Live 12 local manual, §25.5.3 pp.499–500; exact revision unconfirmed; installed 12.4 | N26/parity gestures: mirrored transform and snap bypass can share Alt on Windows but differ on macOS; dispatch by handle and axis, not a universal modifier meaning. Measurement case M6-A6C-063. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope simplification — selected-domain curve refit

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-064: Simplify Envelope acts on a time selection, removes unnecessary points and can replace them with straight or curved segments. The guide gives no numerical error tolerance, algorithm or guarantee of preserving exact audio output. | Live 12 local manual, §25.5.4 pp.500; exact revision unconfirmed; installed 12.4 | N26/ADR-0003/0203 d13: simplification is an irreversible-in-place refit made reversible by stored before/after data; do not advertise sample-identical output or deterministic equivalence without measurement. Measurement case M6-A6C-064. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope shapes — scaling differs from endpoint-connected ramps

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-065: Sine, triangle, sawtooth, inverse sawtooth and square shapes scale to the selected duration and full parameter range; with no time selection they use current grid width. The lower-row ramp sets and ADSR instead connect to the neighboring automation value indicated by each shape’s dotted line. | Live 12 local manual, §25.5.5 pp.501; exact revision unconfirmed; installed 12.4 | ADR-0203 d7: shape identity must include endpoint-connect rules, not just normalized point presets. Preserve exact Live shape variants as a measurement item before implementing the UI set. Measurement case M6-A6C-065. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Envelope edit routing — one lane, many lanes and parameter paste

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-066: Cut/Copy/Duplicate/Delete inside one automation lane affects only that envelope; multiple lanes can be edited together. Clip-track edits include associated envelopes only with Lock Envelopes off. Envelope data can also be copied between unrelated parameters; conversion/scaling rules are not specified. | Live 12 local manual, §25.5.6, §25.5.7 pp.502; exact revision unconfirmed; installed 12.4 | N26/ADR-0003/0203 d13: command target is selection domain, not merely focused track. Cross-parameter paste requires domain conversion and range validation separate from time rebasing. Measurement case M6-A6C-066. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Tempo automation — display range also sets MIDI mapping range

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-067: The tempo envelope is selected through Main → Mixer → Song Tempo. Its minimum/maximum boxes scale the displayed value axis and also define the range of a MIDI controller assigned to tempo. | Live 12 local manual, §25.5.8 pp.502–503; exact revision unconfirmed; installed 12.4 | FEATURES §1/N26/ADR-0181: these fields are not presentation-only zoom; changing them has a controller-input consequence while existing tempo data must be handled explicitly. Measurement case M6-A6C-067. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip envelopes — target registry depends on clip type

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-068: Audio clips offer Clip sample controls, track effects and mixer targets; MIDI clips offer MIDI Ctrl, track devices and mixer targets. Only Show Adjusted Envelopes filters the chooser, and target LEDs indicate authored envelopes rather than current visibility. | Live 12 local manual, §26.1 pp.504–505; exact revision unconfirmed; installed 12.4 | N26/FEATURES §3/4: derive the envelope target registry from capabilities and stable device IDs. ADI hybrid tracks may contain both clip types but their envelopes still have different valid local domains. Measurement case M6-A6C-068. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip envelope clear — default reset versus parameter-wide delete

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-069: Clear Envelope resets the current clip envelope to its default value; this is different from Delete Automation on a control. The same passage describes Ctrl+Backspace/Cmd+Delete while discussing the context menu, so exact key behavior and focus routing should be checked rather than inferred. | Live 12 local manual, §26.1, §25.3 pp.505,494; exact revision unconfirmed; installed 12.4 | N26/ADR-0003/0203 d13: clip-local reset, range deletion and parameter-wide removal are separate commands. Default value may differ by modulation/MIDI domain; absence of stored points is not necessarily equivalent to zero. Measurement case M6-A6C-069. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip automation/modulation — concurrent target feedback

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-070: A Session target may have both automation and modulation envelopes, shown with red and blue chooser LEDs. Automation moves the knob’s base position; modulation is shown on its ring. Track-volume modulation is additionally indicated by a dot below the fader thumb. | Live 12 local manual, §26.3, §26.3.1 pp.508–511; exact revision unconfirmed; installed 12.4 | ADR-0046/N26: authored automation, relative modulation, manual override and effective value need distinct observable state; do not overload one red/blue flag or animate the base value from modulation. Measurement case M6-A6C-070. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Pitch envelope — clamped additive range and response caveat

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-071: Clip transposition envelope output adds to the clip’s base transposition and is clamped to −48…+48 semitones. The guide says warp settings affect how closely playback follows the curve; smaller Tones/Texture grains or Beats granulation divisions give more immediate response. Its older Granulation Resolution terminology is not reconciled with the newer Preserve control. | Live 12 local manual, §26.2.2, §9.3.1 pp.506–507,244–245; exact revision unconfirmed; installed 12.4 | ADR-0188/AWRP: envelope evaluation and audible engine response are separate. Preserve ADI’s approved engine gaps and measure the control-name correspondence instead of assuming a new parameter. Measurement case M6-A6C-071. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Sample Offset envelope — Beats-only musical read-position modulation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-072: Sample Offset is available only in Beats mode. Positive values read toward later source material and negative values toward earlier material; the grid is in sixteenth-note offsets and the documented range is −8…+8 sixteenths. It modulates the read position rather than rewriting sample bytes. | Live 12 local manual, §26.2.4 pp.507–508; exact revision unconfirmed; installed 12.4 | ADR-0188 d2/FEATURES §3: source read offset is a musical-time modulation domain distinct from clip start, timeline slip and warp anchors; define conversion through a nonuniform warp map. Measurement case M6-A6C-072. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### MIDI controller envelopes — bounded support and reset uncertainty

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-073: MIDI Ctrl exposes most controller numbers up to 119, not an unconditional guarantee for every CC. Recorded/imported controller data can be edited as envelopes. MIDI Envelope Auto-Reset can reset certain messages on a new clip, but these chapters do not list the complete message set or reset values. | Live 12 local manual, §26.1, §26.4 pp.505,513; exact revision unconfirmed; installed 12.4 | FEATURES §4/N26: MIDI stream controller envelopes remain distinct from host parameter automation and per-note expression. Capability and reset policy need explicit tables before claiming complete controller support. Measurement case M6-A6C-073. | Unknown / not tested — documentary boundary or unresolved detail |

#### Unlinked envelopes — independent periods and launch origin

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-074: Each unlinked envelope can have its own start/region/loop and arbitrary loop length, independent of the sample and other envelopes. Its start marker determines where playback begins on clip launch; markers and brace snap to the zoom-adaptive grid. Hiding the grid permits non-metrical loop periods. | Live 12 local manual, §26.5, §26.5.1, §26.5.2, §26.5.3, §26.5.4 pp.513–515; exact revision unconfirmed; installed 12.4 | N26/FEATURES §3: each envelope needs its own timing state, not a single clip-wide unlinked-loop slot. Tempo-synchronized evaluation can still yield cycles not aligned with bar boundaries. Measurement case M6-A6C-074. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Linked envelopes — warp editing from the envelope view

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-075: Linked clip envelopes stretch or compress when warp markers move, and warp markers can be edited directly in the Envelope Editor. This couples envelope timing to authored warp changes while unlinked envelope timing remains independently configured. | Live 12 local manual, §26.5.5, §26.5 pp.513–516; exact revision unconfirmed; installed 12.4 | AWRP/N26: one marker edit can update audible timing and linked envelope coordinates regardless of the editor that initiated it. Avoid keeping separate Sample Editor and Envelope Editor warp maps. Measurement case M6-A6C-075. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Arrangement clip edit — bar move versus content slip

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-076: The clip bar is the draggable move target; its waveform/MIDI body is not. Ctrl+Shift on Windows or Shift+Option on macOS drags content within fixed boundaries for warped audio or MIDI. Adding Alt on Windows or Cmd on macOS bypasses snap for the content slip. | Live 12 local manual, §6.7 pp.170–171; exact revision unconfirmed; installed 12.4 | FEATURES §3/N17; ADR-0203 d7: clip placement, region trim and source-content offset require distinct operations. Unwarped audio support must not be inferred from a gesture documented only for warped audio and MIDI. Measurement case M6-A6C-076. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Arrangement clip snapping — structural targets beyond the grid

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-077: Moved/resized clips snap not only to the editing grid but also to other clip edges, locators and time-signature changes. Fixed/adaptive grid state and temporary snap inversion are separately documented. | Live 12 local manual, §6.7, §6.10 pp.171,175; exact revision unconfirmed; installed 12.4 | parity-7.3 row 27/ADR-0129: object snapping remains a concrete missing parity item; candidate priority, pixel threshold and modifier effects require measurement. Measurement case M6-A6C-077. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Arrangement fades — visibility and temporary access in Automation Mode

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-078: Fade handles appear on hover only when the audio track is tall enough. With Automation Mode enabled, holding F while hovering an automation lane temporarily reveals fade controls. | Live 12 local manual, §6.8 pp.172; exact revision unconfirmed; installed 12.4 | parity-7.3/ADR-0203 d5/d7: hidden handles do not mean missing fades; temporary access must not overwrite the persistent automation-mode state or target the wrong lane. Measurement case M6-A6C-078. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Arrangement fades — independent edge/peak geometry and limits

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-079: Fade-start/end handles can alter duration without moving the peaks, but edges cannot cross peaks. Fade curves have a separate handle. Fades cannot cross clip loop boundaries or overlap the opposite fade; a dotted limit line shows the applicable boundary, including when hidden under a crossfade. | Live 12 local manual, §6.8 pp.172–173; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0203 d7: model fade boundary, peak and curve separately; crossfade constraints depend on both clips’ source/loop geometry. Cubase’s advanced editor remains the approved additional UI. Measurement case M6-A6C-079. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Arrangement fade creation — selection-dependent command and reset policy

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-080: A selection including a clip start/end creates its corresponding fade; a selection spanning adjacent clips’ boundary creates a crossfade. With Create Fades on Clip Edges enabled, deleting a fade resets it to 4 ms and adjacent clips get automatic 4 ms crossfades; with it disabled, Delete removes the fade. | Live 12 local manual, §6.8 p.173; §21.6 p.428 (comp crossfades); exact revision unconfirmed; installed 12.4 | ADR-0203 d7: ADI defaults automatic fades off. Command dispatch must inspect selected boundaries and distinguish reset-to-default from removal; document this without assuming a particular crossfade law. Measurement case M6-A6C-080. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Arrangement fades — clip ownership independent of automation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-081: Arrangement fades belong to clips rather than tracks and are independent of automation envelopes. They therefore cannot be represented solely as track-volume automation lanes. | Live 12 local manual, §6.8 pp.174; exact revision unconfirmed; installed 12.4 | FEATURES §3/N26/ADR-0069: preserve separate clip fade data through moves, splits, render and offline processing. Exact interaction with Lock Envelopes remains a test, not an inference from the name. Measurement case M6-A6C-081. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Arrangement time selection — body, loop and cross-track extension

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-082: Dragging the waveform/MIDI body selects time; clicking the loop brace selects material within the loop. Shift-click extends selection within/across tracks, and Shift-arrows extend or shorten it. Ctrl on Windows or Option on macOS plus horizontal arrows moves the insert marker to clip edges/locators of the selected tracks. | Live 12 local manual, §6.9 pp.174–175; exact revision unconfirmed; installed 12.4 | parity-7.3 rows 30/36/N17: keep insert marker, contiguous time range and selected tracks distinct. This does not answer the still-open Select All origin/return-track question. Measurement case M6-A6C-082. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Arrangement split — cursor split versus range isolation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-083: Split at a cursor divides the clip at that position; applying it to a selected internal timespan isolates the selected portion as its own clip with independent edges. The resulting pieces can be moved/edited independently. | Live 12 local manual, §6.12 pp.176–177; exact revision unconfirmed; installed 12.4 | FEATURES §3/N17: range edits need precise boundary splitting before delete/copy/move. The guide does not establish generated names, source offset rebasing, note-crossing policy or exact transaction grouping. Measurement case M6-A6C-083. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Linked-track editing — membership changes and selection identity

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-084: Hovering a linked-track indicator highlights the linked set; clicking it selects that set. Ctrl/Cmd plus Link Tracks adds selected tracks to an existing set. Linking a subset or mixed linked/unlinked selection can form a new linked instance; Unlink can remove only selected members. | Live 12 local manual, §6.14.1 pp.178–179; exact revision unconfirmed; installed 12.4 | FEATURES §3/ADR-0003/0203 d13: linked-edit membership is separate from folder/group routing and shared clip aliases. Membership edits must not implicitly change bus routing or shared-media identity. Measurement case M6-A6C-084. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Clip navigation — Arrangement time selection links editor zoom

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-085: Selecting time on an Arrangement clip zooms Clip View to that time. The Clip View Selector always depicts the complete start-to-end clip and its outlined current viewport; dragging that outline scrolls/zooms. Editing in the Sample/MIDI editor temporarily pauses Follow, which resumes on stop/restart or scrub-area click. | Live 12 local manual, §6.2, §8.7 pp.163,221–222; exact revision unconfirmed; installed 12.4 | ADR-0129/0203 d7: linked view navigation must preserve clip/source coordinates and separate enabled Follow from temporarily paused following; the full-clip selector is not the same extent as the loop brace. Measurement case M6-A6C-085. | Supported hypothesis — documented; exact local manual revision unconfirmed and installed Live 12.4 behavior unmeasured |

#### Warp repair prose — Set 1.1.1 and odd-loop examples need exact observation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6C-086: The short-loop recipe describes Set 1.1.1 Here as placing the first warp marker, while the long-loop recipe describes moving the start marker. The odd-loop example also mixes nine-bar loop correction with an even-bar marker instruction. These recipes do not define a complete coordinate transformation or odd-length inference rule. | Live 12 local manual, §9.2.4.2, §9.2.4.3, §9.2.5.1 pp.238–240; exact revision unconfirmed; installed 12.4 | AWRP/ADR-0203 d7: keep beat origin, clip start and source anchor separate. Do not turn an ambiguous example into a rule that forces odd-bar clips onto even lengths. Measurement case M6-A6C-086. | Unknown / not tested — documentary boundary or unresolved detail |

### Routing, recording, comping, browser, files and Rack hosting

#### Typed routing and port availability

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-001: Audio tracks receive audio; MIDI tracks receive MIDI. MIDI tracks without an instrument output MIDI, while other track outputs are audio. The upper chooser selects source/destination type; its lower chooser exposes available channels or subdestinations. Internal source and destination menus include compatible signal types. | Live 12 local manual, §17 / §17.5 pp.368,375; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,6; parity-7.6 rows 18–20: menu eligibility follows the actual graph endpoint type. ADI hybrid-track support is an approved difference, not a reason to discard endpoint types. Measurement case M6-A6H-001. | Supported hypothesis — documented; installed behavior unmeasured |

#### Audio hardware channel identity

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-002: External audio chooser entries expose signal/overload meters. Audio Settings determine enabled mono channels and stereo pairs; renamed channels appear in routing choosers and names belong to the particular audio device. A chooser change applies to all selected tracks. | Live 12 local manual, §17.2 pp.370; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2; parity-7.6 row 19: store hardware endpoint identity separately from a user-facing name and current device availability. Measurement case M6-A6H-002. | Supported hypothesis — documented; installed behavior unmeasured |

#### Computer MIDI keyboard shortcut ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-003: The computer MIDI keyboard maps the middle letter row from C3, uses Z/X for octave and C/V for velocity in increments of twenty. It consumes keys otherwise available to remote mappings; Shift plus a single-letter shortcut can access that shortcut while the keyboard is enabled. | Live 12 local manual, §17.3.2 pp.373–374; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §4,9; keyboard parity: resolve note entry, text input, application shortcuts and remote assignments by explicit focus/context priority. Measurement case M6-A6H-003. | Supported hypothesis — documented; installed behavior unmeasured |

#### MIDI diagnostic indicators

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-004: Three input/output indicator pairs distinguish synchronization, remote-control and track MIDI; sync indicators appear only when an external sync source is enabled. Each pair separates received and sent messages. | Live 12 local manual, §17.3.4 pp.374–375; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §4,9; ADR-0181: expose the input stage that consumed a message so a remote assignment can be distinguished from a missing instrument route. Measurement case M6-A6H-004. | Supported hypothesis — documented; installed behavior unmeasured |

#### External hardware instrument as a route wrapper

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-005: The External Instrument device can send MIDI to a hardware synthesizer and return its audio on one track. Separate ordinary MIDI-output and audio-input tracks are another routing construction; the wrapper is an integration path rather than the synthesizer itself. | Live 12 local manual, §17.3.3 pp.374; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,6; ADR-0122: represent external send/return and unavailable ports explicitly. Built-in instrument DSP remains excluded from this research. Measurement case M6-A6H-005. | Supported hypothesis — documented; installed behavior unmeasured |

#### Resampling excludes its own destination

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-006: Resampling takes the Main output as an audio-track input and suppresses the recording track’s own output during resampling, so it is not included in the recording. Recorded files go to Samples/Recorded after saving, or the configured temporary location beforehand. | Live 12 local manual, §17.4 pp.375; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,8; ADR-0127: a resample route needs an explicit self-feedback exclusion and a recording media lifetime. Measurement case M6-A6H-006. | Supported hypothesis — documented; installed behavior unmeasured |

#### Send-to versus tap-from route ownership and solo

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-007: Routing A’s output to B leaves B’s input configuration alone and supports many senders; solo B keeps its feeding sources audible, while solo A removes other contributors. Making B tap A leaves A’s own output unchanged and supports multiple consumers. Sends Only can use a return as the submix. | Live 12 local manual, §17.5 / §17.5.2.3 pp.375–376,381–382; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2; ADR-0122: distinguish destination assignment from nonexclusive source taps, and calculate solo audibility over graph relationships. Measurement case M6-A6H-007. | Supported hypothesis — documented; installed behavior unmeasured |

#### Track tap points and solo exception

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-008: Pre FX precedes devices and mixer; Post FX follows devices but precedes mixer; Post Mixer follows both. Soloing a receiving track keeps its source audible for Pre FX/Post FX taps, but the manual says a Post Mixer tap does not make the source audible. | Live 12 local manual, §17.5.1 pp.376–377; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2; parity-7.6 row 7/N23: tap stage and solo dependency are separate contracts; do not generalize bus solo behavior to every tap. Measurement case M6-A6H-008. | Supported hypothesis — documented; installed behavior unmeasured |

#### Rack chain routing subaddresses

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-009: Instrument/Effect Rack chains expose Pre FX, Post FX and Post Mixer taps; Drum Rack routing taps are exposed for return chains. Chain Post Mixer is before Rack summation. Soloing a track tapping a Rack chain at any of these points keeps that tapped output audible. | Live 12 local manual, §17.5.1.1 pp.378; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0060; FEATURES §2,6: address a nested chain and tap stage by stable IDs, not a flattened Rack-name string; Rack tap solo differs from ordinary track Post Mixer. Measurement case M6-A6H-009. | Supported hypothesis — documented; installed behavior unmeasured |

#### Recording processed input without duplicate monitoring

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-010: The documented post-effects recording setup uses a dedicated processing track monitored In and receiving recording tracks monitored Off. Receiving Post FX commits device processing; Post Mixer additionally commits source level and panning. An instrument track’s audio can likewise be recorded onto a separate audio track. | Live 12 local manual, §17.5.2.1 / §17.5.2.2 pp.379–380; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,8; recording path tests: separate monitored sound, recorded tap and destination playback ownership. Measurement case M6-A6H-010. | Supported hypothesis — documented; installed behavior unmeasured |

#### Track In versus direct instrument MIDI destination

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-011: A receiving MIDI track offers Track In, which feeds its recordable input, and an instrument destination, which bypasses recording and monitoring. A track used only as an instrument container can select No Input, which removes its Arm button. Muting that container silences the instrument audio even when other tracks continue sending MIDI. | Live 12 local manual, §17.5.2.4 pp.382–384; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,6; parity-7.6 rows 8,18: preserve input-stage versus instrument-stage routes and separate MIDI event activity from audible output. Measurement case M6-A6H-011. | Supported hypothesis — documented; installed behavior unmeasured |

#### Multi-output plug-ins retain their main-output owner

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-012: Multi-timbral plug-ins can receive separate MIDI channels from feeder tracks while audio tracks tap individual outputs. External Instrument can combine MIDI send and auxiliary-audio return on one feeder track, including inside Racks; only auxiliary outputs are available there, while the plug-in’s main output remains on its hosting track. | Live 12 local manual, §17.5.2.5 / §17.5.2.6 pp.384–386; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,6; ADR-0122: model MIDI channel, plug-in output bus and destination ownership independently. The manual warns not to generalize a built-in sampler’s remove-tapped-output-from-main convenience to plug-ins. Measurement case M6-A6H-012. | Supported hypothesis — documented; installed behavior unmeasured |

#### Sidechain input as an addressable destination

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-013: A sending audio track may route its output to a device’s sidechain subdestination on another track. A plug-in instrument with an audio sidechain can receive this route too; devices with their own sidechain source choosers provide another way to select the connection. | Live 12 local manual, §17.5.2.7 pp.387; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,6; ADR-0122: distinguish program input, sidechain input and MIDI input capabilities rather than deriving capabilities only from track kind. Measurement case M6-A6H-013. | Supported hypothesis — documented; installed behavior unmeasured |

#### MIDI taps on instrument tracks

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-014: When a MIDI track receives MIDI from another track, Live taps MIDI after that source’s MIDI effects and immediately before its instrument, even though the source track’s final output is audio. | Live 12 local manual, §17.5.2.8 pp.387–388; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,4,6; ADR-0060: the graph can expose a MIDI subendpoint on an audio-producing instrument track. Measurement case M6-A6H-014. | Supported hypothesis — documented; installed behavior unmeasured |

#### Arming and native-controller ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-015: Arming selects the track for immediate Device View access; on a natively supported control surface, arming a MIDI track automatically locks that surface to the instrument in the track. | Live 12 local manual, §19.2 pp.406; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0181; FEATURES §9: controller focus and track selection need a defined relationship to explicit device locks and multitrack arming. Measurement case M6-A6H-015. | Supported hypothesis — documented; installed behavior unmeasured |

#### Arrangement loop recording retains every audio pass

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-016: Arrangement loop recording retains the audio from each pass. The manual describes previous passes being recoverable through repeated Undo or by moving the new clip’s loop/start markers over a long recorded sample containing the passes; the last pass supplies the initial loop window. | Live 12 local manual, §19.3.1 pp.407; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,8; ADR-0127: recording file extent, visible clip window and undoable takes cannot be represented by one destructive overwrite. Measurement case M6-A6H-016. | Supported hypothesis — documented; installed behavior unmeasured |

#### MIDI overdub can coexist with editing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-017: During Session pattern overdub, Session Record toggles between recording and playback while the clip keeps looping; Undo may remove the last take, and notes can be drawn, moved or deleted during overdub. Alt/Option double-click on an empty slot implicitly arms its track and launches a new clip. | Live 12 local manual, §19.3.3 pp.409; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §4,8; ADR-0003/ADR-0203 d13: concurrently arriving recorded notes and explicit editor changes need ordered transaction boundaries. Measurement case M6-A6H-017. | Supported hypothesis — documented; installed behavior unmeasured |

#### Metronome rhythm survives incompatible signatures

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-018: Auto rhythm follows the time-signature denominator. Divisions that do not fit a bar are disabled; if a signature change invalidates the chosen division, ticking temporarily uses Auto and returns to the prior selected division when it becomes compatible again. | Live 12 local manual, §19.4.1 pp.412; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §1; transport/settings: retain requested rhythm separately from currently effective rhythm. Measurement case M6-A6H-018. | Supported hypothesis — documented; installed behavior unmeasured |

#### Metronome recording-only and punch boundary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-019: Enable Only While Recording leaves the activated metronome visually highlighted during transport playback but makes it audible only while recording. With Arrangement Punch-In enabled, it becomes audible after the punch-in point. | Live 12 local manual, §19.4.1 pp.412; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §1,8; transport parity: metronome enabled, running, count-in and audibility states should be distinct. Measurement case M6-A6H-019. | Supported hypothesis — documented; installed behavior unmeasured |

#### Count-in transport display

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-020: A non-None recording count-in defers recording until completion. Arrangement Position displays it in blue using negative bars-beats-sixteenths up to 1.1.1. | Live 12 local manual, §19.6 pp.413; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §1,8; transport UI: a count-in needs a temporal state before committed recording, not a negative-position clip. Measurement case M6-A6H-020. | Supported hypothesis — documented; installed behavior unmeasured |

#### Recording media location and format policy

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-021: Record, Warp & Launch Settings choose recorded sample file type and bit depth plus clip defaults such as Warp Mode. Recorded samples live in the current Project’s Samples/Recorded, or the configured Temporary Folder until the Set is saved. | Live 12 local manual, §19.7 / §19.8 pp.413; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,8; ADR-0127: distinguish recording encoding, playback processing defaults and temporary media promotion; ADI’s Warp-off import deviation remains authoritative. Measurement case M6-A6H-021. | Supported hypothesis — documented; installed behavior unmeasured |

#### Capture MIDI has distinct empty/existing-set regimes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-022: For an empty stopped Set, Capture detects tempo only within 80–160 BPM; a single captured note sets a one-, two-, four- or eight-bar loop from its start/end. Running transport, any existing clip or automated tempo instead makes Capture retain the current tempo; capture can overdub an already playing clip on the same track. | Live 12 local manual, §19.10 / §19.10.1 / §19.10.2 pp.415–417; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §4,8; extend A-H03 with explicit eligibility guards rather than a universal tempo-detection action. Measurement case M6-A6H-022. | Supported hypothesis — documented; installed behavior unmeasured |

#### Bounce naming and durable media

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-023: Bounce-created tracks/clips append (Bounce) to their source names; an unnamed source clip yields (Bounce). The new track inherits source track color, and rendered files are placed in Current Project/Samples/Processed/Bounce. | Live 12 local manual, §20.1 pp.420; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,8; ADR-0127: generated media, presentation labels and source identities should be recorded separately in a bounce transaction. Measurement case M6-A6H-023. | Supported hypothesis — documented; installed behavior unmeasured |

#### Bounce selection partitions tracks and accepts take-lane targets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-024: A time selection spanning Arrangement tracks produces a separate new audio track for each track with a selected clip. Paste Bounced Audio accepts an audio track, an empty MIDI track or a take lane; the source can be Session clips from one track, Arrangement clips/time, or time on a group’s main lane. | Live 12 local manual, §20.1 / §20.3 pp.419,423; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,3; extend A4-006/A4-009 with explicit selection partition and destination capability checks. Measurement case M6-A6H-024. | Supported hypothesis — documented; installed behavior unmeasured |

#### Comping and automation view exclusion

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-025: Audio and MIDI Arrangement tracks can have any number of take lanes; their main lane is normally audible. Take lanes are hidden during Automation Mode, and Show Take Lanes or Insert Take Lane exits that mode. Left-arrow navigation from a take lane returns to the main lane and folds all its takes. | Live 12 local manual, §21.1 pp.424–425; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,7; arrangement parity: persisted take content is independent of a mutually exclusive editing view. Measurement case M6-A6H-025. | Supported hypothesis — documented; installed behavior unmeasured |

#### Take-lane insertion, bulk edits and deletion scope

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-026: Insert Take Lane operates on one or multiple selected tracks and unfolds existing lanes. Take lanes support duplication, deletion, reordering within their own track, multiselection resizing and simultaneous renaming. Delete All Unused Take Lanes removes lanes that contain no recorded material used in a main-lane comp. | Live 12 local manual, §21.2 pp.426; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3; typed lane operations need stable parent ownership. Unused is defined against the main comp, not simply empty media. Measurement case M6-A6H-026. | Supported hypothesis — documented; installed behavior unmeasured |

#### Recording take-lane reuse and last-take promotion

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-027: Recording over existing clips adds a take lane per pass, while an existing take lane is reused if no other clip exists after the punch-in point. The last recorded clip is copied to the main lane. Recorded clips normally inherit track color; Clip Color Random can assign each take a different color. | Live 12 local manual, §21.3 pp.426–427; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,8; record-pass allocation must inspect existing lane occupancy relative to punch-in, while main-lane promotion is a copy operation. Measurement case M6-A6H-027. | Supported hypothesis — documented; installed behavior unmeasured |

#### Importing directly to take lanes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-028: Samples and MIDI files can be dragged from the browser or OS file manager to take lanes. With multiple samples selected, Ctrl/Cmd during the drag places each into sequential tracks. | Live 12 local manual, §21.4 pp.427; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3; take-lane content is not limited to recordings, and track distribution is a modifier-controlled import policy. Measurement case M6-A6H-028. | Supported hypothesis — documented; installed behavior unmeasured |

#### Multi-track audition chooses one take per track

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-029: Take lanes from different tracks may audition together, but only one lane per track can audition. When a multi-lane time/header selection spans tracks, the last selected lane in each track is the one auditioned. | Live 12 local manual, §21.5 pp.427; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3; extend A-H04 with ordered-selection semantics, not merely a set of selected lane IDs. Measurement case M6-A6H-029. | Supported hypothesis — documented; installed behavior unmeasured |

#### Comp replacement and boundary manipulation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-030: Copy Selection to Main Lane/Enter copies selected take material. Ctrl/Cmd plus Up/Down replaces main-lane material with adjacent takes, skipping empty lanes and moving a take-lane selection along with it. Draw Mode offers drag-to-copy or click-to-replace an existing main-lane time selection. A source-highlight edge can move the split between adjacent comp parts. | Live 12 local manual, §21.6 / §21.7 pp.428–429; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3; define selection, source interval and boundary edit separately; copying remains independent as already documented in A-H04. Measurement case M6-A6H-030. | Supported hypothesis — documented; installed behavior unmeasured |

#### Instrument insertion and implicit arming

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-031: In the documented Auto-monitoring workflow, inserting an instrument on a MIDI track normally activates its Arm automatically so external MIDI can be heard. Double-clicking a device in the browser adds it to a selected track, or creates a new track when none is selected. | Live 12 local manual, §23.2 pp.441–442; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6; parity-7.6 row 8: insertion may affect selection, device state and recording eligibility, not only the chain list. Measurement case M6-A6H-031. | Supported hypothesis — documented; installed behavior unmeasured |

#### Plug-in X-Y field and synchronized editor state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-032: A plug-in panel’s X-Y field assigns two parameters through dropdowns; changes in the floating plug-in editor and Live’s panel affect the same parameters. A temporary touched parameter can become permanent by selecting it for X or Y. | Live 12 local manual, §23.3.1 / §23.3.1.1 / §23.3.1.2 pp.461,463; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0150, ADR-0110: both views share parameter identity; an X-Y gesture may touch two parameter ops within one intended interaction. Measurement case M6-A6H-032. | Supported hypothesis — documented; installed behavior unmeasured |

#### VST preset bank ownership is instance-local

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-033: The manual describes a VST instance owning a fixed-size preset bank; edits change the currently selected preset. It distinguishes that instance-local bank from shared Live device presets and describes program/bank import, export and preset rename. | Live 12 local manual, §23.4.2 pp.467–468; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6; ADR-0110/0142: preserve a plug-in state change as an instance operation. The passage uses legacy VST program/bank terminology and does not prove identical VST3 behavior; ADI does not host VST2. Measurement case M6-A6H-033. | Supported hypothesis — documented; installed behavior unmeasured |

#### Device clocks can remain misaligned despite compensation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-034: Delay compensation includes instruments, effects and return tracks, but the manual warns that tempo-synced devices using Live’s internal clock may sound out of sync when placed after latency-producing devices. Track Delay controls are unavailable when compensation is disabled. | Live 12 local manual, §23.6 pp.469–470; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6,8; ADR-0122: separate signal alignment from transport-clock phase. Extend A3-012 without assuming delay compensation aligns every timing consumer. Measurement case M6-A6H-034. | Supported hypothesis — documented; installed behavior unmeasured |

#### Workflow defaults are distinct assets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-035: Defaults/Dropping Samples differentiates On Drum Rack from On Device View. Slicing defaults are Racks with one empty sampler chain and may include effects/Macro mappings, with multiple slicing presets selectable. Audio-to-MIDI defaults are separated by Drums, Harmony and Melody; drum defaults require a Drum Rack. | Live 12 local manual, §23.2.6 pp.456–458; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §4,6,9: distinguish user-defined import/conversion recipes from the plugin-panel defaults already covered in A5H-027; no built-in sampler DSP is specified here. Measurement case M6-A6H-035. | Supported hypothesis — documented; installed behavior unmeasured |

#### Preset save destination wording needs reconciliation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-036: The Projects and Presets passage says new instrument/effect presets default to the current Project, while Saving Presets describes the Save Preset button saving to the User Library and dragging to a chosen Project. These passages do not establish one unambiguous default for every save gesture. | Live 12 local manual, §5.5.2 / §23.2.5 pp.141–142,455–456; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6,9; retain separate Save-button and drag destinations; do not overwrite the current A5H-026 interpretation with a universal Project-default claim. Measurement case M6-A6H-036. | Unknown — source wording conflict; installed behavior unmeasured |

#### Rack graph topology and nested ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-037: Instrument/Effect Rack chains process the same input in parallel, each chain processing its own devices serially; their outputs are mixed into the Rack output. Drum chains instead receive their assigned note. Racks can contain nested Racks and pass their combined output to following track devices. | Live 12 local manual, §24.1 / §24.1.1 pp.471–472; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0060; FEATURES §6: a Rack owns a subgraph and summing node, not merely a UI folder. Measurement case M6-A6H-037. | Supported hypothesis — documented; installed behavior unmeasured |

#### Rack kind constrains the internal chain

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-038: MIDI Effect Racks accept MIDI effects; Audio Effect Racks accept audio effects and can follow an instrument on a MIDI track. Instrument/Drum Racks place MIDI effects before an instrument and audio effects after it. Drum Racks may also contain up to six audio-effect return chains. Grouping devices or chains again nests another Rack; Ungroup dismantles the selected Rack. | Live 12 local manual, §24.2 pp.473; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0060; FEATURES §6: enforce type rules at each nested edge and preserve external references when grouping/ungrouping. Measurement case M6-A6H-038. | Supported hypothesis — documented; installed behavior unmeasured |

#### Rack views and chain selection are navigational state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-039: Rack Macro, Chain List and Devices views can be hidden independently; Drum Racks add Pad View. Hiding all views folds the Rack to its narrow title. A hierarchical Device View selector menu jumps to nested devices. Selecting a chain determines the visible device chain; multiselection displays the number of selected chains. | Live 12 local manual, §24.3 / §24.4 pp.474–476; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6,9; parity-7.4: maintain view/selection paths separately from device topology and hidden state. Measurement case M6-A6H-039. | Supported hypothesis — documented; installed behavior unmeasured |

#### Chain presets and cross-track drag

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-040: Chains have their own activator, solo and Hot-Swap controls; audio-producing Rack chains also have volume and pan. Entire chains can be saved/recalled as presets and carry name, info text and color. Dragging a chain over another track focuses that track and exposes its Device View for insertion. | Live 12 local manual, §24.4 pp.475–476; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6; ADR-0060/0127: a chain preset includes nested state and media dependencies; drag navigation should not itself commit a move. Measurement case M6-A6H-040. | Supported hypothesis — documented; installed behavior unmeasured |

#### Rack Auto Select responds to signal eligibility

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-041: With Auto Select enabled, chains currently processing signals become selected. Drum Racks select a chain on its assigned note; Instrument/Effect Racks use their zones to determine which chains participate. | Live 12 local manual, §24.4.1 pp.476–477; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6,9; ADR-0181: automatic signal-follow selection and explicit user selection need an ownership policy. Measurement case M6-A6H-041. | Supported hypothesis — documented; installed behavior unmeasured |

#### Zone eligibility is a conjunction

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-042: Instrument/MIDI Effect Rack chains test key, velocity and chain-select zones before forwarding a note; all applicable zones must pass. Audio Effect Racks have no key/velocity zones, and Drum Racks use note-routing choosers instead of zones. Key/velocity fade ranges attenuate incoming note velocity. | Live 12 local manual, §24.5 / §24.5.1 / §24.5.2 / §24.5.3 pp.477–478; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0060; FEATURES §6: distinguish event admission from velocity scaling; extend A3-026 with the all-zones eligibility rule. Measurement case M6-A6H-042. | Supported hypothesis — documented; installed behavior unmeasured |

#### Chain-select bank ranges and tail policy

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-043: Chain-select zones use 0–127 with a default one-value zone at 0. Disjoint ranges create exclusive preset banks, while overlapping fade ranges crossfade banks. The already documented no-fade versus fade tail behavior remains part of this topology. | Live 12 local manual, §24.5.4 / §24.5.4.1 / §24.5.4.2 pp.479–481; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0060; A3-027: a selector changes admissible routes, not necessarily device lifetime. Bank layout is data distinct from the current selector. Measurement case M6-A6H-043. | Supported hypothesis — documented; installed behavior unmeasured |

#### Drum route Receive, Play and choke rules

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-044: Drum chains receive one assigned MIDI note and may translate it to a different Play note. Sixteen choke groups let a triggered chain silence others in its group. Receive All Notes disables Play and Choke and passes incoming notes unchanged; a Preview control injects a test note. | Live 12 local manual, §24.6 pp.481; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6; ADR-0060: distinguish note matching, note remapping and voice cancellation at the container boundary; this is host routing, not sampler DSP. Measurement case M6-A6H-044. | Supported hypothesis — documented; installed behavior unmeasured |

#### Drum Rack internal returns can leave the Rack

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-045: Drum-chain sends are post-fader and appear only after internal returns exist. Up to six internal audio-effect returns are supported; a return chain can output to the Rack’s main output or directly to a Set return track. | Live 12 local manual, §24.6 pp.481–482; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,6; ADR-0060: nested return routes can cross a container boundary and must be represented in the global dependency graph. Measurement case M6-A6H-045. | Supported hypothesis — documented; installed behavior unmeasured |

#### Drum pad is a note address, not one chain

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-046: Each pad represents a MIDI note and all nested chains that can receive it. A single-chain pad supports that chain’s name/mute/solo/preview/Hot-Swap; a Multi pad controls all matching chains, displays mixed mute/solo states, disables rename/Hot-Swap, and can delete all those chains together. | Live 12 local manual, §24.6.1 pp.482–483; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6; ADR-0060: pad identity and chain identity must remain distinct in selection, persistence and compound deletion. Measurement case M6-A6H-046. | Supported hypothesis — documented; installed behavior unmeasured |

#### Pad moves swap mappings without rewriting clips

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-047: Dragging a Drum Rack pad to another swaps their note mappings, so existing MIDI clips can trigger different sounds. Alt/Cmd-drag layers their chains in a nested Instrument Rack. Multiple chains assigned the same Receive note are all triggered by that pad. | Live 12 local manual, §24.6.1 pp.483; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6; ADR-0060: a pad remapping is not a MIDI-note transpose; distinguish mapping swap from chain extraction that carries notes. Measurement case M6-A6H-047. | Supported hypothesis — documented; installed behavior unmeasured |

#### Pad content replacement preserves surrounding effects

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-048: Dropping another sample onto a Drum Rack pad containing Simpler replaces that Simpler and sample while retaining upstream MIDI effects and downstream audio effects. Multiple samples fill chromatically upward, replacing affected samples; Alt/Cmd-drag layers the selection onto one pad through a nested Instrument Rack. | Live 12 local manual, §24.6.1 pp.482–483; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6,9: browser drop target semantics can replace a nested source while preserving its surrounding chain. Built-in sampler implementation is outside this scope. Measurement case M6-A6H-048. | Supported hypothesis — documented; installed behavior unmeasured |

#### Pad viewport controls supported hardware note mapping

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-049: The Drum Rack overview moves in groups of sixteen pads, or by single rows with its modifier. A natively supported pad surface assigned in Settings follows the visible pad region when the Rack is on a MIDI-receiving track. | Live 12 local manual, §24.6.1 pp.482–483; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0181; FEATURES §9: a viewport may change controller targeting even when the musical clip is unchanged; separate generic MIDI input from supported surface scripts. Measurement case M6-A6H-049. | Supported hypothesis — documented; installed behavior unmeasured |

#### Macro visibility, randomization default and recall objects

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-050: Racks support up to sixteen Macros, with eight shown initially; shown/hidden state is saved in the Set. The manual specifically excludes Volume-mapped Macros in Instrument Rack presets from randomization by default. New stores a numbered variation, while launch and overwrite are separate actions; per-Macro randomization and variation exclusions are already covered in A3-029. | Live 12 local manual, §24.7 / §24.7.2 / §24.7.3 pp.484,486–488; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6; ADR-0060/0114: hidden controls can still own mappings; default exclusion is narrower than all volume controls in all user-created Racks. Measurement case M6-A6H-050. | Supported hypothesis — documented; installed behavior unmeasured |

#### Macro ownership still permits clip modulation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-051: A Macro-mapped device parameter becomes directly disabled because the Macro owns its control, but the manual still permits external modulation via clip envelopes. A Macro retains real units for multiple mappings only when every target has both the same unit type and range; otherwise its display uses the generic 0–127 scale. | Live 12 local manual, §24.7.1 pp.485; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6; ADR-0114: this sharpens A3-028 while preserving ADI’s approved multi-source summation/curves, which are not established by Live’s disabled control. Measurement case M6-A6H-051. | Supported hypothesis — documented; installed behavior unmeasured |

#### Rack mixer multiselection differs by editing surface

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-052: Instrument/Drum Racks with multiple chains can unfold into the Session mixer; chain controls there mirror the chain list. Multiselected chains receive the same mixer-parameter edit only when the edit occurs in the Session mixer, not when adjusting that parameter in the Rack chain list. Dragging an internal Drum Rack return into the mixer creates a new Set return track. | Live 12 local manual, §24.8 / §24.8.1 pp.488–490; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,6; ADR-0060: mirrored values do not imply identical selection propagation. Return extraction is a container-to-project route migration. Measurement case M6-A6H-052. | Supported hypothesis — documented; installed behavior unmeasured |

#### Browser full-height actions have different panel side effects

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-053: Dragging the browser’s bottom edge down automatically closes Info View and the active Clip/Device View. Selecting Full-Height Browser from the View menu instead expands to the bottom while keeping those views open. | Live 12 local manual, §4 pp.61; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §9; shell/browser parity: distinguish a sizing gesture from a named layout mode even when the resulting browser height looks similar. Measurement case M6-A6H-053. | Supported hypothesis — documented; installed behavior unmeasured |

#### Browser label identity and grouping are configurable

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-054: Factory and custom Library labels can be renamed and given different icons. Library labels are content-type views, while Places lists storage/services; for example Sounds collects instrument sound presets, Instruments is device-oriented, and Modulators also includes devices with the corresponding tag. | Live 12 local manual, §4.2.1 / §4.6 / §4.7 pp.68,81–84; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0104; FEATURES §9: store category identity independently from displayed label, and do not treat a category as a filesystem directory. Measurement case M6-A6H-054. | Supported hypothesis — documented; installed behavior unmeasured |

#### Pack download and installation are separate states

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-055: Packs distinguishes installed content, Updates and Available Packs. Downloads can pause, resume or cancel; selecting multiple Packs supports starting, pausing and resuming together. A completed download requires a separate Install action with progress. Pack sizes can be displayed. Deleting an installed Pack makes it available again, and Library Settings can hide downloadable sections. | Live 12 local manual, §4.7.1 / §4.7.2 pp.84–89; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §9; asset-management research only: a download queue, installed catalog and referenced media need separate state machines; ADI content-service integration is not approved by this evidence. Measurement case M6-A6H-055. | Supported hypothesis — documented; installed behavior unmeasured |

#### Cloud/service availability and local assets are separate

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-056: The local manual documents Cloud as Move/Note-to-Live Set transfer, not reverse sync, with factory/imported assets stored in User Library. Missing assets may mean a custom library drive is unavailable; Collect All and Save can make further Live work into a local Project. | Live 12 local manual, §4.7.4 / §4.7.6.1 pp.103–105,109; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §10; ADR-0127/0182: useful transfer-versus-collaboration distinction. This is documentary contrast, not a proposal to clone Ableton Cloud or a verified current service limit. Measurement case M6-A6H-056. | Supported hypothesis — documented; installed behavior unmeasured |

#### Standalone transfer has capability preflight

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-057: The manual’s Push workflow pairs over the same Wi-Fi using a six-digit code, exposes Projects and User Library, and refreshes by leaving/reentering its browser label. Returning Sets to Push requires native devices, freezing plug-in tracks and collecting samples; disconnecting the label unpairs it. | Live 12 local manual, §4.7.5 pp.105–107; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §9–10; ADR-0127: interoperability preflight should disclose unsupported devices and missing assets before transfer. No Push integration is inferred for ADI. Measurement case M6-A6H-057. | Supported hypothesis — documented; installed behavior unmeasured |

#### Splice account and query scope

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-058: In the local manual’s Splice integration, browsing/previewing is available without login, while adding, saving and Search with Sound require login via matching device codes in the OS browser and Live. Active internet is required; searching within a chosen category preserves that category scope. | Live 12 local manual, §4.7.3 / §4.7.3.1 / §4.7.3.2 / §4.7.3.3 pp.89–98; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §9; ADR-0104: distinguish local browser search from a remote provider’s query/session and entitlement state. No external-content integration or purchase is authorized or required by this research. Measurement case M6-A6H-058. | Supported hypothesis — documented; installed behavior unmeasured |

#### Remote sound search requires an explicit reference

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-059: The documented Search with Sound accepts a dragged clip or captured time selection, processes that reference and returns compatible samples; the results retain replay/replace/delete controls for the reference. This is separate from Live’s local similarity search. | Live 12 local manual, §4.7.3.2 pp.97–98; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0104/ADR-0064: distinguish local analysis from provider processing and clearly represent the reference being submitted. Existing ADI service/local-processing boundaries in ADR-0064 as amended by ADR-0203 d8 remain authoritative. Measurement case M6-A6H-059. | Supported hypothesis — documented; installed behavior unmeasured |

#### Remote preview and insertion preserve audible intent differently

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-060: Splice preview has its own loop, timestretch, key/scale and loudness controls. The manual says inserted Splice clips use Complex Warp regardless of the normal default to match preview; adding a sample also licenses/saves it, and downloads may target User Library, Current Project or a custom folder. | Live 12 local manual, §4.7.3.3 pp.99–101; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,9; ADR-0203: ADI’s Warp-off import rule remains approved. Separate preview transform, committed clip processing, entitlement and media destination if a provider is ever added. Measurement case M6-A6H-060. | Supported hypothesis — documented; installed behavior unmeasured |

#### Remote library metadata is not project content

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-061: The documented Splice library holds likes, collections and downloaded samples. Collections support name/description, private/shared status and deletion; provider Settings separately manage subscription/display/usage logs and license information. | Live 12 local manual, §4.7.3.4 / §4.7.3.5 pp.101–103; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §9–10: provider account metadata, local asset identity and project references are separate stores; retain as comparative research only. Measurement case M6-A6H-061. | Supported hypothesis — documented; installed behavior unmeasured |

#### User Library has independent location and lifecycle

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-062: User Library is separate from installed content, can be relocated in Library Settings, and stores presets/defaults/clips/templates and other reusable assets. Its initial folders may be customized or removed; saved items also appear in matching Library category views. | Live 12 local manual, §4.7.6 pp.108–111; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0104; FEATURES §9: app-level library ownership survives project closure and must not depend on a fixed default folder layout. Measurement case M6-A6H-062. | Supported hypothesis — documented; installed behavior unmeasured |

#### Sample analysis cache versus saved clip defaults

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-063: A sample’s adjacent .asd file caches analysis and may also store Clip View Save defaults such as warp settings. Individual clips referencing that sample can still differ; new imports use saved defaults. Disabling Create Analysis Files suppresses sidecars; lost analysis can be regenerated, but saved default clip settings cannot. | Live 12 local manual, §5.1.2 pp.121–122; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,8,9; ADR-0104/0127: derived cache and authored defaults have different recovery requirements, even if the reference stores them together. Measurement case M6-A6H-063. | Supported hypothesis — documented; installed behavior unmeasured |

#### Analysis can gate first playback

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-064: Uncompressed mono/stereo samples may mix sample rates and bit depths without prior conversion. A long sample may need analysis before playback, unless it is already analyzed or Auto-Warp Long Samples is off; compressed samples decode into temporary uncompressed media. | Live 12 local manual, §5.1 / §5.1.2 pp.120–121; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,8; ADR-0104: import/readiness is asynchronous and distinct from clip creation; extend A5H-015 cache policy with a playback-readiness state. Measurement case M6-A6H-064. | Supported hypothesis — documented; installed behavior unmeasured |

#### Live Clip recall depends on destination occupancy

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-065: A Live Clip stores clip/envelope settings and its original track’s devices, while audio content remains externally referenced. Dropping onto a track with no clips or devices, or into a new-track drop area, reconstructs the device chain; a destination already containing clips or devices receives clip settings without those devices. | Live 12 local manual, §5.3 pp.129–130; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,6,9; ADR-0127: a reusable clip asset has optional chain state whose application depends on target occupancy, not merely target type. Measurement case M6-A6H-065. | Supported hypothesis — documented; installed behavior unmeasured |

#### Templates instantiate an untitled Set

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-066: Template Sets preserve configuration such as multichannel I/O, devices and key/MIDI mappings. Save As Template places them in User Library/Templates and the browser’s Templates category; loading creates Untitled.als rather than opening the template as the working named Set. A template can be chosen as the default. | Live 12 local manual, §5.4.5 pp.134–135; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §9; distinguish a template asset, user-selected default and new working project identity. Do not derive factory defaults from user templates. Measurement case M6-A6H-066. | Supported hypothesis — documented; installed behavior unmeasured |

#### Set hierarchy exposes reusable subresources

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-067: An expanded Set in the browser exposes tracks, nested groups, Session clips, grooves and a Devices entry for importing only a chain. Current Project also exposes Return/Main track resources. Whole-Set merging still excludes returns as documented in A4-011; visibility alone does not establish every resource’s drag behavior. | Live 12 local manual, §5.4.3 / §4.7.7 pp.131–134,112; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §6,9; model browsing a saved document as structured resources, preserving the distinction between visible and importable. Measurement case M6-A6H-067. | Supported hypothesis — documented; installed behavior unmeasured |

#### Save As can create a project while retaining external dependencies

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-068: Saving outside an existing Project creates a new Project, but media can remain referenced from the old Project until collected. Multiple Set versions within one Project share the media collection; opening a contained Set also selects its Project. A Project may contain subfolders, with relinking needed after external reorganization. | Live 12 local manual, §5.5.1 / §5.10.1 / §5.10.3 / §5.10.4 / §5.10.5 pp.137–141,158–159; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0127; FEATURES §8,10: a new document path does not imply a self-contained media copy; distinguish shared version storage from portable export. Measurement case M6-A6H-068. | Supported hypothesis — documented; installed behavior unmeasured |

#### Older-major Sets require a new save target

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-069: The manual says Sets created by an older major Live version cannot be overwritten directly; Save As is requested to preserve compatibility. | Live 12 local manual, §5.5.1 pp.141; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §8,10; persistence/versioning: use an explicit migration/save boundary rather than silently upgrading the only original. Measurement case M6-A6H-069. | Supported hypothesis — documented; installed behavior unmeasured |

#### Collection scope is categorized and explicit

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-070: Files outside a Project—including Packs, User Library and other Projects—count as external. Collection offers location categories with counts/sizes and lets the user choose which to copy; audio goes under Samples and devices under Presets. Manage User Library/Project can repair and collect across their aggregate contents, then Collect and Save confirms changes. | Live 12 local manual, §5.7 / §5.7.2 pp.145–150; exact manual revision unconfirmed; installed 12.4 not tested | ADR-0127; FEATURES §8,9,10: resolve a dependency manifest by scope before copying; a category excluded from collection remains an external dependency. Measurement case M6-A6H-070. | Supported hypothesis — documented; installed behavior unmeasured |

#### Export-to-browser has a deferred commit boundary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-071: With Collect Files on Export set to Ask, Copy/Don’t Copy proceeds to naming; dependency copying commits when the name is confirmed. Individual clips export as .alc; §5.4.4 separately allows a multi-selection of Session clips to export as a new .als. The one-clip wording in §5.7.1 is not a universal multi-clip prohibition. Tracks may export in multiples; collection creates a Project folder. | Live 12 local manual, §5.7.1 pp.147–148; §5.4.4 p.134; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §9; ADR-0127: refine A3-022 into asset creation, naming and dependency-copy stages; external file operations need defined cancel/failure behavior distinct from project undo. Measurement case M6-A6H-071. | Supported hypothesis — documented; installed behavior unmeasured |

#### Video export depends on audio export

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-072: Create Video is enabled only when Arrangement contains video, and video export always also renders audio. Audio rendering precedes video rendering; available encoders and their settings depend on installation, and an encoder without configurable options disables its settings button. | Live 12 local manual, §5.1.3.4 pp.126; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §3,8 video P2; ADR-0206 d4: export eligibility and multi-stage job progress are capability-driven, not a fixed codec list. Measurement case M6-A6H-072. | Supported hypothesis — documented; installed behavior unmeasured |

#### Real-time render retry has explicit attempt state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6H-073: When hardware routing requires real-time rendering, Live first waits ten seconds by default for external devices to become silent; that wait is editable or skippable. During rendering, drop-outs can automatically restart from the beginning; manual Restart and an attempt count are exposed. Cancel deletes the partially rendered file. | Live 12 local manual, §5.1.3.5 pp.126–127; exact manual revision unconfirmed; installed 12.4 not tested | FEATURES §2,8; extends A3-031: external-device settling and full-job restart are separate phases; no claim that hardware state itself rewinds deterministically. Measurement case M6-A6H-073. | Supported hypothesis — documented; installed behavior unmeasured |

### Controllers, video, accessibility, settings and learning

#### Remote control is a typed input layer

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6R-001: Manual clip-launch mappings bind Session slots rather than the clips currently occupying them. Assigned MIDI notes cease feeding MIDI tracks because remote control consumes them. | Live 12 local manual, §§33,33.1; pp.836; exact revision unconfirmed against installed 12.4 | FEATURES §9; controller_maps must distinguish slot identity from clip identity and remote consumption from instrument input. Measurement case M6-A6R-001. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-002: The guide allows six supported surface instances; instant mappings follow device selection and can be manually overridden. Supported devices can be locked to a surface, but not every surface supports locking. | Live 12 local manual, §§33.1.1,33.1.1.1; pp.837–838; exact revision unconfirmed against installed 12.4 | ADR-0181: selection-follow and explicit pin require separate ownership and capability checks. Six is a reference limit, not an ADI requirement. Measurement case M6-A6R-002. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-003: Any number of Remote-enabled MIDI input ports can contribute merged mapping input. Hardware feedback requires the output port Remote switch as well as input enablement. | Live 12 local manual, §§33.1.2; pp.838–839; exact revision unconfirmed against installed 12.4 | FEATURES §9: incoming bindings, outbound feedback and port availability are separate routes. The text does not establish whether port identity participates in binding lookup. Measurement case M6-A6R-003. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-004: None applies the first absolute value immediately; Pick-Up waits for the hardware to reach the target; Value Scaling converges before switching to direct tracking. The numerical convergence rule is unstated. | Live 12 local manual, §§33.1.3; pp.839–840; exact revision unconfirmed against installed 12.4 | ADR-0188: preserve takeover mode and ownership transitions; do not invent a scaling equation from the description. Measurement case M6-A6R-004. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-005: The Mapping Browser shows manual MIDI, key or Macro mappings for the active mode, with target path and editable Min/Max. Inversion is explicit; contextual instant mappings are absent from this list. | Live 12 local manual, §§33.2; pp.840; exact revision unconfirmed against installed 12.4 | FEATURES §9/6: mapping provenance, displayed path and stable target identity are separate. Measurement case M6-A6R-005. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-006: Mapped notes use target-specific actions: clip launch follows launch mode, switches toggle on Note On, radio choices cycle, and a continuous target toggles between bounds unless a note range distributes discrete values. | Live 12 local manual, §§33.2.2; pp.841; exact revision unconfirmed against installed 12.4 | FEATURES §9: retain event type and target kind rather than one generic setValue operation. Measurement case M6-A6R-006. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-007: Chromatic slot mapping uses a held root note followed by lower and upper limit notes; the root retains the clip’s default transposition. | Live 12 local manual, §§33.2.2; pp.841; exact revision unconfirmed against installed 12.4 | FEATURES §5/9: root/range mapping is independent of stored clip pitch and slot ownership. Measurement case M6-A6R-007. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-008: Absolute CC values 64–127 act as Note On for mapped slots, and 0–63 as Note Off. Track/device activators instead use membership in the mapping range; inverted bounds reverse that membership rule. | Live 12 local manual, §§33.2.3; pp.841–842; exact revision unconfirmed against installed 12.4 | FEATURES §9: boolean mapping needs an interval predicate distinct from the fixed launch threshold. Measurement case M6-A6R-008. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-009: Pitch bend and 14-bit Absolute messages are supported over 0–16383 with the split at 8191/8192. Radio and continuous targets map across their respective ranges. | Live 12 local manual, §§33.2.3; pp.842; exact revision unconfirmed against installed 12.4 | FEATURES §9: decode source precision before target conversion; resolution support does not prove sample-accurate scheduling. Measurement case M6-A6R-009. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-010: Relative modes include Signed Bit, Signed Bit 2, Binary Offset and Two’s Complement, each with a linear variant. Detection uses received motion and permits manual correction; moving slowly left is the documented setup gesture. | Live 12 local manual, §§33.2.4; pp.842–843; exact revision unconfirmed against installed 12.4 | ADR-0181: decoding and acceleration are distinct from undoable absolute edits. Measurement case M6-A6R-010. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-011: Relative increments trigger mapped slots and turn switches on; decrements release slots and turn switches off. Radio controls step through choices. Continuous targets interpret increments using the selected protocol. | Live 12 local manual, §§33.2.4; pp.842–843; exact revision unconfirmed against installed 12.4 | FEATURES §9: relative encoder messages are not equivalent to absolute thresholds. Measurement case M6-A6R-011. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-012: Relative Session mapping keeps the highlighted scene centered and exposes a separate cancellation target for a queued scene launch. Selection, pending launch and playing scene are distinct states. | Live 12 local manual, §§33.2.4.1; pp.843; exact revision unconfirmed against installed 12.4 | FEATURES §5: navigation and queued performance actions must not share one state variable. Measurement case M6-A6R-012. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-013: The settings overview separates Link from Tempo & MIDI, while the controller chapter still names a combined Link, Tempo & MIDI tab. This is an internal naming/version inconsistency. | Live 12 local manual, §§33.1,2.3.4,2.3.5; pp.31,836–839; exact revision unconfirmed against installed 12.4 | FEATURES §14/parity-settings: verify installed tab names/order; do not use the older chapter label as an exact UI string. Measurement case M6-A6R-013. | Unknown / not tested — conflicting documentary labels; no installed UI observation |

#### Controller workflows expose host operation scope

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6R-014: Surface browsing is filtered by the selected device/track type. Push 1 hides folders without loadable items, while Push 2 provides load-next/previous behavior for pad replacement. | Live 12 local manual, §§34.2,35.2,35.3.4; pp.846–847,886–888,892; exact revision unconfirmed against installed 12.4 | FEATURES §9/6: browsing capability and insertion destination belong to the active target context. Measurement case M6-A6R-014. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-015: Duplicating a drum pad replaces the destination devices but preserves recorded destination notes. Pad content and note events have separate ownership. | Live 12 local manual, §§34.3.4.1,35.3.4.1; pp.850,893–894; exact revision unconfirmed against installed 12.4 | FEATURES §6/4: expose pad-device copy independently of note copy. Measurement case M6-A6R-015. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-016: A sequencer tap creates or deletes a step and starts clip playback; Mute deactivates a step without deleting it. Triplet resolution leaves two columns per row unavailable. | Live 12 local manual, §§34.3.5,35.3.5; pp.850–852,895–897; exact revision unconfirmed against installed 12.4 | FEATURES §4/5/9: step enable, note existence, grid projection and transport start are separate operations. Measurement case M6-A6R-016. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-017: Push 2 Delete plus a drum pad removes that pad’s notes; when no notes for that pad are recorded in the current pattern, the same gesture deletes its devices. Arrangement focus also changes the target of plain Delete. | Live 12 local manual, §§35.3.5,35.17; pp.897,939; exact revision unconfirmed against installed 12.4 | FEATURES §9: destructive commands need explicit context resolution and visible target identity. Measurement case M6-A6R-017. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-018: Record cycles from recording to playing to overdubbing. New stops the selected clip and prepares another; Scene Workflow also preserves playing material on other tracks in a new scene. | Live 12 local manual, §§34.3.6,35.3.6; pp.853,899–900; exact revision unconfirmed against installed 12.4 | FEATURES §5/8: record state and new-idea construction should be independent commands with defined scope. Measurement case M6-A6R-018. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-019: Enabling Fixed Length during an ongoing recording stops recording and loops the latest configured span. Push 2 Phrase Sync can start a new fixed-length clip at its current phrase phase rather than at the clip beginning. | Live 12 local manual, §§34.3.7,35.3.7; pp.854–855,900–901; exact revision unconfirmed against installed 12.4 | FEATURES §5/8: retained capture range, clip origin and performance phase require different timestamps. Measurement case M6-A6R-019. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-020: Repeat retains its enabled state and division per track. Pressure alters repeated-note dynamics; a quick press latches Repeat, whereas holding it enables it temporarily. | Live 12 local manual, §§34.4.1,35.4.1; pp.855–856,901–902; exact revision unconfirmed against installed 12.4 | FEATURES §4/9: performance state can be track-local; physical hold state must not become project note data. Measurement case M6-A6R-020. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-021: Accent forces played and step-sequenced notes to velocity 127, including overriding the 16-velocity pad choice. It supports latched and momentary use. | Live 12 local manual, §§35.3.6; pp.899–900; exact revision unconfirmed against installed 12.4 | FEATURES §4/9: define precedence between raw velocity, selected fixed velocity and accent override. Measurement case M6-A6R-021. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-022: Push 2’s detailed quantization procedure says recorded quantization does not apply the Swing amount; its control summary says the Swing encoder affects Record Quantizing. Preserve the inconsistency. | Live 12 local manual, §§35.4.2,35.17; pp.903,938; exact revision unconfirmed against installed 12.4 | FEATURES §4/8: distinguish recording quantization from subsequent destructive quantization. Measurement case M6-A6R-022. | Unknown / not tested — contradictory detailed and summary passages |
| A6R-023: Push 2 Record targets Arrangement when Arrangement is focused and Session when Session is focused; Shift reverses that target. | Live 12 local manual, §§35.4.3,35.17; pp.903,940; exact revision unconfirmed against installed 12.4 | FEATURES §8/9: focus is part of command routing, not audio playback ownership. Measurement case M6-A6R-023. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-024: Adding a track while a child of a group is selected inserts the new track into that group. Surface Add Track can combine choosing a track type with loading its first device. | Live 12 local manual, §§34.5,35.5; pp.857,904; exact revision unconfirmed against installed 12.4 | FEATURES §2/9: use explicit insertion parent and compound-operation rollback. Measurement case M6-A6R-024. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-025: Scale layout includes fixed C (or nearest in-key note) anchoring versus shifting to the chosen root, chromatic versus in-key notes, rotated thirds/fourths and sequential layouts. Scale options are saved with the Set. | Live 12 local manual, §§34.5.1,35.5.1; pp.859–860,906–907; exact revision unconfirmed against installed 12.4 | FEATURES §4/9: performance layout metadata is distinct from changing existing MIDI notes or project tuning. Measurement case M6-A6R-025. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-026: Loop-selector visibility is remembered per track. Step-sequencer pitch-range indicators distinguish currently visible notes from other pitches containing notes. | Live 12 local manual, §§34.6,35.6; pp.861,908; exact revision unconfirmed against installed 12.4 | FEATURES §4/9: preserve editor viewport separately from clip content and playing range. Measurement case M6-A6R-026. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-027: Selecting a page inside the loop pins the editing view without changing the loop; selecting a page outside the loop changes the loop. Reselecting the loop restores follow; Push 2 also restores follow by holding a Page button. | Live 12 local manual, §§34.6.1,35.6.1; pp.862–863,909–910; exact revision unconfirmed against installed 12.4 | FEATURES §4/9: identical navigation gestures have conditional data-edit effects that need clear feedback. Measurement case M6-A6R-027. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-028: Sequencer-page duplication adds copied notes to destination content instead of clearing it. The combined melodic layout inserts the selected pitch set into one or multiple held steps. | Live 12 local manual, §§34.6.1,34.7.2,35.6.1,35.7.2; pp.863–864,910–913; exact revision unconfirmed against installed 12.4 | FEATURES §4: merge, replace and set-of-pitches insertion must be distinct operations. Measurement case M6-A6R-028. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-029: Selecting a MIDI track on Push implicitly arms it; Push 2 also supports explicit track arming. Note-mode up/down navigation launches immediately and carries the previous clip position, according to Scene versus Clip Workflow. | Live 12 local manual, §§34.8,35.9; pp.865,919–920; exact revision unconfirmed against installed 12.4 | FEATURES §5/8/9: implicit arm, explicit arm, focus and launch state need separate fields. L06 glyph measurement remains open. Measurement case M6-A6R-029. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-030: Split Stereo Pan cannot be edited in the global pan-bank view, although its value is shown. Track-specific mode exposes its separate stereo controls. Push 2 pages through sends beyond the first six. | Live 12 local manual, §§34.10,35.11; pp.867,926–927; exact revision unconfirmed against installed 12.4 | FEATURES §2/9: controller target availability depends on pan mode and page; missing controls must not silently switch modes. Measurement case M6-A6R-030. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-031: Delete plus an encoder deletes recorded automation when present but resets the parameter when no automation exists. Shift plus Automation re-enables all manually overridden automation. | Live 12 local manual, §§34.11,35.12; pp.868,928–929; exact revision unconfirmed against installed 12.4 | FEATURES §7/9: envelope deletion, parameter reset and global override reset are different commands. Measurement case M6-A6R-031. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-032: Held sequencer steps expose note nudge as a percentage from the preceding grid line, coarse/fine duration and velocity; editing an empty held step can create notes. Multi-step displays show a value range. | Live 12 local manual, §§34.12.1,35.14.4; pp.869–870,934; exact revision unconfirmed against installed 12.4 | FEATURES §4: define reference grid, selected-note set and create-on-edit semantics. Measurement case M6-A6R-032. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-033: Per-step device/mixer automation applies over the time represented by held steps and can be created on steps with no notes. | Live 12 local manual, §§34.12.2,35.13; pp.870,929–930; exact revision unconfirmed against installed 12.4 | FEATURES §7: automation ownership is a time interval independent of note existence. Measurement case M6-A6R-033. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-034: Session/Note modes can be held temporarily; software reflects the hardware’s 8×8 controlled region. Selecting a clip with Select avoids launch. Surface navigation and audible content are independent. | Live 12 local manual, §§34.13,35.15; pp.870–872,934–936; exact revision unconfirmed against installed 12.4 | FEATURES §5/9: maintain a controller viewport and non-performing selection path. Measurement case M6-A6R-034. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-035: Session Overview maps each pad to an 8×8 region and uses scene buttons for larger blocks. Its selected-block indicator is distinct from blocks containing playing clips. | Live 12 local manual, §§34.13.1,35.15.1; pp.871–872,936; exact revision unconfirmed against installed 12.4 | FEATURES §5/9: aggregate selected, playing and absent states separately. Controller colors are not the UI palette RGB. Measurement case M6-A6R-035. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-036: Scene Workflow Duplicate captures all playing clips into a new scene; New does so except for the selected track. Clip Workflow confines ordinary Duplicate/New to the selected track; Shift+Duplicate captures a scene. | Live 12 local manual, §§34.14,35.16; pp.873–874,938; exact revision unconfirmed against installed 12.4 | FEATURES §5/9: one physical command resolves to either clip-local or cross-track snapshot operations. Measurement case M6-A6R-036. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-037: Surface tempo encoders use 1 BPM steps and 0.1 BPM with Shift; the main encoder becomes cue volume with Shift. Fine control is a mode-dependent interpretation of the same input. | Live 12 local manual, §§34.15.0.7,35.17; pp.879,938,941; exact revision unconfirmed against installed 12.4 | FEATURES §1/2/9: use parameter units and explicit modifier routing, with grouped edit gestures under ADR-0181. Measurement case M6-A6R-037. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-038: Surface Undo uses the application’s shared history, including mouse edits. Quantize targets selected notes, or the whole clip if none are selected; audio quantization targets transients. | Live 12 local manual, §§34.15.0.8,35.17; pp.880,939–940; exact revision unconfirmed against installed 12.4 | FEATURES §4/7/9: origin attribution does not imply a private controller undo stack. Measurement case M6-A6R-038. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-039: A second momentary footswitch controls Record with one tap and New with a quick double tap. The first acts as sustain; polarity can differ by pedal. No double-tap timing window is specified. | Live 12 local manual, §§34.15.0.13,35.17.0.1; pp.884,943; exact revision unconfirmed against installed 12.4 | FEATURES §8/9: distinguish debouncing, single/double gesture arbitration and sustain event state. Measurement case M6-A6R-039. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-040: Push 2 custom drum-pad colors persist in the Set but appear only on the hardware, not in Live’s visible drum pads. | Live 12 local manual, §§35.3.4.1; pp.894; exact revision unconfirmed against installed 12.4 | FEATURES §9; ADR-0211: surface presentation metadata must not be mistaken for the desktop clip/track palette. Measurement case M6-A6R-040. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-041: Surface device insertion respects MIDI-before-instrument and audio-after-instrument ordering; adding an instrument can replace the existing one. Effect reordering is a held selection plus encoder gesture. | Live 12 local manual, §§35.10.1; pp.922–923; exact revision unconfirmed against installed 12.4 | FEATURES §6/9: typed graph validation and replacement scope must apply consistently across input surfaces. Measurement case M6-A6R-041. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-042: Device Mode can unfold ordinary racks to their selected chain; the guide says Drum Rack device folding follows Live instead. Mixer expansion of a track containing a rack is a separate operation that exposes chains. | Live 12 local manual, §§35.10.2,35.11.1; pp.923–925,927–928; exact revision unconfirmed against installed 12.4 | FEATURES §6/9: rack-device view expansion and mixer chain expansion require separate view identities. Measurement case M6-A6R-042. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-043: In Push 2 Clip Mode, moving Loop Position carries Start Offset only when both were aligned. Position controls use sixteenth subdivisions with Shift; zoom centers on the most recently touched position control. | Live 12 local manual, §§35.14; pp.930–931; exact revision unconfirmed against installed 12.4 | FEATURES §3/9: marker linkage is conditional; viewport anchoring should not rewrite marker state. Measurement case M6-A6R-043. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-044: Push 2 MIDI Crop removes material outside the loop; its real-time display folds to show all notes while sequencer mode shows an editable time/pitch region. | Live 12 local manual, §§35.14.1,35.14.2,35.14.3; pp.932–933; exact revision unconfirmed against installed 12.4 | FEATURES §4/9: cropping is destructive clip editing while fit-to-content is a view operation. Measurement case M6-A6R-044. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-045: Push 2 documents sensitivity, gain and dynamics defaults of 5, a linear curve at gain 4/dynamics 7, and 100% display/LED brightness. Low LED brightness can alter perceived colors. | Live 12 local manual, §§35.16; pp.937–938; exact revision unconfirmed against installed 12.4 | FEATURES §9: distinguish device calibration defaults from application settings and use numeric evidence for color research. Measurement case M6-A6R-045. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-046: User Mode disables built-in surface behavior for custom assignments; the recommended relative encoder interpretation is Two’s Complement. It is a mode change, not removal of the connected MIDI device. | Live 12 local manual, §§34.15.0.3,35.17; pp.876,941; exact revision unconfirmed against installed 12.4 | FEATURES §9: switching surface modes must release owned actions and prevent double handling. Measurement case M6-A6R-046. | Supported hypothesis — documented; installed behavior unmeasured |

#### Video, media identity and time domains

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6R-047: The local guide describes MOV import; picture is displayed for Arrangement video clips, while the same movie in Session is treated as audio. Codec/OS support is not established by the extension alone. | Live 12 local manual, §§27.1; pp.517; exact revision unconfirmed against installed 12.4 | FEATURES §3; ADR-0206: picture capability depends on placement and decoder, not only media type. Measurement case M6-A6R-047. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-048: Consolidate, Reverse and Crop replace a video clip with an audio-only clip internally while leaving the original movie untouched. Ordinary edge trimming retains the video component. | Live 12 local manual, §§27.2.1; pp.518; exact revision unconfirmed against installed 12.4 | FEATURES §3: derived audio media must preserve original-video lineage and make loss of picture explicit. Measurement case M6-A6R-048. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-049: The floating Video Window stays above Live; geometry is not Set-specific. Double-click enables full screen on the chosen monitor, while Alt/Option-double-click restores original movie size. | Live 12 local manual, §§27.2.2; pp.518; exact revision unconfirmed against installed 12.4 | FEATURES §9; ADR-0206: global window geometry and project media state have separate lifetimes. Measurement case M6-A6R-049. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-050: A movie can contain partial audio/video tracks; picture gaps display black and audio gaps produce silence. Missing component coverage differs from a missing file. | Live 12 local manual, §§27.2.2.1; pp.519; exact revision unconfirmed against installed 12.4 | FEATURES §3/8: media duration and each stream’s supported interval must be represented separately. Measurement case M6-A6R-050. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-051: Warp must be enabled for video Tempo Leader. Of overlapping eligible Arrangement leaders, the bottom-most currently playing clip controls tempo; other video clips may themselves be warped. | Live 12 local manual, §§27.2.3; pp.519; exact revision unconfirmed against installed 12.4 | FEATURES §1/3: tempo authority is resolved across active clips and can differ from the clip whose picture is displayed. Measurement case M6-A6R-051. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-052: Dragging a video Warp Marker updates the shown frame; embedded QuickTime markers appear as alignment cues. Frame display is feedback for a musical-time edit. | Live 12 local manual, §§27.2.3.1; pp.519; exact revision unconfirmed against installed 12.4 | ADR-0206: retain exact source-frame identity while musical coordinates change. Measurement case M6-A6R-052. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-053: The scoring workflow can hide pre-roll by shifting the clip start, then restore it by moving the arrangement and extending the video edge. Export range follows the Arrangement selection, not an automatic original-movie range. | Live 12 local manual, §§27.3,27.4; pp.520–522; exact revision unconfirmed against installed 12.4 | FEATURES §1/3/8: source offset, song zero, timecode zero and export range are independent. Measurement case M6-A6R-053. | Supported hypothesis — documented; installed behavior unmeasured |

#### Accessibility, learning and settings gaps

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6R-054: Move Clips with Arrow Keys is documented on by default. With it off, left/right collapses a time selection to its start/end rather than moving the selected material. | Live 12 local manual, §§40.1.1; pp.973–974; exact revision unconfirmed against installed 12.4 | FEATURES §9; parity-7.2-shell: selection/navigation preferences change command semantics without changing musical data by themselves. Measurement case M6-A6R-054. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-055: Settings Tab navigation works independently of the main Tab-focus preference; arrows edit controls, Shift+Tab from the first control returns to the page chooser, and Escape closes Settings. Language changes require restart. | Live 12 local manual, §§40.1.2; pp.974; exact revision unconfirmed against installed 12.4 | FEATURES §14: Settings has its own keyboard scope and per-setting application timing. Measurement case M6-A6R-055. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-056: Accessibility options can speak menu commands, slider limits and time in seconds; Info View help is exposed through the screen reader’s help-text configuration. | Live 12 local manual, §§40.1.3,40.1.4; pp.975; exact revision unconfirmed against installed 12.4 | FEATURES §9: semantic name/value/range/help and display units should be separate from visual text. Measurement case M6-A6R-056. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-057: Return tracks have visual clip slots that cannot contain or launch clips. Track options, crossfader controls and performance impact are documented hidden by default in the Session mixer. | Live 12 local manual, §§40.4.1.2; pp.976–977; exact revision unconfirmed against installed 12.4 | FEATURES §5/9: accessible slot appearance must not imply an editable slot; defaults require clean-build confirmation. Measurement case M6-A6R-057. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-058: Keyboard clip-edge adjustment is a provisional edit committed by Enter and canceled by Escape. The guide says screen-reader support does not yet cover audio fades or MPE editing. | Live 12 local manual, §§40.4.1.3,40.4.1.4; pp.978–979; exact revision unconfirmed against installed 12.4 | FEATURES §3/4/9: cancellation is part of the gesture contract; reference accessibility limitations need not become ADI limitations. Measurement case M6-A6R-058. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-059: Ctrl/Cmd+F selects All as well as focusing search. Tab moves through search/filter-toggle/menu/sidebar/filters/content; down from search reaches results. Right arrow can audition even with automatic preview off. | Live 12 local manual, §§40.4.1.6; pp.980; exact revision unconfirmed against installed 12.4 | FEATURES §9: search scope, keyboard focus and explicit preview are separate state transitions. Measurement case M6-A6R-059. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-060: Learn View is online content that can update independently of Live. Its last page and video position survive panel closing within a running session; an offline/reconnect case exposes Reload. | Live 12 local manual, §§2.2.1; pp.23–27; exact revision unconfirmed against installed 12.4 | FEATURES §9: help content availability and application build version are separate. A learning service is a research reference, not an approved ADI requirement. Measurement case M6-A6R-060. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-061: Opening a lesson video picture-in-picture closes Learn View. Its always-on-top window can outlive focus in Live; returning from it focuses Live and closes picture-in-picture. | Live 12 local manual, §§2.2.1; pp.25–27; exact revision unconfirmed against installed 12.4 | FEATURES §9: a panel-to-window transfer needs one content/session identity and explicit focus return. Measurement case M6-A6R-061. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-062: Settings cover focused-view outlines, scrollbar policy, follow behavior, UI labels, pen input and permanent scrub areas. macOS can follow the system output device; these are separate from per-Set routing. | Live 12 local manual, §§2.3.1,2.3.2,2.3.3; pp.30–31; exact revision unconfirmed against installed 12.4 | FEATURES §14: maintain application/device/window scope and platform-specific availability. Measurement case M6-A6R-062. | Supported hypothesis — documented; installed behavior unmeasured |
| A6R-063: The manual’s Settings overview groups file/cache/Max paths, library locations/dependency collection, plug-in paths/windows, recording/warp/launch defaults and update/usage options; it does not enumerate every control or factory value. | Live 12 local manual, §§2.3.6,2.3.7,2.3.8,2.3.9,2.3.10; pp.32; exact revision unconfirmed against installed 12.4 | FEATURES §14: use an explicit setting census; absence from prose is not evidence a control is absent. Measurement case M6-A6R-063. | Unknown / not tested — complete installed setting census still required |

### Shipped default template: bounded structure observations

#### Shipped default template: evidence scope

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-001: The shipped template identifies its creator as Live 12.1d1 and format revision as 12.0_12117. Its presence in a 12.4 installation does not make each saved value a measured 12.4 factory default. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML Ableton/@Creator, @MinorVersion; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §9/14: retain artifact creator, parser version, installed host and user template precedence separately. Measurement case M6-A6S-001. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: topology

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-002: This file contains two MIDI tracks, two audio tracks, two returns, separate Main and PreHear objects, and eight scenes. The stored return names are A-Reverb and B-Delay. Built-in device parameters were excluded. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML LiveSet/Tracks, MainTrack, PreHearTrack, Scenes; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §2/9: distinguish initial musical topology from preview infrastructure and remembered names. This does not close the measured new-return question. Measurement case M6-A6S-002. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: names and identifiers

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-003: Tracks have numeric IDs and separate effective/user names. The first two MIDI IDs are 12 and 13 while the first audio ID is 8; order is not encoded by increasing ID. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML LiveSet/Tracks/*/@Id, Name; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §2/9: use stable identity plus explicit ordering; generated labels must not be object keys. Measurement case M6-A6S-003. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: route identity

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-004: Route blocks store a target string independently of upper/lower display strings and MIDI expression configuration. A visible chooser label is not sufficient to reconstruct routing identity. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML Tracks/*/DeviceChain/*Routing; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §2/4: separate canonical endpoint, label and expression protocol state. Measurement case M6-A6S-004. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: parameter dimensions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-005: Mixer controls retain a manual value, automation target and modulation target as separate fields. Stereo pan mode and the left/right pan values are distinct from the ordinary pan value. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML Tracks/MidiTrack/DeviceChain/Mixer; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §2/7: separate stored base state, automation address, modulation address and pan mode; field presence does not establish render order. Measurement case M6-A6S-005. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: view subdivisions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-006: Separate visibility fields exist for Arrangement mixer, Session mixer and Arrangement track controls. Their stored values can differ; a shared signal model does not imply a shared visibility switch. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML LiveSet/ViewStates; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §9; original L09/L10 remain unmeasured. Use distinct panel-state owners and test shortcut dispatch separately. Measurement case M6-A6S-006. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: dormant scene values

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-007: The first scene stores tempo 120 with its tempo-enabled flag false; follow-action values are present while FollowActionEnabled is false. A disabled value is not necessarily absent or zero. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML Scenes/Scene and FollowAction; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §5: preserve dormant configuration separately from enabled state and avoid executing inactive values. Measurement case M6-A6S-007. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: transport and freeze

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-008: Transport loop position/length, loop activation, punch-in/out and current time are separate entries. Track device chains contain distinct main and freeze sequencer containers. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML Transport; Tracks/*/DeviceChain; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §1/2/8: stored playback region and activation differ; rendered derivatives need separate ownership. The fields do not prove scheduler internals. Measurement case M6-A6S-008. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: editor tools state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-009: MIDI editor algorithms have saved configuration blocks and named remote-control entries. Quantize stores start/end enablement separately; Time Warp stores breakpoints plus preserve-range/stretch-end flags. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML LiveSet/NoteAlgorithms; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §4/9: tool configuration, remote binding and generated notes are separate state. Numeric enum codes remain uninterpreted. Measurement case M6-A6S-009. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: surface metadata

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-010: Per-track JSON contains Push note-mode, selected notes, repeat rate and repeat-enabled keys. These are saved presentation/performance metadata candidates, not proof of their restoration semantics. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML Tracks/*/ViewData; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §9: maintain controller state independently of MIDI clip content and validate migration of unknown metadata. Measurement case M6-A6S-010. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped default template: window-state caveat

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6S-011: The template includes a VideoWindowRect filled with sentinel coordinates, per-window detail-panel flags and expression-lane sizes/minimized flags. Field presence alone does not establish whether global preferences override them. | Installed DefaultLiveSet.als, SHA-256 aeec3f07599da0a61cc86fd9ddeb78a5ce61fe473f676d50c1ae3e281c289a1f; XML VideoWindowRect; ViewState*DetailOpen; ExpressionLanes; private E39/default-template-observation.json; inspected by Codex 2026-11-04 | FEATURES §9: conflicting stored/local presentation state needs a precedence contract; do not contradict the manual’s global video geometry claim from an unused field. Measurement case M6-A6S-011. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

### Shipped theme: semantic vocabulary and evidence limits

#### Theme state vocabulary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6T-001: The shipped theme has distinct named tokens for disabled controls, selection, hover, focus, automation, learning and implicit arm. This establishes a resource vocabulary, not a complete runtime state machine. | Read-only XML inspection of shipped Default Dark Neutral Medium.ask, SALON-TV 2026-11-04; private E39/theme-observation.json, SHA-256 c4d6dfc1193f7dc0d094f1b66254954f17da932afbd9cde265f536cb18bf4fa3; creator Ableton Live 12.1d1 | FEATURES §9; window parity: model semantic states separately from final color values; inspect combinations before specifying precedence. Measurement case M6-A6T-001. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Palette evidence boundary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6T-002: Sixteen numbered Clip color tokens exist in this file. They do not establish the 70-color chooser, its order, selection rules or screen RGB. The file also contains six- and eight-digit hex values; channel/alpha interpretation is not established by inspection alone. | Read-only XML inspection of shipped Default Dark Neutral Medium.ask, SALON-TV 2026-11-04; private E39/theme-observation.json, SHA-256 c4d6dfc1193f7dc0d094f1b66254954f17da932afbd9cde265f536cb18bf4fa3; creator Ableton Live 12.1d1 | FEATURES §9: keep L07/L08 and the complete palette census unresolved; do not substitute resource constants for measured palette values. Measurement case M6-A6T-002. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Meter and focus presentation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6T-003: The theme separates meter ramps such as HeadphonesVuMeter and SendsOnlyVuMeter, with ordered threshold tokens and OnlyMinimumToMaximum flags. It also declares a MainViewFocusIndicator independently of editor backgrounds. | Read-only XML inspection of shipped Default Dark Neutral Medium.ask, SALON-TV 2026-11-04; private E39/theme-observation.json, SHA-256 c4d6dfc1193f7dc0d094f1b66254954f17da932afbd9cde265f536cb18bf4fa3; creator Ableton Live 12.1d1 | FEATURES §2/9: meter role, scale, peak/hold behavior and visual interpolation are separate contracts; provide semantic focus evidence for accessibility. Measurement case M6-A6T-003. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

### Versioned release changes

#### Release deltas: later source is not installed behavior

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6D-001: 12.4.3: Color assignment now crosses both musical views. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.3, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | ADI color-command scope must be versioned; qualify A5C-008 instead of silently replacing its local-manual evidence. Measurement case M6-A6D-001. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-002: 12.4.3: Re-Pitch now applies clip grooves. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.3, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Treat warp-mode/groove interaction as a version-dependent audio contract. Measurement case M6-A6D-002. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-003: 12.4.15: Search focus selects the existing query. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.15, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Search selection and query replacement must be separately specified. Measurement case M6-A6D-003. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-004: 12.4.15: Seed/Rhythm output uses release velocity 64. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.15, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Persist generated expression explicitly and test older-project migration. Measurement case M6-A6D-004. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-005: 12.4.15: Plug-ins can expose 32 audio output buses. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.15, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Capability limits belong to the reference build, not a universal host constant. Measurement case M6-A6D-005. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-006: 12.4.15: Duplication followed by transformations receives undo/identity fixes. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.15, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Repeated edits require unique note identity, preserved originals and intentional transactions. Measurement case M6-A6D-006. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-007: 12.4.3: Deleting empty time no longer adds redundant history. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.3, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | No-op detection must be separate from gesture logging. Measurement case M6-A6D-007. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-008: 12.4.3: SMPTE API conversion now follows tempo automation. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.3, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Check API time conversion independently of ruler rendering. Measurement case M6-A6D-008. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-009: 12.4.1: Link Audio recording alignment received latency-compensation fixes. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.1, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Network-clock agreement alone does not establish recorded sample placement. Measurement case M6-A6D-009. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-010: 12.0: Push 2 changed away from its legacy remote-script implementation. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.0, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Qualify the entire local Push 2 chapter as potentially legacy; hardware/firmware/mode must accompany each result. Measurement case M6-A6D-010. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-011: 12.4.3: Windows filter/tag screen-reader announcements received fixes. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.3, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Accessibility gaps in the local manual may be historical. Measurement case M6-A6D-011. | Supported hypothesis — official release documentation; installed behavior unmeasured |
| A6D-012: 12.4.3: Single-track exports now append the track name. | [Ableton release history](https://www.ableton.com/en/release-notes/live-12/), section 12.4.3, accessed 2026-11-04; later releases do not describe installed 12.4 automatically | Naming policy is distinct from stem selection and must match the selected baseline. Measurement case M6-A6D-012. | Supported hypothesis — official release documentation; installed behavior unmeasured |

### Public API semantics and host boundaries

#### API identity and notification boundaries

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6P-001: API IDs are device-scoped, unsaved and never reused within that scope. Moving an object usually preserves identity, except moves implemented by deletion/recreation. | [Official Live API overview](https://docs.cycling74.com/userguide/m4l/live_api_overview/), Object ids; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §10/9: durable ADI object identity must not depend on an ephemeral reference API handle. Measurement case M6-A6P-001. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-002: Notification callbacks cannot directly mutate Set content; the guide recommends deferring the mutation. | [Official Live API overview](https://docs.cycling74.com/userguide/m4l/live_api_overview/), Notifications; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §10: observation delivery and mutation commit need explicit scheduling and reentrancy rules. Measurement case M6-A6P-002. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-003: API beat times use doubles in quarter-note units. Real-time remote parameter control deactivates automation and bypasses undo history. | [Official Live API overview](https://docs.cycling74.com/userguide/m4l/live_api_overview/), Datatypes; live.remote~; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §7/10: distinguish transient modulation from authored transactions; ADI gestures still follow its own undo contract. Measurement case M6-A6P-003. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |

#### API selection, playback and edit scope

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6P-004: Visible tracks exclude folded-group children, but include tracks outside the viewport. Capture MIDI accepts an explicit destination or uses the focused window’s view. | [Official Live API song](https://docs.cycling74.com/apiref/lom/song/), visible_tracks; capture_midi; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §2/5/8/9: logical visibility is distinct from rendering visibility; API destinations should not depend implicitly on focus. Measurement case M6-A6P-004. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-005: Device movement can choose the nearest legal insertion point and returns its resulting index. | [Official Live API song](https://docs.cycling74.com/apiref/lom/song/), move_device; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §6/10: a requested placement and committed placement need separate reporting; typed graph constraints must be visible. Measurement case M6-A6P-005. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-006: The public Song page exposes undo/redo and availability checks; it does not specify per-gesture transaction grouping. | [Official Live API song](https://docs.cycling74.com/apiref/lom/song/), can_undo; can_redo; undo; redo; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | ADR-0003/ADR-0128; FEATURES §10: do not derive human gesture boundaries from API calls. Measurement case M6-A6P-006. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |

#### API clip time domains and deferred writes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6P-007: Loop endpoints use beats for MIDI/warped audio and seconds for unwarped audio; with Loop off they mean clip start/end. Warp-marker sample times use seconds; sample_length is a sample count. | [Official Live API clip](https://docs.cycling74.com/apiref/lom/clip/), loop_start; loop_end; warp_markers; sample_length; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §3; ADR-0061: require explicit units at every boundary; never compare frame counts directly with seconds. Measurement case M6-A6P-007. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-008: Warp mapping interpolates between neighboring marker pairs and includes a final hidden marker defining the last segment’s tempo. | [Official Live API clip](https://docs.cycling74.com/apiref/lom/clip/), warp_markers; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §3: define extrapolation and terminal-segment ownership; a visible marker list may omit required mapping state. Measurement case M6-A6P-008. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-009: Marker insertion can preserve timing when sample time is omitted; supplied times must remain ordered and segment tempos within 5–999 BPM. Warp toggling is deferred. | [Official Live API clip](https://docs.cycling74.com/apiref/lom/clip/), add_warp_marker; warping; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §3/10: validate monotonically ordered mappings and asynchronous state transitions before dependent edits. Measurement case M6-A6P-009. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |

#### API note identity and query boundaries

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6P-010: Range queries select note starts, not all overlapping notes. Modification batches are ignored if they include notes absent from the clip. | [Official Live API clip](https://docs.cycling74.com/apiref/lom/clip/), get_notes_extended; apply_note_modifications; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §4/10: selection predicates and stale-identity error scope must be explicit. Measurement case M6-A6P-010. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-011: New API notes default to velocity 100 and release velocity 64. Clip quantization zero means Global; Song quantization zero means None. | [Official Live API clip](https://docs.cycling74.com/apiref/lom/clip/), add_new_notes; launch_quantization; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4; [Song reference](https://docs.cycling74.com/apiref/lom/song/), clip_trigger_quantization | FEATURES §4/5/10: distinguish API creation defaults from GUI defaults and use typed enums. Measurement case M6-A6P-011. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-012: Clip.stop does not cancel a merely triggered clip; held launch is represented separately by set_fire_button_state. | [Official Live API clip](https://docs.cycling74.com/apiref/lom/clip/), stop; set_fire_button_state; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §5/10: canceling a queued action differs from stopping active playback. Measurement case M6-A6P-012. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |

#### API parameter state and presentation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6P-013: Automation has absent/active/overridden states. A parameter can separately be active, inaudible-but-editable, or noneditable; macros and remote ownership can disable direct editing. | [Official Live API parameter](https://docs.cycling74.com/apiref/lom/deviceparameter/), automation_state; state; is_enabled; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §6/7/10: availability, audibility and automation override must not collapse into one boolean. Measurement case M6-A6P-013. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-014: Internal numeric value, GUI numeric value and formatted text are separate interfaces. Enum labels are available for quantized parameters; default_value is restricted to nonquantized parameters. | [Official Live API parameter](https://docs.cycling74.com/apiref/lom/deviceparameter/), value; display_value; str_for_value; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §6/9/10: the parameter feed needs typed units, formatting and reset capability, including unavailable defaults. Measurement case M6-A6P-014. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-015: A parameter-level function re-enables its automation independently of the Song-level action. | [Official Live API parameter](https://docs.cycling74.com/apiref/lom/deviceparameter/), re_enable_automation; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §7: local override release and global override release should have explicit scopes. Measurement case M6-A6P-015. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |

#### API track capabilities and routing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6P-016: Routes expose identifiers separately from display names and enumerate available choices. Capability checks distinguish armable, frozen, MIDI-input/output and audio-input/output tracks. | [Official Live API track](https://docs.cycling74.com/apiref/lom/track/), input/output_routing_type/channel; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §2/10: validate operations against dynamic capabilities, preserving ADI’s approved hybrid-track difference. Measurement case M6-A6P-016. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-017: Setting solo through the API bypasses exclusive-solo handling. Implicit arm is separate; returning a group to Arrangement includes nested children. | [Official Live API track](https://docs.cycling74.com/apiref/lom/track/), solo; implicit_arm; back_to_arranger; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §2/8/10: low-level setters and user commands can have different scope; ADI’s API must choose deliberately. Measurement case M6-A6P-017. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |
| A6P-019: The documented insert_device function accepts native devices only. Clip creation rejects wrong track types, frozen tracks, active recording and positions outside 0–1576800. | [Official Live API track](https://docs.cycling74.com/apiref/lom/track/), insert_device; create_audio_clip; create_midi_clip; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §3/6/10: GUI parity cannot be inferred from public API coverage. ADI’s general operation API is an independent design contract. Measurement case M6-A6P-019. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |

#### API metering and interface costs

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6P-018: Stereo meter values are smoothed peaks; combined level holds peaks for one second. The documentation warns that stereo meter observation consumes GUI resources. | [Official Live API track](https://docs.cycling74.com/apiref/lom/track/), input/output_meter_left/right/level; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §2; ADR-0175: meter subscriptions need batching and visibility-aware throttling; no CPU saving is measured here. Measurement case M6-A6P-018. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |

#### API take-lane ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6P-020: Take lanes have independent names and observable Arrangement clip lists. Their creation methods use beat positions and reject frozen, recording or incompatible tracks. | [Official Live API take](https://docs.cycling74.com/apiref/lom/takelane/), arrangement_clips; create_audio_clip; create_midi_clip; accessed 2026-11-04; LOM index labels reference 12.4.5, not installed 12.4 | FEATURES §3/8/10: lane identity, clip membership and the active comp are separate model concerns. Measurement case M6-A6P-020. | Supported hypothesis — public API documentation; availability and installed execution unmeasured |

### Tuning formats and interoperability

#### ASCL extension grammar and encoding

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-001: ASCL uses post-pitch @ABL comments, requires UTF-8, quotes space-containing arguments, and disables directives with an extra leading exclamation mark. | [ASCL Specification](https://help.ableton.com/hc/en-us/articles/10998372840220-ASCL-Specification), General notes; Live 12.1 baseline; accessed 2026-11-04 | ADR-0178 import research: parse musical intervals separately from extension metadata and encoding validation. Measurement case M6-A6F-001. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### ASCL reference and names

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-002: REFERENCE_PITCH supplies octave −2…8, zero-based degree and positive hertz. NOTE_NAMES supplies names beginning at implicit degree zero. | [ASCL Specification](https://help.ableton.com/hc/en-us/articles/10998372840220-ASCL-Specification), REFERENCE_PITCH; NOTE_NAMES; Live 12.1 baseline; accessed 2026-11-04 | ADR-0178: existing degree names fit part of this contract; explicit reference-frequency representation remains a format/design question. Measurement case M6-A6F-002. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### ASCL range validation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-003: At most one NOTE_RANGE directive is allowed. Frequency bounds span 4–21000 Hz; index octaves span −99…99 and require earlier REFERENCE_PITCH. | [ASCL Specification](https://help.ableton.com/hc/en-us/articles/10998372840220-ASCL-Specification), Note range directives; Live 12.1 baseline; accessed 2026-11-04 | ADR-0178: validate lexical, dependency and semantic errors before publishing imported objects. Measurement case M6-A6F-003. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### ASCL range order is not frequency order

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-004: Explicit ranges cap playback by 128 notes, 21kHz or the supplied maximum. Index boundaries permit nonmonotonic pitches. | [ASCL Specification](https://help.ableton.com/hc/en-us/articles/10998372840220-ASCL-Specification), Note range directives; Live 12.1 baseline; accessed 2026-11-04 | ADR-0178 requires rising nonnegative degrees: this is a genuine model-policy mismatch. Do not silently sort imported degree identities or call a valid source malformed. Measurement case M6-A6F-004. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### ASCL default index mapping distinguishes twelve-degree systems

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-005: Without explicit ranges, 128 notes center around 440Hz; twelve-degree systems instead align the nearest pitch with MIDI index 69. | [ASCL Specification](https://help.ableton.com/hc/en-us/articles/10998372840220-ASCL-Specification), Default note range; Live 12.1 baseline; accessed 2026-11-04 | ADR-0178: a tuning’s interval list does not by itself determine keyboard-index mapping. Store or expose the import mapping choice. Measurement case M6-A6F-005. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### ASCL descriptive metadata is not executable configuration

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-006: SOURCE records provenance; LINK supplies an informational URL whose direct accessibility may be restricted. | [ASCL Specification](https://help.ableton.com/hc/en-us/articles/10998372840220-ASCL-Specification), Documentation directives; Live 12.1 baseline; accessed 2026-11-04 | ADR-0178: preserve attribution and links independently of imported musical data; treat URL text as metadata, not instructions. Measurement case M6-A6F-006. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### ASCL compatibility pin has limits

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-007: The published directive set is scoped to Live 12.1; no format-version field or unknown-directive policy is specified. | [ASCL Specification](https://help.ableton.com/hc/en-us/articles/10998372840220-ASCL-Specification), Introduction and complete directive list; Live 12.1 baseline; accessed 2026-11-04 | ADR-0178: this is a documentary baseline, not a claim that later Live versions reject new directives. Define forward-compatibility and metadata preservation intentionally. Measurement case M6-A6F-007. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### SCL header and implicit degree

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-008: SCL has one description line, a declared pitch count and that many pitch entries. Degree zero is implicit; blank descriptions and zero counts are valid. | [Scala scale file format](https://www.huygens-fokker.org/scala/scl_format.html), The rules; unversioned page; accessed 2026-11-04 | ADR-0178: separate source-format validity from musical usability and application-specific minimum degree count. Measurement case M6-A6F-008. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### SCL numeric grammar matters

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-009: Decimal points mark cents; otherwise entries are ratios, including bare integers over one. Trailing text is ignored. Negative ratios are invalid, negative cents valid. | [Scala scale file format](https://www.huygens-fokker.org/scala/scl_format.html), Pitch-value rules and valid examples; unversioned page; accessed 2026-11-04 | ADR-0178 currently forbids negative stored degrees: decide reject-with-explanation, preserve an alternate source representation, or explicitly approved normalization. A generic SCL parser must not misread integer ratios as cents. Measurement case M6-A6F-009. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### SCL capacity and legacy encoding differ from ASCL

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-010: Ratio components require at least 31 positive bits. Implementations may impose size limits. Traditional SCL permits 8-bit text, unlike extension-bearing UTF-8 ASCL. | [Scala scale file format](https://www.huygens-fokker.org/scala/scl_format.html), The rules and character-set footnote; unversioned page; accessed 2026-11-04 | ADR-0178: choose documented resource limits and encoding migration while preserving musical precision and source attribution. Measurement case M6-A6F-010. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### Scale intervals and keyboard maps are separate formats

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-011: The SCL specification distinguishes scale intervals from .kbm files mapping keyboard notes to degrees. | [Scala scale file format](https://www.huygens-fokker.org/scala/scl_format.html), Keyboard-mapping note; unversioned page; accessed 2026-11-04 | ADR-0178 already defers keyboard mapping; keep import capability claims granular, and do not equate accepted scale files with a complete transferable performance layout. Measurement case M6-A6F-011. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### Live MIDI export loses tuning context

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-012: MIDI export preserves note indices but omits tuning metadata and microtonal-retuning MPE. The FAQ proposes retuning to 12-TET before exporting, then Undo. | [Tuning Systems FAQ](https://help.ableton.com/hc/en-us/articles/11535414344476-Tuning-Systems-FAQ), Tunings with MIDI; unversioned page; accessed 2026-11-04 | ADR-0178 / ADR-0203 d7: explicit interoperability loss needs documentation; any ADI richer export is an addition requiring an agreed representation. Measurement case M6-A6F-012. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### Live does not import Scala keyboard maps

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-013: Live does not support .kbm. Loading plain .scl supplies a reference pitch; exporting .ascl can retain a corrected reference. | [Tuning Systems FAQ](https://help.ableton.com/hc/en-us/articles/11535414344476-Tuning-Systems-FAQ), About Tuning Systems; unversioned page; accessed 2026-11-04 | ADR-0178: reference correction and keyboard-layout import are independent capabilities. A successful interval import does not establish compatibility with another host’s sounding pitch. Measurement case M6-A6F-013. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

#### Tuning persistence and bypass cross the project/track boundary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6F-014: The tuning is embedded in .als; Bypass Tuning sends original MIDI without retuning. The FAQ says tuning hot-swap is unavailable. | [Tuning Systems FAQ](https://help.ableton.com/hc/en-us/articles/11535414344476-Tuning-Systems-FAQ), About Tuning Systems; Tunings with MIDI; Finding and accessing; unversioned page; accessed 2026-11-04 | ADR-0178 d5: compare these separate Live ownership states while retaining ADI’s per-key-map tuning; per-track bypass remains deferred. This finding does not change the approved ADI model. Measurement case M6-A6F-014. | Supported hypothesis — official documentary contract; no installed Live 12.4 interoperability execution |

### Remaining host contracts: resource, external and MIDI boundaries

#### Stem separation chooses destination and presentation from context

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-001: Browser separation inserts results into whichever main view is focused. In an empty Set, source tempo can become project tempo when sample warping is enabled. Multiple separate stems form a group using source-track color, with stem-category colors on child tracks/clips; one stem or a merged result uses a single track. | Live 12 local manual §22.2, pp.433–436; exact manual revision unknown | ADR-0203 d8 / FEATURES §6: capture insertion view, tempo-change eligibility and result graph before starting the asynchronous stem-separation job; color inheritance is distinct from generated audio. Measurement case M6-A6G-001. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Link can bridge an external tempo authority

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-002: Link can operate alongside Tempo Follower or MIDI synchronization, publishing the tempo received from that external input into the Link session. The first participant establishes initial tempo. With another peer connected and transport stopped, the position display shows phase progress; playback waits for the quantization boundary. | Live 12 local manual §36, §36.1, §36.1.2, pp.944,947; exact manual revision unknown | FEATURES §1/9 and ADR-0185: distinguish local transport, external clock source, shared tempo and a queued phase-aligned start. Measurement case M6-A6G-002. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### CPU displays have different modes and per-track indicators are relative

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-003: CPU display can show Average, Current or neither; Current is documented hidden by default. The overload-warning default is explicitly described for new Live 11 installations, not a verified Live 12.4 default. Session track impact uses six rectangles to indicate relative contribution within the current Set, not a calibrated per-track percentage. | Live 12 local manual §37.1.1, §37.1.3, pp.954–955; exact manual revision unknown | ADR-0207 / FEATURES performance UI: preserve display mode, warning preference and relative track impact as separate states; never infer an operating-system metric from the label. Measurement case M6-A6G-003. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Disk overload distinguishes damaged capture from playback dropout

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-004: The Disk Overload indicator signals reads or writes that could not complete fast enough. During recording the documented result is a gap in the recorded sample; during playback it is a dropout. RAM Mode and fewer simultaneous channels are described as load-reduction options. | Live 12 local manual §37.2, pp.957–958; exact manual revision unknown | FEATURES §8 / recovery: model data-loss diagnostics separately from transient monitoring errors and distinguish disk starvation from audio-thread overload. Measurement case M6-A6G-004. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Frozen clip transfer retains frozen representation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-005: Dropping a frozen clip into the new-track area creates a new frozen track; a partial Arrangement selection transfers only that portion. The manual also describes frozen-track playback on a computer lacking the original devices. It does not establish how missing-device editing or unfreezing is resolved. | Live 12 local manual §37.1.4, pp.957; exact manual revision unknown | FEATURES §2/3/8: retain derivative audio, source graph and selection bounds as separate owned objects; portability of rendered playback does not establish editable plug-in availability. Measurement case M6-A6G-005. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Single summing-point precision differs from the processing pipeline

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-006: The fact sheet describes 64-bit accumulation at each mix point within a 32-bit processing architecture. Its single-point cancellation example does not promise bit-identical results through multiple consecutive mix points; the text allows very small signal differences across multiple points. | Live 12 local manual §38.2.4, pp.961; exact manual revision unknown | ADR-0207 d2 already decides ADI’s default: 32-bit processing and 64-bit accumulation at every mix point, with optional 64-bit processing throughout. Compare graph topology and numerical behavior under that decision; the reference is a vendor-documented hypothesis, not a new precision choice. Measurement case M6-A6G-006. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Render neutrality requires controlled rate, depth and prior operations

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-007: The documented undithered-render test requires export rate to match the audio-engine rate and no preceding non-neutral operations. Same or higher output bit depth cancels in the described tests; lowering depth introduces quantization error. Neutral playback refers to Live’s signal before hardware output, not the analogue chain. | Live 12 local manual §38.1, §38.2.1, §38.2.2, pp.959–960; exact manual revision unknown | ADR-0207 / export validation: state the full test configuration instead of making an unconditional null-render claim. Measurement case M6-A6G-007. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Recording neutrality depends on the source precision

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-008: The fact sheet conditions external recording neutrality on capture depth meeting the converter’s delivered depth. Internal processed signals use the documented 32-bit path; lower-depth unprocessed sources can remain neutral at their original depth. Merely writing a low-depth source into a wider file does not restore information. | Live 12 local manual §38.2.5, §38.2.6, §38.3.5, §38.3.6, pp.961,964–965; exact manual revision unknown | FEATURES §8 / ADR-0207: keep input precision, processing precision and storage encoding distinct in record-format tests and defaults. Measurement case M6-A6G-008. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Neutral routing and splitting claims have explicit comparison points

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-009: The routing claim compares the selected source tap with its destination, not necessarily the originating track’s final output. Splitting already-neutral audio is documented as sample-accurate and seamless for looped/unlooped unwarped or unstretched warped clips, without altering source samples. | Live 12 local manual §38.2.9, §38.2.10, pp.963; exact manual revision unknown | FEATURES §2/3 and ADR-0207: test tap identity and boundary behavior while controlling automatic fades and gain stages. Measurement case M6-A6G-009. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Export conversion and dither are separate non-neutral stages

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-010: The fact sheet names SoX for export sample-rate conversion; it does not establish that real-time playback uses the same path. It also states that a gain edit enters the 32-bit processing domain and warns against dithering an audio file repeatedly. These claims require build-specific numerical validation. | Live 12 local manual §38.3.2, §38.3.4, pp.964; exact manual revision unknown | ADR-0207 / export: represent engine rate, render conversion, final storage depth and dither as distinct decisions; library mention is not reconstruction of its algorithm. Measurement case M6-A6G-010. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### MIDI timing validation separates DAW clocks from hardware serialization

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-011: The manual’s input test splits one MIDI signal into Live MIDI capture and an audio-recorded electrical reference, then compares event times. In its conventional MIDI-cable setup, serial transmission and hardware scanning can separate simultaneous notes, and computer timestamps do not schedule the external receiver. Published interface results are historical examples, not installed-machine limits. | Live 12 local manual §39.1, §39.2, §39.4, §39.5, pp.967–971; exact manual revision unknown | ADR-0054 / FEATURES §4/9: separate input recording, plug-in playthrough and hardware output tests; report fixed latency and jitter distributions independently. Measurement case M6-A6G-011. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

#### Shortcut tables contain unresolved editor bindings

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-012: The note-editing prose uses Ctrl+arrows on Windows to select neighboring notes, while the shortcut table uses Alt+arrows. The MPE chapter gives Alt/Option+3 for its editor, while the table separates Clip View focus from Alt/Option+Shift+3 for MPE. Within the Windows MIDI-editor table, Ctrl+Shift+F labels both Follow and note filters. | Live 12 local manual §10.5.2, §12.1, §41.2, §41.10, §41.12, pp.255,326,985,991,993–994; exact manual revision unknown | ADR-0203 d1/d7 and keyboard parity: keep competing documentary bindings visible and resolve them by installed-build/focus measurements; do not silently select one table as authoritative. Measurement case M6-A6G-012. | Unknown / not tested — conflicting or overlapping documentary bindings; installed behavior unmeasured; context and version precedence unresolved |

#### Global quantization shortcuts are distinct from editor grid commands

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6G-013: The shortcut table maps Ctrl/Cmd+6,7,8,9,0 to sixteenth, eighth, quarter, one-bar and off global launch quantization. Ctrl/Cmd+1…5 instead controls editor grid size/triplets/snapping/adaptation. These are different timing settings despite neighboring keys. | Live 12 local manual §41.13, §41.14, pp.995; exact manual revision unknown | FEATURES §1/4/5 and keyboard parity: route launch timing and editor snapping to separate model fields and UI feedback. Measurement case M6-A6G-013. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; exact source revision unknown |

### Shipped host schema: identity, types and version boundaries

#### Schema evidence boundary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-001: The installed file declares named classes and typed members. Its schema version, translator number and change count are separate header fields. A shipped declaration does not establish the active serializer, the meaning of numeric Type codes, migration behavior or an exhaustive native-file specification. | Read-only shipped schema 12.0_12402.txt; LiveSet; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | FEATURES §9/10: pin schema evidence independently of app version; use it to form validation questions rather than copy an undocumented project format. Measurement case M6-A6Z-001. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Note identity and musical values

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-002: The note declaration separates a 64-bit note ID from enabled state, probability, velocity deviation, attack/release velocities, start and duration. Timing is declared Double and the velocity/probability quantities Float. These names/types do not establish units, valid ranges or numerical scheduling. | Read-only shipped schema 12.0_12402.txt; MidiNoteEvent; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | FEATURES §4/10; ADR-0054: identity, enablement, probability and expressive values need separate fields; storage type does not prove audio precision or event accuracy. Measurement case M6-A6Z-002. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Expression references notes independently of pitch buckets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-003: Notes are grouped under key tracks, while a separate per-note event store has lists carrying NoteId and CC fields. Events carry a time offset, value and two pairs of curve-control coordinates. The declaration does not define curve mathematics or missing-note cleanup. | Read-only shipped schema 12.0_12402.txt; MidiNotes,KeyTrack,PerNoteEventList,PerNoteEvent; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | FEATURES §4; ADR-0054: preserve expression ownership during pitch changes and note surgery; distinguish key-indexed lookup from durable note identity. Measurement case M6-A6Z-003. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Probability group identity is separate from note identity

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-004: A probability group declares group type, note membership, probability and a 64-bit group ID. The note container separately declares generators for group IDs and note IDs. No random seed or playback algorithm is established by these declarations. | Read-only shipped schema 12.0_12402.txt; MidiNotes,NoteProbabilityGroup; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | FEATURES §4: keep grouped selection/probability and note identity independently addressable; serialization vocabulary alone cannot prove reproducible randomness. Measurement case M6-A6Z-004. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Warp anchors have explicit coordinate names

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-005: WarpMarker declares separate Double source-second and beat coordinates. SampleWarpProperties contains anchors and onsets separately, alongside mode/control state. This supports a structural separation of analysis candidates and authored mapping without proving an interpolation or stretch kernel. | Read-only shipped schema 12.0_12402.txt; WarpMarker,SampleWarpProperties; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | FEATURES §3; ADR-0061/ADR-0176: retain typed time domains and separate derived onset analysis from user constraints. Measurement case M6-A6Z-005. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Tuning carries interval, mapping and descriptive state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-006: Tuning declarations distinguish cents from optional numerator/denominator ratios, period in cents/ratio, reference and default reference, allowed note range, note names, scales and descriptive provenance. Presence of these fields does not establish valid order, defaults or import acceptance. | Read-only shipped schema 12.0_12402.txt; TuningSystem,NoteTuning,TuningRatio; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | ADR-0178: retain the approved ADI per-key-map model and explicit representation policy; reference metadata must not be mistaken for degree identity. Measurement case M6-A6Z-006. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Envelope data and lane presentation are distinct declarations

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-007: Both automation and clip envelopes declare a target pointer and event automation. ClipEnvelope additionally has a loop slot and scroll-time preservation. Individual AutomationLane declarations contain device/envelope selection, content selection and height; AutomationLanes separately contains the collection and AreAdditionalAutomationLanesFolded. | Read-only shipped schema 12.0_12402.txt; AutomationEnvelope,ClipEnvelope,AutomationLane,AutomationLanes; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | FEATURES §7/9: data ownership, looping and lane presentation should remain distinct; these declarations do not specify lane order or the unfold-triangle behavior. Measurement case M6-A6Z-007. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Take-lane content and audition state are independently represented

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-008: TakeLane declares name, annotation, height, selection, audition and clip automation; TakeLanes has a separate collection and fold state. These declarations do not identify the active-comp reconstruction algorithm or prove audition is restored on reopen. | Read-only shipped schema 12.0_12402.txt; TakeLane,TakeLanes; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | FEATURES §3/8/9: separate lane membership, presentation, audition and the main comp; safe restoration must be specified independently. Measurement case M6-A6Z-008. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### File identity includes multiple locator hints

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-009: FileRef declares RelativePath/RelativePathType, Path/Type, LivePackName/LivePackId, OriginalFileSize (Int64), OriginalCrc and SourceHint. Declarations alone do not establish path interpretation, size units, search priority, collision handling or the CRC algorithm. | Read-only shipped schema 12.0_12402.txt; FileRef; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0 | FEATURES §8/9; ADR-0127: content identity, display paths, package membership and relinking hints need explicit semantics; do not treat a pathname as immutable identity. Measurement case M6-A6Z-009. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

#### Shipped schema revisions do not equal app-version history

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6Z-010: Comparing 12.0_12117 with 12.0_12402 shows SelectedBreakpointValue added to LiveSet, and LomId plus an Arrangement-clip-list wrapper added to TakeLane. The selected note, expression, probability, tuning and clip-envelope declarations compared here are unchanged. This is a resource comparison, not a dated application change log. | Read-only shipped schema 12.0_12402.txt; LiveSet,TakeLane; SALON-TV 2026-11-04; schema header Version=5 / TranslatorNumber=12402 / ChangeCount=2; private E42, SHA-256 0ab9f8448849459d13b635f236da65a84c8d45fe2e02e19c41d9e9678e2c34e0; comparison 12.0_12117.txt SHA-256 95dd223921a2f873a5e62bc391be904a153430ad749e2f9e802c362ab6d98eef | FEATURES §9/10: model API exposure, view state and musical content separately, and require observed migration evidence before adopting a compatibility rule. Measurement case M6-A6Z-010. | Confirmed by static analysis — this shipped artifact only; runtime meaning/defaults unmeasured |

### Editor extension and Stacks authoring contracts

#### Stacks bank structure is a cardinality-indexed JSON resource

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-001: The .stacks example has a top-level chord_rulesets array. Each entry contains scale_cardinality and rules; each rule is an array of intervals. | [How to Create Custom Chord Rules for Stacks](https://help.ableton.com/hc/en-us/articles/14789643520668-How-to-Create-Custom-Chord-Rules-for-Stacks), Creating custom chord banks / Example .stacks file; Article explicitly introduces support as of Live 12.1; no schema-version field or exact revision declared; accessed 2026-11-04 | ADR-0203 d7: if compatible bank import is adopted later, parse a distinct resource type and preserve its original payload; this example is not a complete formal JSON Schema. Measurement case M6-A6X-001. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Stacks chooses an exact or nearest cardinality ruleset

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-002: Intervals count scale degrees, or chromatic semitones with scale disabled. Missing cardinalities use the nearest ruleset. The panel displays the bank name and selected cardinality. | [How to Create Custom Chord Rules for Stacks](https://help.ableton.com/hc/en-us/articles/14789643520668-How-to-Create-Custom-Chord-Rules-for-Stacks), Terminology / Chord banks / Loading chord banks; Article explicitly introduces support as of Live 12.1; no schema-version field or exact revision declared; accessed 2026-11-04 | ADR-0178 and ADR-0203 d7: distinguish active musical scale from the bank ruleset chosen for generation; retain that choice in a reproducible edit proposal. Measurement case M6-A6X-002. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Stacks custom banks are not limited to bundled chord sizes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-003: Custom rules permit negative intervals, omitted root zero and degrees beyond one octave. Rule counts and rule lengths are unrestricted in the guide; arbitrary tuning cardinalities are supported. | [How to Create Custom Chord Rules for Stacks](https://help.ableton.com/hc/en-us/articles/14789643520668-How-to-Create-Custom-Chord-Rules-for-Stacks), Tips for creating custom chords; Article explicitly introduces support as of Live 12.1; no schema-version field or exact revision declared; accessed 2026-11-04 | ADR-0178 / ADR-0203 d7: avoid importing bundled-bank sizes as format limits. Use explicit resource limits for ADI and document them as ADI policy. Measurement case M6-A6X-003. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Stacks bank selection lasts for the open Set but is not serialized

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-004: A loaded bank applies to all MIDI clips while that Set remains open. The bank selection is not saved with the Set and must be loaded again afterward. | [How to Create Custom Chord Rules for Stacks](https://help.ableton.com/hc/en-us/articles/14789643520668-How-to-Create-Custom-Chord-Rules-for-Stacks), Loading chord banks; Article explicitly introduces support as of Live 12.1; no schema-version field or exact revision declared; accessed 2026-11-04 | ADR-0203 d7: separate generated notes from generator resource dependency. Decide deliberately whether ADI embeds, references or omits that dependency rather than copying transient reference behavior accidentally. Measurement case M6-A6X-004. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Stacks bank changes require explicit reload and offer reset

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-005: Invalid banks produce an error. Edited files require another Browser double-click to reload; restarting is unnecessary. The bank-name menu restores the default bank. | [How to Create Custom Chord Rules for Stacks](https://help.ableton.com/hc/en-us/articles/14789643520668-How-to-Create-Custom-Chord-Rules-for-Stacks), Loading chord banks / Tips for creating custom chords; Article explicitly introduces support as of Live 12.1; no schema-version field or exact revision declared; accessed 2026-11-04 | ADR-0203 d7: bank parsing, activation and note generation should be separate operations with an explicit failure policy. Measurement case M6-A6X-005. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Custom MIDI Tool note dictionaries have documented minimum fields

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-006: The notes array contains per-note dictionaries. Generators require pitch, start_time and duration; velocity, mute, probability, velocity_deviation and release_velocity are optional. | [Max for Live MIDI Tools](https://docs.cycling74.com/userguide/m4l/live_miditools/), Creating A Transformation / Step 2; Creating a Generator / Step 2; Current unversioned Cycling 74 guide; explicit minimum Live 12.0; exact Max/API revision undeclared; accessed 2026-11-04 | ADR-0203 d7 / ADR-0054: define an editor-extension data contract separately from realtime MIDI events. Omitted defaults, ranges and MPE preservation require additional specification. Measurement case M6-A6X-006. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Custom MIDI Tool transformations preserve untouched fields and order updates

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-007: Transformations should retain unmodified dictionary fields. UI parameter changes must update the transformation value before triggering another input request. | [Max for Live MIDI Tools](https://docs.cycling74.com/userguide/m4l/live_miditools/), Creating A Transformation / Steps 3 and 4; Current unversioned Cycling 74 guide; explicit minimum Live 12.0; exact Max/API revision undeclared; accessed 2026-11-04 | ADR-0203 d7 / ADR-0054: extensions should preserve opaque fields so a pitch edit does not erase unrelated expression or future metadata; enforce deterministic parameter-to-request ordering. Measurement case M6-A6X-007. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Custom MIDI Tool processing is a synchronous transaction

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-008: An input request without an output dictionary can leave an unfinished transaction and show a synchronization error. node.script is unsuitable when the operation depends on its asynchronous result. | [Max for Live MIDI Tools](https://docs.cycling74.com/userguide/m4l/live_miditools/), Common Errors / Synchronisation; Limitations / node.script; Current unversioned Cycling 74 guide; explicit minimum Live 12.0; exact Max/API revision undeclared; accessed 2026-11-04 | ADR-0203 d7: define request completion, cancellation and failure isolation for ADI extensions before supporting long-running work; do not place an unbounded extension operation on an interactive critical path. Measurement case M6-A6X-008. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### MIDI Tool audio-engine and transport-object limitations

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-009: MIDI Tools cannot process audio. Their transport object cannot control transport, and reported transport values may be unreliable. | [Max for Live MIDI Tools](https://docs.cycling74.com/userguide/m4l/live_miditools/), Limitations / MSP objects and audio processing; Transport; Current unversioned Cycling 74 guide; explicit minimum Live 12.0; exact Max/API revision undeclared; accessed 2026-11-04 | ADR-0203 d7: treat editor tools as a capability-limited host extension, with transport and audio-engine access explicit if ADI ever offers them. Measurement case M6-A6X-009. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Custom MIDI Tool tutorial contains identity and timing ambiguities

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-010: The sample uses note_id, but explanatory text says node_id. A generator example calls duration 0.0625 a sixteenth note despite describing beat-time units; neither discrepancy establishes an exact format rule. | [Max for Live MIDI Tools](https://docs.cycling74.com/userguide/m4l/live_miditools/), Creating A Transformation / Step 2; Creating a Generator / Step 2; Current unversioned Cycling 74 guide; explicit minimum Live 12.0; exact Max/API revision undeclared; accessed 2026-11-04 | ADR-0203 d7: pin actual API revision and verify timing units independently before serializing extension output; preserve this source defect instead of silently correcting it into a fact. Measurement case M6-A6X-010. | Unknown — conflicting/incomplete documentation; installed Live 12.4 behavior unmeasured |

#### MIDI Tool apply cycles isolate a stable baseline from repeated previews

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-011: A bang starts or continues an apply cycle using its initial notes/context snapshot. Outside interaction ends it. Apply starts and ends one cycle immediately. The input object works only in Note Generator/Transformation AMXD types. | [live.miditool.in reference](https://docs.cycling74.com/reference/live.miditool.in), Discussion; Current unversioned Cycling 74 object reference; exact Max/API revision undeclared; accessed 2026-11-04 | ADR-0003/ADR-0128 / ADR-0203 d7/d13: retain a stable edit baseline separately from preview output, then define explicit accept/cancel boundaries for native and extension tools. Measurement case M6-A6X-011. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### MIDI Tool input documentation disagrees about outlet roles

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-012: Discussion assigns notes to the left outlet, context to the middle, and independent notifications such as pitch_range to the right. The bang section instead puts context on the right; the MIDI-message section is unfinished. | [live.miditool.in reference](https://docs.cycling74.com/reference/live.miditool.in), Discussion versus Messages / bang and (MIDI); Current unversioned Cycling 74 object reference; exact Max/API revision undeclared; accessed 2026-11-04 | ADR-0203 d7: use a versioned host schema and explicit event categories; this incomplete reference cannot define a complete context/notification protocol. Measurement case M6-A6X-012. | Unknown — conflicting/incomplete documentation; installed Live 12.4 behavior unmeasured |

#### MIDI Tool output distinguishes transformed state from generated additions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-013: Output dictionaries follow the input shape. A Transformation supplies the new note state; a Generator adds notes and can superimpose them on existing notes. | [live.miditool.out reference](https://docs.cycling74.com/reference/live.miditool.out), Messages / dictionary; Current unversioned Cycling 74 object reference; exact Max/API revision undeclared; accessed 2026-11-04 | ADR-0203 d7: keep operation kind explicit. Reconcile this API wording with A-T02 and the manual overlap description before deciding replacement scope. Measurement case M6-A6X-013. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### MIDI Tool availability depends on editor mode and nonempty targets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-014: Transform is disabled for empty clips or empty selected time ranges. Generate works regardless of selection. Both panels are inactive in Envelopes mode. | [MIDI Tools and Device Updates in Live 12 FAQ](https://help.ableton.com/hc/en-us/articles/11535349458588-MIDI-Tools-and-Device-Updates-in-Live-12-FAQ), Why is Transform disabled?; Article addresses Live 12; no exact revision declared; accessed 2026-11-04 | ADR-0203 d7: derive command availability from target content and active editor mode rather than a single global tool-enabled flag. Measurement case M6-A6X-014. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Native tool mapping is separate from automation and Max tool hosting

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-015: Native MIDI Tool parameters accept key/MIDI mappings; this excludes Max MIDI Tools. MIDI Tool parameters cannot be automated. | [MIDI Tools and Device Updates in Live 12 FAQ](https://help.ableton.com/hc/en-us/articles/11535349458588-MIDI-Tools-and-Device-Updates-in-Live-12-FAQ), Can MIDI Tools parameters be automated or MIDI/Key mapped?; Article addresses Live 12; no exact revision declared; accessed 2026-11-04 | ADR-0203 d7: specify edit-tool bindings independently of track automation and extension capabilities; no assumption that every visible parameter is an automatable project parameter. Measurement case M6-A6X-015. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Changing note selection commits a MIDI Tool preview boundary

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-016: Deselecting notes or changing selection commits the edit. Transform-off can restore the baseline before that boundary; afterward reversal uses ordinary Undo. | [MIDI Tools and Device Updates in Live 12 FAQ](https://help.ableton.com/hc/en-us/articles/11535349458588-MIDI-Tools-and-Device-Updates-in-Live-12-FAQ), When are changes created through transformations committed or saved?; Article addresses Live 12; no exact revision declared; accessed 2026-11-04 | ADR-0003/ADR-0128 / ADR-0203 d7/d13: selection changes are transaction boundaries with user-visible consequences; define whether unrelated focus actions also commit. Measurement case M6-A6X-016. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### MIDI Tool parameter persistence belongs to the Set

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-017: Transform and Generate parameters are saved in the .als Set. Quantize settings previously used global preferences. | [MIDI Tools and Device Updates in Live 12 FAQ](https://help.ableton.com/hc/en-us/articles/11535349458588-MIDI-Tools-and-Device-Updates-in-Live-12-FAQ), Where are Quantize parameters saved?; Article addresses Live 12; no exact revision declared; accessed 2026-11-04 | ADR-0203 d7: classify tool parameters as document state separately from result notes and global editor preferences; compare the transient Stacks bank-selection rule independently. Measurement case M6-A6X-017. | Supported hypothesis — documented; installed Live 12.4 behavior unmeasured; public documentation revision qualified |

#### Selection fallback differs between the FAQ and detailed manual utilities

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-018: The FAQ limits pitch/time and velocity/chance utilities to selection. Manual §10.5.9 describes whole-clip fallback for buttons with no selection; Randomize repeats that fallback. The scope/version difference is unresolved. | [MIDI Tools and Device Updates in Live 12 FAQ](https://help.ableton.com/hc/en-us/articles/11535349458588-MIDI-Tools-and-Device-Updates-in-Live-12-FAQ), Which notes are affected by transformations?; Article addresses Live 12; no exact revision declared; accessed 2026-11-04; cross-check: local Live 12 manual §10.5.9 p.263 and velocity Randomize p.274, exact local revision unknown | FEATURES §4: keep command-specific target selection as an explicit requirement; do not replace detailed manual contracts with a blanket fallback rule. Measurement case M6-A6X-018. | Unknown — conflicting/incomplete documentation; installed Live 12.4 behavior unmeasured |

#### Custom MIDI Tool folder guidance differs across official sources

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6X-019: The help article recommends User Library/MIDI Tools/Transformations and /Generators and describes Suite. The local manual instead names Max Transformations and Max Generators and also discusses Standard with Max for Live. | [Using third-party MIDI Tools](https://help.ableton.com/hc/en-us/articles/14462716734876-Using-third-party-MIDI-Tools), Installation instructions; Article states Live 12 Suite; no exact revision declared; accessed 2026-11-04; cross-check: local Live 12 manual §11.1.1 p.291, exact local revision unknown | ADR-0203 d7: distinguish extension discovery conventions from licensing and supported search roots; retain version-specific source qualifications. Measurement case M6-A6X-019. | Unknown — conflicting/incomplete documentation; installed Live 12.4 behavior unmeasured |

### Current Push integration and versioned behavior

#### Current Push 2: controller registration ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-001: Live 12 automatically manages Push 2 without a visible Control Surface entry; adding a manual entry can disrupt connection. | [Setting up Push 2 (Windows)](https://help.ableton.com/hc/en-us/articles/209072529-Setting-up-Push-2-Windows), Setting up Push 2 with Live 12; accessed 2026-11-04. Current unversioned Ableton support article; Live 12 setup distinguished from Live 11 and earlier. | ADR-0181/FEATURES §9: distinguish discovered connection ownership from editable mapping records. This refines A6D-010; absence from a settings list is not absence of an active controller. Measurement case M6-A6U-001. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push 2: display transport has its own lifecycle

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-002: Push 2 display rendering uses the host GPU. Its Windows display driver installs on first detected connection while Live runs. | [Setting up Push 2 (Windows)](https://help.ableton.com/hc/en-us/articles/209072529-Setting-up-Push-2-Windows), Connecting Push 2, subsections 1 and 3; accessed 2026-11-04. Current unversioned Ableton support article; Live 12 setup distinguished from Live 11 and earlier. | ADR-0181/FEATURES §9: controller display availability and musical input availability require separate diagnostics and lifecycle states. This is an architectural lesson, not an ADI hardware commitment. Measurement case M6-A6U-002. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push 2: migration and explicit legacy fallback

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-003: The macOS guide clears old Push 2 surface assignments and restarts Live; if connection still fails, -Push2UseLegacyScript plus restart selects legacy setup. | [Setting up Push 2 (Mac)](https://help.ableton.com/hc/en-us/articles/209776005-Setting-up-Push-2-Mac), Troubleshooting Push 2 connection in Live 12; Legacy Push 2 Setup Mode in Live 12; accessed 2026-11-04. Current unversioned Ableton support article; Live 12 migration and legacy fallback sections. | ADR-0203/ADR-0181: reference evidence must record integration mode alongside app and firmware versions. Keep macOS troubleshooting scope; this does not establish Windows behavior or authorize changing installed options. Measurement case M6-A6U-003. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push 2: automatic arm policy is configurable

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-004: 12.4: Push 2 can auto-arm audio+MIDI, MIDI only, or neither; activation can cover Note mode alone or Note+Session. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.4 and Live 12.4, 2026-05-05, Status and Preferences Tabs Redesign; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | FEATURES §8/9: qualify A6R-029’s older implicit-arm description with explicit policy and active mode. Selection, automatic arm and explicit arm remain separate state; defaults are not established here. Measurement case M6-A6U-004. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push 2: explicit arm survives selection

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-005: 12.0.20: with Exclusive Arm disabled, selecting MIDI tracks on Push 2 preserves other explicitly armed tracks. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.0.20 with Live 12.0.20, 2024-08-06, Bugfixes; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | FEATURES §8/9: refine A6R-029 with independent explicit-arm ownership; controller focus must not erase an intentional recording destination. Measurement case M6-A6U-005. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push 2: device duplication gesture

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-006: 12.3: Duplicate plus a device’s upper display button duplicates that device on Push 2/3. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.3 with Live 12.3, 2025-11-25, Additional Improvements; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | FEATURES §6/9/10: route a surface command through the same device identity and transaction system as a desktop edit; do not infer duplication’s automation or mapping policy. Measurement case M6-A6U-006. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push 2: layout navigation changes editor presentation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-007: 12.3.5: holding Layout opens a chooser; 16 Pitches, Macro Variations, XYZ Control and Rhythm Generator layouts unfold the piano roll. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.3.5 with Live 12.3.5, 2026-01-26, New Features and Improvements / Push 2 and 3; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | FEATURES §4/9: surface layout and editor presentation are linked state. This documents host navigation only; generator algorithms and built-in device DSP remain outside scope. Measurement case M6-A6U-007. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push 2: deleted device banks after undo

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-008: 12.3.5 fixes a Push 2/3 crash when reopening a Rack’s second parameter bank after deletion and Undo. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.3.5 with Live 12.3.5, 2026-01-26, Bugfixes / Push 2 and 3; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | ADR-0181/FEATURES §6/10: controller subscriptions must rebind to restored object identity instead of retaining dead parameter handles. The release note does not define all restored selection semantics. Measurement case M6-A6U-008. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push 2: rename remains a desktop action

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-009: 12.4.3 removes Push 2 clip/scene/track Rename menus; renaming remains in Live. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.4.3 with Live 12.4.3, 2026-07-14, Push Bugfixes; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | FEATURES §9: keep surface capability metadata separate from the underlying project command. A disabled or absent surface command does not remove desktop editing capability. Measurement case M6-A6U-009. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured; 12.4.3 is later than installed 12.4; do not project this menu state onto the installed build |

#### Current Push 2: observable layout state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-010: 12.2 and 12.3 notes describe observable pad_layout strings for Push 2/3 control surfaces in the Python API. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.2 with Live 12.2, 2025-06-11, improvements; repeated in Push 2.3 with Live 12.3, 2025-11-25, Additional Improvements; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | ADR-0181: expose surface layout as observable UI state rather than reconstructing it from note traffic. Repeated release entries do not establish a unique introduction version; stable enum values are undocumented here. Measurement case M6-A6U-010. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push integration: browser action placement and preview state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-011: 2.0/12.0.1 notes move Load Previous/Next to lower display buttons and make Preview color reflect activation. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.0 with Live 12.0.1, 2024-03-19, New Features and Improvements, shared Push integration bullets; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | FEATURES §9: qualify older controller browsing diagrams after A6D-010’s integration migration. Treat preview state and load destination as independent; exact colors and Push 2 hardware rendering remain open. Measurement case M6-A6U-011. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured; Push 2.0 here denotes the software release, not the Push 2 hardware generation; shared release bullet requires hardware verification |

#### Current Push 2: modulation indication in parameter feedback

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-012: 12.4: Push 2/3 adds a dot beside parameter names when Max for Live modulates them. | [Push with Live 12 — Release Notes](https://www.ableton.com/en/release-notes/push-12/), Push 2.4 and Live 12.4, 2026-05-05, Additional Features and Improvements / Push 2 and 3; accessed 2026-11-04. Versioned release history, selected entries from Push software 2.0/Live 12.0.1 through Push software 2.4.3/Live 12.4.3; Push software version is not hardware generation. | ADR-0181: the parameter feed needs modulation ownership as well as a displayed value; a surface indicator must distinguish modulation from automation or direct manual input. Measurement case M6-A6U-012. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push User Mode: input and feedback ports differ

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-013: User Mode requires Track+Remote on its input port and Remote on its output for LED feedback; custom mappings provide no display feedback. | [Push User Mode for custom MIDI mappings](https://help.ableton.com/hc/en-us/articles/209071249-Push-User-Mode-for-custom-MIDI-mappings), Push User Port MIDI Preferences; Custom MIDI Mapping; accessed 2026-11-04. Current unversioned support article, with hardware-specific Push 1/2 versus Push 3 pad descriptions. | ADR-0181/FEATURES §9: availability of input, outbound feedback and screen rendering are separate capabilities. This refines A6R-046 without assuming automatic control-surface setup configures custom mappings. Measurement case M6-A6U-013. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

#### Current Push User Mode: encoder touch and motion are separate

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-014: Encoder touch sends a note; rotation sends CC, independently mappable. Support recommends Relative (lin 2’s Comp.); the local manual instead recommends Relative (2’s Comp.). | [Push User Mode for custom MIDI mappings](https://help.ableton.com/hc/en-us/articles/209071249-Push-User-Mode-for-custom-MIDI-mappings), Custom MIDI Mapping / Encoders; compare local Live 12 manual §35.17 p.941; accessed 2026-11-04. Current unversioned support article, with hardware-specific Push 1/2 versus Push 3 pad descriptions. | ADR-0181: keep input decoding and acceleration policy explicit. A6R-046’s local-manual Relative (2’s Comp.) wording differs from current support; this is documentary disagreement, not a measured migration. Measurement case M6-A6U-014. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured; current support and local manual differ in recommended mapping label; installed interpretation unmeasured |

#### Current Push User Mode: reserved escape and parallel pitch bend

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6U-015: User cannot be mapped because it exits User Mode. Mapping the touch strip does not stop simultaneous pitch-bend output. | [Push User Mode for custom MIDI mappings](https://help.ableton.com/hc/en-us/articles/209071249-Push-User-Mode-for-custom-MIDI-mappings), Custom MIDI Mapping / Buttons and Touch strip; accessed 2026-11-04. Current unversioned support article, with hardware-specific Push 1/2 versus Push 3 pad descriptions. | ADR-0181/FEATURES §4/9: preserve an escape path and represent parallel performance/control messages explicitly; a mapping assignment need not consume every message from the physical control. Measurement case M6-A6U-015. | Supported hypothesis — official documentation; installed Live 12.4 and Push 2 hardware behavior unmeasured |

### Current synchronization, MIDI backend and video references

#### Link Audio identity survives display-name changes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-001: The public API separates peer/channel identifiers from mutable display names. A channel identifier is stable for that channel’s lifetime, and discovery callbacks report appearance, disappearance and renaming. This is not a documented promise of identity surviving application restart. | [Ableton Link developer documentation](https://ableton.github.io/link/), Link Audio / Channels; Discovering Link Audio Channels; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0126/0204 d5: keep the existing LAN-node decision; this refines E24’s protocol evidence without reopening product scope. FEATURES §2/9: endpoint bindings must not depend on labels. Measurement case M6-A6I-001. | Supported hypothesis — current official API/protocol documentation; installed Live 12.4 use and behavior unmeasured |

#### Link Audio arrival blocks and clock domains need adaptation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-002: Received buffers arrive on Link-managed threads and need not match the receiver’s block size or sample rate. beginBeats/endBeats map timing to the receiving session and yield no value for another Link session. | [Ableton Link developer documentation](https://ableton.github.io/link/), Receiving Audio; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0126/0204 d5 and ADR-0185: evidence informs the approved LAN node’s boundary tests; it does not assert a new Link Audio integration or a shared hardware sample clock. Measurement case M6-A6I-002. | Supported hypothesis — current official API/protocol documentation; installed Live 12.4 use and behavior unmeasured |

#### Link Audio publication must share the rendered timing snapshot

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-003: Publishing a buffer must use the same captured session state, quantum and beat position used to render it. Link Audio can be toggled without disabling beat/tempo synchronization; the API says to use LinkAudio instead of simultaneously instantiating Link. | [Ableton Link developer documentation](https://ableton.github.io/link/), Link Audio API; Sending Audio; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0185 / FEATURES §1: model timing synchronization and audio-stream lifecycle separately. ADR-0204 d5 remains the product decision. Measurement case M6-A6I-003. | Supported hypothesis — current official API/protocol documentation; installed Live 12.4 use and behavior unmeasured |

#### Link Audio discovery and localhost have separate failure cases

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-004: The support FAQ allows localhost between compatible apps or multiple Live instances. Across devices it requires a local network permitting peer communication, discovery and multicast. VPNs, firewalls and public/corporate isolation can prevent peer discovery; Wi-Fi support does not imply stable performance. | [Ableton Link Audio FAQ](https://help.ableton.com/hc/en-us/articles/25425913328924-Link-Audio-FAQ), Connecting with Link; Troubleshooting Link Audio; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0126/0204 d5 / FEATURES §9: diagnostics should distinguish unavailable peers, forbidden network traffic and insufficient streaming capacity; no router or firewall settings are changed by this research. Measurement case M6-A6I-004. | Supported hypothesis — current official support documentation; installed Live 12.4 behavior unmeasured |

#### Link Audio monitoring topology and buffer tuning interact

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-005: Monitoring at both sender and receiver creates a delayed duplicate; peers monitoring each other’s main output can feed back. The FAQ recommends monitoring once. It also proposes larger sender buffers and smaller receiver buffers when tuning Link latency, with peer-buffer level and dropouts guiding adjustment. | [Ableton Link Audio FAQ](https://help.ableton.com/hc/en-us/articles/25425913328924-Link-Audio-FAQ), How do I adjust for latency; Should I monitor Link Audio; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | FEATURES §2/8 and ADR-0126: separate routing feedback, duplicate monitoring and underrun diagnosis. This is vendor setup guidance, not a measured optimal buffer combination or an ADI default. Measurement case M6-A6I-005. | Supported hypothesis — current official support documentation; installed Live 12.4 behavior unmeasured |

#### Windows Link availability depends on the audio driver choice

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-006: Ableton’s FAQ says that from Live 9.7.1, selecting MME/DirectX audio disables Link and removes its transport-bar control; ASIO is required on Windows. This concerns the audio backend, not the separately named MIDI MME API. | [Ableton Link features and functions FAQ](https://help.ableton.com/hc/en-us/articles/209776125-Link-features-and-functions-FAQ), Does Link work with MME/DirectX drivers on Windows systems; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | FEATURES §1/14 / ADR-0185: expose capability dependencies rather than treating a missing synchronization control as an unrelated layout preference. Do not infer ADI must inherit this backend restriction. Measurement case M6-A6I-006. | Supported hypothesis — current official support documentation; installed Live 12.4 behavior unmeasured |

#### Windows MIDI service availability does not establish Live API adoption

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-007: Microsoft currently describes the service as present on updated supported Windows 11 releases from 25H2, with its developer API still preview. Legacy WinMM/WinRT MIDI 1.0 routes into the service and gains a subset including multi-client access; new-API outbound scheduling is a separate capability. | [Microsoft Windows MIDI Services overview/home](https://microsoft.github.io/MIDI/), Windows MIDI Services is Here; Benefits for everyone; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0188 d7 / FEATURES §14: ADI already requires displaying the active backend. Record OS build, service/runtime and API path independently; this source does not identify the installed Live backend. Measurement case M6-A6I-007. | Supported hypothesis — current official API/protocol documentation; installed Live 12.4 use and behavior unmeasured |

#### Modern Windows MIDI changes timestamp epoch and endpoint identity

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-008: The migration guide contrasts legacy incoming timestamps relative to port opening with new-API timestamps relative to system boot. New API timestamps are absolute clock values, not requested delays. Endpoint identifiers, rather than editable names or changing legacy port indices, identify devices. | [Microsoft Moving from WinMM to Windows MIDI Services](https://microsoft.github.io/MIDI/kb/moving-from-winmm-to-wms/), Concrete concept mapping; Incoming timestamps; Reacting to endpoints; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0054/0188 d7: convert timestamps at the backend boundary before ADI event scheduling and preserve endpoint identity separately from labels; port reopen must not silently shift recorded time. Measurement case M6-A6I-008. | Supported hypothesis — current official API/protocol documentation; installed Live 12.4 use and behavior unmeasured |

#### Windows MIDI enumeration is asynchronous even when API calls are synchronous

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-009: Microsoft warns that service initialization can take seconds and should not block the UI. Device discovery can lag physical connection; endpoint/legacy-port watchers announce availability and updates. A hardware-connect notification alone does not prove the MIDI endpoint is ready. | [Microsoft Moving from WinMM to Windows MIDI Services](https://microsoft.github.io/MIDI/kb/moving-from-winmm-to-wms/), A note on threading; Reacting to endpoints arriving and leaving; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0188 d7 / FEATURES §9/14: show pending versus available device state and retain an explicit fallback/reconnect policy; this is Windows integration evidence, not observed Live UI behavior. Measurement case M6-A6I-009. | Supported hypothesis — current official API/protocol documentation; installed Live 12.4 use and behavior unmeasured |

#### Windows scheduled MIDI has an explicit clock and finite horizon

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-010: MidiClock uses an unsigned 64-bit counter and reports TimestampFrequency; it is not wall time. Zero requests immediate sending. The documented maximum future timestamp is currently five minutes; exceeding it fails. Negative time offsets saturate at zero rather than wrapping. | [Microsoft MidiClock API](https://microsoft.github.io/MIDI/sdk-reference/MidiClock/), Static Properties; Static Functions for Offset; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0054/0188 d7: keep duration, backend timestamp, audio frame and musical beat types distinct; query clock constants instead of hard-coding present units or limits. Measurement case M6-A6I-010. | Supported hypothesis — current official API/protocol documentation; installed Live 12.4 use and behavior unmeasured |

#### Transport-origin MIDI timestamps differ from later service stamping

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-011: The transport specification requires timestamps in QPC tick space and prefers timestamps taken close to the wire. A zero transport timestamp means absent provenance and permits later service stamping; it is not the send API’s separate immediate-send meaning. | [Microsoft Developing MIDI Service Transport Plugins](https://microsoft.github.io/MIDI/kb/service-transport-plugin-development/), Timestamp format requirements (QPC ticks); accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0054/0188 d7: retain timestamp origin and conversion in diagnostics so input-driver delay cannot be mistaken for engine jitter. No claim is made about Live’s transport implementation. Measurement case M6-A6I-011. | Supported hypothesis — current official API/protocol documentation; installed Live 12.4 use and behavior unmeasured |

#### Installed Live Windows scheduling backend remains unresolved

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-012: The Microsoft sources specify operating-system capabilities, not which API Live 12.4 selects. They cannot establish whether this installation uses service-backed legacy ports, a native MIDI 2.0 path, future-timestamp scheduling, or any particular timestamp conversion. | [Microsoft Windows MIDI Services overview/home](https://microsoft.github.io/MIDI/), API capability boundary; inference limit of this source; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | ADR-0188 d7 / FEATURES §14: preserve the manual’s conditional MME statement and add a build/OS/backend investigation; do not convert an OS feature announcement into a measured Live capability. Measurement case M6-A6I-012. | Unknown / not tested — installed Live 12.4 backend adoption and timing semantics unmeasured |

#### Live 12 Windows video import no longer needs legacy codec installation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-013: Current support lists MOV and H.264 MP4 on both platforms, AVI additionally on Windows, and M4V on macOS. It says Live 12 Windows import no longer needs third-party codecs; Haali installation advice is explicitly for Live 11 and earlier. Images cannot be imported as video. | [Ableton Using Video](https://help.ableton.com/hc/en-us/articles/209773125-Using-Video), Importing video; Windows video settings / Codecs; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | FEATURES §3 and ADR-0206/0207: keep a decoder capability matrix by platform and container/codec. Historical codec requirements must not become ADI prerequisites or trigger installations here. Measurement case M6-A6I-013. | Supported hypothesis — current official support documentation; installed Live 12.4 behavior unmeasured |

#### Video seeking workload depends on keyframe density

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| A6I-014: Ableton recommends H.264 in MOV/MP4 with a keyframe every frame or every second frame for smoother playback; fewer keyframes can cause stutter. This is a preparation recommendation, not a promised acceptance limit, mandatory default or documented maximum resolution. | [Ableton Using Video](https://help.ableton.com/hc/en-us/articles/209773125-Using-Video), Optimizing playback; accessed 2026-11-04; current web revision, not pinned to installed Live 12.4 | FEATURES §3 / ADR-0206: measure scrubbing and random access independently from sequential decoding; preserve source-frame alignment while the preview catches up. Measurement case M6-A6I-014. | Supported hypothesis — current official support documentation; installed Live 12.4 behavior unmeasured |
