# Why SQLite — and the three things the original proposal got wrong

This document exists so we never re-litigate the container decision, and so the
errors in the first draft don't get quietly re-introduced by someone reading
only the spec.

---

## 1. The container decision: SQLite. Confirmed.

The original proposal was right, and right for mostly the right reasons.

| Property | XML/JSON in a zip (Ableton, Bitwig, `.dawproject`) | SQLite |
|---|---|---|
| Save cost | O(project size) — full reserialize + recompress | O(delta) — bounded by pages touched |
| Autosave during playback | CPU spike proportional to project size | Flat, backgroundable |
| Crash mid-save | Truncated/corrupt file is possible | Atomic commit or rollback; never half-written |
| Partial read | Must parse the whole document | Query only what you need |
| Unknown data survival | Requires the writer to round-trip carefully | Untouched tables simply stay on disk |
| Concurrent reader | No | Yes (WAL) |

The last two rows deserve more credit than the proposal gave them.

**Unknown-data survival is nearly free in SQLite and expensive everywhere else.**
A full-rewrite format (XML, JSON) has to consciously carry forward every field it
doesn't understand, and every writer bug silently destroys user data. In SQLite
we do incremental `UPDATE`s in place — a table written by a future version that
this build has never heard of is not parsed, not touched, and not lost. Forward
compatibility is the *default* rather than a feature someone has to remember to
implement. This is the strongest single argument for the container and the
original draft buried it.

**A concurrent reader means the render/export/analysis path can read a consistent
snapshot while the user keeps editing.** That is a real feature — background
bounce, offline analysis for the AI agent, waveform generation — and it is
structurally impossible in a single-document format.

### The honest cost

SQLite is a binary blob, so a `.adi` is opaque to `git diff`, to `grep`, and to
a text merge. For an open-source project whose users are exactly the kind of
people who put projects in version control, that is a real loss. Reaper's `.RPP`
being plain text is a genuine advantage we are giving up.

We pay it back with a deterministic text projection — see ADR-0007. The `.adi`
is the working file; `adi export --text` produces a canonical, diffable,
line-oriented rendering. We do **not** make the text form primary; the
performance argument wins for the format people actually save into.

---

## 2. Error one: "incremental saves update the specific database row"

Half true, and the half that's false matters.

SQLite's unit of write is the **page** (4 KiB by default), not the row. A commit
also writes WAL frames, updates any affected indexes, and eventually checkpoints.
The saving is real — write cost is proportional to the *delta*, not to the
project — but the mechanism is not row granularity, and believing it is leads
straight to a design mistake:

> **If a clip's notes are one BLOB, editing one note rewrites that whole BLOB.**

So "store MIDI as binary blobs" is correct *only with a granularity policy*.
Blob-per-clip is right: clips are small (a busy 8-bar MIDI clip is a few KB), and
a rewrite touches one or two pages. Blob-per-track, or the obvious-seeming
blob-per-project, would reintroduce exactly the O(project size) save cost we
adopted SQLite to escape.

**Rule, in the spec as a MUST: no BLOB in the core tier may span more than one
user-visible editable object.** See SPEC §6.

## 3. Error two: "the audio engine reads them directly into memory buffers"

This is the dangerous one, and it must not survive into any implementation.

**The audio thread must never touch SQLite.** No file I/O, no allocation, no
mutex, no syscall, no `sqlite3_step` — not at background priority, not "just for
reads", not once at the top of the callback. A `sqlite3_step` that misses the
page cache is a disk read; a disk read inside a 128-sample callback at 48 kHz
(2.67 ms of budget) is a dropout, and on a bad day a multi-hundred-millisecond
stall.

The file format is a **persistence layer**, not a runtime model:

```
  .adi (SQLite)  --load-->  Project Model (heap, mutable, message thread)
                                    |
                            publish immutable snapshot
                                    |  (atomic pointer swap, RCU-style)
                                    v
                            Audio Thread (read-only, lock-free, no alloc)
```

This changes the schema in a concrete way, which is why it belongs here and not
only in an architecture doc: we optimise the on-disk layout for **fast bulk
decode into the runtime model**, not for in-place access. That is what actually
justifies the BLOBs — a memcpy-and-fixup of 4,000 notes beats 4,000 row reads by
two orders of magnitude — but it justifies them for *loading*, not for playback.

## 4. Error three: "open it in Cubase and your Session View is preserved"

This will not happen. Steinberg and Ableton are not going to implement our
format. Presenting the extension mechanism as cross-DAW round-tripping oversells
it, and would make us look naive to exactly the audience we want contributing.

