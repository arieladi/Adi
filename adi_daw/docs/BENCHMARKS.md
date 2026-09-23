# Headless block benchmark

`adi_block_benchmark` measures synthetic engine callbacks for ADR-0102. It
calls `Graph` through `BlockProcessor` directly, without an audio device, GUI,
JUCE or third-party audio plugin. Source: [benchmark_blocks.cpp](../tools/benchmark_blocks.cpp).
The benchmark is opt-in and is not a device-latency or Ableton comparison.

## Build and run

These shell commands run from the **repository root**. Prerequisites are Git,
CMake 3.25 or newer, a C/C++20 toolchain and a CMake build tool. Dependencies
are pinned by the repository's fetch script; use the build-only set:

```bash
bash adi_daw/tools/fetch_external.sh --build-only

cmake -S adi_daw -B adi_daw/build-bench-gcc \
  -DCMAKE_BUILD_TYPE=Release -DADI_WITH_JUCE=OFF -DADI_BUILD_BENCHMARKS=ON \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build adi_daw/build-bench-gcc --target adi_block_benchmark --parallel 3

cmake -S adi_daw -B adi_daw/build-bench-clang \
  -DCMAKE_BUILD_TYPE=Release -DADI_WITH_JUCE=OFF -DADI_BUILD_BENCHMARKS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build adi_daw/build-bench-clang --target adi_block_benchmark --parallel 3
```

Use separate build directories for the compilers. Finish both builds before
measuring and run the compilers sequentially. Check the exit status of each
command; do not use a partial CSV from a failed run.

```bash
adi_daw/build-bench-gcc/adi_block_benchmark --self-test
adi_daw/build-bench-gcc/adi_block_benchmark --iterations 2000 > gcc-control.csv
adi_daw/build-bench-gcc/adi_block_benchmark --breakdown --iterations 2000 > gcc-breakdown.csv

adi_daw/build-bench-clang/adi_block_benchmark --self-test
adi_daw/build-bench-clang/adi_block_benchmark --iterations 2000 > clang-control.csv
adi_daw/build-bench-clang/adi_block_benchmark --breakdown --iterations 2000 > clang-breakdown.csv
```

`--help` describes the CLI. The default is 1,000 measured callbacks; the allowed
`--iterations N` range is 1 through 1,000,000. There are **32 additional warmup
callbacks per project/block-size combination**, excluded from reported times.
`--self-test` checks quantiles, the strict deadline comparison, empty-input
rejection and the breakdown count/time partition. The benchmark also refuses
incorrect final audio or lost expression events rather than publishing timings
for a broken fixture.

To run the full validation as well, build all targets first (the target-only
commands above do not build the test binaries):

```bash
cmake --build adi_daw/build-bench-gcc --parallel 3
bash adi_daw/tools/test_all.sh "$PWD/adi_daw/build-bench-gcc"
cmake --build adi_daw/build-bench-clang --parallel 3
bash adi_daw/tools/test_all.sh "$PWD/adi_daw/build-bench-clang"
```

## Matrix and projects

