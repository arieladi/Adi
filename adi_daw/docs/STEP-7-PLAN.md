# Step 7 — the minimal arrangement UI: the plan

**Status: PROPOSED, for the director's review. No UI code is written until he
approves it** (win's round 5). Owner: mac. Companion ADR: **ADR-0180**.

Step 7 is *"the minimal arrangement UI: the first thing you can make a track
in"* (README, roadmap). This document says **in what order it is built, what
it must not foreclose, what it is checked against, and what it excludes**. It
does not redesign the shell: `UI-ARCHITECTURE.md` §1–§2 already describe the
layout and the component tree, and §8–§11 are *decided* in ADR-0050. This plan
amends that document in two named places and otherwise builds what it says.

---

## 0. Why a plan before the code

Step 6's host half produced four merged PRs, and **four of its defects were
found by building the next piece, not by reviewing the previous one**:
implementing `request_flush` revealed that the previous commit had silently
killed every CLAP main-thread callback; writing the `paramsClear` test revealed
that its `missing` flag was decorative; a planted fault revealed dead code in
the Airwindows filter. Re-reading a diff found none of them.

So this plan is ordered on one principle: **each step is the thing that would
expose the previous step being wrong.** That is not a stylistic preference —
it is the only review mechanism that has actually worked on this project.

---

## 1. What step 7 is NOT

Fixed here so the plan cannot grow:

| Not in step 7 | Where it lives |
|---|---|
| The Session view, docked or detachable | step 12 (ADR-0101, ADR-0117) |
| The AI agent, at any tier | steps 8–9 |
| Pure Data devices and the patch editor | step 10 (ADR-0145 d8, ADR-0177) |
| Control-surface hardware (Stream Deck etc.) | P2 (ADR-0181 d4) |
| An Inspector | rejected (ADR-0047) |
| Aux sends | rejected (ADR-0072) |
| Comping, VariAudio, Direct Offline Processing | later steps |
| A second shell document | forbidden — revise `UI-ARCHITECTURE.md` |

Step 7 also does **not** close any "Not decided" left open by an ADR. Where one
is in the way, this plan names it (§6) rather than quietly deciding it.

---

## 2. The three amendments this plan makes to `UI-ARCHITECTURE.md`

### 2.1 The repaint clock is per WINDOW, not one on the root

`UI-ARCHITECTURE.md:248` says *"a single `juce::VBlankAttachment` on the root
drives the whole shell."* **ADR-0063 d3 says otherwise, and it is right**: a
window on a second monitor has a different refresh rate, so each window gets
its own frame clock with a per-window coalesced dirty set over a shared source
of truth. ADR-0063 records this as *"the interaction worth catching now rather
than at step 7"* and says explicitly *"this lands in mac's lane."*

This matters beyond tidiness: the analyser's big floating window (ADR-0183 d4)
and every plug-in editor window (ADR-0076) are on that seam. A dirty set
written as one global set cannot be split per window later without touching
every component that marks dirt.

**The amendment:** one `VBlankAttachment` **per window**; the dirty set is
per-window; the source of truth is shared. An OpenGL view **attaches to its
window's clock and never drives its own repaint** — `setContinuousRepainting`
is the second clock ADR-0183 d4 already considered and refused.

### 2.2 `SnapshotReader` is not `SnapshotPublisher::AudioRead`

`UI-ARCHITECTURE.md` names a `SnapshotReader` that takes one reference at the
top of each frame (:261–266, ADR-0050 d3). It does not exist in code, and the
obvious implementation is wrong.

`AudioRead`'s safety argument requires **exactly one reader**, moving only
forward, announcing before it dereferences (`publisher.hpp:38-41, 147-170`).
A second reader storing into `inUse_` breaks epoch reclamation — the retired
graph is freed while the UI holds it. **The UI gets its own read path**, and
the plan states this before anyone writes the obvious three lines.

### 2.3 `MainSplit` is a panel→slot map, not a `StretchableLayoutManager`

`UI-ARCHITECTURE.md:55` specifies `MainSplit` as a
`juce::StretchableLayoutManager` with three columns. **ADR-0080 — a `DECIDED
(direction)` companion to ADR-0063 — rules that out**, and no reader of this
plan had looked at it until a completeness pass went hunting for what nobody
had read.

A `StretchableLayoutManager` is slot-indexed and holds the widths itself,
which is exactly the `leftWidth`/`rightWidth` shape ADR-0080 d2 names as the
bug. What it obliges instead:

- layout is an ordered **panel→slot** mapping, never a `bool swapped` (d1);
- **a width belongs to the PANEL and follows it across a side swap** (d2),
  stored in `ui_view` beside ADR-0063's dock state;
- minimum widths are **per panel**; a swap that does not fit clamps, takes the
  remainder from the arrangement, and **refuses with a message** rather than
  silently (d3);
- a swap **reorders, never reconstructs** (d4) — ADR-0063 d1's rule, arriving
  for a second reason;
- FlexBox/Grid at the top with **splitters owning the widths** (d5), because
  JUCE flex has no draggable divider.

Step 7.2 builds the panel→slot map with per-panel widths and minima from the
first line. Retrofitting it costs the whole layout pass plus every persisted
width.

---

## 3. The build order

Each numbered step is buildable, testable, and **exposes a flaw in the step
before it** if there is one. The first two need no window at all.

### Step 7.1 — the seams, headless

`SnapshotReader` (UI-side, per §2.2) and `OpSubmitter`. No JUCE, no window.
Tested against a hand-built `Snapshot`, which `UI-ARCHITECTURE.md:94-98` says
is the point of the no-committed-state rule.

*Exposes:* whether a component can be written that owns no project state.

### Step 7.2 — `AdiRootComponent` + `TransportBar`, one window

The smallest thing that proves a window reads a snapshot and submits an op.
Brings in `juce_gui_basics`, the per-window `VBlankAttachment`, the dirty-set
drain, and `ui_view` state (SPEC §8.4).

Also builds the **command layer**, which is not optional and was nearly
missed: every row of ADR-0129's checklist — the gate step 7 is measured by —
is a keyboard gesture (`Z`, `X`, `H`, `W`, `+`/`-`, Ctrl/Cmd+wheel,
Shift+wheel, Alt+wheel, Cmd+Option+drag). Under §2.1's per-window clocks a key
pressed while the analyser's window has focus reaches **that** peer, so without
an application-level command target installed on every window the host creates,
spacebar stops the transport in one window and does nothing in the other.

`ApplicationCommandManager` / `KeyPressMappingSet` appear nowhere in `docs/`.
The key map's home is settled here rather than left open: **`src/adi/settings/`
already carries a `shortcuts.*` page** (`registry.cpp:506`), and ADR-0047 §1
says the map is app-scoped and needs a home outside the `.adi`. That is the
home. Named as a decision in ADR-0180 so the director can reject it.

*Exposes:* whether the frame really takes one snapshot reference that every
component shares — a mixer and a timeline rendering different snapshots in one
frame is ADR-0050 d3's failure, and it only appears once two components read.

### Step 7.3 — `DeviceChainStrip` + the DAW-drawn panel

The first consumer of **ADR-0198's `panel::Record`** and the first place a
control reads the *parameter feed* rather than the model (ADR-0181 d5). Calls
`panel::resolve` for Live's count rule — it does not re-derive 64-or-fewer —
and `panel::activeAlgorithmParams` for the ADI Airwindows suites.

Includes the **strip resizer** (ADR-0184) with the floor of §6.1.

**The feed is a message-thread coalescer, and is NOT any window's frame.**
ADR-0181 d3 says the feed emits *"on the one UI clock (ADR-0050 d1)"* — which
§2.1 has just replaced. Two things force the correction: the control API's
surface client is **in no window at all**, and two windows would otherwise
drain different frames. So the feed coalesces on the message thread over one
shared publication, and each window — and the control API — drains it. Meter
and scope-tap reads are per window over that same publication, or ADR-0050 d3's
"one snapshot per frame" holds inside a window and breaks between them.

*Exposes:* whether `panel::Record` carries what a control actually needs. If a
field is missing, it is missing now, before three call sites write to it.

### Step 7.4 — the floating-window host

Built **once, before anything needs two of them**: the analyser's big window
(ADR-0183 d4), plug-in editors (ADR-0076), and any future undock (ADR-0063 d1).

Three properties that are cheap here and expensive later:
- **Undocking REPARENTS the same component** into a `juce::DocumentWindow`
  (ADR-0063 d1) — never a second instance, which would be two views of one
  thing disagreeing.
- Dock state lives in `ui_view` (ADR-0063 d2), and restoring a rectangle
  **carries monitor identity and clamps to the current display arrangement**
  (SPEC §8.4, a MUST). Restoring `x=3200` on a machine that lost its second
  monitor puts the window offscreen.
- Each window owns its frame clock (§2.1).

*Exposes:* whether the per-window clock decision of §2.1 actually holds, and
whether a component survives being reparented.

### Step 7.5 — the arrangement

`TimelineRuler`, `TrackHeaderList`, `ArrangementCanvas`, and the **playhead as
its own one-pixel component above the canvas, transparent to hit-testing**
(ADR-0050 d2). `UI-ARCHITECTURE.md`'s §2 tree does not list the playhead;
this plan places it, because painting it into the canvas repaints the full
timeline width at 60 Hz and the cost only shows at a hundred tracks.

`ArrangementCanvas` stays **one component** that paints clips itself and keeps
a position-indexed structure for hit-testing (ADR-0044) — which means
hand-written hit-testing, focus and accessibility, and that is the known price.

*Exposes:* whether the dirty-set coalescing is real, since this is the first
step where something moves every frame.

### Step 7.6 — the mixer strip, and closing

`MixerPanel` / `MixerStrip[]` in `TrackOrderModel` order. Then the parity
sessions of §5.

---

## 4. The three seams the analyser needs, designed in from the start

The analyser session (ADR-0183) builds its view **inside this shell**, not
beside it (win's round 7). Three things must therefore exist before it starts,
and all three are cheap now:

1. **A floating-window host** — step 7.4, for its big window (d4).
2. **A place in the device strip** — step 7.3, for the analyser panel.
3. **One repaint clock per window that an OpenGL view attaches to** — §2.1,
   for its OpenGL context (d3) and `VBlankAttachment` (d4).

**The question neither session would hit alone:** *what happens when the
device whose panel is shown is deleted while its floating window is open?*

An earlier draft of this plan proposed "the window closes". **That is not
implementable as written**, and the reason is worth stating because it is a
property of the engine rather than of the UI:

- `Session::refresh` does **not destroy** a deleted device. The row is
  **RETIRED** — left out of every chain and kept alive (`session.hpp:106`,
  `session.cpp:291-304`). `Session::instanceFor()` returns a raw pointer that
  **stays valid forever**, so there is no dangling-pointer crash — and equally
  **no event to close on**. A window holding it shows a live, editable panel
  for a device that is in no chain. Nothing pushes that fact; a frame would
  have to poll `Entry::retired`.
- **Undo brings the device back**, which is the case the engine was
  deliberately built for: *"a row that came BACK — an undone removal — finds
  its instance waiting, state and all"* (`session.cpp:280-287`). A window
  closed on delete has no answer for where it reopens.
- `window_state` is keyed `ref_id = device_id` (`schema.sql:931`) with no
  `kind` for a DAW-drawn device view. If the UI deletes that row on close,
  undo cannot restore the window's geometry.

**So the proposal is: the window stays open and goes INERT** — it shows the
device as retired and refuses edits, exactly as ADR-0011 keeps a missing
plug-in in the chain rather than dropping it, and an undo makes it live again
with its geometry intact. **For the director**, with the alternative (close on
retire, and accept that undo does not restore the window) named beside it.

---

## 5. The parity gate (ADR-0108)

**Step 7 closes as "verified", not "done":** the checklists pass *and* Adi
signs a human side-by-side session against a live Live 12, with the date and
every deviation recorded in `collab/mac.md`. Scripted input is not a substitute
for the signed session.

**The checklist is written from the Live 12 manual chapter BEFORE the feature**
— it is the acceptance test, not a report written afterwards.

**ADR-0129's table is adopted, not duplicated.** It is already marked *first
checklist under ADR-0108* and cites §6.1, §6.2, §6.9, §41, §41.9 and §41.16,
with three dated director-approved deviations. Writing a second navigation
checklist beside it is the failure ADR-0108 exists to prevent.

Chapters to be cited per piece, to be filled in with the director before each
one is built — this plan does **not** invent chapter numbers.

Every difference from Live is a **defect** unless (a) Adi approved it, dated,
in the checklist, or (b) an ADR rejects the Live behaviour — only ADR-0072
(sends) and ADR-0047 (Inspector) are named today.

---

## 6. What this plan does not decide, and must not

Named so they are visible rather than silently resolved:

1. **The device strip's floor is 169 logical pixels, UNVERIFIED.** It is the
   director's figure for Live 12's default Device View; the measurement is with
   him. **The plan carries it as unverified and must not harden it into a
   constant.** It is also a deliberate departure from Live, whose Device View is
   fixed height (ADR-0184 d1–d2, approved and dated 2026-09-27).
2. **Whether `ArrangementCanvas` gets an `OpenGLContext`** — deferred to a
   step-7 profile, and the deferral survives ADR-0183.
3. ~~Where the keyboard map lives~~ — **proposed here** (§7.2): the
   `shortcuts.*` page in `src/adi/settings/`, app-scoped, outside the `.adi`.
   ADR-0047 §1 left it open and ADR-0129's gate cannot be met without it.
4. **The source of `Record::playing`** — ADR-0181 d3 says values that move
   without an op go into lock-free slots; which slot, and who owns it, is not
   built.
5. **Who calls `ParamOps::drain` at run time**, named as not decided by
   ADR-0124.
6. **Live's Delete warning before `device.setPanel`** — ADR-0154 names it
   explicitly as the UI's job and does not decide it: before removing a
   parameter the UI must check automation lanes, clip envelopes and
   MIDI/key/macro mappings.

---

## 7. What is already built that step 7 calls

Step 7 writes **no** new host or engine code. It calls what step 6 landed:

- `panel::resolve` / `panel::search` — Live's count rule and the Parameter List
- `panel::Record`, `panel::finalize`, `panel::activeAlgorithmParams` (ADR-0198)
- `DeviceInstance::paramText`, `ParamShape` — the plug-in's own words
- `Session::automationOverridden` / `reenableAutomation` (ADR-0162)
- `ParamEditCapture`, `ParamOps` — every value edit goes through the capture
  (ADR-0124), and a first edit emits its opener so undo lands on a value

If step 7 finds itself adding host code, that is a signal the record is wrong,
and the right response is to fix the record rather than route around it.
