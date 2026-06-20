# Handoff — Adi Ableton VST Controller (state + how to extend)

Snapshot for continuing development in a fresh session. Current version **1.4.2.0**.

## 0. Repo + ground rules

- Repo: `~/Documents/GitHub/Adi` (GitHub repo "Adi"). Plugins live in
  `tools/elgato_stream_deck_plugins/`. This plugin: `adi_ableton_vst_controller/`.
- **NEVER `git push`.** Commit locally only (scoped to the plugin folder, with a
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer); the user
  pushes via GitHub Desktop. Identity: Adi Ariel / adidatabase@gmail.com.
- **No nested `.git`** inside the plugin (it's part of the Adi monorepo).
- All text files are **LF** (`.gitattributes` enforces it; `.ps1` = CRLF).
- Each predefined VST = a version bump (manifest + package.json + CHANGELOG),
  committed scoped, then the user pushes.

## 1. The four plugins in the folder (context)

| Plugin | Type | Notes |
|--------|------|-------|
| `adi_ableton_vst_controller` | HTML/CEF + Python Remote Script | **the active project** (this doc) |
| `adi_visualizers_and_meters` | HTML/CEF (Web Audio) | self-contained, done |
| `com.adiariel.console.sdPlugin` | Node (`@elgato/streamdeck`) | bundled `bin/plugin.js` runs as-is |
| `midi_control` | HTML/CEF + native C++ helper | prebuilt mac binary in `bin/macos/`; Windows `.exe` user-built |

## 2. What this plugin is

A Stream Deck plugin (HTML/CEF runtime, SDK v2, mac 10.15+/win 10+) for a
36-key / 6-dial / touchscreen device ("device type 13"; also runs on Stream
Deck +). It tracks the selected Ableton track + device and maps it to the dials,
touchscreen and keys. Generic mode for any device; **predefined per-VST
controllers** for known plugins.

### Data flow
```
Ableton  ──(LOM)──▶  AdiVST Remote Script (Python)  ──ws://127.0.0.1:9006──▶  Stream Deck plugin (CEF)
  parameter.value      live_bridge.py reads/writes,                              bridge.js (state.pv/allParams)
                       ws_server.py (stdlib RFC6455)                             controllers render to touchscreen
```
Two sockets in the plugin page: the Elgato registration socket (`sd-client.js`)
and the Ableton bridge (`bridge.js`).

## 3. Folder structure

```
adi_ableton_vst_controller/
├── com.adiariel.ableton-vst.sdPlugin/
│   ├── manifest.json            2 actions: ".dial" (Encoder), ".key" (Keypad)
│   ├── app.html                 loads js in order (see §4)
│   ├── js/
│   │   ├── sd-client.js         AVC.SD  — Elgato socket + senders + event bus + deviceOfType(13)
│   │   ├── bridge.js            AVC.Bridge — Ableton WS client, state store, typed cmd senders
│   │   ├── touchscreen.js       AVC.Touchscreen — virtual 1200×100 canvas → per-dial pixmaps
│   │   ├── keys.js              AVC.Keys — 36-key mgr (EQ8 launcher + preset folder, long-press)
│   │   ├── plugin.js            orchestrator (connectElgatoStreamDeckSocket, ~15fps loop)
│   │   └── controllers/
│   │       ├── DeviceController.js  base Strategy + AVC.gfx + AVC.STEP + AVC.showVal + AVC.registry
│   │       ├── GenericController.js first 6 non-quantized params
│   │       ├── EQ8Controller.js     native "Eq8" (split-screen graph)
│   │       ├── PulsarMassiveController.js / ProQ3Controller.js / SpectreController.js / IndeqController.js
│   │       └── registry.js          registers all controllers
│   ├── pi/ inspector.html|js, sdpi.css      Property Inspector (key role, dial slot, bridge port)
│   ├── layouts/dial.json                    encoder touchscreen layout (one pixmap "full" 200×100)
│   └── imgs/                                 generated icons
├── ableton/remote_script/AdiVST/
│   ├── __init__.py              create_instance
│   ├── AdiVST.py               ControlSurface; drains inbound cmd queue in update_display() (~10Hz)
│   ├── ws_server.py            stdlib RFC6455 WebSocket server (no pip)
│   └── live_bridge.py          ALL Live Object Model access (runs on Live's main thread)
├── ableton/max_for_live/       M4L alternative (placeholder)
├── demo/                       hardware-free browser preview (mock bridge) — index.html, demo.js, styles.css
├── scripts/ gen_icons.py validate.py install-{mac,windows} pack.{sh,ps1}
└── docs/ PROTOCOL ARCHITECTURE ABLETON_SETUP PROQ3 SPECTRE INDEQ PULSAR_MASSIVE HANDOFF
```

`app.html` load order (matters; each augments `window.AVC`):
sd-client → bridge → DeviceController → Generic → EQ8 → PulsarMassive → ProQ3 →
Spectre → Indeq → registry → touchscreen → keys → plugin.

## 4. The named-parameter bridge channel (THE reusable mechanism)

This is how every predefined VST controller talks to Ableton. It is generic and
already built — new controllers just use it.

**Client → bridge (via `AVC.Bridge.cmd.*`):**
| cmd | sends | effect |
|-----|-------|--------|
| `getAllParams()` | `get_all_params` | bridge replies with `all_params` snapshot |
| `watch(indices)` | `watch` | bridge adds value listeners → emits `p` on change |
| `setIndex(i,norm)` | `set_index` | absolute set, norm 0..1 across [min,max] |
| `deltaIndex(i,delta)` | `delta_index` | linear nudge: value += delta*(max-min) |
| `deltaLogIndex(i,delta)` | `delta_log_index` | geometric nudge value*=2^(delta*4) (freq/Q) |
| `stepIndex(i,dir,steps)` | `step_index` | quantized → wrap value_items; else N steps; else fine |
| `toggleIndex(i)` | `toggle_index` | flip min↔max (2-state) |

**Bridge → client (handled in `bridge.js`, stored in state):**
- `all_params` → `state.allParams = [{i,name,value,min,max,quantized,items,disp}]`
- `p` → `state.pv[i] = {value, disp}` (live updates for watched indices)
- `device` (has_device, class_name, name, index, controller), `track`, etc.

**`disp` is Ableton's exact string** (`DeviceParameter.str_for_value()` in
`live_bridge._fmt_generic`) — e.g. "47.924 Hz", "0.00 dB", "Bell". Always display
it via `AVC.showVal(disp, fallback)` so the touchscreen mirrors Ableton and never
shows a reinvented number. `AVC.STEP` = 0.02 (normalized change per dial tick).

