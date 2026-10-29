# UI architecture — the shell, and how the graph carries a hybrid track

**Status:** design. Nothing built. The UI is roadmap step 7; this exists so
step 6 does not make it impossible.

`win` wrote §1–§7 when the director asked for a component tree, before the
lanes were split; the tree and its three rules are his and they hold. §8–§11
are mine and they are all the same omission: **the tree says what the shell
*is* and nothing said what it *does per frame*.** That is where a JUCE DAW UI
actually fails — not in its hierarchy.

**Owner: `mac`.** The director assigned the component hierarchy to mac after
this draft was written. It stands as a **starting proposal, not a decision** —
revise or replace it rather than writing a second one beside it. What is
*decided* lives in the ADRs below and does not move; everything here about
component shape is a suggestion with reasons attached, and the reasons are the
part worth keeping or arguing with.

Decisions live in ADR-0044 (grouping), ADR-0045 (hybrid tracks), ADR-0046
(modulation) and ADR-0047 (the shell, the view states, and the two rejections).
This document is the shape those decisions imply, not a second place they are
decided.

---

## 1. The layout

Ableton-shaped, fixed: browser left, timeline top, devices bottom, mixer right.

```
┌──────────────────────────────────────────────────────────────────────┐
│ TransportBar            play · tempo · sig · view state · CPU        │
├──────────┬─────────────────────────────────────────┬─────────────────┤
│          │ TimelineRuler                           │                 │
│ Browser  ├─────────────────────────────────────────┤  MixerPanel     │
│ Panel    │ TrackHeaderList │ ArrangementCanvas     │                 │
│          │                 │                       │  (strips, in    │
│ (collap- │                 │   ← one component     │   TrackOrder-   │
│  sible)  │                 │                       │   Model order)  │
│          ├─────────────────┴───────────────────────┤                 │
│          │ DetailEditor        [collapsible]       │                 │
├──────────┴─────────────────────────────────────────┴─────────────────┤
│ DeviceChainStrip   follows selection, full width; drag taller ↕      │
└──────────────────────────────────────────────────────────────────────┘
```

## 2. The component tree

```
juce::DocumentWindow
└── AdiRootComponent
    │     owns: SnapshotReader (ADR-0019), OpSubmitter, TrackOrderModel,
    │           ViewState. Owns no project data of its own.
    ├── TransportBar
    ├── MainSplit                       panel->slot map, 3 slots (ADR-0080)
    │   ├── BrowserPanel                collapsible
    │   ├── CentreSplit                 vertical, resizable
    │   │   ├── ArrangementView
    │   │   │   ├── TimelineRuler       bars/beats from the signature map
    │   │   │   ├── TrackHeaderList     one small Component per track
    │   │   │   └── ArrangementCanvas   ONE Component (see §3)
    │   │   └── DetailEditor            collapsible
    │   │       └── LayerStack          opt-in superimposition (ADR-0047)
    │   └── MixerPanel
    │       └── MixerStrip[]            one per track, in TrackOrderModel order
    ├── DockResizer                     the strip's top edge: drag it taller (ADR-0184)
    └── DeviceChainStrip
        └── DeviceView[]                one per device on the selected track
```

### `MainSplit` is a panel→slot map, not a `StretchableLayoutManager` (ADR-0080)

*(Amended by ADR-0180 §2.3, approved 2026-09-29. The tree above said
`juce::StretchableLayoutManager, 3 cols`; ADR-0080 rules that out.)*

A `StretchableLayoutManager` is slot-indexed and holds the widths itself, which
is exactly the `leftWidth`/`rightWidth` shape ADR-0080 d2 names as the bug. What
the layout is instead:

- an ordered **panel→slot** mapping, never a `bool swapped` (d1);
- **a width belongs to the PANEL and follows it across a side swap** (d2),
  stored in `ui_view` beside ADR-0063's dock state;
- minimum widths are **per panel**; a swap that does not fit clamps, takes the
  remainder from the arrangement, and **refuses with a message** rather than
  silently (d3);
- a swap **reorders, never reconstructs** (d4) — which is ADR-0063 d1's rule
  arriving for a second reason;
- FlexBox/Grid at the top with **splitters owning the widths** (d5), because
  JUCE flex has no draggable divider.

The map and its refusal are `adi::ui::PanelLayout`
(`src/adi/ui/panel_layout.hpp`), which is pure logic and tested headless: the
arithmetic of "does this swap fit" is where a layout is actually wrong, and it
needs no window to be checked.

