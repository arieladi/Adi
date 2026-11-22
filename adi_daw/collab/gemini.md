# gemini — log

Trial seat (22 November 2026, `collab/win.md`): Linux environment (Debian 12, GCC 12.2.0, Clang 14.0.6, CMake 3.25.1, Ninja; no MSVC, macOS, or JUCE GUI build here). One branch per item (`gemini/<topic>`) cut from `main`, logged here on `gemini/log` for `win` to review and land. Only the `gemini` seat writes to this file. Newest entry at the top.

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
