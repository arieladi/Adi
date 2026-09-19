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
│ DeviceChainStrip          follows selection, full width              │
└──────────────────────────────────────────────────────────────────────┘
```

## 2. The component tree

```
juce::DocumentWindow
└── AdiRootComponent
    │     owns: SnapshotReader (ADR-0019), OpSubmitter, TrackOrderModel,
    │           ViewState. Owns no project data of its own.
    ├── TransportBar
    ├── MainSplit                       juce::StretchableLayoutManager, 3 cols
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
    └── DeviceChainStrip
        └── DeviceView[]                one per device on the selected track
```

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

## 5. The three view states

| State | Shows |
|---|---|
| **Global** | every track, fitted vertically |
| **Group / stem focus** | the selected group and its children; everything else hidden |
| **Detailed zoom** | the selected track at edit resolution, detail editor open |

The state is one value in `ui_view`, and every panel derives visibility from it
rather than holding its own flag — otherwise the mixer and the timeline end up
showing different subsets and the user cannot tell which is lying.

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

**One clock, draining coalesced dirt.** A single `juce::VBlankAttachment` on the
root drives the whole shell at display rate. Components never call `repaint()`
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
