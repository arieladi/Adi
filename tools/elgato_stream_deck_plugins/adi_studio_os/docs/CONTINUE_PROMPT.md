# Continue Studio OS — paste this into a new session

We are continuing **Studio OS**, my Elgato **Stream Deck + XL** master plugin.

**Repo:** `~/Documents/GitHub/Adi/tools/elgato_stream_deck_plugins/adi_studio_os`

Read first, in this order:

1. `docs/CONTINUE.md` — the full handoff: global rules, my protocols, the deploy
   sequence, and a field-notes section of traps that each cost real debugging time.
   Current as of **Batch 36**.
2. `docs/DECISIONS.md` — append-only ruling log and the source of truth, ~4,050 lines.
   **Batches 32–36 at the bottom are the most recent and supersede a lot above them.**
   It is append-only: correct a stale entry with a NEW entry that cites the old one by
   line number (`DECISIONS.md:2931`), never by editing it. Ten V-numbers are duplicated,
   so "see V55" is ambiguous four ways — always cite the line.
3. `docs/EQ8_MAPPING.md` — the EQ8 dial and touch map. **FROZEN.**

**Never `git push`.** Commit locally only, in the same turn as the work. Trailer:
`Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`
The AdiVST remote script lives in a **sibling** plugin folder — commit it separately.

## State

**1220 tests green across seven suites** — six JS
(`node scripts/test_{core,service,console,modules,viz,ableton}.mjs`) plus
`python3 scripts/test_bridge.py`.

**Batch 36 (V64) is DEPLOYED and running on the hardware** — `service v2.5.0`,
`surface COMPLETE — 36/36 keys, 6/6 dials`, app idle ~1%, clock at a true 999 ms.

**⚠️ ABLETON MUST BE RESTARTED once.** V64 changed the remote script (handshake
validation, `song` as a property, transport listeners) and Live loads remote scripts
**at launch**. Until Adi restarts Live, those changes are inert.

The surface, as of now:

* **Carousel:** `0 Numpad → 1 Divisions → 2 NAV OFF → 0` (V59 deleted the Calculator).
  **Never compare a state index to a literal** — ask `States.FULL` / `States.DELAY` /
  `States.isFullScreen()`. Two renumberings have already broken literals.
* **Ableton owns TWO screens** (V61): `ableton.hub` = Level 1, transport (Play/Stop/Loop)
  on row 0 and five mode folders (VST · MIDI · Device · OS · Delay) on row 3;
  `ableton.vst` = Level 2, the VST grid unchanged. Strip ownership is `focus`
  (`none|vst|mix|os`) and **nav never touches it** — that is what keeps the VST key lit
  after BACK. Never compare `focus` to a literal outside `ableton.js`.
* **Device mode** (V62): Mute/Solo/Arm on dials 1–3 (presses, not turns), dial 4 spare,
  Pan on 5, Volume on 6.
* **Settings** (V64): `js/core/settings.js` is the **single writer** for global settings.
  Modules read/write namespaced keys; nothing else may call `setGlobalSettings`.

## THE AUDIT IS DONE — six reports are the master list

Do not re-audit. Read the relevant one before touching an area:

| Report | Covers |
|---|---|
| `docs/AUDIT.md` | the first purge menu (partly actioned by V60) |
| `docs/AUDIT_PHASE1.md` | backend, Python, installers, docs |
| `docs/AUDIT_DECISIONS.md` | DECISIONS.md itself — all 6 repairs now applied in Batch 36 |
| `docs/AUDIT_PHASE1B.md` | the Node service (the phantom-client leak — FIXED in V64) |
| `docs/AUDIT_PHASE2.md` | frontend and rendering |
| `docs/AUDIT_PHASE3.md` | Python and the remote script |
| `docs/AUDIT_PHASE4_FEATURES.md` | **the Feature Gap Report — what we forgot.** Read this one before proposing any new work. |

**Everything the audits found that Adi authorised was repaired in Batch 36 (V64).**
What is left is in the TODO section at the very bottom of `DECISIONS.md`.

## HOW ADI WANTS AGENTS USED — a hard rule

**ONE agent per prompt, maximum. No fan-out.** Multi-agent fan-outs crashed the session
twice (nine of twelve agents died on a session limit). If a job is too big for one agent,
chunk it across prompts and wait for his go-ahead between chunks.

