The shell suite's `browserDevices` scene passes with Pd on again, and the Browser no longer calls Hybrid Reverb unbuilt. The clones seat reported the crash in its log (22 November); the scene is the window lane's.

What was wrong
- **The scene expected Hybrid Reverb dimmed.** DEV-22 made it loadable (`builtin_catalog.cpp`: `ready` with Pd on), so its double-click loaded it onto track 4.
- **No track was selected for the device loads.** The plug-in loads before them end with `selection.track = 0` (DF-W9 on N57), so Auto Filter and Utility were refused. The strip was then read empty (`panels[0]`), and the suite crashed.
- **CI never showed it.** CI's Step 7 UI (`ui.yml`) runs the suite with Pd off, where that part is not compiled.

What changes
- **The Browser** (`BrowserPanel::unavailable`): no special case for Hybrid Reverb. With Pd off it is left out with the other Pd devices and says so ("runs on ADI's Pd devices, which this build leaves out"). The old "not built in ADI yet" is no longer true.
- **The scene:**
  - with Pd on, Hybrid Reverb is loadable like the rest, checked without loading it, so track 4 stays empty;
  - with Pd off, it is refused with the Pd reason;
  - track 4 is selected again before the device loads;
  - the strip is read only when it holds a panel.

Tests
- **Pd off**, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`) on this branch: `adi_app` and the six UI suites `ui.yml` runs build and pass.
- **Pd on:** the same commit runs in `window/return-in-group`'s tree (#677's head plus main), with the same lines in both files.
  - `adi_ui_shell_tests` runs through, 2981 checks, 0 failures. Before the change it stopped in `browserDevices` (FAIL: the Audio Effects check, Hybrid Reverb, Auto Filter, Utility, "the strip shows it"), then crashed.
  - `tools/test_all.sh`: PASS, 63701 checks across 269 suites.
- **The sweeps:** the shadow and clang warning sweeps are clean on `ui_browser.cpp` and this branch's lines in `tests/test_ui_shell.cpp`. They flag main's own two lines, as on every window branch.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
