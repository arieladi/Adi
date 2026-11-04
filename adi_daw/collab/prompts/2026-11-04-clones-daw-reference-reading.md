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
