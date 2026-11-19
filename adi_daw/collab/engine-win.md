# engine-win — log

Windows engine seat. Work is cut to `engine-win/<topic>`. Newest entry first. Win takes the PR requests below.

---

## 2026-11-19 · Hosted acceptance gate and owned publication staging

**PR requested** — `engine-win/acceptance-ci` at `4c25144068552da347f5bfe23dbeb3fa89d89e0a`, stacked on `engine-win/acceptance-assets` at `a60371a84436bcbb4c5cb50bdacc8e62abd94343`. Title **Run research fixtures and gate ADI hosted transport acceptance**.

Windows hosting CI builds VST3, CLAP, reporting-delay and scanner diagnostics. The strict x64 MSVC build passed all three module self-tests, the DF-E21 oracle and existing hosted render test. The Python job verified the 82-input SHA-256 manifest, reproduced 66 signals, seven edge and latency inputs, eight MIDI files and their events, and checked 16 tuning inputs and ten Grid specs. The remaining hosting leg is being ported separately.

**For the cloud engine seat** — after taking this branch, configure Windows hosting CI with `-DADI_ENABLE_DF_E21_ACCEPTANCE=ON` on the DF-E21 implementation. `adi_df_e21_host_acceptance` uses the real scanner, Session, VST3 host and render runner. Both real hosted delays report 1024 or 4800 samples after preparation. The deliberate main baseline fails all twelve cases at missing VST3 ProcessContext. Later timing assertions need the transport implementation. The default oracle remains green. CLAP and the other full-contract additions are listed in the test README. DF-E23 awaits the cloud worker implementation before the ADI recording-buffer comparison.

**PR requested** — `engine-win/publication-owned-copy` at `629a9f3de471b6d38857c25342f8962221178380`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Publish verified copies without destination cleanup races**.

DF-E19(b). The fallback streams and confirms a privately staged copy, then claims the final name with an exclusive hard link. Failure cleanup never unlinks the public destination. The copy returns its file identity and directory sync result. Strict x64 MSVC passed the collect/export and two project publication suites. The late-collision and injected-failure checks both retain the other writer's bytes and the source stage. A filesystem with neither the permitted exclusive rename nor hard links now refuses publication. Per-caller policy and job/run reporting are separate steps.

## 2026-11-19 · Scanner success cache and complete export names

**PR requested** — `engine-win/scanner-success-cache` at `f8fa0efaa8dd154e143e393d66342cb7382bc875`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Cache validated plug-in discovery results**.

DEV-25 and DF-E20(c), first scanner step. Registry v3 keeps a whole validated positive packet keyed to format, canonical path, file size, modification time, relevant bundle files, protocol and helper build. Three unchanged probes launch one owned child. Changed size with preserved mtime, changed mtime, changed helper, invalid cache, interrupted completion and legacy v2 migration are covered. An unavailable fingerprint still permits isolated discovery and reports that it could not cache. Strict x64 MSVC Debug passed all five scanner, cache, process, quarantine and registry suites. Negative outcomes, the rest of the retry policy, catalogue-first loading and readiness phases are separate work. The sixteen-case acceptance is still open.

**PR requested** — `engine-win/export-final-names` at `96ac4186d6cfeff80664ec8a761e40bcd3d05b23`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Check complete export filenames against the byte limit**.

DF-E19(d) and EX-07. WAV and NI Stems validate the final 200-byte UTF-8 budget including the full format suffix and reserved-name prefix. Strict x64 MSVC Debug passed 245 queue checks and 61 NI metadata checks. Native WAV publication accepted 199 and 200 bytes and refused 201. The case-only collision left the first file intact on the Windows test filesystem. The NI suffix boundary passed its codec-independent check. The native AAC codec was unavailable for actual NI boundary renders. Cross-filesystem case folding and the full EX-07 collision-policy choices remain open.

## 2026-11-19 · Acceptance assets published

**PR requested** — `engine-win/acceptance-assets` at `a60371a84436bcbb4c5cb50bdacc8e62abd94343`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Add the director's research acceptance assets**.

All 78 files match the validated v2 patch's Git blobs. Changes are limited to `adi_daw/tests/research_intake/`. The strict x64 MSVC Debug build (`ADI_WERROR=ON`, `/W4 /WX`) and CTest passed `adi_store_tests`, `adi_plugin_scanner_tests` and `adi_scan_quarantine_tests`. Pinned Python dependencies reproduced 66 signal files, seven clip and latency files and eight MIDI fixtures. The verifier checked their hashes and events, 16 tuning fixtures and ten Grid specifications. Fourteen sampler/statistics unit tests passed. The data checks do not close product parity or the DF-E21 acceptance gate.

**In progress** — `engine-win/acceptance-ci` is stacked on this branch. It wires the generators and probe builds into CI and provides an explicit gate for the cloud engine seat's DF-E21 work.
