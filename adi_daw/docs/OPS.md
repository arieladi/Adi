# The op vocabulary — step 3

**Status:** framework complete and **implemented** in `src/adi/ops.{hpp,cpp}`.
The catalogue covers all P0/P1 surface and most P2; six of the 174 are wired up
with handlers and inverses, chosen to exercise all three inverse shapes rather
than to be a lot of ops. The rest is mechanical. The contracts in §1–§8 are the
part that is expensive to change.

Every mutation to an `.adi` project is an **op**: a typed, scoped, attributed,
invertible command appended to the `ops` table in the same transaction that
performs it (SPEC §8.1, ADR-0003). This document defines what an op *is*.

This is the last design document before code.

---

## 1. What we took, and from where

Read before writing: MAGDA's `magda::remote::OperationRegistry`
(`reference/magda-core/magda/daw/api/remote_api.hpp`) and Zrythm's action model
(`reference/zrythm/src/actions/`, `src/gui/backend/legacy_actions/`).

**From MAGDA — the registry shape.** Three things, all of which their code
argues for better than a summary can:

- **Exactly one scope per operation, not a set.** Their reasoning, which is
  correct: every operation belongs to exactly one thing a user would decide
  about, and a set "would invite combinations nobody can reason about from a
  settings checkbox."
- **The handler lives on the descriptor**, not in a table keyed by name, "so a
  transport cannot reach a different implementation than the one declared."
- **Scope defaults to the safe value, paired with a registry-wide assertion that
  every write declares something else** — so a new write op that forgets its
  scope is caught at startup, "rather than by a client discovering it can edit."

**From Zrythm — the taxonomy, and one thing we had missed entirely.** Their
`UndoableAction` declares, per action, not just how to undo but *what it
disturbs*: `needs_pause()`, `needs_transport_total_bar_update()`,
`affects_audio_region_internal_positions()`. Their new `UndoStack` takes a
`CallbackWithPausedEngineRequester`.

Not every mutation can be applied to a running engine by swapping a snapshot.
Our op descriptor therefore declares its engine impact (§4).

**From neither: persistence.** Worth stating plainly, because it was described
to us the other way round. **Zrythm's undo is not persistent either** — the new
stack is a Qt `QUndoStack`, the legacy one an in-memory `UndoStack`. MAGDA's
terminates in JUCE's in-memory `UndoManager`. Both die with the process. Zrythm
gives us *how to carve edits into actions*, not how to persist them.

---

## 2. Op identity and naming

`domain.verb`, lowercase, dot-separated, stable forever once shipped.

A name is an API. Renaming one is a format break, because it appears in every
`ops.op_type` row of every project ever saved. Get it right or accept it.

## 3. The descriptor

```cpp
struct OpDescriptor {
    std::string_view  name;              // "clip.move"
    std::string_view  summary;
    Scope             scope;             // exactly one — §5
    EngineImpact      engineImpact;      // — §4
    Schema            payloadSchema;     // CBOR-shaped, closed
    OpHandler         apply;             // never null for a write
    InverseBuilder    buildInverse;      // never null for a write
    bool              coalescable;       // §6.4
    bool              ephemeral;         // §10
};
```

**Registry invariants, asserted at startup — the process refuses to start if any
fails:**

1. Every op with `scope != Read` has a non-null `apply` **and** `buildInverse`.
2. Every op has a closed payload schema: unknown fields rejected, not ignored.
   **This applies to a payload arriving from a caller, and is not in conflict
   with §8 rule 3, which preserves unknown fields read back from the log.** They
   are opposite directions of travel and the distinction is load-bearing:

   - A **caller** (UI, script, agent) is asking us to *do* something. An unknown
     field is a typo or a version mismatch, and accepting it silently means the
     caller believes it set something it did not. **Reject.**
   - Reading an op **already in the log**, an unknown field belongs to a newer
     build. Dropping it would corrupt a log we are only carrying. **Preserve.**

   Conflating the two is how a format quietly loses data, so the implementation
   has two separately-named functions rather than one with a flag.
3. No two ops share a name.
4. Every name matches `^[a-z][a-zA-Z0-9]*\.[a-z][a-zA-Z0-9]*$`.
5. **No payload schema contains a field whose value may be resolved from ambient
   state** (§7). Enforced by schema review, not by the compiler — see §7.4.

