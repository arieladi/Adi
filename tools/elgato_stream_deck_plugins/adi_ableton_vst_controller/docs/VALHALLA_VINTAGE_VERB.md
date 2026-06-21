# ValhallaVintageVerb controller

`ValhallaVintageVerbController` (`js/controllers/ValhallaVintageVerbController.js`)
is a predefined strategy for **Valhalla DSP ValhallaVintageVerb (VST3)**, a
reverb. Resolved by device name (`/vintage\s*verb/i`). Same paged design as the
ValhallaRoom controller. Parameters resolve by **name** from the bridge's
`all_params`; pin with `ValhallaVintageVerbController.OVERRIDES`.

## Dial pages (tap MAIN / DAMP / SHAPE, or press a dial to advance)

| Dial | MAIN | DAMP | SHAPE |
|------|------|------|-------|
| 1 | Mix | High Freq | Attack |
| 2 | Predelay | High Shelf | Early Diffusion |
| 3 | Decay | Bass Xover | Late Diffusion |
| 4 | Size | Bass Mult | Mod Rate |
| 5 | High Cut | Decay | Mod Depth |
| 6 | Low Cut | Mix | Size |

Decay/Mix/Size repeat on deeper pages so they're always reachable.

## Bottom bar (globals, full width)

- **left = Reverb Mode** (`ReverbMode`) — the algorithm (Concert Hall, Plate, …).
- **right = Color Mode** (`ColorMode`) — the era voicing (1970s / 1980s / Now;
  Live reports it as `seventies` etc.).

Both are real automatable quantized params (unlike ValhallaRoom, whose second
slot was an unexposed Preset). Tap to cycle, hold/right-tap = previous. Every
value is shown via Ableton's `str_for_value` through `AVC.showVal`; continuous
params use `delta_index`.

## Expected parameter names (from the Ableton Configure view)

`Mix`, `PreDelay`, `Decay`, `Size`, `Attack`, `HighFreq`, `HighShelf`,
`BassXover`, `BassMult`, `EarlyDiffusion`, `LateDiffusion`, `ModRate`, `ModDepth`,
`HighCut`, `LowCut`, `ReverbMode`, `ColorMode`.

Matching is anchored (e.g. `^bassxover$`) with looser fallbacks. Unresolved roles
are logged (`ValhallaVintageVerb unresolved roles: …`) and the full list arrives
as `all_params`. Pin exact names/indexes via `OVERRIDES`.

## How it was verified

- Names taken from the user's Ableton Configure screenshot (VVV v2.1.2).
- Headless resolution test against the real param list: all 17 roles resolve,
  paging maps the 6 dials per page, and both bottom-bar selectors step.
- `scripts/validate.py`, headless `app.html` load (registry resolves
  `ValhallaVintageVerb`, no regression to ValhallaRoom, clean console), demo
  screenshots of each page.
