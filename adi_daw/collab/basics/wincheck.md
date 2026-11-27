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