## 4. Engine impact

```cpp
enum class EngineImpact {
    None,          // no audio-domain effect (renaming a track)
    Snapshot,      // applied live by publishing a new snapshot — the common case
    GraphRebuild,  // topology changed; graph prepared off-thread, then swapped
    RequiresPause, // cannot be done live: engine stops, applies, restarts
};
```

`RequiresPause` must stay a small, explicitly justified set — every member is a
click or stall the user hears. In the catalogue it currently has **exactly one
member**, `project.setSampleRate`, and the validator prints the list on every run
so growth is visible rather than gradual. The other two cases that need a stopped
engine — changing audio device, closing the project — are application actions
that mutate no project state, so they are not ops at all.

## 5. Scopes

| Scope | Covers |
|---|---|
| `read` | Observation only. Never writes, never appends to the op log. |
| `edit` | Mutates project content: tracks, clips, notes, automation, devices. |
| `transport` | Play, stop, record, locate, loop. |
| `session` | Clip and scene launching, follow actions. |
| `hardware` | Controller maps, audio/MIDI device binding, input routing. |

Agent capability tiers (AI-AGENT.md §2) are now *defined* as scope sets:

| Tier | Scopes |
|---|---|
| Observe | `read` |
| Propose | `read`, plus may build an op batch it may not commit |
| Apply | `read`, `edit`, `session`, `transport` |

`hardware` is never granted to an agent. Repointing someone's audio device or
rewriting their controller map is not an editing action.

## 6. Inverses

### 6.1 The rule

An op's inverse is computed **before** the forward op is applied, inside the same
transaction, from the state about to be overwritten. If `buildInverse` cannot
produce a correct inverse, the op does not commit.

### 6.2 Three shapes, in order of preference

1. **Symmetric** (`sym`) — same op, swapped arguments. `clip.move {id, from, to}`
   inverts to `clip.move {id, to, from}`. Most ops should be built to land here.
2. **Paired** (`pair`) — a dedicated opposite. `track.create` ↔ `track.delete`.
3. **State capture** (`cap`) — a CBOR blob of everything the forward op
   destroyed. Required for anything lossy.

### 6.3 What state capture costs, and why it is bounded

**Media is never captured.** Deleting a track unlinks `media_files` rows; it does
not delete audio. The inverse of "delete a 40-clip audio track" is the track row,
its clips and its device state — a few hundred KB — not gigabytes. The
content-addressed media pool (ADR-0005) is what makes this true.

Where an inverse would still exceed a threshold (proposed: 4 MB), the op is
marked `lossy_undo` and the user is told **before** it commits. Silently dropping
undo is not acceptable; neither is a 2 GB op log.

### 6.4 Coalescing

Ops marked `coalescable` merge with the immediately preceding op when actor,
target and `txn_id` match and they fall inside a window (proposed: 300 ms). The
**first** op's inverse is kept — that is the one that returns to where the drag
started. Coalescing happens before the log is written, never after.

---

## 7. Determinism: ops never read ambient state

**This is the contract the whole catalogue depends on, and it generalises beyond
selection.**

### 7.1 Selection is session state, and ops know nothing about it

Selection lives in Layer 3 (`session_state`), written on a debounce and on clean
close, so the workspace restores exactly. It **never** enters the op log as
input.

Two failure modes this avoids, both real:

- **If selection were op state**, every mouse click would be a transaction. The
  undo tree fills with navigation, and undoing an accidental delete takes five
  keystrokes.
- **If ops could target "whatever is selected"**, the system loses determinism. An
  agent or script firing `clip.delete` while the user clicks a different track a
  millisecond before commit destroys the wrong data. That is not a race we can
  test our way out of; it has to be impossible by construction.

The UI is the translator. On Delete, it reads the ephemeral selection, resolves
it to entity IDs, and embeds those absolute IDs in the payload:
`clip.delete { ids: [92, 93] }`. The op has no concept of selection.

### 7.2 The rule is broader than selection

Selection is one instance of a general hazard. **No op payload may contain a
value that the handler resolves from ambient state.** Also banned as implicit
inputs:

| Ambient thing | Must be passed as |
|---|---|
| Current selection | explicit `ids` |
| Playhead position | explicit `pos_ticks` |
| Current grid / snap | explicit `grid_ticks` |
| "The current track" | explicit `track_id` |
| Loop / cycle region | explicit `start`, `end` |
| Current tool or mode | explicit parameters |
| Default new-clip length | explicit `length_ticks` |

`note.quantize { clip_id, grid: "current" }` has exactly the same bug as
targeting the selection, and would be exactly as hard to reproduce.

The test: **an op replayed from the log a year later, on a machine with a
different UI state, must do precisely what it did the first time.** If it
wouldn't, it is reading ambient state.

### 7.3 IDs are allocated by the caller, not the handler

A consequence that is easy to miss and expensive to retrofit.

`clip.create` takes the new clip's ID **in its payload**. It does not allocate one
and return it. Reason: undo a `clip.create`, then redo it, and the clip must come
back with the *same* ID — otherwise every later op in the redo chain that
references it is now pointing at nothing. The same applies to tracks, lanes,
devices, chains, automation lanes, scenes, markers and note IDs.

Handlers therefore reject an ID that already exists rather than reassigning. The
allocator lives with the caller (UI, script, agent), which draws from a
project-scoped counter in `adi_meta`.

This is what makes the op log **replayable**, not merely undoable — and
replayability is what step 4's round-trip corpus will actually test.

### 7.4 How this is enforced

Invariant 5 in §3 cannot be checked by a compiler. It is enforced by:

- **Code review of every new op schema**, against the table in §7.2.
- **A replay test in the step-4 corpus**: apply a log to an empty project twice,
  under deliberately different UI state, and assert the resulting projects are
  byte-identical after canonical text projection (ADR-0007). An op reading
  ambient state fails this.

### 7.5 Selection and undo — the one place they touch

Undoing a delete should restore what was selected, or the user has to find their
clips again. That is a genuine requirement, and it is *not* a reason to weaken
§7.1.

Resolution: the **first op of each transaction** may carry an advisory
`sel_before` CBOR blob in `ops.tags`, recording what was selected when the
transaction began. It is:

- never an input to `apply` — handlers cannot read it;
- purely a UI hint on undo;
- droppable by compaction, and safely ignorable by any reader.

Selection influences the *view* after an undo. It never influences *what* an op
does. Keeping it out of the payload and in advisory metadata is what lets both
statements be true.

---

## 8. Encoding

CBOR (RFC 8949), deterministic encoding per §4.2, via `nlohmann/json` (ADR-0016).

One concrete advantage over MAGDA's JSON transport: **CBOR has real integer
types.** Their code carries a `jsonInteger()` helper to prove a value integral
and in range before casting, because JSON has one numeric type. Our tick
positions are `i64` at 5,765,760 PPQ and exceed a double's 2^53 exact range — a
silent truncation in JSON, a non-issue in CBOR.

1. **Deterministic** encoding: shortest-form integers, sorted map keys, no
   indefinite-length items. Identical input must encode byte-identically,
   because payloads reach content hashes and the text projection.

   Deterministic, *not* RFC 8949 §4.2 canonical — the two differ and only the
   weaker one is needed or provided. §4.2 orders keys by encoded bytes, which is
   length-first (`"z"` before `"aa"`); nlohmann orders them lexicographically.
   Byte-level interoperability with a foreign canonical encoder is not a
   requirement, and claiming it while not having it would be worse than not
   having it. See ADR-0025.
2. Map keys are **short strings** (`"id"`, `"t"`, `"pos"`), not integers.
   Integer keys were specified here originally and are not implementable with
   `nlohmann::json`, whose object keys are always strings in both directions; a
   one-character CBOR string key costs one byte more than an integer key, which
   is the whole price. Key names are as permanent as op names and are **never
   reused** for a different meaning. See ADR-0025.
3. Unknown keys are **preserved** on read and re-emitted on write (ADR-0008,
   ADR-0012). An older build must not strip a newer build's fields from an op it
   is merely carrying.
4. No floats where an integer will do. Tick positions are integers. Gain is `f64`
   because it genuinely is.

---

## 9. The catalogue

`S` scope: `e` edit · `t` transport · `s` session · `h` hardware
`E` engine: `N` none · `S` snapshot · `G` graph rebuild · `P` requires pause
`Inv` inverse: `sym` symmetric · `pair` paired · `cap` state capture · `—` not undoable (§10)
`P` priority per FEATURES.md.

