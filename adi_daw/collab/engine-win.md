# engine-win — log

Windows engine seat. Work is cut to `engine-win/<topic>`. Newest entry first. Win takes the PR requests below.

---

## 2026-11-19 · Scanner success cache and complete export names

**PR requested** — `engine-win/scanner-success-cache` at `f8fa0efaa8dd154e143e393d66342cb7382bc875`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Cache validated plug-in discovery results**.

DEV-25 and DF-E20(c), first scanner step. Registry v3 keeps a whole validated positive packet keyed to format, canonical path, file size, modification time, relevant bundle files, protocol and helper build. Three unchanged probes launch one owned child. Changed size with preserved mtime, changed mtime, changed helper, invalid cache, interrupted completion and legacy v2 migration are covered. An unavailable fingerprint still permits isolated discovery and reports that it could not cache. Strict x64 MSVC Debug passed all five scanner, cache, process, quarantine and registry suites. Negative outcomes, the rest of the retry policy, catalogue-first loading and readiness phases are separate work. The sixteen-case acceptance is still open.

**PR requested** — `engine-win/export-final-names` at `96ac4186d6cfeff80664ec8a761e40bcd3d05b23`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Check complete export filenames against the byte limit**.

DF-E19(d) and EX-07. WAV and NI Stems validate the final 200-byte UTF-8 budget including the full format suffix and reserved-name prefix. Strict x64 MSVC Debug passed 245 queue checks and 61 NI metadata checks. Native WAV publication accepted 199 and 200 bytes and refused 201. The case-only collision left the first file intact on the Windows test filesystem. The NI suffix boundary passed its codec-independent check. The native AAC codec was unavailable for actual NI boundary renders. Cross-filesystem case folding and the full EX-07 collision-policy choices remain open.

## 2026-11-19 · Acceptance assets published

**PR requested** — `engine-win/acceptance-assets` at `a60371a84436bcbb4c5cb50bdacc8e62abd94343`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Add the director's research acceptance assets**.

All 78 files match the validated v2 patch's Git blobs. Changes are limited to `adi_daw/tests/research_intake/`. The strict x64 MSVC Debug build (`ADI_WERROR=ON`, `/W4 /WX`) and CTest passed `adi_store_tests`, `adi_plugin_scanner_tests` and `adi_scan_quarantine_tests`. Pinned Python dependencies reproduced 66 signal files, seven clip and latency files and eight MIDI fixtures. The verifier checked their hashes and events, 16 tuning fixtures and ten Grid specifications. Fourteen sampler/statistics unit tests passed. The data checks do not close product parity or the DF-E21 acceptance gate.

**In progress** — `engine-win/acceptance-ci` is stacked on this branch. It wires the generators and probe builds into CI and provides an explicit gate for the cloud engine seat's DF-E21 work.
