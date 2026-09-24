# The `.adi` Project Format — Specification

**Version:** 0.1 — DRAFT. Nothing here is frozen. No compatibility promise is
made until this document says `STABLE`.
**Container:** SQLite 3
**Companion files:** [`schema.sql`](schema.sql) (normative DDL),
[`RATIONALE.md`](RATIONALE.md) (why, and what we rejected)

Key words **MUST**, **MUST NOT**, **SHOULD**, **MAY** are used as in RFC 2119.

---

## 1. Scope and design goals

`.adi` is the native save format of ADI DAW. It is designed to be:

1. **Fast to save incrementally** — a save is proportional to what changed, not
   to the size of the project, so autosave can run during playback.
2. **Impossible to half-write** — every save is an atomic transaction.
3. **Forward compatible by default** — an older build that opens a newer project
   preserves everything it doesn't understand, and says so out loud when that
   data is musically essential.
4. **Complete** — the file holds not just the music but the *session*: window
   positions, zoom, selection, undo history, controller mappings. Reopening a
   project puts you back exactly where you were.
5. **Specified well enough to reimplement** — a third party MUST be able to write
   a correct reader from this document alone, without reading our source.

### Non-goals

- **Not** a format other DAWs will open. Interop happens through converters
  (`.dawproject`, MIDI, AAF, stems), not through shared files. See RATIONALE §4.
- **Not** a streaming/interchange format for the audio engine. It is a
  persistence layer. The audio thread never touches it. See RATIONALE §3.
- **Not** a text format. A diffable text projection is a derived export, not the
  file you work in. See ADR-0007.

---

## 2. Identity

| Property | Value |
|---|---|
| Extension | `.adi` |
| Alias extension | none. `.adibundle` is retired (ADR-0127): media is never embedded, and a shareable project is a ZIP (§10.4) |
| MIME type | `application/vnd.adi.project` |
| SQLite `application_id` | `1094994225` (= `0x41444931`, ASCII `ADI1`) |
| SQLite `user_version` | `schema_major * 1000 + schema_minor` |
| UTI (macOS) | `org.adidaw.project` |

A reader **MUST** verify `application_id` before trusting any table. A SQLite
file with the wrong `application_id` is not a `.adi` and MUST be rejected with a
clear message rather than partially parsed.

> **Known extension collision:** `.adi` is also used by ADIF amateur-radio
> contact logs (a plain-text format). There is no registry to conflict with and
> no overlap in application domain, and the SQLite magic header plus
> `application_id` disambiguates unambiguously on content. We accept the
> collision. It is worth knowing about before someone reports it as a bug.

---

## 3. Container rules

### 3.0 Minimum SQLite version

A reader **MUST** use **SQLite 3.37.0 or later** (November 2021). Every table in
`schema.sql` is declared `STRICT`, and an older SQLite does not ignore that
keyword — it fails to parse the schema, so it cannot open a `.adi` at all.

The requirement buys type enforcement on 23 `REAL` columns. Without `STRICT`,
flexible typing lets a `TEXT` value sit in a numeric column and the C API coerces
it on read, so a reader returns a number that is not what is stored, silently.
For a format meant to be reimplemented from this document, a declared type that
is merely advisory is a trap. See ADR-0029.

### 3.1 Required pragmas on create

```sql
PRAGMA application_id = 1094994225;
PRAGMA user_version   = 1005;          -- schema 1.5
PRAGMA page_size      = 4096;          -- set before the first write; see §3.4
PRAGMA encoding       = 'UTF-8';
PRAGMA foreign_keys   = ON;
```

