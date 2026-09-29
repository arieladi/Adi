# PR #167 — drone parallel workers

Merged main after the Windows Pd flake. Main's README count and legacy log
are retained unchanged; this per-PR log preserves the earlier branch entries.
The Windows Pd investigation remains with the analyser session.

## 2026-09-28 - drone main refresh after Windows runner timeout

Merged main into codex/drone-parallel, preserving both log histories. The six
temporary-folder drone tests pass again. Prior Windows CI timed out in existing
C++ suites; this merge requests a fresh run without changing those tests.

---

## 2026-09-28 - mission 3, Task D: concurrent drone requests

Win delegated tools/adi-drone/drone.py for this change. On codex/drone-parallel
from main, added --parallel N to watch/run-once with N request workers, default
1 on the original calling thread. Claims use queue-to-running atomic rename
plus a short intra-process lock: a forced race demonstrated that two concurrent
Win32 rename calls can both open the source before either move completes.
Logs retain their format and are serialized per line; timing/eta/collect are
unchanged. All workers join before idle unload. Recovery still uses running/
and the existing partial-result replacement. No live drone directories changed.

Six temporary-folder tests pass: duplicate claim, four requests in flight,
crash/recovery, byte-for-byte serial comparison with 92eb485, watch idle/unload,
and invalid parallel counts. No Ollama requests or scheduled tasks were created.