*(The director, 2026-10-28.)* Swapped, the track names lead: names, the track
controls, the strip, then the lanes, as Bitwig and Cubase have them, so a
track's name stays beside its controls. The arrangement leaves a gap between its
names and its lanes and the root puts the track controls in it
(`ArrangementView::setNamesFirst`). The swap is shown, not hidden (ADR-0203 d2):
small arrows under the two view switches at the head of Live's strip down the
arrangement's outer edge, "Flip sides" on hovering, lit while flipped; the View
menu's Flip Sides does the same and is its keyboard path (the arrows take no
focus). Flipped, the strip moves to the window's left edge so the track controls
meet the lanes: the strip is its own component, `SideStrip`, beside the track
controls on Live's sides (the director, 28 October 2026).

### The device strip's height (ADR-0184)

Live 12 lets its Clip View grow (§10a) and keeps its Device View at one
height. ADI lets the device strip grow too: a dated departure from Live under
ADR-0108, approved by the director on 2026-09-27.

- **The floor is Live's default device area** (ADR-0188 d6), in logical
  pixels at 100 % zoom, scaled with the UI zoom and the display's scale
  factor. The director's figure was 169 for the device body. Measured on Live
  11.2.7 on 2026-10-27, the whole Device View panel is 196 with its 4-pixel
  edges, and the body under the title bars is about 170. ADI's strip is that
  whole panel, so its floor is 196 (`collab/window/look-live11.md`), to be
  read again on Live 12. The strip can only be made bigger.
- **The ceiling** leaves the transport, the ruler and one track row visible.
- **The height is view state,** kept per project in `ui_view` (SPEC §8.4)
  like the other panel sizes.
- **What fills the height:**
  - a device view that declares it scales (a Pd device's second view such as
    the analyser, the scope, meters) is redrawn at the new size, never
    stretched as a bitmap;
  - the DAW-drawn panel (ADR-0150) keeps its natural height and top-aligns;
  - plug-in windows float, as before (ADR-0076).

### Three rules the tree exists to enforce

**No component owns COMMITTED project state.** Every one of them reads the
current `Snapshot` and emits ops. None holds a `SQLite::Database&`, a row id it looked
up itself, or a cached copy of anything. This is ADR-0010 at the UI layer, and
it is what makes the whole shell testable against a hand-built `Snapshot` — the
same trick that makes `buildTree` testable without a database.

*One refinement, because the absolute form is not implementable.* A control
mid-interaction does hold state the snapshot cannot: the characters in a text
field before commit, a fader's position during a drag, an in-flight IME
composition. Forcing those through ops would emit an op per keystroke, fill the
undo tree with typing, and break IME on every platform that has one. The rule
is about **committed** state: transient interaction state is owned by the
control, lives until commit or cancel, and never survives either.

**View state lives in `ui_view`, not in members.** The three-state toggle, panel
widths, collapsed groups, zoom. That puts it in Layer 3, so it survives a reload,
is per project, and is excluded from the text projection — which is what stops a
colleague's zoom level turning up in a `git diff`.

**Selection is session state, and it is read, never inferred.** `DeviceChainStrip`
and `DetailEditor` follow `session_state`, per ADR-0021. Neither of them decides
what is selected, and no op they emit reads it.

*Built (window lane, 2026-10-27):* `adi::ui::SessionState`
(`src/adi/ui/session_state.hpp`) holds the one selection a project has, in
`session_state`'s `selection` row. Each window presents a copy at the top of its
frame, as it presents one snapshot, and writes go back through it, so two windows
never disagree about what is selected. An edit carries the selection as its
transaction's advisory `sel_before`, and undo puts it back.

## 3. `ArrangementCanvas` is one component, and that is the load-bearing decision

Not one `Component` per clip. JUCE's hit-testing, z-order and repaint bookkeeping
are per component, and a project with a few thousand clips makes that the
bottleneck long before the painting does. The canvas paints clips itself and
keeps a position-indexed structure for hit-testing.

The cost is that everything a `Component` gives free must be written: hit-testing,
focus, drag state, and accessibility. That last one is not optional and is
easiest to get wrong here — see §6.

`TrackHeaderList` *is* one component per track, because there are tens of them
rather than thousands and they hold real controls.

## 4. Groups, and the one thing that must not drift

