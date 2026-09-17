# The op vocabulary — step 3

**Status:** draft framework + first tranche. The full catalogue is not finished.

Every mutation to an `.adi` project is an **op**: a typed, scoped, attributed,
invertible command appended to the `ops` table in the same transaction that
performs it (SPEC §8.1, ADR-0003). This document defines what an op *is*. The
catalogue in §7 is the beginning of the list, not the end of it.

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

That last one is the kind of invariant you only think to write after being burnt,
and we adopt it verbatim.

**From Zrythm — the taxonomy, and one thing we had missed entirely.** Their
`UndoableAction` declares, per action, not just how to undo but *what it
disturbs*: `needs_pause()`, `needs_transport_total_bar_update()`,
`affects_audio_region_internal_positions()`. And their new `UndoStack` takes a
`CallbackWithPausedEngineRequester`.

**That is the finding that changes our design.** Not every mutation can be
applied to a running engine by swapping a snapshot. Some — changing graph
topology, altering sample rate, anything that must allocate inside the audio
domain — require the engine to be paused. Our op descriptor therefore declares
its engine impact (§4), which neither our SPEC nor the plan that preceded this
document accounted for.

**From neither: persistence.** This is worth stating plainly because it was
described to us the other way round. **Zrythm's undo is not persistent either.**
Its new stack is a Qt `QUndoStack` of `QUndoCommand`s; the legacy one is an
in-memory `UndoStack` of `UndoableAction` objects. MAGDA's terminates in JUCE's
in-memory `UndoManager`. Both die with the process.

So Zrythm gives us *how to carve edits into actions*, not how to persist them.
Nothing in either repo tells us how to write a durable, branching, CBOR-encoded
op log — that part is ours, and it is the part ADR-0018 is built on.

---

## 2. Op identity and naming

`domain.verb`, lowercase, dot-separated, stable forever once shipped:

