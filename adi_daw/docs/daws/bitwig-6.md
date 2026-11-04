# Bitwig Studio 6 — research v0.1, 2026-11-04

Baseline: `5c9e1cd8ebbb5dd940815aa733d49c1217c9aa19`. Researcher: Codex on SALON-TV. Installed launcher/host file version 6.1.0.22652; build-info.sh says 6.1, revision 162652, commit 94a90411037fa337883222813b7372a3ace9dbd7. Activated edition/features were not established. Local manual explicitly says v5.2; the public latest welcome page retrieved on 2026-11-04 says v5.3. Neither is treated as a measured 6.1 specification.

Authority: current `docs/FEATURES.md`, `docs/DECISIONS.md`, `docs/LANES.md` and window parity checklists. Dream Reference v0.2 is director-facing context, not a superseding specification (director clarification, 2026-11-04). No product changes or live GUI tests were made.

Evidence keys resolve under private `D:\ADI-Research\2026-11-04-daws\EVIDENCE-INDEX.md`. Proprietary binaries, code extracts, projects and raw results stay there, outside this repository. Manual page numbers below are the printed pages in the local extracted chapters under git-ignored `reference/DOCS/`.

Statuses distinguish static facts, measured behavior, supported hypotheses and unknowns. A manual claim is labelled a supported hypothesis about the installed build. No row in this milestone is confirmed by DAW measurement. A string, import, symbol or pseudocode result does not prove a feature executes, its GUI behavior, its DSP precision or its CPU cost.

## View selector, filter, groups and heights

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| The older manual names ARRANGE, MIX and EDIT as application views, and normal/half-size track height. These are not evidence that project/group/detail focus is the same three-state control described in ADI. | Bitwig v5.2 manual §2.2.2 p.37; §3.1.4 pp.57–58 | ADR-0112; parity-7.3 rows 31–32; M-BITWIG-VIEW distinguishes application view, group focus and zoom | Supported hypothesis — documented, not measured on installed build |
| Exactly which tracks survive a filter, nested groups, collapsed groups, FX/master/deactivated tracks and selection; per-track resize after half-height toggle remain unmeasured. | M-BITWIG-VIEW; installed 6.1 required | One consistent TrackOrder for timeline/mixer; N16; do not promote a Dream sketch to native Bitwig fact | Unknown / not tested |

## Displays, project panel and popup browser

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| The older manual documents display profiles and compound two-window view labels. Arbitrary detachable panels and their restore/reopen behavior have not been established for installed 6.1. | v5.2 §2.1 p.30, §2.2.2 p.37; M-BITWIG-PANELS | ADR-0063; UI-ARCHITECTURE; ADI reparenting is an ADI decision | Supported hypothesis — documented, not measured on installed build |
| Project panel contents, popup-browser insertion/replacement scope, cancel versus confirm and persistence need live capture. | v5.2 §4 pp.67–108, §14 pp.372–396; M-BITWIG-PANELS | Browser N15; project/group filter; snapshot/view state | Unknown / not tested |

## Modulation sources, targets, gesture and saved state

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Older documentation allows device/track/group/project-level modulation; track level reaches its mixer/devices, group level reaches child tracks. This does not imply unrestricted cross-track target access from every modulator location. | v5.2 §16.2.2 pp.437–438; mapping controls pp.430–431 | ADR-0046, ADR-0114; FEATURES §6–7; M-BITWIG-MOD | Supported hypothesis — documented, not measured on installed build |
| Actual source catalog, target restrictions, bipolar mapping amount, scaling, polyphony, duplicate/copy and save/reopen persistence need controlled 6.1 projects. | M-BITWIG-MOD | Store base value separately from modulation; verify serialization and reset history | Unknown / not tested |

## Note expression, MPE and operators

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Pressure/timbre/micro-pitch and other per-note expressions are documented; actual ranges, units and MPE round-trip need measurement. | v5.2 §11.1.2 pp.307–313; M-BITWIG-NOTE | FEATURES §4; ADR-0054, ADR-0097–0100; independent curves, not channel tricks | Supported hypothesis — documented, not measured on installed build |
| Chance, repeats and conditional occurrence are documented; interaction order, reset on transport/reopen and random-seed persistence are untested. | v5.2 §12 pp.344–358; M-BITWIG-NOTE | Separate event probability/repetition/condition fields; reserve decisions for win | Supported hypothesis — documented, not measured on installed build |

## Audio events and comping

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| The older manual distinguishes an outer clip from its contained audio events; per-event fades and stretch exist. Gain/pitch editing, comp-lane promotion, crossfades and undo need installed-build tests. | v5.2 §10 pp.243–296, particularly p.245; M-BITWIG-AUDIO | FEATURES §3; keep clip placement separate from source/event editing | Supported hypothesis — documented, not measured on installed build |

## Native engine, host and FFT starting points

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Engine, UI app and x64/x86 plug-in hosts are separate shipped executables; the x64 host was imported by REA/Ghidra with 6977 recovered procedures and 2599 strings. Counts are analysis output, not understood functions. | E01, E08; BitwigPluginHost-X64-AVX2.exe | Investigate process isolation/lifecycle separately from DSP graph; actual hosting policy unmeasured | Confirmed by static analysis |
| Engine exports FFT forward/backward/window functions for 128 through 8192 powers of two. The 128 forward/backward wrappers share target 0x140871100 and context global 0x1462da5b0, differing in a direction argument. | E09; VA 0x140191e80 and 0x140191ec0; Ghidra 12.2 assembly and pseudocode | Candidate reusable FFT-plan boundary for CPU study; do not infer all FFT sizes or all device algorithms | Confirmed by static analysis |
| The 128 window-fill wrapper obtains a pointer and requests a 0x200-byte copy. Cached-window selection is a hypothesis; sample type, coefficients, callers and hot-path use are not proven. | E09; VA 0x140192520, helper 0x1404b49c0, tail target 0x1433e21d0 | Investigate precomputation/cache invalidation; do not treat byte count alone as float32 proof | Supported hypothesis |
| Whole-engine arithmetic, scheduling, compensation, offline parity and CPU cost remain unmeasured. | M-AUDIO; M-CPU; private RESEARCH-MATRIX.md | Evidence is insufficient for an efficiency ranking | Unknown / not tested |