### 3.2 Required pragmas while a project is open

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;          -- FULL on removable/network volumes
PRAGMA busy_timeout = 5000;
```

`WAL` is what makes autosave-during-playback cheap and lets the render/export
path read a consistent snapshot while the user keeps editing.

### 3.3 Close policy — the WAL sidecar rule

**A cleanly closed `.adi` MUST be exactly one file on disk.**

While open, SQLite maintains `foo.adi-wal` and `foo.adi-shm` alongside it. A user
who copies or emails only `foo.adi` from that state gets a project that opens
*cleanly* but is silently missing everything since the last checkpoint — a worse
failure than an error.

On clean close, and on every explicit **Save As** / **Export**, the writer
**MUST**:

```sql
PRAGMA wal_checkpoint(TRUNCATE);
PRAGMA journal_mode = DELETE;
```

If `-wal` files are found next to a `.adi` at open time, the reader MUST let
SQLite recover them normally (that is the crash-recovery path working as
designed) and SHOULD tell the user the project was recovered from an unclean
shutdown, naming how much was recovered.

### 3.4 Page size

`page_size` **MUST** be 4096 unless a project's measured blob profile justifies
otherwise, and it can only be set before the first write. 4096 matches the write
granularity argument in RATIONALE §2: a single note edit dirties one or two
pages. A larger page size makes big blob reads marginally faster and small edits
strictly more expensive; we optimise for the edit.

### 3.5 Atomicity

Every user-visible action **MUST** be one SQLite transaction. A transaction that
writes core-tier data **MUST** also append its op-log row (§8.1) inside the same
transaction. There is no valid state in which the project changed but the op log
did not.

### 3.6 Concurrency

Exactly one process **MAY** have a project open for writing. The writer takes an
advisory lock row in `session_lock` carrying host name, PID, and a heartbeat
timestamp, so a second instance can say *"open in ADI DAW on STUDIO-PC since
14:32"* rather than silently corrupting expectations. A stale lock (heartbeat
older than 60 s) MAY be broken by the user after an explicit prompt.

Any number of readers MAY open the project read-only.

---

## 4. The time model

This is the most consequential decision in the format, because combining Ableton
(beat-native, everything warps) with Cubase (musical *or* linear time, per
track) means the format cannot pick one domain.

### 4.1 Two domains, declared per object

Every positioned object carries a `time_base`:

| `time_base` | Unit | Column | Meaning |
|---|---|---|---|
| `0` = musical | ticks | `pos_ticks` | Follows the tempo map. Moves when tempo changes. |
| `1` = linear | nanoseconds | `pos_ns` | Absolute wall-clock. Pinned to picture/timecode. |

An object **MUST** populate exactly the column matching its `time_base`; the
other **MUST** be `NULL`. Readers MUST NOT infer one from the other — the
conversion is a function of the tempo map and is the runtime's job, not the
file's.

### 4.2 Musical time: `ADI_PPQ = 5765760` ticks per quarter note

```
5765760 = 2^7 × 3^2 × 5 × 7 × 11 × 13
```

This constant is chosen, not arbitrary. It is divisible by:

- **every tuplet from 2 to 16** — triplets, quintuplets, septuplets, 11- and
  13-tuplets are all *exactly* representable, with no accumulated rounding.
  Conventional PPQs handle the common cases and then quietly stop: 960 (Logic)
  and 480 (Cubase) divide cleanly by 2, 3, 4, 5, 6 and 8, but **960/7 = 137.14…
  and 480/7 = 68.57…**, so a septuplet is rounded at every note, and 11- and
  13-tuplets are worse. Rounding compounds across a decade of edits, and it is
  unacceptable in notation, where a septuplet is not an approximation of
  anything.
- **128th notes, and 128th-note tuplets** (2^7).
- **every common interchange PPQ** — 24 (MIDI clock), 96, 192, 384, 480, 960 —
  so importing and re-exporting a MIDI file is lossless in both directions.

Range at `i64`: ±1.6 × 10¹² quarter notes ≈ 25,000 years at 120 BPM. Overflow is
not a practical concern; readers MUST still range-check, because a corrupt file
is not a hypothetical.

**Positions of events inside a clip are relative to the clip's origin**, not to
the project. Moving a clip is therefore one row update and touches no event data.

### 4.3 Linear time: `i64` nanoseconds

Nanoseconds, not samples, because a project's sample rate can change and every
sample-domain position would then be wrong. `i64` ns gives ±292 years.

Sample-exactness where it actually matters — the read position inside a source
audio file — is stored in **frames at that file's own immutable sample rate**
(§6.4), which is exact by construction and never needs rescaling.

### 4.4 Tempo and signature maps

`tempo_map` is a list of `(pos_ticks, bpm, curve)`. `curve` supports `jump`,
`linear` and `bezier` ramps, because Cubase-style tempo ramps and Ableton-style
tempo automation must both round-trip.

The tick↔nanosecond mapping is *derived* from this map and **MUST NOT** be
cached in the file. A cached mapping is a second source of truth and will
eventually disagree with the first. Runtimes cache it in memory.

`time_signature_map` is independent of `tempo_map`: they change at different
places and conflating them is a common and annoying format bug.

---

## 5. Layer structure

Five layers, each with a different compatibility contract:

| Layer | Contents | If a reader doesn't understand it |
|---|---|---|
| **0 — Container** | `application_id`, `user_version`, `adi_meta` | Refuse to open |
| **1 — Core** | Timeline, tracks, clips, events, routing, mixer | Refuse to open (major) / ignore column (minor) |
| **2 — Plugin state** | Opaque plugin byte streams + param mirror | Preserve; warn on render |
| **3 — Session** | Op log, undo tree, UI state, controller maps | Ignore safely |
| **4 — Extensions** | Namespaced opaque data | Preserve; warn if `essential` |

Layer 3 is the one a minimal third-party reader can skip entirely and still be
correct. That is deliberate: it keeps the barrier to writing a `.adi` reader low.

---

## 6. Layer 1 — the core model

Full DDL is in [`schema.sql`](schema.sql). This section specifies the parts a
DDL listing can't express.

### 6.1 Tracks

One `tracks` table, self-referencing via `parent_id`, ordered by
`index_in_parent`. `kind` covers: `audio`, `midi`, `instrument`, `group`,
`return`, `master`, `vca`, `marker`, `tempo`, `signature`, `chord`,
`arranger`, `video`, `transposition`.

**A group is one object** (ADR-0044): a collapsible container in the timeline
*and* a summing bus in the mixer. There is no separate `folder` kind. This
reverses what this section used to say — that Cubase's signal-free folder and
Ableton's bus are distinct and we model both. ADI follows Ableton, and carrying
the other alongside it would leave a container that looks like a group and
whose fader does nothing.

**A track with no `main` routing row routes to its parent, or to the master if
it has none** (ADR-0065). The default is a rule, not a row, so a project where
every track routes the obvious way stores no `routing` rows at all.

`routing.origin` records whether a connection was made by grouping (`'auto'`)
or by the user (`'user'`). An `'auto'` row is a materialisation of that
default and grouping keeps it pointing at the right place; a `'user'` row is
the user's own routing and grouping **MUST NOT** touch it. Re-parenting a
track **MUST** leave no state in which it is inside a group and routed
elsewhere by an `'auto'` row.

**`audio`, `midi` and `instrument` are hints** (ADR-0045). They set the icon,
the default device and what a double-click creates. A reader **MUST NOT** infer
from them what a track may contain: a MIDI clip on an `audio` track is legal,
and always was — `clips.track_id` has never consulted `tracks.kind`. The
remaining kinds name a role in the signal graph and do bind.

### 6.2 Clips and lanes

`clips` sit on a `(track_id, lane_id)` pair. `lanes` gives us take lanes and
comping for free: a comp is a set of clips across lanes on the same track with an
active-region selection, which is how both Ableton 11+ and Cubase model it.

A track may hold clips of **several kinds at once** — an audio clip and a MIDI
clip on one track is the normal case, not an edge case (ADR-0045). The device
chain receives both, and an instrument in that chain **adds** its output to the
audio already present rather than replacing it.

A clip is `kind ∈ {audio, midi, automation, video, marker}` and carries position,
length, loop window, fades, gain and mute. Loop fields are separate from position
and length so that a looping clip is *one* object rather than N repeats — the
Ableton model, and the correct one.

### 6.3 Event streams — the BLOB contract

Every core-tier BLOB begins with a **16-byte stream header**:

```
off  size  type     field
  0     4  char[4]  fourcc      'ANOT' notes, 'AAUT' automation,
                                'AEXP' note expression, 'ACTL' CC/controller,
                                'ASYX' sysex, 'AWRP' warp markers
  4     2  u16      version     layout version, starts at 1
  6     2  u16      rec_size    bytes per record in THIS blob
  8     4  u32      count       number of records
 12     4  u32      flags       bit0: records sorted ascending by time
                                bit1: time values are ns, not ticks
                                bits 2..31 reserved, MUST be 0
