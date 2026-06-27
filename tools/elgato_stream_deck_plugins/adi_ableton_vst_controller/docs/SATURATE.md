# Saturate controller

`SaturateController` (`js/controllers/SaturateController.js`) is a predefined
strategy for **Newfangled Audio Saturate** (spectral clipper / saturation,
VST3/AU). Resolved by device name (`/newfangled\s*saturate/i`, `/\bsaturate\b/i`
— anchored so it never catches Ableton's native **Saturator**, class `Saturator`).
Parameters resolve by **name** from the bridge's `all_params`; pin with
`SaturateController.OVERRIDES`.

## Fixed layout (no paging)

| Dial / zone | Param | Control |
|-------------|-------|---------|
| 1 | `Input Level` | continuous |
| 2 | `Clipper Drive` (the DRIVE knob, 0–24 dB) | continuous |
| 3 | `Clipper Shape` (Soft ↔ Hard, %) | continuous |
| 4 | `Clipper Detail` (Detail Preservation, None ↔ All, %) | continuous |
| 5 | `Output Level` | continuous |
| 6 | `Output Compensation` | continuous |
| **Bar** | **Switches** (full-width, 3 cells) | see below |

The six dials all adjust on turn (`delta_index`). The bottom bar holds three
cells:

| Cell | Param | Action |
|------|-------|--------|
| METER | `Meter Selector` (Gain Curve / Waveform) | tap cycles (`step_index`); hold / right-tap = previous |
| OUT MODE | `Output Level Select` (Automatic / Manual) | tap cycles; hold / right-tap = previous |
| LOCK | `Gain Lock` | tap toggles (`toggle_index`) |

Every value is shown via Ableton's `str_for_value` through `AVC.showVal` — so
`Clipper Shape` / `Clipper Detail` read "%" and the levels read "dB" exactly as
Ableton shows them (unitless fallbacks show the raw number). The switch cells
show Ableton's own label text (`Gain Curve`, `Automatic`, `Off`/`On`). Dial press
is unused (no paging / band state — the switches live in the bar).

> **Not mapped** (cosmetic / wrapper, and the per-module enables): `Active`,
> `Color Scheme`, `UI Scale`, `Meters On`, `Use OpenGL`, `Show Meters`,
> `Draw Curve`, and the three `Clipper … Active` (Drive / Shape / Detail) enables.
> The INPUT/OUTPUT Peak & RMS readouts and the gain-curve/waveform graph live in
> the GUI, not as params. Add anything else via Configure + `OVERRIDES`.

## Expected parameter names (from the Ableton Configure view)

`Input Level`, `Clipper Drive`, `Clipper Shape`, `Clipper Detail`,
`Output Level`, `Output Compensation`, `Meter Selector`, `Output Level Select`,
`Gain Lock`.

Matching is anchored (e.g. `^clipper drive$`, which won't grab
`Clipper Drive Active`) with looser fallbacks. Unresolved roles are logged
(`Saturate unresolved roles: …`) and the full list arrives as `all_params`. Pin
exact names / indexes via `OVERRIDES` if a build names them differently.

## How it was verified

- Names taken from the user's Ableton Configure screenshot of Newfangled Saturate.
- Headless resolution test against the full param list (including the cosmetic +
  `Clipper … Active` params): the 9 roles resolve to the right names/indices, the
  cosmetic + Active params stay unmapped, `Clipper Drive/Shape/Detail` resolve to
  the knobs (not their `… Active` siblings), continuous dials nudge, Meter / Out
  Mode cycle, and Gain Lock toggles.
- `scripts/validate.py`, headless `app.html` load (registry resolves
  `Newfangled Saturate` → `SaturateController`, no regression to the other
  controllers, clean console), demo screenshot.
