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
| Chord track, chord pads, harmonic linking | Cubase | P3 | 🔶 | `key_map` is the anchor; chord events need their own table |
| Timecode, video sync, pull-up/down | Cubase | P3 | 🔶 | `project.frame_rate_*` exists; video handling unspecified |
| Tempo detection from audio | both | P2 | ✅ | writes `tempo_map` |

## 2. Tracks, routing and mixing

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Audio / MIDI / instrument tracks | both | P0 | ✅ | |
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
| ADI virtual audio device out (master or any bus as an OS input) | REAPER (ReaRoute) | P2 | — | an OS driver: BlackHole fork on macOS; on Windows our own `sysvad`-based (MIT) kernel driver signed for free through SignPath Foundation, the route Virtual-Audio-Driver has used since 2025; bundling VB-CABLE was proposed and rejected (ADR-0106, ADR-0117, ADR-0118) |
| Track freeze / bounce in place | both | P1 | ✅ | `tracks.frozen`, `freeze_media_id` |
| Control Room (separate monitor path, cue mixes, talkback) | Cubase | P3 | 🔶 | `routing.kind='cue'` reserved; no monitor-section model |
| Crossfader, DJ-style mixing | Ableton | P3 | 🔶 | needs a master-section table |
| Channel strip (gate/comp/EQ built in) | Cubase | P2 | ✅ | internal `devices` |
| Mixer snapshots | Cubase | P2 | ✅ | `snapshots.kind='mixer'` |
| Per-channel delay compensation offset | both | P1 | ✅ | `mixer_strip.delay_samples` |

## 3. Clips, editing and comping

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Clip loop window separate from placement | Ableton | P0 | ✅ | one object, not N repeats (SPEC §6.2) |
| Fades, crossfades, fade curves | both | P0 | ✅ | |
| Take lanes and comping | both | P1 | ✅ | `lanes` |
| Linked / aliased clips (edit one, change all) | both | P2 | ✅ | `clips.alias_of` |
| Track versions | Cubase | P2 | ✅ | `snapshots.kind='track_version'` |
| Ripple edit / insert-delete time | both | P1 | — | pure op, no schema needed |
| Warp / elastic audio | Ableton | P1 | ✅ | `audio_clips.warp_markers` (AWRP) |
| Hitpoints, slice to track | Cubase | P2 | ✅ | hitpoints are warp markers |
| AudioWarp quantise / groove from audio | Cubase | P2 | 🔶 | groove templates need a table |
| Groove pool | Ableton | P2 | ❌ | shared groove templates — no schema |
| VariAudio / pitch-time editing of vocals | Cubase | P3 | ❌ | per-segment pitch model; big, unspecified |
| ARA2 (Melodyne, SpectraLayers) | Cubase | P3 | 🔶 | ARA state lives in `plugin_state`; doc model needed |

## 4. MIDI and expression

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Piano roll, velocity, CC lanes | both | P0 | ✅ | `event_streams` |
| Quantise, groove, swing | both | P0 | ✅ | |
| Per-note probability | Ableton | P2 | ✅ | in the v1 note record |
| Per-note microtuning | neither, fully | P2 | ✅ | `tuning_cents` in the v1 note record |
| **Per-note expression / MPE** | both, partially | **P1** | ✅ | `note_expression` — first-class, SPEC §6.3.2 |
| **MPE+ (Haken), 14-bit Y and Z at 500 Hz** | neither | **P1** | ✅ | `ExpressionPoint.value` is f32, so bit depth was never the constraint. The binding constraint is ADR-0042's sub-block floor, which MUST NOT exceed `sample_rate/500` (ADR-0054). |
| **MPE out to plugins, VST3 and CLAP** | both, partially | **P1** | — | CLAP (ADR-0099): the dialect the plugin declares -- CLAP note expression, MIDI-MPE or MIDI. VST3 (ADR-0097): per plugin, VST3 note expression, MPE over MIDI on member channels, or plain MIDI with poly aftertouch. The controller's channel never reaches a plugin. Pitch, pressure and timbre measured by ear per route (ADR-0098, ADR-0100); a fixture VST3 exercises the IMidiMapping parameter path in CI. Surge XT reads MpeMidi, Serum 2 reads note expression, and `Auto` can only be right for one of them -- so the route choice must be remembered, which it is not yet. |
| Scale-aware / scale-locked editing, **every scale including Arabic and microtonal** | Ableton 12 + | P2 | 🔶 | reads `key_map`; a 12-bit `scale_mask` cannot name a quarter tone, so `tuning_systems` + `tuning_degrees` + `key_map_degrees` child tables come first (ADR-0103, ADR-0117, gap 6). Notation stays out. |
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
| Live performance | — | **ADI Live app** | — | a separate product on the same engine, after the DAW (ADR-0105) |

