# Blackhole controller

`BlackholeController` (`js/controllers/BlackholeController.js`) is a predefined
strategy for **Eventide Blackhole** (H9 plug-in series, VST3/AU reverb). Resolved
by device name (`/\bblackhole\b/i`). Same paged design as the Valhalla
controllers. Parameters resolve by **name** from the bridge's `all_params`; pin
with `BlackholeController.OVERRIDES`.

## Dial pages (tap MAIN / MOD, or press a dial to advance)

| Dial | MAIN | MOD |
|------|------|-----|
| 1 | Mix | Mod Depth |
| 2 | Gravity | Mod Rate |
| 3 | Size | Feedback |
| 4 | Predelay | Resonance |
| 5 | Low (EQ) | In Level |
| 6 | Hi (EQ) | Out Level |

## Bottom bar (Blackhole's signature switches, full width)

`KILL` (mute) · `FREEZE` (hold the tail) · `HOTSWITCH` (morph) — tap to toggle —
and `TEMPO` (`TempoSync`: Manual / Sync / Off) — tap to cycle. Every value is
shown via Ableton's `str_for_value` through `AVC.showVal`; continuous params use
`delta_index`.

`Ribbon Controller` and `Tempo` are intentionally left to the plugin GUI (the
ribbon is a performance morph; Tempo only applies when TempoSync = Sync). Add them
via `OVERRIDES` / new roles if you want them on the device.

## Expected parameter names (from the Ableton Configure view)

`Mix`, `Gravity`, `Size`, `Predelay`, `Low Level`, `Hi Level`, `Mod Depth`,
`Mod Rate`, `Feedback`, `Resonance`, `In Level`, `Out Level`, `Kill`, `Freeze`,
`HotSwitch`, `TempoSync`.

Matching is anchored (e.g. `^low level$`) with looser fallbacks. Unresolved roles
are logged (`Blackhole unresolved roles: …`) and the full list arrives as
`all_params`. Pin exact names/indexes via `OVERRIDES`.

## How it was verified

- Names taken from the user's Ableton Configure screenshot of a Blackhole instance.
- Headless resolution test against the real param list: all 16 roles resolve,
  paging maps the 6 dials per page, the three switches toggle and TempoSync steps.
- `scripts/validate.py`, headless `app.html` load (registry resolves `Blackhole`,
  no regression to the other controllers, clean console), demo screenshots.
