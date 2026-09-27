# Feature scope — Ableton ∪ Cubase, and what neither has

**Purpose of this document.** Not a wish list. The format has to be designed
against the *full* feature set even though the software will implement a
fraction of it for years, because retrofitting schema is the expensive kind of
mistake and reserving it now is nearly free. Every row here is checked against
[`format/SPEC.md`](format/SPEC.md): if the format can't already carry it, that's
flagged, and it is a bug in the format, not in the plan.

**Priority key**

| | Meaning |
|---|---|
| **P0** | MVP. Without it there is no DAW. |
| **P1** | The first release anyone would actually use. |
| **P2** | What makes it competitive rather than a toy. |
| **P3** | The long tail. Reserve format space, build much later. |

**Format key**

| | Meaning |
|---|---|
| ✅ | Schema exists in v1.0 |
| 🔶 | Partially reserved — needs more schema before build |
| ❌ | No schema yet — must be specified before this feature is built |

---

## 1. Transport, timeline and time

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Arrangement timeline | both | P0 | ✅ | `clips`, `tracks` |
| Tempo map with ramps | both | P0 | ✅ | `tempo_map.curve` covers jump/linear/bezier |
| Time signature map | both | P0 | ✅ | separate table, deliberately (SPEC §4.4) |
| Musical **and** linear time per track | Cubase | P0 | ✅ | `time_base`; the reason for the dual-domain model |
| Loop / cycle, punch in-out | both | P0 | ✅ | `markers.kind='cycle'` |
| Markers, marker track, **with a note and an optional agent prompt** | both + | P1 | 🔶 | `markers` gains nullable `note`, `prompt` (ADR-0115) |
| Arranger track / section playlist | Cubase | P2 | ✅ | `arranger_sections` + `arranger_chain` |
| Key/scale track | Cubase-ish | P2 | ✅ | `key_map`, with a 12-bit `scale_mask` so any scale fits |
| **Ableton Link**: tempo, beat and phase shared with other apps and peers | Ableton | P1 | — | ADR-0185 d7. The SDK is GPL-2.0-or-later (its `LICENSE.md`), pinned into `third_party/` when built. It sits beside PTP (ADR-0107), which shares a sample clock, a different job. Link Audio stays a wish (ADR-0126) |
| Chord track, chord pads, harmonic linking | Cubase | P3 | 🔶 | `key_map` is the anchor; chord events need their own table |
| Timecode, video sync, pull-up/down | Cubase | P3 | 🔶 | `project.frame_rate_*` exists; video handling unspecified |
| Tempo detection from audio | both | P2 | ✅ | writes `tempo_map` |

