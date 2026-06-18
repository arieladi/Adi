# Changelog

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
