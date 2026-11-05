# The op vocabulary — step 3

**Status:** framework complete and **implemented** — machinery in
`src/adi/ops.{hpp,cpp}`, catalogue in `src/adi/ops_catalog.cpp`, undo in
`src/adi/history.{hpp,cpp}`.

The implemented ops have handlers and inverses, covering most of the
P0 editing surface. `adi_tool ops` lists them. Roughly half are generated from a
table rather than hand-written: most of P0 is "set one column, the inverse is the
old value", and 24 textually identical handlers is 24 chances to get a `WHERE`
clause wrong. Generated, that failure mode does not exist and the 25th is one
line.

Every one is covered by `adi_catalog_tests` — applied, then undone, then checked
that the world came back, which exercises the inverse the way undo actually will
rather than inspecting the stored blob. And all of them are in the round-trip
corpus (ADR-0021 §7.4, `adi_replay_tests`), which is the only test that can catch
a handler reading ambient state.

The contracts in §1–§8 are the part that is expensive to change.

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
   **An empty schema is legal and meaningful** — `transport.play` takes no
   parameters, and an empty closed schema says "takes nothing, and any field is
   an error". An earlier draft of this invariant rejected an empty schema on a
   write op, on the theory that it meant someone had forgotten to declare one;
   it cannot tell the two cases apart, and a forgotten schema fails loudly on
   first use regardless, because every field is then rejected as unknown.
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
| `hardware` | Controller maps, audio/MIDI device binding, input routing. |

Agent capability tiers (AI-AGENT.md §2) are now *defined* as scope sets:

| Tier | Scopes |
|---|---|
| Observe | `read` |
| Propose | `read`, plus may build an op batch it may not commit |
| Apply | `read`, `edit`, `transport` |

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

**Media is never captured.** Deleting a track unlinks its media references; it does
not delete audio. The inverse of "delete a 40-clip audio track" is the track row,
its clips and its device state — a few hundred KB — not gigabytes. The
content-addressed media pool (ADR-0005) is what makes this true.

`track.delete {id}` captures all columns and SQLite storage types of the deleted
track subtree and its transitive foreign-key children: clips and streams, lanes,
mixer/summing rows, nested device chains, parameters, state hashes and panels.
It also removes internal routing edges, automation owned by a deleted
track/clip/device/edge, and their controller bindings. External hardware port ids
remain hardware ids even when numerically equal to a deleted object. Surviving
rows changed by `ON DELETE SET NULL` (for example aliases or VCA membership) are
captured as before/after images. Media pool, shared state blobs and plug-in refs
stay in place; the inverse carries references, not their bytes. Weak remark
anchors remain, and can be shown as orphaned remarks.

The paired inverse is `track.create` with the legacy descriptive fields plus an
optional `contents` object (version 1, root id, ordered typed row images). Ordinary
create and old captured inverses without `contents` remain supported. The full
restore validates expected current rows and identities before writing, refuses
collisions or changed surviving rows, restores under the journal transaction,
and verifies the causal image and foreign keys. Composite keys and future keyed
foreign-key children are included. Affected tables with triggers, generated
columns, no declared primary key, or an affected `SET DEFAULT` reference refuse
atomically until their effects have an explicit safe capture path; they are not
silently dropped. Unknown extension/session rows without ownership foreign keys
remain untouched. All capture and restoration runs on the owner thread.

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
devices, chains, automation lanes, markers and note IDs.

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

`S` scope: `e` edit · `t` transport · `h` hardware
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
| `project.insertKeyEvent` | e | S | pair `removeKeyEvent` | P2 |
| `project.removeKeyEvent` | e | S | cap | P2 |
| `project.setKeyScale` | e | S | sym | P2 |
| `tuning.create` | e | S | pair `tuning.delete` | P2 |
| `tuning.delete` | e | S | cap | P2 |
| `tuning.replace` | e | S | sym | P2 |
| `project.setSampleRate` | e | **P** | sym | P0 |
| `project.setTimecodeOrigin` | e | N | sym | P3 |
| `project.setFrameRate` | e | N | sym | P3 |

Native key changes and ADR-0178 tuning definitions use the explicit-ID
payloads in `ENGINE-TUNING-EDITS.md`. Inserts never upsert. Replacing a tuning
validates all existing selected degrees; deleting a referenced tuning refuses.
All six handlers capture complete inverses, use the journal transaction, and
request a snapshot update without retuning notes. The text projection covers
the five existing relational scale tables. No schema/binary-layout change.

### 9.2 Tracks and lanes

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `track.create` | e | G | pair `track.delete` | P0 |
| `track.delete` | e | G | cap | P0 |
| `track.reorder` | e | S | sym | P0 |
| `group.setFreezeScope` | e | G | sym | P1 |
| `track.setFreeze` | e | G | sym | P1 |
| `track.setParent` | e | G | sym | P1 |
| `track.rename` | e | N | sym | P0 |
| `track.setColor` | e | N | sym | P1 |
| `track.setRole` | e | G | sym | P1 |
| `track.setMute` | e | S | sym | P0 |
| `track.setSolo` | e | S | sym | P0 |
| `track.setSoloDefeat` | e | S | sym | P2 |
| `track.setArm` | e | G | sym | P0 |
| `track.setMonitorMode` | e | G | sym | P0 |
| `track.setRecordQuantize` | e | S | pair `track.restoreRecording` | P1 |
| `track.setPlayQuantize` | e | S | pair `track.restoreRecording` | P1 |
| `track.setKeepMonitoringLatency` | e | S | pair `track.restoreRecording` | P1 |
| `track.restoreRecording` | e | S | cap | P1 |
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

N31 per-track policy (schema 1.14): track.setRecordQuantize {id,on,grid?},
track.setPlayQuantize {id,on,grid?,forgivenessMs?}, and
track.setKeepMonitoringLatency {id,on}. Grid is 1/4, 1/8, 1/16 or 1/32;
omitted/null grid retains the selected grid while on=false selects Free.
Play-Q forgiveness is explicit 0..1000 ms, initially zero. Each inverse is
track.restoreRecording {id,row?}: a complete typed prior optional row, including
unknown fields/BLOBs; omitted/null row restores absence. No coalescing discards
those captures. Snapshot TrackNode.recording exposes every field. A caller's
new-track batch appends trackRecordingDefaults(id, record.quantize choice,
keepMonitoringLatency) with values already read from settings; replay never
reads app preferences. The recording writer and live Play-Q scheduling are
separate work, not claimed implemented by these policy ops.

`track.setRole {id, role?}` assigns `kick`, `bass`, `vocal` or `reference`;
omission or explicit null clears it. It targets an audio/MIDI/instrument/group/
return track, never Main or a metadata-only track. A reference assignment refuses
when another reference exists; a single batch can clear the old role then assign
the new one. No implicit extra ids enter the payload. Its symmetric inverse
captures the nullable old role; an unknown stored future role refuses overwrite
so undo never loses it. Graph rebuild makes the N23 cue backend re-read the role.
This op requires win's native `tracks.role` format/migration. Until that lands,
it refuses atomically with a format-unavailable error; it never changes the
schema. The snapshot reads null on older schemas and preserves native role text
(including unknown future values) for display/sharing.

### 9.3 Clips

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `clip.create` | e | S | pair `clip.delete` | P0 |
| `clip.delete` | e | S | cap | P0 |
| `clip.attachAudio` | e | G | pair `clip.detachAudio` | P0 |
| `clip.detachAudio` | e | G | cap | P0 |
| `clip.move` | e | S | sym | P0 |
| `clip.resize` | e | S | sym | P0 |
| `clip.split` | e | G | pair `clip.join` | P0 |
| `clip.join` | e | G | cap | P1 |
| `clip.duplicate` | e | G | pair `clip.delete` | P0 |
| `clip.setName` | e | N | sym | P1 |
| `clip.setColor` | e | N | sym | P1 |
| `clip.setMute` | e | S | sym | P0 |
| `clip.setGain` | e | S | sym, coalescable | P0 |
| `clip.setLoop` | e | S | cap | P0 |
| `clip.setContentOffset` | e | S | sym | P1 |
| `clip.setFade` | e | S | sym | P0 |
| `clip.setFades` | e | S | cap | P0 |
| `arranger.splice` | e | G | cap | P0 |
| `clip.setAlias` | e | S | sym | P2 |
| `clip.makeUnique` | e | S | cap | P2 |
| `clip.consolidate` | e | S | cap | P2 |
| `clip.setLane` | e | S | sym | P1 |
| `clip.setStartMarker` | e | S | cap | P1 |
| `clip.setEndMarker` | e | S | cap | P1 |
| `clip.setTimeSignature` | e | S | cap | P1 |
| `clip.setScale` | e | S | cap | P1 |

N17 compatibility: when the optional clip_playback_details table is present,
Start clears its content offset and retained fade domains, End clears its
physical source limit, and a loop-window edit clears both physical loop
endpoints. A switch-only loop edit retains the dormant window. Inverses capture
the full typed details row, including future fields or row absence; undo and
redo restore it. This optional SQL seam adds no dependency on N17's schema.

N33 Main Clip Properties: `clip.setStartMarker {id,ticks}` and
`clip.setEndMarker {id,ticks}` use source-relative musical ticks (0..2^50).
Start precedes End. Moving Start pins the old default End; placement/length
stay fixed. End bounds non-loop playback; Loop uses its own window.
`clip.setLoop {id,on,start?,length?}` edits the existing loop columns atomically;
enabling needs a nonnegative start and positive length. A broken old loop can
always be disabled without supplying a new window.
`clip.setTimeSignature {id,num?,den?}` takes both fields (1..255, power-of-two
1..128), or neither to inherit the project. `clip.setScale {id,root?,scale?,mask?}`
takes all three fields, or none to inherit: root 0..11, a nonempty scale id and
root-relative 12-bit mask including the tonic. Fields apply to this clip only.
These ops capture the full clip and optional property row for exact undo,
updating in place without cascading children; clearing retains extensions.
`clip.delete` also captures all native clip cascade rows and external alias
links. Legacy captured create payloads remain readable. No unknown child-table
capture is claimed.

The clip.setFades payload is {id,in?:{ticks?,curve?,tension?},out?:{...}}.
It is audio-only; ticks are nonnegative and no longer than the clip's musical
span, curve is 0..5, tension is finite [-1,1] or null for legacy arithmetic.
Each selected side must set at least one known field. Both sides are one edit;
its inverse captures native fields and the full typed details row or absence.

The clip.split op and its captured inverse clip.join take {id,changes}:
full typed before/after rows, never ambient ID allocation. The clip.duplicate
op creates only one captured clip and its children; its inverse is clip.delete.
Delete captures full native rows, opaque children and external alias links.
The arranger.splice op applies arrangement-only captured row deltas; its inverse
swaps them. Project changes are limited to length_ticks. Before-image mismatch,
unknown/generated columns or uncaptured foreign-key effects refuse the edit.
UNIQUE map positions and automation ordinals move in safe order; restore creates
parents before children, including arranger sections and tuning bindings.
Composite key_map_degrees targets encode id as {key_map_id,degree}; existing
single-key packets keep their integer id. Every field of a composite FK is
matched together. Replay uses captured values without evaluating defaults.
Delete Time uses the same captured arranger.splice packet, including an
optional saved-loop before/after pair. Its preparation preserves the old
end context, full tuning children and both global automation curve domains.
Whole-Set Copy Time captures a value without journaling. Cut/Paste/Duplicate
Time use arranger.splice with source-relative timeline data remapped to captured
native ids. Paste inserts the source tempo integral for linear objects, carries
source map/automation context into the gap and restores the destination context
at its end. Root map events may be overwritten at zero with exact typed undo.
Clipboard tuning/lane/media contracts and the captured journal head are checked.
These commands add no opcode; their recorded requests replay independently of
clipboard or window state.

