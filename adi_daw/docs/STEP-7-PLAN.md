# Step 7 — the minimal arrangement UI: the plan

**Status: APPROVED by the director (2026-09-29) with four changes, all applied
here.** Owner: mac. Companion ADR: **ADR-0180**, now `DECIDED (direction)`.

Step 7 is *"the minimal arrangement UI: the first thing you can make a track
in"* (`README.md:156`). This document says **in what order it is built, what it
must not foreclose, what it is checked against, and what it excludes**. It does
not redesign the shell: `UI-ARCHITECTURE.md` §1–§2 already describe the layout
and the component tree, and §8–§11 are *decided* in ADR-0050. This plan amends
that document in **three** named places (§2) and otherwise builds what it says.

**The four changes the director made, and where each landed:**

| # | The change | Where |
|---|---|---|
| 1 | The arrangement moves ahead of the device strip — step 7 is "the first thing you can make a track in", so the track comes first | §3, reordered |
| 2 | Add what "make a track" actually needs, each with its Live-manual checklist written first | §3.0 |
| 3 | Add a UI test strategy: offscreen render and compare, synthesised interaction, GUI built in CI on Windows and Linux too | §5 |
| 4 | **Ruled:** deleting a device closes its floating window; undo restores the device, not the window | §4 |

**Still open, and not assumed:** two things this plan put to the director are
not in his reply, so they are carried as open rather than counted as approved —
the **third** amendment (§2.3) and the **key map's home** (§7.3). His message
says *"Both of your amendments to `UI-ARCHITECTURE.md` are right"* and names
two; §2.3 arrived in the same PR and awaits his word.

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

The director's reordering sharpens it rather than breaking it: the arrangement
is the first thing that moves every frame, so putting it at 7.3 tests the
per-window clock **earlier** than the old order did.

---

## 1. What step 7 is NOT

Fixed here so the plan cannot grow:

| Not in step 7 | Where it lives |
|---|---|
| The Session view, docked or detachable | step 12 (ADR-0101, ADR-0117) |
| The AI agent, at any tier | steps 8–9 |
| Pure Data devices and the patch editor | step 10 (ADR-0145 d8, ADR-0177) |
| ADI Mobile's three modes | step 15 (ADR-0200) — but see §8 |
| Control-surface hardware (Stream Deck etc.) | P2 (ADR-0181 d4) |
| An Inspector | rejected (ADR-0047) |
| Aux sends | rejected (ADR-0072) |
| Comping, VariAudio, Direct Offline Processing | later steps |
| A second shell document | forbidden — revise `UI-ARCHITECTURE.md` |

Step 7 also does **not** close any "Not decided" left open by an ADR. Where one
is in the way, this plan names it (§6) rather than quietly deciding it.

---

## 2. The three amendments this plan makes to `UI-ARCHITECTURE.md`

### 2.1 The repaint clock is per WINDOW, not one on the root — APPROVED

`UI-ARCHITECTURE.md:248-249` says *"a single `juce::VBlankAttachment` on the
root drives the whole shell at display rate."* **ADR-0063 d3 says otherwise,
and it is right**: a window on a second monitor has a different refresh rate,
so each window gets its own frame clock with a per-window coalesced dirty set
over a shared source of truth. ADR-0063 records this as *"the interaction worth
catching now rather than at step 7"* and says explicitly *"this lands in mac's
lane."*

This matters beyond tidiness: the analyser's big floating window (ADR-0183 d4)
and every plug-in editor window (ADR-0076) are on that seam. A dirty set
written as one global set cannot be split per window later without touching
every component that marks dirt.

**The amendment:** one `VBlankAttachment` **per window**; the dirty set is
per-window; the source of truth is shared. An OpenGL view **attaches to its
window's clock and never drives its own repaint** — `setContinuousRepainting`
is the second clock ADR-0183 d4 already considered and refused.

### 2.2 `SnapshotReader` is not `SnapshotPublisher::AudioRead` — APPROVED

