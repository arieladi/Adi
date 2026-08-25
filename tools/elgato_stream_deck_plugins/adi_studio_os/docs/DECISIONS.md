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


---

## Batch 15 — real icons, and the invisible operator

### V22 — the Root Hub wears the real application icons

**RULING — the Ableton and DJ tiles carry the vendors' own app icons**, extracted
from the installed applications, instead of the `♪` and `⏻` glyphs standing in
for them. This is the swap V9 designed the label geometry around: "V3 swaps text
for artwork ... a node-for-node replacement with no layout consequences."

`js/core/art.js` is a registry keyed by NAME, and a binding carries the name —
never the bytes. render.js derives each key's id from its content and `SD.image()`
dedupes on that, so a 6 KB base64 payload riding on the binding would be walked
by the FNV hash on 36 keys, 15 times a second. Only the renderer touches bytes.

Both `href` and `xlink:href` are emitted, with the xlink namespace declared on
the root `<svg>`. The modern attribute is correct; the legacy one costs 40 bytes
and is the difference between an icon and a blank key on an older rasteriser.

### V23 — the calculator says what it is holding *(the "+ disaster")*

Reported: `2`, `+`, `2` shows `1`. Driven through the real socket the engine
returns **4**, and `2 × 2` — which Adi confirms works — takes an identical path
through `calcSetOp` → `applyOp`. The arithmetic is not the fault.

**What the screen showed was.** The display row printed the current operand and
nothing else, so after `2` `+` it read exactly `2` — pixel-for-pixel identical to
`2` with no operator pending. A registered `+` and a dropped `+` were
indistinguishable, which is precisely what an unreliable key feels like from the
outside: you cannot tell whether it took, so you press again, and the answer
changes.

**RULING — segment 0 shows the pending operation** (`2 +`) above the number,
in the operator amber, and clears when `=` resolves it. No key moved and no
function changed; the machine says out loud what it is holding.

`×` and `÷` never felt broken because they are plain taps on the display row.
`+` and `−` are long presses on caps printed `C` and `.` — a hold that does not
register looks exactly like a hold that was never meant to do anything. **The
remaining question is whether the operators should stop being holds at all**,
which cannot be answered without giving up a key, and is therefore Adi's (P5).


---

## Batch 17 — the clock is reverted, and the deploy check was lying

### V25 REVERTED — the clock is removed in full

Adi's machine began freezing for minutes at a time after the V25 deploy.
**RULING — the clock is removed completely**, to be revisited later from a
stable base. `git reset --hard` to the commit before it; V24 (MIDI inside
Ableton) went with it, since the two shipped together.

**No root cause is claimed.** What is known: V25 added a 4-second timer that
forced a FULL 42-cell repaint at idle, on a surface that until then only
repainted on interaction — and it did so on the same build where the Ableton
bridge had just gone live for the first time, so `pump()` was running its 15 fps
`composite()` + `repaint()` against real Live data, and `Bridge.on('state')` was
firing `pickController()` on every parameter message. The clock was at minimum an
additional independent repaint source layered on that. Before any clock returns,
**the cost of the existing 15 fps pump against a live bridge has to be measured**
— that load exists with or without a clock.

### FIELD NOTE — the Stream Deck binary is `MacOS/Stream Deck`

This one invalidated its own verification, which is the worst kind.

The app's main process is
`/Applications/Elgato Stream Deck.app/Contents/MacOS/`**`Stream Deck`** — the
binary is NOT named "Elgato Stream Deck". So:

* `pgrep -x "Elgato Stream Deck"` — never matches.
* `pgrep -f "…/MacOS/Elgato"` — never matches.
* **Correct:** `pgrep -lf "Elgato Stream Deck.app/Contents/MacOS/Stream Deck"`

Every "wait for the process to actually die" check in this session used a pattern
that cannot match, so it returned instantly and reported success. The app caches
plugin files while running; an rsync under a live app changes nothing on the
device. A main process from two days earlier was found still alive after several
"app confirmed dead" reports.

**Confirm a restart by the LOG, not by a pgrep**: a genuine launch writes a fresh
`~/Library/Logs/ElgatoStreamDeck/com.adiariel.studioos<N>.log` — and note the
number ROTATES per launch, so `ls -t …studioos[0-9].log | head -1` is the only
reliable way to find the current one. `QtWebEngineCore` helpers and a
`termination_handler` can outlive a kill; `pkill -f "Elgato Stream Deck.app"`
clears them.

### V26 — an art tile shows the artwork alone

**RULING — the Ableton and DJ tiles drop their captions and the icon fills the
cap.** A macOS app icon carries its own margin inside the square, so drawing it
at the full inner face needs no hand-tuned inset; anything smaller reads as a
stamp floating on a button rather than as the application. A tile that still has
a caption keeps the small high-set icon, unchanged.


---

## Batch 18 — the freeze had a name, and the clock came back safely

Adi is away from the studio, so everything here is verified headlessly and by
render; the hardware pass is his.

**Standing rule overridden, explicitly.** "The AdiVST remote script is verified
and must not be modified" has held since the port. Adi has now instructed the
opposite for V30. The override is honoured with two constraints of its own: the
change is **purely additive** (`diff` reports ZERO removed lines in both files)
and the wire protocol gains one verb while nothing existing moves, so an older
script simply ignores it.

### V27 — `setFeedback` had no dedupe, and THAT was the render loop

`SD.image()` has dropped no-op writes since the first commit — the field note
about content-derived SVG ids exists because of it. **`setFeedback()` never did.**

So a static 36-key surface cost nothing on the wire while the SIX DIAL ZONES were
re-sent in full on every repaint. Under the Ableton pump's 15 fps that is
**6 × 15 = 90 multi-kilobyte WebSocket messages every second, forever, whether or
not a single pixel changed.** That is the render loop that overloaded the machine.
The V25 clock did not cause it; a 4-second full repaint merely made the
previously-idle Root Hub start doing it too, on top of a pipe already saturated
by a bridge that had just gone live for the first time.

**RULING — `SD.feedback()` is the deduped sibling of `SD.image()`, and paintDial
uses it.** Measured in `test_core`: a cold strip sends 6, ten repaints of an
unchanged strip send **0**, and a zone whose content genuinely moved still sends.
An idle strip went from ~90 messages/second to zero.

`flushCounts()` reports key and zone writes separately, because only the zone
count could ever run away and a single number would have hidden it.

### V28 — the LED clock, ported from Elgato's own font

**RULING — the clock is the Clocks plugin's own "LED" style**, the one Adi
highlighted: seven-segment digits with the UNLIT segments still faintly drawn
behind them, which is the whole trick — it reads as an LED panel rather than a
blocky font. Ported from
`com.elgato.clocks.sdPlugin/action/fontstyles/fonts/led.svg`.

The source ships one `<g>` per digit, each repeating the same seven paths and
dimming the ones that are off. That collapses: the skeleton is stored **once** and
each digit is a **seven-bit lit mask** over it. The font is a few hundred bytes
instead of 6 KB, which is what makes it cheap enough to redraw every second. Cell
geometry is the source's own — 51×93, digits advancing 56, colons 28, straight off
its `translatex` table. Blue is `#4A90E2`, from the plugin's own preset palette.
The city comes from the machine's IANA zone, which is what "Sydney" is in the
photo.

**RULING — the clock takes the right-hand zone ONLY WHEN THAT ZONE IS EMPTY, and
never in State 2.** Adi named the Root Hub, State 0 and State 1 as must-show and
State 2 as must-hide. One rule delivers all four and is safer than a list of
states: the Root Hub leaves dials 5-6 blank by design and States 0/1 do not touch
the strip, so the clock appears in every case he named — while a live VST strip
keeps its sixth zone, honouring the standing rule that the screen belongs to the
VSTs. **A clock is never worth covering a control for.**

**Safety, measured rather than asserted.** `paintClockZone()` repaints ONE zone
and never the keys. Over a simulated minute: 60 ticks → **60 zone messages, 0 key
writes**, one ~4.4 KB frame each. A tick inside the same second sends nothing at
all, and a tick in State 2 does not run. Against the 90 messages/second the strip
already sent before V27, the clock is noise. On the machine, with it ticking:
app 0.1–1.4 % CPU, load average falling.

The tick is a self-rescheduling timeout aimed at the next whole second, so it
cannot stack a backlog and the digit flips when the second does.

### V29 — the Ableton hub is a clean slate

**RULING — the browser arrows and the LIVE key are removed.** ◀TRK / TRK▶ /
◀DEV / DEV▶ spent four of nine keys on row 0 driving Live's own selection, which
the mouse already does well and which said nothing about the session. LIVE
re-requested parameters: debug plumbing, not a control. `selectTrack` /
`selectDevice` stay on the Bridge for whatever wants them later.

Three keys became one: a single quiet readout says what the strip is controlling,
and says "Offline" plainly instead of lighting a red debug lamp. Row 0 is now the
**device shelf** — Pro-Q 3, EQ8, Presets — and the rest is deliberately empty. An
empty key invites the next shortcut; a row padded with generic controls does not.

The hub tests stopped counting keys and started naming them: a count passes for
the wrong reasons and fails every time a shortcut is added.

### V24 (re-landed) — MIDI Control lives in the Ableton hub

Off the Root Hub, into the DAW's own hub: `(0,1)` wide, `(4,1)` compact, those
being the first spare cell each layout has. Implemented and reverted with the
clock in Batch 17; it was never implicated.

### V30 — the first real workflow shortcut: insert Pro-Q 3

**RULING — one key drops a FabFilter Pro-Q 3 on the selected track**, wearing
FabFilter's own mark for the same reason the Root Hub tiles wear theirs.

The remote script gains `cmd_load_device(name)`. `_create_eq8` searched
`audio_effects` only — correct for a stock device, useless for a plug-in, which
lives under `browser.plugins` and may be VST3, VST2 or AU on another machine. So
the new loader walks **every** browser root and matches on NAME, normalised
case-and-punctuation-insensitively, exact match first and only then a contains
pass — so "Pro-Q 3" can never be satisfied by "Pro-Q 3 (m/s)" while the exact
item exists. Only `is_loadable` items are handed to `load_item`, so a folder of
the same name is never loaded.

**It is tested without Live.** `scripts/test_bridge.py` stubs the `Live` module,
builds a fake browser tree shaped like the real one, and asserts the walk, the
match, the exact-beats-variant rule, the folder rule and all three failure paths
(unknown device, empty name, no selected track). Eleven assertions on code that
had never been testable at all.

**Installed** to `~/Music/Ableton/User Library/Remote Scripts/AdiVST`, byte-
identical to the repo. **Live must be restarted** to pick up a changed remote
script.

### Field note — the preview sheet drifted from the paint path AGAIN

`scripts/preview.mjs` built its strip by calling `R.zone()` directly, so the clock
was absent from a sheet that was otherwise correct — the same drift as Batch 16,
reintroduced by the revert. It mirrors `paintDial` again. **When the paint path
grows a step, preview.mjs grows it too, or the sheet quietly stops being evidence.**


---

## Batch 19 — why the clock froze, and why it does not any more

Adi tested V28 on hardware. Two things came back, and the second one named the
first.

### V31 — a page timer CANNOT keep time here; the tick moved into a Worker

**The symptom, and the tell.** The seconds froze. Two photographs taken two
minutes apart both read **`:40`** — `16:16:40` and `16:18:40`. That is not a slow
clock, it is a clock firing **once per minute, aligned**.

**The cause.** `app.html` runs in a HIDDEN WebView. The embedded Chromium
throttles timers on a hidden page: first to a 1 s floor, and then — once the page
has been hidden a few minutes — to *intensive wake-up throttling*, roughly once a
minute, aligned. V28's self-rescheduling `setTimeout` walked into it. Nothing was
blocking and nothing was slow; the timer simply was not being allowed to run.

Worth stating plainly, because it was my own bad reasoning: the Elgato Clocks
plugin uses a plain `setInterval(fn, 1000)` and works, and I took that as licence
to use a page timer. Reading its font and not its clock was the mistake Adi
called out.

**RULING — the heartbeat lives in a dedicated Worker** (`js/core/clock-worker.js`).
A worker has no visibility state of its own, so its interval keeps real time
regardless of the page; `message` delivery is not throttled either. The worker
posts a timestamp once a second and does nothing else — no rendering, no payload,
no state — so it can never be the thing that stalls. If a Worker cannot be created
at all, it falls back to the `setInterval` the Elgato plugin itself uses.

**MEASURED, not asserted.** The tick reports its own cadence to the plugin log
after ten ticks and every five minutes thereafter, so a throttled clock announces
itself instead of being discovered in a photograph:

```
16:34:53.778  clock: started via worker
16:35:03.006  clock: 10 ticks via worker — avg 1000ms (min 996, max 1005)
```

**What a tick costs, for the record:** one ~2.8 KB string build and one deduped
`setFeedback` on a SINGLE zone. It never calls `repaint()`, never touches a key,
never composites the Ableton strip, and does not run at all while the clock is
invisible — so it cannot block the socket, stall `ableton.js`, or perturb NAV.
App CPU with it ticking: 1–5 %, load average falling.

### V32 — no ghost digits

**RULING — the unlit segments are not drawn.** V28 drew them at the source font's
`#222222` / 0.5, believing they were the point of the style. On hardware they read
as a faded `00:00:00` sitting behind the time.

Adi's observation settled it: they do **not** appear in Elgato's own app. The
reason is in the font's own root element —

```
extras="dimmedLEDColor:fill=#222222,dimmedOpacity:opacity=0.5"
```

`extras` is a **themeable knob** (`clock_font.js` resolves it via `__extras`), so
the dimmed segments are an option the vendor's UI can switch off, not a fixed
feature of the face. Ours now never emits them. The frame also dropped from ~4.4
KB to ~2.8 KB, which is free on a per-second redraw.

*Note for a future pass:* the ghosts appeared on the DEVICE but not in the app's
own preview of our plugin, which means the two rasterise the same SVG differently.
Not chased, since the segments are gone either way — but it is a real difference
worth remembering if a future face ever depends on opacity.


---

## Batch 20 — one cause behind almost every bug in this project

Adi reported the Ableton UI "choking": EQ8's **physical dials respond instantly**
while its touch screen takes a full minute to appear, never updates when a dial
turns, and behaves randomly under the finger; Pro-Q 3 shows `??` and never clears
when the focused device changes. His read was a massive unoptimised payload
saturating the JS thread.

**It is not the payload. It is that a page timer cannot keep time in this
WebView, and almost everything in the plugin was scheduled on one.**

