# ADI DAW — decision log

One entry per decision that would be expensive to reverse. Append only; when a
decision changes, add a new entry that supersedes the old one rather than editing
history. Same convention as `VST-ADI/ARCHITECTURE.md`.

**Status values:** `DECIDED` · `PROVISIONAL` (will revisit before v1.0) ·
`SUPERSEDED BY ADR-nnnn` · `OPEN`

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

## ADR-0018 — Our relationship to MAGDA and Tracktion Engine — `OPEN`

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

## ADR-0018 (revised) — Stay independent; JUCE for hosting, Tracktion as reference — `DECIDED` (2026-09-17)

**Supersedes the OPEN status of ADR-0018 above.**

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
