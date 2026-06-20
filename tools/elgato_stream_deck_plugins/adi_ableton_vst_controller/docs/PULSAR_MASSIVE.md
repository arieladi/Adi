# Pulsar Massive mapping

Predefined controller: [`js/controllers/PulsarMassiveController.js`](../com.adiariel.ableton-vst.sdPlugin/js/controllers/PulsarMassiveController.js).
Resolved when the selected device's name matches `/pulsar\s*massive/i`,
`/massive\s*passive/i` or `/\bmp[.\s-]?eq\b/i` (so it does **not** catch NI's
"Massive" synth).

## A-channel only

Pulsar Massive exposes a full **A** and **B** parameter set plus a `Stereo Mode`.
This controller is built for the normal stereo-linked workflow: it maps **only the
A parameters** and leaves Stereo Mode / the B channel / `ChannelA Active` alone.
Param names are anchored to the `A` suffix (`^band 1 gain a$`, `^gain a$`, …) so
the B parameter is never matched by accident.

## Verified parameter names (from the Ableton Configure view)

Bands `N = 1..4` → **Low / Warmth / Presence / Air**:

| Role | Live name | Control |
|------|-----------|---------|
| `bN_gain` | `Band N Gain A` | GAIN dial mode |
| `bN_freq` | `Band N Freq A` | FREQ dial mode (stepped) |
| `bN_width` | `Band N Bandwidth A` | WIDTH dial mode |
| `bN_active` | `Band N Active A` | tap bottom-left / dial press (IN/OUT) |
| `bN_type` | `Band N Type A` | tap bottom-right (Bell/Shelf) |

Centre section:

| Role | Live name | Control |
|------|-----------|---------|
| `drive` | `Drive A` | dial 5 |
| `gain` | `Gain A` | dial 6 |
| `low_pass` | `Low Pass Freq A` | zone 5 bottom (step) |
| `high_pass` | `High Pass Freq A` | zone 6 bottom (step) |
| `auto_gain` | `Auto Gain` | zone 5 top (toggle) / dial 5 press |
| `transfo` | `Transformer` | zone 6 top (Off/1/2) / dial 6 press |

## Dials + touch (Stream Deck + XL)

A strip-wide **mode** (tap the `GAIN | FREQ | WIDTH` tabs) sets what the 4 band
dials adjust:

| Dial | Role |
|------|------|
| 1-4 | active mode (Gain / Freq / Bandwidth) for Low / Warmth / Presence / Air |
| 5 | Drive |
| 6 | channel Gain |

Per band zone: top = mode tabs, middle = band name + value, bottom = **IN/OUT**
(left) and **Bell/Shelf** (right). Dial press toggles the band's IN/OUT. Zone 5
(Drive): top = Auto Gain, bottom = Low Pass step (tap left/right). Zone 6 (Gain):
top = Transformer (cycles Off / Transformer 1 / Transformer 2), bottom = High Pass
step. Every value is shown via Ableton's own `str_for_value` string through
`AVC.showVal`.

> Touch note: the Stream Deck encoder touchscreen only delivers a tap (position +
> long-tap), no drag. Low/High Pass "stepping" is therefore tap the left vs right
> half of the bottom row.

## If a build names them differently

VST3 indexes aren't stable, so roles resolve by **name** at device-bind time.
Unresolved roles are logged (`PulsarMassive unresolved roles: …`) and the full
list arrives over the bridge as `all_params`. Pin exact names or indexes:

```js
AVC.PulsarMassiveController.OVERRIDES = {
  b1_gain: 'Band 1 Gain A',   // exact Live name
  transfo: 41,                // …or a numeric parameter index
};
```

## How it was verified

- Names taken from the user's Ableton Configure screenshot of a Pulsar Massive
  instance (A + B + globals).
- Headless resolution test against the exact A **and** B param list: confirms all
  A-side roles resolve to the `… A` parameter (never the B one) and the centre
  globals resolve, with no unresolved roles.
- `scripts/validate.py`, headless `app.html` load (clean console), and a demo
  screenshot in each dial mode.
