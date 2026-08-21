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

**1130 tests green** (1075 JS + 55 Python) (`node scripts/test_{core,service,console,modules,viz,ableton}.mjs` **and `python3 scripts/test_bridge.py`**) and **Batch 29 is deployed** (`service v2.5.0`, `surface COMPLETE - 36/36 keys, 6/6 dials`).

### FROZEN at Adi's instruction - do not write code for these

**EQ8's mapping, Pro-Q 3 data mapping, and the Calculator.**

### THE REMOTE SCRIPT KEEPS GAINING VERBS - LIVE MUST BE RESTARTED

Ableton loads Remote Scripts **at launch**. Additive verbs so far: `load_device`
(V30), `device_key` (V52), `track_volume_delta` / `track_pan_delta` / `get_mix`
(V50), `device_step` / `device_pos` (V53). "Must not be modified" means no editing
existing code paths; additive verbs are the V30 exception.

### Batch 29 (V55) - Adi's artwork on the bands, and the red traffic light

- **The category palette has its own names**: `catEq` violet, `catDyn` amber,
  `catSynth` teal, `catMeter` emerald. They used to borrow rekordbox's red and the
  calculator's amber, so a category could not be recoloured alone.
- **The bands wear Adi's four images, SLICED PER KEY.** This is the important
  constraint: every key is its own image with no shared canvas behind it, so
  embedding a band's whole picture in each of its eight keys would be ~95 MB of SVG
  on a pipe V27 showed is overwhelmed by ~90 multi-KB messages a second. So
  `scripts/slice_backgrounds.py` cuts each image into eight 144x144 tiles ONCE, and
  `js/core/backgrounds.js` is 101 KB for all 32. **Regenerate with that script if an
  image changes** - do not hand-edit the file.
- **Tiles are row-major and the tile index IS the cell's slot index**, so there is
  no second mapping to drift. The tile follows the SLOT, never the paged item, or the
  art would slide sideways when NEXT pages plugins through the bands. `(0,0)` is Back
  and is handed the EQ block's tile 0 explicitly, or the picture has a blank corner.
- **The button material is drawn ON TOP** via `faceOpacity` (0.38), plus a 0.27 dark
  scrim for label legibility. **The scrim was tuned by looking**: 0.38 crushed the
  violet and teal bands almost to black. The flat V54 tint survives underneath as the
  fallback.

### FIELD NOTE - `xlink:href` costs the SIZE OF THE IMAGE, not 40 bytes

V22's comment said the legacy attribute "costs 40 bytes". It repeats the whole data
URI, so a 5 KB tile makes a 10 KB key. Harmless with one app icon, obvious with 32
tiles. Both attributes are kept - a blank key on the device is worse - but budget
raster payloads at DOUBLE their size.

### FIELD NOTE - never string-match through hashId()

`hashId()` ends in a literal backslash-u-0001 join argument. Editing around it by
text match has mangled it three times. Edit that function BY LINE.

### The red traffic light's guard list

`NEVER_QUIT` in service/os.js refuses Stream Deck (killing it kills the plugin),
Finder / Dock / SystemUIServer / loginwindow / WindowServer, **and Ableton Live** -
deliberately, because an accidental long press that killed Live mid-session would
lose work in a way no other key can. **One line to remove if Adi disagrees.**

### Pro-Q 3 — ANSWERED by Adi (Batch 23), not yet implemented

**The Configure theory was correct.** Adi has built an Ableton template with **six
bands fully Configured and exposed**, and has supplied the parameter names. He asked
that the actual control be implemented **later** — do not write Pro-Q 3 code until he
says so. The diagnostic below is no longer the blocker; the remaining work is
matching `ProQ3Controller.ROLES` against the six exposed bands he provided.

The analysis that got us here is kept below because it applies to the other nine
controllers, which all resolve parameters by name the same way.

**Pro-Q 3 was functionally dead** for this reason: the controller binds
parameters BY NAME from `all_params` (`"band 1 frequency"`, `"band 1 shape"`, …),
it requests them itself in `onState`, and the Python side returns every name Live
reports. So the names Live sends do not match.

The likely cause: **Live only exposes a VST's parameters that have been
"Configured"** in Live's own Configure mode, and a freshly inserted Pro-Q 3 (which
is exactly what the V30 key creates) has no Configure mapping at all.

V39 added the diagnostic that settles it in one press — focus a Pro-Q 3 in Live,
then:

```
grep "exposes" $(ls -t ~/Library/Logs/ElgatoStreamDeck/com.adiariel.studioos[0-9].log | head -1)
```