Window callers use clip_commands.hpp and material_commands.hpp, rather
than assembling row images. Prepared edits carry ADR-0153's expected head,
including the empty root, and must be committed with their captured options.
See [the editing API](ENGINE-EDIT-COMMANDS.md) for units, thread ownership,
supported commands and explicit refusals.

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

**Warp material edits (N17).** The captured clip/range/Time producers also
accept attached Warp audio. They preserve AWRP/source windows and capture
source musical fade domains in clip_warp_fade_domains (schema 1.13), so Split
retains the original curve on both pieces with no new internal edge fade.
clip.setFades clears the selected side's retained domain and captures its
complete typed row in the inverse, alongside the existing nanosecond shape.

**Captured Warp import (N17).** `clip_edit::prepareImportAudio` with
`ImportOptions::warp` analyzes the source off the callback, then captures
`audio_clips.warp_markers` (AWRP), source trim, mode and musical clip length
inside the existing `clip.duplicate` row image. `commitMaterial` groups that
image with its optional `media.import`/`media.relink` as one undo step. Undo,
redo and replay restore the literal marker bytes; they do not rerun detection.
The options and six modes are described in ENGINE-WARP.md. This producer
does not imply that the individual catalogue Warp setters above are applied.
Bounce explicitly sets Warp off before preparing its import.

**Existing clean clip's Warp button (N17).** Call
`clip_edit::prepareSetWarp(store, clipId, WarpOptions, referenceRate)` off the
callback, retaining the document generation, then `commitPrepared`.
The producer captures an `arranger.splice` full-row edit of the existing clip,
its audio source and playback/fade metadata; no file analysis runs in apply,
undo or redo. Enable captures trim/map/mode and musical duration; disable
captures the actual physical source duration/pins. Cancellation, changed head
or changed source bytes rejects commit. This clean-import API refuses edited
source offsets, loops, aliases, End markers, reverse/pitch, trimmed extents
and unknown future marker rewrites (ENGINE-WARP.md). Individual catalogue
Warp setters remain separate from this captured producer.

### 9.5 Notes and expression

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `note.insert` | e | S | pair `note.delete` | P0 |
| `capture.createMidi` | e | G | pair `clip.delete` | P1 |
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
| `note.restoreNotes` | e | S | cap | P0 |
| `note.applyGroove` | e | S | cap | P2 |
| `note.legato` | e | S | cap | P2 |
| `noteExpression.write` | e | S | cap | P1 |
| `noteExpression.clear` | e | S | cap | P1 |
| `cc.write` | e | S | cap | P0 |
| `cc.delete` | e | S | cap | P0 |
| `cc.restoreStream` | e | S | cap | P0 |
| `sysex.write` | e | S | cap | P3 |

N34: `note.setMute {clip,note,muted}` sets the note's active flag as
`active = !muted`. `note` is its exact uint64 ANOT id, not a row index;
zero/unassigned, absent and duplicate targets are refused. The MIDI clip and
its canonical notes stream must exist. Undo restores the old muted bit.
The handler patches only bit0 in the original record stride, preserving all
other fields/flags, unknown future tails, header and trailing bytes, row id and
other streams. An unreadable stream is refused without an empty/lossy rewrite.
After committing, refresh Session and N32's Clip View read. Deactivating a held
note releases it on the next published playback callback.

Implemented transform payloads:

- `note.quantize {clip, note_ids[], grid_ticks, origin_ticks, strength, swing,
  starts, ends}` supplies every setting explicitly. `note_ids` is a nonempty,
  duplicate-free array of positive uint64 IDs. `grid_ticks` is positive and at
  most INT64_MAX/2; `origin_ticks` is signed. Strength and swing are fixed-point
  percentages from 0 to 10000 (100.00%). Swing delays odd divisions by up to half
  a grid division. Nearest-grid ties choose the later line; fractional strength
  rounds the integer movement toward its target on a half tick. Starts and ends
  may be chosen independently. Start-only preserves duration. Independently
  quantized endpoints that collapse keep a one-tick duration. Overflow refuses
  the complete op. All arithmetic uses integers, including above 2^53 ticks.
- `note.transpose {clip, note_ids[], semitones}` shifts only the listed pitches.
  Semitones is an integer from -127 to 127. If any selected pitch would leave
  0..127 the entire op refuses; intervals are never silently clipped. Tuning,
  velocity, channel, flags, probability, note IDs and attached expression stay.
  Its inverse uses the opposite semitone distance on the same explicit IDs.
- `note.restoreNotes {clip, stream:{bytes}}` is the captured quantize inverse.
  Bytes is a CBOR byte string (or an array of byte integers for a JSON caller).
  The restored stream must contain exactly the existing note identities. This
  captures exact ordering, stream flags, future record tails and any trailer,
  rather than reconstructing a normalized stream. Its own inverse captures the
  previous bytes. Unknown/malformed streams and false SortedByTime flags refuse.

Quantize sorts whole raw records stably by time; each record keeps its extension
bytes. All transforms are one native undo transaction, irrespective of selection
size; multiple clip requests may share one transaction. Selection flag bits and
UI selection/grid/playhead state are never read. The owner can build requests
with `notes::quantizeRequest` / `transposeRequest` in note_transforms.hpp.
Controller-lane write/delete remains the next Phase C increment.

Implemented controller-lane editing uses existing addressed `event_streams`
rows (`stream_kind='cc'`, ACTL), with channel 0..15 and controller 0..127:

- `cc.write {id,clip,channel,controller,points[]}` upserts explicit point IDs in
  that stream. `id` is its explicit SQL row ID, required even on creation.
  A point is `{id,time,value,tension?,curve?,flags?}`: positive uint64 ID,
  signed relative musical ticks, finite normalized double value 0..1, tension
  -1..1 (default 0), curve 0..5 (default Hold), byte flags (default 0).
  Duplicate incoming IDs, unsupported layouts, invalid values and slot/ID
  collisions refuse the whole op. Unmentioned points retain their raw bytes.
- `cc.delete {id,clip,channel,controller,point_ids[]}` removes only listed
  points. Empty/duplicate/missing selections refuse. Deleting the last point
  retains an empty addressed lane.
- `cc.restoreStream {id,clip,channel,controller,state:{data}}` is the captured
  inverse. Data is NULL for previous absence, or `{bytes}` containing compact
  CBOR binary (JSON callers may supply byte integers). Undo of first creation
  removes its row. Existing streams restore exact bytes, including future
  record tails/header flags/trailer. The same ID/address collision checks apply.

Streams are sorted stably by time. Updating an existing future record preserves
its full tail, while insertion of a new record into an unknown wider layout
refuses. Values retain full normalized precision; MIDI byte quantization belongs
at the host boundary. `controllers::readLanes` returns typed points, raw bytes and
per-lane errors; an unbound NULL address is distinct from channel/controller zero.
ClipNode.controllers publishes the same data immutably, and every field/raw
change participates in structural sharing. SnapshotBuilder reads all project
fields in one SQLite read transaction, including when another connection edits
between the controller and clip queries. The row model carries these lanes too.
`controllers::prepareWrite` finds the addressed slot, allocates explicit stream
and point IDs (zero input point IDs request new IDs), and validates the request
under one owner-side read transaction. It reserves/writes nothing. The returned
request contains all actual IDs. `commitPrepared` refuses a changed history head
or another file/open Store client; retain the document generation across async
work, as with the other prepared editing APIs. The window needs no SQL access.
These are editing/read APIs; ordinary controller-event playback/host routing is
the next increment. No schema or audio-callback change accompanies this one.

Playback prepares immutable per-track controller timelines off the callback.
Musical/linear placement, source offsets, clip loops and step tempo changes use
musical curve phase; curves use the shared six-shape evaluator. Overlapping lanes
at the same channel/CC use the latest clip start, then greatest clip ID. Empty
lanes send nothing; invalid lanes and aliases report preparation errors. Tempo
ramps retain the MIDI player's existing step-tempo warning. Preparation is capped
at one million spans and placed points. Controllers are sampled on a global
64-sample cadence and at exact point/placement/note boundaries, independent of
callback size, with exact state chased on start/seek.

Controller values remain doubles through graph compensation and VST3 mapped
parameters. CLAP uses MIDI bytes on a port advertising MIDI/MIDI-MPE, alongside
native CLAP notes when that is the chosen note dialect; quantization occurs only
at that boundary. Unsupported CLAP ports and unmapped VST3 controllers are counted
by each host's `unsupportedControllers()`. VST3 mapping discovery retains the
existing limitation to a reachable edit controller; no mapping is invented for a
two-component plug-in whose controller JUCE does not expose.

Owned sustain/sostenuto/hold-2 (CC64/66/69) are released before note-offs on stop,
seek, transport/clip wrap, mute or generation replacement. A source keeps pending
release ownership through graph delay and retries rejected/full-list releases,
including after the lane disappears. Ordinary CC values hold after a clip ends.
Controller pressure reserves note-event space and rotates addresses;
`MidiClips::droppedControllers()` counts deferred/refused updates. Callbacks use
fixed state and prepared arrays, with no Store access, allocation or locks.

Implemented controller-lane editing uses existing addressed `event_streams`
rows (`stream_kind='cc'`, ACTL), with channel 0..15 and controller 0..127:

- `cc.write {id,clip,channel,controller,points[]}` upserts explicit point IDs in
  that stream. `id` is its explicit SQL row ID, required even on creation.
  A point is `{id,time,value,tension?,curve?,flags?}`: positive uint64 ID,
  signed relative musical ticks, finite normalized double value 0..1, tension
  -1..1 (default 0), curve 0..5 (default Hold), byte flags (default 0).
  Duplicate incoming IDs, unsupported layouts, invalid values and slot/ID
  collisions refuse the whole op. Unmentioned points retain their raw bytes.
- `cc.delete {id,clip,channel,controller,point_ids[]}` removes only listed
  points. Empty/duplicate/missing selections refuse. Deleting the last point
  retains an empty addressed lane.
- `cc.restoreStream {id,clip,channel,controller,state:{data}}` is the captured
  inverse. Data is NULL for previous absence, or `{bytes}` containing compact
  CBOR binary (JSON callers may supply byte integers). Undo of first creation
  removes its row. Existing streams restore exact bytes, including future
  record tails/header flags/trailer. The same ID/address collision checks apply.

