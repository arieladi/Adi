# Wavesfactory Spectre controller

`SpectreController` (`js/controllers/SpectreController.js`) is a predefined
strategy for **Wavesfactory Spectre (VST3)**, a fixed 5-band enhancer/EQ.
Parameters are resolved by **name** from the bridge's `all_params` list (VST3
indexes aren't version-stable); override with `SpectreController.OVERRIDES`.

## Dials

- **Dials 1-5 → bands 1-5.** Each column has its own dial mode cycling **FREQ ↔ GAIN**.
  FREQ uses `delta_log_index` (geometric), GAIN uses `delta_index` (linear).
- **Dial 6 → dynamic Q.** It controls the Q of `activeBand`. Turning any band
  dial (or tapping a band's shape/middle) sets `activeBand` to that band, so dial
  6 instantly *follows* the last-touched band. Zone 6 shows `Target: Band N`.

## Touchscreen

| Zone | Top | Middle | Bottom |
|------|-----|--------|--------|
| 1 | Shape (tap cycle) | Freq/Gain/Q stacked, active mode highlighted (tap = cycle mode) | **Quality** |
| 2 | Shape | … | **Color** |
| 3 | Shape | … | **Presets** |
| 4 | Shape | … | **Mode** |
| 5 | Shape | … | **Processing** |
| 6 | Target: Band N | active band's **Q** value | **Bypass** toggle |

Bottom-row global settings cycle on tap (hold / right-tap = previous). The Q line
in a band's middle row is highlighted when that band is the active (dial-6) target.

## Expected parameter names

Per band *n* = 1..5 (continuous unless noted):

| Role | Expected name(s) |
|------|------------------|
| `b{n}_freq`  | `Band n Frequency` / `Band n Freq` |
| `b{n}_gain`  | `Band n Gain` / `Band n Amount` |
| `b{n}_q`     | `Band n Q` / `Band n Bandwidth` |
| `b{n}_shape` | `Band n Shape` / `Band n Type` (quantized) |

Globals (quantized / toggle):

| Role | Column | Expected name(s) |
|------|--------|------------------|
| `quality`    | 1 bottom | `Quality` / `Oversampling` |
| `color`      | 2 bottom | `Color` / `Character` |
| `presets`    | 3 bottom | `Preset` / `Program` |
| `mode`       | 4 bottom | `Mode` / `Algorithm` / `Style` |
| `processing` | 5 bottom | `Processing` / `Channel Mode` / `Routing` |
| `bypass`     | zone 6 bottom | `Bypass` / `Enabled` / `Active` |

> Spectre's exact parameter names and value lists depend on the plugin
> build/version and on whether Live has **Configured** them. Unresolved roles are
> logged to Live's `Log.txt`; set `SpectreController.OVERRIDES = { b1_freq: 'Band 1 Frequency', quality: 12 }`
> to pin names or indexes. "Presets" is only mappable if Spectre exposes a
> selectable preset/program parameter; otherwise that column logs as unresolved.
