# gemini — log

Permanent verification seat (`ADR-0252`): Linux environment (from 25 November: fresh Ubuntu 24.04 VM, GCC 12.4.0, Clang 14.0.6 + libstdc++ 12 via `--gcc-toolchain`, CMake 3.28.3, Ninja, 8 GB RAM / 2 vCPUs; 22–24 November entries were Debian 12, GCC 12.2.0, Clang 14.0.6, CMake 3.25.1). One branch per item (`gemini/<topic>`) cut from `main`, logged here on `gemini/log` for `win` to review and land. Only the `gemini` seat writes to this file. Newest entry at the top.

---

## 2026-11-26 — `walk/*` Round 20 (`walk/b13-default-track-numbers` `ad05c4bf`)

Verified updated `origin/walk/b13-default-track-numbers` head `ad05c4bf` (merged `origin/main` `bdd8926c` + 4-line `tests/test_stack_model.cpp` Debug allocation-check fix; JUCE UI files unchanged from `deb41b55` where all 7 JUCE TUs passed with 0 warnings on both compilers) under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** with `-DADI_WERROR=ON` (headless Pd on, JUCE off, `-DADI_ACCEPTANCE=ON`).

| Branch | SHA | Compiler | Warnings | Touched Suites (`ctest`, 6/6) | Acceptance (`ctest -L acceptance`) |
|---|---|---|---|---|---|
| `walk/b13-default-track-numbers` | `ad05c4bf` | GCC 12.4.0 | 0 | **PASS** (6/6, 38.51 s: `adi_project_bootstrap_tests` 0.39 s, `adi_track_recording_tests` 0.56 s, `adi_ui_seams_tests` 10.30 s, `adi_ui_document_tests` 18.40 s, `adi_ui_stack_model_tests` 2.66 s, `adi_separated_stems_tests` 6.18 s) | **PASS** (2/2, 296.71 s: `adi_builtin_realtime_tests` 255.00 s, `adi_acceptance_wts01` 41.71 s) |
| `walk/b13-default-track-numbers` | `ad05c4bf` | Clang 14.0.6 + libstdc++ 12 | 0 | **PASS** (6/6, 36.10 s: `adi_project_bootstrap_tests` 0.40 s, `adi_track_recording_tests` 0.60 s, `adi_ui_seams_tests` 9.73 s, `adi_ui_document_tests` 16.88 s, `adi_ui_stack_model_tests` 2.55 s, `adi_separated_stems_tests` 5.91 s) | **PASS** (2/2, 269.41 s: `adi_builtin_realtime_tests` 231.73 s, `adi_acceptance_wts01` 37.67 s) |

No findings on `walk/b13-default-track-numbers` (`ad05c4bf`).

---

## 2026-11-26 — `walk/*` Round 19 (`walk/b13-default-track-numbers` `deb41b55`)

Verified `origin/walk/b13-default-track-numbers` at `deb41b55` under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** with `-DADI_WERROR=ON` (headless Pd on, JUCE off, `-DADI_ACCEPTANCE=ON`), plus all 7 touched JUCE UI translation units with `-DADI_WITH_JUCE=ON -DADI_WERROR=ON`.

| Branch | SHA | Compiler | Warnings | Touched Suites (`ctest`, 6/6) | Acceptance (`ctest -L acceptance`) | JUCE TUs (`-DADI_WITH_JUCE=ON -DADI_WERROR=ON`) |
|---|---|---|---|---|---|---|
| `walk/b13-default-track-numbers` | `deb41b55` | GCC 12.4.0 | 0 | **PASS** (6/6, 36.71 s: `adi_project_bootstrap_tests`, `adi_track_recording_tests`, `adi_ui_seams_tests` 10.14 s, `adi_ui_document_tests` 17.87 s, `adi_ui_stack_model_tests` 2.74 s, `adi_separated_stems_tests` 4.98 s) | **PASS** (2/2, 299.89 s: `adi_builtin_realtime_tests` 257.43 s, `adi_acceptance_wts01` 42.45 s) | **PASS** (7/7, 0 warnings: `ui_main.cpp.o`, `ui_mix_analysers.cpp.o`, `ui_mix_focus.cpp.o`, `ui_mixview.cpp.o`, `ui_shell.cpp.o`, `ui_stack.cpp.o`, `test_ui_shell.cpp.o`) |
| `walk/b13-default-track-numbers` | `deb41b55` | Clang 14.0.6 + libstdc++ 12 | 0 | **PASS** (6/6, 36.32 s: `adi_project_bootstrap_tests`, `adi_track_recording_tests`, `adi_ui_seams_tests` 9.65 s, `adi_ui_document_tests` 17.27 s, `adi_ui_stack_model_tests` 2.52 s, `adi_separated_stems_tests` 5.71 s) | **PASS** (2/2, 275.39 s: `adi_builtin_realtime_tests` 236.71 s, `adi_acceptance_wts01` 38.67 s) | **PASS** (7/7, 0 warnings: `ui_main.cpp.o`, `ui_mix_analysers.cpp.o`, `ui_mix_focus.cpp.o`, `ui_mixview.cpp.o`, `ui_shell.cpp.o`, `ui_stack.cpp.o`, `test_ui_shell.cpp.o`) |

No findings on `walk/b13-default-track-numbers` (`deb41b55`).

### Also completed before pausing for `walk/b13`: 7 waiting after-3-December `engine/*` stacks

Checked with `-DADI_WERROR=ON` (Pd on, JUCE off; pre-DF-M62 bases patched locally in `FloatRealTransform.h:61` only to see past it, reverted immediately; pre-DF-E69 bases checked via touched `.o` targets on Clang 14):

| Branch | SHA | Base | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Notes |
|---|---|---|---|---|---|
| `engine/df-e17a-move-overlap` | `98ee01f8` | `01c70983` | **PASS** (`adi_clip_edit_tests` PASS; `ui_shell.cpp.o` JUCE 0 warnings; `adi_ui_seams_tests` only hits pre-DF-E76 `-fsanitize-trap=signed-integer-overflow` on base) | **PASS** (2/2: `adi_clip_edit_tests` 16.56 s, `adi_ui_seams_tests` 9.21 s; `ui_shell.cpp.o` JUCE 0 warnings) | Clean; rebase picks up landed `DF-E76`. |
| `engine/tm-tt01-tempo-ramps-current` | `7c37df25` | `dbf98b0c` | **PASS** (11/11 touched suites, 50.92 s, 0 warnings across 41 files) | **27/31 touched `.o` PASS (0 warnings)**; 4 TUs (`freeze_commands`, `clip_playback`, `midi_clips`, `automation_clips`) only hit pre-DF-E69 base (`timeline_clips.hpp:13` / `midi_clips.cpp:840`, fixed on `main`) | 0 warnings introduced by branch. |
| `engine/srf08-mcu-vpot-wire` | `5f41d14d` | `73f13028` | **PASS** (`adi_mcu_fader_tests` 0.09 s, DF-M62 patched locally) | **PASS** (`adi_mcu_fader_tests` 0.06 s, DF-M62 patched locally) | Clean on both compilers. |
| `engine/tm-ic04-compound-file` | `3ff8ba64` | `73f13028` | **PASS** (`adi_compound_file_tests` 0.17 s, DF-M62 patched locally) | **PASS** (2/2 touched `.o`, 0 warnings; full link blocked only by pre-DF-E69 base) | Clean. |
| `engine/tm-ls03-midi-transport-edges` | `eb545c1e` | `87ea349c` | **PASS** (2/2: `adi_time_base_tests` 4.65 s, `adi_midi_sync_wire_tests` 0.01 s; DF-M62 patched locally) | **PASS** (6/6 touched `.o`, 0 warnings; full link blocked only by pre-DF-E69 base) | Clean. |
| `engine/tm-ic02-smf-codec` | `ec515123` | `73f13028` | **PASS** (`adi_smf_tests` 0.01 s, DF-M62 patched locally) | **PASS** (2/2 touched `.o`, 0 warnings; full link blocked only by pre-DF-E69 base) | Clean. |
| `engine/dev04-drum-pad-routing` | `b1b50df2` | `73f13028` | **PASS** (`adi_device_ops_tests` 2.04 s, DF-M62 patched locally) | **PASS** (2/2 touched `.o`, 0 warnings; full link blocked only by pre-DF-E69 base) | Clean. |

(Note: `2026-11-27-gemini-basics.md` read from `origin/win/sanity-1126`; moving to Item 3 `DF-E89` loan `gemini/df-e89-release-warnings` next, continuing to check `origin/walk/*`, `origin/basics/*`, and `origin/land/*` first before every step.)

---

## 2026-11-26 — Closing Rule / Queue Item 11 (Clang 14.0.6 + libstdc++ 12 ASan + LSan + UBSan on `origin/main` `2b1f777f`)

Completed the second compiler leg of Standing Queue Item 11: all **278** headless suites (`-LE performance`, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`) under **Clang 14.0.6 + libstdc++ 12** with `-O1 -g1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -DADI_WERROR=ON`, `ASAN_OPTIONS=detect_leaks=1:alloc_dealloc_mismatch=1:halt_on_error=1`, `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` on `origin/main` `2b1f777f` (identical source tree to `5ebc7453`). All 479 core/helper targets built with **0 warnings**.

| Outcome | Count | Details |
|---|---|---|
| **PASS (ASan + LSan + UBSan)** | **271 / 278** | 270 within 180 s + `adi_history_retention_fuzz_tests` PASS in 286.74 s (`detect_leaks=1`). Includes `adi_acceptance_wts01` (31.13 s) and **all 23 Pd suites with `detect_leaks=1`** — confirming landed `DF-E67 (b)` completely resolved the prior 23-suite `canvas_setargs` LSan leak. |
| **SKIP** | **1 / 278** | `adi_loudness_ffmpeg_tests` (no `ffmpeg` binary in VM). |
| **PASS (ASan + UBSan, `detect_leaks=0`); system LSan only** | **4 / 278** | `adi_progressive_decode_tests` (8.37 s), `adi_mp4_aac_tests` (0.79 s), `adi_ni_stems_tests` (5.99 s), `adi_aac_import_tests` (1.31 s): 0 ASan/UBSan errors; same Ubuntu 24.04 system `dlopen("libavcodec.so.60")` `call_init` LSan report via `src/adi/audio/aac_ffmpeg.inc:51` as in the GCC 12 ASan sweep. |
| **FAIL (known `DF-M79`, sharpened)** | **1 / 278** | `adi_mix_compressor_tests`: 0 ASan/LSan/UBSan reports; fails `neutral path preserves signed zero and subnormal programme` (196 checks, 1 failure) because Clang 14 folds `gr == 1.f ? right : right * gr` (`src/adi/mix/compressor/Core.cpp:467-468`) into unconditional `mulss` inside `FlushDenormals` **at `-O1` as well as `-O2`/`-O3`**. |
| **FAIL (NEW UBSan finding)** | **1 / 278** | `adi_pd_object_walk_tests` — `float-cast-overflow` UB in `canvas_connect` (`pd-loader-fixed/g_editor.c:4567:51`), caught by Clang 14 UBSan (GCC 12 `-fsanitize=undefined` omits `float-cast-overflow` by default). See Finding below. |

### New Finding on `origin/main` (`2b1f777f` / `5ebc7453`) under Clang 14 UBSan

- **Undefined behavior (`float-cast-overflow`) in Pure Data `canvas_connect` on untrusted `.pd` patch input (`src/juce/pd_loader_fix.cmake:291-317` / generated `pd-loader-fixed/g_editor.c:4567:51`):**
  - `canvas_connect(t_canvas *x, t_floatarg fwhoout, t_floatarg foutno, t_floatarg fwhoin, t_floatarg finno)` immediately executes `int whoout = fwhoout, outno = foutno, whoin = fwhoin, inno = finno;` **before** ADI's `DEV-09 PD2` bounds checks (`pd_loader_fix.cmake:299-314`).
  - When `testASeededFuzzOfPatchTextNeverCrashes()` (`tests/test_pd_object_walk.cpp:447`) opens a fuzzed `.pd` patch with `1e+21` in a `#X connect` line, Clang 14 UBSan aborts:
    ```
    pd-loader-fixed/g_editor.c:4567:51: runtime error: 1e+21 is outside the range of representable values of type 'int'
        #0 in canvas_connect pd-loader-fixed/g_editor.c:4567:51
        #1 in mess_dispatch third_party/libpd/pure-data/src/m_dispatch.h:120:21
        #7 in adi::device::LibPdEngine::open src/juce/pd_engine.cpp:355:14
        #8 in testASeededFuzzOfPatchTextNeverCrashes tests/test_pd_object_walk.cpp:447:18
    ```
  - On x86_64 out-of-range float-to-`int` yields `INT_MIN` (caught accidentally by `whoin < 0`), whereas on ARM64 `fcvtzs` saturates positive overflow to `INT_MAX`. Guarding `fwhoout`, `foutno`, `fwhoin`, `finno` for finite values in `[0, INT_MAX]` before casting to `int` inside `src/juce/pd_loader_fix.cmake` eliminates the UB portably.
  - **Owner:** `devices` (`src/juce/pd_loader_fix.cmake:291-317`).
  - **Smallest repro:** Configure with Clang 14.0.6 `-fsanitize=undefined -fno-sanitize-recover=undefined -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`, build `adi_pd_object_walk_tests`, run `./adi_pd_object_walk_tests`.

---

## 2026-11-26 — Closing Rule (`origin/main` `2b1f777f`: `#790` `win/df-w52-w57` + `#791`)

Verified `origin/main` at `2b1f777f` (merging `#790` `468402f9`, fixing `DF-W52` plug-in instrument loading/replacement and `DF-W57` empty track rename default, plus `#791`) under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** with `-DADI_WERROR=ON` (headless Pd on, JUCE off, `-DADI_ACCEPTANCE=ON`), plus all 5 touched/adjacent JUCE UI translation units with `-DADI_WITH_JUCE=ON -DADI_WERROR=ON`.

| Target | SHA | Compiler | Warnings | Touched Suites (`ctest`) | Acceptance (`ctest -L acceptance`) | JUCE TUs (`-DADI_WITH_JUCE=ON -DADI_WERROR=ON`) |
|---|---|---|---|---|---|---|
| `origin/main` (`#790` `win/df-w52-w57`) | `2b1f777f` | GCC 12.4.0 | 0 | **PASS** (2/2: `adi_ui_seams_tests` 9.90 s, `adi_ui_document_tests` 16.82 s) | **PASS** (2/2: `adi_builtin_realtime_tests` 254.59 s, `adi_acceptance_wts01` 42.87 s) | **PASS** (5/5, 0 warnings: `ui_stack.cpp.o`, `ui_shell.cpp.o`, `ui_arrangement.cpp.o`, `ui_main.cpp.o`, `test_ui_shell.cpp.o`) |
| `origin/main` (`#790` `win/df-w52-w57`) | `2b1f777f` | Clang 14.0.6 + libstdc++ 12 | 0 | **PASS** (2/2: `adi_ui_seams_tests` 9.32 s, `adi_ui_document_tests` 14.80 s) | **PASS** (2/2: `adi_builtin_realtime_tests` 233.10 s, `adi_acceptance_wts01` 37.78 s) | **PASS** (5/5, 0 warnings: `ui_stack.cpp.o`, `ui_shell.cpp.o`, `ui_arrangement.cpp.o`, `ui_main.cpp.o`, `test_ui_shell.cpp.o`) |

No new findings on `2b1f777f`.

---

## 2026-11-26 — Closing Rule (Full 278-suite Clang 14 Release `-O3 -DNDEBUG` `ctest` on `origin/main` `cf6d0ca4`)

Ran all **278** headless suites (`-LE performance`, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`) under **Clang 14.0.6 + libstdc++ 12 Release (`-O3 -DNDEBUG -DADI_WERROR=ON`)** on `origin/main` (`cf6d0ca4`).

| Outcome | Count | Details |
|---|---|---|
| **PASS** | **276 / 278** | Includes `adi_acceptance_wts01` (11.30 s), all Pd/DSP/engine/scanner/history/Mix suites except `adi_mix_compressor_tests`. Total wall time 476.69 s (`-j 2`). |
| **SKIP** | **1 / 278** | `adi_loudness_ffmpeg_tests` (no `ffmpeg` binary in VM). |
| **FAIL** | **1 / 278** | `adi_mix_compressor_tests` — 196 checks, **1 failure** (see Finding below). |

### Finding on `origin/main` (`cf6d0ca4`) under Clang 14 Release (`-O2` / `-O3`)

- **`adi_mix_compressor_tests` fails in Clang 14.0.6 `-O2` and `-O3` (even with `-ffp-contract=off`): `src/adi/mix/compressor/Core.cpp:467-468` flushes subnormal `float` input `0x00000001` (`denorm_min()`) to `0x00000000` on the unity-gain neutral path (`tests/test_mix_compressor.cpp:407-408`):**
  - `Core.cpp:522` enables x86 MXCSR FTZ+DAZ via `filters::FlushDenormals flush;`, while `Core.cpp:467-468` relies on `float outR = gr == 1.f ? right : right * gr;` to bypass multiplication at unity gain.
  - At `-O2` and `-O3`, Clang 14 / LLVM optimizes `gr == 1.f ? right : right * gr` into unconditional `mulss` (`right * gr`), assuming default IEEE-754 semantics where `x * 1.0f` is bit-identical to `x`. With MXCSR DAZ+FTZ active, `mulss(denorm_min, 1.0f)` flushes `0x00000001` to `0x00000000` (`-0.0f` `0x80000000` is preserved; passes under GCC 12 `-O3` and Clang 14 Debug `-O0`).
  - **Owner:** `mixview` (`src/adi/mix/compressor/Core.cpp`).
  - **Smallest repro:**
    ```sh
    clang++-14 --gcc-toolchain=/home/antigravity/gcc12-toolchain -std=c++20 -O2 -DNDEBUG -ffp-contract=off \
      -Iadi_daw/src -Iadi_daw/third_party/json/include \
      adi_daw/tests/test_mix_compressor.cpp adi_daw/src/adi/mix/compressor/Core.cpp -o /tmp/test_mc && /tmp/test_mc
    # FAIL neutral path preserves signed zero and subnormal programme
    # Mix compressor: 196 checks, 1 failures; 643072 compared samples; audio allocations 0/0
    ```

---

## 2026-11-26 — Closing Rule (Full-tree Release `-O3 -DNDEBUG -DADI_WERROR=ON` sweep on `origin/main` `cf6d0ca4`)

Swept all **757** headless translation units (**473** core/library TUs + **284** test TUs, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`) under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** in `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`) with `-DADI_WERROR=ON`.

| Compiler | Core TUs (473) | Test TUs (284) | Acceptance (`adi_acceptance_wts01`) | Summary |
|---|---|---|---|---|
| **Clang 14.0.6 + libstdc++ 12 (`-O3 -DNDEBUG -DADI_WERROR=ON`)** | **473 / 473 PASS** (0 warnings) | **284 / 284 PASS** (0 warnings) | **PASS** (10.19 s; note: Release `adi_builtin_realtime_tests` runs full 10× `kLength=1.0` timed workload and exceeded the 600 s shell ceiling while actively running at 99.5% CPU; CTest timeout is 900 s) | **757 / 757 CLEAN — 0 warnings.** |
| **GCC 12.4.0 (`-O3 -DNDEBUG -DADI_WERROR=ON`)** | **469 / 473 PASS** (4 failed) | **234 / 284 PASS** (50 failed) | Blocked by 4 `adi_core` TUs | **703 / 757 clean; 54 TUs fail** across 5 warning classes (extends `DF-E89`, full breakdown below). |

### GCC 12.4.0 Release (`-O3 -DNDEBUG -DADI_WERROR=ON`) failures on `origin/main` (`cf6d0ca4`)

Smallest repro for any TU below:
```sh
cmake -S adi_daw -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 \
  -DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON
cmake --build build-rel --target <target>
```

1. **`-Werror=mismatched-new-delete` (44 test TUs, 0 `src/` TUs — same class as `DF-M77` / `DF-E89`):** inline `operator delete(...) { std::free(p); }` inlined into STL destructors while `operator new` is not inlined.
   - **Shared header (7 test TUs; owner: `engine` / `devices`):** `tests/replace_operator_new.hpp:43` (`test_builtin_devices.cpp`, `test_clip_envelopes.cpp`, `test_freeze_commands.cpp`, `test_midi_capture.cpp`, `test_scala_import.cpp`, `test_send_automation.cpp`, `test_warp_playback.cpp`).
   - **37 individual test files (owners: `engine`, `engine-win`, `mixview`, `devices`):** `test_analysis_cache_trim.cpp:43`, `test_analysis_comparison.cpp:34`, `test_audio_recording.cpp:29`, `test_bypass_delay.cpp:23`, `test_clip_candidate_ready.cpp:33`, `test_clip_playback.cpp:41`, `test_colorcab.cpp:19`, `test_cue_monitor.cpp:21`, `test_dsp.cpp:58`, `test_external_tempo.cpp:26,28`, `test_gesture_graph.cpp:41`, `test_host.cpp:44`, `test_loudness.cpp:48`, `test_midi_clips.cpp:42`, `test_midi_controllers.cpp:37`, `test_mix_compressor.cpp:43`, `test_mix_deesser.cpp:35`, `test_mix_eq_chain.cpp:43`, `test_mix_focus.cpp:1325`, `test_mix_gate.cpp:37`, `test_note_overlap_playback.cpp:44`, `test_offline_capture.cpp:19`, `test_param_automation.cpp:51`, `test_param_live_replay.cpp:52`, `test_pcm_stream.cpp:36`, `test_pd.cpp:49`, `test_redux_pd.cpp:42`, `test_retap.cpp:45`, `test_sample_settings.cpp:33`, `test_sample_slots.cpp:25`, `test_shared_analysis.cpp:3057`, `test_spectrum_average.cpp:151`, `test_stack_model.cpp:57`, `test_strip_lane_bindings.cpp:25`, `test_transport_settings.cpp:35`, `test_triggered_scope.cpp:225`, `test_warp.cpp:33`. (Also `test_reverb.cpp:25` at `-O2`, reported in Round 15.)
2. **`-Werror=array-bounds` (7 TUs — GCC 12 `-O3` single-element `std::vector` initializer-list / `__builtin_memcpy` false positive):**
   - `src/juce/pd_amxd.cpp:319` (`f.internal = {{0, 0, 1, 0}}`, owner: `devices`).
   - `src/adi/ui/note_edits.hpp:95` (via `tests/test_ui_note_edits.cpp`, owner: `window` / `window-fn`).
   - `tests/test_auto_warp.cpp:73`, `tests/test_edit_placement.cpp:322`, `tests/test_freeze_commands.cpp:464`, `tests/test_gesture_playback.cpp:432` (owner: `engine`).
   - `tests/test_sampler.cpp:190` (owner: `devices`).
3. **`-Werror=maybe-uninitialized` (2 `src/` TUs — GCC 12 `-O3` on `std::optional<std::int64_t>`):**
   - `src/adi/clip_commands.cpp:507` (`secondId`, owner: `engine`).
   - `src/adi/return_send_tap.cpp:64` (`previous`, owner: `engine`).
4. **`-Werror=stringop-overflow=` (1 `src/` TU):**
   - `src/adi/audio/mp4_aac.cpp:44` inlined at `:205` (`sampleDescription` calling `zeros(Bytes&, 8)` after `put` realloc, owner: `engine-win`).
5. **`-Werror=unused-result` (1 test TU — already registered in `DF-E89`):**
   - `tests/test_project_replace.cpp:289,292` (unchecked POSIX `read`/`write`, owner: `window-fn`).

---

## 2026-11-26 — Closing Rule (After-3-December `PR requested` sweep, `engine` seat): 5 stack heads (`au08`, `mi04`, `tm-wt02`, `tm-wt03`)

All five branches predate `DF-M62` (`FloatRealTransform.h:61`) and `DF-E69` (Clang 14 structured-binding / `std::ranges` fixes since landed on `main`); `FloatRealTransform.h:61` was patched transiently locally only to compile past it and reverted after each branch.

| Branch | Head | Touched files | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Findings |
|---|---|---|---|---|---|
| `engine/au08-simplify-envelope` (stacked on `au03-write-guards`) | `be846d1b` | `automation_shapes.{hpp,cpp}`, `automation_splice_store.{hpp,cpp}`, `automation_recording.{hpp,cpp}`, 3 test files (+872/-43) | **PASS (3/3)** — 0 warnings (`adi_automation_splice_store_tests` 6.07 s, `adi_automation_shapes_tests` 2.35 s, `adi_automation_recording_tests` 1.20 s) | **PASS (0 warnings on all touched TUs)** (pre-DF-E69 base only) | None (rebase onto `main` before landing). |
| `engine/au08-envelope-clipboard` (stacked on `au02`) | `9ad32e5e` | `automation_shapes.{hpp,cpp}`, `automation_splice.{hpp,cpp}`, `automation_splice_store.{hpp,cpp}`, 2 test files (+479/-61) | **PASS (3/3)** — 0 warnings (`adi_automation_splice_store_tests` 7.70 s, `adi_automation_splice_tests` 0.05 s, `adi_automation_shapes_tests` 2.61 s) | **PASS (0 warnings on all touched TUs)** (pre-DF-E69 base only) | None (rebase onto `main` before landing). |
| `engine/mi04-step-scale-degrees` (includes `mi04-invert-selected-pitches`, `mi06-fit-to-scale`) | `c3b86647` | `scale_context.{hpp,cpp}`, `note_transforms.{hpp,cpp}`, `test_note_diff.cpp`, `test_scale_context.cpp` (+367/-4) | **PASS (3/3)** — 0 warnings (`adi_scale_context_tests` 0.63 s, `adi_note_transforms_tests` 2.39 s, `adi_note_diff_tests` 13.35 s) | **PASS (0 warnings on all touched TUs)** (pre-DF-E69 base only) | None (rebase onto `main` before landing). |
| `engine/tm-wt02-interior-warp-markers` | `6fa013a5` | `warp_map.{hpp,cpp}`, `test_warp.cpp`, `test_warp_playback.cpp` (+155/-3) | **PASS (2/2)** — 0 warnings (`adi_warp_playback_tests` 25.25 s, `adi_warp_tests` 6.90 s) | **PASS (0 warnings on all 3 touched TUs)** (`warp_map.cpp.o`, `test_warp.cpp.o`, `test_warp_playback.cpp.o`) | None (rebase onto `main` before landing). |
| `engine/tm-wt03-beats-zero-crossing` (includes `tm-wt03-native-warp-interpolation`) | `c52e6ccd` | `warp.cpp`, `test_warp.cpp` (+197/-16) | **PASS (2/2)** — 0 warnings (`adi_warp_playback_tests` 24.54 s, `adi_warp_tests` 6.93 s) | **PASS (0 warnings on both touched TUs)** (`warp.cpp.o`, `test_warp.cpp.o`) | None (rebase onto `main` before landing). |

---

## 2026-11-26 — Closing Rule (After-3-December `PR requested` sweep, `window` seat): `window/key-conflicts` (`cc7174a1`), `window/text-goldens` (`b88bd09a`), `window/command-palette` (`051ec6c0`)

Built all touched JUCE TUs and headless tests under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** with `-DADI_WITH_JUCE=ON -DADI_WERROR=ON`.

| Branch | Head | Touched files | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Findings |
|---|---|---|---|---|---|
| `window/key-conflicts` (stacked on `window/shortcut-sets`, WIN-05) | `cc7174a1` | `src/juce/ui_shell.{hpp,cpp}`, `tests/test_ui_shell.cpp` (+950/-55) | **PASS** (0 warnings on `ui_shell.cpp.o` & `test_ui_shell.cpp.o`) | **PASS** (0 warnings on `ui_shell.cpp.o` & `test_ui_shell.cpp.o`) | None. |
| `window/text-goldens` (WTS-03 first half) | `b88bd09a` | `tests/ui_golden_text.hpp`, `tests/test_ui_shell.cpp` (+205) | **PASS** (0 warnings on `test_ui_shell.cpp.o`) | **PASS** (0 warnings on `test_ui_shell.cpp.o`) | None. |
| `window/command-palette` (WIN-06) | `051ec6c0` | `src/adi/ui/fuzzy_match.hpp`, `src/juce/ui_palette.{hpp,cpp}`, `src/juce/ui_shell.{hpp,cpp}`, `tests/test_ui_shell.cpp`, `tests/test_ui_window_state.cpp` (+584/-7) | **PASS** (0 warnings on `ui_palette.cpp.o`, `ui_shell.cpp.o`, `test_ui_shell.cpp.o`; `adi_ui_window_state_tests` 0.19 s PASS) | **PASS** (0 warnings on `ui_palette.cpp.o`, `ui_shell.cpp.o`, `test_ui_shell.cpp.o`; `adi_ui_window_state_tests` 0.21 s PASS) | None. |