Streams are sorted stably by time. Updating an existing future record preserves
its full tail, while insertion of a new record into an unknown wider layout
refuses. Values retain full normalized precision; MIDI byte quantization belongs
at the host boundary. `controllers::readLanes` returns typed points, raw bytes and
per-lane errors; an unbound NULL address is distinct from channel/controller zero.
ClipNode.controllers publishes the same data immutably, and every field/raw
change participates in structural sharing. SnapshotBuilder reads all project
fields in one SQLite read transaction, including when another connection edits
between the controller and clip queries. The row model carries these lanes too.
`controllers::prepareWrite` finds the addressed slot, allocates explicit stream
and point IDs (zero input point IDs request new IDs), and validates the request
under one owner-side read transaction. It reserves/writes nothing. The returned
request contains all actual IDs. `commitPrepared` refuses a changed history head
or another file/open Store client; retain the document generation across async
work, as with the other prepared editing APIs. The window needs no SQL access.
These are editing/read APIs; ordinary controller-event playback/host routing is
the next increment. No schema or audio-callback change accompanies this one.

Playback prepares immutable per-track controller timelines off the callback.
Musical/linear placement, source offsets, clip loops and step tempo changes use
musical curve phase; curves use the shared six-shape evaluator. Overlapping lanes
at the same channel/CC use the latest clip start, then greatest clip ID. Empty
lanes send nothing; invalid lanes and aliases report preparation errors. Tempo
ramps retain the MIDI player's existing step-tempo warning. Preparation is capped
at one million spans and placed points. Controllers are sampled on a global
64-sample cadence and at exact point/placement/note boundaries, independent of
callback size, with exact state chased on start/seek.

Controller values remain doubles through graph compensation and VST3 mapped
parameters. CLAP uses MIDI bytes on a port advertising MIDI/MIDI-MPE, alongside
native CLAP notes when that is the chosen note dialect; quantization occurs only
at that boundary. Unsupported CLAP ports and unmapped VST3 controllers are counted
by each host's `unsupportedControllers()`. VST3 mapping discovery retains the
existing limitation to a reachable edit controller; no mapping is invented for a
two-component plug-in whose controller JUCE does not expose.

Owned sustain/sostenuto/hold-2 (CC64/66/69) are released before note-offs on stop,
seek, transport/clip wrap, mute or generation replacement. A source keeps pending
release ownership through graph delay and retries rejected/full-list releases,
including after the lane disappears. Ordinary CC values hold after a clip ends.
Controller pressure reserves note-event space and rotates addresses;
`MidiClips::droppedControllers()` counts deferred/refused updates. Callbacks use
fixed state and prepared arrays, with no Store access, allocation or locks.

**Retrospective MIDI input memory (ADR-0132 d9).** `engine/midi_memory.hpp`
provides a bounded `MidiMemory(inputId,trackId,sampleRate,noteChannelBase,capacity)`
for each explicitly eligible input/track. Allocate it before callbacks; default
capacity is 65,536 events, bounded 2..1,048,576. One producer serializes controls,
`push(Event,absoluteDriverSample)` and `advance(exclusiveBlockEnd)`, including
silent blocks. Armed OR monitored input listens independently of transport
play/record; neither gate retains input. Queue gate changes to that producer.
Input on/off, explicit discontinuity, backwards clocks, malformed musical input
and a pinned-slot drop begin a new epoch. Forward rests preserve the history.

Snapshots retain typed note-on/off, expression and controller events, double
values, uint64 identities, integer timestamps above 2^53, original packet frames
and coincident serial order. Input channels are declared rather than guessed:
MpeParser notes use 1..16, while clip notes use 0..15; controllers always address
0..15 and controller 0..127. The memory preserves the declared note convention
and never reconstructs MPE allocation. Note keys/velocities, controller addresses/
values, finite expression values and times are checked. Unassigned release IDs
remain unassigned for owner resolution. Addressed ParamValue/ParamMod events are
ignored without clearing musical history. Feed live input only, never playback
or controller-delivery feedback. No MIDI byte encoding or float narrowing occurs.

The single owner calls `snapshot(output,error)` off callback and receives an
immutable bounded history with input/track/format/convention/epoch, current
exclusive end sample and a `prefixDropped` notice when capacity evicted older
events. This is not a promise that a retained suffix contains complete notes or
a complete phrase. A concurrent overwrite/in-flight slot refuses the entire
snapshot with empty output; callers may retry. Slot ownership avoids data races;
the producer never waits for a reader, instead dropping/counting that event and
starting fresh history. Stats distinguish copied/dropped/ignored/invalid input.
Producer controls/parser/copy calls allocate, lock, log and perform file I/O
nowhere. Constructor/destructor and snapshot work belong off callback.

This increment supplies exact rolling input and the actual MPE-parser handoff,
not automatic host/UI wiring, phrase detection, Live tempo/loop inference or a
native committed clip. Owner-side Capture must resolve complete notes and missing
prefixes before preparing edits. Existing ANOT velocity bytes and AEXP floats
cannot preserve every arbitrary captured double; persistence must name/refuse
that limitation rather than silently narrow it. No schema/new opcode or measured
Live parity claim is introduced by the RAM memory.

Optional graph workers are runtime scheduling, not project editing.
Graph/GraphHost/Session::setParallelRendering takes explicit measured frame and
eligible-width thresholds, with optional owner-created driver initialization.
Only independently safe nodes opt in; callback-affine providers stay serial.
Private render scratch/counters, fixed consumer sums and completion barriers
preserve existing audio/event contracts in both widths. See
`ENGINE-GRAPH-WORKERS.md` for lifecycle, FP environment, failure and benchmark
contracts. No project opcode/schema/blob-layout change.

Scale highlighting, ruler and Fold projections are runtime read concerns.
`engine::readClipScaleContext` resolves clip-local/inherited scale and explicit
tuning degrees coherently without reading notes or writing history. Held
contexts offer bounded pitch/membership queries with an explicit absolute
tonic; they do not retune notes. See `ENGINE-SCALE-CONTEXT.md` for limits,
non-octave periods and native pitch precision. No new opcode or schema.

Scale highlighting, ruler and Fold projections are runtime read concerns.
`engine::readClipScaleContext` resolves clip-local/inherited scale and explicit
tuning degrees coherently without reading notes or writing history. Held
contexts offer bounded pitch/membership queries with an explicit absolute
tonic; they do not retune notes. See `ENGINE-SCALE-CONTEXT.md` for limits,
non-octave periods and native pitch precision. No new opcode or schema.

`engine::readScaleTimeline` resolves the global per-key tuning map in one
owner snapshot. Held contexts apply at exact inclusive int64 tick boundaries;
nextChangeAfterTicks returns the next strictly later boundary without overflow.
Reads/compilation are owner work; bounded callback lookups allocate nothing.
The global map preserves separate clip-local overrides and never retunes stored
notes. See `ENGINE-SCALE-TIMELINE.md` for lifetime, budgets and errors. This is
a runtime read API, with no new opcode or schema.

Scala file parsing is owner preparation, not a log operation.
`engine::parseScala(bytes,id,name)` captures an explicit owned definition,
including the final repeat period and double cent offsets. See
`ENGINE-SCALA-IMPORT.md` for its bounded accepted grammar and errors. Native
writer integration logs resolved values; replay never reads the original file
or recomputes ratios. No opcode/schema/binary-layout change in the parser.

Scale highlighting, ruler and Fold projections are runtime read concerns.
`engine::readClipScaleContext` resolves clip-local/inherited scale and explicit
tuning degrees coherently without reading notes or writing history. Held
contexts offer bounded pitch/membership queries with an explicit absolute
tonic; they do not retune notes. See `ENGINE-SCALE-CONTEXT.md` for limits,
non-octave periods and native pitch precision. No new opcode or schema.

Scale highlighting, ruler and Fold projections are runtime read concerns.
`engine::readClipScaleContext` resolves clip-local/inherited scale and explicit
tuning degrees coherently without reading notes or writing history. Held
contexts offer bounded pitch/membership queries with an explicit absolute
tonic; they do not retune notes. See `ENGINE-SCALE-CONTEXT.md` for limits,
non-octave periods and native pitch precision. This read concern adds no opcode or schema; native scale writes are in §9.1.

Implemented controller-lane editing uses existing addressed `event_streams`
rows (`stream_kind='cc'`, ACTL), with channel 0..15 and controller 0..127:

- `cc.write {id,clip,channel,controller,points[]}` upserts explicit point IDs in
  that stream. `id` is its explicit SQL row ID, required even on creation.
  A point is `{id,time,value,tension?,curve?,flags?}`: positive uint64 ID,
  signed relative musical ticks, finite normalized double value 0..1, tension
  -1..1 (default 0), curve 0..5 (default Hold), byte flags (default 0).
  Duplicate incoming IDs, unsupported layouts, invalid values and slot/ID
  collisions refuse the whole op. Unmentioned points retain their raw bytes.
- `cc.delete {id,clip,channel,controller,point_ids[]}` removes only listed
  points. Empty/duplicate/missing selections refuse. Deleting the last point
  retains an empty addressed lane.
- `cc.restoreStream {id,clip,channel,controller,state:{data}}` is the captured
  inverse. Data is NULL for previous absence, or `{bytes}` containing compact
  CBOR binary (JSON callers may supply byte integers). Undo of first creation
  removes its row. Existing streams restore exact bytes, including future
  record tails/header flags/trailer. The same ID/address collision checks apply.

Streams are sorted stably by time. Updating an existing future record preserves
its full tail, while insertion of a new record into an unknown wider layout
refuses. Values retain full normalized precision; MIDI byte quantization belongs
at the host boundary. `controllers::readLanes` returns typed points, raw bytes and
per-lane errors; an unbound NULL address is distinct from channel/controller zero.
ClipNode.controllers publishes the same data immutably, and every field/raw
change participates in structural sharing. SnapshotBuilder reads all project
fields in one SQLite read transaction, including when another connection edits
between the controller and clip queries. The row model carries these lanes too.
`controllers::prepareWrite` finds the addressed slot, allocates explicit stream
and point IDs (zero input point IDs request new IDs), and validates the request
under one owner-side read transaction. It reserves/writes nothing. The returned
request contains all actual IDs. `commitPrepared` refuses a changed history head
or another file/open Store client; retain the document generation across async
work, as with the other prepared editing APIs. The window needs no SQL access.
These are editing/read APIs; ordinary controller-event playback/host routing is
the next increment. No schema or audio-callback change accompanies this one.

Playback prepares immutable per-track controller timelines off the callback.
Musical/linear placement, source offsets, clip loops and step tempo changes use
musical curve phase; curves use the shared six-shape evaluator. Overlapping lanes
at the same channel/CC use the latest clip start, then greatest clip ID. Empty
lanes send nothing; invalid lanes and aliases report preparation errors. Tempo
ramps retain the MIDI player's existing step-tempo warning. Preparation is capped
at one million spans and placed points. Controllers are sampled on a global
64-sample cadence and at exact point/placement/note boundaries, independent of
callback size, with exact state chased on start/seek.

Controller values remain doubles through graph compensation and VST3 mapped
parameters. CLAP uses MIDI bytes on a port advertising MIDI/MIDI-MPE, alongside
native CLAP notes when that is the chosen note dialect; quantization occurs only
at that boundary. Unsupported CLAP ports and unmapped VST3 controllers are counted
by each host's `unsupportedControllers()`. VST3 mapping discovery retains the
existing limitation to a reachable edit controller; no mapping is invented for a
two-component plug-in whose controller JUCE does not expose.