The extension tier is still **essential**, for reasons that are real:

1. **Forward compatibility between our own versions.** v1.2 adds chord tracks;
   v1.0 opens the project, doesn't understand them, preserves them, and the user
   who opens it again in v1.2 still has their chord track. This is the case that
   actually happens, constantly, and it is the one that burns users in every
   other DAW.
2. **Third-party extensions.** Scripts, devices and community plugins need
   somewhere to persist project-scoped state without us minting schema for them.
3. **Import round-trips.** When we import `.dawproject`, AAF, OMF or a MIDI file,
   whatever we can't map natively is parked in an extension row so re-export is
   lossless. *This* is the interop story — and it is a converter story, not a
   shared-file story.

And the proposal missed the failure mode that makes preservation dangerous:

> An unknown extension that is **musically essential** must not be silently
> ignored.

If v1.0 opens a project, ignores a chord track it can't read, and the user edits
and saves, the data survives — good. But if the user *renders* that project they
get silently wrong audio and no warning. So extensions carry a `criticality`
flag: `advisory` (ignore freely) or `essential` (preserve, and **tell the user**
this project uses features this build doesn't understand, before they render or
export). See SPEC §9.

---

## 5. What the proposal missed entirely

### 5.1 Media

Audio files are ~99.9% of a project's bytes, and the proposal doesn't mention
them once. Whether a `.adi` references or embeds its audio is the single most
user-visible decision in the whole format. See ADR-0005.

### 5.2 The WAL sidecar trap

A SQLite database in WAL mode is **three** files: `song.adi`, `song.adi-wal`,
`song.adi-shm`. A user who emails a collaborator just the `.adi` after a crash,
or copies it off a card mid-session, loses everything since the last checkpoint —
and the file will open *cleanly*, just old, which is considerably worse than an
error.

Policy, specified rather than left to the implementer: WAL while the project is
open, **checkpoint + `journal_mode=DELETE` on clean close**, so a closed `.adi`
is always exactly one self-contained file. See SPEC §3.3.

### 5.3 The op log is not just undo — it is the AI agent's entire safety model

The proposal mentions "a serialized log of actions" as a way to get persistent
undo. That undersells it enormously.

If **every** mutation to the project — from the user, from a script, from the AI
agent, from an importer — is a typed `Op` appended to a log, then one design
decision buys all of:

- persistent undo/redo across restarts (the stated goal), and a branching undo
  *tree* rather than a stack, so exploring an idea never destroys the other one;
- crash recovery finer-grained than the last save;
- **an AI agent that cannot do anything a user couldn't do, and nothing that
  can't be undone with one keystroke** — because the agent's only interface is
  the same op vocabulary, every op is tagged with its actor, and an agent action
  commits as one transaction that reverts as one unit;
- a preview/diff step: the agent proposes a changeset, you see it, you apply it;
- a scripting and macro API for free, because it already exists;
- the sync unit for real-time collaboration later;
- reproducible bug reports — attach the op log.

An AI agent inside a DAW with direct mutable access to the project model is a
terrifying thing to ship. An AI agent that can only emit ops is a normal feature.
This is the architectural decision that makes the agent safe, and it has to live
in the *format* — not just in the app — because the log must survive a restart.

See ADR-0003 and [AI-AGENT.md](../AI-AGENT.md).

---

## 6. Summary of the delta from the original proposal

| Original | Status |
|---|---|
| SQLite container | **Kept.** Better arguments, in §1. |
| Transactional crash safety | **Kept**, with the WAL sidecar policy it needs (§5.2). |
| Lazy loading | **Kept.** |
| MIDI as binary blobs | **Kept, constrained**: one blob per editable object, never larger (§2). |
| Audio engine reads blobs directly | **Rejected.** Persistence layer is not the runtime model (§3). |
| Three tiers | **Restructured to five layers** — container / core / plugin / session / extension. |
| Cross-DAW round-tripping via vendor blobs | **Reframed.** Forward-compat + extensions + importer fidelity (§4). |
| Undo history in the file | **Promoted** from a feature to the backbone of the design (§5.3). |
| — | **Added:** media pool, content addressing, embed policy (§5.1). |
| — | **Added:** dual musical/linear time domain, exact tick base (SPEC §4). |
| — | **Added:** per-note expression as a first-class citizen (SPEC §6.3). |
| — | **Added:** plugin state that survives a *missing* plugin (SPEC §7). |
