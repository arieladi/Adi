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

## Regression watch — session runtime, 2026-09-23

Main `5744ac4`, same i5-3550S / Ubuntu 26.04.1 / schedutil (all four CPUs),
GCC 15.2.0 and Clang 21.1.8 Release, 48 kHz stereo, 32 warmups and 2,000
measured callbacks per row. Builds completed before sequential measurement.
The comparison uses the complete #66 CSV at `e4c3b00`; times are µs and deltas
are signed p50 percentages. The three historical tables above remain intact.
The active-64 1024-frame movements exceed 20% and are reported to win in
[linux's log](../collab/linux.md); they do not establish a cause.

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

## Regression watch — parameter capture merged, 2026-09-23

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

The retained #66 p50 baselines are GCC 300.029 µs and Clang 344.565 µs.

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

See [linux’s log](../collab/linux.md) for sanitizer validation and the assigned audits.

## Regression watch — #73, `fb97ea8`, 2026-09-23

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

Release and sanitizer validation passed; see [linux’s log](../collab/linux.md).

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

## Regression watch — #74, `e1a4a5a`, 2026-09-23

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

**Validation limitation:** Clang ASan+UBSan found a stack-use-after-scope in `adi_param_ops_tests` teardown. Release and GCC TSan pass, but this revision is not sanitizer-clean. The exact trace and reproducer are recorded for win in [linux’s log](../collab/linux.md).

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
