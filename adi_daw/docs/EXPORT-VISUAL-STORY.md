# Feature Specification / ADR: Export Page AI Visual Story Director & Sharded Scene Telemetry Engine (`.adiviz` Bundle)

> **Status: APPROVED** by the director on 2026-10-25, as written; recorded as ADR-0205 in
> `DECISIONS.md`, which says where this hooks into the export pipeline and what priority it has.
> The text below is the director's specification, reproduced without summary or omission. One
> part did not reach the repository: the specification announces "Complete JSON Schema Examples"
> and gives **File 1** (the master manifest); the per-scene chunk file (**File 2**,
> `scenes/scene_XXX.adiviz.json`) was not in the text received and is to be supplied by the
> director; this file is amended, not rewritten, when it arrives.

- **Status:** Approved
- **Components:** Export Engine (C++ Offline Render & Stem Loop), SQLite Project Metadata, Export Page UI, Connected AI / LLM Story Director
- **Target Output:** `.adiviz/` Sharded Scene Directory Bundle (`00_master_manifest.adiviz.json` + per-scene `~8s` JSON chunks)
- **Scale Target:** 10+ minute tracks, up to 300+ channels, supporting 500+ MB of uncompressed multi-band spectrum & waveform telemetry without overflowing downstream AI context/memory limits

---

## 1. Core Architecture: Why Sharded ~8-Second Scene Chunks?

A 10-minute track with 300 channels at 30 FPS (18,000 frames) with full per-channel waveform, RMS, transient onset, and multi-band FFT spectrum data produces **500+ MB of rich JSON telemetry**.
- If exported as a single monolithic 500 MB file, generative AI models and local video pipelines crash or run out of context/VRAM.
- By slicing the 500+ MB dataset into **self-contained ~8-second scene files** (e.g., 4 or 5 bars per chunk—at 143 BPM, 1 bar = `1.6783s`, 4 bars = `6.713s`, 5 bars = `8.391s`, configurable on the Export page by bar count or seconds), each individual scene file is only **~5–8 MB**.
- Every ~8-second scene file contains its own local story prompt, continuity locks (start state from previous chunk, end state for next chunk), camera directives, and the raw per-frame DSP telemetry for just those frames—allowing any local or cloud Video AI to generate the track chunk-by-chunk with 100% detail and zero memory overflow.

---

## 2. Three Selectable Telemetry Granularity Levels (Export UI Options)

On the `adi_daw` Export Page, the user can choose which level of data density to export (or export all three into the scene chunks):

### Level 1: `aggregation_plus_heroes` (Visual Bus Aggregation + Hero Overrides)
- Aggregates the 300 channels into high-level **Visual Buses** (e.g., `vbus_low_end`, `vbus_percussion`, `vbus_leads`, `vbus_atmos`, `vbus_fx`).
- Allows the user to pin specific **Hero Channels** (e.g., `ch_042_acid_lead`) that bypass or override the bus and receive their own dedicated visual rules and telemetry stream.

### Level 2: `stems_with_featured_channels` (Audio Stem Mirroring + Flagged Child Channels)
- Directly mirrors `adi_daw`'s **Audio Stem Export** groups (e.g., `stem_drums`, `stem_bass`, `stem_synths`, `stem_pads`, `stem_fx`).
- Exports composite DSP telemetry for each stem as a whole, **PLUS** allows the user to flag specific individual child channels *inside* a stem for special high-detail extraction and dedicated visual behavior.
- *Example:* `stem_synths` (containing 45 synth tracks) drives overall background energy and color warmth, while child track `ch_042_acid_lead` inside `stem_synths` is flagged as a `featured_child` so it additionally triggers bright green neon light trails proportional to its own volume and filter cutoff.

### Level 3: `full_300ch_uncompressed` (Exhaustive Raw Channel Dump — 500+ MB Scale)
- Exports every single active channel out of all 300 channels on every frame within each ~8-second scene chunk.
- Includes full per-frame **Waveform Summary** (`rms`, `true_peak`, `crest_factor`, `stereo_width`), **7-Band FFT Spectrum Analyzer Results** (`sub_20_60`, `bass_60_250`, `low_mid_250_500`, `mid_500_2k`, `upper_mid_2k_4k`, `presence_4k_8k`, `air_8k_20k`), **Spectral Features** (`spectral_centroid`, `spectral_flux`, `pitch_hz`), and **Transient/MIDI Events** (`onset_velocity`, `active_midi_notes`).