Owned sustain/sostenuto/hold-2 (CC64/66/69) are released before note-offs on stop,
seek, transport/clip wrap, mute or generation replacement. A source keeps pending
release ownership through graph delay and retries rejected/full-list releases,
including after the lane disappears. Ordinary CC values hold after a clip ends.
Controller pressure reserves note-event space and rotates addresses;
`MidiClips::droppedControllers()` counts deferred/refused updates. Callbacks use
fixed state and prepared arrays, with no Store access, allocation or locks.

**Retrospective MIDI input memory (ADR-0132 d9).** `engine/midi_memory.hpp`
provides a bounded `MidiMemory(inputId,trackId,sampleRate,noteChannelBase,capacity)`
for each explicitly eligible input/track. Allocate it before callbacks; default
capacity is 65,536 events, bounded 2..1,048,576. One producer serializes controls,
`push(Event,absoluteDriverSample)` and `advance(exclusiveBlockEnd)`, including
silent blocks. Armed OR monitored input listens independently of transport
play/record; neither gate retains input. Queue gate changes to that producer.
Input on/off, explicit discontinuity, backwards clocks, malformed musical input
and a pinned-slot drop begin a new epoch. Forward rests preserve the history.

Snapshots retain typed note-on/off, expression and controller events, double
values, uint64 identities, integer timestamps above 2^53, original packet frames
and coincident serial order. Input channels are declared rather than guessed:
MpeParser notes use 1..16, while clip notes use 0..15; controllers always address
0..15 and controller 0..127. The memory preserves the declared note convention
and never reconstructs MPE allocation. Note keys/velocities, controller addresses/
values, finite expression values and times are checked. Unassigned release IDs
remain unassigned for owner resolution. Addressed ParamValue/ParamMod events are
ignored without clearing musical history. Feed live input only, never playback
or controller-delivery feedback. No MIDI byte encoding or float narrowing occurs.

The single owner calls `snapshot(output,error)` off callback and receives an
immutable bounded history with input/track/format/convention/epoch, current
exclusive end sample and a `prefixDropped` notice when capacity evicted older
events. This is not a promise that a retained suffix contains complete notes or
a complete phrase. A concurrent overwrite/in-flight slot refuses the entire
snapshot with empty output; callers may retry. Slot ownership avoids data races;
the producer never waits for a reader, instead dropping/counting that event and
starting fresh history. Stats distinguish copied/dropped/ignored/invalid input.
Producer controls/parser/copy calls allocate, lock, log and perform file I/O
nowhere. Constructor/destructor and snapshot work belong off callback.

This increment supplies exact rolling input and the actual MPE-parser handoff,
not automatic host/UI wiring, phrase detection, Live tempo/loop inference or a
native committed clip. Owner-side Capture must resolve complete notes and missing
prefixes before preparing edits. Existing ANOT velocity bytes and AEXP floats
cannot preserve every arbitrary captured double; persistence must name/refuse
that limitation rather than silently narrow it. No schema/new opcode or measured
Live parity claim is introduced by the RAM memory.

The live `MpeParser` now tracks a plain channel's note identity per key, so
distinct-key chords can release out of order through the router and Capture.
An MPE member retains its existing single-note expression ownership. Unmatched
releases keep identity zero; reset clears the per-key map without recycling IDs.
Same-key retrigger retains the existing newest-identity release policy; this
increment does not invent FIFO ownership for ambiguous repeated MIDI-1 keys.

Ordinary CCs now emit typed Controller events on zero-based channels with their
original 0..127 address and normalized double value. This includes zone-master
controls and ordinary pedals on member channels. Member-channel CC74/106 retains
the original MPE per-note timbre path and exact 14-bit combination. Outside a
member, CC74/106 stays ordinary addressed CC. Other high-resolution CC pairs keep
both original addressed components, without pretending a missing LSB arrived.
The native controller route can recover their original MIDI-1 wire values.
Consumed data bytes with their status bit set are refused before state changes,
array indexing or normalization; a malformed timbre MSB cannot seed a later pair.
Sink refusal remains counted/nonblocking. Parsing/reset/delivery allocates and
performs callback I/O nowhere. Program change, SysEx, poly aftertouch and full
native Capture composition are outside this input correction; no schema/opcode
or measured Live parity claim is introduced.

Implemented controller-lane editing uses existing addressed `event_streams`
rows (`stream_kind='cc'`, ACTL), with channel 0..15 and controller 0..127:

- `cc.write {id,clip,channel,controller,points[]}` upserts explicit point IDs in
  that stream. `id` is its explicit SQL row ID, required even on creation.
  A point is `{id,time,value,tension?,curve?,flags?}`: positive uint64 ID,
  signed relative musical ticks, finite normalized double value 0..1, tension
  -1..1 (default 0), curve 0..5 (default Hold), byte flags (default 0).
  Duplicate incoming IDs, unsupported layouts, invalid values and slot/ID
  collisions refuse the whole op. Unmentioned points retain their raw bytes.
- `cc.delete {id,clip,channel,controller,point_ids[]}` removes only listed
  points. Empty/duplicate/missing selections refuse. Deleting the last point
  retains an empty addressed lane.
- `cc.restoreStream {id,clip,channel,controller,state:{data}}` is the captured
  inverse. Data is NULL for previous absence, or `{bytes}` containing compact
  CBOR binary (JSON callers may supply byte integers). Undo of first creation
  removes its row. Existing streams restore exact bytes, including future
  record tails/header flags/trailer. The same ID/address collision checks apply.

Streams are sorted stably by time. Updating an existing future record preserves
its full tail, while insertion of a new record into an unknown wider layout
refuses. Values retain full normalized precision; MIDI byte quantization belongs
at the host boundary. `controllers::readLanes` returns typed points, raw bytes and
per-lane errors; an unbound NULL address is distinct from channel/controller zero.
ClipNode.controllers publishes the same data immutably, and every field/raw
change participates in structural sharing. SnapshotBuilder reads all project
fields in one SQLite read transaction, including when another connection edits
between the controller and clip queries. The row model carries these lanes too.
`controllers::prepareWrite` finds the addressed slot, allocates explicit stream
and point IDs (zero input point IDs request new IDs), and validates the request
under one owner-side read transaction. It reserves/writes nothing. The returned
request contains all actual IDs. `commitPrepared` refuses a changed history head
or another file/open Store client; retain the document generation across async
work, as with the other prepared editing APIs. The window needs no SQL access.
These are editing/read APIs; ordinary controller-event playback/host routing is
the next increment. No schema or audio-callback change accompanies this one.

Playback prepares immutable per-track controller timelines off the callback.
Musical/linear placement, source offsets, clip loops and step tempo changes use
musical curve phase; curves use the shared six-shape evaluator. Overlapping lanes
at the same channel/CC use the latest clip start, then greatest clip ID. Empty
lanes send nothing; invalid lanes and aliases report preparation errors. Tempo
ramps retain the MIDI player's existing step-tempo warning. Preparation is capped
at one million spans and placed points. Controllers are sampled on a global
64-sample cadence and at exact point/placement/note boundaries, independent of
callback size, with exact state chased on start/seek.

Controller values remain doubles through graph compensation and VST3 mapped
parameters. CLAP uses MIDI bytes on a port advertising MIDI/MIDI-MPE, alongside
native CLAP notes when that is the chosen note dialect; quantization occurs only
at that boundary. Unsupported CLAP ports and unmapped VST3 controllers are counted
by each host's `unsupportedControllers()`. VST3 mapping discovery retains the
existing limitation to a reachable edit controller; no mapping is invented for a
two-component plug-in whose controller JUCE does not expose.

Owned sustain/sostenuto/hold-2 (CC64/66/69) are released before note-offs on stop,
seek, transport/clip wrap, mute or generation replacement. A source keeps pending
release ownership through graph delay and retries rejected/full-list releases,
including after the lane disappears. Ordinary CC values hold after a clip ends.
Controller pressure reserves note-event space and rotates addresses;
`MidiClips::droppedControllers()` counts deferred/refused updates. Callbacks use
fixed state and prepared arrays, with no Store access, allocation or locks.

**Retrospective MIDI input memory (ADR-0132 d9).** `engine/midi_memory.hpp`
provides a bounded `MidiMemory(inputId,trackId,sampleRate,noteChannelBase,capacity)`
for each explicitly eligible input/track. Allocate it before callbacks; default
capacity is 65,536 events, bounded 2..1,048,576. One producer serializes controls,
`push(Event,absoluteDriverSample)` and `advance(exclusiveBlockEnd)`, including
silent blocks. Armed OR monitored input listens independently of transport
play/record; neither gate retains input. Queue gate changes to that producer.
Input on/off, explicit discontinuity, backwards clocks, malformed musical input
and a pinned-slot drop begin a new epoch. Forward rests preserve the history.

Snapshots retain typed note-on/off, expression and controller events, double
values, uint64 identities, integer timestamps above 2^53, original packet frames
and coincident serial order. Input channels are declared rather than guessed:
MpeParser notes use 1..16, while clip notes use 0..15; controllers always address
0..15 and controller 0..127. The memory preserves the declared note convention
and never reconstructs MPE allocation. Note keys/velocities, controller addresses/
values, finite expression values and times are checked. Unassigned release IDs
remain unassigned for owner resolution. Addressed ParamValue/ParamMod events are
ignored without clearing musical history. Feed live input only, never playback
or controller-delivery feedback. No MIDI byte encoding or float narrowing occurs.

The single owner calls `snapshot(output,error)` off callback and receives an
immutable bounded history with input/track/format/convention/epoch, current
exclusive end sample and a `prefixDropped` notice when capacity evicted older
events. This is not a promise that a retained suffix contains complete notes or
a complete phrase. A concurrent overwrite/in-flight slot refuses the entire
snapshot with empty output; callers may retry. Slot ownership avoids data races;
the producer never waits for a reader, instead dropping/counting that event and
starting fresh history. Stats distinguish copied/dropped/ignored/invalid input.
Producer controls/parser/copy calls allocate, lock, log and perform file I/O
nowhere. Constructor/destructor and snapshot work belong off callback.

This increment supplies exact rolling input and the actual MPE-parser handoff,
not automatic host/UI wiring, phrase detection, Live tempo/loop inference or a
native committed clip. Owner-side Capture must resolve complete notes and missing
prefixes before preparing edits. Existing ANOT velocity bytes and AEXP floats
cannot preserve every arbitrary captured double; persistence must name/refuse
that limitation rather than silently narrow it. No schema/new opcode or measured
Live parity claim is introduced by the RAM memory.

The live `MpeParser` now tracks a plain channel's note identity per key, so
distinct-key chords can release out of order through the router and Capture.
An MPE member retains its existing single-note expression ownership. Unmatched
releases keep identity zero; reset clears the per-key map without recycling IDs.
Same-key retrigger retains the existing newest-identity release policy; this
increment does not invent FIFO ownership for ambiguous repeated MIDI-1 keys.

