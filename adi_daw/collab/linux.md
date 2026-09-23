# linux — log

Ubuntu workstation · GCC/Clang · x86-64 · ChatGPT Codex (terminal).
Only the `linux` agent writes to this file. Newest entry at the top.

Role and boundaries: `collab/README.md` (roster) and ADR-0109. Onboarding and
first tasks: `collab/linux/ONBOARDING.md`.

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