## 2. Tracks, routing and mixing

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Audio / MIDI / instrument tracks | both | P0 | ✅ | |
| Volume, pan, mute and solo on every track; solo across groups | both | P0 | ✅ | `mixer_strip`, `tracks.muted/soloed/solo_defeat`. **In the engine** (ADR-0163): a strip after each chain, 5 ms ramps, four pan laws with Live's the default (SPEC §6.9), solo that keeps a soloed track's group and a soloed group's children. Width, input gain, phase, delay offset and VCA are stored, not yet applied. |
| Group tracks: folder **and** bus, one object | Ableton | P0 | ✅ | `kind='group'`. Grouping auto-routes children into the bus in the same transaction; `routing.origin` protects a manual override (ADR-0044). |
| ~~Folder (organisational) tracks~~ | Cubase | **no** | — | **Removed** (ADR-0044). One grouping concept. A container whose fader does nothing is the thing users pick by accident. |
| Hybrid tracks: audio and MIDI on one channel | Bitwig | **P0** | ✅ | `tracks.kind` is a hint, never a constraint (ADR-0045) |
| Return tracks / FX channels | both | P0 | ✅ | `kind='return'` + `routing.kind='send'` |
| Sends, pre/post fader | both | P0 | ✅ | |
| Sidechain routing | both | P1 | ✅ | `routing.kind='sidechain'` |
| VCA faders | Cubase | P2 | ✅ | `kind='vca'`, `mixer_strip.vca_group_id` |
| Direct Routing (multiple simultaneous outs) | Cubase | P2 | ✅ | falls out of `routing` being a table |
| Up to 64 buses per node; no cable UI | REAPER (engine), Ableton (UI) | P2 | ✅ | replaces ADR-0056's two buses; routing stays Ableton's menus and auto-grouping (ADR-0113) |
| System-audio (loopback) input on any track | neither | P1 | — | WASAPI loopback, Core Audio process taps; a resampler with declared latency (ADR-0106) |
| ADI virtual audio device out (master or any bus as an OS input) | REAPER (ReaRoute) | P2 | — | an OS driver: BlackHole fork on macOS; on Windows our own `sysvad`-based kernel driver (sysvad is MS-PL, fetched at build time, never vendored; the ruling on shipping MS-PL-derived code is the director's, ADR-0120) signed for free through SignPath Foundation, the route Virtual-Audio-Driver has used since 2025; bundling VB-CABLE was proposed and rejected (ADR-0106, ADR-0117, ADR-0118). Build workflow: `driver-build.yml` |
| Track freeze / bounce in place | both | P1 | ✅ | `tracks.frozen`, `freeze_media_id` |
| Control Room (separate monitor path, cue mixes, talkback) | Cubase | P3 | 🔶 | `routing.kind='cue'` reserved; no monitor-section model |
| Crossfader, DJ-style mixing | Ableton | P3 | 🔶 | needs a master-section table |
| Channel strip (gate/comp/EQ built in) | Cubase | P2 | ✅ | internal `devices` |
| Mixer snapshots | Cubase | P2 | ✅ | `snapshots.kind='mixer'` |
| **Native analog group summing**: on a group, each child through a console's channel half, the sum through its buss half; 8 Airwindows console flavours, level-matched; drive as gain staging | neither | P1 | ✅ | Built (ADR-0174): `group_summing` (schema 1.7), `group.setSumming`, `group.setSummingFlavor`, `group.setSummingDrive`. The toggle, dial and flavour menu in the group header are mac's. EveryConsole's six systems wait for an upstream fix |
| Per-channel delay compensation offset | both | P1 | ✅ | `mixer_strip.delay_samples`. **Plays** (ADR-0172): `mixer.setDelay`, either sign, ±1 s, as latency of the opposite sign, so delay compensation places it |

## 3. Clips, editing and comping

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Clip loop window separate from placement | Ableton | P0 | ✅ | one object, not N repeats (SPEC §6.2) |
| Fades, crossfades, fade curves | both | P0 | ✅ | |
| Take lanes and comping | both | P1 | ✅ | `lanes` |
| Linked / aliased clips (edit one, change all) | Cubase, FL Studio | P2 | ✅ | `clips.alias_of`. Cubase's *shared copies*; in FL every Playlist clip is an instance of its pattern. Live has none (this row said "both" until ADR-0160). **Stays P2** (ADR-0160). The engine plays no alias yet and reports one as a problem (ADR-0155). |
| **Make unique**: one linked clip becomes an independent copy | FL Studio, Cubase | P2 | ✅ | One op, `clip.makeUnique`: the source's content and settings are copied into the clip and `alias_of` is cleared; an audio clip keeps the same media file. Not a toggle: undo re-links, and `clip.setAlias` links again. Cubase: *Convert to Real Copy* (p. 272). FL's *Make unique as sample*, a new audio file, is `clip.consolidate`. FL's *Select all similar Clips* comes with it. **Ships with linked clips, never after them** (ADR-0160). |
| Track versions | Cubase | P2 | ✅ | `snapshots.kind='track_version'` |
| Ripple edit / insert-delete time | both | P1 | — | pure op, no schema needed |
| Warp / elastic audio | Ableton | P1 | ✅ | `audio_clips.warp_markers` (AWRP) |
| Hitpoints, slice to track | Cubase | P2 | ✅ | hitpoints are warp markers. One onset detector, cached per media file, serves hitpoints, slicing, Audio Alignment and the agent's `analyze.transients`; its filters are Cubase's Threshold, Intensity, Minimum Length and Beats (p. 637) (ADR-0176) |
| **Audio Alignment**: target clips' timing matched to a reference clip | Cubase | P2 | ✅ | The result is warp markers: one changeset of `audioClip.setWarpMarkers`, plus a split and a crossfade at a partial overlap; no schema change. The matching is a dynamic time-warping path over audio features, not transient-to-transient mapping, which fails on vocals and on takes with an extra note. Cubase's options (pp. 261–263): Match Words, Prefer Time Shifting (tries the scope's `bestOffset` first) and Alignment Precision; its bounce-first rules are not copied. **Needs warped playback first** (ADR-0061), which the engine does not have yet (ADR-0176) |
| AudioWarp quantise / groove from audio | Cubase | P2 | 🔶 | groove templates need a table |
| Groove pool | Ableton | P2 | ❌ | shared groove templates — no schema |
| VariAudio / pitch-time editing of vocals | Cubase | P3 | ❌ | per-segment pitch model; big, unspecified |
| ARA2 (Melodyne, SpectraLayers) | Cubase | P3 | 🔶 | ARA state lives in `plugin_state`; doc model needed |

## 4. MIDI and expression

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Piano roll, velocity, CC lanes | both | P0 | ✅ | `event_streams` |
| Layered editing: several clips and tracks in one note editor, one in focus | both | P2 | — | ADR-0047 d2, opt-in. Live 12's multi-clip editing and Focus Mode (§10.8.1: inactive clips' notes in gray); Cubase's Key Editor with several parts and an active part (p. 1142). Rendering and hit-testing only; no schema. |
| **Ghost-note focus switch** in layered editing | FL Studio, Live | P2 | — | Switch the editor to the clip and track that own a background note. **Parity gesture: a click on it**, as Live's Focus Mode does. FL's double right-click and X1 mouse button, from FL's own *Piano roll > Ghost notes*, are an optional binding in an FL-style mouse preset, because ADI's right-click opens the context menu, as Live's does. The context menu offers it too. Not ANOT's `ghost` flag (ADR-0160). |
| Quantise, groove, swing | both | P0 | ✅ | |
| Per-note probability | Ableton | P2 | ✅ | in the v1 note record |
| Per-note microtuning | neither, fully | P2 | ✅ | `tuning_cents` in the v1 note record |
| **Per-note expression / MPE** | both, partially | **P1** | ✅ | `note_expression` — first-class, SPEC §6.3.2 |
| **MPE+ (Haken), 14-bit Y and Z at 500 Hz** | neither | **P1** | ✅ | `ExpressionPoint.value` is f32, so bit depth was never the constraint. The binding constraint is ADR-0042's sub-block floor, which MUST NOT exceed `sample_rate/500` (ADR-0054). |
| **MPE out to plugins, VST3 and CLAP** | both, partially | **P1** | — | CLAP (ADR-0099): the dialect the plugin declares -- CLAP note expression, MIDI-MPE or MIDI. VST3 (ADR-0097): per plugin, VST3 note expression, MPE over MIDI on member channels, or plain MIDI with poly aftertouch. The controller's channel never reaches a plugin. Pitch, pressure and timbre measured by ear per route (ADR-0098, ADR-0100); a fixture VST3 exercises the IMidiMapping parameter path in CI. Surge XT reads MpeMidi, Serum 2 reads note expression, and `Auto` can only be right for one of them -- so the route choice must be remembered: decided, in an application registry per plugin and on the device row per project (ADR-0134 d7), not built yet. |
| **MIDI 2.0 in (UMP)**: 32-bit notes, per-note pitch, pressure and controllers from hardware | MIDI 2.0 | P2 | ✅ | ADR-0185 d2. Parsed into ADI's own events and `AEXP` per-note expression, so MPE channel rotation stops being the only route. Out: a CLAP whose note port declares MIDI 2.0 gets UMP (ADR-0099 left it unbuilt); a VST3 cannot, since JUCE 9.0.2's VST3 host has no UMP path (ADR-0073). In: JUCE 9.0.2's own UMP endpoints (`juce::universal_midi_packets::Endpoints`) on CoreMIDI, ALSA and Windows MIDI Services, with `JUCE_USE_WINDOWS_MIDI_SERVICES` switched on (ADR-0188 d7) |
| **MIDI-CI property exchange**: a controller discovers the DAW and labels itself from the track in focus | MIDI 2.0 | P3 | — | ADR-0185 d3: moved from WISH. It reads the one parameter feed (ADR-0181 d3), as the control API and OSCQuery do. It waits on the OS MIDI stacks exposing MIDI-CI |
| **Hardware CV/Gate**: CV Out and CV In nodes in the modulation graph, for Eurorack | Bitwig | ADI 2 | — | **Deferred to ADI 2, unprioritized** (ADR-0188 d7): no settings and no calibration work until then. The design stays ADR-0185 d5's. Audio-rate and sample-accurate. Three safeguards: the user marks DC-coupled outputs; a CV channel is never the main or monitor output and never summed to the master, because DC can damage a speaker; a per-output calibration for 1 V/octave. The round trip is declared as latency. The Lynx E44's coupling is to be confirmed from its manual |
| Scale-aware / scale-locked editing, **every scale including Arabic and microtonal** | Ableton 12 + | P2 | 🔶 | reads `key_map`; a 12-bit `scale_mask` cannot name a quarter tone, so `tuning_systems` + `tuning_degrees` + `key_map_degrees` child tables come first (ADR-0103, ADR-0117, gap 6). **In the format since 1.8** (ADR-0178), with `key_map_tunings` beside `key_map`; their ops come with the editing. Notation stays out. |
| Expression Maps (articulations) | Cubase | P3 | ❌ | needs its own schema; big win for orchestral |
| Logical Editor / Project Logical Editor | Cubase | P3 | — | query+transform over the model; no schema |
| Score editor / notation | Cubase | P3 | ❌ | engraving data is **not** derivable from MIDI |
| MIDI effects / devices in chain | both | P1 | ✅ | `devices` with `subtype='midi_effect'` |
| Capture MIDI (retroactive record) | Ableton | P2 | — | runtime buffer, no schema |

> **Why per-note expression is P1 and not P3.** A Continuum, Osmose or Seaboard
> produces continuous per-note pitch, pressure and timbre. Cubase VST Note
> Expression and Ableton MPE each model part of this and neither is a superset.
> Storing it as "MIDI channel tricks" discards the performance irreversibly, and
> no later version can recover it. This is the one place where being late is
> equivalent to being wrong.

## 5. Session View — returns last, as a secondary window

ADR-0037 removed the clip-launching matrix; **ADR-0101 brings it back as a
secondary, undockable window** (F3, like Cubase's MixConsole), built after the
arrangement is finished and verified (ADR-0108). The same window carries a
Cubase-style MixConsole view with a toggle between the two.

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Session View, docked in the main window Ableton-style, detachable | Ableton | P3 (last) | ❌ | `scenes`, `clip_slots` return to Layer 1 in a schema PR now, mirroring Live's shape: a scene list, one slot per track per scene, launch settings on the clip (ADR-0101, ADR-0117); the 14 ops return with the UI |
| MixConsole view toggled inside it | Cubase | P3 (last) | ✅ | same strips, reparented; one `TrackOrderModel` (ADR-0063) |
| Live performance | — | ~~ADI Live app~~ dropped | — | **Dropped by ADR-0190:** live performance is ADI's own Session view, docked and detachable (ADR-0101, ADR-0117), as in Ableton. The suite is ADI and ADiJ (ADR-0133, ADR-0188 d1, ADR-0190). ADiJ runs on ADI's engine; Mixxx is a reference, never forked (ADR-0188 d8) |

ADI is still an arrangement-first DAW: the timeline is the default screen and the
first thing built, and what it takes from Cubase is **arrangement and
audio-editing depth**. Sketching has to be earned on the timeline *first*; the
Session window is the last step, not a way around that.

## 6. Devices, racks and plugins

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| VST3 hosting | — | P0 | ✅ | `plugin_state.stream_role` |
| CLAP hosting | — | **P0, mandated** | ✅ | same. **JUCE has no CLAP host**, so this is our code (ADR-0041). Level with VST3 and sequenced after it — CLAP's `PARAM_MOD` is ADR-0046's rule expressed in a plugin API, so the modulation architecture depends on it (ADR-0052). Audited against the pinned headers (linux, #72): activation bounds, the start result, the host's rescan query, pre-activation flushes and the main-thread latency rule corrected (ADR-0123); the active-state port scan is mac's. |
| LV2 (Linux) | — | P2 | ✅ | same |
| ~~AU / AUv3 (macOS)~~ | — | **no** | ✅ | **Ruled out** (ADR-0041). Not the hosting wrapper — JUCE ships one — but the registry-based discovery, `auval`, a third stream role and a macOS-only bug class around it. |
| ~~VST2~~ | — | **no** | ✅ | **Ruled out** (ADR-0015): SDK unobtainable for years, and its terms were never GPL-compatible. Not recoverable. |
| *Unhosted formats still open as placeholders* | — | **P0** | ✅ | `plugin_refs.format` still admits `au`, `auv3`, `vst2`. We do not host them; the format must still be able to say one was **there**, or a converter silently drops devices (ADR-0011, ADR-0041). |
| **Missing-plugin preservation** | both | **P0** | ✅ | SPEC §7.1 — non-negotiable. Live in the session: a plugin that will not load stands in as a placeholder carrying its parameter mirror and its state bytes, bypassed, in its place in the chain (ADR-0122 d4, d5) |
| **A project, live: the session runtime** | — | **P0** | ✅ | `engine::Session` (ADR-0122): a `.adi` read, its device rows resolved through an injected loader, the graph published to the device callback. The rows place devices — an edit re-read follows `ord` on the same instances; a removed device retires rather than dies, and an undo finds it waiting. Sources (a clip reader, a tone) inject like devices. Headless, all seven ABIs; JUCE supplies only the callback and the loader. Racks are skipped and named until ADR-0060's node exists. `adi_play` (`src/juce/play.cpp`) proves it through a real device: a project with the fixture VST3 and a real CLAP, a tone through the CLAP at the level it went in, a 512 → 2048 block-size change mid-run on the same instances. |
| **Plugin parameter edits inside the plugin's window are ops** (global Ctrl-Z) | neither | **P0** | ✅ | one gesture, one op; chunk snapshots for what is not a parameter (ADR-0110, amends ADR-0038). Built: the capture layer (linux, #70) and the glue `engine::ParamOps` (ADR-0124) — CLAP output events and JUCE's VST3 listener in, `device.setParam` out, undo back through the plugin with the echo swallowed; the first edit of a parameter writes where it started. Proved on a real VST3 through JUCE (the fixture's in-window drag is one op; ADR-0141). A preset picked in the plugin's own browser is one `device.loadState`, VST3 and CLAP, and the state round-trips through the project (ADR-0142). |
| Racks: instrument / effect / drum | Ableton | P2 | ✅ | `device_chains` with key/vel/chain zones |
| Macros with per-target range and **multi-breakpoint curve**, several mappings per target, per-macro enable | Ableton + | P2 | 🔶 | `macros`, `macro_mappings`; `curve` becomes a breakpoint BLOB (ADR-0114) |
| Cross-track modulation and macro targets | Bitwig | P2 | 🔶 | arrives with the modulation schema (ADR-0114, ADR-0046) |
| A Pd device with a second, floating view (the analyser) | neither | P2 | — | published arrays in the device contract (ADR-0116) |
| **`.amxd` translator** with a pre-flight scan: Max for Live devices into ADI Pd | neither | P2 | — | An allowlist scan first. If every box is in the translation table, the device converts automatically, with Max's right-to-left order made explicit. If not, nothing is written, and a dialog hands over the JSON and a prompt for an AI. Whatever comes back passes a second, vanilla-only scan. Needs the device contract (DEVICE-CONTRACT-PANEL §6). |
| Plugin delay compensation | both | P0 | ✅ | `devices.latency_samples`. Reported in samples and **excludes** the device buffer — ADR-0042. |
| 2048–4096-sample blocks, tested | — | **P0** | — | runtime. Dense chains, not low-latency tracking (ADR-0042). **4096 is the cap on every platform** and the granted size is the only one that exists (ADR-0049). |
| Sub-block automation and MIDI accuracy | both | **P0** | ✅ | runtime. At 8192 a block is 171 ms; per-block updates would step audibly. The price of the row above (ADR-0042). |
| Change block size without reloading | both | **P0** | — | runtime. At 8192 overdubbing is impossible, so moving between sizes mid-session is not optional (ADR-0042). **Built at the engine** (ADR-0122 d6): `Session::prepare` at a new size rebuilds the graph on the same instances, tested at every size from 32 to 4096; a same-format restart is a no-op. |
| VST3 silence flags + tail-aware suspension | — | **P0** | ✅ | runtime; `devices.always_process` opts out (ADR-0043) |
| Host-side modulation to any VST3 parameter | Bitwig | P1 | — | routing persists, output never does (ADR-0046) |
| ~~Plugin sandboxing (crash isolation)~~ | Cubase | **no** | — | **Rejected** (ADR-0047). Not for IPC cost — that amortises over a 171 ms block and is cheapest here — but for latency and complexity. Affordable because a crash loses one gesture, not the session (ADR-0001). |
| ~~Dedicated Inspector panel~~ | Cubase | **no** | — | **Rejected** (ADR-0047). Mixer strip, device chain and detail editor must carry every property between step 7 and step 9, and must be keyboard-reachable. |

### Live's devices, cloned (ADR-0169)

Behaviour from the Live 12 manual (`reference/DOCS`, chapters 28–32). Names
are ours where Ableton's is coined. Every one is a native device in the graph,
for the transport, the tempo map, the groove pool and the track's scale
(`key_map`). Randomness is seeded, so a render repeats exactly.

| Device | Kind | P | Fmt | Notes |
|---|---|---|---|---|
| Arpeggiator | MIDI effect | P1 | ✅ | Styles, including Play Order, Chord Trigger and three random styles; Hold; Offset; Groove; Rate in ms or synced; Distance and Steps, in semitones or scale degrees; Gate; Retrigger (off, note, beat) with Interval; Repeats; velocity decay to a target |
| Chord | MIDI effect | P1 | ✅ | Six shifts of ±36 st with Learn; per-note velocity or chance; Strum up to 400 ms with Tension and Crescendo; per-note events to the generated notes |
| Scale | MIDI effect | P1 | ✅ | A 13×13 note matrix, base and scale or the track's own, Transpose ±36, Fold, Lowest and Range |
| Pitch | MIDI effect | P1 | ✅ | ±128 st or ±30 degrees, step buttons, Lowest and Range with Block, Fold or Limit |
| Velocity | MIDI effect | P1 | ✅ | A curve with Drive and Compand; output range; Clip, Gate or Fixed; Random; note-on, note-off or both |
| Note Length | MIDI effect | P1 | ✅ | Trigger on note on or note off; Gate and Length in ms or synced; Latch; release velocity, Decay and Key Scale |
| Random | MIDI effect | P1 | ✅ | Chance, Choices × Interval, Random or Alt (round robin), Add, Sub or Bi, scale-aware |
| CC Control | MIDI effect | P2 | ✅ | Mod wheel, pitch bend, pressure, a switch and twelve assignable controls, Learn and Send |
| Note Echo, MPE Control, Expression Control, MIDI Monitor | MIDI effect | P2 | ✅ | Live Suite's Max for Live MIDI effects, as native devices |
| Envelope MIDI, Shaper MIDI | modulator | P2 | 🔶 | They map to any parameter, so they wait for the modulation architecture (ADR-0046, ADR-0052) |
| **Utility** | audio effect | P1 | ✅ | Phase L and R; channel mode; Width and Mid/Side; Mono; Bass Mono 50–500 Hz with audition; Gain −∞ to +35 dB; Balance; Mute; DC filter |
| **OneShot** (Live's Simpler) | instrument | P1 | ✅ | Classic, 1-Shot and Slice; Start, Loop, Length and Fade; warp; filter; LFO; envelope; voices; the Controls tab |
| **Sampler** | instrument | P2 | ✅ | OneShot's engine plus multisample zones (key, velocity, sample select), loops with crossfade, a modulation oscillator, modulation, MIDI routing and MPE |
| **Redux** | audio effect | P2 | ✅ | Rate with Jitter; pre and post filters; Bits with Shape; DC Shift; Dry/Wet Sources (ADR-0187 d3): Bespoke's `BitcrushEffect` (GPL-3.0), Airwindows DeRez (MIT, already in `third_party`), the p0p bitcrusher (GPL-3.0); none has jitter, which is ours to write. |
| **Shifter** | audio effect | P2 | ✅ | Pitch, Freq and Ring modes; Spread and Wide; a synced delay with Feedback and Tone; an LFO with ten shapes; an envelope follower Sources (ADR-0187 d3): Freq and Ring from Surge's Frequency Shifter and Ring Modulator (GPL-3.0), Speechrezz FrequencyShifter (MIT) or Pd's `hilbert~`; Pitch from ADR-0176 d8's candidates. |
| **Overdrive** | audio effect | P2 | — | ADR-0187 d3. A band-pass before a waveshaper (BYOD's, GPL-3.0), a low-pass tone stage after it, and an envelope follower on a VCA for Dynamics; Dry/Wet. In Pd: `env~` (RMS in dB, ADR-0096) and `*~` |
| Live 12's MIDI Tools (Transform and Generate) | clip ops | P2 | — | Not devices: clip-editing tools (manual chapter 11). Noted by ADR-0169, **not ruled** |

## 7. Automation

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Track and device automation | both | P0 | ✅ | `automation_lanes`. **Track lanes play** (ADR-0164): volume, pan and mute, read by the strip every 32 samples at the playhead, with Live's override and Re-Enable (ADR-0162). **Device lanes play in the engine** (ADR-0165): normalized `ParamValue` events on ADR-0054's grid and at every point, on their own segments, with the same override. Real plug-ins hear them once the device host translates them, which is mac's (VST3 `IParameterChanges`, CLAP plain values). |
| Clip envelopes / clip modulation | Ableton | P1 | ✅ | `automation_data.clip_id` |
| Event volume curves drawn on the clip (Cubase 14) | Cubase | P1 | ✅ | a clip-scoped gain lane rendered on the event (ADR-0115) |
| Automation modes: touch/latch/cross/overwrite/trim | Cubase | P2 | ✅ | `tracks.automation_mode` |
| Curved automation segments | both | P1 | ✅ | `curve` + `tension` per point |
| Automation in real units, not just normalized | neither | P1 | ✅ | `value_domain='real'` — SPEC §6.3.3 |

## 8. Recording and audio

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Multi-track audio + MIDI record | both | P0 | ✅ | |
| Loop record to take lanes | both | P1 | ✅ | |
| Input monitoring modes | both | P0 | ✅ | `tracks.monitor_mode` |
| Media pool with content addressing | Cubase-ish | P1 | ✅ | BLAKE3, SPEC §10.1 |
| ~~Collect & Embed~~ **Collect and Export to ZIP** | both | P1 | ✅ | media is never embedded (ADR-0127); `media_blobs` and `media_files.embedded` retired and locked in schema 1.1, removed at 2.0 (ADR-0136); SPEC §10.4 |
| Missing-file relink by content hash | neither | P1 | ✅ | SPEC §10.2 |
| Stem / batch / queued export | Cubase | P2 | — | runtime |

## 9. Session and workspace

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Restore zoom, scroll, selection, playhead | partially | P1 | ✅ | `ui_view`, `session_state` |
| Restore plugin windows *with monitor identity* | neither, well | P1 | ✅ | `window_state` — SPEC §8.4 |
| **Persistent undo across restarts** | neither | P1 | ✅ | `ops` |
| **Branching undo tree** | neither | P2 | ✅ | `op_branches` |
| Project-scoped controller maps | partially | P2 | ✅ | `controller_maps` |
| **External control surfaces, the Stream Deck + XL first**: dials, keys and touch strips with feedback | Bitwig, Live | P2 | ✅ | ADR-0181. A relative tick resolves to a value at the input and a turn coalesces into one absolute `device.setParam`; the log never holds deltas, because a clamped delta has no inverse. A surface is a client of the loopback control API (ADR-0039): paired once, an `Origin` check, an allow-list of ops applied at once as the user's own (`actor_detail = surface:<name>`). Mackie, HUI and OSC go through `controller_maps`. The Elgato plug-in is not built now. **Acceleration:** ticks under 30 ms apart multiply the step by 4^((30 − Δt)/30), at most 4×. **Follow or pin:** the page follows the selection unless the surface's Pin key locks it to a device or channel. **Contention:** last touched wins; a non-owning absolute control catches up by Takeover Mode (ADR-0188 d4). **Reference, not a base:** the director's Studio OS plug-in (`tools/elgato_stream_deck_plugins/adi_studio_os`), built for Ableton and Rekordbox on the same 36 keys and 6 dials as the + XL. ADI's plug-in is new; what carries over: **Its navigation:** a stack of screens whose root cannot be popped, so Back always ends at home (`js/core/nav.js`). **Its timing lesson:** the plug-in's hidden page throttles its own timers (a 500 ms long-press measured 1187 ms), so real delays run in a Worker (`js/core/timing.js`). **Its tempo-delay calculator:** 60000/BPM, times 4/denominator, dotted x3/2, triplet x2/3, kept as exact fractions until they are shown (`js/modules/console.js`). ADI's reads the tempo map instead of a typed BPM. **Its local WebSocket service** (`service/ws-server.js`, 127.0.0.1) has the shape of ADR-0181's client, but it has no `Origin` check, which ADR-0181 requires. **The per-plug-in VST controllers** (`adi_ableton_vst_controller`, copied into Studio OS's `js/ableton/`) are a reference in the same way: they drive plug-ins through Ableton today, and ADI's will drive them through adi-daw, over the control API and the parameter feed. They are not refined yet; the best-working are the two Analog Obsession ones, dBComp and Indeq (`docs/DBCOMP.md`, `docs/INDEQ.md`). |
| **One parameter feed** for the UI and every surface: name, stored and playing value, the plug-in's text, the automation state | neither | P1 | — | ADR-0181 d3. Built with step 7's UI on the one UI clock; the audio thread publishes into lock-free slots and never calls an observer |
| **The device strip grows taller than Live's, never shorter** | neither | P1 | — | ADR-0184: a dated departure from Live (ADR-0108), whose Clip View grows and whose Device View does not. The floor is Live's default device area, 169 logical pixels by the director's figure, checked against Live (ADR-0188 d6). Views that declare they scale (a Pd analyser, the scope) fill the height; the DAW-drawn panel top-aligns; plug-in windows float. The height is `ui_view` state; no schema change |
| **Visual array streams for surfaces with screens**: spectrum, meters, correlation, scope windows | neither | P2 | — | ADR-0184 d4. Opt-in per stream on the control API; built from the slots the UI reads, display-ready and no faster than the UI clock. Raw audio is a separate, capped stream, marked while held. **Wire format** (ADR-0188 d6): WebSocket binary frames, a 16-byte little-endian header (u16 stream id, u16 channel count, u32 frame size, u64 timestamp), then float32 values. **Names:** `track.<id>.meter`, `track.<id>.spectrum`, `master.correlation`, `device.<id>.scope` |
| **OSCQuery**: the parameter tree served to TouchOSC and Lemur, discovered on the network | OSC | P2 | — | ADR-0185 d4. The one parameter feed as OSCQuery JSON, found over DNS-SD; values as OSC 32-bit floats. OSC has no authentication, so it is off by default. Turning it on opens read and write, after a warning to use it only on trusted networks; an interface the user picks and an optional client list stay. An in-process responder on nlohmann/json, no web-server library (ADR-0188 d7) |
| **TUIO** touch sources: tables, IR overlays, network surfaces, and macOS | TUIO | P3 | — | ADR-0185 d6. Not needed for Windows touch screens, where JUCE already delivers native multi-touch. TUIO 1.1 over OSC (UDP 3333) joins the same input path, off until opened |
| Templates | both | P1 | — | a `.adi` with a flag |
| **Swap the docked side of browser and mixer** | Bitwig/Cubase muscle memory | P2 | ✅ | `ui_view` — ADR-0080; width follows the panel, not the side |
| Named view filters, AI view groups, far/close scaling, collapsible mixer and device strips | Bitwig | P2 | ✅ | `ui_view`; a `view.*` op family (ADR-0112) |
| App-scoped sample library and the floating palette (Cmd+I), usable with no project open | neither | P1 | — | an app-level SQLite store; a preview path in the engine (ADR-0104) |
| Open a historical undo node in a silent, read-only tab | neither | P2 | — | a materialised copy by replay; paste back is the ordinary transaction (ADR-0111) |

## 10. What neither DAW has — the actual reason to build this

| Feature | P | Fmt | Notes |
|---|---|---|---|
| **Open, documented, reimplementable format** | P0 | ✅ | the whole premise |
| **Every mutation is a typed, attributed op** | P0 | ✅ | SPEC §8.1 |
| **AI agent that can only act through ops** | P2 | ✅ | [AI-AGENT.md](AI-AGENT.md) |
| Agent changes previewable as a diff before commit | P2 | ✅ | |
| **Agent runtime, `adi-agent`**: pi-mono's loop in a sidecar, three tools over the RPC boundary | P2 | — | ADR-0168. It takes OpenClaw's loop and ideas, not a fork of OpenClaw (**approved** by the director). It needs, in order: the registry's JSON Schema export, with field descriptions; the loopback RPC server (ADR-0039); the sidecar; the chat panel. Its tools are `project_read`, `ops_describe` and `changeset_propose`. |
| **A scope built in**: any two tracks compared (overlay, stacked, difference, sum), with correlation and offset, aligned by delay compensation, on the grid, true peak | P1 | — | ADR-0167. OScope and PsyScope need routing, and cannot see another track's latency. A measured offset is fixed with `mixer.setDelay` (registered and playing, ADR-0172). It adds a second audio-to-UI path, which amends ADR-0050 d4. **Engine side built** (ADR-0175): `Session::openScope` taps, stamped with when each frame is heard; BS.1770-4 true peak; `correlation` and `bestOffset`. The panel is mac's. |
| Undo that survives a reboot | P1 | ✅ | |
| Undo you can *branch*, so exploring costs nothing | P2 | ✅ | |
| Text projection for version control | P2 | — | ADR-0007 |
| **Multiplayer Remote Sync (CRDT op-based)**: several people editing one project over the internet, Excel or Figma style | P3 | 🔶 | The op log is the substrate. Since schema 1.6 every op carries its client and a Lamport clock (`op_clocks`, ADR-0161). Still to decide, in SPEC §12 item 6: server-ordered as Figma and Excel are, or peer CRDT; row ids from two clients; concurrent reordering; shared undo. Media and plug-in state travel by BLAKE3 hash already. No network or UI code before it is scheduled. **Its shape is decided in ADR-0182.** |
| Scripting API identical to the agent's op vocabulary | P2 | ✅ | one API, not two |
| **Local-only projects**, with the full branching history and no network | P0 | ✅ | today's behaviour; collaboration is opt-in per project (ADR-0182 d1) |
| **Back up project to cloud**: a consistent copy plus its media, into a Google Drive, iCloud Drive, OneDrive, Dropbox or IDrive folder | P2 | — | ADR-0182 d3. The SQLite backup API or `VACUUM INTO`, never a copy of the live file, and Collect and Export's ZIP for the audio. A drive is never a project's home |
| **Collaborative projects**: ops streamed to the user's own S3-compatible bucket; media chosen by the author and stored by hash; incoming changes previewed, then applied; collisions resolved per object; summaries from the ops, AI prose optional | P3 | — | ADR-0182. Not Git of the text projection, which leaves out eight tables. A collision needs each batch's base, a version vector: Lamport clocks alone cannot tell concurrent from sequential. **Keep both everywhere two can be held** (the director's revision): a clip becomes a take; continuous data (automation, event volume, tempo) keeps the collaborator's region as an inactive ghost, colour-coded, resolved one region at a time by default (Accept All and Reject All exist in the sync panel's menu, never suggested, one op each) by Keep Mine, Adopt Theirs or Combine, a 0 to 100% weighted blend in the lane's own domain, never a forced 50/50; discrete lanes do not blend (ADR-0182 d9) |
| **Sync, in detail**: batches named `batch_<lamport>_<client_id>.cbor`, the Lamport number zero-padded to 20 digits; a `sync_vectors` version-vector table; undoing a synced batch appends a compensating batch; remarks anchored to ranges and ops through `remark_anchors` | P2 | — | ADR-0188 d5. Both tables land with sync's schema minor |

---

## 10.5 Native DSP nodes, and asynchronous AI

Two blocks of the director's blueprint (2026-09-20). The **decisions** are
ADR-0058 to ADR-0064; this is the backlog they govern.

**Measured, not asserted:** the engine's callback cost per project and block size lives in [`BENCHMARKS.md`](BENCHMARKS.md) (ADR-0102 d3): how to run the headless benchmark, the eight-size matrix, and the recorded tables before and after the sleeping-node fixes (#60, #65).

### Native DSP nodes (ADR-0062)

Built into the graph rather than hosted, for transport access, the full-
resolution event stream, and sidechain-as-an-edge — **not** for zero latency,
which is a property of the algorithm and not of where it is compiled.

| Node | P | Latency | Notes |
|---|---|---|---|
| Sub-sample phase utility | P1 | polarity **0**, nudge ~0 | polarity inversion is exactly free; a fractional delay is not. **Part of Utility** (ADR-0169) |
| Audio-rate envelope follower | P1 | 0 | a modulator under ADR-0046; first real consumer of ADR-0052's `PARAM_MOD` problem |
| Grid-locked volume shaper | P1 | 0 | reads the tempo map directly — the reason it is native |
| Transient shaper | P2 | 0 | built on the envelope follower above, not on the hitpoint detector: shaping is a real-time gain, detection is peak picking with look-ahead (ADR-0176 d4) Reference: the p0p `trainsient` (GPL-3.0) (ADR-0187 d3). |
| Frequency shifter | P2 | Hilbert transform is not free | ring-mod / Hilbert, sample-accurate linear shift. **Part of Shifter** (ADR-0169) |
| Vocoder | P2 | filter-bank dependent | sidechain via `Bus::Sidechain`, no user wiring |
| Multiband graph splitter | P2 | **thousands of samples in linear phase** | **blocked on N-bus outputs** (ADR-0056). Declares its latency; a minimum-phase mode is a user choice, not a silent default |

### The DSP plugin line — future (ADR-0093)

Started with ADR-0166: RMSC is built as a CLAP, and ZL Equalizer 2 builds as
the equalizer's baseline. The rest are prepared for, with references in
`reference/`. Working titles only — shipped names are ours (ADR-0093 d4).

| Goal | Form | P | Needs first |
|---|---|---|---|
| Dynamic EQ: matched phase, linear phase, per-band dynamics | CLAP | P3 | Plugin-line licence decided; matched phase from Vicanek (2016); ZLEqualizer's code usable since ADR-0138, making the plugin AGPLv3. **The baseline builds:** ZL Equalizer 2 as CLAP, unchanged (ADR-0166). "Pro-Q3 clone" is a description, never a name |
| True-peak limiter: lookahead, oversampling, modes | CLAP | P3 | Plugin-line licence decided (the LSP maths is LGPL) |
| Lookahead brickwall limiter, 1.5 / 3 / 6 ms | Pd | P3 | **A Pd patch able to declare its latency** (ADR-0035 has no such thing), and pinned DSP sort order |
| Eight-band parametric EQ | Pd | P3 | Pd's inverted `biquad~` feedback signs; four biquads for a 48 dB/oct cut |
| ADAA clipper, adjustable knee, up to 4x oversampling | CLAP | P3 | Plugin-line licence decided (the chowdsp waveshapers are GPLv3) |
| Ring-modulation sidechain ducker (RMSC) | Pd, **CLAP ✅** | P3 | Judged as amplitude modulation, which is what it is. **ADI RMSC is built** (ADR-0166, `plugins/rmsc`): CLAP, VST3 and standalone; stereo main and sidechain; threshold, release, depth, Merge AUX; a scope of the applied gain. The engine's own DSP, extended with the threshold and the release |
| **Pd devices from Surge's effects**: Shifter, Redux, Delay and Reverb first, then the rest of the 31 | Pd | P2 | ADR-0187 d1, d2; ADR-0188 d8: vanilla Pd where the DSP is small; where it is large, an ADI external wrapping Surge's C++, compiled into the engine and registered with libpd as a built-in. No external is ever loaded from disk. Until then Surge XT Effects, built by the fork, hosts every Surge effect as one plug-in |
| **A DJ filter** (a device, and ADiJ's) | Pd, native | P2 | ADR-0187 d3: Mixxx's filter effect (GPL-2.0-or-later) as the behaviour, JUCE's `StateVariableTPTFilter` as the core |
| **OTT-style multiband**, harmonic and percussive split first | native | P3 | ADR-0187 d3: ANATOMY's cos² split and three-band upward and downward compression, cloned as behaviour; its code is AGPL-3.0, so copying it makes the receiver AGPLv3 |
| **A drum rack tab, with beatbox-to-MIDI** | native | P3 | ADR-0187 d3: `d33p` (GPL-3.0) as the reference; beatbox-to-MIDI runs natively only under ADR-0186's rules |
| **Chord Comb** (working name): six tuned combs for color bass, Saw and Square modes, Decay as a T60, Color in the loop, eight stored chord States | Pd | P2 | ADR-0192 d2: `[adi.combchord~]` compiled in, DSP in `src/adi/dsp/`; Surge's Combulator (GPL-3.0-or-later) as the reference. MIDI tuning once MIDI reaches Pd devices. win_codex |
| **Color Cab** (working name): a time-stripped formant filter built from a dropped sample, zero latency | Pd | P2 | ADR-0192 d3, d4: `[adi.colorcab~]`; a minimum-phase FIR from the sample's smoothed, flattened spectrum, swapped by crossfade; the sample through `[adi.sample]`. win_codex |
| **The guitar suite**: neural amps, pedals, cabinets | sibling CLAP | P3, next year | ADR-0186 d6: RTNeural, NeuralAmpModelerCore, AIDA-X, Proteus, AmpForge, Soundshed Guitar (AGPL-3.0), ToobAmp. Not scheduled |

Four of the six carry latency, which makes them the first plugins able to test
ADR-0079/0085/0092 with source we can read instead of borrowing FabFilter's.

### Open-source plug-ins built as CLAP (ADR-0166)

Upstream code, unchanged, each its own binary under its own licence
(`plugins/README.md`). All scan and play in the DAW.

| Plug-in | Upstream licence → our build | CLAP id | Status |
|---|---|---|---|
| Smartelectronix: Smexoscope and ten more | GPL-3.0 → AGPLv3 (JUCE 8) | `com.adi.smartelectronix.*` | ✅ |
| ChowTapeModel | GPL-3.0 → GPLv3 | `org.chowdsp.CHOWTapeModel`, theirs | ✅ |
| ChowCentaur | BSD-3 → GPLv3 (JUCE 6) | `com.adi.chowdsp.chowcentaur` | ✅ |
| ZL Equalizer 2 | AGPL-3.0 → AGPLv3 | `com.adi.zlaudio.zlequalizer2` | ✅ |
| Dragonfly Hall, Room, Plate, Early Reflections | GPL-3.0 → GPLv3 | `michaelwillis.dragonfly.*`, theirs | ✅ |
| **ADI Airwindows**: 160 of 524 by the director's five rules (ADR-0170), in eleven suite plug-ins (Color among them) with the algorithm chosen inside, a 5 ms crossfade and auto gain (ADR-0171, ADR-0173); the GUI is mac's. The console systems are the mixer's | MIT → GPLv3 | `com.adi.airwindows.<suite>` | ✅ |
| **Host-side auto gain** for any plug-in, reusing `dsp::AutoGain` | — | — | idea: needs a per-device flag, which is a schema change |

### Time-stretch (ADR-0061)

| Engine | P | Job |
|---|---|---|
| Bungee (MPL-2.0, pinned) | P1 | scrubbing, varispeed, zero and negative speed |
| libsamplerate (BSD-2-Clause, pinned) | P1 | Re-Pitch: variable-ratio resampling, speed and pitch together (ADR-0188 d2) |
| Rubber Band (GPL-2.0-**or-later**) | P1 | high-quality warp and pitch-shift. The licence is pre-authorised (`OPEN_SOURCE_POLICY.md` §3); confirm the "or later" wording at the pinned commit. **Not built:** a warped clip plays as silence today, and Audio Alignment waits on this (ADR-0176) |

**Which engine plays each warp mode** (ADR-0188 d2, closing ADR-0176 d8):

| Live mode | Engine | Parity note |
|---|---|---|
| Beats | a native slice player on the hitpoint detector | loop modes and Transient Envelope as per-segment envelopes |
| Tones | Rubber Band R3 | Grain Size maps onto R3's window option: a named gap |
| Texture | a native granular player | Fluctuation is random grain jitter |
| Re-Pitch | libsamplerate, variable-ratio resampling | exact: tape and turntable varispeed |
| Complex | Rubber Band R3 | |
| Complex Pro | Rubber Band R3, formants preserved when transposed | Envelope has no Rubber Band counterpart: a named gap |

Listening tests against Live gate each mode (ADR-0108). No SoundTouch.

### Freezing and racks

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Track freeze | both | P1 | ✅ | `tracks.frozen`, `freeze_media_id` — present since the first draft |
| **Group** freeze | Ableton | P1 | ✅ | renders the summed bus; children leave the graph entirely (ADR-0059) |
| Freeze fingerprint | neither | **P1** | ✅ | an unfreeze against a changed chain is silently stale audio, which sounds fine and is wrong (ADR-0059) |
| Racks with 8–16 macros | Ableton | P2 | ✅ | a rack is a node owning a nested graph; a macro is a modulator (ADR-0060) |

### Asynchronous AI (ADR-0064)

**Remote APIs only. No bundled Python, no model weights.** Offline these grey
out and recording, VST3 hosting, graph processing and saving are untouched —
guaranteed structurally, because nothing in `adi_core` may link an AI path.

**Real-time inference is DSP, not AI** (ADR-0186, amending ADR-0064). A network that runs on the CPU, allocates nothing on the audio thread, and needs no Python and no GPU may run inside the binary as a node: neural amps are the first case. A model is data, loaded like an impulse response, with its own licence. Everything generative stays remote.

Every one of these lands as **ops** in one `txn_id`, so each is one Ctrl-Z.

| Workflow | Upstream | P | What it emits |
|---|---|---|---|
| Stem splitting | Demucs | P1 | a group folder and N tracks, phase-aligned |
| Audio → MIDI | Basic Pitch | P1 | notes onto a hybrid track |
| Intelligent sample management | Essentia / CLAP | P1 | BPM, key and text-searchable tags in the browser |
| Reference matching | Matchering | P2 | a native EQ curve, not a rendered file |
| Vocal chopping | Whisper | P2 | clip splits at word transients from speech timestamps |
| Drum humanisation | Magenta | P2 | micro-timing and velocity edits to existing notes |
| De-reverberation | DeepFilterNet | P2 | new media; the original survives |
| Super-resolution | AudioSR | P2 | new media; the original survives |
| Timbre transfer | IRCAM RAVE | P3 | new media; the original survives |
| Neural morphing | NSynth | P3 | new media |
| Text → audio | Stable Audio Open / AudioGen | P3 | new media on a new clip |
| Infilling and looping | VampNet / AudioLDM 2 | P3 | new media over an erased range |
| Pitch-tracking synthesis | DDSP | P3 | notes plus expression from humming |
| Spoken word | Coqui XTTS | P3 | new media |
| YouTube subtitle search | yt-dlp + transcript API | **P3, legal review first** | see below |

> **The YouTube scraper is not like the others.** Importing the audio is a
> download YouTube's terms prohibit, of somebody's copyrighted recording.
> `yt-dlp` is a legitimate tool and this is not a refusal — but shipping it as a
> built-in feature of a distributed DAW is a different act from a user running
> it themselves. ADR-0064 recommends building the **timestamp search** against a
> URL the user supplies and making the import an explicit action on material the
> user asserts they may use.

## 10.6 ADiJ, the DJ application (ADR-0105, ADR-0133, ADR-0191)

Built after the DAW, on the same engine (roadmap step 13). The rows are its
backlog; the priorities are within ADiJ.

| Feature | App | P | Notes |
|---|---|---|---|
| **USB export for CDJs**: `export.pdb`, the ANLZ files (grid, cues, waveforms) and the settings files, written by ADiJ | ADiJ | P2 | ADR-0191 d2. From crate-digger's Kaitai specs under MPL-2.0 (ADR-0189); rekordcrate (MPL-2.0) and Vynull (GPL-3.0) as references. Before it is called complete, each target player is checked for whether it reads `export.pdb` or the encrypted Device Library Plus (`exportLibrary.db`); writing the latter needs a key, which is the director's question |
| **Rekordbox XML**, written and read | ADiJ | P2 | ADR-0191 d2. The desktop application's own interchange: tracks, playlists, cues, grids. Reading it brings a Rekordbox library in with no database key |
| **A source on Pro DJ Link**: CDJs browse and load ADiJ's tracks over Ethernet, with waveforms, grids and cues, no USB stick | ADiJ | P2, after the USB export | ADR-0191 d3. Announcements on UDP 50000–50002, a database server, an NFS v2 file server; the rekordbox mode first (no privileged port 111). Its own threads, off by default, read only; a network fault never stops a deck. Amends ADR-0105 d4's "no network path". Vynull (GPL-3.0) as the reference |
| **Stems on a deck**: four stems with a level, a mute and a filter each | ADiJ | P2 | ADR-0191 d4. From NI Stems files (`.stem.mp4`: a master and up to four stereo stems, one codec, one rate; Mixxx's `soundsourcestem.cpp`) or from separation at analysis time, behind the RPC boundary and cached by BLAKE3 hash (ADR-0186 d2 keeps Demucs in Tier 2) |
| **AAC and M4A decoding**: an MP4 demuxer of our own, AAC through the platform decoders | both | P2 | ADR-0191 d4. ADI decodes WAV, AIFF, FLAC, MP3 and Ogg today; a DJ library is full of M4A, so this comes first for ADiJ. Linux's decoder is decided with the Linux DJ app |
| **Importing an NI Stems file** as a group with one track per stem, time-aligned | ADI | P2 | ADR-0191 d4. Splitting a clip into stems is already P1 (ADR-0064) |
| **Exporting stems as an NI Stems file** | ADI | P3 | ADR-0191 d4. AAC encoding from the operating system (Media Foundation, Core Audio); Linux is open |

## 11. Deliberately out of scope

Saying no now is cheaper than saying no later.

- **Notation-first workflow.** Dorico and MuseScore exist. We carry enough
  engraving data to not destroy it (P3), and stop there. Scale-aware editing,
  microtonal scales included, is in (ADR-0103); an engraving editor is not.
- **Mastering suite, spectral repair.** Plugins do this.
- **Sample library management beyond the app-scoped library index** (ADR-0104).
- ~~**Being a live-performance instrument.**~~ Reversed by ADR-0101 and
  ADR-0105: the clip launcher returns as a secondary window, built last. The
  separate **ADI Live** app was dropped by ADR-0190: live performance is ADI's
  own Session view.
  Neither is on the path to the first release.
- **Audio Units and VST2 hosting.** VST3, and CLAP when we write it. A
  project that references an AU still opens, with the device preserved as a
  bypassed placeholder — never dropped. (ADR-0041, ADR-0011)
- **Mobile.** Not until desktop is genuinely good.

---

## 12. What this tells the format

The audit above produces five gaps that must be closed before the matching
feature can be built. **Three are listed as open items in SPEC §12; two are
not** — groove templates and VariAudio-class editing appear only here, which is
exactly the kind of drift between two documents that leaves a gap owned by
neither:

1. **Chord track events** — `key_map` anchors it, chord events have no table.
2. **Expression Maps / articulations** — no schema at all.
3. **Score/engraving data** — not derivable from MIDI, no schema.
4. **Groove pool / groove templates** — shared, project-scoped, no table.
   *Not in SPEC §12.*
5. **VariAudio-class pitch-segment editing** — no model. *Not in SPEC §12.*
6. **Tuning systems and microtonal scale membership** — `scale_mask` is 12
   bits; a maqam is not a subset of 12-TET (ADR-0103). **Closed in schema 1.8**
   (ADR-0178); the other five stay on the backlog at the director's word.

None of them is P0 or P1. That is the useful result: **the v1.0 schema is
sufficient for everything in P0 and P1**, which means we can start building
without a format migration hanging over the first release.

Two small Layer 1 additions are taken while nothing has shipped, and belong in
the same schema PR: `scenes` and `clip_slots` return (ADR-0101), and `markers`
gains `note` and `prompt` (ADR-0115).

## 13. Verification — behavioural parity and the side-by-side gate (ADR-0108)

Copying the look of a reference is not the feature; the behaviour is.

- Every feature that copies Ableton, Cubase, Bitwig or REAPER names its manual
  chapter and carries a **parity checklist** written from that chapter before
  the feature: gestures, modifier keys (Alt/Cmd drags, snapping), outcomes.
- The checklist is run **side by side against a live instance of the reference
  DAW**, by the developer, and Adi signs the session. A roadmap step is
  "verified", not "done", when that has happened.
- Every deviation is a **defect** unless Adi approved it (dated, in the
  checklist) or an ADR explicitly rejects it (ADR-0072 for sends, ADR-0047 for
  the Inspector).
- Enhancements layer on top of parity, never instead of it.

## 14. Decided in the Settings review (ADR-0125 to ADR-0135)

The Settings Reference's checklist (R-01 to R-27) is ruled, and the director's
review notes added these. Status is the decision's; nothing below is built yet
unless it says so.

| Feature | From | P | Decision |
|---|---|---|---|
| Settings window: page list, Find box, scope badges, presets, bundles, Safe Mode | Live + REAPER + Cubase | P1 | ADR-0125 |
| The agent may change UI and workflow settings only, through its own logged pipeline | neither | P2 | ADR-0125 d1-d3 |
| Hebrew and RTL text; the timeline stays left-to-right | — | P1 | ADR-0125 d4 |
| LAN audio streaming node (from SonoBus), jitter buffer declared as latency | SonoBus | P2 | ADR-0126 |
| Media never embedded; **Collect and Export to ZIP** with BLAKE3 checks | — | P0 | ADR-0127 (supersedes SPEC §10.4) |
| DAWproject import and export | Bitwig | P1 | ADR-0127 d7 |
| History as a snapshot tree: named milestones, revert without loss, read-only tabs | ESXi | P1 | ADR-0128 |
| Live 12's navigation and zoom gestures, verified against the manual | Ableton | P0 | ADR-0129 |
| Per-window UI scaling; plugin windows never scaled by us | — | P1 | ADR-0129 d3-d4 |
| Master Focus Dial: hover for native controls, touch for plugins, lock, HUD, fine mode | Steinberg / REAPER + | P1 | ADR-0130 |
| Info View and delayed tooltips | Ableton | P0 | ADR-0131 |
| Remarks anchored to tracks, clips, devices, parameters; agent remarks marked | Word | P2 | ADR-0131 |
| Recording 32-bit float, WAV promoted to RF64 at 4 GiB; `bext` and iXML | Cubase | P0 | ADR-0132 d1-d2 |
| Import of every common format through one decoder, decoded into the cache | — | P1 | ADR-0132 d3-d4 |
| Import defaults: no warp, no fade (deviations from Live, approved); Warp = Auto-Warp on demand | Ableton + Bitwig | P0 | ADR-0132 d6-d7 |
| Rec-Q toggle on the MIDI track header; quantize is its own undo step | Bitwig + Ableton | P1 | ADR-0132 d8 |
| Retrospective capture: MIDI always, audio 30 s ring per armed input | Ableton + Cubase | P1 | ADR-0132 d9 |
| Control room | Cubase | **v2** | deferred, ADR-0125 d5 |
| Play-Q: live input released on the grid, late forgiveness, amber warning | — | P2 | ADR-0133 d4 |
| Inactive tabs offline; buses allocated on demand | — | P1 | ADR-0134 d1-d2 |
| AudioGridder: local fallback, placeholder, Remap Remote Host | — | P1 | ADR-0134 d3 |
| **ASIO on Windows**: built (JUCE's bundled headers, GPL-3.0); the audio probe fails CI without it; not yet heard on a real ASIO driver | all | **P0** | ADR-0134 d5, ADR-0137 |
| Plugin capabilities registry; the chosen expression route stored in the project. **Built**: the registry, the route op, the session applying it (VST3) | — | P1 | ADR-0134 d7, ADR-0149 |
| Plug-in panel exactly as Live: 64 or fewer parameters shown, more opens empty; Configure, temporary entries, recording and mapping add them (compact blocks withdrawn) | Ableton | P0 | ADR-0145 d1, ADR-0150 d1-d2 |
| Parameter List: a searchable list of every declared parameter, beside Configure. **Built** (core): `panel::search`; the panel is project state, `device.setPanel` | — | P1 | ADR-0150 d3, ADR-0154 |
| "Zoom on Selection" setting restores Live's wheel-zoom anchor | Ableton | P1 | ADR-0145 d2 |
| Sample-rate mismatch bar: Switch Hardware / Resample Temporarily, "Don't ask me again" | — | P1 | ADR-0145 d3 |
| Buffer sizes 64 to 4096 by hand; ASIO through JUCE only, no raw bypass | — | P0 | ADR-0145 d5 |
| Sample rates 44.1 kHz to 768 kHz (lower-rate files play, converted up) | — | P0 | ADR-0157 |
| AudioGridder: mDNS discovery; server plugin lists cached per application | AudioGridder | P1 | ADR-0145 d6 |
| PTP grandmaster whitelist by IP and MAC | — | P1 | ADR-0145 d7 |
| Pure Data editor inside the DAW, built on plugdata; the agent proposes, the user approves | Max for Live | P1 | ADR-0145 d8 |
| The editor marks every object the engine does not have (plugdata ships ELSE and cyclone; ADI's externals are built in) | — | P1 | ADR-0188 d8 |
| `[adi.transport $0]`: playing, BPM, time signature, bar, beat, and ticks within the quarter note (exact in Pd's 32-bit floats) | Max for Live | P1 | ADR-0188 d3 |
| Pd parameters: a range change keeps the real value; modulation evaluated by the DAW and sent once per Pd block | — | P1 | ADR-0188 d3 |
| Propose tier: a queued changeset the user applies | — | P1 | ADR-0145 d9 |
| Library metadata sync as JSON keyed by BLAKE3 hash; drives recognised by volume | — | P2 | ADR-0145 d11 |
