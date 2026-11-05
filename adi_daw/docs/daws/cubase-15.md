# Cubase 15 — research v0.5, 2026-11-04

Baseline: `5c9e1cd8ebbb5dd940815aa733d49c1217c9aa19`. Researcher: Codex on SALON-TV. Installed Cubase15.exe file version 15.0.5.114. Activated edition was not verified. The available manual is Cubase Pro 15.0.30: documented features below may depend on edition and may differ from installed 15.0.5.

Authority: current `docs/FEATURES.md`, `docs/DECISIONS.md`, `docs/LANES.md` and window parity checklists. Dream Reference v0.2 is director-facing context, not a superseding specification (director clarification, 2026-11-04). No product changes or live GUI tests were made.

Evidence keys resolve under private `D:\ADI-Research\2026-11-04-daws\EVIDENCE-INDEX.md`. Proprietary binaries, code extracts, projects and raw results stay there, outside this repository. Manual page numbers below are the printed pages in the local extracted chapters under git-ignored `reference/DOCS/`.

Statuses distinguish static facts, measured behavior, supported hypotheses and unknowns. A manual claim is labelled a supported hypothesis about the installed build. No row in this milestone is confirmed by DAW measurement. A string, import, symbol or pseudocode result does not prove a feature executes, its GUI behavior, its DSP precision or its CPU cost.

## Direct Offline Processing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| The manual describes a new processed file in the project Edits folder, unchanged original audio, processed ranges referring to the derivative, and process history saved with the project. | Pro 15.0.30, Direct Offline Processing p.575 | FEATURES §3; item FX / offline recipe plus derivative provenance; M-CUBASE-DOP | Supported hypothesis — documented, not measured on installed build |
| Auto Apply applies edits immediately; manual mode offers Apply/Discard. Processes can be changed, bypassed, removed and undone; favorites can hold individual processes or batches (36 across four banks). | pp.576–578, Favorites pp.583–585 | Separate pending controls, committed recipe, rendering job and active derivative; transaction grouping still to measure | Supported hypothesis — documented, not measured on installed build |
| Actual file paths/hashes, partial-range splice rules, shared clips, history after reopen and permanent-apply semantics have not been tested. | M-CUBASE-DOP | Do not infer filesystem or journal behavior from the dialog alone | Unknown / not tested |

## Fade editor and crossfades

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Event-based fades are realtime/per-event; clip-based fades affect events sharing that clip. Editor offers editable curve points, interpolation types, length, presets and As Default. | Fades, Crossfades, Event Volume pp.381,385–387 | FEATURES §3; Live handles at P0, Cubase editor at P2; ADR-0203 d7 | Supported hypothesis — documented, not measured on installed build |
| The exact shape count, curve equations, gain convention, overlap limits, equal-power/equal-gain behavior and combined undo remain unmeasured. | Crossfades pp.391–398; M-CUBASE-FADE | Do not promote the Dream phrase eight shapes to eight verified algorithms | Unknown / not tested |

## Range tool

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Moving/duplicating range contents splits intersected events; Insert Silence creates space at range start and shifts later material. | Range Editing pp.291–293 | N17; FEATURES §3; ADR-0203 d7; distinguish destructive silence from inserting timeline time | Supported hypothesis — documented, not measured on installed build |
| Split/Cut/Copy/Paste/Insert Silence/Delete Time across selected, hidden, locked and grouped tracks, automation and markers require an explicit membership/undo matrix. | Range Editing pp.285–294; M-CUBASE-RANGE | ADI operations must declare affected tracks/time domains; keep ripple scope explicit | Unknown / not tested |

## Track Versions, markers and Notepad

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Track Versions stores alternatives of events/parts on a track; the manual excludes automation tracks from Track Versions. | Track Handling, Track Versions pp.230–236 | FEATURES §3 snapshots.kind=track_version; do not assume a whole-track state snapshot matches Cubase | Supported hypothesis — documented, not measured on installed build |
| Marker properties and descriptions, and track Notepad, are documented separately. ADI threaded remarks and marker prompts are additions, not established Cubase behavior. | Markers pp.422–433; Track Inspector Notepad p.168; M-CUBASE-VERSIONS | FEATURES §1; ADR-0115, ADR-0131, ADR-0203 d17 | Supported hypothesis — documented, not measured on installed build |

## MixConsole and Control Room

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| MixConsole has channel sections/racks; exact processing order, movable order and history coverage require state/audio tests. Visual rack order alone does not prove DSP order. | MixConsole pp.434–514; M-CUBASE-MIX | ADR-0209; FEATURES §2; distinguish slot placement, bypass and channel state | Supported hypothesis — documented, not measured on installed build |
| The Pro manual permits up to four mono/stereo cue channels with talkback/click, plus a stereo phones channel that can select sources. | Control Room p.522; M-CUBASE-CUE | N23 and N43; model independent cue routing and monitor selection; ADI Reference bypass remains ADR-0209 | Supported hypothesis — documented, not measured on installed build |

## Render in Place and Export Audio Mixdown

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| The manual explicitly says Render in Place does not support side-chain routing. This is a documented limitation to test, not a rule to copy into ADI. | Rendering Audio and MIDI p.1471 | Freeze/bounce and export tests must retain sidechain context; FEATURES §2/8; M-CUBASE-EXPORT | Supported hypothesis — documented, not measured on installed build |
| Export supports a naming-scheme dialog and auto-incremented names. Channel batches, cycle-marker ranges, effects treatment, queue behavior and collision outcomes need exact job/file observations. | Export Audio Mixdown pp.1478–1502, especially p.1481 | ADR-0071, ADR-0205 d9; queue jobs carry source/range/treatment/format/name | Supported hypothesis — documented, not measured on installed build |

## Native module architecture and precision

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| baios.dll exposes GetPluginFactory/InitDll/ExitDll. GetPluginFactory lazily builds/reuses a factory and registers two constructors at 0x18000ac00 and 0x18000ac40. This is a component boundary, not the audio callback. | E10; factory VA 0x18000a3f0; REA/Ghidra 12.1.4; hash in E01 | Follow device-manager construction, callback registration and thread ownership in next static pass | Confirmed by static analysis |
| The same module imports MMCSS and thread/affinity APIs and contains ASIO Manager / sampleRateChanged / blockSizeChanged strings. These are search leads, not proof of scheduling policy. | E11; PE import table and string VAs | Prioritize callback/thread xrefs; do not infer lock freedom or deadline behavior | Supported hypothesis |
| Actual float/double boundaries, summing/pan laws, compensation, render precision, DSP costs and deadlines have not been measured. | M-AUDIO; M-CPU | ADR-0207 optional full-double path; compare equivalent settings only | Unknown / not tested |

## Expanded host research — 2026-11-04

Scope expanded by the director: all host functions are in the research inventory; built-in instrument and effect implementations are excluded. Editing utilities, MIDI transformations, modulation, routing, hosting and modular-system semantics remain in scope. Rows below are behavior contracts and ADI design implications, not new ADRs or permission to implement. An unknown is not a negative finding. Private evidence: E17–E20; precise validation missions: DEEP-CLAUDE-MISSIONS.md.

### F7: selection, ownership and disk state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C-D01: DOP applies to events, Pool clips or selected ranges, including multiple events. Only the selected part is processed. New audio goes into Edits; untouched portions still reference original audio. | Cubase Pro 15.0.30 manual, Direct Offline Processing, p.575; installed 15.0.5.114 requires verification | ADR-0069: model source spans, source identity, chain ownership and rendered derivatives explicitly; test a middle range rather than only whole clips. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D02: A shared clip can be processed for every referring event (Continue) or separated for the selected event (New Version); the prompt depends on the shared-clip preference. | Cubase Pro 15.0.30 manual, p.575; On Processing Shared Clips, pp.387–388; installed 15.0.5.114 requires verification | ADR-0069 / ADR-0160: distinguish a shared media file, a shared edit history and an alias; do not silently fan out edits. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D03: Processing history persists across project reopening, track archives and backups. Missing processes remain visibly unavailable. | Cubase Pro 15.0.30 manual, pp.575–576; installed 15.0.5.114 requires verification | ADR-0069 / missing-plugin preservation: retain opaque state and rendered audio independently; specify reopening without a processor. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D04: F7 opens/closes DOP. Locked events cannot be edited. Added processors recall last-used parameters; Reset restores defaults. | Cubase Pro 15.0.30 manual, pp.576–577,597; installed 15.0.5.114 requires verification | ADR-0203 d6 / ADR-0069: distinguish factory defaults, remembered tool settings and saved clip state; expose locks to the operation validator. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D05: Multi-event selection displays an alphabetically ordered aggregate list with counts of process instances. This is not a single event’s execution order. | Cubase Pro 15.0.30 manual, Applying Processing to Multiple Events, p.581; installed 15.0.5.114 requires verification | ADI DOP: represent an aggregate inspector separately from each ordered chain; mixed values and per-event differences need an explicit model. | Supported hypothesis — documented; installed behavior unmeasured |

