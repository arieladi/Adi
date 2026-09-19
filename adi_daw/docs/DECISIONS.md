# ADI DAW — decision log

One entry per decision that would be expensive to reverse. Append only; when a
decision changes, add a new entry that supersedes the old one rather than editing
history. Same convention as `VST-ADI/ARCHITECTURE.md`.

**Status values:** `DECIDED` · `DECIDED (direction)` · `PROVISIONAL` (will
revisit before v1.0) · `SUPERSEDED BY ADR-nnnn` · `OPEN`

`DECIDED (direction)` means the *direction* is settled and will not be
relitigated, while the design it implies is deliberately not taken yet. It is
not `PROVISIONAL`, which means the decision itself may change. An entry using it
must name what is still open.

---

## ADR-0001 — The project file is a SQLite database — `DECIDED`

**Context.** Alternatives were XML-in-zip (Ableton, `.dawproject`), JSON, a
custom binary chunk format (RIFF-like), or plain text (Reaper `.RPP`).

**Decision.** SQLite 3, `application_id` = `0x41444931`.

**Why.** Save cost becomes O(delta) rather than O(project), which is what makes
autosave-during-playback possible; commits are atomic so a crash cannot produce a
half-written project; a second connection can read a consistent snapshot while
the user edits, which gives us background render and analysis for free. The
decisive argument, and the one most easily missed: **unknown data survives by
default**, because we never rewrite the file wholesale. A full-rewrite format has
to consciously carry forward every field it doesn't understand, and every writer
bug is silent data loss.

**Cost accepted.** The file is opaque to `git diff` and to text merge. Mitigated,
not eliminated, by ADR-0007.

Full argument: [`format/RATIONALE.md`](format/RATIONALE.md) §1.

---

## ADR-0002 — The extension is `.adi` — `DECIDED`

**Decision.** `.adi`, MIME `application/vnd.adi.project`, UTI
`org.adidaw.project`. `.adibundle` is an alias extension signalling embedded
media, with no semantic difference (ADR-0005).

**Known collision.** ADIF amateur-radio contact logs also use `.adi`. There is no
registry to conflict with, no domain overlap, and content-sniffing is
unambiguous (SQLite magic + `application_id`). Accepted knowingly.

---

## ADR-0003 — Every mutation is a typed op, and the op log lives in the file — `DECIDED`

**Context.** The original proposal treated a serialized action log as one feature
among several — a way to get persistent undo.

**Decision.** It is the backbone. Every mutation to Layers 1, 2 and 4 appends a
row to `ops` **in the same transaction that performs it**. There is no valid
state in which the project changed and the log did not.

**Why this is not just about undo.** One decision buys: persistent undo across
restarts; a branching undo *tree* rather than a stack; sub-save crash recovery;
a scripting API; the sync unit for eventual collaboration; reproducible bug
reports; and — the reason it had to be decided at format time — **the entire
safety model for the AI agent**. An agent that can only emit ops is attributable
(`ops.actor`), undoable in one keystroke (shared `txn_id`), auditable by query,
and structurally incapable of doing anything a user couldn't.

An agent with direct access to the project model is frightening. An agent that
can only append ops is an ordinary feature. That difference is this ADR.

See [`AI-AGENT.md`](AI-AGENT.md) §1.

---

## ADR-0004 — Dual time domain; musical time is i64 ticks at PPQ 5765760 — `DECIDED`

**Context.** Ableton is beat-native; Cubase supports musical *or* linear time per
track. "Combined" means the format cannot pick one.

**Decision.** Every positioned object declares `time_base`: musical (i64 ticks) or
linear (i64 nanoseconds). Exactly one position column is populated.

`ADI_PPQ = 5765760 = 2^7 × 3^2 × 5 × 7 × 11 × 13`, chosen so that every tuplet
from 2 to 16 is *exactly* representable, along with 128th notes and every common
interchange PPQ (24/96/192/384/480/960). Logic's 960 and Cubase's 480 stop at
the common cases — both divide cleanly by 2, 3, 4, 5, 6 and 8, but `960/7` and
`480/7` are not integers, so every septuplet is rounded, and 11- and 13-tuplets
are worse. We represent all of them exactly.

Linear time is nanoseconds, not samples, because a project's sample rate can
change and every sample-domain position would silently become wrong. Sample
exactness is preserved where it actually matters — read positions inside source
files, stored in frames at that file's own immutable rate.

**Rejected:** doubles-as-beats (Ableton) — precision degrades over long projects
and makes exact merges impossible. Rationals — exact, but painful to index, sort
and do arithmetic on, for a benefit a well-chosen PPQ already delivers.

---

## ADR-0005 — Media is referenced by default; embedding is a flag, not a format — `DECIDED`

**Decision.** `media_files` is a content-addressed pool (BLAKE3). By default
media is referenced. **Collect & Embed** populates `media_blobs`; **Extract
Media** empties it. A `.adi` with embedded media is *the same format*, read by
the same code.

**Why content addressing.** One column gives deduplication, integrity
verification (catch a truncated or replaced file *before* it renders as silence)
and relink-by-content rather than relink-by-guessing-at-filenames.

**Rejected:** the usual project / bundle / archive split. Three formats, three
code paths, and a conversion step. One schema with a flag is reversible at any
time and strictly simpler.

---

## ADR-0006 — Session View is core tier, not a vendor extension — `DECIDED`

**Context.** The original proposal put Ableton's clip matrix in a vendor sandbox
blob, preserved but not understood.

**Decision.** `scenes` and `clip_slots` are Layer 1, alongside the arrangement.

**Why.** It is the premise of the project. "Ableton and Cubase combined" means
the clip launcher and the linear arrangement are peers in the data model, both
always present, with clips referenced from either. Demoting the launcher to
opaque data would reproduce exactly the bolted-on, second-class feel that every
other DAW's clip launcher has.

---

## ADR-0007 — A text projection exists, but the binary file is primary — `DECIDED`

**Context.** ADR-0001 costs us `git diff`, and our users are exactly the people
who put projects in version control.

**Decision.** `adi export --text` produces a deterministic, canonical,
line-oriented rendering for diffing and review. It is **derived**, never
authoritative, and round-tripping through it is not a supported workflow.

