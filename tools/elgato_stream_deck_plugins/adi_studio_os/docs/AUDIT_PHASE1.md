# Deep audit — Phase 1: Python, backend, installers, docs

**Nothing has been deleted or changed.** This is a report, as asked.

Scope, as ruled: the Python codebase (the AdiVST remote script and the project's Python
tooling), the Node **backend** service, the installers, and the documentation. **The
frontend JS/UI is Phase 2** and was only read where it was needed to prove whether a
backend thing is called.

> **A scope call to check:** you described Phase 1 as "Python… MIDI handlers, backend
> routing logic". In this project those last two are **Node**, not Python
> (`service/*.js`). I included them, because Phase 2 is "frontend JS/UI" and they would
> otherwise fall between the two phases. Say the word and §4 comes out.

## Method, and what it actually covered

Nine agents in three sequential batches, never more than three concurrent, per your
limit. **Five returned. One stalled, and three died on the session limit** (it resets
at 3:40am). So:

| Area | Covered by | Depth |
|---|---|---|
| `live_bridge.py` (1169 L) | agent | **all 1169 lines read line by line** |
| `ws_server.py` + `AdiVST.py` + `__init__.py` (522 L) | agent | **all read in full** |
| Protocol reconciliation, both wires | agent | **all four verb lists built and diffed** |
| `service/os.js` (915 L) | agent | **all 915 lines read line by line** |
| Installers + deploy (431 L) | agent | **all three read in full** |
| `service/index.js` · `ws-server.js` · `midi.js` · `home.js` (711 L) | **me, by hand** (agent stalled) | targeted: every method vs every caller |
| Python tooling (1303 L) | **me, by hand** (session limit) | mechanical AST scan + every named lead |
| `DECISIONS.md` archaeology (3529 L) | **me, by hand** (session limit) | **targeted lead-checking, NOT an end-to-end read** |
| Doc drift | **me, by hand** (session limit) | every claim about backend paths, ports, versions |

**125 agent findings, plus my own.** The one real coverage gap is the last-but-one row —
see §7. I also **independently re-verified every "bug"-class finding**, because those are
the ones you would act on; two of the verifications changed the answer.

---

# §0 — URGENT: the red traffic light will kill Ableton Live

**This is deployed on your hardware right now.** It is the one thing in this report I
would fix before you next work in Live.

`NEVER_QUIT` (`service/os.js:745`) exists to stop the red traffic light at (4,2) from
quitting or force-killing Ableton mid-session — your words, in DECISIONS: *"an accidental
long press that killed Live mid-session would lose work in a way no other key on this
board can."*

**The guard entry never matches.** `guarded()` (`os.js:773-776`) compares the frontmost
app's **System Events process name** against the list, by exact match or by
*name-starts-with-entry*. Ableton's process name on this machine is **`Live`** — verified
from the bundle, `CFBundleName = Live` and `CFBundleExecutable = Live` in
`/Applications/Ableton Live 11 Suite.app`. So:

```
guarded("Live")          ->  FALSE     <-- Ableton is NOT protected
guarded("Ableton Live")  ->  true      <-- the name that never arrives
guarded("Stream Deck")   ->  true
guarded("Finder")        ->  true
```

I ran the real `guarded()` body against those inputs to confirm it rather than reasoning
about it. Short press = graceful quit, long press = `kill -9`.

**The fix is one entry** — add `"Live"` to `NEVER_QUIT`. I have not made it, because you
said report-only. **Say go and it is a one-line change.** Until then: don't long-press the
red traffic light with Live frontmost.

Two related defects, both real but **dormant because Windows has never been run**:

* **`quitFront` bypasses the guard entirely on Windows** — `os.js:779` returns
  `hotkey("alt+f4")` *before* the guard is consulted.