### F7: apply, audition, cancellation and history

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C-D06: Auto Apply normally renders changes immediately. Turning it off exposes Apply/Discard; Enter applies when the window has focus. | Cubase Pro 15.0.30 manual, Auto Apply, pp.576–579; installed 15.0.5.114 requires verification | ADR-0069 / ADR-0110: separate editable draft, committed chain, current render and last usable render. Undo grouping requires M2-C-DOP. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D07: Reverse/Silence, favorites, batches, dragged insert chains and direct key commands can apply despite Auto Apply being off. | Cubase Pro 15.0.30 manual, pp.577,582,584–585,597; installed 15.0.5.114 requires verification | ADI DOP: command-level commit policy is needed; a global checkbox alone cannot describe the reference behavior. Decide explicitly before copying exceptions. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D08: Audition hears the chain prefix through the selected process, excluding later stages; Alt/Option audition bypasses the selected stage for comparison. | Cubase Pro 15.0.30 manual, DOP Toolbar, p.579; installed 15.0.5.114 requires verification | ADR-0069 / N23: use an audition prefix and comparison state separate from the final clip render and monitor gain. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D09: Modifying or deleting while a render runs starts a new render; visual progress remains available. Audition can begin before processing finishes. | Cubase Pro 15.0.30 manual, pp.576,581; installed 15.0.5.114 requires verification | ADI DOP research proposal: version render requests, reject obsolete completions, publish audio atomically and retain undo references. Cancellation/cache strategy is not established for Cubase. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D10: Reorder changes processing order. Bypass recalculates the chain and is saved. Processes with settings can be copied, but copying from a multi-event selection is unavailable. | Cubase Pro 15.0.30 manual, pp.587–588; installed 15.0.5.114 requires verification | ADR-0069: serialize order and bypass; cache keys must change on either. Test A→B versus B→A and bypass→undo→reopen. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D11: Making DOP permanent clears the process list and processed badge; the manual says this operation cannot be undone. | Cubase Pro 15.0.30 manual, pp.588–589; installed 15.0.5.114 requires verification | ADR-0069 explicitly requires undo and retained renders: keep that ADI decision; Cubase’s irreversible command is not a default to inherit. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D12: A learning plug-in can learn through audition with Auto Apply off; Discard cancels the processing application while the learned parameters remain for the next event. | Cubase Pro 15.0.30 manual, Applying Plug-in Effects with Learning Function, pp.582–583; installed 15.0.5.114 requires verification | ADR-0069: separate temporary learning state from committed render state. State transitions must be tested, not inferred from button names. | Supported hypothesis — documented; installed behavior unmeasured |

### F7: handles, tails, presets and channel boundaries

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C-D13: Extend Process Range uses available source before/after the event in milliseconds. It is global and mutually exclusive with Tail. | Cubase Pro 15.0.30 manual, pp.585–586; installed 15.0.5.114 requires verification | ADR-0069 / freeze fingerprint: distinguish source handles from synthetic output tail; include processing range and source bounds in cache identity. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D14: Tail is global, applies to plug-in effects rather than built-in processes, and normally enlarges the event. A previously resized event is an exception: its length is not automatically adjusted and the tail can mix with underlying clip audio. | Cubase Pro 15.0.30 manual, Tail, p.586; installed 15.0.5.114 requires verification | ADI: store render extent and timeline extent separately; test trimmed-source tails and adjacent clips before adopting any policy. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D15: Favorites hold one operation or an ordered batch and dedicated settings, up to 36 across four banks. Batches are global; unavailable plug-ins in a batch are skipped by the documented reference. | Cubase Pro 15.0.30 manual, Favorites / Batch Processing, pp.583–585; installed 15.0.5.114 requires verification | ADR-0069 / missing-plugin policy: distinguish global presets from project histories; ADI must not silently adopt the skip behavior. | Supported hypothesis — documented; installed behavior unmeasured |
| C-D16: Stereo effects applied to mono material use the left output; mixed channel configurations can produce processor-dependent results. | Cubase Pro 15.0.30 manual, Applying Processing, p.581; installed 15.0.5.114 requires verification | ADI: require explicit input/output channel negotiation for offline jobs; preserve source width unless a deliberate conversion is requested. | Supported hypothesis — documented; installed behavior unmeasured |

### Audio utilities are editing operations, not excluded effect instruments

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C-U01: DOP Gain spans −50 to +20 dB. Phase inversion can affect either stereo side or both; Reverse and Silence have no adjustable parameters. | Cubase Pro 15.0.30 manual, Built-In Audio Processes, pp.590–595; installed 15.0.5.114 requires verification | ADR-0069: utility operations need explicit units, channel masks and ranges; test silence and reversal at fractional range boundaries. | Supported hypothesis — documented; installed behavior unmeasured |
| C-U02: Peak normalization targets −50 to 0 dBFS. Loudness normalization targets −34 to 0 LUFS, skips audio shorter than 0.4 s and may create peaks over 0 dB. | Cubase Pro 15.0.30 manual, Normalize, p.591; installed 15.0.5.114 requires verification | ADR-0195 / export targeting: keep analysis, gain and peak protection separate; define short-file behavior rather than copying a threshold blindly. | Supported hypothesis — documented; installed behavior unmeasured |
| C-U03: Pitch Shift separates semitones/cents from time correction, supports multiple intervals and a pitch envelope. Time Stretch exposes original/result duration, bars/beats, percentage and a locator range. | Cubase Pro 15.0.30 manual, pp.592–597; installed 15.0.5.114 requires verification | AWRP / ADR-0061: unit conversions and source/output duration belong in the operation contract; matching labels is not matching stretch DSP. | Supported hypothesis — documented; installed behavior unmeasured |
| C-U04: Stereo Flip offers swap, either channel copied to both, merge and subtraction. Remove DC Offset has no parameters and is recommended for full clips. | Cubase Pro 15.0.30 manual, pp.594–595; installed 15.0.5.114 requires verification | ADI audio utilities: test asymmetric stereo and DC independently; exact merge/subtract scaling remains unmeasured. | Supported hypothesis — documented; installed behavior unmeasured |
| C-U05: Detect Silence has separate open/close thresholds, minimum open/closed times, pre/post-roll, preview with gaps muted, region creation or stripping, optional fades and per-event versus batch application. | Cubase Pro 15.0.30 manual, Audio Functions / Detect Silence, pp.603–606; installed 15.0.5.114 requires verification | FEATURES §3 / N17: represent detected ranges separately from the edit transaction; preview is read-only until Process. Threshold equality and sample rounding need tests. | Supported hypothesis — documented; installed behavior unmeasured |
| C-U06: Statistics distinguishes sample extrema, true peak, DC, estimated bit depth/pitch, AES17 RMS and loudness measures. Spectrum analysis is selection-scoped. | Cubase Pro 15.0.30 manual, pp.607–610; installed 15.0.5.114 requires verification | ADI analysis/Scope: record metric definition, window and selected channels. A statistics readout does not establish internal processing precision. | Supported hypothesis — documented; installed behavior unmeasured |

### Fade editor and advanced crossfade contract

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C-F01: Event fades are realtime and can differ between events sharing a clip. DOP fades are clip processing. Crossfades are event-based; auto fades are another independent layer. | Cubase Pro 15.0.30 manual, Fades, Crossfades, and Event Volume, p.381; installed 15.0.5.114 requires verification | FEATURES §3 / ADR-0203 d7: preserve these ownership distinctions; double-click fade editor is P2 while handles remain P0. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F02: Fade editor supports spline, damped spline and linear interpolation, editable points, presets, Restore since opening, numeric length with Apply Length, As Default and Apply. | Cubase Pro 15.0.30 manual, Fade Dialog for Event-Based Fades, pp.385–387; installed 15.0.5.114 requires verification | ADI fade model: separate curve shape, time extent, preset and dialog draft. Presets/defaults include length; mixed selected lengths need a test. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F03: Range boundaries determine fades differently when the range is inside, before or beyond the event; multiple selected events can receive the same fade type. | Cubase Pro 15.0.30 manual, Creating Fades with the Range Selection Tool, pp.383–385; installed 15.0.5.114 requires verification | N17: enumerate range/event intersection cases; tests must include partial overlap, several tracks and unequal event lengths. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F04: Crossfades require overlapping events or overlapping source clips; the latter may resize events. Without source overlap they cannot be created. | Cubase Pro 15.0.30 manual, Crossfades, p.388; installed 15.0.5.114 requires verification | ADI: validate source handles before placing a crossfade; adjacent timeline rectangles alone are insufficient. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F05: Advanced crossfade exposes separate in/out curves, length, overlap, splice offset, symmetry, event volume, and Move Fade versus Move Audio nudges. Moving audio applies to the incoming event, not the outgoing one. | Cubase Pro 15.0.30 manual, Crossfade Editor, pp.389–395; installed 15.0.5.114 requires verification | ADI: distinguish boundary edits from source-slip edits. Track these as separate typed operations with one transaction for a linked gesture. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F06: Chaining can move later events to the track end, only until a gap, or not at all. Symmetry couples curve controls; numeric length can preserve splice positions. | Cubase Pro 15.0.30 manual, pp.391,393–395; installed 15.0.5.114 requires verification | N17 / ripple editing: capture downstream affected IDs before applying; test a gap, overlaps, locked events and undo as a group. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F07: Equal Gain keeps the summed amplitude factors constant; Equal Power keeps the power factors constant. Equal Power restricts curve editing to one point. | Cubase Pro 15.0.30 manual, p.391; installed 15.0.5.114 requires verification | ADI fade testing: correlated copies, independent noise and inverted copies reveal different transitions. Do not promise identical loudness for every signal. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F08: Audition can isolate fade-out, full crossfade or fade-in with pre/post-roll and its own level. Previous/next navigation can optionally change Project selection. | Cubase Pro 15.0.30 manual, p.392; installed 15.0.5.114 requires verification | ADI: separate audition transport, monitor level, edited crossfade and selection-follow policy; test normal playback after closing. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F09: Auto fades span 1–500 ms, may inherit project settings or override per track, and are not drawn as fade lines. | Cubase Pro 15.0.30 manual, Auto Fades, pp.396–398; installed 15.0.5.114 requires verification | ADR-0203 d7: retain ADI automatic-edge-fades-off default. Hidden audio treatment must be inspectable; do not equate invisible with absent. | Supported hypothesis — documented; installed behavior unmeasured |
| C-F10: Event volume curves travel with moved/copied events and allow Bézier segments; curve point selection supports tilt, compression and vertical scaling. Event level offset is a separate control. | Cubase Pro 15.0.30 manual, Event Volume Curves, pp.398–403; installed 15.0.5.114 requires verification | ADR-0115: keep curve gain, scalar gain and fades independently editable; evaluation order and clipping remain M2-C-FADE questions. | Supported hypothesis — documented; installed behavior unmeasured |