A group is one object (ADR-0044) and it appears in two places: as a collapsible
parent in `TrackHeaderList` / `ArrangementCanvas`, and as a strip in
`MixerPanel`. Both read the same `TrackOrderModel`, which is derived from the
snapshot's track forest — collapse state from `ui_view`, order from
`index_in_parent`.

Two views of one list is how they end up disagreeing about order after an
insertion. One model, two readers, no second ordering anywhere.

Collapsing a group hides its children in the timeline. It does **not** hide them
in the mixer by default; that is what the group/stem focus view state is for.

*Built (window lane, 2026-10-29): Live's group tracks (manual 18.3).* The model is
`adi::ui::TrackOrder` (`src/adi/ui/track_order.hpp`), held by `ArrangementGeometry`
and built again in place for each new snapshot: each group followed by what it
holds, by `index_in_parent` then id, at any depth; Main last, as Live draws it. Every
walk down the tracks reads it through `ArrangementGeometry::rows` (the lanes, the
names, the track controls, the Mixer View, the Overview, the Channel EQ window, the
keys, the Status Bar's number), so no panel orders tracks itself. The snapshot's own
order (`index_in_parent, id`) mixes one group's tracks with another's and is not
used for display.

A group's title shows Live 12's group switch (three lines, the top one out while
open); a track in a group sits past a band in the group's colour for each group it
is in; a group's lane is darker and shows the clips of everything it holds, a thin
row each (the manual's figure of a folded group). The switch, U, and Alt+U (§42.16,
§42.19) fold and unfold; the folds are one value per window in `ui_view`
(`WindowState::foldedGroups`). `ArrangementGeometry::foldedAway` takes what a folded
group holds out of the lanes, the names and the track controls; the Mixer View keeps
it, as this section says. A group the project view leaves out folds nothing, so a
view of a group inside it still shows.

## 5. The three view states

| State | Shows |
|---|---|
| **Global** | every track, fitted vertically |
| **Group / stem focus** | the selected group and its children; everything else hidden |
| **Detailed zoom** | the selected track at edit resolution, detail editor open |

The state is one value in `ui_view`, and every panel derives visibility from it
rather than holding its own flag — otherwise the mixer and the timeline end up
showing different subsets and the user cannot tell which is lying.

*Built (window lane, 2026-10-29): Bitwig's project view, the director's choice.* A
button at the top of the track names opens a menu of the whole project and its
groups as a tree, each in its folder colour; choosing a group shows it, everything
inside it and Main. The view is one value per window in `ui_view`
(`WindowState::viewGroup`). `adi::ui::TrackTree` (headless) answers what a view
shows; `ArrangementGeometry::hidden` holds what it leaves out, and a hidden track's
height is 0, so every walk down the rows (lanes, names, track controls, the Mixer
View's columns, hit tests, the keys) passes over it from the one list. The tree is
built only while a group is chosen or the menu is open, in buffers kept between
frames, so a new generation with the same rows allocates nothing (§8).

*The director's choice, 2026-10-29:* until the snapshot carries a track's parent,
place in its parent, kind and colour (window log, N6), the window reads them from
the project file (`rows::readModel`, as the engine's MIDI player does;
`adi::ui::refreshTrackRows`), wired by the application in `ui_main.cpp`: when the
menu opens, and on each new snapshot whose tracks changed or whose project has a
group (a project without groups reads nothing). It is the one place the window
reads past the snapshot; STEP-7-PLAN §2.2 lists what changes when N6 arrives. If the
engine's way proves better, N6 replaces it: the arrangement's `TrackRows` is the only
thing that changes.

**Open, and it is not a UI question:** where a *keyboard* map lives.
`controller_maps` is project-scoped and right for MIDI and OSC. A keymap is
app-scoped: a user's shortcuts should not change because they opened someone
else's project. It needs a home outside the `.adi` and does not have one yet.

## 6. What the missing Inspector obliges

ADR-0047 rejects a dedicated inspector, so `MixerStrip`, `DeviceChainStrip` and
`DetailEditor` carry every editable property between them. Two consequences to
design for rather than discover:

- **From step 7 until the agent can change values at step 9, those three are the
  only surface.** Anything not reachable there is not reachable.
- **`ArrangementCanvas` needs an accessibility layer of its own.** A single
  custom-painted component exposes nothing to a screen reader by default, and
  "ask the agent" is not a substitute for a focusable, announcing control. JUCE
  gives us `AccessibilityHandler` for exactly this, and clips need virtual
  children.

  *Built (window lane, 2026-10-27):* JUCE gathers accessibility children only from
  child components (`AccessibilityHandler::getChildren` is not virtual), so the
  "virtual children" are `ClipAccessible`s: one per clip on screen, painting
  nothing, transparent to the mouse, recycled as the view scrolls (§10). A clip's
  title ("<track>, clip at bar 3 beat 1, 4.00 beats") is built only when a reader
  asks, so a frame allocates nothing for it. Its press action selects the clip, and
  moving the selection by key posts an announcement of where it landed.

---

## 7. How the graph carries a hybrid track

ADR-0045 removed the audio/MIDI distinction from tracks. The graph change that
makes that cost nothing is small, and it has to be made before anything is
built, because retrofitting it means touching every node.

### Every port is a pair

```
struct Port {
    juce::AudioBuffer<float>& audio;   // sized for maxBlockSize at prepare
    EventList&                events;  // timestamped, sample-accurate
};
```

Not a typed port that is audio *or* events. A typed port is what forces a special
case at every junction, and "hybrid track" would then be a feature. With a pair,
hybrid is the absence of a restriction and there is nothing to implement.

### Every device passes through what it does not consume

| device | consumes | produces | passes through |
|---|---|---|---|
| instrument | events | audio, **added to** what is already on the bus | events |
| audio effect | audio | audio | events, unchanged |
| note effect | events | events | audio, unchanged |

The row that matters is the instrument's. On a track holding an audio clip *and*
a MIDI clip, the clip reader fills both halves of the port, and the instrument
sums into the audio already there. Replacing it would silently mute the audio
clips, and the user would find out at mixdown.

Passing events through an audio effect is what lets an instrument sit after one,
which is the Bitwig behaviour and is free once the port is a pair.

### Node processing, in the order the other ADRs require

```
for each node in topological order:
    if suspended by ADR-0043 (silent in, no events, tail expired)
       or by ADR-0040 (declared optional, UI hidden, provably unobservable):
        flag outputs silent; skip
    else:
        split the block at every event boundary   ← ADR-0042
        for each segment:
            apply modulation for this segment     ← ADR-0046
            process(segment)
```

Three decisions meeting in six lines, and they fit because they were taken
together: the sub-block split ADR-0042 requires for smooth automation at 8192
samples is the same split ADR-0046's modulation needs, and the silence and tail
bookkeeping of ADR-0043 is what decides whether a node runs at all.

### What a group node does

A group is a summing node: it mixes its children's audio, concatenates their
event lists, and runs its own device chain on the result. It is silent when all
its children are silent, so ADR-0043's suspension propagates up a group tree
without any special case for groups.

---

## 8. The frame, which is the part that was missing

Nothing above says *when* anything repaints, and in a JUCE DAW that is the
decision that determines whether the UI is usable. A shell that repaints on
every change is correct and unusable; the arrangement is the largest surface in
the window and the playhead moves 60 times a second across it.

**One clock PER WINDOW, draining coalesced dirt.** *(Amended by ADR-0180 d1,
approved 2026-09-29. This paragraph previously said a single
`juce::VBlankAttachment` on the root drives the whole shell; ADR-0063 d3 says
per-window and it is right, because a window on a second monitor has a
different refresh rate.)*

Each window owns a `juce::VBlankAttachment` and its own coalesced dirty set; the
source of truth is shared. An OpenGL view **attaches to its window's clock and
never drives its own repaint** — `setContinuousRepainting` is the second clock
ADR-0183 d4 considered and refused. Components never call `repaint()`
in response to a model change; they set a dirty bit and the frame drains it.
Two changes to one track between frames cost one repaint, and an op storm —
which ADR-0039's remote actor can produce — costs one repaint per frame rather
than one per op.

**The playhead never dirties the arrangement.** It is its own component, one
pixel wide, above `ArrangementCanvas` and transparent to hit-testing. Moving it
repaints two thin strips. Painting it *into* the canvas is the single most
common way a DAW timeline ends up repainting its whole width at 60 Hz, and the
cost does not appear until someone has a hundred tracks on screen.

**The UI reads the snapshot once per frame, not once per query.** `SnapshotReader`
takes one reference at the top of the frame and every component reads that same
one. Otherwise two panels can render different snapshots in one frame and the
mixer disagrees with the timeline — the §4 failure, arriving through timing
rather than through a second model.

*(Amended by ADR-0180 d2 and ADR-0201, approved 2026-09-29.)* `SnapshotReader`
is **not** `SnapshotPublisher::AudioRead`. `AudioRead`'s reclamation requires
exactly one reader, moving only forward, announcing before it dereferences; a
second reader storing into `inUse_` frees the graph the audio thread is
rendering. The UI's reader is built instead: it holds a
`shared_ptr<const engine::Snapshot>` and never names a publisher
(`src/adi/ui/snapshot_reader.hpp`).

Where that reference comes from is `engine::ProjectView` (ADR-0201): one
message-thread cell, refreshed by whatever commits, whose `current()` a frame
takes ONCE. There is no publisher and no `collect()` on this path, because
**nothing crosses threads** — the audio thread plays from the realised graph and
never reads an `engine::Snapshot`.

Under the per-window clocks above, two windows drain at different moments and
may briefly hold consecutive generations. **Accepted** (director, 2026-09-29):
each window's frame is consistent within itself and the skew is bounded by one
refresh. Anything that crosses windows — a drag from a floating window — **re-reads
`ProjectView::current()` when it lands and never carries a snapshot between
windows.**

**Open:** whether `ArrangementCanvas` wants an `OpenGLContext`. It would help a
large canvas on Windows and is worth measuring rather than assuming; on macOS
CoreGraphics is competitive and the context costs a GL thread and some driver
risk. Decide it with a profile at step 7, not now.

---

## 9. Metering does not go through any of this

The highest-frequency data in the window, and nothing above carries it. Saying
where it goes matters because all three obvious answers are wrong.

**Not an op.** A meter is not a mutation; metering through the op log would fill
the undo tree at audio rate.

**Not the snapshot.** ADR-0019 publishes a snapshot when the *structure*
changes. Republishing at metering rate would make an edit-cost mechanism carry
a per-frame signal and defeat the structural sharing it was built for.

**Not a lock.** It originates on the audio thread.

It is a **lock-free scalar per metered point**, written by the audio thread and
read by the frame:

```
struct MeterTap { std::atomic<float> peak, rms; };   // one per track and bus
```

Relaxed ordering is sufficient: a meter that is one frame stale is invisible,
and the audio thread must never wait to publish one. This is the only path in
the UI where the audio thread writes something the UI reads, which is why it is
worth naming rather than leaving to whoever builds the mixer.

---

## 10. What is realised, and what is only drawn

§3 settles the canvas: one component, because a few thousand clips is where
per-component bookkeeping stops working. The same argument reaches two places
§2 leaves as arrays, and the mixer is the sharper case.

`MixerStrip[]` is one component per track, each with a fader, sends, and a
meter that updates every frame. At the density this DAW is being built for —
the director's workflow is dense arrangements — a few hundred strips is a few
hundred repainting components, and only a dozen are on screen.

**Realise what is visible; draw the rest not at all.** `MixerPanel` and
`TrackHeaderList` keep components for the visible span plus a small margin and
recycle them on scroll. `TrackOrderModel` already knows the full order, so
virtualisation is a windowing concern and not a second model — the §4 rule
survives.

This costs nothing today and cannot be retrofitted cheaply: a strip that has
assumed it lives forever will hold state that a recycled one loses.

---

## 11. Two things the other ADRs oblige the UI to carry

**Modulation needs a control base, not a widget (ADR-0046).** Any parameter can
be a modulation target, so every parameter control — fader, knob, the
device-chain controls — must share a base that knows its parameter identity,
can render a modulation depth around its current value, and can accept a source
dropped onto it. Retrofitting that into controls written as plain sliders means
touching all of them.

This **depends on the device/parameter contract**, which ADR-0035 and ADR-0040
still leave open: "its parameter identity" is exactly what that contract
decides. The control base cannot be written before it, and that is the gate.

**A hybrid port means a device view cannot be typed by its track (ADR-0045).**
Every port carries audio *and* events, so `DeviceView` renders what the device
consumes and produces rather than what the track "is" — there is no audio track
to ask. §7's instrument row is also a UI obligation: an instrument **sums into**
audio already on the bus, and a chain that does not show that is a surprise the
user meets at mixdown.

**And an answer to §5's open question.** A keymap does not belong in `.adi`, for
the reason §5 gives: shortcuts must not change because you opened someone else's
project. It belongs in an app-scoped `juce::PropertiesFile` alongside the audio
device selection, which is the same category — a property of this installation
rather than of this project. Nothing about it needs the format, and putting it
there would make every project file a vector for changing a user's keyboard.

---
