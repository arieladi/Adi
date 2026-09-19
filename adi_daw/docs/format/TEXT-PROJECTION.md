# The canonical text projection — design

**Status: design, not yet implemented.** `docs/` is the source of truth; when this
and `src/adi/textproj.*` disagree, one of them is a bug and this document says
which. Implements ADR-0007. Required by ADR-0021.

---

## 1. What this is, and what it is not

`adi export --text` renders a `.adi` as deterministic, canonical, line-oriented
text. Per ADR-0007 it is **derived, never authoritative**, and round-tripping
through it is not a supported workflow. Import is an open question in that ADR
and this document does not answer it — nothing here should be read as a grammar
for a parser we intend to write.

It has exactly two consumers, and they want different things:

1. **A human running `git diff`.** The people who would use this DAW are the
   people who put projects in version control. This consumer wants a small diff
   for a small change, and wants to read it without a decoder ring.
2. **ADR-0021's replay test.** Apply an op log twice under deliberately different
   UI state, project both, assert byte-identical. This consumer wants no
   ambient state to reach the output, at any cost in readability.

Where they conflict, consumer 2 wins, because consumer 2 is a correctness oracle
and consumer 1 is a convenience. This document flags every place they conflict.

### The two hard requirements

**R1 — byte-identical under replay.** Two projections of the same project,
produced on different machines, with different UI state, different SQLite
versions, different locales, different insertion histories and different
compilers, must be byte-identical.

**R2 — nothing outside Layers 0–2 appears.** Not as content, and not as an
*ordering influence*. Specifically forbidden: row ids and any surrogate key;
insertion, storage or rowid order; map or hash iteration order; and everything in
Layer 3 (op log, undo tree, `ui_view`, `window_state`, `session_state`,
selection, playhead, zoom, tool mode, controller maps).

R2 is why `controller_maps` is excluded despite being project-scoped and
genuinely interesting to diff. It is Layer 3, and Layer 3 is the layer ADR-0021's
test deliberately varies.

---

## 2. The central design decision: containment over reference

An object with exactly one owner does not need to be *referenced*. It needs to be
*printed inside its owner*. Applied consistently, this converts a reference into
containment and the identifier disappears with no addressing scheme at all.

That one move eliminates roughly twenty of the twenty-seven id-bearing relations
in the schema, including:

`mixer_strip.track_id` · `audio_clips.clip_id` · `plugin_state.device_id` ·
`plugin_params.device_id` · `macros.device_id` · `macro_mappings.macro_id` ·
`note_expression.clip_id`+`note_id` · `event_streams.clip_id` ·
`automation_data.lane_id` · `automation_lanes.owner_kind`/`owner_id` ·
`clips.track_id` · `clips.lane_id` · `lanes.track_id` · `devices.chain_id` ·
`device_chains.parent_device_id`/`track_id` · `tracks.parent_id` ·
`media_blobs.media_id` · `extensions.scope_kind`/`scope_id` ·
`devices.plugin_ref_id` · `plugin_refs.shell_id`

Three consequences worth stating on their own:

- **`note_expression` prints inside its note.** This is what removes `note_id`
  from the output entirely — the single largest source of opaque identifiers in
  any MIDI-bearing project.
- **Automation lanes print inside their owner.** A device's lane sits under that
  device; a routing row's lane sits under that routing line. `owner_kind` /
  `owner_id` never appears.
- **`plugin_refs` is not projected as a table.** It is a pure normalisation
  table holding nothing absent from the device itself, so the plugin identity is
  inlined on the device line. A shared plugin's version then appears once per
  instance — which is correct: you *want* to see that all forty Serum instances
  moved to 1.372 in one diff hunk.

### The seven that genuinely remain

1. `routing` endpoints (track / device / hardware port / bus)
2. `mixer_strip.vca_group_id` → track
3. `clip_slots` → track, scene, clip
4. `clips.alias_of` → clip
5. `audio_clips.media_id` and `tracks.freeze_media_id` → media
6. clip-scoped `automation_data` → its lane
7. `macro_mappings.target_device_id` → device

These get **designators**: quoted, slash-separated paths of content-derived
labels. See §6.

---

## 3. Physical form

- UTF-8, **no BOM**. LF only; a CR anywhere in the output is a bug.
- Indentation is exactly two spaces per level. Tabs never appear as indentation.
- No trailing whitespace. No line is only spaces. The file ends with one LF.
- Exactly one blank line between top-level sections; never inside a block.
- **All string ordering is `memcmp` over UTF-8 bytes.** Never locale collation,
  and never after Unicode normalisation.