* **`forceQuitFront` is worse** — `os.js:793` runs `Stop-Process … Where-Object
  { $_.MainWindowHandle -ne 0 } | Select-Object -First 1 … -Force`, which is not the
  frontmost app at all; it is *whichever process Windows happens to list first with a
  window*. Unguarded, and aimed at the wrong target.

DECISIONS.md:2931 claims *"Both verbs resolve the frontmost app FIRST and refuse by
name."* **That claim is false on Windows and, because of the name mismatch above,
ineffective for Ableton on macOS.**

---

# §1 — Other bugs (nothing here is dead code)

| # | Bug | Where | Confidence |
|---|---|---|---|
| 1.1 | **Band 8 of EQ8 is unreachable, and the page press is a permanent no-op.** `EQ8_DIALS = 6` (`live_bridge.py:22`) encodes the legacy "all six dials are band dials" assumption, so `cmd_eq8_page` clamps focus to `8-6+1 = 3`. But **V37 gave dial 1 to Output Gain**, so the frontend has five band dials and `EQ8Controller._maxFocus()` returns **4**. The frontend waits for a focus of 4 that the backend can never report. The legacy plugin's own controller even carried the comment *"keep in sync if either changes"* — **the sync broke at V37.** | `live_bridge.py:370` | certain |
| 1.2 | **Clicking a device in Live with the mouse leaves the Prev/Next caption stale.** `_on_device_changed` emits `device` but never `_emit_device_pos()`. The `devices` listener fires when the device *list* changes, not when the *selection* does, and the frontend only requests position once, in `enter()`. | `live_bridge.py:142` | certain |
| 1.3 | **A close frame is echoed to a socket that has already been torn down** — the reply at `ws_server.py:292` is written after the client is removed, so it never reaches the wire. | `ws_server.py:292` | certain |
| 1.4 | **`cmd_load_preset` can call `track.delete_device(-1)`** for an EQ8 nested inside a Rack: `_device_index` indexes the top-level chain only and returns `-1` on miss, and `-1 is not None`. Self-limiting (wrapped in a try that logs) and the whole verb is dormant, so it cannot fire today. | `live_bridge.py:990` | likely |
| 1.5 | **The only two setters that bypass `_safe_set`.** `cmd_track_volume_delta` / `cmd_track_pan_delta` write `param.value` directly, skipping the `is_enabled` check every one of the other 17 setters uses. Degrades to a logged dispatch error on a macro-mapped or frozen fader, not a crash. | `live_bridge.py:778` | likely |
| 1.6 | **`test_service.mjs` passes by matching a comment.** Line 239 asserts the *source text* of `os.js` contains `axFullScreenToggle()`. **No such function exists** — the only occurrence is inside a comment at `os.js:568` explaining why the approach was abandoned. Delete the comment and the test fails; break the feature and it still passes. | `test_service.mjs:239` | certain |
| 1.7 | **`install-mac.sh` reads `$ROOT` from the environment** rather than deriving it, so a stale exported `ROOT` sends the install somewhere unintended. `deploy-mac.sh:17` derives it correctly — use that as the template. | `install-mac.sh:100` | certain |
| 1.8 | **`install-mac.sh` never restarts an already-running service.** This is the exact failure the deploy script's own header warns about at length: rsyncing new code does nothing to the process already running, and unknown verbs then fail silently because they are fire-and-forget. | `install-mac.sh:166` | certain |
| 1.9 | **`deploy-mac.sh` rsyncs the service *without* `--delete`**, so a removed service file lingers in the deployed copy. The plugin sync does use `--delete`; the service sync does not. | `deploy-mac.sh:30` | certain |
| 1.10 | **Deploy step 3 verifies only the plugin.** `diff -r` covers `com.adiariel.studioos.sdPlugin` and never the service directory — so the half that has no `--delete` is also the half that is never checked. | `deploy-mac.sh:36` | certain |
| 1.11 | **The AdiVST sync fails silently.** `[ -d "$ADIVST" ] && rsync … || true` swallows a genuine rsync failure as well as a missing directory, and nothing downstream confirms the remote script landed. | `deploy-mac.sh:32` | certain |
| 1.12 | **`deploy-mac.sh` waits for the app to die in an unbounded loop** (`until ! pgrep …; do sleep 1; done`) — an app that refuses to quit hangs the deploy forever with no message. | `deploy-mac.sh:25` | certain |
| 1.13 | **The two scripts disagree on how to find Node.** `install-mac.sh:67` *discovers* the newest version (`sort -V | tail -1`); `deploy-mac.sh:20` **hardcodes `20.20.0`**. Verified: that is the only version installed today, so it works — but a Stream Deck update shipping 22.x breaks every deploy. (It fails loudly at step 4, at least.) | `deploy-mac.sh:20` | certain |
| 1.14 | **`install-mac.sh:182` hardcodes `com.adiariel.studioos0.log`, and the log number rotates per launch.** `deploy-mac.sh:50` already does this right — copy that line. | `install-mac.sh:182` | certain |
| 1.15 | **`need()` never checks the `@2x` variant**, so a missing Retina asset passes preflight. | `install-mac.sh:103` | certain |

