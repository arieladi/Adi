# EQ Eight (native Live device)

`EQ8Controller` drives Ableton's **EQ Eight** (`class_name "Eq8"`). Unlike the
VST controllers it does **not** use the named-parameter channel — it uses the
dedicated `eq8` / `eq8_band` / `eq8_globals` bridge messages and `eq8_*` commands
(see `docs/PROTOCOL.md`). Because EQ Eight is a *native* device, **all** its
parameters are always exposed by the LOM (no "Configure" step needed); the bridge
resolves them by name in `live_bridge.py`.

## Parameter names (verified against Ableton's Configure view)

The bridge matches band parameters with `_BAND_RE` in `live_bridge.py`:

```
^(\d+)\s+(Frequency|Gain|Resonance|Filter On|Filter Type)\s+([AB])$
```

Only the **A** edit-channel is used (Mode = Stereo, Edit = A). For each band
`N = 1..8`:

| Role | Live parameter name | Dial mode | Nudge |
|------|---------------------|-----------|-------|
| Frequency | `N Frequency A` | FREQ | geometric (`delta_log`) |
| Gain | `N Gain A` | GAIN | linear (dB) |
| Q / Resonance | `N Resonance A` | Q | geometric |
| Filter type | `N Filter Type A` | — (tap) | cycle `value_items` |
| Enable | `N Filter On A` | — (tap / dial press) | toggle |

Globals (matched by exact name):

| Role | Live parameter name | Control |
|------|---------------------|---------|
| Output Gain | `Output Gain` | GLOB mode, dial 1 |
| Scale | `Scale` | GLOB mode, dial 2 |

> The plugin GUI labels the global output simply **"Gain"**, but Live's parameter
> (Configure) name is **`Output Gain`**. `Mode`, `Edit Mode` and `Adaptive Q`
> exist on the device but are not controlled — the controller assumes Stereo / A.

Every displayed value is Ableton's own `str_for_value` string (`freq_disp`,
`gain_disp`, `q_disp`, `output_disp`, `scale_disp`), shown via `AVC.showVal` with
a local numeric format only as a fallback.

## Dial + touch map (Stream Deck + XL: 6 dials, touch strip, 36 keys)

The touch strip is 6 zones × 200 px. A **strip-wide mode** (set by tapping the
top tabs) decides what all 6 dials do:

- **FREQ / GAIN / Q** — the 6 dials adjust that parameter for the focused 6-band
  window `focus..focus+5`. Per zone: top = mode tabs `FREQ│GAIN│Q│GLOB`, middle =
  band number + value, bottom = **enable** (left) / **filter type** (right, tap to
  cycle, shift/long-press = previous). **Dial press** toggles that band's enable.
  **◀ / ▶** (far-left of zone 1, far-right of zone 6, middle row) paginate the
  window: 1-6 → 2-7 → 3-8.
- **GLOB** — **dial 1 = Output Gain**, **dial 2 = Scale** (both adjustable, values
  shown); the summed frequency-response graph fills zones 3-6. Tap a band-mode tab
  to return.

Keys (via `keys.js`) are unchanged: EQ8 launcher (create / cycle / closest),
preset folder, track/device navigation.

## How it was verified

- Names confirmed against the user's Ableton **Configure** screenshot (bands 1 & 2
  + Output Gain + Scale) and Live's documented EQ Eight LOM names; the `\d+` in
  `_BAND_RE` generates bands 1-8 automatically.
- `scripts/validate.py` (manifest + assets + Remote Script compiles).
- Headless `app.html` load (clean console; controller + registry resolve `Eq8`).
- Headless exercise of the controller with a full 8-band + Output + Scale mock
  (mode switching, dial nudges in every mode, pagination, enable/type, GLOB).
- Demo screenshot in band mode and GLOB mode.
