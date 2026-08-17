# Continue Studio OS — session handoff prompt

Paste the block below into a new session.

---

We are continuing **Studio OS**, a single "Master Plugin" for my Elgato **Stream Deck + XL** that merges five of my older standalone Stream Deck plugins into one navigable surface.

**Repo:** `~/Documents/GitHub/Adi/tools/elgato_stream_deck_plugins/adi_studio_os`
**Read first:** `docs/DECISIONS.md` — every architectural crossroad, the options I was offered, and my ruling. It is the source of truth and it is append-only. Batches 11 and 12 at the bottom are the most recent and supersede a lot above them. Then `docs/ARCHITECTURE.md`.

**Never `git push`.** Commit locally only, scoped to this plugin folder, and commit finished work in the same turn rather than making me ask. Trailer: `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.

## Hardware (verified, not assumed)

Stream Deck **+ XL**: 36 keys (9 cols × 4 rows), 6 dials, 1200×100 touch strip = 6 zones of 200×100. `button = row*9 + col + 1`. Button 1 = (0,0), Button 35 = (7,3), Button 36 = (8,3).

## Architecture in one paragraph

CEF frontend (`app.html`) owns the UI, navigation and Web Audio. A Node backend service (`service/`, on `ws://127.0.0.1:9011`) owns native MIDI and OS routing — the frontend can do neither. Ableton talks over `ws://127.0.0.1:9006` to my existing **AdiVST Python Remote Script, which is verified and must not be modified**. One universal `cell` action sits on all 36 keys and one `dial` action on all 6 dials; everything is driven centrally.

## Global rules currently in force

- **Button 1** — long press = Back / level up; short press = contextual select. Released to the module in State 4. It is the **only** reserved key on the board.
- **Buttons 35 and 36** — no engine role, plain keys (V2). Button 36's carousel and the whole D9/D9a hanging-note apparatus are gone; (8,3) in Rekordbox is a **Beat Jump** now, not a held nudge.
- **NAV trigger (V3/V13)** — a **long press on the right-most dial (dial 6)** cycles `0 → 1 → 2 → NAV OFF → 0`. **State 3 (Context) was scrapped in Batch 13**; NAV OFF is now index 3. It works in NAV OFF too, so NAV can always be recalled. Dial 6's *short* press therefore resolves on release; dials 1–5 stay immediate.
- **Merged keys (V6)** — any binding may declare `hold`; short press runs `tap` on release, long press runs `hold`. Binding-level opt-in, not a new anchor.
- **Dial borrowing is PER STATE (V4/V14)** — States 0 and 1 touch **no** dials, which IS the pass-through: the module keeps six and stays FULL. **State 2 takes TWO (physical 5 and 6) and is the Compact suite's only consumer** — it is what puts the Ableton controllers into `build(4)`. There is no 5-zone case any more.
- **Responsive layouts (L1)** — a docked nav window does NOT overlay the module; it takes columns and the module re-lays-out via declared breakpoints. Screens declare `layouts: [{cols, keys(col,row)}]` with **region-local** coordinates. Engine: `js/core/layout.js`.
- **Windows borrow the RIGHTMOST dials (L3b)** — `borrowDials: N` takes the last N. Dials 1–4 always stay with the module.
- **Every window is the same 4×4 dock** (16 keys, cols 5–8). States: 0 Numpad · 1 Calculator · 2 Time Divisions · 3 NAV OFF (docks nothing, module reclaims all 36 keys).
- **Never compare the state INDEX to a literal.** Ask `States.isFullScreen()`. A hardcoded `4` in `input.js` silently un-reserved Button 1 everywhere the moment State 3 was removed.

## Three protocols I insist on

**1. Stop at every conflict.** Whenever legacy behaviour clashes with the new design, or a decision is genuinely mine to make, STOP that component, explain the conflict, propose creative options with trade-offs, and ask. Do not guess or write a workaround. Log the ruling in `docs/DECISIONS.md` before implementing. This has caught real problems every single time — keep doing it.

