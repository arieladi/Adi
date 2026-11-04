# Clones lane — DAW reference reading, 2026-11-04

Director-authorized research milestone only. Do not build or modify a DAW, clone, engine, UI, schema or ADR.

Read first: current `docs/FEATURES.md`; `docs/DECISIONS.md` (especially ADR-0108, 0110–0115, 0160, 0203–0211); `docs/LANES.md`; all `collab/window/parity-*.md`; `collab/window.md` needs and Measure on Live lines; `docs/AWAITING.md`. Read the director's `8_Adi-Daw Dream Reference claude draft v0.2 preview.pdf` as context. Current repository decisions govern; the director clarified that Dream is chiefly for him. Do not replace missing ORC-VST ignored material with older SALON-TV drafts silently.

Use a separate `clones/*` worktree. This explicit assignment extends reference reading (LANES §4 item 5) to Ableton Live 12, Bitwig 6 and Cubase 15. Save one versioned file per app under `docs/daws/`; REAPER and FL only for features ADI explicitly references. Every topic table has exactly: what the app does; evidence/source; ADI decision/need/item; status. Manuals remain in git-ignored reference/. Do not put proprietary code, binaries, decompilation or raw private projects into git.

Inspect installed executable/module versions and SHA-256 first. No DAW updates or binary changes. Use REA/Ghidra read-only; save projects, annotations and raw results separately under the private research root. Label static evidence, measurements, hypotheses and unknowns. Functions/counts are not proof of whole-program understanding; controller colours are not measured UI colours; user settings are not factory defaults; manual versions are not interchangeable.

Priority: Live window questions and automation/undo; Bitwig filter/view/group distinctions; Cubase offline/fade/range behavior; then core audio and efficiency contracts. Existing ADI deviations remain, including Warp off and Create Fades off on import. Source reading must answer a linked ADI question, not retell a manual.

No desktop capture, mouse/keyboard automation or GUI-host testing by Codex in this mission. Prepare precise Claude missions: exact installed build, clean starting state, fixtures/routing/settings, actions, screenshots/audio/event logs, acceptance criteria and final app state. Continue independent static work. A measured fact that answers a window question is relayed by win into both the original Measure on Live line(s) and the relevant awaiting-measurements row; preserve duplicate Arm-glyph references. Do not close those rows with resource or manual evidence.

Maintain private INVENTORY.md, RESEARCH-MATRIX.md, ENGINE-CONTRACTS.md, SOUND-RESULTS.md, CPU-RESULTS.md, OPEN-QUESTIONS.md, EVIDENCE-INDEX.md and HANDOFF.md. Use deterministic probes, explicit latency/alignment and state sequences. CPU studies stay on SALON-TV with fixed equivalent workloads, warm-up, interleaved A/B and B/A, median/tails/deadline misses and editor/metering separated.

Finish with a useful, bounded milestone: independent behavior contracts now supportable, remaining unknowns, actual sound/CPU measurements (possibly none), Claude tests needed and evidence-supported next implementation. Wait for explicit product-build instruction.


## Director expansion — full host research

The director requests deep study of all host features beyond the initial lane questions, prioritizing Cubase F7 Direct Offline Processing and advanced fades/arrangement audio utilities, Live warp and MIDI functions, and Bitwig modular/modulation behavior. Skip built-in effect/instrument implementations. Keep editor transformations, DOP audio utilities, Grid/host modulation semantics, plug-in hosting, routing and integration in scope.

Work in bounded, evidence-backed passes without claiming the entire app is understood. Maintain a coverage inventory with gaps, exact versions and unmeasured behaviors. Connect every recorded contract to ADI's existing decisions/needs. Include ownership, state transitions, persistence, failure/cancel and undo boundaries, not just control descriptions. The v0.2 app files add 127 contract rows and host-wide queues; private DEEP-CLAUDE-MISSIONS.md supplies nine reproducible missions. The no-GUI-testing instruction above continues to apply. No processor algorithm or product implementation is requested.



## Director clarification — the examples are not the scope

Research every intended feature in all 15 Dream Reference sections, cross-checking the whole current FEATURES list and decisions. Treat offline processing, warp/MIDI and modulation as examples, not a stopping boundary. The whole-feature register now decomposes 220 requirements and an independent index retains 319 FEATURES table data rows (including metadata). These are completeness guards, not completed measurements.

Read and maintain docs/DAW-RESEARCH-COVERAGE.md, DAW-RESEARCH-FEATURE-INDEX.md and DAW-RESEARCH-INTEGRATION.md alongside the five per-app notes. Include controllers, workspace/accessibility, files/history, routing/monitoring, host lifecycle, export/interchange, AI/collaboration, telemetry, mobile and ADiJ. Where three reference DAWs have no equivalent, state that an independent ADI contract/primary-source study is needed rather than inventing parity. Preserve explicit exclusions and deferred work. Built-in reference device implementations stay excluded; host integration remains included.

Each research claim needs ownership, trigger/state transition, undo, persistence, failure and relevant numerical/timing evidence. Do not close an entire feature because its description is known. Continue independent static work while separate Claude missions cover live behavior. No GUI automation, product implementation or silent ADR change is authorized by this research expansion.

