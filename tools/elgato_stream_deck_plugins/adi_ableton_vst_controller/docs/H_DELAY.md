# H-Delay controller

`HDelayController` (`js/controllers/HDelayController.js`) is a predefined strategy
for **Waves H-Delay** (Hybrid Line delay; the Stereo / Mono-Stereo / Mono
variants). Resolved by device name (`/\bh[-\s]?delay\b/i`). Parameters resolve by
**name** from the bridge's `all_params`; pin with `HDelayController.OVERRIDES`.

## Fixed 6-dial layout (no paging)

H-Delay exposes only a handful of Configured parameters, so the 6 dials map them
1:1 (like the INDEQ controller):

| Dial | Param | Type |
|------|-------|------|
| 1 | `Mix` | continuous |
| 2 | `Delay BPM` (note division) | stepped |
| 3 | `Feedback` | continuous |
| 4 | `HiPass` | continuous |
| 5 | `LoPass` | continuous |
| 6 | `PingPong` (routing mode) | stepped |

Continuous dials adjust on turn (`delta_index`). **Delay** and **PingPong** are
stepped: turn the dial, tap the zone, or press the dial to cycle (shift / right-tap
on the zone = previous). Every value is shown via Ableton's `str_for_value`
through `AVC.showVal`.

> Only **Configured** params are reachable for a VST3. H-Delay's other controls
> (Dry/Wet, Output, Analog, Mod Depth/Rate, sync source, LoFi, Tap) aren't mapped
> until you add them in Ableton's Configure view — then they can be wired in.

## Expected parameter names (from the Ableton Configure view)

`Mix`, `Delay BPM`, `Feedback`, `HiPass`, `LoPass`, `PingPong` (the routing-mode
param; may report as `Stereo` / `Ping Pong` / `ØL` / `ØR`).

Matching is anchored (e.g. `^hipass$`) with looser fallbacks. Unresolved roles are
logged (`H-Delay unresolved roles: …`) and the full list arrives as `all_params`.
If the routing param is named differently in your build, pin it:

```js
AVC.HDelayController.OVERRIDES = { pingpong: 'Stereo', delay: 'Delay BPM' };
```

## How it was verified

- Names taken from the user's Ableton Configure screenshot of H-Delay Stereo.
- Headless resolution test against the Configured param list: all 6 roles resolve,
  continuous dials nudge, and Delay / PingPong step on turn and tap.
- `scripts/validate.py`, headless `app.html` load (registry resolves `H-Delay`,
  no regression to the other controllers, clean console), demo screenshot.