```

followed by `count × rec_size` bytes. All integers are **little-endian**. Floats
are IEEE 754.

> **This is the forward-compatibility mechanism for binary data, and it is
> mandatory.** A reader **MUST** stride by `rec_size` from the header, never by
> `sizeof(its own struct)`.
>
> `rec_size` **MUST** be either a size that some released version of this record
> type used, or greater than the reader's own record size. Any other value is
> rejected: it would land inside a field and tear it, yielding a value that is
> neither the writer's nor the documented zero default, with no error raised.
> See ADR-0023. If `rec_size` is larger than the reader knows, the
> extra tail bytes are a newer version's fields: skip them and preserve the blob
> byte-for-byte on save. If smaller, the missing fields take their documented
> defaults. This lets us add a field to every note in the world without a schema
> migration and without breaking older builds.

**Granularity rule (MUST):** a core-tier BLOB **MUST NOT** span more than one
user-visible editable object. One notes blob per MIDI clip. One automation blob
per lane per clip. Never one per track, and never one per project. See
RATIONALE §2 for why this rule is the difference between a fast format and a slow
one wearing a fast format's clothes.

#### 6.3.1 `ANOT` — note record, v1, 40 bytes

```
off  size  type  field           notes
  0     8  i64   start_ticks     relative to clip origin
  8     8  i64   dur_ticks       > 0
 16     8  u64   note_id         stable within clip; 0 = unassigned
 24     1  u8    key             0..127
 25     1  u8    vel_on          1..127
 26     1  u8    vel_off         0..127  (release velocity)
 27     1  u8    channel         0..15
 28     2  u16   flags           b0 mute, b1 selected, b2 has_expression,
                                 b3 tied_to_next, b4 ghost, b5..15 reserved
 30     2  u16   probability     0..10000 = 0.00%..100.00%
 32     4  f32   tuning_cents    -1200.0..+1200.0, per-note microtuning
 36     4  u32   reserved        MUST be 0
```

`note_id` is not decoration. It is the anchor for per-note expression (§6.3.2),
for op-log inverses that must identify *which* note moved, and for the AI agent
to reference a note without ambiguity. It MUST be stable across a save/load
cycle and MUST NOT be reused within a clip.

`probability` and `tuning_cents` are in v1 rather than bolted on later because
retrofitting a per-note field is the exact churn the stream header exists to
avoid — but it is still cheaper to reserve the space now.

#### 6.3.2 `AEXP` — per-note expression, v1, 24 bytes

Stored one row per `(clip_id, note_id, dimension)` in `note_expression`, so
editing one note's pressure curve rewrites only that curve.

```
off  size  type  field
  0     8  i64   time_ticks      relative to NOTE start
  8     4  f32   value           dimension-defined, see below
 12     4  f32   tension         -1.0..+1.0 curve shape
 16     1  u8    curve           0 hold, 1 linear, 2 exp, 3 log, 4 s-curve,
                                5 bezier (shape from `tension`)
 17     1  u8    flags
 18     6  —     reserved        MUST be 0
