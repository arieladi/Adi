# Studio OS — dead-code audit

> ## ✅ RULED ON AND PARTLY ACTIONED — Batch 33 (V60)
>
> Adi approved the purge of `SOS.SvgCtx`, `METER.corr`/`METER.bal`, the empty engine
> exports, `Rekordbox.wirePersist` and the un-ported viz scaffolding, and instructed
> that **the audio/viz path and all fourteen Ableton controllers stay**. Done in V60 —
> 473 lines out. See DECISIONS.md Batch 33.
>
> **Two items came BACK during the purge**, and the reason generalises for next time:
> `Clock.LIT_COLOR` was listed test-only but its one caller pins two colours to a
> single palette entry, so deleting it would delete an invariant; and
> `Surface.inOverlay`'s assertion was dropped rather than rewritten because it
> asserted a constant against itself. **Test-only is not the same as dead, and the
> mechanical sweep in §4 could not tell the difference.**
>
> **Still awaiting a ruling:** §2's five controllers with no insert key (~1,450 lines
> — reachable today by focusing the plugin in Live), §2's dead bridge plumbing and the
> two dead remote-script verb families, §3's parked service verbs and `service/home.js`,
> §5's Windows paths, and §6's doc drift (partly fixed in Batches 32-33).

**Asked for in Batch 32**, alongside the Calculator's removal: *"scan the current
codebase and provide a comprehensive audit report of any unused, dormant, or orphaned
code… Do NOT delete anything else yet (other than the Calculator)."*

**Nothing in this document has been deleted.** It is a purge menu. Every row carries
a `file:line` so you can look before you rule.

## How to read the tiers

| Tier | Meaning |
|---|---|
| **A — SAFE** | Nothing live depends on it. Deleting it changes no behaviour. |
| **B — DORMANT BY DESIGN** | Real, working code that a ruling parked or that hardware has not reached yet. Deleting it throws away work you asked for. |
| **C — YOUR CALL** | Deleting it is a product decision, not a cleanup. |

**Verification status, stated honestly.** This audit was run as a fan-out of eight
independent finders plus an adversarial verifier per finder. **The session limit
killed nine of the twelve agents**, including every verifier and the whole
`doc-drift` finder. Three finders completed (service, assets/scripts, Ableton
controllers) — 75 raw findings, **none adversarially verified**. So I re-verified the
high-stakes claims by hand and **corrected four of them**; those corrections are
marked ⚠️ below. The five dimensions whose finders died I swept by hand instead.
**Coverage gaps are listed at the bottom.** Treat anything not marked "verified by
hand" as a lead, not a verdict.

---

# 0. Bugs, not dead code

**There are none of the kind I went looking for.** Every verb the frontend can send
has a service handler — the `sent-but-unhandled` set is *empty*. That was the one
category that would have been urgent, so it is worth saying plainly.

The one real defect found is a test, not shipping code:

**`scripts/test_service.mjs` is flaky under machine load.** It spawns the real
service and races on real socket timing; a dropped `midi.ports` reply cascades into
the five assertions after it (`virtual CoreMIDI port created`, `port published as a
source`, `two notes tracked as sounding`, `note off decrements`, `disconnect
logged`). It reproduces on unmodified code. The `ask()` helper needs a longer timeout
or a retry. **Re-run it on a quiet machine before believing a failure there.**

---

# 1. The audio / visualizer / BlackHole cluster

You named this one, so it goes first — and **the headline is not what you expected.**

## ⚠️ Correction 1 — `BlackholeController.js` has nothing to do with audio routing

`js/ableton/BlackholeController.js` is a controller for the **Eventide Blackhole
reverb**, an H9-series plugin (`registry.js:57-60`, matched on `/\bblackhole\b/i`).
It is unrelated to **BlackHole 2ch**, the macOS loopback driver the visualizers need.
Same name, different things. Nothing in the codebase touches the loopback driver —
there is no device-enumeration or input-selection code for it anywhere.

## ⚠️ Correction 2 — the visualizer audio path is NOT unwired

`docs/ARCHITECTURE.md:31` draws an `audio/engine.js` with the note *"Web Audio
FFT/meters ◄── Visualizers live here"*. **That file does not exist and never did** —
`js/audio/` is not a directory. But the capability was not dropped; it was folded
into `js/modules/viz.js`, and it is **real, live, complete code**:

