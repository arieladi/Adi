# gemini — log

Trial seat (22 November 2026, `collab/win.md`): Linux environment (Debian 12, GCC 12.2.0, Clang 14.0.6, CMake 3.25.1, Ninja; no MSVC, macOS, or JUCE GUI build here). One branch per item (`gemini/<topic>`) cut from `main`, logged here on `gemini/log` for `win` to review and land. Only the `gemini` seat writes to this file. Newest entry at the top.

---

## 2026-11-22 — Queue item 9 (DF-E12): root cause found and reproduced for flaky `Store::create` of `Cached.adi`

**For the engine seat (owner: `engine`).** Head: `main` (`abbc32ab`), GCC 12.2.0 Debug, ext4 temp filesystem.

### Reproduction & exact `StoreError`

1. **Control (shared-lease neighbours only)**: 350 repeats of `adi_project_files_tests` (150 via `ctest -R '^adi_project_files_tests$' --repeat until-fail:150 -j 8` in 319.44 s + 200 across 2 concurrent direct workers) beside 2 busy CPU spinners and looping `adi_store_tests`, `adi_history_tests`, `adi_decode_cache_consistency_tests` (`-j 3`): **350 / 350 PASS** (none of those suites take `LOCK_EX` on unlinked files).
2. **Smallest reproducer (exclusive-lease neighbours from parallel `ctest`)**: run `adi_project_files_tests` alongside looping `adi_project_replace_tests` and `adi_settings_tests` (both invoke `replaceFile` -> `ProjectFileLease::openExclusive` -> `LOCK_EX`):
   - Run A (2 workers each of `adi_project_replace_tests` + `adi_settings_tests`, `ctest --repeat until-fail:80 -j 8`): **failed on iteration 47** (`114.22 s`) with `***Exception: SegFault` (hits the unchecked `s->db()` at `adi_daw/tests/test_project_files.cpp:200` when `Store::create(future, error)` returns null).
   - Run B (3 workers each of `adi_project_replace_tests` + `adi_settings_tests`, `adi_project_files_tests` under `strace -f -e trace=flock,openat,unlink,rename,renameat,renameat2`): **failed on iteration 5** with the exact DF-E12 message and `StoreError`:
     - stdout: `FAIL: cached replay fixture (Cached.adi): could not create the database` (`StoreError::CannotCreate`, `adi_daw/tests/test_project_files.cpp:184-185`).
     - `strace`:
       - `openat(AT_FDCWD, ".../checkpoint-copy/Cached.adi", O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC, 0666) = 17`
       - `openat(AT_FDCWD, "/tmp/adi-project-leases-1000", O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_DIRECTORY) = 18`
       - `openat(18, "64528-8781908.lock", O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600) = 19`
       - `flock(19, LOCK_SH|LOCK_NB) = -1 EAGAIN (Resource temporarily unavailable)`

### Root cause (`adi_daw/src/adi/media/publish_file.cpp`, owner: `engine`)

- `openLeaseInode` / `lockNative` (`adi_daw/src/adi/media/publish_file.cpp:237-264`) key POSIX leases solely on `(st_dev, st_ino)` via `/tmp/adi-project-leases-<uid>/<dev>-<ino>.lock` with non-blocking `flock(..., LOCK_NB)`.
- `ProjectFileLease::open` (`publish_file.cpp:438`) queries the target's `(st_dev, st_ino)` via `stat()` (`identityAt`, line 227) and holds **no open file descriptor on the target inode itself** (only on the sidecar `.lock` file).
- In `replaceFile` (`publish_file.cpp:514-601`), `lease = ProjectFileLease::openExclusive(target, error)` acquires `LOCK_EX` on `<dev>-<old_ino>.lock`. At line 540, `renameReplacing(source, target)` unlinks `target`. Because no process holds an open fd on `target`, the kernel frees `old_ino` immediately — while `lease` remains alive on the stack through `publishedResult(target, *sourceId, hooks.syncDirectory)` (`syncFolder` / `fsync`, line 600) until `replaceFile` returns.
- On ext4 (where freed inodes are rapidly recycled across parallel tests in the same temp filesystem — only 164 `.lock` inodes were used across hundreds of runs), a concurrent `Store::create` (`publish_file.cpp:435`, `O_CREAT|O_EXCL`) receives that recycled inode `old_ino`, opens the matching `<dev>-<old_ino>.lock`, and gets `EAGAIN` from `flock(LOCK_SH|LOCK_NB)`. `Store::createEmpty` (`adi_daw/src/adi/store.cpp:362-363`) maps `EAGAIN` to `StoreError::CannotCreate`.

### Secondary findings noticed in the same path (owner: `engine`)

- `adi_daw/src/adi/media/publish_file.cpp:455`: when `lockNative` fails during `create == true`, `ProjectFileLease::open` returns `{}` without unlinking the freshly created 0-byte `value->path` (unlike the `openLeaseInode` failure path at lines 448-451), leaving an empty orphan file at the caller's path.
- `adi_daw/tests/test_project_files.cpp:200`: `auto s = Store::create(future, error); s->db().exec(...)` dereferences `s` without checking for null (causing the SIGSEGV on iteration 47 when the same race hits `future-source`).

---

## 2026-11-22 — Queue item 8 (DF-M62 confirmation build): `mixview/df-m62-float-transform-20261122` (`d22b81c1`)