That last rule is load-bearing and deserves its reason. Normalising to NFC before
rendering — which one panel design proposed — makes the output depend on the ICU
version linked into the build. Two machines with different ICU versions then
produce different bytes and R1 fails. The cost is that NFC "é" and NFD "é" are
distinct names that do not collide; that is the correct trade, because the
alternative imports a versioned dependency into a determinism claim.

### Defaults are omitted

An attribute equal to its DDL default for the schema version named in the header
is not emitted. This is the single largest readability and diff win: a default
track is one line rather than twenty.

It also silently couples the projection to `schema.sql`, so the header carries a
`defaults` generation number. When a minor schema bump changes a default, the
projection changes; stamping the generation makes that visible in the diff
instead of arriving as a mystery rewrite.

### Derived fields are never emitted

`StreamHeader.count`, `StreamHeader.flags` bit 0 (the projection always sorts),
and `NoteFlags::HasExpression` (the nested expression block is the truth). Where a
stored derived field disagrees with the derivation, the projector writes the
derivation and reports the disagreement **on stderr, never in the file** — a
lint line in the output would itself be content, and content that appears only
sometimes breaks R1.

---

## 4. Escaping

Inside a quoted string, exactly these escapes and no others:

```
\\   \"   \n   \r   \t   \u{XXXX}
```

`\u{XXXX}` is one to six uppercase hex digits naming a scalar value; surrogates
are not representable.

A seventh escape, `\x{HH}`, names **one raw byte that is not valid UTF-8**.
Added during implementation, because names arrive from a `TEXT` column with no
validation and SQLite will store whatever bytes it was handed — so invalid UTF-8
is reachable input, not a hypothetical. Rejecting it would make the projector
non-total, which the determinism review penalised for good reason; substituting
U+FFFD would be lossy and would silently merge two distinct names into one
label. Escaping the byte is total and injective. Overlong encodings, encoded
surrogates and truncated sequences are all handled this way, byte by byte.

A character **MUST** be escaped if it is `"` or `\`; any C0 control
(U+0000–U+001F) or U+007F; any C1 control (U+0080–U+009F); U+2028 or U+2029;
U+FEFF; or any **bidi control**: U+061C, U+200E, U+200F, U+202A–U+202E,
U+2066–U+2069.

The bidi rule is not pedantry, and it is the one rule here that exists for a
security reason rather than a determinism one. U+202E RIGHT-TO-LEFT OVERRIDE in a
track name visually reverses the remainder of the rendered line, so a diff hunk
can be made to *display* as something other than what it says. A format whose
entire purpose is human review must not be forgeable by its own content.
Escaping makes the attack visible as `\u{202E}`.

---

## 5. Numbers

### 5.1 Floats

Every float is rendered as **the shortest decimal string that round-trips to the
identical bit pattern**, at the width it is stored at.

- `f64` columns round-trip through `double`.
- **`f32` fields round-trip through `float` and are never widened.**
  `AEXP.value`, `AEXP.tension`, `AAUT.tension` and `ANOT.tuning_cents` are `f32`
  ([blob.hpp](../../src/adi/blob.hpp)). Formatting an `f32` via `double`
  produces `0.10000000149011612` where `0.1` is correct and sufficient.
- `-0.0` renders `-0.0`, distinctly from `0.0`. They are different bit patterns
  and ADR-0021's oracle compares bytes.
- **Every finite value contains a `.` or an `e`**, so a float never renders as a
  bare integer and `-0.0` is visibly distinct from `0.0`. `std::to_chars` emits
  `-0` and `16777216`; both are valid shortest round-trips and neither is a
  canonical form, so the projection appends `.0`.
- **Exponents are `e308` and `e-308`** — no `+`, no zero padding. `1e+308` and
  `1e308` are again both valid shortest round-trips. Pinning the shape here
  means the format does not silently change if a standard library revises its
  output.
- Non-finite values render `nan` and `inf` / `-inf`. **NaN payloads and
  signalling NaNs are not rendered**, because payload preservation is not
  consistent across the compilers in ADR-0022's matrix — notably x87 on the i386
  leg quietens signalling NaNs in transit. A design that renders payloads fails
  R1 on our own CI.

### 5.2 The typing hazard — a finding for `win`

**`schema.sql` declares zero `STRICT` tables**, and has 23 `REAL` columns. Under
SQLite's flexible typing every one of them can legally hold a `TEXT` or `BLOB`
value, so `sqlite3_column_double()` will silently coerce and the projection will
print a number that is not what is stored.

The projection handles this by rendering the *stored type* when it is not the
declared one — `gain !text("loud")` — so corruption is visible rather than
laundered. But the real fix is `STRICT` tables, and that is `win`'s call.

### 5.3 Ordering never sorts rendered text

Integers sort as integers. This is a correctness rule, not a style one: a
bytewise comparison of rendered position tokens sorts `10|1|0` before `2|1|0`,
which is deterministic and wrong-looking, and will be reported as a bug forever.
Sort keys are raw values; rendered tokens are output only.

---

## 6. Names, labels and designators

### 6.1 Label

A member's **label** is:

1. its base label — `name` for tracks, scenes, lanes, devices, markers, sections
   and clips; the plugin name for a device with an empty name; `param_ref` for an
   automation lane; the hash prefix for media;
2. if that is empty, a kind-specific fallback. For **unpositioned** objects this
   is the 0-based rank in the container's fixed order, written `#3`.