Ordinary CCs now emit typed Controller events on zero-based channels with their
original 0..127 address and normalized double value. This includes zone-master
controls and ordinary pedals on member channels. Member-channel CC74/106 retains
the original MPE per-note timbre path and exact 14-bit combination. Outside a
member, CC74/106 stays ordinary addressed CC. Other high-resolution CC pairs keep
both original addressed components, without pretending a missing LSB arrived.
The native controller route can recover their original MIDI-1 wire values.
Consumed data bytes with their status bit set are refused before state changes,
array indexing or normalization; a malformed timbre MSB cannot seed a later pair.
Sink refusal remains counted/nonblocking. Parsing/reset/delivery allocates and
performs callback I/O nowhere. Program change, SysEx, poly aftertouch and full
native Capture composition are outside this input correction; no schema/opcode
or measured Live parity claim is introduced.

`midi_capture.hpp` adds owner-side `capture::prepareMidi(store,snapshot,spec,error)`
and `commitMidi`. The spec supplies a fresh positive clip ID, project tick
position, explicit integer source range containing the complete retained input,
name and held-note closing policy. Source driver timestamps are independent of
project placement. A coherent read checks the regular unlocked/unfrozen target,
new identities and current step-tempo clock. Every note/CC boundary must roundtrip
its exact sample at the captured rate. Prefix-truncated histories, serial/time
gaps, unknown/duplicate ownership, mismatched releases, zero-duration notes,
tempo ramps and unsupported expressions refuse without writes. Existing ANOT
velocity bytes accept only source values that roundtrip exactly through the
MIDI-1 word; arbitrary doubles are refused, never silently quantized. CC values
remain full double. Held notes optionally close at the explicit Capture end with
release velocity zero. The caller retains document generation across async work.

Preparation returns one `capture.createMidi` request with fully resolved clip/
stream IDs and native binary images. Commit requires the same still-open Store
pointer/client and captured head, including NULL; a supplied transaction veto
cannot replace the head guard. Callback misuse returns `callback` before SQL or
allocation. The operation writes each stream once, so large captures avoid
per-note stream rewrites. One undo removes the new clip and cascaded streams;
Redo/replay uses only captured values, independent of input devices or UI state.

The closed opcode payload contains `clip` (the existing explicit MIDI clip.create
row packet), `controllers` (array of `{id,channel,controller,bytes}`), and optional
`notes` (`{id,bytes}`). Bytes are CBOR binary known-v1 sorted ACTL/ANOT images,
with exact record stride/count and no opaque trailer. At most 2,048 controller
addresses and 1,048,576 total points/notes are accepted. All stream IDs are
explicit positive int64; record IDs remain uint64. Fresh records are checked for
unique identities, times within the clip, valid domains/curves/velocity/channel/
probability/tuning and zero reserved/flag fields. Invalid/future images,
duplicate/occupied IDs/addresses, locked/frozen targets and late SQL failure
roll back clip, streams and journal together. Existing rows are never overwritten.
This adds an opcode, not a schema or binary-layout change.

Native playback retains its existing CC first-value chase, ordinary-control
hold, pedal-release ownership and cross-stream ordering. The exact raw input
transcript remains in memory; this is not a promise to persist its cross-stream
serial ordering or unknown controller state before a first point. Automatic Live
phrase/tempo/loop inference, full-double note/expression persistence and host/UI
Capture command wiring remain explicit subsequent work. No measured Live parity
claim is introduced by this native MIDI-1/CC Capture path.

### 9.6 Automation

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `automation.createLane` | e | S | cap | P0 |
| `automation.deleteLane` | e | S | cap | P0 |
| `automation.writePoints` | e | S | cap | P0 |
| `automation.deletePoints` | e | S | cap | P0 |
| `automation.movePoints` | e | S | cap | P0 |
| `automation.setCurve` | e | S | cap | P1 |
| `automation.restoreData` | e | S | cap | P1 |
| `automation.clearLane` | e | S | cap | P1 |
| `automation.setLaneEnabled` | e | S | cap | P1 |
| `automation.setLaneVisible` | e | N | cap | P1 |
| `automation.setDefaultValue` | e | S | cap | P2 |
| `automation.restoreLane` | e | S | cap | P1 |

The native lane lifecycle in `automation_lanes.hpp` implements create/delete,
clear, enable, visibility and default-value edits, all on the document owner.
`createLane(LaneSpec)` resolves every creation default into a complete `row`
payload. The outer `id` must match its explicit positive row ID. The polymorphic
owner must exist; parameter reference must be nonempty; time base is 0/1;
value domain is normalized/real/enum; finite defaults must respect optional
min/max; height is nonnegative. No handler allocates an identity or reads a UI
selection. An existing ID refuses creation instead of overwriting the lane.

Delete and clear payloads contain `id`; setters contain `id` and `value` (boolean
for enable/visibility, finite double within lane bounds for default value).
Clear removes points from every arrangement/clip stream while retaining data-row
IDs, clip addresses, header version/stride/flags and trailers. An unreadable
stream refuses the whole transaction, including any earlier clears. Delete
cascades all streams. `automation.restoreLane` captures a `state` object holding
the exact original row or null and every data row (`id`, `clip`, CBOR binary
`bytes`), and restores its own inverse. Legacy scalar metadata and malformed
opaque stream bytes are preserved by deletion undo, without applying new-lane
validation to the captured state. Explicit row/stream ID collisions fail within
the journal transaction. Each setter currently captures the full lane rather
than coalescing gestures; visibility has no forward engine impact. No schema,
Automation Arm, shape preset or playback change is introduced by lifecycle ops.

The native point-editing increment implements `writePoints`, `deletePoints`,
`movePoints`, `setCurve` and captured `restoreData` in `automation_edits.hpp`.
The lifecycle operations above supply lane creation and deletion.
All edits run on the document owner through `OpJournal`, never in a callback.

Every point-edit request contains `id` (explicit automation_data row ID), `lane`
(existing automation_lanes ID), and `clip` (JSON null for arrangement, otherwise
an existing clip ID). The complete address must match the stored row; an absent
row can be created only by writePoints and only if its lane/clip slot is free.
No handler generates IDs, reads UI selection, or consults the grid.

- `writePoints.points`: array of `{id,time,value,curve,tension}`. Positive full
  uint64 point IDs upsert existing points or insert new ones. Time is nonnegative
  int64 in the lane's unit, value is finite double, curve is 0..5 (SPEC §6.3.2),
  tension is finite -1..1 stored as float. Existing flags/reserved bytes survive.
- `movePoints.points`: `{id,time,value}` for existing points only; times/values
  are absolute, avoiding delta overflow and ambient snapping. Curve and tension
  survive. This increment captures exact inverses rather than coalescing moves;
  the UI can submit a completed gesture or a single batch.
- `deletePoints.ids` / `setCurve.ids`: nonempty arrays of existing positive
  uint64 IDs. setCurve also carries curve and tension. A duplicate or missing
  selection refuses the whole edit. Empty resulting streams remain addressed.
- `restoreData`: internal inverse packet with the same address, object `state` containing boolean
  `present` and CBOR binary `bytes`; restores original bytes or row absence.
  It captures its own inverse and does not normalize the original stream.

Edits sort whole records stably by time, including coincident-point order. Known
prefix edits and deletion retain header version/flags, per-record opaque suffixes,
reserved bytes and trailing data. Insertion into a wider, newer or unknown-flag
stream is refused because unknown fields cannot be invented. Duplicate/zero
stored point IDs and unreadable streams refuse edits. Undo/redo retains the exact
original layout even across reordering. Caller beforeCommit rejection rolls back
stream and journal together. Caller-provided shape points use the existing six
curve formulas; this increment adds no Live shape preset or parity claim, Automation Arm, clip-envelope playback, or automated send levels.

`automation_shapes.hpp` adds owner-thread shape generation and prepared inclusive
range replacement, expressed as the existing deletePoints/writePoints operations
in one transaction (no new opcode). The named templates are sine, triangle,
sawtooth/inverse, square, exponential rise/fall, logarithmic rise/fall and ADSR.
Geometry/defaults are explicit implementation behavior awaiting Live parity;
blocked official-manual access is not evidence that these presets match Live.

`ShapeSpec` carries the lane-unit range, low/high values, cycles and resolution.
Sine starts low and peaks halfway; triangle is linear; saws run to the opposite
endpoint and restart at each cycle; square uses Hold points at its half-cycle.
Curved ramps use the shared exponential/logarithmic formulas at tension +1.
ADSR uses explicit attack/decay/release fractions in hundredths of a percent and
a normalized sustain level; zero-duration phases create coincident steps. Sine is linearly sampled at the requested resolution; triangle, saws and ramps
retain exact vertices/native curve descriptors regardless of resolution. Cycles are
bounded 1..128 and steps/cycle 2..2048. All times are placed with integer quotient
and remainder, including beyond 2^53. Nonfinite/reversed ranges, unknown shapes,
overlapping ADSR phases and insufficient time resolution are refused.

`prepareShapeInsertion` reads one SQLite snapshot without writing/reserving IDs,
respects an outer read transaction, and resolves fresh positive uint64 point IDs
that avoid every existing identity. It refuses unknown AAUT insertion layouts,
ambiguous identities, wrong data/lane/clip addresses and lane-bound violations.
It replaces points in inclusive [start,end], retaining outside records; the
resulting points determine interpolation across either boundary, with no promise
of unchanged extrapolated sound outside a boundary lacking its own point.
`commitShapeInsertion` checks the original open Store/client and commits with an
explicit expected head, including an expected NULL head. Stale work changes
nothing; exact row absence/bytes are restored in one undo. Callers keep their
own document generation across asynchronous preparation. This adds no UI or
Automation Arm/recording behavior.

The Automation Arm recording foundation is `engine/automation_recording.hpp`:
one `AutomationRecorder` per explicit data/lane/optional-clip target and producer.
Arm defaults on and transport recording off. Manual parameter edits remain on
ParamOps; only manually sourced, explicitly timestamped gestures enter this
capture. Playback, echo and modulation events must not be submitted.

Exactly one producer calls controls/push; the UI queues gate changes to that
producer rather than racing it from another thread. A separate single consumer
calls drain/stats. Times are nonnegative int64 in lane units, never guessed from
drain time. Begin/Value/End preserve double values; ungestured Value begins an
implicit gesture; equal-time samples coalesce to the last value. Disarm/Stop
close the open gesture at the supplied time using the last value. A new Begin
closes the prior gesture at its last timestamp; the caller must delimit loop or
seek boundaries explicitly. Backward/nonfinite input invalidates the gesture.

Producer calls use a fixed SPSC ring and perform no allocation, Store/plugin I/O,
locks or logging. Overflow taints a whole gesture. Only completed healthy gestures
leave drain; rejected data is counted, never silently committed as a partial
curve. The consumer point budget also rejects the whole gesture. Gates and
accepted/dropped/ignored statistics are inspectable, with discarded-gesture
statistics owned by the consumer. Zero ring capacity safely rejects everything.

`prepareRecordedAutomation` uses `prepareAutomationWrite` to resolve fresh point
identities and one native delete/write transaction. Raw input must have zero IDs,
flags/reserved bytes and ordered finite values/tensions/known curves; replacement
uses the captured inclusive time range and preserves exact undo. Document/client
and history-head guards are identical to shape preparation. No schema or new
opcode is needed. This provides engine capture/commit APIs, not automatic wiring
of every host callback or the existing UI Arm button: those producers must supply
correct lane-unit timestamps and serialize controls on their producer thread.
No clip-envelope playback or send-level automation is claimed by recording capture.

