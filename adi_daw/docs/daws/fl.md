# FL Studio — linked-pattern reference only — research v0.1, 2026-11-04

Baseline: `5c9e1cd8ebbb5dd940815aa733d49c1217c9aa19`. Researcher: Codex on SALON-TV. No FL Studio binary was analyzed or executed. FEATURES §3 and ADR-0160 explicitly reference linked clips and Make Unique; no Session-view redesign is authorized.

Authority: current `docs/FEATURES.md`, `docs/DECISIONS.md`, `docs/LANES.md` and window parity checklists. Dream Reference v0.2 is director-facing context, not a superseding specification (director clarification, 2026-11-04). No product changes or live GUI tests were made.

Evidence keys resolve under private `D:\ADI-Research\2026-11-04-daws\EVIDENCE-INDEX.md`. Proprietary binaries, code extracts, projects and raw results stay there, outside this repository. Manual page numbers below are the printed pages in the local extracted chapters under git-ignored `reference/DOCS/`.

Statuses distinguish static facts, measured behavior, supported hypotheses and unknowns. A manual claim is labelled a supported hypothesis about the installed build. No row in this milestone is confirmed by DAW measurement. A string, import, symbol or pseudocode result does not prove a feature executes, its GUI behavior, its DSP precision or its CPU cost.

## Pattern instances and Make Unique

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| Playlist copies share underlying pattern/channel data. Make Unique separates an instance; for audio it reuses the source file, while Make Unique as Sample also creates a separate file. | [Image-Line Playlist manual, Clone Clips / Clip Menus](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/playlist.htm), accessed 2026-11-04, unpaginated | ADR-0160 already defines clip.makeUnique versus clip.consolidate; retain that distinction | Supported hypothesis — documented, not measured on installed build |
