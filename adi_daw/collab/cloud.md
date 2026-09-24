# cloud — log

Claude Code on the web · an ephemeral Linux container · GCC and Clang · x86-64.
Only the `cloud` agent writes to this file. Newest entry at the top.

Role and boundaries: `collab/README.md` (roster) and ADR-0109. I build and
test Linux only; Windows and macOS are proved by the 19 CI jobs, and a PR is
green only when all 19 pass. The container is reclaimed when a session ends,
so nothing is left unpushed.

---

## 2026-09-24 — older 1.x files upgrade on a write open (ADR-0144)

Branch `cloud/migrate`. This closes the gap I reported: `remark.add` and
snapshots failed on 1.1 and 1.2 files with "no such table".

**What landed.** `Store::open` on an older minor of our major:

- **Opened for writing:** it upgrades the file in one transaction. Each later
  minor's objects are created in order, then `adi_meta.schema_minor`, then
  `user_version` last. A failure rolls back to the old version, and the open
  returns `StoreError::MigrationFailed`.
- **Opened read-only:** it never writes. Each missing table gets an empty
  `TEMP` stand-in on the connection, so every reader sees no rows. That is the
  one place ADR-0144 d4 decides it. I rejected an "open for writing to
  upgrade" error, because read-only is how newer-major and locked files open.

`migrationSteps()` lists object **names** per minor. The DDL comes from the
embedded `schema.sql`, so there is no second copy to drift.

The frozen schemas for 1.0, 1.1 and 1.2 were taken from git
(`docs/format/history/`). `validate_schema.py` check 9 proves every minor was
additive and that each frozen file upgrades to the current schema. SPEC §11.1
states the rule.

**Finding:** 1.1 added a comment inside `media_files`'s `CREATE TABLE`, and
SQLite stores that verbatim. So an upgraded 1.0 file can never be
byte-identical in `sqlite_master` without rewriting a table, which the rules
forbid. The comparison ignores comments and whitespace (ADR-0144 d6).

**Tests.** `adi_migrate_tests` (new, 62 checks). Validator check 9. Tree: 3399
checks across 34 suites. Clang `-Werror` builds clean. I cannot build MSVC here;
I checked the new code against your /WX rules by hand. No `getenv`, `setvbuf`
not `setbuf`, `ifstream` not `fopen`, and every size_t conversion is an explicit
cast.

**Plants, eight, all fired** (each reverted):

| # | Defect planted | Failing check |
|---|---|---|
| M1 | the 1.2 step skipped | `1.0: sqlite_master matches a fresh file: +index:idx_remarks_target +table:remarks`; `1.1: remark.add works` |
| M2 | `user_version` set before the tables | `user_version is set after every CREATE and the adi_meta update (at 1, last CREATE 8)` |
| M3 | the migration runs on a read-only open | `opens read-only` |
| M4 | `minor != kSchemaMinor` (a newer minor migrated) | `opens for writing` (the newer-minor test) |
| M5 | no transaction | `the 1.2 step's table was rolled back, not left half-upgraded` |
| M6 | no TEMP stand-ins on a read-only open | `FAILED -- exception escaped: no such table: remarks` |
| V1 | `history/schema-1.1.sql` missing | `docs/format/history/schema-1.1.sql is missing: freeze each minor's schema.sql before bumping` |
| V2 | a frozen minor's DDL differs from what shipped | `1.3 changes the DDL of ['remarks'] -- a CREATE cannot migrate that` |

M5's first attempt did not compile (`txn.commit()` left behind), and the run
then printed the PREVIOUS plant's stale binary failing. That counts as nothing.
I redid it with the commit removed too, and it compiled and fired. Lesson
applied: every plant's build line is read before its test line.

**Not done.** No 1.x → 2.0 path (not additive by definition). The upgrade does
not extract embedded media: linux's operation does that.

---

## 2026-09-24 — history snapshots: schema 1.3 (ADR-0128, ADR-0140)

Branch `cloud/snapshots`, stacked on `cloud/remarks` and rebased onto main
once it merged.

**What landed.** Schema 1.3 adds a STRICT `history_snapshots` table: `id, name
(non-empty), op_seq (NULL = the root), branch_id -> op_branches, created_utc,
auto`. It is not named `snapshots`, because that table has held mixer
snapshots and track versions since 1.0, and a minor bump cannot change its
shape. That is **ADR-0140**, the one decision ADR-0128 left open that needed a
number. `History` gains `takeSnapshot`, `renameSnapshot`, `listSnapshots`,
`revertToSnapshot` and `defaultSnapshotName`. Every time is an argument: the
snapshot's time, the local offset for the automatic name, and the time on the
preserved branch. The date comes from Hinnant's `civil_from_days`, not from
`localtime`. SPEC §8.6 is new, and §8.3 now says compaction must not drop a
snapshot's point. The replay digest and the text projection exclude the
table, as they exclude `op_branches`. `validate_schema` 5j checks it.

**Revert never discards.** The rewind to the common ancestor and the replay
forward happen in one transaction, so a revert works across branches. That
machinery replaced `switchToBranch`'s refusal to cross diverged branches, and
the tests use `switchToBranch` to actually reach what a revert kept. A branch
`before revert to '<name>'` is created only when the head's line would
otherwise be unreachable. When the snapshot is an ancestor on the same line,
that line is the redo line, and the next edit forks it exactly as after an
undo. That means no duplicate branch rows.

**Tests.** `adi_snapshots_tests` (new, 65 checks); validate_schema 5j (6 rows).
Tree: 3256 checks across 31 suites.

**Plants, eight, all fired** (each reverted after):

| # | Defect planted | Failing check |
|---|---|---|
| S1 | revert never creates the preserved branch | `C's line was kept, by the revert itself`; `and is reachable: A,C again: A,B` |
| S2 | preserve even when redo already reaches the tip | `no branch yet: the line is exactly where redo from here leads`; `exactly one branch holds B's line, saw 2` |
| S3 | the common ancestor ignored (rewind everything, replay nothing) | `A,B: C rewound, B replayed: ` (empty) |
| S4 | the caller's UTC offset ignored | `the caller's offset is applied (+03:00): My Song 2026-09-24 12:34` |
| S5 | minutes truncated rather than floored | `before the epoch, floored not truncated: X 1970-01-01 00:00` |
| S6 | a snapshot records the root, not the head | `the second names the head, on the current branch`; `byte for byte, by the replay digest` |
| S7 | revert's transaction removed | `the project is untouched: A,B` (C's rewind survived the failure) |
| S8 | schema: `branch_id`'s foreign key removed | validator `history_snapshots accepted a branch that does not exist` |

My first draft of the revert test expected the preserved branch immediately
after reverting to a same-line ancestor. The code was right and the test was
wrong: until the next edit, that line is the redo line. The branch-naming
assertion moved to the cross-branch test, where the revert itself has to
create the branch.

**Rebases.** `cloud/remarks` was rebased twice before it merged (win's #85
and #86, linux's #88; #88 also added BLAKE3 to `--build-only`). Each time the
README headline was recomputed with `test_all.sh` rather than merged by hand,
ADR-0140 went after win's ADR-0141 (the log is append-only), and the
reservation rows were split so that 0140 is `used` and 0139 stays `reserved`.
My claims row is removed before merging.

**Outside the listed paths.** `digest.cpp`/`.hpp` (one exclusion and its
comment), `DECISIONS.md` (ADR-0140 only), `CMakeLists.txt` (one target),
README counts.

**Not done.** ADR-0139 stays reserved and unused: remarks needed no decision
beyond ADR-0131 and the assignment. No automatic snapshots (ADR-0128 defers
them), no compaction (it doesn't exist yet), no "open read-only" tab (d3,
UI work), and no History window (d6).

---

## 2026-09-24 — remarks: schema 1.2 (ADR-0131 d2-d5)

Branch `cloud/remarks`, my first. Joined the roster (four agents now) on
Adi's instruction.

**What landed.** A STRICT `remarks` table (SPEC §6.8): `id, target_kind
(track|clip|device), target_id, param_id (device only), author (user|agent),
actor_detail, text (non-empty), created_utc, resolved`. Four ops, OPS.md §9.12
and `ops_catalog.cpp`: `remark.add` (paired with `remark.remove`),
`remark.edit` and `remark.resolve` (symmetric), `remark.remove` (captures
every column, replayed through `remark.add`). 160 → 164 ops. `created` is a
payload key, so no handler reads a clock; `author` is a payload key too,
because a handler cannot see who submitted it (OPS.md §7.2), and so the agent's
tool layer MUST always send `agent`. `check` reports `remark.danglingTarget`
as a **warning**: nothing cascades, undoing the delete re-anchors the remark,
and a kind a newer minor adds is not called missing. The text projection
**excludes** remarks, with a reason: projecting needs a `rows::Model` field and
a node syntax, and the replay digest covers them meanwhile. ADR-0131 d5 is
AI-AGENT §7.3, and "an action a remark asked for" is on §2's always-confirm
list. Schema 1.1 → 1.2: `user_version` 1002, `kSchemaMinor` 2, validator
check 5i, the projection fixtures print `schema 1.2`.

**Tests.** `adi_remarks_tests` (new, 52 checks); `adi_check_tests` +7;
validate_schema 5i (7 rows). Tree: 3188 checks across 30 suites.

**Plants, eight, all fired** (each reverted after):

| # | Defect planted | Failing check |
|---|---|---|
| P1 | `remark.remove`'s capture drops `resolved` | `undo restores it column for column: device\|7\|gain\|agent\|model-x\|cut 3 dB at 250 Hz?\|1790000000000042\|0\|` |
| P2 | `remark.add` stamps `created_utc` from `system_clock` | `every column is the payload's: track\|1\|~\|user\|Adi\|the low end is muddy\|1790206989403548\|0\|` (and redo/undo identity) |
| P3 | `checkRemarks` not called | `the orphaned remark is reported: <none>` |
| P4 | the finding raised as an Error | `as a warning -- undo re-anchors it, so it is not corrupt` |
| P5 | the known-kinds filter removed | `an unknown target kind is not reported as dangling: ... remark.danglingTarget(remarks#2)` |
| P6 | the table-exists guard removed | `a file without the table (1.0, 1.1) checks clean: remark.checkFailed(remarks)` |
| P7 | schema: `author` CHECK removed | validator `remarks accepted an author outside user\|agent`; ops `an author outside user\|agent is refused` |
| P8 | schema: the param-on-device CHECK removed | validator `remarks accepted a parameter on a non-device remark`; ops `a parameter on a track remark is refused` |

P3's first version deleted the call and `-Werror` refused the unused function.
A plant that doesn't compile proves nothing, so it was redone as `(void)&checkRemarks`.

**Outside the listed paths, and why.** `CMakeLists.txt` (shared: one target)
and the README headline and counts (39 tables, 164 ops). `docs/format/**` and
`textproj_store.*` are standing claims of win's; Adi assigned them to me for
this work, so I edited them under that assignment and I'm saying so here.

**Rebase.** win's #85 landed mid-review: it reserved 0139-0140 for me and
released their param-ops claim. Rebased; the claims-table conflict was
resolved keeping both sides' rows. My claims row is removed in the last commit
before merging.

**Not done.** No ADR: ADR-0131 plus Adi's assignment decide everything here.
There's no migration: a 1.2 build opening a 1.1 file has no `remarks` table, so
`remark.add` fails there with "no such table". That is the same gap ADR-0136
left open for 1.0 → 1.1. Nothing checks that an agent-submitted `remark.add`
carries `author: agent`; a `check` rule that decodes `ops.payload` could do it,
and it belongs to whoever builds the agent's tool layer.

---
