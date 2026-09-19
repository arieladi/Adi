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
| Markers, marker track | both | P1 | ✅ | |
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
| Scale-aware / scale-locked editing | Ableton 12 | P2 | ✅ | reads `key_map` |
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

## 5. Session View — removed

**There is no Session View.** ADR-0037 removed the clip-launching matrix from the
project: `scenes` and `clip_slots` are gone from the schema and the fourteen
`scene.*` / `session.*` ops are gone from the catalogue.

ADI is a linear, arrangement-timeline DAW. What it takes from Ableton is the
**interface** — channel strips on the right, device chain along the bottom, one
window — and what it takes from Cubase is **arrangement and audio-editing
depth**: comping, take lanes, crossfade control, the sample editor. See SPEC
§6.5.

Everything that used to be justified by "you can sketch in Session View" now has
to be earned on the timeline instead. That is the point.

## 6. Devices, racks and plugins

| Feature | From | P | Fmt | Notes |
|---|---|---|---|---|
| VST3 hosting | — | P0 | ✅ | `plugin_state.stream_role` |
| CLAP hosting | — | P0 | ✅ | same. **JUCE has no CLAP host**, so this is our code and is not the same size of job as the row above (ADR-0041). |
| LV2 (Linux) | — | P2 | ✅ | same |
| ~~AU / AUv3 (macOS)~~ | — | **no** | ✅ | **Ruled out** (ADR-0041). Not the hosting wrapper — JUCE ships one — but the registry-based discovery, `auval`, a third stream role and a macOS-only bug class around it. |
| ~~VST2~~ | — | **no** | ✅ | **Ruled out** (ADR-0015): SDK unobtainable for years, and its terms were never GPL-compatible. Not recoverable. |
| *Unhosted formats still open as placeholders* | — | **P0** | ✅ | `plugin_refs.format` still admits `au`, `auv3`, `vst2`. We do not host them; the format must still be able to say one was **there**, or a converter silently drops devices (ADR-0011, ADR-0041). |
| **Missing-plugin preservation** | both | **P0** | ✅ | SPEC §7.1 — non-negotiable |
| Racks: instrument / effect / drum | Ableton | P2 | ✅ | `device_chains` with key/vel/chain zones |
| Macros with per-target range and curve | Ableton | P2 | ✅ | `macros`, `macro_mappings` |
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

## 11. Deliberately out of scope

Saying no now is cheaper than saying no later.

- **Notation-first workflow.** Dorico and MuseScore exist. We carry enough
  engraving data to not destroy it (P3), and stop there.
- **Mastering suite, spectral repair.** Plugins do this.
- **Sample library management beyond the project media pool.**
- **Being a live-performance instrument.** There is no clip launcher
  (ADR-0037), and competing with Ableton's on-stage reliability story is a
  different project with a different engineering budget.
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

None of them is P0 or P1. That is the useful result: **the v1.0 schema is
sufficient for everything in P0 and P1**, which means we can start building
without a format migration hanging over the first release.