```

`dimension`: `0` pitch (semitones, ±48), `1` pressure (0..1), `2` timbre/slide
(0..1), `3` gain (dB), `4` pan (−1..1), `≥64` plugin-defined.

> **This `curve` enum is not the one in `tempo_map.curve`.** That column is a
> separate, smaller enum — `0` jump, `1` linear, `2` bezier — because a tempo
> ramp has no use for exponential or logarithmic shapes and a bezier tempo ramp
> is the common case. Two enums named `curve` in one format is a trap, so the
> difference is stated rather than left to be discovered.

**Resolution and rate (ADR-0054).** `value` is `f32`, so a 14-bit MPE+ controller loses nothing and neither would a 24-bit one; 7-bit and 14-bit are wire encodings and **MUST NOT** appear in a stored value. A reader **MUST NOT** reconstruct MPE member-channel allocation from `ANOT.channel`: that field records which channel a note arrived on, and allocating channels when playing *to* an MPE destination is an output-side decision made from the zone in force at that moment.

A writer **MAY** thin a captured stream and **MUST** record that it did. An exact capture and a thinned one are different documents, and a file that cannot distinguish them makes "we do not quantise" true of the bit depth and false of the data.

**This is first-class, not an MPE afterthought.** A Haken Continuum, a Roli, an
Osmose or a Seaboard produces continuous per-note pitch, pressure and timbre at
full control-rate resolution; Cubase VST Note Expression and Ableton MPE each
model part of this and neither is a superset. A format that treats per-note
expression as "MIDI channel tricks" throws away the performance. We store the
curves directly, decoupled from the 16-channel MPE transport that happened to
carry them.

#### 6.3.3 `AAUT` — automation point, v1, 32 bytes

```
off  size  type  field
  0     8  i64   time            ticks or ns per the lane's time_base
  8     8  f64   value           in the lane's declared value_domain
 16     4  f32   tension
 20     1  u8    curve           as §6.3.2
 21     1  u8    flags           b0 selected, b1 locked
 22     2  u16   reserved
 24     8  u64   point_id        stable identity
```

`automation_lanes.value_domain` declares whether `value` is `normalized`
(0.0–1.0, what a plugin API speaks), `real` (dB, Hz, ms — what a human reads), or
`enum`. Storing normalized values *only* is the common mistake: if the plugin is
missing or its mapping changes between versions, normalized automation becomes
meaningless, while `real` survives. Where both are knowable, writers SHOULD store
`real` and let the runtime map.

### 6.4 Audio clips and warping

`audio_clips` references a `media_files` row and stores its read window as
`src_start_frames` / `src_len_frames` **in frames at the source file's own sample
rate** (§4.3).

Warping is `warp_mode` plus an `AWRP` blob of warp markers pairing source frames
with musical ticks. Both DAWs' models fit: Ableton's always-on warp with a marker
grid, and Cubase's AudioWarp with hitpoint-derived markers, are the same data
with different UI over it.

### 6.5 There is no Session View

ADI is a **linear, arrangement-timeline DAW**. There is no clip-launching matrix,
and the format has no tables for one. ADR-0037 removed `scenes` and `clip_slots`.

This reverses ADR-0006, which had made them Layer 1 on the argument that
"Ableton and Cubase combined" required both paradigms as peers. It means
something narrower now: Ableton's **interface** — channel strips on the right,
the device chain along the bottom, one window — over Cubase's **arrangement and
audio-editing depth**, including comping, take lanes and crossfade control.

A consequence worth stating in the format rather than only in the UI docs: every
`clip` is on the timeline. `clips.track_id` is `NOT NULL`, and a clip carries a
position in the time domain it declares. There is no unplaced clip, so a reader
never has to ask where a clip lives.

### 6.6 Devices, chains, racks and macros

A track's signal path is an ordered list of `devices`. A device MAY own nested
`device_chains` (Ableton Instrument/Audio-Effect/Drum Racks), each with key,
velocity and chain-select zones. `macros` and `macro_mappings` give the 8/16-macro
model with per-target range and curve.

Modelling racks natively — rather than as "a plugin that happens to contain
plugins" — is what lets the AI agent reason about and build them.

### 6.7 Routing

One `routing` table for every signal connection: main outputs, sends, sidechains,
external inputs, and VCA control links. A row is
`(src_kind, src_id) → (dst_kind, dst_id)` with `kind ∈ {main, send, sidechain,
vca, cue}`, `gain`, `pan`, `pre_fader`, `enabled`.

Cubase Direct Routing (multiple simultaneous outputs per channel) falls out of
this for free; a one-output-per-track column would have made it impossible.

**The endpoint kinds are of two sorts, and a reader must treat them
differently:**

| Kind | `src_id` / `dst_id` is | A failed lookup means |
|---|---|---|
| `track`, `device` | a row id in that table | **corruption** — report it |
| `hw_in`, `hw_out` | a hardware port index on the current audio device | **normal** — the project opened on different hardware |

There is deliberately no `bus` kind: a bus here is a track whose `kind` is
`group`, `return` or `master` (§6.1). An earlier draft permitted `bus` and had no
table for it, which made a dangling reference expressible by construction —
see ADR-0029.

A polymorphic `(kind, id)` pair cannot carry a SQL `FOREIGN KEY`. That is the
price of one routing table rather than six, and it means these references are
**not** enforced by the database: a conforming writer MUST maintain them, and a
reader SHOULD verify them rather than assume.

### 6.8 Remarks (schema 1.2)

```sql
remarks(id, target_kind, target_id, param_id, author, actor_detail, text,
        created_utc, resolved)