| | |
|---|---|
| `viz.js:814-820` | `navigator.mediaDevices.getUserMedia` — real capture |
| `viz.js:830` | real `AudioContext` |
| `viz.js:837-838` | `createMediaStreamSource` → `createScriptProcessor(BLOCK, 2, 2)` |
| `viz.js:786-810` | real per-sample RMS, peak, correlation and balance DSP |
| `viz.js:1062` | the Audio key on the hub, wired to `audioToggle` |

So **RMS and peak metering are implemented and wired, not orphaned.** The reason you
have never seen it move is the one already in the handoff: **BlackHole 2ch needs a
reboot before `getUserMedia` can see it.** `getUserMedia` captures an *input* device
and never system output, so without the loopback there is simply no signal
(`viz.js:12-20` documents the whole chain, and every failure mode paints itself on
the surface via `audio.status`).

**This is a reboot away from working, not a rewrite.** Nothing here should be purged.

## What IS genuinely dead in viz.js

`IMPLEMENTED = { spectrum: 1, scope: 1, waveform: 1, meters: 1 }` (`viz.js:947`) —
**4 of the 9 views are real; 5 were never ported** (`bands`, `rme`, `gonio`, `corr`,
`bal`). The honest surprise: **the un-ported views are ~40 lines of scaffolding, not
thousands.** The bodies were never written, so there is far less to reclaim here than
the question implies.

| Tier | Item | Where | LOC |
|---|---|---|---|
| **A** | `RME_BANDS`, `RME_FLO`, `RME_FHI`, `RME_LABELS` — ISO 1/3-octave tables for the un-ported DIGICheck view. Defined once, **referenced nowhere**. Verified by hand. | `viz.js:133-137` | ~5 |
| **A** | **`METER.corr` and `METER.bal` are computed every frame and never read.** Written at `viz.js:805` and `:808`, and the only other mention is their initialiser at `:179`. Their views are un-ported. Verified by hand. | `viz.js:803-808` | ~6 |
| **A** | The `sLR` / `sLL` / `sRR` accumulators that feed them sit **inside the per-sample hot loop** — and `sLL`/`sRR` are byte-for-byte duplicates of `sL`/`sR` (`+= a*a`, `+= b*b`). Three redundant multiply-adds per sample, per channel, at 15 fps, for two numbers nothing draws. | `viz.js:791-798` | ~3 |
| **A** | `audioStop()` resets `rmsL/rmsR/peakL/peakR` but **not** `corr`/`bal` — the tell that they were bolted on and forgotten. Harmless only because nothing reads them. | `viz.js:877` | 1 |
| **C** | `DEFAULTS` entries for the 5 un-ported views (`bands`, `rme`, `gonio`, `corr`, `bal`) — real tuning constants copied verbatim from the legacy plugin. **Delete only if you are abandoning those views.** | `viz.js:118-128` | ~11 |
| **C** | The 5 `VIEWS` / `CYCLE` entries and the `not ported` tile path that paints them. Keeping them is what makes the gap visible on the device. | `viz.js:93-96`, `:978`, `:1013` | ~10 |
| **B** | `Viz._start` (`startPump`) is exported and **has no caller** — `preview.mjs` only ever calls `_stop`. Verified by hand. | `viz.js:1131` | 1 |
| **G** | `scripts/test_viz.mjs` is 102 lines against viz.js's 1134. **The least-tested large file in the project**, and untested + unreachable is exactly where rot hides. | — | — |
| **G** | `ARCHITECTURE.md` still draws `audio/engine.js`. It does not exist. | `ARCHITECTURE.md:31` | doc |
| **G** | `manifest.json` advertises "live audio visualizers" — true in code, never yet true on this machine. | `manifest.json:8` | doc |

**Still live, leave alone:** `RME_LIT` / `RME_OFF` (the segmented-LED `<pattern>`
machinery, `viz.js:349`), `RME_MARK` / `RME_MARK_OFF` (`viz.js:383`), `ZONE_RED` /
`ZONE_YEL` (`viz.js:919`). These carry the `meters` view's `style: 'rme'` variant and
are not part of the un-ported `rme` view despite the name.

---

# 2. The Ableton controller fleet

## ⚠️ Correction 3 — no controller is unreachable

