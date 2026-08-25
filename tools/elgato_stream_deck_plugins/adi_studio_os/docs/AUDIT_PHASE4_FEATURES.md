# Phase 4 — Feature Gap Report

**Not a code audit.** Phases 1–3 covered dead code, leaks and bugs. This one answers a
different question: **what have we forgotten or left unfinished?**

Sources read end to end: `DECISIONS.md` (3601 L), `ARCHITECTURE.md`, `CONTINUE.md`,
`PLAN_ABLETON_HUB.md`, `EQ8_MAPPING.md`, the master `HANDOFF.md`, **all five legacy plugins'
READMEs, CHANGELOGs and manifests**, and 111 commit messages. **Nothing modified.**

I independently verified the four highest-stakes claims. All four held, and one **corrects
something I have told you repeatedly.**

## Two corrections to my own earlier reports

**1. "Pro-Q 3: 34 roles declared, 0 ever bound" was misleading.** The roles are **fully
wired** — `onDial` maps slot → band → mode → role → `deltaIndex`/`deltaLogIndex`
(`ProQ3Controller.js:290-296`), and `onTouch` maps the three pills to shape/slope/stereo
(`:314-323`). What has never happened is **resolution**: the roles match *by name* against
Live's `all_params`, and on failure the controller logs `ProQ3 unresolved roles: …`. So the
remaining work is **matching your six Configured band names**, not implementing the
controller. Much smaller than I implied.

**2. The spectrum "wall" was never a legacy feature.** `grep -i wall` finds nothing in the old
visualizers plugin. It is a Studio OS design of *mine* (`viz.js:149-150`) — so it is a plan I
dropped, not a capability you lost.

---

# The short version

- **The acoustic tool is gone, and the log says it survived.** Note → Hz at A4 = 442, octave →
  wavelength in cm. It was one of your six console actions — the legacy plugin's own manifest
  **advertised it in the product description**. L5 ratified it: *"The acoustic readout (A4=442)
  survives on the header row, so nothing was dropped."* Commit `88698d2` then deleted it seven
  lines at a time and never mentioned it. **Verified: nothing of it exists today.**
- **The visualizers cannot choose an audio input — and that, not a reboot, is the real
  blocker.** The old plugin had an "Input device" dropdown that passed `deviceId` through.
  `viz.js:818` calls `getUserMedia` with **no `deviceId`**, and `enumerateDevices` appears
  nowhere. **Verified.** So BlackHole has to become your *system default input*. "Needs a
  reboot" has been the standing answer for weeks; the actual gap is a missing picker.
- **The rekordbox wake-from-sleep MIDI reopen does not exist, and a comment says it does.**
  `rekordbox.js:576` claims *"the wake-from-sleep reopen … belong[s] to the service now."*
  **Verified: no wake hook anywhere in the project.** Recovery is reactive only — the service
  reopens *after* a send fails, so the first hot cue after waking the Mac is silently dropped.
- **Nothing survives a restart.** Both legacy MIDI plugins persisted state. Studio OS persists
  none: rekordbox encoder levels (D17, deleted), MIDI Control's root/scale/channel/bank, the
  visualizer slots. **One missing piece — a namespaced settings store — blocks all of them,
  plus D16 (the Windows blocker), plus the `OVERRIDES` loader, and it gives the three dead
  Property Inspector fields a purpose.**
- **Six macOS window placements are built and unreachable**: the four *quarters*, *Center*, and
  *Return to Previous Size*. Both platforms written. V43 called the window block *"the macOS
  popover, row for row"* — it is missing the popover's third group. Row 1 of the Root Hub has
  exactly four free cells.
- **Five of your fourteen VST controllers have no key to insert their plugin** — ValhallaRoom,
  Eventide Blackhole, Omnipressor, Saturate, SideMinder. Each was its own commissioned
  release. All five fit page 1 with a slot spare. Only ValhallaRoom was ever flagged.
- **You cannot change the selected track from the board.** `select_track` is live in the
  frontend *and* deployed in the remote script — zero callers. Device mode's **dial 4 is
  spare.** Sends A/B from the same plan were never ruled either way.
- **Turn NAV on inside rekordbox, MIDI Control or Meters and the board prints "No room /
  needs 9 cols."** L6 promised every module two layouts; three still have one. L2 was ruled
  twice and never built.
