# Studio OS — conflict & decision log

Every architectural crossroad hit while merging the five legacy plugins into one
Master Plugin, the options offered, and Adi's ruling. **Nothing gets implemented
until it appears here with a ruling.** Append-only; newest batch at the bottom.

Legacy sources being merged:

| Module | Legacy plugin | Ver |
|---|---|---|
| Ableton Live Hub | `adi_ableton_vst_controller` | 1.5.9.0 |
| Rekordbox | `com.adiariel.rekordbox.sdPlugin` | 1.0.1.0 |
| Console (numpad / calc / delay / acoustic) | `com.adiariel.console.sdPlugin` | 1.0.2.0 |
| MIDI Control (drums / scale touch / CC dials) | `midi_control` | 1.1.0.0 |
| Visualizers & Meters | `adi_visualizers_and_meters` | 1.3.0.0 |

---

## Batch 0 — verified facts (no ruling needed)

**F1 — Target hardware.** Read from the Stream Deck app's own preferences, not
assumed: `DeviceName = "Stream Deck + XL"`, model `20GBX9901`, accessory
`20GBN9901`. Installed profiles confirm the grid:

```
Keypad  cols 0–8 × rows 0–3 = 36 keys      Button N = row*9 + col + 1
Encoder cols 0–5             = 6 dials      Touch strip 1200×100 = 6 × 200×100 zones
Button 1 = (col 0, row 0)   Button 35 = (col 7, row 3)   Button 36 = (col 8, row 3)
```

**F2 — Stream Deck bundles Node 20.20.0** at
`~/Library/Application Support/com.elgato.StreamDeck/NodeJS/20.20.0/node`
(Windows: `%APPDATA%\Elgato\StreamDeck\NodeJS\<ver>\node.exe`). The backend
service runs on the app's own runtime — users never install Node.

**F3 — Prebuilt MIDI natives already cover both platforms.**
`com.adiariel.rekordbox.sdPlugin/vendor/node_modules/@julusian/midi/prebuilds/`
ships N-API v7 binaries for `darwin-arm64`, `darwin-x64`, `win32-x64`,
`win32-arm64`. Consequence: **the `midi_control` C++ helper is obsolete** — its
virtual-MIDI and keystroke duties move into the Node service, which closes the
long-standing "Windows `StreamDeckMidiHelper.exe` still TODO" item in HANDOFF §6.2.

---

## Batch 1 — foundation

### D1 — Surface model *(conflict: legacy plugins are position-agnostic palettes)*

All five legacy plugins publish draggable actions configured per-instance in a
Property Inspector; none of them knows what "button 1" is. Studio OS must own
absolute positions to reserve the anchors.

**RULING — hybrid takeover.** Single-action hardware takeover: one
`studioos.cell` action placed on all 36 keys and one `studioos.dial` on all 6
dials, each reporting its `coordinates` on `willAppear`. All navigation happens
inside the plugin; one profile, forever; no `switchToProfile` anywhere.

**But not a single Node runtime.** Split into two processes joined by a local
WebSocket bridge:

* **CEF frontend** (`CodePath = app.html`) — UI, all key/dial rendering, the
  navigation state machine, and **Web Audio** for the Visualizers module.
* **Node backend service** — native MIDI (rekordbox + MIDI Control), OS
  keystroke/volume/zoom/app-switch routing, smart-home calls.

Rationale: keeps zero-latency native MIDI *and* keeps the Visualizers alive,
neither of which survives a single-runtime plugin.

### D2 — Anchor gestures *(conflicts: Button 36 vs. Deck B nudge; State 4 has no exit)*

Legacy rekordbox puts a **held** nudge on (8,3), and State 4 as specced consumed
both anchors leaving no escape.

**RULING (superseded — see D2a).** Button 36 loses its long-press timer entirely
and fires on `keyDown`; State Carousel and State-4 escape become a 1-second chord
of Button 35 + Button 36.

### D2a — Anchor gestures, final *(supersedes D2; raised as D6)*

D2 collided with its own premise: a chord needs a 1 s hold, but a `keyDown`-only
Button 36 fires Enter/nudge *before* the chord completes. Four suppression schemes
were offered.

