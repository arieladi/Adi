# FabFilter Pro-Q 3 controller

`ProQ3Controller` drives **FabFilter Pro-Q 3 (VST3)** as a 6-band EQ with real
Shape / Slope / Stereo switches and shape-aware Freq/Gain/Q dials.

## Required Ableton "Configure"

Pro-Q 3 exposes only a handful of parameters by default. **Configure** the device
in Ableton (the *Configure* button, then click each control in the plugin) so the
device exposes, per band:

| Param | Bands |
|-------|-------|
| `Band N Frequency` | 1-6 |
| `Band N Q` | 1-6 |
| `Band N Shape` | 1-6 |
| `Band N Slope` | 1-6 |
| `Band N Stereo Placement` | 1-6 |
| `Band N Gain` | 2-5 (the cut bands 1 & 6 have no gain) |

Default preset values (bands 1 = Low Cut, 6 = High Cut, 2-5 bells) are mirrored in
the browser demo.

## Dials — shape-aware

Each column's dial controls one mode: **FREQ / GAIN / Q**. The available modes
depend on the band's current **Shape**, matching FabFilter:

- **no GAIN** for: Low Cut, High Cut, Notch, Band Pass
- **no Q** for: Low Cut, High Cut, Low Shelf, High Shelf, Tilt Shelf, Flat Tilt

So a Bell band offers FREQ/GAIN/Q; a Low Shelf offers FREQ/GAIN; a Low/High Cut
offers FREQ only. Change a band's Shape and its mode tabs update live. Press the
dial (or tap a mode tab) to switch mode. FREQ + Q nudge geometrically
(`delta_log_index`); GAIN is linear (`delta_index`).

## Touchscreen, per band column

```
TOP     B<n>  [ FREQ ][ GAIN ][ Q ]   ← mode tabs (only the applicable modes)
MIDDLE  150 Hz                          ← active mode's live value (from Ableton)
BOTTOM  [SHAPE][SLOPE][STEREO]          ← tap to cycle (shift/right-tap = previous)
```

Switch option lists (cycled via `step_index`, read live from the param's items):
- **Shape:** Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt
- **Slope:** 6 / 12 / 18 / 24 / 30 / 36 / 48 / 72 / 96 dB/oct, Brickwall
- **Stereo Placement:** Left, Right, Stereo, Mid, Side

## Values

All displayed values come straight from Ableton via `str_for_value` (e.g.
"47.924 Hz", "0.00 dB", "Low Cut", "12 dB/oct", "Stereo") — never reformatted.
Unresolved roles are logged to Live's `Log.txt`; pin names/indexes with
`ProQ3Controller.OVERRIDES = { b1_shape: 'Band 1 Shape' }` if your build differs.