```

A remark is a note anchored to a `track`, `clip` or `device`, and through
`param_id` to one parameter of a device (ADR-0131). It is project data: it
travels with the file, and every change is an op — `remark.add`, `remark.edit`,
`remark.resolve`, `remark.remove` (OPS.md §9.12) — so it undoes like any edit.

- `author ∈ {user, agent}` is who wrote the text, and the UI marks agent
  remarks distinctly (ADR-0131 d3). It is not `ops.actor`, which remains the
  record of who submitted the op.
- `created_utc` comes from the op payload. A writer MUST NOT stamp it from a
  clock inside the handler (OPS.md §7).
- `param_id` MUST be NULL unless `target_kind` is `device`.

`(target_kind, target_id)` is polymorphic like a routing endpoint (§6.7), so it
has no foreign key and nothing cascades. **Deleting a target leaves its remarks
in place**, and undoing the delete re-anchors them. A remark whose target is gone
is therefore a legal state, not corruption: a reader SHOULD report it as a
warning (`adi_tool check`: `remark.danglingTarget`) and MUST NOT delete it.

**Remark text is untrusted input.** A project is shared, so a remark may have
been written by anyone. The agent reads remarks as context and never executes
an action because a remark asked for it (ADR-0131 d5, AI-AGENT §7.3).

A 1.0 or 1.1 file has no `remarks` table. A 1.2 reader opening one MUST treat
that as "no remarks", not as an error.

---

## 7. Layer 2 — plugin state

```
plugin_refs    identity of a plugin: format, uid, vendor, name, version, path hint
devices        an instance of a plugin_ref on a chain
state_blobs    opaque device state, keyed by BLAKE3 hash, stored once
plugin_state   (device_id, stream_role, state_hash, format_hint)
plugin_params  (device_id, param_id, name, normalized_value, display_string)
```

`stream_role` exists because plugin state is not always one stream:

| Format | Roles |
|---|---|
| VST3 | `component`, `controller` (two separate `IBStream`s) |
| CLAP | `state` |
| AU | `classinfo` (the fully-qualified property-list dict) |
| LV2 | `state`, plus `files` for its file-reference extension |
| VST2 | `chunk`, or `params` if the plugin is not chunk-capable |

ADI hosts only the first two of those; the rest are here because the format records plugins it cannot load, so that a device is preserved rather than dropped. See §7.4.

Writing all of these into one column and hoping is the standard way DAWs lose
people's synth patches. They get separate rows.

### 7.1 The missing-plugin rule

`plugin_params` mirrors the plugin's parameters by name and value: every
parameter an op has written. It is the entire reason the project is still
workable when the plugin is missing, and when the plugin loads it can be newer
than the chunk (ADR-0142), because a chunk is recorded when the plugin signals a
change, not only at save:

- A writer that records a `plugin_state` chunk **MUST**, in the same
  transaction, rewrite every `plugin_params` row of that device to the value the
  plugin reports after the chunk. No row is then older than the chunk.
- A reader that loads a device's `plugin_state` **SHOULD** then apply every
  `plugin_params` row whose value differs from what the loaded plugin reports: by
  the rule above, such a row is an edit made after the chunk. A row the plugin
  already agrees with is not sent again, so a plugin whose parameters derive
  from its chunk is not fought.

A reader that encounters a `device` whose plugin is unavailable **MUST**:

1. preserve `plugin_state` byte-for-byte;
2. keep the device in the chain as a bypassed placeholder, retaining its position,
   its routing and its automation lane bindings;
3. surface the missing plugin's identity to the user — vendor, name, version and
   the path it was last loaded from;
4. re-inject the preserved state verbatim if the plugin becomes available later.

It **MUST NOT** silently drop the device, and it MUST NOT renumber the chain.
Dropping a device silently rewires the signal path, and the user finds out at
mixdown.

### 7.2 Opaque state is stored once, by hash

`plugin_state` holds a **hash**, not bytes. The bytes live in `state_blobs`,
keyed by their BLAKE3 digest, and op payloads and inverses reference the same
hashes (ADR-0038).

A VST3 chunk is opaque and routinely large. Storing it inline in `plugin_state`
and again in every `ops.inverse` that reverts to it makes the undo log the
largest object in the file, growing with the number of edits rather than the
size of the project. Two streams of one plugin, two devices loaded from the same
preset, and twenty tweaks that end where they started all cost one blob.

Writers **MUST** insert the `state_blobs` row before, or in the same transaction
as, any row or op that references it. Readers **MUST** treat a `plugin_state`
row whose `state_hash` is absent as a corrupt project, not as an empty state:
loading a plugin with blank state silently discards a patch, which is the exact
failure §7.1 exists to prevent.

Orphaned blobs — referenced by no `plugin_state` row and no live op — MAY be
collected during op-log compaction (§8.3) or an explicit vacuum. They MUST NOT
be collected at any other time, because an op that is only reachable through an
undo branch is still live.

### 7.3 What undo covers, and what it cannot

Parameter changes are captured exactly. A VST3 host is told about every
automatable parameter change through `beginEdit` / `performEdit` / `endEdit`, so
one user gesture becomes one `device.setParam` op with a before and an after —
small, symmetric and coalescable.

Opaque internal state is captured at **boundaries**: preset load, device insert
and remove, plugin editor close, project save, and whenever the plugin calls
`IComponentHandler2::setDirty`. There is no VST3 guarantee that a host is told
when a plugin's non-parameter state changes, and plenty of plugins never say.

> A conforming implementation **MUST NOT** claim that undo restores every
> third-party plugin change. It restores every parameter change exactly, and
> opaque state to the resolution of the boundaries above.

This is a limitation of the plugin APIs, not of this format, and stating it is
cheaper than a user discovering it with an hour of sound design at stake.

### 7.4 What ADI hosts, and what it only records

These are two different lists and conflating them loses data.

**Hosted by this implementation: VST3, and CLAP when it is written.** Nothing
else, and adding to that set takes an ADR (ADR-0041). In particular ADI does not
host **VST2**, **AU** or **AUv3**, and no build option turns that on.

**Recorded by the format: whatever was there.** `plugin_refs.format` admits
`vst3`, `vst2`, `clap`, `au`, `auv3`, `lv2`, `ladspa` and `internal`, and it
will keep admitting all of them. A reader that meets a `format` it cannot host
**MUST** apply §7.1 unchanged: preserve the state byte-for-byte, keep the device
in the chain as a bypassed placeholder, and surface what is missing. It **MUST
NOT** reject the file and **MUST NOT** drop the device.

This is what lets a converter from a macOS Logic or Live project produce a valid
`.adi`. If the format refused the string `au`, such a project could not be
represented at all, and the converter's only choices would be to fail or to
silently discard every device — which is the exact failure §7.1 exists to
prevent. Refusing to host a format costs us code we do not write; refusing to
*name* it costs a user their session.

A third-party implementation that does host AU is conforming, and a `.adi` it
writes is readable here. That is the point of specifying the format separately
from the application.

### 7.5 The expression route (schema 1.4)

```sql
device_expression_routes(device_id, route)   -- route: note_expression | mpe_midi | plain
```

How a device receives per-note expression: VST3 note expression, MPE over MIDI,
or plain MIDI with poly aftertouch (ADR-0097, ADR-0134 d7). **No row is Auto**:
the route resolves from what the plugin declares. A row records the user's
choice, and a reader **MUST** play the device on that route whatever its own
defaults say, because the route changes what the plugin plays. A reader that
cannot deliver the recorded route keeps the row and reports it. Written only by
ops (ADR-0146). A file older than 1.4 has no routes: every device is Auto.

### 7.6 The plug-in panel (schema 1.5)

```sql
device_panels(device_id)                          -- a row: the panel is configured
device_panel_params(device_id, ord, param_id)     -- its parameters, in order
```

The parameters a device shows as sliders in its panel (ADR-0150, ADR-0154).
**No `device_panels` row is the default**: a reader shows every modifiable
parameter of a plug-in that declares 64 or fewer, and none of one that declares
more, with a prompt to configure it. A `device_panels` row means the user
configured the panel, and it shows exactly its `device_panel_params` rows in
`ord` order, even none. A reader keeps a configured parameter the plug-in no
longer declares, shows it as missing, and does not drop the row. A file older
than 1.5 has no panels: every device shows the default.

---

## 8. Layer 3 — session state, and the op log

### 8.1 The op log

```sql
ops(seq, txn_id, parent_seq, ts_utc, actor, actor_detail,
    op_type, target_kind, target_id, payload, inverse, tags)