- **The visualizers' entire configuration surface vanished** — ~35 controls (FFT window, block
  size, overlap, averaging time, slope, ranges, colour, A4 tuning, readout hold, scope
  trigger/threshold/amplitude…). **The engine still reads every one of them.** Only the UI is
  gone.
- **Cubase is completely absent** — ARCHITECTURE gave it Key 2; there is no screen, the tile is
  `col: null`, and the `/Applications` probe still runs on every `os.actions` call.
- **Studio OS has no `validate.py`, no packaging script and no CI.** It cannot currently be
  built into a distributable `.streamDeckPlugin`.

---

# 1. Forgotten — nobody ever decided these

Each: what it was · whose idea · last mentioned · today · work · my lean (an opinion, not a
decision).

### 1.1 The acoustic tool — note → Hz, octave → wavelength ⭐ verified
Its own action (`com.adiariel.console.acoustic`) with a touch layout. Dial scrolled note or
octave; the zone showed frequency at A4 = 442 and wavelength in cm (C0 = 2100.34 cm). **Yours.**
Ported in `f2fb107`, ratified at `DECISIONS.md:420`, deleted in `88698d2` (`A4_HZ`, `noteFreq`,
`waveCm`, `NOTE_NAMES`) with no mention in the commit or in V7. **Today: nothing.** Work: ~40
lines, but it needs a home — the Divisions grid has no free cell, and layout is yours (P5).
**Lean: revive.** Real studio tool, cheap, and the log lied about it.

### 1.2 No audio-input selection for the visualizers ⭐ verified
The legacy PI enumerated inputs, applied `deviceId: {exact: …}`, and had a "Restart audio"
button. **Yours.** Today: `getUserMedia` with no constraint; `enumerateDevices` nowhere. Work:
a batch — enumerate, a key or dial to cycle, pass the constraint, restart capture.
**Lean: revive first.** This is the actual blocker on the whole visualizers module, and no
ruling touches it.

### 1.3 Nothing persists across a restart
rekordbox's six encoder accumulators (legacy: 800 ms debounce, restored at boot), MIDI
Control's root/scale/channel/bank, the visualizer slot assignments. **All three were legacy
behaviour you had.** D17 was closed *by deletion* in V60; `midictl.js:140` says persistence is
"deliberately not implemented" pending an orchestrator change; nothing anywhere for viz.
Work: a batch for the store, then a line per module. **Lean: revive — one build, five
payoffs.**

### 1.4 The rekordbox system-wake MIDI reopen ⭐ verified
Legacy hooked `onSystemDidWakeUp` and reopened the port. `rekordbox.js:576` claims the service
owns it now; **it does not exist**, and the service cannot receive that Stream Deck event.
Today: recovery is reactive, so the first message after waking is dropped — in a set, a hot
cue that does nothing. Work: a line or two in `plugin.js`. **Lean: revive — cheapest
real-risk fix on the list.**

### 1.5 Six macOS window placements, built and unreachable
`MENU_TILES` implements 14 layouts; nine have keys. Unbound: the four quarters, `restore`,
`center` — AppleScript *and* Windows fallback both written. V38 ruled only the nine; no ruling
mentions the Quarters group. Row 1 of the Root Hub has four free cells on macOS. Work: four
icons (the `pane()` helper makes each one line) plus four slot rows. **Lean: revive the four
quarters.**