### V34 — every scheduled callback moves to `SOS.Timing`

MEASURED on the device with a probe driven from an unthrottled worker, so the
probe could not itself be the thing delayed:

| sample | `setTimeout(0)` | `setTimeout(500)` |
|---|---|---|
| t + 1 s | 2 ms | overshot by **187 ms** |
| t + 2 min | 4 ms | overshot by **687 ms** |
| t + 4 min | 1 ms | overshot by **692 ms** |
| t + 6 min | 3 ms | overshot by **691 ms** |

**A 0 ms timeout is NOT throttled here** — that part of my earlier reasoning was
wrong and is corrected in the code comments. What gets clamped is any DELAYED
timer: it is aligned to a 1-second grid, so a 500 ms timeout reliably takes
~1190 ms. On Adi's device, with the app window closed rather than merely hidden,
the harsher regime appeared — roughly one fire per MINUTE, which is what froze the
clock at `:40` and what makes a 66 ms render pump take a minute to paint.

Every symptom this project has chased falls out of that one fact:

| symptom | the timer behind it |
|---|---|
| the Ableton strip takes a minute to appear, and never moves when a dial turns | `ableton.js` pump, `setTimeout(pump, 66)` |
| Pro-Q 3's `??` screen never clears on a device change | the same pump — the repaint that would clear it never ran |
| calculator `+` "does string concatenation" | `input.js` long press, 500 ms → ~1190 ms, released before it fired |
| dial-6 long press "does nothing" in NAV OFF | `plugin.js` dial hold, same 500 ms |
| the clock froze at `:40` | the V28 tick |
| MIDI touch notes ring for a second | `midictl.js` 40 ms note-off |
| rekordbox browse auto-repeat feels broken | 140 ms repeat |
| the surface is dead for a minute after a service restart | `ipc.js` 1.5 s reconnect |

**RULING — `js/core/timing.js` is the only place in the plugin allowed to
schedule anything.** `after` / `every` are served by a dedicated Worker, which has
no visibility state and therefore keeps real time; `soon()` uses a MessageChannel
for repaint coalescing. Both degrade to native timers if a Worker cannot be
created, and `kind()` reports which leg is live so this is never guesswork again.

**The rule is enforced by a test, not by discipline:** `test_core` strips comments
from every plugin source and fails if any file calls `setTimeout`/`setInterval`
outside the timing layer. Every timing bug here was a raw page timer, so the
invariant is worth more than the individual fixes.

*Not done, deliberately:* the payload was NOT throttled or paginated. Nothing
measured suggests it needs to be, and cutting parameter data on a hunch would
degrade the controllers while hiding whatever is left. If the strip is still slow
with real Live now that the pump runs at 15 fps, that is the point to measure the
payload — and the pump's own numbers will say so.

### V35 — a dial may declare `hold`

Generalised from the key model (V6): a dial binding with a `hold` gets a timer on
press, runs `hold` if it expires, and resolves its short `press` on RELEASE so
closing a tab can never also open one. Dial 6 keeps its own path — NAV is an
engine gesture, not a binding, and must work on a module that declares nothing.

### V33 — the Root Hub touch strip is OS navigation

Master / Zoom / Apps / Lights are replaced by the five things a hand reaches for
while driving a computer. Zone 6 is left EMPTY on purpose: that is how the clock
claims it (`lastZoneFree`).

| dial | turn | push |
|---|---|---|
| 1 Scroll Y | scroll up/down | Page Down |
| 2 Scroll X | scroll left/right | Home |
| 3 Zoom | Cmd/Ctrl +/− | Cmd/Ctrl 0 |
| 4 Tabs | Ctrl+Tab cycle | new · **HOLD = close** |
| 5 Apps | Cmd/Alt-Tab cycle | Mission Control / Task View |

**Every action is a NAMED VERB on the service**, never a key combo in `root.js`.
A test asserts the dial strip contains no `cmd+`, no `ctrl+`, no `hotkey(` and no
platform name — because the asymmetry is real: a tab cycle is Ctrl+Tab on BOTH
platforms while a new tab is Cmd+T on macOS and Ctrl+T on Windows. That is
precisely what the service layer is for.

`appSwitch` was reused rather than rewritten — it already holds the modifier down
across ticks and releases it only when the dial goes quiet, so the switcher stays
open while you spin. My first pass duplicated it with a worse version; corrected.

**Glyphs are restricted to the proven set.** `↕ ↔ ⌕ ⧉` were the obvious picks and
are all UNPROVEN on this device; they became `▲▼ ◀▶ ± ⊞`, all already shipping
elsewhere. A test derives the proven set from the other modules and fails on
anything outside it — the tofu field note, finally enforced instead of remembered.

### EQ8 — the mapping, written out

`docs/EQ8_MAPPING.md` records exactly what every dial and touch band currently
does, at Adi's request. Two structural causes of the erratic touch are visible
without changing anything:

* **the mode selector is duplicated in all six zones** — `_tabHit` runs on
  zone-local `lx` with nothing restricting it to one zone, so the top 17 % of the
  whole strip is a six-times-over mode switcher;
* **the bottom 36 % is band-mute on the left and filter-type on the right, with no
  dead space**, so a slightly low touch mutes a band.

Nothing there was changed: the parameter mapping is verified data (L4) and the
touch bands are Adi's to rule on.


---

## Batch 21 — the dials did nothing because the service was stale

### THE CAUSE, and it was a deploy bug of mine

Adi reported Root Hub dials 1-4 doing "absolutely nothing" on macOS, and
suspected the `osascript` layer. It was not the osascript. **Every AppleScript
form those dials generate compiles and runs correctly** — verified with
`osacompile` and then for real.

The deploy step started the service only *if it was not already running*. It was
running, so it was never restarted, and the running service was the OLD build
that had never heard of `os.scroll`, `os.tab`, `os.appZoom` or the rest. Probed
live: ten of eleven new verbs came back `unknown verb`. `appSwitch` "worked"
only because it is an old verb.

**And it failed in total silence**, because those verbs are fire-and-forget: no
`id`, so no reply, so no error anywhere. Three fixes, because the bug and its
invisibility are separate problems:

1. **`scripts/deploy-mac.sh`** — the sequence as a script, and it ALWAYS restarts
   the service. It also matches the app on `MacOS/Stream Deck`, the trap from
   Batch 17.
2. **The service logs unknown verbs.** A stale service is now audible.
3. **The plugin logs the service version on connect** (`service v2.1.0 on
   darwin`) and the version is bumped whenever the verb table changes, so a
   mismatch is visible in one glance.

### V36 — the app switcher behaves like the gesture it imitates

`SWITCH_IDLE_MS` was 900 ms, which dropped the held modifier mid-spin: the
switcher committed to whatever was highlighted and reopened on the next tick —
Adi's "it selects apps randomly as I spin".

**RULING — turning ONLY navigates.** The app is chosen either by a short press
(an explicit commit, `os.appSwitchCommit`) or by stopping for **2.5 s**. Mission
Control moves to the dial's HOLD, since the short press now has a job. A test
asserts the negative: spinning any number of times never reaches the commit verb.

### V36 — window layouts, and how Elgato actually does it

Adi suggested looking at the official plugins. Worth recording what is there:
**Elgato's Window Mover ships a compiled native Node addon**
(`bin/addon/mac/System.node`) and drives the Accessibility API directly. It uses
`osascript` for exactly one thing — resolving an app's display name.

We have no addon, but System Events exposes the same AX attributes: `position`
and `size` of a window are readable and writable. **RULING — three keys on row 3
(nearest the dials): Left / Fill / Right**, as named layouts on the service.

What AppleScript *cannot* read is `NSScreen.visibleFrame`, so the usable area is
derived: the menu bar is exact (30 pt) and the Dock is estimated from `dock size`,
which is reported as a NORMALISED 0-1 value. Verified frames on this machine:
`left x=0 y=30 w=720 h=809`, `max w=1440`, `right x=720`. Being exact needs the
native addon; that is stated rather than hidden.

**Two runtime bugs that `osacompile` was happy with**, both found only by running
it — which is the lesson:

* `set hidden to autohide` raises -10006. `hidden` is a reserved term in that
  context. Renamed.
* `front window` of the frontmost process raises -1719 when that process has no
  window — and the Stream Deck app itself is exactly that case. Now guarded: it
  walks to the frontmost process that HAS a window and does nothing if none does.

### V37 — the EQ8 UX rebuild

**RULING — two functions per band on the touch screen, and the mode switcher
moves to the dial press.** See `docs/EQ8_MAPPING.md` for the full map.

* **Dial turn** — the active parameter of that dial's band.
* **Band dial short press** — cycles FREQ -> GAIN -> Q, globally for the strip.
* **Touch, two strict boxes with real dead zones**: `8-44` toggles the band,
  `56-92` cycles the filter type, and `0-7 / 45-55 / 93-99` do NOTHING. `x` is
  ignored inside a zone, so there is no horizontal edge left to miss.
* The zone is DRAWN as its hitboxes — top pill, value in the gap, bottom pill —
  so what you see is what you can press.

Deleted outright: the tab row (`_buildTabs`/`_tabHit`), the pagination arrows
(`_pageArrow`), GLOB mode and its builders. Two of those builders still called
`_buildTabs`, so leaving them would have been a latent crash behind an
unreachable branch. The response-curve maths (`_bandDb`, `_buildGraph`) is KEPT
and self-contained — it is the expensive part to rebuild.

**TWO INFERENCES HERE ARE MINE, not Adi's words, and both are flagged in the code
and the doc:** that dial 1 becomes Output (his instruction named dials 2-6 as the
bands but not what dial 1 does), and that pagination lives on dial 1's press
(touch can no longer carry it). **Consequence worth his ruling: COMPACT drops from
four bands to three**, because dial 1 is spent on Output there too.


---

## Batch 22 — the app switcher, nine window states, and Pro-Q 3

### V38 — spinning NEVER selects an app

**RULING — dial 5's turn only moves the highlight; the app is committed ONLY by a
physical press.**

The reason no timeout works: **releasing the held modifier IS the selection.** So
an idle timeout does not "give up", it CHOOSES — which is precisely the random
behaviour Adi reported, first at 900 ms and again at 2.5 s. But removing the
timeout outright is unsafe: a held Command that is never released leaves the
machine unusable.

The resolution is that the safety net **cancels instead of committing**. Escape
while the switcher is open dismisses it *without* switching, so the guard sends
Escape and only then releases the modifier. Nothing is ever chosen by the passage
of time. The guard is 25 s — a deadlock breaker, not part of the interaction — and
dial 5's HOLD is the same cancel, made explicit. Mission Control loses its home
here; two presses are worth more on a dial that is otherwise a one-way trip.

### V38 — the nine native window states, through macOS's own menu

**RULING — nine keys: the four halves on row 1; Fill, three Arrange sets and the
green-button Full Screen on row 2.**

Adi suggested studying the official plugins, and the finding is worth recording:
**Elgato's Window Mover ships a compiled native Node addon**
(`bin/addon/mac/System.node`) and drives the Accessibility API directly. It uses
`osascript` for exactly one thing — resolving an app's display name.

Three of the nine — Left & Right, Left & Quarters, Quarters — are macOS **Arrange**
commands that place TWO OR MORE windows. No single frame write can express those,
so these click the real menu items, enumerated from this machine rather than
guessed (`Window > Move & Resize > Halves | Quarters | Arrange`, plus top-level
`Fill` and `Center`). Full Screen is **Ctrl+Cmd+F**, not a menu name — more robust,
and exactly what the green traffic light does.

The geometry path is KEPT as a fallback for the states it can express, because the
menu route is English-only and needs an app that has the system Window menu.

**Two runtime bugs that `osacompile` accepted happily**, both found only by
running the script — which is the lesson:

* `set hidden to autohide` raises **-10006**: `hidden` is reserved in that context.
* `front window` raises **-1719** when the frontmost process has no window, and
  the Stream Deck app itself is exactly that case. Now walks to the frontmost
  process that HAS a window and does nothing if none does.

### V39 — Pro-Q 3's mode goes global, and the switches get the zone

**RULING — one mode indicator for the strip, no separator, and the three switches
stretched into the freed space.**