---

## 3. Dual Video Generation Target Modes (Supported Simultaneously)

Every ~8-second `.adiviz.json` scene chunk outputs structured conditioning for **both** major AI video workflows:

### Mode A: Vanilla Generate-From-Scratch (`vanilla_full_scene_generation`)
For Text-to-Video, Audio-to-Video, and Keyframe Diffusion models generating brand-new video from scratch (Wan 2.1/2.2, HunyuanVideo, Sora, Runway, ComfyUI Prompt Travel, Deforum):
- **`scene_narrative_prompt`:** Full visual description of the environment, subjects, lighting, and motion for this 8-second window.
- **`continuity_bridge`:** Explicit description of `entry_state` (matching the final frame of Scene $N-1$) and `exit_state` (setting up Frame 0 of Scene $N+1$) so stitched 8-second clips flow seamlessly across a 10-minute video.
- **`camera_rig_trajectory`:** Pan, tilt, dolly, zoom, and shake vectors driven by low-end and riser telemetry.
- **`sub_bar_beat_cues`:** Beat-by-beat visual action keyframes inside the 8-second window.

### Mode B: Zero-Redraw Additive / Layered VFX (`zero_redraw_layered_vfx`)
For local layered compositors (Wan-Alpha, Wan2.1-VACE-1.3B, AnimateDiff + LayerDiffuse, SAM 2 + CoTracker3, TouchDesigner/Shaders) that keep an existing base video 100% untouched (never passing the base video through a VAE redraw) and composite beat-timed effects on top:
- **`isolated_layer_prompts`:** Prompts designed specifically for transparent RGBA generation or pitch-black (`#000000`) Screen/Additive blending (e.g., glowing green trails, growing mushrooms/vines, sky lightning, spore particles).
- **`compositor_bindings`:** Exact mathematical mapping between `adi_daw` channel telemetry (`rms`, `centroid`, `onset`, `fft_bands`) and layer opacity, RGB tint, glow radius, or displacement warp.
- **`lookahead_envelope`:** Enforces `envelope(frame_start) = 0.0` and `envelope(frame_end) = 0.0` (or pre-triggers growth N frames before a beat transient) so added elements appear on the beats and vanish cleanly before the section ends.

---

## 4. Export Page "Visual Director" UI & Connected AI Story Workflow

1. **User Rule Binding:** On the `adi_daw` Export Page, the user assigns natural-language visual behaviors to Buses, Stems, Featured Child Channels, or Individual Tracks:
   - *Lead Channel (`ch_042`):* "Lights green (`#00FF66`) when playing; brightness = volume (`rms`), trail width = filter cutoff (`spectral_centroid`)."
   - *Percussion Stem (`stem_percussion`):* "Triggers sky movement, cloud fractures, and upper-canopy lightning on transients (`onset`)."
   - *Kick & Bass Stem (`stem_kick_bass`):* "Drives ground pulse, subterranean shockwaves, and subtle camera zoom on `sub_20_60` peak."
2. **Connected AI Story Generation:** Before or during offline bounce, the connected AI reads the SQLite arrangement structure (markers, clip layout, energy curve across all 10 minutes) plus the user's track rules, and generates the overarching 10-minute narrative arc in `00_master_manifest.adiviz.json` and the individual 8-second scene scripts in `scenes/scene_XXX.adiviz.json`.
3. **C++ Offline Render Tap:** As the audio engine renders the master/stems offline, it buckets audio samples into video frames (24/30/60 FPS) and streams the selected granularity data directly into each 8-second scene file.

---

## 5. Complete JSON Schema Examples

### File 1: `00_master_manifest.adiviz.json` (Global Index & Story Arc)