### 9.1 Project and time

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `project.setName` | e | N | sym | P0 |
| `project.setTempo` | e | S | sym | P0 |
| `project.insertTempoEvent` | e | S | pair `removeTempoEvent` | P0 |
| `project.removeTempoEvent` | e | S | cap | P0 |
| `project.setTempoCurve` | e | S | sym | P2 |
| `project.insertTimeSignature` | e | S | pair `removeTimeSignature` | P0 |
| `project.removeTimeSignature` | e | S | cap | P0 |
| `project.insertKeyEvent` | e | N | pair `removeKeyEvent` | P2 |
| `project.removeKeyEvent` | e | N | cap | P2 |
| `project.setSampleRate` | e | **P** | sym | P0 |
| `project.setTimecodeOrigin` | e | N | sym | P3 |
| `project.setFrameRate` | e | N | sym | P3 |

### 9.2 Tracks and lanes

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `track.create` | e | G | pair `track.delete` | P0 |
| `track.delete` | e | G | cap | P0 |
| `track.reorder` | e | S | sym | P0 |
| `track.setParent` | e | G | sym | P1 |
| `track.rename` | e | N | sym | P0 |
| `track.setColor` | e | N | sym | P1 |
| `track.setMute` | e | S | sym | P0 |
| `track.setSolo` | e | S | sym | P0 |
| `track.setSoloDefeat` | e | S | sym | P2 |
| `track.setArm` | e | G | sym | P0 |
| `track.setMonitorMode` | e | G | sym | P0 |
| `track.setInput` | e | G | sym | P0 |
| `track.setTimeBase` | e | S | sym | P0 |
| `track.setAutomationMode` | e | N | sym | P2 |
| `track.setLocked` | e | N | sym | P1 |
| `track.freeze` | e | G | pair `track.unfreeze` | P1 |
| `track.unfreeze` | e | G | cap | P1 |
| `lane.create` | e | S | pair `lane.delete` | P1 |
| `lane.delete` | e | S | cap | P1 |
| `lane.reorder` | e | S | sym | P1 |
| `lane.rename` | e | N | sym | P1 |
| `lane.setMute` | e | S | sym | P1 |
| `lane.setCompTarget` | e | S | sym | P1 |

### 9.3 Clips

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `clip.create` | e | S | pair `clip.delete` | P0 |
| `clip.delete` | e | S | cap | P0 |
| `clip.move` | e | S | sym | P0 |
| `clip.resize` | e | S | sym | P0 |
| `clip.split` | e | S | pair `clip.join` | P0 |
| `clip.join` | e | S | cap | P1 |
| `clip.duplicate` | e | S | pair `clip.delete` | P0 |
| `clip.setName` | e | N | sym | P1 |
| `clip.setColor` | e | N | sym | P1 |
| `clip.setMute` | e | S | sym | P0 |
| `clip.setGain` | e | S | sym, coalescable | P0 |
| `clip.setLoop` | e | S | sym | P0 |
| `clip.setContentOffset` | e | S | sym | P1 |
| `clip.setFade` | e | S | sym | P0 |
| `clip.setAlias` | e | S | sym | P2 |
| `clip.consolidate` | e | S | cap | P2 |
| `clip.setLane` | e | S | sym | P1 |

### 9.4 Audio clips and warping

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `audioClip.setSource` | e | S | sym | P0 |
| `audioClip.setSourceWindow` | e | S | sym | P0 |
| `audioClip.setWarpEnabled` | e | S | sym | P1 |
| `audioClip.setWarpMode` | e | S | sym | P1 |
| `audioClip.insertWarpMarker` | e | S | pair `removeWarpMarker` | P1 |
| `audioClip.moveWarpMarker` | e | S | sym, coalescable | P1 |
| `audioClip.removeWarpMarker` | e | S | cap | P1 |
| `audioClip.setWarpMarkers` | e | S | cap | P1 |
| `audioClip.setTranspose` | e | S | sym | P1 |
| `audioClip.setFormant` | e | S | sym | P2 |
| `audioClip.setReverse` | e | S | sym | P1 |
| `audioClip.setChannelMode` | e | S | sym | P2 |