---

## 2026-11-26 — Closing Rule (After-3-December `PR requested` sweep, `engine-win` stacks): `engine-win/export-profile-validation` (`2b3b7e5c`), `engine-win/source-hash-flac-proof` (`ab4fab82`)

Both branches predate `DF-M62` (`FloatRealTransform.h:61`) and `DF-E69` (Clang 14 structured-binding / `std::ranges` fixes since landed on `main`); `FloatRealTransform.h:61` was patched transiently locally only to compile past it and reverted immediately afterwards.

| Branch | Head | Touched scope | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Findings |
|---|---|---|---|---|---|
| `engine-win/export-profile-validation` (includes `export-slice-readback`, `export-conditional-normalization`, `export-cycle-and-matrix`, `export-windowed-delivery`, and all 6 export codecs) | `2b3b7e5c` | 37 files / +5,832 lines (`aiff/flac/mp3/opus/vorbis_writer`, `delivery_src`, `after_export`, `quantizer`, `normalization`, `queue`, tests) | **FAIL (`-Werror=unused-function` in `test_render_queue.cpp:827,873`)** — all 12 export `src/adi/...` TUs compile with 0 warnings; `adi_wav_file_tests` (2.54 s), `adi_material_commands_tests` (1.38 s), `adi_normalization_tests` (4.64 s), `adi_ni_stems_tests` (5.68 s) PASS; `adi_render_queue_tests` passes **633/0** at runtime when built with `-Wno-unused-function`. | **FAIL (`-Werror,-Wunused-function` in `test_render_queue.cpp:827,873`)** — all 12 export `src/adi/...` TUs compile with 0 warnings (pre-DF-E69 base `project_files.cpp`/`param_ops.cpp`/`timeline_clips.hpp` prevents linking until rebased on `main`). | **Real Linux `-Werror` + dead-test blocker** (see Finding below). |
| `engine-win/source-hash-flac-proof` | `ab4fab82` | 22 files (`source_hash_memo.{hpp,cpp}`, `flac_writer`, `quantizer`, `test_source_hash_memo.cpp`) | **PASS (6/6)** — 0 warnings (`adi_source_hash_memo_tests`, `adi_wav_file_tests`, `adi_normalization_tests`, `adi_render_queue_tests`, `adi_decode_tests`, `adi_pcm_stream_tests`). | **PASS (0 warnings on touched TUs)** (`source_hash_memo.cpp.o`, `test_source_hash_memo.cpp.o`; pre-DF-E69 base). | Clean (needs rebase onto `main` for `DF-M62`/`DF-E69` before landing). |

### Finding on `engine-win/export-profile-validation` (`2b3b7e5c`)

- **`adi_daw/tests/test_render_queue.cpp:827:6` (`explicitDirectTaps`) and `:873:6` (`directStemBatch`) fail `-DADI_WERROR=ON` on BOTH GCC 12.4.0 and Clang 14.0.6 (`-Werror=unused-function`) — and neither test is ever called from `main()`:**
  - MSVC `/W4 /WX` does not emit `C4505` for uncalled functions inside an anonymous namespace, hiding the issue on Windows; both Linux compilers reject it under `-DADI_WERROR=ON`, and both test functions (`explicitDirectTaps` and `directStemBatch`) are dead code never executed in `adi_render_queue_tests`.
  - **Owner:** `engine-win` (`tests/test_render_queue.cpp`).
  - **Smallest repro:**
    ```sh
    git checkout 2b3b7e5c
    cmake -S adi_daw -B build -G Ninja -DADI_WERROR=ON -DADI_WITH_JUCE=OFF
    cmake --build build --target adi_render_queue_tests
    ```

---

## 2026-11-26 — Closing Rule (`land/*` stack): `land/walk-1126e` (`4de90ea5`)

Verified win's combined landing stack (`walk/a28-vblank-window-open` + `walk/a31-plugin-tables-per-tick` + `walk/b12-play-start-dropout` + `walk/b14-clip-rename`) under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** (`-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`), plus all four touched JUCE TUs under `-DADI_WITH_JUCE=ON -DADI_WERROR=ON`.

| Branch | Head | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Acceptance (`ctest -L acceptance`) | JUCE TUs (`-DADI_WERROR=ON`) |
|---|---|---|---|---|---|
| `land/walk-1126e` | `4de90ea5` | **PASS** (0 warnings; `adi_mix_document_tests` 7.32 s, `adi_mix_parameter_feed_tests` 1.52 s, `adi_session_tests` 1.19 s, `adi_ui_document_tests` 16.14 s) | **PASS** (0 warnings; `adi_mix_document_tests` 6.69 s, `adi_mix_parameter_feed_tests` 3.89 s, `adi_session_tests` 1.26 s, `adi_ui_document_tests` 14.22 s) | **2/2 PASS** on both (`adi_builtin_realtime_tests` 252.10 s / 231.37 s; `adi_acceptance_wts01` 42.29 s / 37.97 s) | **PASS** on both (`ui_arrangement.cpp.o`, `ui_shell.cpp.o`, `test_ui_shell.cpp.o`, `test_mix_pipeline_document.cpp.o` — 0 warnings) |

---

## 2026-11-26 — Item 5 (Standing Queue Item 15): `REGISTER-AUDIT` — second check of engine's landed DF-E rows on `origin/main` (`18fcc6fd`)

Audited all 19 rows named in `2026-11-22-gemini-queue.md` item 15 (`DF-E1, E3, E4, E7, E9, E13, E14, E15, E18, E26 (a, b), E27 (a-d), E29 (a-d), E48, E49`) directly against the commits, production source, and tests currently on `origin/main` (`18fcc6fd`). Every headless suite named below also passed on `main` across our GCC 12 `-Werror`, ASan+LSan+UBSan (Item 11), TSan (Item 12), and `-D_GLIBCXX_DEBUG` (Item 13) sweeps.

