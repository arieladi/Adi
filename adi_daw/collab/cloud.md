# cloud — log

Claude Code on the web · an ephemeral Linux container · GCC and Clang · x86-64.
Only the `cloud` agent writes to this file. Newest entry at the top.

Role and boundaries: `collab/README.md` (roster) and ADR-0109. I build and
test Linux only; Windows and macOS are proved by the 19 CI jobs, and a PR is
green only when all 19 pass. The container is reclaimed when a session ends,
so nothing is left unpushed.

---

## 2026-09-24 — the settings store (ADR-0152)

Branch `cloud/settings`. A core library with no UI: `src/adi/settings/`
(`registry`, `store`, `bundle`), its own static library `adi_settings`, and
`docs/SETTINGS.md`.

**What landed.**
- **Registry:** a typed table of 40 settings on 10 pages. Each entry has a key,
  type, scope, page, label, help, default, choices or range, the op for Project
  scope, and the agent flag. The Find box matches every word of the query
  against the label, help, key and page.
- **ADR-0145's settings are in:**
  - Zoom on Selection;
  - silent resampling;
  - buffer sizes 64 to 4096, with 32 refused;
  - ASIO as a single choice, opened through JUCE;
  - the Linux backend and JACK transport sync;
  - custom CLAP and LV2 folders.
- **The per-application `settings.json`:**
  - it names its application, and another application's file is refused and
    never overwritten;
  - unknown keys survive a save;
  - a corrupt file moves aside to `.corrupt-N`;
  - saves are atomic.
- **The change log:** `settings-changes.jsonl`. It always records agent
  changes, and by default user changes too; ADR-0125 left that open and this
  decides it.
- **Presets:** whole or partial. Applying one never touches a page it doesn't
  hold.
- **Bundles:** one ZIP of `manifest.json`, `settings.json` and `presets/`.
  Paths travel as `role:user library/...` or `role:content folder N`, and are
  resolved against the importing machine's folders. A path under no role is
  left out, and export refuses to write any absolute path.
- **The agent's pipeline:** `agentSet` works at Apply tier only and has two
  locks: the per-setting flag, and a structural refusal of any path, the
  Audio, Plug-ins, Privacy and AI pages, and anything not App scope.

**Two bugs found before any plant.**
1. The record folder was a bundle role, so it matched itself instead of the
   user library that holds it. On another machine that role would resolve to
   that machine's own, possibly empty, record folder rather than under its
   user library. Roles are now only library roots (ADR-0152 d5).
2. A segfault: `for (... : bring(x).items())` iterated `items()` of a
   temporary, and range-for extends only the proxy's lifetime. It is now bound
   to a name. After the fix I ran the suite under ASan and UBSan: clean.

**Tests.** `adi_settings_tests`, 80 checks, the same on every platform:
- the registry;
- the store: basics, unknown keys, corrupt files, application isolation;
- the change log;
- the agent whitelist;
- presets;
- bundles.

Tree: 3938 checks across 40 suites. GCC and Clang `-Werror` are clean. MSVC
/WX is not built here; I checked the new code by hand. No `getenv`,
`setvbuf`, streams rather than `fopen`, and every `size_t` conversion explicit.
One of those I found only on re-reading: `sizeof name` passed to miniz as an
`mz_uint`.

**Plants, seven, all fired** (each reverted):

| # | Defect planted | Failing check |
|---|---|---|
| P1 | one application reads another's file (the `app` check removed) | `ADI Live pointed at ADI DAW's file refuses it`; `and never overwrites it` |
| P2 | an unknown key dropped on save | `and the newer build's key is still there, exactly` |
| P3 | a partial preset applies keys from pages it doesn't hold | `a key from a page the preset does not hold is not applied`; `and is reported as skipped` |
| P4 | paths written raw, not as roles | caught by export's own guard: `exported: an absolute path would have left this machine in the bundle` |
| P4b | raw paths AND the guard removed | `no absolute path anywhere in the bundle's settings`; `a path under no role is left out and reported, not shipped` |
| P5 | the agent's whitelist check removed | `refused: audio.bufferSize`, `refused: privacy.crashReports`, `and none of them changed` |
| P6 | a corrupt file not kept aside | `the broken file is kept, byte for byte, beside it` |

P2 and P4 first fired through an escaping JSON exception rather than a named
check. A crash proves less than a check that names the broken promise, so I
made those checks read defensively and planted again: both then failed by
name. P4 alone showed export's guard catching the leak. P4b, with the guard
removed too, showed the test catching it by reading the ZIP.

**Not done.**
- The Settings Reference text is not in this tree, so the registry holds
  what the ADRs record. The Reference's other settings are one row each for
  whoever has it.
- No Device-scope settings yet.
- The change log's retention is still open.
- Safe Mode (R-06) is a startup behaviour, not a store; not built.

---

## 2026-09-24 — the Propose-tier changeset (ADR-0145 d9, ADR-0148)

Branch `cloud/changeset`. Schema 1.4 was already on main (win's #98), so
`agent_requests` was there from the start.

**What landed.** `src/adi/changeset.*`, its own library `adi_changeset`:
- **`Changeset`:** the base head, the queued `OpRequest`s, `actorDetail`,
  `request` and `createdUtc`.
- **`preview`:** a unified diff (Myers, 3 lines of context) of the text
  projection before and after, plus a review list per op: label, target, and
  before and after values. The before value is the op's own inverse, so a
  parameter shows 0.5 → 0.25 even though devices are not in the projection.
- **`apply`:** one transaction, actor `agent`, the `agent_requests` row in that
  same transaction. Stale head → refused.
- **Guardrails, at preview and again at apply:**
  - an allowlist: edit ops only, and of the media ops only `unlink` and
    `relink`;
  - the op cap, 256 by default.
- **`adi_tool propose <file> <changeset.json> [--apply]`:** exit code 3 means
  stale. A preview-only run refuses an older-minor file, because opening it
  would upgrade it (ADR-0144).
- **Docs:** AI-AGENT §6.1.

**Decisions (ADR-0148).**
- **The preview is a transaction that is always rolled back**, running the
  same descriptor steps `commit` does. `commit` itself cannot be used, because
  it opens its own BEGIN and SQLite does not nest.
- **Measured** on 200 tracks and 10,000 clips:

  | Build | Preview | One projection | A backup copy alone |
  |---|---|---|---|
  | Release | 330–390 ms | ~160 ms | 4–5 ms |
  | Debug | ~2.0 s | ~1.1 s | — |

  The mechanism costs about 20 ms; the time is the projection.
- **The request row is written by a TEMP trigger on `ops`**, inside
  `OpJournal`'s own transaction. `ops.*` is win's, and it has no hook for this.
  **win:** a `commit(reqs, inTxn)` callback overload would replace the trigger;
  ADR-0148 d3 describes it.

**A bug the tests caught before any plant:** my first trigger skipped its
insert when a row for that txn id already existed. It committed ops WITHOUT
their request, silently. The fix has no guard: the trigger empties its pending
row after its insert, so it fires once, and a conflicting row aborts the whole
commit. That guard was then planted back as P6.

**Tests.** `adi_changeset_tests`, 60 checks:
- guardrails and the cap;
- a preview leaves the digest, the op count, the head, the parameter, the
  `.adi`'s bytes and a second connection's view unchanged;
- the diff and the before and after values;
- creation and deletion shapes;
- apply is one txn with actor, model, request row, and one undo;
- stale;
- the request row lives or dies with the ops, both ways;
- `media.unlink` leaves the file on disk;
- exact unified-diff output;
- JSON;
- the large-project timing.

The CLI was run end to end: preview, `--apply`, `check` clean. Tree: 3787
checks across 38 suites.

**Headline note for win:** main's README said 3729 across 37, and this Linux
run of main gives 3727. Two checks are platform-dependent. I wrote this run's
3787, as `test_all.sh` requires on Linux.

**Plants, six, all fired** (each reverted):

| # | Defect planted | Failing check |
|---|---|---|
| P1 | apply without the stale check | `refused as stale: `; `nothing was written`; `an explicit older baseHead is honoured -- and is stale` |
| P2 | the preview commits its transaction | `the project is exactly as it was`; `the .adi's bytes are identical after a checkpoint`; `a second connection still reads 'Bass'` |
| P3 | each op committed as its own transaction | `all three share ONE txn_id (AI-AGENT 6.2)`; `undoes all of it` |
| P4 | no trigger; the request row inserted after the commit | `and no op was committed without its request` |
| P5 | the op cap ignored | `three ops over a cap of two: preview refuses: `; `and so does apply, writing nothing` |
| P6 | the trigger skips a txn id that already has a row | `a request row that cannot be written fails the apply`; `and no op was committed without its request` |

P4's first version still created the trigger, so the row was written in the
transaction anyway; it failed an unrelated check. I redid it as the real defect
(no trigger, a late insert), and it fails the check that matters.

**Not done.**
- The wall-clock and token caps of §6.6 belong to the model layer.
- There is no UI for the changeset.
- The stale check and the commit are two steps on one connection. That is safe
  under the single-writer lock (SPEC §3.6), and would need the journal hook to
  be one step.
- MSVC /WX is not built here. I checked the new code by hand: no `getenv`,
  `setvbuf`, `ifstream`, explicit `ptrdiff_t` and `size_t` conversions.

---

## 2026-09-24 — remarks in the text projection (ADR-0139)

Branch `cloud/remarks-projection`, stacked on `cloud/migrate` (#92) until
that merged.

**What landed.**
- **Rows:** `rows::Remark` and `Model::remarks`, read by `readModel`. The table
  always exists now: an upgrading open adds it, and a read-only open gets an
  empty stand-in (ADR-0144).
- **Syntax:** a `remark` child of its track or clip. Lines, in order and only
  when not default: `by agent`, `detail`, `device`, `param`, `created`,
  `resolved`, then `text` last. The text is escaped like a label.
- **Device remarks:** devices are not projected yet, so a device's remark sits
  on the device's track, naming the device and the parameter.
- **Orphans:** a remark whose anchor is gone becomes a top-level node with
  `on -> "!unresolved(<kind>)"`, never dropped.
- **Order:** by `created`, then device, parameter, author and text.
- **Coverage:** `remarks` is Projected now, no longer Excluded. The decision is
  ADR-0139, and TEXT-PROJECTION §9.2 documents it. `textproj.*` (mac's) is
  untouched: the existing node, attribute and ref machinery was enough, so there
  is nothing for mac to review there.

**Tests.** `adi_textproj_store_tests` 158 → 173. Model-level cases cover:
- containment on a track and on a clip;
- the device anchor;
- `by agent`, the detail and `resolved`;
- the orphan;
- escaping of a newline and U+202E;
- storage-order independence.

End to end, a remark goes through the op registry to the byte-exact
projection; undo removes it and redo restores the same bytes. Tree: 3413 checks
across 34 suites. GCC and Clang `-Werror` both build clean.

**Plants, six, all fired** (each reverted):

| # | Defect planted | Failing check |
|---|---|---|
| B1 | the default author flipped (`by` omitted for agents) | `an agent's remark always says so, with who and whether resolved` |
| B2 | an orphaned remark dropped | `a remark whose track is gone is kept, at top level, unresolved` |
| B3 | a device's remark not anchored to its track | `a device's remark sits on the device's track and names the device and parameter` |
| B4 | the `created` sort key removed | `and it is by the time each was written` |
| B5 | the text rendered unescaped | `remark text is escaped: a newline or a bidi override cannot forge the lines around it` |
| B6 | `readModel` not collecting remarks | `the remark is read back into the model`; `the agent's remark projected, byte for byte` |

B3's first version did not compile (an unused lambda under `-Werror`), and the
FAIL it printed came from B2's stale binary. I redid it with `(void)` so that
it compiled and fired on its own. The first draft of the ordering test would
not have caught B4 at all: `first` and `second` sort alphabetically the same
way they sort by time. It now uses `zebra` written before `apple`.

**Not done.** `digest.cpp` was lent but needed nothing: remarks were already in
the digest, and history snapshots were already excluded. There is no remark
anchor for lanes or markers (the schema allows only track, clip and device).

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
