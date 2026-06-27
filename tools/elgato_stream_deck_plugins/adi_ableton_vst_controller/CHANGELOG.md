# Changelog

## [1.5.5.0] — 2026-06-20

### Added
- **New predefined controller: Omnipressor** (Eventide dynamics — expander / gate
  / compressor / limiter, VST3/AU). 16 params, paged like Blackhole:
  - MAIN: Threshold · Attack · Release · Function (EXP↔COMP ratio) · Atten Limit ·
    Gain Limit
  - I/O: Input Gain · Output Gain · In Level · Out Level · Mix · Function
  A full-width bottom bar holds the five switches — **Bass** (Norm/Cut) · **Meter**
  (Input/Gain/Output, cycles) · **Sidechain** · **Line** (In/Out) · **Power**.
  Params resolve by name; values via Ableton's `str_for_value`. Registered by
  device name (`/omnipressor/i`). See docs/OMNIPRESSOR.md.

### Changed
- **`AVC.showVal` now passes through ratio strings** (e.g. `1:1`, `2:1`) in
  addition to unit/label values, so Omnipressor's Function (and any ratio param)
  mirrors Ableton exactly instead of falling back to a raw number. Additive — plain
  numbers still fall back and unit strings are unchanged, so no effect on the other
  controllers.

## [1.5.4.0] — 2026-06-20

### Added
- **New predefined controller: dBComp** (Analog Obsession compressor/limiter,
  VST3/AU). Fixed layout: dials 1-5 = **Threshold · Compression · Output Gain ·
  HPF · Mix**; zone 6 holds the two switches — **Oversampling** (scroll dial 6 /
  tap top) and **Bypass** (press dial 6 / tap bottom). Params resolve by name;
  values via Ableton's `str_for_value`. The unused `Parameter #6/#7` placeholders
  and Ableton's wrapper Gain/Sidechain are not mapped. Registered by device name
  (`/\bd[bB]\s*comp\b/i`). See docs/DBCOMP.md.

## [1.5.3.0] — 2026-06-20

### Added
- **New predefined controller: H-Delay** (Waves Hybrid Line delay; Stereo /
  Mono-Stereo / Mono variants). Fixed 6-dial layout (no paging) over the small
  Configured param set: **Mix · Delay BPM (note division) · Feedback · HiPass ·
  LoPass · PingPong (routing mode)**. Continuous params nudge (`delta_index`);
  Delay and PingPong are stepped — turn the dial, press it, or tap the zone to
  cycle (shift/right = previous). Values via Ableton's `str_for_value`. Registered
  by device name (`/\bh[-\s]?delay\b/i`). See docs/H_DELAY.md.

## [1.5.2.0] — 2026-06-20

### Added
- **New predefined controller: Blackhole** (Eventide, H9 series VST3/AU reverb).
  Paged 6 dials across **MAIN / MOD** (tap tabs or press a dial to advance):
  - MAIN: Mix · Gravity · Size · Predelay · Low (EQ) · Hi (EQ)
  - MOD: Mod Depth · Mod Rate · Feedback · Resonance · In Level · Out Level
  A full-width bottom bar holds Blackhole's signature switches — **Kill · Freeze ·
  HotSwitch** (tap to toggle) and **TempoSync** (tap to cycle Manual/Sync/Off).
  Params resolve by name (mix/gravity/size/predelay/low level/hi level/mod depth/
  mod rate/feedback/resonance/in level/out level + the four switches); values via
  Ableton's `str_for_value`. Ribbon Controller + Tempo left to the GUI. Registered
  by device name (`/\bblackhole\b/i`). See docs/BLACKHOLE.md.

## [1.5.1.0] — 2026-06-20

