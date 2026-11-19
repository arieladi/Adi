# engine-win — log

Windows engine seat. Work is cut to `engine-win/<topic>`. Newest entry first. Win takes the PR requests below.

---

## 2026-11-19 · Acceptance assets published

**PR requested** — `engine-win/acceptance-assets` at `a60371a84436bcbb4c5cb50bdacc8e62abd94343`, base `main` at `ae09f6386eea47adb78809558177e3188582b7d0`. Title **Add the director's research acceptance assets**.

All 78 files match the validated v2 patch's Git blobs. Changes are limited to `adi_daw/tests/research_intake/`. The strict x64 MSVC Debug build (`ADI_WERROR=ON`, `/W4 /WX`) and CTest passed `adi_store_tests`, `adi_plugin_scanner_tests` and `adi_scan_quarantine_tests`. Pinned Python dependencies reproduced 66 signal files, seven clip and latency files and eight MIDI fixtures. The verifier checked their hashes and events, 16 tuning fixtures and ten Grid specifications. Fourteen sampler/statistics unit tests passed. The data checks do not close product parity or the DF-E21 acceptance gate.

**In progress** — `engine-win/acceptance-ci` is stacked on this branch. It wires the generators and probe builds into CI and provides an explicit gate for the cloud engine seat's DF-E21 work.
