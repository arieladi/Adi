# Phase 2 — the frontend, UI and rendering engine

**One agent, frontend only. Nothing deleted, nothing modified.** Everything from Phase 1a
and 1b remains recorded and unapplied.

The agent verified by **executing the real files headlessly** — loading `app.html`'s exact
script sequence in `node` and walking every screen × carousel state × focus mode × page —
rather than by grep. I then re-verified the four findings that indict my own recent work.
**All four held.**

---

# §0 — Three of my own recent changes are wrong. Verified.

Listing these first because they are regressions I introduced, not pre-existing rot.

## 0.1 V61 broke the 15 fps strip on the VST page — the screen the strip exists for

`ableton.js:415`:

```js
var onHub = SOS.Nav.current() === hub;      // `hub` is ableton.hub — LEVEL 1
if (live) { if (onHub) composite(); … }
pumpTimer = SOS.Timing.after(live ? (onHub ? Math.max(30, 1000/FPS) : 250) : 750, pump);
```

Before the split, `hub` **was** the VST grid, so this gate was correct. After V61 it names
Level 1, so on `ableton.vst` — Level 2, where the controller strip actually lives —
`composite()` is never called by the pump and the re-arm drops to **250 ms**.

Verified: `enter vst → Nav.current().id = ableton.vst; === A.hub? false`.

Not frozen — `Bridge.on('state') → pickController() → composite()` still fires on every
Live message. But anything the controller animates *itself* (page/tab state, peak-hold,
the touch marker) only redraws when Live speaks or a dial moves. **I introduced this.**

## 0.2 V62's mixer toggles never dim, because `zone()` reads `dim` only inside the icon branch

`render.js:432-458`. `o.dim` is read **exactly once**, at `:446`, inside
`if (o.icon …)`. Verified by grep and by the agent's round-trip:

```
Render.zone({title:'Pan', value:'C', sub:'x', indicator:0.5, color:'#fff', dim:true})
  === the same call without dim  ->  true
```

Eight dial bindings set `dim` to mean offline/unsupported — `root.js:330, 339, 350, 374,
381` and `ableton.js:697, 718, 726` — and only the two that happen to carry an icon (Zoom,
Tabs) actually dim. **So all three V62 mixer toggles, plus Pan and Volume, show no offline
state at all.** That includes the em-dash "n/a on this track" case I was careful to make
visually distinct: the em dash lands, the dimming does not.

Same shape as the `keySpec()` whitelist trap, one layer down.

## 0.3 V60 preserved ~58 lines of dead code on a premise I never checked

I wrote, in the V60 ruling: *"`RME_LIT` / `RME_OFF` / `RME_MARK` / `RME_MARK_OFF` STAY —
despite the name they serve the `meters` view's `style: 'rme'` segmented-LED variant (the
`<pattern>` at ensurePatterns, and the marker column), which is live."*

**Wrong on all three counts.** Verified:

| Claim | Reality |
|---|---|
| "the `<pattern>` at `ensurePatterns`" | `grep -c ensurePatterns` = **1** — my own comment. No such function exists. |
| the segmented-LED variant is live | `segColumn` / `segRows` / `segMarkRows` / `segDefs` (`viz.js:330-380`) reference **only each other**. Nothing calls `segColumn`. |
| `style: 'rme'` selects it | `style` is set at `viz.js:118` and **never read** anywhere. |

`svgMeters` draws plain `rc()` bars. So the four colours **are** dead, and I kept ~58 lines
of segmented-LED machinery alive by asserting a call path I had not verified — in the same
ruling where I criticised the previous audit for not distinguishing test-only from dead.

## 0.4 And `_scopePeak` / `_wavePeak` are the `METER.corr` bug again, still present

`viz.js:670-673` and `:729` compute per-frame peaks. `this.head` — the only thing the dials
read — is populated **only** by `svgMeters` (`:927`). Verified: neither `_scopePeak` nor
`_wavePeak` is ever read.

So V60 removed `METER.corr`/`METER.bal` and left two more instances of the identical bug.
**Visible consequence:** the scope and waveform dials show `…` instead of a level.

---

# §1 — Level 1 / Level 2 fallout

Beyond §0.1:

