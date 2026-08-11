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


---

## Batch 7 — dial side, and the first native controller

### L3b — Windows borrow the RIGHTMOST dials *(supersedes L3a's direction)*

L3a had windows borrow from the left, which put the calculator's operators at
the opposite end of the device from the dock they belong to.

**RULING — a window borrowing N dials takes the LAST N.** With N=2 that is
physical dials **5 and 6**, directly under the 16-key dock. Dials **1-4 stay with
the module**, globally. A window still addresses its own dials 1..N; the mapping
to physical 5-6 happens in `states.js`, so a window never learns where it was
docked. Modules read `States.moduleDials()` to pick their dial layout.

### L6 — Global dual-layout contract

**RULING — every module and controller from now on ships TWO hand-crafted
layouts:** a Full layout for the whole board, and a Compact layout for when a nav
window is docked. Not a reflow of the full one — a bespoke design.

Workflow, per controller: **(1)** present the Full layout for inspection,
**(2)** design the Compact layout together, **(3)** only then move on.

### L7 — EQ8 Compact Layout

Presented Full first. EQ8 owns **zero keys** in either layout — all 36 belong to
the Ableton hub shell.

**RULING — 4 dials, 4 fixed bands, no GLOB:**

| Dial | Band |
|---|---|
| 1 | Band 1 |
| 2 | Band 2 |
| 3 | Band 3 |
| 4 | **Band 6** |

* Modes **FREQ / GAIN / Q only** — the GLOB tab is dropped entirely in compact.
  No response graph, no Output Gain, no Scale, so all four dials stay strictly on
  bands.
* Bands are **fixed, not a sliding window** — no pagination arrows in compact.
* A GLOB mode carried in from the full layout falls back to FREQ rather than
  rendering an empty strip.

### L4 applied — EQ8 is now native SVG

First controller off the Canvas shim. `js/ableton/svg.js` provides native SVG
primitives (every one records its x-extent so the compositor can clip a zone to
what it can see), and `EQ8Controller.js` emits SVG directly through them.

Carried across **unchanged**, because it is verified data and not ink: the bridge
messages, band indices, `_BAND_RE` name resolution, filter-type classification,
the response model and every graph range.

The remaining 13 controllers are still byte-identical shim copies, and
`scripts/test_ableton.mjs` asserts that on every run — so their parameter maps
demonstrably have not drifted while EQ8 was rewritten.


---

## Batch 8 — per-controller dual layouts

Workflow from here on ("Discovery First"): before asking Adi to design a
controller's Compact layout, output a briefing covering (1) core purpose,
(2) what all 6 dials map to in Full, (3) whether it uses keys, (4) modes/tabs,
(5) what the screen shows. Adi does not memorise the legacy mappings; this
codebase is the memory base.

### L9 — GenericController compact: blind chop

**RULING — parameters 1-4 on dials 1-4, drop 5 and 6.** Deliberately the dumbest
rule available: this is a catch-all whose parameter choice is already arbitrary
(whatever the device exposes first), so ranking them for a small screen would
invent meaning that is not there. The generic logic is being overhauled later.

### L8 — Pulsar Massive compact: a fourth DRIVE tab

Four dials cannot hold four bands AND the centre section, but Drive and Gain are
too characterful to lose.

**RULING — add a `DRIVE` tab in compact only**, beside GAIN / FREQ / WIDTH:

| Tab | Dial 1 | Dial 2 | Dial 3 | Dial 4 |
|---|---|---|---|---|
| GAIN / FREQ / WIDTH | Low | Warmth | Presence | Air |
| **DRIVE** | Drive *(press = Auto Gain)* | Gain *(press = Transformer)* | HPF | LPF |

Nothing from the full layout is lost — it moves behind a tab. The filters become
dial-driven here because touch steppers would waste a whole zone. DRIVE exists
only in compact; carried into the full layout it falls back to GAIN, where dials
5-6 already hold the centre section.

### Native SVG progress (L4)

| Controller | Native | Compact |
|---|---|---|
| EQ8 | ✅ | ✅ bands 1/2/3/6, no GLOB |
| Generic | ✅ | ✅ blind chop |
| Pulsar Massive | ✅ | ✅ DRIVE tab |
| ProQ3, Spectre, Indeq, ValhallaRoom, ValhallaVintageVerb, Blackhole, HDelay, DbComp, Omnipressor, Saturate, SideMinder | ❌ shim copies | ❌ |

`scripts/test_ableton.mjs` asserts on every run that the not-yet-rewritten
controllers are still byte-identical to 1.5.9.0, so their verified parameter maps
demonstrably have not drifted.


---

## Batch 9 — Pro-Q 3, and the touch axis the port lost

### L10 — the touch Y coordinate is restored end-to-end *(engine-wide regression)*

Found while tracing ProQ3 for its Discovery briefing, and it was never a Pro-Q 3
problem — it broke every Ableton controller at once.

The legacy plugin forwarded both axes (`js/plugin.js:78`,
`Touch.touch(ctx, pos[0], pos[1], hold)`). Studio OS forwarded only x: the SDK
handler passed `pos[0]` alone, the dial-descriptor contract was `touch(x, hold)`,
and the Ableton module handed its controllers a hardcoded `0` for y. Every
hit-test in every controller is banded by y — `TAB [2,17]`, `MID`, `BOT [62,97]`
— so **no tab and no pill could ever be hit**, in EQ8, Pulsar or the 11 shim
copies. `scripts/test_ableton.mjs` missed it because it called
`onTouch(250, 50, false)` directly, bypassing the module wiring that supplied
the zero.

For Pro-Q 3 this was fatal rather than cosmetic: Shape, Slope and Stereo
Placement have no route except touch, so the plugin's defining feature was
unreachable.

**RULING — restore y end-to-end.** The dial-descriptor contract becomes
`touch(x, y, hold)`, `plugin.js` forwards `tapPos[1]`, and the Ableton module
stops substituting `0`. Exactly the legacy signature. Six modules implement
`touch`; the four that read `hold` take the signature bump, the rest already
ignored everything after x.

Guarded by a test that drives a tap through the **real** wiring
(`States.resolveDial(n).touch(...)`) rather than calling a controller directly,
because calling the controller directly is precisely what hid this for a whole
port.

### L11 — ProQ3 compact: bands 1 / 2 / 3 / 6

Presented Full first, rendered. ProQ3 owns **zero keys** in either layout.

Unlike EQ8 (which had to drop GLOB) and Pulsar (which had to grow a DRIVE tab),
ProQ3's zone artwork needs **no redesign** — each zone is still 200×100 and draws
identically. Its mode tabs are **per column**, not global, so there is no global
tab row to hang a fourth tab off; the Pulsar trick is structurally unavailable.
The only question was which four of six bands the dials address.

