# SideMinder ME2 controller

`SideMinderController` (`js/controllers/SideMinderController.js`) is a predefined
strategy for **RJ Studios SideMinder ME2** (SideMinder Mastering Edition — a
3-band dynamic stereo-width maximizer, VST3/AU). Resolved by device name
(`/sideminder/i`, `/side\s*minder/i`). Parameters resolve by **name** from the
bridge's `all_params`; pin with `SideMinderController.OVERRIDES`.

`L` / `M` / `H` = the Low / Mid / High band.

## Paged layout (tap WIDTH / LIMIT / TRIM tabs, or press a dial, to advance)

| Dial | WIDTH | LIMIT | TRIM |
|------|-------|-------|------|
| 1 | L-Width | L-Release | L-Offset |
| 2 | M-Width | M-Release | M-Offset |
| 3 | H-Width | H-Release | H-Offset |
| 4 | LM Xover (`LMXovr`) | L-Ratio | L-Trim |
| 5 | MH Xover (`MHXovr`) | M-Ratio | M-Trim |
| 6 | I/O Trim (`IO Trim`) | H-Ratio | H-Trim |

- **Width** = Static Width Adjust % (0–200). **Release** = the Width-Limiter
  release (slow↔fast). **Ratio** = Width-Limiter ratio. **Offset** = Side-Mid
  Offset (dB). **Trim** = per-band Level Trim (dB).
- All dials nudge on turn. The two crossovers are frequencies → geometric nudge
  (`delta_log_index`); everything else is linear (`delta_index`). Pressing any
  dial advances the page.

## Global switch bar (full-width, 6 cells)

| Cell | Param | Action |
|------|-------|--------|
| BANDS | `#Bands` (1 / 2 / 3-Bands) | tap cycles (`step_index`); hold = previous |
| LINK | `BandLink` / Control Link (Independent / Relative / Ganged) | tap cycles; hold = previous |
| MONO | `Output Mono` | tap toggles |
| DELTA | `Norm/Delta` (Output Delta) | tap toggles |
| EXT SC | `ExtSC` (external sidechain) | tap toggles |
| BYPASS | `Bypass` | tap toggles |

Every value is shown via Ableton's `str_for_value` through `AVC.showVal` — widths
read "%", crossovers "Hz", offset/trim "dB", ratios "10.00 : 1" (the `:` passes
through), and the switch cells show Ableton's own labels.

> **Not mapped** (left to the GUI; add via Configure + `OVERRIDES` if you want
> them on the dials/bar): the per-band **Width-Out**, **Limiter-Out** and **Band
> Solo** toggles (`L/M/H-Width Out`, `L/M/H-Limiter`, `L/M/H-Solo`), the
> **Bass-Narrow / Bass-Mono** controls, the **correlation-meter** source
> (`Cmeter`), **Advanced**, and the Output/Input monitor. `MONO` degrades to "—"
> if the build exposes no `Output Mono` param (use `OVERRIDES`).

## Expected parameter names (from the Ableton Configure view)

Per band (L/M/H): `*-Width`, `*-Release`, `*-Ratio`, `*-Offset`, `*-Trim`.
Globals: `LMXovr`, `MHXovr`, `IO Trim`, `#Bands`, `BandLink`, `Bypass`,
`Norm/Delta`, `ExtSC`, `Output Mono`.

Matching is anchored (e.g. `^l width$`, with a `(?! out)`-guarded fallback so the
`L-Width Out` toggle is never grabbed) plus looser fallbacks. Unresolved roles
are logged (`SideMinder unresolved roles: …`) and the full list arrives as
`all_params`. Pin exact names / indexes via `OVERRIDES`.

## How it was verified

- Names taken from the user's Ableton Configure screenshot of SideMinder ME2.
- Headless resolution test against the full param list (including the per-band
  Out/Limiter/Solo toggles and the bass/meter/advanced params): the 24 mapped
  roles resolve to the right names/indices, the `*-Width` dials resolve to the
  amount params (not the `*-Width Out` toggles), the unmapped params stay
  unmapped, paged dials nudge (crossovers via `delta_log_index`), and the bar
  cells cycle/toggle.
- `scripts/validate.py`, headless `app.html` load (registry resolves
  `SideMinderME2` → `SideMinderController`, no regression to the other
  controllers, clean console), demo screenshot.
