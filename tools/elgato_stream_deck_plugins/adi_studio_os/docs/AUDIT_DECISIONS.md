# Phase 1a — audit of DECISIONS.md

**One agent, one file, read end to end (3601 lines).** Nothing changed: no code, and no edit
to DECISIONS.md itself. The correct repair for an append-only log is a *new entry*, and
that needs your word.

I re-verified the five highest-stakes claims by hand before writing this. All five held,
and one of them says **an entry I wrote is factually wrong**.

---

# The five things that matter

## 1. ⚠️ V60's justification for a deletion was FALSE — and I wrote it

V60 (DECISIONS.md:3254) deleted `SD.sendToPI` on the grounds that *"there are no
Property Inspectors — D1"*.

**A Property Inspector ships and is declared.** `manifest.json:13` →
`"PropertyInspectorPath": "pi/inspector.html"`, and the file is 4,692 bytes of working UI
with four editable fields and a `setGlobalSettings` call.

D1 (line 47) rules out **per-key / per-instance** Property Inspectors — one universal
action driven centrally. It does not abolish the plugin-level PI, and one exists. The
deletion itself is probably still fine (nothing called `sendToPI`), but **the reason is
wrong and it is now load-bearing text in an append-only log** — and it is the exact
surface D16 was told to use.

## 2. ⚠️ The Property Inspector has four fields. One of them works.

This is worse than "D16 was never implemented". **It is a decoy**: you can type a port
name, the PI will remember it, and the surface silently ignores it.

| PI field | Written to global settings | Read back by `plugin.js` | Applied |
|---|---|---|---|
| `servicePort` | yes | **yes** (`:168-170`) | ✅ calls `IPC.setUrl` |
| `abletonPort` | yes | assigned at `:172` | ❌ **never read again** |
| `rekordboxPort` | yes (`inspector.html:77`) | **no** | ❌ |
| `studioPort` | yes (`inspector.html:77`) | **no** | ❌ |

Verified: `grep -n "rekordboxPort\|studioPort" js/plugin.js` → no matches at all.

**And V60 made `abletonPort` worse.** It removed `setUrl: Bridge.setUrl` from
`ableton.js`'s exports — the only plausible route for applying that field. Before V60 the
field was unapplied with the mechanism one line away; now the mechanism is gone too.
(Confirmed with `git log -S "setUrl: Bridge.setUrl"` → `72d4838`, the V60 purge.)

## 3. D14 was ruled, on hardware, and reversed — and the log still asks for the ruling

`service/os.js:897-902` says it outright: *"**D14 REVISED on hardware:** Start / Run /
Shell are Windows concepts, and mapping them to Launchpad / Spotlight / Terminal was a
derived guess **Adi rejected** — they are now `mac: null`."* Verified in `ACTIONS`:
`start`, `run`, `shell` are all `{ mac: null, win: … }`.

DECISIONS.md:213-230 still prints the rejected table **and** still closes with *"Not
ruled — say the word and any row changes."* Worse, V43 (line 2295) cites *"(D14)"* as the
authority for `mac: null` — so the log cites D14 for the opposite of what D14 says, and
there is no tiebreak inside the file.

## 4. The two EQ controllers have silently diverged, and L11's whole rationale is void

L11 (line 584) rules Pro-Q 3's compact layout to bands 1/2/3/6 **explicitly because** it
is *"identical to the EQ8 compact ruling (L7), so dial 4 is 'the top end' across both EQ
controllers and there is one muscle memory rather than two."*

Then V37 (line 1989) gave EQ8's dial 1 to Output Gain, dropping EQ8 compact from four
bands to three. Verified:

```
EQ8Controller.COMPACT_BANDS  = [1, 2, 3];      // dials 2..4 -> band
ProQ3Controller.COMPACT_BANDS = [1, 2, 3, 6];   // L11: dial 1..4 -> band
```

**On EQ8 dial 4 is band 3; on Pro-Q 3 dial 4 is band 6.** One muscle memory became two,
`ProQ3Controller.js:65` still cites L11 as its authority, and nothing in the log noticed.
V37 also flags its own two inferences as *"MINE, not Adi's words… Consequence worth his
ruling"* — **still unruled**, and this is what it silently cost.

## 5. Two V-numbers are cited across the codebase and defined nowhere