**RULING — dials 1-4 → bands 1, 2, 3, 6**, identical to the EQ8 compact ruling
(L7), so dial 4 is "the top end" across both EQ controllers and there is one
muscle memory rather than two. Bands are **fixed, not a sliding window** — no
pagination, exactly as L7.

Considered and rejected: *all bells* (B2/B3/B4/B5), which would have kept every
dial three-moded but put both cuts out of reach; *spread* (B1/B3/B4/B6), which
matched no existing precedent; *paginate*, which L7 already ruled out.

The known cost of this choice — Pro-Q 3's cut bands expose no Gain and, being
cuts, no Q either, so dials 1 and 4 are FREQ-only — is answered by L12 rather
than accepted.

### L12 — a single-mode band's dial press steps its SLOPE *(changes Full too)*

On a band whose Shape allows only FREQ (Low Cut, High Cut), `onDialPress` cycled
a one-element mode list and therefore did nothing at all — in both layouts.

**RULING — when a band has only one available mode, its dial press steps the
Slope instead.** Zero cost, because there is no second mode to cycle to, and
Slope is the control that actually matters on a cut. This applies in **Full as
well as Compact**: it is a change to the full layout, deliberately, not a
compact-only special case. Bands with two or more modes are untouched — press
still cycles the mode.

### Derived, flagged for veto

* **Modes are keyed by BAND, not by dial slot.** `_mode[slot]` was indexed by
  column; slot == band-1 in Full so the two were the same thing, but the moment
  dial 4 means band 6 the layouts would alias each other's modes. Invisible in
  Full, correct in Compact.
* **A mode carried in from Full falls back to FREQ** when the compact band's
  shape cannot offer it — the same guard EQ8 uses for GLOB, and the same guard
  the legacy `_validateModes` already applied on every state change.

### Native SVG progress (L4), updated

| Controller | Native | Compact |
|---|---|---|
| EQ8 | ✅ | ✅ bands 1/2/3/6, no GLOB |
| Generic | ✅ | ✅ blind chop |
| Pulsar Massive | ✅ | ✅ DRIVE tab |
| **ProQ3** | ✅ | ✅ bands 1/2/3/6, press = Slope on cuts |
| Spectre, Indeq, ValhallaRoom, ValhallaVintageVerb, Blackhole, HDelay, DbComp, Omnipressor, Saturate, SideMinder | ❌ shim copies | ❌ |


---

## Batch 10 — Spectre, and a workflow rule

### P3 — Chain the next Discovery briefing *(workflow)*

**RULING — finishing a controller must not end in a turn that only asks whether
to continue.** When a controller is committed, the **next** controller's
Discovery briefing goes at the bottom of the same message, unprompted, rendered
picture included. A round-trip that carries no information costs context for
nothing.

The *briefing* chains; the *implementation* does not. Work still stops at the
Compact-layout ruling, which is Adi's. Recorded as protocol #3 in
`docs/CONTINUE.md` so it survives the handoff.

### L13 — Spectre compact: a fourth GLOB tab, 4 widespread bands

Presented Full first, rendered in all three modes. Spectre owns **zero keys** in
either layout.

The squeeze: **5 bands + 1 globals zone is exactly 6**, so four dials must drop
two — and unlike ProQ3 the candidates are not the same kind of thing. Losing the
globals costs Output, Mix and Mode (Mix especially, since it is how the whole
effect is dialled back); losing bands costs the mid detail the plugin is bought
for. Spectre, unlike ProQ3, has a **strip-wide** tab row, so the Pulsar trick
(L8) is structurally available here.

**RULING — reuse the fourth-tab pattern. Nothing is lost; the globals move
behind a tab.**

| Tab | Dial 1 | Dial 2 | Dial 3 | Dial 4 |
|---|---|---|---|---|
| GAIN / FREQ / Q | Lo Shelf | Peak 1 | **Peak 3** | Hi Shelf |
| **GLOB** | Output | Mix | Mode | — (spacer) |

* Bands in compact are **Lo Shelf / Peak 1 / Peak 3 / Hi Shelf** — the widespread
  set. **Peak 2 is dropped**, keeping the outer two peaks so coverage spreads
  rather than clustering in the low-mids.
* Bands are **fixed**, no pagination — consistent with L7 and L11.
* `GLOB` exists **only in compact**; carried into the full layout it falls back
  to GAIN, where dial 6 already holds Output and Mode.

