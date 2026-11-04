# Ableton Live 12 — research v0.1, 2026-11-04

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
