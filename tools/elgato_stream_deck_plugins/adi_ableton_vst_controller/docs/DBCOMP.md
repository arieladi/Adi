# dBComp controller

`DbCompController` (`js/controllers/DbCompController.js`) is a predefined strategy
for **Analog Obsession dBComp** (compressor / limiter, VST3/AU). Resolved by
device name (`/\bd[bB]\s*comp\b/i`). Parameters resolve by **name** from the
bridge's `all_params`; pin with `DbCompController.OVERRIDES`.

## Fixed layout (no paging)

| Dial / zone | Param | Control |
|-------------|-------|---------|
| 1 | `Threshold` | continuous |
| 2 | `Compression` (ratio) | continuous |
| 3 | `Output` (`Output Gain`) | continuous |
| 4 | `HPF` (sidechain high-pass) | continuous |
| 5 | `Mix` (dry/wet) | continuous |
| 6 | **Switches** | Oversampling (scroll dial 6 / tap top) · Bypass (press dial 6 / tap bottom) |

Continuous dials adjust on turn (`delta_index`). Zone 6 shows two pills —
**OVERSAMP** and **BYPASS**; scroll/tap-top cycles Oversampling, press/tap-bottom
toggles Bypass. Every value is shown via Ableton's `str_for_value` through
`AVC.showVal`.

> Not mapped: the unused `Parameter #6` / `Parameter #7` placeholders, and
> Ableton's own wrapper controls (Gain, Sidechain input). The GUI's `EXT SC`
> external-sidechain switch is driven by Ableton's sidechain routing, not a VST
> param. Add anything else via Configure + `OVERRIDES`.

## Expected parameter names (from the Ableton Configure view)

`Threshold`, `Compression`, `Output Gain`, `HPF`, `Mix`, `Oversampling`, `Bypass`.

Matching is anchored (e.g. `^threshold$`) with looser fallbacks. Unresolved roles
are logged (`dBComp unresolved roles: …`) and the full list arrives as
`all_params`. Pin exact names/indexes via `OVERRIDES`.

## How it was verified

- Names taken from the user's Ableton Configure screenshot of dBComp.
- Headless resolution test against the param list: the 7 roles resolve, the unused
  Parameter #6/#7 stay unmapped, continuous dials nudge, Oversampling cycles and
  Bypass toggles.
- `scripts/validate.py`, headless `app.html` load (registry resolves `dBComp`,
  no regression to the other controllers, clean console), demo screenshot.
