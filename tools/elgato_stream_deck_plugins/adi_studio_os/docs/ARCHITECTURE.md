# Studio OS — architecture

One Master Plugin for the Stream Deck **+ XL** (36 keys, 6 dials, 1200×100 strip)
that absorbs all five legacy plugins into a single navigable hierarchy.

Rulings that produced this shape live in [DECISIONS.md](DECISIONS.md).

## Process topology

```
┌─────────────────────── Elgato Stream Deck app ────────────────────────┐
│  registration WS                                                       │
│        │                                                               │
│  ┌─────▼──────────── com.adiariel.studioos.sdPlugin (CEF) ──────────┐  │
│  │  CodePath = app.html — Chromium, no Node                          │  │
│  │                                                                   │  │
│  │  core/surface.js    36-key + 6-dial model, coords ⇄ button number  │  │
│  │  core/input.js      long-press timers, 35+36 chord detector        │  │
│  │  core/nav.js        hierarchy (Level 0 → hubs → sub-menus)         │  │
│  │  core/states.js     State 0–4 carousel + overlay compositor        │  │
│  │  core/render.js     SVG → data-URI → setImage / setFeedback        │  │
│  │  modules/*          ableton · rekordbox · console · midi · viz     │  │
│  │  audio/engine.js    Web Audio FFT/meters  ◄── Visualizers live here │  │
│  └───────┬──────────────────────────────────┬────────────────────────┘  │
└──────────┼──────────────────────────────────┼───────────────────────────┘
           │ ws://127.0.0.1:9011              │ ws://127.0.0.1:9006
           │ (Studio OS IPC)                  │ (unchanged legacy protocol)
┌──────────▼───────────────────────┐   ┌──────▼──────────────────────────┐
│  studioos-service  (Node 20.20)  │   │  AdiVST Remote Script (Ableton) │
│  bundled w/ the app's own node   │   │  Live's CPython — docs/PROTOCOL │
│                                  │   └─────────────────────────────────┘
│  midi/out.js    @julusian/midi   │
│    · "Adi RekordBox Controller"  │──► rekordbox PERFORMANCE (MIDI LEARN)
│    · "Adi Studio OS MIDI"        │──► Ableton / any DAW (drums, CC, scale)
│  os/keys.js     numpad, hotkeys  │
│  os/volume.js   master vol/mute  │
│  os/apps.js     launch, alt-tab  │
│  home/lights.js dimmer HTTP      │
└──────────────────────────────────┘
```

**Why two processes.** A Stream Deck plugin has exactly one `CodePath` — either
CEF or Node, never both. CEF gives Canvas and Web Audio but no native MIDI and no
`child_process`; Node gives both of those but has no DOM. Splitting keeps
zero-latency native MIDI *and* the real-time visualizers. The pattern is already
proven twice in this repo (`midi_control` → C++ helper on 9234, Ableton VST →
Python remote script on 9006).

**Why the service is not a child of the plugin.** CEF cannot spawn processes, so
the service is installed as a login-start agent (macOS `LaunchAgent` with
`RunAtLoad` + `KeepAlive`; Windows Task Scheduler entry) by `scripts/install-*`.
The frontend retries the socket forever and renders an explicit *service offline*
surface rather than failing silently.

## Surface model

One universal action per controller type, placed once and never rearranged:

| Action | Controller | Instances |
|---|---|---|
| `com.adiariel.studioos.cell` | Keypad | 36 — one per key |
| `com.adiariel.studioos.dial` | Encoder | 6 — one per dial |

Each instance reports `coordinates` on `willAppear`; `core/surface.js` maps
`{column,row}` → button number `row*9 + col + 1` and keeps a context registry.
Every repaint is driven centrally by the active module + state, so no instance
holds behaviour of its own and there are no per-key Property Inspectors.

Keys are painted as **SVG strings → `data:image/svg+xml;base64` → `setImage`** —
sharper than the legacy 144px canvas rasterisation and free of a DOM canvas
dependency, which keeps the renderer portable if the frontend ever moves.

Dials use a single full-bleed 200×100 pixmap (`layouts/dial.json`), so the six
strip zones can be composited as one continuous 1200×100 image — the slicing
technique proven in the legacy `touchscreen.js`.

## Global rules (enforced centrally, not per module)

* **Button 1** — long press = up one hierarchy level; short press = contextual
  select/enter. *(exact behaviour in State 4 pending D7)*
* **Button 36** — no long-press timer anywhere; fires on `keyDown` for
  zero-latency module actions.
* **Button 35 + Button 36, held 1 s** — State Carousel / State-4 escape, in every
  state and every sub-plugin.

## Navigation

```
Level 0  Root Main Hub
         Key 1 → Ableton Live Hub     Key 2 → Cubase Hub (placeholder)
         rest  → OS shortcuts, app launchers, smart-home
         dials → 1 Master Vol (push=mute) · 2 OS Zoom · 3 App Switcher
                 4 Room Lighting · 5–6 blank
Level 1  Module hubs → Level 2 sub-menus …
```

Orthogonal to the hierarchy, the **State Carousel** composites over whatever
level is active. It has been renumbered twice — V13 scrapped State 3 (Context Nav)
and V59 deleted the Calculator — and is now:

```
0  Numpad      16-key dock on cols 5-8, NO dials
1  Divisions   the same dock + 2 borrowed dials (readout/grid/format + BPM)
2  NAV OFF     docks nothing; the module reclaims all 36 keys
```

Never compare a state index to a literal — ask `States.isFullScreen()`, or
`States.FULL` / `States.DELAY`. See V13 and V59 in DECISIONS.md.