It logs the device name, the parameter COUNT and the first ten NAMES. A count of
~1 confirms the Configure theory; real-but-different names mean the match patterns
in `ProQ3Controller.ROLES` need editing (`OVERRIDES` exists for pinning them).

### Also awaiting a ruling

1. **The two EQ8 inferences are mine, not Adi's words** (V37): dial 1 = Output,
   and pagination on dial 1's press. Consequence: **COMPACT dropped from four
   bands (B1/B2/B3/B6) to three**, because dial 1 is spent on Output there too.
2. **Whether `+` / `−` should stop being long presses** in the calculator — the
   cure needs a key to give up, so it is a footprint decision (P5).

### Immediate next tasks, in order

1. **Answer the Pro-Q 3 question above.** Nine other controllers resolve
   parameters by name the same way, so whatever is wrong here is wrong for all of
   them — it is the single highest-value unblock left.
2. **Compact layouts for Rekordbox** (ruled L2: both decks, 4 hot cues each — now
   cues **A–D**), **MIDI Control**, **Visualizers** — none has one, so docking a
   window over them still hits the engine's "No room" path. **I asked to be
   prompted before these are started.**

## How the Ableton controllers work

- Every controller implements `build(zones)` returning an `SOS.Svg.bag()`; `setZones(n)` tells it how many dials it has (6 = Full, 4 = Compact). Primitives in `js/ableton/svg.js`.
- `SOS.SvgCtx` (the old Canvas-2D shim in `js/modules/ableton.js`) has **no controllers left to serve** — L4 is complete. `test_ableton.mjs [2]` now asserts the inverse of what it used to: no `*Controller.js` may still match 1.5.9.0. `registry.js` is deliberately still identical — it is the device→strategy table, not ink.
- **Keep the parameter maps EXACTLY.** Role tables, name regexes, `OVERRIDES`, registry match patterns and response models were verified against my real Ableton "Configure" screenshots. Only ever rewrite the *drawing*.

## Workflow

- **Verify headlessly**, never by asking me to check: `node scripts/test_*.mjs` plus `python3 scripts/test_bridge.py`. All SEVEN suites, every time.
- **Look at what you built before deploying.** Render the REAL modules through the REAL `render.js` into an SVG sheet and screenshot it in the Browser pane — a mock proves nothing. The pane's screenshots go black when scrolled, so keep each sheet inside one viewport.
- **Deploy to hardware** — this exact sequence, because the app caches plugin files while running and a plain restart picks up nothing:
  0. **Just run `./scripts/deploy-mac.sh`.** It encodes all of this, and in particular it ALWAYS restarts the service — a deploy that only started it "if not running" left four Root Hub dials calling verbs a stale service had never heard of, silently, because those verbs are fire-and-forget.
  1. Quit the app and wait for it to ACTUALLY die. The binary is `MacOS/`**`Stream Deck`**, not "Elgato Stream Deck" — `pgrep -x "Elgato Stream Deck"` and `pgrep -f ".../MacOS/Elgato"` NEVER match and will report success instantly while the app runs on. Use `pgrep -lf "Elgato Stream Deck.app/Contents/MacOS/Stream Deck"`, and `pkill -f "Elgato Stream Deck.app"` to clear QtWebEngine helpers too.
  2. `rsync -a --delete <repo>/com.adiariel.studioos.sdPlugin/ ~/Library/Application\ Support/com.elgato.StreamDeck/Plugins/com.adiariel.studioos.sdPlugin/`
  3. `diff -r` the two folders and grep the DEPLOYED files for markers of the change — never trust the copy.
  4. Archive the old log, `open -a "Elgato Stream Deck"`, then confirm `surface COMPLETE — 36/36 keys, 6/6 dials` with no errors. **The log number ROTATES per launch** — find the current one with `ls -t ~/Library/Logs/ElgatoStreamDeck/com.adiariel.studioos[0-9].log | head -1`. A fresh log is the only trustworthy proof the app really restarted.
  5. Sample CPU afterwards (`ps -o %cpu= -p <pid>`). Idle should be ~0%.
  - Sync the **plugin folder only**. The full installer also regenerates my profile, which I do not want touched.

## Field notes that cost real debugging time