### Arrangement range operations

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C-R01: Moving/duplicating range contents splits intersected events so only the portion inside moves. Nudging the range selection itself can leave content untouched. | Cubase Pro 15.0.30 manual, Range Editing, pp.290–291; installed 15.0.5.114 requires verification | N17: selection state is not project content; one range edit needs an atomic split/move transaction. | Supported hypothesis — documented; installed behavior unmeasured |
| C-R02: Delete/Cut leave empty space; Delete Time/Cut Time close it. Insert Silence shifts later events by the selected duration. | Cubase Pro 15.0.30 manual, pp.292–293; installed 15.0.5.114 requires verification | FEATURES §3 / N17: use separate commands and define selected-track/global-track scope explicitly; ripple is not a synonym for delete. | Supported hypothesis — documented; installed behavior unmeasured |
| C-R03: Split cuts at both selection edges. Crop removes outside material from intersected events but leaves wholly inside/outside events unaffected. | Cubase Pro 15.0.30 manual, pp.293–294; installed 15.0.5.114 requires verification | N17: test disjoint and fully covered events; crop must not be implemented as deleting every unselected event. | Supported hypothesis — documented; installed behavior unmeasured |
| C-R04: Paste collision, automation following, hidden/locked tracks, tempo/marker shifts and clipboard ownership across projects are not established here. | Cubase Pro 15.0.30 manual, Range Editing, pp.285–294; M2-C-RANGE; installed 15.0.5.114 requires verification | ADI acceptance matrix: evaluate each before claiming Cubase-level range editing. | Unknown / not tested |

### Render in Place and export are different job contracts

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C-E01: Render in Place supports separate events, adjacent blocks or one combined event. Dry transfers effects; more inclusive processing choices bake more of the path, optionally including master effects. | Cubase Pro 15.0.30 manual, Rendering Audio and MIDI, pp.1471–1476; installed 15.0.5.114 requires verification | ADR-0059 / ADR-0071: specify render source, baked stages, output width and what stays live. Verify exact Channel Settings boundary; manual wording is not a graph trace. | Supported hypothesis — documented; installed behavior unmeasured |
| C-E02: Track render can keep, mute, disable, remove or hide sources; event render exposes keep/mute. Tail and output location are independent settings; settings are remembered. | Cubase Pro 15.0.30 manual, pp.1473–1477; installed 15.0.5.114 requires verification | ADI freeze/bounce: source-state change belongs in the same undo transaction as placement; remembered settings are not defaults. | Supported hypothesis — documented; installed behavior unmeasured |
| C-E03: The manual says Render in Place does not support sidechain routing. | Cubase Pro 15.0.30 manual, p.1471; installed 15.0.5.114 requires verification | Preserve ADI compensated sidechain requirement. Test the native limitation, but do not inherit it as an architectural restriction. | Supported hypothesis — documented; installed behavior unmeasured |
| C-E04: Export batches select channels and locator/cycle-marker/arranger ranges. Effects choices distinguish dry pre-pan, channel post-pan, groups/sends and master inclusion. | Cubase Pro 15.0.30 manual, Export Audio Mixdown, pp.1480–1485; installed 15.0.5.114 requires verification | ADR-0071 / ADR-0205 d9: every stem job needs an explicit tap and shared-return policy. Stem sums are not automatically the full mix. | Supported hypothesis — documented; installed behavior unmeasured |
| C-E05: Naming composes ordered attributes, separators and padded counters with previews. Queue admission rejects invalid ranges and non-unique generated names. | Cubase Pro 15.0.30 manual, pp.1486–1491; installed 15.0.5.114 requires verification | ADI exporter: resolve the entire output path set before rendering; test Unicode, repeated marker names, case collisions and existing files. | Supported hypothesis — documented; installed behavior unmeasured |
| C-E06: Up to 20 jobs persist with the project; Update Job changes one selected job. After Export is outside saved per-job settings. Cancel removes already-exported files while retaining the queue. | Cubase Pro 15.0.30 manual, pp.1491–1494; installed 15.0.5.114 requires verification | ADR-0071: decide immutable job snapshot versus live project state explicitly; use temporary output publication and an ADI-approved cancellation policy. | Supported hypothesis — documented; installed behavior unmeasured |
| C-E07: Real-time export is needed for external hardware; when all channels cannot be handled together, the reference retries smaller batches. | Cubase Pro 15.0.30 manual, p.1485; installed 15.0.5.114 requires verification | ADI real-time jobs: record external device dependencies and transport state. No offline-equivalence or CPU comparison is proven. | Supported hypothesis — documented; installed behavior unmeasured |
| C-E08: BWF/iXML, channel splitting, mono downmix, RF64 and speaker layout are separate export choices. The manual’s repeated float32 wording does not settle every processing mode. | Cubase Pro 15.0.30 manual, pp.1495–1500; installed 15.0.5.114 requires verification | ADR-0207 / format export: test metadata independently of samples and selected engine precision; do not repeat ambiguous manual text as measured fact. | Supported hypothesis — documented; installed behavior unmeasured |

### Static command-label evidence — implementation boundary still open

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C-S01: Ghidra decoded two RIP-relative LEA instructions referring to the DOP command label. This binds the label to installed executable bytes; it does not identify the renderer, cache or undo handler. | E20: Cubase15.exe SHA-256 291b02c4a80243bb8d7d12f4ce8dfc55d5fef79fba555f412ee4d68cd36b13ff; instruction VAs 0x1430c0a99 and 0x1430c53af, label VA 0x145f02960 | Further reverse-engineering must recover caller/function boundaries and execution relationships before deriving an internal architecture. The DOP contract above is documentary. | Confirmed by static analysis — scope limited to the named artifact |


## Host-wide coverage queue

The whole host is in scope; these rows keep unclosed areas visible. They are research questions, not claims about the app or new ADI commitments. A feature named below is not established absent merely because it is not covered above.

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| To establish: Arrangement objects and project organization | Source inventory: Ch.5–9,17–19 | Part/event/shared-copy identity, snap/glue/slip/resize, arranger chains, markers/notes and Track Versions. N17; ADR-0115/0160; FEATURES §1/3. | Unknown / not tested — remaining scope |
| To establish: Audio editor depth | Source inventory: Ch.15,26,28–33 | Hitpoint threshold/group quantize, AudioWarp, tempo definition, alignment, VariAudio segments and ARA document ownership. ADR-0061/0176; FEATURES §3. | Unknown / not tested — remaining scope |
| To establish: Recording and media lifecycle | Source inventory: Ch.13–14,35–36 | Cycle lanes/comping, pre-record, monitor routes, Pool references/regions/cleanup, relink and archive safety. FEATURES §8; ADR-0132. | Unknown / not tested — remaining scope |
| To establish: MixConsole and monitoring | Source inventory: Ch.4,20–24 | Rack order, channel settings/history/snapshots, cue/talkback/listen, panners, output tap boundaries and direct routing. N23/N43; no built-in strip DSP study. | Unknown / not tested — remaining scope |
| To establish: Automation, remote controls and host modulation | Source inventory: Ch.38,43–46 | Touch/latch/trim/write/read, suspend/fill, remote takeover, modulation mapping persistence and parameter state. N26; ADR-0046/0181. | Unknown / not tested — remaining scope |
| To establish: MIDI composition and event transformations | Source inventory: Ch.39,47–50,53–57 | Pattern editor as note workflow, logical/input/project transforms, quantize/legato/humanize, CC lanes and chord tools; no bundled MIDI-effect DSP. FEATURES §4. | Unknown / not tested — remaining scope |
| To establish: Expression and articulation | Source inventory: Ch.51–52 | Expression Maps, per-note data and MPE import/export; identity across duplicate/version/undo. FEATURES §4; ADR-0054. | Unknown / not tested — remaining scope |
| To establish: Tempo, sync, video, surround and interchange | Source inventory: Ch.37,58,61–64 | Musical/linear time, ramps, timecode/video, surround routing and file handoffs; device algorithms excluded. FEATURES §1/8; ADR-0206. | Unknown / not tested — remaining scope |
| To establish: Project, preferences, shortcuts and performance | Source inventory: Ch.3,6,11–12,42,65–68 | Settings defaults versus remembered state, file recovery, plug-in scan/blacklist, ASIO-Guard/latency and CPU. ADR-0131/0207; no measured results yet. | Unknown / not tested — remaining scope |



## Whole-Dream research expansion — 2026-11-04

Coverage is driven by [the whole-feature register](../DAW-RESEARCH-COVERAGE.md), not just the initial examples. Manual claims remain supported hypotheses about the installed build; all live behavior, undo and performance checks remain pending. REAPER is included only where Dream/current ADI explicitly references it. E25/E26 identify new static artifacts. See whole-host test missions in the private research root.

### Analysis, pitch editing and extension ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C3-001: Hitpoints may be detected on import/record; locked points survive filtering, disabled points are excluded, and moving a point locks it by default. | Cubase Pro 15.0.30, Hitpoints pp.636–640; installed 15.0.5.114 | ADR-0176: cache analysis candidates separately from user edits/locks; rebuilding analysis must preserve authored choices. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-002: Threshold, Intensity, Minimum Length and Beats filter different properties. Hitpoints can generate slices, grooves, markers, regions, events, warp markers or MIDI notes. | Cubase Pro 15.0.30, Hitpoints pp.637–641; installed 15.0.5.114 | One detector can feed multiple reversible operations, but each output type has its own identity and timing contract. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-003: Slicing a clip also replaces events referring to that edited clip, according to the manual. | Cubase Pro 15.0.30, Hitpoints p.641; installed 15.0.5.114 | ADR-0160: test shared media versus shared edit history before deciding fan-out; offer explicit scope rather than silently affecting unrelated placements. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-004: VariAudio analyzes monophonic material into tonal segments and non-tonal gaps. Gaps may need manual inclusion to participate in pitch edits. | Cubase Pro 15.0.30, VariAudio pp.659,662–663; installed 15.0.5.114 | Reserve segment identity, analysis revision, boundaries and unpitched regions; note blocks alone cannot represent the full source. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-005: VariAudio has separate pitch correction, pitch-curve straightening, formant shift and segment volume. Length-affecting edits and many offline processes can invalidate its data. | Cubase Pro 15.0.30, VariAudio pp.659–661; installed 15.0.5.114 | Future pitch editor: dependency invalidation and reanalysis must be explicit; preserve editable source state across DOP, crop, undo and reopen. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-006: ARA extensions can attach to individual events or an entire audio track; track attachment includes later events and ignores audio parts. | Cubase Pro 15.0.30, Extensions pp.703–706; installed 15.0.5.114 | ARA requires document/source/region ownership beyond an insert plug-in. Track-level ownership and per-event detach need different commands. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-007: Make Extension Permanent is documented as non-undoable and affects other events referring to the same audio file; it processes clip material beyond trimmed event edges. | Cubase Pro 15.0.30, Extensions p.705; installed 15.0.5.114 | Do not copy this destructive policy into ADI’s reversible operations. Test shared references and preserve a recoverable source/recipe. | Supported hypothesis — documented; installed behavior unmeasured |

