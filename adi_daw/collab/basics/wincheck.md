# Windows check

The Windows checker builds each moved chain before win's landing check. Newest entries are on top.

**Setup.** Visual Studio 2022 (MSVC 19.44, x64), Ninja, Debug, `ADI_WERROR=ON`, `ADI_ACCEPTANCE=ON`, two builds:
the window (`ADI_WITH_JUCE=ON`, `ADI_WITH_PD=ON`) and headless (`ADI_WITH_JUCE=OFF`, `ADI_WITH_PD=ON`).
Pinned sources from `tools/fetch_external.sh --with-juce`, all pins verified, JUCE 9.0.5 at `d60b7a78`.
Every target of both builds is built, then `ctest -LE performance` runs in the headless build. The window suites
are not run and `ADI.exe` is never started: the desktop may be in use.

A fix for a branch is one commit on `wincheck/<branch>`, made from that branch's checked head, for its author to merge.

| When (UTC) | Branch and head | Built | Headless suites | Errors (file:line) and the smallest fix |
|---|---|---|---|---|
| 2026-11-27 16:40 | `seats/session-chain` `1d32a673fc8b4910a02e689e31065560e3a1a9e5` | **No**: headless and window both stop in `adi_core` (18 units) | Not run: every suite linking `adi_core` was missing | 1. `src/adi/engine/launch_gestures.hpp:112` C4324, the `alignas(64)` counters pad `LaunchGestureQueue`. Fix: `#pragma warning(disable: 4324)` around the class, as `mix/limiter/DisplayTelemetry.h` does. 2. `src/adi/engine/launch_follow_playback.cpp:113` C4127, `jumpCanCrossGroup` is `constexpr`. Fix: `if constexpr`, same behaviour. **Both on `wincheck/seats/session-chain` `5658f185201e83b624fa2ddcb1dcfdbe157c96b8`, for the Session seat to merge.** With it: every target of both builds compiles, 288 of 289 headless suites pass, and `adi_library_tests` (below) passes alone. |
| 2026-11-27 16:40 | `main` `17a6888039781f9d34c69d2dc400a6f8c98038ad` (every branch) | Yes | `adi_library_tests` timed out in the parallel run | `CMakeLists.txt:1889` `TIMEOUT 120`. The index worker's `THREAD_MODE_BACKGROUND_BEGIN` (`src/adi/library/platform.cpp:19`) is starved by busy suites: 2 s alone, 137 s and 57 s beside 16 busy suites, passing each time, so this is not a hang. Fix: `RUN_SERIAL TRUE` on the suite, **on `wincheck/main` `9734b0c449b584103b8e1e125f902902522cf025`**, for win or the library's owner to merge. Until it lands, this checker reruns a timed-out suite alone before reporting it. |
