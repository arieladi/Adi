# Reference integration contracts and contradictions — v0.3, 2026-11-04

Read with [whole-feature coverage](DAW-RESEARCH-COVERAGE.md) and the [per-app notes](daws/ableton-12.md). These are research findings and proposed tests, not ADR changes. E24 is the private, hash-bound primary-source manifest at `D:\ADI-Research\2026-11-04-daws\evidence\wide\primary-source-manifest.json`. Public SDK sources were pinned to commits; installed-host use of these interfaces is not established by reading their declarations.

## Plug-in editing, expression and scheduling boundaries

| What the reference specifies | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| VST3 brackets parameter edits with beginEdit/performEdit/endEdit on the UI thread. Group editing synchronizes timestamps across related parameters. Dirty state can cover changes outside parameters. | [VST3 IComponentHandler / IComponentHandler2](https://github.com/steinbergmedia/vst3_pluginterfaces/blob/4f547e8e102b47de4a8b8aaf343c73b700786372/vst/ivsteditcontroller.h) | ADR-0110: collect gestures and opaque state changes without calling the UI interface from audio processing. Group timestamps alone do not define ADI's undo policy; test grouped controls, missing endEdit and editor close. | Confirmed by source inspection — protocol only |
| CLAP separates parameter value from modulation. Note addressing uses port/channel/key/note ID, including wildcard forms; per-voice modulation has explicit precedence over global modulation. Gesture events are available but not universally guaranteed by the event contract. | [CLAP events.h](https://github.com/free-audio/clap/blob/a47f6badb49d948fd009998f28309cdab78979c9/include/clap/events.h) | ADR-0054/0099/0110: preserve note identity and parameter base/effective distinction. Test overlapping same-key notes and incomplete gesture sequences. Do not translate all modulation to destructive parameter changes. | Confirmed by source inspection — protocol only |
| CLAP parameter exchange uses process or flush; they must not run concurrently. Flush loses sample-offset information. Bypass parameter value alone must not decide whether process is called. | [CLAP params.h](https://github.com/free-audio/clap/blob/a47f6badb49d948fd009998f28309cdab78979c9/include/clap/ext/params.h) | Host lifecycle tests must cover stopped/active transitions, parameter edits during restart, and bypass tails. A reference DAW's visible bypass control does not reveal its scheduling policy. | Confirmed by source inspection — protocol only |
| ARA extends ordinary plug-in APIs with arbitrary audio access and bidirectional musical-content information, including tempo, key and scale. | [Celemony ARA SDK overview](https://github.com/Celemony/ARA_SDK/blob/a2b1aac1d1d5c4eed387db85a9c0cdb7d460254c/README.md) | FEATURES §3: require a document/source/region lifecycle, analysis invalidation and state restore beyond the insert-effect model. Cubase's event/track attachment rules are documented separately in C3-006–007. | Confirmed by source inspection — overview; detailed API lifecycle still open |

## Project interchange and backup

| What the reference specifies | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| DAWproject is an interchange format carrying audio, notes/expression, automation, structure and plug-in state. Native project storage, optimal runtime performance and application preferences are non-goals. | [DAWproject README](https://github.com/bitwig/dawproject/blob/ee4dcdde75940f30e14e55401a26955a58b8322b/README.md); E24 also pins Project.xsd | Build a loss matrix for native ADI data versus interchange: branching history, remarks, collaboration clocks and app state must not be silently treated as round-trippable. Export support does not certify another app's importer. | Confirmed by source inspection — format scope; no cross-app import/export measurement |
| SQLite's online backup API can copy incrementally while allowing other clients to continue writing; VACUUM INTO is another documented live-backup method. | [SQLite Online Backup API](https://sqlite.org/backup.html), retrieved 2026-11-04 and hashed in E24 | ADR-0182 already chooses a consistent snapshot. Extend acceptance to matching media/state dependencies, cancellation, disk-full and restore into a fresh process. A consistent database alone is not a dependency-complete project archive. | Documented API capability; ADI backup behavior not tested here |

## Musical synchronization and upstream changes

| What the reference specifies | Where it says so | What ADI does with it | Status |
|---|---|---|---|
| Link's public repository describes beat/tempo/phase synchronization and host-time mapping distinct from the audio device's sample clock. | [Link README](https://github.com/Ableton/link/blob/5ba907ae027a8c2ec8824463e37296dcefc5e9be/README.md) | ADR-0185: test join/leave, tempo ownership, clock drift and start/stop independently of audio streaming and project synchronization. | Confirmed by source inspection — no runtime synchronization measurement |
| The inspected upstream revision includes LinkAudio.hpp. Its comments describe demand-driven sinks/sources, interleaved signed 16-bit samples, and APIs with explicit realtime-safety limits. | [LinkAudio.hpp](https://github.com/Ableton/link/blob/5ba907ae027a8c2ec8824463e37296dcefc5e9be/include/ableton/LinkAudio.hpp) | Flag the older ADR-0204/FEATURES technical availability premise for win's review. Header availability is not a completed integration/build audit. Keep ADI's existing decision until reviewed; do not equate this transport with full-precision remote plug-in hosting. | Confirmed by source inspection — upstream header; feasibility, platform coverage and implementation completeness not established |

## Reference attribution and decision conflicts

| Finding | Evidence | ADI consequence | Status |
|---|---|---|---|
| Dream illustrates tuning as clip-local, while the Live manual loads/saves it at Set level. | Dream §5; Live §15 pp.346–350; A3-024–025 | Preserve ADI per-key-change tunings under ADR-0178, but label them an addition rather than inferred Live behavior. | Documentary discrepancy; installed Live behavior still unmeasured |
| Saved undo and alternate undo/redo paths already appear in REAPER's guide. | REAPER v7.80 §2.28 pp.53–54; R3-001 | Avoid broad claims that no reference has persistent/branching history. ADI typed attribution, snapshots and collaboration remain distinct requirements. | Documentary precedent; no REAPER execution |
| FEATURES §4 includes a Score Editor row, while §11 explicitly excludes a notation-first workflow. | Current FEATURES.md at baseline 5c9e1cd8; Dream asks for note editing/tuning, not a notation-first editor | Track engraving-data preservation and flag the conflicting feature row for win. The earlier missing Score Editor source is not a blocker for current host research. No new notation-editor commitment. | Repository inconsistency recorded; no source document changed |
| Follow Action timing has a more specific quantization exception than the general launch paragraph. | Live §16.4 p.358 versus §16.7 p.362; A3-004 | Test None/Global/explicit clip quantization before implementing precedence. | Documentary ambiguity; measurement required |
| A generator's replacement scope is described differently in two Live manual passages. | Prior A-T rows, Live pp.288/290 | Keep selection/empty-space/Auto Apply tests; do not close it based on one paragraph. | Unresolved from milestone 2 |
| The same nominal transfer can preserve different data depending on library save, within-project copy or cross-project copy. | Bitwig v5.2 pp.391–393; B3-006 | ADI must explicitly define devices, envelopes, expression, media and identity in its cross-project operation. Verify current Bitwig 6.1 before treating old-manual loss behavior as current. | Documentary difference; installed behavior unmeasured |
| Control Room presets own certain inserts outside the project; project and workstation state are not interchangeable. | Cubase Pro 15.0.30 p.530; C3-026 | Portability tests must distinguish monitor correction from musical processing; export should use its declared graph. | Documentary reference; exact installed persistence unmeasured |

## What needs independent ADI specifications

The Dream's collaboration conflict handling, agent authority, per-frame video telemetry, remote hosting failover, always-saved operation model, mobile project continuity and ADiJ require their own specifications and tests. No evidence here establishes equivalent features in Live, Cubase or Bitwig. The [coverage register](DAW-RESEARCH-COVERAGE.md) retains each requirement instead of forcing a misleading reference-app analogy.

For every domain, record: stable object identity and ownership; source versus derived data; time/value units; trigger and state transitions; one-gesture transaction boundary; save/reopen and missing dependencies; cancellation/failure; and real-time/export differences where applicable. These are acceptance dimensions, not a substitute for actual reference evidence.

## Live base research expansion — milestone 6

The [Live v0.6 notes](daws/ableton-12.md) add 445 detailed contracts across editing, routing, recording, controllers, accessibility, files, public API and tuning interchange. A6M/A6C/A6H/A6R cover host workflows; A6S/A6T are bounded installed-resource observations; A6D is versioned release evidence; A6P is public API research; A6F is format/interoperability research; A6G fills additional resource, clock and numerical-test boundaries. Each includes an ADI implication and a private follow-up case.

The new source review preserves ADI per-key-map tuning and flags source-valid scales that conflict with ADR-0178's current model. It separates typed time units, ephemeral API handles, deferred changes, UI versus API commands and data lost during interchange. A6Z adds bounded shipped-schema evidence; A6X/A6U/A6I address the extension/current-integration documentary gaps. These findings refine questions under the existing decisions; they authorize no model change.

The 610-entry Live contents inventory retains stable IDs and now links 373 entries to exact M6 findings. Four Simpler-operation entries are explicitly excluded under the director's instruction. Links are navigation, not completion certification; all original Live measurement questions and full settings/text/menu/palette/history/audio/CPU verification remain open.

## Live device expansion — milestone 7

358 additional research contracts across 62 named devices, each with source, ADI implication, evidence status and a paired unexecuted case. [Live v0.7](daws/ableton-12.md) now includes all 42 native audio effects, four Max audio effects, eight native MIDI effects, six Max MIDI effects, and the explicitly named Sampler/Simpler exceptions. Other built-in instruments remain excluded. Historical exclusions describe earlier passes and are superseded for this scope.

A7A/A7B/A7M describe device behavior and edge cases; A7R adds bounded preset/schema and Push controls; A7N records selected REA/Ghidra observations. The private DEVICE-RESEARCH-MISSIONS-7.md contains matching cases and nine device censuses. Thirteen original fixtures supply precise audio/event inputs, with all runtime results NOT RUN. Earlier host gaps and all L01–L10 questions stay open.

Current FEATURES/decisions/parity remain authoritative. ADR-0169 defines the existing device priorities; ADR-0187 candidates remain candidates. ADR-0203 d7 governs randomness, and ADR-0207 d2 already sets ADI precision. These findings add no priority, DSP implementation or new ADR. The manual and bounded installed-file evidence do not prove factory defaults, exact DSP curves, numeric latency, audio equivalence or CPU advantage.