## Base-reference clarification — export, current Bitwig UI and all Live host behavior

The director explicitly names Cubase export capabilities and Bitwig selected-track auto-zoom, and requires complete Ableton host coverage as the base reference. Study all Live host workflows and controls, including material absent from the Dream examples. Preserve the exclusion of built-in instrument/effect implementations. A-BASE in docs/daws/ableton-12.md inventories all 610 host/integration contents entries from the local manual (including parent sections/examples), with unmeasured completion dimensions and additional undocumented-control work. This is not a completion percentage.

The v0.4 notes add 49 contracts: Cubase 19, Bitwig 6 and Live 24. B4 corrects the reliance on the older manual's half-height control using official version-6 Auto-zoom documentation. C4 separates Quick Export, stems, ranges, processing taps, queues, conflicts, formats/metadata, post-export actions and interchange/video/ADM. A4 distinguishes Live's export/consolidate/bounce operations and expands editing, recording, envelopes, history and accessibility. BASE-REFERENCE-MISSIONS.md in the private research root supplies the follow-up cases. No measured facts have been fabricated and no original Live question has been closed.

## Deeper-run directive — VariAudio, multi-screen, customization, Live priority

The director explicitly adds Cubase VariAudio, detachable/multi-screen elements, extensive keyboard shortcut customization and preferences, with Ableton still the most important reference. Deepen behavior and state ownership, not just topic inventories. 172 new rows: Live 107, Cubase 65. Each retains behavior, source, ADI mapping and evidence status, with its own measurement case. No new DAW GUI/audio/CPU measurement or implementation.

Read the v0.5 app additions A5C/A5H/A5W and C5V/C5W. Keep key ambiguities open: VariAudio flatten scope, workspace/monitor recovery, macro transaction boundaries, Live shortcut contradictions and every original Live measurement. Distinguish project/window/profile/device state and visible Undo History from crash-recovery artifacts. Built-in processor implementations remain excluded; editor processing and host integration remain in scope. No GUI automation or product work is authorized by this research.

## Milestone 6 directive — continue through the Ableton host

The director requests deeper coverage across everything still uncovered in Ableton, the base reference, while continuing to skip built-in instruments and effects. Read FEATURES, DECISIONS, parity checklists and Dream context before choosing findings. 445 additional Live research contracts, each with source, ADI implication, status and a paired unexecuted measurement case. 373 contents entries have exact M6 contract links; these are navigation links, not completion certification.

Use A6M/A6C/A6H/A6R/A6S/A6T/A6D/A6P/A6F/A6G/A6Z/A6X/A6U/A6I in ableton-12.md and private LIVE-BASE-MISSIONS-6.md. Preserve source/build boundaries, scope distinctions and documented contradictions. Complete settings defaults, exact text/menu/palette censuses, gesture undo, audio/CPU behavior and unmeasured version differences remain open. No product implementation, GUI automation or new ADR is part of this work. All ten original Live questions retain their IDs and require measured evidence before win relays an answer.

## Milestone 7 directive — include effects and the two named samplers

The director explicitly expands research to Ableton MIDI devices and audio effects, naming Redux, Shifter, Drum Buss, Gate, Sampler and Simpler. Include Sampler/Simpler as named exceptions; continue excluding other built-in instruments. Read FEATURES, DECISIONS, parity checklists and Dream context first. 358 additional research contracts across 62 named devices, each with source, ADI implication, evidence status and a paired unexecuted case.

Use A7A/A7B/A7M/A7R/A7N, the 62-device register and private DEVICE-RESEARCH-MISSIONS-7.md. Distinguish documented control behavior, preset/schema observations, selected code paths and measured behavior. The local manual revision is unconfirmed, and historical/current web sources must retain their version limits. No desktop capture, GUI automation, DAW-host testing, product implementation or app update is authorized in this research session. Prepare Claude cases; preserve all ten original window questions until measured evidence arrives. Keep remaining host and control-level gaps explicit.

## Milestone 8 directive — Drum Rack, Packs and legacy variants

Read FEATURES, DECISIONS, parity checklists and Dream context first. Include Drum Rack container behavior, Pack effects and legacy variants; retain the Sampler/Simpler exceptions and skip other built-in instruments. 66 contracts: {'D': 25, 'P': 28, 'L': 12, 'I': 1}. D=Drum Rack; P=Pack effects; L=legacy; I=installation boundary. Each has an ADI implication and an unexecuted M8 case. Resolve dependencies and source versions before interpreting tests; public Pack pages are incomplete behavioral specifications. No GUI testing or product implementation in this research session.

## Milestone 9 directive — Bitwig MIDI and The Grid

Read FEATURES, DECISIONS, parity and Dream context first; inspect actual implementation and lane branches before making feasibility claims. Research MIDI/Note FX, non-instrument devices, modulators, containers and Grid semantics. Skip other built-in instruments. 173 additional Bitwig contracts: grid 59, midi-devices 71, audio-devices 43. Each has source, ADI implication, status and a paired unexecuted test. Evaluate native host services, Pd authoring and registered native kernels together, with explicit event/voice/rate/feedback/latency/edit/identity boundaries. No product implementation, new ADR, GUI testing or app updates. Preserve source versions and identify unavailable measurements.
