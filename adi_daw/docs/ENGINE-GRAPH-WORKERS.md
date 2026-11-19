# Graph render workers

The owner can configure `Graph`, `GraphHost`, or `Session` with
`setParallelRendering(workers, minimumFrames, minimumNodes, initialize, context)`.
The two thresholds are required: supply measured hardware/project policy. Zero
workers retains serial processing, the default. Limits are 0..16 workers,
positive frame threshold and eligible width at least two. `initialize` and
`context` are optional. Invalid configuration leaves the prior choice usable.

Graph configuration requires a subsequent prepare. GraphHost uses the policy
on its next candidate; Session rebuilds immediately when loaded. An owner getter
may expose the latest prepared candidate before audio adoption; the swap still
occurs on the normal callback boundary. Failed candidate preparation does not
publish it. Valid requested policy remains available for retry. This is runtime
configuration, with no project opcode, schema, persisted preference or UI edit.

Preparation creates persistent workers only when the maximum block and eligible
level width can use them, capped by width minus one. Workers finish startup and
optional driver initialization before prepare returns. Initialization runs once
on each worker, potentially concurrently, outside CallbackScope, so the driver can set priority, affinity
or audio-thread identity; synchronize shared setup data inside that hook as needed. Its opaque context must remain valid for future
rebuilds until the policy is replaced or host destroyed. False refuses the whole
pool and joins partial workers. Resource/driver setup precedes borrowed-node
prepare, so this failure cannot reset a still-playing borrowed source.

`Node::canProcessOnWorker()` defaults false and is queried before borrowed-node
prepare; report a stable capability, not a flag established by prepare. Opt in only for independent
per-instance DSP state, thread-safe render/tail/suspension/tap hooks and any
required driver registration. Restore FP controls changed by a node. SumNode,
GainNode and freshly realized MixNode junctions opt in; external/device adapters
retain their callback affinity. Duplicate Node pointers in one graph always stay
on the callback. Source generation, forwarding, splitting and delivery hooks
remain serialized before/after render jobs. Borrowed-node prepare/release still
requires the existing caller lifecycle synchronization with rendering.

Each executing participant has private main/sidechain mix buffers, double sums,
compensation scratch, idle flags and render counters. Slot output/edge state
belongs to its one node. Nodes at a level read completed lower levels; consumer
input order never changes with job completion order. Both precisions, float-only
fallback, compensation, tails, bypass, input taps and event delivery keep their
existing contracts. Tap pointer publication uses a direct acquire load paired
with the publishing release, also understood by thread-race tools.

The callback posts nonblocking OS semaphore wakeups, renders its partition and
callback-only nodes, then spins/yields on atomic completion flags. It never waits
on a semaphore or takes a callback mutex, allocates/frees, starts/joins threads
or logs. Linux uses POSIX semaphores, Windows native semaphores and Apple dispatch
semaphores; only the Linux leg is exercised here. Each job copies the caller's
floating-point environment, explicitly including x86 FTZ/DAZ, then restores the
worker's prior environment. Helpers are warmed during owner setup. Caller FP
controls remain unchanged. Progress still depends on finite, nonthrowing node
work and appropriate driver scheduling; this API is no deadline guarantee.

Unposted/failed jobs run their partition on the caller after other jobs finish.
A separate completed bit survives failure of a subsequent idle wait or FP
cleanup, so an already executed DSP action is never repeated. Dispatch context
lives through completion. Owner release/destruction stops and joins workers
before render storage or node release. No process/prepare/release overlap on the
same graph is permitted.

Below the frame/width thresholds the original serial schedule runs. A cheap
likely-active test also chooses serial for fully suspended levels. It does not
skip nodes or replace their existing silence/tail/tap decisions. Worker-local
counters reduce after barriers; old render/event counters remain comparable.
`parallelLevels`, `workerNodeBlocks` and `workerFallbacks` identify actual work
and fallback. Raw GraphStats inspection requires exclusive/between-process
access, as with its existing mutable counters.