**Rejected:** making text primary (Reaper's model). The save-performance argument
that motivates ADR-0001 applies to the file people actually save into, hundreds
of times a session.

**Open:** whether the text projection is also an *import* path, and if so how
conflicts with the op log are resolved. Deliberately unanswered.

---

## ADR-0008 — Every core-tier BLOB carries a 16-byte self-describing header — `DECIDED`

**Decision.** `fourcc | version | rec_size | count | flags`, then
`count × rec_size` bytes. **Readers MUST stride by the header's `rec_size`, never
by `sizeof(their own struct)`.** Unknown tail bytes in a record are a newer
version's fields: skip them, preserve them byte-for-byte on save.

**Why.** This is the forward-compatibility mechanism for binary data. It lets us
add a field to every note in every project in the world without a schema
migration and without breaking older builds — which, given how many per-note
fields DAWs have accreted over thirty years, we will certainly need.

---

## ADR-0009 — No core-tier BLOB may span more than one editable object — `DECIDED`

**Decision.** One notes blob per MIDI clip. One automation blob per lane per
clip. Never per track, never per project.

**Why.** SQLite's write unit is the page, not the row. "Store MIDI as blobs" is
right, but a blob that covers a whole track means editing one note rewrites the
whole track — reintroducing precisely the O(project size) save cost that
ADR-0001 exists to escape. This rule is the difference between a fast format and
a slow one wearing a fast format's clothes.

---

## ADR-0010 — The file format is a persistence layer, never the runtime model — `DECIDED`

**Context.** The original proposal claimed the audio engine could read stored
blobs "directly into memory buffers".

**Decision, stated as a prohibition because it will be tempting:** **the audio
thread never touches SQLite.** No file I/O, no allocation, no mutex, no syscall,
no `sqlite3_step` — not at background priority, not "just for reads". The DB
loads into a heap model on the message thread, which publishes immutable
snapshots to the audio thread by atomic pointer swap.

**Why it is in the format doc and not only the architecture doc:** it changes the
schema. We optimise on-disk layout for *fast bulk decode*, which is what actually
justifies the BLOBs — a memcpy-and-fixup of 4,000 notes beats 4,000 row reads by
two orders of magnitude. It justifies them for loading, not for playback.

---

## ADR-0011 — A missing plugin never causes a device to be dropped — `DECIDED`

**Decision.** When a plugin cannot be instantiated, the reader preserves
`plugin_state` byte-for-byte, keeps the device in the chain as a bypassed
placeholder retaining its position, routing and automation bindings, surfaces the
plugin's identity to the user, and re-injects the state verbatim if it becomes
available. `plugin_params` mirrors every parameter by name and value as a
human-readable fallback.

**Why.** Silently dropping a device rewires the signal path, and the user finds
out at mixdown. This is a `MUST` in the spec, not a quality-of-implementation
detail.

---

## ADR-0012 — Unknown data is preserved; unknown *essential* data warns — `DECIDED`

**Decision.** Extension rows carry `criticality ∈ {advisory, essential}`. Readers
MUST preserve rows they don't understand and MUST NOT refuse to open because of
one. A reader that encounters an unknown `essential` extension MUST warn before
render, export or bounce.

**Why the second half matters.** Preservation alone protects the file but not the
user: if v1.0 ignores a chord track it can't read and the user *renders*, they
get silently wrong audio and no warning. Silent degradation at render time is
data loss with extra steps.

---

## ADR-0013 — Lives in the `Adi` monorepo for now; graduates before going public — `PROVISIONAL`

**Decision.** `adi_daw/` as a top-level directory in `github.com/arieladi/Adi`,
matching the `VST-ADI/` and `finance/` pattern.

**Why provisional.** A monorepo shared with a personal website, a finance app and
a scripts folder is a poor home for an open-source project that wants outside
contributors: issues, PRs, releases, CI and stars all belong to the wrong thing,
and `git clone` drags in unrelated history. Fine while this is design work.
**Revisit before the first public commit** — at that point it should become
`github.com/arieladi/adi-daw` with its own history.

**Confirmed 2026-09-17**, staying here for now. Note for whoever does the split:
`git subtree split -P adi_daw` preserves this directory's history into the new
repo, so the move is cheap *provided* nothing outside `adi_daw/` ever comes to
depend on it. Keep the boundary clean.

---

## ADR-0014 — C++ with JUCE — `DECIDED` (2026-09-17)

**Decision.** C++ for the engine and application, JUCE for audio device I/O,
plugin hosting and the GUI substrate.

**Why.** A DAW is, in bulk, a plugin host and an audio-I/O layer. JUCE supplies
both on every target platform from day one; without it, the first months are
spent writing ASIO/WASAPI/CoreAudio/ALSA backends and a VST3 host before
anything makes a sound. Every plugin SDK is C++-first, so the FFI cost is zero.
And the existing familiarity is real and specific — `VST-ADI` already has a
working JUCE/CMake toolchain and a headless VST3 validator built against it.

**Rejected: Rust.** The safety argument is genuinely strong and points at exactly
the code most likely to be subtly wrong — the lock-free snapshot handoff in
ADR-0010. It would also be more attractive to open-source contributors in 2026.
It loses on the thing that dominates the work: VST3 and AU hosting from Rust
means FFI to C++ anyway, through crates that are considerably thinner than JUCE.

**Rejected: Rust core with a C++ hosting shim.** Most of the safety benefit, at
the cost of a two-language build and an FFI boundary crossed on the hot path.
The right answer for a funded team; too much overhead for the first release here.

**Consequence.** ADR-0010 loses its compiler-enforced safety net. The snapshot
handoff between message thread and audio thread must therefore be a small,
isolated, heavily reviewed and heavily tested piece of code, written once and
not casually modified. In Rust the borrow checker would enforce this; in C++ it
has to be enforced by discipline, so it must be **structural** — one type, one
file, an explicit API that makes the wrong thing hard to express.

**Consequence.** JUCE's free path is GPLv3, which decides ADR-0015.

---

## ADR-0015 — GPLv3 — `DECIDED` (2026-09-17)

**Decision.** GNU General Public License v3.0. `adi_daw/LICENSE` holds the
canonical text.

**Why.** It is compatible with JUCE's free licensing path (ADR-0014), which
would otherwise cost a commercial JUCE licence. It has direct precedent in this
exact space — Ardour, LMMS, and Vital, which we already fork in `VST-ADI/`. And
it matches the premise: an open DAW with an open format, where a fork stays open.

**Cost accepted.** No closed-source derivatives, including our own later. Some
proprietary SDK integrations become awkward or impossible.

**Consequence: VST2 is out.** FEATURES.md listed VST2 hosting as P2,
"licensing-dependent". It now resolves to no on two independent grounds: the
VST2 SDK has not been obtainable from Steinberg for years, and its licence terms
were never GPL-compatible. VST3, CLAP, AU and LV2 cover the ground. This is a
loss for old projects and old plugins, and it is not recoverable — worth being
plain about rather than leaving as a dangling maybe.

**Open, deliberately.** Whether to require a CLA from contributors. A CLA keeps
relicensing possible later; it also deters exactly the contributors a GPL
project attracts. Not urgent until there are outside contributors, but it gets
much harder to add after there are.

---

## ADR-0016 — Op payloads are CBOR, via nlohmann/json — `DECIDED` (2026-09-17)

**Context.** SPEC §12.1 left `ops.payload` / `ops.inverse` unencoded and flagged
it as essentially unchangeable once the first op type ships.

**Decision.** CBOR (RFC 8949), encoded with `nlohmann/json`'s `to_cbor` /
`from_cbor`.

**Why CBOR.** Self-describing, so an op written by a newer version can still be
inspected and preserved by an older one — the same forward-compatibility posture
as ADR-0008 and ADR-0012. Deterministic encoding is specified (RFC 8949 §4.2),
which matters because op payloads end up in content hashes and in the text
projection of ADR-0007. Binary, so the log stays compact. And readable with
off-the-shelf tools when debugging, which a hand-rolled TLV would not be.

**Why nlohmann/json rather than a dedicated CBOR library.** Header-only, MIT, no
schema compiler in the build, and its CBOR codec is mature. The same value type
projects cleanly into the JSON Schema an op registry needs (ADR-0018), so one
library serves both the on-disk encoding and the agent's tool schemas.

**Rejected: FlatBuffers / Cap'n Proto.** Zero-copy reads are the wrong thing to
optimise for — op payloads are small and read on the message thread, never in
the audio callback (ADR-0010). The cost is a schema compiler in the build and a
generated-code step, for a benefit we do not need.

**Rejected: MessagePack.** Very close call. CBOR wins on having a specified
deterministic encoding and an IETF standard behind it.

---

## ADR-0017 — External code lives in two directories with different rules — `DECIDED` (2026-09-17)

**Decision.** `third_party/` holds permissively-licensed code we link against and
ship. `reference/` holds GPL-family code we *read* and which is never on the
include path. Both are gitignored; each clone is its own repo, as with
`VST-ADI/vital`. `tools/fetch_external.sh` provisions both.

**Why it is a decision and not a directory layout.** We are GPLv3 (ADR-0015), so
what we may copy from is a design constraint. The sharp case is **Zrythm, which
is AGPL-3.0, not GPL-3.0** — the incompatibility runs one way, and AGPL code
cannot come into this project without dragging §13 network obligations onto the
combination. Zrythm is therefore marked read-for-design-only: we study how it
decomposes edits into undoable actions, and write our own code. Architecture is
not what copyright protects; source lines are.

**Also recorded because both mistakes cost real time:** GitHub reports
`NOASSERTION` for Zrythm, Tracktion and Ardour alike while their actual terms
differ sharply (AGPL-3.0, GPL-3.0-or-later-or-commercial, and GPL-2.0-or-later
respectively). Check `COPYING`/`LICENSE` in the tree, never the sidebar. And the
Helio repo we were pointed at, `Ahornberg/helio-workstation`, is a fork stale
since January 2022; the live one is `helio-fm/helio-sequencer`.

Full inventory and rationale: [`EXTERNAL-CODE.md`](EXTERNAL-CODE.md).

---

## ADR-0018 — Our relationship to MAGDA and Tracktion Engine — `SUPERSEDED BY ADR-0018R`

**The situation.** [MAGDA](https://github.com/Conceptual-Machines/magda-core) is
an actively developed GPL-3.0 DAW on C++20 + JUCE + Tracktion Engine that already
ships Session/Arrangement/Mix views with clip launching, nestable racks with 16
macros and 16 bezier LFOs per device, a piano roll with CC lanes, and an in-app
AI agent that generates and executes a DSL. That is a large overlap with the
design in this repository, and it was not known when ADR-0001..0015 were written.

**What is genuinely still ours.** MAGDA inherits Tracktion Engine's persistence:
`ValueTree` serialized to XML, undo via JUCE's in-memory `UndoManager`. SQLite
appears in their tree only for plugin metadata and the media database, not the
project file. Every argument in `format/RATIONALE.md` therefore stands, and the
one that matters most for the agent — **undo that persists across restarts and
branches** — is exactly what a `ValueTree` undo stack cannot do, and exactly what
is hardest to retrofit into an engine built around one.

**The options, unranked and undecided:**

1. **Stay independent.** Own engine, own format. Most work, most control, and the
   `.adi` differentiator stays sharp.
2. **Build on Tracktion Engine, keep our own persistence.** Skips years of plugin
   hosting and graph work. The tension is real: TE's model *is* `ValueTree`, so
   layering an op log and SQLite over it means fighting the engine's grain.
3. **Contribute the format to MAGDA.** Fastest route to it existing at all. We
   stop owning the direction, and MAGDA requires a CLA.
4. **Fork MAGDA.** Inherits a working DAW and a GPL-compatible licence. Inherits
   its architecture too, including the persistence we specifically rejected.

**Not deciding this yet is the right call** — it should be made after reading
their `OperationRegistry` and TE's `ValueTree` layer properly, which is step 3
work. But it must be decided before any engine code, because it invalidates or
confirms most of what comes after.

**Related open item:** MAGDA requires a contributor CLA, which bears on the CLA
question left open at the end of ADR-0015.

---

## ADR-0018R — Stay independent; JUCE for hosting, Tracktion as reference — `DECIDED` (2026-09-17)

**Supersedes ADR-0018 above.** Cited as `ADR-0018R`; the two entries are
distinct and the number is never reused. See ADR-0028.

**Decision.** Own engine, own project model, own persistence. JUCE for audio I/O,
plugin hosting and GUI. Tracktion Engine and MAGDA stay in `reference/` as
design references, not dependencies.

**Why.** Tracktion Engine's model *is* `ValueTree` — its state, its undo, its
serialization and its change propagation are all built on it. Grafting an
event-sourced, SQLite-backed, branching op log onto that means fighting the
engine's grain in every file we touch, and we would spend more time subverting
its assumptions about how state is held than building a DAW. The persistence
model is the one thing this project is actually *for* (ADR-0001, ADR-0003), so
inheriting an engine that contradicts it is the wrong trade.

Confirmed by reading both: MAGDA terminates its mutations in JUCE's in-memory
`UndoManager`, and **Zrythm's undo is also in-memory** — a Qt `QUndoStack` in the
new model, an in-memory `UndoStack` in the legacy one. Neither persists. Durable
branching undo is not a feature we would be duplicating; it does not exist in
either, and it is not retrofittable into the engine MAGDA is built on.

**The distinction that makes this affordable, and that the framing of the
question obscured: JUCE is not Tracktion Engine.** Rejecting Tracktion does not
mean writing plugin hosting or device I/O from scratch —
`juce::AudioPluginFormatManager` and `juce::AudioDeviceManager` are the hosting
layer and are entirely independent of Tracktion's `ValueTree` model. We keep the
part that saves years and decline the part that would cost them.

**What we still take from `reference/`:** Tracktion for latency-compensation
maths and graph construction, Ardour for the editing maths, Helio for pure-JUCE
timeline rendering, MAGDA for the operation registry (now ADR-0020), Zrythm for
action taxonomy — design only, it is AGPL (ADR-0017).

---

## ADR-0019 — Snapshot reclamation is epoch-based, not a return queue — `DECIDED` (2026-09-17)

**Context.** ADR-0010 bans the audio thread from touching SQLite and specifies an
immutable snapshot published by atomic pointer swap. It did not say who frees the
old snapshot, and the audio thread cannot call `delete`.

**The proposal considered** was a two-ring "janitor" pipeline: an SPSC
`publish_ring` carrying new snapshots to the audio thread, and a second SPSC
`reclaim_ring` in which the audio thread pushes retired snapshots to a background
thread that frees them.

**Rejected, for three reasons.**

1. **It has an unhandled failure path on the hot path.** An SPSC push can fail
   when the ring is full — and it will be full exactly when the janitor thread
   has been descheduled, which is precisely when the system is under stress. The
   audio thread is then holding a pointer it can neither hand off nor free, and
   the design says nothing about what happens next. That is the shape of a bug
   that passes every test and leaks in the field, or worse, invites someone to
   add a retry loop in the callback.
2. **There is a simpler mechanism with no failure path at all.** The audio thread
   publishes *which* snapshot it is using; the writer frees anything else. One
   atomic store in the callback, no queue, nothing to overflow.
3. **It answers the wrong question.** Reclamation is the easy half. The hard half
   is that a full snapshot per edit is O(project size) to build — reintroducing,
   in the engine, exactly the cost ADR-0001 exists to avoid in the file. A
   200-track project with automation does not survive rebuilding the world on
   every fader move, no matter how elegantly the old copy is freed.

**Decision, two parts.**

**(a) Structural sharing.** Snapshots are persistent immutable structures that
share unchanged subtrees. Editing one clip copies the path from root to that
clip and shares everything else. A snapshot becomes a cheap root pointer, and
publication cost is proportional to the edit rather than to the project.

**(b) Epoch reclamation.** The audio thread holds
`std::atomic<const Snapshot*> current`. At block start it loads `current`,
stores that pointer into `inUse` (release) **before** dereferencing, and uses it
for the whole block. The message thread keeps retired snapshots in a list and
frees any that `inUse` is not equal to, having observed it once — safe because
the audio thread publishes before use and `current` has already moved on, so it
cannot go back. Two atomic operations in the callback; no allocation, no
deallocation, no locks, no queue, no failure path.

**This is what production JUCE DAWs actually do**, which is checkable rather than
asserted: `reference/tracktion_engine` uses
`std::atomic<PreparedNode*> currentPreparedNode` with a retain/release count and
an atomic `nodeToRelease` for deferred destruction
(`tracktion_graph/tracktion_Node.h`). Atomic pointer publication plus deferred
release — not a return queue.

**Consequence.** `third_party/lockfree` is still wanted, for message-thread →
audio-thread *parameter* and event traffic, which genuinely is a queue. It is
not the snapshot mechanism.

**Consequence.** This is the "one small, isolated, heavily tested type" the
README has been carrying as an open question since ADR-0014. It is now specified,
and it should be written once, with its memory ordering commented line by line,
and then left alone.

---

## ADR-0020 — The op registry, and per-op engine impact — `DECIDED` (2026-09-17)

**Decision.** The op vocabulary is a registry of descriptors carrying name,
scope, engine impact, payload schema, apply handler and inverse builder, with
startup-asserted invariants. Specified in [`OPS.md`](OPS.md).

**Adopted from MAGDA** (`reference/magda-core`, GPL-3.0, compatible): exactly one
scope per operation rather than a set; handler on the descriptor rather than in a
name-keyed table; and a safe scope default paired with a registry-wide assertion
that every write declares otherwise, so a forgotten scope fails at startup rather
than being discovered by a client that finds it can edit.

**Adopted from Zrythm** (design only — AGPL, ADR-0017): per-op declaration of
what the action *disturbs*. Their `UndoableAction` carries `needs_pause()`,
`needs_transport_total_bar_update()` and
`affects_audio_region_internal_positions()`, and their undo stack takes an
engine-pause requester.

**This is the finding that changed the design.** Nothing in SPEC or ADR-0010
accounted for mutations that *cannot* be applied to a running engine by swapping
a snapshot. Some require a graph rebuild; a few require stopping the engine
outright. Ops therefore declare `EngineImpact ∈ {None, Snapshot, GraphRebuild,
RequiresPause}`, and the runtime can catch an op that exceeds what it declared —
instead of the mismatch surfacing as an unreproducible dropout.

`RequiresPause` is kept deliberately tiny (sample-rate change, device change,
project close). Every member is a stall the user hears.

**Correction worth recording.** Zrythm's action classes we were pointed at —
`ArrangerSelectionsAction`, `TracklistSelectionsAction`, `PortConnectionAction` —
live in `src/gui/backend/legacy_actions/`. Zrythm is mid-migration to a new
Qt/QML operator model in `src/actions/`. Both are worth reading, for different
things: the legacy tree for its inverse and engine-impact declarations, the new
one for how the taxonomy is being re-cut. Neither is a persistence model.

---

## ADR-0021 — Ops never read ambient state; selection is session state — `DECIDED` (2026-09-18)

**Decision.** Selection lives in Layer 3 session state and never enters the op
log as input. More generally: **no op payload may contain a value the handler
resolves from ambient state.** The UI resolves selection, playhead, grid, current
track, loop region and tool mode to literals at op-construction time.

**Why, in order of weight.**

1. **Determinism.** If ops could target "whatever is selected", an agent or
   script firing `clip.delete` while the user clicks a different track a
   millisecond before commit destroys the wrong data. That is not a race we can
   test our way out of — it has to be impossible by construction. This argument
   is the whole reason the decision is not a matter of taste.
2. **Undo hygiene.** If selection were op state, every mouse click would be a
   transaction, the undo tree would fill with navigation, and undoing an
   accidental delete would take five keystrokes.

**The generalisation matters as much as the specific case.**
`note.quantize { clip_id, grid: "current" }` has precisely the same defect as
targeting the selection, and would be precisely as hard to reproduce. The test
is: *an op replayed from the log a year later, on a machine with different UI
state, must do exactly what it did the first time.*

**Consequence — caller-allocated IDs.** `clip.create` takes the new clip's ID in
its payload rather than allocating one. Undo a create, redo it, and the object
must return with the *same* ID, or every later op referencing it points at
nothing. Same for tracks, lanes, devices, chains, automation lanes, scenes,
markers and notes. Handlers reject a colliding ID rather than reassigning. This
is what makes the log **replayable** rather than merely undoable — and
replayability is what step 4's round-trip corpus tests.

**Consequence — the one place selection and undo touch.** Undoing a delete should
restore what was selected, or the user has to find their clips again. So the
first op of a transaction may carry an advisory `sel_before` blob in `ops.tags`:
never an input to `apply`, purely a UI hint on undo, droppable by compaction.
Selection influences the *view* after an undo; it never influences *what* an op
does. Keeping it in advisory metadata rather than the payload is what lets both
statements hold.

**Enforcement.** Invariant 5 in OPS.md §3 cannot be compiler-checked. It is
enforced by review against the table in OPS.md §7.2, and by a replay test in the
step-4 corpus: apply a log twice under deliberately different UI state and assert
the results are byte-identical after canonical text projection (ADR-0007). An op
reading ambient state fails that test.

## ADR-0022 — Binary layout claims are proved by CI on every ABI, and the endianness guard is proved by a job required to fail — `DECIDED` (2026-09-18)

**Context.** SPEC §6.3 publishes exact sizes and field offsets for four record
types and claims a third party can implement a reader from the document alone.
`blob.hpp` turns each claim into a `static_assert`. Until now those asserts had
been evaluated by one compiler on one architecture, which proves the claims for
MSVC x64 and for nothing else. A format specification cannot rest on that.

**Decision, three parts.**

1. **A toolchain that compiles this tree has proved SPEC §6.3 for its own ABI**,
   whether or not it can then run a test. So CI's matrix is chosen by ABI rather
   than by convenience, and a compile-only leg is a first-class result. The set
   is clang and gcc on arm64 and x86_64, gcc on i386, and MSVC on x86_64 —
   covering LP64, LLP64 and ILP32, and libc++, libstdc++ and the MS STL.

2. **The little-endian precondition is proved by a job that must fail.**
   `blob.hpp` opens with `static_assert(std::endian::native ==
   std::endian::little)`. CI cross-compiles `blob.cpp` for big-endian s390x and
   requires the compile to fail *with that assert's own message in the
   diagnostic*. An assertion nobody has watched fire is a comment.

3. **The numbers in the spec are compared against the numbers the compiler
   produced**, by `.github/scripts/check_spec_layout.py`, on every ABI.
   `adi_tests` proves the layouts are self-consistent and the `static_assert`s
   prove they equal four numbers written in a header; neither proves those
   numbers are the ones `SPEC.md` publishes, and SPEC.md is what an implementer
   reads.

**Why it is an ADR and not just a CI file.** It commits the project to a
standard: adding a field to a record, or a new record type, means adding its size
to the spec *and* to the validator, and it means every supported ABI agrees
before the change lands. It also fixes the meaning of a green tick, which is
otherwise the least examined artifact in any repository.

**What this does not do.** It does not catch value-level bugs. A layout can be
byte-correct on every ABI and the reader still mis-handle a hostile file; the
32-bit `size_t` overflow in `StreamReader`'s length check is exactly such a case,
and the ILP32 leg will stay green until a test exercises it. Compilation proves
layout. Tests have to prove behaviour.

**Numbering note.** This is 0022 because 0021 is the highest number in this file.
`ADR-0018` appears twice — once `OPEN` and once as `ADR-0018 (revised)`. The
revision was appended rather than edited, which is right, but it reused the
number, which leaves "ADR-0018" ambiguous to cite. Superseding by editing is what
this log forbids, so the fix is a further append, and it belongs to whoever owns
that decision.

---

## ADR-0023 — `rec_size` must be a size some writer released; readers are total — `DECIDED` (2026-09-18)

**Amends ADR-0008.** Does not supersede it: the striding contract stands, this
closes a case it did not cover.

**The gap.** ADR-0008 says a reader strides by the header's `rec_size` and that
fields absent from a narrower record take their zero default. It did not say
what happens when `rec_size` lands *inside* a field. A `rec_size` of 29 on a
40-byte note record copies one byte of the two-byte `flags` at offset 28 and
leaves the other zero, so the reader returns `flags = 0x00EF` where the writer
wrote `0xBEEF` — and reports `error() == Ok`. Absent is defined. Torn is not.

**Decision.** `rec_size` is not a free integer. It is the size of some writer's
record, so it must be a size some writer actually emitted:

| `rec_size` | Result |
|---|---|
| equal to a released size for that type | accepted — older version, absent fields zero |
| greater than the reader's `sizeof(Rec)` | accepted — newer version, tail skipped (ADR-0008) |
| anything else | **rejected**, `RecSizeUnknown` |

Released sizes live in `StreamTraits<Rec>::released` next to the struct. **Adding
a field means appending the new size there in the same commit as the struct
change** — that is the whole maintenance burden this creates, and it is
deliberately in the one place a reader author cannot miss.

**Decision, second part: every accessor is total.** A reader parses whatever is
on disk — a file truncated by a full volume, cut short by a failed sync, or
written by someone hostile. `StreamReader::operator[]` is **removed** rather than
guarded, and replaced by `at()` returning `std::optional`. A guarded operator
still reads as safe at the call site while silently returning a zeroed record;
an optional makes the caller say what it wants to happen.

**Found by the `mac` agent, reproduced independently on `win` before acting.**
The old `operator[]` on a reader in a failed state memcpy'd from a null span:
segfault, exit 139. Out of range, it read ~40 MB past a 96-byte buffer and
returned the garbage silently.

**Third part: length arithmetic is 64-bit.** `count * rec_size` was computed in
`size_t`. On a 32-bit host `count = 131072, rec_size = 32768` is exactly 2^32,
which wraps to 0, so the required length became 16 and a header-only blob passed
validation while `count()` still reported 131,072. Now computed in `std::uint64_t`
with one checked narrowing. ADR-0022 notes CI's ILP32 leg stays green on this
until a test exercises it; that test now exists, and it asserts the fixture is
rejected on every ABI rather than asserting a particular error code, because the
correct rejection differs between LP64 and ILP32.

**Consequence for testing.** Each of the three real record types has exactly one
released size, which makes the older-narrower branch of ADR-0008 *unreachable*
for them — correctly, but therefore untested. `tests/test_main.cpp` carries a
synthetic two-size record type to keep that branch covered until a real type
gains a v2, at which point it can go.

## ADR-0024 — `third_party/` is pinned by tag *and* commit; `reference/` deliberately is not — `DECIDED` (2026-09-18)

**Context.** `tools/fetch_external.sh` cloned every dependency with
`--depth 1 --single-branch`, i.e. whatever the default branch pointed at that
minute. For `nlohmann/json` the default branch is **`develop`**. So every ABI
result recorded in ADR-0022 — seven ABIs agreeing on four record sizes — was
produced against an upstream that can move between two runs of the same commit
of *our* code. The tick was green; it was not reproducible. The same run next
week could fail for reasons nobody here caused, and the time to find that out is
not while diagnosing an unrelated bug.

**Decision.**

1. **Everything in `third_party/` is pinned**, because it is compiled into our
   binaries and its headers participate in our struct layouts. Each entry carries
   a release tag **and the commit that tag pointed at when it was pinned**.

2. **The commit is the assertion; the tag is the source.** The fetch checks out
   *by tag* and then verifies the resulting `HEAD` against the recorded commit.
   Checking out the commit directly would force the tree to the right bytes and
   make the check tautological — it would paper over a re-pointed tag instead of
   reporting it, which is the one thing the check exists for. A tag is a mutable
   ref; matching its name proves nothing about the bytes.

3. **`reference/` stays unpinned**, and that is a decision rather than an
   oversight. It is read for design and never compiled. The whole point of having
   Ardour, Zrythm and Tracktion on disk is to see what they do *now*. Nothing
   there can reach a build artifact, so nothing there needs to be reproducible.

4. **CI fetches through the script** (`--build-only`) rather than cloning by
   hand, so the pin is enforced on every run, before anything is compiled. A
   `provenance` job prints tag, pinned commit, fetched commit and date on every
   run, so "what did this build link against" is answerable by reading a log
   rather than by archaeology.

**The policy for moving a pin, which is the part that matters.**

A pin that is bumped reflexively is not a pin. Moving one is a reviewed change
with its own commit, and:

- **It is never a fix for a red build.** If `fetch_external.sh` reports a
  mismatch, the correct first response is to find out *why upstream's tag moved*.
  Re-pointing a release tag is a supply-chain event. Copying the new hash into
  the table to make CI green again destroys the only evidence that it happened.
  The script says so in its own failure message, because that is where someone
  will be standing when they are tempted.
- **Bump for a reason, and name it** in the commit message: a fix we need, a
  security advisory, a platform we are adding. "Newer" is not a reason.
- **One dependency per commit**, so a bisect over a regression lands on a single
  upstream change.
- **The full CI matrix must pass on the new pin before it merges** — all seven
  ABIs, not just the platform of whoever bumped it. A dependency that changes a
  struct layout will show up on exactly one of them, which is the reason ADR-0022
  shaped the matrix by ABI in the first place.
- **Record what moved.** Tag, old commit, new commit, and why, in the agent log.

**What this cost, visibly.** Pinning SQLiteCpp to its `3.3.3` tag moved the
vendored sqlite3 amalgamation from **3.53.4 to 3.49.2**, because its `master`
branch was ahead of its own most recent release. That is the trade working as
intended: we now build against a version someone released, rather than against a
branch tip that happened to be current. The `provenance` job prints that number
on every run so the cost stays visible instead of becoming folklore.

**Supersedes nothing.** ADR-0017 set out what the two external directories are
for; this decides how each is fetched, and does not change that boundary.

---

## ADR-0025 — Op payload keys are short strings, and "deterministic" is not "canonical" — `AMENDS ADR-0016` (2026-09-18)

Two things ADR-0016 asserted that the library cannot do. Both found by reading
`third_party/json`, before writing the codec against them.

**1. Integer map keys are not implementable with nlohmann, so keys are short
strings.** `basic_json::object_t` is `ObjectType<StringType, …>` — object keys
are always strings — and `binary_reader.hpp` calls `get_cbor_string(key)`, so it
cannot *read* an integer-keyed CBOR map either. OPS.md §8 rule 2 asked for
something the chosen library refuses in both directions.

Keys are therefore short strings: `"id"`, `"t"`, `"pos"`. A one-character CBOR
string key costs 2 bytes against an integer key's 1, so nearly all the
compactness survives, and we keep one library serving both the on-disk encoding
and the JSON-Schema projection the op registry needs for agent tool schemas —
which was an explicit reason for choosing it.

**Rejected: switching to a CBOR library that supports integer keys.** It would
buy roughly one byte per field and cost the schema projection, which is the part
that makes the registry and the agent share a single definition (ADR-0020).

**Key names are as permanent as op names.** They appear in every `ops.payload` of
every project ever saved. A retired key's name is never reused for a different
meaning.

**2. We require *deterministic* encoding, not RFC 8949 §4.2 canonical.**
ADR-0016 cited §4.2. nlohmann orders object keys with `std::less<StringType>`,
i.e. lexicographically, while §4.2 core-deterministic order is by *encoded*
bytes, which is length-first — canonical CBOR puts `"z"` before `"aa"`, and
nlohmann does the opposite.

What we actually need is narrower than what was claimed: **identical input must
encode to identical bytes**, so a payload hashes stably and text-projects stably
(ADR-0007). `std::map` ordering gives exactly that. The stronger property —
interoperating byte-for-byte with another implementation's canonical encoder —
is not something we need, and claiming it while not having it is worse than not
having it.

If we ever do need §4.2, it is a writer-side key-ordering pass, not a library
change. Recorded so the decision is available rather than rediscovered.

---

## ADR-0026 — The current undo branch lives in `op_branches.is_current` — `DECIDED` (2026-09-18)

**The contradiction.** SPEC §8.2 said the current-head pointer lives in
`session_state`. `schema.sql` put it in `op_branches.is_current` and seeded no
such `session_state` key. Two normative documents describing one pointer in two
places, which means an implementer reading either one alone writes something the
other rejects. Found by `mac`.

**Decision: `op_branches.is_current` is authoritative.** SPEC §8.2 is corrected.

**Why that side.** `session_state` is an untyped `TEXT` key-value store. A branch
pointer held there has no foreign key, so nothing stops it naming a branch that
was deleted, and nothing stops two writers disagreeing about the key's name. In
`op_branches` the pointer sits on the row it describes, and "exactly one branch
is current" becomes an enforceable constraint rather than a convention — a
partial unique index, added here.

`session_state` remains the right home for genuinely free-form session data:
playhead, zoom, selection (ADR-0021). The rule is that anything with referential
integrity belongs in a real table.

---

## ADR-0027 — The agent's non-undoable ops are exactly those that persist nothing — `DECIDED` (2026-09-18)

**The contradiction, and it is a safety claim.** `AI-AGENT.md` §1 stated flatly
that *everything the agent does is undoable*. OPS.md §5 grants the Apply tier the
`session` and `transport` scopes, which contain ten ops marked explicitly
non-undoable. The document that exists to say why the agent is safe was making a
claim the op catalogue contradicts. Found by `mac`, correctly ranked as
load-bearing.

**Decision.** The claim is corrected rather than the catalogue. The ten ops are
`transport.play/stop/seek/setLoop/setRecord/setMetronome` and
`session.launchClip/launchScene/stopTrack/stopAll`.

**The property that actually holds, and is what the safety argument needs:**

> Every change the agent makes to the *project* is undoable. The ops it can reach
> that are not undoable are exactly those that mutate no persisted state — they
> are performance, not editing. Nothing they do survives a save, so there is
> nothing for undo to restore.

That is a weaker claim than the original and it is true, which is the trade
worth making in a safety document. It also explains why those ops are safe to
grant rather than merely asserting it: an agent that starts playback has changed
nothing a user could lose.

**They remain audited.** Ephemeral ops still appear in the log tagged
`ephemeral`, still carry `actor = 'agent'`, and are still skipped by undo but not
by the audit trail (OPS.md §10). "What did the agent do at 14:32" stays
answerable.

**Consequence.** Any op added to `transport` or `session` scope that *does* touch
persisted state must be undoable, or it does not belong in those scopes. That
invariant is now the thing keeping this ADR true, and it belongs in the registry
checks.

---

## ADR-0028 — ADR-0018 is split into 0018 and 0018R; numbers are never reused — `DECIDED` (2026-09-18)

**The problem.** `ADR-0018` appears twice in this file: once `OPEN`, stating the
MAGDA/Tracktion question, and once as `ADR-0018 (revised)` deciding it. Appending
rather than editing was right; reusing the number was not. "ADR-0018" now has two
referents, and a citation cannot disambiguate them. Found by `mac`, who correctly
left it to the agent that owns the decision.

**Decision.** The two entries are cited as **ADR-0018** (the open question, kept
verbatim) and **ADR-0018R** (the decision that resolves it). Neither is edited
beyond adding that label — the log's append-only rule includes its own mistakes.

**The general rule, which is the point of recording this:** a number, once used,
names one entry forever. Revisiting a decision takes the next free number and
says what it supersedes. `0018R` is a one-off repair of an existing collision,
not a pattern to copy — ADR-0025 amends ADR-0016 by taking a fresh number, which
is the shape every future revision should have.

---

## ADR-0029 — Every table is STRICT, and `routing` loses the `'bus'` kind — `DECIDED` (2026-09-19)

Two schema defects, both found by `mac` while building the text projection, both
verified against the file before acting.

### 1. Every table is `STRICT`

**The defect.** `schema.sql` declared no `STRICT` tables and has **23 `REAL`
columns**. Under SQLite's flexible typing, any of them can legally hold `TEXT` —
and `sqlite3_column_double()` then coerces it silently. A reader returns a
number that is not what is stored, and nothing anywhere reports a problem. For a
format whose entire premise is that a third party can implement a correct reader
from the spec, a column whose declared type is advisory is a trap laid for that
implementer.

**Decision.** All 38 tables are `STRICT`. Verified, not assumed: inserting
`'loud'` into `mixer_strip.volume_db` now raises *"cannot store TEXT value in
REAL column"* instead of being accepted.

**Cost accepted: this raises the minimum SQLite to 3.37 (November 2021).** Older
versions do not merely ignore `STRICT` — they fail to parse the schema. A reader
built against, say, a distro SQLite from 2020 cannot open a `.adi` at all. Four
years is long enough that the trade is worth it, but it is a real exclusion and
SPEC §3 now states the requirement rather than leaving it to be discovered.

**Enforced** by `validate_schema.py` check 5b, per table rather than by counting
the keyword — a comment mentioning `STRICT` would satisfy a count. Proved it can
fail by removing `STRICT` from one table and watching it name that table.

### 2. `routing` permitted a reference kind with no possible target

**The defect, and it is mine.** `routing.src_kind`/`dst_kind` had a `CHECK`
allowing `'bus'`, and there is no `buses` table — nor was there ever going to be
one. A bus in this model is a track whose `kind` is `'group'`, `'return'` or
`'master'` (ADR-0006 and SPEC §6.1). So the constraint permitted a **dangling
reference by construction**: not a row that happens to point nowhere, but a
reference kind whose target could not exist. The text projection surfaced it as
`"!unresolved(bus)"` because it had nowhere to look, which is the correct
behaviour for a reader and the wrong situation for a schema.

**Decision.** `'bus'` is removed.

**What the fix exposed, and is now written down.** The remaining kinds are of two
different sorts, and conflating them is how this got in:

| Kind | `src_id` / `dst_id` means | Unresolvable is |
|---|---|---|
| `track`, `device` | a **row id** in that table | corruption |
| `hw_in`, `hw_out` | a hardware **port index** on the current device | **normal** — the project moved to another studio |

That distinction matters to every reader, not just ours: it decides whether a
failed lookup is an error or an expected condition. It was implicit before and
is now in the DDL and in SPEC §6.7.

**The structural cost, stated plainly.** A polymorphic `(kind, id)` pair cannot
carry a `FOREIGN KEY`. That is the price of one `routing` table instead of six,
and it is still the right trade — Cubase Direct Routing falls out of it for free
(ADR-0006's reasoning applies equally here). But it means SQLite cannot enforce
these references, so `validate_schema.py` check 5c enforces the *schema-level*
half (every internal kind names a real table) and a future `adi_tool check`
must enforce the *data-level* half (every internal id resolves to a live row).
Until that command exists, this class of corruption is undetected at rest.

---

## ADR-0030 — Undo moves the head; it does not append — `DECIDED` (2026-09-19)

Three decisions that only became visible when undo/redo was built on the journal.

### 1. Undo and redo do not write op rows

**The tension.** ADR-0003 says *every* mutation appends an op row in the same
transaction that performs it. Undo mutates the project. Taken literally, an undo
would append a row — and so would the redo, and the next undo, without bound.
Toggling Ctrl-Z would grow the file forever and the "tree" would degenerate into
a linear log of do/undo/redo/undo.

**Decision.** Undo and redo **move the head pointer** along the existing log and
apply the stored inverse or payload. They append nothing.

**Why this does not weaken ADR-0003.** The invariant that matters is that *the
log fully describes the project's state*, not that every write has its own row.
After an undo, the project's state is exactly "the log up to head", which is
completely described — by the rows that already exist. Appending would add no
information and destroy the structure.

The atomicity half of ADR-0003 is untouched and is what the tests actually
check: **the head move is in the same SQLite transaction as the inverses it
describes.** A crash between them would leave the project changed and the head
disagreeing, and the next undo would then act on the wrong transaction.

Tested by running five undo/redo cycles and asserting the row count is unchanged.

### 2. Ephemeral ops are outside the tree, not filtered out of it

`transport.play` and `session.launchClip` write no project state (ADR-0027), so
they must not be undoable. They are given **`parent_seq = NULL`** and do not
advance the head — they are in the log for audit and simply are not in the
chain.

**Rather than leaving them in the chain and skipping them on every walk.** A
filter is something each of undo, redo, fork detection and branch-tip-walking
would have to remember independently, and the one that forgot would be a bug
nobody finds until an agent's `transport.play` swallows a Ctrl-Z. Keeping them
out of the structure makes "skip" free.

### 3. After a fork, redo follows the highest-seq child

Undoing and then committing something new leaves the head with two children.
Redo has to pick one.

**Decision: the highest seq, which is the most recently created line.** The
abandoned line is saved as a branch row first, so nothing is lost — SPEC §8.2's
"it forks" rather than "it is destroyed". `OpJournal::commit` and
`History::nextRedo` use the same rule, which is why they cannot disagree about
which line is live.

**Verified non-vacuously:** disabling fork preservation makes the test fail with
*"the abandoned line was saved as a branch rather than destroyed"*, and removing
the undo transaction makes the atomicity test report *"saw 1"* track where two
were expected.

**Not implemented: switching between diverged branches.** It needs rewind to the
common ancestor and replay forward. `switchToBranch` refuses rather than
approximating, because getting it wrong corrupts a project. Re-selecting a
branch already at the current head works.

### 4. `sel_before` needed its own column — an ADR-0021 × ADR-0029 collision

ADR-0021 §7.5 put the advisory selection hint in `ops.tags`. ADR-0029 then made
every table `STRICT`. `ops.tags` is `TEXT`, so a CBOR blob can no longer be
stored there at all — the mechanism became unimplementable, and neither ADR was
wrong in isolation.

Fixed with a nullable `ops.sel_before BLOB`. Still advisory, still never an input
to a handler, still droppable by compaction.

**Worth recording as a class of problem, not just an instance.** It surfaced only
when something first tried to write one, which is months after both decisions
looked settled. A decision that tightens a constraint everywhere should be
checked against decisions that relied on the looseness.

---

## ADR-0031 — The replay oracle is a canonical database digest — `DECIDED` (2026-09-19)

**Context.** ADR-0021 §7.4 requires a replay test: apply an op log twice under
deliberately different UI state and assert the results are identical. It named
the canonical text projection (ADR-0007) as the comparison. The projection's
pure renderers and ordering exist, but its store adapter does not yet, so the
test had no oracle and the replay property was an untested claim.

**Decision.** `src/adi/digest.{hpp,cpp}` renders the **project tier** of a `.adi`
into one canonical string, sorted by content rather than storage, and that is
the oracle. `adi_tool digest` exposes it.

**Why this is not a stopgap.** It compares *more* than the text projection can:
every column of every project table, including ones nothing renders yet. An
oracle that covered only what some renderer emits would pass while the two
databases differed in a column the renderer had not learned about. When the
projection's store adapter lands it becomes a second, human-readable oracle —
not a replacement.

**The exclusion list is the design, not housekeeping.** `ops` and `op_branches`
are excluded because the log is not the project and replay legitimately produces
new seqs and timestamps. `adi_meta` and `session_lock` are volatile.

And `session_state`, `ui_view`, `window_state` are excluded **because the test
sets them differently on purpose**. That is what makes a match mean something:
the two projects have different selection, playhead, grid and zoom by
construction, so identical digests prove the ops did not consume any of it.

**Verified by planting the bug it exists to catch.** A `track.rename` handler was
temporarily made to append the current selection to the name — a textbook
ADR-0021 §7.2 violation. Result:

```
replay  FAIL  the two projects are IDENTICAL despite different UI state
          A: name=sRhodesclip:1,clip:2,track:10
          B: name=sRhodestrack:7
ops     PASS -- 74 checks, 0 failure(s)
history PASS -- 95 checks, 0 failure(s)
```

**The unit suites did not notice.** They cannot: a unit test does not vary the
ambience. That gap is the entire justification for this test existing, and it is
why the corpus was worth building before widening the op catalogue — a sixtieth
op tests the same pattern the sixth did, while this tests a property nothing
else can reach.

**Rendering details that matter.** Values carry a type tag, so integer `1` and
text `"1"` cannot digest alike. Doubles use `%.17g`, the shortest round-tripping
form. Blobs are hashed with their length rather than dumped, so the digest stays
readable when it differs. Text escapes the field and record separators, because
a track name containing one could otherwise make two different projects digest
identically — the one failure a comparison oracle must not have.

---

## ADR-0032 — `media_files.hash_blake3` is UNIQUE — `DECIDED` (2026-09-19)

**Decision.** The index becomes `CREATE UNIQUE INDEX ... WHERE hash_blake3 <> ''`.

**Why.** ADR-0005 says content addressing is what gives deduplication — "the same
sample dropped in twenty times is one file". A non-unique index made that a
comment rather than a rule: nothing stopped twenty rows holding one hash, and
with duplicates, relink-by-content has no single answer and the pool is not a
pool. Reported twice by `mac`, who needs hash-as-designator for the text
projection.

**Partial, on `hash_blake3 <> ''`,** so a row whose hash is not yet computed does
not collide with every other such row. Nobody looks a file up by the empty
string, so the index loses nothing by excluding them.

Enforced by `validate_schema.py` check 5d, which asserts both halves: a duplicate
hash is rejected, and two un-hashed rows are not.

---

## ADR-0033 — `adi_tool check` verifies what SQLite structurally cannot — `DECIDED` (2026-09-19)

**Context.** ADR-0029 closed with an admission: a polymorphic `(kind, id)` pair
cannot carry a `FOREIGN KEY`, so a `routing` row pointing at a deleted track was
undetected at rest, and the data-level half of referential integrity needed a
command that did not exist. This is that command.

**Decision.** `src/adi/check.{hpp,cpp}` plus `adi_tool check`. Read-only: it
never repairs, because a repair that guesses is how a corrupt project becomes a
plausible-looking wrong one. 20 checks in three families, each covering
something the database cannot:

1. **Polymorphic references.** Seven `(kind, id)` sites — `routing` twice,
   `automation_lanes`, `ui_view`, `extensions`, `controller_maps`, `ops`. A kind
   naming a real table must resolve; `hw_in`/`hw_out` are hardware port indices
   and a failed lookup there is *normal* (SPEC §6.7); an unrecognised kind is a
   **warning**, not an error, because a newer version may have added one and
   ADR-0012 says unknown data is preserved rather than rejected.

2. **Inside the blobs.** A notes blob is opaque to SQL. Whether its header parses,
   whether its `rec_size` is a size some writer released (ADR-0023), and whether
   a `note_expression` row names a note that exists **in another blob** — a
   reference from one blob into another, which nothing relational can see.

3. **Structural invariants across rows.** Exactly one current branch; a
   `head_seq` that names a real op; ephemeral ops outside the undo tree; and no
   cycle in `parent_seq`, which would make undo fail to terminate. A cycle
   satisfies every foreign key, which is precisely why it needs its own check.

**Every check is planted with the corruption it finds.** A check nobody has
watched reject something is a comment. The corruptions are written with raw SQL
deliberately: they are states the op layer cannot produce, which is the point —
they arrive from a crash, a bad merge, a third-party writer, or a future version
of us with a bug.

**Two things the tests corrected about my own assumptions**, both in the
direction of SQLite enforcing more than I credited it with:

- `op_branches.head_seq` **is** a real `FOREIGN KEY`. Planting a dangling head
  needs `foreign_keys = OFF`. The check still earns its place — a file from a
  third party, or from us with the pragma off, can arrive that way — but the
  comment claiming SQLite could not see it was wrong.
- `clips` **does** CHECK half of SPEC §4.1: `time_base = 1 OR pos_ns IS NULL`
  stops a musical clip carrying a nanosecond position. What it does not cover is
  a clip with *no* position at all, which is equally invalid and equally silent.
  Only that half is ours.

---

## ADR-0034 — `StreamReader` cannot bind to a temporary — `DECIDED` (2026-09-19)

**Found by `mac` in the first audit of `blob.hpp`, reported as a low-severity
item, and then written by `win` while building the checker.** That sequence is
the argument for the fix.

**The defect.** `StreamReader` holds a non-owning `std::span`. The implicit
`vector`-to-`span` conversion made this compile cleanly:

```cpp
StreamReader<NoteRecord> r(blobOf(column), FourCC::Notes);
```

The temporary dies at the end of the statement; `r` outlives it. It does not
crash — it reads freed memory that usually still holds the old bytes, or reports
`count() == 0`. In `check.cpp` it produced an **empty note set and therefore a
false "orphaned expression" finding**: a checker confidently reporting corruption
that was not there.

**Decision.** `StreamReader(std::vector<std::byte>&&, FourCC) = delete;`

The rvalue overload turns the mistake into a diagnostic at the call site. The
caller keeps the buffer in a named local, which it had to do anyway.

**Why this is worth an ADR rather than a quiet fix.** The lesson is not about
spans. A reported low-severity finding sat unfixed because it was theoretical,
and the same author then made exactly that mistake within the week. "Low severity
because nobody would write that" is a prediction about people, and it was wrong
within days. Where a hazard can be closed at the type level for one line, it
should be, rather than ranked and deferred.

## ADR-0035 — A visual patching device tier, embedded via libpd — `DECIDED (direction)` (2026-09-19)

**Goal.** A user can drop a device on a track, open it, and build a synth or an
effect by patching boxes together — and can do so without buying anything,
because the patching environment ships with the DAW and is as free as the DAW
is. Pure Data, embedded through `libpd`, is how.

Pd is the right choice on the merits and not only on price: Miller Puckette
wrote Max and then wrote Pd, so this is the same lineage rather than an
imitation of it, and the patch format is plain text, which matters here more
than it would elsewhere (see *Consequences*).

### One correction to the framing, stated first because it sets the scope

**Embedding libpd does not give us Max for Live.** It gives us Max for Live's
*engine*. Everything that makes M4L feel like part of Live is the integration
layer around it: devices that expose named parameters the host can automate and
map to a controller, a device UI that is not a patcher window, preset and state
handling, freezing, and the Live Object Model that lets a patch see and change
the set. None of that comes with libpd.

So the work is not "embed libpd" — that part is comparatively small. The work is
the **device contract**: how a patch declares its parameters, how those become
automatable lanes in our schema, how state is saved, and what happens when a
patch and a project disagree. That contract is what this ADR commits us to
designing, and it is deliberately not designed here.

### Licences, verified rather than assumed

- **libpd and the Pd core are BSD-3-Clause** — upstream calls it the "Standard
  Improved BSD License". Permissive, GPLv3-compatible, and imposes nothing on
  us beyond attribution. No conflict with ADR-0015.
- **Pd *externals* are a separate question.** Many are GPL, which is fine; some
  are neither free nor redistributable. Shipping any external is a per-library
  decision and belongs in `docs/EXTERNAL-CODE.md`, not here. Vanilla Pd — the
  objects built into the core — carries no such problem.

**A correction I owe the record: I expected RNBO to be disqualified on licence
grounds, and it is not.** Code RNBO generates is **dual-licensed**, under either
Cycling '74's own terms or **GPLv3**, explicitly so that it can be combined with
GPLv3 code such as JUCE and the VST3 SDK. A GPLv3 project can use it.

### Why libpd is the tier we build, and RNBO is not a rival

They are not competitors; they sit at different points and only one of them can
carry the goal above.

| | libpd | RNBO |
|---|---|---|
| When the patch is compiled | loaded and edited **at runtime** | exported to C++ **ahead of time** |
| What the user needs to author | nothing but our DAW | **Max plus the RNBO add-on**, both paid and proprietary |
| Licence of the result | BSD-3 engine, user's patch is the user's | dual, GPLv3 available |
| Copyright in the generated code | n/a | **Cycling '74 retains it** |

The deciding line is the second row. A patching tier whose authoring requires
commercial software is not the goal stated at the top — it moves the paywall
rather than removing it. RNBO stays interesting as a *separate, later* route for
shipping a fixed DSP algorithm compiled into the binary, and nothing here
forecloses it; it is simply not the extensibility story.

### What embedding actually constrains

- **Pd computes in ticks of 64 frames**, and `libpd_process_float` wants a
  buffer that is `channels × ticks × 64`. Our device wrapper owns the
  reblocking, because a DAW buffer size is not required to be a multiple of 64
  and users will pick 100 or 480.
- **Multi-instance is a compile-time flag.** Upstream builds with `MULTI=true`
  and `PDINSTANCE`; without it there is one global Pd interpreter, which is
  useless for a DAW where every track may hold a device. This is a hard
  requirement on how we build it, not a runtime option.
- **ADR-0010 still governs.** `libpd_process_float` runs on the audio thread;
  opening a patch, editing it, allocating, and anything that touches the
  filesystem does not. The existing prohibition does not bend for this.
- **ADR-0011 generalises.** A patch referencing an external we do not have is
  the missing-plugin rule in a new costume: preserve the patch byte-for-byte,
  keep the device in the chain bypassed, surface what is missing. Do not
  silently drop the object.

### Consequences worth naming now

- **A `.pd` patch is text, so it diffs.** Every other device's state is an
  opaque plugin blob that the text projection can only render as a digest
  (TEXT-PROJECTION §9). A Pd device's state is the patch, and a patch is lines.
  This is the first device state that can appear in a `git diff` as something a
  human reads — which is a genuine argument for the tier beyond openness.
- **Where the patch lives is an open format question.** It is file-shaped
  (`media_files`), state-shaped (`plugin_state`), and text-shaped all at once,
  and the choice changes what the projection can do with it.
- **The agent question is not answered.** ADR-0003 says every mutation is a
  typed op. Whether the agent may edit a patch — and if so whether that is one
  op or a vocabulary of them — is a real decision and is not taken here.

### Status

**The direction is decided: libpd, vanilla Pd, multi-instance, as a first-class
device tier.** Three things are explicitly open and each needs its own ADR
before code: the device/parameter contract, patch storage in the schema, and the
agent's relationship to patch contents.

Sequenced after plugin hosting (roadmap step 6), because a Pd device is a device
and the device/parameter/automation contract has to exist before a second kind
of device can honour it.

---

## ADR-0036 — The engine skeleton is built and tested without JUCE — `DECIDED` (2026-09-19)

**Context.** Step 5 is the audio engine skeleton: graph, transport, and the
snapshot handoff ADR-0019 specified. ADR-0014 chose JUCE, so the obvious move is
to start with a JUCE audio callback.

**Decision.** The handoff, the engine-side project model and the tempo
conversion are built in `src/adi/engine/` with **no JUCE and no audio device**,
and tested headlessly. JUCE arrives in step 6, wiring a real device to a
mechanism that is already proven.

**Why.** The riskiest thing in the project is the lock-free handoff — ADR-0014
noted that choosing C++ removed the compiler-enforced safety net ADR-0010 was
relying on. A bug there is a dropout or a crash in a user's session, and it is
timing-dependent, which means it is exactly the kind of bug an audio device makes
*harder* to find: you cannot run a real device ten thousand times a second, you
cannot make it deterministic, and a glitch is hard to distinguish from a slow
callback.

Headless, the same mechanism runs **6.9 million read blocks against 666,000
publications in 1.2 seconds**, on a thread doing nothing but hammering it. That
is more contention in one test than a real session produces in a week.

**Proved rather than asserted.** The safety argument in `publisher.hpp` turns on
the free condition being strictly greater — a retired snapshot may be freed only
once `inUse_ > seq`, never `>=`, because `>=` frees the snapshot the audio thread
is currently inside. Changing that one character:

```
adi_engine_tests    Segmentation fault    exit 139
```

It does not fail a check; it takes the process down before printing a line. One
character between a working engine and a crash, which is why the reasoning is
written out above the code rather than left as an off-by-one someone tidies up.

**Structural sharing, also measured.** ADR-0019 required that publication cost
what the edit cost rather than O(project). With 20 tracks: an unchanged rebuild
shares all 21 nodes; moving one fader shares 20 of 21 and rebuilds exactly one
track. The previous snapshot is observably untouched, which is what makes it safe
for the audio thread to still be reading it.

**Tempo is integrated segment by segment**, not `ticks × 60 / bpm / ppq` with a
single bpm. The naive form is correct until the first tempo change and wrong
after it — four quarters at 120 then four at 60 is six seconds, not four — which
is why the test asserts both the right answer and that it is not the wrong one.

**What this is not.** There is no audio graph, no processing, no device. A
snapshot describes tracks and clips; nothing renders them. That is step 6, and
calling this an audio engine would be a lie.