The single-agent pattern that worked best: give it ONE file or ONE area, tell it what is
already known so it goes deeper instead of re-deriving, and insist it **verify by
executing** rather than by grep. The best findings in this whole project came from agents
that built a harness and ran the real code — the never-written ring buffer, the half-open
socket, the wedged Live Set.

## Adi's protocols — these matter more than speed

* **Stop at every conflict and get his ruling before writing code.** State findings and
  conflicts as **plain text, never an interactive menu**, then wait. He replies with one
  unified instruction.
* **Never move keys or widen a module's footprint without his approval.** Adding to a
  genuinely blank cell is fine; rearranging is not.
* **Verify headlessly AND look at a render before deploying.** `SECTIONS=… node
  scripts/preview.mjs out.html`, then screenshot it. Never ask him to check something you
  could check yourself.
* **Chain the next briefing.** When you finish a piece, put the next one's Discovery
  briefing at the bottom of the same success message rather than asking "shall I
  continue?".

## FROZEN — do not write code for these

**EQ8's layout and Pro-Q 3's mapping.** Adi in Batch 36: *"the EQ8 control is terrible and
needs heavy optimization"* and *"My Pro-Q 3 pre-mapped parameter preset stopped working on
my end."* **Leave both layouts exactly as they are until he rules.** Both are on the TODO
list; see the notes there for what the actual first step is (for Pro-Q 3 it is running
V39's diagnostic, not writing code — the 34 roles are already wired, it is *name
resolution* that fails).

## Two things he is still owed

1. **A route from the Root Hub to the Meters hub.** He has never seen the visualizers run:
   *"There is no button mapped to open the visualizer."*
2. **Written instructions for routing Ableton's audio into BlackHole.** V64 added the input
   picker and fixed the ring buffer, so the software side is ready — the routing
   instructions are not written.

## Known state to not re-discover

* **Rekordbox has never been MIDI-learned.** *"It is currently just a UI placeholder."*
  The 36-key DJ surface controls nothing yet, by design. Not a defect to chase.
* **`test_service.mjs` flakes under sustained back-to-back full sweeps** — a `midi.ports`
  timeout cascades into six failures. 10/10 clean isolated. Pre-existing, environmental,
  ruled out as a product bug. Re-run on a quiet machine before believing it.
* **`install-windows.ps1` has never been run** and has six known first-run failures.
* **H-Delay and Valhalla are not installed here**, so those keys redden with "not
  installed" — that is V49 working, not a fault.

## The traps that will bite you if you skip the field notes

* **Nothing in the frontend may call `setTimeout`/`setInterval` — use `SOS.Timing`.** Page
  timers in this hidden WebView are clamped (500 ms measured at ~1190 ms; ~once a minute
  with the window closed). A test fails the build if any other file schedules anything.
  This does **not** apply to `service/` — Node timers there are fine.
* **`keystroke "<letter>"` is layout-dependent and this Mac runs a HEBREW layout.** Always
  send a `key code`. A test enforces it.
* **Deploy with `./scripts/deploy-mac.sh`.** It always restarts the service and also syncs
  the remote script into Live. The app binary is `MacOS/Stream Deck`, so the obvious
  `pgrep` patterns never match.
* **`osacompile` proves syntax, not behaviour. Run it.**
* **THREE hand-written field whitelists must agree**: `keySpec()` for keys, `zoneUriFor()`
  for dials, and `scripts/preview.mjs`, which mirrors both and has drifted twice. Check
  `lastZoneFree()` too.
* **NEVER string-match through `hashId()`.** Its join argument is a literal
  backslash-u-0001 escape and editing around it by text has mangled it three times. Edit
  that function **by line**, then verify the join byte-for-byte.
* **Glyphs outside the proven set render as tofu.** Drawn shapes (`js/core/icons.js`) have
  no font behind them and are the way around it.
* **A source-text assertion cannot tell code from a comment.** Two tests in this project
  passed for months by matching prose (`axFullScreenToggle()`, `AXFullScreen`). Assert
  behaviour through a real export instead; if you must match source, strip comments first.
* **Before deleting a dead symbol, read its neighbours.** V60 deleted `Ableton.setUrl` as
  dead — and it *was* dead, which was exactly why the PI's `abletonPort` field could never
  work. A dead symbol is sometimes the missing half of a live feature.
* **`xlink:href` costs the SIZE OF THE IMAGE**, not "40 bytes". Budget raster payloads at
  double.
* **Backgrounds are generated** — `python3 scripts/slice_backgrounds.py` after changing an
  image. Do not hand-edit `js/core/backgrounds.js`.
