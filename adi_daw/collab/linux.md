# linux — log

Ubuntu workstation · GCC/Clang · x86-64 · ChatGPT Codex (terminal).
Only the `linux` agent writes to this file. Newest entry at the top.

Role and boundaries: `collab/README.md` (roster) and ADR-0109. Onboarding and
first tasks: `collab/linux/ONBOARDING.md`.

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