**Positioned objects do not fall back to their position.** The panel's winning
design used the rendered position token as the label for an unnamed clip, and a
judge caught the consequence: the label *is* the position, so moving the clip
changes its label, re-sorts it, relocates its whole block and churns every
designator citing it. An unnamed clip uses `#n` like everything else.

3. If a label collides with a sibling's, **every** colliding member gets `~k`
   appended — `k` being its 1-based rank among equally-labelled siblings in the
   container's content order. All of them, including the first, so the presence
   of `~` is itself the signal "this name is not unique here".

A literal `/` in a segment escapes to `\u{2F}`. Designators are always quoted, so
renaming `Bass` to `Bass Gtr` cannot change the token's shape.

### 6.2 Designator

```
"/trk/Rhythm/Drums"                    "/trk/Rhythm/Drums/dev/Serum"
"/trk/Bass/clip/Verse"                 "/trk/Bass/auto/volume"
"/trk/Bass/auto/volume"                "/media/1f4a9c2e7b0d3a51"
"/trk/Rhythm/Drums/dev/Serum/macro/Drive"
```

The top-level track forest is **role-partitioned** from `tracks.kind` into
`/trk`, `/ret`, `/vca`, `/master` and `/glob`. There is no `/scene` space:
ADR-0037 removed Session View, so `scenes` and `clip_slots` are gone from
Layer 1 and nothing addresses a slot. Sends overwhelmingly target
returns and the master, and those spaces are untouched by inserting an audio
track — a cheap halving of insertion churn, taken from the path-addressed design.

### 6.3 Media is the one place content addressing is right

`media_files.hash_blake3` is not a surrogate: the format already made content the
identity of a media file, for dedup, integrity and relink. Media designators are
`"/media/<hash prefix>"` — the shortest prefix of at least 16 hex digits unique
in the pool, rounded up to a multiple of 4.

**Caveat, verified:** `idx_media_hash` is a plain index, **not unique**
(`schema.sql:526`), so two rows may share a hash. When they do, the prefix cannot
disambiguate and the colliding rows take `~k` by §6.1.3. Worth `win` considering
whether that index should be `UNIQUE`.

### 6.4 Unresolvable references

A dangling FK renders `"!unresolved(<kind>)"`, never a number, and `--strict`
exits non-zero.

**A schema finding, verified:** `routing.src_kind` and `routing.dst_kind` both
permit `'bus'`, but **there is no `buses` table** in `schema.sql`. Every `bus`
endpoint is therefore unresolvable by construction — a CHECK constraint that
admits a reference kind with no referent. Either the table is missing or the
enum member is. `win`'s call.

---

## 7. Ordering

Every collection needs a **total** order that is a function of content alone.
This is the hardest requirement in the document, because — as established before
the panel ran — no ordering column in the core model is uniqueness-enforced:

| table | ordering column | unique? |
|---|---|---|
| `tracks` | `index_in_parent` | no |
| `lanes` | `ord` | no |
| `clips` | `pos_ticks` | no |
| `devices` | `ord` | no |
| `device_chains` | `ord` | no |
| `automation_lanes` | *none* | — |
| `markers` | `pos_ticks` | no |
| `scenes` | `ord` | **yes** |

Notes inside an `ANOT` blob and points inside an `AAUT` blob have no ordinal at
all and can collide on time.

### 7.1 The chain

For each collection, in order, until a strict order is reached:

- **K1 — declared semantic keys**, per collection (§7.3), compared as raw values.
- **K2 — the member's own skeleton projection**, compared by `memcmp`. The
  skeleton is the member's full recursive projection with every cross-reference
  token replaced by a single `?`. This is what breaks the circularity: order
  depends on skeletons, skeletons contain no designators, designators depend on
  order.
- **K3 — refinement.** For members still tied, recompute the skeleton with each
  tied member's neighbours labelled by their K2 class rather than by `?`, and
  repeat to a fixed point. This lets the reference graph speak.
- **K4 — permutation minimisation.** If a class is still non-singleton, emit the
  lexicographically least rendering over all permutations of that class.

### 7.2 Why K4 rather than "individualise one member"

The winning panel design's final step was: *if a class is still non-singleton,
individualise its skeleton-least member and re-run refinement.* The determinism
judge's objection is correct and fatal — a class is non-singleton at that point
*precisely because its members are indistinguishable*, so "the skeleton-least
member" is an arbitrary pick wearing a canonical hat.

K4 is canonical by construction: minimising over the permutations of the tied
class does not choose a member, it chooses the least output. Members that tie
through K4 are byte-identical in the rendering, so which one is "first" is
unobservable — the automorphism argument, which is the correct way to close this,
and which the name-addressed design got right even though its σ* construction
did not.

K4 is factorial in the class size. **The class size is bounded at 8**; above
that, the projector fails loudly rather than producing output. This is a
deliberate choice of a total-but-refusing projector over a total-but-arbitrary
one, and it is a real limitation, recorded in §11.

### 7.3 Declared keys per collection

| collection | K1 |
|---|---|
| tracks (siblings) | `index_in_parent`, kind, name |
| lanes | `ord`, kind, name |
| clips | `pos_ticks`, `length_ticks`, kind, name |
| notes | `start_ticks`, `key`, `channel`, `dur_ticks`, `vel_on` |
| expression points | `time_ticks`, `value` |
| automation points | `time`, `value` |
| automation lanes | `param_ref` |
| devices | `ord`, name |
| device chains | `ord`, name |
| scenes | `ord` |
| markers | **`pos_ticks` (raw), then kind, then name** |
| routing | src designator, dst designator, kind |
| macros | `ord`, name |

Markers are ordered by **raw ticks**, not by the rendered `bar|beat|tick` token.
Ordering on the rendered token couples marker order to the time-signature map, so
editing a meter reorders unrelated markers — ordering on derived content, which
is a subtler form of the same mistake as ordering on a row id.

### 7.4 The `(ord n)` token

The stored ordinal is emitted **only when it disagrees with the member's 0-based
document position**. In a healthy project that is never, so there is no churn;
when the schema defect above actually bites, it becomes visible on the exact line
it affects, inside the diff, rather than being silently normalised away.

---

## 8. Time

Positions render `bar|beat|tick` with a raw-tick gloss where exactness matters.
**Durations render as reduced fractions of a whole note** — `1/16`, `3/8`,
`1/12`, `1/80`.

**A duration only reduces to a small fraction when it is on a grid.** Recorded,
unquantised performance is the common case and does not reduce at all:
1441441 ticks is 1441441/23063040 in lowest terms, which is exact and useless.
So a denominator above **1024** renders as raw ticks with a `t` suffix instead.
Both forms are exact; the fraction is the readable one when it exists and the
tick count is honest when it does not.

The achievable denominators are exactly the divisors of a whole note, and a
whole note is `2^9 * 3^2 * 5 * 7 * 11 * 13` — four times the ADI_PPQ
factorisation in SPEC 4.2. So 1024 is not itself reachable; **512 is the
power-of-two ceiling**, and the bound is a cutoff rather than a target.

This is taken from the name-addressed design and it is strictly better than the
winner's `beats|ticks`. Most notes in most music are sub-beat, so under
`beats|ticks` a sixteenth reads `0|1441440` and the duration column is
unreadable on the majority of lines in the file. `1/16` is exact, independent of
the tempo and signature maps, and directly readable as a note value.

**Position glosses are clip-relative inside a clip**, never absolute. The
path-addressed design rendered absolute glosses on notes, which means moving a
clip rewrites the gloss on every note inside it — a 400-note clip move becomes a
400-line diff for a one-field change.

Linear-time objects render `s` / `ms` / `ns` per their declared `time_base`.

---

## 9. What is projected

