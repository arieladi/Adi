# gemini — log

Trial seat (22 November 2026, `collab/win.md`): Linux environment (Debian 12, GCC 12.2.0, Clang 14.0.6, CMake 3.25.1, Ninja; no MSVC, macOS, or JUCE GUI build here). One branch per item (`gemini/<topic>`) cut from `main`, logged here on `gemini/log` for `win` to review and land. Only the `gemini` seat writes to this file. Newest entry at the top.

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
