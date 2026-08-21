# Continue Studio OS — paste this into a new session

We are continuing **Studio OS**, my Elgato **Stream Deck + XL** master plugin.

**Repo:** `~/Documents/GitHub/Adi/tools/elgato_stream_deck_plugins/adi_studio_os`

Read first, in this order:

1. `docs/CONTINUE.md` — the full handoff: global rules, my protocols, where things
   stand, the deploy sequence, and a field-notes section of traps that each cost real
   debugging time. Current as of **Batch 31**.
2. `docs/DECISIONS.md` — append-only ruling log and the source of truth. **Batches
   27–31 at the bottom are the most recent and supersede a lot above them.**
3. `docs/EQ8_MAPPING.md` — the EQ8 dial and touch map (frozen, see below).

**Never `git push`.** Commit locally only, in the same turn as the work. Trailer:
`Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`
The AdiVST remote script lives in a **sibling** plugin folder — commit it separately.

## State

**1153 tests green across seven suites** — six JS
(`node scripts/test_{core,service,console,modules,viz,ableton}.mjs`) plus
`python3 scripts/test_bridge.py`. **Batch 31 is deployed and running on the
hardware** (`service v2.5.0`, `surface COMPLETE — 36/36 keys, 6/6 dials`).

## FROZEN — do not write code for these

**EQ8's mapping, Pro-Q 3 data mapping, and the Calculator.** Two EQ8 rulings noted in
DECISIONS are parked, not live.

## ⚠️ Ableton must be RESTARTED after any deploy that touches the remote script

Live loads MIDI Remote Scripts **at launch**. If plugin keys, Track Mode dials or the
device arrows do nothing, that is why. Additive verbs so far: `load_device` (V30),
`device_key` (V52), `track_volume_delta` / `track_pan_delta` / `get_mix` (V50),
`device_step` / `device_pos` (V53).

**"The remote script must not be modified" means: no editing existing code paths.**
Purely *additive* verbs are the established exception, set by V30 and live ever since.
`cmd_eq8_key` and `cmd_select_device` are both still byte-for-byte intact and both now
unused.

## The traps that will bite you if you skip the field notes

* **Nothing may call `setTimeout`/`setInterval` — use `SOS.Timing`.** Page timers in
  this hidden WebView are clamped (500 ms measured at ~1190 ms; ~once a minute with
  the app window closed). A test fails the build if any other file schedules anything.
* **`keystroke "<letter>"` is layout-dependent and this Mac runs a HEBREW layout.**
  `keystroke "f"` lands on physical key code 0 (`A`) — that is how Ctrl+Cmd+F became
  Finder's *Make Alias*. Always send a `key code`. A test enforces it.
* **Deploy with `./scripts/deploy-mac.sh`.** It always restarts the service. The app
  binary is `MacOS/Stream Deck`, so the obvious `pgrep` patterns never match.
* **`osacompile` proves syntax, not behaviour. Run it.** Five real AppleScript bugs
  were found only by running: `hidden` is reserved, `front window` raises -1719,
  `set w to window 1` then reusing `w` raises -1728, a fixed `delay` was too short to
  see a change, and Chrome accepts an `AXFullScreen` write and ignores it.
* **THREE hand-written field whitelists must agree**: `keySpec()` for keys,
  `zoneUriFor()` for dials, and `scripts/preview.mjs`, which mirrors both and has
  drifted twice. Also check `lastZoneFree()` — a new content field must count as
  content there or the clock paints over it.
* **NEVER string-match through `hashId()`.** Its join argument is a literal
  backslash-u-0001 escape and editing around it by text has mangled it three times. Edit that
  function **by line**.
* **`xlink:href` costs the SIZE OF THE IMAGE**, not "40 bytes" as an old comment
  claimed — the whole data URI is repeated. Budget raster payloads at double.
* **Glyphs outside the proven set render as tofu.** Drawn shapes (`js/core/icons.js`)
  have no font behind them and are the way around it.

## Where the surface stands

**Root Hub** — row 0 Ableton · rekordbox · Tasks · Meters · Chrome; row 1 breathing;
rows 2–3 the nine macOS window states as native pictograms, with the **red traffic
light at (4,2)** (short = quit frontmost, long = force quit, guarded by `NEVER_QUIT`
which includes **Ableton Live** — one line to remove if you disagree) and the **green
one at (4,3)**. Strip: Scroll Y · Scroll X · Zoom · **Apps** · **Tabs** · clock.

**Ableton hub** — four two-column bands backed by your four images, sliced per key:

```
cols 0-1  EQ (violet)        cols 2-3  Dynamics (amber)
cols 4-5  Synths (teal)      cols 6-7  Analyzer & Effects (emerald)
col 8     MIDI · Prev device · Next device · NEXT      (0,0) = Back
```

Every plugin key: **short press** = insert if absent, focus if present, cycle if
several (racks included); **long press** = force a new instance. **Idle Track Mode**
(no device focused): dials 1–4 mirror the Root Hub strip, dial 5 Pan, dial 6 Volume in
strictly 0.5 dB steps.

**Backgrounds are generated** — `python3 scripts/slice_backgrounds.py` after changing
an image. Do not hand-edit `js/core/backgrounds.js`. The VU meter and radar are
patched out by measured bands with a per-column cross-fade.

## Two things I care about most

**Stop at every conflict and get my ruling before writing code** — state findings and
conflicts as plain text, never an interactive menu, then wait; I reply with one
unified instruction.

**Verify headlessly AND look at a render of the real modules before deploying** —
never ask me to check something you could check yourself.

## Open items you may be asked about

* **Pro-Q 3 control** is still unimplemented. The Configure theory was confirmed and
  I have a template with six bands exposed; the remaining work is matching
  `ProQ3Controller.ROLES` against them. Ask before starting.
* **Compact layouts for Rekordbox, MIDI Control and Visualizers** — none has one, so
  docking a window over them still hits the engine's "No room" path. Ask before
  starting.
* **5 un-ported Visualizer views** — bands, rme, gonio, corr, bal.
* **The Windows pass** — installers written, never run there.
* **H-Delay and Valhalla are not installed on this machine**, so those two keys
  report "not installed" until they are. `ValhallaRoom` has a controller too and is
  one line from being added beside `ValhallaVintageVerb`.
* **BlackHole 2ch needs a reboot** before the visualizers see audio.
