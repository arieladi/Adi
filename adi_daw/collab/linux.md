# linux — log

Ubuntu workstation · GCC/Clang · x86-64 · ChatGPT Codex (terminal).
Only the `linux` agent writes to this file. Newest entry at the top.

Role and boundaries: `collab/README.md` (roster) and ADR-0109. Onboarding and
first tasks: `collab/linux/ONBOARDING.md`.

---

## 2026-09-24 — Collect and Export, media ops and legacy extraction (ADR-0143)

Branch `linux/collect-export`, on Adi's assignment and the pre-existing claim
from #90. ADR-0143 records the decisions; its reservation is marked used.
Only the granted media/check/CLI/catalogue/doc/test files changed, plus this
log, my CMake source/target lines, and README's measured counts (including the
ADR inventory count). No store, schema, projection, engine, JUCE or fixture edits.

### Operations and commands

Path-taking `media::importMedia(store, path, id, importedUtc)` hashes the file
with BLAKE3 and captures a deterministic journal payload. Relative inputs and
stored paths use SQLite's absolute database filename as their base, not cwd.
A same-hash import returns the existing id without logging a duplicate. The
registered `media.import`/`media.unlink` pair captures every media column,
including nullable metadata and peaks; unlink never deletes files and refuses
rows still referenced by a clip/frozen track, or holding legacy chunks.
`media::relinkMedia` accepts a moved file only when its content hash matches;
its symmetric inverse restores paths/missing state. Handlers never re-open
files during undo/redo/replay. Tests replay persisted CBOR with all original
media files deleted and compare digests/inverses.

`adi_tool collect-export <in.adi> <out.zip>` opens the source read-only and uses
SQLite backup to capture committed WAL data. All copying, extraction and row
rewrites happen in a private staging copy. Copied bytes are hashed, all current
pool rows get relative `audio/` paths, and the copy passes the structural and
external-media check before its WAL is folded and its Store closed. ZIP64
contains the `.adi` and STORE audio. Publication refuses an existing output
and leaves no partial ZIP on failure. Historical op payloads are retained;
collection relocates current rows, not the historical record.

`adi_tool extract-media <file.adi>` streams contiguous ordered legacy chunks,
including chunks whose flag was already 0, into collision-safe audio files.
Hashes are verified before publication. One SQL transaction clears flags,
rewrites paths and deletes all chunks. Tests cover both 1.0-shaped files and
upgraded files with the three forbidding triggers restored. The old fixtures
also omit the post-1.1 remarks/snapshot tables so their declared minor is true;
they do not depend on or duplicate cloud's automatic upgrader. A failure on
the second media row rolls back the first row and removes its newly created
file. Existing files are never overwritten.

**Boundaries/limitations decided, not hidden:** publication uses same-filesystem
hard links for no-clobber semantics and fails explicitly if unsupported. Normal
failure cleanup is tested; SQLite and files are not one crash-atomic resource,
so power/process loss can leave unreferenced files. Configured media/search
folders do not yet exist in this headless layer; resolution tries relative,
project audio basename, then absolute hint, and stops at the first hash
mismatch. `adi_tool check` verifies files read-only; the structural checker API
keeps its existing no-filesystem default. Undecoded media metadata stays null.

### Defect plants (all reverted)

| Plant | Guard observed failing |
|---|---|
| Skip content-hash comparisons in resolution and staging | `hash mismatch makes CLI fail`; no-partial-ZIP and extraction rollback guards also fail |
| Open/rewrite the original database instead of the staged copy | `collection never rewrites rows in original database` |
| Omit deleting extracted blob chunks | `all blob chunks deleted after extraction` |
| Compute relative paths against cwd | `relative path is computed against adi folder, not cwd` |
| Reuse the temporary inode after hard-link publication | `collect-export CLI succeeds with live source WAL` |
| Remove media.relink from the registration text | validate_ops: media op missing from catalogue or implementation |
| Unstrike the retired media.embed row | validate_ops: retired embedding op must stay struck and unimplemented |

The staging-inode bug was a real first-draft failure: a second copy truncated
an inode already linked into the staged audio folder. The post-copy hash check
stopped publication. Unlinking the temporary staging name after each publication
fixed it; two different files with the same basename roundtrip independently.
Gemini's read-only audit independently identified this already-tested defect.
Its assertion that the existing test retained only one row was wrong: that very
test failed first, and explicitly checks two rows. No Gemini repository writes,
commits or credentials. Reports and all plant logs are kept under
`/home/adi/Documents/Codex/2026-09-23/i-n/work/collect-export/`.

### Validation and handoff

New suites: 37 media-op checks and 53 collection/extraction checks. Both pass
Clang ASan+UBSan and GCC TSan with `halt_on_error=1`, including sanitized CLI
subprocesses. This is coverage of the new suites, not a claim that every old
suite was rerun under sanitizers. All required plants fired at their named
checks. Full-tree GCC/Clang totals and the final rebase are recorded here before
merge. The first full GCC run caught the stale README ADR count; corrected.
MSVC /WX cannot be run on this Linux host; new code uses setvbuf, no getenv or
unsafe CRT string/file APIs, and explicit narrowing at SQLite blob boundaries.

**Proposed SPEC §10.4 wording for cloud/win (not edited in their files):**
“Collect and Export takes a consistent snapshot of the `.adi`, copies each
referenced media file into `audio/` beside that copy, verifies its BLAKE3,
rewrites only the copy's media paths, folds the copy's WAL, and publishes a
ZIP64 containing the copy and its audio. The user's original database and its
media remain unchanged. A mismatch names the file, fails the command and
leaves no partial archive.”

---

## 2026-09-24 — ADR-0127 building blocks: portable BLAKE3 and streaming ZIP64

Branch `linux/blake3-zip`, from `f74348a`. Adi granted the new media directory,
two tests, dependency-fetch rows, external-code inventory rows, CMake lines and
README headline. No Collect/Export command, ops, schema, JUCE, fixtures, workflow,
engine wiring or benchmark changes. Claims were the first commit.

`media/blake3.hpp`: `blake3Bytes(span<const byte>)` and `blake3File(path)` return
`HashResult` (`hex`, `HashError`, explicit bool); success is a 64-character
lowercase unkeyed BLAKE3 digest. Missing/non-regular files and read/resource
failures return errors, never exceptions. File input uses 64 KiB chunks.
Official C **1.8.7 / f3149ec5bb5449af877ba20377a11008ff499fa2**, portable sources
only; SSE2/SSE4.1/AVX2/AVX512 and NEON disabled, no assembler.

`media/zip_writer.hpp`: `ZipWriter(path)`, `addFile(source, UTF-8 archivePath,
ZipCompression::store|deflate)`, explicit `finish()`, sticky `ZipError`.
STORE is the default. C++ filesystem-aware streams handle Unicode disk paths;
archive names carry the UTF-8 flag. Relative archive paths reject traversal,
absolute names, backslashes and NUL. Destruction cleans up; it does not silently
finalize. A failed export leaves a partial output for the caller to remove.
Sources must remain unchanged during addFile. Payload I/O is bounded to 64 KiB;
miniz keeps central-directory metadata proportional to the number of entries.
No filesystem timestamps are copied. Official miniz **3.1.2 /
77d0dce8627735138c51770d1799a1ef48f2117d**, MIT. Both dependencies are pinned by
tag AND full SHA in `fetch_external.sh --build-only`, successfully fetched locally.

**win — verified dependency findings, resolved in this wrapper's configuration:**

- miniz's automatic ZIP64 promotion misses an archive crossing 4 GiB through
  smaller entries. A real file of 2 GiB + 123 bytes added twice failed
  `ZIP64 aggregate archive finalizes without oversized entry` and
  `aggregate ZIP64 end records` (51 checks, two failures before the fix).
  We deliberately start ZIP64 even for small archives, as ADR-0127 permits.
  The aggregate guard is retained alongside the >4 GiB single-entry guard.
- The concurrent reader roundtrip under GCC TSan reported a race in
  `tzset_internal`, reached through miniz's `mz_zip_dos_to_time_t`/file-stat
  conversion. The original halt-on-error report is preserved. Time conversion
  is unused here; `MINIZ_NO_TIME` disables it in the dependency and all consumers,
  with no sanitizer suppression. Both suites then passed TSan (42 + 39 checks).

**Gemini:** read-only audit through `agy --model gemini-3.1-pro-high`; no repo
writes or git actions. It independently reported the aggregate-promotion defect,
which the test above proves. Its other claim, that a 32-bit build necessarily
has a 32-bit `streamoff` without `_FILE_OFFSET_BITS=64`, was not accepted:
a compiled native i386 probe has sizeof(void*)=4, sizeof(streamoff)=8 and
successfully writes/seeks to 4 GiB + 123 bytes. No speculative macro change.

### Defect plants

Mutants were compiled from scratch copies; none is kept in production. Each
listed plant exited nonzero at the named guard(s). The official JSON's 35 cases
exercise the exposed 32-byte unkeyed API, not unexposed keyed/XOF modes.

| Plant | Failing guard |
|---|---|
| Wrong digest hex nibble | Official unkeyed 32-byte hash vectors |
| Hash only the first read | File hash matches memory across streaming boundaries |
| Missing hash file returns success | Missing file reported without throwing; directory refused |
| Swap STORE and deflate levels | STORE default and deflate optional methods |
| Set miniz's ASCII-name flag | Unicode name preserved with UTF-8 flag |
| ZIP input capped at 64 KiB | Streamed length; miniz roundtrip bytes/CRC |
| Omit archive finalization | Completed central directory; empty archive readback |
| Bypass archive-path validation | Unsafe paths and embedded NUL rejected |
| Missing/non-regular source returns success | Missing source; sticky failure; directory source |
| Permit output as its own input | Output cannot be its own source |
| Second finish returns failure | Finish is idempotent |
| Add after finish reports success | Add after finish rejected |
| Refuse empty-archive finalization | Empty archive finalizes/readback |
| Truncate input size to 32 bits (`ADI_ZIP_BIG=1`) | ZIP64 sizes, entire payload/CRC, later entry's >4 GiB offset |
| Leave miniz's auto-promotion enabled | Aggregate >4 GiB archive finalization and ZIP64 end records (above) |

### Validation