**1b. Conflicts are plain output, never a menu (P4).** State the findings and every conflict as text, then STOP and wait. Do not present an interactive multiple-choice question — Adi rules on the whole set at once and replies with one unified instruction.

**2. Dual-layout contract + "Discovery First" (L6).** Every module and controller ships TWO hand-crafted layouts: **Full** (whole board) and **Compact**. Not a reflow — a bespoke design.

Before asking me to design any controller's Compact layout, output a **Discovery briefing**:
1. **Core purpose** — what the controller handles.
2. **Full layout, dials 1–6** — exactly what each dial rotates and what press does.
3. **Full layout, keys** — does it use any? (So far: none do. All 36 keys belong to the Ableton hub shell.)
4. **Modes / tabs / pagination.**
5. **Visuals** — what I actually see on the strip.

I do not memorise legacy mappings. You have perfect recall of the codebase — act as my memory base. A rendered picture of the strip alongside the briefing is very welcome.

**3. Chain the briefings.** When you finish a controller and commit it, do NOT stop and wait for me to ask for the next one. Put the **next controller's Discovery briefing at the very bottom of the same commit/success message**. A turn spent asking "shall I continue?" costs context for no information. This chains the *briefing* only — still stop for my ruling before writing code.

## Where things stand

**854 tests green** (843 JS + 11 Python) (`node scripts/test_{core,service,console,modules,viz,ableton}.mjs` **and `python3 scripts/test_bridge.py`**) and **Batch 13 is deployed and running on the hardware**. Last commit: "dedupe setFeedback, port the LED clock, clean the Ableton hub, insert Pro-Q 3".

| Piece | State |
|---|---|
| Core engine, Node service, installers | done |
| Root Hub | done, 9-col + 5-col |
| Rekordbox, MIDI Control | done — **Full layout only**; rekordbox is Omnis-Duo skinned (V16/V20/V21) |
| Visualizers | working, **4 of 9 views** (spectrum, scope, waveform, meters) |
| Ableton hub shell | done, 9-col + 5-col |
| **All 14 Ableton controllers** | ✅ native SVG, both layouts — L4 COMPLETE |
| State 0 Numpad | ✅ V3 — `C` is now `✱` (sends real KP_Multiply) |
| State 1 Calculator | ✅ V3 — display row, merged keys, grouped numbers |
| State 2 Time Divisions | ✅ V3 — 3×3 fractions, ▲/▼ range, value in one place |
| State 2 Time Divisions | ✅ V15 — dial 5 readout/grid/format, value key TYPES via `os.type` |
| Rekordbox Omnis-Duo skin | ✅ V16 — A–H pads, indigo chassis, circular CUE / ▶‖ |
| Ableton smart launcher | ✅ V17 — Root Hub tile starts the newest installed Live |
| **The 14 Compact strips** | ✅ reachable at last — State 2 is the consumer (V14) |

### The V3 NAV refactor (DECISIONS Batch 11, rulings V1–V9)

Carousel gained a NAV-OFF position; Button 36 became a plain key; the NAV trigger moved to dial 6's long press; dial borrowing became per-state; States 0–2 were rebuilt; the key aesthetic was replaced.

### The first hardware pass (DECISIONS Batch 12, rulings V10–V12)

- **V10** — an active key has **no outline at all**. Tinted face + inner glow only. Two earlier attempts still drew a rim and it kept reading as "a green border".
- **V11** — Time Divisions: **columns are the variants, rows are the divisions**, the nine cells carry their fraction and nothing else, row 0's three labels are **static headers**, ▲/▼ shift the grid and **clamp** (greying out at the ends), and the computed value appears in exactly one place.
- **V12** — Calculator: operands coerced to Number in `applyOp`; numbers grouped with thousands separators and split **on** the separator (`12000` → `12,` | `000`); dim `0. 000 000 000` placeholder spanning all four keys at rest; operator glyph in the top ~28% of each display key.