```json
{
  "schema_version": "3.0-sharded",
  "project_meta": {
    "title": "Oscillation_Full_Live_Edit",
    "bpm": 143.0,
    "time_signature": "4/4",
    "sample_rate": 48000,
    "fps": 30,
    "total_duration_sec": 604.19,
    "total_frames": 18126,
    "total_bars": 360,
    "bar_duration_sec": 1.6783,
    "scene_chunk_bars": 5,
    "scene_chunk_duration_sec": 8.3916,
    "scene_chunk_frames": 252,
    "total_scene_files": 72,
    "total_daw_channels": 300,
    "exported_granularity_tiers": [
      "aggregation_plus_heroes",
      "stems_with_featured_channels",
      "full_300ch_uncompressed"
    ],
    "supported_render_modes": [
      "vanilla_full_scene_generation",
      "zero_redraw_layered_vfx"
    ]
  },
  "global_story_arc": {
    "theme": "Bioluminescent cyber-organic forest awakening under a reactive storm canopy",
    "acts": [
      { "act": 1, "bars": [1, 64], "label": "Atmospheric Awakening", "intensity_range": [0.1, 0.4] },
      { "act": 2, "bars": [65, 160], "label": "First Acid Build & Hypnotic Drop", "intensity_range": [0.5, 0.95] },
      { "act": 3, "bars": [161, 240], "label": "Deep Forest Breakdown", "intensity_range": [0.2, 0.6] },
      { "act": 4, "bars": [241, 360], "label": "Peak Climax & Dissolve", "intensity_range": [0.7, 1.0] }
    ]
  },
  "visual_bindings_registry": [
    {
      "target_id": "stem_kick_bass",
      "tier": "stem",
      "member_channels": ["ch_001", "ch_002", "ch_003", "ch_004"],
      "featured_children": ["ch_001_sub_kick"],
      "user_rule": "Kick and bass drive ground pulse, mycelium shockwaves, and camera impact."
    },
    {
      "target_id": "stem_synths",
      "tier": "stem",
      "member_channels": ["ch_040", "ch_041", "ch_042", "ch_043"],
      "featured_children": ["ch_042_acid_lead"],
      "user_rule": "Overall synth stem controls ambient bioluminescent warmth; featured child ch_042_acid_lead lights neon green (#00FF66) with volume intensity and expands with filter cutoff."
    },
    {
      "target_id": "stem_percussion",
      "tier": "stem",
      "member_channels": ["ch_015", "ch_016", "ch_017", "ch_018"],
      "featured_children": ["ch_018_tribal_snare"],
      "user_rule": "Percussion triggers sky movement, cloud vortex shifts, and upper-canopy lightning."
    }
  ],
  "scene_index": [
    {
      "scene_id": "scene_014",
      "file": "scenes/scene_014_bars_066_070.adiviz.json",
      "bar_start": 66,
      "bar_end": 70,
      "time_start_sec": 109.09,
      "time_end_sec": 117.48,
      "frame_start": 3273,
      "frame_end": 3524
    }
  ]
}
```

### File 2: `scenes/scene_XXX.adiviz.json` (one ~8-second scene chunk)

*Not received with the specification on 2026-10-25. To be supplied by the director; the fields it
must carry are named in sections 2 and 3 above (the per-frame telemetry of the chosen tier, the
Mode A conditioning: `scene_narrative_prompt`, `continuity_bridge`, `camera_rig_trajectory`,
`sub_bar_beat_cues`; the Mode B conditioning: `isolated_layer_prompts`, `compositor_bindings`,
`lookahead_envelope`).*

---

## Where this hooks into ADI's render pipeline (win, 2026-10-25; the ruling is ADR-0205)

What exists on `main` today, read off the code:

- **One offline render, of the master.** `adi_play --render SECONDS --render-output file.wav`
  (`src/juce/play.cpp`, `renderOffline`) drives `Session::process(AudioIo)` block by block with no
  audio device (ADR-0151), primes the clip reader between blocks (only an offline driver may wait
  for the disk), and captures the **master** output into one 32-bit float WAV (`writeRender`, via a
  temporary file so a failure leaves the previous file untouched). It prints the peak per second
  and `--require-peak` proves audio came out. There is no per-track or per-group file writer yet.
- **An isolated offline analysis pass** exists: `Session::autoGainStage` (ADR-0195 d4) renders the
  project offline without touching the live transport or plug-in instances, measures every track,
  and commits one transaction. It is the pattern an export job runner follows.