### Arrangement, markers and time domains

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C3-008: One arranger track defines arbitrarily sized, possibly overlapping sections; several ordered/repeated chains can reuse them without moving timeline events. | Cubase Pro 15.0.30, Arranger Track p.404; installed 15.0.5.114 | Dream §6: distinguish section definitions, chain instances and linear arrangement; test overlap, tempo/meter changes and jump discontinuities. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-009: Flatten can target one or several chains/new projects, optionally make real copies, and change treatment of notes crossing section edges. It may remove material. | Cubase Pro 15.0.30, Arranger Track pp.407–408; installed 15.0.5.114 | Define flatten as a new reversible derivation with explicit boundary-note and alias policy, not an unqualified rearrange command. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-010: Position and cycle markers differ. Up to ten marker tracks can exist, but one is active; active-track selection controls ordinary marker editing; Audio Mixdown has its own marker-track chooser (C4-004). | Cubase Pro 15.0.30, Markers pp.422–424; installed 15.0.5.114 | Dream §6/13: marker identity, range endpoints and active-list filter are separate. ADI notes/prompts remain additions, never auto-executed on transport. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-011: Musical track time base makes audio event starts follow tempo changes, but does not itself stretch the audio. Linear time base is distinct. | Cubase Pro 15.0.30, Tempo and Time Signature pp.1446–1447; installed 15.0.5.114 | ADR-0061/time model: placement anchoring and content warp are independent switches; test a tempo ramp with both combinations. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-012: Tempo points can step or ramp; time-signature edits snap to bar starts even though tempo snap is configurable. | Cubase Pro 15.0.30, Tempo and Time Signature pp.1448–1449; installed 15.0.5.114 | Time conversions and meter edits need separate invariants; test bar numbering, negative/preroll time and marker/export ranges. | Supported hypothesis — documented; installed behavior unmeasured |

### Automation writing, gaps and transport transitions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C3-013: Write enables Read. A plug-in moved between insert slots on one channel retains automation; moving it to another channel does not retain it according to this manual. | Cubase Pro 15.0.30, Automation pp.870–872; installed 15.0.5.114 | ADI stable device/parameter IDs should survive authorized moves; measure reference loss behavior rather than adopting it silently. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-014: Automation Follows Events overwrites destination automation. Selecting another displayed parameter hides the old curve without deleting it. | Cubase Pro 15.0.30, Automation pp.874,880–881; installed 15.0.5.114 | N26: model content movement separately from lane presentation, including collision policy and complete undo. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-015: Virgin Territory represents genuine gaps without an automation curve; terminator points and range deletion create boundaries. | Cubase Pro 15.0.30, Automation pp.881–882; installed 15.0.5.114 | No-data is not zero, hold-last or a straight line. Define evaluation on entry, seek, loop and stopped edits. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-016: Touch punches out on release; Auto-Latch continues to stop/write-disable; Cross-Over punches out when the re-touched control crosses the prior curve. | Cubase Pro 15.0.30, Automation p.884; installed 15.0.5.114 | Dream §10: represent writing mode and pass state separately; test takeover after seeks and hardware touch loss. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-017: Trim modifies a prior pass; freezing trim merges it into the automation curve, manually or at configured pass/mode boundaries. | Cubase Pro 15.0.30, Automation pp.884,889,894; installed 15.0.5.114 | Separate offset layer from committed curve; capture the precise commit event and undo representation. Exact trim-domain math remains unmeasured. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-018: Fill replaces a defined region with the punch-out value; options include back to punch/start, to end and loop. Fill can be one-shot or remain enabled. | Cubase Pro 15.0.30, Automation pp.890–892; installed 15.0.5.114 | A single gesture can affect time outside the touched interval. Preview affected range and include it in the transaction. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-019: Continue Writing on Transport Jump determines whether a locate interrupts writing. Return Time controls return to the existing curve; Reduction Level removes/simplifies recorded events. | Cubase Pro 15.0.30, Automation p.894; installed 15.0.5.114 | Recording must declare discontinuity policy, smoothing and simplification separately; do not equate a visually similar reduced curve with sample-identical playback. | Supported hypothesis — documented; installed behavior unmeasured |

### MixConsole, monitor paths and recall

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C3-020: MixConsole history includes parameter and modulator edits; recalling one of up to ten project-saved mix snapshots can be undone in that history. | Cubase Pro 15.0.30, MixConsole pp.443–444; installed 15.0.5.114 | Dream §7/12: test interaction with project undo. ADI’s unified operation journal must not accidentally create unrelated histories. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-021: The docked and separate MixConsole do not follow each other’s visibility changes. Channel-strip modules can be reordered. | Cubase Pro 15.0.30, MixConsole pp.443,504; installed 15.0.5.114 | Dream §1/7: synchronized visibility and moved-not-duplicated panels are ADI choices; distinguish view ordering from signal ordering. Processor algorithms remain excluded. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-022: Direct Routing provides a main destination plus seven additional destinations post-fader/post-pan. Summing Mode enables simultaneous outputs rather than switching one destination. | Cubase Pro 15.0.30, MixConsole pp.492–494; installed 15.0.5.114 | Routing: represent fan-out versus exclusive selection explicitly; first destination defines reference channel width in Cubase, not an ADI requirement. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-023: Control Room retains configuration when disabled. Its Main Mix route differs from other output buses; only one monitor set is active. | Cubase Pro 15.0.30, Control Room pp.520–521; installed 15.0.5.114 | N23/N43: separate render outputs from monitor selection and app/device configuration; test device reconnect and output reassignment. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-024: Each cue channel creates per-channel sends with level, pan and pre/post. Cue sources can select main mix, external input or its dedicated cue mix; talkback/click are separate. | Cubase Pro 15.0.30, Control Room pp.522–524; installed 15.0.5.114 | Build a route/tap truth table for performer, engineer, reference and export. Never assume Main mute or monitor dim affects all these paths. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-025: AFL/PFL selects listen tap after/before fader and pan; Listen Dim controls how much main mix remains. Talkback supports click-latch versus hold-momentary. | Cubase Pro 15.0.30, Control Room p.528; installed 15.0.5.114 | Reference/cue design: model selection, dim and momentary input gestures independently; test overlapping talkback/listen/reference commands. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-026: Cue, Monitor and Control Room insert slots are saved in Control Room presets, not the project. Monitor inserts follow Control Room faders. | Cubase Pro 15.0.30, Control Room p.530; installed 15.0.5.114 | Project versus workstation state matters for portable projects; speaker correction must not silently enter exported stems. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-027: Use Current Mix Levels copies selected fader values to cue sends and switches them pre-fader. Reset Cue Sends disables them, sets 0 dB and restores post-fader. | Cubase Pro 15.0.30, Control Room p.531; installed 15.0.5.114 | Batch actions change multiple fields, not only displayed gain; capture one undo operation and exact selected-channel scope. | Supported hypothesis — documented; installed behavior unmeasured |

### Articulation and query-based editing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C3-028: Expression-map Directions persist until a succeeding direction (within exclusion groups); Attributes apply to individual notes. Sound slots translate semantic articulation into instrument-specific messages. | Cubase Pro 15.0.30, Expression Maps p.1262; installed 15.0.5.114 | FEATURES §4: articulation is not raw keyswitch MIDI. Keep semantic identity separate from library mapping and fallbacks. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-029: Note expression follows notes through quantize, move, copy, duplicate and delete. VST note expression availability depends on the instrument; ordinary CC is channel-wide except poly pressure. | Cubase Pro 15.0.30, Note Expression pp.1293–1295; installed 15.0.5.114 | ADR-0054: preserve curves attached to note IDs; test simultaneous equal-pitch notes and route-specific capability fallback. | Supported hypothesis — documented; installed behavior unmeasured |
| C3-030: Logical Editor separates AND/OR predicates, function and transform actions. Project Logical Editor adds pre/post commands and supports undo. | Cubase Pro 15.0.30, Logical Editor pp.1405–1407; Project Logical Editor pp.1424–1425; installed 15.0.5.114 | Dream §14 agent/API: reuse explicit query-plus-operation plans and preview resolved objects; do not turn arbitrary prose into direct database writes. | Supported hypothesis — documented; installed behavior unmeasured |

## Base-reference audit and targeted expansion — 2026-11-04

Research scope now explicitly requires complete Live host coverage as the ADI base, Cubase export capabilities, and current Bitwig UI behavior. These entries add documented contracts; installed behavior remains unmeasured. See E30–E32 in the private evidence index.