```
track.create        clip.move           note.quantize
track.delete        clip.split          note.insert
track.reorder       clip.setLoop        automation.writePoints
device.insert       session.launchClip  transport.seek
```

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
    bool              coalescable;       // §6.3
};
```

`apply` and `buildInverse` sit on the descriptor for MAGDA's reason: there is one
place to look, and nothing can reach a different implementation than the one
declared.

**Registry invariants, asserted at startup — the process refuses to start if any
fails:**

1. Every op with `scope != Read` has a non-null `apply` **and** a non-null
   `buildInverse`.
2. Every op has a payload schema. Closed: unknown fields are rejected, not
   ignored.
3. No two ops share a name.
4. Every op name matches `^[a-z][a-zA-Z0-9]*\.[a-z][a-zA-Z0-9]*$`.

## 4. Engine impact — the Zrythm lesson

```cpp
enum class EngineImpact {
    None,          // no audio-domain effect at all (e.g. renaming a track)
    Snapshot,      // applied live by publishing a new snapshot — the common case
    GraphRebuild,  // topology changed; new graph prepared off-thread, then swapped
    RequiresPause, // cannot be done live: engine must stop, apply, restart
};
```

Declared per op, and enforced: an op marked `Snapshot` that tries to mutate graph
topology is a bug the runtime can catch, not a mystery dropout in the field.

`RequiresPause` must stay a small, explicitly justified set — every member is a
click or a stall the user will hear. Current expected members: sample-rate
change, audio-device change, and project close. Everything else should be
`GraphRebuild` or better.

## 5. Scopes

One per op, following MAGDA:

| Scope | Covers |
|---|---|
| `read` | Observation only. Never writes, never appends to the op log. |
| `edit` | Mutates project content: tracks, clips, notes, automation, devices. |
| `transport` | Play, stop, record, locate, loop, tempo-follow. |
| `session` | Clip launching, scene launching, follow actions. |
| `hardware` | Controller maps, audio/MIDI device binding, input routing. |

A client — UI, script, agent, remote — is granted scopes explicitly. An unknown
client gets `read` and nothing else. The AI-AGENT.md capability tiers (§2) are
now *defined* as scope sets rather than as their own parallel concept:

| Tier | Scopes |
|---|---|
| Observe | `read` |
| Propose | `read` + may *build* an op batch, may not commit it |
| Apply | `read`, `edit`, `session`, `transport` |

`hardware` is never granted to an agent. Repointing someone's audio device or
rewriting their controller map is not an editing action, and there is no request
worth doing it for.

## 6. Inverses

### 6.1 The rule

An op's inverse is computed **before** the forward op is applied, inside the same
transaction, from the state about to be overwritten. Not derived later, not
recomputed on undo. If `buildInverse` cannot produce a correct inverse, the op
does not commit.

### 6.2 Three shapes, in order of preference

1. **Symmetric** — the inverse is the same op with swapped arguments.
   `clip.move {id, from, to}` inverts to `clip.move {id, to, from}`. Cheapest,
   and most ops should be built to land here.
2. **Paired** — a dedicated opposite. `track.create` ↔ `track.delete`.
3. **State capture** — the inverse carries a CBOR blob of everything the forward
   op destroyed. Required for anything lossy: deleting a track, consolidating
   clips, destructive bounce, flattening a rack.

### 6.3 What state capture costs, and why it is bounded

State capture is the one that can get expensive, and the bound is not obvious —
so, explicitly: **media is never captured.** Deleting a track unlinks
`media_files` rows; it does not delete audio. The inverse of "delete a 40-clip
audio track" is therefore the track row, its clip rows and its device state — a
few hundred KB at worst — not gigabytes of audio. The content-addressed media
pool (ADR-0005) is what makes this true, which is a second payoff from that
decision.

Where an inverse would still exceed a threshold (proposed: 4 MB), the op is
marked `lossy_undo` and the user is told *before* it commits that this step
cannot be undone. Silently dropping undo is not acceptable; neither is a 2 GB op
log.

### 6.4 Coalescing

Dragging a fader emits hundreds of `mixer.setVolume` ops. Ops marked
`coalescable` merge with the immediately preceding op when actor, target and
`txn_id` match and they fall inside a time window (proposed: 300 ms). The
*first* op's inverse is kept — that is the one that returns to where the drag
started.

Coalescing happens before the op log is written, never after. A coalesced log is
the log.

## 7. Encoding

CBOR (RFC 8949), deterministic encoding per §4.2, via `nlohmann/json` (ADR-0016).

One concrete advantage over MAGDA's JSON transport worth recording: **CBOR has
real integer types.** Their code carries a `jsonInteger()` helper to prove a
value is integral and in range before casting, because "JSON has one numeric
type, so a field declared as a count arrives as a double whenever it was written
with a decimal point." Our tick positions are `i64` at 5,765,760 PPQ (ADR-0004)
and comfortably exceed the 2^53 exact-integer range of a double. In JSON that is
a silent truncation waiting to happen. In CBOR an `i64` is an `i64`.

Rules:

1. Deterministic encoding (RFC 8949 §4.2): shortest-form integers, sorted map
   keys, no indefinite-length items. Payloads reach content hashes and the text
   projection (ADR-0007), so byte-identical input must encode byte-identically.
2. Map keys are small integers, not strings — an op log is read far more often
   than it is authored, and `1` is not `"clipId"` repeated fifty thousand times.
   Key IDs are assigned per op type and never reused.
3. Unknown keys are **preserved** on read and re-emitted on write, matching
   ADR-0008 and ADR-0012. An older build must not strip a newer build's fields
   out of an op it is merely carrying.
4. No floats where a rational or fixed-point will do. Tick positions are
   integers. Gain is `f64` because it genuinely is.

## 8. First tranche

Enough to build steps 4–7 against. `E` = engine impact, `S` = scope.

| Op | S | E | Inverse |
|---|---|---|---|
| `project.setTempo` | edit | Snapshot | symmetric |
| `project.setTimeSignature` | edit | Snapshot | symmetric |
| `project.setName` | edit | None | symmetric |
| `track.create` | edit | GraphRebuild | `track.delete` |
| `track.delete` | edit | GraphRebuild | state capture |
| `track.reorder` | edit | Snapshot | symmetric |
| `track.rename` | edit | None | symmetric |
| `track.setMute` / `setSolo` / `setArm` | edit | Snapshot | symmetric |
| `track.setParent` | edit | GraphRebuild | symmetric |
| `clip.create` | edit | Snapshot | `clip.delete` |
| `clip.delete` | edit | Snapshot | state capture |
| `clip.move` | edit | Snapshot | symmetric |
| `clip.resize` | edit | Snapshot | symmetric |
| `clip.split` | edit | Snapshot | `clip.join` |
| `clip.setLoop` | edit | Snapshot | symmetric |
| `clip.setFade` | edit | Snapshot | symmetric |
| `note.insert` | edit | Snapshot | `note.delete` |
| `note.delete` | edit | Snapshot | state capture |
| `note.move` | edit | Snapshot | symmetric |
| `note.setVelocity` | edit | Snapshot | symmetric, coalescable |
| `note.quantize` | edit | Snapshot | state capture |
| `noteExpression.write` | edit | Snapshot | state capture |
| `automation.writePoints` | edit | Snapshot | state capture |
| `automation.deletePoints` | edit | Snapshot | state capture |
| `automation.setMode` | edit | None | symmetric |
| `device.insert` | edit | GraphRebuild | `device.remove` |
| `device.remove` | edit | GraphRebuild | state capture |
| `device.setEnabled` | edit | Snapshot | symmetric |
| `device.setParam` | edit | Snapshot | symmetric, coalescable |
| `routing.connect` | edit | GraphRebuild | `routing.disconnect` |
| `routing.disconnect` | edit | GraphRebuild | state capture |
| `mixer.setVolume` / `setPan` | edit | Snapshot | symmetric, coalescable |
| `session.launchClip` | session | Snapshot | not undoable — §9 |
| `session.launchScene` | session | Snapshot | not undoable — §9 |
| `session.setFollowAction` | edit | Snapshot | symmetric |
| `transport.play` / `stop` / `seek` | transport | None | not undoable — §9 |
| `media.import` | edit | None | `media.unlink` |
| `media.relink` | edit | Snapshot | symmetric |

## 9. Ops that are not undoable, and why that is fine

`transport.play` and `session.launchClip` are **performance**, not editing. They
write no project state and appending them to the undo history would fill it with
noise while burying the last real edit.

They still go through the registry — same validation, same scopes, same
attribution — and they still emit an op row, tagged `ephemeral`. Undo skips
ephemeral ops; the audit trail does not. So "what did the agent do at 14:32"
remains answerable, and Ctrl-Z still lands on the thing you actually changed.

Compaction (SPEC §8.3) may drop ephemeral ops first.

## 10. Still open

1. **The rest of the catalogue.** Comping, warping, racks and macros, freeze,
   track versions, arranger sections, chord track.
2. **Batch semantics.** An agent request is one `txn_id`, but a partial failure
   mid-batch needs a defined outcome. Presumed all-or-nothing, matching SPEC
   §3.5; not yet written down.
3. **Key-ID assignment.** §7 rule 2 needs an actual registry of integer keys per
   op type, and a rule for extending one.
4. **Selection.** Zrythm models selection as part of the action
   (`ArrangerSelectionsAction`). We have not decided whether selection is op
   state, session state, or neither. It affects almost every editing op, so it
   should be decided before the catalogue is finished.