Rejected: dropping the globals outright (Mix unreachable while docked); keeping
the globals zone and dropping two peaks (loses the plugin's whole point);
hanging the globals off dial presses (the presses already mean Switch, and
nobody would ever find them).

### Derived, flagged for veto

* **In GLOB, dial 3's press cycles Mode forward** — the same gesture as dial 6's
  press in the full layout, so "press to cycle Mode" means one thing in both.
  Redundant with turning dial 3, deliberately.
* **In GLOB, dial 4 is a read-only band-status readout**, not a blank: five dots
  showing which bands are switched on. Adi ruled "unmapped/empty or a visual
  spacer"; this claims no control, it just stops the zone being a black hole and
  answers the one question the compact layout can no longer show — what happened
  to Peak 2.
* **Touch in GLOB** mirrors the full globals zone: MIX steps from its own ◂ ▸
  row, MODE cycles from its pill (hold = backwards). Output is dial-only, as it
  is in full.

### L13a — the two GLOB derivations, ratified

Adi explicitly approved both items that L13 flagged for veto: **dial 3's press
cycles Mode** in GLOB (consistent with dial 6 in full), and **dial 4 carries the
five-dot band-status readout** rather than a blank. They are rulings now, not
derivations.

### L14 — INDEQ compact: drop the frequency steppers, stay stateless

Presented Full first, rendered. INDEQ owns **zero keys** in either layout.

INDEQ is the odd one out: a fixed 3-band EQ with **no dynamic state at all** — no
modes, no tabs, no focus, no pagination. Six knobs on six dials, six toggles on
the touch rows, permanently. That rules the fourth-tab pattern (L8 / L13) out on
principle rather than on space: Pulsar and Spectre already had a strip-wide tab
row to extend, and INDEQ has none. Adding one would invent a mode concept the
plugin does not have.

**RULING — dials 1-4 are Low Gain, Mid Gain, High Gain, Output.** The two
stepped corner-frequency dials (Low Freq, Mid Freq) are dropped.

Rationale, Adi's: the corner frequencies on a fixed-frequency EQ are set-and-
forget setup decisions, while the three gains plus Output are exactly what a hand
reaches for mid-listen — which is the situation compact exists for.

* **Dial presses keep mirroring their zone's top toggle**, exactly as in full:
  HPF · Mid Bandwidth · High Band Shape · Bypass.
* **No tabs, no pages, no hidden state.** Compact is the same four zones drawn
  verbatim — the zone artwork is not redesigned, it is selected.
* Because the zones are carried whole, High Gain keeps its bottom row too, so the
  **8/16 kHz High Frequency switch survives**. Of the six toggles only **Low Band
  Shape** is lost, because its zone (Low Freq) is one of the two dropped.

Rejected: dropping High and Output to keep two complete bands (loses the high
band and three toggles); a shared "frequency of the last band you touched" dial
(adds focus state to a deliberately stateless controller); inventing a tab.

### L15 — ValhallaRoom compact: keep all four pages, lose the PRESET half

Presented Full first, rendered on all four pages. ValhallaRoom owns **zero keys**
in either layout.

The first reverb, and the first controller that was **already paged** — 19
continuous parameters were folded onto 6 dials long before compact existed. So
the compact question was never "what do we drop"; it was "does the existing
paging stretch one notch further". It does.

The genuinely new problem was the **global bar**: the only full-width element any
controller has, and at 800 px it is sliced through the middle, leaving MODE in
the module's half and PRESET stranded in the borrowed half where it can never be
drawn or touched.

**RULING — V1 + V3.**

* **All four pages survive** (MAIN / EARLY / LATE / RT). Each uses the **first
  four parameters of its own page**; dials 5 and 6 are dropped.
  - MAIN loses Diffusion and Early/Late Mix
  - EARLY loses Early Send (and its repeat of Mix)
  - LATE and **RT survive whole** — RT is exactly four parameters
* **Mix and Decay stay reachable everywhere**, because they already repeat on the
  deeper pages. Adi's reasoning: riding Mix and Decay is the frequent move when
  dialling dense spatial atmospheres; Diffusion and Early Send are not.
* **The PRESET half of the bar is dropped in compact**, and MODE expands to span
  the full compact width. ValhallaRoom does not reliably expose a preset
  parameter at all, so compact was about to spend half a bar rendering
  "— (not exposed)". MODE gets a target twice the size instead.
* **Press behaviour is unchanged: any dial advances the page.** No per-dial press
  action exists in either layout.

Rejected: re-paging into five pages so nothing is lost (splits MAIN in two, and
the page you land on stops being "everything important"); turning the bar into a
fifth GLOB page (the bar is already touch-driven and works — converting it to
dials is motion without gain).

### L16 — VintageVerb compact: three pages kept, and the bar KEEPS both halves

Presented Full first, rendered on all three pages. VVV owns **zero keys** in
either layout.

Architecturally this is ValhallaRoom again — paged reverb, press-any-dial to
advance — so L15 mostly transfers. **One thing does not.** ValhallaRoom's
right-hand bar slot was an unexposed Preset that printed "— (not exposed)",
which is exactly why dropping it in compact (V3) cost nothing. VVV's right-hand
slot is **ColorMode**, a real exposed quantized selector, and on this plugin the
era voicing (`seventies` / `eighties` / `now`) is one of its most characterful
controls. Reusing V3 here would throw away a live control on the strength of a
precedent that was only ever about a dead one.

**RULING — W1.**

* **All three pages survive** (MAIN / DAMP / SHAPE), each yielding its **first
  four parameters**; dials 5 and 6 are dropped.
  - MAIN loses High Cut and Low Cut
  - SHAPE loses Mod Depth and Size
  - **DAMP survives functionally whole** — its four unique parameters are exactly
    dials 1-4, and only its Decay/Mix *repeats* go
* **The bar keeps BOTH halves**, split at the compact width: MODE left, COLOR
  right, ~394 px each — still wider than a whole dial zone, and ample for
  "Chorus Space" and "seventies". Both stay fully touch-interactive.
* **Press behaviour unchanged: any dial advances the page.**

Adi's reasoning: the thematic integrity of the three pages matters more than
holding on to setup parameters like High Cut / Low Cut, and 394 px is a massive
touch target.

Implementation consequence worth recording: because the bar splits at the
CURRENT width rather than a hardcoded 1200, the compact bar needs no special
case at all — the same code draws and hit-tests both layouts. VVV is the
simpler controller of the two precisely because nothing had to be dropped.

Rejected: stacking MODE over COLOR as two half-height rows (~16 px each, tight
for no gain); one bar that toggles between them (adds hidden state to a bar that
has none); re-paging to four pages so nothing is lost (VVV's pages are thematic —
damping vs shape — and splitting them makes the themes fuzzy).

### L17 — Blackhole: re-paged to 3 × 4, and FULL gives up two dials

Presented Full first, rendered on both pages. Blackhole owns **zero keys** in
either layout.

Two things made this different from the Valhallas. **Nothing repeats across
Blackhole's pages** — each of the 12 dial parameters appears exactly once, so
there is no "Mix is always reachable" safety net and anything dropped is
genuinely gone until you change page. And the bar has **four** cells rather than
two, so at 800 px the 300 px cells stop tiling: HOTSWITCH is sliced in half and
TEMPO lands entirely in the borrowed region.

**RULING — B3, and it changes the FULL layout too.**

* **Re-paged to three pages of exactly four**, which 12 parameters divide into
  with no remainder:
  - **MAIN** — Mix · Gravity · Size · Predelay
  - **MOD** — Mod Depth · Mod Rate · Feedback · Resonance
  - **LEVELS** — In Level · Out Level · Low EQ · Hi EQ
* **In FULL, dials 5 and 6 are deliberately left unmapped.** This is the point,
  not a side effect: Adi wants the plugin to feel **100 % identical in both
  layouts, with zero hidden parameters**. Consistency of workflow beats filling
  all six dials.
* **The bar keeps all four cells in both layouts**, scaled to the current width —
  300 px each at full, 200 px each at compact. All four switches are live
  controls; nothing here is dead the way ValhallaRoom's PRESET was.
* **Pressing ANY dial advances the page** (MAIN → MOD → LEVELS → MAIN),
  including the two unmapped dials in full.

This is the first ruling that deliberately spends hardware rather than
information — and the first time a Compact layout has driven a change back into
the Full one. Worth remembering as a precedent when a later controller divides
evenly into fours.

**Derived, flagged for veto:** an unmapped zone in full paints a dim
`press = page` rather than a bare em-dash, so an empty dial reads as intentional
instead of broken, and still announces the one thing it does. `dialTitle` returns
the same string (a *borrowed* dial still returns `''`, so the two states stay
distinguishable).

Rejected: rescaling the bar and keeping two pages of six (B1 — costs Low/Hi EQ
and In/Out Level with no repeats to fall back on); dropping TEMPO in compact (B2
— TempoSync changes what Predelay means, so it is not a dead slot).

### L18 — H-Delay compact: drop the filters, keep both steppers

Presented Full first, rendered. H-Delay owns **zero keys** in either layout.

The barest controller in the set: six Configured parameters on six dials, 1:1,
with **no tabs, no pages, no bar and nothing repeated** — less structure even
than INDEQ, which at least had toggle rows. So none of the established tricks
apply: there is no tab row to extend (Spectre), no page to add (Blackhole), no
dead slot to reclaim (ValhallaRoom), and no repeat to fall back on.

**RULING — H2. Dials 1-4 are Mix, Delay BPM, Feedback, PingPong.** HiPass and
LoPass are dropped.

Adi's reasoning, and it is L14's applied again: riding the note division and the
ping-pong routing is performance-critical for leads and rhythmic atmospheres,
while the two filters are purely mix cleanup — a set-and-forget setup task.

* **All three interactions survive on both stepped dials** — turn, tap the zone,
  press the dial; hold-tap still steps backwards.
* **No tabs, pages or hidden modes are invented.** Compact SELECTS zones, exactly
  as INDEQ does (L14): `COMPACT_SLOTS` indexes the same `DIAL` table, so a
  compact zone IS the full zone and nothing can drift between them.
* Keeping PingPong also keeps **both** stepped dials — the richest interactions
  on the controller. Dropping it would have left compact with three plain
  continuous dials and one stepper.

Rejected: the blind chop (H1 — loses the routing mode, which changes what the
delay *is*); pairing the filters onto one dial with a press to swap (H3 — invents
a hidden mode on a controller with none); re-paging 2×4 Blackhole-style (H4 —
Adi: "re-paging just to save two filters and ending up with a half-empty page is
bad architecture". L17 was honest because 12 divides by 4 exactly; 6 does not).

### L19 — dBComp compact: the four knobs you ride, no switches

Presented Full first, rendered. dBComp owns **zero keys** in either layout.

The first dynamics processor in the run. Fixed panel, no paging, and one
structural oddity: **zone 6 is not a knob**. It is a two-pill switch panel where
the dial and the touch zone address different parameters — turn or tap-top for
Oversampling, press or tap-bottom for Bypass. One zone, two controls, four
gestures.

**RULING — D2. Dials 1-4 are Threshold, Compression, Output, Mix.** HPF and the
entire switch zone are dropped.

Adi's reasoning: "If I load a compressor, I'm actively using it" — Bypass is one
click away in Ableton's own device header, Oversampling is a set-once quality
toggle, and HPF is a sidechain setup decision. Retaining **Mix** for parallel
compression matters more than any of them. That leaves the classic four-knob
compressor surface, which is exactly what compact should be.

* **No tabs, pages, hidden gestures or press-to-bypass are invented.** Compact
  SELECTS zones, as INDEQ (L14) and H-Delay (L18) do.
* Because the switch zone is simply never selected, its dial/press/touch
  behaviour survives untouched in full without a single branch in compact.

Rejected: keeping the switch zone and dropping Mix (D1 — costs parallel
compression, and Bypass already has a good home outside the deck); Threshold /
Compression / Mix / switches (D3 — drops makeup gain); folding Bypass onto a
knob's press (D4 — a hidden gesture, the thing already rejected twice).

### L20 — Omnipressor: re-paged 3×4 (L17 again), and a 3-cell compact bar

Presented Full first, rendered on both pages. Omnipressor owns **zero keys** in
either layout.

Two problems at once. **Five bar cells do not divide into four zones** — at
800 px the 240 px cells stop tiling, slicing LINE and stranding POWER — and
unlike Blackhole the dials offered no escape either: 11 unique knobs will not fit
two pages of four.

**RULING — O2 for the dials, O3 for the bar.**

**Dials — re-paged to three pages of exactly four**, the L17 move repeated. The
arithmetic is nearly as clean as Blackhole's: 11 unique knobs, and `Function`
(the signature EXP↔COMP ratio) already repeated, so three pages of four cover
everything with `Function` appearing twice:

  - **MAIN** — Threshold · Attack · Release · Function
  - **LIMITS** — Atten Limit · Gain Limit · Mix · Function
  - **I/O** — Input Gain · Output Gain · In Level · Out Level

**In FULL, dials 5 and 6 stay unmapped on purpose**, carrying the dim
`press = page` hint established in L17. Adi: "Consistency and preserving the
exact same layout across both Full and Compact modes is paramount." Second time
this trade has been made deliberately.

**Bar — compact drops POWER and LINE**, leaving BASS · METER · SC re-tiled to
~266 px each; full keeps all five at 240 px. Adi handles bypass in Ableton, and
LINE is a routing setup switch rather than something to ride.

**Noted tension, accepted:** the dials are now identical across layouts while the
bar is not. That is deliberate — the dial pages are a *workflow* the muscle
memory depends on, whereas the bar is a set of independent switches where
dropping two costs nothing but reach. It is the first controller where the two
halves of the strip follow different compact rules, so it is worth remembering
that this was chosen rather than overlooked.

Rejected: keeping two pages and rescaling the bar to five 160 px cells (O1 —
loses Mix, and 160 px would be the narrowest target in the project); a 4-cell bar
keeping POWER (O4 — POWER duplicates Live's own device on/off).

### L21 — Saturate compact: the clipper trio plus Output

Presented Full first, rendered. Saturate owns **zero keys** in either layout.

The bar was not the problem here — three cells tile any width cleanly (400 px →
266 px). The whole question was which four of the six continuous knobs, which
split naturally into a **character trio** (Drive · Shape · Detail) and **three
level controls** (Input · Output · Out Comp).

**RULING — S1. Dials 1-4 are Clipper Drive, Clipper Shape, Clipper Detail,
Output Level.** Input Level and Output Compensation are dropped.

The three clipper knobs are what Saturate *is*; splitting them would be like
dropping one of Pro-Q's modes. Input goes because on a clipper, pushing Input and
pushing Drive do nearly the same job, and Drive is the one with the metering
behind it. The bar keeps all three cells in both layouts.

**Flagged, not blocking — Adi runs OUT MODE = Automatic.** Asked which mode he
uses, the answer was "automatic", and he chose S1 anyway with that in hand. Worth
recording because the two interact: with Output Level Select on Automatic the
plugin computes output itself, so the Output dial compact keeps may read as
inert on hardware. If it does, swapping it for Input is a one-line change to
`SaturateController.COMPACT_SLOTS` — `[1,2,3,4]` becomes `[0,1,2,3]` — and
nothing else moves. Left as ruled rather than second-guessed.

Rejected: Input / Drive / Shape / Detail (S2 — the "everything before the
clipper" set, which is also what a blind first-four chop happens to give); any
re-paging (6 into 4 leaves a 2-slot page, the split already rejected for
H-Delay in L18).

### L22 — SideMinder: Full stays perfect, compact accepts orphans

Presented Full first, rendered on all three pages. SideMinder owns **zero keys**
in either layout. The hardest compact of the run, and the last shim copy.

Two things fought each other. Every page is built from **L/M/H triads**, so a
first-four chop *splits* one — LIMIT compact keeps `L-Ratio` with no M or H
beside it. But the alternatives all cost more: re-paging 4×4 drops a whole triad
(the Releases) and spends two Full dials, and the zero-loss six-page version
squeezes the tab row to **32 px per tab**, which is unusable.

**RULING — M1 for the dials, M2's reduction for the bar.**

* **Three pages kept** (WIDTH / LIMIT / TRIM), each yielding its **first four**
  parameters in compact. Dials 5 and 6 are dropped there.
* **The FULL layout is untouched** — all six dials used, all 18 parameters
  reachable, Release triad intact. Adi: keeping Full perfect and retaining the
  Release controls matters more than avoiding orphans in compact, and the 32 px
  tabs were a non-starter.
* **Orphaned `L-Ratio` and `L-Trim` in compact are accepted, explicitly.** This
  is the first time a compact layout deliberately shows part of a group — worth
  recording as a considered trade rather than an oversight.
* **Compact bar drops BYPASS and EXT SC**, keeping BANDS · LINK · MONO · DELTA
  at exactly 200 px each; full keeps all six at 200 px. MONO and DELTA are the
  two you ride on a width tool — mono-compatibility checking and hearing only
  the side processing. Bypass is handled in Live; EXT SC is routing setup.

Note this is the mirror image of L17/L20: there, Full gave up dials to keep the
layouts identical. Here Full is held perfect and compact takes the compromise.
Both are legitimate; the difference is which side the plugin's value sits on.

Rejected: 4 pages × 4 dropping the Release triad (M2's dials); 6 pages of triads
with I/O Trim on dial 4 (M3 — zero loss, unusable tabs); 3 pages with a
hand-picked four (M4 — wastes a dial on two of three pages).

### L4 COMPLETE — every controller is native SVG

SideMinder was the last byte-identical copy. `SOS.SvgCtx`, the Canvas-2D shim
written so the port could avoid editing 2,500 lines of verified parameter maps,
now has no controllers left to serve. The byte-identity assertion in
`scripts/test_ableton.mjs` is replaced by its inverse: **no file in `js/ableton`
may still match 1.5.9.0**, which is now the stronger claim.

All 14 controllers ship both layouts. Every parameter map crossed the port
unedited.

### Native SVG progress (L4) — final

| Controller | Native | Compact |
|---|---|---|
| EQ8 | ✅ | ✅ bands 1/2/3/6, no GLOB |
| Generic | ✅ | ✅ blind chop |
| Pulsar Massive | ✅ | ✅ DRIVE tab |
| ProQ3 | ✅ | ✅ bands 1/2/3/6, press = Slope on cuts |
| Spectre | ✅ | ✅ GLOB tab, bands Lo/P1/P3/Hi |
| Indeq | ✅ | ✅ gains + Output, steppers dropped |
| ValhallaRoom | ✅ | ✅ 4 pages × first 4 dials, MODE-only bar |
| ValhallaVintageVerb | ✅ | ✅ 3 pages × first 4 dials, bar keeps MODE + COLOR |
| Blackhole | ✅ | ✅ identical to full — 3 pages × 4, 4-cell bar |
| HDelay | ✅ | ✅ Mix / Delay / Feedback / PingPong, filters dropped |
| DbComp | ✅ | ✅ Thresh / Comp / Output / Mix, switch zone dropped |
| Omnipressor | ✅ | ✅ 3 pages × 4 (same as full), bar drops POWER + LINE |
| Saturate | ✅ | ✅ Drive / Shape / Detail / Output, 3-cell bar kept |
| **SideMinder** | ✅ | ✅ 3 pages × first 4, bar drops BYPASS + EXT SC |


---

## Batch 11 — the V3 NAV refactor

A set of core system changes prepared in another session. Four of them collided
with rulings already in force; Adi resolved all four before any code was written.

### V1 — the carousel gains a NAV-OFF position *(clarifies L17-era State 4)*

The refactor brief said NAV cycles "States 0-3", which would have left the module
permanently at 5 columns — and Rekordbox at 5 columns loses half its hot cues
(L2), while the Root Hub, the Ableton hub, MIDI Control and the Visualizers all
declare 9-column layouts.

**RULING — the cycle is `0 → 1 → 2 → 3 → OFF → 0`.** State 4 survives as the
NAV-OFF position: NAV hides completely and the active module reclaims all 36
keys. Nothing else about it changes.

### V2 — Button 36 is a plain key *(supersedes D2a, D9, D9a)*

**RULING — Button 36 loses every engine role**, exactly like Button 35. It is a
standard single-trigger `KeyUp` button, globally.

The reason D9/D9a existed at all was Rekordbox's *held* Nudge ▶▶ Deck B at (8,3),
which needed Note On at press and Note Off at release, and which forced a 500 ms
cap so the carousel could still open. Adi has ruled — with the Pioneer Omnis-Duo
as the reference — that **(8,3) should be a standard Beat Jump, not a continuous
nudge**. With the held gesture gone, the whole special case dissolves: no timer,
no forced Note Off, no hanging-note risk, no `max 0.5 s` caption.

D2a's Button 36 long-press is therefore removed, and with it the last reason
`bindingKind()` existed for that key.

### V3 — the NAV trigger moves to the right-most dial

**RULING — a long press (500 ms) on dial 6 cycles the NAV state.** It is the
only state gesture, and it works in every state including OFF, so NAV can always
be recalled. Dial 6's *short* press consequently resolves on release rather than
on press; dials 1-5 are untouched and stay immediate.

### V4 — NAV dial boundaries, per state *(corrects the brief; refines L3b)*

The brief's "NAV must never touch the dials" was overly broad and would have made
all 14 Compact strip layouts unreachable. The real boundary is per state:

| State | Keys | Dials |
|---|---|---|
| 0 Numpad | 16-key dock | **none** — strip untouched |
| 1 Calculator | 16-key dock | **none** — strip untouched |
| 2 Time Divisions | 16-key dock | **1** (dial 6) for BPM |
| 3 Context | 16-key dock | **2** (dials 5-6) |
| 4 NAV OFF | none | none |

**State 3 is what triggers the Compact layouts.** Borrowing two dials leaves the
module four, which is precisely the `build(4)` path every Ableton controller now
implements. The compact work is not dormant — State 3 is its consumer.

### V5 — State 0 keeps its layout; `C` becomes `✱` *(amends D5)*

Bottom-left is now an asterisk. Everything else about the numpad stands.

### V6 — State 1 rebuilt: a display row and merged keys

The operators no longer live on borrowed dials (V4), so they move onto the keys
via long-press, and the freed top row becomes a real display.

* **Top row (4 keys) is the display.** It starts at `0` on the left and grows
  rightwards, three characters per key, so a twelve-digit result reads across the
  row as one number.
* **Merged keys — short press / long press:**
  `0` / **BACK** · `.` / **−** · `C` / **+**
* `=` replaces Enter. Digits, `×`, `÷` and `⌫` are unchanged.

### V7 — State 2 rebuilt: three variant rows *(supersedes L5)*

L5's one-division viewport is gone, along with its two borrowed dials.

```
col:      0            1            2            3
row0   [ NOTES ]   [ DOTTED ]  [ TRIPLETS ]  [ 104.90 ms ]   <- value, tap = ms/Hz
row1   [ 1/8 ]     [ 1/16 ]    [ 1/32 ]      —               <- straight
row2   [ 1/8 D ]   [ 1/16 D ]  [ 1/32 D ]    —               <- dotted
row3   [ 1/8 T ]   [ 1/16 T ]  [ 1/32 T ]    [ BPM 143 ]
```

* The three top keys are **cycle buttons, not modes**: tapping any of them slides
  the visible three-division window (1/8-1/32 → 1/4-1/16 → … → 1/1-1/4), hold
  slides it back. Each one labels the row it belongs to.
* Default window is 1/8-1/32 and the default selection is **straight 1/16**.
* The value key shows the selected cell and **toggles ms ⇄ Hz** on tap.
* **BPM sits bottom-right** and is turned with dial 6; tapping it resets.

**Math is exact and rounds only at the text layer.** `ms = 60000 / BPM` for a
quarter, scaled by `4 / denominator`; `triplet = straight × 2/3`;
`dotted = straight × 3/2`; `Hz = 1000 / ms`. No `Math.round` anywhere in the
computation — output formatting is **2 dp for ms, 4 dp for Hz**.

### V8 — State 3 is an empty context shell

**RULING — the breadcrumb fallback is removed.** State 3 is a 16-key shell that
the active module fills, plus two borrowed dials. A module with nothing to say
leaves it empty rather than painting a nav-tree readout. Ableton is the first
consumer and keeps its track/device strip.

### V9 — the key aesthetic

**RULING — kill the programmer UI.** Keys are now a soft raised face (vertical
gradient with a hairline top edge), and **active keys use a tinted face plus an
inner glow** rather than the old 4 px accent ring. Type is Inter/SF Pro with real
weight separation: the number is heavy, units and captions are small and quiet.

**Future-proofing:** every label is a single `<text>` node at a known anchor, so
V3's image swap is a node-for-node replacement with no layout consequences.


---

## Batch 12 — the first hardware pass

V3 shipped to the device and Adi tested it. Three things came back.

### V10 — an active key has NO outline at all

V9 replaced the 4 px accent ring with a tinted face, an inner glow AND a 1.5 px
tinted rim. On hardware the rim still read as "a green border".

**RULING — the perimeter is untouched.** An active key is lit purely from
within: the face tints toward the accent, a wider/brighter radial glow sits under
the label, and the top hairline brightens. Nothing is stroked. It reads as an
illuminated cap rather than a selected element.

### V11 — Time Divisions, transposed and de-cluttered *(supersedes V7's grid)*

V7 put the variants on ROWS and the divisions on COLUMNS, and printed the
computed time inside all nine cells. On the device that was unreadable.

**RULING — columns are the variants, rows are the divisions, and the nine cells
carry their fraction and nothing else:**

```
col:      0          1            2            3
row0   [ NOTES ]  [ DOTTED ]  [ TRIPLETS ]  [ 104.90 ms ]   tap = ms/Hz
row1   [ 1/8 ]    [ 1/8 D ]   [ 1/8 T ]     [ ▲ ]
row2   [ 1/16 ]   [ 1/16 D ]  [ 1/16 T ]    [ ▼ ]
row3   [ 1/32 ]   [ 1/32 D ]  [ 1/32 T ]    [ BPM 143 ]
```

* Row 0's three labels are **static column headers** — they carry no action at
  all. V7 had made them cycle triggers; that is gone.
* **▲ / ▼ shift the whole 3×3** through the division table. They **clamp** rather
  than wrapping, and grey out at the ends of their travel: an arrow that silently
  jumps from 1/1 back to 1/128 is worse than one that visibly stops.
* The computed value appears in **exactly one place**, the top-right key.

### V12 — the calculator display is grouped, not chopped

Three fixes from the same pass.

**Arithmetic.** `277 + 5` came back `2775`. The engine was provably correct in
isolation — driven through the real bindings it returns 282 — so the fault was
that the `+` never reached it: the operator lived on a long press captioned
"hold +" in small grey type and was effectively invisible on the cap. Both ends
are fixed: **operands are coerced to Number inside `applyOp`** (belt and braces —
`+` is the one operator where a stray string succeeds quietly instead of
failing), and the caption is now **`HOLD +` in promoted amber type**.

**Grouping.** Numbers are formatted with thousands separators and split ON those
separators, so the break lands where a reader expects it:
`12000 → "12," | "000"`, not `"120" | "00"`.

**Resting state.** The row shows a dim `0. 000 000 000` across all four keys, so
it is obviously one wide screen rather than a lone digit on the left.

**Key geometry.** Each display key gives its top ~28 % to a centred operator
glyph above a hairline, and the rest to the number at display size.

### Field note — glyphs outside the proven set render as tofu

`⌷` (U+2337) was used as a "hold" mark and came out as an empty box on the
device. The key font is not guaranteed to carry anything beyond the set already
in use. `scripts/test_console.mjs` now pins the caption to that set.


---

## Batch 13 — scrapping State 3, and the Omnis-Duo

Adi reviewed a conflict briefing covering all three phases and returned one
unified ruling for every open question at once. Five of the eight items below
contradicted a rule already in force; none was decided here.

**Workflow change (P4).** Conflicts are presented as PLAIN OUTPUT and the session
then stops. No interactive multiple-choice menu: Adi wants to see every conflict
side by side and rule on them as one coherent set, because a menu fragments a
decision he prefers to make whole.

### V13 — State 3 is scrapped; the cycle is `0 → 1 → 2 → OFF → 0`

**RULING — State 3 (Context) is removed entirely**, superseding V8 and amending
V1's five-position carousel.

An empty global shell was the wrong home for module sub-menus. A module that is
full-screen owns all 36 keys and can present its own; a *global* state that every
module has to fill is a shell that most modules leave blank. NAV OFF keeps its
job unchanged and simply moves from index 4 to index 3.

Removed with it: `States.wireContext`, the `contextProvider` seam, the breadcrumb
fallback in `modules/index.js`, and the four per-module `context` screens
(ableton, rekordbox, midictl, viz).

**Two engine bugs surfaced doing this**, both invisible until the index moved:

* `input.js` compared `getState() !== 4` to decide whether Button 1 was
  reserved. With NAV OFF at 3 that silently un-reserved Back in *every* state.
  The hook is now `isFullScreen()` — it asks the question instead of comparing
  the index, so the number can never rot again.
* `setState()` stored any integer it was handed. `setState(4)` on a 4-position
  carousel produced a state with no name, no dock width and no window, and every
  later carousel step was offset by one. Out-of-range is now refused and logged.

### V14 — State 2 is the Compact consumer, and 0/1 pass through by taking nothing

The brief asked for the Compact strips to "pass through" onto States 0 and 1.
They already do — and as FULL, which is strictly better: States 0/1 borrow no
dials, so the module keeps six and `composite()` calls `build(6)`. Painting a
4-dial Compact layout there would have blanked two live dials and left them dead
(`hub.dials()` returns a faceless zone past `lastZones`, with no `rotate`).

**RULING — States 0 and 1 keep taking NOTHING, which IS the pass-through. State 2
borrows TWO dials (physical 5 and 6) and becomes the sole consumer of the 14
Compact layouts.** V4's table is amended: State 2 goes from one borrowed dial to
two.

This also closes a latent hole nobody had hit: State 2's single borrowed dial
made `moduleDials()` return 5, so every controller was being asked for a
`build(5)` that L6 never commissioned. There is no 5-zone case left.

### V15 — State 2 gains a readout, a format dial and a PASTE key

The second borrowed dial has to earn its place. It does three jobs:

* **Dial 5 turn** — scrolls the division grid, same clamp and direction as ▼.
  The ▲/▼ keys are unchanged; the dial duplicates them, it does not replace them.
* **Dial 5 push** — toggles ms ⇄ Hz. This moves OFF the value key (amends V11).
* **Dial 5 face** — the computed figure at display size in green, with its
  division above and its unit below. `R.valueZone()`; the figure auto-fits rather
  than truncating, because a 1/1 at 60 BPM is `4000.00` and Hz runs to eight
  characters.

**The value key now TYPES.** Tapping the top-right key sends the figure to the
focused application through a new `os.type` verb — the point of computing a delay
time on a device that sits next to the keyboard. The unit is NOT typed; a plugin
field wants the number.

`os.type` **whitelists** its payload to digits, dot and minus rather than
escaping it. Nothing that survives the filter can break out of the AppleScript
string literal it is interpolated into, so there is nothing left to escape.
Verified through the real socket: `abc"; do shell script "x` filters to empty and
is refused.

The green is `#39d353` — the green the rekordbox hot cues already ship. On a
device where the proven set is the safe set, a new colour is a new risk for
nothing.

### V16 — the rekordbox Omnis-Duo skin *(module-local, deviates from V9/V10)*

**RULING — rekordbox may look like the hardware it replicates rather than like
Studio OS.** Explicit permission to deviate from the global key aesthetic, scoped
to this one module. Four sampled hex codes, and nothing outside `rekordbox.js`
may read them:

| | |
|---|---|
| chassis / canvas | `#1a202c` |
| performance pad | `#232d3d` |
| circular transport recess | `#121822` |
| muted printed lettering | `#64748b` |

* **Hot cues are lettered A-D over E-H, identical on BOTH decks**, exactly like
  the Omnis-Duo. The deck letter leaves the cap; the two banks are told apart by
  which side of the SHIFT column they sit on. The shift layer reads `DEL A`…
  `DEL H` and keeps its red lettering, which is the only warning the cap gives.
  **Purely cosmetic** — the note is still `HOT_CUE + (slot − 1)`, asserted
  through the real binding, so every MIDI-LEARNed mapping is untouched.
* **CUE and PLAY are perfect circles.** This needed no new geometry: the cap is
  already a square with a corner radius, so `shape: 'circle'` takes that radius
  to half the width. Only the top catch-light changed — a straight chord between
  two corner radii collapses to zero length at r = w/2, so it becomes an arc.
* Renderer additions: `shape`, `face`, `canvas`, `titleColor`, all forwarded
  through `keySpec()` (the hand-written whitelist from the field notes) and all
  folded into `hashId()` — a skin field left out of the hash would let a slate
  pad and a default pad share an id, and `SD.image()`'s dedupe would then skip
  the repaint entirely.
* An unmapped cell paints as **bare chassis**, not the engine's near-black blank.
  A near-black hole in an indigo board reads as a broken key. **Button 1 is asked
  twice**: null while NAV is on so states.js can hang Back on it, chassis with
  NAV off where the module owns it. The first render of this skin got exactly
  that case wrong and left a black square at (0,0) — caught in the preview sheet
  before deploying, which is what the preview sheet is for.

### V17 — the Ableton smart launcher, and the bridge had no server

**RULING — the Root Hub's Ableton tile launches Live if Live is not running, and
navigates either way.**

The plumbing already existed: `IPC.os.launch` → `os.launch` → `open -a` /
`Start-Process`. What did not exist was the version hunt — the bundle is
`Ableton Live 11 Suite` here and `…12 Suite` elsewhere, so the tile names an
ACTION (`ableton`) and the service resolves the newest installed Live at press
time, comparing embedded version numbers numerically. macOS `open -a` focuses a
running app rather than starting a second one, so the check is belt-and-braces;
`Bridge.isOnline()` supplies it.

**The 14 VST layouts were never missing.** Diagnosed rather than rebuilt: the
AdiVST remote script existed only as source in the old repo. It was **not
installed** — `/Applications/Ableton Live 11 Suite.app/…/MIDI Remote Scripts/`
held only `Radium49_61`, `~/Music/Ableton/User Library/Remote Scripts/` did not
exist, and nothing listened on 9006. So `Bridge.isOnline()` was false, `pump()`
never composited, and the registry never resolved past Generic. Writing new
routing over the working routing would have been the wrong fix.

Installed byte-identical to `~/Music/Ableton/User Library/Remote Scripts/AdiVST`,
which is version-agnostic on macOS and therefore serves Live 11 and Live 12 from
one copy — and survives Live updates, which the app-bundle location does not.
**Still requires Adi to select AdiVST as a Control Surface in Live's settings.**

**Confirmed, not changed:** VSTs paint the touch strip and dials only and own
zero keys (L7). The keys staying as the Ableton hub shell while a VST is focused
is the intended design.

### Superseded by this batch

* **V1** — the five-position carousel. Four positions now.
* **V4** — State 2's single borrowed dial. Two now.
* **V8** — State 3 as an empty context shell. There is no State 3.
* **V11**, in part — the value key's ms/Hz toggle moved to dial 5's push.
* **L2** — still unbuilt, but its "4 hot cues each" now means cues **A-D**.


---

## Batch 14 — the first Omnis-Duo hardware pass

Adi tested Batch 13 on the device. An initial instruction set was **withdrawn
mid-flight** (it would have moved the calculator display onto the touch strip and
widened it past 16 keys) and replaced with the set below. Nothing from the
withdrawn version was implemented.

**Workflow change (P5) — LAYOUT IS ADI'S TO CHANGE.** Never alter a physical key
layout, or expand a module's footprint (more columns, borrowed dials, a display
moved onto the strip), without presenting options and getting explicit approval
first. Specifically: **the Calculator stays inside its 16 keys, and the screen
strip and dials are reserved for the VSTs.** A layout is muscle memory on
physical hardware; widening a module "to make things fit" silently rewrites how
the instrument is played. When Adi specifies an exact layout himself, that IS the
approval.

### V18 — one bad cell must not be able to freeze the surface *(the real "dial 6 does nothing")*

Reported: in NAV OFF, a long press on dial 6 did nothing. Driven through the real
socket — Ableton hub, NAV OFF, 620 ms press — the carousel fires and the state
advances correctly. The gesture was never broken.

**The surface was.** `paint()` was four bare lines and let any exception escape
before `painting = false` ran. Every subsequent `repaint()` returns early on that
flag, so the device stopped updating **permanently** — while the state machine
carried on working. The state really did change on every long press; nothing ever
repainted to show it, which is indistinguishable from "the dial does nothing".

**RULING — every cell is painted inside its own guard and the flag is cleared in
a `finally`.** A controller that throws costs one blank zone, not the board.

Newly reachable precisely because Adi selected AdiVST in Live: a controller's
`build()` ran against real device parameter shapes for the first time, and it is
called from `paintDial`.

*Test note:* the first regression test for this passed against the broken code,
because it overrode `States.resolveKey` — and `paintKey` calls the closure-local
`resolveKey`, not the exported one. It now poisons `SD.image`, which the paint
loop genuinely reaches, and it fails without the fix.

### V19 — the calculator: float casting on the operator, and no duplicates

**The `+` bug.** Driven through the real bindings, `277 + 5` returns **282** —
the engine was already correct, and the concatenation Adi sees is the HOLD not
registering, after which two digit presses append. Both ends addressed anyway:

* **RULING — the float cast sits on the execution line itself**, not merely at
  the top of `applyOp`. `+` is the one operator where a string operand succeeds
  quietly and produces a plausible-looking wrong number, so there is now no path
  to a `+` in this engine that is not `parseFloat(x) + parseFloat(y)`.
* **RULING — every function exists exactly once.** `⌫` was reachable BOTH as a
  tap on display segment 2 and as the long half of `0`. Backspace stays on the
  display row; **`0` becomes a plain immediate key** — no timer, no caption, no
  latency. That was the only duplicate on the board.
* **RULING — `+` and `−` stay as the only two holds**, on `C` and `.`, in the
  right-hand operator column. Eighteen functions, sixteen keys, two holds. A test
  now counts every reachable function and fails on any duplicate.

The 16-key footprint is unchanged, the display stays on the keys, and the
calculator still borrows **zero** dials.

### V20 — rekordbox: nudge restored, and the two arrow pairs swap rows

**RULING — all four nudges are HELD again** (Note On down, Note Off up), with no
exception for Button 36. V2 had made (8,3) a tap only because the State Carousel
lived on that key's long press and a held Note On needed a forced Note Off at the
500 ms boundary. The carousel moved to dial 6 in V3, so the special case has had
no reason to exist since — and pulling a deck back into phase by hand is a
gesture you lean into, which a tap cannot express.

**RULING — Beat Jump moves to row 0, Nudge takes the bottom row:**

```
col:     0      1      2      3      4      5      6      7      8
row0   [◀◀A]  [▶▶A]  [ ▲ ]  [ ▼ ]  [ ⊞ ]  [    ] [    ] [◀◀B]  [▶▶B]   BEAT JUMP
row3   [◀◀A]  [▶▶A]  [▶‖A]  [CUE A][    ] [CUE B][▶‖B]  [◀◀B]  [▶▶B]   NUDGE (held)
```

The bottom row is now nothing but the four things a hand rests on mid-mix. The
browser strip shifted two columns right. Deck B's forward Beat Jump gains its
second chevron, so both decks read identically.

Nudge and Beat Jump wear the SAME glyphs, so they are separated by colour and
caption: Beat Jump takes the browser blue, Nudge keeps its purple.

**(0,0) is now Deck A's Beat Jump**, which makes the nav-anchor rule load-bearing
rather than cosmetic: `keyFor` returns null there whenever NAV is on, or the cap
would PAINT as a beat jump and BEHAVE as Back — input.js reserves the key before
the module is consulted. With NAV off it is the module's, exactly as D7 says.

### V21 — the circular transport buttons stand on the bezel

V16's circles were drawn on a chassis-coloured key, so the corners still lit and
it read as a square with a circle inside it. Every Stream Deck key is a lit
square with unlit plastic around it; the only way to get a standalone round
button is to make the square disappear.

**RULING — CUE and PLAY get a BLACK field** (`#000000`), which merges into the
physical bezel and leaves the circle floating. Ordinary pads keep the chassis.

### Superseded by this batch

* **V2** — (8,3) as a single-trigger Beat Jump. It is a held nudge again.
* **V12**, in part — the `⌫` long press on `0` is gone; backspace is display-row only.
* **V16**, in part — the circular caps sit on bezel black, not chassis.