### Quick Export, stem selection and range rules

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C4-001: Quick Export uses Main Mix and derives its range from project events, ignoring locators. Effects can extend the end by up to 30 seconds; an active arranger chain instead defines the range without a tail. | Cubase Pro 15.0.30 manual, Quick Audio Export p.1478; installed 15.0.5.114 | ADR-0071: keep quick export as a separate preset/contract; test leading silence, isolated late events, long tails and active arranger chains. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-002: Quick Export offers WAV at project sample rate/24-bit and MP3 at 44.1 kHz. The local manual prints an ambiguous MP3 bitrate unit; the actual encoded bitrate remains unverified. | Cubase Pro 15.0.30 manual, p.1478; installed 15.0.5.114 | Do not copy a suspected unit error into ADI defaults. Inspect produced stream metadata and measure decoded duration/encoder padding. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-003: Audio Mixdown exports activated output, audio, instrument, FX-return and group channels. MIDI channels themselves are excluded. Selection synchronization filters the list; MIDI-driven instruments require selecting the VSTi channel. | Cubase Pro 15.0.30 manual, pp.1480,1494; installed 15.0.5.114 | Distinguish selected UI tracks, eligible audio endpoints and checked export jobs; test hidden children and instrument multi-outs. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-004: Locator, selected cycle-marker ranges and selected arranger chains are separate range sources. Export has its own marker-track chooser; arranger repeat modes are ignored. | Cubase Pro 15.0.30 manual, pp.1480–1481; installed 15.0.5.114 | Correct the earlier overbroad active-marker-track implication in C3-010. Test selected export marker track independently of the active project marker track; render repeated chain instances explicitly. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-005: Output Routing determines export width; the manual warns that no selected main output bus produces silence. Effects from an event before the export range may remain audible inside it. | Cubase Pro 15.0.30 manual, pp.1489–1490; installed 15.0.5.114 | Jobs need graph context and pre-range state, not just intersecting clips. Test disconnected routes and preceding reverb/sidechain material without silently trimming state. | Supported hypothesis — documented; installed behavior unmeasured |

### Processing taps, names, jobs and output ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C4-006: Multiple-channel export distinguishes DRY before pan, channel processing after pan, complete groups/sends, and groups/sends plus output processing. Post-pan routing can turn a mono source into stereo output. | Cubase Pro 15.0.30 manual, Effects pp.1484–1485; installed 15.0.5.114 | Record tap, width and shared-return contribution independently. Test stem recombination under nonlinear/shared processing; never promise sum-to-mix by naming alone. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-007: Existing-path conflicts offer ask, increment-to-unique or overwrite. Naming attributes depend on the selected channel/range and include ordered free text, channel metadata, project, counter and cycle-marker identity. | Cubase Pro 15.0.30 manual, pp.1482,1486–1488; installed 15.0.5.114 | Resolve all output paths before work. Test duplicate ranges, case-insensitive collisions, Unicode, padding and changed names after enqueue. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-008: Up to 20 project-saved jobs capture channel selection, range, name/path, format and effects. Update Job modifies only the selected job; After Export is outside job state. | Cubase Pro 15.0.30 manual, pp.1490–1494; installed 15.0.5.114 | Distinguish saved settings from a frozen project revision. Test editing source media/automation after enqueue and changing After Export before starting. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-009: Queue admission rejects invalid ranges and generated-name collisions. Cancel stops the queue and removes already exported files while keeping the job list. | Cubase Pro 15.0.30 manual, p.1491; installed 15.0.5.114 | ADI needs an explicit publication/rollback policy. Test cancellation only in a disposable destination; record completed versus partial files and pre-existing collision handling. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-010: Create Audio Track also enables Insert to Pool; disabling Pool disables track creation. New Project is limited to uncompressed output outside the project Audio folder and carries exported tracks plus tempo/signature. | Cubase Pro 15.0.30 manual, p.1485; installed 15.0.5.114 | Model dependent switches and post-render import as a separate transaction; test insertion time, active-project change, errors and undo. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-011: Realtime export is required for external paths and some plug-ins; external audio needs monitoring. Overloaded realtime channel batches are restarted with fewer channels. External MIDI input suppression is separately selectable. | Cubase Pro 15.0.30 manual, pp.1485–1486; installed 15.0.5.114 | Record reproducibility and live-input isolation per job. Test restart/tails without inferring offline equivalence or performance from UI progress. | Supported hypothesis — documented; installed behavior unmeasured |

### Formats, channel conversion and interchange exports

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C4-012: Audio Mixdown lists WAV, AIFF, MP3, FLAC and Ogg Vorbis. WAV/AIFF documentation lists integer and floating-point bit depths; the FLAC subsection repeats float-depth prose that requires independent verification. | Cubase Pro 15.0.30 manual, pp.1495–1501; installed 15.0.5.114 | Keep a format capability matrix rather than one universal bit-depth selector. Validate file headers/decoded samples; do not infer engine precision or FLAC support from repeated prose. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-013: Interleaved, split-channel, mono-downmix and surround L/R-only are distinct choices. Stereo mono-downmix uses project pan law; the described surround fold divides the sum by channel count. | Cubase Pro 15.0.30 manual, pp.1483,1496; installed 15.0.5.114 | Test channel order, gains, LFE, polarity and metadata. A stereo extraction is not a surround downmix; expose that distinction in ADI. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-014: BWF, Wave marker chunks, iXML tempo/project metadata, dual-mono labels, Wave Extensible and RF64 have separate switches. Marker metadata can include specified marker tracks independently of audio range selection. | Cubase Pro 15.0.30 manual, pp.1483–1484; installed 15.0.5.114 | Test metadata without normalizing samples; cover large files, non-ASCII text, negative/project offsets and marker positions outside the rendered interval. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-015: Format presets are application-wide and included in profiles; they store type, sample rate, bit depth and channel mode. Naming schemes and project-saved jobs have different ownership. | Cubase Pro 15.0.30 manual, pp.1487–1488,1501–1502; installed 15.0.5.114 | Separate workstation presets from portable job settings; test missing/renamed presets on another machine. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-016: AAF can reference or embed media and consolidate events with millisecond handles; frame quantization changes placement. Muted-event export can encode minus-infinity gain. | Cubase Pro 15.0.30 manual, pp.1545–1547; installed 15.0.5.114 | ADR-0206: quantify edit-point loss and retained source handles. Keep interchange separate from flattened stems and from native history. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-017: OMF export excludes MIDI, video and unused Pool audio; OMF 2 has optional clip gain, fades and names. Handles and frame quantization are explicit. | Cubase Pro 15.0.30 manual, pp.1551–1553; installed 15.0.5.114 | Publish a per-field loss report and test a second importer, rather than treating successful open as a faithful round trip. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-018: Cubase documents DAWproject export/import for specified track types, routing, mixer data and automation; insert plug-ins are limited to VST3 in the listed support. ADM BWF is a separate bed/object/metadata export. | Cubase Pro 15.0.30 manual, pp.1547–1548; installed 15.0.5.114 | Keep native interchange, object audio and rendered delivery separate. Test unsupported plug-in state, note expression and routing rather than assuming format-level support proves host support. | Supported hypothesis — documented; installed behavior unmeasured |
| C4-019: Video export uses locator range and a selected stereo output. The detailed section offers H.264/MP4 resolutions through UHD, project frame rate and AAC audio at 44.1/48 kHz; this conflicts with the chapter introduction’s Full-HD-only wording. | Cubase Pro 15.0.30 manual, pp.1526,1540–1541; installed 15.0.5.114 | ADR-0205: retain version-qualified codec/capability tests, frame/sample alignment and exact selected audio path. Do not silently adopt the introductory restriction. | Supported hypothesis — documented; installed behavior unmeasured |

## Deep host research — VariAudio, workspaces and Live priority — 2026-11-04

This pass prioritizes Live as the base reference and expands Cubase VariAudio, multi-display/workspace behavior, key commands and preferences. All new entries are documentary or explicitly unresolved; none is a new installed-app measurement. Source hashes and per-row cases are in private E34–E36 and DEEP-HOST-MISSIONS-5.md. Existing ADI priorities/deviations remain unchanged.

Source manual: Cubase Pro 15.0.30; installed: 15.0.5.114. VariAudio research preserves its current ADI priority and does not authorize a segment model implementation. Separate windows, lower-zone editors, linked mixers and arbitrary detachment are different capabilities; do not describe every panel as detachable.