### Batch 13 (rulings V13–V17) — see DECISIONS

State 3 scrapped, State 2 became the Compact consumer and gained a readout + PASTE
key, rekordbox got the Omnis-Duo skin, the Ableton tile became a smart launcher,
and the AdiVST remote script was finally installed.

### Batch 14 (rulings V18–V21) — the first Omnis-Duo hardware pass

The surface could be frozen permanently by one throwing cell (V18 — this was the
real "dial 6 does nothing"); the calculator lost its duplicate `⌫` and got the
float cast on the operator line (V19); rekordbox got its held nudge back and the
beat-jump/nudge rows swapped (V20); the circular caps moved onto bezel black (V21).

**P5 — LAYOUT IS ADI'S TO CHANGE.** Never alter a key layout or widen a module's
footprint without presenting options first. The Calculator stays inside its 16
keys; the screen strip and dials are reserved for the VSTs.

### Batch 15 (V22–V23)

Real app icons via `js/core/art.js` (a NAME registry — never put base64 on a
binding, it lands in the per-frame hash); the calculator now prints its pending
operation, which is the feedback whose absence made `+` feel broken.

**Awaiting hardware feedback on V18–V23.** Ask what still looks wrong before starting anything new.

### Open, waiting on Adi's ruling (raised, not yet designed)

1. **MIDI Control belongs inside Ableton**, not on the Root Hub — needs a home in
   the Ableton hub's already-full 9-col row 0.
2. **macOS window management on the Root Hub** — note D14: mapping Windows
   concepts onto macOS guesses was explicitly REJECTED once already. macOS 26 has
   real tiling shortcuts, so this can be actual key sends rather than a guess.
3. **A clock on the touch strip, right-hand side** — conflicts head-on with "the
   screen and dials are strictly for the VSTs" (P5) and with the 6-zone VST
   layouts. Needs a ruling on which zone and what yields to what.
4. **Whether `+` / `−` should stop being long presses** — the cure needs a key to
   give up, so it is a footprint decision (P5).

### Immediate next tasks, in order

1. **Confirm the Ableton bridge against real Live.** The remote script is now installed at `~/Music/Ableton/User Library/Remote Scripts/AdiVST` (byte-identical, unmodified) but **Adi still has to select AdiVST as a Control Surface** in Live → Settings → Link/Tempo/MIDI. Until then port 9006 has no server and every controller resolves to Generic. Nothing Ableton-side has ever seen real Live.
2. **Compact layouts for Rekordbox** (ruled L2: both decks, 4 hot cues each — now cues **A–D**), **MIDI Control**, **Visualizers** — none has one, so docking a window over them still hits the engine's "No room" path. **I asked to be prompted before these are started.**

## How the Ableton controllers work

- Every controller implements `build(zones)` returning an `SOS.Svg.bag()`; `setZones(n)` tells it how many dials it has (6 = Full, 4 = Compact). Primitives in `js/ableton/svg.js`.
- `SOS.SvgCtx` (the old Canvas-2D shim in `js/modules/ableton.js`) has **no controllers left to serve** — L4 is complete. `test_ableton.mjs [2]` now asserts the inverse of what it used to: no `*Controller.js` may still match 1.5.9.0. `registry.js` is deliberately still identical — it is the device→strategy table, not ink.
- **Keep the parameter maps EXACTLY.** Role tables, name regexes, `OVERRIDES`, registry match patterns and response models were verified against my real Ableton "Configure" screenshots. Only ever rewrite the *drawing*.

## Workflow

