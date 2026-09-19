# ADI DAW

An open-source digital audio workstation with an open, documented, reimplementable
project format — combining Ableton Live's device model and screen layout with
Cubase's arrangement, editing and mixing depth, and an AI agent that can only act
through the same undoable operations a human uses.

**Status:** format specified, reference implementation building.
**627 checks across 8 suites**, green on 7 ABIs. Nothing is frozen.
**Language:** C++ with JUCE (ADR-0014) · **Licence:** GPLv3 (ADR-0015); a build
linking JUCE is a combined work with AGPLv3 obligations on the JUCE part (ADR-0048)

A long-term project built step by step. The format came first because it is the
only part that is genuinely expensive to change later; the store, op log, undo
tree and integrity checker are built on it now.

---

## Why start with the file format

Because it is the only part that is genuinely expensive to change later.

Every other decision — language, UI toolkit, which features ship first — can be
revisited by rewriting code. A format decision propagates into every project file
a user ever saves, and a format migration is the kind of release that loses
people's trust. So the format gets designed against the **full** feature set
(see [`docs/FEATURES.md`](docs/FEATURES.md)) even though the software will
implement a fraction of it for years, and the schema reserves space for things we
will not build until v3.

The audit in FEATURES.md §12 produces the result that matters: the v1.0 schema is
sufficient for everything ranked P0 and P1, so the first release can be built
without a format migration hanging over it.

---

## The format in one paragraph

A `.adi` file is a SQLite 3 database. Saving is proportional to what changed, not
to project size, so autosave runs during playback without a CPU spike. Commits are
atomic, so a crash cannot leave a half-written project. Data written by a newer
version survives untouched in an older one, because the file is never rewritten
wholesale. The file holds not just the music but the whole session — window
positions, zoom, selection, controller maps — and a persistent, *branching* undo
history that survives a reboot. Every mutation to a project is a typed, attributed
operation appended to a log in the same transaction that performs it, which is
what makes undo persistent, scripting free, and an in-DAW AI agent safe rather
than frightening.

---

## What's here

| | |
|---|---|
| [`docs/format/SPEC.md`](docs/format/SPEC.md) | The `.adi` format specification, v0.1 draft. Written so a third party can implement a reader from it without reading our source. |
| [`docs/format/schema.sql`](docs/format/schema.sql) | Normative DDL. Executable, and verified on every change. 37 tables. |
| [`docs/format/RATIONALE.md`](docs/format/RATIONALE.md) | Why SQLite, what we rejected, and the three errors in the original proposal that must not come back. |
| [`docs/FEATURES.md`](docs/FEATURES.md) | Ableton ∪ Cubase, prioritised P0–P3, each row checked against the schema. |
| [`docs/AI-AGENT.md`](docs/AI-AGENT.md) | The agent's architecture, capability tiers and guardrails. |
| [`docs/UI-ARCHITECTURE.md`](docs/UI-ARCHITECTURE.md) | The Ableton-shaped shell, the component tree, and how the graph carries a hybrid track. |
| [`docs/OPS.md`](docs/OPS.md) | The op vocabulary: descriptor, scopes, engine impact, inverses, CBOR encoding, first tranche. |
| [`docs/DECISIONS.md`](docs/DECISIONS.md) | Decision log. Append-only. 52 entries. |
| [`docs/EXTERNAL-CODE.md`](docs/EXTERNAL-CODE.md) | The nine external repos we read or link against, and the licence boundary between them. Read before copying a line out of `reference/`. |
| [`tools/validate_schema.py`](tools/validate_schema.py) | Proves the DDL executes, FKs resolve, and UNIQUE indexes actually enforce uniqueness. |
| [`tools/validate_ops.py`](tools/validate_ops.py) | Checks the 160-op catalogue: unique names, inverses, scope rules, coalescing, and that the prose count matches the tables. |
| [`tools/fetch_external.sh`](tools/fetch_external.sh) | Clones/refreshes `third_party/` and `reference/`. Both gitignored, pinned by tag and commit. |
| [`tools/test_all.sh`](tools/test_all.sh) | Every test binary, both validators, and the spec-vs-binary layout check. Binaries are discovered, not listed. |
| [`tools/build.bat`](tools/build.bat) | Windows build. A `.bat` because `vcvars64` must run in the same shell. |
| [`src/adi/`](src/adi/) | The reference implementation: `blob` (SPEC §6.3 layouts), `store` (the `.adi` itself), `ops` + `ops_catalog` (50 ops), `history` (branching undo), `digest` (the replay oracle), `check` (what SQLite cannot enforce), `textproj` (the canonical text projection). |
| [`LICENSE`](LICENSE) | GPLv3. Our code. JUCE is AGPLv3 and a build that links it carries AGPL obligations on that part — ADR-0048. |