### Added
- **New predefined controller: ValhallaVintageVerb** (Valhalla DSP, VST3 reverb).
  Same paged design as ValhallaRoom; the 6 dials page across **MAIN / DAMP /
  SHAPE** (tap tabs or press a dial to advance):
  - MAIN: Mix · Predelay · Decay · Size · High Cut · Low Cut
  - DAMP: High Freq · High Shelf · Bass Xover · Bass Mult · Decay · Mix
  - SHAPE: Attack · Early Diffusion · Late Diffusion · Mod Rate · Mod Depth · Size
  The bottom bar holds the two selectors — Reverb Mode (`ReverbMode`, the
  algorithm) and Color Mode (`ColorMode`, the era voicing). Params resolve by name
  (mix/predelay/decay/size/attack/highfreq/highshelf/bassxover/bassmult/
  earlydiffusion/latediffusion/modrate/moddepth/highcut/lowcut + the two modes);
  values shown via Ableton's `str_for_value`. Registered by device name
  (`/vintage\s*verb/i`, no collision with ValhallaRoom). See
  docs/VALHALLA_VINTAGE_VERB.md.

## [1.5.0.0] — 2026-06-20

### Added
- **New predefined controller: ValhallaRoom** (Valhalla DSP, VST3 reverb) — the
  first non-EQ controller. The 6 dials are **paged** (tap MAIN / EARLY / LATE / RT
  tabs, or press a dial to advance):
  - MAIN: Mix · Predelay · Decay · High Cut · Diffusion · Early/Late Mix
  - EARLY: Early Size/Cross/Mod Rate/Mod Depth/Send · Mix
  - LATE: Late Size/Cross/Mod Rate/Mod Depth · Decay · Mix
  - RT: Bass Mult/Xover · High Mult/Xover · Decay · Mix
  A full-width bottom bar holds Reverb Mode (`type`, cycles the algorithm) and a
  Preset stepper (graceful "— (not exposed)" when the build has no preset param).
  Params resolve by name (mix/predelay/decay/HighCut/diffusion/earlyLateMix/early*/
  late*/RTBassMultiply/RTXover/RTHighMultiply/RTHighXover/type); values shown via
  Ableton's `str_for_value`. Registered in registry.js by device name
  (`/valhalla\s*room/i`). See docs/VALHALLA_ROOM.md.

## [1.4.5.0] — 2026-06-20

### Changed
- **Spectre: verified parameter names + full per-band control.** Rewrote the
  controller around the real Ableton Configure names — the 5 named bands
  (LowShelf / Peak 01 / Peak 02 / Peak 03 / HighShelf), each with `Frequency`,
  `Gain`, `Q`, `Switch`, `Color`, `Processing`; globals `Output`, `Dry Wet` (Mix),
  `Mode`. (The old controller guessed `Band N …`, a non-existent per-band Shape,
  and treated Color/Processing as globals.)
- **Strip-wide GAIN / FREQ / Q dial modes** — dials 1-5 adjust the active mode's
  param for the 5 bands; dial 6 = Output (press = cycle Mode). Per band: dial
  press = Switch (on/off), tap = cycle Color / Processing. Zone 6 = Mode + Mix.
  Fixed shape glyphs (shelf/bell/bell/bell/shelf); all values via Ableton's
  `str_for_value`. See docs/SPECTRE.md.

## [1.4.4.0] — 2026-06-20

### Changed
- **Pulsar Massive: verified parameter names + full per-band control (A-channel).**
  Replaced the guessed names with the real Ableton Configure names, anchored to the
  `A` suffix so the B parameter is never matched: per band (Low / Warmth / Presence
  / Air) `Band N Gain A`, `Band N Freq A`, `Band N Bandwidth A`, `Band N Active A`,
  `Band N Type A`; centre `Drive A`, `Gain A`, `Low Pass Freq A`, `High Pass Freq A`,
  `Auto Gain`, `Transformer`. The B channel, Stereo Mode and ChannelA Active are
  intentionally not mapped (stereo-linked workflow).
