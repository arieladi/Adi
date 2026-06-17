# Changelog

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