- **Glyphs outside the proven set render as tofu.** `⌷` (U+2337) came out as an empty box on the device. Stay inside the glyph set already in shipped use; there is a test pinning this.
- **`keySpec()` in `states.js` must forward every new binding field.** It is a hand-written whitelist and a forgotten field paints a silently wrong key — this has now bitten twice (`size`/`subStrong`, then `segDim`). **There are THREE of these lists, not one:** `keySpec()` for keys, **`zoneUriFor()` for dials**, and `scripts/preview.mjs`, which mirrors both and has drifted twice. Add a field to all three in the same edit, and check `lastZoneFree()` too — it decides whether a zone is empty enough for the clock to take, so a new content field must count as content there as well.
- **Key SVG ids must be derived from content, not a counter.** `SD.image()` dedupes by data URI; a per-call id makes every key look different every frame and turns a static surface into 36 writes at 15 fps.
- **THE SERVICE IS A SEPARATE PROCESS. Restart it on EVERY deploy.** rsyncing new code does nothing to the one already running, and an unknown verb from a stale service fails silently (fire-and-forget has no reply). The plugin log now prints `service vX.Y.Z` on connect — check it matches.
- **`osacompile` proves syntax, NOT behaviour.** Two AppleScript bugs compiled cleanly and failed only when run: `hidden` is reserved inside `dock preferences`, and `front window` raises -1719 when the frontmost process has no window (the Stream Deck app itself). Run the script, do not just compile it.
- **`keystroke "<letter>"` IS LAYOUT-DEPENDENT. Send a `key code`.** This machine's
  layout is **Hebrew** (`com.apple.keylayout.Hebrew`), which has no `f`. Measured:
  `keystroke "f"` typed **ש** — the character on physical key code **0**, i.e. `A` —
  so `ctrl+cmd+f` arrived as **Ctrl+Cmd+A**, which in Finder is **Make Alias**. That
  is the whole "Full Screen duplicates files" bug. It also silently killed dial 4's
  New/Close Tab (`cmd+t` / `cmd+w`) from the day they shipped, and it is not even
  deterministic — the same call typed nothing at all on a later run. `MAC_ANSI` in
  `service/os.js` maps every letter and digit to its `kVK_ANSI_*` code and a test
  fails the build if any macOS path interpolates a letter into `keystroke`.
- **NOTHING may call `setTimeout`/`setInterval`. Use `SOS.Timing`.** A page timer in this hidden WebView is clamped: measured, a 500 ms timeout takes ~1190 ms, and with the app window closed it degrades to roughly once a MINUTE. That single fact was behind the frozen clock, the calculator `+`, the dial-6 long press, the minute-long Ableton strip, ringing MIDI notes and slow reconnects. `js/core/timing.js` serves delays from a Worker; a test fails the build if any other file schedules anything.
- **`setFeedback` has no dedupe of its own — use `SD.feedback()`.** `SD.image()` always deduped; setFeedback did not, so six dial zones were re-sent in full on every repaint. Under the 15 fps Ableton pump that was ~90 multi-KB messages a second and it is what overloaded the machine. Anything new that paints a zone goes through `SD.feedback()`.
- **`scripts/preview.mjs` must mirror `States.paintDial` exactly** — it has now silently drifted TWICE, each time leaving a sheet that looked right and was not evidence.
- **Tests that call a controller's `onTouch` directly cannot see wiring bugs.** The dropped touch-Y axis survived a whole port that way. Drive gestures through the real socket (`test_core.mjs [11]`, `[12]`).

## Known open items

- **The bridge IS live now** (AdiVST is selected as a Control Surface) and EQ8 works end to end: its dials move real bands. **Pro-Q 3 does not** — see the blocked item above. The other twelve controllers have still only ever seen synthetic state, and every one of them resolves parameters by name exactly as Pro-Q 3 does.
- **The 14 Compact strips remain the least-proven code in the project.** State 2 is their only consumer (V14).
- **The 5 un-ported Visualizer views** — bands, rme, gonio, corr, bal. Each paints a labelled "not ported" tile rather than a blank key.
- **Windows pass** — installers written but never run there. Needs my machine: the active-profile pointer (the macOS route is a preferences plist key with no known Windows equivalent) and loopMIDI ports named `Adi RekordBox Controller` / `Adi Studio OS MIDI`.
- **BlackHole 2ch is installed but needs a reboot** before the visualizers see audio.
- Service port is **9011**, not 9010 — Logitech G HUB permanently owns 9010 on this machine.
- Pending, logged in DECISIONS: **D16** (rekordbox MIDI port name has no settings home — needed for Windows), **D17** (encoder level persistence unwired), **D14** flagged as derived-not-ruled and open to veto.
