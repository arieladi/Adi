# ADI DAW

An open-source digital audio workstation with an open, documented, reimplementable
project format — combining Ableton Live's clip-launching and device model with
Cubase's arrangement, editing and mixing depth, and an AI agent that can only act
through the same undoable operations a human uses.

**Status: design. No code. Nothing is frozen.**
**Language:** C++ with JUCE (ADR-0014) · **Licence:** GPLv3 (ADR-0015)

This is a long-term project being built deliberately, step by step. Step 1 — the
save format and the feature scope it has to carry — is what's in this directory.

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
| [`docs/format/schema.sql`](docs/format/schema.sql) | Normative DDL. Executable, and verified on every change. 38 tables. |
| [`docs/format/RATIONALE.md`](docs/format/RATIONALE.md) | Why SQLite, what we rejected, and the three errors in the original proposal that must not come back. |
| [`docs/FEATURES.md`](docs/FEATURES.md) | Ableton ∪ Cubase, prioritised P0–P3, each row checked against the schema. |
| [`docs/AI-AGENT.md`](docs/AI-AGENT.md) | The agent's architecture, capability tiers and guardrails. |
| [`docs/OPS.md`](docs/OPS.md) | The op vocabulary: descriptor, scopes, engine impact, inverses, CBOR encoding, first tranche. |
| [`docs/DECISIONS.md`](docs/DECISIONS.md) | Decision log. Append-only. 22 entries. |
| [`docs/EXTERNAL-CODE.md`](docs/EXTERNAL-CODE.md) | The nine external repos we read or link against, and the licence boundary between them. Read before copying a line out of `reference/`. |
| [`tools/validate_schema.py`](tools/validate_schema.py) | Proves the DDL executes, FKs resolve, and UNIQUE indexes actually enforce uniqueness. |
| [`tools/validate_ops.py`](tools/validate_ops.py) | Checks the 174-op catalogue: unique names, inverses, scope rules, coalescing, and that the prose count matches the tables. |
| [`tools/fetch_external.sh`](tools/fetch_external.sh) | Clones/refreshes `third_party/` and `reference/`. Both gitignored. |
| [`LICENSE`](LICENSE) | GPLv3. |

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
3. **Session View is a peer of the arrangement, not an add-on.** Both are core
   schema. This is the whole premise. (ADR-0006)
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
| **3** | The op vocabulary: every op type, payload, inverse | **done** — 174 ops, `docs/OPS.md` |
| **4** | Reference reader/writer library + round-trip test corpus | **next** |
| **5** | Audio engine skeleton: graph, transport, snapshot handoff | |
| **6** | Plugin hosting: CLAP and VST3 | |
| **7** | Minimal arrangement UI — the first thing you can make a track in | |
| **8** | Session View | |
| **9** | The agent, at Observe tier only | |
| **10** | Propose and Apply tiers | |

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
