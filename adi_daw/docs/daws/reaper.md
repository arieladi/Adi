# REAPER — referenced scope only — research v0.3, 2026-11-04

Baseline: `5c9e1cd8ebbb5dd940815aa733d49c1217c9aa19`. Researcher: Codex on SALON-TV. No REAPER binary was analyzed or executed. Inclusion is justified by FEATURES §3 and §8 and ADR-0205 d9; version-specific behavior remains to verify.

Authority: current `docs/FEATURES.md`, `docs/DECISIONS.md`, `docs/LANES.md` and window parity checklists. Dream Reference v0.2 is director-facing context, not a superseding specification (director clarification, 2026-11-04). No product changes or live GUI tests were made.

Evidence keys resolve under private `D:\ADI-Research\2026-11-04-daws\EVIDENCE-INDEX.md`. Proprietary binaries, code extracts, projects and raw results stay there, outside this repository. Manual page numbers below are the printed pages in the local extracted chapters under git-ignored `reference/DOCS/`.

Statuses distinguish static facts, measured behavior, supported hypotheses and unknowns. A manual claim is labelled a supported hypothesis about the installed build. No row in this milestone is confirmed by DAW measurement. A string, import, symbol or pseudocode result does not prove a feature executes, its GUI behavior, its DSP precision or its CPU cost.

## Region render matrix and naming

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| The guide shows source/region selections producing files, unchecked regions omitted, and a Master Mix choice. Normal versus via-master routing and shared-return consequences need runtime tests. | REAPER User Guide v7.80 §21.10 pp.417–418; M-REAPER | ADR-0071; ADR-0205 d9; FEATURES §8; selected cells become queue jobs; grid UI stays P2 | Supported hypothesis — documented, not measured on installed build |
| Wildcards include $project, $track and $region; path separators create subdirectories. Exact escaping, duplicate/empty names and collisions remain untested. | v7.80 §21.1.1 p.409 and §21.10 p.418 | ADR-0205 d9 naming templates; validate generated paths and preview resolved job names | Supported hypothesis — documented, not measured on installed build |
| Queued renders capture project state at enqueue time; later changes require removing/re-adding the queued item according to the guide. | v7.80 §21.9 p.417 | ADR-0071: explicitly bind export jobs to a consistent source snapshot; test update semantics | Supported hypothesis — documented, not measured on installed build |

## Ripple rules

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| The guide distinguishes off, per-track and all-track ripple; locked-item handling and tempo-map/envelope/edge behavior depend on preferences. Exact boundary and undo behavior remains untested. | v7.80 §7.20 pp.139–140; M-REAPER | FEATURES §3: explicit affected-track scope; do not equate ripple mode with every range command | Supported hypothesis — documented, not measured on installed build |



## Whole-Dream research expansion — 2026-11-04

Coverage is driven by [the whole-feature register](../DAW-RESEARCH-COVERAGE.md), not just the initial examples. Manual claims remain supported hypotheses about the installed build; all live behavior, undo and performance checks remain pending. REAPER is included only where Dream/current ADI explicitly references it. E25/E26 identify new static artifacts. See whole-host test missions in the private research root.

### Dream-referenced history, project tabs and monitoring

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| R3-001: Undo history can be saved with the project and retain alternative undo/redo paths. Preferences control whether selections and cursor positions enter history. | User Guide v7.80 §2.28 pp.53–54 | Dream §12: persistent/branching history has a reference precedent; ADI’s operation attribution/collaboration remains distinct. Correct any “neither DAW has this” generalization. | Supported hypothesis — documented; installed behavior unmeasured |
| R3-002: Project tabs support track/item transfer; background playback, synchronized starts, offline media and hiding background FX windows are separate options. | v7.80 §2.29 pp.54–55 | ADI historical tabs must remain silent/read-only regardless of ordinary project-tab playback; define cross-project ID remapping and destination undo. | Supported hypothesis — documented; installed behavior unmeasured |
| R3-003: Monitoring FX do not enter renders, do not affect the metronome and are not saved in the project file. | v7.80 §6.13 p.118 | Dream §7/8: separate workstation monitor processing from project/master export processing and reference audition. | Supported hypothesis — documented; installed behavior unmeasured |
| R3-004: Ripple all has separate tempo-map and edge-point options, with locked-item behavior governed by preferences. Trim-to-selection also removes time when ripple is active. | v7.80 §7.20–7.21 pp.139–140 | Dream §3: test ripple mode × command × lock × automation/tempo scope. Avoid assuming all range tools share one affected-object rule. | Supported hypothesis — documented; installed behavior unmeasured |
| R3-005: Parameter modulation has baseline, audio control, LFO and linked-parameter sources. Existing automation replaces the manual baseline; audio control selects track channels. | v7.80 §19.1–19.3 pp.385–386 | Dream §10: base automation and modulation composition need an explicit rule; compare with Bitwig transfer/scaling without assuming identical domains. | Supported hypothesis — documented; installed behavior unmeasured |
