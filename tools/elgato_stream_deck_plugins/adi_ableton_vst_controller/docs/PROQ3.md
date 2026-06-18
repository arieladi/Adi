# FabFilter Pro-Q 3 controller

`ProQ3Controller` (`js/controllers/ProQ3Controller.js`) is a predefined strategy
for **FabFilter Pro-Q 3 (VST3)**. It resolves parameters by **name** from the
bridge's `all_params` list, so it tolerates version-specific parameter indexes.

## Static 6-band preset (required)

Pro-Q 3 allocates bands dynamically, which the Live API can't address reliably.
Save a preset with **exactly 6 bands instantiated** and select it before use:

| Band | Role in the default layout |
|------|----------------------------|
| 1 | Low Cut (bypassed by default) |
| 2-5 | Active bells |
| 6 | High Cut (bypassed by default) |

The controller maps the **first 6 band parameter groups** Live exposes.

## Multi-functional dials

Each of the 6 columns has an independent **dial mode** cycling **FREQ → GAIN → Q**
(`this._dialMode[slot]`). Turning a dial sends the parameter for *that column's*
current mode. Change the mode by tapping row 2 of the column, or by pressing the
dial. Modes are per-column and don't affect each other.

- FREQ → `delta_log_index` (geometric / musical)
- GAIN → `delta_index` (linear)
- Q → `delta_log_index` (geometric)

## Expected parameter names (per band *n* = 1..6)

The fuzzy resolver matches these (normalized, case-insensitive; first match wins).
Pin exact names/indexes in `ProQ3Controller.OVERRIDES` if your build differs.

| Role | dial/touch | Expected name(s) | Live type |
|------|-----------|------------------|-----------|
| `b{n}_used`   | row 1 power (tap toggle) | `Band n Used` / `Enabled` / `Active` / `On` / `Bypass` | quantized |
| `b{n}_freq`   | dial (FREQ mode) | `Band n Frequency` / `Band n Freq` | continuous |
| `b{n}_gain`   | dial (GAIN mode) | `Band n Gain` | continuous |
| `b{n}_q`      | dial (Q mode) | `Band n Q` / `Resonance` | continuous |
| `b{n}_shape`  | row 4 left (tap cycle) | `Band n Shape` / `Type` / `Filter Type` | quantized |
| `b{n}_slope`  | row 4 right (tap cycle) | `Band n Slope` / `Order` | quantized |
| `b{n}_stereo` | row 5 (tap cycle) | `Band n Stereo Placement` / `Placement` / `Stereo` | quantized |

Typical value lists Pro-Q 3 reports (read live via `all_params.items`):
- **Shape:** Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt
- **Slope:** 6, 12, 18, 24, 30, 36, 48, 72, 96 (dB/oct)
- **Stereo Placement:** Stereo, L, R, M, S

## Verifying / overriding names

1. Select Pro-Q 3 in Live; the plugin logs any **unresolved roles** to Live's
   `Log.txt`.
2. To see every exposed name, the bridge's `get_all_params` returns them; or read
   them in Live's device view. FabFilter VST3s sometimes expose a parameter only
   after you **Configure** it (device title-bar → Configure → wiggle the control).
3. Override in your setup, e.g.:
   ```js
   AVC.ProQ3Controller.OVERRIDES = { b2_freq: 'Band 2 Frequency', b1_used: 12 };
   ```

> Value units (Hz / dB) depend on how Live surfaces the VST3 parameter. The
> controller formats Hz/dB when the reported range looks like engineering units
> (e.g. a freq whose max ≥ 1000) and otherwise shows the raw value.
