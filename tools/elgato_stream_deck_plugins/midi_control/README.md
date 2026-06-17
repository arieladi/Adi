# Adi Ariel MIDI Control — Stream Deck + / XL MIDI Controller

Turns the device into a multi-bank MIDI controller for Ableton Live, plus an OS numpad.

| Region | Function |
|---|---|
| Left 4×4 block | Drum pads — momentary MIDI notes **C1 → D#2** (bottom-left = C1, Ableton drum-rack order) |
| Right 5×4 block (−bottom-right) | OS numpad — `0–9 . + - × ÷ Enter Clear` (sends keystrokes, **not** MIDI) |
| Center touch strip | Scaled touch keyboard — 8 zones, driven by root note + scale |
| Bottom dials (×6) | MIDI-learn CC dials, 3 banks (**CC 20–37**) |
| Bottom-right key | **Set Selector** — cycles `DIALS 1-6 / 7-12 / 13-18` |

## ⚠️ Two things you must know first

1. **Hardware:** No single Elgato product has 6 dials + a 35-key matrix + an 8-zone strip. This
   plugin is **coordinate-driven** — drop the actions where your hardware has them and the code
   auto-detects positions. On a Stream Deck + (4 dials), the 8 touch zones map 2-per-segment.
2. **Windows virtual MIDI:** Windows has no built-in virtual-MIDI API and **RtMidi cannot create
   virtual ports on Windows**. The native helper uses the **teVirtualMIDI** driver, which installs
   for free with **loopMIDI** (https://www.tobias-erichsen.de/software/loopmidi.html).

## Install

1. **Install loopMIDI** (provides the teVirtualMIDI driver). You do *not* need to create a loopMIDI
   port — the helper creates its own port named **`Stream Deck MIDI Control`**.
2. **Get the teVirtualMIDI SDK** and copy it into `third_party/teVirtualMIDI/` as
   `include/teVirtualMIDI.h`, `lib/x64/teVirtualMIDI64.lib`, `bin/x64/teVirtualMIDI64.dll`.
3. **Build the helper:**
   ```
   cmake -S . -B build -A x64
   cmake --build build --config Release
   ```
   → `build/Release/StreamDeckMidiHelper.exe` (with `teVirtualMIDI64.dll` beside it).
4. **Add icon PNGs** under `imgs/…` (paths referenced in `manifest.json`): plugin/category icons,
   and per-action `icon`/`key` images (`@1x` + `@2x`). The plugin won't load without them.
5. **Install the plugin:** the `com.adiariel.midicontrol.sdPlugin` folder (with `manifest.json`,
   `index.html`, `pi.html`, `plugin.js`, and `imgs/`) goes in the Stream Deck plugins directory,
   then restart the Stream Deck app.
6. **Run the helper** (`StreamDeckMidiHelper.exe`). The plugin auto-reconnects, so launch order
   doesn't matter; add it to Windows startup (Task Scheduler / Startup folder) to make it persistent.

## Lay out the controls

Drag the actions onto the device:
- **Drum Pad** → a 4×4 block (any position; bottom-left becomes C1).
- **Num Pad Key** → a 5×4 block.
- **Set Selector** → the bottom-right key of the numpad block.
- **MIDI Dial** → each dial.
- **Scale Touch** → across the touch strip. Open its settings (Property Inspector) to pick
  **Root note**, **Scale**, and **MIDI channel** — these apply to the whole keyboard.

## Enable it in Ableton Live (required)

Open **Preferences → Link / Tempo / MIDI**. Under **MIDI Ports**, find the input named
**`Stream Deck MIDI Control`** and turn **both** switches on:

- **Track = On** → lets the drum pads and the touch keyboard send notes into Live.
- **Remote = On** → lets the dials be MIDI-learned for CC mapping.

Then **MIDI-map a dial:** click Live's **MIDI** button (top-right), select a parameter, turn the
dial once, and click **MIDI** again. Repeat per bank — the Set Selector swaps the dials between
CC 20-25 / 26-31 / 32-37, and each bank remembers its own values.

## Notes / behavior

- **Drum pads** send Note On (velocity 110, ch 1) on press and a true Note Off on release.
- **Touch keyboard:** the Stream Deck SDK only reports discrete *taps* (no finger-release), so each
  tap fires Note On followed by a timed Note Off (`TOUCH_NOTE_MS`, default 280 ms). 7-note scales
  fill zones 1-7 with zone 8 = root + 1 octave; 8-note scales (e.g. Diminished) fill all 8 zones.
- **Dials:** rotation sends absolute CC (step 2 per tick); pressing a dial resets it to 64.
- **Numpad:** keystrokes are injected via `SendInput` (numpad virtual-keys); `Clear` = Esc.
- All tunables (base notes, velocities, CC banks, ports) live at the top of `plugin.js` and
  `main.cpp`.