The finder called five controllers `unreachable-feature`. **That is wrong, and acting
on it would have deleted ~1,450 lines of working code.** Verified by hand:

* **All 14 controllers have a registry entry** (`registry.js`, one `ctor:` per file —
  checked every file against the table, zero misses).
* `pickController()` (`ableton.js:625-627`) resolves the strategy from
  **whatever device is focused in Live**, via `AVC.registry.resolve(st)`. It does not
  consult the plugin catalogue at all.
* So **every controller activates the moment you focus its device in Live**, whether
  or not a key can insert it.

The real, defensible finding is narrower: **five controllers have no key on the
surface to insert their device.** The catalogue in `plugins.js:112-176` holds 17
device names; these five are not among them.

| Tier | Controller | Device it drives | LOC |
|---|---|---|---|
| **C** | `ValhallaRoomController` | Valhalla **Room** — the handoff already flags it as "one line away" from a key beside VintageVerb | 300 |
| **C** | `BlackholeController` | **Eventide Blackhole** reverb (not the audio driver) | 295 |
| **C** | `OmnipressorController` | Eventide Omnipressor | 292 |
| **C** | `SaturateController` | Newfangled Audio Saturate | 248 |
| **C** | `SideMinderController` | RJ Studios SideMinder ME2 | 317 |
| | | **total** | **1,452** |

**Your call, and it is a two-way door:** add five catalogue keys and they all become
first-class, or delete them. What they are *not* is dead — they work today if you
focus the plugin in Live.

## Dormant because the plugin is not installed here

| Tier | Item | Note | LOC |
|---|---|---|---|
| **B** | `HDelayController` | H-Delay (Waves) is not installed on this machine. Its key reddens with "not installed" — V49 working, not a fault. | 215 |
| **B** | `ValhallaVintageVerbController` | Same — Valhalla is not installed. | 276 |
| **B** | `SpectreController` | Has a catalogue key. Per the handoff it has only ever seen synthetic state. | 425 |
| **B** | `PulsarMassiveController` | Same. | 400 |
| **C** | `ProQ3Controller` | **34 roles declared, 0 ever bound.** Functionally dead until the six Configured bands you built are matched against `ROLES`. You asked for this to wait. | 330 |

## The SvgCtx tombstone — the single cleanest purge in the project

| Tier | Item | Where | LOC |
|---|---|---|---|
| **A** | **`SOS.SvgCtx`** — the Canvas-2D-emits-SVG shim. `docs/CONTINUE.md` already says it "has **no controllers left to serve** — L4 is complete", and that is now measured: **zero of the 14 controller files reference it.** Its only remaining callers are 5 references in `test_ableton.mjs`, which exist to test the shim itself. Verified by hand. | `ableton.js:54-298` | ~244 |
| **A** | `SvgCtx` methods with **no reference at all**, tests included: `closePath`, `_alpha`, `_pathBounds`, `_mark`, `_font` | `ableton.js` | ~25 |
| **A** | `Ableton._ctx` — the exported handle to it. Definition and export line only. Verified by hand. | `ableton.js:949` | 1 |
| **A** | `composite()`'s Canvas-shim fallback branch — reachable only if a controller used `SvgCtx` | `ableton.js:607` | ~8 |
| **A** | `AVC.DeviceController.prototype.renderTouch` — unreferenced | `ableton.js:343` | ~4 |

Purging `SvgCtx` means deleting its tests too. **Test `[2]` in `test_ableton.mjs`
already asserts the inverse** (that no `*Controller.js` still matches the old
version), so the tombstone is documented — it just was not removed.

## Dead bridge plumbing, both sides

| Tier | Item | Where | LOC |
|---|---|---|---|
| **A** | `Bridge.cmd.eq8Key` / `listPresets` / `loadPreset` | `ableton.js:534` | ~5 |
| **A** | `Bridge.cmd.selectTrack` / `selectDevice` / `setIndex` | `ableton.js:560` | ~3 |
| **A** | Inbound `presets` and `eq8_state` handlers — nothing sends the requests | `ableton.js:493` | ~6 |
| **B** | `cmd_eq8_key` (+ `_create_eq8`) in the AdiVST remote script — **byte-for-byte intact and unused**, exactly as the handoff says | sibling repo | ~51 |
| **B** | `cmd_list_presets` / `cmd_load_preset` / `_find_preset` in the remote script | sibling repo | ~85 |