| # | Finding | Where |
|---|---|---|
| L2 | **`lastZones` is unreachable as a guard and stale-prone as state.** `States.resolveDial()` only ever asks `focusDial` about dials the module owns, so `dial - 1 >= lastZones` cannot be true. And because `composite()` is triple-gated (needs `active`, `isOnline()`, *and* Level 1), `lastZones` can sit at 4 while `moduleDials()` is 6 — blanking dials 5-6 and suppressing the "bridge offline" affordance. Asking `States.moduleDials()` directly removes both the guard and the variable. | `ableton.js:340, 347, 766` |
| L3 | **`level1Keys`'s `cols` parameter is inert** — the two layout entries are behaviourally identical. Measured: zero label differences across all 4×9 cells. `Layout.resolve()` already enforces the column bound. | `ableton.js:871-894` |
| L4 | **Back out of Level 2 destroys a window docked on Level 2.** `syncToScreen` re-applies D15 on every nav change and `ableton.hub` is `fullScreenCapable`. Measured: `enter vst → carousel to Numpad (module cols = 5) → BACK → state 2, dock gone`. New in V61 — before the split there was no in-module Back to arrive with. It also partly contradicts V61's own promise that "BACK changes the keys, never the strip": `focus` survives, the carousel state does not, and `moduleDials()` jumps 4 → 6. | `states.js:177-190` |
| L5 | `shortName()` orphaned — it served the V49-deleted `(8,1)` device readout. | `ableton.js:459` |
| L6 | **Seven stale comment blocks describing screens that no longer exist** — including the whole V50 "idle Track Mode" block, `ableton.js:429-454` describing Level 2 as though it were the hub, and two blocks in `root.js` claiming the Ableton idle state mirrors dials 1-4 when `osDial` mirrors **1-5**. | listed in the agent's table |
| L7 | `_composite` and `_transport` are exported "for scripts/test_ableton.mjs" and have **zero consumers**. Every other `_`-seam does have one. | `ableton.js:977, 979` |

**Answered:** `hubKeys(5)` is reachable and tested. `deviceFocused()` is live (one caller,
`vstDial`). Level 1's 5-column breakpoint is reachable but identical to the 9-column one.

---

# §2 — Dead DOM references: none, and structurally cannot be

**The entire `js/` tree contains zero `document.*`, `getElementById`, `querySelector`,
`createElement` or DOM event wiring** — 40 files, verified by grep. `app.html`'s markup is
text nobody sees (offscreen WebView) with no JS consumer.

`app.html` loads every `.js` in the tree except `timer-worker.js`, which is correct — it is
a `new Worker()` target. Every `src` resolves. No dead CSS in either document; all six
addressed ids in `pi/inspector.html` exist. `layouts/dial.json` declares exactly one
element, `full`, which is exactly what `States.paintDial` sends.

One dangling reference, in prose: `rekordbox.js:605` names a `scripts/test_rekordbox.mjs`
that does not exist.

---

# §3 — Un-ported visualizer scaffolding: **85 lines**

`IMPLEMENTED` covers 4 of 9 views. The largest single item is the **58-line segmented-LED
cluster** from §0.3. The rest: `BANDS`/`BAND_LABELS`, the three `GONIO_*` constants, four
`WALL_*` constants for a spectrum wall that was never built, `fmtBal`/`fmtDb`, the legacy
`analyserL/R` + `dataL/R` AnalyserNode fields (replaced by ScriptProcessor), `this.gL`/`gR`
goniometer scratch (**32 KB per Analyzer × 6 slots**), `this.trail`, `this._rmeCfg`, and the
two dead DSP peaks from §0.4.

Correctly kept: `VIEWS` (all 9, drives the picker), `VIEW_META` for all 9, the "not ported"
tile, and `DEFAULTS` trimmed to 4. Two smaller items: `CYCLE` still lists three un-ported
views the cycle loop skips, and `viewTile(button, name)` never uses `button`.

---

# §4 — The four whitelists: no drift, three dead fields

**All four agree today, field for field** — `keySpec()`, `zoneUriFor()`, `preview.mjs` and
`lastZoneFree()`. The key half can no longer drift, because `preview.mjs` calls
`States.decorate` + `States.keySpec` directly. **The dial half still can**, because
`zoneUriFor` is not exported, so `preview.mjs` keeps a hand copy. **Exporting it deletes
the second whitelist.**

