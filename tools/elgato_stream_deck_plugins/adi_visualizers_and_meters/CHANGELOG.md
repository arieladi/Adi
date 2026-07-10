# Changelog

All notable changes to this project are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versioning is the four-part
scheme Stream Deck expects in the manifest (`MAJOR.MINOR.PATCH.BUILD`).

## [1.2.0.0] — 2026-07-05

### Added
- **Tap readout on every view** (touch strip). The v1.1.0.0 SPAN-style
  spectrum readout now has per-view siblings, all with the same interaction
  (tap = place/move, tap-and-hold = clear or reset, dial press = cycle
  views, auto-hide after the view's Readout hold seconds):
  - **Scope**: time from trigger + equivalent period frequency + note (tap
    one cycle into the wave to read its pitch) + level at that instant,
    with the marker dot riding the trace.
  - **Waveform**: how far back that moment is (e.g. `-380ms`) + the
    column's peak level.
  - **Octave bands**: tapped band's center frequency, nearest note and live
    L/R levels; the band's slot is highlighted.
  - **Meters**: exact numbers for the tapped channel (`L rms -12.4 pk
    -6.0dB`).
  - **Goniometer**: live correlation + balance numbers.
  - **Correlation / Balance**: the exact value the needle points at.
- New PI options: Tuning A4 + Readout hold on scope and bands, Readout hold
  on waveform; notes on the other views document the tap.
- Engine: shared `drawReadout`/`drawMarkerDot` helpers, `fmtNote`/`fmtBal`
  exports, and an `AVM._ringPush` test hook for injecting synthetic PCM
  (hardware-free verification of scope/waveform readouts).
- Verified: extended `scripts/test_readout.mjs` (scope period→A3, band
  center notes, formatting helpers) + a six-panel visual render with
  injected PCM/meter values — every readout matched the injected ground
  truth (220 Hz sine → `4.55ms 220Hz A3 ±0¢`; meters → `L rms -12.4 pk
  -6.0dB`; `corr +0.73`; `bal R +4%`).

## [1.1.0.0] — 2026-07-05

### Added
- **Spectrum tap readout (SPAN-style).** Tap the touch strip on the spectrum
  view to read **frequency, nearest note ± cents, and level** at that point —
  e.g. `110Hz A2 ±0¢ -18.2dB` — with a dashed hairline and a dot marking the
  curve, exactly like SPAN's mouse hover. Compensated for the 200×100 slot:
  - **Snap to peak** (default on): the tap snaps to the strongest column
    within ~±8 px, so a fingertip near the kick reads the kick.
  - **True-peak frequency**: refines the 200-column grid (≈60 cents wide in
    the bass) to the actual FFT peak bin via log-domain parabolic
    interpolation — sub-Hz accuracy at 110 Hz.
  - Readout text scales with the render surface (strip vs key), dark backing
    strip for legibility, auto-hides after a configurable hold (default 6 s).
  - New spectrum options in the Property Inspector: **Tuning A4**
    (440 SPAN-default / 442 / 432), **Tap snap to peak**, **Readout hold**.
- Interaction changes on the **spectrum view only**: touch tap places/moves
  the readout (view cycling stays on the dial press); tap-and-hold clears the
  readout, or resets the view when none is shown. Other views unchanged.
- Browser demo parity: single-click places the marker, double-click clears it
  and cycles views. New headless test `scripts/test_readout.mjs` (note math,
  tap→frequency mapping, snap, bin interpolation — exit 0 = pass).

## [1.0.1.0] — 2026-07-05

### Fixed
- **manifest.json: added the required top-level `UUID`**
  (`com.adi.visualizers-and-meters`). The app inferred it from the folder
  name, but the official manifest schema marks UUID required and
  `streamdeck validate`/`pack` fail without it. Validated: 0 errors against
  @elgato/schemas (Draft-7). No behavior change.

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
