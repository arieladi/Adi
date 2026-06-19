# FabFilter Pro-Q 3 controller

`ProQ3Controller` (`js/controllers/ProQ3Controller.js`) is a predefined strategy
for **FabFilter Pro-Q 3 (VST3)**. It is built around what Pro-Q 3 actually
exposes to Ableton in its **default device configuration** — verified against a
live instance.

## What the default Pro-Q 3 exposes (important)

Ableton's default Pro-Q 3 device surfaces **16 parameters** — per band only
**Frequency, Gain, Q** — and the two cut bands expose **no Gain**:

| Band | Exposed params | Role |
|------|----------------|------|
| 1 | `Band 1 Frequency`, `Band 1 Q` | low cut (no gain) |
| 2 | `Band 2 Frequency`, `Band 2 Gain`, `Band 2 Q` | bell |
| 3 | `Band 3 Frequency`, `Band 3 Gain`, `Band 3 Q` | bell |
| 4 | `Band 4 Frequency`, `Band 4 Gain`, `Band 4 Q` | bell |
| 5 | `Band 5 Frequency`, `Band 5 Gain`, `Band 5 Q` | bell |
| 6 | `Band 6 Frequency`, `Band 6 Q` | high cut (no gain) |

Pro-Q 3 does **not** expose Shape, Slope, Stereo Placement or band-enable by
default — those only appear if you add them manually via Ableton's **Configure**.
This controller therefore does not depend on them; it focuses on the always-present
Freq/Gain/Q. (If you later Configure extra params, they're simply ignored here.)

## Dials — multi-functional

Each column has its own **dial mode**:
- **Bell bands (2-5):** FREQ → GAIN → Q
- **Cut bands (1, 6):** FREQ → Q (no gain)

Turning the dial drives the active mode — Frequency and Q use a geometric/log
nudge (`delta_log_index`), Gain is linear (`delta_index`). **Press the dial** to
cycle the mode; **tap a mode tab or a value row** on the touchscreen to pick it.

## Touchscreen, per band column

```
TOP     B<n>  [ FREQ ][ GAIN ][ Q ]      ← mode tabs (cut bands show FREQ | Q)
MIDDLE  FREQ   150 Hz                      ← Freq / Gain / Q stacked,
        GAIN   +0.0 dB                       the active (dial) mode highlighted
        Q      1.00
BOTTOM  BELL                                ← band-type hint (context only)
```

## Verifying / overriding

The resolver matches `Band N Frequency` / `Band N Gain` / `Band N Q`
case-insensitively (Ableton truncates the label to `Band N ...quency` in the
Configure grid, but the full name is `Band N Frequency`). If your build names
them differently, pin them:

```js
AVC.ProQ3Controller.OVERRIDES = { b1_freq: 'Band 1 Frequency', b2_gain: 12 };
```

Unresolved roles are logged to Live's `Log.txt`. Frequency reads back in Hz/kHz,
Gain in dB, Q as a number — matching the values shown in Ableton.