**For the Mix View seat (and win).** Unblocked by `mixview/df-m62-float-transform-20261122` (`d22b81c1`, `adi_daw/src/adi/mix/limiter/Dsp/FloatRealTransform.h:61`). Built completely **unpatched** (no local DF-M62 workaround) with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`.

| Check on `d22b81c1` (unpatched) | Toolchain | Result | Notes |
|---|---|---|---|
| `adi_mix_limiter` (`DspEngine.cpp`, `BackgroundOwner.cpp`, `MultirateDispatch.cpp`) | GCC 12.2.0 `-DADI_WERROR=ON` | **CLEAN (0 warnings)** | `FloatRealTransform.h:61` `-Werror=sign-compare` gone |
| `adi_mix_limiter` (`DspEngine.cpp`, `BackgroundOwner.cpp`, `MultirateDispatch.cpp`) | Clang 14.0.6 `-DADI_WERROR=ON` | **CLEAN (0 warnings)** | `adi_mix_limiter` builds clean; full tree still hits pre-existing `main` `adi_core` Clang 14 errors (`project_files.cpp:308`, etc.) |
| Full tree (`adi_core`, `adi_settings`, `adi_tool`, `adi_clap_probe`, `adi_plugin_scanner`, `adi_tests_textproj`, all 264 test targets) | GCC 12.2.0 `-DADI_WERROR=ON` | **267 / 268 CLEAN** | Sole failure is unrelated pre-existing `adi_ui_seams_tests` (`CMakeLists.txt` `-fsanitize-trap=signed-integer-overflow` rejected by GCC 12.2, owner `window-fn`/`win`) |
| Limiter, PFFFT, spectrum, Mix View, and DSP suites (21 suites) | GCC 12.2.0 `ctest` | **21 / 21 PASS** (27.10 s) | `adi_mix_limiter_tests`, `adi_mix_limiter_owner_tests`, `adi_spectrum_tests`, `adi_ui_spectrum_slope_tests`, `adi_ui_spectrum_average_tests`, `adi_dsp_tests`, `adi_mix_functions_tests`, `adi_mix_document_tests`, `adi_mix_render_intent_tests`, `adi_mix_pdc_pin_tests`, `adi_mix_eq_tests`, `adi_mix_eq_host_tests`, `adi_mix_eq_pipeline_tests`, `adi_mix_parameter_feed_tests`, `adi_mix_compressor_host_tests`, `adi_mix_carve_tests`, `adi_mix_key_tests`, `adi_mix_focus_tests`, `adi_mix_eq_chain_tests`, `adi_mix_eq_chain_backend_tests`, `adi_mix_compressor_tests` |

DF-M62 is confirmed fixed on GCC 12.2 and Clang 14.0.6 (`d22b81c1`).

---

## 2026-11-22 — Queue item 7 (DF-E10): 500 parallel repeats of `adi_link_session_tests` under CPU load

**For the engine seat.** Head: `main` (`abbc32ab`, `RESOURCE_LOCK` present), GCC 12.2.0 Debug (`-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`).

- **Load applied concurrently**: 4 busy CPU spinner processes on a 2-vCPU VM + continuous background `ctest -j 4` loop over `adi_graph_workers_tests`, `adi_dsp_tests`, `adi_engine_tests`, and `adi_session_tests`.
- **Command 1**: `ctest -R adi_link_session_tests --repeat until-fail:500 -j 8 --output-on-failure`
  - **Result**: **500 / 500 PASS** (0 failures, 43 checks per run), wall time **132.05 s** (~0.26 s/run under load vs 0.145 s idle).
- **Command 2 (RESOURCE_LOCK check under the same load)**: `ctest -R '^(adi_link_session_tests|adi_link_playback_tests)$' --repeat until-fail:100 -j 8 --output-on-failure`
  - **Result**: `adi_link_session_tests` **100 / 100 PASS** (total **600 / 600 PASS**, 0 failures); `adi_link_playback_tests` **7 / 7 PASS** (~31.5 s each, serialized by `RESOURCE_LOCK`, stopped early once `adi_link_session_tests` finished all 100 repeats).

No DF-E10 failure reproduced across 600 loaded repeats on Linux GCC 12.2.

---

## 2026-11-22 — Queue item 6 (RT-TIMING): `adi_builtin_realtime_tests` Debug and Release timings on `devices/perf-pass` (main) and `devices/perf-debug-timeout`

**For the devices lane.** Built with GCC 12.2.0 (`-DADI_ACCEPTANCE=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_WERROR=OFF`), Debug (`-g0`) and Release (`-O3 -DNDEBUG`), measured against the 900 s CTest `TIMEOUT` (`adi_daw/cmake/devices/realtime.cmake:10`). `devices/perf-pass` (`920fac00`) is already merged on `main` (`abbc32ab`).

### Summary table

| Branch / Head | Build | Wall time vs 900 s | Checks | Allocations | Result |
|---|---|---:|---|---:|---|
| `main` (`abbc32ab`, includes `devices/perf-pass` `920fac00`) | Debug | > 240.00 s (capped; 29/40 single-device passes finished, 0% of Part 2 — projects past 900 s) | partial (29/40 devices, 0 failures seen) | 0 | **TIMEOUT RISK** (exceeds practical Debug budget) |
| `main` (`abbc32ab`, includes `devices/perf-pass` `920fac00`) | Release | **188.92 s** | 156 checks, 6 timing failures | 0 | **FAIL** (timing only; well under 900 s) |
| `devices/perf-debug-timeout` (`a4146193`, includes `devices/perf-sampler`) | Debug | **152.80 s** (2 m 32.8 s) | **64 / 64 PASS** (all 21 built-ins incl. 32-slot Sampler + WTS-01 project) | 0 | **PASS** (~5.9× margin under 900 s) |
| `devices/perf-debug-timeout` (`a4146193`, includes `devices/perf-sampler`) | Release | **188.29 s** | 168 checks, 6 timing failures | 0 | **FAIL** (same 6 timing bounds; Sampler & OneShot pass) |

### Key numbers for `devices`

- **`devices/perf-debug-timeout` (`a4146193`) Debug fix confirmed**: finishes in **152.80 s** on Linux GCC 12.2 Debug (all 64 checks pass, 0 allocations across all 21 built-ins including the 32-slot Sampler and WTS-01 contended project).
- **`devices/perf-sampler` improvements confirmed on Linux GCC 12.2 Release (`a4146193`)**:
  - `OneShot` 64 f: p50 **1.76 µs**, p99 **2.98 µs** (0.22% dl) vs `main` p50 7.38 µs / p99 11.48 µs (0.86% dl); 128 f: p50 **3.31 µs**, p99 **4.09 µs** (0.15% dl) vs `main` p50 14.57 µs / p99 22.07 µs (0.83% dl).
  - `Sampler` (32 slots): 64 f p50 **63.73 µs**, p99 **93.81 µs** (7.04% dl, 0 allocs); 128 f p50 **111.63 µs**, p99 **184.22 µs** (6.91% dl, 0 allocs) — well inside the 15% bound.
  - WTS-01 project (Release, `a4146193`): 64 f p50 300.27 µs / p99 337.88 µs (25.34% dl); 128 f p50 553.59 µs / p99 622.78 µs (23.35% dl); contended 64 f / 128 f both PASS within deadline with 0 allocations.
- **Finding (Release timing failures on both `main` `abbc32ab` and `a4146193`, owner: `devices`)**:
  - `Hybrid Reverb`: 64 f p50 333.86 µs / p99 372.50 µs (**27.94%** vs 25% bound); 128 f p50 664.86 µs / p99 713.55 µs (**26.76%** vs 25% bound); 0 allocations; no recurring stall over 50% dl. (Note: `devices/log` standing queue item 10 already addresses Hybrid Reverb under the common bound on later post-3-Dec branches.)
  - `Vocoder` (notable recurring 6× jump between p50 and p99 on Linux GCC 12.2 Release):
    - 64 f: p50 **114.82 µs**, p99 **724.49 µs** (**54.34%** vs 15% bound), max 798.65 µs, recurring **731.54 µs** (> 50% dl = 666.7 µs), 0 allocations.
    - 128 f: p50 **228.71 µs**, p99 **1441.77 µs** (**54.07%** vs 15% bound), max 1589.30 µs, recurring **1457.00 µs** (> 50% dl = 1333.3 µs), 0 allocations.
    - Repro: Release GCC 12.2 `-DADI_ACCEPTANCE=ON`, run `./adi_builtin_realtime_tests` (`adi_daw/tests/test_builtin_realtime.cpp`, `Vocoder` case, parameter 0 swept across blocks).

---

## 2026-11-22 — Queue item 5 (BENCH): Release empty-project callback cost on main and engine/empty-project-cost

**For the engine seat.** Release build (`-DCMAKE_BUILD_TYPE=Release`, GCC 12.2.0 `-O3 -DNDEBUG`, `-DADI_BUILD_BENCHMARKS=ON -DADI_ACCEPTANCE=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`, 48 kHz stereo; `-DADI_WERROR=OFF` because GCC 12 `-O3` emits libstdc++ `-Wrestrict`/`-Wmaybe-uninitialized`/`-Wstringop-overflow` alongside DF-M62 `FloatRealTransform.h:61`). Deadlines: 1333.33 µs at 64 frames, 2666.67 µs at 128 frames.

### 1. `main` (`abbc32ab`)

- `adi_block_benchmark --self-test`: **PASS** (`quantiles, strict deadline and empty-input rejection and timing partition`).
- `adi_block_benchmark` on `main` does not yet carry `--empty-project`; empty factory New on `main` was measured via `adi_builtin_realtime_tests` (`[breakdown] an empty New project`) and the graph/loudness fixtures via `adi_block_benchmark`:

| Fixture (`main` `abbc32ab`) | Frames | Callbacks | p50 (µs) | p50 (% dl) | p99 (µs) | p99 (% dl) | Max / recurring (µs) | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| Empty New project (`adi_builtin_realtime_tests`) | 64 | RT suite | 88.48 | 6.64% | 105.40 | 7.91% | 115.63 (recurring) | 0 allocations |
| Empty New project (`adi_builtin_realtime_tests`) | 128 | RT suite | 175.30 | 6.57% | 214.05 | 8.03% | 205.88 (recurring) | 0 allocations |
| `--loudness` 2 ch | 64 | 4000 | 5.485 | 0.41% | 8.948 | 0.67% | 14.901 (max) | 0 dropouts |
| `--loudness` 2 ch | 128 | 4000 | 10.711 | 0.40% | 16.008 | 0.60% | 25.594 (max) | 0 dropouts |
| `--breakdown` `silence-heavy` (6 proc / 315 skip) | 64 | 1000 | 13.085 | 0.98% | 19.833 | 1.49% | 25.413 (max) | body p50 0.462 µs, resid p50 12.617 µs |
| `--breakdown` `silence-heavy` (6 proc / 315 skip) | 128 | 1000 | 13.230 | 0.50% | 21.130 | 0.79% | 36.843 (max) | body p50 0.525 µs, resid p50 12.699 µs |
| `--breakdown` `active-64` (321 proc / 0 skip) | 64 | 1000 | 50.164 | 3.76% | 74.220 | 5.57% | 722.874 (max) | body p50 17.037 µs, resid p50 33.131 µs |
| `--breakdown` `active-64` (321 proc / 0 skip) | 128 | 1000 | 59.372 | 2.23% | 75.567 | 2.83% | 91.358 (max) | body p50 22.291 µs, resid p50 37.093 µs |

### 2. `engine/empty-project-cost` (`899a3a26`) vs paired stock `main_loudness.cpp` (`ac07be74`)

- `adi_loudness_tests` on `899a3a26`: **PASS — 87 checks, 0 failures**.
- `adi_block_benchmark --self-test` on `899a3a26`: **PASS**.
- `adi_block_benchmark --empty-project --iterations 4000` (32-bit stereo, Pd enabled, 31 nodes: 2.0 processed / 29.0 suspended per block). Medians across 3 runs (max shown as range across the 3 runs):

| Case (`--empty-project`, 4000 cb × 3) | Build | p50 (µs) | p50 (% dl) | p99 (µs) | p99 (% dl) | Max across 3 runs (µs) | Dropouts |
|---|---|---:|---:|---:|---:|---:|---:|
| 64 frames, stopped | Stock loudness (`ac07be74`) | 88.527 | 6.64% | 107.216 | 8.04% | 115.181 – 1778.170 | 1, 0, 0 |
| 64 frames, stopped | `engine/empty-project-cost` (`899a3a26`) | 81.357 | 6.10% | 122.992 | 9.22% | 1147.751 – 2099.248 | 0, 2, 0 |
| 64 frames, playing | Stock loudness (`ac07be74`) | 88.569 | 6.64% | 115.360 | 8.65% | 186.202 – 1245.057 | 0, 0, 0 |
| 64 frames, playing | `engine/empty-project-cost` (`899a3a26`) | 81.332 | 6.10% | 97.421 | 7.31% | 115.765 – 174.605 | 0, 0, 0 |
| 128 frames, stopped | Stock loudness (`ac07be74`) | 176.182 | 6.61% | 220.947 | 8.29% | 261.623 – 1759.139 | 0, 0, 0 |
| 128 frames, stopped | `engine/empty-project-cost` (`899a3a26`) | 160.304 | 6.01% | 182.613 | 6.85% | 249.179 – 258.697 | 0, 0, 0 |
| 128 frames, playing | Stock loudness (`ac07be74`) | 176.306 | 6.61% | 202.158 | 7.58% | 288.919 – 332.438 | 0, 0, 0 |
| 128 frames, playing | `engine/empty-project-cost` (`899a3a26`) | 160.024 | 6.00% | 181.107 | 6.79% | 228.559 – 260.031 | 0, 0, 0 |

Delta on GCC 12.2 `-O3`: `899a3a26` cuts median empty-project callback cost by **~7.2 µs (-8.1%)** at 64 frames and **~16.0 µs (-9.1%)** at 128 frames, matching the standalone 2-channel `--loudness` cost (~5.5 µs at 64 frames, ~10.7 µs at 128 frames) plus Main wrapper overhead. Occasional >1 ms single-block spikes on the first stopped case reflect shared-VM preemption (also present on stock).

---

## 2026-11-22 — Queue item 4 (PD64): full headless ctest at `PD_FLOATSIZE=64` vs `PD_FLOATSIZE=32` on GCC 12.2 and Clang 14.0.6

**Scope:**
- `origin/land/devices-1122` (`a8af403a`) is already merged into `main` (`73f13028`), so `main` (`73f13028`) includes the full `land/devices-1122` stack. Tested `main` (`73f13028`) at both `PD_FLOATSIZE=64` and `PD_FLOATSIZE=32`, plus the latest unmerged devices stack top `devices/tuner` (`eb460dc3`) at `PD_FLOATSIZE=64`.
- Configure check confirmed on both compilers: `-- Pd tier: PD_FLOATSIZE=64 (double-precision t_float and t_sample)`.

**New finding for `devices` (`-DADI_WERROR=ON -DADI_PD_FLOATSIZE_64=ON` on GCC 12.2 and Clang 14.0.6):**
- **`src/adi/pd_builtins/colorcab_tilde.cpp:25`** and **`src/adi/pd_builtins/oneshot_tilde.cpp:33,78,86,88`** (owner: `devices`): implicit narrowing from `t_float` / `t_floatarg` (`double` under `PD_FLOATSIZE=64`) to `float` fails `-Werror=float-conversion` on GCC 12.2 and `-Werror,-Wimplicit-float-conversion` on Clang 14.0.6 (9 diagnostics total):
  - `src/adi/pd_builtins/colorcab_tilde.cpp:25`: `const float id = argc ? atom_getfloat(argv) : 1;`
  - `src/adi/pd_builtins/oneshot_tilde.cpp:33`: `const float slot = argc ? atom_getfloat(argv) : 1;`
  - `src/adi/pd_builtins/oneshot_tilde.cpp:78`: `const float id = atom_getfloat(atoms), value = atom_getfloat(atoms + 1);` (2 diagnostics)
  - `src/adi/pd_builtins/oneshot_tilde.cpp:86`: `x->state->midi.note(key, velocity, channel);` (3 diagnostics)
  - `src/adi/pd_builtins/oneshot_tilde.cpp:88`: `x->state->midi.bend(value, channel);` (2 diagnostics)
- Repro: `cmake -S adi_daw -B build-pd64 -DADI_WERROR=ON -DADI_PD_FLOATSIZE_64=ON -DADI_WITH_JUCE=OFF -DADI_WITH_PD=ON && cmake --build build-pd64 --target adi_pd_builtins`.
- Every other Pd library TU (`libpd_static`, all other `src/adi/pd_builtins/*_tilde.cpp`, all `src/juce/pd_*.cpp`, `src/adi/pd_builtins/native_devices.cpp`, and all `*pd*` test TUs — 150 of 153 Pd objects on Clang 14 and all remaining on GCC 12) compiled with zero warnings under `-DADI_WERROR=ON -DADI_PD_FLOATSIZE_64=ON`.

**Result table (`PD_FLOATSIZE=32` vs `PD_FLOATSIZE=64`):**

| Target / Branch | SHA | Compiler | `PD_FLOATSIZE` | `-DADI_WERROR=ON` | Headless CTest (269 suites, excl. acceptance) | Failures & Owners |
|---|---|---|---|---|---|---|
| `main` (includes `land/devices-1122`) | `73f13028` | GCC 12.2.0 | `32` | PASS (DF-M62 locally patched; excl. `adi_ui_seams_tests`) | **268/268 runnable PASS** (1 build-blocked) | `adi_ui_seams_tests`: `CMakeLists.txt:2084` (`-fsanitize-trap=signed-integer-overflow` unrecognized on GCC 12, owner `window-fn`/`win`) |
| `main` (includes `land/devices-1122`) | `73f13028` | GCC 12.2.0 | `64` | FAIL (`colorcab_tilde.cpp:25`, `oneshot_tilde.cpp:33,78,86,88`) | **268/268 runnable PASS** (`-DADI_WERROR=OFF`; 1 build-blocked) | Compile `-Werror=float-conversion`: `devices`; `adi_ui_seams_tests` flag error: `window-fn`/`win`. **0 runtime failures at 64.** |
| `main` (includes `land/devices-1122`) | `73f13028` | Clang 14.0.6 | `32` | 710/721 objs clean (DF-M62 locally patched) | link blocked by `main` `adi_core` Clang 14 errors | `timeline_clips.hpp:13`, structured-binding captures in `project_files.cpp:308`, `param_ops.cpp`, `midi_clips.cpp:659`, `test_gain_stage.cpp:138` (owner `engine`/`engine-win`) |
| `main` (includes `land/devices-1122`) | `73f13028` | Clang 14.0.6 | `64` | 150/153 Pd objs clean (`colorcab_tilde.cpp:25`, `oneshot_tilde.cpp:33,78,86,88`, DF-M62) | link blocked by `main` `adi_core` Clang 14 errors | Same 9 `-Wimplicit-float-conversion` diagnostics in `colorcab_tilde.cpp` & `oneshot_tilde.cpp` (owner `devices`) + `main` `adi_core` blockers (`engine`/`engine-win`) |
| `devices/tuner` (unmerged devices top) | `eb460dc3` | GCC 12.2.0 | `64` | Same 2 `main` files only (0 new warnings in 10 new devices) | **43/43 Pd + new-device suites PASS** (`-DADI_WERROR=OFF`) | Same 2 `main` files (`colorcab_tilde.cpp`, `oneshot_tilde.cpp`, owner `devices`); **0 runtime failures at 64.** |

---

## 2026-11-22 — Queue item 3 (WTS-01 on Linux): `main`, DF-E53 (`03be0b99`), and DF-E56 (`0b539fb2`)

**What ran:**
- Configured with `-DADI_ACCEPTANCE=ON -DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DCMAKE_BUILD_TYPE=Debug` (DF-M62 `FloatRealTransform.h:61` transiently patched locally and reverted after each run).
- **GCC 12.2.0:** built `adi_acceptance_wts01` cleanly with zero warnings on all three heads and ran `ctest -L acceptance -R wts01 -V` (note: unfiltered `ctest -L acceptance` also matches `adi_builtin_realtime_tests`, which exceeds 10 min in Debug without `devices/perf-debug-timeout`; timed separately in item 6).
- **Clang 14.0.6:** `tests/acceptance/test_session_wts01.cpp` compiles cleanly with zero warnings on all three heads (`main` `73f13028`, `03be0b99`, `0b539fb2`), but linking `adi_acceptance_wts01` under Clang 14.0.6 + libstdc++ 12 is blocked on `main` by the pre-existing `adi_core` Clang 14 errors logged in item 2 (`src/adi/engine/timeline_clips.hpp:13:24` `<ranges>` filter, and structured-binding lambda captures in `src/adi/project_files.cpp:308:47`, `src/adi/engine/param_ops.cpp:339/347/1995/2008`, `src/adi/engine/midi_clips.cpp:659`).

**Result table (GCC 12.2.0 — matches win's 20 November table identically; 2b is the sole FAIL on all three heads):**

| Step | `main` (`73f13028`) | `gemini/df-e53-canonical-param-ref` (`03be0b99`) | `gemini/df-e56-mpe-router-overflow` (`0b539fb2`) | Notes |
|---|---|---|---|---|
| 1 New project | PASS | PASS | PASS | 7 tracks, 120 BPM 4/4, 48 kHz, silence |
| 2a Record 4 bars | PASS (`max\|d\| 0`) | PASS (`max\|d\| 0`) | PASS (`max\|d\| 0`) | 384,000 frames, sample-exact |
| 2b Play the take back | **FAIL** (DF-E64) | **FAIL** (DF-E64) | **FAIL** (DF-E64) | 256/384,000 frames differ (0..255), 256 first-block underruns; `ENGINE` primed control `max\|d\| 0` |
| 3a OneShot on MIDI track | PASS | PASS | PASS | `Insert OneShot`, loaded, slot 1 filled, armed |
| 3b Live keyboard into OneShot | NOT_BUILT (`ENGINE` ok) | NOT_BUILT (`ENGINE` ok) | NOT_BUILT (`ENGINE` ok) | `ENGINE`: onset `+1`, peak `0.1256` (window-fn N60) |
| 3c, 3d VST3 instrument | JUCE-only | JUCE-only | JUCE-only | `adi_acceptance_wts01_vst3` not built with `ADI_WITH_JUCE=OFF` |
| 4 Fader drag + Undo | PASS | PASS | PASS | `-6/-12/-24 dB` live, `+1` op on release, Undo back to `0 dB` (`x1.0000`) |
| 5a Returns with two sends | PASS | PASS | PASS | `max\|d\| 0` from block 8 (`x2.000000`), `-12 dB` return `max\|d\| 5.96e-08` (`x1.251189`) |
| 5b Return inside a group | NOT_BUILT (`ENGINE` ok) | NOT_BUILT (`ENGINE` ok) | NOT_BUILT (`ENGINE` ok) | Post-`#677` `ENGINE`: `x0.3143` (window grouping rule `window/return-in-group`) |
| 6a Volume automation | PASS (`max\|d\| 0.0309 dB`) | PASS (`max\|d\| 0.0309 dB`) | PASS (`max\|d\| 0.0309 dB`) | Within 0.1 dB across 3 points |
| 6b Built-in param automation | NOT_BUILT (`ENGINE` ok) | NOT_BUILT (`ENGINE` ok) | NOT_BUILT (`ENGINE` ok) | `ENGINE`: Utility `x1.995262` -> `x1.000000`, `max\|d\| 2.2e-08` (window-fn DF-W37) |
| 7a Save & reopen rows | PASS | PASS | PASS | Reopened rows equal (`tracks 7, strips 7, devices 3, routing 8, lanes 1, clips 2`) |
| 7b Reopen playback match | PASS (`max\|d\| 0`) | PASS (`max\|d\| 0`) | PASS (`max\|d\| 0`) | Over 409,600 frames |
| 7c Export WAV | PASS (`max\|d\| 0`) | PASS (`max\|d\| 0`) | PASS (`max\|d\| 0`) | 768,000 frames, A==B bit-exact, matches offline render & live from block 8 |
| 7d, 7e Export with VST3 | JUCE-only | JUCE-only | JUCE-only | `adi_acceptance_wts01_vst3` not built with `ADI_WITH_JUCE=OFF` |
| **Summary** | **1 FAIL (2b only)** | **1 FAIL (2b only)** | **1 FAIL (2b only)** | Zero regressions on DF-E53 or DF-E56 |

---

## 2026-11-22 — Queue item 2 (PR-BUILDS, Round 1): all unmerged PR-requested heads on GCC 12.2 and Clang 14.0.6 `-DADI_WERROR=ON`

**Scope and environment:**
- Collected every unmerged `2026-11-21`+ `PR requested` branch across all 7 seat logs (`engine/log`, `engine-win/log`, `devices/log`, `window/log`, `window-fn/log`, `mixview/log`, `clones/log`) plus the sole unmerged `land/*` stack (`land/engine-win-1122b`): **46 stack tops / standalone branches** covering all **83** unmerged `2026-11-21`+ PR-requested heads (`engine/df-e23-followups-staged` is noted superseded by `#677` on `main`, per `engine.md`; `mixview/review-scope-lands-after-2026-12-03` shares exact SHA `ffeebdcb` with `mixview/df-m55-info-spectrum-scale-lands-after-2026-12-03`).
- Built with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DCMAKE_BUILD_TYPE=Debug` on GCC 12.2.0 and Clang 14.0.6 (libstdc++ 12).
- **Local DF-M62 patch:** transiently patched `src/adi/mix/limiter/Dsp/FloatRealTransform.h:61` ONLY on branches that do not carry `mixview/df-m62-float-transform-20261122` (`d22b81c1`); **all 3 `mixview/*` branches were built completely unpatched** and compiled `FloatRealTransform.h` cleanly.
- **Pre-existing `main` (`73f13028`) older-toolchain blockers (reported here for `win` / owners; separated from new branch errors below):**
  1. **GCC 12.2 (`CMakeLists.txt:2083-2084`, owner `window-fn` / `win`)**: `adi_ui_seams_tests` passes `-fsanitize-trap=signed-integer-overflow` under `if(NOT MSVC)`, which GCC 12.2 rejects (`unrecognized command-line option`; GCC added `-fsanitize-trap=` in GCC 13; GCC 12 uses `-fsanitize-undefined-trap-on-error`). Repro: `cmake -S adi_daw -B build -DCMAKE_CXX_COMPILER=g++ && cmake --build build --target adi_ui_seams_tests`.
  2. **Clang 14.0.6 + libstdc++ 12 (`src/adi/engine/timeline_clips.hpp:13:24`, owner `engine` / `engine-win`)**: `model.clips | std::views::filter(...)` fails Clang 14 concept check against libstdc++ 12 (`bits/ranges_util.h:133,139`), blocking 8 TUs (`freeze_capture.cpp`, `freeze_commands.cpp`, `gain_stage.cpp`, `clip_playback.cpp`, `midi_clips.cpp`, `midi_controllers.cpp`, `automation_clips.cpp`, `tests/test_take_lane_ops.cpp`).
  3. **Clang 14.0.6 C++20 DR2313/P1091 structured-binding lambda capture (owner `engine`)**: unsupported before Clang 16 in `src/adi/project_files.cpp:308:47` (`hash`), `src/adi/engine/param_ops.cpp:339:133` (`role`), `:347:142` (`id`), `:1995:146` (`id`), `:2008:146` (`id`), `src/adi/engine/midi_clips.cpp:659:111,137` (`source`), and `tests/test_gain_stage.cpp:138:38` (`track`). Because (2) and (3) block linking `libadi_core.a` under Clang 14 unmodified, the Clang 14 column compiles every headless `.cpp` object touched/added by each branch with `-DADI_WERROR=ON`.

**New pre-landing findings caught in Round 1 (before win's landing windows):**
- **`clones/shaper-trim` (`69422cef`, owner `clones`) — FAILS GCC 12.2 and Clang 14.0.6 `-DADI_WERROR=ON`**: 19 `-Werror=sign-conversion` errors in `src/adi/dsp/adi_shaper_wave.hpp` (lines 48, 121, 136, 138, 145, 148, 149, 150, 151, ...: `int` index into `std::array<..., N>`). Repro: checkout `69422cef`, configure with `-DADI_WERROR=ON`, build `adi_core`.
- **`mixview/review-cumulative-20261122` (`cafd7fde`) and `mixview/df-m55-info-spectrum-scale-lands-after-2026-12-03` (`ffeebdcb`, owner `mixview`) — FAIL GCC 12.2 and Clang 14.0.6 `-DADI_WERROR=ON`** (note: DF-M62 `FloatRealTransform.h:61` IS fixed and clean here):
  - `tests/test_shared_analysis.cpp:1249:79` (`cafd7fde`) / `:1773:79` (`ffeebdcb`): GCC 12 hard error `local variable 'revision' may not appear in this context`.
  - `tests/test_shared_analysis.cpp:1326:21` (`cafd7fde`) / `:1850:21` (`ffeebdcb`): GCC 12 + Clang 14 `-Werror=range-loop-construct` (`loop variable 'pair' creates a copy from type 'const std::pair<double, int>'`).
  - `src/adi/ui/shared_analysis.cpp:904:81` (`cafd7fde`) / `:1260:81` (`ffeebdcb`): Clang 14 `-Werror=float-conversion` (`implicit conversion turns floating-point number into integer: 'double' to 'bool'`).
  - `src/adi/ui/window_state.cpp:25:29`: Clang 14 hard error `no matching member function for call to 'emplace'` (C++20 parenthesized aggregate init).
- **`engine/n67-auto-monitor-default` (`f2802a00`, owner `engine`) — FAILS `adi_textproj_store_tests` (2 of 245 checks failed)**: `FAIL the whole projection of a real project, byte for byte` and `FAIL the agent's remark projected, byte for byte` (golden projections in `tests/test_textproj_store.cpp` still expect default monitor `off` instead of `auto`). Repro: checkout `f2802a00`, build & run `adi_textproj_store_tests`.

**Result table (Round 1):**

| Branch | SHA | GCC 12.2 `-Werror` | Clang 14.0.6 `-Werror` (changed objs) | Warnings / Errors (`file:line`) | CTest (GCC 12.2) | Owner |
|---|---|---|---|---|---|---|
| `land/engine-win-1122b` | `a031b8e7` | PASS-BUILD (DF-M62 patched) | PASS (17 obj) | none | 8/8 unit PASS (WTS-01 2b known DF-E64) | win/engine-win |
| `engine/dev06-rack-macros` | `0a2e3eb9` | PASS-BUILD (DF-M62 patched) | PASS (39 obj) | none | 18/18 PASS | engine |
| `engine/dev18-event-transforms` | `4e749553` | PASS-BUILD (DF-M62 patched) | PREEXISTING-ONLY (17 obj) | preexisting Clang 14 `main` blockers only | 9/9 PASS | engine |
| `engine/dev25-plugin-readiness` | `d8995f97` | PASS-BUILD (DF-M62 patched) | PASS (4 obj) | none | 2/2 PASS | engine |
| `engine/df-e10-link-stop-diagnostic` | `b29455f4` | PASS-BUILD (DF-M62 patched) | PASS (1 obj) | none | 1/1 PASS | engine |
| `engine/df-e17-edit-invariants-current` | `ab14b5dd` | PASS-BUILD (DF-M62 patched) | PASS (6 obj) | none | 4/4 PASS | engine |
| `engine/df-e24-vst3-aux-sinks` | `2705f6d2` | NO-HEADLESS-TGT | NO-HEADLESS-OBJ | none | 0 headless tests matched | engine |
| `engine/df-e27-finite-open-payloads` | `d0cf6d3c` | PASS-BUILD (DF-M62 patched) | PASS (2 obj) | none | 1/1 PASS | engine |
| `engine/df-e27-host-telemetry` | `0d14bd14` | PASS-BUILD (DF-M62 patched) | PASS (2 obj) | none | 4/4 PASS | engine |
| `engine/df-e55-vca-device-insert` | `b157aa00` | PASS-BUILD (DF-M62 patched) | PASS (5 obj) | none | 2/2 PASS | engine |
| `engine/empty-project-cost` | `899a3a26` | PASS-BUILD (DF-M62 patched) | PASS (2 obj: main_loudness + test_main_loudness) | none | 1/1 PASS (adi_main_loudness_tests 57/57; adi_builtin_realtime_tests timed out in Debug without perf-debug-timeout) | engine |
| `engine/mi08-expression-playback-current` | `e105cf3e` | PASS-BUILD (DF-M62 patched) | PREEXISTING-ONLY (15 obj) | preexisting Clang 14 `main` blockers only | 10/10 PASS | engine |
| `engine/n63-ordered-note-batch` | `c97f2a96` | PASS-BUILD (DF-M62 patched) | PASS (4 obj) | none | 2/2 PASS | engine |
| `engine/n64-recording-snapshot` | `8a32d9e3` | PASS-BUILD (DF-M62 patched) | PASS (8 obj) | none | 6/6 PASS | engine |
| `engine/n67-auto-monitor-default` | `f2802a00` | PASS-BUILD (DF-M62 patched) | PASS (5 obj) | none | **5/6 unit FAIL** (`adi_textproj_store_tests` 2/245 fail; + WTS-01 2b DF-E64) | engine |
| `engine/param-replacement-receipt` | `4a1b47e7` | PASS-BUILD (DF-M62 patched) | PREEXISTING-ONLY (2 obj) | preexisting Clang 14 `main` blockers only | 2/2 PASS | engine |
| `engine/pd03-snapshot-all` | `67515e1c` | PASS-BUILD (DF-M62 patched) | PREEXISTING-ONLY (param_ops.cpp structured bindings) | preexisting Clang 14 `main` blockers only | 1/1 PASS | engine |
| `engine-win/export-host-owner` | `6b78d94c` | PASS-BUILD (DF-M62 patched) | PASS (5 obj) | none | 1/1 unit PASS (WTS-01 2b known DF-E64) | engine-win |
| `engine-win/play-first-block` | `01b1e723` | PASS-BUILD (DF-M62 patched) | PREEXISTING-ONLY (clip_playback/timeline_clips) | preexisting Clang 14 `main` blockers only | 6/6 PASS | engine-win |
| `engine-win/project-plugin-saved-path` | `43787b7b` | NO-HEADLESS-TGT | NO-HEADLESS-OBJ | none | 0 headless tests matched | engine-win |
| `engine-win/recording-failed-prefix` | `0900deeb` | PASS-BUILD (DF-M62 patched) | PASS (5 obj) | none | 3/3 PASS | engine-win |
| `engine-win/recording-placement-offset` | `01abcaa3` | PASS-BUILD (DF-M62 patched) | PASS (2 obj) | none | 1/1 PASS | engine-win |
| `engine-win/scanner-cache-paths` | `1d6dd030` | PASS-BUILD (DF-M62 patched) | PASS (7 obj) | none | 4/4 PASS | engine-win |
| `devices/beat-repeat-contract` | `e243ab13` | DOCS-ONLY | DOCS-ONLY | none | - | devices |
| `devices/hybrid-reverb-nupc` | `fb1c7e9a` | PASS-BUILD (DF-M62 patched) | PASS (29 obj) | none | 30/30 PASS | devices |
| `devices/perf-debug-timeout` | `a4146193` | PASS-BUILD (DF-M62 patched) | PASS (6 obj) | none | 4/4 PASS | devices |
| `devices/tuner` | `eb460dc3` | PASS-BUILD (DF-M62 patched) | PASS (38 obj) | none | 15/15 PASS | devices |
| `window/browser-devices-test` | `88649293` | NO-HEADLESS-TGT | NO-HEADLESS-OBJ | none | 0 headless tests matched | window |
| `window/grid-ticks` | `199b6c3a` | PASS-BUILD (seams flag err) (DF-M62 patched) | PASS (3 obj) | `CMakeLists.txt:2084` (preexisting GCC 12 `-fsanitize-trap=signed-integer-overflow` on `adi_ui_seams_tests`) | 1/1 PASS | window |
| `window/held-peak` | `80e9557c` | PASS-BUILD (DF-M62 patched) | PASS (1 obj) | none | 1/1 PASS | window |
| `window/input-clock` | `aaaea9db` | NO-HEADLESS-TGT | NO-HEADLESS-OBJ | none | 0 headless tests matched | window |
| `window/menu-parameters` | `ab91e9f7` | NO-HEADLESS-TGT | NO-HEADLESS-OBJ | none | 0 headless tests matched | window |
| `window/return-in-group` | `b882c743` | PASS-BUILD (seams flag err) (DF-M62 patched) | PASS (1 obj) | `CMakeLists.txt:2084` (preexisting GCC 12 `-fsanitize-trap=signed-integer-overflow` on `adi_ui_seams_tests`) | 0 headless tests matched | window |
| `window/undo-settles` | `01ef073b` | PASS-BUILD (DF-M62 patched) | PASS (2 obj) | none | 1/1 PASS | window |
| `window-fn/cueing` | `c6a557e6` | PASS-BUILD (DF-M62 patched) | NO-HEADLESS-OBJ | none | 0 headless tests matched | window-fn |
| `window-fn/delay-compensation` | `ea67a671` | PASS-BUILD (DF-M62 patched) | PASS (1 obj) | none | 0 headless tests matched | window-fn |
| `window-fn/lane-chooser` | `305e9b3a` | PASS-BUILD (DF-M62 patched) | PASS (1 obj) | none | WTS-01 only: 2b known DF-E64 | window-fn |
| `window-fn/lane-ref-canonical` | `996d5b1f` | PASS-BUILD (seams flag err) (DF-M62 patched) | PASS (1 obj) | `CMakeLists.txt:2084` (preexisting GCC 12 `-fsanitize-trap=signed-integer-overflow` on `adi_ui_seams_tests`) | 0 headless tests matched | window-fn |
| `window-fn/midi-live` | `fe24a5b0` | PASS-BUILD (DF-M62 patched) | PASS (1 obj) | none | WTS-01 only: 2b known DF-E64 | window-fn |
| `window-fn/monitor-auto` | `5ac33b36` | PASS-BUILD (seams flag err) (DF-M62 patched) | PASS (3 obj) | `CMakeLists.txt:2084` (preexisting GCC 12 `-fsanitize-trap=signed-integer-overflow` on `adi_ui_seams_tests`) | WTS-01 only: 2b known DF-E64 | window-fn |
| `window-fn/split-pan` | `1d8030a5` | NO-HEADLESS-TGT | NO-HEADLESS-OBJ | none | 0 headless tests matched | window-fn |
| `mixview/df-m55-info-spectrum-scale-lands-after-2026-12-03` | `ffeebdcb` | FAIL-BUILD (unpatched) | FAIL (3 new err) | `tests/test_shared_analysis.cpp:1773,1850`; `src/adi/ui/shared_analysis.cpp:1260`; `src/adi/ui/window_state.cpp:25` | - | mixview |
| `mixview/df-m62-float-transform-20261122` | `d22b81c1` | PASS-BUILD (unpatched) | NO-HEADLESS-OBJ | none | 0 headless tests matched | mixview |
| `mixview/review-cumulative-20261122` | `cafd7fde` | FAIL-BUILD (unpatched) | FAIL (3 new err) | `tests/test_shared_analysis.cpp:1249,1326`; `src/adi/ui/shared_analysis.cpp:904`; `src/adi/ui/window_state.cpp:25` | - | mixview |
| `clones/shaper-trim` | `69422cef` | FAIL-BUILD (DF-M62 patched) | FAIL (19 new err) | `src/adi/dsp/adi_shaper_wave.hpp:48,121,136,138,145,148-151,...` (`-Werror=sign-conversion`, 19 errs) | - | clones |
| `clones/two-head-reader` | `9b7cd7ca` | PASS-BUILD (DF-M62 patched) | PASS (1 obj) | none | 7/7 PASS | clones |
| `engine/df-e23-followups-staged` | `72dfdc77` | skipped (superseded by `#677` on `main`, per `engine.md`) | — | — | — | engine |

---

## 2026-11-22 — Queue item 1 (DF-E56-ATTACK): refused MpeMidi attack leaves sounding voice untouched

**Changed:**
- `adi_daw/src/adi/engine/mpe_output.hpp`: added `MpeOutList::capacity()`, `MpeOutList::remaining()`, and `MpeOutList::drop(std::int32_t)` (non-throwing, bounds-safe for null storage or negative capacity).
- `adi_daw/src/adi/engine/mpe_output.cpp`: in `MpeRouter::noteOn` (`ExpressionRoute::MpeMidi`), check `if (out.remaining() < 4) { out.drop(4); return; }` before calling `allocate()` or emitting any reset control, so a refused attack never emits orphan pitch-bend/timbre/pressure resets onto a shared member channel while preserving total dropped-message accounting.
- `adi_daw/tests/test_mpe_output.cpp`: reproduced the engine follow-up review's exact refused-attack probe (single member channel, first note bent +12 semitones to word `10240`, second note into 1, 2, or 3 free slots emits zero resets with `dropped == 4` and leaves `liveNotes`, `channelOf`, and `sharedChannels` unchanged; retrying with 4 free slots succeeds), and updated the dense bounded-list block test (`Out(16)`) to verify zero orphan resets.

**What ran and results:**
- Pre-fix verification against `da6d1629`: failed 5 of 130 checks in `adi_mpe_output_tests` (all 3 refused-attack slot counts and the 2 no-orphan-reset bounded-list assertions).
- GCC 12.2.0 full CMake + Ninja Debug (`-DADI_WERROR=ON -D_GLIBCXX_ASSERTIONS`, with DF-M62 `FloatRealTransform.h:61` transiently patched locally and reverted before commit): `adi_core` + `adi_mpe_output_tests` built with zero warnings; `adi_mpe_output_tests` PASS — 130 checks, 0 failures. Related suites (`adi_mpe_output_tests`, `adi_mpe_tests`, `adi_pd_engine_tests`, `adi_delay_compensation_tests`, `adi_midi_controllers_tests`, `adi_midi_capture_input_tests`): 6/6 PASS.
- Clang 14.0.6 (`-std=c++20 -D_GLIBCXX_ASSERTIONS -Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wno-unused-parameter -Werror`):
  - Standalone compile, link, and run of `mpe_output.cpp` + `mpe_input.cpp` + `test_mpe_output.cpp`: zero warnings, PASS — 130 checks, 0 failures.
  - Note on full CMake `adi_core` under Clang 14.0.6 (older-toolchain finding for `engine`): `adi_daw/src/adi/project_files.cpp:308:47` fails to compile under Clang 14 (`error: reference to local binding 'hash' declared in enclosing function 'adi::Store::collectAllAndSave'`, C++20 lambda capture of structured binding `for (const auto& [hash, rows] : candidates)` at line 298, unsupported in Clang < 16). Owner: `engine` (`src/adi/project_files.cpp`).
- Checked new C++ against all five patterns in `collab/win.md` (20 November) and MSVC `/W4 /WX` rules; `git diff --check` clean.

PR requested: gemini/df-e56-mpe-router-overflow at 0b539fb2b5c98bdec2b3cf6f149dd6945c577470

---

## 2026-11-22 — DF-E56 review fixes: immediate switchTo and pending NoteOff queue

**Changed:**
- `adi_daw/src/adi/engine/mpe_output.hpp`, `adi_daw/src/adi/engine/mpe_output.cpp` (lent from `engine`; `clap_host.cpp` and `vst3_host.cpp` untouched):
  - Added a fixed-size, allocation-free `pendingOffs_` queue (`kMaxNotes` slots, `pendingNoteOffs()` accessor) with `enqueuePendingOff`, `drainPendingOffs`, and `resetLiveState` (public `reset()` clears both live state and pending NoteOffs).
  - `MpeRouter::switchTo`: drains any pending NoteOffs first, pushes a NoteOff for each live note (queuing any that fail `out.push` into `pendingOffs_`), and switches `route_` / calls `resetLiveState()` immediately so single-shot callers (`vst3_host`, `clap_host`) never stay stranded on the old route.
  - `MpeRouter::noteOff`: if `out.push` fails, queues the NoteOff (channel, key, id, clamped velocity) into `pendingOffs_` and releases the slot in `notes_` so 128 dropped NoteOffs never fill the note table.
  - `MpeRouter::route`: calls `drainPendingOffs(segmentStart, out)` first, before MCM (`if (pendingCount_ == 0 && mcmPending_)`) and before any new block events, compacting remaining entries on partial drain.
- `adi_daw/tests/test_mpe_output.cpp`:
  - Added tests for: dropped `NoteOff` delivered on the next block with empty input on its original channel without duplication; 128 dropped `NoteOff`s releasing all slots and draining before 128 fresh notes (`tableFull() == 0`); the reviewer's 120-note / 104-slot partial `switchTo` probe switching immediately and finishing all 16 remaining NoteOffs + MCM + new MPE note within two blocks; partial `MpeMidi -> Plain` switch preserving original member channels across blocks; `reset()` clearing pending NoteOffs; and dense bounded-list block draining.

**What ran and results:**
- Pre-fix verification against previous branch tip `c798e2a4`: 13 of 126 checks failed across the new pending-NoteOff and immediate-switch cases.
- CMake + Ninja Debug build (`-D_GLIBCXX_ASSERTIONS`, GCC 12.2.0):
  - `adi_mpe_output_tests`: PASS — 126 checks, 0 failures.
  - Related suites (`ctest --parallel 2`): 6/6 PASS (`adi_mpe_output_tests`, `adi_mpe_tests`, `adi_pd_engine_tests`, `adi_delay_compensation_tests`, `adi_midi_controllers_tests`, `adi_midi_capture_input_tests`).
- Strict `-Werror` check: `src/adi/engine/mpe_output.cpp`, `tests/test_mpe_output.cpp`, and host callers `src/juce/clap_host.cpp`, `src/juce/builtin_host.cpp`, `src/juce/pd_engine.cpp` all compiled cleanly under `-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wno-unused-parameter -Werror -D_GLIBCXX_ASSERTIONS` on both `g++` 12.2.0 and `clang++` 14.0.6.
- Checked new C++ against all five patterns in `collab/win.md` (20 November) and MSVC `/W4 /WX` rules; `git diff --check` clean.
- Could not run here: MSVC `/W4 /WX`, macOS/Apple Clang, and full-tree `-DADI_WERROR=ON` under libstdc++ 12 (DF-M62).

PR requested: gemini/df-e56-mpe-router-overflow at da6d1629c820fb077260dcd6d619cf0106ac7080

---

## 2026-11-22 — DF-E53 review fixes: replay exemption, Warning severity, SPEC §6.3.3, shared parser

**Changed:**
- `adi_daw/src/adi/device_param_ref.hpp` (new shared header-only parser in `namespace adi`):
  - Added `parseDeviceParamRef(std::string_view, std::uint32_t&, bool* canonical = nullptr)` and `isCanonicalDeviceParamRef(std::string_view)` with per-digit `UINT32_MAX` overflow guard, replacing the three separate copies in `automation_lanes.cpp`, `check.cpp`, and `param_automation.cpp` (also ready for `DF-W38`).
- `adi_daw/src/adi/automation_lanes.cpp`:
  - Skipped the canonical device `param_ref` refusal when `c.replay` is true (matching `marker_ops.cpp` / `ops.hpp`) so Redo and branch switches never strand legacy logged `createLane` ops.
- `adi_daw/src/adi/check.cpp`:
  - Changed `checkAutomationLanes` findings from `Severity::Error` to `Severity::Warning` so Collect and Export is never blocked on playable legacy files; reports `"played as parameter N"` for numeric non-canonical refs and `"not played"` otherwise.
- `adi_daw/src/adi/engine/param_automation.cpp`:
  - Switched `bindDeviceAutomation` to the shared `parseDeviceParamRef`.
- `adi_daw/docs/format/SPEC.md` (§6.3.3):
  - Documented the canonical unsigned decimal rule for device lane `param_ref` (digits only, no sign, no leading zero, `"0"` allowed, at most `4294967295`; writers MUST write canonical form; readers play numeric non-canonical refs as their numeric value and report them).
- Tests:
  - `adi_daw/tests/test_automation_lanes.cpp`: added redo replay test rewriting a logged `automation.createLane` payload to `"001"`, undoing, and verifying `history.redo().ok` restores `"001"`.
  - `adi_daw/tests/test_check.cpp`: updated `testDeviceAutomationParamRef` to assert `Severity::Warning`, `rep.clean() && rep.warnings > 0`, `"played as parameter N"` for numeric non-canonical refs (`"001"`, `"00"`, `"07"`, `"00000000001"`), and `"not played"` for non-numeric/out-of-range refs.
  - `adi_daw/tests/test_param_automation.cpp`: added binding check for `"00000000001"` under canonical `"1"`.

**What ran and results:**
- Pre-fix verification against previous branch tip `8cde0b32`: failed 1/56 in `adi_automation_lanes_tests` (redo of legacy `"001"`), 10/74 in `adi_check_tests` (Warning + detail), and 2/152 in `adi_param_automation_tests` (11-digit leading-zero numeric ref).
- CMake + Ninja Debug build (`-D_GLIBCXX_ASSERTIONS`, GCC 12.2.0):
  - `adi_automation_lanes_tests`: PASS — 56 checks, 0 failures.
  - `adi_check_tests`: PASS — 74 checks, 0 failures.
  - `adi_param_automation_tests`: PASS — 152 checks, 0 failures.
  - `adi_collect_export_tests`: PASS.
  - All 4 Python validators (`validate_ci.py`, `validate_ops.py`, `validate_pd.py`, `validate_schema.py`) and `.github/scripts/check_spec_layout.py`: PASS.
- Strict `-Werror` check: all 6 modified `.cpp` files (including `device_param_ref.hpp`) compiled cleanly under `-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wno-unused-parameter -Werror -D_GLIBCXX_ASSERTIONS` on both `g++` 12.2.0 and `clang++` 14.0.6.
- Checked new C++ against all five patterns in `collab/win.md` (20 November) and MSVC `/W4 /WX` rules; `git diff --check` clean.
- Could not run here: MSVC `/W4 /WX`, macOS/Apple Clang, and full-tree `-DADI_WERROR=ON` under libstdc++ 12 (DF-M62).

PR requested: gemini/df-e53-canonical-param-ref at 03be0b99a848966e1211fe81ea72c08bf0e4ba00

---

## 2026-11-22 — DF-E56: keep MpeRouter state consistent when its output list is full

**Changed:**
- `adi_daw/src/adi/engine/mpe_output.cpp` (lent from `engine`; `mpe_output.hpp` untouched):
  - `MpeRouter::switchTo`: release each sounding note (`active_`, `lastOff_`, `clock_`, `used`) only if its `NoteOff` push succeeds, and switch `route_` / `reset()` only once every sounding note has been ended. If `out` fills partway through, already-ended notes are not duplicated and the remaining notes stay tracked on the old route so a later call completes the switch.
  - `MpeRouter::route`: clear `mcmPending_` only when `sendMcm` drops nothing (`out.dropped()` unchanged across the 3 RPN pushes), so a partial or zero-capacity list retries the complete 3-message MCM on the next block.
  - `MpeRouter::allocate` / `MpeRouter::noteOn`: move `shared_` and `collisions_` increments after `out.push(o)` succeeds, and return before mutating `active_`, `lastOn_`, `clock_`, or `slot` when `NoteOn` is refused.
  - `MpeRouter::noteOff`: return before decrementing `active_`, updating `lastOff_`/`clock_`, or clearing `nt->used` when `NoteOff` is refused.
- `adi_daw/tests/test_mpe_output.cpp`:
  - Added `testFullOutputListPreservesState` covering zero/partial MCM overflow + retry, combined MCM + note-on overflow (`Out(2)` preserving existing `size == 2 && dropped == 5`), `MpeMidi` note-on overflow after resets fit (`sharedChannels` unchanged), `Plain` same-key note-on overflow (`keyCollisions` unchanged), `NoteExpression` note-on overflow, `NoteOff` overflow + retry on original channel, zero/partial `switchTo` overflow + completion without duplicate `NoteOff`s, and a dense bounded-list block invariant check.

**What ran and results:**
- Pre-fix verification: running the new `adi_mpe_output_tests` against unmodified `main` (`0ea10b8e`) failed 16 of 118 checks across all overflow cases.
- CMake + Ninja Debug build (`-D_GLIBCXX_ASSERTIONS`, GCC 12.2.0):
  - `adi_mpe_output_tests`: PASS — 118 checks, 0 failures.
  - Related suites (`ctest --parallel 2`): 6/6 PASS (`adi_mpe_output_tests`, `adi_mpe_tests`, `adi_pd_engine_tests`, `adi_delay_compensation_tests`, `adi_midi_controllers_tests`, `adi_midi_capture_input_tests`).
- Strict `-Werror` check: both modified translation units (`src/adi/engine/mpe_output.cpp`, `tests/test_mpe_output.cpp`) compiled cleanly with `-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wno-unused-parameter -Werror -D_GLIBCXX_ASSERTIONS` under both `g++` 12.2.0 and `clang++` 14.0.6.
- Checked new C++ against all five patterns in `collab/win.md` (20 November) and MSVC `/W4 /WX` rules; `git diff --check` clean.
- Could not run here: MSVC `/W4 /WX` (Windows), macOS/Apple Clang, and full-tree `-DADI_WERROR=ON` under Debian 12's `libstdc++` 12 (which returns `size_t` from `std::bit_width`, failing `-Werror=sign-compare` in unrelated `src/adi/mix/limiter/Dsp/FloatRealTransform.h:61`).

PR requested: gemini/df-e56-mpe-router-overflow at c798e2a47f8e070b111a088ecb2818289da7f462

---

## 2026-11-22 — DF-E53: enforce canonical device param_ref and key laneFor on parsed id

**Changed:**
- `adi_daw/src/adi/automation_lanes.cpp` (engine-owned, per trial brief to refuse in the op):
  - Added `isCanonicalDeviceParamRef` (accepts `"0"` or `[1-9][0-9]*` with value `<= UINT32_MAX`; refuses leading zeros such as `"00"`/`"001"`, signs, spaces, overflow, and non-decimal strings).
  - Enforced canonical device `param_ref` (and string type check on `owner_kind`) in `validateRow` for `automation.createLane` when `owner_kind == "device"`. Left `restore` untouched so undo of deleted legacy rows still preserves stored scalars.
- `adi_daw/src/adi/check.cpp` (engine-owned, per trial brief to refuse in the project check):
  - Added `checkAutomationLanes` to `checkProject`: reports `Severity::Error` with code `"automation.paramRef"` at `automation_lanes#<id>.param_ref` for any `owner_kind = 'device'` row whose `param_ref` is not a canonical unsigned 32-bit decimal id.
- `adi_daw/src/adi/engine/param_automation.cpp` (lent from `engine`):
  - In `bindDeviceAutomation`, keyed `out->laneFor` on `std::make_pair(lane.ownerId, std::to_string(paramId))` and used the canonical decimal id in the duplicate-lane diagnostic. Kept `laneFor`'s declared type as `std::map<std::pair<std::int64_t, std::string>, std::int64_t>` so `param_automation.hpp`, `session.cpp`, and `mixview`'s `src/adi/ui/parameter_feed.cpp` needed no edits while overrides looked up by canonical id now match legacy `"001"` lanes and `"001"` + `"1"` collide as duplicates.
- Tests:
  - `adi_daw/tests/test_automation_lanes.cpp`: device `createLane` accepts `"0"`, `"1"`, `"4294967295"`, refuses `"00"`, `"01"`, `"001"`, `"+1"`, `"-1"`, `" 1"`, `"1 "`, `"4294967296"`, `"cutoff"` (each with distinct lane id), and delete/undo preserves a legacy `"001"` device lane.
  - `adi_daw/tests/test_check.cpp`: `testDeviceAutomationParamRef` verifies canonical device refs and non-device lanes check clean, and raw-SQL non-canonical/invalid device refs produce `automation.paramRef` errors.
  - `adi_daw/tests/test_param_automation.cpp`: unit binding checks for `"001"` keyed under `"1"` and `"001"`/`"1"` duplicate suppression in both orders, plus end-to-end `Session` test (`testNonCanonicalLegacyOverrideAndDuplicate`) verifying stored-param refresh override, `overrideDeviceAutomation(1, "1")`, and duplicate suppression.

**What ran and results:**
- Pre-fix verification: running the new tests against unmodified `main` (`0ea10b8e`) failed 9/52 in `adi_automation_lanes_tests`, 9/73 in `adi_check_tests`, and 13/150 in `adi_param_automation_tests` (31 failures total).
- CMake + Ninja Debug build (`-D_GLIBCXX_ASSERTIONS`, GCC 12.2.0):
  - `adi_automation_lanes_tests`: PASS — 52 checks, 0 failures.
  - `adi_check_tests`: PASS — 73 checks, 0 failures.
  - `adi_param_automation_tests`: PASS — 150 checks, 0 failures.
  - Related suites (`ctest --parallel 2`): 22/22 PASS (`adi_automation_lanes_tests`, `adi_automation_snapshot_tests`, `adi_collect_export_tests`, `adi_edit_plan_tests`, `adi_ui_document_tests`, `adi_history_compaction_tests`, `adi_history_stored_tests`, `adi_automation_edits_tests`, `adi_automation_range_undo_tests`, `adi_automation_shapes_tests`, `adi_automation_recording_tests`, `adi_automation_write_policy_tests`, `adi_send_automation_tests`, `adi_project_bootstrap_tests`, `adi_track_delete_tests`, `adi_check_tests`, `adi_changeset_tests`, `adi_ops_schema_tests`, `adi_control_projection_tests`, `adi_migrate_tests`, `adi_builtin_devices_tests`, `adi_param_automation_tests`).
  - Python validators (`validate_ci.py`, `validate_ops.py`, `validate_pd.py`, `validate_schema.py`) and `.github/scripts/check_spec_layout.py`: all PASS.
- Strict `-Werror` check: all 6 modified translation units (`src/adi/automation_lanes.cpp`, `src/adi/check.cpp`, `src/adi/engine/param_automation.cpp`, `tests/test_automation_lanes.cpp`, `tests/test_check.cpp`, `tests/test_param_automation.cpp`) compiled cleanly with `-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wno-unused-parameter -Werror -D_GLIBCXX_ASSERTIONS` under both `g++` 12.2.0 and `clang++` 14.0.6.
- Checked new C++ against all five patterns in `collab/win.md` (20 November) and MSVC `/W4 /WX` rules; `git diff --check` clean.
- Could not run here: MSVC `/W4 /WX` (Windows), macOS/Apple Clang, and full-tree `-DADI_WERROR=ON` under Debian 12's `libstdc++` 12 (`src/adi/mix/limiter/Dsp/FloatRealTransform.h:61` `std::bit_width` sign-compare).

PR requested: gemini/df-e53-canonical-param-ref at 8cde0b320ca1d9b7fb36b32ceb3b21127f7fa646