`UI-ARCHITECTURE.md:261-263` names a `SnapshotReader` that takes one reference
at the top of each frame (ADR-0050 d3). It does not exist in code, and the
obvious implementation is wrong.

`AudioRead`'s safety argument requires **exactly one reader**, moving only
forward, announcing before it dereferences (`publisher.hpp:38-41, 147-170`).
A second reader storing into `inUse_` breaks epoch reclamation — the retired
graph is freed while the UI holds it. **The UI gets its own read path**, and
the plan states this before anyone writes the obvious three lines.

**Newly measured, and it raises the stakes:** `ProjectPublisher` — the alias at
`engine/snapshot.hpp:114` — has **zero references anywhere in the repository**,
tests included. Nothing has ever published an `engine::Snapshot`. `SnapshotBuilder`
gained its first production caller only on 2026-09-29 (`engine/gain_stage.cpp:93`,
via ADR-0195's Auto Gain Stage). So step 7.1 is not wiring an established
mechanism — it is the **first** user of the publish side, and the first place
its epoch discipline is exercised at all.

### 2.3 `MainSplit` is a panel→slot map, not a `StretchableLayoutManager` — NOT YET ACKNOWLEDGED

> **For the director.** This amendment was in the reviewed PR but his approval
> names two amendments, not three. It is carried as open. Nothing in §3 depends
> on it before 7.2, so a ruling any time before 7.2 starts costs nothing.

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
before it** if there is one. Only the first needs no window at all.

### 3.0 What "make a track" needs — the director's second change

Step 7's own name is the acceptance test: *the first thing you can make a track
in*. That sentence has five requirements the previous draft did not list, and
three of them are **not** already in the engine. Measured 2026-09-29:

| Capability | Engine today | Step | What step 7 must add |
|---|---|---|---|
| **New / Open / Save** a project | **PARTIAL** | 7.2 | `Store::create` and `Store::open` are solid (`store.cpp:218`, `:272`); save is continuous and per-edit atomic, and `Store::close()` is the one-file rule (`store.cpp:370-399`). But **`Store::create` never inserts the `project` row**, so `project.setName` silently updates nothing on a brand-new file (`ops_catalog.cpp:309-315`) and the snapshot falls back to 48 kHz (`snapshot.cpp:130-134`). **Save As does not exist in any form.** Both are engine gaps step 7 must raise, not route around. |
| **Undo / redo**, menu and keys | **EXISTS** | 7.2 | `adi::History::undo()/redo()`, and the menu queries are already there: `canUndo`/`canRedo`/`nextUndo`/`nextRedo` return a `Step` whose `label` is commented *"what the undo menu shows"* (`history.hpp:39-52`). Only the menu item and the key binding are missing. |
| **Add a track** | **EXISTS** | 7.3 | The `track.create` op, which also inserts the mixer strip in the same op so "a track exists" and "it can be mixed" are never one undo apart (`ops_catalog.cpp:344-350`). Fourteen track kinds (`schema.sql:218-225`). |
| **Drop audio onto it** | **PARTIAL — the real gap** | 7.3 | `media_files` is a real content-addressed table and `importMedia` dedupes by BLAKE3 (`media/media_ops.cpp:183-205`); the decoder handles RIFF/AIFF/FLAC/MP3/Ogg; `ClipPlayback` plays `audio_clips` rows. But **no op in the 73-op catalogue writes an `audio_clips` row** — every such row in the tree is raw SQL in a test or in `tools/make_demo_project.py:104`. So there is **no undoable path from "a file and a track" to "a clip that plays"**, and `importMedia` leaves `sample_rate`, `channels`, `frames` and `bit_depth` NULL. This is an engine gap for win, and it blocks 7.3's headline gesture. |
| **Choose the audio device** | **PARTIAL** | 7.2 | `juce::AudioDeviceManager` appears only in `adi_play` and `adi_audio_probe`, driven by CLI flags. The settings registry already defines `audio.driverType` / `inputDevice` / `outputDevice` / `sampleRate` / `bufferSize`, and `AppSettings` persists them atomically — but **nothing reads those keys into an `AudioDeviceSetup`**. Step 7 writes that binding and the chooser. ADR-0052 d4 applies to plug-in formats, not to this. |

**Each of the five gets its Live 12 manual checklist written from the manual
BEFORE it is built** (ADR-0108), like every other piece. This plan invents no
chapter numbers; they are filled in with the director per piece.

**Two of these are engine work, not UI work**, and they go to win rather than
being absorbed into step 7: the missing `project` row on create, and the
missing `audio_clips` op. Step 7 must not paper over either with raw SQL — §7's
rule is that adding host or engine code from inside step 7 is the signal that
the record is wrong.

### Step 7.1 — the seams, headless

`SnapshotReader` (UI-side, per §2.2) and `OpSubmitter`. No JUCE, no window.
Tested against a hand-built `Snapshot`, which `UI-ARCHITECTURE.md:94-98` says
is the point of the no-committed-state rule.

Because nothing has ever published an `engine::Snapshot` (§2.2), this step also
stands up the publish side for the first time and tests its epoch discipline
directly, rather than inheriting a mechanism in use.

*Exposes:* whether a component can be written that owns no project state.

### Step 7.2 — `AdiRootComponent` + `TransportBar`, one window

The smallest thing that proves a window reads a snapshot and submits an op.
Brings in `juce_gui_basics`, the per-window `VBlankAttachment`, the dirty-set
drain, and `ui_view` state (SPEC §8.4).

Carries three of §3.0's five: **New/Open/Save**, **undo/redo in the menu and on
keys**, and **the audio-device chooser**. The undo half is a thin binding —
`History` already returns the label the menu shows.

Also builds the **command layer**, which is not optional and was nearly
missed: every row of ADR-0129's checklist — the gate step 7 is measured by —
is a keyboard gesture (`Z`, `X`, `H`, `W`, `+`/`-`, Ctrl/Cmd+wheel,
Shift+wheel, Alt+wheel, Cmd+Option+drag). Under §2.1's per-window clocks a key
pressed while the analyser's window has focus reaches **that** peer, so without
an application-level command target installed on every window the host creates,
spacebar stops the transport in one window and does nothing in the other.

`ApplicationCommandManager` / `KeyPressMappingSet` appear nowhere in `docs/`.
The key map's home is **proposed, not decided** (§7.3).

*Exposes:* whether the frame really takes one snapshot reference that every
component shares — a mixer and a timeline rendering different snapshots in one
frame is ADR-0050 d3's failure, and it only appears once two components read.

### Step 7.3 — the arrangement

**Moved ahead of the device strip by the director.** Step 7 is the first thing
you can make a track in, so the track comes before the device.

`TimelineRuler`, `TrackHeaderList`, `ArrangementCanvas`, and the **playhead as
its own one-pixel component above the canvas, transparent to hit-testing**
(ADR-0050 d2). `UI-ARCHITECTURE.md`'s §2 tree does not list the playhead;
this plan places it, because painting it into the canvas repaints the full
timeline width at 60 Hz and the cost only shows at a hundred tracks.

`ArrangementCanvas` stays **one component** that paints clips itself and keeps
a position-indexed structure for hit-testing (ADR-0044) — which means
hand-written hit-testing, focus and accessibility, and that is the known price.

Carries the other two of §3.0: **adding a track** and **dropping audio onto
it**. The second is blocked on the missing `audio_clips` op and must not be
worked around in the UI.

**The frame drain, stated here because this is the first thing that moves.**
ADR-0181 d3 says the parameter feed emits *"on the one UI clock (ADR-0050
d1)"* — which §2.1 has just replaced. Two things force the correction: the
control API's surface client is **in no window at all**, and two windows would
otherwise drain different frames. So the feed coalesces on the message thread
over one shared publication, and each window — and the control API — drains it.
Meter and scope-tap reads are per window over that same publication, or
ADR-0050 d3's "one snapshot per frame" holds inside a window and breaks between
them. 7.4 is the first *parameter* consumer of that feed; 7.3 is the first
consumer of the drain itself.