Session's document-owner write policy is separate from the SPSC producer gates.
`setAutomationWriteState(armed,recording)` exposes the default on/off policy;
`beginAutomationWrite(lane)` requires both gates and an existing playable strip,
send or device lane. It temporarily excludes that lane from automation binding and
clears its previous ordinary override. Manual parameter/fader ops committed while
that lane is writing do not create an ordinary override. Other lanes retain the
usual override behavior. Re-Enable Automation does not cancel active recording.

The owner closes the producer gesture, drains and commits its captured points,
refreshes Session, and only then calls `finishAutomationWrite(lane,true)` to follow
the new curve. Turning a gate off prevents new writes but does not prematurely
resume old automation while captured data is pending. On capture/commit failure,
`finishAutomationWrite(lane,false)` explicitly leaves a normal manual override.
Deleted/disabled/unrecognized lanes drop stale writing state at rebuild. Unknown
or gated starts and finishing an inactive write are refused. A rebuild failure
returns false while retaining the requested policy for a subsequent owner-side
rebuild retry, as Re-Enable Automation does. These are owner-side
APIs; they introduce no Store calls/allocations into audio callbacks. The UI/host
coordinator must still map timestamps/targets and serialize matching recorder
producer controls; no automatic host callback wiring or measured Live parity is
claimed. No schema or opcode change is required.

Clip-relative `automation_data` now plays as absolute-value overlays on existing
track/device/send lanes. Values remain in the lane's declared normalized/real/enum
domain; this does not invent a relative-modulation mode absent from the format.
The arrangement stream (or default value) remains the fallback outside active
half-open clip placements. Latest audible clip start wins overlap, with higher
clip ID as the tie breaker; ending/muting/deleting the winner exposes the
underlying clip at its ongoing phase, then the arrangement.

Owner-thread compilation reads clip-relative points with their original curves,
content offset and musical loop window. It handles musical/linear placements,
tick/nanosecond envelopes, the pre-loop intro, loops starting inside their window,
and step tempos. Native curve phase survives trimming and tempo changes rather
than being renormalized between cropped samples. Nanosecond envelopes use elapsed
seconds plus the musical content offset; while looping, the musical loop phase
maps back through the source-relative step-tempo clock. All six shared curves and
coincident-point ordering are preserved. Device-event emission includes exact
rounded clip/point/wrap boundaries between the existing cadence ticks. Strip
processing retains its existing control grid and smoothing; both graph widths
read the same immutable envelope values. Transport seeks/wraps use the pure
sample-position lookup, with no callback allocations or Store access.

Compilation is bounded per lane: 1,048,576 native clip points/change boundaries,
16,384 accepted placements, and a corresponding repeat budget. Over-budget or
invalid clip streams are refused whole and named while valid neighbors and
arrangement playback remain. Empty streams do not mask arrangement data. The clip
must belong to the lane owner's track (including nested-device chains). Unknown
clocks, aliases, unsupported owners and loops shorter than one session sample
remain explicitly refused; clip-owned relative gain/modulation semantics are not
claimed. Ramp tempos remain a reported step-tempo fallback, as in arrangement
playback. No schema/opcode/UI change or measured Live parity claim is introduced.

Send gain automation addresses an existing track-origin `routing` row of kind
`send`, with `owner_kind="routing"`, its row ID as `owner_id`,
`param_ref="gain_db"` and `value_domain="real"` (dB). Other routing parameters,
domains and non-send targets are named and not played. Disabled lanes do not
bind; duplicate gain lanes report first-wins ownership. Arrangement and absolute
clip envelopes use the same compiler as faders; a send's clip must belong to its
source track. The send's second strip stage reads the lane with the existing
control cadence and five-millisecond smoothing at either precision, retaining
the existing silence threshold and +24 dB ceiling. Enable Send and solo remain
gates; source mute, Pre/Post behavior, source automation and track-delay/PDC
remain in the first stage. Graph lifetime holders retain both stages' programs.

For solo reachability, an enabled automated send potentially carries signal if
its arrangement/default or any clip point rises above silence. This conservative
whole-program decision keeps the edge reachable without rebuilding topology or
PDC during playback; actual gain always follows the current curve. An arbitrary
audio destination can therefore remain solo-reachable during a silent section
of a send that is audible elsewhere. A wholly silent lane does not open it.
This policy is explicit implementation behavior, not measured Live parity.

Manual `routing.setGain` changes override only that send's lane, independently of
equal numeric track IDs; Re-Enable resumes its curve. Send lanes participate in
Session's begin/commit/refresh/finish recording policy above, without permanently
overriding recorded manual movement. Changing Enable Send does not override gain.
Existing native lane/point ops supply editing and undo; no schema/new opcode or
UI/host producer wiring is introduced.

### 9.7 Devices, chains, macros

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `pluginRef.register` | e | N | pair `pluginRef.unregister` | P1 |
| `pluginRef.unregister` | e | N | cap | P1 |
| `device.insert` | e | G | pair `device.remove` | P0 |
| `device.remove` | e | G | cap | P0 |
| `device.move` | e | G | sym | P0 |
| `device.setEnabled` | e | S | sym | P0 |
| `device.setParam` | e | S | sym, coalescable | P0 |
| `device.loadState` | e | S | cap | P0 |
| `device.setPreset` | e | S | cap | P1 |
| `device.setExpressionRoute` | e | S | sym | P1 |
| `device.setPanel` | e | N | sym | P0 |
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

N7's `ParamOps::uiEvent` emits the existing `device.setParam` requests with
the same first-touch opener and normalization as a plug-in's own edit capture.
Commit each completed gesture's batch as one transaction; unbracketed UI edits
finish on the ordinary drain timer. Session-connected UI input overrides its
device automation lane before gesture completion, without an extra op. Runtime
overrides resume through Session's existing Re-Enable API. The UI and plug-in
captures have separate producers. `docs/ENGINE-UI-PARAM-CAPTURE.md` defines the
commit, lifetime, refusal and delayed-echo contract. No new vocabulary/schema.

Device clipboard (N18): readDeviceCopy(Store&,id), in device_copy.hpp,
returns the registered device.remove inverse as a device.insert payload, plus
the referenced plug-in metadata. It is a read on the Store's owner, including
read-only projects: no live plug-in call, state-byte copy or op. Stored params,
state hashes, panel order, expression route, nullable rack kind and remote host
reference are retained. Native order/latency/parameter flags remain signed
64-bit; device.insert accepts optional rackKind and remoteHost (null clears).

prepareDevicePaste captures an ordered batch with a caller-supplied new ID,
destination chain and optional before-device. It normalizes that chain's
integer ordinals in its existing (ord,id) order, inserts immediately before the
selection (or appends), and captures every move. One undo restores exact prior
ordinals, including legacy ties and large values. Paste checks the copied
plug-in's format/UID/shell identity and remote endpoint identity, so ID reuse
after history changes cannot select a different plug-in or host; labels and
binary relocation remain compatible. commitDevicePaste preserves
the captured expected head, including a null root. Keep the document generation
with an asynchronous result; cross-project state/ref import and nested rack
images are not supplied and return explicit refusals. The reader still exposes
a rack's root metadata and hasNestedChains. Flush pending host edit capture
before reading if the clipboard should include that gesture.

The browser calls prepareBuiltinInsert(store, uid, track, beforeDevice) then
commitBuiltinInsert with its captured expected head. Preparation allocates all
IDs and captures the complete typed plugin_refs row (including future columns
and BLOBs) in pluginRef.register's {id,row}; unregister takes {id} and captures
that full row for its inverse. A used reference cannot be unregistered (FK).
The batch creates a reference/track chain only when absent, orders the device
before the selected device or at the chain end, and commits one undo group.
No scanning, filesystem access or ambient ID allocation occurs in handlers.

N28's prepareSampleAssignment/prepareSampleClear produce ordinary media.import
(or relink) and device.loadState requests as one undo group. The built-in
sample:<slot-id> role stores CBOR content in state_blobs, identified by its
captured hash; no PCM/profile bytes enter the op payload. The first assignment
writes the empty baseline first, so live undo restores the empty slot before
removing the originally absent state row. Existing inverses preserve that role
and format hint exactly. See ENGINE-SAMPLE-SLOTS.md for the window's call path,
rate conversion, missing-file snapshot and immutable-content publication limit.



### 9.8 Routing and mixer

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `return.setSendTap` | e | G | cap | P1 |
| `routing.connect` | e | G | pair `routing.disconnect` | P0 |
| `routing.disconnect` | e | G | cap | P0 |
| `routing.setGain` | e | S | sym, coalescable | P0 |
| `routing.setPan` | e | S | sym, coalescable | P1 |
| `routing.setPreFader` | e | G | sym | P0 |
| `routing.setEnabled` | e | S | sym | P0 |
| `routing.reorder` | e | S | sym | P1 |
| `mixer.setVolume` | e | S | sym, coalescable | P0 |
| `mixer.setPan` | e | S | sym, coalescable | P0 |
| `mixer.setPanMode` | e | S | cap, sym | P1 |
| `mixer.setPanLeft` | e | S | cap, sym, coalescable | P1 |
| `mixer.setPanRight` | e | S | cap, sym, coalescable | P1 |
| `mixer.setWidth` | e | S | sym, coalescable | P2 |
| `mixer.setInputGain` | e | S | sym, coalescable | P1 |
| `mixer.setPhaseInvert` | e | S | sym | P1 |
| `mixer.setDelay` | e | G | sym, coalescable | P1 |
| `group.setSumming` | e | G | sym | P1 |
| `group.setSummingFlavor` | e | G | sym | P1 |
| `group.setSummingDrive` | e | S | sym, coalescable | P1 |
| `mixer.setPanLaw` | e | S | sym | P2 |
| `mixer.setVcaGroup` | e | S | sym | P2 |

N12: `mixer.setPanMode {id,mode}` selects `"stereo"` or `"split"` on an
existing mixer strip. `mixer.setPanLeft {id,pan}` and
`mixer.setPanRight {id,pan}` position each input of the stereo pair, finite
-1..+1. Values persist while dormant in the other mode; existing `mixer.setPan`
is dormant in split mode. The optional additive settings row starts at stereo,
left -1, right +1. Inverses capture prior scalar values and first-touch absence
(`_erase`); mode inverses use `_restore` to retain an unknown future mode exactly.
Those bookkeeping fields are for replay, not window controls. All updates retain
unrelated settings columns. Refresh Session and Snapshot after committing. Track
lanes `panLeft` and `panRight` use real -1..+1 units and the existing override /
Re-Enable Automation path. The window does not implement the panning matrix.

`return.setSendTap {id, pre}` is one return-owned Pre/Post choice (ADR-0203 d3).
It persists with zero sends and changes every incoming send as one undo step.
Ordinary new routing.connect sends inherit an explicit choice on their return.
A conflicting new per-edge routing.setPreFader edit refuses; the same value is
allowed. Historical scalar payloads retain their old apply semantics. Absent
preferences preserve legacy row taps and expose uniform or mixed state; no
migration rewrites existing audio. A producer for a new send captures the
uniform legacy choice as literal ops before creating it.

Its captured inverse uses internal optional restore {current, target}, each with
nullable choice and ordered incoming {id, pre} values. It checks current flags,
choice presence and edge set before writing. Restores update only tap columns,
never replace rows, so unrelated/future metadata remains in place. Undo of a new
preference restores absence. All writes belong to the journal transaction.