- **Per-channel taps, already in the graph.** Every strip carries a `ScopeTap` after the fader and a
  pre-fader tap (`src/adi/engine/scope.*`, `mixer.hpp`; ADR-0175): a wait-free ring, one writer,
  any reader, **stamped with the time each frame is heard**, so a channel's telemetry lines up with
  the master's timeline whatever latency its plug-ins add. `Session::openScope(track, seconds)`
  opens one on any track or the master. `dsp::TruePeakMeter` (BS.1770-4) reads them. The analyser
  (ADR-0183, `dsp/spectrum.*`, the published arrays of `published_array.hpp`) and its multi-track
  overlay subscribe to the same taps, before or after the inserts, with the FFT on a worker thread
  and only visible strips subscribed (ADR-0184 d4).
- **Routing** is the planner's (`src/adi/engine/plan.*`): tracks into groups into the master, group
  summing (ADR-0174), track delay (ADR-0172), Live's pan law (ADR-0163); sends and returns are
  back as Live and **do not play yet** (ADR-0203 d3, phase A of `BUILD-BRIEF.md`).
- **Stem export is designed, not built.** ADR-0071: an export is a queue of jobs (source: track,
  group, region, master; range; through the master or not; wet or dry; format; a name template),
  rendered on the offline graph, never on the audio thread; the AI fills the form and **the user
  presses render**, because files on disk have no undo. The render matrix (ADR-0203 d14, D-07) is
  another way to fill the same queue. Freeze (ADR-0059, ADR-0203 d16) renders a node or a group
  subtree to a file and swaps a reader in; "Freeze All Channels" renders every child separately.
  The Export settings page (PR #215) holds the defaults: file type, bit depth, dither, the name
  template `{project} - {track}`, the loudness target. The LUFS targeting of ADR-0203 d14 is the
  export's loudness stage. All of it is P1/P2 and unbuilt.
- **Naming:** ADR-0071 wrote wildcards as `$track_$group_$bpm`; the registry's template uses
  `{project} - {track}`. One syntax is kept when the queue is built (win: the braces, as the
  registry has them), and wildcards resolve at render time (ADR-0071 d5).

How the Visual Story Director uses exactly that pipeline:

1. **The telemetry is read from the taps, inside the export job's offline loop.** The job runner of
   ADR-0071 is `renderOffline`'s block loop generalised: per block, each selected source's
   `ScopeTap` (post-fader for a stem, pre-fader where a rule asks for it) is read for the block's
   stamped frames, bucketed into video frames (`sample_rate / fps` samples, 1600 at 48 kHz and 30
   FPS), and the per-frame fields of Level 3 are computed from the bucket: `rms`, `true_peak`
   (`TruePeakMeter`), `crest_factor`, `stereo_width`, the seven bands (a fixed filterbank or
   `dsp/spectrum`'s STFT binned to the seven ranges), `spectral_centroid`, `spectral_flux`,
   `pitch_hz`, `onset_velocity` (the hitpoint detector of ADR-0176 d4), `active_midi_notes` (the
   MIDI clip player's held notes, ADR-0158's `heldNotes()`). Offline, so the cost of 300 channels
   is time, not glitches.
2. **The three tiers are three selections over the same taps.** Level 2's stems are the export
   queue's own sources (the groups, tracks and master the user ticked in the render matrix);
   Level 1's Visual Buses are telemetry-only sums of member taps and need no audio routing;
   featured children and heroes are the same tap read twice. Nothing is rendered twice: the audio
   stems and the telemetry come out of one pass.
3. **The scene files are written as the pass crosses bar boundaries.** The tempo map gives the
   chunk's bars; the chunk closes when the pass passes its last frame, so a 500 MB job never holds
   more than one scene in memory.
4. **The story director is the agent** (ADR-0168, the Propose tier): before the pass it reads the
   project through the agent's view of it (the projection and the ops, as every agent read does;
   ADR-0010: nothing reads the SQLite file from the render) — markers, clips, the energy curve,
   the user's rules bound on the Export page — and writes `00_master_manifest.adiviz.json` and the
   per-scene scripts; the pass then fills the telemetry into the scene files it indexed. The user
   presses render (ADR-0071 d3); the manifest is reviewable before the pass, as a queue is.
5. **The user's rule bindings** are Export-page state on the job (ADR-0071 d1: a job is a value),
   stored with the project's export presets, never a project op (they change no sound).