### 9.5 Notes and expression

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `note.insert` | e | S | pair `note.delete` | P0 |
| `note.delete` | e | S | cap | P0 |
| `note.move` | e | S | sym | P0 |
| `note.resize` | e | S | sym | P0 |
| `note.setVelocity` | e | S | sym, coalescable | P0 |
| `note.setReleaseVelocity` | e | S | sym, coalescable | P2 |
| `note.setChannel` | e | S | sym | P1 |
| `note.setProbability` | e | S | sym, coalescable | P2 |
| `note.setTuning` | e | S | sym, coalescable | P2 |
| `note.setMute` | e | S | sym | P1 |
| `note.quantize` | e | S | cap | P0 |
| `note.transpose` | e | S | sym | P0 |
| `note.applyGroove` | e | S | cap | P2 |
| `note.legato` | e | S | cap | P2 |
| `noteExpression.write` | e | S | cap | P1 |
| `noteExpression.clear` | e | S | cap | P1 |
| `cc.write` | e | S | cap | P0 |
| `cc.delete` | e | S | cap | P0 |
| `sysex.write` | e | S | cap | P3 |

### 9.6 Automation

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `automation.createLane` | e | S | pair `deleteLane` | P0 |
| `automation.deleteLane` | e | S | cap | P0 |
| `automation.writePoints` | e | S | cap | P0 |
| `automation.deletePoints` | e | S | cap | P0 |
| `automation.movePoints` | e | S | sym, coalescable | P0 |
| `automation.setCurve` | e | S | sym | P1 |
| `automation.clearLane` | e | S | cap | P1 |
| `automation.setLaneEnabled` | e | S | sym | P1 |
| `automation.setLaneVisible` | e | N | sym | P1 |
| `automation.setDefaultValue` | e | S | sym | P2 |

### 9.7 Devices, chains, macros

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `device.insert` | e | G | pair `device.remove` | P0 |
| `device.remove` | e | G | cap | P0 |
| `device.move` | e | G | sym | P0 |
| `device.setEnabled` | e | S | sym | P0 |
| `device.setParam` | e | S | sym, coalescable | P0 |
| `device.loadState` | e | S | cap | P0 |
| `device.setPreset` | e | S | cap | P1 |
| `device.rename` | e | N | sym | P1 |
| `device.setLatency` | e | G | sym | P0 |
| `chain.create` | e | G | pair `chain.delete` | P2 |
| `chain.delete` | e | G | cap | P2 |
| `chain.reorder` | e | G | sym | P2 |
| `chain.rename` | e | N | sym | P2 |
| `chain.setZones` | e | S | sym | P2 |
| `chain.setMute` | e | S | sym | P2 |
| `chain.setSolo` | e | S | sym | P2 |
| `macro.setValue` | e | S | sym, coalescable | P2 |
| `macro.rename` | e | N | sym | P2 |
| `macro.map` | e | S | pair `macro.unmap` | P2 |
| `macro.unmap` | e | S | cap | P2 |
| `macro.setRange` | e | S | sym | P2 |

### 9.8 Routing and mixer

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `routing.connect` | e | G | pair `routing.disconnect` | P0 |
| `routing.disconnect` | e | G | cap | P0 |
| `routing.setGain` | e | S | sym, coalescable | P0 |
| `routing.setPan` | e | S | sym, coalescable | P1 |
| `routing.setPreFader` | e | G | sym | P0 |
| `routing.setEnabled` | e | S | sym | P0 |
| `routing.reorder` | e | S | sym | P1 |
| `mixer.setVolume` | e | S | sym, coalescable | P0 |
| `mixer.setPan` | e | S | sym, coalescable | P0 |
| `mixer.setWidth` | e | S | sym, coalescable | P2 |
| `mixer.setInputGain` | e | S | sym, coalescable | P1 |
| `mixer.setPhaseInvert` | e | S | sym | P1 |
| `mixer.setDelay` | e | G | sym | P1 |
| `mixer.setPanLaw` | e | S | sym | P2 |
| `mixer.setVcaGroup` | e | S | sym | P2 |