**`install-windows.ps1` — written, never run.** A desk-check found six more things that
would fail on first contact, listed in §4 rather than here, since you cannot currently
test any of them.

---

# §2 — Dead code, by area

## 2a. The AdiVST remote script — ~172 lines dead in `live_bridge.py`

| Item | Line | LOC | Note |
|---|---|---|---|
| `cmd_load_device` | 494 | 37 | superseded by `device_key` (V49/V52) |
| `cmd_load_preset` | 974 | 29 | the preset system |
| `cmd_eq8_key` | 380 | 23 | replaced by V52 |
| `_emit_eq8_state` + the whole `eq8_state` message | 415 | 13 | **emitted by nothing, handled by nothing** |
| `cmd_select_device` | 1015 | 12 | V53 built `device_step` alongside it instead |
| `cmd_select_track` | 1005 | 10 | parked by V29 |
| `_create_eq8` | 452 | 11 | only caller was `cmd_eq8_key` |
| the EQ8 "Scale" arm of `cmd_eq8_global_delta` | 348 | 8 | unreachable |
| `_find_preset_root` | 951 | 5 | |
| `cmd_list_presets` | 957 | 16 | |
| `cmd_set_index` | 1081 | 4 | |
| `_fmt_hz` | 28 | 4 | **never called since it was written** |

Plus, smaller: `import math` unused (`:15`); `self._preset_items` never read (`:76`);
`self.preset_folder` + its constructor keyword (`:64`); an unused `track` local
(`:761`); `_BAND_RE` accepts a "B" edit-channel then discards it (`:25`); **five
class-body string literals used as section headings** (`:464`, ~75 lines of strings that
are evaluated and thrown away — harmless but odd); and `self._mixtoggles` is used
without being initialised in `__init__` (`:923` — my V62, and it works only because
every read goes through `getattr(self, "_mixtoggles", [])`).

**Duplicate logic:** the parameter-nudge normalisation is written four times
(`:336`, ~30 L), and `cmd_select_device` / `cmd_device_step` are two implementations of
the same index walk.

## 2b. `ws_server.py` — ~36 lines that our one client never reaches

`WSServer.client_count()` (`:80`) and `_Client.fileno()` (`:50`) are dead outright.
The rest is **defensive network code that our single CEF client never exercises**:
per-client `broadcast` targeting (`:183`), the whole fragmentation/continuation path
(`:299`, 14 L), binary frames parsed then discarded (`:305`), ping→pong (`:295`), the
64-bit length branch (`:261`), and the not-masked path (`:271`).

**I would keep most of this.** "Never exercised by our client" is not the same as "cannot
run", and this is a hand-rolled server on a network boundary. The two genuinely dead
functions are the only clear purge.