ADI is still an arrangement-first DAW: the timeline is the default screen and the
first thing built, and what it takes from Cubase is **arrangement and
audio-editing depth**. Sketching has to be earned on the timeline *first*; the
Session window is the last step, not a way around that.

## 6. Devices, racks and plugins

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| VST3 hosting | — | P0 | ✅ | `plugin_state.stream_role` |
| CLAP hosting | — | **P0, mandated** | ✅ | same. **JUCE has no CLAP host**, so this is our code (ADR-0041). Level with VST3 and sequenced after it — CLAP's `PARAM_MOD` is ADR-0046's rule expressed in a plugin API, so the modulation architecture depends on it (ADR-0052). |
| LV2 (Linux) | — | P2 | ✅ | same |
| ~~AU / AUv3 (macOS)~~ | — | **no** | ✅ | **Ruled out** (ADR-0041). Not the hosting wrapper — JUCE ships one — but the registry-based discovery, `auval`, a third stream role and a macOS-only bug class around it. |
| ~~VST2~~ | — | **no** | ✅ | **Ruled out** (ADR-0015): SDK unobtainable for years, and its terms were never GPL-compatible. Not recoverable. |
| *Unhosted formats still open as placeholders* | — | **P0** | ✅ | `plugin_refs.format` still admits `au`, `auv3`, `vst2`. We do not host them; the format must still be able to say one was **there**, or a converter silently drops devices (ADR-0011, ADR-0041). |
| **Missing-plugin preservation** | both | **P0** | ✅ | SPEC §7.1 — non-negotiable |
| **Plugin parameter edits inside the plugin's window are ops** (global Ctrl-Z) | neither | **P0** | ✅ | one gesture, one op; chunk snapshots for what is not a parameter (ADR-0110, amends ADR-0038) |
| Racks: instrument / effect / drum | Ableton | P2 | ✅ | `device_chains` with key/vel/chain zones |
| Macros with per-target range and **multi-breakpoint curve**, several mappings per target, per-macro enable | Ableton + | P2 | 🔶 | `macros`, `macro_mappings`; `curve` becomes a breakpoint BLOB (ADR-0114) |
| Cross-track modulation and macro targets | Bitwig | P2 | 🔶 | arrives with the modulation schema (ADR-0114, ADR-0046) |
| A Pd device with a second, floating view (the analyser) | neither | P2 | — | published arrays in the device contract (ADR-0116) |
| Plugin delay compensation | both | P0 | ✅ | `devices.latency_samples`. Reported in samples and **excludes** the device buffer — ADR-0042. |
| 2048–4096-sample blocks, tested | — | **P0** | — | runtime. Dense chains, not low-latency tracking (ADR-0042). **4096 is the cap on every platform** and the granted size is the only one that exists (ADR-0049). |
| Sub-block automation and MIDI accuracy | both | **P0** | ✅ | runtime. At 8192 a block is 171 ms; per-block updates would step audibly. The price of the row above (ADR-0042). |
| Change block size without reloading | both | **P0** | — | runtime. At 8192 overdubbing is impossible, so moving between sizes mid-session is not optional (ADR-0042). |
| VST3 silence flags + tail-aware suspension | — | **P0** | ✅ | runtime; `devices.always_process` opts out (ADR-0043) |
| Host-side modulation to any VST3 parameter | Bitwig | P1 | — | routing persists, output never does (ADR-0046) |
| ~~Plugin sandboxing (crash isolation)~~ | Cubase | **no** | — | **Rejected** (ADR-0047). Not for IPC cost — that amortises over a 171 ms block and is cheapest here — but for latency and complexity. Affordable because a crash loses one gesture, not the session (ADR-0001). |
| ~~Dedicated Inspector panel~~ | Cubase | **no** | — | **Rejected** (ADR-0047). Mixer strip, device chain and detail editor must carry every property between step 7 and step 9, and must be keyboard-reachable. |