The mode was per band AND shape-aware, which is why it appeared eight times.
Mode is now global (matching EQ8's V37) and **shape-awareness moves rather than
disappearing**: `_modeFor(band)` resolves on READ, so a Low Cut reports FREQ while
the strip says Q instead of pretending it can honour it.

Geometry: the header (band tag + live value) is one 18 px line and the switches run
**22-97 — 75 px instead of 41**, nearly double the target height. The tab row is
gone, so a touch in the header does nothing and the three switches own everything
below it.

**A correction to Adi's reading of the screenshot:** the "white separator line"
under each mode indicator is not a separator — it is the **parameter value**,
rendering as an em dash because the parameter is unresolved. It will show a real
number once the binding works, so it was kept (compacted onto the header line)
rather than deleted.

### Pro-Q 3 is still functionally dead, and this is why

Not fixed, because the missing information is on Adi's machine. What IS
established:

* the controller resolves parameters **by name** from `all_params`, matching
  `"band N frequency"`, `"band N gain"`, `"band N shape"` and so on;
* it requests them itself (`getAllParams()` in `onState`), so nothing is missing
  on the plugin side;
* the Python side enumerates `device.parameters` and returns every name.

So `?` means **the names Live sent do not match**. The overwhelmingly likely cause:
Live only exposes a VST's parameters that have been **"Configured"** in Live's own
Configure mode. The header of ProQ3Controller says as much — it was written for a
Pro-Q 3 "whose Ableton Configure exposes, per band: Frequency, Q, Shape, Slope,
Stereo Placement". **A freshly instantiated Pro-Q 3 — which is exactly what V30's
key inserts — has no Configure mapping at all**, so `device.parameters` is
effectively empty and nothing can bind.

**V39 adds the diagnostic that would have answered this immediately:** on every
`all_params` the plugin now logs the device name, the parameter COUNT and the
first ten NAMES. One press with Pro-Q 3 focused produces the answer.

---

## Batch 23 — the alias bug was a keyboard layout, and the window keys got real icons

### THE CAUSE, and it was not the shortcut

Adi's report: the Full Screen key "is duplicating files in Finder and creating
aliases", with a screenshot of three `תמחור בית מבונה.docx alias` files. His
suspicion was a wrong keystroke (`Cmd+D`) or the wrong execution context.

Neither. **The shortcut in the table was correct and the letter on the wire was
not.** `hotkey()` emitted `keystroke "f" using {control down, command down}`, and
`keystroke` asks System Events to produce a **CHARACTER**, which it resolves
through the **currently selected keyboard layout**. This machine's layout is
`com.apple.keylayout.Hebrew`, which has no `f` in it at all.

MEASURED, by typing into a TextEdit document and reading the bytes back:

| sent | typed | means |
|---|---|---|
| `keystroke "f"` | **ש** U+05E9 | the character on **physical key code 0** — `A` |
| `key code 3` | **כ** U+05DB | the character on physical key `F`. Correct. |

So `ctrl+cmd+f` left the service as **Ctrl+Cmd+A**, and in Finder Ctrl+Cmd+A is
**Make Alias**. Three presses, three aliases. Nothing was ever "duplicating".

It was not deterministic either: minutes later the same `keystroke "f"` typed
nothing at all, as did `t`, `w` and `0`. A character-based keystroke under a
non-Latin layout is simply unreliable.

**IT WAS NEVER JUST FULL SCREEN.** Every letter hotkey in the plugin went through
that one line. Verified against a real TextEdit window:

* `keystroke "w" using {command down}` — window **stayed open**
* `key code 13 using {command down}` — window **closed**

So **dial 4's New Tab and Close Tab (V33/V35) have been dead since the day they
shipped**, silently, for exactly the same reason. Adi never reported them; they
were broken anyway.

### V40 — every key leaves the service as a PHYSICAL key code

**RULING (mine, as a bug fix — no design choice in it):** `MAC_ANSI` maps a–z and
0–9 to their Carbon `kVK_ANSI_*` codes and `hotkey()` resolves through it, after
`MAC_SPECIAL` so that "delete" stays Delete and never becomes the letter d. A key
code is the physical key and is identical under every layout, so the layout is no
longer part of the path. `os.type()` (V15's delay-calculator value key) had the
same defect and now sends key codes too.

`macKeyCode()` is exported purely so the resolution can be pinned by a test
without synthesising input on the machine running it.

### V40 — Full Screen is an ACCESSIBILITY WRITE, not a keystroke

Fixing `hotkey()` makes the shortcut correct, but a shortcut is still a keystroke
aimed at whatever has focus, and any future mis-resolution lands on a file command
again. Adi's instruction was that this key "must strictly toggle the native macOS
full screen … NOT duplicate files".

**RULING — read `AXFullScreen` on the front window and write its inverse.** It is
the same attribute the green traffic light drives, it cannot type, so it cannot
touch a file: the failure mode is structurally gone rather than corrected. Verified
on a real window — read false, write true reports true, write false reports false,
so it toggles both ways. `key code 3 using {control down, command down}` is kept as
the fallback for a window that does not expose the attribute (also verified: it
entered and left full screen).

### V40 — the Quads key could never have worked

Found while reading the menu, not reported. `Window > Move & Resize` is **one flat
menu whose group titles are disabled rows**, and it contains the name "Quarters"
**twice** — index 7 is the greyed-out *Quarters* heading, index 22 is the *Arrange*
command. `menu item "Quarters"` always resolves to the first match, so the key was
asking AppleScript to click a label.

Enumerated and then confirmed by running the filter against a live window:

```
"Quarters":  enabled-matches=1  total-matches=2
"Left":      enabled-matches=1  total-matches=1
```

**RULING — match the first ENABLED item of that name.** Group headings are skipped
by construction, no index is hardcoded against a future macOS, and an Arrange
command that is greyed out (one window open) now falls through to the geometry
path instead of erroring. Verified end to end: clicking *Left* moved a real window
to `x=0 y=30 w=960`.

### V41 — the nine window keys wear the native macOS pictograms

**RULING (Adi's, explicit) — exact SVG replicas of the icons in the macOS
"Move & Resize" popover, large enough to fill the cap; and for Full Screen, the
green traffic light rather than the popover's own Full Screen icon.**

`js/core/icons.js` is a NEW registry, deliberately separate from `art.js`: art.js
holds raster bytes because an application's real icon can only be pixels, whereas
every one of these is a shape we draw, so it stays as SVG source — a few hundred
bytes instead of six kilobytes, editable, and never resampled. render.js splices
the markup straight into the key's own SVG; there is no nested `<image>`.

**This retires nine compromises.** The halves were ◀ ▶ ▲ ▼ and Fill plus all three
Arrange sets were **⊞ four times over**, so four keys painted the same picture and
the caption carried the entire meaning. The tofu rule is what forced that — the
proven glyph set has no pictogram for "left and quarters". **A drawn shape has no
font behind it, so it cannot come out as an empty box**, which is the first time
this project has been able to step outside the proven set safely.

* **No caption**, per V26 and Adi's "must completely fill the physical button
  space". The label stays on the slot table as the key's identity for logs and
  tests, exactly as `hub.label` does for the app tiles.
* **`icon` is a name, never markup on the binding** — same reason as `art` (V22).
  It is added to `hashId()` and to `keySpec()`; a missing `keySpec` field is the
  trap that has now bitten three times, so the test asserts on the RENDERED SVG.
* Ids inside an icon are namespaced through an `__ID__` placeholder. A fixed
  gradient id would be fine at runtime (a key is its own document) and wrong in
  the preview sheet, which inlines every key into one page.

**THE TRAFFIC LIGHT HAS NO GLYPH ON IT, and that was decided by looking.** The
obvious move was the pair of opposing triangles the real button shows under the
pointer. Rendered at cap scale against three variations of triangle size and gap,
**all of them read as one diagonal bar across the circle** — which on a Mac window
button is the *not available* badge. A glyph that says the opposite of what the key
does is worse than none, so the disc is bare: exactly the button in the screenshot
Adi pointed at, and the only coloured cap on the surface.

### Field note — `keystroke "<letter>"` is layout-dependent, `key code` is not

This is the fifth distinct bug in this project traced to one shared cause, and it
belongs next to the `setTimeout` note. Anything that must reach a specific key on
this machine sends a **key code**. There is a test asserting no macOS path
interpolates a letter into `keystroke`.

### STILL AWAITING ADI'S RULING — the Root Hub grid redesign

Not implemented. His four bullets cannot all be satisfied at once under one
reading of the drawing, and P5 says the layout is his. The conflict, the arithmetic
and the proposed grid are stated in full in the session output; nothing was moved.

---

## Batch 24 — the grid, to Adi's drawing

### V41a REVERSED — the traffic light keeps its arrow *(Adi overrules me)*

I shipped the bare disc in V41 because at cap scale I read the two opposing
triangles as a single diagonal bar, which on a Mac window button is the "not
available" badge. Adi saw both renders and was explicit: **the first one was right
and the bare circle is the wrong one.** Restored verbatim.

Recorded because the reasoning was sound and the conclusion was still wrong — he
is the one reading this at arm's length on hardware, and the comment in `icons.js`
now says so, so a future pass does not "fix" it back.

### V42 — the Zoom dial wears a magnifier

**RULING — replace the `±` on dial 3's zone with the magnifying glass Adi
supplied.** Drawn, not typed: `⌕` is outside the proven glyph set and is exactly
the kind of character that shipped as an empty box once.

This needed the icon registry to reach DIALS, not just keys, which surfaced the
dial half of a trap that had only ever been documented for keys:

* `R.zone()` gained `icon`, occupying the value's slot so the title and caption do
  not move;
* **`zoneUriFor` in `states.js` is the dial equivalent of `keySpec()`** — a
  hand-written whitelist that paints a silently empty zone if a field is forgotten.
  It now forwards `icon` (and `dim`);
* `scripts/preview.mjs` mirrors that list and has drifted TWICE before, so it was
  updated in the same edit rather than afterwards;
* **`lastZoneFree()` now counts `icon` as content.** Without it a zone carrying only
  an icon reads as empty and the clock paints straight over it.

### V43 — THE GRID, exactly as drawn

**RULING (Adi's, and it settles the Batch 23 ambiguity):**

```
        col 0     col 1      col 2    col 3    col 4
row 0   Ableton   rekordbox  Tasks    Meters   Chrome      shortcuts
row 1     ·          ·         ·        ·        ·          breathing room
row 2   Left       Right     Top      Bottom     ·          Move & Resize
row 3   Fill       L | R     L | Qt   Quads    ● Full       Fill & Arrange
```

His words were "the two rows closest to the touchscreen filled with our mac
navigation icons which should be 8, 4 above the other, then the green alone with
empty key above". **So the "four directional arrows" of Batch 23 were the four
half-snaps all along** — reading B — and there are no new keys. Nothing had to be
cut, and the ten slots hold nine keys with the one deliberate gap at (4,2).

**The window block is now the macOS popover row for row**, which was not something
I arranged: the popover's own two groups ARE four halves over four fill/arrange
states with Full Screen separated below. The eight pictograms therefore sit in the
same relative positions as the icons they replicate.

Three consequences worth stating rather than burying:

1. **ROW 0 IS MIXED**, so a hub declares its `col` instead of being found by its
   index in `HUBS`. An array position cannot interleave hub tiles with app tiles.
2. **CUBASE IS UNPLACED** (`col: null`). Row 0 is five slots wide and Adi named all
   five. The tile was never functional — **there is no `cubase.hub` screen anywhere
   in the plugin**, so a machine with Cubase installed would have shown a key that
   navigated into nothing. The entry and its availability probe are kept for
   whenever the hub is actually built, and a test asserts it is unplaced rather than
   quietly deleted.
3. **Start / Run / Shell and Lynx move to row 1.** They are `mac: null` (D14) or
   gated on an uninstalled app, so row 1 is empty on this machine, and putting them
   in the gap is the only placement that cannot collide with the window block on
   either platform. If Adi installs Lynx Mixer one tile will appear in the breathing
   row; that is better than dropping the tile silently, and he can then say where it
   belongs.

**Both gaps are held by OMISSION**, not by a placeholder binding — `resolveKey`
returns null and the engine paints a blank. Tests assert both, so filling them in
later reads as a regression rather than a tidy-up.

---

## Batch 25 — the VST launcher tree

**Development is FROZEN on EQ8, Pro-Q 3 data mapping and the Calculator** at Adi's
instruction. Nothing in this batch touches any of them.

### V44 — a hierarchical VST launcher under the Ableton hub

**RULING (Adi's) — a "Plugins" folder on the main Ableton screen, category
sub-folders beneath it, load shortcuts inside those, and a dedicated Back button
on every sub-page. The taxonomy, naming and arrangement were explicitly left to
my judgement.**

```
Ableton hub  ->  Plugins  ->  EQ | Dynamics | Synths | Meters  ->  loaders
```

**THE WHOLE TREE IS ONE TABLE.** The brief was a structure that "doesn't get
cluttered as we add many more tools", so `CATEGORIES` in `js/modules/plugins.js`
generates every screen: adding a plugin is one line, a category is four, and
neither touches a layout, a key index or a Back button. There is exactly one menu
renderer, so a grid fix fixes all five pages.

### The taxonomy, and the two placements that are judgement calls

| folder | plugins |
|---|---|
| **EQ** | EQ Eight · FabFilter Pro-Q 3 · Pulsar Massive · Spectre |
| **Dynamics** | Compressor · Glue Compressor · soothe |
| **Synths** | Serum |
| **Meters** | SPAN · bx_meter · s(M)exoscope |

Grouped by **what the plugin does to sound**, not by vendor, because that is the
axis a hand reaches along mid-session: "I need an EQ" comes before "I need a
FabFilter". Four categories is deliberately few — a top level you can read without
hunting beats a precise one.

1. **PULSAR MASSIVE IS FILED UNDER EQ, NOT SYNTHS.** Adi's list sat it next to
   Serum, which reads like a synth, but it is Pulsar Audio's Manley Massive Passive
   emulation — a passive program EQ. The registry is the evidence, not my opinion:
   `PulsarMassiveController` matches `/massive\s*passive/i` and `/\bmp[.\s-]?eq\b/i`.
   This is the one item I moved.
2. **SOOTHE IS FILED UNDER DYNAMICS.** soothe2 is spectral in *what* it touches but
   level-dependent in *when* it acts, so it sits with the compressors.

"standard Compressors" was read as **plural** and became Ableton's two stock ones,
Compressor and Glue Compressor. Spectre sits in EQ because it is a per-band
harmonic enhancer and `SpectreController` is band-shaped.

### The load path was already built and was NOT touched

`Bridge.cmd.loadDevice(name)` (V30) sends `{c:'load_device', name}` to the AdiVST
remote script, **which is verified and must not be modified**. Its
`cmd_load_device` walks Live's browser roots and matches EXACT normalised name
first, then SUBSTRING, then loads onto `song.view.selected_track` — precisely "the
currently selected track" from the brief. No Python changed.

**THE SUBSTRING PASS DICTATED THE NAMES.** Measured on this machine, Xfer's synth
is installed as `Serum2.vst3`, not "Serum", so the short stem "Serum" finds it
through the substring pass while "Serum2" would miss a plain "Serum" elsewhere.
Short distinctive stems resolve on more machines than exact product names. Where a
stem would be ambiguous it is spelled out: **"FabFilter Pro-Q 3", never "Pro-Q",
because Pro-Q 2 is also installed here** and the substring pass would be free to
pick it. A test pins both halves of that reasoning.

### NOT INSTALLED, verified by looking

**soothe, Spectre and Pulsar Massive are absent from every plug-in folder on this
machine** (`/Library/Audio/Plug-Ins/{VST3,VST,Components}` and the user
equivalents). Their keys are built anyway and are deliberately NOT hidden — unlike
a Root Hub app tile there is nothing to probe, because only Live knows its own
browser. They will report "not installed" on the status zone.

### The status zone, and why it is not polish

**`device_loaded` was never emitted and `error` was only logged**, so a load
shortcut was fire-and-forget in both directions: a name Live's browser does not
have produced a key press with no visible consequence at all. With three of the ten
missing here that is the common case, not the edge case — and it is the same silence
that hid the stale-service bug in Batch 21. The bridge now emits `device_loaded` and
the launcher shows the result.

**The readout sits on DIAL 2, not dial 5.** Under L3b a docked window borrows the
RIGHTMOST dials and 1-4 always stay with the module; State 2 takes physical 5 and 6
(V14). A status zone on 5 vanished behind the Time Divisions window the moment
anything docked — **the preview sheet showed it, which is the second time that
"look at it before deploying" caught a design error rather than a typo.** Zone 1
carries the folder name, zone 2 the result, zone 6 is left blank for the clock. Both
cost no keys, which is the clutter the brief asked me to design out.

### Every sub-page is `fullScreenCapable`, and that is load-bearing

Not cosmetic. **Button 1 is the reserved Back anchor only OUTSIDE NAV OFF**; in NAV
OFF it belongs to the module, which is what allows (0,0) to be a plain Back key
answering a SHORT press. On a non-full-screen page the same key would need a 500 ms
hold — not the button Adi asked for. It also means entering and leaving these pages
changes no state and docks nothing, so no numpad ever appears over the menu.

### The pump only composites for the screen that draws the strip

`Nav.enter` pushes without exiting, so the pump kept running under the launcher.
That is wanted — the status zone needs repaints — but `composite()` builds the whole
1200x100 controller strip and on a menu page nothing paints it. It was 15 frames a
second of an image thrown away. Compositing is now gated on the hub being the
current screen and the cadence drops to 4 fps off it. Gating with one question here
beats coupling two modules' lifecycles, which can desynchronise.

### Left alone deliberately

Pro-Q 3 and EQ8 still have their own fast keys on the device shelf even though both
now also appear inside Plugins. **That duplication is intentional and is Adi's to
rule on (P5)** — they are two-press-shorter and were there first.

### Awaiting Adi

* **Per-plugin icons.** He asked to be asked. Everything is text plus vendor
  captions for now; only Pro-Q 3 has artwork in the project (`SOS.Art.proq3`) and it
  is deliberately not used inside the tree, so the folder reads uniformly.
* **INDEQ, dBComp, Vital** are installed and INDEQ/dBComp already have controllers,
  but they were not on his list, so no keys were invented for them.

---

## Batch 26 — the Great Flattening, and Chrome

**FROZEN at Adi's instruction, and untouched here: EQ8, Pro-Q 3 data mapping, the
Calculator.**

### V46 — the VST tree is scrapped; the Ableton hub IS the launcher

**RULING (Adi's) — "The Stream Deck XL has 32 keys, which is plenty of room. We
want to flatten the menu and put the plugin shortcuts directly on the main Ableton
hub, categorized by columns for fast muscle memory."** V44's `Plugins → category →
loaders` tree is gone, one batch after it shipped. `plugins.js` survives as the
CATALOGUE — the part worth keeping was the single table, not the navigation.

```
        cols 0-1    cols 2-3      cols 4-5   cols 6-7      col 8
row 0   Back        Glue          Serum      SPAN          MIDI
row 1   Pro-Q 3◆    Massive       —          Scope         status
row 2   —           Soothe        —          —             —
row 3   —           —             —          —             NEXT
        RED  EQ     YELLOW  Dyn   GREEN  Syn CYAN  Meters  utility
```
(EQ8 / INDEQ fill the rest of the red band, Comp / Spectre / dBComp the yellow,
Vital the green, bx_meter the cyan. ◆ = real extracted logo.)

### THE HARDWARE DISAGREED WITH THE BRIEF, and this is the resolution

Adi wrote "32 keys", "8 columns wide", and numbered the functional keys **col 7**.
The **+ XL is 36 keys and 9 columns** (verified, CONTINUE.md). His screenshot cuts
off the ninth column — **which is exactly why he drew MIDI and NEXT in the margin to
the RIGHT of his cyan Meters box rather than inside it.**

Read as "the rightmost column" every part of the brief agrees at once: "Top-Right"
and "Bottom-Right" are the real right edge (col 8), cols 6-7 stay wholly Meters as
the cyan box shows, and four category bands plus one utility column uses all 36 keys
instead of stranding four. Taking "col 7" literally would have made col 7 both a
Meters cell and the MIDI/NEXT cell, and left an empty column at the edge.

### V46 — the group frames, and why they were the hard part

**RULING — "draw visual borders, subtle background tints, or grouped frames behind
these specific column groups."**

**THERE IS NO CANVAS BETWEEN KEYS.** Each key is its own 144x144 image with a real
bezel gap either side, so a box around eight keys cannot be one drawn rectangle — it
has to be assembled from eight keys each painting the piece that falls inside their
own square. A binding therefore declares which of its sides are the GROUP's outer
boundary (`frame: {color, t, r, b, l}`) and `render.js` gives it a colour wash over
the whole canvas — visible in the 6 px margin, which is what ties the band together
across the gaps — plus a hard bar on each outer side.

Three things this forced, all found by rendering it:

* **The bars are emitted AFTER the face**, or the raised face covers them and the
  whole feature is invisible.
* **`frame` is part of `hashId()`.** Two loaders can differ only by which edge of
  the box they sit on, and `SD.image()` dedupes by URI — without it the second key
  wears the first one's border.
* **ARTWORK GIVES BACK 8 px INSIDE A FRAME.** Unlabelled art fills the inner face
  (V26); on the sheet the FabFilter and Vital tiles were edge-to-edge and their
  red/green bands all but vanished. Seen, then fixed.

**EVERY CELL OF A BAND IS RETURNED, EMPTY OR NOT** — a blank that still carries the
frame is what makes the band read as a box four rows tall. Returning null for the
empties would stop the colour halfway down.

### PRESETS IS GONE ENTIRELY

"Completely remove the Presets button and its folder. I never requested it." Key,
folder, the `mode`/`setMode` flag and the `_setMode` export are all deleted.
`Bridge.cmd.listPresets/loadPreset/newPreset` are left alone — those are protocol
against the verified remote script, not UI.

### Icons — only real ones, and an honest list of the gaps

**RULING — "if you need an icon and it exists locally or you can extract it, just
use it. Do not invent fake SVGs."** Every plug-in bundle on the machine was searched.

| extracted | Chrome (`app.icns`, path supplied), Vital (`Icon.icns`), FabFilter (already shipped) |
|---|---|
| **no product mark exists** | Serum2 (no images in the bundle), SPAN (empty Resources), **s(M)exoscope (the .vst3 is a bare 5 MB FILE, not a bundle)**, bx_meter / INDEQ / dBComp (UI parts only — knobs, plates, needles), EQ Eight / Compressor / Glue Compressor (**Live DRAWS its device graphics; there are no per-device files**) |

Those keys wear text plus a vendor caption. Extracting a background plate or a knob
and calling it a logo would be the same fake icon, just sourced locally. Adi asked
to be asked for the rest.

### V47 — Chrome could not exit full screen, and the reason was invisible

**Adi: "it CANNOT exit full screen in Google Chrome (pressing it again does
nothing)." MEASURED: the AX write is accepted and does NOTHING, in BOTH
directions.** Writing `true` to a windowed Chrome left it reading `false` and raised
no error, so osascript exited 0 and V40's `(await ax()) || hotkey(...)` never reached
its fallback. The failure was invisible to the only thing being checked.

**RULING — the Chromium family is named and goes straight to the keystroke** (Adi's
own suggestion), and the AX path is now VERIFIED rather than assumed: the script
re-reads the attribute and reports `unchanged`, and only then does the keystroke run.
Verified through the real `windowLayout("fullscreen")`: Chrome windowed →
FULLSCREEN → windowed (read off Chrome's own View menu), TextEdit false → true →
false with no keystroke at all.

**TWO MORE APPLESCRIPT BUGS, both found only by RUNNING it — the standing lesson:**

* **`set w to window 1` and then reusing `w`** raised `Can't get window "re.txt" of
  application process "TextEdit" (-1728)`. Assigning a System Events UI-element
  specifier collapses it to a by-name reference which then fails to resolve. **The
  AX path was erroring on EVERY app and only the keystroke fallback was doing the
  work — so V40's whole "never type, so it can never touch a file" property was
  silently gone.** The geometry path never had it because it addresses `window 1`
  inline, which is now the fix.
* **A fixed `delay 0.25` was too short.** TextEdit ENTERING full screen still read
  the old value, so it reported `unchanged` and the caller ran the keystroke as
  well — one mistimed animation away from toggling twice and landing back where it
  started. It now POLLS for the change, up to ~1.8 s, exiting the moment it flips.

### V47 — the touch strip

* **The Tabs zone** wears an SVG replica of the image Adi supplied: two overlapping
  cards, the back one with a white body and the front one solid with a white title
  bar and a plus. The two cards are deliberately different, because that asymmetry
  is what reads as "an inactive tab behind the active one" and it is in his source.
* **The scroll arrows are the clock's blue.** "Exactly the same" is the requirement,
  so `PALETTE.clock` is now the single source and `clock.js` resolves it at call
  time; a test asserts the two agree so they cannot drift into two similar blues.

### Flagged for Adi, not decided

1. **The EQ8 key lost its create-or-focus behaviour.** It used to call `eq8Key()`,
   which selected an existing EQ Eight if the track had one; as an EQ-band loader it
   now always inserts. That follows "the shortcut buttons must STRICTLY send the Live
   API command to instantiate that device", and EQ8 is frozen, so it was not
   redesigned — but it is a real behaviour change and reversible in one line.
2. **Pulsar Massive and Spectre are under DYNAMICS because he put them there.** I
   flagged in Batch 25 that Pulsar Massive is a Manley Massive Passive emulation — a
   passive program EQ, which the registry's own patterns confirm — and he has since
   assigned it to Dynamics explicitly. Recorded, not re-argued.

---

## Batch 27 — tints, the unified plugin key, and Track Mode

**FROZEN and untouched: EQ8's mapping, Pro-Q 3 data mapping, the Calculator.**
(The EQ8 *key* changed, but only to stop being special — see V52.)

### THE INVESTIGATION Adi asked for: what happened to track volume and pan

**Answer: nothing happened to it, because it never existed.** Searched
exhaustively rather than recalled — `live_bridge.py`, `AdiVST.py`, the whole
protocol doc, every file of the legacy `adi_ableton_vst_controller`, all of
`docs/DECISIONS.md`, and `git log -S` across the repo. There is no Ableton track
volume or pan anywhere in the history. No ruling, no verb, no dial.

**What he is almost certainly remembering is one of two adjacent things:**

1. **The Root Hub's MASTER VOLUME dial (D12 era), which V33 displaced.** That is
   SYSTEM output volume, not an Ableton track: `os.volume` and `os.mute` still
   exist on the *service* and still work. The note in root.js has said so all
   along — "Master volume and the D12 lighting dimmer are not gone as concepts;
   they simply no longer have a home on THIS strip". It was dropped because the
   V33 OS-navigation strip took all five usable zones, not because it was rejected.
2. **The uncommitted `load_device` work in the working tree.** `AdiVST.py` and
   `live_bridge.py` have been dirty in git since Batch 18 — that diff is V30's
   loader, which I wrote and never committed because the commit scope is "the
   plugin folder only" and AdiVST sits outside it. So "we discussed adding to the
   remote script" is a real memory; it just was not about the mixer.

**AND THAT ANSWERS THE OTHER QUESTION IT RAISES.** "The remote script is verified
and must not be modified" has one established exception, set by V30 and live on the
hardware ever since: **purely ADDITIVE verbs that touch no existing code path.**
Volume, pan and the unified device key are implemented that way — new methods, new
dispatch branches, nothing existing edited. `cmd_eq8_key` is left byte-for-byte
intact even though nothing calls it any more.

**THE UNCOMMITTED PYTHON IS NOW COMMITTED**, in its own commit. Leaving the verbs
this batch depends on living only in the working tree and the deployed User Library
copy was a real hazard: a `git checkout` would have silently broken the hub.

### V52 — the unified plugin key: short = smart focus, long = force insert

**RULING (Adi's) — "Do not make EQ8 special."** Every plugin key on the hub:

| press | behaviour |
|---|---|
| **short** | none on the track -> insert · one -> focus it · several -> focus the NEXT on each press |
| **long** | always append a new instance |

This is `cmd_eq8_key`'s Conditions A/B/C generalised from "devices whose
class_name is Eq8" to "devices whose NAME matches" — the only handle a VST gives
us, since every VST3 shares `class_name` "PluginDevice" and class matching cannot
tell Pro-Q 3 from Serum.

**THE DECISION HAPPENS IN LIVE, NOT IN THE PLUGIN**, and that is deliberate. The
plugin's picture of the track is a snapshot pushed on change; a key that chose
between insert and focus from that snapshot would be racing it. One verb,
`device_key`, with a `new` flag for the hold.

**A BUG THE TEST CAUGHT BEFORE THE HARDWARE DID.** The first cut reused
`cmd_load_device`'s exact-then-CONTAINS matching for the on-track search. Contains
is right for the browser — "Serum" has to reach the installed `Serum2` — and wrong
on a track: `"compressor"` is contained in `"gluecompressor"`, so a track holding
only a Glue Compressor answered the **Comp** key by focusing the Glue and never
inserting the Compressor that was asked for. It is exact-then-**PREFIX** now, which
keeps the leniency that matters (a device named for a later version still answers
to its stem) while a plugin whose name merely ENDS with another's can no longer
impersonate it.

The `hold` is V6/V35's binding-level opt-in, so the engine times the key like an
anchor and resolves the short press on RELEASE — a long press can never also fire
the short one, and input.js needed no changes at all.

### V50 — Track Mode: the idle state

**RULING — with no device focused, the dials become track controls instead of six
blank zones.**

| dial | idle |
|---|---|
| 1-4 | the Root Hub's OS-navigation strip, **mirrored** |
| 5 | track Pan |
| 6 | track Volume, **strictly 0.5 dB** |

Dials 1-4 are not reimplemented — `osNavDial` was extracted from root.js and both
strips call it. Two hand-written copies of "the standard OS navigation strip" is
how it quietly stops being standard. **The clock loses zone 6 here on purpose**
(`lastZoneFree` gives it up when something else uses the zone), which is what Adi
asked for. **Neither track dial takes a press**: dial 6's long press is the engine's
NAV gesture, and volume is the one control on this strip you must not fire by
accident.

**WHY 0.5 dB IS HARDER THAN IT SOUNDS, and how it is exact.** `mixer_device.volume`
is a DeviceParameter whose `value` is NORMALISED 0..1 on Live's own fader curve.
There is no dB setter and the curve cannot be inverted analytically. So the dB is
read from the parameter's OWN display string and the normalised value for a target
dB is found by **bisection on that same function** — exact by construction, correct
on any Live version, no curve constants to go stale. ~30 halvings per detent, which
is nothing beside the round trip that delivered it.

Two details that only a test would have found:
* **The value is SNAPPED to the grid before stepping.** A fader parked at -6.02 dB
  by a mouse drag must land on -6.0 and stay on the half-dB grid; stepping from the
  raw value carries that 0.02 forever.
* **24 bisection halvings left ~0.01 dB of residue** — visible in a readout printed
  to two decimals. It is 30 now.

Both mixer parameters are **watched for the lifetime of the selected track**, so the
readout cannot go stale when the fader is moved with the mouse.

### V49 — the bands are TINTED, not just outlined

**RULING — "The thin colored bezels are too subtle and hard to see depending on the
viewing angle... the background color of the keys themselves MUST clearly
differentiate the sections."**

Every cell of a band now carries `face` — render.js's existing material override,
which V16's Omnis-Duo skin already used — set to the band colour crushed 78 %
toward black. Dark red / amber / green / cyan caps. **Reusing `face` rather than
inventing a second tint mechanism meant it was already in `hashId()` and already in
`keySpec()`**, so nothing new could silently fail to reach the ink. The outlines
stay on top. The Back key takes the EQ band's tint so that corner is not the one
bare cap; the utility column is deliberately untinted, because it is not a category.

### V49 — the categories, corrected

**Pulsar Massive and Spectre moved OUT of Dynamics and into EQ**, at Adi's
instruction: "You were correct in your previous warning regarding Pulsar Massive."
Both are band tools. Pinned in both directions by a test so neither drifts back.

### V49 — the invented "Device" readout is deleted

"I do not know what the Device screen you invented is, but it does nothing useful."
He is right — it was a key with no tap handler, and its one remaining job, reporting
a load that missed, belongs on **the key you actually pressed**. `lastError` is now
matched on the device string, so pressing Soothe on a machine without Soothe reddens
Soothe and nothing else. Asserted as an absence, because a careless revert is what
would bring it back.

### V49 — NEXT always has somewhere to go

**RULING — "The NEXT button should have an empty next layout for more plugins in
the future with the same visual split."** `itemPages()` returns a minimum of 2, so
page 2 is the same four tinted, framed bands with nothing in them. Pages are
`bandPages x itemPages`, which at 5 columns walks EQ+Dyn, EQ+Dyn spare, Syn+Met,
Syn+Met spare — one control, still just "show me more".

### V51 — the MIDI screen had no exit, and the comment lied

**RULING — a real Back key at (0,0).** The bug is worth recording exactly: the code
returned `null` there with the comment *"Button 1 is left unbound so the engine
paints Back."* **The engine only does that OUTSIDE NAV OFF** — `decorate()` guards
on `!isFullScreen()` and input.js reserves Button 1 on the same condition — and the
screen declares `fullScreenCapable`, so it is ALWAYS in NAV OFF. Button 1 was an
unbound, unpainted, dead key and the module's only exit was the dial-6 NAV gesture.
**The comment stopped being true the moment the screen went full-screen-capable, and
nothing failed loudly.** Same class of bug as V51's stale claims elsewhere: a
comment asserting an invariant that a later change quietly broke.

### Operational note — LIVE MUST BE RESTARTED

Ableton loads MIDI Remote Scripts at launch, so the new verbs do not exist in a Live
that was already running when the deploy landed. Live WAS running at deploy time.

---

## Batch 28 — flat tints, deep search, and device stepping

**FROZEN and untouched: EQ8's mapping, Pro-Q 3 data mapping, the Calculator.**

### V54 — the coloured bezels are DELETED, not switched off

**RULING — "The tints look good and we no longer need the thin colored border
lines."** So the whole V45 group-frame feature is gone from render.js: the coloured
outer bars, the margin wash, the `frame` field, its term in `hashId()` and its line
in `keySpec()`. It had no other caller, and a feature left switched off is a decoy.

**THE PART THAT ACTUALLY MATTERED was setting `canvas` as well as `face`.** With
`face` alone the raised cap was tinted and the 6 px margin around it stayed
near-black — so every key read as a tinted button sitting inside a dark ring, which
is exactly the border he asked to be rid of. Both fields together make the key one
flat block of colour edge to edge. Both were already in `hashId()` from V16, so the
dedupe could not silently break.

**Still there, deliberately:** the 1 px neutral hairline and the soft vertical
gradient from V9/V10. Those are the global key material on every module, not a
category border, and changing them is a P5 aesthetic decision rather than this
instruction. Flagged for him rather than assumed.

### V54 — uniform text, no logos on the plugin grid

**RULING — "Mixing real logos with text labels for the other plugins is visually
confusing... use plain, uniform text for ALL plugin buttons."** Pro-Q 3 and Vital
lose their artwork; every cell is now a name plus a vendor caption.

This REVERSES V46's "use the actual plugin logos as the buttons instead of text
where possible", and the reason is worth keeping: two pictures among twelve names
read as a mistake rather than as emphasis. **`proq3` and `vital` were removed from
art.js entirely** — 30 KB of base64 that nothing draws is a decoy — with the
extraction paths and the `sips` command left in the header so they are one command
away. **Root Hub icons are untouched**; Adi was explicit that Chrome and the app
tiles keep theirs.

### V53 — Smart Focus descends into racks

**RULING (Adi, answering my own question) — YES, search inside Racks.**

`track.devices` is only the top-level chain. `_all_devices` now walks it
depth-first, descending through every rack's `chains` **and its `return_chains`** —
which is where a reverb send inside a drum rack lives — and recursing, because a
drum rack is routinely three levels deep.

**DEPTH-FIRST IN CHAIN ORDER, and that ordering is load-bearing**: the same
flattened list feeds the device-step arrows, so it has to be the order you see in
Live or "next" would jump over a rack instead of entering it.

**The bug this fixes was worse than "it does not find it".** Before this, pressing
Pro-Q 3 while an instance sat inside a rack found nothing on the track and therefore
INSERTED another one — every press, forever. A test pins that case directly.

There is a `MAX_RACK_DEPTH` of 12. Live's device tree is finite and acyclic so it is
not a real limit; it is there because this code runs inside Ableton's own process and
a runaway recursion there takes the DAW down with it.

### V53 — the device-step arrows

**RULING — Up and Down in the two cells between MIDI and NEXT, stepping the
selected device on the track "including traversing into and out of nested
devices/Racks."**

They walk the same flattened list, so they descend into a rack and come back out the
other side. **They CLAMP rather than wrap**: stepping off the end of a chain should
stop, the way an arrow key stops at the end of a list — wrapping from the last device
to the first is the kind of surprise that makes you stop trusting the key. From a
cold start with nothing selected, "next" begins at the first device and "prev" at the
last, so both arrows do something useful.

The caption is the position in the flattened tree (`3/8`), which is the only thing on
the surface that can tell you a Pro-Q 3 is three deep inside a drum rack. It arrives
on its own `device_pos` message rather than as a new field on `device`: that one is
verified protocol several controllers key off, and a tree walk on every device change
would make it pay for something only two keys read.

### Field note — never string-match through hashId()

`hashId()`'s join argument is a literal backslash-u-0001 escape, and every attempt to
edit around it by matching text through it has silently mangled it. This batch
produced a doubled `join` and a syntax error that took render.js and five suites down
at once. **The test suite caught it in a single run**, which is the point — but the
lesson is to edit that function BY LINE, never by matching text across it. Third
time.

### Operational note — LIVE MUST BE RESTARTED, again

`device_step`, `device_pos` and the recursive search are new remote-script code.
Ableton loads Remote Scripts at launch, so a Live that was already running does not
have them.

---

## Batch 29 — Adi's artwork on the bands, and the red traffic light

**FROZEN and untouched: EQ8's mapping, Pro-Q 3 data mapping, the Calculator.**

### V55 — the category palette gets its own names

**RULING — "EQ is now Deep Violet, Dynamics is Dark Amber, Synths is Deep Teal,
Meters is Emerald Green."**

| band | colour |
|---|---|
| EQ | `catEq` `#8b5cf6` deep violet |
| Dynamics | `catDyn` `#d99125` dark amber |
| Synths | `catSynth` `#14b8a6` deep teal |
| Meters | `catMeter` `#10b981` emerald green |

These were BORROWED module colours before — EQ took `rekordbox`'s red and Dynamics
the calculator's `console` amber — so a category could not be recoloured without
dragging an unrelated module with it. They are their own palette entries now, and a
test asserts none of them is a module colour any more.

### V55 — the bands wear Adi's images, sliced per key

**RULING — replace the flat tints with his four images, each 1:2 over a 2x4 block,
"one continuous, unbroken piece of art spanning across the bezel gaps".**

**THE IMAGES ARE CUT UP OFFLINE, AND THAT IS THE WHOLE ENGINEERING PROBLEM.** Every
Stream Deck key is its own image, handed to setImage as a data URI inside its own
SVG; there is no shared canvas behind the keys. Embedding each band's whole picture
in each of its eight keys would put **32 copies of four 2 MB JPEGs — about 95 MB of
SVG — on a pipe that V27 established is overwhelmed by ~90 multi-KB messages a
second.** So `scripts/slice_backgrounds.py` cuts each image into eight 144x144
tiles once, and `js/core/backgrounds.js` is 101 KB for all thirty-two.

Details that had to be right:

* **The sources are 1440x2912, which is 1:2.022, not 1:2.** 32 px is CROPPED from
  the height before the resize rather than squashing the frame by 1.1 % — losing
  1 % of the picture is less visible than distorting every circle in it.
* **Tiles are ROW-MAJOR, and the tile index IS the cell's slot index.** plugins.js
  already computes a slot for every cell, so there is no second mapping that could
  drift; a test reads the whole grid back and asserts each band uses its eight tiles
  exactly once, in position order. A permutation here would scramble the art on
  hardware in a way no renderer unit test would notice.
* **THE PICTURE BELONGS TO THE BLOCK, NOT THE ITEMS.** The slot, not the paged item
  index, chooses the tile — otherwise the background would slide sideways every time
  NEXT paged the plugins through the bands.
* **`(0,0)` is Back, which ableton.js owns rather than plugins.js**, so it is handed
  the EQ block's tile 0 explicitly. Without that the picture has one blank corner.
* **The tile fills the whole 144 canvas, not the inner face.** Any inset would leave
  a dark gutter around every key and break the continuity the feature exists for.

### V55 — the button material stays ON TOP of the art

**RULING — "do not remove the existing 1px neutral hairline and soft vertical
gradient; render them on top of these new background images."** So `face()` gained
`faceOpacity`: the gradient is drawn at 0.38 over the picture instead of replacing
it, and the hairline and 1 px edge draw at full strength. A key still reads as a cap
rather than as a photo.

**Plus a scrim, which is not decoration.** The Dynamics image is a cream VU dial and
white labels on it are unreadable bare. A flat dark wash at 0.27 costs a little of
the art and buys every caption on the surface — Adi's "ensure the white text labels
remain perfectly legible" is a requirement, not a preference. **Tuned by looking:**
0.38 was safe but crushed the violet and teal bands almost to black, so it came down
to 0.27 and the bright-VU case was re-checked at cap scale before it shipped.

The flat V54 tint survives underneath as the FALLBACK, so a build without
backgrounds.js still reads as four categories rather than four dead blocks.

### CORRECTING A COMMENT V22 GOT WRONG BY THREE ORDERS OF MAGNITUDE

`<image>` emits both `href` and `xlink:href`, and V22's note said the legacy
attribute "costs 40 bytes". **It costs the SIZE OF THE IMAGE** — the whole data URI
is repeated, so a 5 KB tile produces a 10 KB key. Invisible while the only raster
was one 6 KB app icon; obvious the moment 32 tiles arrived. Both attributes are kept
(a blank key on the real device beats a duplicated payload that never leaves the
machine), but the tile budget is now set WITH the doubling in mind, which is why the
tiles are 144 px and not 288, and a test caps a tile at 6 KB.

### V55 — the red traffic light

**RULING — the green button's twin in the cell above it. Short press quits the
frontmost app, long press force-quits it.**

Drawn from the SAME helper as the green cap — `trafficLight(top, bot, glyph)` — and
the green one was refactored onto it in the same edit. Two hand-written circles that
were "the same" would drift the first time either was touched, and a comment
claiming they matched would then be false. macOS's own red, with the cross that
button actually shows under the pointer rather than the green one's expand pair.

**THE GUARD LIST IS THE PART THAT MATTERS.** This key can end a process, so it must
not be able to end the wrong one. `NEVER_QUIT` covers:

* **Stream Deck** — killing it kills the plugin issuing the command; the surface
  would go dark mid-press.
* **Finder, Dock, SystemUIServer, loginwindow, WindowServer** — system UI that a
  studio session should never be one long press away from losing.
* **Ableton Live** — deliberately included. This is a studio surface, the Ableton
  hub is two presses away, and an accidental long press that killed Live mid-session
  would lose work in a way no other key on this board can. **Flagged for Adi: one
  line to remove if he disagrees.**

Both verbs resolve the frontmost app FIRST and refuse by name, so the guard covers
the graceful path too. Graceful means the QUIT APPLE EVENT aimed at the app by name,
not a blind Cmd+Q: an event cannot land on the wrong app if focus moves between the
press and the script, and it still raises the save prompt. The keystroke stays as a
fallback for an app that ignores the event.

`hold` on a Root Hub slot is new, and it is only emitted when a slot ASKS for one —
a phantom hold on every OS key would delay all of their short presses to the 500 ms
boundary for nothing.

### A test that was passing for the wrong reason

The V43 assertion "(4,2) is empty, so the green cap stands alone" still passed after
the red light was added, because that block never runs the service availability probe
and the key is gated on it. Rewritten to say WHY it is null there, with the real
behaviour tested separately with the probe driven. A test that cannot fail is worse
than no test.

---

## Batch 30 — the centre elements are gone, and Apps/Tabs swap

**FROZEN and untouched: EQ8's mapping, Pro-Q 3 data mapping, the Calculator.**

### V56 — removing the VU meter and the radar, and why detection lost to measurement

**RULING — "programmatically remove, hide, or obscure the distracting center
elements": the VU meter from the Dynamics image, the radar circle from the Meters
image.**

**THREE DETECTORS FAILED BEFORE THE FOURTH ANSWER WAS "STOP DETECTING".** Row
brightness, row contrast (stddev), and centre-versus-margin deviation all found the
VU meter's bright FACE and all three missed the dark bezel ring around it. The ring
is very dark brown on very dark brown — it barely registers on any statistic. A band
that stops even a few pixels inside it smears those pixels down the entire patch,
because the fill blends between the boundary rows: **the first render came out with a
tombstone-shaped ghost exactly where the meter had been, and the radar came out as a
rounded rectangle.** Both were caught by rendering a before/after sheet and looking
at it, not by any assertion.

So the bands are **measured from the files and written down**:

```
dyn     VU unit's OUTER BEZEL   y  880..1813   ->  band  850..1855
meter   radar's outer bezel     y 1103..1767   ->  band 1050..1815
```

For four fixed, hand-made images that is more honest than a detector tuned until it
happens to agree — the numbers are checkable, and `verify_band` re-checks them at
generation time and prints the distance to the nearest surviving feature. It fired a
warning on the Meters band (2 px from the LED bars) which is exactly the case worth
being told about: **the bars are art Adi is keeping**, and they sit immediately above
and below the radar. `EXPECT_SIZE` fails loudly if a source is ever swapped for a
different size, since the bands stop being measurements of anything at that point.

**REMOVED BY PER-COLUMN CROSS-FADE, NOT A FLAT FILL.** Each column of the band is a
linear blend from the clean background above it to the clean background below it.
Two reasons a flat colour would not do: the backgrounds are a vertical gradient, so
a solid patch reads as a plate; and doing it per column carries the faint VERTICAL
grid lines straight through the repair, which is visible in the Meters result. The
boundary colour is the MEDIAN of ten rows taken from a STANDOFF of eight rows
outside the band — one sampled row smears its own noise and any residual soft edge
down the whole patch, which is the same failure in a smaller form.

Side effect worth noting: the tiles got SMALLER (101 KB to 86 KB for all 32), because
what was removed was the busiest part of two of the four images.

### V57 — Apps and Tabs swap

**RULING — swap them globally; specifically, Track Mode's dial 4 becomes Apps.**
Apps takes dial 4, Tabs moves to dial 5, and each control keeps its own three
gestures.

**IT FOLLOWED FOR FREE IN THE ABLETON HUB, AND THAT IS THE ARGUMENT FOR V50's
REFACTOR.** Track Mode mirrors dials 1-4 through the shared `Root.osNavDial` rather
than copying them, so swapping the Root Hub strip moved the Ableton idle strip in the
same edit. Two hand-written copies would have needed two edits and would have drifted
the first time one was missed.

A consequence stated rather than hidden: **Tabs is now absent from Track Mode
entirely**, because that strip only mirrors 1-4 and dials 5 and 6 are Pan and Volume.
That is what Adi asked for — Apps on dial 4 there — and a test asserts the absence so
it reads as intended rather than as a loss.

The tabs artwork travelled with the control to dial 5. A test asserts that too: the
order being right while an icon stayed behind on dial 4 would have looked like a
rendering bug and been hunted in the wrong file.

---

## Batch 31 — Analyzer & Effects, and the pagination invariant

**FROZEN and untouched: EQ8's mapping, Pro-Q 3 data mapping, the Calculator.**

### V58 — the time-based effects come back, into the existing band

**RULING — rename Meters to "Analyzer & Effects" and add H-Delay, Valhalla and
Live's Delay to it. "We are NOT creating a new layout for them."** So cols 6-7 now
hold six: SPAN, bx_meter, Scope, Delay, H-Delay, Valhalla. Still inside the block's
eight, so nothing overflows yet.

**THE `id` STAYS `'meter'` WHILE THE TITLE CHANGES.** The id is the key into
`SOS.Bg` and into every test; renaming it would mean regenerating backgrounds.js for
no visible gain, since nothing paints the title — it is identity only. The two
disagreeing is deliberate and both the code and a test say so.

### The three device strings, and two traps in them

* **`Delay` is EXACT, and that is the Compressor/Glue trap again.** Live also ships
  **Filter Delay**, **Grain Delay** and **Echo** — verified against Live 11's own
  core library on this machine — every one of which a contains-pass would be free to
  pick. The remote script runs exact before contains, so the stock Delay wins.
* **`H-Delay` is a stem on purpose**: the browser entry is "H-Delay Stereo" or
  "H-Delay Mono", and the stem reaches either.
* **`Valhalla` is SPELLED OUT as `ValhallaVintageVerb`, not left as a stem.** A bare
  "Valhalla" would be non-deterministic: the project carries controllers for
  **ValhallaRoom AND ValhallaVintageVerb**, two different reverbs, so the stem would
  land on whichever the browser walked into first. Adi named one plugin, so this is
  one key. **ValhallaRoom is one line away if he wants it as well — flagged, not
  assumed.**

**NOT INSTALLED on this machine: H-Delay and Valhalla** (no Waves and no Valhalla in
any plug-in folder). Their keys will redden with "not installed" until they are,
which is the V49 behaviour working as designed rather than a fault.

### V58 — the pagination invariant, now actually pinned

**RULING — "Page 2 must remain an exact structural continuation of Page 1... the
overflow plugins must simply spill into their exact same respective columns on Page
2. Ensure this layout consistency is strictly maintained so muscle memory applies to
both pages."**

The mechanism was already right — `index = slot + itemPage * capacity`, per band —
but it was **only an intention, because nothing in the catalogue overflowed, so no
test had ever exercised the spill.** That is now fixed in both directions:

* page 2 shows the same four bands in the same order, and every cell on it still
  carries its own band's artwork (a band drifting sideways is the failure that would
  break muscle memory);
* **the artwork is identical on both pages** — the picture belongs to the block, so
  the tile follows the slot and not the page;
* **the overflow is FORCED in the test.** Three extra items are pushed into Analyzer
  & Effects and the spill is asserted to land in cols 6-7 **and nowhere else**,
  starting from the top of the block; and the three bands that did not overflow are
  asserted to stay EMPTY on page 2 — not shifted, not repeated. That last one is
  what makes page 2 a continuation rather than a remix.

### A count that should not have been hardcoded

Three assertions counted "14 loaders" and broke the moment a plugin was added. Two
of them were really asking *"is every catalogue item reachable and does it fire?"*,
so they now derive the expected number from `sum(min(items, capacity))` — the count
that actually FITS on page 1. That keeps them true when Adi adds a plugin and still
fails if an item becomes unreachable, which is the thing worth catching. The third is
a factual total and stays written down, because changing it should be deliberate.

---

## Batch 32 — V59: the Calculator is deleted

### V59 — RULING: "I find the standard Calculator module useless for my workflow. Please completely delete the calculator feature, its UI, and its associated code from the codebase."

Adi's words, and they override the FROZEN note that had been protecting the
Calculator since Batch 23. Frozen meant "write no new code for it"; it never meant
it could not be removed.

**The Calculator was State 1 of the carousel, so this is a RENUMBERING, not just a
deletion.** That is the whole risk in the change and it is the second time it has
happened — V13 removed State 3 (Context) the same way. The cycle is now:

```
State 0  Numpad      16-key dock, NO dials
State 1  Divisions   16-key dock + 2 borrowed dials (readout/grid/format + BPM)
State 2  NAV OFF     docks nothing
```

`COUNT` 4 -> 3, `FULL` 3 -> 2, `DELAY` 2 -> 1, `DOCK_COLS` `[4,4,4,0]` -> `[4,4,0]`.

**Every index lives in states.js and nothing outside it may hold a literal.** That
rule already existed (a hardcoded `4` in input.js silently un-reserved Button 1 when
V13 landed) but only the *engine* obeyed it — **the TESTS were full of literal 3s**,
and eight of them failed the moment COUNT changed. They now ask
`States.isFullScreen()`, `States.FULL` and `States.DELAY` by name, so a third
renumbering costs nothing. Two new assertions pin the shape itself:
`DOCK_COLS.length === COUNT`, and `FULL === COUNT-1 && DELAY === COUNT-2`. Two more
assert the ABSENCE — no state named `Calc`, and `Console.calculator` undefined —
because that is the only thing that catches a re-add.

### What went with it

| Removed | Where | Lines |
|---|---|---|
| the `calculator` screen (display row, digit grid, the 2 merged operator holds) | `js/modules/console.js` | ~96 |
| the arithmetic engine (`calcDigit/Decimal/Backspace/Clear/SetOp/CycleOp/Equals`, `applyOp`, `num`, `fmtCalc`, `OPS`) | `js/modules/console.js` | ~65 |
| the V12 grouped display (`chunks`, `withCommas`, `segment`, `segmentDim`, `isResting`, `SEGS`, `PLACEHOLDER`) | `js/modules/console.js` | ~49 |
| the `seg` / `segDim` render path — the four-key display screen | `js/core/render.js` | ~29 |
| `seg` / `segDim` in the `keySpec()` whitelist | `js/core/states.js` | 1 |
| `o.seg` / `o.segDim` from `hashId()`'s identity list | `js/core/render.js` | 2 |
| the `registerOverlay(1, ...)` registration | `js/modules/index.js` | 1 |
| the `calc` preview sheet | `scripts/preview.mjs` | 1 |
| blocks `[6]`, `[6b]`, `[9d]`, `[10]` — 24 assertions | `scripts/test_console.mjs` | ~160 |

**`seg` was the calculator's UI, so it left with it.** It had exactly one caller
ever, and the field note about the three hand-written whitelists
(`keySpec()` / `zoneUriFor()` / `preview.mjs`) applies in reverse here: this is the
first time a field has been *removed* from `keySpec()`, and all three lists were
checked in the same edit. `zoneUriFor()` never carried `seg` — it is a key field, not
a dial field — and `preview.mjs` reads `keySpec()` rather than listing fields itself,
so both were already correct.

**`hashId()` was edited BY LINE, never by string match**, per the standing field
note. Its join argument was verified byte-for-byte afterwards: still exactly one
literal backslash-u-0001 escape, and still zero raw control characters anywhere in
the file.

### Left behind deliberately, and reported rather than removed

**`o.flat` in `render.js`'s `face()`** — the flat-material flag — now has no caller.
It is one arm of one ternary inside the function that paints all 36 keys, and
reworking that path to reclaim a single word is a bad trade. `PALETTE.faceLo` is NOT
orphaned with it; the line below still uses it for every key's bottom stop.

### The open ruling this closes

**P5 — "whether `+` / `-` should stop being long presses in the calculator"** is
moot and is hereby closed. It was the last open item on the Calculator.

### A pre-existing flake found while verifying, NOT caused by this change

`scripts/test_service.mjs` is timing-sensitive and fails in a cascade under machine
load: a dropped `midi.ports` reply takes the five assertions after it down with it
(`virtual CoreMIDI port created`, `port published as a source`, `two notes tracked as
sounding`, `note off decrements`, `disconnect logged`). It passes clean on a quiet
machine, and the only diff this batch makes to `service/` is one comment. Worth a
real fix (the `ask()` helper needs a longer timeout, or a retry) but it is not this
batch's bug.

---

## Batch 33 — V60: the approved purge

### V60 — RULING: purge the true dead code the audit found

Adi, on `docs/AUDIT.md`: *"Let's officially purge the true dead code you found
(`SOS.SvgCtx`, `METER.corr/METER.bal`, the 14 empty engine exports,
`Rekordbox.wirePersist`, and the un-ported viz scaffolding). Keep the audio/viz path
and all Ableton controllers intact."*

**473 lines deleted, 152 added.** All fourteen controllers and the whole Web Audio
path are untouched, as instructed.

### `SOS.SvgCtx` — the big one, ~258 lines

The Canvas-2D-emits-SVG shim. It existed so the thirteen legacy controllers could be
copied in byte-for-byte rather than rewritten — the right call at the time, and the
reasoning is kept as a comment because "why is the compositor shaped like this?" is
otherwise a real half hour for the next reader. L4 then ported all fourteen to native
`build()`, which left it serving nobody.

Gone with it: `AVC.DeviceController.prototype.renderTouch`, `Ableton._ctx`, the `ctx`
instance, `composite()`'s shim fallback branch, and `AVC.gfx`'s three Canvas-only
drawing helpers (`clear`, `roundRect`, `text2`). **`AVC.gfx`'s COLOURS stay** — `bg`,
`text` and `bandColors` have 14, 26 and 15 controller references between them.

**`composite()` now has ONE path.** A controller without `build()` logs and leaves the
strip alone, which is the loud failure the shim's own header comment asked for.

**Its tests went with it** — block `[3]`, 13 assertions, was the shim's only remaining
caller. **But the compositor probe was rebuilt, not deleted:** it used `SvgCtx` merely
as a convenient generator of known geometry, while the mechanism it tests is the
per-zone clipping that fixed the 17.5 KB payload bug. It is a native `SOS.Svg.bag()`
now and still asserts both halves. Two absence assertions replace block `[3]`:
every controller implements `build()`, and `SOS.SvgCtx` is `undefined` — the second is
the only thing that catches a re-introduction.

### `METER.corr` / `METER.bal` — and the reason they cost more than two lines

Written every frame, read **nowhere**: their two views (`corr`, `bal`) were never
ported. The tell was that `audioStop()` reset the four meter fields and not these two.

The real cost was the three accumulators feeding them from **inside the per-sample
loop** — and `sLL` / `sRR` were byte-for-byte duplicates of `sL` / `sR` (both
`+= a*a`, `+= b*b`). Every sample of every block paid for three redundant
multiply-adds at 15 fps to produce two numbers nothing drew.

`test_viz.mjs`'s balance and correlation assertions were their only readers, which is
the whole point. Replaced with an absence assertion, so a pass that re-adds the
compute without a view to draw it fails.

### The un-ported viz scaffolding

`RME_BANDS`, `RME_FLO`, `RME_FHI`, `RME_LABELS` — the ISO 1/3-octave table for the
un-ported `rme` view, defined once and referenced nowhere. And the `DEFAULTS` blocks
for all five un-ported views (`bands`, `rme`, `gonio`, `corr`, `bal`).

**The picker still lists all nine views and still paints "not ported".** Every read is
`DEFAULTS[view] || DEFAULTS.spectrum`, so the fallback already covered a missing key.
One visible change, small and worth writing down: `viewColor('gonio')` now returns
`PALETTE.viz` instead of `#38f0a0`, because gonio was the only un-ported view carrying
a colour.

**`RME_LIT` / `RME_OFF` / `RME_MARK` / `RME_MARK_OFF` STAY.** Despite the name they
serve the `meters` view's `style: 'rme'` segmented-LED variant, which is live. Named
for the un-ported view, used by the ported one — exactly the trap the audit flagged.

### `Rekordbox.wirePersist` — D17 is CLOSED by deletion

The apparatus was a seam: `wirePersist(fn)` to hand in a writer, an 800 ms debounce,
and a validating `restore(saved)`. It was left unwired until the orchestrator owned a
namespaced store, because the global settings object is shared by every module and a
module writing it directly reintroduces the 1.0.1.0 read-modify-write race one level
up.

**That store never arrived.** `persist` was null for the entire life of the project,
`saveLevelsSoon()` returned on its first line every time a dial moved, and `restore()`
was never called. Encoder levels reset on every launch, which is the behaviour Adi has
been living with. `SAVE_DEBOUNCE_MS`, `snapshot()` and `Rekordbox._keys` went too.

**The reasoning is kept as a comment**: if persistence returns, it returns as a
namespaced store owned by the orchestrator, never as a module writing shared settings.

### The engine exports

`IPC.droppedCount`, `IPC.DEFAULT_URL`, `States.overlayScreen` (a dead alias of
`navScreen`), `Nav.keyBinding`, `SD.deviceOfType`, `SD.flushDirty` (superseded by
`flushCounts`), `SD.sendToPI` (there are no Property Inspectors — D1),
`Surface.isKey`, `Surface.OVERLAY_COL_MIN`, `Surface.inOverlay`,
`Clock.CELL_W/CELL_H/ADV_DIGIT/ADV_COLON`, `Ableton.setUrl`, `root.defineSlot`,
`root.clearSlots`, `Viz._start`.

### TWO ITEMS WERE PUT BACK, and the reason matters

The audit listed both as "test-only", and **test-only is not the same as dead**:

* **`Clock.LIT_COLOR` is RESTORED.** Its one caller pins the Root Hub scroll arrows
  and the clock's lit digits to the SAME palette entry so they cannot drift into two
  similar blues. Deleting it deletes an invariant. Its sibling geometry constants
  really were export-only and did go.
* **`Surface.inOverlay`'s assertion was dropped rather than rewritten**, because it
  asserted `OVERLAY_COL_MIN` against itself. The dock boundary already has real
  coverage on the LIVE path in `test_core [7]` — "col 8 belongs to the docked window",
  "col 4 belongs to the module" — both read through `Layout.split()`.

**The general lesson for the next purge: check whether a test-only export is holding a
cross-check before deleting it.** One of these two was, and the sweep that found them
could not tell the difference.

---

## Batch 33 (cont.) — V61: the Ableton control centre

### V61 — RULING, and it CORRECTED A MISREADING OF MINE

I read "use the hardware keys above the touch screen as Mode/Folder selectors" as a
persistent global row, and drafted a plan whose cost was shrinking every plugin band
from 8 cells to 6 and re-slicing `backgrounds.js`. Adi:

> "You misunderstood my layout intention. The Mode Selectors are **NOT** a persistent
> global row that stays visible everywhere. They are simply folder/navigation keys
> that live exclusively on the **Ableton Home Page (Level 1)**. Therefore: **DO NOT**
> shrink the VST layout. **DO NOT** re-slice the images. When I press the `VST` folder
> on Level 1, it navigates to the VST Page (Level 2), which remains exactly as it is
> today (full 4 rows, 8 cells per category)." And: "These Mode Selectors are
> **Ableton-only**."

**That resolves B1, B2 and B3 at once, and the resolution is better than the
workaround.** Because the mode keys are Level-1 only, putting them on **row 3** — the
row physically nearest the touch strip, which was the point of "above the touch
screen" — costs *nothing*: Level 1 has no bands to displace. `backgrounds.js` is
untouched and `slice_backgrounds.py` did not need running.

**The lesson: my B1 blocker was real, but B2 only existed because of the wrong
reading. When a plan's cost looks disproportionate to the ask, the reading is the
thing to re-check first.**

### The two screens

```
LEVEL 1  ableton.hub — the control centre. What the Root Hub tile opens.

     col 0     col 1    col 2     col 3    col 4    cols 5-8
 r0  BACK      PLAY     STOP      LOOP     ·        ·
 r1  ·         ·        ·         ·        ·        ·
 r2  ·         ·        ·         ·        ·        ·
 r3  VST       MIDI     Device    OS       Delay    ·

LEVEL 2  ableton.vst — the OLD hub, unchanged. Same hubKeys(cols), same four
         two-column bands, same 8 cells each, same pagination, same sliced
         artwork, same utility column (MIDI / Prev / Next / NEXT).
```

Rows 1-2 and cols 5-8 of Level 1 are **deliberately empty** — that is the room the
split bought, and filling it is Adi's call.

**A test guards the ruling directly**: it counts the VST page's band cells and fails
unless all 32 are still there across all four rows. That is the assertion that would
have caught my original plan.

### Strip focus — the green VST key is a DECOUPLING, not a tint

Adi: *"If I press BACK to return to the Level 1 Ableton Hub, the VST folder key MUST
remain highlighted to clearly indicate that the dials and touch screen are still
actively controlling VSTs."*

Until now the strip followed NAVIGATION — `composite()` painted only while the hub's
own `dials()` was being asked, so Back stopped the module owning the strip. Ownership
is now one module-level variable that **nav never touches**:

| `focus` | strip |
|---|---|
| `none` | empty — **the Level 1 default, per Adi's ruling** |
| `vst` | the device/macro controller owns all six zones |
| `mix` | Ableton track controls (Device mode) |
| `os` | the Root Hub's OS navigation strip, on explicit request |

**THE TINT FALLS OUT FOR FREE.** Each mode key paints `active: focus === '…'`, and
`active` is already a `keySpec()` field that `render.js` draws as a lit cap. **Nothing
was added to the three hand-written whitelists** — no new render path, no new binding
field. That was a design goal, not luck: that trap has bitten twice.

**THE PUMP LIFECYCLE IS THE SUBTLE PART.** `nav.js`'s `enter()` pushes and calls
`onEnter` on the NEW screen without touching the parent; `pop()` calls `onExit` on the
POPPED screen only. So `stopPump()` belongs on **Level 1's** `onExit` — that fires
when Level 1 is popped, i.e. going up to the Root Hub, the one moment the module
really stops owning the surface. Putting it on the VST screen instead would kill the
strip on the way BACK to Level 1, which is exactly the retention that was asked for.
**Level 2 has no `onExit` at all, on purpose.**

Focus is **sticky across leaving the module**: come back to Ableton and the strip is
where you left it. Not asked for either way; it is the behaviour that surprises least.

### This is a THIRD orthogonal state machine, and it is fenced accordingly

`focus` sits beside nav level and the carousel state, and this project has been hurt
in exactly that spot twice — a hardcoded `4` in `input.js` when V13 removed a state,
and eight literal `3`s in the test suites when V59 removed another. So:

* nothing outside `ableton.js` compares `focus` to a literal; the values live in
  `FOCUS`;
* the tests assert the **shape**: every mode key's focus value must be a member of
  `FOCUS`, no two modes may claim the same one, `FOCUS.NONE` is claimed by none, and
  at most one mode key may be lit at a time.

### The OS mode key — AN INTERPRETATION, flagged

Adi asked to *"remove the standard OS Nav controls (Scroll, Zoom, Apps, Tabs) from the
touch screen and dials whenever we are inside the Ableton Hub"*, **and** kept `OS` in
the list of five folders. Those pull in opposite directions, so: the OS strip is no
longer the **default** (that is `FOCUS.NONE`, empty), and the OS mode key is what
brings it back on request.

That is the only reading under which the OS key is not dead on arrival — the other
four mode keys are strip-focus switches, so this one is too. **One line to change if
Adi wants it to navigate to the Root Hub instead.**

It is still MIRRORED from `Root.osNavDial`, never copied. Two hand-written copies of
the same five dials is how "the standard OS navigation strip" quietly stops being
standard — and a test still pins it, because that mirroring is why the V57 Apps/Tabs
swap propagated for free.

### Track Mode is now Device mode

V50's idle Track Mode (dials 1-4 mirroring the OS strip, Pan on 5, Volume on 6) was an
*idle fallback*; it is an explicit *mode* now. **Pan and Volume keep their exact
physical positions — 5 and 6 — because moving a working control to tidy a layout is
not an improvement.** Dials 1-4 are deliberately empty, reserved for Mute / Solo /
Record Arm, per *"leave those dial/touch slots empty for now so we can build dedicated
Track/Mixer controls there later."*

**Those three need three more additive remote-script verbs that do not exist.** Checked
the script: there is no mute/solo/arm verb of any kind.

### The transport, and ONE new remote-script verb

**`transport`, carrying an action** — `play` / `stop` / `loop` — rather than three
separate verbs. Purely additive, which is the V30 exception, and one addition is a
smaller change to a file that "must not be modified" than three.

* `play` sets `song.is_playing = True` — Live's own "play from here", which is what
  the spacebar does. `start_playing()` would always jump to the start marker, which is
  a different control.
* `stop` calls `song.stop_playing()` rather than `is_playing = False`: the former also
  returns the playhead to the start marker, which is what a transport STOP means in
  Live as opposed to a pause.
* `loop` toggles `song.loop`.

It goes through the Song rather than firing keystrokes, so it works with Live in the
background and cannot be eaten by whatever window has focus.

**The script pushes a `transport` message back**, so Play and Loop LIGHT from Live's
own state. **Stop has no lit state at all** — it is momentary, not a toggle, so
`active` is simply absent on that key rather than `false`.

**⚠️ LIVE MUST BE RESTARTED** after deploying the remote script, or these are
fire-and-forget messages into a script that has never heard of them. The remote-script
change is a **separate commit in the sibling folder.**

### The transport icons are DRAWN, not typed

The proven glyph set has `▶` but **no filled square** and nothing that reliably reads
as a loop. And one drawn shape beside two font glyphs gives three adjacent keys three
different optical weights. So all three are vector icons in `js/core/icons.js`
(`transportPlay` / `transportStop` / `transportLoop`), on a 56×56 box so each sits
centred at the same size. Same reasoning as the nine window states, and a test pins it
the same way: no glyph, a real icon name, three different pictures, all present in the
registry.

**Looked at, not just tested**: rendered at cap scale through the real `render.js`
before committing. The loop's ring gap is wide enough to read and the arrowhead does
not collide with it.

---

## Batch 34 — V62: Mute / Solo / Arm, and OS mode confirmed

### V62a — RULING: the OS key was right

> "You were completely right about the OS Mode interpretation. I DO want the OS key
> to bring back the Scroll/Zoom/Apps/Tabs strip inside the Ableton Hub when needed."

V61's flagged interpretation is now a ruling. No code change — the behaviour shipped
that way. The note in V61 is upgraded from "one line to change if Adi disagrees" to
settled.

### V62b — RULING: "Write the Mute, Solo and Record Arm verbs and map them to dials 1-3"

Device mode now owns FIVE of the six dials:

```
dial 1  Mute    push toggles      dial 4  (still spare)
dial 2  Solo    push toggles      dial 5  Pan      turn
dial 3  Arm     push toggles      dial 6  Volume   turn, 0.5 dB
```

**ONE additive verb again**, `track_toggle` carrying a `which` of `mute` / `solo` /
`arm` — same reasoning as V61's `transport`: the smallest possible addition to a file
that must not be modified. **Live must be RESTARTED.** Separate sibling-repo commit.

### They are PRESSES, and that is the opposite of the rule for 5 and 6

A toggle has two positions, so a detent-per-turn would need a direction it does not
have. And the reason Volume must not take a press — an accidental push changes a level
you were reading — simply does not apply to a control whose entire job is to flip.
Turning dials 1-3 is deliberately inert, and a test asserts it.

### THE TRACK TYPE MATTERS, AND LIVE DOES NOT WARN YOU

**A return track has no `arm`. The master track has none of the three.** Setting a
missing attribute on a Live object raises. So:

* each of the three is read through its own `getattr` in its own `try` — a single
  blanket try would drop all three because one was missing;
* `None` is sent for "this track cannot do that", which is **not the same as OFF** and
  must not look like it. The zone paints an em dash with **no indicator bar at all**,
  dimmed. A full/empty bar would read as a working control, so a master track would
  look like three un-muted toggles that silently do nothing;
* the frontend **refuses the press outright** on an unsupported toggle rather than
  sending a verb that cannot land;
* `can_be_armed` is asked before arming when Live exposes it — cheaper than catching
  the exception, and it logs a real reason.

Three states, three looks, all three asserted: `OFF` + dark bar, `ON` + full bar +
the mode's colour, `—` + no bar + dimmed.

### The toggles are WATCHED, exactly like Volume and Pan

`_mix_listen` now also attaches to `mute` / `solo` / `arm` so clicking Mute with the
mouse moves the dial. These are track PROPERTIES, not device parameters, so they take
`add_<name>_listener` on the track rather than `add_value_listener` — a different
teardown call, which is why they live in their own `_mixtoggles` list. **Leaking them
would fire `_emit_mix` for a track that is no longer selected**, which reads on the
surface as a dial that will not settle.

### DEPLOYED — Batches 32, 33 and 34 are on the hardware

`./scripts/deploy-mac.sh`, twice. Fresh log both times, `service v2.5.0`,
`surface COMPLETE — 36/36 keys, 6/6 dials`, no errors, app idling at 0.9%. The
DEPLOYED files were grepped for the change markers rather than trusted, including
`SOS.SvgCtx` being absent from everything but two history comments.

**Ableton was not running, so the remote script is in place and will be picked up on
its next launch.** Both `transport` (V61) and `track_toggle` (V62) land together.

### V62c — test_service.mjs's flakiness is FIXED, and the scary hypothesis was wrong

The suite had failed in a cascade under load twice, and the obvious suspect was a
robustness bug: a malformed frame killing the service, with the assertion checking
`exitCode` 100 ms too early to see it. **Tested directly, on a throwaway instance on
its own port: a malformed frame does NOT kill the service, and it answers
`midi.ports` immediately afterwards.** The product code is fine.

Two real causes in the TEST, both fixed:

1. **`await wait(700)` for boot was a guess.** Plenty on an idle machine, not always
   enough on a busy one. It polls the real health endpoint now.
2. **CoreMIDI enumeration was capped at 2.5 s**, and it gets slower when another
   process — the DEPLOYED service — holds virtual ports open, which is exactly the
   state right after a deploy. Every MIDI assertion read the reply from that one
   call, so ONE timeout took six down with it. The MIDI verbs get 12 s.

**14 consecutive clean runs, 6 of them with three other suites running concurrently.**

---

## Batch 35 — V63: the red traffic light could kill Ableton, and did not know it

### V63 — EMERGENCY FIX, at Adi's instruction

> "Fix the `NEVER_QUIT` guard immediately. Change the protected process name to "Live"
> (and ensure Windows is properly protected too) so it actually prevents Ableton from
> being force-quit. Make this code change and DEPLOY it right now."

**The guard had never worked for Ableton.** `guarded()` matches on the frontmost app's
**process name**, and Ableton's process name is **`Live`** —
`/Applications/Ableton Live 11 Suite.app` has `CFBundleName` *and*
`CFBundleExecutable` both set to `Live`. The list said `"Ableton Live"`, so
`guarded("Live")` was **false**: short press = graceful quit, long press = `kill -9`, on
the one application whose loss costs a session of work. It was live on the hardware for
every batch since V55 shipped the key.

`"Live"` now comes FIRST in the list, because it is the string that actually arrives.
`"Ableton Live"` and `"Ableton"` are kept — they cost nothing, and prefix matching then
also covers `Live 11 Suite` / `Live 12` and the Windows spelling.

**THE FAILURE MODE OF THIS LIST IS ASYMMETRIC, and that is the design rule to remember:
a name that matches too much REFUSES TO QUIT something; a name that matches too little
KILLS something.** So it errs wide on purpose. Prefix-matching `"Live"` would also
protect a hypothetical `LiveFoo.app` — an annoyance, against the alternative.

### Windows was not protected at all

Both verbs returned **before** the guard was ever consulted:

* `quitFront` returned `hotkey("alt+f4")` on the first line.
* `forceQuitFront` ran `Get-Process | Where MainWindowHandle -ne 0 | Select -First 1`
  and force-killed it. **That is not the frontmost app** — it is whichever process
  Windows happens to enumerate first with a window, so it could kill something the user
  never touched.

Both now resolve the real foreground window via `GetForegroundWindow` +
`GetWindowThreadProcessId` (new `WIN_FRONT_SHIM` / `winFrontApp()` / `psOut()`, all
additive), run `guarded()` on the result, and `forceQuitFront` kills **that pid
specifically** with the same `pid <= 4` sanity floor the macOS path uses.

DECISIONS.md:2931's claim that *"Both verbs resolve the frontmost app FIRST and refuse by
name"* was false on Windows and ineffective for Ableton on macOS. It is true now.

### The test is BEHAVIOURAL, and there is a lesson in why

`guarded()` is exported now so the test calls it instead of grepping for a name. That is
deliberate: **the audit found that `test_service.mjs`'s neighbouring assertion had been
passing by matching a COMMENT** — it required `/axFullScreenToggle\(\)/` in os.js's
source, and no such function exists; the only occurrence is a comment explaining why the
approach was abandoned. Deleting the comment would have failed the test; breaking the
feature would not. That assertion is now the attribute write alone.

Seven new assertions: Ableton protected under all four spellings, the Stream Deck app and
the five system processes still protected, **ordinary apps still quittable** (a guard that
protects everything is a red key that does nothing, which reads as broken), and an
empty/null name not protected by accident.

**And the same trap bit me immediately.** My first Windows assertion also required the
ABSENCE of `"MainWindowHandle -ne 0"` — and failed, because that string is in the comment
recording what the old code did. **An absence assertion against a file that documents its
own history is fragile by construction.** Rewritten as positives: `GetForegroundWindow`
and `GetWindowThreadProcessId` present, and `Stop-Process -Id ${w.pid} -Force` present.

### Deployed

`./scripts/deploy-mac.sh`. Fresh log, `service v2.5.0`, `surface COMPLETE — 36/36 keys,
6/6 dials`. **The DEPLOYED `os.js` was then re-parsed and its real `guarded()` executed
against the real list** — `guarded("Live")` is `true` and `guarded("Google Chrome")` is
`false` on the copy that is actually running. 1156 tests green.

---

## Batch 36 — V64: the Master Repair, step 1 — backend and Python

Adi authorised the full purge and repair after Phase 4, in a strict order. This is step 1.

### The security hole: the AdiVST handshake accepted anything

`ws_server.py`'s `_try_handshake` validated only that *some* `Sec-WebSocket-Key` header
existed — any method, any path, any protocol version, **any Origin**. Measured before the
fix: a `POST /anything HTTP/1.0` carrying `Origin: https://evil.example` got a **101** and
its verb reached the dispatcher.

**That matters because WebSocket handshakes are not subject to CORS.** A browser will open
`ws://127.0.0.1:9006` from any page with no preflight, so any web page open on this Mac could
reach the full verb table — including `eq8_load_preset`, which calls `track.delete_device`.

Now: `GET` only, `Upgrade: websocket` and `Connection: Upgrade` required,
`Sec-WebSocket-Version: 13` required, and **Origin absent or `null` only**. The Origin check
is the one that actually stops a browser: a browser always sends it and cannot forge it, while
our CEF frontend is a `file://` page and sends either nothing or `null`. Verified — every
hostile shape refused with nothing reaching the dispatcher, both legitimate shapes still get
101.

### The half-open socket, and why a note could stay sounding

`http.Server` builds its sockets with `allowHalfOpen: true`, so when a CEF page vanishes the
server socket gets **`'end'`** and then stays writable forever — **`'close'` is never
emitted.** Measured on the app's own bundled node 20.20.0: `'end'` true, `'close'` false at
+300 ms, +1 s and +2 s.

So `socket.on("close", …)` — the only reaper — never ran, and the 15 s heartbeat was left to
do it in **two** cycles because cycle 1 only arms `awaitingPong`. Thirty seconds of a phantom
client, during which `index.js`'s `clients.size === 0` never becomes true, **so the real
client leaving never triggers `midi.panicAll()`.** A held note could stay sounding — the one
guarantee the service's own header makes.

Three changes: listen for `'end'` and `terminate()`; `_gone()` now **destroys the socket and
drops its buffers** (it used to leave all three listeners attached, and since the heartbeat
only walks `server.clients`, nothing could ever reap that socket again — measured at 1 MiB
retained per continuation frame); and `_feed` refuses to run on a dead client.

Verified on the real runtime: abrupt teardown now reaps in **under 150 ms**, `panicAll` fires,
and two fast reconnects plus one live client report **1 client, not 3** — the original F5
symptom, which `deploy-mac.sh` reproduces by restarting the app twice.

**One correction to the audit:** it reported verbs still dispatching after `_gone()`. The
frame loop already `return`s on CLOSE, so the specific CLOSE-then-more-frames case was
*already* correct — verified. The new entry guard covers the direct-`_feed`-after-death path
instead. Less dramatic than reported.

### The Windows guard list did not exist

V63 taught the Windows path to consult `guarded()`, but **every entry in the list was a macOS
process name.** Elgato's Windows build runs as **`StreamDeck` — no space** — and neither
"Stream Deck" nor "Elgato Stream Deck" is a prefix of `streamdeck`, so the red key could kill
its own host. Added: `StreamDeck`, `explorer`, `dwm`, `csrss`, `winlogon`,
`ShellExperienceHost`, `Taskmgr`. Verified all seven now protected, macOS unchanged, and
ordinary apps still quittable.

### A listen failure must exit, not limp on

`ws-server.js`'s error handler logged and **kept running**: measured, after `EADDRINUSE` the
process stayed alive forever with no listener, so every key was a silent no-op and `KeepAlive`
could not rescue it because it never exited. It now exits on `EADDRINUSE`/`EACCES` and says
why. Verified: second instance exits 1 with both log lines.

### The Command key is released on the way out

`os.appSwitch` holds Command down across the whole gesture and never releases it; all three
releases lived inside the process. **A deploy SIGTERMs the service**, so a SIGTERM between a
dial-5 turn and its press left Command logically down with no timer left to release it.
`shutdown()` and both crash handlers now call `appSwitchCancel()` — the keystroke analogue of
`panicAll()`. A named `unhandledRejection` handler was added too, so a floated promise no
longer arrives as a contextless `uncaughtException`.

### `song` is a property now, and that one line was a class of bug

It was captured **once** in `__init__`. `_Framework` exposes `song` as a *method* — the
framework expects it re-read — and loading a different Live Set kills every handle in the old
Song. Measured: `resend_all()` raised `Accessing out of date Live object`, **after** sending
`hello`, so the frontend had already set `online = true`. The hub read as connected with a
dead surface, and every later subscribe threw identically.

A property fixes **all 26 call sites without touching one of them** — which also sits far more
comfortably inside "no editing existing code paths" than 26 edits would.

### `hasattr(track, "arm")` could abort a whole track change

`hasattr` swallows `AttributeError` and nothing else. If Live raises anything else for `arm` on
a return or master track, the exception escaped `_mix_listen`, aborted `_on_track_changed`, and
left the hub showing the **new** track's name with the **old** track's device page and dials —
permanently, because nothing retries. Reachable only by mouse-clicking a return track, which
is exactly how it escaped every test. Now a `getattr` probe inside its own `try`, matching what
`_emit_mix` did 110 lines above and `cmd_track_toggle` 80 lines below. Both exception types are
now tested.

### The transport was never watched, and never sent on connect

V61 added the verbs and `_emit_transport` — whose only caller was `cmd_transport` itself. So
Play and Loop rendered unlit on every connect even while Live was playing, and pressing
spacebar in Live changed nothing. **V62 got this right for mute/solo/arm one version later.**
Now `setup()` listens on `is_playing` and `loop`, and `resend_all` emits it.

### Every mixer touch emitted `mix` twice

The setter moves the parameter, Live's value listener fires `_emit_mix`, and then the command
called `_emit_mix` again by hand. The frontend's `mix` case also fires `state`, so a fast dial
spin cost **two full surface re-renders per detent**. `_emit_mix` now coalesces against the
last payload, with `force=True` for `resend_all` and `cmd_get_mix` — deduping rather than
deleting one call, because the hand call is the only emit on a track with no listener
registered.

### Stale device positions, both halves

`_on_device_changed` emitted `device` but never `device_pos`, so a mouse click on a different
device left the Prev/Next "n/m" caption stale. Its **sibling** `cmd_device_key` had the same
gap, which is why the arrows looked right and the plugin keys did not. Both fixed — **and the
first attempt only fixed half of it**: the emit sits below an early return, so deselecting a
device still went stale until the no-device path got its own call.

### `_unlisten`'s bookkeeping moved inside its own `try`

The list rebuild compares **tuples**, so for every other entry it can call `__eq__` on a Live
handle that is already dead — and this method exists precisely to tolerate dead subjects. It
now falls back to dropping by **identity**, which cannot touch a dead object.

### THE TEST SUITE NOW RUNS THE REAL CONSTRUCTOR

Phase 3's structural finding was that all four test bridges override `__init__` and never call
it, so `setup()`, `teardown()` and the whole listener lifecycle had **zero coverage** — which
is exactly the surface every bug above lived on.

The four stubs now expose their song through a `_cs.song()` the way `_Framework` really does,
so they go through the property that was just fixed. And a new block `[20]` instantiates the
**real** `LiveBridge` against a Live-shaped fake that models Live's listener registry *and* its
habit of raising for a missing attribute. **55 → 70 assertions**, covering: transport listeners
and emit-on-connect, mix coalescing, both `arm`-raises variants, a new Live Set not wedging the
bridge, `device_pos` following the selection, and teardown leaving nothing behind.

---

## Batch 36 (cont.) — V64 step 2a: the three UI bugs, and a root cause underneath them

### THE RING BUFFER WAS NEVER WRITTEN, NOT ONCE, SINCE THE ORIGINAL PORT

Found while verifying the scope readout, and it is much larger than the bug I was
chasing. `ringPush(cl, cr, cm)` takes **blocks** — `var n = cl.length`. `push()` called it
**per sample with three scalars**:

```js
ringPush(a, b, (a + b) * 0.5);        // a is a NUMBER
```

So `cl.length` was `undefined`, the loop `i < undefined` never ran, and `ringW` never
advanced. **Nothing was ever written to any ring buffer.**

Everything that reads the ring therefore read pure silence: **the spectrum, the scope and
the waveform — three of the four implemented views.** The meters looked fine because they
are computed from the block directly and never touch the ring, which is exactly why nobody
caught it: the one view you could sanity-check was the one view that did not depend on the
broken path.

**Provenance checked: this is not a V60 regression.** `git log -S` puts it in `38f97a8`,
the original "working Visualizers" commit, and it was already per-sample before my purge —
I preserved the line verbatim. So **the visualizers have never had signal in three of four
views**, and that sits *underneath* the missing input picker rather than beside it.

Fixed by writing the block once, outside the loop, with an explicit `count` so a reused
mono scratch buffer larger than the current block cannot contribute stale tail samples.

### The scope and waveform dials showed an ellipsis

`_scopePeak` and `_wavePeak` were computed every frame and read by **nothing**, while only
`svgMeters` ever populated `this.head` — the same shape as the `METER.corr` / `METER.bal`
bug V60 removed. **Two more instances of it survived that purge.**

Rather than delete the loops, they now feed the readouts they were plainly written for:
peak in dB plus the window length. With the ring fix above, all three level views now
report a real level — verified, and pinned by nine new assertions.

**Spectrum is deliberately excluded from that assertion**: its `head` is the SPAN-style tap
readout, empty until you touch the strip. That is the marker slot, not a level.

**And a note on how I nearly mis-read this.** My first check showed an em dash and I almost
took it for a broken fix. It was an empty ring: the waveform reads the most recent
`windowMs` of samples — 1.5 s, i.e. 72000 at 48 kHz — so a single 4096-sample push leaves
the read window almost entirely zero and the peak legitimately reads as silence. The test
now fills the ring properly and says why.

### `dim` now dims the whole zone

It was read in exactly ONE place — inside `zone()`'s icon branch. **Eight dial bindings set
`dim`** to mean offline or unsupported (five on the Root Hub, three in Ableton's mixer
mode) and only the two carrying an icon ever showed it. So all three V62 mixer toggles plus
Pan and Volume announced "bridge offline" in words while looking exactly as live as ever —
and the em-dash "n/a on this track" case I had deliberately made distinct was only half
distinct.

Applied as one group opacity over the whole zone: it cannot be forgotten by a future
element the way a per-call check was. The icon's own `opacity="0.5"` was dropped, because
two 0.5 layers read as 0.25 and look broken rather than dim.

### The V61 pump gate had to widen

`var onHub = SOS.Nav.current() === hub` was exactly right before the split, when `hub` WAS
the VST grid. After V61 it named Level 1, so on **Level 2 — the screen the controller strip
exists for** — `composite()` was never called by the pump and the re-arm dropped to 250 ms.
Not frozen, because `Bridge.on('state')` still composites on every Live message; but
anything the controller animates itself only redrew when Live spoke or a dial moved.

Asked by screen identity (`cur === hub || cur === vst`) rather than by `focus`, because the
pump's job is to paint and both screens paint through the same `focusDial`.
