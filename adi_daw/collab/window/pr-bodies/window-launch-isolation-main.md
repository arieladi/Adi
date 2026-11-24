A test launch runs on its own, quietly (WTS-01, the window's half; ADR-0180 d8; ADR-0108 d4, d5). Rebuilt on main as win asked (collab/win.md, 24 November): `window/launch-isolation`'s two commits, `f4065789` and `44fdaf45`, each with exactly its old lines, around WIN-02's places and WIN-03's kinds. It replaces that branch.

What it does
- **An instance of its own:** a launch with `--smoke-output` or `--control-test` runs beside an ordinary ADI instead of handing its command line to that copy (`moreThanOneInstanceAllowed`). The running copy ignores any such line that reaches it.
- **Quiet:** its window shows without taking the foreground. JUCE brings a window to the front as it shows; `AdiWindow::quiet` skips that, and so does `SettingsWindow::quiet` for a smoke run's Settings window (the review's finding, `44fdaf45`). It reports to stderr, never in a window.
- **ADI_HOME:** `--control-test` refuses to start without `ADI_HOME` (exit 2). A smoke run without it says so on stderr and keeps its settings beside its project.
- **For win:** please set `ADI_HOME` in `ui.yml`'s native smoke step (e.g. `$env:ADI_UI_BUILD/home`). The smoke then refuses without it as well.
- **Not here:** the control API is WTS-05's.

Tests
- `adi_ui_shell_tests` `launchIsolation`: the two flags make a test launch, quoted or not; an ordinary launch, a file named after a flag, or a quoted name holding its words is not; a quiet window shows and takes no focus.
- The built app with `--control-test` and no `ADI_HOME`: it says "--control-test needs ADI_HOME, a folder of its own for this instance" and exits 2 (AT-7).
- GCC 13, Debug, `-DADI_WERROR=ON`, JUCE 9.0.5, the gate's own build of `788908b2`: shell 3162, seams 580, document 337, window_state 177, 0 failures; the full build is clean.
- Pd off as `ui.yml` builds it: adi_app and the six UI suites, 6 of 6 passed.
- The shadow and clang sweeps flag only main's lines (`src/juce/ui_main.cpp` 399 and 406, `6b78d94c`; `tests/test_ui_shell.cpp` 6351, 13432, 17078).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
