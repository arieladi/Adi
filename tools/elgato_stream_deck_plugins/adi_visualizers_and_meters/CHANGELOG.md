# Changelog

All notable changes to this project are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versioning is the four-part
scheme Stream Deck expects in the manifest (`MAJOR.MINOR.PATCH.BUILD`).

## [1.0.0.0] — 2026-06-17

### Added
- Initial release of **Adi Visualizers & Meters**.
- Single **Audio View** action usable on Stream Deck + dials (Encoder) or any
  key (Keypad).
- Eight live views from one shared stereo capture: spectrum analyzer,
  oscilloscope (with trigger), waveform, peak/RMS meters with peak-hold,
  ISO octave bands, goniometer (vectorscope), stereo correlation and balance.
- Press / touch / key-press cycles the view; the dial adjusts a per-view
  parameter; long-touch resets the view. All state persists per action.
- Property Inspector exposing window, block size, overlap, averaging, slope,
  frequency/dB ranges, colors, channel, trigger and more, plus a shared
  refresh-rate and input-device selector.
- Hardware-free browser demo (`demo/`) sharing the exact analysis/drawing
  engine used by the plugin.
- Cross-platform tooling: icon generator, manifest/asset validator, install
  scripts and DistributionTool packaging scripts for macOS and Windows.
