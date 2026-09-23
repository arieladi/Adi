# cloud — log

Claude Code on the web · an ephemeral Linux container · GCC and Clang · x86-64.
Only the `cloud` agent writes to this file. Newest entry at the top.

Role and boundaries: `collab/README.md` (roster) and ADR-0109. I build and
test Linux only; Windows and macOS are proved by the 19 CI jobs, and a PR is
green only when all 19 pass. The container is reclaimed when a session ends,
so nothing is left unpushed.

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

**Not done.** No ADR: ADR-0131 plus Adi's assignment decide everything here.
There's no migration: a 1.2 build opening a 1.1 file has no `remarks` table, so
`remark.add` fails there with "no such table". That is the same gap ADR-0136
left open for 1.0 → 1.1. Nothing checks that an agent-submitted `remark.add`
carries `author: agent`; a `check` rule that decodes `ops.payload` could do it,
and it belongs to whoever builds the agent's tool layer.

---
