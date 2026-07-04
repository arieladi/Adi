# Elgato Stream Deck Plugins — Project Handoff

Project-wide handoff for `~/Documents/GitHub/Adi/tools/elgato_stream_deck_plugins/`.
Covers all five plugins, shared conventions, and what's left to do. The main
focus is the Ableton VST controller, but all five are active.

## 0. Ground rules (apply to every plugin)

- Repo: `~/Documents/GitHub/Adi` (GitHub repo "Adi"); plugins under
  `tools/elgato_stream_deck_plugins/`. Identity: Adi Ariel / adidatabase@gmail.com.
- **NEVER `git push`.** Commit locally only, **scoped to the one plugin folder**,
  with trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. The user
  pushes via GitHub Desktop.
- No nested `.git` inside any plugin. All text files **LF** (`.ps1` CRLF) — each
  plugin that ships scripts has a `.gitattributes`.
- Stream Deck plugins are HTML/CEF (run in the app's embedded Chromium) unless the
  manifest has a `Nodejs` block. Targets: macOS 10.15+ / Windows 10+.
- Can't run hardware or Ableton in the dev env → verify by: `scripts/validate.py`,
  headless-loading pages via a local `http.server` + the Claude Preview MCP, and
  for the VST controller a headless resolution test + demo screenshot. For native
  param names / device specifics, the **user's Ableton "Configure" screenshots are
  ground truth.**

## 1. The five plugins

| Folder | Name | Ver | Type | Status |
|--------|------|-----|------|--------|
| `adi_ableton_vst_controller` | Adi Ableton VST Controller | 1.5.5.0 | HTML/CEF + Python Remote Script | **main**, active — 11 predefined controllers, all verified |
| `adi_visualizers_and_meters` | Adi Visualizers & Meters | 1.0.0.0 | HTML/CEF (Web Audio) | complete, can extend |
| `com.adiariel.console.sdPlugin` | Adi Ariel Console | 1.0.0.0 | Node (`@elgato/streamdeck`) | bundled, runs as-is |
| `midi_control` | Adi Ariel MIDI Control | 1.1.0.0 | HTML/CEF + native C++ helper | mac done, Windows binary pending |
| `com.adiariel.rekordbox.sdPlugin` | Adi Ariel RekordBox MIDI | 1.0.0.0 | Node (`@elgato/streamdeck` 1.4.x) + vendored MIDI natives | complete; verified headless (validate + 19-msg MIDI loopback) |

---

## 2. adi_ableton_vst_controller (MAIN)

Full detail in `adi_ableton_vst_controller/docs/HANDOFF.md` — read it before
touching this plugin. In brief:

- Stream Deck plugin ⟷ `ws://127.0.0.1:9006` ⟷ Python Remote Script (`AdiVST`)
  ⟷ Ableton LOM. Tracks selected track/device; maps to 6 dials + touchscreen + 36 keys.
- Reusable **named-parameter channel** (`getAllParams/watch/setIndex/deltaIndex/
  deltaLogIndex/stepIndex/toggleIndex`; state in `allParams`+`pv`; `disp` is
  Ableton's `str_for_value` string shown via `AVC.showVal`).
- **Strategy pattern**: `DeviceController` base → `GenericController`,
  `EQ8Controller`, and predefined VSTs; `registry.js` resolves by class_name /
  device-name regex / hint. Adding a predefined VST is a documented recipe (see
  the detailed handoff §6).

### Parameter-verification status — ✅ ALL VERIFIED (v1.5.5.0)

Predefined controllers resolve Ableton params **by name** (anchored regex like
`/^band 1 gain a$/` + looser fallbacks + an `OVERRIDES` map; unresolved roles are
logged; every value shown via Ableton's `str_for_value` through `AVC.showVal`).
All 11 were built/verified against the user's real Ableton "Configure" screenshots:

| Controller | Plugin | Type | Layout | Ver |
|-----------|--------|------|--------|-----|
| ProQ3Controller | FabFilter Pro-Q 3 | EQ | per-band, FREQ/GAIN/Q multi-mode | (prior) |
| IndeqController | Analog Obsession INDEQ | EQ | fixed 6 dials + 6 toggles | (prior) |
| EQ8Controller | Ableton EQ Eight (native) | EQ | FREQ/GAIN/Q/GLOB modes + graph | 1.4.3.0 |
| PulsarMassiveController | Pulsar Massive (A-channel) | EQ | GAIN/FREQ/WIDTH modes | 1.4.4.0 |
| SpectreController | Wavesfactory Spectre | EQ | GAIN/FREQ/Q modes, named bands | 1.4.5.0 |
| ValhallaRoomController | Valhalla ValhallaRoom | reverb | paged MAIN/EARLY/LATE/RT + Mode/Preset bar | 1.5.0.0 |
| ValhallaVintageVerbController | Valhalla VintageVerb | reverb | paged MAIN/DAMP/SHAPE + ReverbMode/Color bar | 1.5.1.0 |
| BlackholeController | Eventide Blackhole | reverb | paged MAIN/MOD + Kill/Freeze/HotSwitch/Tempo bar | 1.5.2.0 |
| HDelayController | Waves H-Delay | delay | fixed 6 dials (Delay/PingPong stepped) | 1.5.3.0 |
| DbCompController | Analog Obsession dBComp | comp | 5 knobs + Oversampling/Bypass switch zone | 1.5.4.0 |
| OmnipressorController | Eventide Omnipressor | dynamics | paged MAIN/I/O + Bass/Meter/SC/Line/Power bar | 1.5.5.0 |

**Layout patterns** (reuse for new controllers): EQ → per-band zones + multi-mode
dial tabs; reverb/large → paged dials (tap tabs / press dial to advance) + a
full-width bottom switch bar; small Configured set (delay/comp) → fixed 6-dial with
stepped params on turn/tap; dynamics → paged + multi-cell switch bar.

EQ Eight is the only **native** device — it uses its own bridge messages
(`eq8` / `eq8_band` / `eq8_globals`) + the `_BAND_RE` regex in `live_bridge.py`,
not the named channel; its server side gained Output Gain/Scale globals and
per-band gain/Q commands in v1.4.3.0.

`AVC.showVal(disp, fallback)` shows Ableton's string when it has a letter, `%`, or
`:` (ratios, e.g. Omnipressor Function `1:1`); else the numeric fallback — so
UNITLESS params (Blackhole Gravity, dBComp Compression) display the raw value.

---

## 3. adi_visualizers_and_meters

Real-time audio analyzer on the Stream Deck + touchscreen (800×100) and/or keys.
Pure Web Audio + Canvas, **zero deps**, self-contained.

- `com.adi.visualizers-and-meters.sdPlugin/`: one action "Audio View" (Keypad +
  Encoder). `js/engine.js` is the shared DSP+draw engine (FFT, AudioWorklet meter
  processor, ring buffers); `js/plugin.js` is the Stream Deck bridge; `pi/` is the
  inspector. 8 views: spectrum, scope, waveform, meters, octave bands, goniometer,
  correlation, balance.
- `demo/` runs the exact engine in a browser (no hardware). `scripts/validate.py`,
  `gen_icons.py`, install/pack scripts present.
- Runtime: mic permission; for system audio use a loopback device (BlackHole on
  mac, VB-Cable on Windows).
- To continue: add views / config options in `engine.js` + the PI; verify via the
  demo + validate.py. Same commit/version/push rules.

## 4. com.adiariel.console.sdPlugin

A **Node** Stream Deck plugin (`@elgato/streamdeck`). Six actions: launcher, bpm,
range, acoustic, delaycell, numpad. Layouts under `layouts/` (acoustic/bpm/calc/
range), Property Inspector `ui/inspector.html`.

- Source: `src/plugin.js`; bundled with rollup to `bin/plugin.js` (the committed
  bundle runs as-is; the Stream Deck app supplies Node). `rollup.config.mjs`,
  `package.json` (deps `@elgato/streamdeck`; dev `@elgato/cli`, rollup).
- To rebuild after editing `src/plugin.js`: `npm install` then `npm run build`
  (regenerates `bin/plugin.js`). `node_modules` is gitignored; the bundle is committed.
- No `validate.py` yet (could add one). Its functional logic isn't documented in
  this handoff — read `src/plugin.js` before changing it.
- To continue: edit `src/plugin.js`, `npm run build`, commit `bin/plugin.js` + src.

## 5. midi_control

HTML/CEF Stream Deck plugin + a **native C++ helper** that creates a virtual MIDI
port and synthesizes keystrokes (the plugin's sandbox can't). See its detailed
`README.md`.

- `com.adiariel.midicontrol.sdPlugin/` (index.html, pi.html, plugin.js, imgs) talks
  to the helper over `ws://127.0.0.1:9234`. Actions: drum, numpad, setselector,
  dial, scaletouch. Icons generated (`scripts/gen_icons.py`), `scripts/validate.py`.
- Native helper: `main.cpp` + `CMakeLists.txt` (FetchContent pulls IXWebSocket +
  nlohmann/json; CoreMIDI on mac, teVirtualMIDI on Windows — SDK vendored in
  `third_party/`). Prebuilt **mac arm64** binary committed at `bin/macos/`;
  `bin/windows/` has the dll, **the `.exe` is still TODO** (user builds it once on
  Windows: `cmake -B build -A x64 && cmake --build build --config Release`, then
  copy `build\Release\StreamDeckMidiHelper.exe` into `bin\windows\` and commit).
  `build/` is gitignored.
- Runtime: Windows needs **loopMIDI**; macOS needs **Accessibility** permission
  (numpad keystrokes only). Unsigned binaries → one-time Gatekeeper/SmartScreen allow.
- To continue: edit `plugin.js` (UI/MIDI logic) and/or `main.cpp` (native); rebuild
  the helper only if `main.cpp` changed.

## 5b. com.adiariel.rekordbox.sdPlugin

Class-compliant **virtual MIDI controller for rekordbox PERFORMANCE mode** on the
Stream Deck **+ XL** (device type 13 — 9×4 keys, 6 dials, 1200×100 strip = six
200×100 touch zones). Node plugin like the console one (rollup `src/` →
committed `bin/plugin.js`), plus a **committed `vendor/node_modules`** carrying
easymidi → `@julusian/midi` prebuilt N-API 7 binaries (darwin-arm64/x64,
win32-x64/arm64) loaded via `createRequire(vendor/_resolve_.cjs)` — end users
never compile. macOS creates the virtual port "Adi RekordBox Controller";
Windows attaches to a loopMIDI port (RtMidi can't create virtual ports there —
and fails silently, hence the explicit platform branch + retry loop).

- Control matrix in `src/midimap.js` (Ch1=Deck A, Ch2=Deck B, Ch3=browser):
  hot cues 16-23 / delete 24-31 (local **shift layer**, 2 shift keys), play/cue
  + shifted variants, held nudge notes, load-on-volume-push, beat jump 40/41
  from touch taps, browse 50-52; absolute 7-bit CC 20/21/22 = volume/filter/
  tempo with per-deck accumulators shown on the LCD (layouts/*.json bar/gbar).
- README has the full MIDI chart **incl. type-in-able 4-digit hex codes**, the
  rekordbox 7 MIDI-LEARN walkthrough and pitfalls (⚠ MIDI LEARN needs Core plan
  or Hardware Unlock — "Free Plus" does NOT include it; TempoSlider Type must
  be switched 14-bit→7-bit; nudge = PitchBendUp/Down, CSV-import fallback).
- Gotcha discovered building it: in `@elgato/streamdeck` **1.4.x**
  `onDidReceiveSettings` lives on `streamDeck.settings`, NOT
  `streamDeck.actions` (console plugin's src targets an older 1.x — its
  committed bundle still runs, but it will break if rebuilt against 1.4.x).
- Manifest needs Stream Deck app **7.3+** (first with device type 13 profiles).
- Verify: `python3 scripts/validate.py`, `npm run smoke` (real virtual-port
  loopback, 19 messages, mac/linux), `node bin/plugin.js` must fail only with
  the "missing -port/-pluginUUID" registration error.

## 6. Open work across the project

1. **VST controller — add more predefined controllers** as the user sends plugin
   GUI + Ableton Configure screenshots (per-plugin process; detailed handoff §6).
   All 11 current controllers are verified. Next version: 1.5.6.0+.
2. **midi_control — build + commit the Windows helper `.exe`** (user, on Windows).
3. **rekordbox plugin — on-hardware pass** (user): export the `AdiRekordBox`
   profile into the plugin root, map in rekordbox per the README chart (needs
   Core plan or Hardware Unlock), verify loopMIDI flow on Windows.
4. (Optional) add `validate.py` to the console plugin; package `.streamDeckPlugin`
   builds; per-plugin feature work.

**Permissions (dev env):** `~/.claude/settings.json` has a tool-level allow-list
(Bash/Edit/Write/Read + the Claude_Preview MCP tools) so the desktop app stops
prompting, plus a `deny` on `Bash(git push:*)` backstopping the never-push rule.

**Demo:** serve from the PLUGIN ROOT — `cd adi_ableton_vst_controller &&
python3 -m http.server 8000` → `http://localhost:8000/demo/index.html`. Serving
from `demo/`, or opening the file directly, breaks the `../` controller paths.