| Number | DECISIONS headings | Citations in code/tests | What it actually is |
|---|---|---|---|
| **V48** | **0** | **12** | track volume/pan + the unified device key. `test_bridge.py` blocks [7]-[9] are all titled "V48". |
| **V45** | **0** | **10** | Batch 26's icon/frame work — `render.js`, `art.js`, `icons.js`, `clock.js`, `root.js`, two test files. |

Verified: `grep -n "^### V45\|^### V48" docs/DECISIONS.md` → neither appears as a heading.
So twelve code sites and three test blocks cite a decision a reader cannot look up.

---

# The rest of the agent's report

## Missed implementations — decided, never built

Ranked. §1 and §2 above are the top two; the remainder:

| Item | Line | State |
|---|---|---|
| **L2** — Rekordbox compact, "both decks, 4 hot cues each" | 368, restated 1385 | Never built. `rekordbox.js` declares no `layouts` at all, so the 9-column board is clipped when a window docks. |
| **L6** — "every module and controller ships TWO layouts" | 447 | Honoured for all 14 controllers and 3 screens; **not** for `rekordbox.js`, `midictl.js`, `viz.js`, `plugins.js`. `States.moduleDials()` exists and **only `ableton.js` calls it**. |
| **D4** — "ship all five, zero deferrals" | 138 | Visualizers is 4 of 9 views, and V60 *deleted* the scaffolding for the other five. They are now further from existing than when D4 was ruled; nothing revisits D4. |
| **Pro-Q 3's six Configured bands** | *absent* | The commitment ("Adi supplied them, implement later") is **not in DECISIONS.md at all** — only in `AUDIT.md:170`. A reader of the decision log would not know you have already done your half. |
| **V43** — the Cubase tile | 2289 | Still no `cubase.hub` screen. Deliberate and tested, but D11 (193) still reads as though Cubase is a working row-0 tile. |
| **D12** — room lighting | 198 | The seam holds, but V33/V57 took every free dial zone, so "filling in config" no longer yields a control. |

## Outdated rulings that still read as current

**The most dangerous block in the file is lines 339-350**, titled *"Global rules as ruled
(implementation contract)"* — the exact heading a reader searches for. **Six of its seven
rows are now wrong**: Button 35's "Clear", Button 36's carousel, the dials-5-6 overlay,
the cols-5-8 overlay region, "State 2 = full-device takeover", and "States 0-3". All
superseded by D7/L1/L3a/L3b/V2/V3/V13/V14/V59 — all *later* in the file, so append-only
ordering saves a reader who gets that far.

Then: the **Status** section (285-336) has four false claims including "byte-identical
copies" and "`SOS.SvgCtx` … is the strip compositor" (deleted in V60) and "the 9006
protocol is **unchanged**" (six additive verbs since). **V28** (1648) says the clock must
hide "in State 2" — V59 renumbered, so a literal reading now inverts the rule. **V33's
dial table** (1865) is wrong in both columns for dial 5 after V57 and V38. The **L4
"final" table** (1033) still shows EQ8 compact as bands 1/2/3/6. **V44's** dial-2 readout
(2389) was scrapped by V46. Plus nine smaller ones (Numpad "final", D5, D11, V4's
five-state table, D2a/D9/D9a, V17).

**D17 is the confirmed trap:** lines 262-266 present the persistence seam as live; V60
deleted it. Both sit under a heading reading *"Batch 5 — pending"*, so a reader skimming
for open work finds **D16 (genuinely open) and D17 (closed) presented identically**.

## Contradictions

Beyond §1-4 above: **V45/V46 share the group-frame feature** — DECISIONS heads it "V46",
the code calls it V45 in eight places. **D9a says "Implemented in `js/core/input.js`"**;
`input.js:37` says there is *"no timer on Button 36, no forced release, and no
`bindingKind` lookup."* And **three rulings are recorded as bare option letters that read
as cross-references** — "RULING — V1 + V3" (line 744) means ValhallaRoom options, not the
carousel rulings; "RULING — D2" (893) means a dBComp option, not anchor gestures.

**On V55 vs V63:** the agent judged V63's repair *adequate and the model to copy* — it
cites the stale line by number ("DECISIONS.md:2931's claim … was false"), says in which
two ways, and proves the fix by executing the deployed `guarded()`. Residual risk: line
2931 itself carries no marker, so a reader who stops before Batch 35 still reads a false
claim as current.