**RULING — scrap the chord, revert to the original clean design.**

* **Button 36 long-press (500 ms)** = State Carousel, and the escape from State 4.
  One gesture, every state, every sub-plugin.
* **Button 36 short-press** fires on release (standard long-press pattern — the
  short action cannot be known until the timer is beaten).
* **Button 35 is a plain calculator Clear key.** No gateway logic, no delay, no
  chord role anywhere.

> **Reopens conflict #3** — rekordbox's *held* Nudge ▶▶ Deck B sits on (8,3) and
> cannot be held past 500 ms without firing the carousel. Tracked as **D9**.

### D5 — Numpad bottom row *(12 cells, 11 tokens)*

**RULING —** `0` at (5,3), `.` at (6,3), **`Clear` at (7,3)**. Conventional
calculator order, 7-8-9 on top. With D2a scrapping the chord, (7,3) carries no
gateway logic — it is simply Clear.

### D7 — Button 1 in State 4 *(collides with rekordbox's held Nudge ◀◀ Deck A)*

**RULING — State 4 releases Button 1.** In States 0–3 Button 1 is Back
(long-press) / contextual select (short). In State 4 it is handed entirely to the
active module, so Deck A nudge behaves like real hardware. Escape from State 4 is
the D2a Button 36 long-press. **Button 36 is *not* released in State 4** — it
remains the only global gesture there.

### D8 — Overlay geometry for States 1 & 3, and dial ownership

**RULING — one fixed overlay region.** States 0, 1 and 3 all occupy the identical
16-key right-hand block (cols 5–8), so the overlay never moves. The overlay also
claims **dials 5 & 6** — exactly where the legacy calculator operators and the
acoustic note/octave → Hz/cm readout already lived, so the console module ports
without remapping. The active sub-plugin keeps **dials 1–4**. State 2 remains the
sole full-device takeover (all 36 keys + all 6 dials).

### D3 — Overlay geometry *(conflict: States 0/1/2 are wildly different sizes)*

**RULING — right-hand block, with State 2 exempt.**

State 0 (Numpad) is custom-mapped to the **four** right-most columns:

```
col:      5      6      7      8
row0    ┌──────────────────┐  [ + ]
row1    │   3 × 4 number   │  [ − ]
row2    │      block       │  [ ⌫ ]
row3    └──────────────────┘  [ ⏎ ]      (8,3) = Button 36, aligns with the chord
```

No NumLock toggle — the hardware mapping is absolute.

State 2 (Delay Calculator) **takes over the entire board and all dials**, exactly
as laid out in the legacy console plugin.

### D4 — v1 scope *(conflict: two modules don't survive a Node-only runtime)*

**RULING — ship all five, zero deferrals.** The D1 hybrid makes this possible:
CEF handles Web Audio visualizers (listening to BlackHole / virtual cables), the
Node backend handles native MIDI, drum pads and scale touch. Both modules
integrate directly into the unified grid and dial layout under the global rules —
no separate plugins, no profile switching.

---

## Batch 3

### D9 — Button 36 vs. rekordbox's held Nudge ▶▶ Deck B

D2a restored the 500 ms long-press on Button 36, and D7 keeps it reserved even in
State 4. (8,3) is rekordbox's **held** Nudge ▶▶ Deck B, so Deck B cannot be
nudged past 500 ms — while Deck A nudge, freed by D7, works perfectly.

**RULING — accept the 500 ms cap.** No layout changes: the legacy rekordbox row 3
and the whole MIDI map stay exactly as built, and the annotated reference photo
remains valid. Nudge ▶▶ B sends Note On at `keyDown`; if it is still held at
500 ms the plugin sends the matching Note Off and opens the carousel.

### D9a — derived: how Button 36 delivers its short press *(flagged for veto)*

D2a ("standard 500 ms long-press timer") and D9 ("Note On at `keyDown`") describe
*different* delivery models for the same key — the first fires on release, the
second on press. Only one reading satisfies both, and it needs no new rule
because modules already have to declare what kind of binding a key is:

