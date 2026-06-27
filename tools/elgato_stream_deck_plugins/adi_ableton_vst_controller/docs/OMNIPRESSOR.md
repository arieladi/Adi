# Omnipressor controller

`OmnipressorController` (`js/controllers/OmnipressorController.js`) is a predefined
strategy for **Eventide Omnipressor** (dynamics: expander / gate / compressor /
limiter, VST3/AU). Resolved by device name (`/omnipressor/i`). Parameters resolve
by **name** from the bridge's `all_params`; pin with `OmnipressorController.OVERRIDES`.

16 params, so paged like the Blackhole controller.

## Dial pages (tap MAIN / I/O, or press a dial to advance)

| Dial | MAIN | I/O |
|------|------|-----|
| 1 | Threshold | Input Gain |
| 2 | Attack | Output Gain |
| 3 | Release | In Level |
| 4 | Function (EXP↔COMP ratio) | Out Level |
| 5 | Atten Limit | Mix |
| 6 | Gain Limit | Function |

`Function` — the Omnipressor's signature ratio knob (extreme expansion → gate →
1:1 → compression → ∞ limiting) — sits on both pages so it's always reachable.

## Bottom bar (the five switches, full width)

`BASS` (Norm/Cut) · `METER` (Input/Gain/Output — **cycles** on tap) · `SC`
(Sidechain Enable) · `LINE` (In/Out) · `POWER` (On/Off). 2-state switches toggle
on tap; Meter cycles its three positions. Every value is shown via Ableton's
`str_for_value` through `AVC.showVal`; continuous params use `delta_index`.

## Expected parameter names (from the Ableton Configure view)

`Threshold`, `Attack`, `Release`, `Function`, `Atten Limit`, `Gain Limit`,
`Input Gain`, `Output Gain`, `In Level`, `Out Level`, `Mix`, `Bass Switch`,
`Meter Select`, `Sidechain Enable`, `Line`, `Power`.

Matching is anchored (e.g. `^atten limit$`) with looser fallbacks. Unresolved
roles are logged (`Omnipressor unresolved roles: …`) and the full list arrives as
`all_params`. Pin exact names/indexes via `OVERRIDES`.

## How it was verified

- Names taken from the user's Ableton Configure screenshot of Omnipressor.
- Headless resolution test against the param list: all 16 roles resolve, paging
  maps the 6 dials per page, switches toggle and Meter cycles its three states.
- `scripts/validate.py`, headless `app.html` load (registry resolves
  `Omnipressor`, no regression to the other controllers, clean console), demo
  screenshots of both pages.
