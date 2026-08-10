# Continue Studio OS — session handoff prompt

Paste the block below into a new session.

---

We are continuing **Studio OS**, a single "Master Plugin" for my Elgato **Stream Deck + XL** that merges five of my older standalone Stream Deck plugins into one navigable surface.

**Repo:** `~/Documents/GitHub/Adi/tools/elgato_stream_deck_plugins/adi_studio_os`
**Read first:** `docs/DECISIONS.md` — every architectural crossroad, the options I was offered, and my ruling. It is the source of truth and it is append-only. Then `docs/ARCHITECTURE.md`.

**Never `git push`.** Commit locally only, scoped to this plugin folder, and commit finished work in the same turn rather than making me ask. Trailer: `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.

## Hardware (verified, not assumed)

Stream Deck **+ XL**: 36 keys (9 cols × 4 rows), 6 dials, 1200×100 touch strip = 6 zones of 200×100. `button = row*9 + col + 1`. Button 1 = (0,0), Button 35 = (7,3), Button 36 = (8,3).

## Architecture in one paragraph

CEF frontend (`app.html`) owns the UI, navigation and Web Audio. A Node backend service (`service/`, on `ws://127.0.0.1:9011`) owns native MIDI and OS routing — the frontend can do neither. Ableton talks over `ws://127.0.0.1:9006` to my existing **AdiVST Python Remote Script, which is verified and must not be modified**. One universal `cell` action sits on all 36 keys and one `dial` action on all 6 dials; everything is driven centrally.

## Global rules currently in force

- **Button 1** — long press = Back / level up; short press = contextual select. Released to the module in State 4.
- **Button 36** — long press (500 ms) = State Carousel and the only escape from State 4. Short press delivery depends on the binding kind (`momentary` fires on press, `tap` on release).
- **Button 35** — no engine role. Plain key.
- **Responsive layouts (L1)** — a docked nav window does NOT overlay the module; it takes columns and the module re-lays-out via declared breakpoints. Screens declare `layouts: [{cols, keys(col,row)}]` with **region-local** coordinates. Engine: `js/core/layout.js`.
- **Dial borrowing (L3b)** — a nav window declares `borrowDials: N` and takes the **rightmost** N (so N=2 is physical dials **5 and 6**, under the dock). **Dials 1–4 always stay with the module.**
- **Every window is the same 4×4 dock** (16 keys). States: 0 Numpad · 1 Calculator · 2 Delay · 3 Context · 4 Full Screen (docks nothing).

## Two protocols I insist on

**1. Stop at every conflict.** Whenever legacy behaviour clashes with the new design, or a decision is genuinely mine to make, STOP that component, explain the conflict, propose creative options with trade-offs, and ask. Do not guess or write a workaround. Log the ruling in `docs/DECISIONS.md` before implementing.

**2. Dual-layout contract + "Discovery First" (L6).** Every module and controller ships TWO hand-crafted layouts: **Full** (whole board) and **Compact** (nav window docked, module has 5 cols and dials 1–4). Not a reflow — a bespoke design.

Before asking me to design any controller's Compact layout, output a **Discovery briefing**:
1. **Core purpose** — what the controller handles.
2. **Full layout, dials 1–6** — exactly what each dial rotates and what press does.
3. **Full layout, keys** — does it use any? (So far: none do. All 36 keys belong to the Ableton hub shell.)
4. **Modes / tabs / pagination.**
5. **Visuals** — what I actually see on the strip.

I do not memorise legacy mappings. You have perfect recall of the codebase — act as my memory base. A rendered picture of the strip alongside the briefing is very welcome.

**3. Chain the briefings.** When you finish a controller and commit it, do NOT stop and wait for me to ask for the next one. Put the **next controller's Discovery briefing at the very bottom of the same commit/success message**. A turn spent asking "shall I continue?" costs context for no information. This chains the *briefing* only — still stop for my ruling on the Compact layout before writing code.

## Where things stand

**Done and verified — 487 tests green** (`node scripts/test_{core,service,console,modules,viz,ableton}.mjs`):

