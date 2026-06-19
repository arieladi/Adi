# Adi Ariel MIDI Control — Stream Deck + / XL MIDI Controller

Turns the device into a multi-bank MIDI controller for Ableton Live, plus an OS numpad.

| Region | Function |
|---|---|
| Left 4×4 block | Drum pads — momentary MIDI notes **C1 → D#2** (bottom-left = C1, Ableton drum-rack order) |
| Right 5×4 block (−bottom-right) | OS numpad — `0–9 . + - × ÷ Enter Clear` (sends keystrokes, **not** MIDI) |
| Center touch strip | Scaled touch keyboard — 8 zones, driven by root note + scale |
| Bottom dials (×6) | MIDI-learn CC dials, 3 banks (**CC 20–37**) |
| Bottom-right key | **Set Selector** — cycles `DIALS 1-6 / 7-12 / 13-18` |

## How it works (Windows + macOS)

The Stream Deck plugin itself (`index.html` + `plugin.js`) is plain HTML/JS and runs identically on
both OSes. It forwards MIDI / keystroke commands as JSON over `ws://127.0.0.1:9234` to a small
**native helper** (`StreamDeckMidiHelper`) that you build and run alongside it. Only the helper is
platform-specific, and it's the same source file (`main.cpp`) compiled per OS:

| | Virtual MIDI port | Keystrokes |
|---|---|---|
| **Windows** | teVirtualMIDI driver (ships with loopMIDI) | `SendInput` |
| **macOS** | CoreMIDI virtual source — **built in, no driver** | `CGEvent` |

## ⚠️ Two things you must know first

1. **Hardware:** No single Elgato product has 6 dials + a 35-key matrix + an 8-zone strip. This
   plugin is **coordinate-driven** — drop the actions where your hardware has them and the code
   auto-detects positions. On a Stream Deck + (4 dials), the 8 touch zones map 2-per-segment.
2. **Virtual MIDI differs by OS:** Windows has no built-in virtual-MIDI API (and RtMidi can't create
   virtual ports there), so it needs the **teVirtualMIDI** driver, which installs for free with
   **loopMIDI** (https://www.tobias-erichsen.de/software/loopmidi.html). **macOS needs nothing** —
   the helper creates a CoreMIDI virtual source directly.

In both cases the helper creates its own port named **`Stream Deck MIDI Control`** — you do not need
to create any loopMIDI / IAC port yourself.

## Install — common steps

1. **Icons** are included under `imgs/…` (all `@1x` + `@2x` paths referenced in `manifest.json`).
   Regenerate them any time with `python3 scripts/gen_icons.py`; check completeness with
   `python3 scripts/validate.py`.
2. **Install the plugin:** the `com.adiariel.midicontrol.sdPlugin` folder (with `manifest.json`,
   `index.html`, `pi.html`, `plugin.js`, and `imgs/`) goes in the Stream Deck plugins directory,
   then restart the Stream Deck app.
3. **Run the native helper** for your OS. **Prebuilt binaries are committed in `bin/`:**
   `bin/macos/StreamDeckMidiHelper` (Apple Silicon / arm64) and
   `bin/windows/StreamDeckMidiHelper.exe` (x64, with `teVirtualMIDI64.dll` beside it). Just run
   the one for your machine — you only need to build from source (below) if you change `main.cpp`
   or need a different CPU architecture. The plugin auto-reconnects, so launch order doesn't matter.

## First run (prebuilt binaries — NO build needed)

This is the path for anyone downloading the repo. There is **no CMake** for end users on
**Apple Silicon Mac** or **x64 (Intel/AMD) Windows** — just run the binary in `bin/`. Because the
binaries are **unsigned** (free hobby app), the OS asks you to allow them **once**:

**macOS (Apple Silicon):**
1. Allow the unsigned binary once — run:
   ```
   xattr -dr com.apple.quarantine bin/macos/StreamDeckMidiHelper
   ```
   *(or try to launch it, then click "Open Anyway" in System Settings → Privacy & Security).*
2. Grant **Accessibility** (System Settings → Privacy & Security → Accessibility) so the numpad
   keys can send keystrokes. *(MIDI output needs no permission.)*
3. Run `./bin/macos/StreamDeckMidiHelper` — add a **Login Item** / `launchd` agent to autostart.
   *Intel Macs aren't covered by the prebuilt binary — build from source below.*

**Windows (x64):**
1. Install **loopMIDI** once — https://www.tobias-erichsen.de/software/loopmidi.html (provides the
   teVirtualMIDI driver; you don't create a port).
2. Run `bin\windows\StreamDeckMidiHelper.exe` (keep `teVirtualMIDI64.dll` beside it). If SmartScreen
   warns, click **More info → Run anyway** (one time). Add to **Startup** to autostart.

> **To remove the security prompts entirely** (fully seamless for downloaders), the binaries must be
> **code-signed + notarized** — Apple Developer Program (~$99/yr) for macOS and a code-signing
> certificate for Windows. That's optional and paid; without it the one-time "allow" above is the
> same step every unsigned indie tool requires.

## Build from source (optional — only to edit `main.cpp` or target another CPU)

### Windows

1. **Install loopMIDI** (provides the teVirtualMIDI driver). No loopMIDI port needed.
2. **Get the teVirtualMIDI SDK** and copy it into `third_party/teVirtualMIDI/` as
   `include/teVirtualMIDI.h`, `lib/x64/teVirtualMIDI64.lib`, `bin/x64/teVirtualMIDI64.dll`.
3. **Build:**
   ```
   cmake -S . -B build -A x64
   cmake --build build --config Release
   ```
   → `build/Release/StreamDeckMidiHelper.exe` (with `teVirtualMIDI64.dll` beside it).
4. **(once) copy it into the repo so you never rebuild on this PC again:** copy
   `build\Release\StreamDeckMidiHelper.exe` into `bin\windows\` (next to the `teVirtualMIDI64.dll`
   already there) and commit it.
5. **Run** `bin\windows\StreamDeckMidiHelper.exe`. Add it to Windows startup (Task Scheduler /
   Startup folder) to make it persistent.

### macOS

No driver to install — CoreMIDI is part of the OS.

1. **Build:**
   ```
   cmake -S . -B build
   cmake --build build --config Release
   ```
   → `build/StreamDeckMidiHelper`.
2. **Grant Accessibility permission:** the num-pad keys synthesize keystrokes via `CGEvent`, which
   macOS gates behind **System Settings → Privacy & Security → Accessibility**. Add the
   `StreamDeckMidiHelper` binary (or the terminal you launch it from) there, or the num-pad keys
   silently do nothing. *MIDI output needs no permission.*
3. **Run** the helper — the committed `./bin/macos/StreamDeckMidiHelper` (arm64), or your own
   `./build/StreamDeckMidiHelper`. To make it persistent, add a **Login Item** or a `launchd` agent.

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
- **Numpad:** keystrokes use numpad keys — `SendInput` on Windows, `CGEvent` on macOS (needs
  Accessibility permission); `Clear` = Esc on both.
- All tunables (base notes, velocities, CC banks, ports) live at the top of `plugin.js` and
  `main.cpp`.