## Open loops the log says are owed

Genuinely open, most consequential first: **V37's two inferences** (1989, "MINE, not
Adi's words" — and see §4); **D14** (229, ruled but never written back); **D16** (258);
**L3a PARKED** (387, "responsive module dials are the next piece of work"); **L2**;
**V58's ValhallaRoom** ("one line away — flagged, not assumed"); **V61's empty Level 1
space** ("filling it is Adi's call"); **V62b's spare dial 4**; **D12's inert dial**;
**L21's Saturate OUT MODE** ("flagged, not blocking" — needs a hardware observation, and
nobody reported back).

**Marked open but already closed — do not chase:** D9a, D17, P5, Batch 23's Root Hub
ambiguity, V44's per-plugin icons, V47's two flags, V55's "one line to remove if he
disagrees" (you ordered it *strengthened* instead), L13's derivations, and the
`test_service` flake.

**The EQ8 freeze does NOT self-conflict.** Seven notices, all one instruction restated per
batch. Two edges worth knowing: V37 *rebuilt* EQ8's UX and predates the first freeze, so
the freeze protects V37's result rather than L7's; and Batch 27 carves out a narrated
exception for the EQ8 *key*. The only real tension is L7 vs V37, which predates the
freeze.

## Numbering

Gaps: **V48** and **V45** (§5), **Batch 16** (referenced at 1716, never written — it held
V24 and V25, which appear only retroactively as "REVERTED" and "re-landed"), **Batch 2**,
and **P1/P2** (P3 says "recorded as protocol #3 in CONTINUE.md"; the numbering lives
partly in another file).

**Not a gap: D6 exists**, folded into D2a — *"(supersedes D2; raised as D6)"*.

**Duplicate headings — the same V number on unrelated rulings:** V55 ×4 (palette · band
art · face opacity · **the red traffic light**), V49 ×4, V40 ×3, and V36/V38/V46/V47/V53/
V54/V58 ×2 each. **This is why V63's "DECISIONS.md:2931" citation style is right — "see
V55" is ambiguous four ways.**

## What the log gets right — do not "fix" these

The **"Superseded by this batch"** blocks are the file's best feature and are accurate
wherever they exist; the problem is only where they were forgotten. **V41a** is
deliberately counter-intuitive and must survive (*"the reasoning was sound and the
conclusion was still wrong … so a future pass does not 'fix' it back"*). **V60's "TWO
ITEMS WERE PUT BACK"** paragraph is the most useful text in the file for a future auditor.
**Blackhole (L17) and Omnipressor (L20) leave FULL's dials 5-6 unmapped on purpose** —
this looks like an unfinished layout and is not. The **Meters `id`/title mismatch** is
intentional. Every field note is enforced by a test rather than by memory.

**The AdiVST additive-verb list is substantively complete and correct** — every verb the
log claims is additive is present, and nothing pre-existing was edited. One gap, not an
error: `track_volume_delta`, `track_pan_delta` and `get_mix` are additive verbs the log
never names — they are the V48 that does not exist.

## What could not be determined

Whether **V25's promised pump measurement** was ever taken (needs Live, not a grep);
whether the **V63 Windows path actually works** (no Windows machine — and the test asserts
strings are present, which is the category V63 itself warns about); whether
**`ProQ3Controller.OVERRIDES` already matches your six Configured bands** (needs Live with
the device focused — the V39 diagnostic exists and nobody has reported its output);
whether **L17's flagged-for-veto derivation** was ever ratified; and what **P1/P2** say.

---

# Recommended repairs (none applied)

Append-only, so all of these are new entries rather than edits:

1. **A correction entry for V60's PI claim** (§1) and for `abletonPort` losing its
   mechanism (§2). This one I would do first, because I wrote the false text.
2. **`D14a`** — recording that you rejected the Start/Run/Shell macOS mappings on
   hardware, that those three are Windows-only, and that availability is probe-gated.
3. **A ruling on V37's two inferences** (§4), which is what the EQ8/Pro-Q 3 divergence is
   waiting on.
4. **A stale-block marker for lines 339-350** — the "implementation contract" table.
5. **A closing note for D17** in the pending list, so it stops reading like D16.
6. **Retire V45/V48** by defining them, or renumber the code's citations. Defining them is
   cheaper and preserves history.