`AVC.gfx` = shared palette + canvas helpers (`clear, roundRect, text2, clamp,
bandColors`). The touchscreen is ONE virtual canvas `L = {W:1200,H:100,slots:6,
slotW:200,slotH:100}`; each controller draws all 6 zones; `touchscreen.js` slices
it per dial. Touch coords arrive in full-canvas px.

## 5. DeviceController Strategy + registry

Base (`AVC.DeviceController`) hooks a subclass overrides:
- `onState(state)` — cache state; (re)resolve params when the device changes.
- `renderTouch(ctx)` — draw the full 1200×100 canvas.
- `onDial(slot, ticks)` / `onDialPress(slot)` — dial rotate / press (slot 0..5).
- `onTouch(x, y, hold)` — touch in full-canvas px (hold = long/right-tap).
- `dialTitle(slot)` — short label for the dial's encoder feedback.

Resolution (the pattern every VST controller uses): VST3 indexes aren't stable,
so resolve each logical role to a parameter **by name** from `state.allParams`
(normalize: lowercase, punctuation→space; case-insensitive substring; first match
wins), with an `OVERRIDES` map (exact name or numeric index). On device change
call `getAllParams()`; after resolving call `watch([indices])`. Log unresolved
roles via `this.sd.log(...)`.

Registry resolves which controller to use: native devices by `class_name`
(`byClass`), VST/AU plugins by device **name** (`byName`, regex — they all report
class_name "PluginDevice"), else the bridge `hint`, else Generic. Registered in
`registry.js`.

## 6. How to add a new predefined VST controller (the recipe)

The user provides screenshots of (a) the plugin GUI and (b) **Ableton's
Configure view** (the param list). The Configure view is GROUND TRUTH for which
params exist and their exact names — only Configured params are controllable.

1. **Pick a template.** Fixed mapping → copy `IndeqController.js`. Per-column
   modal state → `ProQ3Controller.js` (multi-mode dial, shape-aware) or
   `SpectreController.js` (dynamic activeBand). Stereo-linked → `PulsarMassiveController.js`.