**Remote-script deletions are a separate commit in a separate folder, and they mean
restarting Live.** "Must not be modified" has only ever been relaxed for *additive*
verbs — removing one is a new kind of change and needs your explicit word.

## Smaller items in the fleet

| Tier | Item | Where |
|---|---|---|
| **A** | `SOS.Svg.stroke` and the exported `SOS.Svg.esc` — unreferenced | `svg.js:56` |
| **A** | `SOS.Svg.vgrad` + the `bag` `<defs>` machinery — no consumer | `svg.js:103` |
| **A** | `EQ8Controller` response-curve cluster (`_buildCurve` and friends) | `EQ8Controller.js:249` |
| **A** | `BlackholeController.PAGE_DIALS`, `OmnipressorController.PAGE_DIALS` | both `:65-67` |
| **A** | `ProQ3Controller.prototype._validateModes`, `ProQ3Controller.MODES` | `ProQ3Controller.js:176`, `:190` |
| **G** | `registry.js` header comment points at an "add your VST here" workflow that no longer matches the file | `registry.js:5-13` |

---

# 3. Service verbs with no UI

Nothing in the shipped surface can reach these. **Each is a working verb with the
dial that used to call it removed** — `root.js:224-227` says so outright for the first
three: *"the volume / mute / lights locals went with the dials that used them… they
simply no longer have a home on THIS strip."*

| Tier | Verb | Handler | LOC | Note |
|---|---|---|---|---|
| **B** | `os.volume` | `os.js:174` | ~21 | deliberately parked seam (V33) |
| **B** | `os.mute` | `os.js` | ~7 | same |
| **B** | `home.dim` + **all of `service/home.js`** | `home.js` | ~85 | the D12 lighting dimmer. Hue / Home Assistant / Elgato drivers, config from `~/.studioos/home.json`. **Fully built, zero UI.** |
| **A** | `home.status` | `index.js:78` | ~4 | zero frontend references at all. Verified by hand. |
| **A** | `os.rescan` | `index.js:74` | ~3 | zero frontend references at all. Verified by hand. |
| **A** | `os.zoom` | `os.js` | ~10 | **superseded** by `os.appZoom`, which is what dial 3 calls (`root.js:351`). The one dead verb with no ruling behind it — a genuine leftover. |
| **A** | `os.missionControl` | `index.js:66` | ~7 | no UI caller |
| **A** | `midi.panic` | `index.js:44` | ~2 | no UI caller |
| **A** | `WsServer.broadcast` | `ws-server.js:95` | ~4 | nothing broadcasts |
| **A** | `os.appSwitchHeld` export | `os.js:283` | 1 | |
| **A** | `MENU_TILES: topleft / topright / bottomleft / bottomright` | `os.js:476` | ~6 | no key requests them |
| **A** | `ACTIONS.cubase` | `os.js:824` | 1 | stub — see §5 |
| **C** | Linux / X11 fallback paths | `os.js:78` | ~10 | you do not run Linux |
| **C** | Windows-only paths in `os.js` + `install-windows.ps1` | `os.js:38` | ~75+ | **written, never run.** Dormant until the Windows pass. |

## ⚠️ Correction 4 — three of these were wrongly listed as dead

Verified by hand: **`midi.open` is live** (`rekordbox.js:592`), **`os.actions` is
live** (`root.js:211`, via `IPC.ask`), and **`os.hotkey` and `os.launch` each have a
real UI caller.** The finder missed them because they are called outside `ipc.js`.

---

# 4. Dead engine exports

Each verified by hand: the definition and the export line, and **nothing else in the
repo, tests included**.