## 2c. `service/os.js` — ~76 lines dead, ~76 more dormant by platform

Dead: `hotkey` (26 L — no live sender), `volume` (19), `launch` (7), `zoom` (6, the macOS
system magnifier, superseded by `appZoom`), `mute` (5), `missionControl` (5),
`appSwitchHeld` (1), `rescanApps` (1), and `MENU_TILES.topleft/topright/bottomleft/
bottomright/restore/center` (6). Also `MAC_SPECIAL["+"]` / `WIN_SPECIAL["+"]` unreachable
(`:91`), `MAC_KEY.clear` / `.divide` orphaned when V59 removed the Calculator's token
interception, a `platform` field on every `actionAvailability()` entry that nothing reads,
`macAppInstalled` being a wrapper whose body is one negation, and `windowLayout`'s local
`key` shadowing the exported `key()`.

Dormant by platform: **Windows ~61 lines, Linux/X11 ~15 lines.** Listed separately so you
can rule per-platform.

`WIN_TILES` (`:710`) silently no-ops three of the nine ruled window states on Windows —
a gap, not dead code.

## 2d. The rest of the Node service — 89 lines, and it is otherwise clean

* **`service/home.js` — all 85 lines dormant.** Three complete drivers (Hue, Home
  Assistant, Elgato) behind `~/.studioos/home.json`. **I checked: that file does not
  exist**, so `cfg` falls back to `{driver:"none"}` and `dim()` always takes the no-op
  path — and no key or dial calls it anyway. This is **dormant by ruling, not an
  oversight**: see §3.
* **`WsServer.broadcast` (`ws-server.js`) — dead.** Definition only; zero callers in
  `index.js` and zero in the tests.
* `HANDLERS["os.rescan"]` and `HANDLERS["home.status"]` are dead verbs (`index.js:74`,
  `:78`); `HANDLERS["hello"]` is test-only.

**`midi.js` is clean** — I checked every method against its caller and all eleven are
reached from `index.js`. **`ws-server.js` is in good shape too**: all six opcodes are
handled, fragmentation *is* implemented and used, `onDisconnect` *is* wired
(`index.js:108`), and `BIN` is declared and deliberately dropped. `vendor/_resolve_.cjs`
is **load-bearing** — `midi.js:24` uses it as the `createRequire` anchor. Do not delete it.

## 2e. Python tooling — small and clean

A full AST scan of all six files found only three things: **`find_device()` is dead**
(`make_profile.py:49`, 11 L), and two unused module constants — `DEVICE_TYPE`
(`make_profile.py:35`) and `PAD` (`slice_backgrounds.py:87`). No unused imports, no
commented-out logic, no TODO/FIXME anywhere in the six files.

**Two leads from the previous audit are refuted:**
* The three failed background detectors (brightness / contrast / centre-vs-margin) are
  **not** still present as dead code — only the history comment at
  `slice_backgrounds.py:29` survives, which is correct. `verify_band` and `EXPECT_SIZE`
  are both live and wired.
* The two `gen_icons.py` files are **not** near-duplicates: 212 of ~226 lines differ.
  They are different generators.

Two real portability items: `slice_backgrounds.py:67-68` hardcodes `SRC = ~/Downloads`
**and an absolute `OUT` path with your username baked in**; and `gen_icons.py:81,84` draws
**`▢` (U+25A2)** — the exact glyph the field notes call out as tofu. Different risk here
(PIL with a real font, not a device key), but the same trap.

**`adi_ableton_vst_controller/scripts/validate.py` is orphaned tooling.** It is referenced
only by legacy plugin docs (`HANDOFF.md`, `midi_control/README.md`, the rekordbox
CHANGELOG) and by nothing in Studio OS.

---

# §3 — Dormant by ruling: real code that a decision parked

**Do not read these as oversights.** Each was built deliberately and parked deliberately,
and deleting one throws away work you asked for.