*Exposes:* whether the per-window clock and the dirty-set coalescing are real —
this is the earliest step at which they can be tested at all, which is why the
director moved it here.

### Step 7.4 — `DeviceChainStrip` + the DAW-drawn panel

The first consumer of **ADR-0198's `panel::Record`** and the first place a
control reads the *parameter feed* (step 7.3) rather than the model (ADR-0181 d5).
Calls `panel::resolve` for Live's count rule — it does not re-derive
64-or-fewer — and `panel::activeAlgorithmParams` for the ADI Airwindows suites.

Includes the **strip resizer** (ADR-0184) with the floor of §7.1.

*Exposes:* whether `panel::Record` carries what a control actually needs. If a
field is missing, it is missing now, before three call sites write to it.

### Step 7.5 — the floating-window host

Built **once, before anything needs two of them**: the analyser's big window
(ADR-0183 d4), plug-in editors (ADR-0076), and any future undock (ADR-0063 d1).

Three properties that are cheap here and expensive later:
- **Undocking REPARENTS the same component** into a `juce::DocumentWindow`
  (ADR-0063 d1) — never a second instance, which would be two views of one
  thing disagreeing. **But ADR-0116 d1/d4 also requires a second view of one
  model, both alive at once** (the analyser's docked panel *and* its big
  window). The host must express both shapes; a reparent-only host cannot.