All projects run stereo at **48 kHz**, at **32, 64, 128, 256, 512, 1024, 2048
and 4096 frames**. The everyday range is 256–2048 (Adi's note in
[PR #62](https://github.com/arieladi/Adi/pull/62)); 32–128 covers tracking and
4096 remains the supported cap. The benchmark does not require a sound card
to offer any of these sizes.

The three primary projects and the additional all-active control are:

| CSV project | Tracks × effects per track | Nodes | Workload |
|---|---:|---:|---|
| `active` | 8 × 4 | 41 | All tracks active |
| `silence-heavy` | 64 × 4 | 321 | One active track, 63 silent tracks |
| `mpe-storm` | 8 × 4 | 41 | Active audio plus NoteExpression events for 16 note IDs × 3 dimensions at 500 Hz into track one |
| `active-64` | 64 × 4 | 321 | All-active control for the 64-track silence-heavy graph |

Each track is a constant source and four unity `GainNode`s, feeding a master
`SumNode`: five nodes per track plus one master. These are synthetic sources
and gains, not representative plugin presets. MPE events are produced on a
global sample clock every 96 samples, outside the timed interval; the rate
is not restarted at each block. The MPE fixture exercises routing/splitting,
not a real synthesizer or controller.

Ordinary mode produces **32 summary rows**: four projects × eight sizes.
There is currently no project or block-size filter; extract the rows of
interest from the CSV after the run succeeds.

## What the numbers mean

The timer is `std::chrono::steady_clock` around each `BlockProcessor::process`
call. It measures elapsed wall time, including any preemption within that
interval. Graph processing, routing, mixing and output copying are included;
setup, input-event production, allocation of the measurement arrays, summary
sorting and CSV printing are outside the interval. Callbacks run back-to-back,
not paced by an audio driver. These are steady-state measurements after warmup,
not preparation, first-use or wake-transition timings.

| Ordinary CSV field | Meaning |
|---|---|
| `project`, `tracks`, `effects_per_track`, `frames`, `nodes` | Fixture identity and dimensions |
| `callbacks` | Number of measured callbacks in this row, excluding warmup |
| `p50_us` | Nearest-rank 50th percentile: sorted sample at rank `ceil(0.50 × N)`, one-based; for even N this is not the average of the middle pair |
| `p99_us` | Nearest-rank 99th percentile: rank `ceil(0.99 × N)` |
| `max_us` | Largest observed duration, not a worst-case upper bound |
| `dropouts` | Synthetic deadline misses: duration **strictly greater than** `frames / 48000` seconds |
| `event_drops` | Graph event-drop counter increase during measured callbacks |
| `rejected_events` | Input-event pushes rejected during measured callbacks |

At 48 kHz, the callback budgets at 256/512/1024/2048 are
5.333/10.667/21.333/42.667 ms; 32 is 0.667 ms and 4096 is 85.333 ms.
Deadline misses and event loss are different conditions. The benchmark treats
nonzero event loss as a failed fixture; a deadline miss is reported in the CSV.

`dropouts` is **not device xruns**, and these times are not end-to-end audio
latency. There is no driver, sound-card buffer negotiation, I/O, or real-time
thread scheduling in this test. Zero misses here cannot establish glitch-free
hardware playback. CPU frequency, governor, thermals, cache state, background
load and compiler affect the numbers. Compare matched runs and keep their
sample count, build and machine metadata. No table here supports an Ableton
performance claim or a general worst-case guarantee.

## `--breakdown`

This mode emits one row per measured callback for `silence-heavy` and
`active-64` only: **16 × N rows**, or 32,000 at `--iterations 2000`.
It uses benchmark-local node wrappers; it adds no engine instrumentation hook.

| Breakdown field | Meaning |
|---|---|
| `project`, `frames`, `callback` | Fixture, block size and zero-based measured callback index within that combination |
| `processed_nodes`, `skipped_nodes` | Changes in GraphStats node calls and suspended node-blocks; these event-free fixtures have one segment, so each processed node is called once |
| `callback_us` | Whole callback wall time with wrappers enabled |
| `node_body_us` | Sum of wrapper timings around actual `Node::process` calls |
| `residual_us` | Per-callback `callback_us - node_body_us`: all remaining Graph work and timer bookkeeping, not an isolated skipped-node duration |
| `body_us_per_processed_node` | Node-body sum divided by the processed count |
| `skipped_scheduler_us` | `NA`: skipped nodes do not call their wrapper, so their scheduler time cannot be measured through this interface |

Expect **6 processed / 315 skipped nodes** for silence-heavy and
**321 processed / 0 skipped** for active-64. The counts cover nodes, not tracks
or input edges. `GraphStats::suspendClears` counts suspension buffer clears;
`GraphStats::inputsSkipped`, added by #65, counts skipped input accumulations.
Neither counter is currently exported by this benchmark's CSV, and neither is
the CSV's `skipped_nodes` column.

Clock reads and wrappers perturb the run; ordinary mode is the timing control.
Do not subtract separately reported medians as though they were one callback,
or call the residual pure skipped-node/accumulate time. This mode helps locate
work without claiming an exact scheduler-path split.

## Recorded silence-heavy measurements

All three tables use this setup, recorded on **2026-09-23**:

- Intel Core i5-3550S @ 3.00 GHz, x86-64, 4 cores / 4 threads; Ubuntu 26.04.1 LTS.
- **schedutil governor on all four CPUs**; no affinity or real-time-priority changes.
- GCC 15.2.0 and Clang 21.1.8, Release `-O3 -DNDEBUG`, JUCE off.
- **2,000 measured callbacks after 32 warmups per combination**, 48 kHz stereo.
- Both builds completed before sampling; GCC then Clang, ordinary then breakdown;
  no concurrent build/test workload. Each table is one ordinary-mode run per compiler.

The tables preserve observed p50, p99 and maximum, including scheduling spikes.
A dash means **not measured**, not zero: 256/512/1024 joined the matrix in #63.
All measured rows below had zero event drops and rejected events. Earlier
10,000-iteration runs and the intermediate two-row rerun in #61 remain in
[the Linux log](../collab/linux.md); they are not substituted into this matched
2,000-iteration sequence. Raw per-callback breakdown timings are a separate
instrumented run and are not used for these summary tables.

### 1. Before #60: repeated suspension clearing

Engine base `f9d5c6a`; benchmark/result published in
[PR #59](https://github.com/arieladi/Adi/pull/59), merged as `d49bfd1`.
Machine/governor: i5-3550S, schedutil, as above. The matrix then had five sizes.

| Compiler | Frames | p50 µs | p99 µs | Max µs | Deadline misses |
|---|---:|---:|---:|---:|---:|
| GCC | 32 | 13.296 | 25.548 | 98.206 | 0 |
| GCC | 64 | 18.599 | 36.129 | 78.675 | 0 |
| GCC | 128 | 30.451 | 61.929 | 124.740 | 0 |
| GCC | 256 | — | — | — | — |
| GCC | 512 | — | — | — | — |
| GCC | 1024 | — | — | — | — |
| GCC | 2048 | 604.085 | 1395.681 | 3766.980 | 0 |
| GCC | 4096 | 1729.522 | 3011.969 | 4892.603 | 0 |
| Clang | 32 | 14.211 | 27.631 | 46.230 | 0 |
| Clang | 64 | 18.983 | 36.761 | 117.778 | 0 |
| Clang | 128 | 30.680 | 56.204 | 88.597 | 0 |
| Clang | 256 | — | — | — | — |
| Clang | 512 | — | — | — | — |
| Clang | 1024 | — | — | — | — |
| Clang | 2048 | 660.180 | 1582.226 | 4604.204 | 0 |
| Clang | 4096 | 1792.442 | 4004.392 | 15080.987 | 0 |

### 2. After #60: clear once, before #65's accumulate skip

Eight-size run based on `54947a0` (engine fix from `5c9e62e`), published in
[PR #63](https://github.com/arieladi/Adi/pull/63), merged as `f56d059`.
Machine/governor: the same i5-3550S, schedutil. Sleeping outputs were cleared
once, but active consumers still read them.

| Compiler | Frames | p50 µs | p99 µs | Max µs | Deadline misses |
|---|---:|---:|---:|---:|---:|
| GCC | 32 | 8.138 | 19.458 | 168.255 | 0 |
| GCC | 64 | 9.204 | 20.830 | 69.184 | 0 |
| GCC | 128 | 11.426 | 26.645 | 65.004 | 0 |
| GCC | 256 | 15.767 | 32.378 | 74.570 | 0 |
| GCC | 512 | 23.168 | 44.143 | 105.593 | 0 |
| GCC | 1024 | 36.761 | 69.675 | 160.215 | 0 |
| GCC | 2048 | 72.922 | 121.518 | 202.778 | 0 |
| GCC | 4096 | 168.191 | 496.971 | 597.161 | 0 |
| Clang | 32 | 8.065 | 19.627 | 85.171 | 0 |
| Clang | 64 | 9.184 | 20.137 | 48.198 | 0 |
| Clang | 128 | 10.571 | 22.018 | 83.792 | 0 |
| Clang | 256 | 15.562 | 35.734 | 310.633 | 0 |
| Clang | 512 | 22.311 | 42.853 | 119.180 | 0 |
| Clang | 1024 | 35.633 | 70.740 | 667.865 | 0 |
| Clang | 2048 | 68.857 | 117.662 | 925.122 | 0 |
| Clang | 4096 | 167.182 | 472.023 | 648.571 | 0 |

### 3. After #65: skip known-zero, idle inputs

Measured main **`e4c3b00`**, after
[PR #65](https://github.com/arieladi/Adi/pull/65), with the benchmark unchanged.
Machine/governor: the same i5-3550S, schedutil, checked before and after.

| Compiler | Frames | p50 µs | p99 µs | Max µs | Deadline misses |
|---|---:|---:|---:|---:|---:|
| GCC | 32 | 6.815 | 17.158 | 29.172 | 0 |
| GCC | 64 | 6.844 | 18.026 | 33.218 | 0 |
| GCC | 128 | 6.693 | 17.060 | 32.916 | 0 |
| GCC | 256 | 7.218 | 18.138 | 40.911 | 0 |
| GCC | 512 | 8.254 | 19.400 | 44.719 | 0 |
| GCC | 1024 | 10.891 | 23.182 | 42.985 | 0 |
| GCC | 2048 | 16.997 | 36.594 | 79.059 | 0 |
| GCC | 4096 | 29.359 | 53.357 | 67.332 | 0 |
| Clang | 32 | 7.294 | 17.258 | 101.436 | 0 |
| Clang | 64 | 7.416 | 17.901 | 25.367 | 0 |
| Clang | 128 | 7.619 | 19.010 | 41.191 | 0 |
| Clang | 256 | 8.295 | 19.410 | 99.596 | 0 |
| Clang | 512 | 9.303 | 20.535 | 88.450 | 0 |
| Clang | 1024 | 11.556 | 24.766 | 65.859 | 0 |
| Clang | 2048 | 16.416 | 31.104 | 69.892 | 0 |
| Clang | 4096 | 29.486 | 51.570 | 113.213 | 0 |

At 4096 frames, p50 progresses **1729.522 → 168.191 → 29.359 µs (GCC)** and
**1792.442 → 167.182 → 29.486 µs (Clang)**. The latest change cuts the preceding
4096 median by about 82%. In the everyday 256–2048 range, the new medians are
7.218–16.997 µs for GCC and 8.295–16.416 µs for Clang.

The three stages are measurements, not proof that all changes in p99/max are
caused only by the code; they are separate desktop runs, and the matrix gained
three sizes between tables 1 and 2. Silence-heavy processes only six nodes,
versus 41 for the eight-track active fixture: equality with that active control
is not the performance target, and total time divided by 315 would not isolate
the cost of one sleeping node.

#65 skips only known-zero sources on zero-delay edges without a pending glide;
delayed edges still run to preserve audio in their rings. Its documented signed-
zero change and regression tests belong to the engine review, not to a timing
claim. Both compiler self-tests and full `test_all.sh` validation passed on the
latest measured main: **2,289 checks across 24 suites**, validators clean.

The interpretation and engine decisions are in [DECISIONS.md](DECISIONS.md)
(ADR-0043, ADR-0058 and ADR-0102) and the dated [win log](../collab/win.md).
