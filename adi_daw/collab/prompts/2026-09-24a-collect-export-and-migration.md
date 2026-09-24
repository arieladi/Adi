# Next missions — paste each into its own session. They can run at the same time.

Claims and ADR numbers for all three are on main (PR #90). win is working on the VST3 state
round-trip and the chunk-snapshot op, in `src/adi/engine/**`, `src/juce/**` and `tests/fixtures/**`.

---

## Codex (linux)

```text
You are agent linux on arieladi/Adi, project adi_daw. Mission: Collect and Export, and the
media ops (ADR-0127 d3-d4, ADR-0136 d4). Branch: linux/collect-export, from main.

You may use Gemini CLI with full access as a tool. You review everything it writes, and you
commit. Don't stop to ask me questions; decide, and write the decision down.

BEFORE YOU START
git checkout main && git pull. Read collab/README.md (the claims table and ADR reservations
changed in PR #90), the top entries of collab/win.md and collab/cloud.md, ADR-0127, ADR-0136
and SPEC section 10.

YOUR FILES (claimed for you in collab/README.md)
src/adi/ops_catalog.cpp (the media.* ops only), docs/OPS.md, src/adi/media/**, src/adi/check.*,
src/main.cpp, tests/test_media_ops.cpp, tests/test_collect_export.cpp, tools/validate_ops.py.
CMakeLists.txt is shared: add your own targets only.
NOT YOURS: src/adi/store.*, docs/format/**, src/adi/textproj*, src/adi/store_rows.*,
src/adi/digest.cpp (cloud has them), and src/adi/engine/**, src/juce/**, tests/fixtures/** (win).
If SPEC section 10 needs different words, put the wording in your log and cloud or win folds it in.
ADR-0143 is reserved for you, if you decide anything. Mark its row "used" once written.

WORK
1. The ops. Implement media.import (inverse media.unlink), media.unlink and media.relink,
   each with its inverse, OPS.md payload docs, and replay/validate_ops coverage like the other
   ops. import takes a path and hashes it with your BLAKE3. It stores rel_path relative to the
   .adi's FOLDER when it can, abs_path_hint otherwise, and embedded is always 0. relink matches
   by content hash: a moved file with the same hash is the same media. Retire media.embed and
   media.extract in OPS.md as struck rows citing ADR-0127. Never implement them.
2. adi_tool collect-export <in.adi> <out.zip>. Copy every referenced media file into audio/
   beside a COPY of the .adi. Check each against media_files.hash_blake3. A mismatch stops the
   export, names the file, exits non-zero and leaves no partial zip. Rewrite rows to relative
   audio/ paths in the copy, never in the user's original. Fold the copy's WAL before zipping.
   Write the .adi plus audio/ as ZIP64 with your ZipWriter, STORE for audio. Test it by
   extracting the zip and opening the .adi from there: every media path resolves, and
   adi_tool check is clean.
3. Embedded media in 1.0 files (ADR-0136 d4). collect-export handles them, and so does a new
   adi_tool extract-media <file.adi>. Write the media_blobs chunks to audio/<orig_name> with
   collision-safe names, verify the hash, set embedded=0 and rel_path, and delete the blobs,
   all in one transaction. The 1.1 triggers allow embedded=0 and deleting blobs, so this also
   works on an upgraded file. Build the 1.0-shaped fixture in the test by dropping the three
   triggers; ADR-0136 d1 says why a test may do that.
   Cloud is writing the automatic upgrade of older 1.x files, on open for writing, in parallel
   (store.cpp). Don't depend on it and don't duplicate it. Extraction must work whether the
   file is still 1.0 or already upgraded.
4. Plants. Each must make a test fail before you call the work done:
   - the hash check skipped;
   - rows rewritten in the original instead of the copy;
   - blobs left behind after extraction;
   - a relative path computed against the working directory instead of the .adi's folder.

MSVC /WX RULES (no CI leg catches these; win builds MSVC /WX after each of your merges)
- No bare std::getenv. Use the one-site envVar helper, as in tests/test_wav_file.cpp.
- std::setvbuf(stdout, nullptr, _IONBF, 0), never std::setbuf.
- No fopen, strcpy or sprintf family. Watch size_t-to-int narrowing (C4267) and
  signed/unsigned compares.

Skip the param_edits.hpp stats() note: the header already says it, and win owns that file now.

DONE MEANS
tools/test_all.sh green, sanitizers as usual, validators clean. The plants are recorded in
your ADR or your log. Log in collab/linux.md. Open the PR, verify CI green by head SHA, and
merge your own PR. Then report: the PR number, the checks and suites count, the plants table,
and anything you could not do.
```

---

## Claude cloud session

```text
You are agent cloud on arieladi/Adi, project adi_daw. Two missions, in order, one PR each.
You are in a Linux container. You can build and test the Linux legs. You cannot build MSVC or
the JUCE plugin targets, so don't try. Don't stop to ask questions: decide, and write it down.

BEFORE YOU START
Pull main (it has PR #90). Read collab/README.md: your claims, and ADR-0144 reserved for you
beside 0139. Read the top entries of collab/win.md and collab/linux.md, ADR-0127, 0131, 0136,
0140 and SPEC section 11.

YOUR FILES
Mission A: src/adi/store.*, docs/format/**, tools/validate_schema.py, tests/test_store.cpp,
tests/test_migrate.cpp.
Mission B: src/adi/textproj_store.*, src/adi/store_rows.*, tests/test_textproj_store.cpp,
src/adi/digest.cpp (win lends these).
CMakeLists.txt is shared: add your own targets only.
NOT YOURS: src/adi/ops_catalog.cpp, docs/OPS.md, src/adi/media/**, src/adi/check.*,
src/main.cpp (linux is doing Collect and Export in parallel); src/adi/engine/**, src/juce/**,
tests/fixtures/** (win). src/adi/textproj.* is mac's: if a remark node needs a syntax change
there, keep it additive and say so in your log; mac reviews on Saturday.

MISSION A — cloud/migrate. The gap you reported: remark.add fails on 1.1 and 1.2 files with
"no such table".
- Opening an older-minor file of the same major FOR WRITING upgrades it to kUserVersion in one
  transaction: the tables, indexes and triggers each minor added (1.1 triggers, 1.2 remarks,
  1.3 history_snapshots), then user_version, last.
- One step per minor, applied in order. Failure anywhere leaves the file at its old version.
- A read-only open never writes. Decide how readers treat a table an older file lacks: empty,
  or a clear error that says "open for writing to upgrade". Pick one place to do it, and
  record it in ADR-0144.
- A newer minor is never touched (SPEC section 11). A newer major stays read-only, as now.
- Never drop or rewrite existing data. A 1.0 file with embedded media is upgraded too: the
  triggers only fire on new writes. Don't extract media; linux's extract-media does that.
- Test: build 1.0, 1.1 and 1.2 files (from schema.sql history in git, or by dropping from a
  fresh file). Open each for writing. remark.add and a history snapshot then work, check is
  clean, and sqlite_master matches a fresh file, ignoring order. validate_schema.py can make
  that comparison a check.
- Plants, each failing first: a step skipped for one minor; user_version bumped before the
  tables exist; the migration running on a read-only open; the migration touching a newer
  minor; a failing step that leaves the file half-upgraded.
- SPEC section 11 gets the upgrade rule. ADR-0144 if you decide anything.

MISSION B — cloud/remarks-projection, after A merges. Remarks in the text projection:
a rows::Model field, a node syntax, and the round trip in test_textproj_store. The Excluded
row for remarks in textproj_store.cpp becomes covered. Use ADR-0139 for the syntax decision.
Keep the author and anchor rules of ADR-0131.

MSVC /WX RULES (win builds them after each merge; no CI leg does)
- No bare std::getenv: use the envVar helper, as in tests/test_wav_file.cpp.
- std::setvbuf(stdout, nullptr, _IONBF, 0), never std::setbuf.
- No fopen, strcpy or sprintf family. Watch size_t-to-int narrowing and signed/unsigned
  compares.

DONE MEANS, PER MISSION
Tests green (tools/test_all.sh on Linux), validators clean, plants recorded, log in
collab/cloud.md. Open the PR and verify CI green by head SHA. PR CI skips silently when
mergeable is UNKNOWN: use workflow_dispatch then. Merge your own PR, releasing your claim row
first. Report the PR numbers, the checks count, the plants, and anything you could not do.
```