- Dock state lives in `ui_view` (ADR-0063 d2), and restoring a rectangle
  **carries monitor identity and clamps to the current display arrangement**
  (SPEC §8.4, a MUST). Restoring `x=3200` on a machine that lost its second
  monitor puts the window offscreen.
- Each window owns its frame clock (§2.1).

*Exposes:* whether a component survives being reparented, and whether the
per-window clock decision of §2.1 holds with two real windows rather than one.

### Step 7.6 — the mixer strip, and closing

`MixerPanel` / `MixerStrip[]` in `TrackOrderModel` order. Then the parity
sessions of §5.

---

## 4. The analyser's three seams, and what happens on delete

The analyser session (ADR-0183) builds its view **inside this shell**, not
beside it. Three things must therefore be **designed in from the start** —
their interfaces are fixed in this plan and in ADR-0180 — while the **building**
of them waits for 7.4 and 7.5 (the director's first change):

1. **A floating-window host** — built at step 7.5, for its big window (d4).
2. **A place in the device strip** — built at step 7.4, for the analyser panel.
3. **One repaint clock per window that an OpenGL view attaches to** — §2.1,
   available from 7.2, for its OpenGL context (d3) and `VBlankAttachment` (d4).

### 4.1 The device is deleted while its floating window is open — RULED

**The director's ruling (2026-09-29): the window closes. Undo restores the
device, not the window.**

An earlier draft of this plan proposed the opposite ("the window stays open and
goes inert") and argued the close was *not implementable*. **The ruling stands,
and the argument was wrong** — not about the engine, but about what follows
from it. The engine facts are unchanged and worth keeping, because they decide
*how* the close is implemented:

- `Session::refresh` does **not destroy** a deleted device. The row is
  **RETIRED** — left out of every chain and kept alive (`session.hpp:108`,
  `session.cpp:291-305`), with `trackId` zeroed (`session.hpp:105`).
  `Session::instanceFor()` returns a pointer that **stays valid forever** and
  is not guarded by `retired` at all (`session.cpp:60-73`), so a stale window
  would show a live, editable panel for a device in no chain.
- **Undo brings the device back**: *"a row that came BACK — an undone removal —
  finds its instance waiting, state and all"* (`session.cpp:280-287`).

**So the close is a poll, not a callback, and the plan says so plainly.** There
is no push event; the host holds a `deviceId` and reads
`Session::entryFor(deviceId)->retired` (`session.hpp:226`, `session.cpp:60-63`)
at the top of its frame. `false → true` closes the window. `true → false` is an
undo and, per the ruling, **does nothing** — the device returns, the window
does not.

**Why a poll and not "check after the op we submitted":** `Session::refresh` is
no longer UI-initiated only. It gained a production caller on 2026-09-29 —
`Session::autoGainStage` calls it at `engine/gain_stage.cpp:171` (ADR-0195).
A retire can therefore fire inside an engine call the frame loop did not
schedule, and a host that only looked after its own submissions would miss it.
There is precedent for exactly this poll in shipped code: `ParamOps::attachSession`
skips entries on `e.placeholder || e.retired` (`param_ops.cpp:154`) and its
header documents the contract as *"Call again after `Session::refresh`"*
(`param_ops.hpp:116-118`).

**One consequence of the ruling, decided here as the cheapest reading:** the
window's `window_state` row (`schema.sql:931`, keyed `ref_id = device_id`) is
**left in place** when the window closes. The ruling says undo does not reopen
the window; it does not say the geometry must be forgotten. Keeping the row
costs nothing and means that if the user opens that device's window again after
an undo, it lands where it was. `window_state` has no reader or writer in
shipped code today, so step 7 is its first.

---

## 5. How the UI is tested — the director's third change

Three mechanisms, and CI on three platforms. All of it is greenfield: there is
**no** UI test target, no offscreen-render harness, no image comparison and no
synthesised-event driver in `tests/` today.

### 5.1 Components rendered offscreen and compared

`juce::Component::createComponentSnapshot` (vendored JUCE **9.0.2**,
`juce_Component.h:1163`, implemented `juce_Component.cpp:2115-2142`) is four
lines: allocate an `Image`, wrap it in a `Graphics`, call `paintEntireComponent`.
**It reads no peer, no display and no run loop.** Comparison uses
`juce::Image::BitmapData` (`juce_Image.h:327`, `getPixelColour` at `:364`), and
`juce::PNGImageFormat::writeImageToStream` (`juce_ImageFileFormat.h:171`) writes
a golden image to a file or to memory.

**Two traps, both named before anyone trips them:**

- **Pass `SoftwareImageType{}` explicitly.** The default is `NativeImageType{}`,
  which substitutes ARGB for RGB on macOS (`juce_CoreGraphicsContext_mac.mm:297-300`)
  but **not** on Linux (`juce_Image.cpp:644-648`). Comparing snapshot bytes
  across platforms without it produces differences that have nothing to do with
  the UI.
- **Text is the classic golden-image flake.** `juce_graphics` declares
  `linuxPackages: freetype2 fontconfig` (`juce_graphics.h:57`); a container with
  different fonts renders text differently. Golden images of text-bearing
  components are not attempted until one experiment settles it.

### 5.2 Interaction driven by synthesised events

Honest about what this buys: **JUCE ships no mock peer.** The real routing
functions `Component::internalMouseDown`/`internalMouseUp` are **private** and
friended to `ComponentPeer` alone (`juce_Component.h:2719-2720`, `private:` at
`:2648`). A test can call the public `mouseDown`/`mouseUp` virtuals
(`juce_MouseListener.h:110`, `:137`), but that **bypasses hit-testing, mouse
capture, click counting and registered listeners** — it tests the handler, not
the interaction.

So the strategy is in two tiers, and the plan does not pretend the cheap tier is
the expensive one:

- **Keyboard: real.** `Component::keyPressed` is public (`juce_Component.h:1928`),
  so ADR-0129's gate — which is entirely keyboard gestures — is testable
  directly. That is the gate step 7 is measured by, and it is the tier that
  matters most.
- **Mouse: a fake peer, or nothing.** `ComponentPeer`'s constructor is public and
  only registers itself in `Desktop`'s list (`juce_ComponentPeer.cpp:41-49`), and
  `Component::createNewPeer` is a protected virtual (`juce_Component.h:2761`)
  built for exactly this override. Cost: **29 pure virtuals to stub**, and it has
  not been compiled. Whether that cost is paid is decided at 7.3, when the first
  real hit-testing exists to justify it — not assumed now.

JUCE 9.0.2 has a named concession for this whole approach: the assertion macro
is literally `JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED_OR_OFFSCREEN`, which passes
when `getPeer() == nullptr` (`juce_Component.cpp:35-38`). A peerless component
tree is a supported shape, not a trick.

### 5.3 The GUI built in CI on Windows and Linux as well as macOS

Today **one** job builds with JUCE — `juce` at `ci.yml:653`, a two-leg matrix of
`macos-latest` and `windows-latest`. Linux is absent **on purpose**
(`ci.yml:648-651`).

**A correction to the framing, in the director's favour:** the GUI modules are
*already compiled* by that job on both legs, because `juce_audio_processors`
depends on `juce_gui_extra` → `juce_gui_basics`. So "add Linux" and "add a GUI
build" are the **same** apt problem, and adding the Linux leg is smaller than it
looks.

- **The apt list** is the vendored JUCE's own (`docs/Linux Dependencies.md:66-75`).
  `libcurl4-openssl-dev`, `libwebkit2gtk-4.1-dev` and `ladspa-sdk` are already
  excluded by this project's `JUCE_USE_CURL=0` / `JUCE_WEB_BROWSER=0` /
  `JUCE_PLUGINHOST_LADSPA=0`. **`libjack-jackd2-dev` is NOT needed** — JUCE
  defaults `JUCE_JACK` to 0 itself (`juce_audio_devices.h:175-177`).
  `libasound2-dev` **is** needed: `JUCE_ALSA` defaults to 1 (`:168-170`).
- **xvfb is not needed for the offscreen tier, and the code settles it.** On
  Linux both `NativeImageType` and `SoftwareImageType` produce `SoftwarePixelData`
  — a `HeapBlock` rasterised by `LowLevelGraphicsSoftwareRenderer`
  (`juce_Image.cpp:625-648`, `:587-591`). X11 is `dlopen`'d, never linked
  (`juce_XSymbols_linux.h:645`), `juce_gui_basics` declares no `linuxPackages`,
  and every X entry point degrades when `XOpenDisplay` fails
  (`juce_XWindowSystem_linux.cpp:1584-1618`). **This is a source-level
  conclusion, not an executed one** — nobody has run a `DISPLAY`-less JUCE
  binary here. The cheap experiment, run before the CI leg is written: a ~30-line
  console app that snapshots a component to a PNG under `env -u DISPLAY`. xvfb
  *does* become necessary the moment anything calls `addToDesktop` with the real
  peer factory (`juce_XWindowSystem_linux.cpp:1644-1651`).
- **Windows short build paths.** The director is right about the hazard and it is
  currently survived by luck, not by a workaround: no CI job sets a
  `working-directory` or any short-path trick, and the build runs in
  `D:\a\Adi\Adi` with a longest observed path of 91 characters — roughly 169 to
  spare under `MAX_PATH`. The only `MAX_PATH` warnings in the repo are in
  *local* developer scripts (`tools/build-juce.bat:13-14`,
  `tools/build-external-plugin.bat:17-18`). GUI targets add deeper generated
  paths, so step 7 adds the guard rather than continuing to rely on GitHub's
  layout.

### 5.4 Where a GUI test can and cannot live

**`ADI_WITH_JUCE` defaults OFF** (`CMakeLists.txt:908`), and
`CMakeLists.txt:900-905` states the invariant outright: the JUCE-off build must
stay green on every ABI. The harness — `tools/test_all.sh`, which prints
*"N checks across M suites"* at `:133` and globs `$BUILD/*tests*` at `:35` —
**does not link JUCE today, deliberately.**

So a GUI test binary is the first of its kind here. It **lives behind
`ADI_WITH_JUCE`** and **cannot join the default required matrix** without
breaking that invariant. It reaches for `juce_audio_processors_headless` so the
GUI binary never pulls in audio devices. Two mechanical consequences: a new
suite is one file plus three CMake lines and its target name must contain
`tests`; and `README.md:9`'s headline count is enforced by
`tools/test_all.sh:122-129`, so every new suite fails the run until that line is
updated by hand.

There is already one JUCE-linked suite against this vendored copy —
`adi_rmsc_tests` (`plugins/rmsc/CMakeLists.txt:46-48`) — which escapes the
harness only because it builds into a separate tree. It is the precedent to
follow, and a reminder that the glob is not a guarantee.

---

## 6. The parity gate (ADR-0108)

**Step 7 closes as "verified", not "done":** the checklists pass *and* Adi
signs a human side-by-side session against a live Live 12, with the date and
every deviation recorded in `collab/mac.md`. Scripted input is not a substitute
for the signed session — §5 tests that the code does what it was built to do,
not that what it was built to do is right.

**The checklist is written from the Live 12 manual chapter BEFORE the feature**
— it is the acceptance test, not a report written afterwards. This now covers
all five of §3.0's capabilities as well as the components.

**ADR-0129's table is adopted, not duplicated.** It is already marked *first
checklist under ADR-0108* and cites the **manual's** §6.1, §6.2, §6.9, §41,
§41.9 and §41.16,
with three dated director-approved deviations. Writing a second navigation
checklist beside it is the failure ADR-0108 exists to prevent.

Remaining chapters are filled in with the director before each piece is built —
this plan does **not** invent chapter numbers.

**The rows are requested, not written from scratch** (win, 2026-09-29). The
A2000 is turning the Live 12 manual into checklist rows — behaviour, page, and a
verbatim quote — and win sends them checked. **Before each piece is built, ask
win for that chapter's rows.** Writing a checklist here that the A2000 has
already produced is the same duplication ADR-0108 forbids, arriving from a new
direction: two checklists for one behaviour, and the second one unchecked.

### The GUI reference maps — ask, do not guess

win holds summaries the drone produced of four codebases, on the Windows box:
**helio-sequencer** (a JUCE app, the closest cousin this project has), **Ardour's
editor**, **zrythm's GUI**, and **JUCE's own `gui_basics` / `graphics` /
`opengl`**. win answers a specific question from them, checked against the
source.

This is the intended first move for the questions step 7 already knows it has,
rather than deriving them alone:

| Step | The question to ask |
|---|---|
| 7.3 | How does helio draw its **playhead** — its own component, or painted into the canvas? (ADR-0050 d2 says its own; corroboration is cheap.) |
| 7.3 | How does helio **hit-test clips** inside one big component? (ADR-0044's known price is hand-written hit-testing.) |
| 7.2, 7.3 | How does it **coalesce repaints**, and does anything there keep a per-window clock? (ADR-0180 d1.) |
| 7.5 | How do Ardour and zrythm handle a **second view of one model, both alive at once**? (ADR-0116 d1/d4, the shape a reparent-only host cannot express.) |
| 7.4 | How does any of them lay out a **generic parameter panel** for an arbitrary plug-in? (ADR-0198's `Record`.) |

A reference map is corroboration, not authority: where one disagrees with an
ADR, the ADR wins and the disagreement is recorded rather than quietly followed.

Every difference from Live is a **defect** unless (a) Adi approved it, dated,
in the checklist, or (b) an ADR rejects the Live behaviour — only ADR-0072
(sends) and ADR-0047 (Inspector) are named today.

---

## 7. What this plan does not decide, and must not

Named so they are visible rather than silently resolved:

1. **The device strip's floor is 169 logical pixels, UNVERIFIED.** It is the
   director's figure for Live 12's default Device View; the measurement is with
   him and his reply of 2026-09-29 confirms it stays unverified. **The plan
   carries it as unverified and must not harden it into a constant** — and
   ADR-0200 d5 now requires the same thing for a second reason (§8). It is also
   a deliberate departure from Live, whose Device View is fixed height
   (ADR-0184 d1–d2, approved and dated 2026-09-27).
2. **Whether `ArrangementCanvas` gets an `OpenGLContext`** — deferred to a
   step-7 profile, and the deferral survives ADR-0183.
3. **Where the keyboard map lives — PROPOSED, not ruled.** `src/adi/settings/`
   already carries a `shortcuts.*` page (`registry.cpp:506`), and ADR-0047 §1
   says the map is app-scoped and needs a home outside the `.adi`. That is the
   proposal. **The director's reply does not address it**, so it stays open —
   and ADR-0129's gate cannot be fully met while it is, because every row of
   that gate is a keyboard gesture. Needed before 7.2 completes.
4. **The source of `Record::playing`** — ADR-0181 d3 says values that move
   without an op go into lock-free slots; which slot, and who owns it, is not
   built.
5. **Who calls `ParamOps::drain` at run time**, named as not decided by
   ADR-0124.
6. **Live's Delete warning before `device.setPanel`** — ADR-0154 names it
   explicitly as the UI's job and does not decide it: before removing a
   parameter the UI must check automation lanes, clip envelopes and
   MIDI/key/macro mappings.
7. **Whether the fake `ComponentPeer` is written** (§5.2) — decided at 7.3,
   when there is real hit-testing to justify its 29 stubs.

---

## 8. What ADI Mobile requires of step 7 (ADR-0200 d5)

ADR-0200 landed on main on 2026-09-29, after this plan was first written. Its
d5 assigns requirements to **"step 7, mac" by name**. They are requirements, not
features, and they cost nothing if taken from the first line:

**Input:**
- every action reachable by mouse and keyboard **also** has a way that needs no
  hover, no right button and no modifier key — a long press, a menu, or an
  on-screen control;
- gestures stay **pure functions from input to parameter change**, as the
  Dynamic EQ's already are (ADR-0195 d2), so a touch mapping is a new table
  rather than a rewrite;
- **nothing is shown only on hover.**

**Layout:**
- components take their size from their parent and **never assume a desktop
  minimum**;
- the device strip's floor is **a desktop default, not a constant in the
  component** — which is §7.1 arriving independently, from a different ADR.

**This creates one real piece of work, and it should be said plainly:**
ADR-0129's checklist — step 7's gate — is **entirely modifier gestures**
(Ctrl/Cmd+wheel, Shift+wheel, Alt+wheel, Cmd+Option+drag). ADR-0200 does not ask
for those to be removed; it requires that each *also* has a no-modifier path. So
every row of the ADR-0129 gate acquires a second row: the same action, reachable
without a modifier. That table is written with the director alongside the
manual checklists, at 7.2, and the command layer of §3's 7.2 is where it lands.

---

## 9. What is already built that step 7 calls

Step 7 writes **no** new host or engine code. It calls what step 6 landed:

- `panel::resolve` / `panel::search` — Live's count rule and the Parameter List
- `panel::Record`, `panel::finalize`, `panel::activeAlgorithmParams` (ADR-0198)
- `DeviceInstance::paramText`, `ParamShape` — the plug-in's own words
- `Session::automationOverridden` / `reenableAutomation` (ADR-0162)
- `ParamEditCapture`, `ParamOps` — every value edit goes through the capture
  (ADR-0124), and a first edit emits its opener so undo lands on a value
- `adi::History` — undo, redo, and the label the menu shows (`history.hpp:39-52`)

If step 7 finds itself adding host code, that is a signal the record is wrong,
and the right response is to fix the record rather than route around it. §3.0
names the two engine gaps that this rule sends back to win rather than absorbing.