| layer | treatment |
|---|---|
| 0 Container | header only: format version, `schema`, `ppq`, `defaults` generation |
| 1 Core | fully projected |
| 2 Plugin state | identity and parameters projected; opaque bytes as digest only |
| 3 Session | **excluded entirely** — this is what makes ADR-0021's test possible |
| 4 Extensions | namespace, key, scope and a digest; `essential` flagged |

### 9.1 The pipeline

The circularity is the whole difficulty, and it is worth stating plainly before
the resolution:

    a container's ORDER depends on its members' SKELETONS,
    a member's DESIGNATOR depends on that order,
    and a member's rendering contains designators.

If rendering fed ordering, the projection would be defined in terms of itself.

**It is broken by rendering twice.**

**Pass 1 — bottom-up.** Render every node's *skeleton*: its full recursive form
with each cross-reference replaced by the single token `?`. A skeleton therefore
contains no designator, so ordering may depend on it without circularity. Each
child collection is ordered separately — clips among clips, devices among
devices, since a clip and a device never compete for a position and their
designators differ by selector anyway. Bottom-up because a node's skeleton
contains its children's skeletons, so children must be ordered first.

Sibling references become the edges K3 refines on. A reference to a non-sibling
cannot separate two siblings at that level, and is left to K2, which already
holds it as `?` in both.

**Pass 2 — top-down.** Every order is now fixed, so `assignLabels` is
determined, and a label plus its parent's path is a designator. Then the real
text is emitted with references resolved.

**Containment cycles.** `tracks.parent_id` has no constraint forbidding a loop,
so pass 1 marks nodes in progress and renders a revisited node as `<cycle>`
rather than recursing forever. A corrupt file must produce output, not a stack
overflow.

**Status.** If any collection's order could not be made canonical the whole
projection reports `Ambiguous`. ADR-0021's oracle should require `Exact`:
comparing bytes only means something if the bytes were canonical.

### Opaque plugin state

A `plugin_state` blob renders as its length and a BLAKE3 digest, never as a hex
dump and **never as a chunked digest ladder**. A byte offset inside an opaque
vendor stream is not actionable by any human reading a diff; "the Serum state
changed, 48KB" is the entire useful signal.

`plugin_params` is the readable mirror and is projected. One caution inherited
from the panel: if a host rewrites its parameter mirror on every save without
semantic change, `params` becomes diff noise; that is a plugin-hosting problem to
revisit at step 6, not a projection problem to solve now.

---

## 10. Enforcement

**The replay test (ADR-0021).** Apply a log twice under different UI state,
project both, assert byte-identical. This document exists so that test can.

**A column-coverage check, in CI.** Walk `PRAGMA table_info` for every table in
`schema.sql` and fail unless each column is either projected or on an explicit
exclusion list with a reason. Two independent judges called this the only
mechanism proposed by anyone that structurally prevents a newly added column
being invisible in every diff forever — a failure mode no replay test catches,
because both replays omit it equally. It is the standing "if you write a number
into a doc, add it to a validator" rule applied to schema coverage.

**A referential invariant, grep-checkable.** Every designator cited must resolve
to exactly one object that the document also defines. The projector fails on a
dangling cite; a reviewer can check it with one search.

---

## 11. Known limitations

1. **The projector refuses rather than guesses** above 8 indistinguishable
   siblings (§7.2). Total-but-refusing beats total-but-arbitrary, but it means
   a pathological corpus member cannot be projected at all, and ADR-0021's
   oracle cannot run on it.
2. **A rename churns every reference to the renamed object.** That is inherent
   to name addressing and it is the price paid to avoid rewriting every
   designator on every insertion. Insertion is the more frequent edit.
3. **`--strict` is required to catch unresolvable references.** The default
   renders `!unresolved(...)` and continues, so a broken project still projects.
4. **Nothing here is implemented.** Every number in this document about diff
   sizes comes from the design panel's worked examples, not from running code.

---

## 12. Open, for `win`

Three schema findings surfaced while designing this, all verified against
`schema.sql` at `bcf9212`:

1. **No ordering column in the core model is unique** except `scenes.ord`. The
   projection now defines a total order regardless, so this is no longer
   blocking — but a duplicate ordinal is a bug wherever it occurs, and `UNIQUE`
   constraints would let the store layer reject it at write time instead of the
   projection papering over it at read time.
2. **Zero `STRICT` tables, 23 `REAL` columns.** Any of them can legally hold
   `TEXT`.
3. **`routing` permits `src_kind`/`dst_kind` = `'bus'` and no `buses` table
   exists.** A CHECK constraint admitting a referent that cannot exist.
