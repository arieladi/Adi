# ValhallaRoom controller

`ValhallaRoomController` (`js/controllers/ValhallaRoomController.js`) is a
predefined strategy for **Valhalla DSP ValhallaRoom (VST3)**, a reverb. Resolved
by device name `/valhalla\s*room/i`. Parameters resolve by **name** from the
bridge's `all_params` (VST3 indexes aren't version-stable); pin with
`ValhallaRoomController.OVERRIDES`.

This is the first non-EQ controller: a reverb has no band structure, so the 6
dials are **paged**.

## Dial pages (tap the MAIN / EARLY / LATE / RT tabs)

| Dial | MAIN | EARLY | LATE | RT |
|------|------|-------|------|----|
| 1 | Mix | Early Size | Late Size | Bass Mult |
| 2 | Predelay | Early Cross | Late Cross | Bass Xover |
| 3 | Decay | Early Mod Rate | Late Mod Rate | High Mult |
| 4 | High Cut | Early Mod Depth | Late Mod Depth | High Xover |
| 5 | Diffusion | Early Send | Decay | Decay |
| 6 | Early/Late Mix | Mix | Mix | Mix |

Mix and Decay repeat on the deeper pages so they're always reachable. **Pressing
any dial advances the page** (MAIN → EARLY → LATE → RT → MAIN).

## Bottom bar (globals, full width)

- **left = Reverb Mode** (`type`) — the reverb algorithm (Large Room, Bright Hall,
  …). Tap to cycle, hold/right-tap = previous.
- **right = Preset** — tap ◀ / ▶ to step. ValhallaRoom does **not** reliably
  expose a preset parameter via Configure, so this role often stays unmapped and
  the bar shows "— (not exposed)"; set presets in the plugin GUI. If your build
  does expose one, pin it with `OVERRIDES.preset`.

Every value is shown via Ableton's own `str_for_value` string through
`AVC.showVal`. All continuous reverb params use `delta_index` (linear in the
plugin's normalised range, which already carries the right taper).

## Expected parameter names (from the Ableton Configure view)

`mix`, `predelay`, `decay`, `HighCut`, `diffusion`, `earlyLateMix`, `earlySize`,
`earlyCross`, `earlyModRate`, `earlyModDepth`, `earlySend`, `lateSize`,
`lateCross`, `lateModRate`, `lateModDepth`, `RTBassMultiply`, `RTXover` (bass
xover), `RTHighMultiply`, `RTHighXover`, `type` (reverb mode).

Matching is anchored to these names (e.g. `^rtbassmultiply$`) with looser
fallbacks. Unresolved roles are logged (`ValhallaRoom unresolved roles: …`) and
the full list arrives over the bridge as `all_params`. Pin exact names/indexes:

```js
AVC.ValhallaRoomController.OVERRIDES = {
  reverbmode: 'type',
  bassxover: 'RTXover',
  preset: 42,        // numeric index, if your build exposes a preset param
};
```

## How it was verified

- Names taken from the user's Ableton Configure screenshot (ValhallaRoom v1.6.2).
- Headless resolution test against the real param list: all 19 continuous/mode
  roles resolve, paging maps the 6 dials per page, and the (absent) preset role
  degrades gracefully.
- `scripts/validate.py`, headless `app.html` load (registry resolves
  `ValhallaRoom`, clean console), demo screenshots of each page.