### 1.6 Five VST controllers with no insert key
1,452 lines, each verified against your own Configure screenshots, each shipped as its own
release (1.5.0.0 → 1.5.8.0). Only ValhallaRoom was ever flagged (*"one line away if he wants
it too — flagged, not assumed"*). They still work if you focus the device **in Live** — only
the shortcut is missing. Capacity: EQ 5/8, Dynamics 4/8, Synths 2/8, Analyzer 6/8 — **all five
fit page 1 with one spare.** Work: five lines. **Lean: revive — you paid for these five times
over.**

### 1.7 Track selection has no control; Sends A/B never ruled
`PLAN_ABLETON_HUB.md` §3 proposed Volume · Pan · Send A · Send B · track-select · readout. You
ruled the layout (V61) and the toggles (V62b). **Sends and track select were never addressed
either way.** Today `Bridge.cmd.selectTrack` and `cmd_select_track` exist end to end with zero
callers, and **dial 4 is blank** — so the mixer only ever addresses whatever the mouse
selected. Work: a line for track select; sends need two additive verbs + a Live restart.
**Lean: revive track select now — the verb is already deployed.**

### 1.8 MIDI Control's MIDI channel cannot be changed
`cycleChannel(dir)` is defined at `midictl.js:342` with **zero callers**, and `:359` claims the
channel block "is reachable on the hub board" — it is not. Root and Scale are; channel was left
behind when V13 scrapped State 3. Work: a line. **Lean: revive.**

### 1.9 The Meters "RME LED segments" style
The headline feature of visualizers 1.3.0.0. `style` is written at `viz.js:118` and **never
read**; ~58 lines of `segColumn`/`segRows`/`segDefs` reference only each other. **Worth
knowing: V60 deliberately preserved this palette believing the variant was live — it was
not.** Work: a batch. **Lean: revive — a whole released feature sitting one call away.**

### 1.10 The BPM sweep, both gestures
Dial-hold and touch-hold both ran an accelerating ramp (110 → 15 ms) to 300 BPM; the legacy
README called the touch sweep the point of the control. Today `console.js:246` declares
rotate/press/touch only, and its `touch: function (x)` **drops the `hold` argument** the
framework passes. Work: a line or two. **Lean: revive.**

### 1.11 The visualizer readout can never be cleared and never expires
`markerHold` (2–30 s per view) is in `DEFAULTS` and **read nowhere**; long touch resets the
Analyzer but not `cfg.markerX`, so once you tap, the marker is permanent. Work: two lines.
**Lean: revive.**

### 1.12 The spectrum dial lost its averaging-time knob
Legacy rotate = averaging time ±25 ms (the SPAN smoothing control). Today the dial rotates
`rangeLo` instead, so `avgTime` has **no control at all**. Silently swapped, no ruling. Work: a
line. **Lean: revive, or press-to-swap.**

### 1.13 The `OVERRIDES` escape hatch has no loader — for all thirteen controllers
Every predefined controller declares `OVERRIDES = {}`, documented as the way to pin a
parameter when Live's names don't match. **There is no UI, no config file and no loader** — so
the documented fix requires editing source. This is the *general form* of the Pro-Q 3 problem.
Work: a batch (read `~/.studioos/overrides.json` at connect). **Lean: revive — it de-risks all
thirteen at once.**

### 1.14 The rest, compact
| Item | Today | Work | Lean |
|---|---|---|---|
| **Numpad `÷`** | 16 cells for 17 legacy keys. `Clear` was ruled away by V5; **`divide` was not**, and is still mapped service-side with no caller | a line + your cell choice | revive if a cell exists |
| **Master volume + mute have no home** | `os.volume`/`os.mute` live in the service *and* `ipc.js`; no dial since V33 took all five zones | a line once a zone exists | your call |
| **No "Setup / Ports" screen** | `midi.ports` builds a union *"for a simple UI list"*; `hello`, `home.status`, `os.rescan` are read-verbs with no reader. Together: one screen | a batch | revive — it is where D16 and the input picker belong |
| **No arbitrary launcher / hotkey key** | `root.js:268-272` dispatches `slot.run`/`app`/`hotkey`; **no row uses any of them**. ARCHITECTURE promised "app launchers" | a line per key | revive — you name the apps |
| **Device mode's six touch zones are inert** | `mixDial` returns no `touch` | a line each | low |
| **`midi.panic` has no key** | implemented; the service auto-panics only on last disconnect | a line | revive — a stuck note in a set is worth one key |
| **rekordbox per-dial sensitivity** | `sens {3,2,1}` consumed by `rotate`, **no writer** | needs §1.3 | fold in |
| **No `validate.py`, no `pack.sh`, no CI** | every legacy plugin but the console had validation; two had packaging. Studio OS has neither | a batch | revive `validate.py` |
| **A viz view on a key** | the legacy action declared `Keypad`; `KEY_PTS = 48` exists, never used at key size | a batch | your call |
| **The spectrum wall** | 18 blank keys + four constants. **My plan, not yours** | a project | scrap unless wanted |

**One migration warning, not a gap:** MIDI Control's port was renamed from `Stream Deck MIDI
Control` to the shared `Adi Studio OS MIDI`. **Any existing Ableton MIDI-map for the old name
must be redone.** The rationale lives only in a code comment — never in a ruling — so you may
never have been told.

---

# 2. Dropped requests — things you asked for that got lost

| Your words | Where | What buried it | Today |
|---|---|---|---|
| **L2 — "both decks, 4 hot cues each"** | `:368`, restated `:1385` | Nothing — ruled, then twice deferred | Never built; docking prints `No room` |
| **L6 — "every module and controller ships TWO hand-crafted layouts"** | `:447` | The responsive pivot honoured the 14 controllers, then the module ports moved on | Missing for rekordbox, midictl, viz |
| **D4 — "ship all five, zero deferrals"** | `:138` | The porting run hit a spend limit; 4 of 6 agents died (`961bab8`) | Visualizers: 4/9 views, no config surface, no input picker, **never seen a signal** |
| **V37's two inferences** — *"TWO INFERENCES HERE ARE MINE, not Adi's words… Consequence worth his ruling"* | `:1989` | Never ruled — and it silently voided **L11's whole rationale** (*"one muscle memory rather than two"*) | EQ8 compact `[1,2,3]` vs Pro-Q 3 `[1,2,3,6]` — **dial 4 is band 3 on one, band 6 on the other**, and `ProQ3Controller.js:65` still cites L11. Also cost: EQ8's **Scale** global has no control, and `_buildGraph` is complete and unreachable |
| **Pro-Q 3's six Configured bands** — you built the template and supplied the names | **`CONTINUE.md:180` only — not in DECISIONS at all** | Never logged | Your half is done and the log doesn't know it |
| **"What happened to track volume and pan?"** | `:2573` | The investigation correctly identified the displaced **Root Hub master-volume dial** — then built Ableton *track* volume/pan instead | `os.volume`/`os.mute` still have no control. **The thing you asked about was never given a home** |
| **D12 — room lighting** (you approved the seam) | `:198` | V33 and V57 consumed every free zone | Three working drivers ship; `home.js:74` still logs *"write … to enable dial 4."* **There is no dial 4.** The ruling's "filling in config, not adding a feature" is void |
| **The acoustic tool** | `:420` promised it survived | `88698d2`, silently | Gone |
| **V58 — "ValhallaRoom is one line away if he wants it too"** | `:3047` | Never answered | No key |
| **L21 — Saturate's OUT MODE** *"flagged, not blocking"*, needed one look from you | `:970` | Nobody reported back — **and Saturate has no insert key**, so you may never have reached it | Open, possibly unreachable |
| **D16 — the rekordbox port name** | `:258` — *"global settings + a PI field"* | The field was built; the read-back was not | **A decoy field, and the Windows blocker.** Same for `studioPort`; `abletonPort` is worse — V60 deleted the `setUrl` export that was its one-line fix |

---

# 3. Started but never finished — scaffolding with no UI

Ranked by closeness to done.

| # | Exists | Missing | Work |
|---|---|---|---|
| 1 | PI writes four fields | `plugin.js` reads one and a half | **3 lines** |
| 2 | `divide`/`clear` tokens on all three platforms | no cell | a line each |
| 3 | `cycleChannel()` | no key | **a line** |
| 4 | 6 window layouts, both platforms | no tiles/icons | a line + icon each |
| 5 | 5 registered controllers | no catalogue rows | **5 lines** |
| 6 | `os.volume`/`mute`/`zoom`/`missionControl` end to end | no dial | a line each |
| 7 | `home.js` — three drivers + config seam + verb + facade | no dial (D12) | a line + a config file |
| 8 | `selectTrack`/`selectDevice`, frontend **and** live script | no key or dial | **a line** |
| 9 | `midi.ports` (built "for a UI list"), `hello`, `home.status`, `os.rescan` | no screen | a batch |
| 10 | 58-line segmented-LED cluster | never called; `style` never read | a batch |
| 11 | `_buildGraph` — EQ8's response curve, kept deliberately | nothing calls it | a batch, blocked on V37 |
| 12 | 13 × `OVERRIDES = {}` | no loader | a batch |
| 13 | `Surface.zoneOf` — the strip-x → zone inverse | zero callers; the one piece a strip gesture needs | leave |
| 14 | `WALL_*` + 18 blank keys | the 288-column DSP | a project |
| 15 | `ACTIONS.cubase` probe + `col: null` tile | the whole screen | a project |

**Answering the "hidden switch" question directly: there are none.** No feature flags, no
`enabled: false`, no `if (false)`, no commented-out registrations, no TODO/FIXME anywhere in
the plugin or service. Everything above is unreachable because **a binding was never written**
— not because something is switched off.

**The touch strip, specifically.** Nothing large was planned and dropped. All four drawing
paths are live; tap with x *and* y is live (L10 restored y); long-touch is live but **only the
Ableton controllers and viz read it** — root, console and midictl drop the argument at the
signature, which is exactly how the BPM touch sweep was lost. **No swipe/drag/pinch
scaffolding exists** — the SDK reports discrete taps only, so there is nothing half-built.

**Genuinely spare dial surface: one.** Device mode's dial 4. Plus zone 6 on the Root Hub and in
OS mode (left for the clock), and the six you deliberately left empty on arrival at Level 1.

---

# 4. The original vision vs the live surface

| Original plan (`ARCHITECTURE.md`) | Today | Verdict |
|---|---|---|
| `audio/engine.js` — "Visualizers live here" | folded into `viz.js` | doc drift |
| Level 0 dials: **1 Master Vol (push=mute)** · 2 Zoom · 3 App Switcher · **4 Room Lighting** · 5-6 blank | Scroll Y · Scroll X · Zoom · Apps · Tabs · clock | Zoom and Apps survived. **Master volume+mute and room lighting lost their home and never got another** |
| **Key 2 → Cubase Hub** | no screen; tile `col: null`; the probe still runs every `os.actions` | **completely absent** — and no ruling says it is off the roadmap |
| "rest → OS shortcuts, **app launchers**, smart-home" | two app tiles; `slot.run`/`app`/`hotkey` dispatched and **used by no row** | half-built |
| Button 35+36 held = carousel | plain keys; carousel on dial 6 | **decided** (V2/V3) |
| Service file map (`midi/out.js`, `os/keys.js`, …) | none exist; flat service | doc drift |
| **D4 — "ship all five, zero deferrals"** | four ship well; the fifth is 4/9 with no config surface, no input picker, and has never seen audio | **the one broken promise of the founding batch** |
| D13 — "Sub-menus (**EQ8 presets**, **track/device navigation**, **transport**) become Level 2" | transport landed at Level 1; presets ruled gone; device nav landed as V53; **track nav never landed** | two of three |

---

# 5. Legacy parity — what the five old plugins did that Studio OS does not

**You have never seen this section.** Only losses are listed.

### 5a. Visualizers & Meters 1.3.0.0 — the heaviest losses
5 of 9 views missing (**stated as status in DECISIONS, never actually ruled**). No input-device
selector or Restart-audio. **~35 per-view PI controls gone as UI while the engine still reads
every one** — only `rangeLo`/`timeMs`/`windowMs` and Reset are reachable. The RME LED style
never wired. `markerHold` auto-hide and long-touch clear both non-functional. Rotate =
averaging time silently became rotate = dB floor. Peak-and-RMS reduced to RMS. A view on a
**key** never ported. No per-instance persistence. No live browser demo. No `validate.py`, no
packaging.
*Decided, not lost:* corr/bal DSP (V60, your words), AudioWorklet → ScriptProcessor.

### 5b. Console 1.0.2.0
**The acoustic tool** (forgotten, and `:420` claims the opposite). **The BPM dial-hold and
touch-hold sweeps** (forgotten). **Numpad `÷`** (forgotten). Three *independent* range windows
became one shared `state.start`.
*Decided:* `Clear`→`✱` (V5), the 24-cell grid (V11/V15), launcher/`switchToProfile` (D1), the
Calculator (V59), and the exact-2/3 triplet change — **note your delay times now differ
slightly from the old plugin's**.

### 5c. rekordbox 1.0.1.0 — near-total parity
The MIDI matrix is transcribed number for number and a test diffs every constant. All hot
cues, the shift layer, transport, all four held nudges, browse with its 400/140 ms repeat, all
three dials per deck, load-on-volume-push, beat jump, and the Windows loopMIDI
attach-and-retry are **present** — the retry is better than legacy.
Losses: **the wake reopen** (forgotten), the port rename (D16, open), persisted levels (D17,
deleted), per-dial sensitivity (no writer), the compact layout (L2).

### 5d. midi_control 1.1.0.0
Drum pads (with true Note Off and `allPadsOff()` — better than legacy), the 8-zone scale
keyboard with all 14 interval sets verbatim, root, scale, six CC dials, three banks with
per-bank memory: all **present**.
Losses: **MIDI channel 1-16** (forgotten), **config persistence** (forgotten — D17 is scoped
"(module: Rekordbox)" only), the port rename.
*Decided:* the C++ helper eliminated (F3 — closed a year-old TODO), coordinate-driven
placement (F1/D1).

### 5e. ableton_vst_controller 1.5.9.0
All 14 controllers exist and are registered; `registry.js` byte-identical; every role table
diffed identical except EQ8.
Losses: **EQ8's Scale global** and **the summed response graph** (both consequences of the
unruled V37), **band 8 unreachable** (already logged; frozen), **five controllers with no
insert key**, and `abletonPort` as a decoy.
*Inherited, not a regression:* Pro-Q 3's name resolution — frozen at your instruction.

---

# 6. Deferred and open — who owes the next move

**Awaiting your ruling:** V37's two inferences (**the keystone** — ruling it unblocks EQ8's
Scale, the response graph, and the dial-4 divergence at once) · Pro-Q 3 go-ahead · **compact
layouts for rekordbox / MIDI Control / Meters — you asked to be prompted, and this report is
that prompt** · Level 1's 11 empty cells · Device mode's spare dial 4 · Sends A/B · the five
insert keys · D14's write-back · D16's home · D12's dial · L21's Saturate observation ·
whether Cubase is on the roadmap.

**Awaiting my implementation:** the namespaced settings store (unblocks D16, D17, midictl and
viz persistence, and the `OVERRIDES` loader) · the wake-from-sleep reopen · `markerHold`
expiry and marker clear · the BPM sweep · wiring the RME style · `validate.py` and packaging ·
the Windows pass (blocked on your machine).

---

# 7. Correctly dropped — do not revive

**Your explicit words:** the Calculator · the Presets button and folder (*"I never requested
it"*) · the coloured group frames · plugin logos on the grid · the invented "Device" readout ·
the corr/bal DSP · the macOS Start/Run/Shell mappings (you rejected Launchpad/Spotlight/
Terminal on hardware).

**Ruled by design:** State 3 and its breadcrumb (V13 — `Nav.path()` is its residue) · the
browser arrows and the LIVE key (V29) · the V44 VST tree (V46) · per-key Property Inspectors
and `switchToProfile` (D1/F1 — this kills the launcher actions, per-instance settings, and
multi-hardware support) · the 24-cell delay grid · Button 35/36's engine roles and the D9/D9a
apparatus · the C++ helper (F3) · Mission Control on dial 5 (V38) · **Blackhole and Omnipressor
leaving FULL's dials 5-6 unmapped (L17/L20 — this looks unfinished and is deliberate)** · the
Meters `id`/title mismatch.

---

# 8. What could not be determined

1. **Whether the rekordbox surface has ever been MIDI-LEARNed in rekordbox.**
   `rekordbox.js:8` asserts the notes are *"already MIDI-LEARNed inside"*; the master
   `HANDOFF.md:177` still lists the hardware pass as **your** open task, and it needs a Core
   plan or Hardware Unlock. **If it hasn't been done, the entire 36-key DJ surface has never
   controlled anything.**
2. **Whether the V39 Pro-Q 3 diagnostic has ever been run.** One press prints the device name,
   parameter count and first ten names — which decides whether §1.13's loader is required or
   optional.
3. **Whether V25's promised pump measurement was taken.** *"Before any clock returns, the cost
   of the existing 15 fps pump against a live bridge has to be measured."* No measurement
   appears anywhere.
4. **Whether the Windows paths work at all.** No Windows machine.
5. **Whether the visualizers have ever displayed a real signal on your machine.** Every mention
   is prospective. If BlackHole has never been the system default input, the answer is no —
   which would mean the module has been shipped and audited but never used.
6. **What P1 and P2 are.** P3/P4/P5 are cited by number; P1 and P2 are defined nowhere.