### 9.9 Session view

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `scene.create` | e | S | pair `scene.delete` | P1 |
| `scene.delete` | e | S | cap | P1 |
| `scene.reorder` | e | S | sym | P1 |
| `scene.rename` | e | N | sym | P1 |
| `scene.setTempo` | e | S | sym | P2 |
| `session.setSlotClip` | e | S | sym | P1 |
| `session.clearSlot` | e | S | cap | P1 |
| `session.setLaunchMode` | e | S | sym | P1 |
| `session.setLaunchQuant` | e | S | sym | P1 |
| `session.setFollowAction` | e | S | sym | P2 |
| `session.launchClip` | s | S | — | P1 |
| `session.launchScene` | s | S | — | P1 |
| `session.stopTrack` | s | S | — | P1 |
| `session.stopAll` | s | S | — | P1 |

### 9.10 Markers, arranger, snapshots

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `marker.create` | e | N | pair `marker.delete` | P1 |
| `marker.delete` | e | N | cap | P1 |
| `marker.move` | e | N | sym | P1 |
| `marker.rename` | e | N | sym | P1 |
| `arranger.createSection` | e | N | pair `deleteSection` | P2 |
| `arranger.deleteSection` | e | N | cap | P2 |
| `arranger.resizeSection` | e | N | sym | P2 |
| `arranger.reorderChain` | e | N | sym | P2 |
| `arranger.setRepeats` | e | N | sym | P2 |
| `snapshot.create` | e | N | pair `snapshot.delete` | P2 |
| `snapshot.recall` | e | S | cap | P2 |
| `snapshot.delete` | e | N | cap | P2 |
| `snapshot.rename` | e | N | sym | P2 |

### 9.11 Media

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `media.import` | e | N | pair `media.unlink` | P0 |
| `media.unlink` | e | N | cap | P1 |
| `media.relink` | e | S | sym | P1 |
| `media.embed` | e | N | pair `media.extract` | P1 |
| `media.extract` | e | N | pair `media.embed` | P1 |
| `media.setName` | e | N | sym | P2 |

### 9.12 Transport, hardware, extensions

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `transport.play` | t | N | — | P0 |
| `transport.stop` | t | N | — | P0 |
| `transport.seek` | t | N | — | P0 |
| `transport.setLoop` | t | N | — | P0 |
| `transport.setRecord` | t | G | — | P0 |
| `transport.setMetronome` | t | S | — | P1 |
| `controller.map` | h | N | pair `controller.unmap` | P2 |
| `controller.unmap` | h | N | cap | P2 |
| `controller.setRange` | h | N | sym | P2 |
| `controller.setTakeover` | h | N | sym | P2 |
| `extension.write` | e | N | cap | P1 |
| `extension.delete` | e | N | cap | P1 |

**174 ops** — 63 P0, 62 P1, 46 P2, 3 P3. Every P0 and P1 feature in FEATURES.md
has a corresponding op, or is explicitly a runtime concern with no persisted
state. Counted and consistency-checked by `tools/validate_ops.py`, not asserted.

---

## 10. Ops that are not undoable, and why that is fine

`transport.play` and `session.launchClip` are **performance**, not editing. They
write no project state, and appending them to undo history would bury the last
real edit under navigation.

They still go through the registry — same validation, same scopes, same
attribution — and still emit an op row, tagged `ephemeral`. Undo skips ephemeral
ops; the audit trail does not. So "what did the agent do at 14:32" stays
answerable, and Ctrl-Z still lands on the thing you actually changed.

Compaction (SPEC §8.3) may drop ephemeral ops first.

---

## 11. Still open

1. ~~**CBOR key-name table.**~~ **RESOLVED** — the key names live in the
   registry, as a `Field` table beside each op's descriptor in `src/adi/ops.cpp`.
   That is the one place a reader of the op must already be looking, and it
   means a key cannot be added without also declaring its type and whether it is
   required. They are permanent once shipped, exactly like op names.
2. **Batch failure semantics.** An agent request is one `txn_id`; a partial
   failure mid-batch is presumed all-or-nothing per SPEC §3.5, but the rollback
   path for a `GraphRebuild` op that fails halfway is not written down.
3. **Score, chord track, expression maps, VariAudio, groove pool.** The five
   format gaps in FEATURES.md §12. No schema, therefore no ops.
4. **Comping ops.** `lane.*` gives the structure; the comp *edit* gestures
   (swipe-to-comp, promote take) need their own verbs and are not yet named.