* Binding declared **momentary** (jog nudge, drum pad, cue audition) — `keyDown`
  forwards immediately so the Note On is sample-accurate; at 500 ms the engine
  emits the matching release *first*, then opens the carousel. This is D9 verbatim
  and guarantees no hanging note.
* Binding declared **tap** (numpad Enter, play/pause, menu select) — the legacy
  pattern: fires on release, and is swallowed entirely if the timer already won.
  This is D2a verbatim and means opening the carousel never types a stray Enter.

Implemented in `js/core/input.js`. **Adi has not ruled on this** — it is derived,
not chosen. Say the word and it collapses to one model for all bindings.

---

## Batch 4

### D10 — State 2 delay grid vs. the reserved Button 1

The legacy 24-cell grid starts at (0,0), which is Back in every state including
State 2, destroying the Straight 1/1 ms readout. The board is 9 columns and the
grid is 6.

**RULING — shift the grid to cols 1–6.** All 24 readouts survive; the legacy math
and PI categories port unchanged. Cols 7–8 (8 keys) are free — content TBD.

### D11 — Root Hub keys

**RULING — mostly empty, plus six named:** Windows/Start key, Run, PowerShell,
Task Manager, Chrome, Lynx Mixer. Placed on buttons 3–8, filling out row 0 after
Ableton (1) and Cubase (2); rows 1–3 stay free.

### D12 — Room lighting

**RULING — nothing yet; dial 4 stays inert.** The `home.dim` verb, the service
handler and the driver seam are built anyway (`service/home.js` ships working Hue
/ Home Assistant / Elgato drivers behind `~/.studioos/home.json`), so choosing a
system later is filling in config, not adding a feature.

### D13 — Ableton hub shape

**RULING — the hub IS the flat surface.** Entering it lands directly on the live
device controller exactly as 1.5.9 works today: 6 dials follow Live's selected
device, all 11 predefined VST layouts resolve automatically. Sub-menus (EQ8
presets, track/device navigation, transport) become Level 2 screens entered
deliberately. Hierarchy is added without being imposed on the most-used path.

### D14 — derived: macOS equivalents for the Windows-named Root Hub keys *(flagged for veto)*

Four of the six D11 keys are named for Windows, and this is a Mac. Rather than
ship them broken, `service/os.js` defines each as one concept with two
implementations:

| Key | macOS | Windows |
|---|---|---|
| Start | Launchpad | `Ctrl+Esc` (Start menu) |
| Run | Spotlight (`⌘Space`) | `Shell.Application.FileRun()` |
| Shell | Terminal.app | `Start-Process powershell` |
| Tasks | Activity Monitor | `Start-Process taskmgr` |
| Chrome | Google Chrome | `Start-Process chrome` |
| Lynx | Lynx Mixer | `Start-Process "Lynx Mixer"` |

Chrome is installed here; **Lynx Mixer is not present on this Mac**, so that key
reports failure rather than pretending to work. Not ruled — say the word and any
row changes.

### F4 — verified: how virtual MIDI ports enumerate

Probed on this machine, because it is easy to get backwards and the port readout
depends on it: a virtual `Output` created with `new Output(name, true)` is
published as a **source**, so it appears in `getInputs()` — that is the entry
rekordbox and Ableton read from. `getOutputs()` stays empty without real MIDI
hardware. On Windows the plugin *attaches* to a loopMIDI port, which is a real
destination and does appear in `getOutputs()`. `midi.status()` reports both.

---

## Batch 5 — pending

Raised by the module ports. Rekordbox and MIDI Control are DONE and verified
(`scripts/test_modules.mjs`, 48/48, diffing every constant against the legacy
sources); these are behaviour questions on top of working modules.

### D15 — full-screen hubs auto-enter State 4

**RULING — a screen declaring `fullScreenCapable` auto-enters State 4 on arrival,
and leaving restores the state you had before.** This is the one deliberate
exception to nav and state being orthogonal, and it earns it: arriving at the DJ
surface in the power-on State 0 put the numpad on top of the whole of Deck B, and
getting out meant four Button 36 long-presses before you could touch a deck. The
borrow is *remembered*, so it only rewinds a state it changed, and a manual
carousel cancels the rewind entirely.
* **D16** *(module: Rekordbox)* — the MIDI port name is hardcoded to
  "Adi RekordBox Controller". The legacy PI let you rename it, which is REQUIRED
  on Windows to match the loopMIDI port. It needs a home: global settings + a PI
  field, or a `~/.studioos` config the service reads.
