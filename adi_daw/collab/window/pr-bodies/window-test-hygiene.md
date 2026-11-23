Test hygiene, first half: the suite fails on a JUCE assertion, opens no dialog, and checks its repaints (WTS-02; ADR-0180 d8).

What it does
- **`AssertionWatch`** (`tests/ui_test_hygiene.hpp`): JUCE's assertions are logged with `JUCE_LOG_ASSERTIONS` on `adi_ui_shell_tests` only (never `adi_app`). Each one is counted, and the suite fails on one. A self-test trips one on purpose and must see it counted.
- **The machine's own assertions:** three are counted apart and said once, never failing the run, each pinned to one line of the vendored JUCE:
  - the message queue full: a headless suite posts messages and runs no loop;
  - a cursor shown with no X display;
  - the ALSA sequencer refused: there is no MIDI system.
  Every other assertion, in those files too, fails the run. If a JUCE update moves a line, that assertion fails again and shows where to re-pin.
- **The first full run found 112 real assertions,** nearly all from main's code: `grabKeyboardFocus` on a view not on the screen, in the Clip View and Arrangement scenes, whose roots are off the desktop. JUCE gives such a component nothing and asserts.
  - `focusIfShowing` (`src/juce/ui_look.hpp`) now asks only where the view is shown, at 33 sites in the window's files and the shared `ui_mixer.cpp`. Behaviour does not change.
  - `ui_stack.cpp` (the Mix View seat's) is left as it is: its one unguarded site does not assert in the suite.
- **`noDialogs`:** on Windows the C runtime reports to stderr and opens no box, in Release as in Debug. It replaces the Debug-only guard.
- **`paintHygiene`:** the window painted whole and in 37-pixel strips agrees within 4 per channel. A paint at each zoom limit (1 and 4096 pixels a quarter, at the start and far on) finishes.
- **Not yet:**
  - the paint census waits on its definition;
  - `test_all.sh` counting the JUCE suite is win's.

Review
An adversarial review confirmed one defect, fixed in `062523bc`: the watch set apart every assertion in `juce_Messaging_linux.cpp`, silently. It now sets apart only the queue-full line, and says it.

Tests
- Pd on, head `9e946f45`, the binary built in the gate:
  - `adi_ui_shell_tests` 2936 checks, 0 failures, with the watch on (the machine's three counted apart);
  - `adi_ui_seams_tests` 553, `adi_ui_document_tests` 313, `adi_ui_window_state_tests` 109, 0 failures.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- The full build stops only at main's `tests/scanner_session_reopen.hpp` 208 under GCC 13, reported to the engine seat.
- The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6200 and 12136, `tests/test_ui_mix_functions.cpp` 675).
- Stacked on `window/input-clock` (DF-W18's InputClock). It touches `ui_mixer.cpp`, shared with window-fn: three focus calls only.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
