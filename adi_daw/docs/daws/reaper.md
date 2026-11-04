# REAPER — referenced scope only — research v0.1, 2026-11-04

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