Rebased onto main `5d8e73e` (win's VST3 gesture merge), preserving every other
agent's row. Per win's new log, the ZIP test uses the one-site `getenv`
C4996 helper so MSVC /WX also accepts its optional environment gates. README recomputed from the actual suite output: **3207 checks
across 31 suites**, validators clean with both GCC 15.2.0 and Clang 21.1.8
Release (`test_all.sh`). New suites: **42 hash + 39 ZIP checks**. Both pass
Clang ASan+UBSan and GCC TSan with `halt_on_error=1`; the C dependencies are
instrumented too. Concurrent independent hashes and writers/readers are covered.
Sanitizer coverage here is the two new suites, not a claimed whole-tree rerun.

`ADI_ZIP_BIG=1` ran on real disk-backed files (TMPDIR on the work disk, not the
small /tmp tmpfs): a 4 GiB + 123 byte STORE entry, a following entry above the
4 GiB offset, and two 2 GiB + 123 byte entries crossing the aggregate limit.
Full extraction verifies every byte and CRC with bounded buffers. Native x86-64
passed **53 checks**, 70.53 s wall time, 5744 KiB peak RSS; a separately compiled native i386 executable using the
same sources/dependency definitions passed **53 checks**, 76.04 s wall time,
5416 KiB peak RSS. This also disproves the proposed 32-bit stream-offset defect.
The large tests are opt-in, excluded from the README's default check count.

Python `zipfile` checked all names, payloads, methods and CRCs; `unzip -t` passed
all three sample entries. These are local external checks, not CI dependencies.
PR #88 requires all **19 CI checks green** before self-merge. The claims row is
removed in the closing commit. No tuning or next feature work follows this PR.
Local evidence (audit, before/after findings, plants, external archive, compiler
and test logs): `/home/adi/Documents/Codex/2026-09-23/i-n/work/blake3-zip/`.

---

## 2026-09-24 — ADR-0132 WAV/RF64 and ADR-0133 Play-Q (Adi's two assignments)

The adi-vital rename was completed first in #79, including the GitHub rename
and local fork origin. These two DAW assignments are separate PRs, in order;
this single entry records both. No graph wiring, benchmark reruns or tuning.

### 1. linux/wav-rf64 — streaming WAV writer and WAV/RF64/BW64 reader

Portable, off-callback standard C++ file streams; explicit little-endian sample
and header bytes. Float32, PCM24 and PCM16, 1/2/6/32-channel roundtrips;
EXTENSIBLE above stereo. JUNK reserves 28 payload bytes before fmt, becoming
ds64 in place at the injectable data threshold (default 4 GiB), or earlier
when RIFF's total size would overflow. Odd chunks are padded without including
padding in their data sizes. Float files have fact; optional version-0 bext
contains description, originator and sample time reference, with optional iXML.
The reader bounds-checks chunks, supports ds64 extra chunk-size entries and
refuses truncated or unsupported headers. BW64's reserved ds64 words are ignored.

**Gemini:** `agy --model gemini-3.1-pro-high` was used. I reviewed its draft,
replaced the incomplete implementation (including host-endian I/O and truncated
input acceptance), and kept its subsequent audit read-only. No Gemini commits,
pushes or merges. Local audit artifacts: `work/wav-rf64/gemini-audit.log`.

**Verified audit finding:** a reader rejected non-sentinel 32-bit RIFF sizes in
RF64/BW64 instead of using them. Added two independent fixtures with actual
32-bit sizes and unused zero ds64 lengths: both failed before the fix and pass
after it. The writer continues emitting canonical sentinel sizes on promotion.
Reviewed against [EBU Tech 3306 (2009), §3.4–3.5 and Annex A](https://tech.ebu.ch/files/live/sites/tech/files/shared/tech/tech3306v1_1.pdf)
and [ITU-R BS.2088-2, §2.4 and §4](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2088-2-202511-I!!PDF-E.pdf).
Terminology for win: bext metadata does not itself change the top-level ID from
RF64 to BW64; the reader handles both IDs explicitly, without rewriting an ADR.

**Plants:** each compiled, then returned nonzero from the actual suite. Mutant
sources/binaries and complete outputs are outside the repository in
`work/wav-rf64/`; production source was never left planted.

| Planted defect | Observed failing guard |
|---|---|
| no-promotion | promotion visible before close; RF64 sentinels |
| wrong-ds64-riff | ds64 exact sizes and frames |
| wrong-ds64-data | ds64 exact sizes and frames |
| wrong-format-tag | roundtrip reader refuses the incompatible encoding |
| no-extensible | format tag / EXTENSIBLE above stereo; subformat |
| ignore-ds64 | promoted-file reader refuses invalid sizes |
| pcm-scale | roundtrip identical samples; seek |
| premature-promotion | plain WAV and JUNK; below 1MiB stays WAV |
| wrong-bext-time | bext size and time reference |
| bw64-reserved-as-frames | BW64 reserved-word fixture refused |
| ignore-short-riff-size | non-sentinel 32-bit sizes take precedence over ds64 (both fixtures) |
| accept-bad-riff-length | truncated data refused |
| accept-truncated-as-empty | truncated header refused (six inputs) |
| ignore-ds64-frame-count | wrong RF64 sample count refused |
| Truncate ds64 RIFF size to 32 bits in real >4 GiB run | reader rejects the large file; gated suite exits 1 |

The PCM-scale plant initially survived values limited to ±0.5; expanded the
fixture to ±0.875 and it now fails the sample comparisons. The first Clang
integration run caught the two intentionally failing audit fixtures before the
corrected source was rebuilt; the final runs below supersede it.

**Local validation:** GCC 15.2.0 and Clang 21.1.8 Release `test_all.sh`: **3,086
checks / 28 suites**, validators clean, on the schema-1.1 base from #80.
New suite: **124 checks**, Clang ASan+UBSan and GCC TSan Debug,
`halt_on_error=1` for ASan/UBSan/TSan, UBSan stack traces enabled.
The real big-file run (`ADI_WAV_BIG=1`, disk-backed TMPDIR): **127 checks**,
writing more than 4 GiB of actual float samples and verifying tail seek/read.
Default CI skips these three expensive checks. SoX_ng 14.7.0.9 decoded all
18 WAV/RF64 combinations (three formats × mono/stereo/six-channel) locally;
SoX is not a build or CI dependency. Raw generated files, SoX output, sanitizer
logs, plants and big-file results are retained under task-local `work/wav-rf64/`.

### 2. linux/play-quantize — pure absolute-sample release calculation

Started from main only after WAV/RF64 #82 merged with all 19 CI checks green.
`PlayTempoPoint` is a caller-owned sample-domain map (not snapshot's tick-domain
`TempoEvent`); `playQuantizeRelease` integrates its piecewise-constant tempo from
sample 0 in 960-PPQ ticks. Grid phase is continuous across changes. Fractional
sample positions round up, and the forgiveness edge is inclusive. Invalid inputs
or unrepresentable releases return `nullopt`; no exception, allocation, queue,
graph mutation or block-size argument. Header documents this API contract.
Graph wiring stays with win.

**40 checks:** sixteenth at 120 BPM / 48 kHz = 6,000 samples; sixteenth triplet
= 4,000; tempo changes before the next line (including two changes); exact grid
lines; forgiveness inside/at/past the edge; fractional-sample grids; eight stream
cuts from 32 through 4096 with the same absolute expected results; 20,000 calls
with global `operator new` counting enabled and **zero allocations**; shared
immutable map calls from two threads; invalid/overflow inputs refused.

| Compiled plant | Observed failing guard |
|---|---|
| Halve tempo denominator | sixteenth exact/next line and triplet checks |
| Force gridTicks=240 | triplet exact/next line |
| Use first tempo everywhere | tempo change before next line; two changes |
| Reset accumulated phase at a change | second tempo change; block invariance |
| Remove late forgiveness | forgiveness inside and inclusive edge |
| Exclude the exact forgiveness edge | forgiveness inclusive edge |
| Forgive one extra sample | forgiveness just past edge |
| Treat absolute time modulo 4096 as local | all eight block-size invariant checks |
| Round fractional lines down | fractional line rounds up; rounded line is now |
| Insert operator new/delete into tick conversion | zero allocations guard |
| Return negative input as a valid release | negative event rejected |

Each mutant compiled and its suite exited 1; the unmodified suite then passed.
Full plant sources and outputs: task-local `work/play-quantize/`. All repository
edits, commits, pushes and merges were performed by linux; no Gemini edits in
this second assignment.

**Validation:** GCC 15.2.0 and Clang 21.1.8 Release `test_all.sh` both pass
**3,126 checks across 29 suites**, validators clean. The 40-check Play-Q suite
passes Clang ASan+UBSan and GCC TSan Debug, `halt_on_error=1` throughout and
UBSan stack traces enabled. This is targeted sanitizer coverage of each new
suite, plus whole-tree Release validation on both compilers. README headline
recomputed against current main; claims removed before merge. No benchmark rerun.

---

## 2026-09-23 — Adi-requested #76 validation and #74/#76 matrix reruns

Scheduled regression watch deleted at Adi's instruction. This is one explicit
assignment, on `linux/watch-76`; no recurring watch, feature work or tuning.
Only this log is changed (the branch's first commit claimed it; claim removed
before merge). No engine, CMake or workflow edits.

### 1. Parameter-op sanitizer verification on #76

Checked out **`3eb45cc` (#76)** before building and running
`adi_param_ops_tests`: **75 checks, zero failures under each sanitizer**.
Clang 21.1.8 ASan+UBSan: Debug, `-fsanitize=address,undefined
-fno-omit-frame-pointer`, `ASAN_OPTIONS=halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. GCC 15.2.0 TSan:
Debug, `-fsanitize=thread -fno-omit-frame-pointer`,
`TSAN_OPTIONS=halt_on_error=1`. JUCE off in both builds.
ASan build/run wall time: 10.41/0.18 s; TSan: 8.04/0.23 s.

**win — the #74 stack-use-after-scope does not recur on #76.** Both runs
reach all 75 checks, including the real session/journal/undo loop. This verifies
the fixture lifetime correction; no new guard or plant was added. This request
covers the parameter-op suite under sanitizers, not a fresh whole-tree sanitizer
run. Raw successful output is retained in `work/watch-76/asan-ubsan-param-ops.log`
and `gcc-tsan-param-ops.log` in this task's local work directory.

### 2. Separate benchmark reruns of #74 and #76

Measured **`e1a4a5a` (#74)**, then **`3eb45cc` (#76)**, separately. Both tables
below use the latest #75-published **#74 (`e1a4a5a`) table** as their common
baseline, matching compiler/project/size; they do not chain their deltas.
The earlier #73 table in #75 remains historical context.

Same Intel Core i5-3550S @ 3.00 GHz, Ubuntu 26.04.1, GCC 15.2.0-16ubuntu1
and Clang 21.1.8, Release `-O3 -DNDEBUG`, JUCE off, 48 kHz stereo.
All four projects (active, silence-heavy, active-64 control, mpe-storm), all
eight sizes (32/64/128/256/512/1024/2048/4096), **32 warmups plus 2,000 measured
callbacks per cell**. Both builds finished before sequential GCC then Clang
sampling for each revision. No build/test workload ran alongside sampling;
no affinity, priority or governor changes. **schedutil on all four CPUs was
verified before and after each revision**. No known configuration difference
from #75; the runs are not an isolated or affinity-pinned experiment.
Benchmark self-tests passed for each compiler/revision. CSVs, governor captures,
full SHAs, build logs and timings: task-local `work/watch-76/pr74/` and `pr76/`.

Times below are microseconds. Δ is signed p50 percent versus #75's #74 row.
Misses are callback wall time above the frames/48k deadline, not device xruns.
All cells have zero event drops and rejected events. The scheduler and OS tail
remain part of these wall-time measurements; no device-latency or Ableton claim.

#### #74 rerun — e1a4a5a

| Compiler | Project | Frames | #75 p50 | New p50 | Δ % | p99 | Max | Misses |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | active | 32 | 2.850 | 2.902 | +1.82 | 4.292 | 125.523 | 0 |
| gcc | active | 64 | 3.664 | 3.565 | -2.70 | 10.957 | 2199.083 | 1 |
| gcc | active | 128 | 4.882 | 4.741 | -2.89 | 15.980 | 28.105 | 0 |
| gcc | active | 256 | 8.410 | 8.325 | -1.01 | 19.715 | 86.228 | 0 |
| gcc | active | 512 | 14.748 | 14.036 | -4.83 | 31.764 | 118.970 | 0 |
| gcc | active | 1024 | 28.117 | 27.983 | -0.48 | 54.020 | 72.789 | 0 |
| gcc | active | 2048 | 66.284 | 66.319 | +0.05 | 101.786 | 149.057 | 0 |
| gcc | active | 4096 | 150.329 | 149.063 | -0.84 | 251.122 | 2249.787 | 0 |
| gcc | silence-heavy | 32 | 7.230 | 6.342 | -12.28 | 16.823 | 20.485 | 0 |
| gcc | silence-heavy | 64 | 7.391 | 7.111 | -3.79 | 18.020 | 181.974 | 0 |
| gcc | silence-heavy | 128 | 7.794 | 6.701 | -14.02 | 16.911 | 29.642 | 0 |
| gcc | silence-heavy | 256 | 8.298 | 7.892 | -4.89 | 19.789 | 78.095 | 0 |
| gcc | silence-heavy | 512 | 9.689 | 8.881 | -8.34 | 20.876 | 59.642 | 0 |
| gcc | silence-heavy | 1024 | 11.351 | 11.128 | -1.96 | 24.012 | 45.144 | 0 |
| gcc | silence-heavy | 2048 | 16.453 | 16.536 | +0.50 | 36.266 | 90.593 | 0 |
| gcc | silence-heavy | 4096 | 29.666 | 30.254 | +1.98 | 56.325 | 92.869 | 0 |
| gcc | active-64 | 32 | 24.556 | 23.700 | -3.49 | 43.109 | 62.057 | 0 |
| gcc | active-64 | 64 | 34.209 | 34.884 | +1.97 | 63.071 | 86.568 | 0 |
| gcc | active-64 | 128 | 49.594 | 49.341 | -0.51 | 81.821 | 109.559 | 0 |
| gcc | active-64 | 256 | 81.512 | 82.294 | +0.96 | 125.096 | 141.289 | 0 |
| gcc | active-64 | 512 | 137.438 | 138.321 | +0.64 | 198.992 | 249.808 | 0 |
| gcc | active-64 | 1024 | 398.438 | 345.252 | -13.35 | 1269.518 | 1647.129 | 0 |
| gcc | active-64 | 2048 | 2017.571 | 1964.915 | -2.61 | 3680.145 | 11717.338 | 0 |
| gcc | active-64 | 4096 | 4494.689 | 4517.189 | +0.50 | 6768.520 | 14687.717 | 0 |
| gcc | mpe-storm | 32 | 2.820 | 2.776 | -1.56 | 11.339 | 226.368 | 0 |
| gcc | mpe-storm | 64 | 6.235 | 6.339 | +1.67 | 18.197 | 246.750 | 0 |
| gcc | mpe-storm | 128 | 8.679 | 9.117 | +5.05 | 23.203 | 255.815 | 0 |
| gcc | mpe-storm | 256 | 18.679 | 19.815 | +6.08 | 39.655 | 1692.578 | 0 |
| gcc | mpe-storm | 512 | 40.693 | 41.264 | +1.40 | 76.403 | 357.793 | 0 |
| gcc | mpe-storm | 1024 | 96.253 | 96.216 | -0.04 | 140.009 | 270.582 | 0 |
| gcc | mpe-storm | 2048 | 256.279 | 263.140 | +2.68 | 382.137 | 1197.639 | 0 |
| gcc | mpe-storm | 4096 | 760.300 | 799.219 | +5.12 | 1333.534 | 2125.669 | 0 |
| clang | active | 32 | 2.876 | 2.880 | +0.14 | 3.569 | 1084.999 | 1 |
| clang | active | 64 | 3.536 | 3.608 | +2.04 | 4.235 | 24.671 | 0 |
| clang | active | 128 | 4.879 | 4.860 | -0.39 | 5.994 | 880.560 | 0 |
| clang | active | 256 | 7.736 | 7.512 | -2.90 | 18.381 | 3629.798 | 0 |
| clang | active | 512 | 13.450 | 13.825 | +2.79 | 33.508 | 1029.189 | 0 |
| clang | active | 1024 | 27.768 | 28.164 | +1.43 | 90.169 | 8104.477 | 0 |
| clang | active | 2048 | 66.754 | 66.657 | -0.15 | 99.174 | 148.018 | 0 |
| clang | active | 4096 | 147.266 | 150.528 | +2.22 | 200.160 | 306.815 | 0 |
| clang | silence-heavy | 32 | 6.746 | 7.085 | +5.03 | 18.245 | 39.360 | 0 |
| clang | silence-heavy | 64 | 7.120 | 7.189 | +0.97 | 18.743 | 28.751 | 0 |
| clang | silence-heavy | 128 | 7.132 | 7.040 | -1.29 | 17.083 | 23.518 | 0 |
| clang | silence-heavy | 256 | 7.604 | 7.491 | -1.49 | 18.058 | 29.833 | 0 |
| clang | silence-heavy | 512 | 8.980 | 9.427 | +4.98 | 21.354 | 38.744 | 0 |
| clang | silence-heavy | 1024 | 11.144 | 11.548 | +3.63 | 26.692 | 53.791 | 0 |
| clang | silence-heavy | 2048 | 16.927 | 16.290 | -3.76 | 36.295 | 65.699 | 0 |
| clang | silence-heavy | 4096 | 29.322 | 29.853 | +1.81 | 53.676 | 69.016 | 0 |
| clang | active-64 | 32 | 24.153 | 24.815 | +2.74 | 49.500 | 73.363 | 0 |
| clang | active-64 | 64 | 34.538 | 35.623 | +3.14 | 65.749 | 80.636 | 0 |
| clang | active-64 | 128 | 49.505 | 49.692 | +0.38 | 84.684 | 132.163 | 0 |
| clang | active-64 | 256 | 76.470 | 76.993 | +0.68 | 119.212 | 206.601 | 0 |
| clang | active-64 | 512 | 130.097 | 131.979 | +1.45 | 186.363 | 221.444 | 0 |
| clang | active-64 | 1024 | 302.831 | 351.350 | +16.02 | 1114.810 | 3015.419 | 0 |
| clang | active-64 | 2048 | 1855.141 | 1848.477 | -0.36 | 3354.381 | 3787.830 | 0 |
| clang | active-64 | 4096 | 4482.861 | 4731.923 | +5.56 | 21191.214 | 96156.343 | 1 |
| clang | mpe-storm | 32 | 2.864 | 2.977 | +3.95 | 18.436 | 35.584 | 0 |
| clang | mpe-storm | 64 | 8.309 | 8.561 | +3.03 | 19.575 | 25.652 | 0 |
| clang | mpe-storm | 128 | 11.444 | 11.884 | +3.84 | 27.823 | 5097.352 | 1 |
| clang | mpe-storm | 256 | 25.817 | 27.276 | +5.65 | 50.947 | 3124.332 | 0 |
| clang | mpe-storm | 512 | 56.614 | 58.582 | +3.48 | 139.491 | 2185.784 | 0 |
| clang | mpe-storm | 1024 | 141.244 | 146.363 | +3.62 | 1159.622 | 16505.966 | 0 |
| clang | mpe-storm | 2048 | 411.562 | 434.992 | +5.69 | 3962.434 | 13138.744 | 0 |
| clang | mpe-storm | 4096 | 1411.788 | 1467.078 | +3.92 | 4369.079 | 15189.990 | 0 |

**win — absolute p50 movements greater than 20% against #75:**

None in this rerun.

Deadline misses (all other cells have none):

- gcc active/64: 1 miss(es), p99 10.957 µs, max 2199.083 µs.
- clang active/32: 1 miss(es), p99 3.569 µs, max 1084.999 µs.
- clang active-64/4096: 1 miss(es), p99 21191.214 µs, max 96156.343 µs.
- clang mpe-storm/128: 1 miss(es), p99 27.823 µs, max 5097.352 µs.

#### #76 — 3eb45cc

| Compiler | Project | Frames | #75 p50 | New p50 | Δ % | p99 | Max | Misses |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | active | 32 | 2.850 | 2.837 | -0.46 | 3.739 | 184.931 | 0 |
| gcc | active | 64 | 3.664 | 3.497 | -4.56 | 14.619 | 121.760 | 0 |
| gcc | active | 128 | 4.882 | 4.974 | +1.88 | 18.187 | 3259.518 | 1 |
| gcc | active | 256 | 8.410 | 8.294 | -1.38 | 19.499 | 119.570 | 0 |
| gcc | active | 512 | 14.748 | 14.943 | +1.32 | 55.019 | 4365.427 | 0 |
| gcc | active | 1024 | 28.117 | 29.269 | +4.10 | 91.205 | 3674.618 | 0 |
| gcc | active | 2048 | 66.284 | 68.186 | +2.87 | 103.625 | 148.137 | 0 |
| gcc | active | 4096 | 150.329 | 152.956 | +1.75 | 240.541 | 6691.816 | 0 |
| gcc | silence-heavy | 32 | 7.230 | 6.763 | -6.46 | 17.049 | 21.793 | 0 |
| gcc | silence-heavy | 64 | 7.391 | 6.845 | -7.39 | 17.168 | 31.624 | 0 |
| gcc | silence-heavy | 128 | 7.794 | 7.258 | -6.88 | 18.429 | 32.252 | 0 |
| gcc | silence-heavy | 256 | 8.298 | 7.836 | -5.57 | 19.908 | 43.923 | 0 |
| gcc | silence-heavy | 512 | 9.689 | 8.720 | -10.00 | 21.563 | 38.303 | 0 |
| gcc | silence-heavy | 1024 | 11.351 | 11.539 | +1.66 | 24.809 | 2341.550 | 0 |
| gcc | silence-heavy | 2048 | 16.453 | 17.156 | +4.27 | 34.645 | 56.121 | 0 |
| gcc | silence-heavy | 4096 | 29.666 | 29.178 | -1.64 | 56.647 | 74.403 | 0 |
| gcc | active-64 | 32 | 24.556 | 24.582 | +0.11 | 50.847 | 63.634 | 0 |
| gcc | active-64 | 64 | 34.209 | 35.001 | +2.32 | 61.471 | 80.809 | 0 |
| gcc | active-64 | 128 | 49.594 | 49.319 | -0.55 | 82.523 | 118.393 | 0 |
| gcc | active-64 | 256 | 81.512 | 84.187 | +3.28 | 130.823 | 374.657 | 0 |
| gcc | active-64 | 512 | 137.438 | 140.389 | +2.15 | 214.526 | 1523.546 | 0 |
| gcc | active-64 | 1024 | 398.438 | 431.092 | +8.20 | 1458.960 | 2227.985 | 0 |
| gcc | active-64 | 2048 | 2017.571 | 715.409 | -64.54 | 2982.367 | 7602.882 | 0 |
| gcc | active-64 | 4096 | 4494.689 | 1974.888 | -56.06 | 3790.952 | 5149.892 | 0 |
| gcc | mpe-storm | 32 | 2.820 | 2.532 | -10.21 | 5.502 | 16.842 | 0 |
| gcc | mpe-storm | 64 | 6.235 | 5.918 | -5.08 | 14.740 | 52.225 | 0 |
| gcc | mpe-storm | 128 | 8.679 | 9.419 | +8.53 | 19.734 | 33.636 | 0 |
| gcc | mpe-storm | 256 | 18.679 | 18.701 | +0.12 | 30.091 | 67.218 | 0 |
| gcc | mpe-storm | 512 | 40.693 | 39.268 | -3.50 | 66.518 | 93.970 | 0 |
| gcc | mpe-storm | 1024 | 96.253 | 88.655 | -7.89 | 127.735 | 181.227 | 0 |
| gcc | mpe-storm | 2048 | 256.279 | 231.853 | -9.53 | 296.361 | 392.833 | 0 |
| gcc | mpe-storm | 4096 | 760.300 | 653.667 | -14.03 | 866.196 | 980.120 | 0 |
| clang | active | 32 | 2.876 | 2.877 | +0.03 | 3.260 | 35.875 | 0 |
| clang | active | 64 | 3.536 | 3.646 | +3.11 | 3.937 | 118.942 | 0 |
| clang | active | 128 | 4.879 | 4.690 | -3.87 | 10.410 | 14.029 | 0 |
| clang | active | 256 | 7.736 | 7.179 | -7.20 | 15.804 | 26.416 | 0 |
| clang | active | 512 | 13.450 | 13.090 | -2.68 | 21.893 | 27.857 | 0 |
| clang | active | 1024 | 27.768 | 24.526 | -11.68 | 38.473 | 73.892 | 0 |
| clang | active | 2048 | 66.754 | 66.547 | -0.31 | 83.356 | 151.673 | 0 |
| clang | active | 4096 | 147.266 | 146.606 | -0.45 | 188.024 | 327.950 | 0 |
| clang | silence-heavy | 32 | 6.746 | 6.441 | -4.52 | 14.694 | 20.403 | 0 |
| clang | silence-heavy | 64 | 7.120 | 7.400 | +3.93 | 15.864 | 25.875 | 0 |
| clang | silence-heavy | 128 | 7.132 | 7.067 | -0.91 | 15.803 | 70.891 | 0 |
| clang | silence-heavy | 256 | 7.604 | 7.356 | -3.26 | 10.413 | 26.155 | 0 |
| clang | silence-heavy | 512 | 8.980 | 8.428 | -6.15 | 12.315 | 74.053 | 0 |
| clang | silence-heavy | 1024 | 11.144 | 10.327 | -7.33 | 19.151 | 74.868 | 0 |
| clang | silence-heavy | 2048 | 16.927 | 15.295 | -9.64 | 24.358 | 106.989 | 0 |
| clang | silence-heavy | 4096 | 29.322 | 27.619 | -5.81 | 45.180 | 61.910 | 0 |
| clang | active-64 | 32 | 24.153 | 23.844 | -1.28 | 39.397 | 58.119 | 0 |
| clang | active-64 | 64 | 34.538 | 31.383 | -9.13 | 42.459 | 110.861 | 0 |
| clang | active-64 | 128 | 49.505 | 45.645 | -7.80 | 71.866 | 103.395 | 0 |
| clang | active-64 | 256 | 76.470 | 68.120 | -10.92 | 99.732 | 130.772 | 0 |
| clang | active-64 | 512 | 130.097 | 116.817 | -10.21 | 162.496 | 206.332 | 0 |
| clang | active-64 | 1024 | 302.831 | 222.625 | -26.49 | 490.398 | 611.603 | 0 |
| clang | active-64 | 2048 | 1855.141 | 652.192 | -64.84 | 1590.980 | 1892.682 | 0 |
| clang | active-64 | 4096 | 4482.861 | 1828.406 | -59.21 | 3551.976 | 3925.517 | 0 |
| clang | mpe-storm | 32 | 2.864 | 2.790 | -2.58 | 18.664 | 31.596 | 0 |
| clang | mpe-storm | 64 | 8.309 | 7.867 | -5.32 | 20.021 | 32.084 | 0 |
| clang | mpe-storm | 128 | 11.444 | 10.856 | -5.14 | 26.040 | 45.990 | 0 |
| clang | mpe-storm | 256 | 25.817 | 23.658 | -8.36 | 36.731 | 54.878 | 0 |
| clang | mpe-storm | 512 | 56.614 | 51.503 | -9.03 | 76.029 | 118.113 | 0 |
| clang | mpe-storm | 1024 | 141.244 | 126.901 | -10.15 | 170.440 | 213.332 | 0 |
| clang | mpe-storm | 2048 | 411.562 | 364.679 | -11.39 | 452.943 | 643.845 | 0 |
| clang | mpe-storm | 4096 | 1411.788 | 1176.029 | -16.70 | 1619.509 | 1829.692 | 0 |

**win — absolute p50 movements greater than 20% against #75:**

- gcc active-64/2048: -64.54% (2017.571 → 715.409 µs), p99 2982.367 µs.
- gcc active-64/4096: -56.06% (4494.689 → 1974.888 µs), p99 3790.952 µs.
- clang active-64/1024: -26.49% (302.831 → 222.625 µs), p99 490.398 µs.
- clang active-64/2048: -64.84% (1855.141 → 652.192 µs), p99 1590.980 µs.
- clang active-64/4096: -59.21% (4482.861 → 1828.406 µs), p99 3551.976 µs.

Deadline misses (all other cells have none):

- gcc active/128: 1 miss(es), p99 18.187 µs, max 3259.518 µs.

### 3. GCC active-64/1024 remains an observation

| Measurement | p50 µs | p99 µs | p99/p50 | Δ p50 versus #75 |
|---|---:|---:|---:|---:|
| #75's #74 table | 398.438 | 1747.243 | 4.39 | — |
| This #74 rerun | 345.252 | 1269.518 | 3.68 | -13.35% |
| This #76 run | 431.092 | 1458.960 | 3.38 | +8.20% |

**win — yes, the cell moved again:** #76 is +24.86% versus this fresh #74
rerun, but +8.20% versus the requested #75 baseline. Both new rows have zero
deadline misses. It stays an observation, not an established regression; no
baseline replacement or tuning is inferred. The accepted #66 reference remains.
The large improvements in other active-64 rows listed above are also observations:
#76 changes fixture declaration order and a header's lifetime contract, not
production processing, so these runs do not establish a causal engine speedup.
No unrequested reruns or attribution experiment was started.

After returning to `linux/watch-76`, local `test_all.sh` passed with both GCC
and Clang Release: **2,958 checks across 27 suites, all validators clean**
(wall times 3.30 s and 3.16 s). The benchmark guide and its historical tables
remain unchanged; this prompt's two new tables are recorded together here.
One PR to main; merge only with every CI check green. Stop after merge.

---

## 2026-09-23 — regression watch #74: parameter-op glue; ASan lifetime finding

Measured qualifying merge **`e1a4a5a` (#74)**, parameter-op glue; ASan lifetime finding. Prior table for deltas: **`fb97ea8`**; every compiler/project/size is compared to its matching row. Times are µs, delta is signed p50 percentage.

Intel Core i5-3550S, Ubuntu 26.04.1, GCC 15.2.0 / Clang 21.1.8 Release
(`-O3 -DNDEBUG`, JUCE off), 48 kHz stereo, **32 warmups then 2,000 callbacks**
per project/size. All four projects and all eight standing sizes. **schedutil
on all four CPUs** matched before and after each revision's run. No affinity
or priority changes. Both Release builds finished before sequential benchmark
sampling; no build/test workload ran alongside measurement. Each qualifying
merge was checked separately, in first-parent order, rather than skipped in
favor of the newest tree. Raw CSVs, logs and governor captures are retained in
task-local `work/watch-73-74/` under `pr73` and `pr74`.

**win — requested active/128 check:** Clang recorded **0 misses**, p50 4.879 µs, p99 5.750 µs, max **1287.847 µs** against a 2,666.667 µs budget. The #72 active/128 miss did not recur in this run.

**win — threshold movement:** gcc active-64/1024: +22.64% (324.893 → 398.438 µs), p99 1747.243 µs. This is an unconfirmed observation, not an established regression. The p99/p50 ratio is about 4.39; no cause, fix, bisect or tuning is inferred from a single disturbed-tail measurement. The prior active-64/1024 accepted #66 reference is retained.

Deadline outliers elsewhere (whole-callback wall time, not device xruns):

- gcc active/64: **2 misses**, p50 3.664 µs, p99 5.074 µs, max 3462.363 µs; budget 1333.333 µs.
- clang active/32: **1 misses**, p50 2.876 µs, p99 3.379 µs, max 927.708 µs; budget 666.667 µs.

All other cells have zero misses. All rows have zero event drops and rejected events. No attribution of scheduler outliers to the engine is claimed.

### win — verified ASan stack-use-after-scope in the new test teardown

At `e1a4a5a`, the first `adi_param_ops_tests` case declares `ParamOps ops`
before `Knobs k` (`tests/test_param_ops.cpp:129–130`), attaches k, and leaves
scope without detaching. k dies first. `ParamOps::~ParamOps()` still calls
`a.inst->setParamSink(nullptr,0)` (`src/adi/engine/param_ops.cpp:22`), writing
through that expired stack object in `src/juce/device_model.cpp:205`.

ASan reports **WRITE of size 8, stack-use-after-scope**:

```
DeviceInstance::setParamSink — src/juce/device_model.cpp:205
ParamOps::~ParamOps — src/adi/engine/param_ops.cpp:22
testAGestureBecomesOneOp — tests/test_param_ops.cpp:172
main — tests/test_param_ops.cpp:458
expired variable: k, declared at tests/test_param_ops.cpp:130
```

Reproduced both through CTest and `test_all.sh`. ASan's abort prevents the
rest of the new suite from running; **do not read the release or TSan pass
as full ASan coverage of its 75 checks**. The harness's 2,883-count headline
mismatch is a consequence of the aborted suite, not a stale README.

Minimal standalone reproduction, linked to the unchanged sanitized core:

```cpp
#include "adi/engine/param_ops.hpp"
int main(int argc, char**) {
    adi::engine::ParamOps ops;
    {
        adi::device::MissingDevice device(adi::device::DeviceIdentity{});
        ops.attach(1, device);
        if (argc > 1) ops.detach(1); // control: unregister before destruction
    }
}
```

With no argument: ASan stack-use-after-scope, exit **1**. With an argument:
exit **0**, no report. Both use `ASAN_OPTIONS=halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. For the unmodified suite:

```bash
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1   "$BUILD_ASAN/adi_param_ops_tests"
```

**win:** please correct fixture ownership order or detach before device
lifetime ends, and state the required lifetime relationship in the integration
contract. This proves a fixture/caller lifetime violation; it does not prove
that an active plugin callback in the shipping UI already takes this path.
No source or test fix is made under the measurement-only standing assignment.
Raw trace and the standalone probe/control are under `work/watch-73-74/pr74/`.

| Configuration | CTest `-j 4` | `test_all.sh` |
|---|---|---|
| gcc Release | Not repeated separately | 2,958 checks / 27 suites; PASS, 3.08 s |
| clang Release | Not repeated separately | 2,958 checks / 27 suites; PASS, 3.08 s |
| Clang ASan+UBSan | FAIL 26/27; param-ops suite aborts, 15.53 s | FAIL (ASan abort; headline mismatch is secondary), 24.10 s |
| GCC TSan | PASS 27/27, 8.10 s | PASS 2958 checks, 20.30 s |

All builds and benchmark self-tests pass. Sanitizers use `halt_on_error=1`; UBSan also has `print_stacktrace=1`. Validators pass.

| Compiler | Project | Frames | Prior p50 | New p50 | Δ % | p99 | Max | Misses |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | active | 32 | 2.764 | 2.850 | +3.11 | 3.750 | 107.929 | 0 |
| gcc | active | 64 | 3.501 | 3.664 | +4.66 | 5.074 | 3462.363 | 2 |
| gcc | active | 128 | 4.851 | 4.882 | +0.64 | 6.880 | 851.692 | 0 |
| gcc | active | 256 | 8.075 | 8.410 | +4.15 | 19.457 | 972.850 | 0 |
| gcc | active | 512 | 14.346 | 14.748 | +2.80 | 31.255 | 435.374 | 0 |
| gcc | active | 1024 | 28.081 | 28.117 | +0.13 | 55.749 | 732.776 | 0 |
| gcc | active | 2048 | 66.109 | 66.284 | +0.26 | 104.998 | 462.710 | 0 |
| gcc | active | 4096 | 147.694 | 150.329 | +1.78 | 216.256 | 333.736 | 0 |
| gcc | silence-heavy | 32 | 6.743 | 7.230 | +7.22 | 18.186 | 29.970 | 0 |
| gcc | silence-heavy | 64 | 6.973 | 7.391 | +5.99 | 18.358 | 39.825 | 0 |
| gcc | silence-heavy | 128 | 6.720 | 7.794 | +15.98 | 19.004 | 47.217 | 0 |
| gcc | silence-heavy | 256 | 7.504 | 8.298 | +10.58 | 19.560 | 42.518 | 0 |
| gcc | silence-heavy | 512 | 8.891 | 9.689 | +8.98 | 21.071 | 37.306 | 0 |
| gcc | silence-heavy | 1024 | 10.990 | 11.351 | +3.28 | 23.396 | 51.307 | 0 |
| gcc | silence-heavy | 2048 | 16.493 | 16.453 | -0.24 | 31.407 | 65.111 | 0 |
| gcc | silence-heavy | 4096 | 29.768 | 29.666 | -0.34 | 52.620 | 129.828 | 0 |
| gcc | active-64 | 32 | 24.380 | 24.556 | +0.72 | 49.663 | 391.927 | 0 |
| gcc | active-64 | 64 | 33.980 | 34.209 | +0.67 | 65.526 | 565.935 | 0 |
| gcc | active-64 | 128 | 48.830 | 49.594 | +1.56 | 82.249 | 143.161 | 0 |
| gcc | active-64 | 256 | 80.874 | 81.512 | +0.79 | 125.482 | 766.638 | 0 |
| gcc | active-64 | 512 | 137.708 | 137.438 | -0.20 | 193.853 | 253.649 | 0 |
| gcc | active-64 | 1024 | 324.893 | 398.438 | +22.64 | 1747.243 | 4697.270 | 0 |
| gcc | active-64 | 2048 | 1886.287 | 2017.571 | +6.96 | 4215.627 | 8504.463 | 0 |
| gcc | active-64 | 4096 | 4785.809 | 4494.689 | -6.08 | 6772.884 | 16682.938 | 0 |
| gcc | mpe-storm | 32 | 2.623 | 2.820 | +7.51 | 7.104 | 234.637 | 0 |
| gcc | mpe-storm | 64 | 6.016 | 6.235 | +3.64 | 16.749 | 29.848 | 0 |
| gcc | mpe-storm | 128 | 9.154 | 8.679 | -5.19 | 22.052 | 34.864 | 0 |
| gcc | mpe-storm | 256 | 18.762 | 18.679 | -0.44 | 33.100 | 46.684 | 0 |
| gcc | mpe-storm | 512 | 40.715 | 40.693 | -0.05 | 66.326 | 79.566 | 0 |
| gcc | mpe-storm | 1024 | 95.974 | 96.253 | +0.29 | 130.864 | 146.735 | 0 |
| gcc | mpe-storm | 2048 | 257.837 | 256.279 | -0.60 | 321.500 | 414.580 | 0 |
| gcc | mpe-storm | 4096 | 774.564 | 760.300 | -1.84 | 1108.425 | 3426.430 | 0 |
| clang | active | 32 | 2.876 | 2.876 | +0.00 | 3.379 | 927.708 | 1 |
| clang | active | 64 | 3.632 | 3.536 | -2.64 | 4.609 | 33.723 | 0 |
| clang | active | 128 | 4.909 | 4.879 | -0.61 | 5.750 | 1287.847 | 0 |
| clang | active | 256 | 7.517 | 7.736 | +2.91 | 19.359 | 883.046 | 0 |
| clang | active | 512 | 13.578 | 13.450 | -0.94 | 31.943 | 1296.059 | 0 |
| clang | active | 1024 | 27.683 | 27.768 | +0.31 | 49.498 | 219.522 | 0 |
| clang | active | 2048 | 65.343 | 66.754 | +2.16 | 100.894 | 138.082 | 0 |
| clang | active | 4096 | 147.860 | 147.266 | -0.40 | 201.299 | 270.237 | 0 |
| clang | silence-heavy | 32 | 7.080 | 6.746 | -4.72 | 18.601 | 38.279 | 0 |
| clang | silence-heavy | 64 | 7.001 | 7.120 | +1.70 | 17.559 | 29.269 | 0 |
| clang | silence-heavy | 128 | 7.284 | 7.132 | -2.09 | 18.233 | 75.324 | 0 |
| clang | silence-heavy | 256 | 8.142 | 7.604 | -6.61 | 18.342 | 66.360 | 0 |
| clang | silence-heavy | 512 | 9.138 | 8.980 | -1.73 | 20.066 | 83.450 | 0 |
| clang | silence-heavy | 1024 | 11.197 | 11.144 | -0.47 | 23.815 | 53.593 | 0 |
| clang | silence-heavy | 2048 | 16.169 | 16.927 | +4.69 | 34.361 | 52.162 | 0 |
| clang | silence-heavy | 4096 | 29.281 | 29.322 | +0.14 | 51.920 | 98.293 | 0 |
| clang | active-64 | 32 | 23.642 | 24.153 | +2.16 | 45.000 | 77.591 | 0 |
| clang | active-64 | 64 | 35.986 | 34.538 | -4.02 | 59.500 | 117.678 | 0 |
| clang | active-64 | 128 | 48.289 | 49.505 | +2.52 | 81.392 | 143.501 | 0 |
| clang | active-64 | 256 | 75.388 | 76.470 | +1.44 | 119.888 | 174.744 | 0 |
| clang | active-64 | 512 | 129.197 | 130.097 | +0.70 | 183.260 | 220.565 | 0 |
| clang | active-64 | 1024 | 322.698 | 302.831 | -6.16 | 992.820 | 1636.705 | 0 |
| clang | active-64 | 2048 | 1862.075 | 1855.141 | -0.37 | 3298.455 | 5283.063 | 0 |
| clang | active-64 | 4096 | 4533.511 | 4482.861 | -1.12 | 6599.671 | 8726.432 | 0 |
| clang | mpe-storm | 32 | 2.928 | 2.864 | -2.19 | 13.963 | 35.063 | 0 |
| clang | mpe-storm | 64 | 7.831 | 8.309 | +6.10 | 18.198 | 44.095 | 0 |
| clang | mpe-storm | 128 | 11.080 | 11.444 | +3.29 | 27.934 | 44.888 | 0 |
| clang | mpe-storm | 256 | 24.360 | 25.817 | +5.98 | 44.835 | 109.755 | 0 |
| clang | mpe-storm | 512 | 54.246 | 56.614 | +4.37 | 92.532 | 107.566 | 0 |
| clang | mpe-storm | 1024 | 136.657 | 141.244 | +3.36 | 189.991 | 240.706 | 0 |
| clang | mpe-storm | 2048 | 400.613 | 411.562 | +2.73 | 525.304 | 2509.395 | 0 |
| clang | mpe-storm | 4096 | 1349.275 | 1411.788 | +4.63 | 2236.814 | 3983.118 | 0 |

Documentation only: BENCHMARKS receives the changed numbers; engine, hosts, tests, workflows and other agents’ logs are untouched.

---

## 2026-09-23 — regression watch #73: CLAP contract and cached graph latency

Measured qualifying merge **`fb97ea8` (#73)**, CLAP contract and cached graph latency. Prior table for deltas: **`bad346e`**; every compiler/project/size is compared to its matching row. Times are µs, delta is signed p50 percentage.

Intel Core i5-3550S, Ubuntu 26.04.1, GCC 15.2.0 / Clang 21.1.8 Release
(`-O3 -DNDEBUG`, JUCE off), 48 kHz stereo, **32 warmups then 2,000 callbacks**
per project/size. All four projects and all eight standing sizes. **schedutil
on all four CPUs** matched before and after each revision's run. No affinity
or priority changes. Both Release builds finished before sequential benchmark
sampling; no build/test workload ran alongside measurement. Each qualifying
merge was checked separately, in first-parent order, rather than skipped in
favor of the newest tree. Raw CSVs, logs and governor captures are retained in
task-local `work/watch-73-74/` under `pr73` and `pr74`.

**win — requested active/128 check:** Clang recorded **0 misses**, p50 4.909 µs, p99 6.907 µs, max **154.746 µs** against a 2,666.667 µs budget. The #72 active/128 miss did not recur in this run.

No absolute p50 movement exceeds 20% against the last recorded matrix.

Deadline outliers elsewhere (whole-callback wall time, not device xruns):

- clang active/256: **1 misses**, p50 7.517 µs, p99 12.951 µs, max 12910.365 µs; budget 5333.333 µs.

All other cells have zero misses. All rows have zero event drops and rejected events. No attribution of scheduler outliers to the engine is claimed.

| Configuration | CTest `-j 4` | `test_all.sh` |
|---|---|---|
| gcc Release | Not repeated separately | 2,883 checks / 26 suites; PASS, 3.17 s |
| clang Release | Not repeated separately | 2,883 checks / 26 suites; PASS, 3.07 s |
| Clang ASan+UBSan | PASS 26/26, 6.30 s | PASS 2883 checks, 13.58 s |
| GCC TSan | PASS 26/26, 7.67 s | PASS 2883 checks, 19.99 s |

All builds and benchmark self-tests pass. Sanitizers use `halt_on_error=1`; UBSan also has `print_stacktrace=1`. Validators pass.

**No sanitizer findings** on this revision.

| Compiler | Project | Frames | Prior p50 | New p50 | Δ % | p99 | Max | Misses |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | active | 32 | 2.930 | 2.764 | -5.67 | 13.492 | 41.683 | 0 |
| gcc | active | 64 | 3.609 | 3.501 | -2.99 | 13.658 | 222.222 | 0 |
| gcc | active | 128 | 5.074 | 4.851 | -4.39 | 15.720 | 92.237 | 0 |
| gcc | active | 256 | 8.124 | 8.075 | -0.60 | 19.262 | 32.477 | 0 |
| gcc | active | 512 | 14.621 | 14.346 | -1.88 | 26.676 | 42.161 | 0 |
| gcc | active | 1024 | 27.625 | 28.081 | +1.65 | 50.417 | 74.270 | 0 |
| gcc | active | 2048 | 67.523 | 66.109 | -2.09 | 99.641 | 183.324 | 0 |
| gcc | active | 4096 | 153.008 | 147.694 | -3.47 | 205.348 | 1723.251 | 0 |
| gcc | silence-heavy | 32 | 6.634 | 6.743 | +1.64 | 18.056 | 34.193 | 0 |
| gcc | silence-heavy | 64 | 6.798 | 6.973 | +2.57 | 18.051 | 110.890 | 0 |
| gcc | silence-heavy | 128 | 7.137 | 6.720 | -5.84 | 17.524 | 27.339 | 0 |
| gcc | silence-heavy | 256 | 7.821 | 7.504 | -4.05 | 18.161 | 29.624 | 0 |
| gcc | silence-heavy | 512 | 8.871 | 8.891 | +0.23 | 20.105 | 29.524 | 0 |
| gcc | silence-heavy | 1024 | 10.943 | 10.990 | +0.43 | 22.720 | 47.182 | 0 |
| gcc | silence-heavy | 2048 | 16.207 | 16.493 | +1.76 | 31.651 | 47.500 | 0 |
| gcc | silence-heavy | 4096 | 29.429 | 29.768 | +1.15 | 51.687 | 133.634 | 0 |
| gcc | active-64 | 32 | 24.879 | 24.380 | -2.01 | 46.814 | 68.234 | 0 |
| gcc | active-64 | 64 | 34.451 | 33.980 | -1.37 | 58.110 | 133.452 | 0 |
| gcc | active-64 | 128 | 51.444 | 48.830 | -5.08 | 77.686 | 116.375 | 0 |
| gcc | active-64 | 256 | 82.502 | 80.874 | -1.97 | 121.147 | 159.481 | 0 |
| gcc | active-64 | 512 | 139.155 | 137.708 | -1.04 | 192.483 | 220.752 | 0 |
| gcc | active-64 | 1024 | 340.412 | 324.893 | -4.56 | 1050.901 | 1278.493 | 0 |
| gcc | active-64 | 2048 | 2335.669 | 1886.287 | -19.24 | 3264.622 | 3742.419 | 0 |
| gcc | active-64 | 4096 | 4625.676 | 4785.809 | +3.46 | 7472.574 | 16222.854 | 0 |
| gcc | mpe-storm | 32 | 3.048 | 2.623 | -13.94 | 12.713 | 66.643 | 0 |
| gcc | mpe-storm | 64 | 6.208 | 6.016 | -3.09 | 17.335 | 53.329 | 0 |
| gcc | mpe-storm | 128 | 9.498 | 9.154 | -3.62 | 23.928 | 62.405 | 0 |
| gcc | mpe-storm | 256 | 19.859 | 18.762 | -5.52 | 33.746 | 114.379 | 0 |
| gcc | mpe-storm | 512 | 40.714 | 40.715 | +0.00 | 75.424 | 161.702 | 0 |
| gcc | mpe-storm | 1024 | 96.616 | 95.974 | -0.66 | 152.009 | 205.480 | 0 |
| gcc | mpe-storm | 2048 | 257.031 | 257.837 | +0.31 | 350.114 | 424.551 | 0 |
| gcc | mpe-storm | 4096 | 758.032 | 774.564 | +2.18 | 1206.027 | 1522.190 | 0 |
| clang | active | 32 | 2.845 | 2.876 | +1.09 | 4.195 | 21.937 | 0 |
| clang | active | 64 | 3.515 | 3.632 | +3.33 | 4.534 | 191.980 | 0 |
| clang | active | 128 | 4.766 | 4.909 | +3.00 | 6.907 | 154.746 | 0 |
| clang | active | 256 | 7.312 | 7.517 | +2.80 | 12.951 | 12910.365 | 1 |
| clang | active | 512 | 13.506 | 13.578 | +0.53 | 26.275 | 1140.201 | 0 |
| clang | active | 1024 | 27.475 | 27.683 | +0.76 | 52.813 | 228.785 | 0 |
| clang | active | 2048 | 67.151 | 65.343 | -2.69 | 99.110 | 158.819 | 0 |
| clang | active | 4096 | 147.913 | 147.860 | -0.04 | 199.858 | 240.972 | 0 |
| clang | silence-heavy | 32 | 7.188 | 7.080 | -1.50 | 18.218 | 32.221 | 0 |
| clang | silence-heavy | 64 | 7.191 | 7.001 | -2.64 | 17.768 | 67.191 | 0 |
| clang | silence-heavy | 128 | 7.387 | 7.284 | -1.39 | 18.263 | 57.732 | 0 |
| clang | silence-heavy | 256 | 7.944 | 8.142 | +2.49 | 19.061 | 33.532 | 0 |
| clang | silence-heavy | 512 | 9.591 | 9.138 | -4.72 | 21.508 | 37.476 | 0 |
| clang | silence-heavy | 1024 | 11.479 | 11.197 | -2.46 | 22.991 | 59.000 | 0 |
| clang | silence-heavy | 2048 | 16.397 | 16.169 | -1.39 | 29.430 | 54.377 | 0 |
| clang | silence-heavy | 4096 | 29.893 | 29.281 | -2.05 | 51.156 | 88.902 | 0 |
| clang | active-64 | 32 | 23.563 | 23.642 | +0.34 | 39.443 | 83.352 | 0 |
| clang | active-64 | 64 | 34.931 | 35.986 | +3.02 | 60.880 | 88.798 | 0 |
| clang | active-64 | 128 | 49.520 | 48.289 | -2.49 | 82.248 | 370.931 | 0 |
| clang | active-64 | 256 | 78.243 | 75.388 | -3.65 | 117.746 | 185.837 | 0 |
| clang | active-64 | 512 | 133.088 | 129.197 | -2.92 | 182.949 | 235.884 | 0 |
| clang | active-64 | 1024 | 316.124 | 322.698 | +2.08 | 1043.813 | 1389.389 | 0 |
| clang | active-64 | 2048 | 1828.704 | 1862.075 | +1.82 | 3207.053 | 4463.080 | 0 |
| clang | active-64 | 4096 | 4761.532 | 4533.511 | -4.79 | 6561.494 | 11749.759 | 0 |
| clang | mpe-storm | 32 | 2.887 | 2.928 | +1.42 | 16.757 | 29.107 | 0 |
| clang | mpe-storm | 64 | 7.687 | 7.831 | +1.87 | 14.389 | 49.836 | 0 |
| clang | mpe-storm | 128 | 11.432 | 11.080 | -3.08 | 27.221 | 47.570 | 0 |
| clang | mpe-storm | 256 | 27.014 | 24.360 | -9.82 | 40.194 | 76.364 | 0 |
| clang | mpe-storm | 512 | 56.645 | 54.246 | -4.24 | 88.258 | 115.453 | 0 |
| clang | mpe-storm | 1024 | 141.845 | 136.657 | -3.66 | 189.240 | 227.887 | 0 |
| clang | mpe-storm | 2048 | 412.475 | 400.613 | -2.88 | 501.874 | 595.634 | 0 |
| clang | mpe-storm | 4096 | 1401.860 | 1349.275 | -3.75 | 1783.833 | 1926.369 | 0 |

Documentation only: BENCHMARKS receives the changed numbers; engine, hosts, tests, workflows and other agents’ logs are untouched.

---

## 2026-09-23 — #70 merged: regression watch and read-only audits

Measured main **`bad346e`** after #70 merged with all **19 CI jobs** green.
All four projects, all eight sizes, both Release compilers; same Intel Core
i5-3550S / Ubuntu 26.04.1, GCC 15.2.0 / Clang 21.1.8, 48 kHz stereo,
32 warmups and **2,000** measured callbacks. **schedutil on all four CPUs**
verified before/after; no affinity/priority changes. Builds and audit probes
finished before sequential sampling, with no concurrent build/test workload.

Times are µs; prior is the last recorded `5744ac4` run. Its disturbed
active-64/1024 cell remains visible in that comparison; the accepted reference
for the six reruns is #66's `e4c3b00`, per win. **win:** the Clang
active-64/1024 movement is **−31.97%** versus the disturbed run, an observation
rather than a code-improvement claim. No other p50 moves more than 20% versus
that last run. One measured callback missed its deadline: Clang active/128,
max **3,090.037 µs** against **2,666.667 µs**, with p50 **4.766 µs**. Every
other row has zero misses; all rows have zero event drops and rejected events.

| Compiler | Project | Frames | Prior p50 | New p50 | Delta % | p99 | Max | Misses |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | active | 32 | 2.839 | 2.930 | +3.21 | 14.061 | 118.394 | 0 |
| gcc | active | 64 | 3.613 | 3.609 | -0.11 | 4.616 | 23.938 | 0 |
| gcc | active | 128 | 4.984 | 5.074 | +1.81 | 16.065 | 25.848 | 0 |
| gcc | active | 256 | 8.556 | 8.124 | -5.05 | 19.238 | 50.488 | 0 |
| gcc | active | 512 | 15.133 | 14.621 | -3.38 | 35.426 | 747.471 | 0 |
| gcc | active | 1024 | 28.744 | 27.625 | -3.89 | 55.316 | 94.367 | 0 |
| gcc | active | 2048 | 67.278 | 67.523 | +0.36 | 114.936 | 241.419 | 0 |
| gcc | active | 4096 | 147.774 | 153.008 | +3.54 | 229.177 | 3511.629 | 0 |
| gcc | silence-heavy | 32 | 6.580 | 6.634 | +0.82 | 17.019 | 31.255 | 0 |
| gcc | silence-heavy | 64 | 6.785 | 6.798 | +0.19 | 16.905 | 39.376 | 0 |
| gcc | silence-heavy | 128 | 7.206 | 7.137 | -0.96 | 17.705 | 70.010 | 0 |
| gcc | silence-heavy | 256 | 7.873 | 7.821 | -0.66 | 18.881 | 49.223 | 0 |
| gcc | silence-heavy | 512 | 8.684 | 8.871 | +2.15 | 20.334 | 30.584 | 0 |
| gcc | silence-heavy | 1024 | 11.137 | 10.943 | -1.74 | 23.970 | 37.882 | 0 |
| gcc | silence-heavy | 2048 | 16.399 | 16.207 | -1.17 | 31.179 | 71.780 | 0 |
| gcc | silence-heavy | 4096 | 29.186 | 29.429 | +0.83 | 51.673 | 74.975 | 0 |
| gcc | active-64 | 32 | 24.083 | 24.879 | +3.31 | 44.941 | 65.131 | 0 |
| gcc | active-64 | 64 | 34.407 | 34.451 | +0.13 | 61.940 | 97.446 | 0 |
| gcc | active-64 | 128 | 51.770 | 51.444 | -0.63 | 85.653 | 121.493 | 0 |
| gcc | active-64 | 256 | 82.246 | 82.502 | +0.31 | 124.371 | 148.622 | 0 |
| gcc | active-64 | 512 | 138.798 | 139.155 | +0.26 | 197.781 | 273.633 | 0 |
| gcc | active-64 | 1024 | 369.831 | 340.412 | -7.95 | 775.235 | 1176.906 | 0 |
| gcc | active-64 | 2048 | 2092.997 | 2335.669 | +11.59 | 4684.717 | 10264.848 | 0 |
| gcc | active-64 | 4096 | 4713.983 | 4625.676 | -1.87 | 7130.490 | 11564.062 | 0 |
| gcc | mpe-storm | 32 | 3.033 | 3.048 | +0.49 | 14.362 | 37.364 | 0 |
| gcc | mpe-storm | 64 | 6.218 | 6.208 | -0.16 | 16.839 | 41.342 | 0 |
| gcc | mpe-storm | 128 | 9.320 | 9.498 | +1.91 | 27.002 | 61.181 | 0 |
| gcc | mpe-storm | 256 | 19.036 | 19.859 | +4.32 | 41.961 | 78.005 | 0 |
| gcc | mpe-storm | 512 | 40.229 | 40.714 | +1.21 | 73.369 | 110.636 | 0 |
| gcc | mpe-storm | 1024 | 98.288 | 96.616 | -1.70 | 138.760 | 186.731 | 0 |
| gcc | mpe-storm | 2048 | 260.337 | 257.031 | -1.27 | 334.476 | 706.419 | 0 |
| gcc | mpe-storm | 4096 | 780.128 | 758.032 | -2.83 | 1159.738 | 1437.291 | 0 |
| clang | active | 32 | 2.883 | 2.845 | -1.32 | 3.791 | 25.061 | 0 |
| clang | active | 64 | 3.590 | 3.515 | -2.09 | 13.720 | 32.522 | 0 |
| clang | active | 128 | 4.789 | 4.766 | -0.48 | 14.844 | 3090.037 | 1 |
| clang | active | 256 | 7.690 | 7.312 | -4.92 | 18.679 | 87.666 | 0 |
| clang | active | 512 | 13.895 | 13.506 | -2.80 | 26.692 | 119.182 | 0 |
| clang | active | 1024 | 27.025 | 27.475 | +1.67 | 56.645 | 316.595 | 0 |
| clang | active | 2048 | 66.970 | 67.151 | +0.27 | 110.693 | 142.890 | 0 |
| clang | active | 4096 | 148.687 | 147.913 | -0.52 | 212.296 | 279.830 | 0 |
| clang | silence-heavy | 32 | 7.021 | 7.188 | +2.38 | 18.632 | 47.626 | 0 |
| clang | silence-heavy | 64 | 7.414 | 7.191 | -3.01 | 18.874 | 49.853 | 0 |
| clang | silence-heavy | 128 | 7.619 | 7.387 | -3.05 | 18.723 | 97.787 | 0 |
| clang | silence-heavy | 256 | 8.197 | 7.944 | -3.09 | 19.491 | 103.646 | 0 |
| clang | silence-heavy | 512 | 9.136 | 9.591 | +4.98 | 22.286 | 45.919 | 0 |
| clang | silence-heavy | 1024 | 11.215 | 11.479 | +2.35 | 33.186 | 1103.771 | 0 |
| clang | silence-heavy | 2048 | 16.506 | 16.397 | -0.66 | 33.474 | 91.849 | 0 |
| clang | silence-heavy | 4096 | 30.018 | 29.893 | -0.42 | 54.021 | 113.613 | 0 |
| clang | active-64 | 32 | 24.878 | 23.563 | -5.29 | 46.091 | 137.979 | 0 |
| clang | active-64 | 64 | 35.120 | 34.931 | -0.54 | 67.009 | 125.168 | 0 |
| clang | active-64 | 128 | 51.003 | 49.520 | -2.91 | 84.461 | 150.973 | 0 |
| clang | active-64 | 256 | 78.784 | 78.243 | -0.69 | 122.774 | 185.869 | 0 |
| clang | active-64 | 512 | 139.591 | 133.088 | -4.66 | 194.079 | 270.130 | 0 |
| clang | active-64 | 1024 | 464.662 | 316.124 | -31.97 | 1011.644 | 2091.614 | 0 |
| clang | active-64 | 2048 | 1940.686 | 1828.704 | -5.77 | 3538.979 | 23823.026 | 0 |
| clang | active-64 | 4096 | 4713.592 | 4761.532 | +1.02 | 8188.883 | 17701.421 | 0 |
| clang | mpe-storm | 32 | 2.899 | 2.887 | -0.41 | 18.663 | 37.066 | 0 |
| clang | mpe-storm | 64 | 7.852 | 7.687 | -2.10 | 19.162 | 35.129 | 0 |
| clang | mpe-storm | 128 | 11.030 | 11.432 | +3.64 | 28.553 | 67.901 | 0 |
| clang | mpe-storm | 256 | 24.657 | 27.014 | +9.56 | 48.915 | 69.942 | 0 |
| clang | mpe-storm | 512 | 54.007 | 56.645 | +4.88 | 94.329 | 131.415 | 0 |
| clang | mpe-storm | 1024 | 137.922 | 141.845 | +2.84 | 198.659 | 333.816 | 0 |
| clang | mpe-storm | 2048 | 407.093 | 412.475 | +1.32 | 516.691 | 574.917 | 0 |
| clang | mpe-storm | 4096 | 1338.762 | 1401.860 | +4.71 | 2199.720 | 2585.671 | 0 |

### Active-64 / 1024: three reruns per compiler

| Compiler | Run | p50 µs | p99 µs | p99 / p50 | Δ vs #66 p50 |
|---|---:|---:|---:|---:|---:|
| gcc | 1 | 415.493 | 1394.852 | 3.36 | +38.48% |
| gcc | 2 | 357.506 | 1138.925 | 3.19 | +19.16% |
| gcc | 3 | 352.509 | 1305.059 | 3.70 | +17.49% |
| clang | 1 | 406.762 | 1439.293 | 3.54 | +18.05% |
| clang | 2 | 358.646 | 1071.335 | 2.99 | +4.09% |
| clang | 3 | 406.154 | 1442.914 | 3.55 | +17.87% |

**win:** only one of six rerun p50s is above +20%, and p99/p50 is
2.99–3.70. The required conjunction (all six above threshold with normal
tails) is false. The earlier crossing is a **disturbed run**, #66's baseline
stands, and no bisect or tuning was started.

The benchmark has no project/size selection flag. To run *only* that cell,
a task-local driver compiles the unchanged benchmark declarations and
`measure()` body with Release flags, substituting only main's selection with
`measure(Project{"active-64",64,4,false,false},1024,2000,false)`. Both drivers
link the corresponding main build's unchanged adi_core. No repository
benchmark or CMake edit. The driver is not byte-identical to the full-matrix
executable; selection and raw CSVs are retained in task-local
`work/param-edits-postmerge/`.

### Post-merge validation

GCC and Clang `test_all.sh`: **2,844 checks / 26 suites**, 3.07 / 3.08 s;
all validators pass. Clang ASan+UBSan: CTest `-j 4` 26/26 in 6.73 s,
full validation in 14.43 s. GCC TSan: CTest `-j 4` 26/26 in 8.36 s,
full validation in 20.30 s. `halt_on_error=1` throughout; **no sanitizer
findings**, including the new two-thread capture fixture.

### Assigned audits

**win:** six standalone probe modes fail their contract assertions against
unchanged host code on both GCC and Clang: (C1) advertised audio-port query
callback is null; (C2) process called after start failed; (C3) 128-frame segment
against declared 512–512 bounds; (C4) four audio-port queries while active;
(C5) pre-prepare parameter change rejected with no flush; (C6) event forwarding
calls CLAP's main-thread latency getter on audio. An allocating fake getter
in C6 produces one callback allocation, showing why the thread contract
matters. Severity and fixes belong to win's triage; no host fixes applied.

Reports, exact header/source lines, coverage limitations and complete
standalone reproducers are in [CLAP host contract](linux/audits/clap-host-contract.md)
and [audio-thread paths](linux/audits/audio-thread.md). Publisher review also
has a clean/plant pair: strict `>` reclamation passes; changing it to `>=` in
a scratch header fails the live-reader destructor assertion. No production
header was mutated for the audits. Runtime JUCE/VST3 coverage is explicitly
unavailable; no unverified concern is promoted to a finding.

Gemini was attempted as the granted read-only tool, all tools denied and no
repository credentials supplied. Both requests failed before producing an
audit (`IneligibleTierError`: installed client no longer supported). These
are direct Codex audits, not Gemini output. No new dependency, host patch,
workflow change or Session edit. This documentation follow-up is separate
from #70. Stop after publication; next actions belong to win's triage and
the standing regression watch.

---

## 2026-09-23 — #70 completed after win's README grant

Win's ruling on main `9e8d561` grants the README headline and resolves the
previous entry's scope blocker. The implementation follows ADR-0122 d11:
fixed-capacity SPSC event ring, producer-only atomic counter stores, consumer
per-parameter gesture state, quiet-window coalescing and echo guards. The
snapshot returned by `stats()` is consumer-only; the producer never accesses
the map, edits vector or the snapshot. Clocks are supplied to `drain`; no
clock reads, allocation, locks or retry loops occur in `push`.

The header clarifies consumer-only table growth. More precisely than the
ruling's shorthand “drain is the only place it grows”: `seed` and `expectEcho`
can register a key before its first event, so those consumer calls may also
grow it. The contract explicitly needs both pre-event operations; claiming
that only drain allocates would be false. No semantic contract change.

All required plants (a)–(k), plus the concurrent payload guard (l), compiled
successfully and exited **1 on an assertion**, not a crash or compiler error.
Each mutation was applied alone to the production implementation, the affected
case executed, then the original source restored. Test (l) transfers 10,000
gestures through a 17-slot ring. Plants and failing checks:

| Test | Planted defect | Observed failing assertion |
|---|---|---|
| a | emit at every Value | a: no edits before End |
| b | disable echo swallow | b: armed echo swallowed |
| c | do not extend quiet window | c: extended window not early |
| d | omit stray Begin count | d: stray brackets counted |
| e | omit dropped count | e: accepted and dropped counts |
| f | collapse parameter identity | f: two interleaved params emit twice |
| g | require exact echo equality | g: default tolerance accepts near echo |
| h | never expire guards | h: older guard expires before queued Value |
| i | swallow matching values inside open gesture | i: matching Value inside explicit gesture retained |
| j | invent zero before for unseeded gesture | j: unseeded before is first Value |
| k | allocate on push | k: zero allocations in push |
| l | corrupt copied ring payload | l: concurrent payloads and before/after remain ordered |

The restored suite reports **377 checks, zero failures** on GCC Release,
Clang Release, Clang ASan+UBSan and GCC TSan. Every CTest run uses `-j 4`:
26/26 passed, respectively 1.79 / 1.26 / 6.71 / 7.70 s. Sanitizers use
`halt_on_error=1`; no findings. The granted README line now says
**2,844 checks across 26 suites**. Full `test_all.sh` and validators pass on
both Release compilers and both sanitizer builds (timings recorded in task
artifacts). No other README line changed. Claims are removed before merging;
merge remains conditional on every CI check turning green.

The previous active-64 1024-frame crossing is **a disturbed-run candidate,
not an established regression**, per win's ruling. After #70 merges, the
standing full matrix and sanitizers plus three targeted reruns per compiler
will determine whether it survives; only six >20% p50s with normal tails
justify the requested one-build bisect. Baseline #66 remains the reference.

The assigned read-only audits are separate follow-up work, outside #70.
Gemini CLI was tried with every tool denied and only source text supplied;
both attempts refused service (`IneligibleTierError`: installed client no
longer supported). No repository credential was supplied and no Gemini
finding exists. Direct audits and verified/unverified reports will record
that limitation rather than attributing analysis to Gemini.

---

## 2026-09-23 — ParamEditCapture paused: exact path grant conflicts with validation

**win — ruling needed.** The ADR-0122 d11 capture semantics are implementable,
but the exact path grant omits `adi_daw/README.md`. `tools/test_all.sh` enforces
its exact check/suite headline. Adding the required `adi_param_edits_tests`
necessarily changes the suite count from 25 to 26; the current draft adds
377 checks. Reproduction with the GCC Release tree:

```
README says '**2467 checks across 25 suites**', this run is '**2844 checks across 26 suites**'
FAILED -- see above
```

All binaries and validators passed in that reproduction; the headline mismatch
alone makes the command exit 1. Request the narrow additional grant to update
README's count after final validation (or have win update it). I did not change
README or weaken the validator. Adi explicitly said to log a wrong contract and
stop for win's ruling, so implementation work stops here rather than assuming
permission beyond the exact file list.

Branch `linux/param-edits` contains the prior regression record and a **draft**
implementation of the ring, consumer gesture/echo table and tests (a)–(k), plus
a two-thread 10,000-gesture test. The GCC Release draft suite reports
**377 checks, zero failures**. This is **not completion**: no planted defects
have been run yet, and the new code has not yet had its Clang/sanitizer/CI
validation. Those steps, any necessary corrections, and the post-merge matrix
remain pending. The regression runs in the entry below are on pre-implementation
main, not evidence for the new code. Claims remain active while awaiting the
ruling; the PR stays draft and must not merge in this state.

---

## 2026-09-23 — regression watch before parameter capture: session runtime

Measured main `5744ac4` after #68/#69; #68 (`457f6bd`) is the qualifying
engine merge. #69 changes only JUCE/CMake/docs; the headless engine is identical.
Before starting `ParamEditCapture`, GCC 15.2.0 and Clang 21.1.8 Release ran
all four projects, eight sizes, 2,000 callbacks after 32 warmups, 48 kHz stereo.
Intel Core i5-3550S, Ubuntu 26.04.1, schedutil on all four CPUs before/after;
no affinity/priority changes, both builds finished before sequential sampling.
Baseline is the full CSV captured for #66 at `e4c3b00`; its silence-heavy rows
were published then. All times below are microseconds. Delta is signed p50
percentage change against that matching compiler/project/size baseline.

**win:** active-64 at 1024 frames crosses the 20% reporting threshold:
GCC **+23.27%** (300.029 → 369.831 µs), Clang **+34.85%**
(344.565 → 464.662 µs). These are observations from one matched run, not a
causal attribution to Session. No tuning performed. All other p50 deltas
are within 20%; every row has zero deadline misses, event drops and rejected
events. Silence-heavy at 4096 is 29.186 / 30.018 µs (GCC / Clang).

| Compiler | Project | Frames | Prior p50 | New p50 | Delta % | p99 | Max | Misses |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | active | 32 | 2.773 | 2.839 | +2.38 | 13.024 | 105.713 | 0 |
| gcc | active | 64 | 3.557 | 3.613 | +1.57 | 14.214 | 233.205 | 0 |
| gcc | active | 128 | 4.770 | 4.984 | +4.49 | 14.791 | 75.299 | 0 |
| gcc | active | 256 | 7.852 | 8.556 | +8.97 | 19.974 | 458.775 | 0 |
| gcc | active | 512 | 14.195 | 15.133 | +6.61 | 35.148 | 1425.879 | 0 |
| gcc | active | 1024 | 28.900 | 28.744 | -0.54 | 52.202 | 301.605 | 0 |
| gcc | active | 2048 | 68.723 | 67.278 | -2.10 | 107.874 | 137.936 | 0 |
| gcc | active | 4096 | 151.137 | 147.774 | -2.23 | 199.251 | 248.109 | 0 |
| gcc | silence-heavy | 32 | 6.815 | 6.580 | -3.45 | 17.753 | 48.428 | 0 |
| gcc | silence-heavy | 64 | 6.844 | 6.785 | -0.86 | 18.431 | 36.887 | 0 |
| gcc | silence-heavy | 128 | 6.693 | 7.206 | +7.66 | 17.834 | 38.994 | 0 |
| gcc | silence-heavy | 256 | 7.218 | 7.873 | +9.07 | 18.903 | 29.400 | 0 |
| gcc | silence-heavy | 512 | 8.254 | 8.684 | +5.21 | 19.741 | 41.433 | 0 |
| gcc | silence-heavy | 1024 | 10.891 | 11.137 | +2.26 | 23.026 | 41.472 | 0 |
| gcc | silence-heavy | 2048 | 16.997 | 16.399 | -3.52 | 33.324 | 448.954 | 0 |
| gcc | silence-heavy | 4096 | 29.359 | 29.186 | -0.59 | 50.794 | 89.678 | 0 |
| gcc | active-64 | 32 | 24.156 | 24.083 | -0.30 | 45.952 | 81.148 | 0 |
| gcc | active-64 | 64 | 35.342 | 34.407 | -2.65 | 64.489 | 94.404 | 0 |
| gcc | active-64 | 128 | 50.642 | 51.770 | +2.23 | 84.767 | 119.583 | 0 |
| gcc | active-64 | 256 | 81.519 | 82.246 | +0.89 | 125.866 | 154.797 | 0 |
| gcc | active-64 | 512 | 136.756 | 138.798 | +1.49 | 197.808 | 881.049 | 0 |
| gcc | active-64 | 1024 | 300.029 | 369.831 | +23.27 | 1540.195 | 1721.819 | 0 |
| gcc | active-64 | 2048 | 1861.155 | 2092.997 | +12.46 | 3670.987 | 7530.732 | 0 |
| gcc | active-64 | 4096 | 4414.370 | 4713.983 | +6.79 | 6709.851 | 9631.511 | 0 |
| gcc | mpe-storm | 32 | 2.897 | 3.033 | +4.69 | 16.405 | 43.480 | 0 |
| gcc | mpe-storm | 64 | 5.807 | 6.218 | +7.08 | 13.802 | 53.955 | 0 |
| gcc | mpe-storm | 128 | 9.285 | 9.320 | +0.38 | 24.159 | 68.688 | 0 |
| gcc | mpe-storm | 256 | 18.730 | 19.036 | +1.63 | 39.960 | 380.770 | 0 |
| gcc | mpe-storm | 512 | 40.741 | 40.229 | -1.26 | 73.707 | 133.913 | 0 |
| gcc | mpe-storm | 1024 | 97.539 | 98.288 | +0.77 | 142.582 | 239.663 | 0 |
| gcc | mpe-storm | 2048 | 257.330 | 260.337 | +1.17 | 350.103 | 530.011 | 0 |
| gcc | mpe-storm | 4096 | 749.657 | 780.128 | +4.06 | 1180.548 | 2140.896 | 0 |
| clang | active | 32 | 2.924 | 2.883 | -1.40 | 3.773 | 549.912 | 0 |
| clang | active | 64 | 3.657 | 3.590 | -1.83 | 4.353 | 57.295 | 0 |
| clang | active | 128 | 4.848 | 4.789 | -1.22 | 15.464 | 68.805 | 0 |
| clang | active | 256 | 7.527 | 7.690 | +2.17 | 18.217 | 118.376 | 0 |
| clang | active | 512 | 13.816 | 13.895 | +0.57 | 31.031 | 133.193 | 0 |
| clang | active | 1024 | 28.229 | 27.025 | -4.27 | 53.964 | 111.320 | 0 |
| clang | active | 2048 | 68.204 | 66.970 | -1.81 | 108.251 | 180.907 | 0 |
| clang | active | 4096 | 156.136 | 148.687 | -4.77 | 214.146 | 481.599 | 0 |
| clang | silence-heavy | 32 | 7.294 | 7.021 | -3.74 | 18.064 | 32.247 | 0 |
| clang | silence-heavy | 64 | 7.416 | 7.414 | -0.03 | 19.933 | 277.628 | 0 |
| clang | silence-heavy | 128 | 7.619 | 7.619 | +0.00 | 19.653 | 149.325 | 0 |
| clang | silence-heavy | 256 | 8.295 | 8.197 | -1.18 | 19.736 | 39.556 | 0 |
| clang | silence-heavy | 512 | 9.303 | 9.136 | -1.80 | 20.982 | 162.480 | 0 |
| clang | silence-heavy | 1024 | 11.556 | 11.215 | -2.95 | 23.769 | 51.987 | 0 |
| clang | silence-heavy | 2048 | 16.416 | 16.506 | +0.55 | 36.221 | 119.807 | 0 |
| clang | silence-heavy | 4096 | 29.486 | 30.018 | +1.80 | 51.987 | 94.721 | 0 |
| clang | active-64 | 32 | 23.719 | 24.878 | +4.89 | 47.974 | 155.180 | 0 |
| clang | active-64 | 64 | 35.597 | 35.120 | -1.34 | 66.385 | 119.974 | 0 |
| clang | active-64 | 128 | 49.582 | 51.003 | +2.87 | 85.480 | 130.087 | 0 |
| clang | active-64 | 256 | 76.447 | 78.784 | +3.06 | 128.604 | 696.366 | 0 |
| clang | active-64 | 512 | 138.382 | 139.591 | +0.87 | 326.969 | 5719.331 | 0 |
| clang | active-64 | 1024 | 344.565 | 464.662 | +34.85 | 1737.305 | 4892.843 | 0 |
| clang | active-64 | 2048 | 1932.462 | 1940.686 | +0.43 | 3488.579 | 3695.650 | 0 |
| clang | active-64 | 4096 | 4840.828 | 4713.592 | -2.63 | 6806.684 | 9345.092 | 0 |
| clang | mpe-storm | 32 | 2.854 | 2.899 | +1.58 | 13.669 | 45.364 | 0 |
| clang | mpe-storm | 64 | 7.932 | 7.852 | -1.01 | 19.152 | 59.396 | 0 |
| clang | mpe-storm | 128 | 11.359 | 11.030 | -2.90 | 25.758 | 53.024 | 0 |
| clang | mpe-storm | 256 | 25.645 | 24.657 | -3.85 | 50.167 | 126.489 | 0 |
| clang | mpe-storm | 512 | 54.245 | 54.007 | -0.44 | 88.241 | 186.755 | 0 |
| clang | mpe-storm | 1024 | 136.032 | 137.922 | +1.39 | 192.168 | 259.897 | 0 |
| clang | mpe-storm | 2048 | 397.148 | 407.093 | +2.50 | 534.346 | 787.529 | 0 |
| clang | mpe-storm | 4096 | 1358.913 | 1338.762 | -1.48 | 1895.828 | 2379.674 | 0 |

Full validation before implementation: GCC Release **2,467 checks / 25 suites**
in 3.13 s; Clang Release the same in 3.05 s. Clang ASan+UBSan:
CTest `-j 4` 25/25 in 7.09 s, `test_all.sh` 2,467 checks in 13.97 s.
GCC TSan: CTest `-j 4` 25/25 in 8.09 s, full validation 2,467 checks in
19.92 s. `halt_on_error=1` for each sanitizer; **no sanitizer findings**.
Both benchmark self-tests and all validators pass. The changed measurements
are recorded in BENCHMARKS without replacing its three historical tables.
Raw CSVs and logs: task-local `work/param-edits-regression/`.

---

## 2026-09-23 — after #65: accumulate skips sleeping sources; benchmark guide

Branch `linux/benchmark-guide`, measured **main `e4c3b00`**. Claimed exactly
`docs/BENCHMARKS.md`, the one new docs file granted in #64. No benchmark,
engine, test, CMake, workflow or other documentation edits. Links from FEATURES
and the master reference remain win's assignment.

Same Intel Core i5-3550S, Ubuntu 26.04.1, GCC 15.2.0 / Clang 21.1.8 Release
(`-O3 -DNDEBUG`), JUCE off; **schedutil on all four CPUs**, checked before and
after. **2,000 measured callbacks after 32 warmups per project/size**, 48 kHz
stereo. Both builds completed first, then GCC control/breakdown followed by
Clang control/breakdown, with no concurrent build/test workload and no affinity
or priority changes. Benchmark source and commands are unchanged.

New ordinary silence-heavy rows, beside the before-#60 (#59) and after-#60
(#63) measurements:

| Compiler | Frames | Before #60 p50 | After #60 p50 | After #65 p50 | New p99 | New max | Deadline misses |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | 32 | 13.296 | 8.138 | 6.815 | 17.158 | 29.172 | 0 |
| gcc | 64 | 18.599 | 9.204 | 6.844 | 18.026 | 33.218 | 0 |
| gcc | 128 | 30.451 | 11.426 | 6.693 | 17.060 | 32.916 | 0 |
| gcc | 256 | — | 15.767 | 7.218 | 18.138 | 40.911 | 0 |
| gcc | 512 | — | 23.168 | 8.254 | 19.400 | 44.719 | 0 |
| gcc | 1024 | — | 36.761 | 10.891 | 23.182 | 42.985 | 0 |
| gcc | 2048 | 604.085 | 72.922 | 16.997 | 36.594 | 79.059 | 0 |
| gcc | 4096 | 1729.522 | 168.191 | 29.359 | 53.357 | 67.332 | 0 |
| clang | 32 | 14.211 | 8.065 | 7.294 | 17.258 | 101.436 | 0 |
| clang | 64 | 18.983 | 9.184 | 7.416 | 17.901 | 25.367 | 0 |
| clang | 128 | 30.680 | 10.571 | 7.619 | 19.010 | 41.191 | 0 |
| clang | 256 | — | 15.562 | 8.295 | 19.410 | 99.596 | 0 |
| clang | 512 | — | 22.311 | 9.303 | 20.535 | 88.450 | 0 |
| clang | 1024 | — | 35.633 | 11.556 | 24.766 | 65.859 | 0 |
| clang | 2048 | 660.180 | 68.857 | 16.416 | 31.104 | 69.892 | 0 |
| clang | 4096 | 1792.442 | 167.182 | 29.486 | 51.570 | 113.213 | 0 |

All times are µs; dashes mean unmeasured. At 4096, #65 reduces p50 by
**138.832 µs (GCC)** and **137.696 µs (Clang)** versus the previous table,
about **82%**. Everyday 256–2048 now measures **7.218–16.997 µs (GCC)** /
**8.295–16.416 µs (Clang)**. These are whole-callback measurements: no isolated
per-sleeping-node cost is inferred by dividing by the 315 sleeping nodes.
No misses, event drops or rejected events in the sixteen new ordinary rows.
All measured breakdown callbacks still report **6 processed / 315 skipped**;
those are node counts, not `inputsSkipped` edge counts, which this unchanged
CSV does not export. No hook added.

`docs/BENCHMARKS.md` now documents reproducible GCC/Clang builds and runs,
the eight-size matrix, the three primary projects plus active-64 control,
quantile/deadline definitions, measurement boundaries, full CSV field meanings,
`--breakdown`, and all three historical tables with machine/governor metadata.
It explicitly distinguishes synthetic deadline misses from device xruns and
makes no Ableton comparison. Missing historical sizes remain missing.

Validation: both benchmark self-tests pass; each compiler emits 32 unique
ordinary project/size rows and 32,000 breakdown rows. After sampling,
`test_all.sh` passes **2,289 checks / 24 suites**, validators clean, in
**2.94 s for each compiler**. Guide tables are copied from the recorded CSVs;
relative links and command options are checked against the repository/CLI.
The docs claim is removed in the final pre-merge commit. After green CI and
merge, stop here awaiting win/Adi; no additional engine work started.

---

## 2026-09-23 — everyday buffer sizes become standing benchmark rows

Branch `linux/everyday-blocks`, based on **main `54947a0`** (Adi's note in
PR #62, including win's #60 clear-once fix). The only code edit adds **256,
512 and 1024** to the shared project loop and lists the full matrix in help:
**32/64/128/256/512/1024/2048/4096**. It applies to active, silence-heavy,
active-64 and MPE-storm, and the two projects selected by breakdown mode.
No engine, test, hook, CMake or workflow change.

### Matched setup and silence-heavy results

Same Intel Core i5-3550S (4 cores / 4 threads), GCC 15.2.0 / Clang 21.1.8,
Release `-O3 -DNDEBUG`, JUCE off, **schedutil on all four CPUs** checked before
and after. **2,000 measured callbacks after 32 warmups per combination**,
48 kHz stereo. Completed both builds before running GCC then Clang, control
then breakdown, without concurrent builds/tests or affinity/priority changes.
Commands: `adi_block_benchmark --self-test`, `--iterations 2000`, and
`--breakdown --iterations 2000`.

Times below are microseconds, nearest-rank p50/p99. Pre-fix values are the
round-three table published in #59; previous post-fix values are the two rows
published in #61. A dash means no earlier published measurement at that size,
not zero. **256/512/1024 are new baselines**, so no before/after drop is claimed
for them. Historical tables below remain unchanged.

| Compiler | Frames | Pre-fix p50 (#59) | Prior post-fix p50 (#61) | New p50 | New p99 | New max | Deadline misses |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | 32 | 13.296 | 7.636 | 8.138 | 19.458 | 168.255 | 0 |
| gcc | 64 | 18.599 | — | 9.204 | 20.830 | 69.184 | 0 |
| gcc | 128 | 30.451 | — | 11.426 | 26.645 | 65.004 | 0 |
| gcc | 256 | — | — | 15.767 | 32.378 | 74.570 | 0 |
| gcc | 512 | — | — | 23.168 | 44.143 | 105.593 | 0 |
| gcc | 1024 | — | — | 36.761 | 69.675 | 160.215 | 0 |
| gcc | 2048 | 604.085 | — | 72.922 | 121.518 | 202.778 | 0 |
| gcc | 4096 | 1729.522 | 149.462 | 168.191 | 496.971 | 597.161 | 0 |
| clang | 32 | 14.211 | 7.742 | 8.065 | 19.627 | 85.171 | 0 |
| clang | 64 | 18.983 | — | 9.184 | 20.137 | 48.198 | 0 |
| clang | 128 | 30.680 | — | 10.571 | 22.018 | 83.792 | 0 |
| clang | 256 | — | — | 15.562 | 35.734 | 310.633 | 0 |
| clang | 512 | — | — | 22.311 | 42.853 | 119.180 | 0 |
| clang | 1024 | — | — | 35.633 | 70.740 | 667.865 | 0 |
| clang | 2048 | 660.180 | — | 68.857 | 117.662 | 925.122 | 0 |
| clang | 4096 | 1792.442 | 144.934 | 167.182 | 472.023 | 648.571 | 0 |

**Everyday range, 256–2048:** median cost is **15.767–72.922 µs (GCC)** and
**15.562–68.857 µs (Clang)**. The 48 kHz callback budgets are 5.333–42.667 ms;
p99 is below 0.7% of the corresponding budget at each of these four sizes.
At 2048 the median drops from 604.085 to 72.922 µs (GCC) and 660.180 to
68.857 µs (Clang). These are total synthetic callback costs, not isolated
per-sleeping-node timings or an audio-device xrun test.

**4096 expectation:** 168.191 / 167.182 µs versus the pre-fix
1729.522 / 1792.442 µs, roughly **90% lower**. The remaining excess over the
approximate 150 µs reference is **18.191 / 17.182 µs**. The same-run eight-track
active project measures 149.055 / 150.026 µs, so silence-heavy is
**19.136 / 17.156 µs above that control**. This is close to the expected active
cost plus mixing, with no millisecond-scale excess left.

Compared with #61's post-fix run, the 4096 median is **18.729 / 22.248 µs
higher**; that difference is retained, not hidden. This is one run per
compiler/mode under a desktop governor; the expanded matrix also changes the
preceding workload. It does not isolate a code-regression effect (the engine
has not changed since #61).

### Existing breakdown, no new hook

Every silence-heavy measured callback still has **6 processed / 315 skipped**,
at all eight sizes. Selected breakdown p50s for the everyday range and cap:

| Compiler | Frames | Instrumented callback | Node bodies | Residual |
|---|---:|---:|---:|---:|
| gcc | 256 | 17.576 | 0.656 | 16.902 |
| gcc | 512 | 24.674 | 1.076 | 23.561 |
| gcc | 1024 | 38.659 | 1.908 | 36.704 |
| gcc | 2048 | 72.830 | 3.916 | 68.832 |
| gcc | 4096 | 175.062 | 9.596 | 164.984 |
| clang | 256 | 16.383 | 0.592 | 15.778 |
| clang | 512 | 23.049 | 0.931 | 22.095 |
| clang | 1024 | 36.588 | 1.695 | 34.850 |
| clang | 2048 | 68.752 | 3.706 | 64.957 |
| clang | 4096 | 161.139 | 9.300 | 151.364 |

The residual still grows with frames and includes the unchanged accumulate
reads of sleeping sources, mixing and other scheduler work. Its exact split
is not measured. Any further accumulate optimization remains win's; no hook
is added or needed for this assignment. Separate medians need not add, and
timer overhead means these rows are not interchangeable with ordinary mode.

### Validation and stop

Both self-tests pass. CSV verification finds exactly **32 distinct project/size
rows per compiler** (four projects × eight sizes), and **32,000 breakdown rows
per compiler** (two projects × eight sizes × 2,000 callbacks). Fixture output
and event guards pass; all sixteen requested ordinary silence-heavy rows have
zero deadline misses, event drops and rejected events.

After measurements, `test_all.sh` passes **2,280 checks / 24 suites**, all
validators clean, under GCC Release (**2.99 s**) and Clang Release (**2.98 s**).
`git diff --check` is clean. The benchmark claim is removed in the final
pre-merge commit; net diff is the two-line matrix/help edit and this log.
After green CI and merge, stop here awaiting win/Adi. No further tuning started.

---

## 2026-09-23 — PR #60 remeasurement: the 4096-frame cost falls to ~150 µs

Branch `linux/suspend-remeasure`, measured **main `5c9e62e`** (win's
clear-once fix). Log only; no engine, benchmark, test or instrumentation hook
change. The hook proposal from round three is not needed for this rerun.

Same Intel Core i5-3550S, GCC 15.2.0 / Clang 21.1.8, Release `-O3 -DNDEBUG`,
JUCE off. Governor **schedutil on all four CPUs**, checked before and after.
As in the immediately preceding round-three table: **2,000 measured callbacks
per combination, 32 warmups**, 48 kHz stereo. Both full builds finished before
sampling; GCC then Clang, ordinary then breakdown mode, with no concurrent
build/test workload, affinity or priority changes. Benchmark source and command
lines are unchanged:

```
adi_block_benchmark --self-test
adi_block_benchmark --iterations 2000
adi_block_benchmark --breakdown --iterations 2000
```

The existing CLI runs the full matrix; the requested silence-heavy 32/4096
rows are extracted below. Old values are from the previous round-three run
(engine base `f9d5c6a`, benchmark published in #59 / `d49bfd1`), not the earlier
10,000-iteration run. All times are microseconds;
p50/p99 use nearest rank. Historical entries below remain unchanged.

| Compiler | Frames | Old p50 | New p50 | Reduction µs | Reduction % | Old p99 | New p99 | Old max | New max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc | 32 | 13.296 | 7.636 | 5.660 | 42.57 | 25.548 | 18.472 | 98.206 | 51.895 |
| gcc | 4096 | 1729.522 | 149.462 | 1580.060 | 91.36 | 3011.969 | 258.565 | 4892.603 | 304.768 |
| clang | 32 | 14.211 | 7.742 | 6.469 | 45.52 | 27.631 | 19.153 | 46.230 | 33.250 |
| clang | 4096 | 1792.442 | 144.934 | 1647.508 | 91.91 | 4004.392 | 244.062 | 15080.987 | 287.398 |

**Expectation met.** At 4096 frames the silence-heavy median is down **91.36%
(GCC), 91.91% (Clang)**, to 149.462 / 144.934 µs. The eight-track active project
in these same new runs measured 143.960 / 142.516 µs: silence-heavy is only
**5.502 / 2.418 µs above it**. There is no remaining ~1.8 ms excess to report.
The 32-frame row also improves, by 42.57% / 45.52%.

For direct comparison with the previous instrumented table:

| Compiler | Frames | New instrumented p50 | New body p50 | Old residual p50 | New residual p50 |
|---|---:|---:|---:|---:|---:|
| gcc | 32 | 9.420 | 0.249 | 15.167 | 9.170 |
| gcc | 4096 | 147.243 | 8.733 | 1734.288 | 138.206 |
| clang | 32 | 9.230 | 0.256 | 14.494 | 8.970 |
| clang | 4096 | 147.709 | 9.377 | 1702.603 | 138.074 |

Every measured silence-heavy breakdown callback still reports **6 processed /
315 skipped**. The output/event guards pass, with zero deadline misses, event
drops or rejected events in all four requested ordinary rows. Residual remains
all work outside Node::process bodies, not an isolated accumulate/skipped-path
time. Separate medians need not add, and instrumented vs ordinary runs have
timer overhead and desktop scheduling noise. This is one matched run per
compiler/mode, not a statistical claim about the small GCC/Clang difference.

Validation after measurement: both benchmark self-tests pass;
`test_all.sh` passes **2,280 checks across 24 suites**, validators clean, in
**2.74 s (GCC Release), 2.73 s (Clang Release)**. No hook was added. The only
net change is this log entry, with its claim removed before merge. After
green CI and merge, stop here and await win/Adi.

---

## 2026-09-23 — round three: silent-node measurement and isolated test files

Branch `linux/measurement-isolation`, based on main `f9d5c6a` (win's PR #58).
The round-three assignment authorizes test scratch changes including win's
`test_engine.cpp` and `test_textproj_store.cpp`; the claim lists only the
benchmark, the test helper and ten affected test sources. No production code.

**Measurement limit / hook proposal:** GraphStats exposes node calls and
suspended node-block counts. A benchmark-local Node decorator can time actual
Node::process bodies, but a skipped node never enters that decorator. The
remaining callback duration includes skipped nodes, processed-node scheduling,
mixing, silence detection, event forwarding and output copies; it must NOT be
labelled skipped-node time. I propose win add opt-in, preallocated per-callback
instrumentation around Graph::runNode, recording elapsed time into processed
and suspended buckets (classification already exists there), with the outer
Graph::process time recorded separately. Disabled instrumentation must have
no clock reads. That hook needs win's engine ownership; it is only a proposal
here. The benchmark will expose body time, counts and clearly named residual,
with skipped time explicitly unavailable rather than inferred by subtraction.

**Test plan:** a test-only RAII directory helper uses suite + process id +
per-test token + atomic counter and exclusive create_directory, retrying a
collision without deleting the existing directory. Each file-writing test
owns its scope; SQLite stores close before cleanup. Audit found ten file-writing
suites; their prefixes differ, so the old claim that different suites all
share one path was too broad. Concurrent copies of the SAME suite/build do
share paths. Pure-memory suites need no scratch directory. Use the existing
CTest registration for `ctest -j 4`; no test_all.sh or CMake change is needed.

### Measurement results (before test edits)

Intel Core i5-3550S, four cores, schedutil on all cores; GCC 15.2.0 and
Clang 21.1.8 Release (-O3 -DNDEBUG), JUCE off. Compilers ran sequentially,
control then instrumented; 48 kHz stereo, 32 warmups and 2,000 measured
callbacks per combination. `--self-test` passed. Ordinary mode now includes
`active-64`; `--breakdown --iterations 2000` emits one row per measured callback
for silence-heavy and active-64, with printing after measurement. No engine
hook was added. All fixture output/event checks passed.

Every silence-heavy callback: **6 processed / 315 skipped**. Every active-64
callback: **321 processed / 0 skipped**. Thus suspension is engaging. Body time
covers Node::process only, not mixing or scheduler work. Timer overhead is
included in callback/residual; do not subtract these from uninstrumented runs
as if instrumentation were free. Table times are microseconds, nearest-rank
p50; separate medians need not add exactly. The last column is uninstrumented
callback p50 / processed count, an amortized cost including the whole graph.

| Compiler | Project | Frames | Control callback p50 | Instrumented callback p50 | Body p50 | Residual p50 | Body / processed p50 | Control / processed |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| gcc | silence-heavy | 32 | 13.296 | 15.438 | 0.258 | 15.167 | 0.043 | 2.216 |
| gcc | silence-heavy | 64 | 18.599 | 21.325 | 0.326 | 20.989 | 0.054 | 3.100 |
| gcc | silence-heavy | 128 | 30.451 | 31.076 | 0.428 | 30.631 | 0.071 | 5.075 |
| gcc | silence-heavy | 2048 | 604.085 | 659.031 | 12.350 | 646.554 | 2.058 | 100.681 |
| gcc | silence-heavy | 4096 | 1729.522 | 1777.594 | 39.983 | 1734.288 | 6.664 | 288.254 |
| gcc | active-64 | 32 | 24.501 | 39.534 | 12.634 | 26.831 | 0.039 | 0.076 |
| gcc | active-64 | 64 | 34.517 | 48.231 | 15.634 | 32.433 | 0.049 | 0.108 |
| gcc | active-64 | 128 | 49.562 | 64.105 | 21.206 | 42.598 | 0.066 | 0.154 |
| gcc | active-64 | 2048 | 2096.371 | 2031.531 | 1029.046 | 1024.683 | 3.206 | 6.531 |
| gcc | active-64 | 4096 | 4745.682 | 4829.890 | 2397.882 | 2427.628 | 7.470 | 14.784 |
| clang | silence-heavy | 32 | 14.211 | 14.784 | 0.281 | 14.494 | 0.047 | 2.369 |
| clang | silence-heavy | 64 | 18.983 | 19.991 | 0.312 | 19.668 | 0.052 | 3.164 |
| clang | silence-heavy | 128 | 30.680 | 31.115 | 0.415 | 30.681 | 0.069 | 5.113 |
| clang | silence-heavy | 2048 | 660.180 | 657.451 | 11.424 | 642.725 | 1.904 | 110.030 |
| clang | silence-heavy | 4096 | 1792.442 | 1745.820 | 40.715 | 1702.603 | 6.786 | 298.740 |
| clang | active-64 | 32 | 24.646 | 39.626 | 12.953 | 26.554 | 0.040 | 0.077 |
| clang | active-64 | 64 | 34.112 | 47.744 | 15.851 | 31.724 | 0.049 | 0.106 |
| clang | active-64 | 128 | 48.951 | 62.970 | 21.201 | 41.550 | 0.066 | 0.152 |
| clang | active-64 | 2048 | 1930.943 | 2042.314 | 1106.801 | 944.904 | 3.448 | 6.015 |
| clang | active-64 | 4096 | 4797.749 | 4573.593 | 2283.536 | 2279.074 | 7.114 | 14.946 |

**Does skipped work scale with frames?** The suspended branch in Graph::runNode
clears every output channel with `memset(... frames * sizeof(float))` before
returning. For these 315 skipped stereo nodes that is **80,640 bytes at 32
frames, 10,321,920 bytes at 4096** (128×), so the skipped path explicitly has
frame-proportional memory work. The measured silent residual rises strongly
with frames, consistent with that work, but it is not an isolated skipped-node
time or proof of a linear timing law. Cache and scheduler overhead remain
mixed into it. `skipped_scheduler_us` is explicitly **NA** in each breakdown
row until win supplies the proposed hook. No attribution of the entire
residual to skipping, no engine fix, no Ableton comparison.

### Test isolation and validation

The ten audited file-writing suites now use `tests/temp_directory.hpp`:
catalog, check, CLAP bundle search, device ops, engine, history, ops, replay,
store and textproj/store. Each Scratch/Fixture instance owns its own directory;
the four textproj/store file-writing tests no longer share the main function's
directory. Paths include suite, process id, per-test token and atomic counter.
Creation is exclusive, with bounded retries on collision (including PID reuse),
never remove-and-recreate. RAII removes only the claimed directory after all
SQLite/file members close. The OS PID adapter is confined to tests, supporting
Windows `_getpid` and POSIX `getpid`; no production platform path or library.

The existing create/open guard now also checks two overlapping scratch scopes
with the same test token, marker survival and cleanup of only the peer. This
extends its existing assertion, keeping the tree at **2,272 / 24**.

| Configuration | CTest -j 4 (24/24) | test_all.sh (2,272 / 24 + validators) |
|---|---:|---:|
| GCC 15.2.0 Debug | 2.02 s | 3.94 s |
| Clang 21.1.8 Release | 1.69 s | 2.73 s |
| GCC 15.2.0 TSan | 5.41 s | 16.76 s |

TSan used `TSAN_OPTIONS=halt_on_error=1`, with no report. Two independent CTest
controllers then overlapped the SAME TSan binaries, each at `-j 4`: **48/48
suite executions passed**, both controllers exit 0, 11.11 s combined wall
clock. They used separate copies of the generated CTest registration so the
runners' own LastTest.log did not collide; the test data shared the same system
temporary root. The normal `ctest --test-dir <tsan-build> --output-on-failure
-j 4` run above used the original build registration.

All eleven changed C++ units (ten tests plus benchmark) are warning-clean with
GCC and Clang under `-Wall -Wextra -Wconversion -Wshadow -pedantic`.
`git diff --check` passes. No CMake, `test_all.sh`, `.github/**`,
`test_device.cpp`, production engine, schema or ADR changes.

**Plants observed to fail, outside the tracked tree:**

- Replace the temporary-directory helper with the old fixed-token,
  remove-and-recreate behavior. The new overlap-isolation assertion fails
  under GCC Debug AND GCC TSan (exit 1; the ensuing failed store creation
  also triggers its error assertion: 37 checks, 2 failures). This demonstrates
  data destruction rather than relying on a probabilistic parallel collision.
- Double the timed-node call counter in a scratch benchmark source.
  `--breakdown --iterations 1` exits 1 with
  `invalid processed/skipped timing partition`. Clean GCC/Clang self-tests
  and all measured callback partitions pass.

The claim is removed in the final pre-merge commit. The skipped-scheduler
elapsed-time hook remains proposed for win, not implemented or claimed.
After all CI checks pass and merge, round three stops here awaiting win/Adi.


---

## 2026-09-23 — round two: Release callback measurements and routing guards

Branch `linux/routing-determinism`, main `cb686ff` (win's PR #56).
The first commit claims only `tests/test_graph.cpp` for the granted fixtures.

### 1. Benchmark matrix, before changing tests

Intel Core i5-3550S @ 3.00 GHz, 4 cores / 4 threads, governor **schedutil**
on all four CPUs. GCC 15.2.0 and Clang 21.1.8, CMake Release (`-O3 -DNDEBUG`),
JUCE off, `ADI_BUILD_BENCHMARKS=ON`. Rebuilt from this main. Each executable's
`--self-test` passed, followed by `adi_block_benchmark --iterations 10000`.
GCC then Clang, sequentially; no concurrent builds, no affinity, governor or
real-time priority changes. 48 kHz stereo, 32 warmup callbacks then 10,000
measured callbacks **per row**. Times are microseconds; nearest-rank percentiles.
These are desktop synthetic callback measurements, not device xruns or an
Ableton comparison. Scheduling noise is included in max and deadline misses.

| Compiler | Project | Frames | p50 µs | p99 µs | Max µs | Deadline misses |
|---|---|---:|---:|---:|---:|---:|
| GCC | active | 32 | 2.679 | 3.675 | 1653.823 | 1 |
| GCC | active | 64 | 3.526 | 11.858 | 60.224 | 0 |
| GCC | active | 128 | 4.938 | 15.631 | 87.642 | 0 |
| GCC | active | 2048 | 68.256 | 105.461 | 866.570 | 0 |
| GCC | active | 4096 | 148.549 | 209.553 | 2834.428 | 0 |
| GCC | silence-heavy | 32 | 13.608 | 28.885 | 132.283 | 0 |
| GCC | silence-heavy | 64 | 18.315 | 36.006 | 144.188 | 0 |
| GCC | silence-heavy | 128 | 29.911 | 59.231 | 176.032 | 0 |
| GCC | silence-heavy | 2048 | 598.044 | 1416.755 | 12670.563 | 0 |
| GCC | silence-heavy | 4096 | 1829.263 | 3075.095 | 11624.108 | 0 |
| GCC | mpe-storm | 32 | 2.848 | 13.787 | 566.789 | 0 |
| GCC | mpe-storm | 64 | 5.939 | 16.881 | 101.649 | 0 |
| GCC | mpe-storm | 128 | 9.050 | 22.681 | 83.610 | 0 |
| GCC | mpe-storm | 2048 | 256.581 | 342.481 | 1344.379 | 0 |
| GCC | mpe-storm | 4096 | 764.955 | 1173.893 | 5129.600 | 0 |
| Clang | active | 32 | 2.847 | 4.030 | 1190.550 | 2 |
| Clang | active | 64 | 3.584 | 4.596 | 1293.505 | 0 |
| Clang | active | 128 | 4.783 | 15.590 | 3064.446 | 1 |
| Clang | active | 2048 | 66.393 | 104.751 | 465.184 | 0 |
| Clang | active | 4096 | 150.086 | 207.958 | 352.499 | 0 |
| Clang | silence-heavy | 32 | 13.852 | 30.201 | 289.770 | 0 |
| Clang | silence-heavy | 64 | 19.007 | 40.218 | 4306.622 | 4 |
| Clang | silence-heavy | 128 | 30.287 | 54.533 | 86.413 | 0 |
| Clang | silence-heavy | 2048 | 625.201 | 1356.289 | 5470.874 | 0 |
| Clang | silence-heavy | 4096 | 1745.826 | 3433.002 | 8837.661 | 0 |
| Clang | mpe-storm | 32 | 2.878 | 13.546 | 50.048 | 0 |
| Clang | mpe-storm | 64 | 7.870 | 19.367 | 77.163 | 0 |
| Clang | mpe-storm | 128 | 11.493 | 28.126 | 115.142 | 0 |
| Clang | mpe-storm | 2048 | 412.200 | 524.126 | 663.415 | 0 |
| Clang | mpe-storm | 4096 | 1372.418 | 1977.345 | 5058.860 | 0 |

All 30 rows had zero event drops and zero rejected events; fixture output
validation passed. Active and MPE-storm each have 8 tracks × 4 effects (41 nodes);
silence-heavy has 64 × 4 (321 nodes), 63 silent tracks. MPE is 16 notes × 3
expression dimensions at 500 Hz into track one. The table reports one run per
compiler, not an isolated estimate of compiler speed or a performance gate.

### 2. Fixture proposal and scope

The tests have `RampNode`, `LatentNode`, and a sidechain `KeyThrough`, but no
compressor-shaped node. Proposed test support: a **test-local** ducking probe
with `main / (1 + abs(key))`, capturing both inputs in preallocated buffers.
It models dependency/alignment, not a production compressor's DSP. An event
probe will capture explicit fields and render note/expression changes into
samples at their block-relative frame. Neither belongs in `src/`; no production
node type, engine change, workflow or `test_device.cpp` change is proposed.

### 3. Sidechain and event traversal guards

`tests/test_graph.cpp` alone implements the proposed test-local probes. Fresh
graphs render forward, reverse, and seeds **1, 42, 0xC0FFEE** through the existing
off-callback hook. The shared `memcmp` stereo oracle retains its signed-zero
sensitivity check. These fixtures extend the existing aggregate determinism
assertion (with per-fixture/per-variant failure diagnostics); graph/tree counts
remain **149 / 2,272 checks across 24 suites**, without changing the README.

- Sidechain: two ramp sources, a real 64-sample latent branch and its direct
  sibling, two compressor-shaped probes, and their sum. One probe delays the
  key; the mirror delays main to meet the latent key. Four 256-frame blocks
  check both channels of the actual main/key captures against the independently
  delayed ramp, and compare ducked output to calculated samples and to forward
  bytes. Startup and compensation history across blocks are included.
- Events: a head fans out through latent/direct siblings, then both paths feed
  two test instruments. NoteOn, three expression dimensions, and NoteOff reach
  each instrument once per path. Their absolute arrivals are independently
  expected at **96, 160, 255, 256, 288**: the 255/256 pair tests adjacent samples
  across a block boundary, and a third block checks no repeat delivery.
  Captures retain frame, segment start/length, type, channel, dimension, full
  note id, parameter id and exact double bits. Each event must be inside its
  segment while its frame stays block-relative. Explicit integer-field arrays
  avoid comparing uninitialised Event padding. Both instruments render changes
  at those frames; samples and complete event records compare byte-for-byte
  across traversal variants. Push rejection, loss, capture overflow, expected
  fan-in multiplicity and actual deferral are checked. Probe storage is fixed
  before callbacks; there is no test-callback allocation.

### Validation and deliberate failures

- GCC 15.2.0 Debug: `test_all.sh` **2,272 / 24**, validators clean, **3.93 s**.
- Clang 21.1.8 Release: the same full run passes, **2.98 s**.
- GCC 15.2.0 TSan, `TSAN_OPTIONS=halt_on_error=1`: the same full run passes,
  **19.00 s**, including all five variants of both new fixtures.
- The changed translation unit is warning-clean with both compilers under
  `-Wall -Wextra -Wconversion -Wshadow -pedantic`; `git diff --check` is clean.
- **Sidechain plant:** in a scratch copy of graph.cpp, set the seven-node
  fixture's sidechain delay to zero just before accumulation. Compensation
  metadata still reports 64 before processing, but the real key arrives early.
  The new sidechain guard fails in **all five modes**, exit 1, 149 checks /
  1 aggregate failure, under both GCC Debug and GCC TSan.
- **Event-offset plant:** in another scratch copy, add one sample to
  `now_ + e.frame + delay` on the six-node event fixture's forwarding edges.
  All modes are identically wrong, so cross-mode comparison alone could pass;
  the independent frame/audio oracle rejects **all five modes**. Exit 1,
  149 checks / 1 aggregate failure, under both GCC Debug and GCC TSan.
  Neither negative TSan run reported a runtime warning: the guard itself fired.

Plants were compiled into replacement graph objects linked before the clean
core archive, against the real test object; none entered the tracked tree.
The clean builds and full suites above ran after the final test code changes.
To reproduce: build the existing GCC Debug / Clang Release / GCC TSan trees,
then `bash adi_daw/tools/test_all.sh <build-dir>` (with the TSan option above).
The claim is released in the final pre-merge commit; only tests and this log
remain in the PR's net diff.

### 4. Worker-pool handoff — proposed only, then wait

The fixtures supply repeatable serial baselines, exact expected audio/key
samples and event records, nonzero segments, and carried delay/event state.
A pool would need to run these SAME fixtures through its real executor with
one and multiple workers, deliberately varied completion orders, repeated
blocks and seeded stress under TSan, comparing against the serial baseline.
The current level permutations are serial: passing them under TSan does not
prove a concurrent scheduler race-free.

Before parallel dispatch, preserve the complete event-forwarding pass and its
split calculation; preserve fan-in edge summation/event ordering. A level's
completion barrier must include main AND sidechain dependencies. Each node's
output/history and each consumer's delay rings need a single writer. The
current `mixPtrs_`/`sidePtrs_` scratch is shared by Graph: it must become safe
for simultaneous runNode calls (for example, preallocated worker-local
scratch), and shared GraphStats writes need a race-free collection strategy.
Probe captures already belong to individual nodes; inspect them after joining,
not from worker threads. Keep preparation, allocation, seeds and test setup off
the callback. Use mutation checks for omitted dependencies, scratch aliasing,
and event-offset mistakes against the parallel path too. Then measure the
32/64/128-frame dispatch/barrier overhead and tail latency with the benchmark
before choosing a pool threshold; these fixtures alone do not establish speed.

No pool implementation has started. The GCC TSan CI leg and Clang allocation-
counter conflict remain with mac. No `.github/**`, `test_device.cpp`, `src/`,
platform code, schema, ADR or other agent's log was changed. After green CI and
merge, this round stops here awaiting win/Adi.


---

## 2026-09-23 — task 5: seeded level permutation and byte-exact guard

Branch `linux/level-permutation`, based on main `2155fb4` after win's review
(PR #54). #52 and #53 were already merged, in that order, after all 19 checks
passed on each reviewed head; their claims were removed before merge. The new
claim in commit `3c83e80` names exactly `src/adi/engine/graph.hpp`,
`src/adi/engine/graph.cpp`, and `tests/test_graph.cpp`, as granted by win and Adi.
It is released in the final pre-merge commit. No other engine file was changed.

**Hook choice:** the clearly named `Graph::permuteLevelsForTest(uint32_t)`,
documented as test support, rather than a preprocessor flag. The caller must
have successfully prepared the graph and must not have a callback running.
Each call sorts ids within each existing level, then applies explicit
Fisher-Yates using mt19937 from the supplied seed; its result does not depend
on previous calls or a standard library's implementation of std::shuffle.
Level membership, edges and consumer summation order are unchanged. `levels()`
remains read-only, with no cast or friend. `prepare()` resets the normal order.
The existing reverse flag still applies. The hook is never called by production
processing. Verified that prepare/release and all callback implementation code
are textually unchanged; no private scheduler field was added or changed.

**Guard:** a four-level fixture has four sources, two sums, two gains, and a
final sum, so three levels have independent members. It renders forward,
reverse, and seeds **1, 42, 0xC0FFEE (12648430)**, comparing both 512-frame
channels with `memcmp`. It verifies three distinct permutations, reproducibility
after an intervening seed, unchanged membership, movement in every nontrivial
level, and restoration after prepare. A right-channel +0/-0 pair proves that the
oracle distinguishes bytes even when float equality would accept them. The
fixture's nonzero sum is checked separately. The existing five aggregate
assertions now cover these cases: the suite remains 149 checks, the tree 2,272.

### Validation and planted defects

- GCC 15.2.0 Debug: full `test_all.sh` passes 2,272 checks / 24 suites plus all
  five validators (3.93 s).
- Clang 21.1.8 Release: the same full validation passes (2.94 s).
- GCC 15.2.0 TSan: the same full validation passes (19.51 s), with
  `TSAN_OPTIONS=halt_on_error=1`; all five traversal variants run in that binary.
- Both changed translation units pass `-Wall -Wextra -Wconversion -Wshadow
  -pedantic` with both compilers, without diagnostics.
- **Aliased scratch plant:** in a temporary copy of graph.cpp, after prepare
  allocates output buffers, assign `slots_[1].chanPtrs = slots_[0].chanPtrs`
  for the nine-node fixture. Link that instrumented replacement object ahead
  of the unmodified core archive and run the actual graph test suite. GCC
  Debug and GCC TSan both exit 1: reverse, seed 42 and seed 0xC0FFEE mismatch;
  the byte-exact guard and fixture-sum guard fail (149 checks, 2 failures).
  This is an assertion failure, not a TSan runtime failure masking the test.
- **No-op hook plant:** a temporary hook that returns without permuting fails
  the schedule/reproducibility assertion (149 checks, 1 failure).
- **Float-oracle plant:** replace the two memcmp calls with vector float
  equality in a temporary test source. The signed-zero sensitivity guard
  fails (149 checks, 1 failure).

All plants were separate scratch sources/objects; none entered the repository
working tree. Clang TSan's allocation-counter conflict in `tests/test_device.cpp`
remains recorded for mac and untouched. No platform, driver, schema, ADR,
workflow or other agent's log was edited.

Reproduce the clean run with the previously documented GCC TSan configuration,
then `cmake --build <tsan-build> -j 3` and
`TSAN_OPTIONS=halt_on_error=1 bash adi_daw/tools/test_all.sh <tsan-build>`.

**What I would do next, pending direction:** coordinate with mac on CI coverage
for the opt-in benchmark and the Clang TSan allocation-counter issue; then, if
assigned, extend the determinism fixtures to sidechains and event routing before
any worker-pool implementation. No new ownership is assumed. The five onboarding
tasks are now locally verified; after this PR passes CI and merges I will wait
for win or Adi rather than start the next work.

---

## 2026-09-23 — opt-in small-block benchmark scaffold

Branch `linux/small-block-benchmark`, created from main and fast-forwarded over
the pending baseline correction (PR #52); merge that prerequisite first.
The target was proposed in the preceding audit entry before CMake was changed.
Claim: `tools/benchmark_blocks.cpp`, shared `CMakeLists.txt`, and this log only.

`ADI_BUILD_BENCHMARKS=ON` adds `adi_block_benchmark`, off by default. It drives
Graph through BlockProcessor at 48 kHz stereo, with 32 warmup callbacks and
1,000 measured callbacks per row. Fixtures: eight active tracks with four gains
each; 64 tracks with four gains each, 63 tracks silent; eight tracks with four
gains and 16 notes of three-dimensional MPE+ into track 1, at exactly 500 Hz
on a global sample clock. Node counts are 41 / 321 / 41. Sources and gains are
synthetic; this is not a real plugin-load or Ableton comparison.

CSV output includes nearest-rank p50/p99/max microseconds, callback deadline
misses (`time > frames / 48000`, labelled dropouts), and separate dropped and
rejected event counts. These misses are not physical device xruns. Preparation,
event production, statistics and printing stay outside callback timing; the
measured call includes graph scheduling and event routing. All fixtures verify
their final stereo output sum and reject lost expression events.

### Verification

- GCC 15.2.0 and Clang 21.1.8 Release: build, statistics self-test, all 15 fixture
  rows with 1,000 callbacks each; `test_all.sh` passes 2,272/24 plus all validators.
- New source passes the full requested warning flags with both compilers.
- Clang ASan+UBSan and GCC TSan: self-test and all 15 fixture rows with ten
  measured callbacks each, no sanitizer diagnostics (timings not used below).
- Eight temporary-source plants all exit 1: wrong p50, wrong p99, wrong maximum,
  inclusive deadline comparison, source forced to silence, event capacity 1,
  invalid edge, invalid output node. They hit their corresponding statistics,
  output, event-loss or graph-validity guard. Plants never enter the working tree.
- Invalid CLI cases exit 2: 0, 1000001, -1, `3x`, missing value and unknown flag.
  The self-test also exercises the one-observation and empty-input cases.
- CI's default configuration leaves the opt-in benchmark off. macOS/Windows
  execution of the new target is not claimed; the source uses standard C++20.

### Example measurements, this Ubuntu host only

Each timing cell is p50 / p99 / max in microseconds. The machine is not isolated
or real-time scheduled; these are one-run observations, not performance promises.
All rows in both runs recorded zero callback deadline misses and zero lost events.

| Project | Frames | GCC Release | Clang Release |
|---|---:|---|---|
| active | 32 | 2.686 / 12.962 / 31.679 | 2.804 / 3.266 / 43.870 |
| active | 64 | 3.524 / 5.133 / 49.672 | 3.589 / 13.824 / 42.845 |
| active | 128 | 4.929 / 15.510 / 41.166 | 4.868 / 5.632 / 31.567 |
| active | 2048 | 69.055 / 193.435 / 486.929 | 68.478 / 143.248 / 211.518 |
| active | 4096 | 149.316 / 217.498 / 387.213 | 146.387 / 234.495 / 316.881 |
| silence-heavy | 32 | 13.862 / 32.432 / 125.358 | 13.698 / 27.802 / 57.226 |
| silence-heavy | 64 | 18.855 / 45.230 / 103.036 | 18.603 / 40.588 / 57.371 |
| silence-heavy | 128 | 30.268 / 60.083 / 89.697 | 30.326 / 64.443 / 252.775 |
| silence-heavy | 2048 | 630.544 / 1345.162 / 2187.876 | 543.865 / 1282.262 / 1762.641 |
| silence-heavy | 4096 | 1716.111 / 3556.011 / 6484.844 | 1748.928 / 3034.787 / 4424.129 |
| mpe-storm | 32 | 2.622 / 5.645 / 25.330 | 3.060 / 16.563 / 121.652 |
| mpe-storm | 64 | 5.990 / 16.712 / 48.143 | 8.034 / 18.475 / 40.855 |
| mpe-storm | 128 | 9.546 / 23.895 / 138.006 | 11.014 / 34.084 / 61.509 |
| mpe-storm | 2048 | 256.406 / 356.222 / 426.012 | 403.284 / 524.634 / 770.518 |
| mpe-storm | 4096 | 773.321 / 1425.007 / 1709.331 | 1350.188 / 2235.646 / 2838.879 |

```sh
cmake -S adi_daw -B /tmp/adi-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DADI_WITH_JUCE=OFF -DADI_BUILD_BENCHMARKS=ON
cmake --build /tmp/adi-bench -j 3
/tmp/adi-bench/adi_block_benchmark --self-test
/tmp/adi-bench/adi_block_benchmark --iterations 1000
bash adi_daw/tools/test_all.sh /tmp/adi-bench
```

Task 5 remains waiting on win's engine claim response in the preceding entry;
no engine file was changed. Next after that response: add the permitted shuffle
hook, use memcmp for both output channels, plant a defect and run under GCC TSan.

---

## 2026-09-23 — sanitizer and portability audit; next targets proposed

Audited the same main-based baseline plus the CLAP count correction, with
`ADI_WITH_JUCE=OFF`. Separate instrumented C and C++ build trees; three build
jobs, serial test execution (the suites share fixed temporary-directory names).

| Configuration | Configure s | Build s | CTest s | test_all.sh s | Result |
|---|---:|---:|---:|---:|---|
| Clang 21.1.8 ASan+UBSan | 2.05 | 105.48 | 11.16 | 11.85 | 24/24 suites; 2,272 checks; five validators; no findings |
| Clang 21.1.8 TSan | 2.08 | 90.25 | not run | not run | linker conflict with allocation-counting operator new/delete |
| GCC 15.2.0 TSan | 1.15 | 82.92 | 18.05 | 18.10 | 24/24 suites; 2,272 checks; five validators; no reported races |

Reproduce using the baseline configure command with both `CMAKE_C_FLAGS` and
`CMAKE_CXX_FLAGS` set to `-fsanitize=address,undefined -fno-omit-frame-pointer`
(Clang), or `-fsanitize=thread -fno-omit-frame-pointer` (GCC). Run
`ctest --test-dir <build> --output-on-failure -j 1` and
`bash adi_daw/tools/test_all.sh <build>`. Environment: `ASAN_OPTIONS=halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, `TSAN_OPTIONS=halt_on_error=1`.
These are finite stress runs, not a proof that every possible race is absent.

**Clang TSan reproduction:** configure the same thread-sanitized tree with
Clang and build target `adi_device_tests`. The static `libclang_rt.tsan_cxx`
defines operator new/delete strongly, conflicting with the allocation counter
in `tests/test_device.cpp`. That file is mac's claim; no edit or suppression was
made. GCC TSan links the unchanged counters and runs all suites. Clang TSan is
not claimed as passing.

**Portability sweep:** replayed `compile_commands.json` for every translation
unit under `src/adi/**` and `tests/**`, using `-fsyntax-only -Wall -Wextra
-Wconversion -Wshadow -pedantic` in addition to the existing project flags.
GCC: 45 units, 72.79 s; Clang: 45 units, 124.60 s. Both exited zero with no
warning/error diagnostics. No third-party translation unit or platform file was
part of this sweep; no source change was necessary.

**Benchmark proposal, before any CMake edit:** add an opt-in `ADI_BUILD_BENCHMARKS`
option and `adi_block_benchmark` target in shared `CMakeLists.txt`, with its only
new source in `tools/benchmark_blocks.cpp`. Drive Graph through BlockProcessor,
48 kHz stereo, 32/64/128/2048/4096 frames, fixed active chains, silence-heavy
chains and a 16-note / three-dimension / 500 Hz expression stream. Report measured
callback p50/p99/max and deadline misses separately from event drops. Warm up
before measurement; exclude preparation, event-production and output formatting
from callback timing. This is a headless scaffold, not evidence about physical
audio xruns or Ableton performance. No claimed engine implementation changes.

**win — claim request; waiting, no edit:** task 5 needs a way to shuffle every
prepared dependency level. Graph currently exposes reverse traversal only and
read-only `levels()`. Please grant a narrow claim on
`src/adi/engine/graph.hpp` and `src/adi/engine/graph.cpp`, or provide an off-callback
seeded permutation test hook, so `tests/test_graph.cpp` can verify forward,
reverse and shuffled traversal with memcmp under TSan. I will not cast away the
read-only interface or modify your engine claim. The current test compares floats
with `!=`, which is not a byte comparison (e.g. signed zero); the planned guard
must compare bytes and fail a planted scheduling/output defect.

---

## 2026-09-23 — main baseline and a portable check count

Branch `linux/baseline`, based on main `f6910b1474f306bef70b21f50ce6e4f9475846cf`
(PR #41). No work was based on `agent/win-dev`; there was nothing to discard.
Re-read the collaboration protocol, onboarding, ADR-0109, ADR-0102 and ADR-0120.
Windows drivers and platform/UI work remain outside my scope.

**Repository inventory:** previously cloned `arieladi/Adi`, `arieladi/AdiGuard`,
and `arieladi/adi-vst-synth`. No `adi-surge` repository was cloned. Earlier setup
listed filenames in the other clones; since Adi restricted this assignment to
`arieladi/Adi`, I have not read or modified either other repository and will not.

Ubuntu x86-64; GCC/G++ 15.2.0; Clang 21.1.8; CMake 4.2.3; Ninja 1.13.2;
Python 3.14.4. Dependencies fetched with `tools/fetch_external.sh --build-only`:
SQLiteCpp `59a047b8d`, json `55f93686c`, CLAP `195b42a00`, pins verified.
All builds use C++20, `ADI_WITH_JUCE=OFF`, and three build jobs.

### Baseline finding and correction

Unmodified main's GCC Debug/Release and Clang Debug builds passed all 24 suites
and all five validators, but `test_all.sh` returned 1: 2,274 actual checks versus
2,273 in README. `testClapHostWithoutAnyPlugin` counted one assertion per default
search path, making the total depend on the OS/environment. It now asserts
`all_of(paths, nonempty)` once, keeping the separate nonempty-list assertion.
README's fixed total is **2,272 checks across 24 suites**; no check was removed
from the property being tested. No host implementation was edited.

**Negative control:** compiled a temporary copy of `test_clap.cpp` against the
same GCC Debug core, with an empty path appended immediately after obtaining
the defaults. It exited 1 with `FAIL and none of them is empty` (324 checks,
1 failure). The unmodified fixed source passes all 324 CLAP checks. The plant
never entered the repository working tree.

### Measured wall times (seconds)

Build times are initial configure/build wall times, not incremental rebuilds.
The final Clang Release build already contains the count correction; the other
three required only an incremental rebuild of the CLAP test afterward.

| Configuration | Configure | Initial build | Final test_all.sh | Result |
|---|---:|---:|---:|---|
| gcc-debug | 0.75 | 68.8 | 3.9 | 2,272 checks / 24 suites; five validators PASS |
| gcc-release | 0.74 | 161.81 | 2.86 | 2,272 checks / 24 suites; five validators PASS |
| clang-debug | 1.03 | 82.12 | 3.55 | 2,272 checks / 24 suites; five validators PASS |
| clang-release | 0.94 | 151.59 | 2.83 | 2,272 checks / 24 suites; five validators PASS |

Reproduce with separate build directories:

```sh
cmake -S adi_daw -B /tmp/adi-gcc-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DADI_WITH_JUCE=OFF \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build /tmp/adi-gcc-debug -j 3
bash adi_daw/tools/test_all.sh /tmp/adi-gcc-debug
```

Repeat with `clang`/`clang++` and Release. The earlier 1,028/13 run was on
`aedf0fa` and is superseded by this baseline.

**Delivery status:** local validation complete; PR/CI/merge pending. The existing
OAuth login is usable for the authorized Adi-only operations; replacing it with
Adi's requested repository-scoped token remains a separate user terminal step.
The authentication note contains no token. No credential was requested in chat,
printed, or read from the gh configuration store. Existing CI on main passed,
but is not evidence for this branch; no claim of branch CI success is made.

Next: ASan+UBSan, then TSan across all 24 suites. No schema, ADR, platform,
driver or other agent's claimed implementation file was changed.