## The tool

```bash
adi_tool create <file>   # a new .adi
adi_tool info <file>     # what is in it
adi_tool ops             # every registered op, with scope and engine impact
adi_tool digest <file>   # canonical digest of the project tier (the replay oracle)
adi_tool check <file>    # verify what SQLite structurally cannot
adi_tool export <file>   # the canonical text projection, for git diff (ADR-0007)
```

## Verifying the schema

```bash
python adi_daw/tools/validate_schema.py
```

Requires nothing but a Python with `sqlite3` (i.e. any Python). It executes the
DDL into an in-memory database, checks container identity, resolves every foreign
key, catches UNIQUE indexes that don't enforce uniqueness because SQLite treats
NULLs as distinct, and inserts a minimal project with `foreign_keys=ON`.

Run it after any change to `schema.sql`. It has already caught two real bugs.

---

## Design commitments

These are the load-bearing ones. Full reasoning in
[`docs/DECISIONS.md`](docs/DECISIONS.md).

1. **The audio thread never touches SQLite.** The format is a persistence layer,
   not a runtime model. No I/O, no allocation, no locks in the audio callback —
   the DB loads into a heap model that publishes immutable snapshots by atomic
   pointer swap. (ADR-0010)
2. **Every mutation is a typed, attributed op.** This is what makes undo
   persistent and branching, scripting free, and the AI agent structurally
   incapable of doing anything a user couldn't undo with one keystroke. (ADR-0003)
3. **Linear only: there is no Session View.** The clip-launching matrix was
   core schema until ADR-0037 cut it. The *layout* still follows Ableton —
   channels right, device chain along the bottom — while the arrangement and
   the editing depth behind it follow Cubase. Layout and clip matrix were never
   the same claim. (ADR-0037, superseding ADR-0006)
4. **Per-note expression is first-class.** Continuum, Osmose, Seaboard and MPE
   performances are stored as real curves, decoupled from the 16-channel
   transport that carried them. Neither Ableton's nor Cubase's model is a
   superset; storing it as "MIDI channel tricks" loses the performance
   irreversibly. (SPEC §6.3.2)
5. **A missing plugin never silently drops a device.** State is preserved
   byte-for-byte, the device stays in the chain bypassed, parameters are mirrored
   in readable form. (ADR-0011)
6. **Unknown data is preserved — and unknown *essential* data warns before
   render.** Preservation alone protects the file but not the user. (ADR-0012)
7. **Interop is a converter, not a shared file.** Steinberg and Ableton are not
   going to open our format. `.dawproject`, MIDI, AAF and stems are import/export
   paths. Claiming otherwise would be naive. (RATIONALE §4)

---

## Roadmap

Each step gates the next. No step starts before the previous one is written down.

| | Step | Status |
|---|---|---|
| **1** | Format spec, schema, feature scope, agent design | **done, draft** |
| **2** | Choose implementation language and licence | **done** — C++/JUCE, GPLv3 |
| **3** | The op vocabulary: every op type, payload, inverse | **done** — 160 ops, `docs/OPS.md` |
| **4** | Reference reader/writer library + round-trip test corpus | **done** — store, ops, undo, digest, check |
| **5** | Audio engine skeleton: snapshot handoff, model, transport | **done** — headless, no JUCE (ADR-0036) |
| **6** | JUCE: audio device, the graph, VST3 hosting (ADR-0041), large-block engine (ADR-0042) | **next** |
| **7** | Minimal arrangement UI — the first thing you can make a track in | |
| **8** | The agent, at Observe tier only | |
| **9** | Propose and Apply tiers, and the RPC boundary (ADR-0039) | |
| **10** | Visual patching devices — Pure Data via `libpd` (ADR-0035, ADR-0040) | direction decided, contract not designed |

---

## Open questions

Named so they stay visible:

- **CBOR key-ID registry.** Op payloads use integer map keys; they need an
  actual assignment table and a rule for extending one. (OPS.md §10)
- **Contributor CLA** — keeps relicensing possible, deters the contributors a GPL
  project attracts. Much harder to add once there are contributors. (ADR-0015)
- **Repo home** — this lives in the `Adi` monorepo while it is design work, and
  should graduate to its own repository before the first public commit.
  (ADR-0013)
- Five format gaps that block specific P2/P3 features, listed in FEATURES.md §12.

---

## A note on scope

Combining two of the most mature pieces of software in the industry, and adding an
AI agent, is an enormous amount of work — Ableton and Cubase represent something
on the order of a thousand engineer-years between them. Nothing in this repository
pretends otherwise.

What makes it tractable is sequencing: the format is small enough to get right,
and getting it right is what allows everything after it to be built incrementally
without invalidating what users have already saved.