### VariAudio — VariAudio scope and prerequisites

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-001: VariAudio targets monophonic material, especially vocals. Analysis must segment it before pitch/timing edits. The manual describes edits as undoable in both the Sample Editor window and lower-zone editor. | Cubase Pro 15.0.30 Operation Manual, Pitch Editing and Time Correction with VariAudio p.659; installed Cubase 15.0.5.114 requires verification | FEATURES §3 lists VariAudio as P3 with no segment model; this research identifies contracts without promoting implementation priority or claiming measured quality. Measurement case M5-C5V-001. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Analysis invalidation by other edits

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-002: Length-changing file edits can invalidate VariAudio data. The guide says most offline processes, plug-in processing, and Sample Editor cut/paste/delete/draw may trigger reanalysis; Envelope, Fade In/Out, Normalize and Silence are excluded from the listed built-in trigger set. | Cubase Pro 15.0.30 Operation Manual, VariAudio and Offline Processes p.659; installed Cubase 15.0.5.114 requires verification | ADR-0069: preserve original media and authored pitch state separately from analysis derivatives; invalidation needs operation-specific dependency rules. Measurement case M5-C5V-002. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Segments, gaps and pitch representation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-003: Segments represent tonal portions and retain pitch curves; gaps represent unpitched regions. Weak consonants/breaths may require manual inclusion to receive edits. Segment pitch is derived from the curve average; the displayed waveform is mono even for stereo/multichannel files. | Cubase Pro 15.0.30 Operation Manual, Segments and Gaps pp.662–663; installed Cubase 15.0.5.114 requires verification | A future segment model needs tonal confidence/gaps, curve and representative pitch independently of source channel count. The display does not prove mono processing. Measurement case M5-C5V-003. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Segment boundary correction versus time warp

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-004: Alt/Option-dragging the start/end control changes segmentation boundaries without allowing overlap into the next segment. Ordinary use of the same smart control warps timing. Boundary correction recalculates average pitch. | Cubase Pro 15.0.30 Operation Manual, Smart Controls pp.667–668; Changing the Start and End Point of Segments p.671; installed Cubase 15.0.5.114 requires verification | Separate analysis-region editing from source-to-timeline mapping; a common handle must not imply one data operation. Measurement case M5-C5V-004. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Split/glue and representative pitch

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-005: Splitting or gluing segments recalculates average pitch. Glue targets the preceding/following segment or a contiguous selected group; splitting can change the resulting displayed pitch. | Cubase Pro 15.0.30 Operation Manual, Splitting Segments p.669; Gluing Segments p.670; installed Cubase 15.0.5.114 requires verification | Maintain parent/child lineage for segment edits; preserve intended pitch changes when representation changes. Do not assume note identity survives split/glue unchanged. Measurement case M5-C5V-005. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Segment deletion is a distinct unresolved operation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-006: Backspace deletes selected VariAudio segments. The cited procedure does not establish whether removal resets prior edits, creates an unpitched gap or changes rendered source audio. | Cubase Pro 15.0.30 Operation Manual, Deleting Segments p.670; installed Cubase 15.0.5.114 requires verification | Keep segment deletion distinct from event deletion and sample deletion; define its audio and history consequences before ADI implements it. Measurement case M5-C5V-006. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Pitch snapping and temporary modifiers

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-007: Absolute pitch snap targets semitones; Relative retains the original cents deviation; Off allows free movement. Shift plus arrow keys moves in cents. Drag modifiers can temporarily select Off, Absolute or Relative. | Cubase Pro 15.0.30 Operation Manual, Pitch Snap Mode / Raising or Lowering Pitches p.674; installed Cubase 15.0.5.114 requires verification | Pitch coordinates need separate note center, cents offset and snapping policy; keyboard and drag gestures may have different override semantics. Measurement case M5-C5V-007. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — MIDI-driven pitch assignment

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-008: MIDI Input changes selected pitches, ignores controller data and restricts the documented pitch range to E0–C5. For multiple segments, the first selected segment moves to the incoming note and all others move by the same interval. | Cubase Pro 15.0.30 Operation Manual, Changing Pitches using MIDI Input pp.675–676; installed Cubase 15.0.5.114 requires verification | Reuse explicit selection anchor and transposition semantics; do not treat MIDI input as replacing all selected notes with one pitch. Measurement case M5-C5V-008. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — MIDI step assignment

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-009: With MIDI Input and Step enabled, assigning a note selects the next segment automatically. The guide treats this as a stopped-mode sequential pitch-entry workflow. | Cubase Pro 15.0.30 Operation Manual, Step Mode p.676; installed Cubase 15.0.5.114 requires verification | Step entry needs a cursor/selection transition separate from transport and actual recorded note events. Measurement case M5-C5V-009. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Partial pitch correction versus command quantization

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-010: Correct Pitch can be adjusted with a slider or segment handle. Its assigned key command immediately quantizes segments to the nearest semitone; pitch operations automatically select Solo processing. | Cubase Pro 15.0.30 Operation Manual, Correcting Pitches pp.676–677; Raising or Lowering Pitches p.675; installed Cubase 15.0.5.114 requires verification | Separate correction amount from target pitch and from one-shot quantization. Renderer/algorithm changes are state that must be represented and undoable. Measurement case M5-C5V-010. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Pitch-curve anchors, tilt and rotation

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-011: Tilt can affect the beginning or end of a curve. The default reference anchor is the segment center, can move horizontally and can be reset. Alt/Option plus the tilt control rotates around the anchor. | Cubase Pro 15.0.30 Operation Manual, Setting Tilt/Rotate Anchors / Tilting / Rotating pp.680–681; installed Cubase 15.0.5.114 requires verification | Store curve transformation and anchor as distinct authored state; do not collapse rotation into an absolute pitch offset. Measurement case M5-C5V-011. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Straightening ranges preserves transitions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-012: Pitch straightening reduces curve fluctuation. Separate range handles restrict it to a subrange, allowing surrounding transitions to remain; multiple selected segments can receive a common range setting. | Cubase Pro 15.0.30 Operation Manual, Smart Controls p.668; Straightening Pitch Curves / Ranges pp.681–682; installed Cubase 15.0.5.114 requires verification | Pitch-center correction and local expressive-curve shaping need independent operations and per-segment normalized-range semantics. Measurement case M5-C5V-012. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Timing edits and tempo following

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-013: VariAudio timing edits create warp markers also visible in AudioWarp and alter adjacent segment timing. These edits do not themselves follow project tempo; Musical Mode is needed. Warp comparison uses AudioWarp bypass, distinct from VariAudio pitch/formant/volume bypass. | Cubase Pro 15.0.30 Operation Manual, Timing Modifications / Warping Segments pp.682–683; VariAudio Inspector p.660; installed Cubase 15.0.5.114 requires verification | ADR-0061/0176: share source-to-musical mapping instead of adding an unrelated timing engine; keep tempo-lock state and bypass domains explicit. Measurement case M5-C5V-013. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Formant state is separate from pitch and timing

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-014: Formant shift uses positive/negative percentages with zero unchanged; it can apply to a selection and is documented not to alter pitch or timing. Solo processing is selected automatically. | Cubase Pro 15.0.30 Operation Manual, Formant Shifting pp.684–685; installed Cubase 15.0.5.114 requires verification | Keep formant control, pitch trajectory and time map independent; the numerical transfer function and quality still require audio measurements. Measurement case M5-C5V-014. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Segment volume and mute

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-015: Segment volume can be changed through the Inspector or the segment handle; reducing it to mute or using Edit > Mute silences the segment. | Cubase Pro 15.0.30 Operation Manual, Editing Volume p.685; installed Cubase 15.0.5.114 requires verification | Keep segment gain/mute separate from clip gain, event-volume envelope and track gain. Define composition order and boundaries. Measurement case M5-C5V-015. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Scale suggestion scope and linked editor state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-016: Scale suggestions use selected segments, or all displayed segments when none are selected. The Editor Scale selection is shared between Sample Editor and Key Editor. | Cubase Pro 15.0.30 Operation Manual, VariAudio Inspector p.661; Selecting a Musical Scale pp.671–672; installed Cubase 15.0.5.114 requires verification | Selection-scoped analysis and project/editor scale ownership are separate; changing a view preference can affect another editor. Measurement case M5-C5V-016. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Chord-derived pitch targets and edit snapping

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-017: Scale Assistant can derive targets from chord events, scale events or both. Quantize Pitches and live pitch snapping use the chosen source plus Pitch Snap Mode; chord/scale matching depends on the current event at that time. | Cubase Pro 15.0.30 Operation Manual, Using Chord Track pp.672–673; Quantizing / Snapping pp.677–679; installed Cubase 15.0.5.114 requires verification | FEATURES §3/4: musical guidance is time-varying data, not one global set of allowed semitones; preserve Relative cents offsets separately. Measurement case M5-C5V-017. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — MIDI reference ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-018: MIDI reference notes are read-only lines in Sample Editor. The reference-track selection applies to all Sample Editors in the project and is saved with it; the reference is unavailable when the editor is opened from the Pool. | Cubase Pro 15.0.30 Operation Manual, Showing MIDI Reference Tracks pp.683–684; installed Cubase 15.0.5.114 requires verification | Separate project-wide comparison target from window-local display state and editable segment data. Measurement case M5-C5V-018. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Pitch extraction representations

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-019: MIDI extraction offers note-only, static pitchbend, continuous pitchbend, Note Expression pitchbend and VST3 Tuning curves. Ordinary pitchbend uses a configurable range of 1–24 that must match the destination. | Cubase Pro 15.0.30 Operation Manual, Extract MIDI Dialog pp.687–688; installed Cubase 15.0.5.114 requires verification | ADR-0054: preserve note-relative pitch versus channel-wide bend and destination capability. Use a loss/capability matrix for each route. Measurement case M5-C5V-019. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Amplitude extraction representations

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-020: Extract MIDI can produce fixed velocity, amplitude-dependent velocity, a channel controller curve, per-note controller expression or VST3 Volume expression; VST3 expression requires a compatible destination. | Cubase Pro 15.0.30 Operation Manual, Extract MIDI Dialog pp.688–689; installed Cubase 15.0.5.114 requires verification | Do not equate source amplitude, note-on velocity and continuous gain; ADI expression should preserve the original semantic dimension. Measurement case M5-C5V-020. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Extraction destination and replacement semantics

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-021: Extraction can place a part on the first selected MIDI/instrument track, a new track or the clipboard. First Selected Track deletes prior extraction parts on that track. Pool-only source material is inserted at project start. | Cubase Pro 15.0.30 Operation Manual, Extract MIDI Dialog Destination p.689; Functions Menu p.686; installed Cubase 15.0.5.114 requires verification | Make generated-output ownership explicit; replacing prior derived parts is different from appending arbitrary user notes. Extract uses the whole event according to the function overview. Measurement case M5-C5V-021. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Dimension-specific reset

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-022: Functions can reset pitch centers, pitch curves, formants, volume, warp or all changes. With a selection, the reset targets selected segments; without one, it targets all. | Cubase Pro 15.0.30 Operation Manual, Functions Menu p.686; installed Cubase 15.0.5.114 requires verification | Separate edit dimensions and selection scope in typed operations; reset should not require rerunning unrelated analysis. Measurement case M5-C5V-022. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Reanalysis is not a parameter reset

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-023: Reanalyze Audio clears segmentation and analyzes the whole event again, whereas individual reset commands target edit dimensions. | Cubase Pro 15.0.30 Operation Manual, Functions Menu p.686; installed Cubase 15.0.5.114 requires verification | Cache analysis revision and user-authored segmentation independently; explicit reanalysis needs a declared retention policy for authored edits. Measurement case M5-C5V-023. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Flatten scope contradiction

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-024: The Functions Menu description says Flatten Real Time Processing considers the entire event; the detailed procedure permits selecting segments and says none selected means all. The true installed selection scope is unresolved. | Cubase Pro 15.0.30 Operation Manual, Functions Menu p.686 versus Flattening Real Time Processing p.689; installed Cubase 15.0.5.114 requires verification | ADR-0069: do not infer full-event or selection-only commit from one passage; represent proposed scope clearly and preserve sources. Measurement case M5-C5V-024. | Unknown / not tested — conflicting manual scope descriptions |

