The mixer strip's meter shows the engine's held peak, taken by one owner and shared by every view of the strip. A transient between two frames reaches the Peak Level pill and the peak line, and no view steals another's (DF-W32; the window half of N48). The look does not change. Lands with the mixer's sheet; win places it.

What it does
- **One owner, `HeldPeaks`** (`src/adi/ui/held_peaks.hpp`): takes the engine's held peak (`StripMeter::takePeak`) once and keeps, for each view that reads it, the highest taken since that view last read. Message thread only; no allocation once each view has read once.
- **A strip not framed lets its share go:** a pooled strip hidden by the panel's layout, or every strip of a hidden panel (the Mixer View closed), gives up its share. Shown again, it reads afresh and never draws an old transient as the current level.

Review
An adversarial review confirmed two defects, both the same fault, fixed in `09c2153c`: strips that were not framed stayed registered readers and later showed a stale transient.

Tests
- Pd on: `adi_ui_window_state_tests` 109 checks:
  - one transient, taken once, shown by both views;
  - a slower view shows the highest since it last read;
  - another strip's meter is its own;
  - a view shown again reads what is taken then, not a transient it missed.
- Also Pd on: `adi_ui_shell_tests` 2927, `adi_ui_seams_tests` 553, `adi_ui_document_tests` 313. All 0 failures.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- `tools/test_all.sh` was run once this session on `window/grid-ticks`'s tree, at main `ac07be74` plus window code only (PASS, 64092 checks across 270 suites). This branch changes window code only, so its other suites are main's.
- The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6199 and one near 12100, `tests/test_ui_mix_functions.cpp` 675).
- Carried until it lands: `window/browser-devices-test`'s commit, so main's Pd-on shell suite runs through.
- It shares `ui_mixer.*` with window-fn's MX-23 and MX-02: the second to land merges main.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp

