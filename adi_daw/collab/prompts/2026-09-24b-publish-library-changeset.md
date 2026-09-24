# Next missions — paste each into its own session. They can run at the same time.

Claims and ADR numbers are on main once PR #97 merges (win merges it). win is doing schema 1.4 (the
expression route on the device row and an `agent_requests` table), then the plugin capabilities registry.

---

## Codex (linux)

```text
You are agent linux on arieladi/Adi, project adi_daw. Two PRs, in order, on branch names of your
choice under linux/. ADR-0147 is reserved for you. You may use Gemini CLI with full access as a tool;
you review everything it writes, and you commit. Don't stop to ask; decide, and write it down.

BEFORE YOU START
git checkout main && git pull. Read collab/README.md (claims and reservations from PR #97), the top of
collab/win.md, ADR-0143 and ADR-0145 (d11 is the library index).

YOUR FILES
src/adi/media/**, src/adi/library/** (new), tests/test_collect_export.cpp, tests/test_media_ops.cpp,
tests/test_library.cpp (new). If you need utf8proc: one dependency line each in tools/fetch_external.sh
and docs/EXTERNAL-CODE.md (mac's area; allowed on the director's instruction, say so in your log).
CMakeLists.txt is shared: your own targets only.
NOT YOURS: src/adi/store.*, docs/format/**, src/adi/ops_catalog.cpp, docs/OPS.md, src/juce/**,
tools/validate_schema.py (win); src/adi/changeset.*, src/main.cpp, docs/AI-AGENT.md (cloud).

PR 1 -- PUBLISH WITHOUT HARD LINKS (do this first; it is a bug users will hit)
win's review of #95 found that Collect and Export and extract-media publish with
fs::create_hard_link. exFAT and FAT32 have no hard links, and they are what USB sticks and external
sample drives shared between Windows and macOS use. So exporting to a stick, or extracting on such a
drive, fails. Keep the no-clobber guarantee, lose the hard link:
- Publish with an atomic no-clobber rename where the OS has one: renameat2(RENAME_NOREPLACE) on Linux,
  renamex_np(RENAME_EXCL) on macOS, MoveFileExW without MOVEFILE_REPLACE_EXISTING on Windows. Staging
  is already on the target's filesystem, so a rename works there.
- Where the filesystem refuses that call, fall back to creating the target exclusively (O_CREAT|O_EXCL,
  CREATE_NEW on Windows), copying, flushing, closing, and re-hashing the target before it counts.
  On any failure, remove what you created.
- Platform code lives in one small file (ADR-0109). You can't compile the Windows branch locally; the
  CI MSVC leg does, and win builds MSVC /WX after your merge.
- Tests: make the publisher injectable so a unit test can refuse rename and exercise the fallback in
  CI. Also run it by hand on a loop-mounted vfat image and an exfat image on your Ubuntu machine (sudo),
  and record the result in your log.
- Plants, each failing first: the fallback overwrites an existing file; the fallback skips the
  re-hash; a failed fallback leaves a partial file.

PR 2 -- THE LIBRARY INDEX (ADR-0145 d11)
An application-scoped SQLite database of the user's content folders. It is not a .adi: give it its own
application_id and version, and take its path from the caller, because win's ADR-0146 decides where
application data lives. Core only: no UI, no semantic tagging.
- Hash once. Store volume identity, path relative to the volume root, size, modification time and
  BLAKE3. Re-hash only when size or time changed. Times within 2 seconds count as unchanged: exFAT and
  FAT are coarse. The hash decides a match; the metadata only decides when to look again.
- Drives by identity, not by letter or mount point. The identity must be the same on Windows and macOS
  for the same drive. Decide how, and record it in ADR-0147. Options: the filesystem's own serial or
  UUID, a small marker file at the volume root written once, or matching known entries. Define a
  volume-identity interface; implement Linux; leave Windows and macOS as a documented fallback that win
  and mac fill in.
- Names are compared in NFC, with utf8proc (MIT) if you need it. Case is folded only on volumes that
  are case-insensitive: probe, don't assume by OS.
- Background hashing on a low-priority, cancellable worker. New files are browsable at once; their
  hashes follow.
- Export and import tags, BPM and ratings as JSON keyed by BLAKE3 hash; import merges.
- Measure: re-scanning an unchanged 100,000-file tree must hash nothing. Report the time on your i5.
  Moving the tree to another mount point must re-hash nothing.
- BLAKE3 SIMD: your #88 build is portable, SIMD off. Measure hashing throughput portable versus SIMD on
  the i5 and decide in ADR-0147 whether the indexer, or everything, turns SIMD on. Keep MSVC building.
- Plants, each failing first: the size/time shortcut skipped (everything re-hashed); a moved mount
  re-hashes; NFD and NFC names treated as different files; an import keyed by path instead of hash.

MSVC /WX RULES (no CI leg catches these; win builds MSVC /WX after each merge)
No bare std::getenv (use the envVar helper, as in tests/test_wav_file.cpp); std::setvbuf, never setbuf;
no fopen/strcpy/sprintf family; watch size_t-to-int narrowing and signed/unsigned compares.

DONE MEANS, PER PR
tools/test_all.sh green with sanitizers as usual, validators clean, README counts updated, plants
recorded, log in collab/linux.md. Verify CI green by head SHA, then merge your own PR, releasing your
claims row with the last one. Report the PR numbers, the checks count, the plants, the 100,000-file
timing, the SIMD numbers, and anything you couldn't do.
```

---

## Claude cloud session

```text
You are agent cloud on arieladi/Adi, project adi_daw. One mission, one PR: the Propose-tier changeset
(ADR-0145 d9, AI-AGENT §6). Branch cloud/changeset. ADR-0148 is reserved for you. You're in a Linux
container: build and test the Linux legs, not MSVC or JUCE. Don't stop to ask; decide, and write it down.

BEFORE YOU START
Pull main (claims from PR #97). Read collab/README.md, the top of collab/win.md, ADR-0145 (d9),
docs/AI-AGENT.md §2-§6, ADR-0021, and the journal and history code (src/adi/ops.*, src/adi/history.*).

YOUR FILES
src/adi/changeset.* (new), tests/test_changeset.cpp (new), src/main.cpp, docs/AI-AGENT.md.
CMakeLists.txt is shared: your own targets only. The text projection is read-only for you; if you need
a small additive function in src/adi/textproj_store.*, add it and say so in your log.
NOT YOURS: src/adi/store.*, docs/format/**, tools/validate_schema.py, src/adi/ops_catalog.cpp,
docs/OPS.md, src/juce/** (win); src/adi/media/**, src/adi/library/** (linux).

SCHEMA: win lands schema 1.4 early, in a small PR of its own. It adds
  agent_requests(txn_id INTEGER PRIMARY KEY, request TEXT NOT NULL,
                 actor_detail TEXT NOT NULL DEFAULT '', created_utc INTEGER NOT NULL)
for AI-AGENT §6.8, "the request text is stored alongside the txn". Pull it when it's on main. Until
then, build everything else.

THE MISSION
An agent at Propose tier queues ops; nothing reaches the file until the user presses Apply.
- A Changeset holds the head it was built on, the queued OpRequests, the actor (agent), the
  actor_detail (model and version) and the request text.
- Preview without writing the file: run the ops, then produce the diff and throw the writes away.
  Choose the mechanism (a rolled-back savepoint inside one transaction, or a backup copy), check it
  against how OpJournal opens its own transaction, measure it on a large project, and record the
  choice in ADR-0148.
- The diff: a unified diff of the text projection before and after, and a structured list (op label,
  target, and before and after values where the op has them, parameter values especially).
- Apply: refuse with a clear "stale" result if the head moved since the preview; otherwise commit
  everything as ONE transaction (AI-AGENT §6.2), actor agent, with its agent_requests row in the same
  transaction. One undo step undoes all of it.
- Guardrails from §6: a per-request op cap; refuse op types outside what the agent may emit; never
  media deletion. Unlinking a reference is allowed, and the file is never touched.
- adi_tool propose <file.adi> <changeset.json> [--apply] prints the diff and, with --apply, commits.
- Plants, each failing first: apply without the stale check; preview that writes the file; ops
  committed as separate transactions; the request row outside the transaction; the op cap ignored.

MSVC /WX RULES (win builds MSVC /WX after each merge)
No bare std::getenv (use the envVar helper); std::setvbuf, never setbuf; file streams, not fopen;
explicit size conversions.

DONE MEANS
tools/test_all.sh green, validators clean, README counts updated, plants recorded, log in
collab/cloud.md. Verify CI green by head SHA (PR CI skips silently when mergeable is UNKNOWN: use
workflow_dispatch then). Release your claims row, then merge your own PR. Report the PR number, the
checks count, the preview mechanism and its timing, the plants, and anything you couldn't do.
```