| Item | LOC | Ruling |
|---|---|---|
| `service/home.js` + the `home.dim` verb + `IPC.home.dim` | ~85 | **D12** — *"RULING: nothing yet; dial 4 stays inert. The verb, the handler and the driver seam are built anyway, so choosing a system later is filling in config, not adding a feature."* |
| `os.volume` / `os.mute` and their frontend facades | ~26 | **V33** — the dials that called them moved; `root.js:225` says the concepts are kept on purpose |
| `cmd_select_track` / `cmd_select_device` + both facades | ~22 | **V29** — *"the browser arrows and the LIVE key are removed… selectTrack / selectDevice stay on the Bridge for whatever wants them later"* |
| the whole EQ8 preset system, both sides | ~50 | **the PRESETS ruling** — *"PRESETS IS GONE ENTIRELY… left alone because those are protocol against the verified remote script, not UI"* |
| `cmd_eq8_key` + `_create_eq8` | ~34 | **V52** replaced it; the behaviour change is flagged in DECISIONS as *"Flagged for Adi, not decided"* |
| `ACTIONS.cubase` | 1 | **V43** — the Cubase tile is deliberately unplaced |
| `missionControl` | 5 | **V38** superseded **V36** |
| Windows + Linux paths in `os.js`, and `install-windows.ps1` | ~250 | never run; no ruling either way |
| the `install-mac.sh` LaunchAgent path | ~27 | superseded in practice by `deploy-mac.sh` |

**EQ8 is FROZEN** by seven separate rulings. Finding 1.1 is reported and nothing more.

---

# §4 — Forgotten features, and decisions never carried out

These are the ones you are most likely to actually want.

1. **D16 is still open, and it blocks Windows.** The rekordbox MIDI port name is
   hardcoded to `"Adi RekordBox Controller"`. The legacy Property Inspector let you
   rename it, **which is required on Windows to match the loopMIDI port**. DECISIONS
   says it *"needs a home: global settings + a PI field, or a `~/.studioos` config the
   service reads."* Still hardcoded, still no home.
2. **D14 was never ratified.** Six Root Hub keys have macOS/Windows pairs, and it is
   marked *"flagged for veto… Not ruled — say the word and any row changes."* One of the
   six targets **Lynx Mixer, which is not installed on this Mac**, so that key reports
   failure by design.
3. **`install-windows.ps1` — six things that would fail on first run.** It invokes
   `make_profile.py` (`:172`) which **cannot work on Windows** (the active-profile route
   is a macOS preferences plist with no Windows equivalent); the uninstall relies on
   `Process.CommandLine` (`:88`); `Find-Node`'s `[version]` cast throws on any
   non-numeric NodeJS directory (`:69`); there is no Stream-Deck-installed preflight
   (`:99`); `Get-Command python3` will match the Microsoft Store stub (`:161`); and
   `Stop-StreamDeck` uses `CloseMainWindow` on a tray app (`:55`).
4. **`AdiVST.py`'s `ping` verb** (`:156`) is handled and documented in `PROTOCOL.md` but
   **nothing has ever sent it**. No ruling parks it — the doc entry is the only reason it
   survives.
5. **`self.preset_folder`** (`live_bridge.py:64`) is a constructor keyword nobody passes.
6. **`_emit_eq8_state`** is a whole outbound message type (`eq8_state`) that neither side
   uses — built, never wired.

---

# §5 — Documentation drift (fix the docs, not the code)

* **`ARCHITECTURE.md`'s process-topology diagram names five service files that do not
  exist**: `midi/out.js`, `os/keys.js`, `os/volume.js`, `os/apps.js`, `home/lights.js`.
  **Verified each — none exist.** The service is flat: `index.js`, `ws-server.js`,
  `os.js`, `midi.js`, `home.js`.
* **`service/package.json` says `2.0.0`; `index.js` reports `2.5.0`.** The version on the
  wire and in the plugin log is the one in the code, so the package file has been wrong
  for five minor versions.
