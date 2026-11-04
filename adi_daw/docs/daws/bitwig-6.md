# Bitwig Studio 6 — research v0.2, 2026-11-04

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

## Expanded host research — 2026-11-04

Scope expanded by the director: all host functions are in the research inventory; built-in instrument and effect implementations are excluded. Editing utilities, MIDI transformations, modulation, routing, hosting and modular-system semantics remain in scope. Rows below are behavior contracts and ADI design implications, not new ADRs or permission to implement. An unknown is not a negative finding. Private evidence: E17–E20; precise validation missions: DEEP-CLAUDE-MISSIONS.md.

### Version 6 overlay: use release notes over stale UI instructions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| B-V01: Version 6 adds aliases sharing pattern content/name/colour on one track, while placement and loop properties stay individual. | [Bitwig 6.0 release notes — Alias Clips](https://downloads.bitwig.com/6.0/Release-Notes-6.0.html); E19 | ADR-0160: compare alias identity and make-unique with ADI’s own definition; do not assume cross-track linking from this source. | Supported hypothesis — documented; installed behavior unmeasured |
| B-V02: Automation clips coexist with track automation. Free Time separates inherited clip timing; wrapping and flattening convert representations. | [6.0 release notes — Automation Clips](https://downloads.bitwig.com/6.0/Release-Notes-6.0.html); E19 | N26 / FEATURES §7: investigate representation changes without losing curve timing; do not adopt this larger schema implicitly. | Supported hypothesis — documented; installed behavior unmeasured |
| B-V03: Lane follow policies are editor setting, always follow, never follow or Arranger lock. Points add Hold and seeded Spread. | [6.0 release notes — Automation Editing](https://downloads.bitwig.com/6.0/Release-Notes-6.0.html); E19 | ADI: selection-follow, interpolation and randomness are independent axes; test all separately from automation override. | Supported hypothesis — documented; installed behavior unmeasured |
| B-V04: Most modulated parameters clamp, but designated phase-like parameters wrap. Version 6 also changes layered editing and group presentation. | [6.0 release notes — Wrapped Parameter Modulation / Layered Editing / Group Track superpowers](https://downloads.bitwig.com/6.0/Release-Notes-6.0.html); E19 | ADR-0046: target metadata must specify domain behavior; old display instructions are not reliable 6.1 parity evidence. | Supported hypothesis — documented; installed behavior unmeasured |
| B-V05: The 6.1 release notes explicitly say the user guide is being overhauled and direct readers to 6.0 documentation plus the 5.3 manual for general topics. | [Bitwig 6.1 release notes](https://downloads.bitwig.com/6.1/Release-Notes-6.1.html); E19 | The local 5.2 guide remains a historical source. Version-sensitive screenshots, gestures and defaults stay unknown until measured. | Supported hypothesis — documented; installed behavior unmeasured |

### Unified modulation: source, route and target semantics

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| B-M01: Modulation preserves the target’s editable base value and displays its current modulated value. Sources are added as needed; empty slots expand rather than imposing a fixed LFO count. | Bitwig local v5.2 manual, §16.2 pp.423–425; validate against installed 6.1 | ADR-0046 / ADR-0181: separate stored value, effective value and observer feed; derived samples must never enter the operation log. | Supported hypothesis — documented; installed behavior unmeasured |
| B-M02: Source families cover manual macros/buttons, envelopes/sequences/LFOs, note/controller expression, external audio/CV, mixing/math and randomness. | Bitwig local v5.2 manual, p.424; validate against installed 6.1 | ADI modulation inventory: study source contracts and routing only; individual synthesis/effect algorithms are excluded. CV remains deferred under ADR-0188. | Supported hypothesis — documented; installed behavior unmeasured |
| B-M03: Enable a source’s routing mode, then drag target controls to assign signed relative depths. Several targets can be assigned before leaving mapping mode; displayed depth may exceed the target’s range. | Bitwig local v5.2 manual, pp.427–428; validate against installed 6.1 | ADR-0046 / ADR-0114: mapping is a transaction editing route depth; target clipping/wrapping occurs in evaluation, not by corrupting the stored base. | Supported hypothesis — documented; installed behavior unmeasured |
| B-M04: Modulator parameters can themselves be targets. Routes can be deleted from either source or destination views, individually or together. | Bitwig local v5.2 manual, pp.430–432; validate against installed 6.1 | ADI: stable source/target IDs and referential integrity are necessary; dependency cycles and deletion cleanup need explicit policy and tests. | Supported hypothesis — documented; installed behavior unmeasured |
| B-M05: Track sources reach that track’s devices/mixer; group sources reach descendants; project sources reach all tracks and automatable transport controls. | Bitwig local v5.2 manual, §16.2.2 pp.437–438; validate against installed 6.1 | ADR-0046 / cross-track modulation: define scope before mapping; reparent, duplicate, delete and undo must resolve routes consistently. | Supported hypothesis — documented; installed behavior unmeasured |
| B-M06: Monophonic sources share a value; polyphonic sources can differ per note. Per-Voice can switch capable modulators; CLAP support depends on the plug-in. Nested destinations may receive a mono combination instead. | Bitwig local v5.2 manual, pp.439–440; validate against installed 6.1 | ADR-0052 / ADR-0054: a VST3 parameter route is not automatically equivalent to per-note CLAP modulation. Test voice identity and reduction rules. | Supported hypothesis — documented; installed behavior unmeasured |
| B-M07: Routes support bypass without deletion, individual/all-route transfer and copying to other sources. | Bitwig local v5.2 manual, §16.2.4.3 pp.446–447; validate against installed 6.1 | ADI route operations: preserve route identity, enable state and depth through copy/move/undo; verify destination validity when nesting changes. | Supported hypothesis — documented; installed behavior unmeasured |
| B-M08: Transfer curves include linear, positive half, rectified negative half, absolute, toward-zero, exponential and logarithmic. Transfer is applied before another source scales a particular route. | Bitwig local v5.2 manual, pp.448–450; validate against installed 6.1 | ADR-0114: store the transfer function and depth-scaling connection separately. Exact exponential/logarithmic formulas are not established. | Supported hypothesis — documented; installed behavior unmeasured |
| B-M09: Mapping state, project/preset persistence, parameter renumbering and reconnection after a missing plug-in require measurement; a UI connection alone does not prove saved bytes or timing. | Bitwig local v5.2 manual, §16.2; M2-B-MOD; validate against installed 6.1 | ADI regression set: save/reopen, move group, duplicate container, remove target, undo, plug-in absent, plug-in restored. | Unknown / not tested |

### Modular architecture boundaries — Grid is not the whole host

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| B-G01: Nested chains distinguish pre, post, wet-only and feedback positions. Post-chain ownership groups processing with a saved/moved device; wet-only processing bypasses the dry branch. | Bitwig local v5.2 manual, §16.1.3 pp.421–423; validate against installed 6.1 | ADR-0060: containers need explicit graph ports and serializable ownership. Do not implement wet mix as an unqualified serial chain. | Supported hypothesis — documented; installed behavior unmeasured |
| B-G02: Grid has explicit note/control/audio-through policies and implicit pre-cord connections. Patching and external host routing are different layers. | Bitwig local v5.2 manual, §17.2 pp.487–491; validate against installed 6.1 | ADI modular research: implicit connections must be visible in inspection/export; distinguish default wiring from user links without adopting Grid devices. | Supported hypothesis — documented; installed behavior unmeasured |
| B-G03: Direct feedback is rejected; Long Delay permits feedback with at least a block of delay. | Bitwig local v5.2 manual, §17.2.3 pp.492–493; validate against installed 6.1 | ADI graph: mark delayed edges explicitly; block-size changes can change feedback behavior. Do not infer the whole host uses this Grid rule. | Supported hypothesis — documented; installed behavior unmeasured |
| B-G04: Grid logic uses a 0.5 threshold, phase wraps to [0,1), pitch uses 0.1 per octave around C3, and untyped signals coexist. | Bitwig local v5.2 manual, §17.3.1 pp.493–494; validate against installed 6.1 | ADI graph design study: units and conversion policy belong to ports; these numerical conventions are reference facts, not approved ADI formats. | Supported hypothesis — documented; installed behavior unmeasured |
| B-G05: The guide describes Grid signals as stereo at 4× sample rate, while ordinary modulators are mono at the configured sample rate, including Grid-originated modulators. | Bitwig local v5.2 manual, §17.3.2–3 pp.494–496; validate against installed 6.1 | ADR-0046 / CPU research: do not equate GUI refresh, modulator evaluation, plug-in delivery and Grid execution rates. Installed-build rates remain unmeasured. | Supported hypothesis — documented; installed behavior unmeasured |
| B-G06: Voice lifetime considers enabled gate/envelope/audio-output conditions; all relevant conditions must finish before the voice ends. | Bitwig local v5.2 manual, §17.3.4 pp.496–497; validate against installed 6.1 | ADI host/graph study: tails and note-off are distinct lifecycle events; silence suspension must not discard an active modulation or pending tail. | Supported hypothesis — documented; installed behavior unmeasured |
| B-G07: Plug-in suspend policies include never, host-detected silence and trusting plug-in notifications. MPE pitch-bend range is a host configuration. | Bitwig local v5.2 manual, Plug-in Inspector Parameters, p.445; validate against installed 6.1 | ADR-0122 / host lifecycle: test sleeping/waking, automation during silence, latent tails and missing notifications independently of Grid. | Supported hypothesis — documented; installed behavior unmeasured |

### Audio events, expressions and comping

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| B-A01: A clip contains events, restricts audible extent and owns looping; shortening an event need not shorten its parent clip. Source slip is a separate gesture. | Bitwig local v5.2 manual, §10.1 pp.245–247; validate against installed 6.1 | FEATURES §3 / ADR-0115: separate source offset, event extent and clip placement; test restoring a trimmed boundary. | Supported hypothesis — documented; installed behavior unmeasured |
| B-A02: Stretch markers anchor source beats; moving the audio under a marker differs from moving the marker. Onsets remain separate editable analysis data. | Bitwig local v5.2 manual, pp.248–252; validate against installed 6.1 | AWRP / ADR-0176: shared transient analysis is not the user’s warp map. Test three-anchor local edits and onset deletion. | Supported hypothesis — documented; installed behavior unmeasured |
| B-A03: Gain and pan expressions operate at the audio source, before the device chain; track fader/pan operate later. Waveform display reflects source gain. | Bitwig local v5.2 manual, pp.252–254; validate against installed 6.1 | ADR-0115: insert-stage signal probes must distinguish source gain from post-device volume; identical fader labels do not mean equivalent routing. | Supported hypothesis — documented; installed behavior unmeasured |
| B-A04: Pitch/formant expression stays stored when a playback mode cannot use it. Raw ignores stretch; Repitch uses stretch but ignores independent pitch expression. | Bitwig local v5.2 manual, pp.255,285–286; validate against installed 6.1 | ADI: unsupported mode data must survive a mode switch; disable interpretation without destructively clearing the edit. | Supported hypothesis — documented; installed behavior unmeasured |
| B-A05: The old guide documents neutral playback for Stretch at original pitch/tempo. That does not establish neutrality for every algorithm or for installed 6.1. | Bitwig local v5.2 manual, p.285; validate against installed 6.1 | M2-B-AUDIO: compare Raw and Stretch at unity plus transposed/rate-changed controls; report exact equality only from measured exports. | Supported hypothesis — documented; installed behavior unmeasured |
| B-A06: Gain/pitch adjustments move existing expression points; an empty expression gains a starting point. Normalize inserts a gain point instead of rewriting the source file. | Bitwig local v5.2 manual, pp.268,282; validate against installed 6.1 | ADR-0115: scalar-looking commands may actually edit envelopes; test existing curves and repeated normalization. | Supported hypothesis — documented; installed behavior unmeasured |
| B-A07: Comp regions, one take or all takes can be slid; the current composite can become a new take, and an external audio file can be added as a take. | Bitwig local v5.2 manual, §10.1.4 pp.263–266; validate against installed 6.1 | FEATURES §3: preserve source identity, take offset and composite selection separately; test linked multi-track edits and reopen. | Supported hypothesis — documented; installed behavior unmeasured |

### Operators: persistent event rules versus generated events

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| B-O01: Chance is clip-seeded; Repeat can divide an event into N parts or repeat at a beat rate. Changing event length affects those modes differently. | Bitwig local v5.2 manual, §12.1 pp.345–349; validate against installed 6.1 | ADI future operators: store rule mode and units, seed scope and cycle state. Do not convert repeat counts to a fixed note list on every playback. | Supported hypothesis — documented; installed behavior unmeasured |
| B-O02: Note repeats can vary velocity toward an endpoint along a curve; the endpoint need not be hit because velocity is sampled at note starts. | Bitwig local v5.2 manual, pp.350–351; validate against installed 6.1 | FEATURES §4: test three and four repeats, length changes and per-note expression; preserve the source event as editable. | Supported hypothesis — documented; installed behavior unmeasured |
| B-O03: Occurrence includes first/not-first, previous result, previous key/channel for notes, and Fill state. Recurrence uses a per-event cycle length of 1–8 with enabled steps. | Bitwig local v5.2 manual, pp.351–353; validate against installed 6.1 | ADI sequencing: previous-result scope and transport reset are stateful; test equal-time chords, seeks, retriggers and clip restart. | Supported hypothesis — documented; installed behavior unmeasured |
| B-O04: Slice At Repeats makes real events and disables their Repeats, retaining other operators. Expand and Consolidate can resolve some rules, but Repeats and live Fill dependencies can remain. | Bitwig local v5.2 manual, §12.2 pp.353–357; validate against installed 6.1 | ADI commit/bake commands: materializing events is distinct from rendering audio. Preserve unresolved rules and label what remains live. | Supported hypothesis — documented; installed behavior unmeasured |
| B-O05: Consolidate treats fixed seeds differently from Random; Expand offers Preserve Chance/Spread. Matching audible outcomes requires matching state and loop count. | Bitwig local v5.2 manual, pp.354–357; validate against installed 6.1 | ADR-0160 / undo: save concrete result for redo; test determinism from beginning versus midway and after reopening. | Supported hypothesis — documented; installed behavior unmeasured |

### Installed controller API evidence

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| B-S01: Shipped ModulationSource documentation exposes isMapping, isMapped, name and toggleIsMapping. Mapping mode is distinct from having destinations. | E18: hashed installed control-surface API pages and evidence/deep/bitwig-api-ModulationSource.txt | ADR-0181: surface feedback should distinguish edit mode from persistent routing. This documentation is not an engine call trace. | Confirmed by static analysis — scope limited to the named artifact |
| B-S02: Shipped NoteStep documentation exposes separate expression, chance, occurrence, recurrence mask/length and repeat controls. | E18: evidence/deep/bitwig-api-NoteStep.txt; original HTML hash in bitwig-api-manifest.json | ADI surface/API study: semantic fields are addressable independently of the piano roll. API fields alone do not prove complete project serialization or render semantics. | Confirmed by static analysis — scope limited to the named artifact |

### Workspace and browser distinctions

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| B-H01: Arrange/Mix/Edit views, display profiles, group focus, track filters and track-height controls are different mechanisms. The old guide describes a normal/half-height toggle. | Bitwig local v5.2 manual, §2.2 pp.35–37; §3.1.4 pp.57–58; validate against installed 6.1 | ADR-0047 / ADR-0112: do not identify ADI’s three view states merely with Bitwig’s three named layouts; exact group/filter membership remains M-BITWIG-VIEW. | Supported hypothesis — documented; installed behavior unmeasured |
| B-H02: Browser collections store members; smart collections store filters. The panel browses broadly, while the popup is contextual. | Bitwig local v5.2 manual, §4 pp.67–71; validate against installed 6.1 | N15: distinguish saved query, explicit collection, search state and insert destination; canceling a preview must restore the previous graph. | Supported hypothesis — documented; installed behavior unmeasured |
| B-H03: Source kind affects which filters exist; examples include file type, vendor and channel count. Active filters remain visible when collapsed. | Bitwig local v5.2 manual, §4.2.2 pp.83–86; validate against installed 6.1 | ADI browser: preserve incompatible filter state deliberately across context changes; define AND/OR combination by testing, not from visual layout. | Supported hypothesis — documented; installed behavior unmeasured |
| B-H04: Multiple display windows show one selected project. Display profiles constrain available views; this alone does not prove arbitrary panel detachment. | Bitwig local v5.2 manual, §2.1–2.2 pp.30–37; validate against installed 6.1 | FEATURES §9: ADI’s detach/reparent requirement remains its own decision. Test unplugged monitors, scaling, focus and reattachment. | Supported hypothesis — documented; installed behavior unmeasured |


## Host-wide coverage queue

The whole host is in scope; these rows keep unclosed areas visible. They are research questions, not claims about the app or new ADI commitments. A feature named below is not established absent merely because it is not covered above.

| What the app does | Where it says so / evidence | What ADI does with it | Status |
|---|---|---|---|
| To establish: Current v6.1 UI and automation | Source inventory: 6.0/6.1 release notes; older Ch.2,9,11 | Reconcile every changed gesture, lane follow, alias, scale and group behavior with installed build; N26, ADR-0047/0160. | Unknown / not tested — remaining scope |
| To establish: Track filters, groups, workspace, detachment and touch | Source inventory: Ch.1–3,7,18 | Exact membership predicate, nested group focus, hidden/deactivated tracks, height persistence, multiple displays and touch. ADR-0112; original M-BITWIG-VIEW. | Unknown / not tested — remaining scope |
| To establish: Transport, launcher, recording and arrangements | Source inventory: Ch.5–7 | Launch/release/next actions, recording to Arrangement, overdub, monitor state, quantization and follow behavior. FEATURES §1/5/8. | Unknown / not tested — remaining scope |
| To establish: Browser, project panel, library and file recovery | Source inventory: Ch.4,14 | Popup preview commit/cancel, filter boolean logic, collection persistence, project metadata, missing media and collect/save. N15; FEATURES §9. | Unknown / not tested — remaining scope |
| To establish: Expression, layered note editing and tuning | Source inventory: Ch.11; v6 changes | MPE curves through split/join/stretch, simultaneous pitches, expression import/export, histogram tools and layered focus. FEATURES §4; ADR-0054/0178. | Unknown / not tested — remaining scope |
| To establish: Modulation graph persistence and host delivery | Source inventory: Ch.16–17; installed API E18 | Scopes, cycles, target IDs, transfer/scaling, polyphony and actual VST3/CLAP delivery times; documentary contracts recorded, runtime tests pending. ADR-0046/0052/0114. | Unknown / not tested — remaining scope |
| To establish: Plug-in isolation, latency, bounce, export and controller APIs | Source inventory: Ch.13–16 | Crash/restart restoration, suspend/wake, tail preservation, output taps, multi-out/sidechain and remote gestures. FEATURES §6/8; ADR-0122/0181. | Unknown / not tested — remaining scope |
| To establish: Audio subsystem and efficiency | Source inventory: Ch.0,7,16–17; E08–09 | Rates, device switch/combined device restrictions, processing precision, timing/CPU and mode transitions. Core-engine matrix; no measured results yet. | Unknown / not tested — remaining scope |
