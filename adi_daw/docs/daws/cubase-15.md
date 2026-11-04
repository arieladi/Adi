# Cubase 15 — research v0.1, 2026-11-04

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
