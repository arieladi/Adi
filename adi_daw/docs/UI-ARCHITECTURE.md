# UI architecture — the shell, and how the graph carries a hybrid track

**Status:** design. Nothing built. The UI is roadmap step 7; this exists so
step 6 does not make it impossible.

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

**No component owns project state.** Every one of them reads the current
`Snapshot` and emits ops. None holds a `SQLite::Database&`, a row id it looked
up itself, or a cached copy of anything. This is ADR-0010 at the UI layer, and
it is what makes the whole shell testable against a hand-built `Snapshot` — the
same trick that makes `buildTree` testable without a database.

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