routing.disconnect now captures its full typed row into optional routing.connect
row metadata, including gain, pan, ordinal, enable, origin, channel-map BLOB and
future columns. Captured reconnects restore the raw row exactly and bypass tap
inheritance. Old six-field inverse payloads remain accepted. No UI state or
application settings enters a payload.

### 9.9 Markers, arranger, snapshots

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `marker.create` | e | S | pair `marker.delete` | P1 |
| `marker.delete` | e | S | cap | P1 |
| `marker.move` | e | S | sym, coalescable | P1 |
| `marker.rename` | e | S | sym | P1 |
| `marker.setColor` | e | S | cap | P1 |
| `marker.resize` | e | S | cap | P1 |
| `marker.setNote` | e | S | cap | P1 |
| `marker.setPrompt` | e | S | cap | P1 |
| `arranger.createSection` | e | N | pair `deleteSection` | P2 |
| `arranger.deleteSection` | e | N | cap | P2 |
| `arranger.resizeSection` | e | N | sym | P2 |
| `arranger.reorderChain` | e | N | sym | P2 |
| `arranger.setRepeats` | e | N | sym | P2 |
| `snapshot.create` | e | N | pair `snapshot.delete` | P2 |
| `snapshot.recall` | e | S | cap | P2 |
| `snapshot.delete` | e | N | cap | P2 |
| `snapshot.rename` | e | N | sym | P2 |

`marker.create {id,pos,time_base?,name?,kind?,length?,color?,note?,prompt?}`
creates a locator or cycle marker. Time base defaults to musical (0, ticks),
linear (1) uses nanoseconds. `pos` and `length` stay in that base's units.
Kinds are position (default), cycle, chapter and sync. A supplied length defaults
the kind to cycle; cycles require positive length. `marker.move {id,pos}` keeps
its base, and `marker.resize {id,length}` resizes cycles. `marker.rename {id,name}`
and `marker.setColor {id,color?}` edit identity/colour (omit colour to clear;
otherwise a Live palette index 0..69). Notes/prompts use `{id,text}`, including
an empty string to clear. Prompts are literal stored data, never executed here.
All changes rebuild the immutable marker snapshot, without rebuilding audio.

Delete's inverse captures the complete marker and optional `marker_details` row,
including unknown columns and typed BLOBs. It never overwrites an existing id.
Colour/resize inverse updates the row in place, retaining child rows; note/prompt
undo restores details presence and values. A batch shares one undo transaction,
and a later error rolls back rows and audit data together.

### 9.10 Media

**Retrospective audio memory (ADR-0132 d9).** `engine/audio_memory.hpp`
provides one `AudioMemory(inputId, sampleRate, channels, seconds=30)` per physical
input that may be captured. This is separate from transport recording. Exactly
one producer serializes `setListening(armed,monitored)` and `push(planarInput,
frames,firstStreamSample)`; UI gate changes must be queued to that producer.
The driver clock is explicit and independent of playhead seeks/loops. Either
gate keeps memory listening; switching between armed and monitored preserves
continuity. Switching listening on/off clears the logical retained history.
Discontinuities, clock restarts and rejected input begin a new epoch, so the
latest capture is always a contiguous suffix rather than spliced unrelated audio.

Storage is allocated before callbacks, with a 256 MiB per-input cap; invalid
identity/format/duration is refused at construction. Default stereo 48 kHz holds
11.52 MB of float sample payload plus chunk metadata and one spare chunk. The
latest configured duration is retained exactly, including irregular callbacks
and callbacks larger than the ring. Null input channels mean digital silence.
Counts/timestamps that overflow, negative values and callbacks over 1,048,576
frames are rejected and counted; zero frames is a no-op. The source is the
physical driver's float input, not a narrowed internal double graph bus.

The document owner calls `snapshot(output,error)` off callback. It returns an
immutable interleaved copy with input ID, epoch, first stream sample, format and
frames. Float bits, including signed zero/NaN payloads and above-unity samples,
are preserved without normalization, quantization or dither. The existing Float32
WAV writer can persist it on the owner before `media::prepareImport` below.
Chunk ownership prevents concurrent sample access: the producer never waits for
a reader, instead dropping/counting a pinned segment and starting fresh history.
Snapshots that encounter in-flight/overwritten chunks fail explicitly with empty
output; callers may retry. Producer gates/copies allocate, lock and perform file
I/O nowhere. Snapshotting, persistence and import are owner work. This API does
not automatically attach physical inputs, wire a Capture UI command, materialize
an undoable clip, infer MIDI tempo, or claim Live parity; those are subsequent
Capture integration work. No new opcode/schema is needed for the RAM buffer.

**Retrospective audio memory (ADR-0132 d9).** `engine/audio_memory.hpp`
provides one `AudioMemory(inputId, sampleRate, channels, seconds=30)` per physical
input that may be captured. This is separate from transport recording. Exactly
one producer serializes `setListening(armed,monitored)` and `push(planarInput,
frames,firstStreamSample)`; UI gate changes must be queued to that producer.
The driver clock is explicit and independent of playhead seeks/loops. Either
gate keeps memory listening; switching between armed and monitored preserves
continuity. Switching listening on/off clears the logical retained history.
Discontinuities, clock restarts and rejected input begin a new epoch, so the
latest capture is always a contiguous suffix rather than spliced unrelated audio.

Storage is allocated before callbacks, with a 256 MiB per-input cap; invalid
identity/format/duration is refused at construction. Default stereo 48 kHz holds
11.52 MB of float sample payload plus chunk metadata and one spare chunk. The
latest configured duration is retained exactly, including irregular callbacks
and callbacks larger than the ring. Null input channels mean digital silence.
Counts/timestamps that overflow, negative values and callbacks over 1,048,576
frames are rejected and counted; zero frames is a no-op. The source is the
physical driver's float input, not a narrowed internal double graph bus.

The document owner calls `snapshot(output,error)` off callback. It returns an
immutable interleaved copy with input ID, epoch, first stream sample, format and
frames. Float bits, including signed zero/NaN payloads and above-unity samples,
are preserved without normalization, quantization or dither. The existing Float32
WAV writer can persist it on the owner before `media::prepareImport` below.
Chunk ownership prevents concurrent sample access: the producer never waits for
a reader, instead dropping/counting a pinned segment and starting fresh history.
Snapshots that encounter in-flight/overwritten chunks fail explicitly with empty
output; callers may retry. Producer gates/copies allocate, lock and perform file
I/O nowhere. Snapshotting, persistence and import are owner work. This API does
not automatically attach physical inputs, wire a Capture UI command, infer MIDI
tempo, or claim Live parity. No new opcode/schema is needed for the RAM buffer.

`recording::captureMemory(store,snapshot,track,timelineStartSample,workFolder)`
materializes a valid snapshot as a private bit-preserving Float32 WAV take,
resolving this open project's identity. Timeline placement is explicit and
independent of the input stream timestamp. Then use the existing
`recording::prepareCommit` / `recording::commit` path to publish owned media and
create/attach the playing clip in one native undo step. Preparation and commit
require the same still-open Store handle/client plus the captured journal head;
a different/reopened connection requires fresh preparation. Caller document
generation remains necessary across async work. Failed/stale commits preserve
the private PCM for retry; failed materialization removes only its own work.
Callbacks are refused before file/database access. This reuses N20's publication,
dedup, rollback and exact sample-boundary musical placement rather than introducing
another asset/journal path. See ENGINE-AUDIO-RECORDING.md for recovery and limits.

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `media.import` | e | N | pair `media.unlink` | P0 |
| `media.unlink` | e | N | cap | P1 |
| `media.relink` | e | S | sym | P1 |
| ~~`media.embed`~~ | — | — | retired, ADR-0127 | — |
| ~~`media.extract`~~ | — | — | retired, ADR-0127 | — |
| `media.setName` | e | N | sym | P2 |

**Implemented media payloads (ADR-0143).** Path-taking callers use
`media::importMedia(store, path, id, importedUtc)` and
`media::relinkMedia(store, id, path)`. Relative input paths are relative to the
`.adi` folder. These APIs hash the file before submitting a captured payload;
the journal handlers never re-read files on undo, redo or replay. Duplicate
content returns the existing media id without logging a duplicate import.

**Import-to-clip (N5).** Call `media::prepareImport(store, path, mediaId,
importedUtc, request)` off the audio thread. It captures without changing project
rows/history. `result.audio` supplies source `sampleRate`, `channels`, `frames`,
including for deduplicated media. Append the captured request when its
`opType` is nonempty, and use `result.mediaId`. A reused id (`existing=true`)
usually needs no op; missing/stale paths instead produce a captured
`media.relink` so the imported clip can play from the matching file.
Append `clip.create` and `clip.attachAudio`, then commit the entire request list
once. A failure rolls back media, clip and attachment together. Undo removes
these rows in reverse order; Redo uses captured payloads without reading files.
Existing deduplicated rows keep their stored metadata; capture still supplies
current source dimensions to the caller.

**Audio input recording (N20).** Session::startRecording captures selected
AudioIo inputs into worker-owned WAV files. After stop/acknowledgment and owner
poll, recording::prepareCommit(store, AudioRecordingResult, ClipIds[]) captures
one media/import-or-relink + clip/create + audio/attach batch for the whole
pass; recording::commit applies it as one native undo step. No new opcode or
schema is required. Head, source-project, sample-boundary, file-hash and
publication guards preserve a take for retry on failure. Replay/redo use the
captured rows, never the recorder. See ENGINE-AUDIO-RECORDING.md for the caller's
document-generation/driver contracts, publication ownership and initial linear,
raw-input scope; per-device channel configuration remains win/window work.

`clip.attachAudio {id, media, frames}` attaches source frames `[0,frames)` to
an existing audio clip, with Warp off and all audio properties at defaults.
`frames` must be positive and within the media's frame count when known. Missing
media/clip, a non-audio clip or an existing attachment is refused.
`clip.detachAudio {id}` captures the attachment as its inverse. It refuses
edited source windows, warp, pitch/formant, reverse or channel mode so removal
cannot lose those edits. Neither op reads a file. The UI supplies explicit clip
position/length through `clip.create`; attachment frames are at the immutable
source rate, not the project rate (SPEC §4.3).

`clip.delete` now captures an existing audio attachment in the optional `audio`
object of its inverse `clip.create` payload. That object contains exactly the
`audio_clips` columns other than `clip_id`, including nullable warp-marker bytes
as an array. Recreating the clip restores that row in the same transaction;
deleting and undoing an imported clip therefore preserves its playable content.
The optional decode-cache argument to `prepareImport` lets a worker supply its
configured cache; otherwise the process cache is used. A temporary pin is
released when metadata capture ends.