* **`service/index.js:6` references `docs/IPC.md` twice. It does not exist.**
* **`AdiVST/docs/PROTOCOL.md` is nine verbs and five inbound types behind** the dispatcher.
* **`DECISIONS.md`'s own open-items list is stale**: lines 262-267 still describe D17's
  persistence seam as *"built but unwired"*. **V60 deleted it.** The log is append-only,
  so the fix is a new entry, not an edit.
* `midi.js:197`'s comment says `status()` *"drives the PI's port list"* — there are no
  Property Inspectors (D1).
* `vendor/_resolve_.cjs` tells you to regenerate with `node scripts/vendor.mjs`. **That
  script does not exist.**
* `DECISIONS.md:2931`'s claim about both quit verbs refusing by name — see §0.
* `ws_server.py:87`'s `stop()` comment describes nudging a select loop awake; that is not
  what the code does.
* `docs/AUDIT.md:190` — my own earlier report was partly wrong about `live_bridge.py`;
  this document supersedes it for the backend.

---

# §6 — Checked and healthy (leave alone)

Worth stating, so a future pass does not "clean" any of it:

* **ZERO sent-but-unhandled verbs on either protocol.** This was the class of bug most
  worth finding — these messages are fire-and-forget, so a typo fails with no error
  anywhere. All 32 verbs the frontend sends to Ableton have a dispatcher branch; all 30 it
  sends to the service have a handler; every `os.action` name a key can press resolves in
  `ACTIONS`. Both diffs came back empty.
* **The inbound direction is exactly balanced too** — 18 message types emitted by the
  script, the same 18 handled by the frontend.
* **`AdiVST.py`'s dispatcher has no broken branches in either direction** — all 33 `elif`
  branches resolve to a method that exists, and all 29 `cmd_*` methods have a branch.
* **Listener hygiene in `live_bridge.py` has no leaks.** Checked exhaustively:
  `_listened`, `_param_map`, `_eq8_params` and the mixer listeners all balance, including
  the deliberate filing of EQ8's global params under band 0 so the ordinary teardown loop
  catches them.
* **`MAC_ANSI` is healthy** — a complete lookup table is not dead code, and the 28-line
  comment above it records the measured Hebrew-layout bug it fixes.
* **`macKeyCode`'s test-only export is LOAD-BEARING** — it pins `f !== a` and that
  `MAC_SPECIAL` beats single letters so `"delete"` never collapses to `d`. This is exactly
  the case the last purge nearly got wrong. Keep it.
* **`midi.js`'s `sounding` set and `panic()` are live and deliberate**, not a D9/D9a
  leftover — `panicAll()` runs on the last client disconnect.
* **Every path in all three shell scripts resolves on this machine.** No dangling path.
* `deploy-mac.sh:50` handles the rotating log correctly — it is `install-mac.sh` that is
  wrong. Use the deploy line as the fix template.

---

# §7 — What Phase 1 did NOT cover

1. **`DECISIONS.md` was not read end to end.** The agent assigned to it died on the
   session limit, and I checked its named leads by hand instead (D9/D9a, D12, D14, D16,
   D17, the C++ helper, the preset rulings) — all reported above. **Rulings I was not
   told to look for could still be sitting in those 3529 lines**, and that is the single
   gap I would close first if you want Phase 1 airtight.
2. **The legacy plugins' Python** (~1047 lines across three `gen_icons.py` and three
   `validate.py`) was not audited beyond establishing that nothing live references it.
   **One live dependency confirmed:** `test_ableton.mjs` diffs controller files against
   the legacy AbletonVST folder, so that folder is load-bearing for a test.
3. **`live_bridge.py`'s bug 1.1 was not reproduced on hardware** — Ableton was not
   running. It is proven from the code and the legacy comment, not observed.
4. **The Windows findings are a desk-check**, not a test run.