### VariAudio — Flatten algorithm and loss of editable state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-025: Flattening allows Realtime or MPEX for stretch factors 0.5–2 and forces Realtime outside that range; MPEX is unavailable on Apple silicon. The result disables Musical Mode and loses VariAudio pitch/volume/formant data. | Cubase Pro 15.0.30 Operation Manual, Flattening Real Time Processing pp.689–690; installed Cubase 15.0.5.114 requires verification | ADR-0069 retains original and redo render: ADI can preserve editable recipes even if the reference flattening discards them. Platform capability and algorithm are render inputs. Measurement case M5-C5V-025. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Harmony generation creates independent editing copies

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-026: Generate Harmony Voices analyzes the selected event and creates up to four corresponding track copies with independent events. Chord-track mode uses voicing; without it the guide specifies +3, −3, −6 and −9 semitones. Vibrato reduction is selectable. | Cubase Pro 15.0.30 Operation Manual, Harmony Voices / Generate Harmony Voices pp.690–691; installed Cubase 15.0.5.114 requires verification | Represent generated tracks/events as attributed derived objects, with source lineage and independent editable pitch state rather than assumed shared aliases. Measurement case M5-C5V-026. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Multi-clip display and editing scope differ

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-027: Sample Editor separates visible clips from editable scope: All Clips, Active Clip or All Clips on Active Track. Showing only the active clip is a separate display choice. | Cubase Pro 15.0.30 Operation Manual, Sample Editor Toolbar, View Options p.632; installed Cubase 15.0.5.114 requires verification | Avoid deriving the target edit set solely from visible waveforms; ADI needs explicit active clip, display set and edit set. Measurement case M5-C5V-027. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Segment display, navigation and selection

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-028: Segment colors can follow voice, event, pitch or scale/chord membership; pitch/scale colors share Key Editor schemes. Arrow keys step segments, Alt/Option gestures zoom, and Ctrl/Cmd-click on a piano key selects matching pitches including slight deviations. | Cubase Pro 15.0.30 Operation Manual, Segment Colors pp.664–665; Navigation/Selection p.666; installed Cubase 15.0.5.114 requires verification | Window parity should distinguish semantic coloring, tolerance-based selection and view history. Exact pitch tolerance and persisted zoom remain unmeasured. Measurement case M5-C5V-028. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Bypass, audition and editor location

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-029: VariAudio bypass is described for pitch/formant/volume; warp bypass lives in AudioWarp. Audition can loop selected segments. The editor can move between lower zone and a separate window; their Info/Overview visibility states are independent. | Cubase Pro 15.0.30 Operation Manual, VariAudio Inspector p.660; Auditioning p.666; Sample Editor pp.613–615,633; installed Cubase 15.0.5.114 requires verification | Separate comparison path from persistent edits and from workspace state; moving a panel must preserve data identity while allowing view-specific layout. Measurement case M5-C5V-029. | Supported hypothesis — documented in Cubase Pro 15.0.30; installed 15.0.5.114 behavior unmeasured |

### VariAudio — Unestablished sharing, persistence and journal details

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5V-030: The cited VariAudio chapter does not establish the full native serialization schema, shared-clip edit fan-out, every gesture undo boundary or recovery after interrupted analysis/flattening. General undoability is not that evidence. | Cubase Pro 15.0.30 Operation Manual, VariAudio pp.659–691; source gaps explicitly retained; installed Cubase 15.0.5.114 requires verification | FEATURES §3 VariAudio remains unspecified. Future design needs immutable source identity, analysis/version ownership, authored segment/curve data, deterministic derived renders and operation journaling; these are proposed ADI questions, not recovered Cubase internals. Measurement case M5-C5V-030. | Unknown / not tested — ownership, serialization and full gesture behavior require evidence |