* **D17** *(module: Rekordbox)* — the six encoder accumulators reset to
  127/64/64 on every restart; the legacy persisted them on an 800 ms debounce.
  The seam is built (`wirePersist` / `restore` / `snapshot`) but unwired, because
  global settings are now shared by every module and writing from one of them
  reintroduces the single-writer race the legacy fixed in 1.0.1.0.

### F5 — verified: half-open sockets inflate the client count

The running service reported 3 clients when 1 existed: a CEF page that goes away
without a clean close leaves a half-open TCP connection that never fires `close`.
That breaks the "silence every sounding note when the LAST client disconnects"
guarantee — the last real client leaving never looks like the last one. Fixed
with a 15 s ping/pong heartbeat that terminates a socket which misses its pong;
verified stable at 1 client across two cycles.

### Numpad, final

Settled over two hardware passes: `7 8 9 +` / `4 5 6 −` / `1 2 3 ⌫` /
`C 0 . ⏎`. Zero is centred under the 2/5/8 column like a real numpad, decimal
sits out on the right beside Enter, Clear is bottom-left.

---

## Status

| Module | State |
|---|---|
| Core engine + service | done, verified |
| Console (States 0/1/2) | done, verified |
| Rekordbox | done, verified — constants diffed against `src/midimap.js` |
| MIDI Control | done, verified — C++ helper eliminated |
| Visualizers | **working, 4 of 9 views.** See below |
| Ableton | **done.** 14 controllers, byte-identical copies |

**Visualizers, accurately.** The port arrived as constants + FFT + Analyzer with
3 of 9 view renderers, and — despite a long header comment describing the audio
chain — **no audio capture at all**, no meter computation and no frame pump. So
nothing it drew could ever contain a signal. Written by hand since: capture
(`getUserMedia` → `ScriptProcessorNode` → ring buffers, with every failure mode
painted on the surface), peak/RMS/correlation/balance, the `meters` view, the
frame pump, and both screens. Working views: **spectrum, scope, waveform,
meters**. Still un-ported: bands, rme, gonio, corr, bal — each paints a labelled
"not ported" tile rather than a blank key.

A `ScriptProcessorNode` is used instead of the legacy `AudioWorklet`: the worklet
needs its processor loaded from a blob URL and a CSP that permits it, which is
one more thing to be wrong on a user's machine for no audible benefit at 15 fps.
Swap in a worklet behind the same `push()` call if that ever changes.

**Ableton — the port that edited nothing.** All 14 legacy DeviceControllers draw
with a Canvas 2D context while Studio OS paints SVG. Rewriting each
`renderTouch()` would have meant editing 2,500 lines of layout code whose
parameter maps were verified one by one against Adi's real Ableton "Configure"
screenshots — the highest-risk, lowest-value change available.

So the CANVAS was ported instead of the controllers. `SOS.SvgCtx` implements the
exact Canvas 2D subset they use — 15 methods and 7 properties, established by
grepping every controller rather than guessing — and serialises to SVG. The
controller files are copied **byte-for-byte** into `js/ableton/` and
`scripts/test_ableton.mjs` asserts on every run that they still `diff` clean
against 1.5.9.0. A verified parameter map cannot be broken by a port that never
touches it.

The same shim is the strip compositor: one 1200×100 drawing, each dial handed a
`viewBox` window into it, so an EQ curve spans all six dials as one continuous
picture. Elements carry their x-extent so a zone only receives what it can see —
without that clipping each zone shipped the whole drawing at 17.5 KB, roughly
1.5 MB/s across six dials at 15 fps; it is now 2.9 KB worst case.

The `ws://127.0.0.1:9006` protocol to the AdiVST Remote Script is **unchanged**.

Both earlier gaps traced to the same cause: the porting run hit the org monthly
spend limit and 4 of its 6 agents failed. Ableton and the Visualizers completion
were then written by hand.

