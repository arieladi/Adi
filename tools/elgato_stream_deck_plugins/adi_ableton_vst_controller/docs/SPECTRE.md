# Wavesfactory Spectre controller

`SpectreController` (`js/controllers/SpectreController.js`) is a predefined
strategy for **Wavesfactory Spectre (VST3)**, a fixed 5-band enhancer/EQ.
Resolved by device name `/\bspectre\b/i`. Parameters resolve by **name** from the
bridge's `all_params` (VST3 indexes aren't version-stable); pin with
`SpectreController.OVERRIDES`.

## Bands are named (not numbered) with fixed shapes

The 5 bands and their fixed shapes:

| # | Band (Live name) | Shape |
|---|------------------|-------|
| 1 | `LowShelf` | low shelf |
| 2 | `Peak 01` | bell |
| 3 | `Peak 02` | bell |
| 4 | `Peak 03` | bell |
| 5 | `HighShelf` | high shelf |

There is **no per-band shape parameter** — the shape is fixed. Each band instead
exposes a saturation **Color** and a **Processing** (stereo placement).

## Verified parameter names (from the Ableton Configure view)

Per band (anchored, e.g. `^lowshelf frequency$`):

| Role | Live name | Control |
|------|-----------|---------|
| `bN_freq` | `<Band> Frequency` | FREQ dial mode |
| `bN_gain` | `<Band> Gain` | GAIN dial mode |
| `bN_q` | `<Band> Q` | Q dial mode |
| `bN_switch` | `<Band> Switch` | dial press (on/off) |
| `bN_color` | `<Band> Color` | tap bottom-left (cycle) |
| `bN_proc` | `<Band> Processing` | tap bottom-right (cycle) |

Globals:

| Role | Live name | Control |
|------|-----------|---------|
| `output` | `Output` | dial 6 |
| `mix` | `Dry Wet` | zone 6 bottom (step) |
| `mode` | `Mode` | zone 6 top (cycle) / dial 6 press |

> Other Spectre globals (`Stereo Input`, `Input Compensation`, `Quality`,
> `De-Emphasis`, global `Processing`) are intentionally not mapped — set them in
> Ableton. Add them via `OVERRIDES` / new roles if you want them on the device.

## Dials + touch (Stream Deck + XL)

A strip-wide **mode** (tap the `GAIN | FREQ | Q` tabs) sets what the 5 band dials
adjust:

| Dial | Role |
|------|------|
| 1-5 | active mode (Gain / Freq / Q) for Lo Shelf / Peak 1 / Peak 2 / Peak 3 / Hi Shelf |
| 6 | Output (press = cycle Mode) |

Per band zone: top = mode tabs, middle = shape glyph + band name + value, bottom =
**Color** (left) and **Processing** (right) cycle pills. Dial press toggles the
band's **Switch** (the zone dims when off). Zone 6: top = **Mode** (cycle), middle
= **Output** value, bottom = **Mix** (Dry/Wet) step. Every value is shown via
Ableton's own `str_for_value` string through `AVC.showVal`.

## How it was verified

- Names taken from the user's Ableton Configure screenshot.
- Headless resolution test against the full real param list: all 5 bands ×
  (freq/gain/q/switch/color/proc) and output/mix/mode resolve with no unresolved
  roles, and the unmapped globals stay unmapped.
- `scripts/validate.py`, headless `app.html` load (clean console), demo
  screenshots in each dial mode.