Three fields in `keySpec()` have **no setter anywhere**, verified by walking every
screen × state × focus × page: `kickerColor`, `corner`, `cornerColor`. (`kicker` itself is
set 48 times.) They carry dead entries in `keySpec()`, in `hashId()`, and a dead render
branch.

---

# §5 — Unused functions and dead exports (new)

Declared and never referenced: `Surface.OVERLAY_COL_MIN` (the private var survived V60's
export deletion and is now assigned-never-read), `EQ8Controller.ARROW_W`,
`ProQ3Controller.TAG_W`, `ableton.shortName`, `midictl.cycleChannel`.

Dead exports, ~30 of them. The notable ones:

* **`Bridge.setUrl` + `reconnect()` have zero callers** — which is *why* the PI's
  `abletonPort` is inert. The bridge URL is unchangeable.
* **`Rekordbox`'s entire test seam is unused** — `_midimap`, `_dials`, `_levels`, `_sens`,
  `_shift`, `_notes`, `_fmt`, `_timing`, 10 lines. `test_modules.mjs` regexes the source
  instead (§7).
* **`AVC.Bridge`** — a dead assignment whose comment ("some controllers reach for it
  directly") is false; zero controllers do.
* **`AVC.DeviceController`'s `this.L`** — assigned to every controller, read by none, so
  `L.W` and `L.H` are never read either.
* **`Bridge.handle()` emits 18 events; only six have subscribers** — 14 dead `emit()` calls.
  Harmless (`state` is emitted alongside and *is* subscribed).
* **`IPC`'s `dropped` counter is write-only** — leftover from V60's removal of
  `IPC.droppedCount`.
* Plus `Layout.maxCols`, `Layout.EMPTY`, `Nav.root/depth/path`, `States.name()`,
  `Render.valueZoneUri/SIZES/truncate`, `Clock.city/COLON`, `SD.setFeedbackLayout/openUrl`,
  `IPC.config`, `Plugins.tintAt`, `AVC.LAYOUT`, six `Surface` accessors, four `MidiCtl`
  seams.

**`SOS.Svg.vgrad` is dead only transitively** — its one caller is inside `_buildGraph`,
which is itself dead. The `bag.def`/`defs` machinery dies with it, as does `AVC.gfx.eq`.

**`os.launch` / `os.hotkey` are unreachable for a non-obvious reason:** `root.js:271-272`
*does* call them, but no `SLOTS` entry declares `app`, `hotkey` or `run` — so those two
branches plus `:268` are dead. Deleting the verbs means deleting the branches too.

---

# §6 — Assets: two dead palette entries, nothing else

**`PALETTE.panel` and `PALETTE.panelD` are fully dead** — and `panel`'s comment ("kept:
legacy callers still name it") is **false**; there are no legacy callers.

Every other palette entry, **all 15 icons, all 3 art entries and all 4 background band
keys** are consumed. No orphan assets.

`AVC.gfx` has five dead colours: `panel`, `ok`, `warn`, `bad`, `eq`.

---

# §7 — Dead render paths, and a second silent-drop bug

Beyond §0.2's `zone()`/`dim`:

Two unreachable branches in `key()`, proven by exhaustive enumeration — **`icon`+`title`
never co-occur** (`render.js:351, 355, 359-362`) and **`art`+`title` never co-occur**
(`:383-384, 388-391`), because `root.js` sets `label: undefined` whenever `art` or `icon`
is present, per V26. Both are documented as supported options, so this may be reserved
capability rather than rot — **flagged, not asserted.** (`icon`+`sub` *is* reachable — the
transport keys — so `:363-365` is live.)

Also dead: the `corner`/`cornerColor` branch, the `kickerColor` fallback, and `o.flat`
(established). Two stale docs: `render.js:272` still lists `seg` (removed V59), and
`sd-client.js:8` still tells you to call `flushDirty()` (deleted V60 — and says so 43 lines
later).

Layout logic is otherwise clean.

---

# §8 — Test-quality defects (frontend)

**No raw timers anywhere in `js/`** — all four grep hits are prose. The one live
`setInterval` is in `pi/inspector.html`, a separate *visible* document where throttling
does not apply. Legitimate, though it sits outside the enforcing test's scope.

The defect class from Phase 1b recurs here:

1. **`test_viz.mjs:100` cannot fail either way** — `unported.length === 5 || unported.length === 6`.
   The label says 6, the value is 5, and the `||` passes whether a view gets ported *or* an
   un-ported one is added. The invariant is not pinned.
2. **`test_modules.mjs:495` can silently stop covering anything** — it slices source from
   `indexOf("dials: function (dial)")` and asserts the remainder is hotkey-free. `osNavDial`
   happens to be defined *after* that point purely by ordering luck; moving it up (a pure
   refactor) would reduce the slice to the file tail and the assertion would keep passing
   while checking nothing.
3. **Ten near-vacuous constant checks** — `new RegExp('\\b'+v+'\\b')` against raw source for
   values like `4`, which matches any `4` anywhere including comments.
4. **Two comment-matchable assertions** — `rbSrc.includes(DEFAULT_PORT_NAME)` and
   `mcSrc.includes(scaleName)` against raw source. `test_core.mjs:423` shows the right
   pattern: it strips comments first.
5. **`test_ableton.mjs:507` asserts stale data.** `setState(States.DELAY)` does not
   recomposite, so `bound4 === 4` merely observes the previous 6-zone slices still sitting
   in `zoneSvg` — it would pass even if the Compact reflow never ran. Proven: the "stale"
   zone 1 is byte-identical to the full-width one; an explicit `_composite()` changes it.
   *(The real Compact coverage — 14 per-controller `build(4)` blocks — is sound.)*
6. **`test_viz.mjs` loads a reduced file set** (no icons, art, backgrounds, clock), so its
   key/zone size budgets measure keys with no icons and no band artwork — much looser than
   the device's.
7. **`preview.mjs` has a duplicated section key** (`pick("midi", …)` twice) and **never
   wires `Nav.wire(States.syncToScreen)`**, so the harness cannot reproduce D15 — which is
   why it cannot show L4.

---

# §9 — Checked and HEALTHY

* **Zero DOM references anywhere** — nothing can dangle. Both documents internally
  consistent.
* **All 14 controllers implement `build(zones)`, handle `zones < 6`, and have a dedicated
  COMPACT test block.** The compact path is reachable via `moduleDials() === 4`. **None
  looks unfinished** — which settles the docs' long-standing "least-proven code" worry.
* **The "No room" affordance works** — confirmed for all three layout-less modules.
* **No dangling references to any V59/V60-deleted symbol.** The purges were clean.
* **`Clock.LIT_COLOR` is still load-bearing.** So are `SD.flushCounts` (9 refs),
  `Timing.pending`, `States.stopClock`/`clockKind`/`onClockTick`, and
  `Input.reserved`/`backReserved` — each pins a real invariant.
* **The five insert-key-less controllers are reachable** via `pickController()` from the
  focused device — the earlier audit's error, not repeated.
* `console.js` and `plugins.js` are clean apart from `Plugins.tintAt`.

---

# §10 — What could not be determined

1. **Whether §0.1's pump gate should be widened to `=== hub || === vst`, or whether Level 2
   should rely on Live's push.** Proven broken; the intent is yours.
2. **Whether `render.js`'s `icon+title` / `art+title` branches are rot or reserved
   capability.** Both documented as supported, both unreachable today.
3. **`AVC.gfx.ok`/`warn`/`bad`** — zero references, but they came from the verified 1.5.9.0
   colour table, so deleting them edits data the controllers were built against.
4. **Which `Bridge.cmd` verbs the AdiVST script still answers** — the Python side was out of
   scope, so the eight dead verbs are dead *in the frontend* and may still be live protocol.
5. **No hardware verification.** Reachability, geometry and rendering are proven by
   execution; how a key *looks on the cap* is not.
6. **`rekordbox.js` and `midictl.js` got a lighter behavioural pass** than
   `ableton.js`/`viz.js`/`render.js`. The mechanical scan covered them fully; their logic
   branches were not traced one by one.

---

# Carried forward, all unapplied

* Phase 1a's six DECISIONS.md repairs.
* Phase 1b's leak chain (L1-L3 there: the phantom client and the sounding note).
* Everything here, including **four regressions of my own** in §0.

If anything in *this* phase deserves priority it is **§0.2** — the offline/unsupported
dimming that silently does nothing on six dial bindings — because it is a one-line fix in
`zone()` and it makes five controls tell the truth.
