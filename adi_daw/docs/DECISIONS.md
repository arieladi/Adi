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

---

## ADR-0014 — Implementation language and framework — `OPEN`

Not decided. Blocks nothing in the format (SPEC is deliberately
language-neutral); blocks everything after it.

The live options, with the honest trade:

- **C++ / JUCE** — existing familiarity from VST-ADI, every plugin SDK is C++
  first, JUCE solves audio I/O and plugin hosting on day one. The default answer,
  and the boring-in-a-good-way one. JUCE's licensing (GPL or commercial) has to be
  checked against our chosen licence.
- **Rust** — real safety wins in exactly the places DAWs are historically buggy
  (lifetimes across threads, the lock-free snapshot handoff in ADR-0010), and a
  far more attractive proposition for open-source contributors in 2026. Plugin
  hosting is the weak spot: VST3 and AU hosting from Rust means FFI to C++
  regardless.
- **Rust core + C++ hosting shim** — most of the benefit, at the cost of a
  two-language build and an FFI boundary that will be crossed on the hot path.

Decide before any engine code is written.