| Tier | Item | Where |
|---|---|---|
| **A** | `IPC.droppedCount` | `ipc.js:184` |
| **A** | `States.overlayScreen` — a dead alias; `navScreen` is the live name and both point at the same function | `states.js:424` |
| **A** | `Nav.keyBinding` — `states.js` resolves keys through `Layout` instead | `nav.js:115`, exported `:137` |
| **A** | `SD.deviceOfType` | `sd-client.js:103`, exported `:128` |
| **A** | `SD.flushDirty` — superseded by `flushCounts`, which the tests do use | `sd-client.js:51`, exported `:130` |
| **A** | `Surface.isKey` — `isDial` is used, `isKey` is not | `surface.js:88`, exported `:120` |
| **A** | `Rekordbox._keys` | `rekordbox.js:627` |
| **A** | `SD.sendToPI` / `sendToPropertyInspector` — there are no Property Inspectors | `sd-client.js:97` |
| **A** | `abletonPort` setting → `SOS.Ableton.setUrl` — nothing sets it | `ableton.js:946` |
| **B** | **`Rekordbox.wirePersist` + `restore` — this is D17.** The comment at `rekordbox.js:619` says it outright: *"restore(saved.levels) at boot and wirePersist(fn) to start saving."* The mechanism is built; **nothing calls either end.** Encoder levels reset every launch. | `rekordbox.js:252`, `:620` |
| **A** | Exports of constants used only inside their own file: `IPC.DEFAULT_URL` (`ipc.js:187`), `Surface.OVERLAY_COL_MIN` (`surface.js:112`), `Clock.CELL_W/CELL_H/ADV_DIGIT/ADV_COLON` (`clock.js:184`). The constants are live; only the exports are dead. |
| **A** | `Surface.inOverlay` — one caller, `test_core.mjs:93`. Test-only. | `surface.js:38` |
| **A** | `Clock.LIT_COLOR` — test-only | `clock.js` |
| **A** | `defineSlot` / `clearSlots` | `root.js:221` |

**Left in place from the Calculator removal, reported rather than removed:** `o.flat`
in `render.js`'s `face()` lost its only caller with the `seg` display row. It is one
arm of one ternary inside the function that paints all 36 keys — a bad trade for one
word. `PALETTE.faceLo` is **not** orphaned with it; the next line still uses it.

---

# 5. Scripts, assets and config

| Tier | Item | Where |
|---|---|---|
| **A** | `imgs/plugin/icon.png` / `icon@2x.png` — unreferenced by the manifest |
| **A** | `pi/inspector.html` `rekordboxPort` / `studioPort` fields — `ARCHITECTURE.md` says there are no per-key Property Inspectors, and nothing reads these | `inspector.html:37` |
| **A** | `find_device()` in `make_profile.py` | `make_profile.py:49` |
| **A** | `pick("midi", ...)` is declared **twice** in `preview.mjs` — the second silently shadows the first, so one intended sheet never renders | `preview.mjs:163` |
| **A** | `test_viz.mjs`'s un-ported-view count assertion is test-only bookkeeping | `test_viz.mjs:94` |
| **B** | `install-windows.ps1` profile-generation step — never run | `:172` |
| **C** | `ACTIONS.cubase` + the unplaced Cubase tile. **This is deliberate and correctly handled** — `root.js:85` sets `col: null` and `root.js:64-65` explains that `cubase.hub` does not exist, so a machine with Cubase would have shown a key that went nowhere. Dormant by design; delete only if Cubase is off the roadmap. | `root.js:85` |
| **G** | `slice_backgrounds.py` hardcodes `SRC = ~/Downloads` and an absolute OUT path | `:67` |
| **G** | `install-mac.sh` hardcodes `com.adiariel.studioos0.log` — **the log number rotates per launch**, which the handoff warns about | `:182` |
| **G** | `install-mac.sh` `need()` skips `@2x` variants | `:103` |
| **G** | `service/package.json` says `2.0.0`; `index.js` reports `2.5.0` | `package.json:3` |
| **G** | `make_profile.py --activate-only`, `service/vendor/_resolve_.cjs`'s regeneration comment, and a reference to a `scripts/vendor.mjs` that does not exist | various |
| **G** | `test_service.mjs:213` mentions `axFullScreenToggle`, which no longer exists | |
| **G** | `gen_icons.py` fills with unblended RGBA alpha in `rounded_panel` / `grid_mark`, and uses `▢` (U+25A2) — **outside the proven glyph set**, the exact tofu trap in the field notes | `gen_icons.py:71`, `:81` |
| **A** | `rekordbox.js:624` refers to a `scripts/test_rekordbox.mjs` that does not exist, and to `Rekordbox._mi…` | |

---

# 6. Doc drift (fix the docs, do not purge code)

`ARCHITECTURE.md` is the drifted one. Verified against the code:

| Claim | Reality |
|---|---|
| `audio/engine.js` — "Visualizers live here" | **Does not exist.** Folded into `js/modules/viz.js`. |
| State carousel "0 Numpad · 1 Calculator · 2 Delay Calculator · 3 Context Nav · 4 Full Screen" | **Fixed in Batch 32.** Now correctly `0 Numpad · 1 Divisions · 2 NAV OFF`. |
| "Button 35 + Button 36, held 1 s — State Carousel" | **Wrong since V2.** Both are plain keys; the carousel is a long press on dial 6. `CONTINUE.md` is right. |
| "Key 2 → Cubase Hub (placeholder)" | Deliberately unplaced (`col: null`), and there is no `cubase.hub` screen. |
| Level 0 dial list: "1 Master Vol (push=mute) · 2 OS Zoom · 3 App Switcher · 4 Room Lighting" | **All four moved in V33/V56.** Now Scroll Y · Scroll X · Zoom · Apps · Tabs · clock. |
| The `service/` file map (`midi/out.js`, `os/keys.js`, `os/volume.js`, `os/apps.js`, `home/lights.js`) | **None of those paths exist.** The service is flat: `index.js`, `ws-server.js`, `os.js`, `midi.js`, `home.js`. |
| `CONTINUE.md` "State 2 is the Compact suite's only consumer" | **Fixed in Batch 32** — it is Divisions, now index 1. |

---

# 7. Looks dead, is actually live — leave alone

So you do not have to ask later:

* **All 14 Ableton controllers.** Registry-resolved from the focused device. §2.
* **The whole viz.js audio path.** Real Web Audio, real RMS/peak. Blocked on a
  reboot, not on code. §1.
* **`midi.open`, `os.actions`, `os.hotkey`, `os.launch`.** Live callers outside
  `ipc.js`. §3.
* **`RME_LIT` / `RME_OFF` / `RME_MARK` / `RME_MARK_OFF` / `ZONE_RED` / `ZONE_YEL`.**
  Named for the un-ported view, used by the live one. §1.
* **Parameter role tables, name regexes and `OVERRIDES` in every controller.** These
  are runtime-bound against real Ableton and are data, not ink. Nothing in the repo
  referencing a string is not evidence of anything.
* **`MAC_ANSI` in `os.js`.** A complete ANSI keycode table. A complete lookup table is
  not dead code — and the field notes exist because an incomplete one caused the
  Hebrew-layout bug.
* **`PALETTE.panel`** (`render.js:59`) — the comment already says "kept: legacy
  callers still name it".
* **`registry.js` itself.** Deliberately unchanged; it is the device→strategy table,
  not ink.

---

# 8. What this audit did NOT cover

Stated plainly, because a gap read as coverage is worse than a gap.

1. **No finding was adversarially verified by a second agent.** Every verifier died.
   I hand-verified §1, §2's reachability claim, §3's four corrections and all of §4;
   **§5 and §6 are single-source and unverified.**
2. **The `doc-drift-and-history` finder never ran.** I swept `ARCHITECTURE.md` against
   the code by hand for §6, but **I did not read all 3,083 lines of `DECISIONS.md`
   end to end.** Features scrapped by a ruling whose code still exists are therefore
   *under-reported*. Known leads not yet chased: the Button-36 carousel and the
   D9/D9a hanging-note apparatus (V2), the held nudge at (8,3), the 5-zone
   dial-borrowing case, the pre-slice background path, and D14/D16.
3. **`js/core/render.js` (518 lines) and `js/modules/midictl.js` (464 lines) got only
   the mechanical export sweep**, not a read. Dead *branches* inside live functions
   would not show up.
4. **The 14 Compact strip layouts** — the handoff calls them "the least-proven code in
   the project". I confirmed the path is reachable (Divisions borrows two dials and
   Pro-Q 3 does drop to `build(4)`) but **did not audit each of the 14 individually.**
5. **`imgs/**` coverage is partial.** I confirmed the two unreferenced plugin icons;
   I did not diff every PNG against every reference.
6. **No LOC total for the whole candidate set is given on purpose.** The clusters are
   the honest unit — a single number would imply the tiers are interchangeable, and
   §2 alone contains 1,452 lines I am explicitly telling you *not* to treat as dead.