2. **Define roles** keyed to the exact Configure names (e.g. `band N frequency`,
   `band N gain`). Include an `OVERRIDES = {}`.
3. **Implement** `onState` (sig check → `getAllParams()` → `_resolve` → `watch`),
   `renderTouch` (6 zones × 200px), `onDial/onDialPress/onTouch`, `dialTitle`.
4. **Dial math:** continuous linear (gain/output) → `deltaIndex`; log (freq/Q) →
   `deltaLogIndex`; stepped/quantized switch → `stepIndex`; 2-state → `toggleIndex`.
5. **Display** every value via `AVC.showVal((this.state.pv[role.index]||{}).disp,
   myNumericFallback)`. For quantized switches show `items[round(value)]` (Ableton's labels).
6. **Honor shape/param-dependency** if the plugin disables some knobs per mode
   (Pro-Q 3: cuts have no Gain/Q, shelves no Q — read the shape param, derive modes).
7. **Wire it:** add `<script>` in `app.html` (after DeviceController, before
   registry) AND in `demo/index.html`. Register in `registry.js`
   (`AVC.registry.register({ ctor: AVC.XxxController, names:[/regex/i] })`).
8. **Demo:** add a mock param set in `demo/demo.js` mirroring the Configure
   screenshot (names/values/items), a `setMode` branch, title, hint, controller
   instance, and a toggle button in `demo/index.html`. Keep the existing modes.
9. **Doc:** add `docs/XXX.md` (Configure requirements, role→name table, dial/touch map).
10. **No server changes needed** unless a genuinely new general capability is
    required (that's how `delta_log_index` was added: live_bridge cmd + AdiVST
    dispatch + bridge.js sender + demo mock + PROTOCOL.md).

## 7. Verification loop (do this every change)

1. `python3 scripts/validate.py` (manifest + assets + Remote Script compiles).
2. Serve + headless-load `app.html`: `python3 -m http.server <port>` from the
   plugin dir, then via the Claude Preview MCP load
   `…/com.adiariel.ableton-vst.sdPlugin/app.html`, eval that `window.AVC`,
   `connectElgatoStreamDeckSocket`, the new controller and `registry.resolve(...)`
   exist; check console = clean.
3. **Headless resolution test:** build the EXACT param list from the user's
   Configure screenshot, `new AVC.XxxController({bridge:{cmd:{getAllParams(){},watch(){}}},
   sd:{log(){}},layout:{W:1200,H:100,slots:6,slotW:200,slotH:100}})`, call
   `onState` twice, assert `_missing` is empty and roles resolve to the right names.
4. **Demo screenshot:** load `demo/index.html`, switch to the new mode, exercise
   it, screenshot, confirm the layout.
5. Confirm no registry regressions (other VSTs still resolve), all-LF, no console errors.
6. Bump version, commit scoped (no push), tell the user to push.

## 8. Current predefined controllers (reference implementations)

- **EQ8Controller** — native Ableton EQ Eight (`class_name "Eq8"`); split-screen
  freq-response graph + band controls + pagination. Uses its own bridge messages
  (`eq8`, `eq8_band`, `eq8_state`) + EQ8-specific commands, not the named channel.
- **PulsarMassiveController** — Pulsar Massive (MP.EQ), stereo-linked (L only).
- **ProQ3Controller** — FabFilter Pro-Q 3; real Shape/Slope/Stereo switches +
  shape-aware FREQ/GAIN/Q dial modes (needs Configure to expose those params).
- **SpectreController** — Wavesfactory Spectre; 5 bands + dial 6 = dynamic Q that
  follows the last-touched band (`activeBand`).
- **IndeqController** — Analog Obsession INDEQ; fixed 6 dials + 6 toggles.

## 9. Ableton bridge install (for runtime testing)

Copy `ableton/remote_script/AdiVST` to Live's User Library Remote Scripts
(macOS `~/Music/Ableton/User Library/Remote Scripts/`), select **AdiVST** as a
Control Surface. Port 9006 (matches `PORT` in `AdiVST.py` and the PI). The user
runs this; the dev environment can't run Ableton, so LOM code is verified against
documented APIs + the user's screenshots, and tuned in-Live by the user.