- **Strip-wide GAIN / FREQ / WIDTH dial modes** (tap the tabs) — dials 1-4 adjust
  the active mode's param for the 4 bands, adding the previously-missing **Bandwidth**
  control. Dial 5 = Drive, dial 6 = channel Gain. Per band: tap = IN/OUT and
  Bell/Shelf; dial press = IN/OUT. Zone 5 = Auto Gain + Low Pass; zone 6 = Transformer
  (Off/1/2) + High Pass. All values shown via Ableton's `str_for_value` string. See
  docs/PULSAR_MASSIVE.md.

## [1.4.3.0] — 2026-06-20

### Added
- **EQ Eight: full per-band control + global Output Gain & Scale.** The 6 dials
  now have strip-wide **FREQ / GAIN / Q** modes (tap the top tabs), driving the
  focused 6-band window (◀ ▶ paginate 1-6 → 2-7 → 3-8). A new **GLOB** mode puts
  Output Gain on dial 1 and Scale on dial 2, with the summed frequency-response
  graph filling the rest of the strip. Per band: tap = enable / cycle filter
  type; dial press = enable.
- New bridge commands `eq8_gain_delta`, `eq8_q_delta`, `eq8_global_delta` and an
  `eq8_globals` state message (Output Gain + Scale), plus per-band `freq_disp` /
  `gain_disp` / `q_disp` strings. See docs/EQ8.md.

### Changed
- **EQ Eight values now mirror Ableton exactly** — every reading (band Freq/Gain/
  Q, Output Gain, Scale) is shown via Live's own `str_for_value` string through
  `AVC.showVal`, replacing the controller's reinvented Hz formatting.
- Verified the EQ Eight parameter names against a real Ableton Configure view
  (`<N> Frequency/Gain/Resonance/Filter Type/Filter On A`, `Output Gain`, `Scale`);
  the band-name resolution (`_BAND_RE`) was already correct. The dial focus window
  now resets to band 1 when a different EQ Eight is selected.

## [1.4.2.0] — 2026-06-18

### Added
- **Pro-Q 3: real Shape / Slope / Stereo Placement switches** on every band (tap
  to cycle the plugin's actual option lists), now that those parameters are
  exposed via Ableton's Configure. Bands 1 & 6 restored (Freq + switches).
- **Shape-aware dial modes** — the dial's FREQ/GAIN/Q tabs follow each band's
  current Shape: no Gain for Low Cut/High Cut/Notch/Band Pass; no Q for Low/High
  Cut, Low/High Shelf, Tilt Shelf, Flat Tilt — updating live as the Shape changes.
  See docs/PROQ3.md.

## [1.4.1.0] — 2026-06-18

### Fixed
- **Pro-Q 3 now matches Ableton's default device exposure** — only Frequency /
  Gain / Q per band, and the cut bands (1 = low cut, 6 = high cut) expose no
  Gain. ProQ3Controller was rewritten around that (dial mode adapts: FREQ/GAIN/Q
  for bells, FREQ/Q for cuts) and the Shape/Slope/Stereo/enable controls that
  Live doesn't expose by default were removed. Verified against a live instance.
- **Touchscreen values now mirror Ableton exactly.** The bridge formats each
  parameter with Live's `DeviceParameter.str_for_value()` (e.g. "47.924 Hz",
  "0.00 dB", "Bell"); all controllers display that string verbatim, using their
  own numeric format only as a fallback. Fixes potentially wrong units for VST3
  parameters Live reports as normalized 0..1.

## [1.4.0.0] — 2026-06-18

### Added
- **IndeqController** — predefined strategy for Analog Obsession INDEQ (VST3),
  a fixed 3-band EQ. 6 dials = Low/Mid/High Gain, Low/Mid Freq (stepped) and
  Output; 6 touch toggles = Highpass Filter, Low/High Band Shape, Mid Bandwidth,
  High Frequency (8/16k) and Bypass, with each toggle's label drawn from the
  plugin's own value list. Resolves VST3 params by name with an overridable map;
  see docs/INDEQ.md.