| Piece | State |
|---|---|
| Core engine, Node service, installers | done |
| Console windows (Numpad / Calculator / Delay viewport) | done, responsive |
| Root Hub | done, 9-col + 5-col layouts |
| Rekordbox, MIDI Control | done — **Full layout only** |
| Visualizers | working, **4 of 9 views** (spectrum, scope, waveform, meters) |
| Ableton hub shell | done, 9-col + 5-col |
| **EQ8** | ✅ native SVG + compact (bands 1/2/3/6, no GLOB) |
| **Generic** | ✅ native SVG + compact (params 1–4, blind chop) |
| **Pulsar Massive** | ✅ native SVG + compact (4th DRIVE tab) |
| **ProQ3** | ✅ native SVG + compact (bands 1/2/3/6, press = Slope on cuts) |
| **Spectre** | ✅ native SVG + compact (4th GLOB tab, bands Lo/P1/P3/Hi) |
| **Indeq** | ✅ native SVG + compact (gains + Output, steppers dropped, stateless) |
| **ValhallaRoom** | ✅ native SVG + compact (4 pages × first 4 dials, MODE-only bar) |

**Two engine bugs were fixed on the way through ProQ3 — see DECISIONS L10:**

* **The touch Y coordinate was being dropped**, so no mode tab and no pill in any
  Ableton controller could ever be hit on the real device. The dial-descriptor
  contract is now `touch(x, y, hold)`. Guarded by `test_core.mjs [11]`, which
  drives a real `touchTap` in through the socket — calling a controller's
  `onTouch` directly, which is all the tests did before, cannot see it.
* **`Nav.setRoot` looped forever** on any second call: it drained the stack with
  `while (stack.length) pop(true)` and `pop` refuses to remove the last entry.
  Latent (install runs once at boot) but one line from a hang.

**Immediate next task: `ValhallaVintageVerbController`**. Its Discovery briefing
has already been given — pick up at the Compact-layout ruling.

**After that, in registry order:** Blackhole, HDelay, DbComp, Omnipressor, Saturate, SideMinder.

**Then:** Compact layouts for Rekordbox (ruled L2: both decks, 4 hot cues each), MIDI Control and Visualizers — none have one yet, so docking a window over them currently hits the engine's "No room" path.

## How the Ableton controllers work

- **Native ones** implement `build(zones)` returning an `SOS.Svg.bag()`; `setZones(n)` tells them how many dials they have (6 = Full, 4 = Compact). Primitives in `js/ableton/svg.js`.
- **Not-yet-rewritten ones** are **byte-identical copies** of the 1.5.9.0 originals and still draw through the `SOS.SvgCtx` Canvas shim in `js/modules/ableton.js`. `scripts/test_ableton.mjs` asserts that byte-identity on every run — when you rewrite one, add its filename to the `NATIVE` set **and** to the "is the native rewrite, not a copy" list in that test.
- **When rewriting: keep the parameter maps EXACTLY.** Role tables, name regexes, `OVERRIDES`, registry match patterns and response models were verified against my real Ableton "Configure" screenshots. Rewrite the *drawing* only.

## Workflow

- **Verify headlessly**, never by asking me to check: `node scripts/test_*.mjs`.
- **Look at what you built** before deploying: `node scripts/preview.mjs out.html` renders the real surface (optional `SECTIONS=dj,midi` filter). Serve it via the `studioos-preview` entry in `~/.claude/launch.json` and screenshot it. The Browser pane screenshots go black when scrolled, so keep sheets short.
- **Deploy to hardware:** quit the Stream Deck app, copy `com.adiariel.studioos.sdPlugin` into `~/Library/Application Support/com.elgato.StreamDeck/Plugins/`, relaunch, then confirm `surface COMPLETE — 36/36 keys, 6/6 dials` in `~/Library/Logs/ElgatoStreamDeck/com.adiariel.studioos0.log`.

## Known open items

- **Windows pass** — installers written but never run there. Two things need my machine: the active-profile pointer (the macOS route is a preferences plist key with no known Windows equivalent) and loopMIDI ports named `Adi RekordBox Controller` / `Adi Studio OS MIDI`.
- **Nothing Ableton-side has been confirmed against real Live yet** — the bridge has only ever seen synthetic state.
- **BlackHole 2ch is installed but needs a reboot** before the visualizers see audio.
- Service port is **9011**, not 9010 — Logitech G HUB permanently owns 9010 on this machine.
- Pending, logged in DECISIONS: **D16** (rekordbox MIDI port name has no settings home — needed for Windows), **D17** (encoder level persistence unwired), **D9a / D14** flagged as derived-not-ruled and open to veto.