The Release block benchmark now accepts `--workers`, explicit
`--parallel-min-frames` and `--parallel-min-nodes`. Use threshold 1 to measure
wake cost, then profile a proposed switch on the same hardware. Ordinary CSV
adds configured/created workers, parallel levels and worker node-blocks.
Work counters exclude warmup, like the measured timing rows.
`--breakdown` remains serial because its shared node-body timer is not parallel
safe. The default run is serial. Compare p50/p99/max, strict callback-budget
misses and event drops; these are synthetic callbacks, not device xruns or an
Ableton comparison. Do not select a platform-wide default from one cloud run.
Driver/UI policy, external provider opt-ins and hardware/Live measurements are
separate integration work; the owner APIs already reach real Session graphs.

Measured Linux run, 4 November 2026. Raw CSV and environment/source fingerprints
are in [benchmarks/graph-workers-2026-11-04](benchmarks/graph-workers-2026-11-04).
GCC 14.2, Release -O3/-DNDEBUG/Werror, PD on/JUCE off, 48 kHz stereo. The guest
reports AMD EPYC 9V74; quota is four CPUs. Every process was pinned to guest
CPUs 0..3 (distinct exposed core IDs), with normal OS scheduling. Each row has
32 warmups and 1,000 measured callbacks. Builds/tests were idle while measuring.

The baseline hot renderer/header and original benchmark are exact source from
2d73325a4d1cbcb6a3611ac58dc379949153acb1, compiled with the same Release flags
and linked to validated cold dependencies. No new Graph implementation enters
that executable. This is a focused hot-path baseline, not a separate whole-tree
baseline CI build. The feature serial mode and forced/profiled worker modes use
the complete feature Release core. All eight CSVs pass output/event checks.

Cells below are p50 / p99 / max, in microseconds. Profile means three workers,
minimum 4096 frames and 64 eligible nodes; it creates no pool for narrow or
small-block preparations. These are observations from this guest.

| Precision | Fixture / frames | Baseline | New serial | Profile |
|---|---|---|---|---|
| 32 | active / 32 | 2.113 / 37.647 / 176.866 | 2.123 / 4.286 / 45.979 | 2.153 / 3.776 / 83.045 |
| 32 | active-64 / 4096 | 865.260 / 1642.628 / 3272.106 | 748.525 / 1703.489 / 8364.538 | 614.153 / 910.839 / 4778.099 |
| 32 | silence-heavy / 4096 | 19.880 / 101.753 / 267.853 | 19.599 / 79.260 / 202.274 | 21.472 / 39.400 / 79.770 |
| 64 | active / 32 | 2.353 / 2.865 / 20.961 | 2.364 / 3.916 / 86.209 | 2.393 / 2.514 / 54.391 |
| 64 | active-64 / 4096 | 2113.175 / 4660.882 / 8299.239 | 2150.692 / 3948.242 / 6796.558 | 1002.146 / 1505.192 / 2708.950 |
| 64 | silence-heavy / 4096 | 26.370 / 69.324 / 271.788 | 26.480 / 128.112 / 243.857 | 27.901 / 69.224 / 208.874 |

Strict callback-budget misses across all 32 rows (32,000 callbacks per cell):

| Mode | 32-bit | 64-bit |
|---|---|---|
| baseline | 0 | 0 |
| serial | 0 | 0 |
| forced-workers | 13 | 3 |
| profile | 0 | 1 |

Across the twelve 32/64/128-frame rows, median new-serial/baseline p50 ratios
are 0.991 (32-bit) and 1.010 (64-bit). Tail/max values vary; full data is
retained. Forced workers at 32 frames make the 8x4 active median
59.950 us versus 2.123 us serial, demonstrating wake cost.
The profiled 64-bit run's one 32-frame budget miss occurred with no pool
created. No event drops/rejections occurred in any mode. No device xrun or
Live comparison was run. The API therefore keeps serial as the default and
requires explicit measured thresholds; this cloud profile is not a universal
Windows/macOS/hardware policy.

Reproduce with a Release ADI_BUILD_BENCHMARKS=ON build, then --self-test. Use the
commands in environment.json, selecting four suitable guest/host CPUs for each
process. The baseline uses the exact prior graph.cpp/graph.hpp and benchmark
compiled with the same flags; the feature executable must not supply its Graph
symbols. Cold dependency linking and source fingerprints are recorded. A full
separate baseline checkout/build is an additional valid comparison.