## 7. Automation

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| Track and device automation | both | P0 | ✅ | `automation_lanes` |
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
| Collect & Embed / Extract Media | both | P1 | ✅ | `media_blobs`, SPEC §10.4 |
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
| Undo that survives a reboot | P1 | ✅ | |
| Undo you can *branch*, so exploring costs nothing | P2 | ✅ | |
| Text projection for version control | P2 | — | ADR-0007 |
| Real-time collaboration | P3 | 🔶 | op log is the substrate; model undecided |
| Scripting API identical to the agent's op vocabulary | P2 | ✅ | one API, not two |

---

## 10.5 Native DSP nodes, and asynchronous AI

Two blocks of the director's blueprint (2026-09-20). The **decisions** are
ADR-0058 to ADR-0064; this is the backlog they govern.

### Native DSP nodes (ADR-0062)

Built into the graph rather than hosted, for transport access, the full-
resolution event stream, and sidechain-as-an-edge — **not** for zero latency,
which is a property of the algorithm and not of where it is compiled.

| Node | P | Latency | Notes |
|---|---|---|---|
| Sub-sample phase utility | P1 | polarity **0**, nudge ~0 | polarity inversion is exactly free; a fractional delay is not |
| Audio-rate envelope follower | P1 | 0 | a modulator under ADR-0046; first real consumer of ADR-0052's `PARAM_MOD` problem |
| Grid-locked volume shaper | P1 | 0 | reads the tempo map directly — the reason it is native |
| Frequency shifter | P2 | Hilbert transform is not free | ring-mod / Hilbert, sample-accurate linear shift |
| Vocoder | P2 | filter-bank dependent | sidechain via `Bus::Sidechain`, no user wiring |
| Multiband graph splitter | P2 | **thousands of samples in linear phase** | **blocked on N-bus outputs** (ADR-0056). Declares its latency; a minimum-phase mode is a user choice, not a silent default |

### The DSP plugin line — future (ADR-0093)

Prepared for, not started. References are fetched into `reference/` now; no code
exists. Working titles only — shipped names are ours (ADR-0093 d4).

| Goal | Form | P | Needs first |
|---|---|---|---|
| Dynamic EQ: matched phase, linear phase, per-band dynamics | CLAP | P3 | Plugin-line licence decided; matched phase from Vicanek (2016), not the AGPL reference |
| True-peak limiter: lookahead, oversampling, modes | CLAP | P3 | Plugin-line licence decided (the LSP maths is LGPL) |
| Lookahead brickwall limiter, 1.5 / 3 / 6 ms | Pd | P3 | **A Pd patch able to declare its latency** (ADR-0035 has no such thing), and pinned DSP sort order |
| Eight-band parametric EQ | Pd | P3 | Pd's inverted `biquad~` feedback signs; four biquads for a 48 dB/oct cut |
| ADAA clipper, adjustable knee, up to 4x oversampling | CLAP | P3 | Plugin-line licence decided (the chowdsp waveshapers are GPLv3) |
| Ring-modulation sidechain ducker (RMSC) | Pd | P3 | Judged as amplitude modulation, which is what it is |

Four of the six carry latency, which makes them the first plugins able to test
ADR-0079/0085/0092 with source we can read instead of borrowing FabFilter's.

### Time-stretch (ADR-0061)

| Engine | P | Job |
|---|---|---|
| Bungee (MPL-2.0, pinned) | P1 | scrubbing, varispeed, zero and negative speed |
| Rubber Band (GPL-2.0-**or-later**) | P1 | high-quality warp and pitch-shift; licence to verify under ADR-0024 before use |

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

## 11. Deliberately out of scope

Saying no now is cheaper than saying no later.

- **Notation-first workflow.** Dorico and MuseScore exist. We carry enough
  engraving data to not destroy it (P3), and stop there. Scale-aware editing,
  microtonal scales included, is in (ADR-0103); an engraving editor is not.
- **Mastering suite, spectral repair.** Plugins do this.
- **Sample library management beyond the app-scoped library index** (ADR-0104).
- ~~**Being a live-performance instrument.**~~ Reversed by ADR-0101 and
  ADR-0105: the clip launcher returns as a secondary window, built last, and
  live performance is the **ADI Live** app on the same engine, after the DAW.
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
   bits; a maqam is not a subset of 12-TET (ADR-0103). *Not in SPEC §12.*

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