- **Verify headlessly**, never by asking me to check: `node scripts/test_*.mjs` plus `python3 scripts/test_bridge.py`. All SEVEN suites, every time.
- **Look at what you built before deploying.** Render the REAL modules through the REAL `render.js` into an SVG sheet and screenshot it in the Browser pane — a mock proves nothing. The pane's screenshots go black when scrolled, so keep each sheet inside one viewport.
- **Deploy to hardware** — this exact sequence, because the app caches plugin files while running and a plain restart picks up nothing:
  1. Quit the app and wait for it to ACTUALLY die. The binary is `MacOS/`**`Stream Deck`**, not "Elgato Stream Deck" — `pgrep -x "Elgato Stream Deck"` and `pgrep -f ".../MacOS/Elgato"` NEVER match and will report success instantly while the app runs on. Use `pgrep -lf "Elgato Stream Deck.app/Contents/MacOS/Stream Deck"`, and `pkill -f "Elgato Stream Deck.app"` to clear QtWebEngine helpers too.
  2. `rsync -a --delete <repo>/com.adiariel.studioos.sdPlugin/ ~/Library/Application\ Support/com.elgato.StreamDeck/Plugins/com.adiariel.studioos.sdPlugin/`
  3. `diff -r` the two folders and grep the DEPLOYED files for markers of the change — never trust the copy.
  4. Archive the old log, `open -a "Elgato Stream Deck"`, then confirm `surface COMPLETE — 36/36 keys, 6/6 dials` with no errors. **The log number ROTATES per launch** — find the current one with `ls -t ~/Library/Logs/ElgatoStreamDeck/com.adiariel.studioos[0-9].log | head -1`. A fresh log is the only trustworthy proof the app really restarted.
  5. Sample CPU afterwards (`ps -o %cpu= -p <pid>`). Idle should be ~0%.
  - Sync the **plugin folder only**. The full installer also regenerates my profile, which I do not want touched.

## Field notes that cost real debugging time

- **Glyphs outside the proven set render as tofu.** `⌷` (U+2337) came out as an empty box on the device. Stay inside the glyph set already in shipped use; there is a test pinning this.
- **`keySpec()` in `states.js` must forward every new binding field.** It is a hand-written whitelist and a forgotten field paints a silently wrong key — this has now bitten twice (`size`/`subStrong`, then `segDim`).
- **Key SVG ids must be derived from content, not a counter.** `SD.image()` dedupes by data URI; a per-call id makes every key look different every frame and turns a static surface into 36 writes at 15 fps.
- **`setFeedback` has no dedupe of its own — use `SD.feedback()`.** `SD.image()` always deduped; setFeedback did not, so six dial zones were re-sent in full on every repaint. Under the 15 fps Ableton pump that was ~90 multi-KB messages a second and it is what overloaded the machine. Anything new that paints a zone goes through `SD.feedback()`.
- **`scripts/preview.mjs` must mirror `States.paintDial` exactly** — it has now silently drifted TWICE, each time leaving a sheet that looked right and was not evidence.
- **Tests that call a controller's `onTouch` directly cannot see wiring bugs.** The dropped touch-Y axis survived a whole port that way. Drive gestures through the real socket (`test_core.mjs [11]`, `[12]`).

## Known open items

- **Nothing Ableton-side has been confirmed against real Live yet** — the bridge has only ever seen synthetic state. The 14 Compact strips are the least-proven code in the project.
- **The 5 un-ported Visualizer views** — bands, rme, gonio, corr, bal. Each paints a labelled "not ported" tile rather than a blank key.
- **Windows pass** — installers written but never run there. Needs my machine: the active-profile pointer (the macOS route is a preferences plist key with no known Windows equivalent) and loopMIDI ports named `Adi RekordBox Controller` / `Adi Studio OS MIDI`.
- **BlackHole 2ch is installed but needs a reboot** before the visualizers see audio.
- Service port is **9011**, not 9010 — Logitech G HUB permanently owns 9010 on this machine.
- Pending, logged in DECISIONS: **D16** (rekordbox MIDI port name has no settings home — needed for Windows), **D17** (encoder level persistence unwired), **D14** flagged as derived-not-ruled and open to veto.