---

## Global rules as ruled (implementation contract)

| Control | States 0–3 | State 4 (Full Screen) |
|---|---|---|
| **Button 1** (0,0) | long = Back / level up · short = contextual select | released to the module |
| **Button 35** (7,3) | Clear in State 0/1; module elsewhere | released to the module |
| **Button 36** (8,3) | long = State Carousel · short = context action (on release) | long = escape only; short released to the module |
| **Dials 1–4** | active sub-plugin | active sub-plugin |
| **Dials 5–6** | overlay (States 0/1/3) · module in State 4 | active sub-plugin |
| **Cols 5–8** | overlay region (States 0/1/3) | active sub-plugin |
| **State 2** | full-device takeover — all 36 keys + all 6 dials | n/a |


---

## Batch 6 — the responsive pivot

Adi rejected two things at once: the Canvas-to-SVG shim under the Ableton
controllers, and the overlay model where a docked window hid the module beneath
it. Both are architectural, and between them they supersede D3, D8, D10 and D15's
rationale.

### L1 — Layout model

**RULING — breakpoint layouts.** A screen declares layouts at fixed column
widths and the engine picks the largest that fits, like CSS breakpoints. Every
layout is hand-designed, so nothing lands somewhere stupid by emergent accident,
and every one can be rendered in the preview sheet and tested.

### L2 — Rekordbox compact view

**RULING — both decks, 4 hot cues each.** Keeps the two-deck mirror that makes
the layout readable by feel; drops cues 5-8. *(Not yet implemented.)*

### L3 — Dial sharing *(superseded by L3a)*

**RULING — nav windows are keys-only; the module keeps all six dials.**

### L3a — Dials are borrowed, not shared *(supersedes L3)*

L3 did not survive contact with the delay calculator: a usable delay view needs
a BPM input and a division selector, and spending 2 of 16 keys on each leaves no
room for readouts.

**RULING — a window may declare `borrowDials: N`** and takes the first N dials;
the module keeps the rest. Windows borrow from the LEFT so the borrowed pair is
always dials 1-2 — one fixed place to look.

**PARKED by explicit instruction:** background modules are not yet told they have
fewer dials. They still answer for dials 1-2 and those answers simply are not
painted. Responsive module dials are the next piece of work.

### L4 — Native SVG for Ableton

**RULING — redraw natively, keep the parameter maps exactly.** Every
`renderTouch` becomes a native SVG emitter. The parameter maps, name regexes,
OVERRIDES tables and registry patterns carry across unchanged — that is verified
DATA, not drawing code. Visuals get re-verified against Live; mappings do not.
*(Not yet implemented — `SOS.SvgCtx` and the byte-identical copies are still in
place.)*

### L5 — Delay calculator is a viewport, not a table

The 24-cell grid was never requested and does not fit a 16-key dock.

**RULING — the Nick Fever model, one division at a time**, inside the standard
dock plus 2 borrowed dials:

```
dial 1  BPM              dial 2  note division (slides 1/1 … 1/128)

col:      0            1            2            3
row0   [ 1/8 ]      [ 143 ]      [ C0 ]       [ Oct 0 ]
row1    NORMAL       209.8 ms     4.77 Hz
row2    TRIPLET      139.9 ms     7.15 Hz
row3    DOTTED       314.7 ms     3.18 Hz
```

**Math is exact.** `triplet = normal × 2/3`, not the legacy 0.667 — which its own
comment admitted was "not exact 2/3" and drifts 2.2 ms on a 1/1 at 120 BPM.
Values are ROUNDED, never truncated: a 1/2 triplet at 120 BPM is 666.67 ms and
must read 667. The acoustic readout (A4=442) survives on the header row, so
nothing was dropped to make room.

### Superseded by this batch

* **D3 / D8** — the fixed cols 5-8 overlay and dials 5-6 ownership. Windows now
  dock 4 columns and borrow dials explicitly.
* **D10** — the delay grid's column offset. There is no 24-cell grid any more.
* **D15** — still in force, but now means "docks nothing" rather than
  "borrows the whole board from an overlay".
