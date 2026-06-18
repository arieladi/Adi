# Pulsar Massive (MP.EQ) mapping

Predefined controller: [`js/controllers/PulsarMassiveController.js`](../com.adiariel.ableton-vst.sdPlugin/js/controllers/PulsarMassiveController.js).
Resolved when the selected device's name matches `/pulsar\s*massive/i`,
`/massive\s*passive/i` or `/\bmp[.\s-]?eq\b/i` (so it does **not** catch NI's
"Massive" synth).

## Stereo-linked → Left channel only

The default preset links L↔R, so we map **only the Left-channel parameters**;
moving them drives the Right automatically. No dual L/R controls are built.

## Physical dials

| Dial | Role | Parameter |
|------|------|-----------|
| 1 | **Low** band gain | `b1_gain` |
| 2 | **Warmth** band gain | `b2_gain` |
| 3 | **Presence** band gain | `b3_gain` |
| 4 | **Air** band gain | `b4_gain` |
| 5 | **Master Drive** | `drive` |
| 6 | **Master Gain** | `master_gain` |

Dial press mirrors each zone's top button (bands → toggle IN, zone 5 → Auto Gain,
zone 6 → cycle Transfo).

## Touchscreen zones (aligned above the dials)

| Zone | Top button(s) | Middle | Bottom (tap-left = ◂ / tap-right = ▸) |
|------|---------------|--------|----------------------------------------|
| 1–4 (bands) | left = **IN** (bypass), right = **Shelf/Bell** | band gain name + value | stepped **Frequency** |
| 5 (Drive) | **Auto Gain** | Drive + value | **Low Pass** |
| 6 (Master) | **Transfo** (cycles 1 / OFF / 2) | Master Gain + value | **High Pass** |

> Note on "swipe": the Stream Deck SDK delivers only `touchTap` (tap, with
> position + long-tap) for the encoder touchscreen — there is no swipe/drag event.
> "Swipe left/right" is therefore implemented as **tap the left vs right half** of
> the bottom text (tap the side you'd swipe toward). If device type 13 later
> exposes a drag delta, add it in `onTouch`.

## Expected Live parameter names (Left channel)

VST3 parameter **indexes are not stable across versions**, so the controller
resolves each role to an index by **name** at device-bind time (case-insensitive,
punctuation-insensitive, first matching candidate wins). Expected names per role:

| Role | Expected name candidates (first match wins) |
|------|---------------------------------------------|
| `b1_gain` | `L Band 1 Gain`, `Band 1 Gain L`, `Band 1 Gain`, `Low Gain`, `Gain 1` |
| `b2_gain` | `L Band 2 Gain`, `Band 2 Gain`, `Warmth Gain`, `Gain 2` |
| `b3_gain` | `L Band 3 Gain`, `Band 3 Gain`, `Presence Gain`, `Gain 3` |
| `b4_gain` | `L Band 4 Gain`, `Band 4 Gain`, `Air Gain`, `Gain 4` |
| `bN_in` | `L Band N In`, `Band N In`, `Band N Active`, `Band N On`, `In N` |
| `bN_shape` | `L Band N Shelf`, `Band N Shelf`, `Band N Bell`, `Band N Curve`, `Band N Shape` |
| `bN_freq` | `L Band N Freq`, `Band N Freq`, `Band N Frequency`, `Freq N` (stepped, ~11 positions) |
| `drive` | `Drive`, `L Drive`, `Master Drive`, `Saturation` |
| `master_gain` | `Master Gain`, `Output Gain`, `L Gain`, `Gain`, `Trim` |
| `auto_gain` | `Auto Gain`, `AutoGain`, `Auto-Gain`, `AGC` |
| `transfo` | `Transfo`, `Transformer`, `Xfmr` (3-state: 1 / OFF / 2) |
| `low_pass` | `Low Pass`, `LP Freq`, `LPF`, `Lowpass`, `LP` |
| `high_pass` | `High Pass`, `HP Freq`, `HPF`, `Highpass`, `HP` |

## If your build names them differently

These names are best-effort; your Pulsar Massive version may differ (and Live may
expose only a subset of VST3 params until you **Configure** them in the device
view — click the device title-bar wrench, then the parameters appear by name).

1. **Discover the real names.** Select Pulsar Massive in Live; any role the
   controller can't resolve is logged to Live's `Log.txt`
   (`…/Ableton/Live x.x/Preferences/Log.txt`) and to the Stream Deck log as
   `PulsarMassive unresolved: …`. The full list also arrives over the bridge as
   the `all_params` message.
2. **Pin them.** Add overrides at the top of `PulsarMassiveController.js`:
   ```js
   AVC.PulsarMassiveController.OVERRIDES = {
     b1_gain: 'L Band 1 Boost/Cut',   // exact Live name
     transfo: 41,                      // …or a numeric parameter index
   };
   ```
   Override values may be an exact parameter name **or** a numeric index.
3. Or edit the `ROLES` candidate patterns directly (substrings or RegExp).

Stepped knobs: if Live reports a freq/Transfo parameter as **quantized**
(`value_items` present), the controller steps through those items. If it's a
continuous param that snaps internally, set the role's `steps` count (default 11
for band frequency, 3 for Transfo) so steps land on the snap points.