- INDEQ mode added to the browser demo.

## [1.3.0.0] — 2026-06-18

### Added
- **SpectreController** — predefined strategy for Wavesfactory Spectre (VST3),
  a fixed 5-band enhancer. Dials 1-5 drive bands 1-5, each with an independent
  FREQ/GAIN dial mode; **dial 6 is a dynamic Q** that follows the last-touched
  band (`activeBand`). Touchscreen per band: shape (tap-cycle), stacked
  Freq/Gain/Q with the active mode highlighted, and one global setting anchored
  per column (Quality, Color, Presets, Mode, Processing); zone 6 shows the Q
  target + value and a Bypass toggle. Resolves VST3 params by name with an
  overridable map; see docs/SPECTRE.md.
- Spectre mode added to the browser demo.

## [1.2.0.0] — 2026-06-18

### Added
- **ProQ3Controller** — predefined strategy for FabFilter Pro-Q 3 (VST3),
  assuming a static 6-band preset (band 1 Low Cut, band 6 High Cut, bands 2-5
  bells). **Multi-functional dials**: each column has an independent dial mode
  cycling FREQ/GAIN/Q. 6-band touchscreen per column: power, dial-mode selector,
  live value, Shape | Slope cycles, and Stereo Placement. Resolves VST3
  parameters by name with an overridable map; see docs/PROQ3.md.
- **`delta_log_index`** bridge command — geometric (musical) parameter nudge for
  log-perceived params like frequency and Q; general/reusable by any controller.
- Pro-Q 3 mode added to the browser demo.

## [1.1.0.0] — 2026-06-18

### Added
- **PulsarMassiveController** — predefined strategy for Pulsar Audio "Pulsar
  Massive" (MP.EQ), a stereo-linked VST3. Maps the 6 dials to 4 band gains +
  Master Drive + Master Gain (Left channel only), with a 6-zone touchscreen:
  per-band IN / Shelf-Bell toggles + stepped frequency, Auto Gain + Low Pass,
  and a Transfo (1/OFF/2) cycle + High Pass. Resolves VST3 parameters by name
  with an overridable map; see docs/PULSAR_MASSIVE.md.
- **Named-parameter bridge channel** (general, reusable by any predefined VST):
  `get_all_params`/`all_params`, `watch`/`p`, and index-addressed
  `set_index`/`delta_index`/`step_index`/`toggle_index`.
- Registry now resolves VST/AU plugins by **device name** (they all report
  class_name "PluginDevice"); patterns avoid catching NI's "Massive" synth.
- Pulsar Massive mode added to the browser demo.

## [1.0.0.0] — 2026-06-17

### Added
- Initial release of **Adi Ableton VST Controller** (Stream Deck device type 13:
  36 keys / 6 dials / touchscreen; also runs on Stream Deck +).
- Real-time tracking of the selected Ableton track and device.
- **Generic mode**: first 6 non-quantized parameters → 6 dials; touchscreen
  6-zone layout. Works on native devices and external VST2/VST3/AU.
- **EQ Eight mode**: split-screen touchscreen (response graph + per-band controls
  with ◀ ▶ pagination), 6 dials control band frequencies, dial-press toggles a
  band, pagination shifts the band-focus window.
- **EQ8 key** with context logic A/B/C (next / closest / create instance) and a
  long-press **preset folder** on the 36 keys (short = load onto current EQ8,
  long = new instance with preset).
- Modular **Strategy-pattern** controllers (`DeviceController` base) with a
  registry resolving by Live `class_name`.
- **AdiVST Python Remote Script** bridge: stdlib-only RFC 6455 WebSocket server,
  thread-safe main-thread command pump, full LOM tracking + EQ8/browser logic.
- Property Inspector (key roles, dial slots, bridge port), hardware-free browser
  demo, icon generator, manifest/asset/Python validator, and cross-platform
  install scripts (macOS + Windows) for both the plugin and the Remote Script.