- `media.import`: `{id, row}`. `row` contains exactly `hash_blake3`, `orig_name`,
  `rel_path`, `abs_path_hint`, `sample_rate`, `channels`, `frames`, `duration_ns`,
  `format`, `bit_depth`, `size_bytes`, `missing`, `imported_utc`, `peaks`.
  Hash is 64 lowercase hexadecimal digits. Text: hash/name/format (non-null),
  paths (nullable). Integer metadata is nullable except `missing` (0 or 1).
  Peaks is a nullable byte array. Fresh import captures file size and the
  caller's timestamp. Recognized audio also captures source rate, channels,
  decoded frame count, duration and detected format; native PCM/Float32 WAVs
  also capture bit depth. Non-audio retains nullable audio metadata and an empty
  format. Known corrupt/unsupported audio fails capture.
  `embedded` is always 0 and cannot be supplied. Inverse: `media.unlink {id}`.
- `media.unlink`: `{id}`. Captures every above column, including nulls and
  peaks, into its inverse `media.import`. Never deletes a disk file. Refuses
  embedded/chunk-backed rows and rows still used by audio clips or frozen
  tracks, so undo cannot lose a cascading reference or legacy bytes.
- `media.relink`: `{id, hash, paths: {rel_path, abs_path_hint, missing}}`.
  Hash must equal the row's identity; the path-taking API verifies the new
  file's BLAKE3. Captures the old three location fields as a symmetric inverse.
  Moving/renaming content does not allocate a new media id.

`adi_tool extract-media` is legacy maintenance, not the retired embedding op.
`collect-export` snapshots and relocates a copy, never the original. These
commands do not invent schema migrations or rewrite historical op payloads.

### 9.11 Transport, hardware, extensions

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `cue.setMode` | t | G | — | P1 |
| `cue.setOutput` | t | S | — | P1 |
| `cue.setVolume` | t | S | — | P1 |
| `cue.setTrack` | t | G | — | P1 |
| `cue.setReference` | t | G | — | P1 |
| `mixer.setSoloInPlace` | t | G | — | P1 |
| `transport.play` | t | N | — | P0 |
| `transport.stop` | t | N | — | P0 |
| `transport.seek` | t | N | — | P0 |
| `transport.setLoop` | t | S | — | P0 |
| `transport.setRecord` | t | G | — | P0 |
| `transport.setMetronome` | t | S | — | P1 |
| `controller.bind` | e | N | cap | P1 |
| `controller.unbind` | e | N | pair `controller.bind` | P1 |
| `controller.map` | h | N | pair `controller.unmap` | P2 |
| `controller.unmap` | h | N | cap | P2 |
| `controller.setRange` | h | N | sym | P2 |
| `controller.setTakeover` | h | N | sym | P2 |
| `extension.write` | e | N | cap | P1 |
| `extension.delete` | e | N | cap | P1 |

Cue controls are saved performance commands in Transport scope, not undo steps:
`cue.setMode {on}` selects Solo/Cue; `cue.setTrack {id,on}` changes the Cue selection
without editing a track's stored solo flag. `cue.setOutput {first}` selects a
zero-based stereo pair (even 0..62); `cue.setVolume {db}` accepts -150..+6 dB.
`cue.setReference {on,id?}` selects/engages exclusive reference audition. Omitted
id retains the target (or the native reference role when present). Invalid or
non-audio/Main targets fail. `mixer.setSoloInPlace {on}` controls return audibility
in Solo mode. The caller refreshes ProjectView and Session after success.

Cue is Post FX / pre-fader listening, including muted tracks. Reference audition
keeps its source's volume/pan automation, ignores its Main mute, and bypasses Main
and parent effects/faders with a 5 ms complementary swap. It reaches monitor and
Cue Out; normal Cue selections and clicks are suppressed during the exclusive
swap. Cue Volume scales both. Declaring an audition target alone does not change
its ordinary Main route: the native `tracks.role='reference'` designates the
permanently excluded project reference, once that format field exists (N38).
Offline `performanceState=false` ignores Cue/selection/swap but retains native
project-role exclusion and normal stored solo/mute behavior.

`transport.setLoop` stores `{on, start?, end?}` in the session layer. Bounds
are musical ticks, nonnegative start and end strictly after start. Omitted bounds
retain the last window (initially four 4/4 bars from zero). `transport.setMetronome`
stores `{on}` there. Both remain ephemeral performance ops: journaled for audit,
excluded from undo and musical digests. No inverse is built. A failed batch rolls
back session metadata along with its ops. Their Snapshot impact asks the caller
to refresh ProjectView and Session after commit. Snapshot `transport` carries the
saved values; Session applies loop changes on the driver at the next callback,
and renders the metronome only while playing, aligned to the graph's actual
compensated output latency, including loop wraps. Default output is the first stereo
pair, outside Main's device/fader chain; N23 routes it through Cue Out/volume. Offline
export/analysis sets `SessionSpec::performanceState=false` so saved monitoring
loop/click choices do not change rendered material.

`controller.bind` (ADR-0195 d3) takes `{id, binding}`; binding names all
14 non-id columns of `controller_maps`, including explicit nulls. It inserts
or replaces that project row and captures the previous row for one-step undo.
An explicit null binding clears the row (the inverse of first Learn).
`controller.unbind {id}` captures the complete removed row. Both replay only
resolved data; neither reads machine settings. Learn resolves an automation
lane to its owner and parameter, requires the port Remote role, and refuses
the current Focus Dial CC before constructing an op. Runtime dispatch checks
the live reservation first; a project binding that collides is reported as
shadowed and cannot fire. Hardware-scope map/unmap remain separate planned ops.

N10 controller Learn can address a mixer control before it has an automation
lane: learnControllerTarget(bindingId, kind, targetId, paramRef, sourceCC,
observedValues, policy, error) returns one resolved controller.bind request.
It never creates a lane or reads a project, device or app setting. The existing
lane-based learnController resolves its owner/param_ref and calls the same
producer. Remote permission, Focus Dial reservation, source/channel/CC,
absolute/relative detection and takeover use that explicit policy snapshot.
Native target kinds and positive IDs are validated by the producer; the commit
also verifies current target existence. A caller running an asynchronous Learn
session must keep its document generation and expected-head guard, as for other
producers. The op payload stays unchanged: captured binding fields only, no
machine policy or display-name resolution during replay. No schema/opcode change.

### 9.12 Remarks (ADR-0131)

| Op | S | E | Inv | P |
|---|---|---|---|---|
| `remark.add` | e | N | pair `remark.remove` | P2 |
| `remark.edit` | e | N | sym | P2 |
| `remark.resolve` | e | N | sym | P2 |
| `remark.remove` | e | N | cap | P2 |

A remark is project data, so it is edited through ops and undoes like anything
else (ADR-0131 d2). Payloads, all keys permanent:

- `remark.add {id, kind, target, param?, author, detail?, text, created, resolved?}`
  — `kind` is `track`, `clip` or `device`; `param` names a device parameter and
  is legal only when `kind` is `device`; `author` is `user` or `agent`.
- `remark.edit {id, text}` · `remark.resolve {id, resolved}` · `remark.remove {id}`.

Two §7 points specific to remarks:

- **`created` is in the payload.** The handler never reads a clock; the caller
  stamps the time once, and a replay writes the same row.
- **`author` is in the payload, and `ops.actor` is not consulted.** A handler
  cannot see who submitted it (§7.2: who ran an op must not change what it did).
  The agent's tool layer therefore **MUST** always send `author: "agent"` — the rule
  that lets a human tell who wrote what (ADR-0131 d3) is enforced where the
  agent's requests are built, not in the handler.

**210 ops** — 70 P0, 84 P1, 53 P2, 3 P3. Every P0 and P1 feature in FEATURES.md
has a corresponding op, or is explicitly a runtime concern with no persisted
state. Counted and consistency-checked by `tools/validate_ops.py`, not asserted.

---

## 10. Ops that are not undoable, and why that is fine

Plug-in scan child management is an owner-side application service, not a
project operation. `scan::runProcess` provides explicit executable/argument
launch, bounded stdout, deadline/cancellation and child-tree cleanup; it refuses
audio callbacks before allocation or OS access. See ENGINE-SCAN-PROCESS.md.
This process boundary alone does not change the CLAP/VST3 discovery methods or
implement their durable quarantine; those integration items remain separate.

Plug-in scan quarantine is shared application data in PluginRegistry version 2,
not a project op. Owner beginScan commits a pending marker before child launch;
opaque connection/attempt tickets constrain completion, failures retain a named
block, and explicit blockScan/unblockScan provide disable/re-enable. scanBlocks
exposes bounded sorted owned rows for the window. Callback entry points refuse
before SQL/allocation. See ENGINE-SCAN-QUARANTINE.md. Discovery/helper integration
is separate; this persistence API alone does not move existing scans out of
process or change project/runtime instantiation policy.

The integration in ENGINE-PLUGIN-SCANNER.md now connects both foundations:
ClapHost/Vst3Host directory discovery and the VST3 loader's direct path hint
always use the dedicated bounded helper, with whole path-bound metadata/format
validation before successful quarantine completion. Reports expose failures,
cancelled/unavailable service retains the old catalogue, and CMake places the
mandatory helper beside its consumers. Runtime instantiation remains in process;
no project op, project schema or blob layout changes.

The `transport.*` ops are **performance**, not editing. They write no project
state, and appending them to undo history would bury the last real edit under
navigation.

ADR-0037 removed the four `session.*` launch ops along with Session View, so the
transport scope is the whole of this category, including Cue/monitor commands
that save performance controls in the session layer. These do not reintroduce
Session View or launching.

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


The NI Stems worker producer (`media/stem_import.hpp`,
[ENGINE-NI-STEMS.md](ENGINE-NI-STEMS.md)) captures the original container and
aligned decoded WAVs, then submits existing `media.import`/`media.relink`,
`track.create`/`track.setColor`, `clip.create` and `clip.attachAudio` operations
in one native transaction. It adds no opcode or schema table. The master is
retained in the container, excluded from the playing stem group. Document
identity/generation/head guards and no-clobber file publication precede the
commit point; one Undo removes the whole group. NI export remains application
queue state, without project operations.

### Whole-track freeze state

track.setFreeze carries id, before and after objects. Each captured state holds
armed, frozen, media and render (the complete frozen_renders row or null). The
handler requires the current state to equal before, validates the closed after
shape, then installs it. The inverse swaps the two captures. No filesystem or
device loading occurs in apply. After a native batch, undo or redo, frozen
source fingerprints and flags must remain valid; edits to baked source material
require unfreezing first. Root fader, pan, width, phase, delay, mute, solo and
outgoing sends stay live. Freeze preparation/publication is described in
ENGINE-FREEZE.md.

Group freeze also uses group.setFreezeScope with id and closed before/after
objects containing members (null, or captured id/armed pairs). Installation
checks complete current descendant membership and original arm flags, disarms
members and stores their provenance. Removal restores those arm flags. Its
inverse swaps captures. Root cache, scope and media changes remain one native
transaction; group scopes are validated before commit/undo/redo.

Dedicated track bounce producers (`bounce_commands.hpp`,
[ENGINE-BOUNCE.md](ENGINE-BOUNCE.md)) compose the existing native vocabulary:
Bounce to New Track groups sibling reorder/create/color, optional Main connect,
media import/relink, complete clip creation, naming and optional original mute.
Whole-track Bounce in Place groups full material replacement, parked insert
bypass, baked input-gain reset and optional disarm. Both play Warp off and use
one undo transaction with project/document/head guards. No new opcode or schema
is required. In-place preparation refuses partial extents or live incoming
main/send audio rather than applying a baked mix twice.