| Row | Commit on `main` (PR) | Test on `origin/main` | Verdict & residual |
|---|---|---|---|
| **DF-E1** | `55fd8bbd` + `9cc16085` (via `#696` `054eeb00` / `e8b6ee36`) | `exactLinearEnd` (`test_clip_details.cpp:119`), `absoluteLinearEndSamples` (`test_clip_playback.cpp`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E3** | `83cca8a5` (`#696` `054eeb00`) | `poolBounds` (`test_progressive_decode.cpp:284`) + `ProgressiveFile::~ProgressiveFile` (`progressive_decode.cpp:71-78`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E4** | `47243494` (`#696` `054eeb00`) | `vanishedDirectoryKeepsSiblings`, `failedCommitReleasesStagedHashes` (`test_library.cpp:339,358`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E7** | `f6215fb1` (via `#696` `054eeb00` / `e8b6ee36`) | `clipEditBoundaries` (`test_edit_plan.cpp:308`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E9** | `ab5feee1` (via `#696` `054eeb00` / `e8b6ee36`) | `forwardCompatibleUndo` (`test_automation_range_undo.cpp:42`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E13** | `f3656b3a` (via `#685` `5acebf6b` / `548f8856`) | `exactBoundaries` (`test_automation_splice_store.cpp:242`) + `test_automation_splice.cpp` | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E14** | `9aa23bec` (via `#685` `5acebf6b` / `548f8856`) | `childlessRoots`, `legacyOrigin` (`test_history_move.cpp:518,557`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E15** | `f60f7f53` (`#685` `5acebf6b`) | `transitions` (`test_quiet_gesture.cpp:207-230`) + `transitionStatus` (`quiet_gesture.cpp:8-14`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E18 (a–f)** | `f28eacd0` (`#685` `5acebf6b`) | (a) `switchToBranch` `keepBranchTip` (`history.cpp:630-635`); (b) `tipOf` `COUNT(*)` bound (`history.cpp:171-175`); (c) `opsApplied = ops.size()` (`history.cpp:413,461`); (d) `true_peak.cpp` `std::isfinite` + `test_dsp.cpp:1654`; (e) `marker_ops.cpp:19-22` + `test_markers.cpp`; (f) single `src/adi/clip_image.cpp` | **Confirmed met (all 6 sub-parts).** None. (Already in `DEFERRED.md` Done.) |
| **DF-E26 (a, b)** | `a548544d` (`#685` `5acebf6b`) | (a) `undoneHead` (`test_history_compaction.cpp:260-276`); (b) `connectedRedo` (`test_history_compaction.cpp:307-325`) + unique ordinal suffix loop (`history_archive.cpp:180-191`) | **Confirmed met for (a) & (b).** Residual: only **(c)** (abandoned-branch budget rule, win/director policy item, already noted open in `DEFERRED.md`). |
| **DF-E27 (a)** | `2accc015` (via `#685` / `#696` `e8b6ee36`) | `shortenedFade` (`test_clip_playback.cpp:287`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E27 (b)** | `25ba8fce` (`#685` `5acebf6b`) | `endSourceClocks` (`test_clip_playback.cpp:847`) + `test_sample_defaults.cpp` | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E27 (c)** | `4f7815a3` (via `#685` `5acebf6b` / `548f8856`) | `queuedNoteAtSwap` (`test_midi_clock_continuity.cpp:250`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E27 (d)** | `2414ac7c` (`#685` `5acebf6b`) | `testEphemeralSkipped` (`test_history.cpp:467`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E29 (a–d)** | `dfab28af` (via `#685` `5acebf6b` / `548f8856`) | (a, b) `receivingLoopClock` (`test_transport_settings.cpp:140-175`); (c) drain-adopted + structural preview + cancel (`test_clap.cpp:3405-3441`); (d) rounded 117 BPM bar boundary at 48 kHz & 44.1 kHz (`test_clap.cpp:3444-3473`, `src/juce/vst3_probe.cpp:1124-1166`) | **Confirmed met for (a)–(d).** Note: VST3 half of (d) lives in `vst3_probe.cpp` (Windows/JUCE probe, not headless CTest). Residual: **(e)** was answered by `ADR-0255 d7 (b)` (MIDI keeps upstream physical latency when Delay Compensation is off) — ready to move whole `DF-E29` to Done once (e)'s ruling/test is recorded. |
| **DF-E48** | `c5f4ae0c` (`#696` `054eeb00`) | `scopesOnlyPlayingAssets` (`test_freeze_capture.cpp:126`), `mutedMidiDoesNotForward` (`test_freeze_commands.cpp:453`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |
| **DF-E49** | `b57638c8` (`#696` `054eeb00`) | `concurrentFaultRemainsLatched` (`test_gesture.cpp`) | **Confirmed met.** None. (Already in `DEFERRED.md` Done.) |

---

## 2026-11-26 — Item 5 (Standing Queue Item 14): `FUZZ` (`adi_fuzz_blob` & `adi_fuzz_textproj`) on `origin/main` (`18fcc6fd`)

Configured and built `adi_fuzzers` under **Clang 14.0.6 + libstdc++ 12** (`-DADI_BUILD_FUZZERS=ON -DADI_WERROR=ON -DADI_WITH_PD=OFF -DADI_WITH_JUCE=OFF`; real libFuzzer + ASan + UBSan via `libclang_rt.fuzzer-x86_64.a`). Ran both targets with 2 parallel workers (`-jobs=2 -workers=2 -max_total_time=300 -print_final_stats=1`).

| Target | Seed corpus | CPU-time (2 workers) | Executions | Avg exec/s | Coverage (`cov` / `ft`) | Final corpus | Peak RSS | Crashes / Reports |
|---|---|---|---|---|---|---|---|---|
| `adi_fuzz_blob` (`src/adi/blob.cpp`) | 18 generated seeds (`fuzz_seeds.py`) | **602 CPU-s** (`0.17 CPU-h`, 301 s wall × 2) | **7,553,439** (3,572,627 + 3,980,812) | 11,869 / 13,225 | `cov: 1868` / `ft: 4405` | 46 units / 9,636 B | 514 MB | **0 crashes, 0 ASan/UBSan/invariant failures** |
| `adi_fuzz_textproj` (`src/adi/textproj.cpp`) | Fresh corpus (all P1–P8 properties) | **602 CPU-s** (`0.17 CPU-h`, 301 s wall × 2) | **328,315** (164,760 + 163,555) | 547 / 543 | `cov: 3785` / `ft: 19896` | 1,016 units / 45 KB | 487 MB | **0 crashes, 0 ASan/UBSan/property failures** |
| **Total** | — | **1,204 CPU-s (`0.33 CPU-h`)** | **7,881,754** | — | — | — | — | **0 crashes** |

---

## 2026-11-26 — Item 1 (`walk/*` Round 18): `walk/a31-plugin-tables-per-tick` (`674d6863`)

Built under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`, plus targeted JUCE `-DADI_WERROR=ON` compile check on `tests/test_mix_pipeline_document.cpp.o`.

| Branch | Head | Touched files | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Acceptance (`ctest -L acceptance`) | Extra checks / Findings |
|---|---|---|---|---|---|---|
| `walk/a31-plugin-tables-per-tick` | `674d6863` | `src/adi/ui/parameter_feed.{hpp,cpp}`, `tests/test_mix_parameter_feed.cpp`, `tests/test_mix_pipeline_document.cpp`, `tests/test_ui_document.cpp` | **PASS** (0 warnings; `adi_mix_parameter_feed_tests` 1.60 s, `adi_mix_document_tests` 7.90 s, `adi_ui_document_tests` 15.83 s; JUCE TU `test_mix_pipeline_document.cpp.o` 0 warnings) | **PASS** (0 warnings; `adi_mix_parameter_feed_tests` 1.37 s, `adi_mix_document_tests` 6.87 s, `adi_mix_eq_pipeline_tests` 13.07 s, `adi_ui_document_tests` 14.24 s; JUCE TU `test_mix_pipeline_document.cpp.o` 0 warnings) | **2/2 PASS** on both (`adi_builtin_realtime_tests` 256.59 s / 233.11 s; `adi_acceptance_wts01` 41.77 s / 37.45 s) | Clean across all legs. |

---

## 2026-11-26 — Item 1 (`walk/*` Round 17): `walk/a28-vblank-window-open` (`a45b1cb4`), `walk/b12-play-start-dropout` (`05f157ec`), `walk/b14-clip-rename` (`6a4a7dbb`)

All three heads built under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`, plus targeted JUCE `-DADI_WERROR=ON` compile checks on all touched JUCE TUs.

| Branch | Head | Touched files | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Acceptance (`ctest -L acceptance`) | Extra checks / Findings |
|---|---|---|---|---|---|---|
| `walk/a28-vblank-window-open` | `a45b1cb4` | `src/juce/ui_shell.{hpp,cpp}`, `tests/test_ui_shell.cpp` | **PASS** (0 warnings; headless + JUCE TUs `ui_shell.cpp.o` & `test_ui_shell.cpp.o` 0 warnings) | **PASS** (0 warnings; headless + JUCE TUs `ui_shell.cpp.o` & `test_ui_shell.cpp.o` 0 warnings) | **2/2 PASS** on both (`adi_builtin_realtime_tests` 252.39 s / 255.38 s; `adi_acceptance_wts01` 41.34 s / 38.19 s) | Clean across all legs. |
| `walk/b12-play-start-dropout` | `05f157ec` | `src/adi/engine/session.{hpp,cpp}`, `tests/test_ui_document.cpp` | **PASS** (0 warnings; `adi_session_tests` 1.19 s, `adi_ui_document_tests` 15.87 s) | **PASS** (0 warnings; `adi_session_tests` 1.20 s, `adi_ui_document_tests` 14.54 s) | **2/2 PASS** on both (`adi_builtin_realtime_tests` 250.73 s / 229.66 s; `adi_acceptance_wts01` 41.11 s / 37.37 s) | Clean across all legs. |
| `walk/b14-clip-rename` | `6a4a7dbb` | `src/juce/ui_arrangement.{hpp,cpp}`, `src/juce/ui_shell.cpp`, `tests/test_ui_shell.cpp` | **PASS** (0 warnings; headless + JUCE TUs `ui_arrangement.cpp.o`, `ui_shell.cpp.o`, `test_ui_shell.cpp.o` 0 warnings) | **PASS** (0 warnings; headless + JUCE TUs `ui_arrangement.cpp.o`, `ui_shell.cpp.o`, `test_ui_shell.cpp.o` 0 warnings) | **2/2 PASS** on both (`adi_builtin_realtime_tests` 258.98 s / 234.55 s; `adi_acceptance_wts01` 42.83 s / 37.87 s) | Clean across all legs. |

---

## 2026-11-26 — Item 1 (`walk/*` Round 16): `walk/a27-window-teardown-crash` (`a27380b5`), `walk/a7-oneshot-in-browser` (`fab2a702`)

Both heads built under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`, plus targeted JUCE `-DADI_WERROR=ON` compile checks on all touched JUCE TUs.

| Branch | Head | Touched files | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Acceptance (`ctest -L acceptance`) | Extra checks / Findings |
|---|---|---|---|---|---|---|
| `walk/a27-window-teardown-crash` | `a27380b5` | `src/juce/ui_main.cpp`, `src/juce/window_release.hpp`, `tests/test_ui_shell.cpp` | **PASS** (0 warnings; headless + JUCE TUs `ui_main.cpp.o` & `test_ui_shell.cpp.o` 0 warnings) | **PASS** (0 warnings; headless + JUCE TUs `ui_main.cpp.o` & `test_ui_shell.cpp.o` 0 warnings) | **2/2 PASS** on both (`adi_builtin_realtime_tests` 251.45 s / 229.73 s; `adi_acceptance_wts01` 41.16 s / 38.07 s) | Clean across all legs. |
| `walk/a7-oneshot-in-browser` | `fab2a702` | `src/adi/builtin_insert.cpp`, `tests/test_builtin_devices.cpp`, `src/juce/ui_browser.{hpp,cpp}`, `tests/test_ui_shell.cpp` | **PASS** (0 warnings; `adi_builtin_devices_tests` 3.97 s PASS; JUCE TUs `ui_browser.cpp.o` & `test_ui_shell.cpp.o` 0 warnings) | **PASS** (0 warnings; `adi_builtin_devices_tests` 3.87 s PASS; JUCE TUs `ui_browser.cpp.o` & `test_ui_shell.cpp.o` 0 warnings) | **2/2 PASS** on both (`adi_builtin_realtime_tests` 250.84 s / 230.20 s; `adi_acceptance_wts01` 41.51 s / 37.14 s) | Clean across all legs. |

---

## 2026-11-26 — Item 5 (Standing Queue Item 13): Full `-D_GLIBCXX_DEBUG` headless sweep on `origin/main` (`946db965`)

Built and ran all **277** headless CTest suites (`-LE performance`, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_WERROR=OFF`) on `origin/main` (`946db965`) under **GCC 12.4.0** with `-O1 -g0 -D_GLIBCXX_DEBUG`.

| Outcome | Count | Details |
|---|---|---|
| **PASS (`-D_GLIBCXX_DEBUG`, 0 aborts)** | **275 / 277** | Every non-skipped suite except `adi_link_playback_tests` (includes `adi_history_retention_fuzz_tests` 130.26 s, `adi_clip_playback_tests` 62.17 s, `adi_migrate_tests` 57.29 s, all Pd/DSP/Mix/Engine/Scanner/UI-model suites). |
| **SKIP** | **1 / 277** | `adi_loudness_ffmpeg_tests` (no `ffmpeg` binary installed in VM). |
| **FAIL (audit-hook interaction only; 0 `_GLIBCXX_DEBUG` aborts)** | **1 / 277** | `adi_link_playback_tests` (139 checks, 16 failures — see note below). |
| **`_GLIBCXX_DEBUG` container / iterator / bounds / comparator aborts** | **0 / 276** | **ZERO libstdc++ debug-mode aborts across all 276 executed headless suites.** |

### Note on `adi_link_playback_tests` under `-D_GLIBCXX_DEBUG`

- `adi_daw/tests/test_link_playback.cpp:36-43` wraps `pthread_mutex_lock`/`pthread_mutex_trylock` (`-Wl,--wrap=pthread_mutex_lock`) and counts any lock taken while `adi::audio::onAudioThread` is true (`Audit::clean()` at `:80-87`).
- Under `-D_GLIBCXX_DEBUG`, libstdc++'s `__gnu_debug::_Safe_iterator` takes its internal `_Safe_sequence_base::_M_get_mutex()` lock whenever safe iterators are attached/detached during `std::__debug::*` container traversal inside the audio callback, causing 16 realtime-audit check failures without any `_GLIBCXX_DEBUG` error or abort. (Not a production lock bug; `adi_link_playback_tests` passes 139/139 in standard Debug, ASan+LSan+UBSan, and TSan builds.)
- Also noted during `-O1 -D_GLIBCXX_DEBUG` compilation (pre-existing GCC 12 `-Wmaybe-uninitialized` false positives on `std::optional<int64_t>` at `-O1` with `_GLIBCXX_DEBUG`, non-fatal here since `-DADI_WERROR=OFF`): `src/adi/return_send_tap.cpp:64`, `src/adi/history.cpp:252`, `src/adi/engine/automation_clips.cpp:209`.

---

## 2026-11-26 — Item 1 (`walk/*` Round 15): `walk/a24-mixview-test-clock` (`91ead564`), `walk/a25-scan-fixture-optimised` (`dff01c74`)

Both heads built under **GCC 12.4.0** and **Clang 14.0.6 + libstdc++ 12** with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`, plus targeted JUCE `-DADI_WERROR=ON` (A24) and optimised `-O2 -DNDEBUG -Werror` checks (A25).

| Branch | Head | Touched files | GCC 12.4.0 `-DADI_WERROR=ON` | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | Acceptance (`ctest -L acceptance`) | Extra checks / Findings |
|---|---|---|---|---|---|---|
| `walk/a24-mixview-test-clock` | `91ead564` | `src/juce/ui_stack.{hpp,cpp}`, `tests/test_ui_shell.cpp` | **PASS** (0 warnings; headless + JUCE TUs `ui_stack.cpp.o` & `test_ui_shell.cpp.o` 0 warnings) | **PASS** (0 warnings; headless + JUCE TUs `ui_stack.cpp.o` & `test_ui_shell.cpp.o` 0 warnings) | **2/2 PASS** on both (`adi_builtin_realtime_tests` 252.39 s / 230.47 s; `adi_acceptance_wts01` 41.46 s / 38.79 s) | Clean across all legs. |
| `walk/a25-scan-fixture-optimised` | `dff01c74` | `tests/fixtures/scan_clap.cpp`, `tests/test_reverb.cpp` | **PASS (Debug)** — 3/3 touched (`adi_reverb_tests`, `adi_plugin_scanner_tests`, `adi_scan_self_test_tests`; both `adi_scan_clap_fixture` & `adi_scan_clap_static_fixture` built clean); **FAIL (`-O2 -Werror`, see below)** | **PASS (Debug & `-O2 -Werror`)** — 3/3 touched (`adi_reverb_tests`, `adi_plugin_scanner_tests`, `adi_scan_self_test_tests`); `scan_clap.cpp` (dynamic & static) and `test_reverb.cpp` 0 warnings at `-O2 -Werror` | **2/2 PASS** on both (`adi_builtin_realtime_tests` 250.62 s / 229.81 s; `adi_acceptance_wts01` 41.79 s / 37.79 s) | **Note (GCC 12 `-O2 -Werror`):** `tests/test_reverb.cpp:25:64` still fails GCC 12 optimised `-Werror` (see finding below). `tests/fixtures/scan_clap.cpp` passes GCC 12 `-O2 -Werror` cleanly in both dynamic and `ADI_CLAP_STATIC_FIXTURE=1` modes. |

### Finding on `walk/a25-scan-fixture-optimised` (`dff01c74`)

- **GCC 12.4.0 `-O2 -Werror` still fails on `adi_daw/tests/test_reverb.cpp:25:64` (`-Werror=mismatched-new-delete`):**
  - `void operator delete(void *p, std::size_t) noexcept { std::free(p); }` is inlined into `std::vector` destructors (`liveDefault()` at `:224,:228`, `rt60()` at `:52,:75`) while `operator new` is not inlined, triggering `-Werror=mismatched-new-delete` (same root cause as `DF-M77` in `test_shared_analysis.cpp`; pre-existing on `main`, not introduced by `dff01c74`, but relevant because A25 targets optimised `-Werror` builds of `test_reverb.cpp`).
  - **Owner:** `walk-A` / `devices` (`tests/test_reverb.cpp`).
  - **Smallest repro:**
    ```sh
    g++-12 -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Wswitch-enum -Wshadow -Werror \
      -c adi_daw/tests/test_reverb.cpp -Iadi_daw/src -Iadi_daw/third_party/json/include -o /tmp/test_reverb.o
    ```

---

## 2026-11-26 — Item 5 (Standing Queue Item 12): TSan across all 38 threaded suites on `origin/main` (`559c528a`)

Tested all **38** threaded suites (graph workers, host, gesture, recording, decode, playback, scanner, Link, shared analysis, limiter owner, render queue, control/MCP, project replace/files) on `origin/main` (`559c528a`) with `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`.

| Compiler / Tool | Suites | Result |
|---|---|---|
| **Clang 14.0.6 + libstdc++ 12 (`-O1 -g1 -fsanitize=thread`)** | 21 / 38 linked (`adi_graph_tests`, `adi_device_host_tests`, `adi_session_tests`, `adi_realize_tests`, `adi_gesture_tests`, `adi_gesture_playback_tests`, `adi_quiet_gesture_tests`, `adi_mix_limiter_owner_tests`, `adi_render_queue_tests`, `adi_control_mcp_tests`, `adi_project_replace_tests`, `adi_project_files_tests`, `adi_recording_commands_tests`, `adi_track_recording_tests`, `adi_audio_memory_capture_tests`, `adi_decode_tests`, `adi_decode_cache_consistency_tests`, `adi_scan_self_test_tests`, `adi_scan_cache_tests`, `adi_scan_catalogue_tests`, `adi_scan_prune_tests`) | **21 / 21 PASS** (0 races, 0 deadlocks). Remaining 17 failed at link time (see Finding below). |
| **GCC 12.4.0 (`-O1 -g1 -fsanitize=thread`, dynamic `libtsan`)** | All 17 Clang-link-blocked suites (`adi_graph_workers_tests`, `adi_host_tests`, `adi_gesture_graph_tests`, `adi_link_session_tests`, `adi_link_playback_tests`, `adi_external_tempo_tests`, `adi_ui_shared_analysis_tests`, `adi_audio_recording_tests`, `adi_midi_capture_tests`, `adi_midi_capture_input_tests`, `adi_progressive_decode_tests`, `adi_pcm_stream_tests`, `adi_clip_playback_tests`, `adi_warp_playback_tests`, `adi_plugin_scanner_tests`, `adi_scan_process_tests`, `adi_scan_quarantine_tests`) | **17 / 17 PASS** (0 races, 0 deadlocks). |
| **Combined total** | **38 / 38 threaded suites** | **38 / 38 PASS — ZERO data races, ZERO deadlocks.** |

### Finding on Clang 14 TSan link (`origin/main` `559c528a`)

- **17 test binaries fail to link under Clang 14.0.6 `-fsanitize=thread` due to `multiple definition of operator new / operator delete` vs static `libclang_rt.tsan_cxx-x86_64.a(tsan_new_delete.cpp.o)`:**
  - **Locations:** `adi_daw/tests/replace_operator_new.hpp:102` (used by `adi_midi_capture_tests`, `adi_warp_playback_tests`), plus 15 individual test files with local global `operator new`/`delete` replacements (`test_graph_workers.cpp:30`, `test_host.cpp:42`, `test_gesture_graph.cpp:65`, `test_link_session.cpp:27`, `test_link_playback.cpp:62`, `test_external_tempo.cpp:34`, `test_shared_analysis.cpp:3058`, `test_audio_recording.cpp:28`, `test_midi_capture_input.cpp:17`, `test_progressive_decode.cpp:37`, `test_pcm_stream.cpp:37`, `test_clip_playback.cpp:47`, `test_plugin_scanner.cpp:32`, `test_scan_process.cpp:39`, `test_scan_quarantine.cpp:18`).
  - **Owner:** `engine` / `engine-win` / `mixview` (`replace_operator_new.hpp` + test allocation-counting harnesses).
  - **Smallest repro:**
    ```sh
    cmake -S adi_daw -B build-tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++-14 \
      -DCMAKE_CXX_FLAGS_DEBUG="-O1 -g1 -fsanitize=thread" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
      -DADI_WERROR=OFF -DADI_WITH_JUCE=OFF
    cmake --build build-tsan --target adi_graph_workers_tests
    ```

---

## 2026-11-26 — Item 5 (Standing Queue Item 11): Full ASan + LSan + UBSan sweep on `origin/main` (`dd80444a`)

Built and ran all **277** headless CTest suites (`-LE performance`, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`) on `origin/main` (`dd80444a`) under GCC 12.4.0 (`-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer`) with `ASAN_OPTIONS=detect_leaks=1:alloc_dealloc_mismatch=1:halt_on_error=1` and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.

| Outcome | Count | Suites / Details |
|---|---|---|
| **PASS (ASan + LSan + UBSan, 0 reports, 0 leaks)** | **271 / 277** | Includes `adi_pd_engine_tests`, `adi_pd_native_devices_tests`, `adi_shaper_pd_tests`, every Pd device suite (`DF-E67 (b)` holds cleanly across the whole tree), `adi_plugin_scanner_tests`, `adi_scan_self_test_tests`, `adi_builtin_devices_tests`, and `adi_history_retention_fuzz_tests` (17,095/0, run without 180 s cap) |
| **SKIP** | **1 / 277** | `adi_loudness_ffmpeg_tests` (no `ffmpeg` binary installed in VM) |
| **FAIL (LSan only; PASS under ASan+UBSan with `detect_leaks=0`)** | **5 / 277** | 2 distinct root causes (Findings 1 & 2 below). **0 ASan errors and 0 UBSan errors across all 276 executed suites.** |

### Findings on `origin/main` (`dd80444a`)

1. **LSan leak in `adi_pd_object_walk_tests` (`169 byte(s) leaked in 5 allocation(s)`)**
   - **Location:** `adi_daw/tests/test_pd_object_walk.cpp:447` (`testASeededFuzzOfPatchTextNeverCrashes`) → `adi_daw/src/juce/pd_engine.cpp:355` (`LibPdEngine::open`) → `adi_daw/third_party/libpd/pure-data/src/g_text.c:529-536` (`canvas_msg`: `pd_new` 80 B + `clock_new` 40 B + `outlet_new` 32 B + `binbuf_new` 16 B + 1 B; self-referencing `t_message` / `t_clock` cycle left behind on a fuzzed malformed patch).
   - **Owner:** `devices` (Pd runtime / overlay; `window-fn` if Pd overlay fix).
   - **Compiler / tool:** GCC 12.4.0 ASan+LSan+UBSan (`ASAN_OPTIONS=detect_leaks=1:alloc_dealloc_mismatch=1:halt_on_error=1`).
   - **Smallest repro:** Build `adi_pd_object_walk_tests` with `-fsanitize=address,undefined` and run `ASAN_OPTIONS=detect_leaks=1 ./adi_pd_object_walk_tests` (passes cleanly with `detect_leaks=0`).

2. **LSan leak on repeated `dlopen`/`dlclose` of system `libavcodec`/`libavutil` (`~187,320 byte(s) leaked in 120 allocation(s)`) in 4 AAC suites**
   - **Affected suites:** `adi_progressive_decode_tests`, `adi_mp4_aac_tests`, `adi_ni_stems_tests`, `adi_aac_import_tests` (all 4 pass with `detect_leaks=0`).
   - **Location:** `adi_daw/src/adi/audio/aac_ffmpeg.inc:51-52, 90-97` (`Api()` calls `dlopen(ADI_AVCODEC_NAME, RTLD_NOW | RTLD_LOCAL)` and `~Api()` calls `dlclose(codec)` on every `ffmpeg60Available()` / `encode` / `decode` call, unloading transitive system shared libraries whose `call_init` static constructors allocate memory never freed on `dlclose`).
   - **Owner:** `engine-win` (audio / media).
   - **Compiler / tool:** GCC 12.4.0 ASan+LSan+UBSan (`ASAN_OPTIONS=detect_leaks=1`).
   - **Smallest repro:** Build `adi_mp4_aac_tests` with `-fsanitize=address,undefined` on Linux with `libavcodec60` installed and run `ASAN_OPTIONS=detect_leaks=1 ./adi_mp4_aac_tests`.

3. **Additional GCC 12 `-O1` / Release `-Werror` warnings spotted on `origin/main` (`dd80444a`) during the `-O1` ASan build**
   - `adi_daw/tests/test_cue_monitor.cpp:21:62` — `-Wmismatched-new-delete` on `void operator delete(void* p, std::size_t) noexcept { std::free(p); }` (class of `DF-M69` / `DF-M77`; owner: `engine`).
   - `adi_daw/tests/test_audio_recording.cpp:29:60` — `-Wmismatched-new-delete` on `void operator delete(void* p, std::size_t) noexcept { std::free(p); }` (class of `DF-M69` / `DF-M77`; owner: `engine-win`).
   - `adi_daw/tests/test_project_replace.cpp:289, 292` — `-Wunused-result` on `(void)::write(...)` / `(void)::read(...)` under glibc `_FORTIFY_SOURCE=2` at `-O1`/`-O2` (owner: `engine-win` / `window-fn`).

---

## 2026-11-26 — Item 4 (part 3, after-3-December pile): Devices lane's DEV-22 stack head `devices/spectral-resonator` (`920177d9`)

`devices/spectral-resonator` (`920177d9`) is the tip of the full DEV-22 after-date stack (172 files changed vs merge-base `ac07be74`, +18,854 / -29 lines), carrying all 27 new post-3-December devices (Align Delay, Auto Pan-Tremolo, Auto Shift, Channel EQ, Corpus, DJ Filter, Drum Buss, Dynamic Tube, EQ Three, Erosion, Filter Delay, Glue Compressor, Grain Delay, LFO Modulator, Live Amp, Live Cabinet, Live Compressor, Live Gate, Live Limiter, Live Pedal, Roar, Shaper Modulator, Spectral Resonator, Spectral Time, Spectrum, Tuner, Vinyl Distortion + DF-E77 merged up).

Base `ac07be74` predates four already-landed `main` fixes (`DF-M62` `FloatRealTransform.h:61`, `DF-E69` Clang 14 structured-binding captures in `param_ops.cpp`/`clip_playback.cpp`, `DF-E67 (b)` `canvas_setargs` LSan fix, and `adi::test::countReplacedNew()`). With a transient local 1-line `DF-M62` unblock on `FloatRealTransform.h:61` (reverted immediately after), **zero failures or warnings were found in any of the 172 DEV-22 files**:

| Configuration | Compiler & Flags | Build | Suites & Results | Verdict |
|---|---|---|---|---|
| Headless Debug (`-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`) | GCC 12.4.0 `-DADI_WERROR=ON` (transient DF-M62 unblock for base `ac07be74`) | **PASS** (0 warnings across all 27 new DSP devices, 27 Pd builtins, and 29 test suites) | All 29 DEV-22 suites (`adi_align_delay_tests`, `adi_auto_pan_tests`, `adi_auto_shift_tests`, `adi_builtin_devices_tests`, `adi_channel_eq_tests`, `adi_corpus_tests`, `adi_dj_filter_tests`, `adi_drum_buss_tests`, `adi_dynamic_tube_tests`, `adi_eq_three_tests`, `adi_erosion_tests`, `adi_filter_delay_tests`, `adi_glue_compressor_tests`, `adi_grain_delay_tests`, `adi_lfo_modulator_tests`, `adi_live_amp_tests`, `adi_live_cabinet_tests`, `adi_live_compressor_tests`, `adi_live_gate_tests`, `adi_live_limiter_tests`, `adi_live_pedal_tests`, `adi_pd_native_devices_tests`, `adi_roar_tests`, `adi_shaper_modulator_tests`, `adi_spectral_resonator_tests`, `adi_spectral_time_tests`, `adi_spectrum_device_tests`, `adi_tuner_tests`, `adi_vinyl_distortion_tests`): **29/29 PASS** (94.03 s) | **PASS** |
| Headless Debug (`-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`) | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | **PASS** on all 83 DEV-22 TUs (0 warnings; only pre-DF-E69 `param_ops.cpp`/`clip_playback.cpp` from base `ac07be74` stopped final link) | All 27 DSP + 27 Pd builtin + 29 test TUs compile clean | **PASS (needs rebase onto `main` for Clang 14 link)** |
| Headless ASan+LSan+UBSan | GCC 12.4.0 `-O1 -g1 -fsanitize=address,undefined` (`halt_on_error=1`) | **PASS** | All 27 new DEV-22 suites: **27/27 PASS** with `detect_leaks=1:alloc_dealloc_mismatch=1` (0 leaks, 0 reports). `adi_builtin_devices_tests` (**386/0 PASS**, `detect_leaks=1:alloc_dealloc_mismatch=0`) and `adi_pd_native_devices_tests` (**778/0 PASS**, 42 devices exact 0 max-error, `detect_leaks=0:alloc_dealloc_mismatch=0`; pre-rebase base `ac07be74` lacked `countReplacedNew` and `DF-E67 (b)`). | **PASS** |

- **Note for landing day (owner: `devices`):** Rebase/merge onto current `main` before 3 December to inherit `DF-M62`, `DF-E69`, `DF-E67 (b)`, and `countReplacedNew()`; zero DEV-22 code defects found.

---

## 2026-11-26 — Item 4 (part 2, after-3-December pile): Clones seat's ADI-Shaper stacks (`clones/shaper-time-block-cost` `31df6e89` and `clones/shaper-curve-edit` `b3d393b3`)

Checked ancestry per `clones/log`: Clones has two maximal ADI-Shaper stack heads:
1. **`clones/shaper-time-block-cost` (`31df6e89`)** — full 10-stage DSP cumulative stack (30 files vs merge-base `300efe61`, +8,302 / -241 lines).
2. **`clones/shaper-curve-edit` (`b3d393b3`)** — full evaluator + panel stack (contains `clones/shaper-panel-trim-1125` `a0aa1b64`, 29 files vs merge-base `73e26e4a`, +9,677 / -94 lines).

| Stack / Head | Compiler & Flags | Build | Suites & Results | Verdict |
|---|---|---|---|---|
| `clones/shaper-time-block-cost` (`31df6e89`) | GCC 12.4.0 `-DADI_WERROR=ON` (Pd on, JUCE off) | **FAIL** (5 files, 94 `-Werror` hits; see Findings 1–7) | Blocked by `-Werror` | **FAIL** |
| `clones/shaper-time-block-cost` (`31df6e89`) | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` (Pd on, JUCE off) | **FAIL** (7 files; adds 2 Clang-only `-Werror` files `adi_shaper_drive.cpp:118` and `test_adi_shaper_drive.cpp:349,350`) | Blocked by `-Werror` | **FAIL** |
| `clones/shaper-time-block-cost` (`31df6e89`) | GCC 12.4.0 ASan+LSan+UBSan (`-DADI_WERROR=OFF -O1 -g1 -fsanitize=address,undefined`) | PASS | All 9 non-Pd shaper suites (`adi_shaper_tests`, `adi_shaper_filter_tests`, `adi_shaper_sweep_tests`, `adi_shaper_drive_tests`, `adi_shaper_crush_tests`, `adi_shaper_noise_tests`, `adi_shaper_reverb_tests`, `adi_shaper_time_tests`, `adi_shaper_session_tests`): **9/9 PASS** with `detect_leaks=1` (0 reports, 0 leaks). Both Pd suites (`adi_shaper_pd_tests`, `adi_pd_native_devices_tests` 287/0): **2/2 PASS** under ASan+UBSan (`detect_leaks=0`; with `detect_leaks=1` they hit only the pre-DF-E67 (b) `canvas_setargs` 1-byte leaks inherited from base `300efe61`). | **PASS (ASan+UBSan)** |
| `clones/shaper-curve-edit` (`b3d393b3`, includes `a0aa1b64`) | GCC 12.4.0 `-DADI_WERROR=ON` (Pd on, JUCE off) | **PASS** (0 warnings) | All 10 headless suites (`adi_shaper_tests`, `adi_shaper_filter_tests`, `adi_shaper_sweep_tests`, `adi_shaper_drive_tests`, `adi_shaper_crush_tests`, `adi_shaper_time_tests`, `adi_shaper_wave_tests`, `adi_shaper_session_tests`, `adi_shaper_pd_tests`, `adi_pd_native_devices_tests`): **10/10 PASS** | **PASS** |
| `clones/shaper-curve-edit` (`b3d393b3`) | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` (Pd on, JUCE off) | **PASS** (0 warnings) | All 10 headless suites: **10/10 PASS** | **PASS** |
| `clones/shaper-curve-edit` (`b3d393b3`) | GCC 12.4.0 ASan+LSan+UBSan | **PASS** | 8/8 non-Pd suites **PASS** with `detect_leaks=1` (0 reports, 0 leaks); 2/2 Pd suites **PASS** with ASan+UBSan (`detect_leaks=0` for pre-DF-E67 (b) base `73e26e4a`) | **PASS** |
| `clones/shaper-curve-edit` (`b3d393b3`) | GCC 12.4.0 & Clang 14.0.6 `-DADI_WERROR=ON -DADI_WITH_JUCE=ON` (panel UI TUs) | **PASS** (0 warnings on all panel/shell TUs: `adi_shaper_panel.cpp`, `adi_shaper_panel_session.cpp`, `adi_shaper_panel_view.cpp`, `adi_shaper_panel_tests.cpp`, `ui_devices.cpp`, `ui_shell.cpp`, `ui_main.cpp`, `test_ui_shell.cpp`) | Compile clean on both compilers | **PASS** |

### Findings on `clones/shaper-time-block-cost` (`31df6e89`) — 7 files fail Linux `-DADI_WERROR=ON` (Owner: `clones`)

Smallest repro for all 7:
```sh
cmake -S adi_daw -B build -G Ninja -DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF
cmake --build build --target adi_shaper_tests adi_shaper_drive_tests adi_shaper_noise_tests adi_shaper_pd_tests -- -k 0
```
1. **`adi_daw/src/adi/dsp/adi_shaper.cpp:44,47,52,55,59,60,76,96,98,283`** — **GCC 12.4.0 & Clang 14.0.6** (`-Werror=sign-conversion`, 51 hits under GCC 12): indexing `std::array` with signed `int`.
2. **`adi_daw/src/adi/dsp/adi_shaper_reverb.cpp:102`** — **GCC 12.4.0** (`-Werror=conversion`) & **Clang 14.0.6** (`-Werror,-Wimplicit-int-float-conversion`): implicit conversion from `std::array::size_type` (`unsigned long`) to `double`.
3. **`adi_daw/src/adi/dsp/adi_shaper_drive.cpp:118`** — **Clang 14.0.6 only** (`-Werror,-Wunneeded-internal-declaration`): `function 'floorMagnitude' is not needed and will not be emitted`.
4. **`adi_daw/tests/test_adi_shaper.cpp:687,689,690,707,813,814,820,1012,1013,1018,2075,2126,2132,2136,2137,2570,2579,2585`** — **GCC 12.4.0 & Clang 14.0.6** (`-Werror=sign-conversion`, 32 hits under GCC 12): indexing `std::vector` with signed `int`.
5. **`adi_daw/tests/test_adi_shaper_drive.cpp:349,350`** — **Clang 14.0.6 only** (`-Werror,-Wfloat-conversion`): `if (!strength && ...)` implicitly converts `const float` to `bool` (use `strength == 0.f`).
6. **`adi_daw/tests/test_adi_shaper_noise.cpp:200,207`** — **GCC 12.4.0 & Clang 14.0.6** (`-Werror=sign-conversion`, 3 hits): indexing `std::array<double, 1024>` with signed `int i`.
7. **`adi_daw/tests/test_adi_shaper_pd.cpp:532,533,724,727,992,993,1028`** — **GCC 12.4.0 & Clang 14.0.6** (`-Werror=sign-conversion`, 7 hits): indexing `std::array<float, 64>` / `std::array<float, 8192>` with signed `int`.

---

## 2026-11-26 — Item 4 (part 1, after-3-December pile): Mix View joined head `mixview/mx30-parked-realtime-burst-lands-after-2026-12-03` (`b87b53a8`)

Checked branch ancestry first: **`mixview/mx30-parked-realtime-burst-lands-after-2026-12-03` (`b87b53a8`) contains all 85 of 85 `lands-after-2026-12-03` branches on origin** (66 files changed vs merge-base `73e26e4a`, +14,990 / -573 lines).

| Configuration | Compiler & Flags | Build | Suites & Results | Verdict |
|---|---|---|---|---|
| Headless Debug (`-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`) | GCC 12.4.0 `-DADI_WERROR=ON` | PASS (0 warnings, all 698 targets) | Full headless `ctest` (`-LE performance`): **277/277 PASS**, 1 skipped (`adi_loudness_ffmpeg_tests`), 0 failures (`adi_history_retention_fuzz_tests` 17,095/0 PASS when run without 180 s cap) | **PASS** |
| Headless Debug (`-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`) | Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | PASS (0 project warnings; 2 third-party `ZLEqualizer` `spin_lock.hpp` `[[clang::nonblocking]]` warnings) | All 38 Mix View / analysis / graph / scope / spectrum / envelope suites: **38/38 PASS** | **PASS** |
| Headless ASan+LSan+UBSan | GCC 12.4.0 `-O1 -g1 -fsanitize=address,undefined` (`detect_leaks=1:alloc_dealloc_mismatch=1:halt_on_error=1`) | PASS (emitted `-Wmismatched-new-delete` warning at `tests/test_shared_analysis.cpp:6224`, see Finding 1) | All 15 directly touched Mix View suites (`adi_mix_limiter_tests`, `adi_mix_limiter_owner_tests`, `adi_mix_document_tests`, `adi_mix_eq_tests`, `adi_mix_eq_host_tests`, `adi_mix_eq_pipeline_tests`, `adi_mix_eq_chain_tests`, `adi_mix_eq_chain_backend_tests`, `adi_mix_focus_tests` 480/0, `adi_graph_tests`, `adi_ui_analysis_comparison_tests`, `adi_ui_spectrum_slope_tests`, `adi_ui_spectrum_average_tests`, `adi_ui_shared_analysis_tests`, `adi_ui_triggered_scope_tests`): **15/15 PASS** (0 sanitizer reports, 0 leaks) | **PASS** |
| Headless Release probe (`-O2 -Werror`) | GCC 12.4.0 `-O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` | **FAIL** (`tests/test_shared_analysis.cpp:6224:50`) | See Finding 1 below | **FAIL** |
| JUCE UI TUs (`-DADI_WITH_JUCE=ON -DADI_WITH_PD=OFF`) | GCC 12.4.0 & Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON` | **FAIL** (`tests/test_ui_shared_analysis.cpp:2150:74`; all 7 other touched UI TUs `ui_analysis.cpp`, `ui_mix_analysers.cpp`, `ui_mix_channels.cpp`, `ui_mix_focus.cpp`, `ui_mixview.cpp`, `ui_stack.cpp`, `test_ui_mix_functions.cpp` compiled with 0 warnings on both) | See Finding 2 below | **FAIL** |

### Findings (2 Linux `-Werror` landing-day blockers)

1. **`adi_daw/tests/test_shared_analysis.cpp:6224:50` — GCC 12.4.0 Release (`-O2`) `-DADI_WERROR=ON` build failure**
   - **Owner:** Mix View (`mixview`)
   - **Compiler:** GCC 12.4.0 (`-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror`)
   - **Error:** `error: 'void free(void*)' called on pointer returned from a mismatched allocation function [-Werror=mismatched-new-delete]` on `void operator delete(void *p) noexcept {std::free(p);}` (inlined into `std::vector<float>` / `std::make_unique<FocusCapturedTruePeak>` destructors in `focus_scope.hpp:411`, `test_shared_analysis.cpp:4378`, `5792`). Same class as `DF-M69`.
   - **Smallest repro:**
     ```sh
     g++-12 -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -DADI_VERSION_STRING=\"0.1.0\" -DADI_WITH_PD=1 \
       -Ibuild/generated -Iadi_daw/src -isystem adi_daw/third_party/clap/include \
       -isystem adi_daw/third_party/SQLiteCpp/include -isystem adi_daw/third_party/SQLiteCpp/sqlite3 \
       -isystem adi_daw/third_party/json/include -c adi_daw/tests/test_shared_analysis.cpp -o /dev/null
     ```
2. **`adi_daw/tests/test_ui_shared_analysis.cpp:2150:74` — GCC 12.4.0 AND Clang 14.0.6 `-DADI_WERROR=ON -DADI_WITH_JUCE=ON` build failure**
   - **Owner:** Mix View (`mixview`)
   - **Compilers:** GCC 12.4.0 (`error: conversion from 'int' to 'float' may change value [-Werror=conversion]`) and Clang 14.0.6 (`error: implicit conversion from 'int' to 'float' may lose precision [-Werror,-Wimplicit-int-float-conversion]`).
   - **Line:** `return SharedSpectrumViewTestAccess::moveCursor(pane.view, {left + column + .5f, top + height * .5f});`
   - **Smallest repro:**
     ```sh
     cmake -S adi_daw -B build-ui -G Ninja -DADI_WITH_JUCE=ON -DADI_WITH_PD=OFF -DADI_WERROR=ON
     cmake --build build-ui --target CMakeFiles/adi_ui_shared_spectrum_tests.dir/tests/test_ui_shared_analysis.cpp.o
     ```

---

## 2026-11-26 — Item 1 (`walk/*` fixes, Round 14): rebased `walk/a10-first-send-live` (`eb5df19b`) and `walk/a22-fader-handle-at-top` (`4a0d72c1`)

Both branches merged `origin/main` (`d12f9094`, PR `#773`). Verified on GCC 12.4.0 and Clang 14.0.6 + libstdc++ 12 with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON`:

| Branch | Head SHA | Compiler (`-DADI_WERROR=ON`) | Warnings | Touched suites | Acceptance (`adi_acceptance_wts01`) | Verdict |
|---|---|---|---|---|---|---|
| `walk/a10-first-send-live` | `eb5df19b` | GCC 12.4.0 | 0 | `adi_ui_seams_tests` PASS (9.69 s) | PASS (43.32 s) | **PASS** |
| `walk/a10-first-send-live` | `eb5df19b` | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_ui_seams_tests` PASS (8.26 s) | PASS (37.98 s) | **PASS** |
| `walk/a22-fader-handle-at-top` | `4a0d72c1` | GCC 12.4.0 | 0 | (JUCE UI only: `ui_look`, `ui_mixer`, `test_ui_shell`) | PASS (43.47 s) | **PASS** |
| `walk/a22-fader-handle-at-top` | `4a0d72c1` | Clang 14.0.6 + libstdc++ 12 | 0 | (JUCE UI only: `ui_look`, `ui_mixer`, `test_ui_shell`) | PASS (37.90 s) | **PASS** |

---

## 2026-11-25 — Item 3 (part 4): `gemini/co01-collab-core` (`9e9b739a`, re-review fixes)

Addressed all six findings from win's 25 November engine re-review (`collab/win.md`), test-first, touching loaned paths only (`src/adi/collab/{batch,replica}.{hpp,cpp}`, `tests/test_collab_{fractional,sync}.cpp`):
1. **Major — atomic rewind pre-check & `rebaseFromBase` recovery (`replica.cpp`):** `syncStep` now walks the exact applied undo ancestry from `History::readTimeline()` and verifies ALL `N` materialized pending `txnId`s BEFORE executing a single `History::undo()` (and redoes any undone steps if `undo()` unexpectedly fails), so a foreign txn `X` in `[P1, X, P2]` never leaves the store half-rewound or touches the redo stack. Persisted `baseHistorySeq_` and added `CollabReplica::rebaseFromBase(error)` using `History::moveTo(baseHistorySeq_)` (preserving departed foreign edits on a system branch per ADR-0128) and re-applying `pending_` cleanly so sync unblocks and converges. Tested with `[P1, X, P2]`.
2. **Major — bounded wire encoding in `commitLocal` and before push (`replica.cpp`, `batch.hpp`):** `commitLocal` validates `maxOpsPerBatch`, `idSpace`, clock headroom, and encodes a worst-case probe (`seq = UINT64_MAX`) checked against `validateCborLimits` and `decodeBatch` BEFORE mutating the store, `pending_`, or counters. Step 3 also validates the final encoded bytes before `bucket.putIfAbsent`. Tested refusal of >65536 CBOR items, >4096 ops, and tight `maxBatchBytes` before touching store or bucket.
3. **Major — persist pending suffix across restart (`replica.cpp`):** `savePersistedState()` serializes `pending_` (`localTxnId` + CBOR hex) and `baseHistorySeq_` into `session_state`; `loadPersistedState()` restores and reconciles them against the store's undo ancestry; `seedInitialOrderKeys()` excludes local-only pending creates/deletes from `baseOrderKeys_` and reconstructs `currentOrderKeys_` by replaying materialized `pending_` ops. Tested restart with 2 unsynced offline pending batches converging against concurrent peer edits.
4. **Medium — author-bound `batchId` & composite `(author, batchId)` dedup (`batch.{hpp,cpp}`, `replica.{hpp,cpp}`):** added `isValidBatchIdForAuthor` requiring `<32-hex author>-<positive decimal counter>` in `decodeBatch`, and keyed `appliedBatchKeys_` and pending-suffix matching on `(authorClientId, batchId)`. Tested hostile `batchId` spoofing.
5. **Medium — Lamport/vector clock overflow cap & rejected-batch clock isolation (`batch.{hpp,cpp}`, `replica.cpp`):** added `kMaxLamportClock = 1 << 60` (`DecodeLimits::maxClock`) enforced on `batch.lamport`, `op.lamport`, and every vector entry at decode; checked headroom before incrementing in `commitLocal`; stopped merging `vector`/`lamport` from batches rejected by `applyBatchToStore`. Tested `INT64_MAX` hostile clocks, rejected-batch isolation, and exact `maxClock` refusal.
6. **Medium — journal-tagged idempotent crash recovery (`replica.cpp`):** stamped sequenced batches (`collab:seq=<S>:author=<A>:batch=<B>`) and local batches (`collab:local:author=<A>:batch=<B>`) in `OpRequest::actorDetail` (committed atomically inside `OpJournal::commit`'s SQLite transaction); `reconcileWithJournal()` recovers applied `(author, batchId)` keys, `localBatchCounter_`, and repaired `localTxnId`s from the applied undo ancestry on load so replaying after a crash between `OpJournal::commit` (or `putIfAbsent`) and `savePersistedState()` is 100% idempotent with zero double-apply and exact fractional order keys. Tested both crash boundaries.

| Compiler / tool | Flags | Suites & counts | Verdict |
|---|---|---|---|
| GCC 12.4.0 | `-DADI_WERROR=ON` (Debug, Pd on, JUCE off) | `adi_collab_fractional_tests` **701/0**, `adi_collab_sync_tests` **144/0** (0 warnings) | **PASS** |
| Clang 14.0.6 + libstdc++ 12 | `-DADI_WERROR=ON` (Debug, Pd on, JUCE off) | `adi_collab_fractional_tests` **701/0**, `adi_collab_sync_tests` **144/0** (0 warnings) | **PASS** |
| GCC 12.4.0 ASan+LSan+UBSan | `-O1 -g1 -fsanitize=address,undefined` (`detect_leaks=1:alloc_dealloc_mismatch=1:halt_on_error=1`) | `adi_collab_fractional_tests` **701/0**, `adi_collab_sync_tests` **144/0** (0 reports, 0 leaks) | **PASS** |
| GCC 12.4.0 TSan | `-O1 -g1 -fsanitize=thread` (`halt_on_error=1`) | `adi_collab_fractional_tests` **701/0**, `adi_collab_sync_tests` **144/0** (0 races) | **PASS** |

**Review requested: `gemini/co01-collab-core` at `9e9b739a`** (lands after 3 December; PR requested only after engine passes this head).

---

## 2026-11-25 — Item 3 (part 3): `gemini/pd15-project-paste` (`60fa699c`, re-review fixes)

Addressed the routing & VCA re-review findings from win's 25 November engine re-review (`collab/win.md`), test-first, touching loaned paths only (`src/adi/project_paste.{hpp,cpp}`, `tests/test_project_paste.cpp`, `CMakeLists.txt`):
1. **Medium — internal sidechains (+ `routing_key_taps`) and internal user Main routes:** added Step 5b (after Step 5 populates `result.deviceIds`) to copy `routing` rows where `kind = 'sidechain'` with both source track and destination device inside the pasted subtree, remapping both ends and carrying `routing_key_taps.tap` through `routing.connect`'s `keyTap` plus `gain_db`/`pan`/`enabled`, and to copy user Main routes (`kind = 'main'`, `origin = 'user'`) whose destination track is inside the pasted subtree.
2. **Medium — external routing and VCA warnings:** every external user Main route, every external/non-device sidechain route (incoming or outgoing), and every VCA / other non-send route touching the pasted subtree is reported in `result.warnings`. `readTrackSnapshot` now also reads `mixer_strip.vca_group_id` and emits a warning when non-null. (`ops_catalog.cpp` has no `mixer.setPanLaw` op, so warning on non-default `pan_law` remains the documented contract.) Updated header contract in `project_paste.hpp`.
3. **Tests:** added `testRoutingSidechainUserMainAndVca` covering internal sidechain + `pre_fx` `keyTap`, internal user Main route, external incoming & outgoing sidechains, external user Main route, `mixer_strip.vca_group_id` + `kind = 'vca'` warnings, full undo digest restoration, and solo-track paste turning an internal sidechain into a warned external sidechain.

| Compiler / tool | Flags | Suites & counts | Verdict |
|---|---|---|---|
| GCC 12.4.0 | `-DADI_WERROR=ON` (Debug, Pd on, JUCE off) | `adi_project_paste_tests` **137/0** (0 warnings) | **PASS** |
| Clang 14.0.6 + libstdc++ 12 | `-DADI_WERROR=ON` (Debug, Pd on, JUCE off) | `adi_project_paste_tests` **137/0** (0 warnings) | **PASS** |
| GCC 12.4.0 ASan+LSan+UBSan | `-O1 -g1 -fsanitize=address,undefined` (`detect_leaks=1:alloc_dealloc_mismatch=1:halt_on_error=1`) | `adi_project_paste_tests` **137/0** (0 reports, 0 leaks) | **PASS** |
| GCC 12.4.0 TSan | `-O1 -g1 -fsanitize=thread` (`halt_on_error=1`) | `adi_project_paste_tests` **137/0** (0 races) | **PASS** |

**Review requested: `gemini/pd15-project-paste` at `60fa699c`** (lands after 3 December; PR requested only after engine passes this head).

---

## 2026-11-25 — Item 3 (part 2): `gemini/srf09-osc-core` (`39a8c20a`, re-review fixes)

Addressed all three findings from win's 25 November engine re-review (`collab/win.md`), test-first, touching loaned paths only (`src/adi/osc_namespace.{hpp,cpp}`, `tests/test_osc_namespace.cpp`):
1. **Major — quiet-window implicit gestures (`ADR-0181 d1/d2`, `SRF-02` `QuietSession` semantics):** writes without an explicit `.../gesture 1` open one implicit gesture on first write and keep it open across continued writes, closing once after a monotonic quiet interval (`pollGestures(nowMs, sink)` or `flushFeedback(nowMs, sink)`, default 250 ms; unsigned elapsed comparison; clock regression refused with `DispatchStatus::ClockRegression`; delayed polling closes an expired implicit gesture before opening a fresh one so separated runs never merge; explicit `.../gesture 0` also closes an open implicit gesture). Updated header contract (`osc_namespace.hpp:29-50`) and added tests for a 3-message fader sweep (1 begin, 3 sets, 1 end = 5 callbacks), a 3-tick nudge turn (1 begin, 3 uncoalesced nudges, 1 end = 5 callbacks), delayed polling, explicit close, and clock regression.
2. **Medium — unmatched and cross-client gesture ends:** `handleExplicitGesture` now emits a gesture end ONLY when the open gesture entry was found and erased for THAT client; unmatched `.../gesture 0` and cross-client `.../gesture 0` return `DispatchStatus::IgnoredRelease` without invoking `WriteSink` and leave another client's gesture open. Covered by tests.
3. **Medium — policy / peer lifecycle cleanup:** added sink-taking `setPolicy(policy, sink)` (closes all open gestures on disable/invalid bind, and closes open gestures of any de-listed client) and `removeFeedbackPeer(client, sink)` (closes that client's open gestures), documented the 1-arg precondition in `osc_namespace.hpp`, and tested disabling, de-listing, and peer removal.

| Compiler / tool | Flags | Suites & counts | Verdict |
|---|---|---|---|
| GCC 12.4.0 | `-DADI_WERROR=ON` (Debug, full CMake + standalone) | `adi_osc_codec_tests` **208/0**, `adi_osc_namespace_tests` **363/0** (0 warnings) | **PASS** |
| Clang 14.0.6 + libstdc++ 12 | `-DADI_WERROR=ON` (Debug, full CMake + standalone) | `adi_osc_codec_tests` **208/0**, `adi_osc_namespace_tests` **363/0** (0 warnings) | **PASS** |
| GCC 12.4.0 ASan+LSan+UBSan | `-O1 -g1 -fsanitize=address,undefined` (`detect_leaks=1:alloc_dealloc_mismatch=1`) | `adi_osc_namespace_tests` **363/0** (0 reports) | **PASS** |
| GCC 12.4.0 TSan | `-O1 -g1 -fsanitize=thread` (`setarch x86_64 -R`) | `adi_osc_namespace_tests` **363/0** (0 races) | **PASS** |

**Review requested: `gemini/srf09-osc-core` at `39a8c20a`** (lands after 3 December; PR requested only after engine passes this head).

---

## 2026-11-25 — Item 3 (part 1): `gemini/srf10-oscquery-core` (`13717f7cd`)

Per win's 25 November engine re-review (`collab/win.md`, "the engine's re-review of gemini's four fixed branches"), `gemini/srf10-oscquery-core` (`13717f7cd`, stacked on `gemini/srf09-osc-core` `94ba8bce4`) passes with notes (4 of 4 first-review findings fixed; GCC 12, Clang 14, ASan+LSan+UBSan, and standalone fuzz harness all green at `13717f7cd`). Lands after 3 December.

**PR requested:** `gemini/srf10-oscquery-core` at `13717f7cd` (lands after 3 December; stacked on `gemini/srf09-osc-core`).

---

## 2026-11-25 — Item 2 (PR-BUILDS): today's eight requested branches (devices & window-fn)

Verified all eight branches from Tonight's Order item 2 on Linux Ubuntu 24.04 x86_64 under GCC 12.4.0 and Clang 14.0.6 + libstdc++ 12 with `-DADI_WERROR=ON` (Pd on, JUCE off for headless branches; JUCE 9.0.5 on for `window-fn/df-e76`; plus ASan+LSan for DF-E67 (b) and Release `-O3` Vocoder timing for DF-E68 (a)).

| Branch | Head SHA | Compiler (`-DADI_WERROR=ON`) | Warnings | Suites & Results | Owner / Verdict |
|---|---|---|---|---|---|
| `devices/time-locate-revision` | `80bbcb6b` | GCC 12.4.0 & Clang 14.0.6 | 0 / 0 | `adi_pd_native_devices_tests`: **291/0 PASS** on both | `devices` — **PASS** |
| `devices/df-e36-tests` | `fae3ce72` | GCC 12.4.0 & Clang 14.0.6 | 0 / 0 | `adi_pd_engine_tests`: **705/0 PASS** on both | `devices` — **PASS** |
| `devices/vocoder-p99` | `4697d2dc` | GCC 12.4.0 & Clang 14.0.6 | 0 / 0 | `adi_vocoder_tests` **48/0**, `adi_pd_native_devices_tests` **287/0** PASS on both; Release `-O3` Linux pitch-tracking spike cut from **+262.9 µs** (64f p99 **1223.9 µs**, 91.8% of 1333 µs) to **+6.7 µs** (64f p99 **447.6 µs**, 33.6%); 128f spike **+145.5 µs** (p99 **1487.0 µs**, 55.8%) → **+4.3 µs** (p99 **687.6 µs**, 25.8%) | `devices` — **PASS (DF-E68 (a) Linux proof confirmed)** |
| `devices/df-e67b-canvas-args` | `aab2e60c` | GCC 12.4.0 & Clang 14.0.6 (`-DADI_WERROR=ON`) + GCC 12.4.0 ASan+LSan (`detect_leaks=1`) | 0 / 0 | Debug: `adi_pd_engine_tests` **702/0 PASS** on both. **LSan FAIL:** `735 byte(s) leaked in 51 allocation(s)` remain (`s_path.c`, `x_vexp.c`, `g_scalar.c` unpatched; only the 225 `g_canvas.c` 1-byte leaks were removed) | `devices` — **PARTIAL** (adds 40-cycle regression test, does not clear LSan; see verdict below) |
| `window-fn/df-e67b` | `0271bf12` | GCC 12.4.0 & Clang 14.0.6 (`-DADI_WERROR=ON`) + GCC 12.4.0 ASan+LSan (`detect_leaks=1`) | 0 / 0 | Debug: `adi_pd_engine_tests` **701/0 PASS** on both. **LSan PASS (`exit=0`):** **0 bytes leaked in 0 allocations** across all 701 checks (patches `g_canvas.c`, `s_path.c`, `x_vexp.c`, `g_scalar.c`, and `g_template.c`) | `window-fn` — **PASS (DF-E67 (b) Linux LSan proof confirmed; better proof)** |
| `window-fn/df-e76` | `d8b9a43d` | GCC 12.4.0 (`-DADI_WITH_JUCE=ON -DADI_WITH_PD=OFF -DADI_WERROR=ON`) & Clang 14.0.6 + libstdc++ 12 (`-DADI_WITH_JUCE=ON`) | 0 / 0 | GCC 12: `adi_play_render_tests` **PASS** (37.93 s, symlinked VST3 alias check green). Clang 14: all 3 touched/test JUCE TUs (`juce_device_loader.cpp.o`, `vst3_host.cpp.o`, `test_play_render.cpp.o`) compile clean with 0 warnings | `window-fn` (row owner `engine-win`) — **PASS (DF-E76 Linux proof confirmed)** |
| `window-fn/df-e12` | `8f62b45b` | GCC 12.4.0 & Clang 14.0.6 | 0 / 0 | `adi_project_replace_tests` **53/0**, `adi_project_files_tests` **54/0**, `adi_settings_tests` **330/0** PASS on both; **25/25 parallel repeats** (`-j 4`) of all three suites together: **0 failures** (135.59 s) | `window-fn` (row owner `engine-win`) — **PASS (DF-E12 Linux proof confirmed)** |
| `window-fn/df-e78` | `8f44a215` | GCC 12.4.0 & Clang 14.0.6 (transient local DF-M62 patch on `FloatRealTransform.h:61` only, because base `2b3b7e5c` predates DF-M62) | 0 / 0 (on all 15 changed TUs) | GCC 12: `adi_render_queue_tests` **666/0**, `adi_normalization_tests` **41/0**, `adi_wav_file_tests` **176/0** PASS. Clang 14: all 15 changed export/test TUs (`aiff_writer.cpp.o`, `test_render_queue.cpp.o`, etc.) compile clean with 0 warnings (full `adi_core` link stops at pre-DF-E69 `src/adi/project_files.cpp:308` inherited from base `2b3b7e5c`) | `window-fn` (row owner `engine-win`) — **PASS (DF-E78 confirmed; lands after 3 Dec with export stack)** |

### DF-E67 (b) comparison: which of the two branches is the better proof?

- **`window-fn/df-e67b` (`0271bf12`) is the better proof and complete fix.** Under GCC 12.4.0 ASan+LSan (`detect_leaks=1`), `devices/df-e67b-canvas-args` (`aab2e60c`) removes only the 225 `g_canvas.c` `canvas_setargs` 1-byte leaks (960 → 735 bytes, 276 → 51 allocations) and **still fails LSan with 735 bytes leaked in 51 allocations** across `s_path.c`, `x_vexp.c`, and `g_scalar.c`. `window-fn/df-e67b` (`0271bf12`) fixes all four Pd leaks plus the `g_template.c` `gtemplate_add` dangling-stub bug and passes `adi_pd_engine_tests` under LSan with **0 bytes leaked in 0 allocations (exit 0)**.
- `devices/df-e67b-canvas-args` (`aab2e60c`) adds the useful 40-cycle regression test (`testAbstractionArgumentsLeaveNothingBehind`), which combines cleanly on top of `window-fn/df-e67b`'s overlay (as win already combined in `#752` `8b0be92f`).

(No PR requested for Item 2; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 13): `walk/a10-first-send-live`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a10-first-send-live` | `a9b5e728` | `src/adi/ui/send_edits.hpp`, `src/juce/ui_shell.cpp` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | `adi_ui_seams_tests` **586/0**, `adi_ui_document_tests` **343/0**, `adi_gesture_playback_tests` **623/0** | **PASS** (43.59 s) | **PASS** |
| `walk/a10-first-send-live` | `a9b5e728` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_ui_seams_tests` **586/0**, `adi_ui_document_tests` **343/0**, `adi_gesture_playback_tests` **623/0** | **PASS** (40.13 s) | **PASS** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 12): `walk/a21-commit-dropout`, `walk/a22-fader-handle-at-top`, `walk/a23-return-index`, `walk/a2b-track-colours-stay`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a21-commit-dropout` | `9efbe4d4` | `src/adi/engine/{gesture_preview.cpp,session.{hpp,cpp}}`, `tests/test_ui_document.cpp`, `tests/ui_test_hygiene.hpp` (JUCE) | GCC 12.4.0 | 0 | `adi_session_tests` **214/0**, `adi_gesture_playback_tests` **623/0**, `adi_ui_document_tests` **359/0** | **PASS** (43.88 s) | **PASS** |
| `walk/a21-commit-dropout` | `9efbe4d4` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_session_tests` **214/0**, `adi_gesture_playback_tests` **623/0**, `adi_ui_document_tests` **359/0** | **PASS** (38.17 s) | **PASS** |
| `walk/a22-fader-handle-at-top` | `3c466091` | `src/juce/{ui_look,ui_mixer}.{hpp,cpp}` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (42.26 s) | **PASS (headless)** |
| `walk/a22-fader-handle-at-top` | `3c466091` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (40.41 s) | **PASS (headless)** |
| `walk/a23-return-index` | `aa030be7` | `src/adi/ui/track_edits.hpp`, `tests/test_ui_seams.cpp`, `src/juce/ui_shell.cpp` (JUCE), `tests/ui_test_hygiene.hpp` (JUCE) | GCC 12.4.0 | 0 | `adi_ui_seams_tests` **601/0** | **PASS** (43.66 s) | **PASS** |
| `walk/a23-return-index` | `aa030be7` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_ui_seams_tests` **601/0** | **PASS** (38.93 s) | **PASS** |
| `walk/a2b-track-colours-stay` | `0b3b3e25` | `src/adi/store.cpp`, `src/adi/ui/track_edits.hpp`, `src/juce/ui_shell.cpp` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | `adi_store_tests` **72/0**, `adi_ui_seams_tests` **586/0**, `adi_ui_document_tests` **343/0** | **PASS** (43.31 s) | **PASS** |
| `walk/a2b-track-colours-stay` | `0b3b3e25` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_store_tests` **72/0**, `adi_ui_seams_tests` **586/0**, `adi_ui_document_tests` **343/0** | **PASS** (38.66 s) | **PASS** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 11): `walk/b4-live-take`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/b4-live-take` | `e264e3c7` | `src/juce/{ui_arrangement.{hpp,cpp},ui_shell.cpp}` (JUCE), `tests/test_ui_shell.cpp` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (44.10 s) | **PASS (headless)** |
| `walk/b4-live-take` | `e264e3c7` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (38.52 s) | **PASS (headless)** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 10): `walk/a13-automated-controls`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a13-automated-controls` | `a5b5875b` | `src/adi/ui/automation_mode.hpp`, `src/juce/ui_mixer.{hpp,cpp}` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | `adi_ui_document_tests` **343/0**, `adi_ui_seams_tests` **586/0** | **PASS** (43.74 s) | **PASS** |
| `walk/a13-automated-controls` | `a5b5875b` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_ui_document_tests` **343/0**, `adi_ui_seams_tests` **586/0** | **PASS** (38.86 s) | **PASS** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 9): `walk/a18-record-after-save-as`, `walk/a20-gesture-drain-hang`, `walk/b11-launch-args`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a18-record-after-save-as` | `35f58275` | `src/adi/engine/session.cpp`, `tests/test_ui_document.cpp` | GCC 12.4.0 | 0 | `adi_session_tests` **214/0**, `adi_ui_document_tests` **349/0**, `adi_recording_commands_tests` **185/0** | **PASS** (45.97 s) | **PASS** |
| `walk/a18-record-after-save-as` | `35f58275` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_session_tests` **214/0**, `adi_ui_document_tests` **349/0**, `adi_recording_commands_tests` **185/0** | **PASS** (38.31 s) | **PASS** |
| `walk/a20-gesture-drain-hang` | `e2a6c89b` | `src/adi/gesture_session.cpp`, `tests/test_gesture_playback.cpp`, `tests/ui_test_hygiene.hpp` (JUCE) | GCC 12.4.0 | 0 | `adi_gesture_tests` **780/0**, `adi_gesture_playback_tests` **643/0**, `adi_gesture_graph_tests` **125/0**, `adi_quiet_gesture_tests` **185/0** | **PASS** (42.89 s) | **PASS** |
| `walk/a20-gesture-drain-hang` | `e2a6c89b` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_gesture_tests` **780/0**, `adi_gesture_playback_tests` **643/0**, `adi_gesture_graph_tests` **125/0**, `adi_quiet_gesture_tests` **185/0** | **PASS** (39.50 s) | **PASS** |
| `walk/b11-launch-args` | `eff67d85` | `src/juce/{ui_main.cpp,ui_shell.{hpp,cpp}}` (JUCE), `tests/test_ui_shell.cpp` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (42.63 s) | **PASS (headless)** |
| `walk/b11-launch-args` | `eff67d85` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (39.95 s) | **PASS (headless)** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 8): `walk/b10-mixer-title-select`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/b10-mixer-title-select` | `1746e3ce` | `src/juce/ui_mixer.{hpp,cpp}` (JUCE), `tests/test_ui_shell.cpp` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (46.79 s) | **PASS (headless)** |
| `walk/b10-mixer-title-select` | `1746e3ce` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (40.12 s) | **PASS (headless)** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 7): `walk/a12-peak-hold-follows-track`, `walk/a14-automation-click`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a12-peak-hold-follows-track` | `bce69522` | `src/juce/ui_mixer.{hpp,cpp}` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (45.25 s) | **PASS (headless)** |
| `walk/a12-peak-hold-follows-track` | `bce69522` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (39.02 s) | **PASS (headless)** |
| `walk/a14-automation-click` | `d68cf1b0` | `src/juce/{ui_arrangement,ui_shell}.{hpp,cpp}` (JUCE), `tests/fixtures/ui/keymap-*.txt`, `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | N/A (JUCE-gated; `adi_ui_seams_tests` 586/0) | **PASS** (45.44 s) | **PASS (headless)** |
| `walk/a14-automation-click` | `d68cf1b0` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (JUCE-gated; `adi_ui_seams_tests` 586/0) | **PASS** (38.77 s) | **PASS (headless)** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 6): `walk/a8-ctrl-drag-copy`, `walk/a9-value-box-vertical`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a8-ctrl-drag-copy` | `cff2c092` | `src/adi/clip_commands.{hpp,cpp}`, `tests/test_edit_plan.cpp`, `src/juce/{ui_arrangement,ui_shell}.{hpp,cpp}` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | `adi_edit_plan_tests` **682/0**, `adi_clip_edit_tests` **434/0** | **PASS** (43.24 s) | **PASS** |
| `walk/a8-ctrl-drag-copy` | `cff2c092` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_edit_plan_tests` **682/0**, `adi_clip_edit_tests` **434/0** | **PASS** (38.59 s) | **PASS** |
| `walk/a9-value-box-vertical` | `49dc7cc5` | `src/juce/{ui_look,ui_mixer}.cpp` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (45.06 s) | **PASS (headless)** |
| `walk/a9-value-box-vertical` | `49dc7cc5` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (40.15 s) | **PASS (headless)** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 5): `walk/a3-new-untitled`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a3-new-untitled` | `8aa41b6a` | `src/juce/{ui_main.cpp,ui_shell.{hpp,cpp}}` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (43.64 s) | **PASS (headless)** |
| `walk/a3-new-untitled` | `8aa41b6a` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (38.00 s) | **PASS (headless)** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 4): `walk/a16-default-input`, `walk/b3-take-name`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a16-default-input` | `8924da46` | `src/juce/{app_audio_page.{hpp,cpp},ui_main.cpp}` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (43.36 s) | **PASS (headless)** |
| `walk/a16-default-input` | `8924da46` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (39.84 s) | **PASS (headless)** |
| `walk/b3-take-name` | `78a0284f` | `src/adi/ui/{arrangement_geometry.hpp,project_document.cpp}`, `tests/test_ui_document.cpp`, `src/juce/{ui_clipview.{hpp,cpp},ui_mix_channels.cpp}` (JUCE) | GCC 12.4.0 | 0 | `adi_ui_document_tests` **344/0**, `adi_ui_seams_tests` **586/0** | **PASS** (44.12 s) | **PASS** |
| `walk/b3-take-name` | `78a0284f` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_ui_document_tests` **344/0**, `adi_ui_seams_tests` **586/0** | **PASS** (39.27 s) | **PASS** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 3): `walk/a2-insert-track-order`, `walk/a5-oneshot-sample-tile`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a2-insert-track-order` | `d7bb9ee3` | `src/adi/ui/{arrangement_geometry,track_edits}.hpp`, `tests/test_ui_seams.cpp`, `src/juce/{ui_arrangement.hpp,ui_shell.cpp}` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | `adi_ui_seams_tests` **609/0 PASS** | **PASS** (43.83 s) | **PASS** |
| `walk/a2-insert-track-order` | `d7bb9ee3` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_ui_seams_tests` **609/0 PASS** | **PASS** (40.47 s) | **PASS** |
| `walk/a5-oneshot-sample-tile` | `a7b05251` | `src/juce/ui_devices.{hpp,cpp}` (JUCE), `tests/{test_ui_shell.cpp,ui_test_hygiene.hpp}` (JUCE) | GCC 12.4.0 | 0 | N/A (all files JUCE-gated) | **PASS** (44.06 s) | **PASS (headless)** |
| `walk/a5-oneshot-sample-tile` | `a7b05251` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (all files JUCE-gated) | **PASS** (40.46 s) | **PASS (headless)** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 2): `walk/b6-take-clears-range`

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/b6-take-clears-range` | `18dbc1c0` | `src/adi/recording_commands.cpp`, `tests/test_audio_memory_capture.cpp`, `tests/test_recording_commands.cpp` | GCC 12.4.0 | 0 | `adi_recording_commands_tests` **192/0**, `adi_audio_memory_capture_tests` **32/0**, `adi_track_recording_tests` **33/0**, `adi_audio_recording_tests` **169/0** | **PASS** (46.90 s) | **PASS** |
| `walk/b6-take-clears-range` | `18dbc1c0` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_recording_commands_tests` **192/0**, `adi_audio_memory_capture_tests` **32/0**, `adi_track_recording_tests` **33/0**, `adi_audio_recording_tests` **169/0** | **PASS** (39.66 s) | **PASS** |

(No PR requested; run-only verification.)

---

## 2026-11-25 — Item 1 (`walk/*` fixes, Round 1): `walk/a1-granted-buffer`, `walk/a6-vst3-editor`, `walk/a15-ui-tests-release-build`

Verified the first three `walk/*` fix heads on origin (report branches `walk/windows10` `0668b7b7` and `walk/windows11` `ed351d30` carry markdown reports only). Built under GCC 12.4.0 and Clang 14.0.6 + libstdc++ 12 with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON -DCMAKE_BUILD_TYPE=Debug -g0`, running every touched headless suite and `adi_acceptance_wts01` (`ctest -R '^adi_acceptance_wts01$'`; note `ctest -L acceptance` also lists `adi_builtin_realtime_tests`, which is the separate DSP performance suite).

| Branch | Head SHA | Touched files | Compiler (`-DADI_WERROR=ON`, Pd on, JUCE off) | Warnings | Touched headless suites | `adi_acceptance_wts01` | Verdict |
|---|---|---|---|---|---|---|---|
| `walk/a1-granted-buffer` | `d5d449ea` | `CMakeLists.txt`, `src/adi/settings/device_grant.{hpp,cpp}`, `src/juce/ui_main.cpp` (JUCE), `tests/test_settings.cpp` | GCC 12.4.0 | 0 | `adi_settings_tests`: **339/0 PASS** | **PASS** (43.02 s) | **PASS** |
| `walk/a1-granted-buffer` | `d5d449ea` | same | Clang 14.0.6 + libstdc++ 12 | 0 | `adi_settings_tests`: **339/0 PASS** | **PASS** (38.08 s) | **PASS** |
| `walk/a6-vst3-editor` | `a1b81e0b` | `src/juce/editor_host_component.cpp` (JUCE), `tests/test_plugin_editor_juce.cpp` (JUCE) | GCC 12.4.0 | 0 | N/A (both files JUCE-gated) | **PASS** (42.72 s) | **PASS (headless)** |
| `walk/a6-vst3-editor` | `a1b81e0b` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (both files JUCE-gated) | **PASS** (39.01 s) | **PASS (headless)** |
| `walk/a15-ui-tests-release-build` | `68c30e05` | `tests/ui_test_hygiene.hpp` (included by `tests/test_ui_shell.cpp`, JUCE-gated) | GCC 12.4.0 | 0 | N/A (JUCE-gated) | **PASS** (43.83 s) | **PASS (headless)** |
| `walk/a15-ui-tests-release-build` | `68c30e05` | same | Clang 14.0.6 + libstdc++ 12 | 0 | N/A (JUCE-gated) | **PASS** (39.52 s) | **PASS (headless)** |

(No PR requested; run-only verification.)

---

## 2026-11-24 — Standing queue rerun (Item 12, TSAN) on `main` (`48fb9953a`)

Head: `main` (`48fb9953a`, post `#739`, `#740`, `#742`, `#744`). Compiler & flags: GCC 12.2.0 (`-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O1 -g1 -fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS_DEBUG="-O1 -g1 -fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DADI_WITH_JUCE=OFF -DADI_WITH_PD=ON -DADI_WERROR=OFF`), executed under `setarch x86_64 -R` with `TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:exitcode=66` (Clang 14.0.6 still lacks `libclang_rt.tsan-x86_64.a` on this VM).

- **Counts:** **43 / 43 PASS (100%)**, **0 data races, 0 deadlocks**.
- **Verdict:** **PASS — all 43 threaded suites completely TSan-clean on `main`.**
- **What changed since our 23 November rerun (`7e3e627d`, 41/42):**
  - `adi_progressive_decode_tests` and `adi_pcm_stream_tests` (DF-E65 landed on `main`): **now PASS** (0 races).
  - `adi_mix_focus_tests` (DF-M66 landed in `#740`) and `adi_recording_commands_tests` (DF-E32 (b) landed in `#744`): **PASS** (0 races).
- **Suites verified (43):** `adi_graph_workers_tests`, `adi_host_tests`, `adi_device_host_tests`, `adi_clap_tests`, `adi_session_tests`, `adi_transport_generation_tests`, `adi_cue_monitor_tests`, `adi_gesture_tests`, `adi_gesture_graph_tests`, `adi_gesture_playback_tests`, `adi_quiet_gesture_tests`, `adi_audio_recording_tests`, `adi_recording_commands_tests`, `adi_track_recording_tests`, `adi_automation_recording_tests`, `adi_audio_memory_capture_tests`, `adi_midi_capture_input_tests`, `adi_midi_capture_tests`, `adi_offline_capture_tests`, `adi_freeze_capture_tests`, `adi_decode_tests`, `adi_decode_space_tests`, `adi_decode_cache_consistency_tests`, `adi_progressive_decode_tests`, `adi_pcm_stream_tests`, `adi_plugin_scanner_tests`, `adi_scan_self_test_tests`, `adi_scan_cache_tests`, `adi_scan_prune_tests`, `adi_scan_catalogue_tests`, `adi_scan_process_tests`, `adi_scan_quarantine_tests`, `adi_link_session_tests`, `adi_link_playback_tests`, `adi_render_queue_tests`, `adi_mix_limiter_owner_tests`, `adi_mix_focus_tests`, `adi_mix_eq_chain_tests`, `adi_mix_eq_chain_backend_tests`, `adi_control_mcp_tests`, `adi_ui_document_tests`, `adi_collect_export_tests`, `adi_mpe_output_tests`.

---

## 2026-11-24 — Standing queue rerun (Item 13, GLIBCXX-DEBUG) on `main` (`48fb9953a`)

Head: `main` (`48fb9953a`, post `#739`, `#740`, `#742`, `#744`). Compiler & flags: GCC 12.2.0 (`-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-g0" -DCMAKE_CXX_FLAGS_DEBUG="-g0 -D_GLIBCXX_DEBUG" -DADI_WITH_JUCE=OFF -DADI_WITH_PD=ON -DADI_WERROR=OFF`).

- **Counts:** **276 / 277 PASS**, **0 `_GLIBCXX_DEBUG` aborts** across the entire headless test suite (batch 1 `1..150`: 150/150 PASS in 222.85 s; batch 2 `151..280`: 126/127 PASS in 407.51 s).
- **Verdict:** **PASS — zero `_GLIBCXX_DEBUG` aborts; nothing new broken.**
- **What changed since our 23 November rerun (`7e3e627d`, 272/275):**
  - `adi_mix_opto_gain_tests` (DF-M64, landed in `#740`): **now PASS** (previously aborted in `std::span` ctor).
  - `adi_ui_seams_tests` (DF-W39, landed in `#739`): **now builds and PASSES** on GCC 12.2.
  - Sole remaining non-pass suite is the known informational harness interaction in `adi_link_playback_tests` (0 `_GLIBCXX_DEBUG` aborts; 16/139 `RealtimeAudit` checks trip because `__wrap_pthread_mutex_lock` counts libstdc++ debug safe-iterator mutex locks inside audio callbacks).

---

## 2026-11-24 — Standing queue rerun (Items 3 & 10): WTS-01, full-tree `-DADI_WERROR=ON` sweep, and full headless ctest on `main` (`48fb9953a`)

Reran standing queue Items 3 (`WTS-01`) and 10 (`WERROR-SWEEP`, plus full headless `ctest`) on `main` at `48fb9953a` (post `#739`, `#740`, `#742`, `#744`) with **zero local patches/workarounds** (DF-M62, DF-E69, DF-W39, DF-M63–M67, DF-E67 (a), DF-E32 (b), and DF-E35 (a) are all on `main`).

| Compiler | Flags | Full-tree build (`1060/1060`) | `adi_acceptance_wts01` | Headless `ctest -LE acceptance` | Verdict |
|---|---|---|---|---|---|
| GCC 12.2.0 | Debug `-g0 -DADI_WITH_JUCE=OFF -DADI_WITH_PD=ON -DADI_WERROR=ON -DADI_ACCEPTANCE=ON` | **PASS** — 0 errors, 0 warnings | **PASS — `WTS01 session: 0 FAIL`** (all steps 1, 2a, 2b, 3a, 3b, 4, 5a, 5b, 6a, 6b, 7a, 7b, 7c PASS; 37.33 s) | **277/277 PASS** (0 failed; 444.46 s) | **PASS (nothing new broken)** |
| Clang 14.0.6 + libstdc++ 12 | Debug `-g0 -DADI_WITH_JUCE=OFF -DADI_WITH_PD=ON -DADI_WERROR=ON -DADI_ACCEPTANCE=ON` | **PASS** — 0 errors, 0 warnings | **PASS — `WTS01 session: 0 FAIL`** (all steps 1, 2a, 2b, 3a, 3b, 4, 5a, 5b, 6a, 6b, 7a, 7b, 7c PASS; 19.41 s) | **277/277 PASS** (0 failed; 453.76 s) | **PASS (nothing new broken)** |

- **What changed since our 23 November sweep (`7e3e627d`):** both GCC 12.2.0 and Clang 14.0.6 + libstdc++ 12 now build the entire headless tree (1060/1060 targets) with `-DADI_WERROR=ON` completely clean with zero workarounds, and all 277 headless suites plus `adi_acceptance_wts01` pass 100% on both compilers. No new warnings or regressions.

---

## 2026-11-24 — Queue item 2 (PR-BUILDS, Round 8): `window/shaper-panel-fixes` (`c10003c49`, `#716`)

Verified the window lane's newly requested branch `window/shaper-panel-fixes` (`c10003c49`, stacked on `#716` `1baf1fd8c`) on Debian 12 Linux x86_64 with JUCE 9.0.5 (`-DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON`, `-DJUCE_ALSA=0` since this VM lacks `libasound2-dev`).

| Branch | Head SHA | Compiler & Flags | Build / Warnings | Test Results | Owner / Verdict |
|---|---|---|---|---|---|
| `window/shaper-panel-fixes` | `c10003c49` | GCC 12.2.0, Debug `-DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON` (`-Wcast-align -Wconversion -Wsign-conversion -Werror`, `-DJUCE_ALSA=0`); DF-M62 (`FloatRealTransform.h:61`) patched locally in throwaway worktree because `#716`'s base `1baf1fd8c` predates DF-M62 | **0 warnings** across all 11 ADI-Shaper targets (`adi_shaper_wave_tests.cpp`, `adi_shaper_panel.cpp`, `adi_shaper_panel_session.cpp`, `adi_shaper_panel_view.cpp`, and all DSP/Pd/panel/session/view test TUs), `libadi_ui_shell.a`, `adi_ui_shell_tests`, `adi_ui_document_tests`, `adi_ui_window_state_tests`, and `ui_main.cpp.o`. (`adi_ui_seams_tests` skipped on GCC 12 because base `1baf1fd8c` predates DF-W39's `-fsanitize-trap` fix.) | **PASS (13/13 non-shell suites):** `adi_shaper_tests`, `adi_shaper_filter_tests`, `adi_shaper_sweep_tests`, `adi_shaper_drive_tests`, `adi_shaper_crush_tests`, `adi_shaper_time_tests`, `adi_shaper_pd_tests`, `adi_shaper_wave_tests`, `adi_shaper_panel_tests` (PASS; prints `juce_Component.cpp:3079` unshown-focus assertions already noted by window lane for clones), `adi_shaper_panel_session_tests`, `adi_shaper_panel_view_tests`, `adi_ui_document_tests` (**319/0**), `adi_ui_window_state_tests` (**129/0**). In `adi_ui_shell_tests`, `shaperWrench` **PASSED** (`[shell] begin shaperWrench` -> `end shaperWrench`); suite later fails/crashes in `browserDevices` due to base `#716`'s pre-fix fixture fault (already documented in `window.md`). | window + clones — **PASS (strict GCC `-Wconversion` casts and new panel/session/view/shaperWrench tests confirmed clean)** |
| `window/shaper-panel-fixes` | `c10003c49` | Clang 14.0.6 + libstdc++ 12, Debug `-g0 -DJUCE_ALSA=0 -DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON` | **0 warnings, 0 errors** on all 9 touched `.cpp` TUs (`adi_shaper_wave_tests.cpp.o`, `adi_shaper_panel.cpp.o`, `adi_shaper_panel_tests.cpp.o`, `adi_shaper_panel_session.cpp.o`, `adi_shaper_panel_session_tests.cpp.o`, `adi_shaper_panel_view.cpp.o`, `adi_shaper_panel_view_tests.cpp.o`, `ui_shell.cpp.o`, `test_ui_shell.cpp.o`). Full `adi_core` link skipped on Clang 14 because base `1baf1fd8c` predates DF-E69 (`src/adi/project_files.cpp:308`). | Compile-only verification of all touched TUs clean on Clang 14 `-Werror` | window + clones — **PASS** |

---

## 2026-11-24 — Engine review fixes on all four after-3-December PLAN branches (`SRF-09`, `SRF-10`, `PD-15`, `CO-01`)

Addressed every finding from win's 24 November engine review in `collab/win.md` (`aef709ad6`) across all four after-3-December PLAN branches, rebased/merged on `main` (`a44834a98`), and re-verified on GCC 12.2, Clang 14.0.6, ASan+LSan+UBSan, and TSan. All four branches stay strictly within their loaned paths (`collab/prompts/2026-11-22-gemini-queue.md`) and land after 3 December (`ADR-0252 d4`).

### 1. `gemini/srf09-osc-core` (`94ba8bce4`, merged `main` `a44834a98`)

- **Files touched (loaned paths only):** `src/adi/osc_codec.hpp`/`.cpp`, `src/adi/osc_namespace.hpp`/`.cpp`, `tests/test_osc_codec.cpp`, `tests/test_osc_namespace.cpp`, `tests/fuzz_osc_codec.cpp`, `CMakeLists.txt`.
- **Review fixes:**
  1. **Address unification with SRF-10:** `/adi/track/{n}`, `/adi/track/{n}/send/{s}`, `/adi/track/{n}/device/{d}/param/{p}`, `/adi/master/device/{d}/param/{p}`, and `/adi/focused/device/param/{p}` all parse canonical positive 64-bit stable ids (`std::uint64_t`) matching SRF-10 OSCQuery `FULL_PATH`s (non-canonical forms such as leading zeros or `0` are refused).
  2. **Nudge echo suppression:** `canonicalFeedAddress()` strips `"/nudge"` so nudges suppress the actual feed-bearing parameter/dial address instead of a non-feed address; gesture addresses return empty so they never poison feed suppression.
  3. **Refused / clamped / nudged / unchanged confirmation:** added `confirmNextFlush_` tracking so refused (`InvalidArgument` or sink-refused), clamped, nudged, and unchanged writes suppress same-flush echo and confirm the true settled feed value back to the sender on the next flush.
  4. **Medium findings:** added explicit `/gesture` addresses plus automatic begin/end gesture bracketing on continuous strip/param/macro/dial writes (`ADR-0181 d2`) with `endClientGestures()` / `endAllGestures()`; added `BindPolicy` / `validateBindPolicy()` refusing wildcard binds (`0.0.0.0`, `::`, `*`), privileged ports (`< 1024`), and non-loopback addresses without `trustedNetworkAcknowledged` (`ADR-0185 d4`, `ADR-0188 d7`); added message-thread ownership contract (`ownerThread_`, `adoptCurrentThread()`, failing closed with `DispatchStatus::WrongThread`).
- **Verification (`94ba8bce4`):**

| Compiler / tool | Flags | Suites & counts | Verdict |
|---|---|---|---|
| GCC 12.2.0 | `-DADI_WERROR=ON` (Debug, full CMake + standalone) | `adi_osc_codec_tests` **208/0**, `adi_osc_namespace_tests` **379/0** (0 warnings) | PASS |
| Clang 14.0.6 + libstdc++ 12 | `-DADI_WERROR=ON` (Debug, full CMake + standalone) | `adi_osc_codec_tests` **208/0**, `adi_osc_namespace_tests` **379/0** (0 warnings) | PASS |
| GCC 12.2.0 ASan+LSan+UBSan | `-O1 -g1 -fsanitize=address,undefined` (`detect_leaks=1:alloc_dealloc_mismatch=1`) | `adi_osc_codec_tests` **208/0**, `adi_osc_namespace_tests` **379/0** (0 reports) | PASS |
| GCC 12.2.0 TSan | `-O1 -g1 -fsanitize=thread` (`setarch x86_64 -R`) | `adi_osc_namespace_tests` **379/0** (0 races) | PASS |

**Review requested: `gemini/srf09-osc-core` at `94ba8bce4`** (lands after 3 December; PR requested only after engine passes this head).

---

### 2. `gemini/srf10-oscquery-core` (`13717f7cd`, stacked on `gemini/srf09-osc-core` `94ba8bce4`)

- **Files touched (loaned paths only):** `src/adi/oscquery.hpp`/`.cpp`, `tests/test_oscquery.cpp`, `tests/test_oscquery_http.cpp`, `tests/fuzz_oscquery.cpp`, `CMakeLists.txt` (plus stacked SRF-09 files).
- **Review fixes:**
  1. **Blocker (MSVC C1060):** replaced the deeply nested `Json{...}` initializer list in `tests/test_oscquery.cpp` with a flat raw-string `Json::parse(R"json(...)json")` golden and split the HTTP / `?HOST_INFO` / `Host` checks into `tests/test_oscquery_http.cpp`.
  2. **Blocker (address split with SRF-09):** unified 1-to-1 with SRF-09 stable 64-bit ids and added `testSrf09AddressParity()` verifying every leaf `FULL_PATH` emitted by `build()` resolves through `adi::osc::resolveAddress()`.
  3. **Major (UTF-8 throw):** replaced every `.dump()` call in `src/adi/oscquery.cpp` with `j.dump(-1, ' ', false, Json::error_handler_t::replace)` so invalid UTF-8 bytes emit U+FFFD and never throw.
  4. **Major (DNS rebinding):** `parseHttpRequest()` now requires a single syntactically valid `Host` header (400 on missing, duplicate, or malformed `Host`), and `handleHttpRequest()` enforces `isHostAllowed()` against loopback, `oscIp`, and `allowedHosts` (plus `httpPort` when configured), returning `403 Forbidden` on disallowed hosts.
- **Verification (`13717f7cd`):**

| Compiler / tool | Flags | Suites & counts | Verdict |
|---|---|---|---|
| GCC 12.2.0 | `-DADI_WERROR=ON` (Debug, full CMake + standalone) | `adi_oscquery_tests` **123/0** (0 warnings) | PASS |
| Clang 14.0.6 + libstdc++ 12 | `-DADI_WERROR=ON` (Debug, full CMake + standalone) | `adi_oscquery_tests` **123/0** (0 warnings) | PASS |
| GCC 12.2.0 ASan+LSan+UBSan | `-O1 -g1 -fsanitize=address,undefined` (`detect_leaks=1:alloc_dealloc_mismatch=1`) | `adi_oscquery_tests` **123/0** (0 reports) | PASS |
| Clang 14.0.6 / GCC 12.2.0 fuzz harness | standalone `tests/fuzz_oscquery.cpp` + `src/adi/oscquery.cpp` alone (`-Werror` on Clang 14; ASan+LSan+UBSan seed replay on GCC 12) | 0 warnings, 0 ASan/LSan/UBSan reports | PASS |

**Review requested: `gemini/srf10-oscquery-core` at `13717f7cd`** (stacked on `gemini/srf09-osc-core` `94ba8bce4`; lands after 3 December; PR requested only after engine passes this head).

---

### 3. `gemini/pd15-project-paste` (`81e487a25`, merged `main` `a44834a98`)

- **Files touched (loaned paths only):** `src/adi/project_paste.hpp`/`.cpp`, `tests/test_project_paste.cpp`, `CMakeLists.txt`.
- **Review fixes:**
  1. **ADR-0068 d5 in-place media reference:** zero files are copied into `dest/audio/`; pasted `media_files` rows record the resolved `abs_path_hint` and `rel_path = NULL` (preserving `missing = 1` with the resolved `abs_path_hint` when absent on disk so `dest` relinks by hash).
  2. **Strict BLAKE3 hash validation:** refuses empty or malformed `hash_blake3` (requires 64 hex chars) explicitly — never deduplicates on `""`.
  3. **Take lanes:** copies `lanes WHERE kind = 'take'` via `createTakeLane()` (in reverse `ord` order to preserve exact ordering) + `lane.setMute`, and remaps `clips.lane_id` so take-lane clips stay on their take lanes.
  4. **Group tracks:** pasting a group track recursively pastes its full descendant subtree (`parent_id` and `index_in_parent` remapped) along with `group_summing` and `return_send_taps`.
  5. **Alias links:** preserves internal `alias_of` links across the pasted track/group (topological order: master before alias); clears external alias links and reports them in `result.warnings`.
  6. **Track tables & sends:** copies `mixer_pan_settings` (split pan), `track_recording_options`, and `routing` sends + `routing_send_taps` (mapping to pasted or unique name-matched destination returns); reports unmappable sends and non-default `mixer_strip.pan_law` (no `mixer.setPanLaw` op exists in `ops_catalog.cpp` yet) in `result.warnings` instead of dropping silently.
  7. **Tests:** expanded `tests/test_project_paste.cpp` to cover MIDI (`event_streams` + `note_expression`), take lanes, group subtree + summing, internal/external aliases, split pan, recording options, sends, empty-hash refusal, and all refusals (read-only dest, missing track, master track, nested rack, hash mismatch, audio thread).
- **Verification (`81e487a25`):**

| Compiler / tool | Flags | Suites & counts | Verdict |
|---|---|---|---|
| GCC 12.2.0 | `-DADI_WERROR=ON` (Debug) | `adi_project_paste_tests` **112/0** (0 warnings) | PASS |
| Clang 14.0.6 + libstdc++ 12 | `-DADI_WERROR=ON` (Debug) | `adi_project_paste_tests` **112/0** (0 warnings) | PASS |
| GCC 12.2.0 ASan+LSan+UBSan | `-O1 -g1 -fsanitize=address,undefined -DADI_WERROR=OFF` (`detect_leaks=1:alloc_dealloc_mismatch=1`) | `adi_project_paste_tests` **112/0** (0 reports) | PASS |
| GCC 12.2.0 TSan | `-O1 -g1 -fsanitize=thread -DADI_WERROR=OFF` (`setarch x86_64 -R`) | `adi_project_paste_tests` **112/0** (0 races) | PASS |

**Review requested: `gemini/pd15-project-paste` at `81e487a25`** (lands after 3 December; PR requested only after engine passes this head).

---

### 4. `gemini/co01-collab-core` (`4de5cc8ec`, merged `main` `a44834a98`)

- **Files touched (loaned paths only):** `src/adi/collab/batch.hpp`/`.cpp`, `src/adi/collab/fractional_key.hpp`/`.cpp`, `src/adi/collab/fake_object_store.hpp`/`.cpp`, `src/adi/collab/replica.hpp`/`.cpp`, `tests/test_collab_fractional.cpp`, `tests/test_collab_sync.cpp`, `CMakeLists.txt`.
- **Review fixes:**
  1. **Bounded hostile CBOR validation:** added iterative, zero-recursion `validateCborLimits()` before `decodeFromLog()` capping packet size, nesting depth (32 across arrays/maps/tags), and item count (65536); 10,000-deep hostile CBOR is rejected without stack overflow.
  2. **Strict non-narrowing integer decoding:** `readStrictUint64`, `readStrictInt64`, and `readStrictUint32` refuse floats, booleans, negatives in unsigned fields, and `> 32`-bit overflow in `idSpace`.
  3. **Non-throwing op apply:** `updateOrderKeysForOp()` and `applyBatchToStore()` / `syncStep()` are `noexcept` against malformed op payloads, rejecting bad payloads deterministically instead of throwing out of `syncStep()`.
  4. **Verified rewind:** `rewindPending()` verifies every pending `localTxnId` against `history.nextUndo()->txnId` before calling `history.undo()`, failing with `SyncStatus::Error` rather than undoing unrelated user edits.
  5. **Restart persistence:** persists `appliedSeq`, `localLamport`, `localBatchCounter`, `vector`, `baseOrderKeys`, `appliedBatchIds`, and `allocatorCounters` in Layer-3 `session_state` (excluded from `digestProject()`, no schema change) so restarted replicas preserve exact fractional order keys and monotonic batch/row ids.
  6. **No wedge on corrupt sequenced slot:** one corrupt, malformed, or invalid-id-space sequenced slot is skipped/rejected deterministically (`skippedBadBatchCount` / `rejectedRemoteBatchCount`, `appliedSeq_` advances) so sync never wedges.
  7. **Remote id-space validation:** `validateBatchIdSpace()` enforces `idSpaceOf(id) == batch.idSpace && idCounterOf(id) > 0` on all row-creating ops in batches.
  8. **Remote-commit seam:** added injectable `RemoteCommitHook` on `CollabReplica` so the core delegates remote commits cleanly once the engine lands `CommitOptions::remote` outside our loan.
- **Verification (`4de5cc8ec`):**

| Compiler / tool | Flags | Suites & counts | Verdict |
|---|---|---|---|
| GCC 12.2.0 | `-DADI_WERROR=ON` (Debug) | `adi_collab_fractional_tests` **692/0**, `adi_collab_sync_tests` **65/0** (0 warnings) | PASS |
| Clang 14.0.6 + libstdc++ 12 | `-DADI_WERROR=ON` (Debug) | `adi_collab_fractional_tests` **692/0**, `adi_collab_sync_tests` **65/0** (0 warnings) | PASS |
| GCC 12.2.0 ASan+LSan+UBSan | `-O1 -g1 -fsanitize=address,undefined -DADI_WERROR=OFF` (`detect_leaks=1:alloc_dealloc_mismatch=1`) | `adi_collab_fractional_tests` **692/0**, `adi_collab_sync_tests` **65/0** (0 reports) | PASS |
| GCC 12.2.0 TSan | `-O1 -g1 -fsanitize=thread -DADI_WERROR=OFF` (`setarch x86_64 -R`) | `adi_collab_sync_tests` **65/0** (0 races) | PASS |

**Review requested: `gemini/co01-collab-core` at `4de5cc8ec`** (lands after 3 December; PR requested only after engine passes this head).

### Needs from engine (outside our loaned paths)

- **For `PD-15`:** a `mixer.setPanLaw` op in `ops_catalog.cpp` so non-default `mixer_strip.pan_law` values can be pasted through the command layer (today `project_paste.cpp` preserves the default and reports non-default `pan_law` in `result.warnings`).
- **For `CO-01` (per queue `Not yours`):**
  1. `CommitOptions::remote` in the store/history layer (writing remote author `op_clocks` and retiring superseded unsequenced rows on rebase; `CollabReplica` already exposes `RemoteCommitHook` to plug it in).
  2. Id-space integration in `row_ids.*` (the upper-32-bit `idSpace` + lower-32-bit counter allocator currently lives in `src/adi/collab/batch.*`).
  3. Machine-local column exclusion in `digest.cpp` and schema minor reservation for `order_keys` / `sync_vectors` if promoted from Layer-3 `session_state` to Layer-1 tables.

---

## 2026-11-24 — 24 November Linux proofs: remaining six fix branches (Items 2b–2g)

All six remaining branches from win's 24 November prompt verified on Debian 12 (Linux x86_64, GCC 12.2.0 and Clang 14.0.6 with libstdc++ 12; JUCE 9.0.5 `d60b7a780` with `-DJUCE_ALSA=0` for the JUCE-ON check). Item 1 (`land/window-1124`) and Item 2a (`window/df-w40-clang14-lambda` `37e26e69`) were logged at `d3e39fa2`.

| Item | Branch | Head SHA | Compiler & Flags | Suites / Counts | Verdict |
|---|---|---|---|---|---|
| 2b | `mixview/df-m63-m64-test-ub` | `1245f211` | GCC 12.2, Debug `-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer`, `ASAN_OPTIONS=detect_leaks=1:alloc_dealloc_mismatch=1:halt_on_error=1`, `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` | `adi_mix_gate_tests`: **371/0** (2,670,592 samples, 0/0 callback allocs/deallocs); `adi_mix_opto_gain_tests`: **309/0** (0/0 callback allocs/deallocs); 0 ASan/LSan/UBSan reports | **PASS — DF-M63 & DF-M64 confirmed fixed** |
| 2b | `mixview/df-m63-m64-test-ub` | `1245f211` | GCC 12.2, Debug `-g0 -D_GLIBCXX_DEBUG -DADI_WERROR=ON` | `adi_mix_gate_tests`: **371/0**; `adi_mix_opto_gain_tests`: **309/0**; 0 aborts, 0 warnings | **PASS** |
| 2c | `devices/df-e67a-operator-new-forms` | `de0e353a` | GCC 12.2, Debug `-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer`, `ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=1:halt_on_error=1`, `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` | `adi_replace_operator_new_tests`: **39/0**; `adi_builtin_devices_tests`: **246/0**; `adi_oneshot_tests`: **359/0**; `adi_pd_native_devices_tests`: **287/0**; `adi_sampler_tests`: **119/0**; 0 `alloc-dealloc-mismatch` / ASan / UBSan reports | **PASS — DF-E67 (a) confirmed fixed** |
| 2d | `mixview/df-m66-focus-mailbox-checks` | `b8daf1d5` | GCC 12.2, Release `-O3 -DNDEBUG -DADI_WERROR=OFF` (see note below; GCC 12.2 Debug `-g0 -DADI_WERROR=ON` also compiled clean) | `adi_mix_focus_tests`: **200/200 runs PASS** (480 checks, 0 failures, 0/0 callback allocs/deallocs per run) | **PASS — DF-M66 confirmed fixed** |
| 2d | `mixview/df-m66-focus-mailbox-checks` | `b8daf1d5` | Clang 14.0.6 + libstdc++ 12, Release `-O3 -DNDEBUG -DADI_WERROR=ON` | `adi_mix_focus_tests`: **200/200 runs PASS** (480 checks, 0 failures, 0/0 callback allocs/deallocs per run, 0 warnings) | **PASS (400/400 total runs green)** |
| 2e | `mixview/df-m67-limiter-unchanged-prepare` | `2808100e` | GCC 12.2, Debug `-g0 -DADI_WERROR=ON` (0 ADI warnings) | `adi_core` + `adi_mix_limiter_tests` **284/0**, `adi_mix_functions_tests` **44/0**, `adi_mix_compressor_host_tests` **110/0** (0/0 callback allocs), `adi_mix_eq_host_tests` **941/0**, `adi_builtin_devices_tests` **250/0** | **PASS — DF-M67 & DF-M36 confirmed** |
| 2e | `mixview/df-m67-limiter-unchanged-prepare` | `2808100e` | Clang 14.0.6 + libstdc++ 12, Debug `-g0 -DADI_WERROR=ON` (0 warnings) | `adi_core` + `adi_mix_limiter_tests` **284/0**, `adi_mix_functions_tests` **44/0**, `adi_mix_compressor_host_tests` **110/0** (0/0 callback allocs), `adi_mix_eq_host_tests` **941/0**, `adi_builtin_devices_tests` **250/0** | **PASS** |
| 2f | `mixview/df-m65-function-host-fp-contract` | `39242769` | GCC 12.2 & Clang 14.0.6 + libstdc++ 12, Debug `-g0 -DADI_WERROR=ON` (0 warnings on both) | `adi_core` + `adi_mix_functions_tests` **44/0**, `adi_mix_limiter_tests` **280/0**, `adi_mix_compressor_host_tests` **110/0** on both compilers; `compile_commands.json` confirmed `-ffp-contract=off` on `src/juce/mix_function_host.cpp`, `src/juce/mix_compressor_host.cpp`, `tests/test_mix_limiter.cpp`, `tests/test_mix_compressor_host.cpp` on both compilers | **PASS (Linux build + flag verified; macOS arm64 leg is the runtime proof)** |
| 2g | `engine/df-e17a-move-overlap` | `98ee01f8` | GCC 12.2, Debug `-g0 -DADI_WERROR=ON` (headless, 0 warnings) | `adi_clip_edit_tests` **446/0**, `adi_take_lane_ops_tests` **82/0**, `adi_edit_plan_tests` **680/0**, `adi_ops_tests` **140/0**, `adi_history_tests` **148/0** (`adi_ui_seams_tests` hits pre-`#739` DF-W39 `-fsanitize-trap=signed-integer-overflow` on GCC 12 because `98ee01f8` predates `#739`; fixed on `main` `b0f3248f`) | **PASS** |
| 2g | `engine/df-e17a-move-overlap` | `98ee01f8` | Clang 14.0.6 + libstdc++ 12, Debug `-g0 -DADI_WERROR=ON` (headless, 0 warnings) | `adi_clip_edit_tests` **446/0**, `adi_ui_seams_tests` **581/0**, `adi_take_lane_ops_tests` **82/0**, `adi_edit_plan_tests` **680/0**, `adi_ops_tests` **140/0**, `adi_history_tests` **148/0** | **PASS** |
| 2g | `engine/df-e17a-move-overlap` | `98ee01f8` | Clang 14.0.6 + libstdc++ 12, Debug `-g0 -DJUCE_ALSA=0 -DADI_WITH_JUCE=ON -DADI_WERROR=ON` | `CMakeFiles/adi_ui_shell.dir/src/juce/ui_shell.cpp.o` compiled clean (**0 errors, 0 warnings**) | **PASS** |

### New finding noted during Item 2d (not fixed; reported for Mix View)
- **`tests/test_mix_focus.cpp:1325`** (owner: Mix View): GCC 12.2 Release (`-O3 -DADI_WERROR=ON`) emits `-Werror=mismatched-new-delete` (`'void free(void*)' called on pointer returned from a mismatched allocation function`) when inlining `std::vector<double>` deallocation into the inline `void operator delete(void* p) noexcept { ... std::free(p); }` defined directly in `test_mix_focus.cpp` (same family as DF-E66 / DF-E67 (a); `test_mix_focus.cpp` does not yet use `replace_operator_new.hpp`). Debug (`-O0`) on GCC 12.2 and Release (`-O3`) on Clang 14.0.6 both compile with `-DADI_WERROR=ON` without warning. Repro: `cmake -B b -DCMAKE_BUILD_TYPE=Release -DADI_WITH_JUCE=OFF -DADI_WERROR=ON -DCMAKE_CXX_COMPILER=g++ && cmake --build b --target adi_mix_focus_tests`.

### Next up
- Win's engine reviews of our four after-3-December PLAN branches (`gemini/srf09-osc-core`, `gemini/srf10-oscquery-core`, `gemini/pd15-project-paste`, `gemini/co01-collab-core`) are now on `main` in `collab/win.md` (`aef709ad6`). Addressing every review finding on those four branches next (highest priority per standing queue before requesting re-review; all land after 3 December).

---

## 2026-11-24 — Queue item 1 (`PR #739` `land/window-1124`) and item 2a (`window/df-w40-clang14-lambda`)

**For win.** Verified PR `#739` (`land/window-1124`, initial head `51614964` and updated head `357a64d0`) and DF-W40 (`window/df-w40-clang14-lambda` at `37e26e69`) on Linux with pinned JUCE 9.0.5 (`d60b7a780`, fetched via `adi_daw/tools/fetch_external.sh --with-juce`). Note: this Debian 12 VM has GCC 12.2.0 and Clang 14.0.6 with libstdc++ 12 (**no GCC 13** installed, so the GCC 13 leg for DF-E71 and `adi_play` could not be run here) and lacks `libasound2-dev`, so JUCE-ON builds pass `-DJUCE_ALSA=0` alongside `-g0 -DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON`.

### 1. PR `#739` — `land/window-1124` (`51614964` -> `357a64d0`)

- **Validator (`357a64d0`):** `python3 adi_daw/tools/validate_parity.py --self-test` -> **PASS** (`self-test: passes on the tree, fails on a planted bad tag`).
- **GCC 12.2.0 (`-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-g0" -DCMAKE_CXX_FLAGS_DEBUG="-g0 -DJUCE_ALSA=0" -DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON`):**
  - Build: **PASS** (`558/558` on `51614964`, `557/557` incremental on `357a64d0`; 0 warnings across `adi_ui_shell_tests`, `adi_ui_seams_tests`, `adi_ui_document_tests`, `adi_ui_window_state_tests`, `adi_play_render_tests`, `adi_play_quantize_tests`, and `adi_app`).
  - Test runs (`env -u DISPLAY`):
    - `adi_ui_shell_tests` (`357a64d0`): **PASS — 3157 checks, 0 failures** (`juce_XWindowSystem_linux.cpp:2703` counted apart as expected; note earlier head `51614964` failed 1/3157 on that assertion before `357a64d0` updated `ui_test_hygiene.hpp`).
    - `adi_ui_seams_tests` (`51614964`; unchanged in `357a64d0`): **PASS — 580 checks, 0 failures** (DF-W39 confirmed: CMake selected `-fsanitize-undefined-trap-on-error`).
    - `adi_ui_document_tests` (`51614964`; unchanged in `357a64d0`): **PASS — 343 checks, 0 failures**.
    - `adi_ui_window_state_tests` (`51614964`; unchanged in `357a64d0`): **PASS — 177 checks, 0 failures**.
    - `adi_play_quantize_tests` (`51614964`; unchanged in `357a64d0`): **PASS — 40 checks, 0 failures**.
    - `adi_play_render_tests` (`51614964`; unchanged in `357a64d0`): **1 expected FAIL (DF-E76)** — `PASS -- known VST3 identities and paths require no discovery during instance load`, then `JUCE Assertion failure in juce_VST3PluginFormatImpl.h:1117` / `FAIL -- same canonical module projects saved alias spelling without rewriting its exact VST3 UID`.
- **Clang 14.0.6 + libstdc++ 12 (`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-g0" -DCMAKE_CXX_FLAGS_DEBUG="-g0 -DJUCE_ALSA=0" -DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON`, `ninja -k 0` on `51614964`):**
  - Build: **`700/703` PASS (0 warnings)**; the **sole** compile error is the known **DF-W40** structured-binding lambda capture at `src/juce/ui_devices.cpp:1016:41` (`reference to local binding 'metadata'`), which blocks linking `adi_ui_shell`, `adi_ui_shell_tests`, and `adi_app` until `window/df-w40-clang14-lambda` lands.
  - Test runs (`env -u DISPLAY`) on all suites that linked:
    - `adi_ui_seams_tests`: **PASS — 580 checks, 0 failures**.
    - `adi_ui_document_tests`: **PASS — 343 checks, 0 failures**.
    - `adi_ui_window_state_tests`: **PASS — 177 checks, 0 failures**.
    - `adi_play_quantize_tests`: **PASS — 40 checks, 0 failures**.
    - `adi_play_render_tests`: **1 expected FAIL (DF-E76)** (`alias.vst3`, identical to GCC 12.2).
- **Verdict (`land/window-1124` at `357a64d0`):** **PASS** on GCC 12.2 (all 5 non-DF-E76 suites + `validate_parity.py --self-test` green; `adi_play_render_tests` hits only known DF-E76) and **PASS modulo DF-W40** on Clang 14.0.6 + libstdc++ 12. GCC 13 leg unavailable on this VM.

### 2a. `window/df-w40-clang14-lambda` (`37e26e69`) — DF-W40 proof

- **Build:** Clang 14.0.6 + libstdc++ 12 (`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-g0" -DCMAKE_CXX_FLAGS_DEBUG="-g0 -DJUCE_ALSA=0" -DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON`).
- **Result:** `src/juce/ui_devices.cpp`, `libadi_ui_shell.a`, `ADI` (`adi_app`), and `adi_ui_shell_tests` all compiled and linked with **0 errors, 0 warnings** (`118/118` incremental steps).
- **Test run (`env -u DISPLAY`):** `adi_ui_shell_tests` **PASS — 3098 checks, 0 failures** (`RC=0`).
- **Verdict:** **DF-W40 confirmed fixed** on Clang 14.0.6 + libstdc++ 12 JUCE-ON `-DADI_WERROR=ON`.

(No PR requested; run-only verification.)

---

## 2026-11-23 — DF-E65 (ThreadSanitizer) and DF-E69 (Clang 14 + libstdc++ 12) closeout verification on `main` (`01c70983`, post-`a0dbb568`)

**For win (to close DF-E65 and DF-E69).** Both fixes landed in `#735` (`cb16116b`), verified on `origin/main` at `01c70983` (descendant of `a0dbb568`), with no local patches (DF-M62 is on `main` via `#733`).

### 1. DF-E65 — ThreadSanitizer on `adi_progressive_decode_tests` and `adi_pcm_stream_tests`

- **Build:** GCC 12.2.0 (`-DCMAKE_BUILD_TYPE=None -DCMAKE_C_FLAGS="-O1 -g1 -fsanitize=thread" -DCMAKE_CXX_FLAGS="-O1 -g1 -fsanitize=thread" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`). Confirmed `src/adi/audio/decode_libs.c` (`stb_vorbis.c`) compiled with `-O1 -g1 -fsanitize=thread`.
- **Run (`TSAN_OPTIONS="halt_on_error=1:exitcode=66"`, `setarch x86_64 -R`):**

| Suite | Repeats | Checks per run | TSan reports | Result |
|---|---:|---:|---:|---|
| `adi_pcm_stream_tests` | 5 / 5 | 54 checks, 0 failures | 0 | **PASS** |
| `adi_progressive_decode_tests` | 5 / 5 | 100 checks, 0 failures | 0 | **PASS** |

- **Verdict:** **DF-E65 confirmed fixed** — the `stb_vorbis` `crc_table` data race is gone under ThreadSanitizer; ready for win to close.

### 2. DF-E69 — Clang 14.0.6 + libstdc++ 12 (`-DADI_WERROR=ON`) on all 11 named units

- **Build:** Clang 14.0.6 against Debian 12 libstdc++ 12 (`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-g0" -DCMAKE_CXX_FLAGS_DEBUG="-g0" -DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`, zero local patches). Built `adi_core` and all 7 test targets covering the 11 DF-E69 units (`462/462` ninja steps, **0 errors, 0 warnings**).
- **All 11 DF-E69 units verified compiled to object files:**
  - **(a) `timeline_clips.hpp` (8 units):** `src/adi/engine/automation_clips.cpp`, `src/adi/engine/clip_playback.cpp`, `src/adi/engine/gain_stage.cpp`, `src/adi/engine/midi_clips.cpp`, `src/adi/engine/midi_controllers.cpp`, `src/adi/render/freeze_capture.cpp`, `src/adi/freeze_commands.cpp`, `tests/test_take_lane_ops.cpp`.
  - **(b) structured-binding lambda captures (4 units; `midi_clips.cpp` shared with (a)):** `src/adi/engine/midi_clips.cpp`, `src/adi/engine/param_ops.cpp`, `src/adi/project_files.cpp`, `tests/test_gain_stage.cpp`.
- **CTest on the Clang 14.0.6 build (`7/7` PASS, `35.27 s`):** `adi_project_files_tests` (1.48 s), `adi_param_edits_tests` (0.38 s), `adi_take_lane_ops_tests` (0.55 s), `adi_midi_clips_tests` (8.76 s), `adi_gain_stage_tests` (4.21 s), `adi_freeze_commands_tests` (9.11 s), `adi_clip_playback_tests` (35.26 s).
- **Verdict:** **DF-E69 confirmed fixed** on Clang 14.0.6 + libstdc++ 12 `-DADI_WERROR=ON`; ready for win to close. (The separate window JUCE-tier structured-binding site remains tracked under DF-W40.)

(No PR requested; run-only closeout verification.)

---

## 2026-11-23 — Queue item 2 (PR-BUILDS, Round 7): all 17 remaining unmerged `engine` maximal tips on GCC 12.2 and Clang 14.0.6 `-DADI_WERROR=ON`

**For win and the engine seat.** Verified all 17 remaining live `engine/*` PR-requested maximal tips sitting outside `land/engine-1123` / `land/engine-1123b` on Linux GCC 12.2.0 `-DADI_WERROR=ON` (`-g0`, Pd on, JUCE off, transient local DF-M62 patch) and Clang 14.0.6 `-Werror` across every changed headless TU, plus affected CTest suites.

### Summary table

| Branch | SHA | GCC 12.2 `-DADI_WERROR=ON` | Clang 14.0.6 `-Werror` (changed headless TUs) | CTest (affected suites) | Owner / Notes |
|---|---|---|---|---|---|
| `engine/au08-simplify-envelope` | `be846d1b` | **PASS** (0 warnings) | **PASS** (6/6 TUs) | **PASS** (12/12) | `engine` — clean |
| `engine/au08-envelope-clipboard` | `9ad32e5e` | **PASS** (0 warnings) | **PASS** (5/5 TUs) | **PASS** (12/12) | `engine` — clean |
| `engine/au08-clear-clip-envelope` | `75ae50ef` | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (12/12) | `engine` — clean |
| `engine/tm-ls03-midi-transport-edges` | `eb545c1e` | **PASS** (0 warnings) | **PASS** (6/6 TUs) | **PASS** (10/10) | `engine` — clean |
| `engine/tm-ic04-otio-linear-writer` | `2f414469` | **PASS** (0 warnings) | **PASS** (5/5 TUs) | **PASS** (4/4) | `engine` — clean |
| `engine/ex02-via-main-routing` | `99c36e2e` | **PASS** (0 warnings) | **PASS** (6/6 TUs) | **PASS** (9/9) | `engine` — clean |
| `engine/pd14-project-templates` | `ca5cd357` | **PASS** (0 warnings) | **PASS on new code** (2/3 TUs; `project_files.cpp:386` is pre-existing DF-E69 from `main:308`) | **PASS** (9/9) | `engine` — clean |
| `engine/tm-fc01-bounce-mixer` | `8bc053ca` | **PASS** (0 warnings) | **PASS** (4/4 TUs) | **PASS** (4/4) | `engine` — clean |
| `engine/mx33-sync-spikes` | `53fad1c3` | **PASS** (0 warnings) | **PASS** (7/7 TUs) | **PASS** (3/3) | `engine` — clean |
| `engine/srf04-controller-nudge` | `45e69a22` | **PASS** (0 warnings) | **PASS** (3/3 TUs) | **PASS** (12/12) | `engine` — clean |
| `engine/pd04-analysis-publication` | `da074f14` | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (4/4) | `engine` — clean |
| `engine/tm-md04-wave-metadata` | `d59778d7` | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (4/4) | `engine` — clean |
| `engine/tm-tt01-tempo-ramps` | `8f0705db` | **PASS** (0 warnings) | **FAIL** (25/29 TUs; 4 fail on inherited `timeline_clips.hpp:13` + pre-existing DF-E69 `midi_clips.cpp:656`) | **FAIL** (50/51: **`adi_transport_info_tests` 6 failures**) | `engine` — finding 1 below |
| `engine/tm-sv01-launch-quantization` | `a676ca80` | **PASS** (0 warnings) | **PASS** (9/9 TUs) | **PASS** (11/11) | `engine` — clean |
| `engine/dev28-main-loudness-meter` | `f8308a55` | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (1/1) | `engine` — clean |
| `engine/dev13-node-segment-quantum` | `59453a16` | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (4/4) | `engine` — clean |
| `engine/dev17-note-identity` | `4af8d018` | **PASS** (0 warnings) | **FAIL** (8/10 TUs; 2 fail on inherited `timeline_clips.hpp:13` + pre-existing DF-E69 `midi_clips.cpp:662`; 3 JUCE-off skipped) | **FAIL** (19/20: **`adi_midi_capture_tests` 3 failures**) | `engine` — finding 2 below |

### New findings (do not fix here; for win / engine)

1. **CTest failure on `engine/tm-tt01-tempo-ramps` (`8f0705db`) — `adi_transport_info_tests` (owner: `engine`)**
   - 6 failed assertions in `adi_transport_info_tests` (`50/51` other affected suites passed):
     - `FAIL stored ramp uses step BPM including change boundary` (×3)
     - `FAIL known bar/beat at tempo change`
     - `FAIL known bar/beat at truncated-bar meter change`
     - `FAIL quarter remainder independent of signature denominator`
   - Smallest repro: checkout `8f0705db`, build and run `adi_transport_info_tests` (Linux GCC 12.2 Debug).

2. **CTest failure on `engine/dev17-note-identity` (`4af8d018`) — `adi_midi_capture_tests` (owner: `engine`)**
   - 3 failed assertions in `adi_midi_capture_tests` (`19/20` other affected suites passed):
     - `FAIL: stored ANOT retains original full uint64 identities and exact source/release velocity words` (×2)
     - `FAIL: native Capture validates binary records, addresses, IDs and closed packets and rolls back every late refusal`
   - Smallest repro: checkout `4af8d018`, build and run `adi_midi_capture_tests` (Linux GCC 12.2 Debug).

(No PR requested; run-only verification round.)

---

## 2026-11-23 — Queue item 2 (PR-BUILDS, Round 6): after-3-December combined tips and remaining standalone heads (`mixview`, `clones`, `devices`, `engine-win`, `window`, `window-fn`)

**For win and seat owners.** Verified 17 remaining PR-requested maximal tips sitting outside `land/*-1123b` on Linux GCC 12.2.0 `-DADI_WERROR=ON` (`-g0`, Pd on, JUCE off) and Clang 14.0.6 `-Werror` across every changed headless TU, plus affected CTest suites. Transient local DF-M62 patch (`FloatRealTransform.h:61`) was applied ONLY where DF-M62 was absent (`mixview/review-scope-lands-after-2026-12-03` built with **no** local DF-M62 patch).

### Summary table

| Branch | SHA | DF-M62 local patch | GCC 12.2 `-DADI_WERROR=ON` | Clang 14.0.6 `-Werror` (changed headless TUs) | CTest (affected suites) | Owner / Notes |
|---|---|---|---|---|---|---|
| `mixview/review-scope-lands-after-2026-12-03` | `58817912` | **None** (`d22b81c1` in stack) | **PASS** (0 warnings) | **FAIL** (10/11 TUs; **NEW** `shared_analysis.cpp:561:19`; 9 JUCE-off skipped) | **PASS** (14/14) | `mixview` — finding 1 below |
| `clones/shaper-filter-steep` | `5fff5d0a` | Yes | **PASS** (0 warnings) | **PASS** (5/5 TUs) | **PASS** (6/6) | `clones` — clean |
| `devices/amxd-audit` | `9fc81e1d` | Yes | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (1/1: `adi_pd_amxd_tests`) | `devices` — clean |
| `devices/pd-midi-out` | `52782524` | Yes | **PASS** (0 warnings) | **PASS** (3/3 TUs) | **PASS** (2/2: `adi_engine_tests`, `adi_pd_engine_tests`) | `devices` — clean |
| `devices/spectral-resonator` (full DEV-22 after-3-Dec stack) | `9a6ea54f` | Yes | **FAIL** (`tests/test_vinyl_distortion.cpp:178:68` `-Werror=sign-conversion`; `ninja -k 0` confirms 0 other GCC errors across all 86 TUs) | **PASS** (86/86 TUs) | SKIP (build failed) | `devices` — finding 2 below |
| `engine-win/export-profile-validation` (combined export stack) | `2b3b7e5c` | Yes | **FAIL** (`tests/test_render_queue.cpp:827,873` `-Werror=unused-function`) | **FAIL** (14/15 TUs; same `tests/test_render_queue.cpp:827,873`) | SKIP (build failed) | `engine-win` — finding 3 below |
| `engine-win/source-hash-flac-proof` | `ab4fab82` | Yes | **PASS** (0 warnings) | **PASS** (10/10 TUs) | **PASS** (7/7) | `engine-win` — clean |
| `engine-win/project-cloud-backup` | `25bf2855` | Yes | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (6/6) | `engine-win` — clean |
| `engine-win/project-sync-folder` | `2a2d0f96` | Yes | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (3/3) | `engine-win` — clean |
| `engine-win/scanner-paused-reader-proof` | `b944e252` | Yes | **PASS** (0 warnings) | **PASS** (1/1 TU) | **PASS** (7/7) | `engine-win` — clean |
| `engine-win/scanner-clean-mode` | `9a1928bb` | Yes | **PASS** (0 warnings) | **PASS** (3/3 TUs; 1 JUCE-off skipped) | **PASS** (8/8) | `engine-win` — clean |
| `engine-win/vst3-class-id-packets` | `7d4128dc` | Yes | **PASS** (0 warnings) | **PASS** (3/3 TUs) | **PASS** (9/9) | `engine-win` — clean |
| `engine-win/recording-launch-discovery` | `0b88a229` | Yes | **PASS** (0 warnings) | **PASS** (6/6 TUs) | **PASS** (6/6) | `engine-win` — clean |
| `engine-win/comp-take-splice` | `2d911206` | Yes | **PASS** (0 warnings) | **PASS** (2/2 TUs) | **PASS** (10/10) | `engine-win` — clean |
| `window/test-hygiene` | `9e946f45` | Yes | **PASS** (`adi_core`, 0 warnings) | N/A (9/9 JUCE-gated) | compile-only (headless) | `window` — headless unaffected |
| `window/held-peak` | `8b527bbb` | Yes | **PASS** (0 warnings) | **PASS** (1/1 headless TU; 3 JUCE-off skipped) | **PASS** (1/1: `adi_ui_window_state_tests`) | `window` — clean |
| `window-fn/shaped-dither` | `93f7541b` | Yes | **PASS** (0 warnings) | **PASS** (7/7 headless TUs; 1 JUCE-off skipped) | **PASS** (6/6) | `window-fn` — clean |

### New findings (do not fix here; for win / owners)

1. **Clang 14.0.6 compile error — `adi_daw/src/adi/ui/shared_analysis.cpp:561:19` (`mixview/review-scope-lands-after-2026-12-03` `58817912`, introduced by `9d7559ac` `mixview/mx21-spectrum-crest-lands-after-2026-12-03`, owner: `mixview`)**
   - Diagnostic: `src/adi/ui/shared_analysis.cpp:561:19: error: no matching member function for call to 'emplace'` on `completed.emplace();` inside `completeWindow()`.
   - Smallest repro: checkout `58817912` and compile `src/adi/ui/shared_analysis.cpp` with `clang++` 14.0.6 `-std=c++20` using the target's include flags.

2. **GCC 12.2 `-Werror=sign-conversion` — `adi_daw/tests/test_vinyl_distortion.cpp:178:68` (`devices/spectral-resonator` `9a6ea54f` / `devices/vinyl-distortion`, owner: `devices`)**
   - Diagnostic: `tests/test_vinyl_distortion.cpp:178:68: error: conversion to 'std::size_t' {aka 'long unsigned int'} from 'int' may change the sign of the result [-Werror=sign-conversion]` on `clicks += std::abs(l[i]) > std::abs(l[i - 1]) + 0.1f ? 1 : 0;`. (Only GCC 12.2 error across all 86 TUs of `9a6ea54f`; Clang 14.0.6 passes 86/86.)
   - Smallest repro: checkout `9a6ea54f`, build `adi_vinyl_distortion_tests` with GCC 12.2 `-DADI_WERROR=ON`.

3. **GCC 12.2 and Clang 14.0.6 `-Werror=unused-function` — `adi_daw/tests/test_render_queue.cpp:827,873` (`engine-win/export-profile-validation` `2b3b7e5c`, owner: `engine-win`)**
   - Diagnostics (both GCC 12.2 and Clang 14.0.6 `-DADI_WERROR=ON`):
     - `tests/test_render_queue.cpp:827:6: error: unused function 'explicitDirectTaps' [-Werror=unused-function]`
     - `tests/test_render_queue.cpp:873:6: error: unused function 'directStemBatch' [-Werror=unused-function]`
   - Smallest repro: checkout `2b3b7e5c`, build `adi_render_queue_tests` with GCC 12.2 or Clang 14.0.6 `-DADI_WERROR=ON`.

(No PR requested; run-only verification round.)

---

## 2026-11-23 — Queue item 2 (PR-BUILDS, Round 5): DF-W39 confirmation (`window-fn/seams-gcc12`) and open PR stack `#722`/`#729`–`#733` (`land/clones-1123b`)

**For win and window-fn.** Verified `window-fn/seams-gcc12` (`f0830617`, closing our DF-W39 finding) and the trimmed open PR landing stack `#722` (`land/engine-win-1123` `46eaba97`) -> `#729` (`land/engine-1123b` `882c770b`) -> `#730` (`land/window-1123b` `620aa8cd`) -> `#731` (`land/mixview-1123b` `d82bafd0`) -> `#732` (`land/devices-1123b` `a78f571b`) -> `#733` (`land/clones-1123b` `f26998ef`).

### Summary table

| Branch / PR | SHA | DF-M62 local patch | GCC 12.2 `-DADI_WERROR=ON` | Clang 14.0.6 `-Werror` | CTest | Owner / Notes |
|---|---|---|---|---|---|---|
| `window-fn/seams-gcc12` (DF-W39) | `f0830617` | Yes (pre-existing) | **PASS** (`adi_ui_seams_tests`, 0 warnings; CMake `ADI_HAS_SANITIZE_TRAP_GROUP=Failed`, `ADI_HAS_SANITIZE_TRAP_ON_ERROR=Success` -> `-fsanitize=signed-integer-overflow -fsanitize-undefined-trap-on-error`) | **PASS** (`clang++` 14.0.6 probe accepts `-fsanitize=signed-integer-overflow -fsanitize-trap=signed-integer-overflow -Werror`) | **PASS** (**568/568 checks**, 0 failures) | `window-fn` — **DF-W39 confirmed fixed on GCC 12.2** |
| `land/clones-1123b` (combined tip of open PRs `#722`, `#729`–`#733`) | `f26998ef` | **None** (`d22b81c1` in stack) | **PASS** (116 targets, 0 warnings) | **FAIL** (65/66 headless TUs clean; only `clip_playback.cpp` fails on `timeline_clips.hpp:13` `std::views::filter`; 14 JUCE-off skipped) | **PASS** (**115/115** affected suites) | `win` / `engine-win` — ready on GCC 12.2 `-DADI_WERROR=ON` with zero local patches; `timeline_clips.hpp:13` remains the lone Clang 14.0.6 failure |

(No PR requested; run-only verification round.)

---

## 2026-11-23 — Queue item 2 (PR-BUILDS, Round 4): combined 7-stack `land/*-1123` train (`de85bc60`) and newly requested window/window-fn heads

**For win (follow-up to Round 3).** Win chained the six updated `land/*-1123` stacks together onto `land/engine-win-1123` (`46eaba97`) -> `land/engine-1123` (`d5163723`, adding DF-E17 `4eda2b5b` and `c5407f3d` for `test_ui_document`) -> `land/window-1123` (`3413cb86`) -> `land/window-fn-1123` (`854e0ba5`) -> `land/mixview-1123` (`f5c59c0c`, DF-M62 `d22b81c1`) -> `land/devices-1123` (`94a2c796`) -> `land/clones-1123` (`de85bc60`), and `window` / `window-fn` requested two new branches. Verified on Linux GCC 12.2.0 `-DADI_WERROR=ON` (`-g0`, Pd on, JUCE off) and Clang 14.0.6 `-Werror`.

### Summary table

| Branch | SHA | DF-M62 local patch | GCC 12.2 `-DADI_WERROR=ON` | Clang 14.0.6 `-Werror` (changed headless TUs) | CTest | Owner / Notes |
|---|---|---|---|---|---|---|
| `land/clones-1123` (combined 7-stack train incl. `engine-win`, `engine`, `window`, `window-fn`, `mixview`, `devices`, `clones`) | `de85bc60` | **None** (`d22b81c1` in train) | **PASS** (136 targets, 0 warnings) | **FAIL** (86/89 headless TUs clean; only the same 3 engine TUs fail on `timeline_clips.hpp:13` + pre-existing DF-E69 `midi_clips.cpp:867`; 15 JUCE-off skipped) | **PASS** (**134/134** affected suites + **10/10** `--repeat until-fail:10` on `adi_ui_document_tests`) | `win` / `engine-win` — confirms DF-M62 clean across full train, DF-E17 clean, and `c5407f3d` eliminates the `test_ui_document.cpp:1282` DF-E64 flake |
| `window/shortcut-ids` (WIN-04, carries `window/function-keys` `c4a62e25`) | `2cfaacbd` | Yes (pre-existing) | **PASS** (`adi_core`, 0 warnings) | N/A (5/5 modified TUs are JUCE-gated) | compile-only (headless) | `window` — headless build unaffected; JUCE leg for win/CI |
| `window-fn/param-touched` (SRF-01) | `7ebeac6b` | Yes (pre-existing) | **PASS** (0 warnings) | **PASS** (1/1 headless TU `project_document.cpp`; 1 JUCE-off skipped) | **PASS** (1/1: `adi_mix_parameter_feed_tests`) | `window-fn` — clean on headless leg |

### Status of Round 3 findings on `de85bc60`

- **Resolved:** `adi_daw/tests/test_ui_document.cpp:1282` (`FAIL and it plays`) — `c5407f3d` ("the dropped clip's test primes the playing target") passed **10/10 consecutive repeats** (`110.54 s`, 0 failures) on `de85bc60`.
- **Still open (Clang 14.0.6 + libstdc++ 12):** `adi_daw/src/adi/engine/timeline_clips.hpp:13` (`std::views::filter`, owner: `engine-win`, introduced by `8c088dcb`) still fails Clang 14.0.6 in `clip_playback.cpp`, `midi_clips.cpp`, and `scale_timeline.cpp`. No other warning or error across all 89 headless TUs of the 7-stack train.

(No PR requested; run-only verification round.)

---

## 2026-11-23 — Queue item 2 (PR-BUILDS, Round 3): all seven `land/*-1123` landing stacks on GCC 12.2 and Clang 14.0.6 `-DADI_WERROR=ON`

**For win (before landing the 23 November stacks).** Verified all seven staged `land/*-1123` heads (`origin/main` at `4836cb61`) with `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF` on Linux GCC 12.2.0 (`-g0`) and Clang 14.0.6 (`-Werror` across every changed headless TU), running all affected CTests plus `adi_acceptance_wts01` (`-DADI_ACCEPTANCE=ON`) where relevant. Transient local DF-M62 patch (`FloatRealTransform.h:61`) was applied ONLY where DF-M62 was absent (and never committed); `land/mixview-1123` was built with **no** local DF-M62 patch.

### Summary table

| Stack | SHA | DF-M62 local patch | GCC 12.2 `-DADI_WERROR=ON` | Clang 14.0.6 `-Werror` (changed headless TUs) | CTest (affected suites) | Owner / Notes |
|---|---|---|---|---|---|---|
| `land/mixview-1123` | `a8891323` | **None** (`d22b81c1` in stack) | **PASS** (0 warnings) | **PASS** (9/9 TUs; 1 JUCE-off skipped) | **PASS** (10/10) | `mixview` — confirms DF-M62 clean on landing stack |
| `land/clones-1123` | `7db8c191` | Yes (pre-existing) | **PASS** (0 warnings) | **PASS** (8/8 TUs) | **PASS** (7/7 shaper suites; WTS-01 fails only on pre-DF-E64 2b) | `clones` — clean |
| `land/devices-1123` | `83f71d5e` | Yes (pre-existing) | **PASS** (0 warnings) | **PASS** (21/21 TUs) | **PASS** (44/44 device suites; WTS-01 fails only on pre-DF-E64 2b) | `devices` — clean |
| `land/engine-win-1123` | `46eaba97` | Yes (pre-existing) | **PASS** (0 warnings) | **FAIL** (11/12 TUs; **NEW** `timeline_clips.hpp:13` in `clip_playback.cpp`; 2 JUCE-off skipped) | **PASS** (39/39 incl. **`adi_acceptance_wts01` PASS**) | `engine-win` — WTS-01 2b fixed; new Clang 14 `<ranges>` failure below |
| `land/engine-1123` | `c4c61cbe` | Yes (pre-existing) | **PASS** (0 warnings) | **FAIL** (44/47 TUs; **NEW** `timeline_clips.hpp:13` in `clip_playback.cpp`, `midi_clips.cpp`, `scale_timeline.cpp` + pre-existing DF-E69 `midi_clips.cpp:867`; 4 JUCE-off skipped) | **PASS** (82/82 incl. **`adi_acceptance_wts01` PASS**) | `engine` / `engine-win` — GCC + all 82 CTests + WTS-01 green |
| `land/window-fn-1123` | `1012a2c2` | Yes (pre-existing) | **PASS** (0 warnings) | **PASS** (3/3 headless TUs; 5 JUCE-off skipped) | **PASS** (2/2: `adi_mix_document_tests`, `adi_ui_document_tests`) | `window-fn` — clean on headless leg |
| `land/window-1123` | `a084f5d9` | Yes (pre-existing) | **PASS** (0 warnings) | **FAIL** (46/49 headless TUs — all window TUs PASS; only inherited 3 engine TUs fail on `timeline_clips.hpp:13` + DF-E69 `midi_clips.cpp:867`; 12 JUCE-off skipped) | **FAIL** (82/84: **NEW** flake in `adi_ui_document_tests` + expected WTS-01 Step 5b trigger) | `window` / `engine-win` / `win` — details below |

### New findings (do not fix here; for win / owners)

1. **NEW Clang 14.0.6 + libstdc++ 12 compile failure — `adi_daw/src/adi/engine/timeline_clips.hpp:13` (owner: `engine-win`, introduced by `8c088dcb` in `engine-win/take-lane-snapshot` `430ea825`, affects `land/engine-win-1123` `46eaba97`, `land/engine-1123` `c4c61cbe`, `land/window-1123` `a084f5d9`)**
   - Diagnostic:
     - `src/adi/engine/timeline_clips.hpp:13:24: error: invalid operands to binary expression ('const std::vector<Clip>' and '_Partial<std::ranges::views::_Filter, ...>')` (cascading from `libstdc++` 12 `bits/ranges_util.h:133:24: error: constraints not satisfied for alias template 'sentinel_t'`).
   - Affected TUs: `src/adi/engine/clip_playback.cpp`, `src/adi/engine/midi_clips.cpp`, `src/adi/engine/scale_timeline.cpp`. (Same Clang 14 + libstdc++ 12 `<ranges>` incompatibility family as DF-E69; replacing `std::views::filter` with a small iterator/loop or non-ranges helper avoids it.)
   - Smallest repro: checkout `46eaba97` and compile `src/adi/engine/clip_playback.cpp` with `clang++` 14.0.6 `-std=c++20` using the build's include flags.

2. **NEW intermittent CTest failure (`FAIL and it plays`) — `adi_daw/tests/test_ui_document.cpp:1282` on `land/window-1123` (`a084f5d9`) (owner: `window` / `engine-win`, DF-E64 interaction)**
   - Observed: `adi_ui_document_tests` failed `FAIL and it plays` (`343 checks, 1 failures`) under `ctest -j4`, passed once standalone, then failed again on iteration 2/10 of `ctest -R '^adi_ui_document_tests$' --repeat until-fail:10` (2 failures in 3 runs on Linux GCC 12.2).
   - Mechanism: `test_ui_document.cpp:1273-1281` runs a tight loop of 64 blocks with zero sleep (`dropped->process(out)` + `dropped->session().clips()->prime()`). Because DF-E64 (`a7983164`, in `land/engine-win-1123` / `land/engine-1123` / `land/window-1123`) holds `Command::Play` in `TransportMailbox::drain` (`window_state.hpp:197`) until `block.sourcesReadyAt(sample, io.frames, true)` returns true (`project_document.cpp:1164`), all 64 no-sleep iterations can finish before the async audio decoder/reader makes the dropped WAV ready, leaving `Play` held and `heard == false`.
   - Smallest repro: on `a084f5d9`, run `ctest -R '^adi_ui_document_tests$' --repeat until-fail:10 --output-on-failure`.

3. **WTS-01 Step 5b trigger confirmed on `land/window-1123` (`a084f5d9`) (owner: `win`, `tests/acceptance/test_session_wts01.cpp`)**
   - On `land/engine-win-1123` (`46eaba97`) and `land/engine-1123` (`c4c61cbe`), `adi_acceptance_wts01` passes **100%** (confirming DF-E64 fixes Step 2b on Linux).
   - On `land/window-1123` (`a084f5d9`, which adds DF-W33 `window/return-in-group`), Step 2b passes and Step 5b now triggers win's expected placeholder failure: `WTS01 5b FAIL | Group Tracks accepted the return; this harness does not test it yet | update the acceptance test now that grouped returns are built`. Ready for win's Step 5b acceptance update once DF-W33 lands.

(No PR requested; run-only verification round.)

---

## 2026-11-23 — PLAN row 4 (CO-01): the collaboration core

Branch: `gemini/co01-collab-core` at `909961ae` (from `origin/main` `4836cb61`, lent paths only under `src/adi/collab/**`, `tests/test_collab_*.cpp`, and `CMakeLists.txt` per ADR-0252 and `collab/RESERVATIONS.md`).

### What changed

- `adi_daw/src/adi/collab/fractional_key.hpp`, `.cpp`: base-62 lexicographic fractional sibling keys (Rocicorp / Figcraft algorithm, PLAN C3) with swapped/equal bound refusal, balanced `nKeysBetween`, `respaceKeys`, and longest-increasing-subsequence `reuseKeysForOrder`.
- `adi_daw/src/adi/collab/batch.hpp`, `.cpp`: `IdSpaceAllocator` (`(idSpace << 32) | counter` for positive 31-bit id spaces), `VersionVector` (`observe`, `merge`, `dominates`, `concurrentWith`), `Batch` / `BatchOp` wire model (typed ops with Lamport clocks and optional `orderKey`, explicitly refusing inverses), 20-digit zero-padded key formatters (`formatSeqKey`, `formatLamportBatchKey`, ADR-0188 d5), and deterministic CBOR `encodeBatch` / `decodeBatch`.
- `adi_daw/src/adi/collab/fake_object_store.hpp`, `.cpp`: thread-safe in-memory `FakeObjectStore` with atomic `putIfAbsent`, lexical `listKeys`, deterministic seeded visibility delays, transient `put`/`get`/`list` drops, and dropped-PUT-ACK fault injection.
- `adi_daw/src/adi/collab/replica.hpp`, `.cpp`: `CollabReplica` over `adi::Store`, applying sequenced batches in strict global `seq` order, rewinding the pending suffix via `History::undo` on remote arrival and re-committing surviving pending batches onto the converged base, claiming the next global `seq` via `putIfAbsent`, and canonicalizing fractional sibling order (`(orderKey, id)`) into dense `devices.ord` / `tracks.index_in_parent` ranks so concurrent inserts at the same slot converge identically.
- `adi_daw/tests/test_collab_fractional.cpp`, `adi_daw/tests/test_collab_sync.cpp`, `adi_daw/CMakeLists.txt`: `adi_collab_fractional_tests` (Rocicorp/Figcraft vectors, swapped-bound refusal, 500 midpoint subdivisions, LIS key reuse, id-space disjointness, version vectors, CBOR round-trip and inverse rejection) and `adi_collab_sync_tests` (two replicas concurrently inserting a device at slot 0 under both seq winners; 3 real SQLite `Store`s + `FakeObjectStore` across 200 seeds with delays, drops, and dropped PUT ACKs converging to equal `digestProject` and sibling order with nothing pending; 3 concurrent replica threads).

### Needs from engine / win (outside lent paths, not edited here)

- Engine: `CommitOptions::remote` seam in `ops.hpp`/`ops.cpp`/`history.cpp` to stamp remote author clocks directly and retire superseded pending rows; `idSpace` integration in `row_ids.*`; machine-local column exclusion (`media_files` paths/missing flags) and `sync_*` table exclusion in `digest.cpp`; schema minor (reserved in `collab/RESERVATIONS.md` when landing) for `sync_vectors` and `order_keys`.
- Win: `SQLITE_ENABLE_SESSION` build option for CO-03 touch sets.

### Verification

- **GCC 12.2 (`-DADI_WERROR=ON`, CMake + ctest, transient local DF-M62 patch reverted):** `adi_collab_fractional_tests` PASS (`692/692` checks, 0.01 s), `adi_collab_sync_tests` PASS (`21/21` checks including 200/200 seeds + slot-0 collision + 3-thread run, 27.30 s); 0 warnings.
- **Clang 14.0.6 (`-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wshadow -Wswitch-enum -Werror`):** all 6 new CO-01 TUs compile with 0 warnings.
- **GCC 12.2 ASan + LSan + UBSan (`-O1 -g1 -fsanitize=address,undefined`):** `adi_collab_fractional_tests` PASS (`692/692`, 0.21 s), `adi_collab_sync_tests` PASS (`21/21`, 42.32 s), 0 leaks, 0 UB.
- **GCC 12.2 TSan (`-O1 -g1 -fsanitize=thread`, `setarch x86_64 -R`):** `adi_collab_fractional_tests` PASS (`692/692`, 0.02 s), `adi_collab_sync_tests` PASS (`21/21`, 294.96 s), 0 data races, 0 deadlocks. (MSVC `/W4 /WX` and macOS legs remain for win.)

Review requested (engine): `gemini/co01-collab-core` at `909961ae` (lands after 3 December; PR requested only after engine review in `collab/engine.md` passes this head).

---

## 2026-11-23 — PLAN row 3 (PD-15): cross-project track paste, the engine half

Branch: `gemini/pd15-project-paste` at `ba43cddc` (from `origin/main` `7aa3ab58`, lent paths only per ADR-0252 and `collab/RESERVATIONS.md`; zero edits to `row_ids.*`, `device_copy.*`, or `publish_file.*`).

### What changed

- `adi_daw/src/adi/project_paste.hpp`, `adi_daw/src/adi/project_paste.cpp`: `pasteTrackFromProject(source, sourceTrackId, dest, actor, actorDetail)` copies one track (mixer strip, top-level device chains and devices, audio and MIDI clips, track/device/clip automation lanes and data, curve segment ranges, and referenced media/state blobs) from a second `Store` (opened read-only) into the active writable `Store` as one undoable `OpJournal` transaction (`expectHead` guarded, one `txn_id`).
  - Allocates every destination row id through `RowIds` (`row_ids.hpp`).
  - Reads devices through public `readDeviceCopy()` (`device_copy.hpp`), deduplicates `plugin_refs` by `(format, uid, shell_id)` using `pluginRef.register` only when absent, and transports missing `state_blobs` by BLAKE3 hash via `OpTransactionInputs` + `OpJournal::commitCaptured()`.
  - Copies media by BLAKE3 hash through public `media::resolveMedia()` and `media::publishFile()` (`VerifiedCopyFallback`), reusing any hash already in `dest.media_files` without copying again, and rolling back published files if the transaction fails.
  - Refuses read-only destinations, Master tracks, missing source tracks, nested rack chains, and audio-thread calls without modifying `source` or publishing files.
- `adi_daw/tests/test_project_paste.cpp`, `adi_daw/CMakeLists.txt`: `adi_project_paste_tests` covering colliding-id full remapping, single-step undo returning the exact `digestProject` start digest and redo, byte-for-byte source `.adi` and audio file immutability, media hash deduplication (pre-existing hash not copied again; two clips sharing one WAV copied once), read-only destination refusal, `state_blobs` hash copy/dedup, and concurrent open `Store`s.

### Verification

- **GCC 12.2 (`-DADI_WERROR=ON`, CMake + ctest, transient local DF-M62 patch reverted):** `adi_project_paste_tests` PASS (`81/81` checks); 0 warnings.
- **Clang 14.0.6 (`-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wshadow -Wswitch-enum -Werror`):** `src/adi/project_paste.cpp` and `tests/test_project_paste.cpp` compile with 0 warnings.
- **GCC 12.2 ASan + LSan + UBSan (`-O1 -g1 -fsanitize=address,undefined`):** `adi_project_paste_tests` PASS (`81/81` checks, 1.65 s), 0 leaks, 0 UB.
- **GCC 12.2 TSan (`-O1 -g1 -fsanitize=thread`, `setarch x86_64 -R`):** `adi_project_paste_tests` PASS (`81/81` checks, 5.86 s), 0 data races, 0 deadlocks. (MSVC `/W4 /WX` and macOS legs remain for win.)

Review requested (engine): `gemini/pd15-project-paste` at `ba43cddc` (lands after 3 December; PR requested only after engine review in `collab/engine.md` passes this head).

---

## 2026-11-23 — PLAN row 2 (SRF-10): OSCQuery v1 tree, ?HOST_INFO, and GET-only HTTP core

Branch: `gemini/srf10-oscquery-core` at `195892d7` (stacked on `gemini/srf09-osc-core` `5e42fa61`, lent paths only per ADR-0252 and `collab/RESERVATIONS.md`).

### What changed

- `adi_daw/src/adi/oscquery.hpp`, `adi_daw/src/adi/oscquery.cpp`: OSCQuery v1 core built on pinned header-only `nlohmann/json` (ADR-0188 d7) with zero dependency on `osc_codec`, `adi_core`, SQLite, or JUCE so `adi_fuzz_oscquery` compiles `tests/fuzz_oscquery.cpp` + `src/adi/oscquery.cpp` alone.
  - `OscQueryTree::build(NamespaceDescription)` constructs a deterministic JSON tree keyed by stable positive ids (`/adi/track/{stableId}/...`, `/adi/master/...`, sends, devices, params) with `FULL_PATH`, float `TYPE` (`"f"`), clamped finite `VALUE`, `RANGE` (`[{"MIN": 0.0, "MAX": 1.0}]`), `ACCESS: 3` (read+write together, ADR-0188 d7), and `DESCRIPTION` combining name and `Record.text`. Zero `LISTEN` keys anywhere.
  - `renderHostInfoJson(HostInfo)` emits `NAME`, `OSC_PORT`, `OSC_TRANSPORT` (`"UDP"`), optional `OSC_IP`, and v1-only `EXTENSIONS` (`ACCESS`, `DESCRIPTION`, `RANGE`, `VALUE` — no `LISTEN`).
  - `parseHttpRequest` and `handleHttpRequest` implement a pure `GET`-only HTTP/1.x responder serving `/`, any tree path, and `/?HOST_INFO` without CORS `*` or file endpoints, refusing non-GET methods (`405` + `Allow: GET`), unknown paths (`404`), malformed/traversal/v2 `?LISTEN` requests (`400`), unsupported HTTP versions (`505`), oversized requests (`413`), oversized targets (`414`), and excessive headers (`431`).
- `adi_daw/tests/test_oscquery.cpp`, `adi_daw/tests/fuzz_oscquery.cpp`, `adi_daw/CMakeLists.txt`: golden JSON test for two tracks + Main, stable-id preservation test when a track is inserted before existing tracks, `?HOST_INFO` and HTTP error-status tests, no-`LISTEN` / no-CORS assertions, and standalone libFuzzer harness (`adi_fuzz_oscquery`, linked only against header-only `nlohmann_json::nlohmann_json` and `adi_warnings`).

### Verification

- **GCC 12.2 (`-DADI_WERROR=ON`, CMake + ctest, transient local DF-M62 patch reverted):** `adi_oscquery_tests` PASS (`70/70` checks) alongside `adi_osc_codec_tests` (`208/208`) and `adi_osc_namespace_tests` (`217/217`); 0 warnings.
- **Clang 14.0.6 (`-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wshadow -Wswitch-enum -Werror`):** all 3 new TUs (`oscquery.cpp`, `test_oscquery.cpp`, `fuzz_oscquery.cpp`) compile with 0 warnings without `osc_codec`; `adi_oscquery_tests` PASS (`70/70`).
- **GCC 12.2 ASan + LSan + UBSan & `_GLIBCXX_DEBUG`:** `adi_oscquery_tests` PASS (`70/70`) under both; 0 leaks, 0 UB.
- **Standalone `fuzz_oscquery` (`tests/fuzz_oscquery.cpp` + `src/adi/oscquery.cpp`, ASan + LSan + UBSan, no `osc_codec`):** replayed 12 HTTP seeds + 239,628 mutated executions in 5.00 s (47,917 exec/s) with 0 crashes, 0 UB, 0 leaks, 0 `LISTEN` or CORS leaks. (MSVC `/W4 /WX` and macOS legs remain for win.)

Review requested (engine): `gemini/srf10-oscquery-core` at `195892d7` (stacked on `gemini/srf09-osc-core` `5e42fa61`; lands after 3 December; PR requested only after engine review in `collab/engine.md` passes this head).

---

## 2026-11-23 — PLAN row 1 (SRF-09): OSC 1.0 decoder, fixed namespace, and no-echo feedback core

Branch: `gemini/srf09-osc-core` at `5e42fa61` (from `origin/main` `7aa3ab58`, lent paths only per ADR-0252 and `collab/RESERVATIONS.md`).

### What changed

- `adi_daw/src/adi/osc_codec.hpp`, `adi_daw/src/adi/osc_codec.cpp`: self-contained OSC 1.0 packet decoder (C++20 stdlib only, zero `adi_core`/JSON/SQLite/JUCE dependencies so `ADI_BUILD_FUZZERS` compiles `tests/fuzz_osc_codec.cpp` + `src/adi/osc_codec.cpp` alone). Decodes OSC 1.0 messages (`i`, `f`, `s`, `b`) and bundles (`#bundle\0` + 64-bit NTP `TimeTag`) with strict 4-byte alignment checks, zero-byte padding verification, negative/overflowing blob rejection, trailing-byte rejection, and explicit `DecodeLimits` (`maxPacketBytes = 65536`, `maxBundleDepth = 8`, `maxElements = 256`, `maxArguments = 64`).
- `adi_daw/src/adi/osc_namespace.hpp`, `adi_daw/src/adi/osc_namespace.cpp`: fixed namespace resolver and dispatcher over `/adi/transport/...`, `/adi/track/{n}/...`, `/adi/master/...`, and `/adi/focused/...` (1-based canonical positive indices; unknown, malformed, and structural paths refused). Off by default (`ClientPolicy::enabled = false`, ADR-0188 d7) with optional `allowedClients` allow-list. Every write emitted to `WriteSink` is stamped `actor = Actor::User` and `actorDetail = "osc:<client>"` (ADR-0181 d2, ADR-0185 d4). Momentary releases (`<= 0` on `play`, `stop`, `re_enable_automation`) are ignored and never stop transport; relative nudges are never coalesced. `flushFeedback()` emits on change only, suppresses echoing any address back to its sender within the flush it arrived in while updating other peers immediately, and confirms settled values (or delivers later changes) on subsequent flushes.
- `adi_daw/tests/test_osc_codec.cpp`, `adi_daw/tests/test_osc_namespace.cpp`, `adi_daw/tests/fuzz_osc_codec.cpp`, `adi_daw/CMakeLists.txt`: unit tests (`adi_osc_codec_tests`, `adi_osc_namespace_tests`) and standalone libFuzzer harness (`adi_fuzz_osc_codec`).

### Verification

- **GCC 12.2 (`-DADI_WERROR=ON`, CMake + ctest, transient local DF-M62 patch reverted):** `adi_osc_codec_tests` PASS (`208/208` checks), `adi_osc_namespace_tests` PASS (`217/217` checks); 0 warnings.
- **Clang 14.0.6 (`-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion -Wshadow -Wswitch-enum -Werror`):** all 5 new TUs (`osc_codec.cpp`, `osc_namespace.cpp`, `test_osc_codec.cpp`, `test_osc_namespace.cpp`, `fuzz_osc_codec.cpp`) compile with 0 warnings; both unit suites PASS (`208/208` and `217/217`).
- **GCC 12.2 ASan + LSan + UBSan (`-O1 -g1 -fsanitize=address,undefined`):** both unit suites PASS (`208/208`, `217/217`), 0 leaks, 0 UB.
- **GCC 12.2 `_GLIBCXX_DEBUG` (`-O1 -D_GLIBCXX_DEBUG`):** both unit suites PASS (`208/208`, `217/217`).
- **Standalone `fuzz_osc_codec` (`tests/fuzz_osc_codec.cpp` + `src/adi/osc_codec.cpp`, ASan + LSan + UBSan):** replayed 10 OSC seeds + 3,786,762 mutated executions in 5.00 s (757,341 exec/s) with 0 crashes, 0 UB, 0 leaks. (MSVC `/W4 /WX` and macOS legs remain for win.)

Review requested (engine): `gemini/srf09-osc-core` at `5e42fa61` (lands after 3 December; PR requested only after engine review in `collab/engine.md` passes this head).

---

## 2026-11-23 — Queue item 2 (PR-BUILDS, Round 2): new/updated requested heads on GCC 12.2 and Clang 14.0.6 `-DADI_WERROR=ON`

Checked all 7 seat logs (`engine/log` `7da7b726`, `engine-win/log` `975577e2`, `devices/log` `cec2d176`, `mixview/log` `a3aa0127`, `clones/log` `1142e6fc`, `window/log` `d6584386`, `window-fn/log` `6fb4f996`) and open `land/*` stacks against Round 1 (`5b471614`). Only one new/updated unmerged landing stack exists: **PR `#716` `land/clones-panel-1122b` (`1baf1fd8`)** (DF-M62 `FloatRealTransform.h:61` patched transiently locally to see past it and reverted).

| Branch | SHA | Compiler | Warnings / Errors (`-DADI_WERROR=ON`, Pd on, JUCE off) | CTest result | Owner |
|---|---|---|---|---|---|
| `land/clones-panel-1122b` (`#716`) | `1baf1fd8` | GCC 12.2.0 | **FAIL** (3 `-Werror=conversion` errors in `adi_daw/tests/adi_shaper_wave_tests.cpp:55:39`, `:81:67`, `:94:67`: implicit `int` to `float` in `p.x / 8192.f` and `xValues[...] / 8192.f`). `src/adi/dsp/adi_shaper_wave.hpp` and all other 14 shaper TUs + `adi_core` are now **0 warnings**. | 7 / 8 shaper suites **PASS** (`adi_shaper_tests`, `adi_shaper_filter_tests`, `adi_shaper_sweep_tests`, `adi_shaper_drive_tests`, `adi_shaper_crush_tests`, `adi_shaper_time_tests`, `adi_shaper_pd_tests`); `adi_shaper_wave_tests` blocked by build error | `clones` / `win` |
| `land/clones-panel-1122b` (`#716`) | `1baf1fd8` | Clang 14.0.6 | **CLEAN (0 warnings)** across all 15 shaper `.o` targets (including `adi_shaper_wave_tests.cpp.o`, `adi_shaper.cpp.o`, `adi_shaper_time.cpp.o`, `adi_shaper_pd_tests.cpp.o`) | Link blocked by pre-existing `main` `adi_core` Clang 14 errors (DF-E69) | `clones` / `win` |

- **Smallest repro (`#716`, `1baf1fd8`)**: checkout `1baf1fd8`, patch DF-M62 locally, configure with GCC 12.2 `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`, run `ninja adi_shaper_wave_tests`.
- All other active `2026-11-21+` PR-requested heads were already covered in Round 1 (`5b471614`) or landed in `#711`/`#712`/`#713`/`#715`/`#718`.

---

## 2026-11-23 — Rerun item 13 (GLIBCXX-DEBUG) on new `main` (`7e3e627d`, post-#711/#713/#715/#718)

Baseline: `main` (`abbc32ab`, 22 Nov). GCC 12.2.0 (`-O1 -g0 -D_GLIBCXX_DEBUG`, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_WERROR=OFF`).

### What is new vs `abbc32ab`

- **Nothing new** — **272 / 275 PASS**, **0 new `_GLIBCXX_DEBUG` aborts**.
- Exact same 3 pre-existing non-pass suites (`adi_mix_opto_gain_tests` `std::span(UINTPTR_MAX - 3, 1)` abort, `adi_link_playback_tests` safe-iterator mutex interaction with `RealtimeAudit`, `adi_ui_seams_tests` GCC 12 flag build-block). All newly landed `#711`/`#713`/`#715`/`#718` code passed `-D_GLIBCXX_DEBUG` cleanly.

---

## 2026-11-23 — Rerun item 12 (TSAN) on new `main` (`7e3e627d`, post-#711/#713/#715/#718)

Baseline: `main` (`abbc32ab`, 22 Nov). GCC 12.2.0 (`-O1 -g1 -fsanitize=thread -fno-omit-frame-pointer`, `setarch x86_64 -R`, `TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:exitcode=66`). Clang 14.0.6 still lacks `libclang_rt.tsan-x86_64.a` on this VM. Expanded from 39 to **42** suites to also cover `adi_ui_document_tests`, `adi_collect_export_tests`, and `adi_mpe_output_tests`.

### What is new vs `abbc32ab`

- **Nothing new** — **41 / 42 PASS**, **0 new data races or deadlocks**.
- Sole failure remains pre-existing `adi_progressive_decode_tests` (`stb_vorbis.c:999` `crc32_init` race on global `crc_table`). All newly landed `#711`/`#713`/`#715`/`#718` threaded paths (N60 live MIDI handoff, DF-E56 MPE router, MI-08 CLAP/session, N66 recording prefix, export host owner, scanner cache ownership, and `adi_ui_document_tests`) are TSan-clean.

---

## 2026-11-23 — Rerun item 11 (ASAN/LSan/UBSan) on new `main` (`7e3e627d`, post-#711/#713/#715/#718)

Baseline: `main` (`abbc32ab`, 22 Nov). GCC 12.2.0 (`-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer`, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_WERROR=OFF`). Clang 14.0.6 still lacks `compiler-rt` (`libclang_rt.asan-x86_64.a`) on this VM.

### What is new vs `abbc32ab`

- **Nothing new** — identical across all **275 / 275** suites:
  - Strict run (`detect_leaks=1:halt_on_error=1`): **239 / 275 PASS**, **35 FAIL** (exact same 1 `adi_mix_gate_tests` heap-UAF, 11 `alloc-dealloc-mismatch` harnesses, 23 Pd `canvas_setargs` LSan leaks), **1 BUILD_FAIL** (`adi_ui_seams_tests`).
  - Re-check of the 34 non-UAF failing suites with `detect_leaks=0:alloc_dealloc_mismatch=0`: **34 / 34 PASS**, **0 UBSan reports** across all 274 runnable suites.
  - All newly landed `#711`/`#713`/`#715`/`#718` code and tests (including DF-E53, DF-E56, MI-08, N60, N63, N64, N66, N67, DF-W37, DF-W38, MX-04, MX-08) passed ASan + LSan + UBSan cleanly.

---

## 2026-11-23 — Rerun item 10 (WERROR-SWEEP) on new `main` (`7e3e627d`, post-#711/#713/#715/#718)

Baseline: `main` (`abbc32ab`, 22 Nov). Full-tree sweep (`-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON -DADI_BUILD_BENCHMARKS=ON`, 739 `.o` objects; DF-M62 `FloatRealTransform.h:61` still unfixed on `7e3e627d`, patched transiently and reverted).

### What is new vs `abbc32ab`

- **GCC 12.2.0**: **Nothing new** — **738 / 739** objects clean (0 warnings-as-errors; sole failure remains pre-existing `adi_ui_seams_tests` `-fsanitize-trap=signed-integer-overflow`).
- **Clang 14.0.6**: **Nothing new** — **728 / 739** objects clean (0 warnings-as-errors; exact same 11 pre-existing C++20/libstdc++ 12 objects across the same 5 sites; `tests/acceptance/test_session_wts01.cpp` compiles clean).

---

## 2026-11-23 — Rerun item 3 (WTS-01 on Linux) on new `main` (`7e3e627d`, post-#711/#713/#715/#718)

Baseline: `main` (`abbc32ab`, 22 Nov). Built with GCC 12.2.0 (`-DADI_ACCEPTANCE=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF`), run via `ctest -L acceptance -E adi_builtin_realtime_tests` (40.52 s).

### What is new vs `abbc32ab`

| Step | Previous (`abbc32ab`) | New (`7e3e627d`) | Change |
|---|---|---|---|
| **3b** (Live keyboard into OneShot) | `NOT_BUILT` | **PASS** | Now passes through the app's MIDI consumer (`first sound at +1 frames; peak 0.1256; 0 refused`; N60 window half landed in `#718`). |
| **6b** (Built-in parameter automated) | `NOT_BUILT` | **PASS** | Now passes through the lane chooser (`Utility Gain` drawn in 4 steps; `x1.995262` before 2 bars, `x1.000000` after, `max\|d\| 2.2e-08`; DF-W37 landed in `#718`). |
| **2a / 2b** (Monitoring default) | `monitor Off` | `monitor Auto` | Reflects N67 landing (`#713`/`#718`); step 2a still `PASS` (`max\|d\| 0`), step 2b remains the sole `FAIL` (DF-E64, 256 underrun frames). |

Everything else is unchanged (`1, 2a, 3a, 4, 5a, 6a, 7a, 7b, 7c` `PASS`; `5b` sole remaining headless `NOT_BUILT` with `ENGINE` ok; `3c, 3d, 7d, 7e` JUCE-only; summary `1 FAIL`).

---

## 2026-11-22 — Queue cycle 2: WTS-01 on new `main` (`abbc32ab`), DF-E64 Linux WTS-01 confirmation (`01b1e723`), and Round 2 PR-BUILDS check

All 15 standing queue items are complete on `main` (`abbc32ab`, post-`#712 land/mixview2-1122`); per the standing queue's empty-list rule, checked for new PR-requested branches and re-ran item 3 on the new `main` (items 10–13 were already run on `abbc32ab`).

### 1. Item 3 rerun (`WTS-01 on Linux`, GCC 12.2.0 `-DADI_ACCEPTANCE=ON`)

| Branch / Head | `adi_acceptance_wts01` result | Wall time | Notes |
|---|---|---:|---|
| `main` (`abbc32ab`, post-`#712`) | **1 FAIL** (step 2b DF-E64 only) | 39.39 s | Exact match with `73f13028` and win's 20 Nov table: steps 1, 2a, 3a, 4, 5a, 6a, 7a, 7b, 7c `PASS`; 3b, 5b, 6b `NOT_BUILT` (`ENGINE` ok); 3c, 3d, 7d, 7e JUCE-only. Also verified all 6 newly landed `#712` Mix View 2 suites (`adi_mix_filters_tests`, `adi_mix_deesser_tests`, `adi_mix_key_eq_tests`, `adi_mix_clean_key_tests`, `adi_mix_opto_gain_tests`, `adi_mix_gate_tests`) pass on `abbc32ab` (`6/6 PASS`, 2.00 s). |
| `engine-win/play-first-block` (`01b1e723`, DF-E64) | **0 FAIL — PASS** | 38.55 s | **For win and `engine-win`:** step 2b now passes bit-exact (`max|d| 0` at frame 0, 0 underruns, held 1 callback); steps 5a, 7b first-play, and 7c live-render also improve to `max|d| 0` from frame 0. |

### 2. Round 2 `PR-BUILDS` check

Fetched `origin --prune` against all 7 seat logs (`engine/log` `7da7b726`, `engine-win/log` `975577e2`, `devices/log` `cec2d176`, `mixview/log` `a3aa0127`, `clones/log` `1142e6fc`, `window/log` `d6584386`, `window-fn/log` `6fb4f996`): **0 new active unmerged PR-requested heads** since Round 1 (the sole unmerged `2026-11-21+` ref not an ancestor of Round 1's 46 tested heads is the superseded `engine/df-e23-followups-staged` `72dfdc77`, already noted in `engine.md` as identical to `main` and ready to close).

### Standing queue status

- **Items 1–15**: all 15 completed and logged (`gemini/df-e53-canonical-param-ref` at `03be0b99` and `gemini/df-e56-mpe-router-overflow` at `0b539fb2` still wait on win's landing; engine review pending on item 1's `0b539fb2`).
- **PLAN rows**: held per queue instructions pending the director's decision on whether `gemini` takes post-3-December PLAN rows or becomes a permanent seat with owned paths.

---

## 2026-11-22 — Queue item 15 (REGISTER-AUDIT): second check of the engine seat's landed DF-E rows on `main` (`abbc32ab`)

**For win (ready to move confirmed rows to Done in one PR).** Verified against `adi_daw/docs/DEFERRED.md` and `origin/main` (`abbc32ab`) by checking every commit is an ancestor of `main`, reading every implementation hunk, and running all 21 corresponding test suites under GCC 12.2.0 (`21 / 21 PASS`, 4,904 checks, 0 failures).

| Row | Verdict | Commit on `main` | Test (live on `abbc32ab`) | Residual |
|---|---|---|---|---|
| **DF-E1** | **CONFIRMED** | `55fd8bbd`, `9cc16085` | `adi_engine_tests` (147/147), `adi_clip_details_tests` (`exactLinearEnd`, 32/32), `adi_clip_playback_tests` (`absoluteLinearEndSamples`, 575/575) | None for DF-E1 itself (`DF-E63` separately tracks three other nanosecond-to-tick readers). |
| **DF-E3** | **CONFIRMED** | `83cca8a5` | `adi_progressive_decode_tests` (100/100; last lease drops `pending`/`admitted`/`reserved` immediately) | None. |
| **DF-E4** | **CONFIRMED** | `47243494` | `adi_library_tests` (`vanishedDirectoryKeepsSiblings`, `failedCommitReleasesStagedHashes`, 74/74) | None. |
| **DF-E7** | **CONFIRMED** | `f6215fb1` | `adi_edit_plan_tests` (`clipEditBoundaries`, 670/670) | None. |
| **DF-E9** | **CONFIRMED** | `ab5feee1` | `adi_automation_range_undo_tests` (`forwardCompatibleUndo` with CASCADE children, extra columns, `BEFORE INSERT` trigger, 93/93) | None. |
| **DF-E13** | **CONFIRMED** | `f3656b3a` | `adi_automation_splice_store_tests` (`exactBoundaries` + untouched segment range skip + prefix boundary value, 216/216) | None. |
| **DF-E14** | **CONFIRMED** | `9aa23bec` | `adi_history_move_tests` (pinned vs unpinned childless root + `legacyOrigin`, 54/54) | None (`ENGINE-HISTORY-RETENTION.md` updated). |
| **DF-E15** | **CONFIRMED** | `f60f7f53` | `adi_quiet_gesture_tests` (`transitions`: `Cancelling` -> `Closed`, `Committing` -> `Reentrant`, 185/185) | None. |
| **DF-E18** | **CONFIRMED** | `f28eacd0` | `adi_history_tests` (`testBranchSwitchPreservesRedoTip`, cycle, rollback `opsApplied==0`, 148/148), `adi_dsp_tests` (NaN/Inf true peak, 433/433), `adi_marker_tests` (bounds + replay, 155/155); dead `captureDeleted`/`restoreDeleted` removed | None. |
| **DF-E26 (a, b)** | **CONFIRMED** | `a548544d` | `adi_history_compaction_tests` (`undoneHead`, `connectedRedo`, liveTip skipped, ordinal branch naming, 85/85) | **DF-E26 (c)** remains open for win / director (abandoned-branch budget policy; explicitly outside `(a, b)`). |
| **DF-E27 (a)** | **CONFIRMED** | `2accc015` | `adi_clip_playback_tests` (`shortenedFade`, 575/575) | Meets DF-E27(a) (`fadeOut` clamped in `clip_playback.cpp:630-636`, `freezeWarpFades`, `freezeAudio`). Minor follow-up note outside DF-E27(a)'s fade-out scope: `freezeWarpFades` (`src/adi/clip_commands.cpp:124`) still checks `in > span` without clamping `fade_in_ticks` if a warped clip is shortened past its fade-in. |
| **DF-E27 (b)** | **CONFIRMED** | `25ba8fce` | `adi_clip_playback_tests` (`endSourceClocks`, 575/575), `adi_sample_defaults_tests` (81/81) | None. |
| **DF-E27 (c)** | **CONFIRMED** | `4f7815a3` | `adi_midi_clock_continuity_tests` (`queuedNoteAtSwap`, 121/121) | None. |
| **DF-E27 (d)** | **CONFIRMED** | `2414ac7c` | `adi_history_tests` (all-ephemeral skip + `op_branches` dedup, 148/148) | **DF-E27 (e)** was folded into DF-E29(b) (confirmed below). **DF-E27 (f, g)** remain open on unmerged engine branches (`d0cf6d3c`, `0d14bd14`). |
| **DF-E29 (a–d)** | **CONFIRMED** | `dfab28af` | `adi_transport_settings_tests` (`receivingLoopClock`, 195/195), `adi_clap_tests` (drain-adopted volume, tempo preview, cancel, 48/44.1 kHz, 585/585); VST3 leg (`src/juce/vst3_probe.cpp`) inspected on `main`, JUCE-only build not run here | **DF-E29 (e)** (`setDelayCompensation(false)` note forwarding) remains parked for the director (excluded from `a–d`). |
| **DF-E48** | **CONFIRMED** | `c5f4ae0c` | `adi_freeze_capture_tests` (`scopesOnlyPlayingAssets`, 31/31), `adi_freeze_commands_tests` (`mutedMidiDoesNotForward`, 144/144) | None. |
| **DF-E49** | **CONFIRMED** | `b57638c8` | `adi_gesture_tests` (`concurrentFaultRemainsLatched` + `Coordinator::drain` comment, 780/780) | None. |

**Move-to-Done summary for win**: **11 whole rows** (`DF-E1`, `DF-E3`, `DF-E4`, `DF-E7`, `DF-E9`, `DF-E13`, `DF-E14`, `DF-E15`, `DF-E18`, `DF-E48`, `DF-E49`) plus sub-parts **`DF-E26 (a, b)`**, **`DF-E27 (a–d, e)`**, and **`DF-E29 (a–d)`** are independently confirmed landed and green on `main` (`abbc32ab`).

---

## 2026-11-22 — Queue item 14 (FUZZ): `adi_fuzz_blob` and `adi_fuzz_textproj` seed replay and mutation fuzzing under ASan+UBSan

Head: `main` (`abbc32ab`).
- **Clang 14.0.6 `-DADI_BUILD_FUZZERS=ON`**: `libFuzzer` (`libclang_rt.fuzzer-x86_64.a`) and `compiler-rt` are not installed on this VM (`/usr/bin/ld: cannot find .../libclang_rt.fuzzer-x86_64.a`).
- **Fallback executed (per queue instructions)**: compiled both `tests/fuzz_blob.cpp` + `src/adi/blob.cpp` and `tests/fuzz_textproj.cpp` + `src/adi/textproj.cpp` with GCC 12.2.0 `-O2 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer` against a standalone `LLVMFuzzerTestOneInput` seed-replay and mutation driver (`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`), plus separate `--coverage` (`gcov -b`) runs for coverage. Both harnesses and subjects compiled with 0 warnings under `-Wall -Wextra -Wpedantic -Wcast-align -Wconversion -Wsign-conversion`.

### Results

| Target | Corpus | Workers × wall time (CPU-hours) | ASan+LSan+UBSan executions | Throughput | `gcov` coverage | Crashes / findings |
|---|---|---:|---:|---:|---|---|
| `adi_fuzz_blob` | 18 official seeds (`tests/fuzz_seeds.py`) + random byte/insert/delete/splice mutations | 2 × 180.0 s (**0.100 CPU-h**) | **433,977,380** (218,886,162 + 215,091,218) | ~1.21M exec/s per worker | `src/adi/blob.hpp`: **100.00%** lines (40/40), **100.00%** branches executed (36/36, 83.33% taken); `tests/fuzz_blob.cpp`: 94.87% lines | **None** (18/18 seeds clean; 0 crashes, 0 ASan/LSan/UBSan reports) |
| `adi_fuzz_textproj` | 10 crafted seeds (empty, zeros, `0xff`, quotes/slashes/controls, UTF-8 bidi controls, IEEE 754 NaN/Inf/`-0.0`, K4 tie patterns) + mutations | 2 × 180.0 s (**0.100 CPU-h**) | **10,364,948** (5,208,074 + 5,156,874) | ~28.8k exec/s per worker | `src/adi/textproj.cpp`: **56.91%** lines (206/362), **51.92%** branches executed (311/599, 41.90% taken; remaining ~43% is `project(Tree)`, not invoked by `fuzz_textproj.cpp`) | **None** (10/10 seeds clean; 0 crashes, 0 invariant traps, 0 ASan/LSan/UBSan reports) |

Total: **0.200 CPU-hours** (**444,342,328** ASan+LSan+UBSan executions across both targets), **0 crashes**.

---

## 2026-11-22 — Queue item 13 (GLIBCXX-DEBUG): all 275 headless suites with `-D_GLIBCXX_DEBUG`

Head: `main` (`abbc32ab`), GCC 12.2.0 (`-O1 -g0 -D_GLIBCXX_DEBUG`, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_WERROR=OFF`).

### Overall counts (275 CTest suites)

- **272 / 275 PASS**
- **1 `_GLIBCXX_DEBUG` abort** (`adi_mix_opto_gain_tests`)
- **1 non-abort test-harness assertion failure** (`adi_link_playback_tests`, expected interaction between `RealtimeAudit` mutex interception and libstdc++ debug safe-iterator mutexes)
- **1 pre-existing build-blocked suite** (`adi_ui_seams_tests`, GCC 12 `-fsanitize-trap=signed-integer-overflow` at `CMakeLists.txt:2083-2084`)

### Findings

| Suite | Type | Location | Details & smallest repro | Owner |
|---|---|---|---|---|
| `adi_mix_opto_gain_tests` | **`_GLIBCXX_DEBUG` abort** (`SIGABRT` in `std::span` ctor `/usr/include/c++/12/span:163`: `function requires a valid iterator range [first, first + count)`) | `adi_daw/tests/test_mix_opto_gain.cpp:170,181` (`refusalAndAlias`) | Line 170 fabricates `overflow = reinterpret_cast<float*>(UINTPTR_MAX - 3)`, and line 181 constructs `std::span<const float>(overflow, 1)`. Computing `overflow + 1` wraps past the end of the address space (`0xfffffffffffffffc + 4 == 0`), violating `std::span`'s valid-range precondition (and C++ pointer-arithmetic UB). Repro: build `adi_mix_opto_gain_tests` with GCC 12 `-D_GLIBCXX_DEBUG` and run `./adi_mix_opto_gain_tests`. | `mixview` |
| `adi_link_playback_tests` | Harness note (0 `_GLIBCXX_DEBUG` aborts; 16/139 `audit.clean()` checks fail) | `adi_daw/tests/test_link_playback.cpp:318-319` | `RealtimeAudit` counts `pthread_mutex_lock` calls inside audio callbacks; under `-D_GLIBCXX_DEBUG`, libstdc++'s `__gnu_debug::_Safe_sequence_base` locks its internal safe-iterator mutex on every `std::__debug` container iteration. Not a `_GLIBCXX_DEBUG` invariant violation. | `engine` (informational) |

---

## 2026-11-22 — Queue item 12 (TSAN): 39 threaded suites under ThreadSanitizer

Head: `main` (`abbc32ab`).
- **Clang 14.0.6**: cannot link `-fsanitize=thread` on this VM (`libclang_rt.tsan-x86_64.a` / `compiler-rt` is not installed; also blocked by the 5 pre-existing `adi_core` Clang 14 errors in item 10).
- **GCC 12.2.0**: built with `-O1 -g1 -fsanitize=thread -fno-omit-frame-pointer` (`-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_WERROR=OFF`) and run under `setarch x86_64 -R` with `TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:exitcode=66` (verified active with a synthetic data-race probe returning exit code 66 with both stacks).

### Results (39 threaded suites: **38 / 39 PASS**, **1 FAIL**)

- **Clean (38 / 39 PASS, 0 data races, 0 deadlocks)**:
  - Graph workers: `adi_graph_workers_tests`
  - Host / device / CLAP / session / transport: `adi_host_tests`, `adi_device_host_tests`, `adi_clap_tests`, `adi_session_tests`, `adi_transport_generation_tests`, `adi_cue_monitor_tests`
  - Gesture: `adi_gesture_tests`, `adi_gesture_graph_tests`, `adi_gesture_playback_tests`, `adi_quiet_gesture_tests`
  - Recording / capture: `adi_audio_recording_tests`, `adi_recording_commands_tests`, `adi_track_recording_tests`, `adi_automation_recording_tests`, `adi_audio_memory_capture_tests`, `adi_midi_capture_input_tests`, `adi_midi_capture_tests`, `adi_offline_capture_tests`, `adi_freeze_capture_tests`
  - Decode: `adi_decode_tests`, `adi_decode_space_tests`, `adi_decode_cache_consistency_tests`
  - Scanner: `adi_plugin_scanner_tests`, `adi_scan_self_test_tests`, `adi_scan_cache_tests`, `adi_scan_prune_tests`, `adi_scan_catalogue_tests`, `adi_scan_process_tests`, `adi_scan_quarantine_tests`
  - Link: `adi_link_session_tests`, `adi_link_playback_tests`
  - Render / Mix View owners / control: `adi_render_queue_tests`, `adi_mix_limiter_owner_tests`, `adi_mix_focus_tests`, `adi_mix_eq_chain_tests`, `adi_mix_eq_chain_backend_tests`, `adi_control_mcp_tests`

### Finding: data race in Ogg Vorbis decoder initialization (owner: `engine`)

- **Suite**: `adi_progressive_decode_tests` (`WARNING: ThreadSanitizer: data race` on global `uint32 crc_table[256]`, `100 checks, 0 failures`, exit code 66).
- **Root cause**: `stb_vorbis.c:3707` (`start_decoder`) unconditionally calls `crc32_init()` (`adi_daw/third_party/miniaudio/extras/stb_vorbis.c:999`), overwriting the process-global `crc_table[0..255]` on every `stb_vorbis_open_file_section` call (`adi_daw/src/adi/audio/decode.cpp:223`). `ProgressiveDecoder` calls `VorbisDecoder` concurrently on the caller thread (`inspectPcm`) and worker threads (`Impl::run`), causing concurrent unsynchronized writes (and concurrent reads via `crc32_update` at `stb_vorbis.c:1005`).
- **Write 1 (main thread)**:
  - `#0 crc32_init` `third_party/miniaudio/extras/stb_vorbis.c:999`
  - `#1 start_decoder` `third_party/miniaudio/extras/stb_vorbis.c:3707`
  - `#2 stb_vorbis_open_file_section` `third_party/miniaudio/extras/stb_vorbis.c:5060`
  - `#3 VorbisDecoder` `src/adi/audio/decode.cpp:223` -> `openDecoder` (`:263`) -> `openPcmStream` (`:395`)
  - `#7 inspectPcm` `src/adi/audio/progressive_decode.cpp:51` -> `ProgressiveDecoder::openHashed` (`:338`) -> `open` (`:296`) -> `publication` (`tests/test_progressive_decode.cpp:404`)
- **Previous write (worker thread T19, created at `progressive_decode.cpp:125`)**:
  - `#0 crc32_init` `third_party/miniaudio/extras/stb_vorbis.c:999`
  - `#1 start_decoder` `third_party/miniaudio/extras/stb_vorbis.c:3707`
  - `#2 stb_vorbis_open_file_section` `third_party/miniaudio/extras/stb_vorbis.c:5060`
  - `#3 VorbisDecoder` `src/adi/audio/decode.cpp:223` -> `openDecoder` (`:263`) -> `openPcmStream` (`:395`)
  - `#7 ProgressiveDecoder::Impl::run` `src/adi/audio/progressive_decode.cpp:255` -> `ProgressiveDecoder::Impl::work` (`:167`)
- **Smallest repro**: build `adi_progressive_decode_tests` with GCC 12 `-O1 -g1 -fsanitize=thread` and run `setarch x86_64 -R ./adi_progressive_decode_tests`.

---

## 2026-11-22 — Queue item 11 (ASAN): all 275 headless suites under ASan, LSan and UBSan

Head: `main` (`abbc32ab`), GCC 12.2.0 (`-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer`, `-DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_WERROR=OFF`).
- **Clang 14.0.6**: cannot link `-fsanitize=address,undefined` on this VM (`libclang_rt.asan-x86_64.a` / `compiler-rt` is not installed; also blocked by the 5 pre-existing `adi_core` Clang 14 errors listed in item 10).

### Overall counts (GCC 12.2.0, all 275 CTest suites)

- **Strict run** (`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`): **239 / 275 PASS**, **35 FAIL**, **1 BUILD_FAIL** (`adi_ui_seams_tests`, pre-existing GCC 12 `-fsanitize-trap=signed-integer-overflow` at `CMakeLists.txt:2083-2084`).
- **Verification re-run** of the 34 non-UAF failing suites with `ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`: **34 / 34 PASS** (**273 / 274 runnable suites clean** for memory safety and UB; **0 UBSan reports** across all 274 runnable suites).

### Findings

| # | Severity / Type | Location | Suites affected | Smallest repro | Owner |
|---|---|---|---|---|---|
| 1 | **ASan heap-use-after-free** (real memory-safety bug) | Read: `adi_daw/src/adi/mix/filters/FiniteInput.hpp:15` (`sanitizeProgramme<float>`) <- `src/adi/mix/gate/Core.cpp:490,571` (`Core::processRequest`) <- `tests/test_mix_gate.cpp:858` (`completeRequestRefusal`). Freed: `tests/test_mix_gate.cpp:828` (move-assigning `std::array<std::vector<float>, 2>` invalidates raw channel pointers still passed to `processRequest` at line 858; allocated at `test_mix_gate.cpp:57,794`, 32,768 B). | `adi_mix_gate_tests` (1) | Build with GCC 12 `-O1 -g1 -fsanitize=address,undefined` and run `./adi_mix_gate_tests` | `mixview` |
| 2 | **ASan `alloc-dealloc-mismatch`** (`operator new(nothrow)` / aligned `operator new` vs custom `operator delete` -> `std::free`) | Test harnesses override global `operator new(size_t)` and `operator delete` with `malloc`/`free`, omitting `operator new(size_t, std::nothrow_t const&)` (tripped by libstdc++ `std::stable_sort` -> `std::get_temporary_buffer`) and/or aligned `operator new` (`adi_shaper_tilde.cpp:36`). Sites: `tests/test_automation_recording.cpp:22`, `tests/test_clip_envelopes.cpp:26`, `tests/test_freeze_commands.cpp:32`, `tests/test_midi_capture.cpp:31`, `tests/test_scala_import.cpp:46`, `tests/test_send_automation.cpp:21`, `tests/test_warp_playback.cpp:29` (`engine`); `tests/test_builtin_devices.cpp:38`, `tests/test_oneshot.cpp:40-42`, `tests/test_pd_native_devices.cpp:34`, `tests/test_sampler.cpp:42` (`devices`). | 11 suites (7 `engine`, 4 `devices`); all 11 PASS with `alloc_dealloc_mismatch=0` | Build with GCC 12 `-fsanitize=address` and run `./adi_builtin_devices_tests` or `./adi_warp_playback_tests` | `engine` (7) / `devices` (4) |
| 3 | **LSan memory leak** (1 B per canvas/instance load, up to 960 B / 276 allocs in `adi_pd_engine_tests`) | `third_party/libpd/pure-data/src/m_memory.c:21,41` (`getbytes`/`copybytes`) <- `pd-loader-fixed/g_canvas.c:233` (`canvas_setargs` stores `THISGUI->i_newargv = copybytes(argv, argc * sizeof(t_atom))`; `getbytes(0)` allocates 1 byte via `calloc`, never freed on `pdinstance_free` / teardown). | 23 Pd suites (`adi_pd_engine_tests`, `adi_pd_object_walk_tests`, `adi_pd_amxd_tests`, `adi_pd_template_tests`, `adi_pd_zero_delay_tests`, `adi_shaper_pd_tests`, `adi_auto_filter_pd_tests`, `adi_chorus_ensemble_pd_tests`, `adi_colorbass_pd_tests`, `adi_delay_pd_tests`, `adi_echo_pd_tests`, `adi_eq_eight_pd_tests`, `adi_multiband_dynamics_pd_tests`, `adi_oneshot_pd_tests`, `adi_overdrive_pd_tests`, `adi_phaser_flanger_pd_tests`, `adi_redux_pd_tests`, `adi_resonators_pd_tests`, `adi_reverb_pd_tests`, `adi_saturator_pd_tests`, `adi_shifter_pd_tests`, `adi_utility_pd_tests`, `adi_vocoder_pd_tests`); all 23 PASS with `detect_leaks=0` | Build with GCC 12 `-fsanitize=address` (`ASAN_OPTIONS=detect_leaks=1`) and run `./adi_pd_engine_tests` | `devices` |

---

## 2026-11-22 — Queue item 10 (WERROR-SWEEP): full-tree `-DADI_WERROR=ON` sweep on GCC 12.2 and Clang 14.0.6

Head: `main` (`abbc32ab`, includes `#712 land/mixview2-1122`), Debug (`-g0`), `-DADI_WERROR=ON -DADI_WITH_PD=ON -DADI_WITH_JUCE=OFF -DADI_ACCEPTANCE=ON -DADI_BUILD_BENCHMARKS=ON`, DF-M62 (`adi_daw/src/adi/mix/limiter/Dsp/FloatRealTransform.h:61`) patched locally and reverted after the run. Every `.o` translation unit in the tree (**739 / 739**) was compiled with `ninja -k 0`.

### Summary

- **GCC 12.2.0 (Debug, `-DADI_WERROR=ON`)**: **738 / 739 objects clean** (0 warnings-as-errors across all 738 compiling objects, including all 52 newly landed `#712` Mix View 2 files).
- **Clang 14.0.6 (Debug, `-DADI_WERROR=ON`)**: **728 / 739 objects clean** (0 warnings-as-errors across all 728 compiling objects, including all `#712` Mix View 2 files and `adi_ui_seams_tests`; 11 objects fail on C++20 front-end / libstdc++ 12 compatibility errors across 5 source sites).

### Findings beyond DF-M62 (`FloatRealTransform.h:61`, fixed on `d22b81c1`)

| File:line | Compiler / Config | Diagnostic | Repro | Owner |
|---|---|---|---|---|
| `adi_daw/CMakeLists.txt:2083-2084` (`tests/test_ui_seams.cpp`) | GCC 12.2.0 | Unrecognized option `-fsanitize-trap=signed-integer-overflow` (added in GCC 13) | `cmake -DCMAKE_CXX_COMPILER=g++ -DADI_WERROR=ON` then `cmake --build . --target adi_ui_seams_tests` | `window-fn` / `win` |
| `adi_daw/src/adi/engine/timeline_clips.hpp:13` | Clang 14.0.6 (libstdc++ 12) | `std::views::filter` fails libstdc++ 12 `sentinel_t` constraint (blocks 8 TUs: `automation_clips.cpp`, `clip_playback.cpp`, `gain_stage.cpp`, `midi_clips.cpp`, `midi_controllers.cpp`, `freeze_capture.cpp`, `freeze_commands.cpp`, `test_take_lane_ops.cpp`) | `cmake -DCMAKE_CXX_COMPILER=clang++ -DADI_WERROR=ON` then compile `automation_clips.cpp.o` | `engine` / `engine-win` |
| `adi_daw/src/adi/engine/midi_clips.cpp:659` | Clang 14.0.6 | Hard error: lambda captures structured binding `source` (Clang < 16 rejects P1091R3) | `cmake -DCMAKE_CXX_COMPILER=clang++` then compile `midi_clips.cpp.o` | `engine` |
| `adi_daw/src/adi/engine/param_ops.cpp:339,347,1995,2008` | Clang 14.0.6 | Hard error: lambda captures structured bindings `role`, `id` | `cmake -DCMAKE_CXX_COMPILER=clang++` then compile `param_ops.cpp.o` | `engine` |
| `adi_daw/src/adi/project_files.cpp:308` | Clang 14.0.6 | Hard error: lambda captures structured binding `hash` | `cmake -DCMAKE_CXX_COMPILER=clang++` then compile `project_files.cpp.o` | `engine` |
| `adi_daw/tests/test_gain_stage.cpp:138` | Clang 14.0.6 | Hard error: lambda captures structured binding `track` | `cmake -DCMAKE_CXX_COMPILER=clang++` then compile `test_gain_stage.cpp.o` | `engine` |

**Cross-references from other non-default configurations swept in items 4 and 5:**
- `-DADI_PD_FLOATSIZE_64=ON` (item 4): `adi_daw/src/adi/pd_builtins/colorcab_tilde.cpp:25` and `adi_daw/src/adi/pd_builtins/oneshot_tilde.cpp:33,78,86,88` fail `-Werror=float-conversion` (GCC 12) / `-Werror,-Wimplicit-float-conversion` (Clang 14), owner `devices`.
- GCC 12.2 Release `-O3 -DADI_WERROR=ON` (item 5): libstdc++ 12 `-Werror=restrict` (`src/adi/digest.cpp:38`, `src/juce/pd_engine.cpp:1203`), `-Werror=maybe-uninitialized` on `std::optional` (`src/adi/return_send_tap.cpp:64`, `src/adi/clip_commands.cpp:471`, `nlohmann/json.hpp:1228`), `-Werror=stringop-overflow` (`src/adi/audio/mp4_aac.cpp`), and `-Werror=mismatched-new-delete` (`tests/test_loudness.cpp:48`).

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