### Multiple screens, command customization and preferences — Multiple windows and displays

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-001: Multiple MixConsole windows can show different channel types or link their scrolling, visibility configurations, filters and fader-related toolbar behavior across displays. The lower-zone MixConsole is excluded from that link. | Cubase Pro 15.0.30 manual, MixConsole pp.434,437,462; installed 15.0.5.114 requires verification | ADR-0063/0180: Cubase permits multiple concurrent views; ADI ordinary panel detachment reparents one component. Do not silently replace ADI ownership with cloned mixers. Measurement case M5-C5W-001. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-002: The lower-zone MixConsole is a fixed Project-window area with independently controlled visibility. Some separate-window facilities, such as Channel Overview and Meter Bridge, are unavailable there. | Cubase Pro 15.0.30 manual, MixConsole pp.434,439; installed 15.0.5.114 requires verification | Detachment is not proven to be a universal operation with identical controls. Inventory docked/separate capability differences before choosing ADI parity. Measurement case M5-C5W-002. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Editor opening and focus

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-003: Double-click/open-close-editor commands and explicit Audio/MIDI editor-opening commands have independently configurable destinations: separate window or lower zone. Drum-map preference can override the default MIDI editor. | Cubase Pro 15.0.30 manual, Editors p.1603; Project Window pp.87–89; installed 15.0.5.114 requires verification | ADR-0063 and window parity: command identity, chosen editor and host destination are separate decisions. Preserve Live baseline defaults unless an ADR already chooses otherwise. Measurement case M5-C5W-003. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-004: Changing the lower-zone MIDI editor via its local chooser is temporary; next opening uses the default editor. Editor Content Follows Event Selection separately controls whether open editors change content with Project selection. | Cubase Pro 15.0.30 manual, Project Window pp.88–89; Editors p.1603; installed 15.0.5.114 requires verification | Separate transient view choice, default preference, pinned content and active selection. A window remaining open does not prove it stays attached to the same clip. Measurement case M5-C5W-004. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-005: Link Project and Lower Zone Editor Cursors links cursors and zoom for Key, Drum and Audio Part editors, excluding Sample Editor; ruler time formats remain independent. | Cubase Pro 15.0.30 manual, Project Window p.89; installed 15.0.5.114 requires verification | ADR-0112/0129: linked position, linked scale and displayed time units are independent state. Preserve ADI current selection-centered zoom ruling under ADR-0203. Measurement case M5-C5W-005. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-006: Channel Settings selection can follow MixConsole edits/selection, with a separate follow toggle. Project-to-MixConsole selection synchronization is controlled by another preference. | Cubase Pro 15.0.30 manual, Channel Settings p.500; Editing—Project & MixConsole p.1600; installed 15.0.5.114 requires verification | One shared selected object is not sufficient for every floating inspector; model pin/follow policy explicitly without adding the rejected dedicated ADI Inspector. Measurement case M5-C5W-006. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-007: Project zones receive command focus separately. Tab/Shift+Tab cycle zones; opening the lower editor transfers focus, and touching a zone toolbar or info line focuses that zone. | Cubase Pro 15.0.30 manual, Project Window pp.99–101; installed 15.0.5.114 requires verification | ADR-0180: global transport keys and local editing commands need an explicit focus hierarchy, particularly across detached windows. Measurement case M5-C5W-007. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Workspaces and persistence

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-008: Global workspaces belong to the computer; project workspaces travel with the project. A workspace stores important window/dialog sizes, positions and layouts. External projects ordinarily use the last local view. | Cubase Pro 15.0.30 manual, Workspaces pp.1576–1577; installed 15.0.5.114 requires verification | ADR-0063/0129/0180: distinguish portable layout intentions from workstation display geometry. Manual evidence does not establish monitor-ID storage or missing-display clamping. Measurement case M5-C5W-008. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-009: Open Projects in Last Used View offers Never, Only External Projects and Always. No Workspace restores the last saved unassigned view; edited workspaces require Update Workspace or saving another workspace/type. | Cubase Pro 15.0.30 manual, Workspaces pp.1577–1578; General p.1609; installed 15.0.5.114 requires verification | Separate current unsaved view, named saved layout, project-saved view and local restore policy. Avoid ambiguous automatic profile writes. Measurement case M5-C5W-009. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-010: Workspace key assignments stay at their list positions when workspaces are moved/deleted; global/project lists are separate, and changing scope requires saving another workspace type. | Cubase Pro 15.0.30 manual, Workspaces Organizer pp.1578–1579; installed 15.0.5.114 requires verification | Shortcut binding by ordinal differs from binding by workspace identity. ADI must choose deliberately and show resulting reassignment in its command editor. Measurement case M5-C5W-010. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-011: Windows management can act on the selected window, associated windows in Cascaded mode, or all windows. It provides activation, minimize/restore, close and selected-window layout reset. | Cubase Pro 15.0.30 manual, Windows Dialog pp.1583–1584; installed 15.0.5.114 requires verification | A recovery path for hidden/off-screen views is valuable independently of detachment. Cascaded here describes operation scope, not an assumed geometric window layout. Measurement case M5-C5W-011. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Display scaling and stacking

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-012: Application Scaling is relative to OS scaling. Windows HiDPI documents supported factors; Plug-in Editors Always on Top is a separate preference affecting plug-in panels. | Cubase Pro 15.0.30 manual, General p.1608; User Interface p.1618; VST—Plug-ins p.1624; installed 15.0.5.114 requires verification | ADR-0129/0203 retain independent ADI-window scaling and third-party ownership. Cubase documentation does not prove per-monitor/per-window scale persistence or universal always-on-top behavior. Measurement case M5-C5W-012. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Profiles and settings ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-013: Profiles include preferences, toolbar/track-control settings, global workspaces, bus presets, format presets, collections, quantize/crossfade presets and key commands. Studio Setup, live Audio Connections, Control Room presets, track/device presets and templates are excluded. | Cubase Pro 15.0.30 manual, Profiles p.1579; installed 15.0.5.114 requires verification | ADR-0125/0203 profiles need an explicit inclusion matrix. A portable profile is not a complete workstation backup, and bus presets are not active hardware routing. Measurement case M5-C5W-013. | Supported hypothesis — documented; installed behavior unmeasured |
| C5W-014: Edits to included settings save in the active profile. New Profile starts from factory settings, Duplicate copies a selected profile, and activation requires application restart; the active profile cannot be deleted. | Cubase Pro 15.0.30 manual, Profiles pp.1579–1583; installed 15.0.5.114 requires verification | Keep desired and effective profile explicit and expose restart requirements. ADI settings remain outside musical undo under current ADRs. Measurement case M5-C5W-014. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Shortcut discovery and multiple bindings

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-015: The key-command search switches between capturing a pressed key and searching a command name; filters distinguish all, assigned, customized and unassigned commands. One function can retain multiple alternative key bindings. | Cubase Pro 15.0.30 manual, Key Commands pp.1555–1558; installed 15.0.5.114 requires verification | ADR-0125/0203: search both directions, show inherited/custom assignments and avoid replacing a binding when the user intends an alternative. Measurement case M5-C5W-015. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Shortcut persistence and reset

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-016: Key commands apply across projects and save as XML presets; loading a preset replaces previous key settings and macros. Per-command reset, reset-all and clear-all are distinct. Export Key Command Assignments produces an HTML listing. | Cubase Pro 15.0.30 manual, Key Commands pp.1554,1556,1559–1560; installed 15.0.5.114 requires verification | Command shortcuts are App scope under ADR-0203; distinguish importable configuration from readable documentation and merge versus replace. Measurement case M5-C5W-016. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Macros and transaction boundaries

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-017: Macros are ordered command lists; insertion occurs after the selected command, ordering is editable, and macros can be duplicated, executed from the editor/Edit menu or assigned keys. The text does not establish atomic undo or failure handling. | Cubase Pro 15.0.30 manual, Key Commands pp.1556–1559; installed 15.0.5.114 requires verification | ADI scripting/command planning should reuse validated commands; do not assume a macro is one journal operation merely because one key runs it. Measurement case M5-C5W-017. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Shortcut interception

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-018: Displaying the On-Screen Keyboard reserves usual shortcuts, with a documented exception set including Save, transport/record, delete, cycle, Transport panel and keyboard visibility. | Cubase Pro 15.0.30 manual, Default Key Commands p.1560; installed 15.0.5.114 requires verification | Define command dispatch priority and exemptions for note-entry mode; a global assignment alone does not guarantee execution in every context. Measurement case M5-C5W-018. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Mouse modifiers and context access

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-019: Tool modifiers are edited separately by category/action; assignment replaces prior modifiers and can prompt. Right-click may show a toolbox instead of the context menu; adding a modifier accesses the context menu. | Cubase Pro 15.0.30 manual, Editing—Tool Modifiers/Tools pp.1601–1602; installed 15.0.5.114 requires verification | ADR-0125 approves modifier presets, but Live remains default. Keep gesture mappings separate from command shortcuts and preserve keyboard/accessibility access to menus. Measurement case M5-C5W-019. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Preferences transactions and partial presets

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-020: Preferences provide Apply, OK and Cancel; Defaults resets only the active page. Partial preference presets store marked pages and leave other pages unchanged when loaded. | Cubase Pro 15.0.30 manual, Preferences pp.1593–1595; installed 15.0.5.114 requires verification | ADI intentionally uses immediate application in its current settings parity; adopt scoped presets/reset semantics without silently copying Cubase staged Apply/Cancel behavior. Measurement case M5-C5W-020. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Control interaction modes

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-021: Knobs support circular, relative circular and linear input; sliders support jump, touch, ramp and relative modes. Mouse-wheel behavior can change parameters everywhere, only outside scrollable areas, or only scroll. | Cubase Pro 15.0.30 manual, Editing—Controls pp.1598–1599; installed 15.0.5.114 requires verification | ADR-0125/0203 keep Live vertical drag and wheel-scroll defaults, with documented optional modes. Gesture coalescing and accidental edits matter to journal semantics. Measurement case M5-C5W-021. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Selection-driven workspace behavior

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-022: Enlarge Selected Track enlarges the current selection and restores the previously selected track to its original size. Selection-sync, scroll-to-track, auto-arm for MIDI/audio, and solo-on-selection are separate preferences. | Cubase Pro 15.0.30 manual, Editing—Project & MixConsole p.1600; installed 15.0.5.114 requires verification | This is a concrete comparison for Bitwig Auto-zoom restoration. Selection is not purely visual when optional arm/solo policies are active. Measurement case M5-C5W-022. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Selection, locking and range semantics

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-023: Lock Event Attributes independently covers position, length and other edits. Range Tool Splits Partly Selected Events can split material during move/nudge/mute/phase inversion; Delete Overlaps removes hidden sections and Shift temporarily overrides it. | Cubase Pro 15.0.30 manual, Editing pp.1595–1596; installed 15.0.5.114 requires verification | FEATURES §3: command scope is a function of preferences and event locks. Preserve a reproducible effective-settings snapshot in tests. Measurement case M5-C5W-023. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Audio editing policy and source ownership

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-024: Preferences control import prompts/conversion, bounce replacement versus keeping current audio, removing regions/hitpoints during offline processing, and creating a new version versus processing an existing shared clip. | Cubase Pro 15.0.30 manual, Editing—Audio pp.1597–1598; installed 15.0.5.114 requires verification | ADR-0069/0203: DOP/shared-clip behavior depends on app policy. ADI’s explicit import/bounce prompt default remains its own decision. Measurement case M5-C5W-024. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Overlap playback versus mute

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-025: Treat Muted Audio Events like Deleted allows an underlying overlapped event to play when the top event is muted. Simple Crossfade Editor selects a reduced dialog rather than deleting advanced fade capability. | Cubase Pro 15.0.30 manual, Editing—Audio p.1597; installed 15.0.5.114 requires verification | Mute may change which event supplies audio, not just gate the top event. Keep simplified/advanced editor preference separate from stored fade representation. Measurement case M5-C5W-025. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — MIDI split and controller continuity

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-026: Split MIDI Events can split crossing notes into new notes. Split MIDI Controllers inserts a nonzero controller value at the second part start; the difference becomes audible when the first part is removed or the second moved. | Cubase Pro 15.0.30 manual, Editing—MIDI pp.1599–1600; installed 15.0.5.114 requires verification | FEATURES §4: preserve event identity and initial controller state explicitly across split, move and deletion; uninterrupted playback alone cannot validate the edit. Measurement case M5-C5W-026. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Zoom behavior and rendering

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-027: Zoom can anchor on selection start or project cursor. Quick Zoom defers event/part redraw until zoom input stops; horizontal-only Zoom tool prevents height changes. | Cubase Pro 15.0.30 manual, Editing—Zoom p.1602; Project Window p.101; installed 15.0.5.114 requires verification | ADR-0203 keeps Live selection-centered zoom; Cubase options inform optional preferences and performance tests, not silent parity replacement. Measurement case M5-C5W-027. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Display state and visibility thresholds

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-028: Event Display controls separate minimum heights for content and names, overlap indication, opacity and curve/fade visibility; reduced opacity can affect UI responsiveness according to the guide. | Cubase Pro 15.0.30 manual, Event Display pp.1604–1605; installed 15.0.5.114 requires verification | Small-track usability depends on visibility thresholds and hit targets. Do not infer hidden data was deleted or UI cost from a static screenshot. Measurement case M5-C5W-028. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Autosave and bounded history

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-029: Auto Save creates backups for dirty open projects with interval and maximum-count controls, overwriting oldest backups at the limit. Maximum Undo Steps is separate; unsaved projects also receive backup names. | Cubase Pro 15.0.30 manual, General pp.1608–1609; installed 15.0.5.114 requires verification | ADI persistent operations/history are distinct from periodic backup and bounded in-memory undo. Keep retention/dependency completeness explicit. Measurement case M5-C5W-029. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Monitoring and PDC preferences

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-030: Auto Monitoring offers Manual, While Record-Enabled, While Record Running and Tapemachine Style. Delay Compensation Threshold limits which latent plug-ins Constrain Delay Compensation affects. | Cubase Pro 15.0.30 manual, VST p.1623; installed 15.0.5.114 requires verification | FEATURES audio monitoring and ADR-0203: monitor state, recording placement and graph latency are separate. The fourth monitoring mode is already an ADI-approved option. Measurement case M5-C5W-030. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Mute behavior and prior state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-031: Mute Pre-Send when Mute changes pre-fader send behavior. Group Channels: Mute Sources as well propagates mute to direct sources, and the guide warns previously muted sources are unmuted when the group is unmuted. | Cubase Pro 15.0.30 manual, VST p.1623; installed 15.0.5.114 requires verification | Define inherited versus explicit mute so ADI does not accidentally erase prior user intent. Reference behavior is a caution, not an adoption requirement. Measurement case M5-C5W-031. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Plug-in lifecycle preference

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-032: Suspend VST3 processing when no audio is received is configurable; the guide recommends leaving it deactivated. Auto-opening editors and warning before removing modified effects are separate options. | Cubase Pro 15.0.30 manual, VST—Plug-ins p.1624; installed 15.0.5.114 requires verification | Do not equate silence detection with safe suspension or use the preference as measured efficiency proof. ADI needs correct tails, autonomous sources and wake behavior. Measurement case M5-C5W-032. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Recording buffers and timing preferences

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-033: Audio Pre-Record Seconds buffers incoming audio in stop/playback; retrospective MIDI capacity is measured in events. MIDI record catch range and ASIO recording compensation are separate settings. | Cubase Pro 15.0.30 manual, Record pp.1615–1616; installed 15.0.5.114 requires verification | Buffers measured in time versus event count have different retention guarantees. Separate capture timestamp correction from monitor latency and UI duration. Measurement case M5-C5W-033. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Transport and audition command scope

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-034: Playback Toggle Triggers Local Preview makes Space control Sample Editor/Pool preview under the documented conditions, otherwise project transport. Scrub quality and insert processing can differ from playback. | Cubase Pro 15.0.30 manual, Transport pp.1616–1617; installed 15.0.5.114 requires verification | ADR-0180 global transport decisions need explicit exceptions; distinguish local audition ownership and processing policy from the main transport. Measurement case M5-C5W-034. | Supported hypothesis — documented; installed behavior unmeasured |

### Multiple screens, command customization and preferences — Recovery and settings persistence

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| C5W-035: Safe Mode distinguishes temporarily disabling preferences, permanently deleting them, and disabling third-party plug-ins. Preference deletion is non-undoable and affects all installed Cubase versions; RAMpresets.xml writes on application exit. | Cubase Pro 15.0.30 manual, Customizing pp.1584–1586; installed 15.0.5.114 requires verification | ADR-0125 Safe Mode should preserve reversible recovery and explicit scope. Never manufacture this test by deleting the user profile; save/exit boundaries matter to portability. Measurement case M5-C5W-035. | Supported hypothesis — documented; installed behavior unmeasured |