```

Every mutation to Layers 1, 2 and 4 **MUST** append an op row in the same
transaction that performs it (§3.5).

- `actor ∈ {user, agent, script, import, migration, remote}`. This column is the
  accountability record for the AI agent: *"what did it change, and when"* is a
  query, not a forensic exercise.
- `actor_detail` names the specific agent, model, script or importer.
- `txn_id` groups ops that must undo as one unit. An agent action, however many
  individual edits it makes, is one `txn_id` and therefore one Ctrl-Z.
- `inverse` holds the data needed to revert, so undo never has to replay from the
  beginning.
- `parent_seq` makes the history a **tree, not a stack** (§8.2).

### 8.2 The undo tree

`op_branches(id, name, head_seq, created_utc, is_current)`. The current branch
is the row with `is_current = 1`, and exactly one row has it — enforced by a
partial unique index, not by convention.

It lives there rather than in `session_state` because `session_state` is an
untyped key-value store: a branch pointer held there has no foreign key, so
nothing stops it naming a deleted branch. See ADR-0026.

Undoing and then doing something new does not destroy the branch you left; it
forks. This gives "try the agent's arrangement, don't like it, go back, and still
be able to return to it" — which for an AI-assisted DAW is not a luxury, it is
the difference between the agent being usable and being frightening.

### 8.3 Compaction

An unbounded op log grows without limit. Policy is stored in the project
(`adi_meta`): keep at most N ops or M days, whichever is larger, with a default
of 10,000 / 90 days. Compaction squashes the tail into a checkpoint and MUST NOT
cross a branch point that is still reachable from a named branch, nor drop an
op that a history snapshot names (§8.6).

### 8.4 UI and session state

`ui_view`, `window_state`, `session_state`: track heights, fold states, zoom,
scroll offsets, selection, playhead, loop, per-plugin-window rectangles *with
their monitor identity*, mixer layout, visible panels, last-used tool.

The monitor identity matters: restoring a plugin window to `x=3200` on a machine
that no longer has a second monitor puts it offscreen. Readers MUST clamp
restored windows to the currently available display arrangement.

### 8.5 Controller maps

`controller_maps` holds project-scoped MIDI/OSC/HUI bindings — control surfaces,
Stream Deck actions, Continuum mappings — with takeover mode (jump / pickup /
scale) per binding. Project-scoped rather than global, because a template for
orchestral mockups and one for a club track want different mappings, and because
a project handed to a collaborator should arrive with its controller layout
intact.

### 8.6 History snapshots (schema 1.3)

```sql
history_snapshots(id, name, op_seq, branch_id, created_utc, auto)
```

A history snapshot is **a name on a point in the log** (ADR-0128). It copies
nothing. `op_seq` is the head when it was taken (NULL: the root, before any op),
and `branch_id` records the branch that was current then. `branch_id` is a
record, not a pointer: a later fork can leave that branch holding another line,
and `op_seq` is what a revert follows (ADR-0140). This is not the `snapshots`
table, which holds mixer snapshots and track versions (Layer 1, OPS.md §9.9).

- **Revert never discards.** It moves the head to the snapshot's point by
  rewinding to the common ancestor and replaying forward, in one transaction,
  on the same branch or across branches. Whatever the head's line held beyond
  that point MUST stay reachable. If no other branch holds its tip and redo from
  the snapshot's point does not lead there, the writer creates a branch for it,
  named `before revert to '<name>'`.
- **Metadata, not ops** (ADR-0128 d5). Taking, renaming and reverting append no
  op row and are not undoable. The replay digest does not cover the table.
- **The automatic name** is `<Project Name> <YYYY-MM-DD HH:MM>` in local time,
  "Untitled" for an unnamed project. The time and the local offset are supplied
  by the caller. A writer MUST NOT stamp `created_utc` from inside the history
  layer, so that tests and replays are exact.

A file older than 1.3 has no `history_snapshots` table; a 1.3 reader treats that
as "no snapshots".

### 8.7 Agent requests (schema 1.4)

```sql
agent_requests(txn_id, request, actor_detail, created_utc)
```

What the user asked the agent, beside the transaction it produced (AI-AGENT
§6.8, ADR-0146). A writer that commits an agent's changeset **MUST** write the
row in the same transaction as its ops, with `actor_detail` equal to theirs.
Log metadata, like the ops rows: not an op, never undone, not in the replay
digest. `created_utc` is supplied by the caller. A file older than 1.4 has no
requests recorded.

---

## 9. Layer 4 — extensions

```sql
extensions(id, ns, key, scope_kind, scope_id, criticality, min_reader, data, mime)
```

- `ns` is a reverse-DNS namespace: `org.adidaw.experimental.foo`,
  `com.vendor.thing`. The `org.adidaw.` prefix is reserved for us.
- `scope_kind` / `scope_id` attach the row to any object — project, track, clip,
  device — so extensions are not forced to be project-global.
- `criticality ∈ {advisory, essential}`.

### 9.1 Rules

1. A reader **MUST** preserve extension rows it does not understand, byte for
   byte, across a load/save cycle.
2. A reader **MUST NOT** fail to open a project because of an unknown extension.
3. A reader that encounters an unknown `essential` extension **MUST** warn the
   user before render, export or bounce, naming the `ns` and `key`. It **SHOULD**
   warn on open. It **MUST NOT** silently produce audio that omits it.
4. `min_reader` lets a writer state the minimum schema version that can
   *interpret* the row, so a reader can tell "newer than me" from "foreign".

Rule 3 is the one that separates this from every "we'll just ignore what we don't
know" format. Silent degradation at render time is data loss with extra steps.

---

## 10. Media

### 10.1 The pool

`media_files` is a content-addressed pool:

```
id, hash_blake3, orig_name, rel_path, abs_path_hint, sample_rate, channels,
frames, format, duration_ns, embedded, size_bytes, imported_utc, missing
```

`embedded` is always 0 from schema 1.1 (§10.5).

Content addressing by BLAKE3 gives us, from one column: deduplication (the same
sample dropped in twenty times is one file), integrity verification (detect a
truncated or replaced file *before* it renders as silence), and reliable relink
(find a moved file by content, not by guessing at names).

### 10.2 Resolution order

A reader resolving a media file **MUST** try, in order:

1. `rel_path` relative to the `.adi`;
2. registered project media folders (the project's `audio/` first);
3. `abs_path_hint`;
4. the user's configured search paths, matched by `hash_blake3`;
5. mark `missing = 1` and surface a relink prompt.

A reader of a schema 1.0 file that still holds embedded media **MAY** resolve
it from `media_blobs` first; a writer **MUST** extract it (§10.5).

A hash mismatch at any step **MUST** be reported, never silently accepted. A
sample that has been replaced on disk by a different file of the same name is one
of the most disorienting failures in a DAW, and it is entirely detectable.

### 10.3 Referenced, always

Media is **referenced**, never copied into the database (ADR-0127). A 4-minute project that touches a
90 GB sample library must not become a 90 GB file.

### 10.4 Collect and Export

The database holds device state (`state_blobs`: plugin chunks, custom
wavetables, preset blobs) and event streams, and never a media file. To share or
archive a project, **Collect and Export** copies every referenced file into the
project's `audio/` folder, verifies each against `hash_blake3` (a mismatch stops
the export and names the file), rewrites its `rel_path`, and writes the `.adi`
and `audio/` into one ZIP64 archive, audio stored uncompressed. Extracted
anywhere, every relative path resolves (ADR-0127).

### 10.5 The retired embedding tables

Schema 1.0 could embed audio in `media_blobs`, flagged by `media_files.embedded`.
From 1.1 both are **retired**: kept in the DDL, empty, and locked by three
triggers that refuse an embedded flag and any blob chunk (ADR-0136). They are
not dropped because §11 promises that a 1.0 reader opens any 1.x file, and a
1.0 reader queries them. They are removed at schema 2.0. A 1.0 file that holds
embedded media is reported by `check` as an error (`media.embedded`) and must
be extracted before it is written again.

---

## 11. Compatibility rules

`user_version = schema_major × 1000 + schema_minor`.

| Situation | Required behaviour |
|---|---|
| `schema_major` > reader's | Open **read-only**. Offer Save As a copy. Never write. |
| `schema_minor` > reader's | Open read-write. Preserve unknown tables and columns. |
| Unknown table | Leave untouched. It survives because we never rewrite the file wholesale. |
| Unknown column | Leave untouched. Writers MUST use named-column `INSERT`/`UPDATE`, never positional. |
| Unknown extension | §9.1 |
| Unknown blob `rec_size` | §6.3 — stride by the header, preserve the tail |
| Unknown enum value | Treat as the documented default for that column and preserve the original on save |

**Writers MUST NOT use `SELECT *` or positional `INSERT`.** Both break silently
the moment a column is added, and "silently" is the operative word.

### 11.1 Upgrading an older minor (ADR-0144)

A minor only **adds** objects (tables, indexes, triggers). It never changes or
drops one. That is what makes an older file upgradable in place, and
`validate_schema.py` check 9 enforces it against the frozen schemas in
[`history/`](history/).

| Opening a file of our major with an older minor | Required behaviour |
|---|---|
| **for writing** | Upgrade it before anything else reads or writes it: in **one transaction**, create each later minor's objects in minor order, set `adi_meta.schema_minor`, and set `user_version` **last**. If any step fails, roll back everything: the file stays at its old version and the open fails with a clear error. |
| **read-only** | Never write. Every table the file lacks reads as **empty**. The reference implementation creates empty `TEMP` tables of the same names on its own connection, in one place (`Store::open`), so every reader is covered. |

- Existing rows are never rewritten or dropped. A 1.0 file with embedded media
  is upgraded as it is; the 1.1 triggers refuse only **new** embedded writes, and
  extracting the old media is a separate, explicit operation (ADR-0127 d3).
- The objects are created from the current `schema.sql`'s own statements, so an
  upgraded file matches a fresh one, ignoring comments inside a `CREATE`
  (SQLite stores those verbatim, and a migration never rewrites an existing
  table).
- A **newer** minor is never touched: no upgrade, no downgrade, no rewrite of
  `user_version`. A newer **major** opens read-only, as above.

Major version bumps are the expensive kind and we expect to make very few. The
mechanisms in §6.3 (blob tails), §9 (extensions) and the "unknown column"
rule exist specifically so that almost everything can be a minor bump.

---

## 12. What is deliberately not decided yet

Named here so they are visible gaps rather than accidental omissions:

1. ~~**Op payload encoding.**~~ **DECIDED** — CBOR (RFC 8949), encoded via
   `nlohmann/json`. Self-describing, so a newer version's op can still be
   inspected and preserved by an older reader; deterministic encoding is
   specified (§4.2), which matters because payloads reach content hashes and the
   text projection. See ADR-0016.
2. **The full op vocabulary.** Every op type, its payload and its inverse. This
   is a large document of its own and it gates the agent work.
3. **Score/notation data.** Cubase's score editor needs engraving information
   (enharmonic spelling, stem direction, beaming, layout) that is *not* derivable
   from MIDI. Reserved as Layer 1 tables, unspecified.
4. **Chord track and Expression Maps.** Both need native schema. Sketched in
   `key_map` and reserved; not specified.
5. **Video.** A video track kind exists; frame-rate/timecode/pull-up handling is
   not specified.
6. **Collaboration.** The op log is the right substrate for it. Whether we go
   CRDT or OT, and what the identity and conflict model is, is out of scope for
   0.1 and must not accidentally be foreclosed by 0.1's choices.
7. **Encryption / signing.** Out of scope. Note that SQLCipher exists if we ever
   want it, and that it changes the file header — so a `.adi` cannot be both
   encrypted and recognisable by `application_id`.

---

## 13. Reference

- Normative DDL: [`schema.sql`](schema.sql)
- Design rationale and rejected alternatives: [`RATIONALE.md`](RATIONALE.md)
- Decision log: [`../DECISIONS.md`](../DECISIONS.md)
- Feature scope this format must eventually carry: [`../FEATURES.md`](../FEATURES.md)
- Agent architecture built on the op log: [`../AI-AGENT.md`](../AI-AGENT.md)
