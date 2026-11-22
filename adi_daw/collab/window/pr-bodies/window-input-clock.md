The window reads the time and the keys held through one seam, `InputClock` (ADR-0231 d2; DF-W18), which a test sets. So the shell suite no longer reads the machine's keyboard or clock, and a slow machine no longer turns a tap into a hold.

What it does
- **The seam** (`src/juce/ui_input_clock.hpp`, `AdiRootComponent::input`): milliseconds and key state, unset reading the system's.
- **The readers routed through it:** `keyIsDown`, the latches in `keyStateChanged`, `AppCommands::key`, Play Reference from the menu, Tap Tempo, the fade handles' F, and the Mix View's [REF] button.
- **Play Reference from a menu** reads the clock once, so it is a tap at one reading, however slow the machine.

Review
An adversarial review confirmed one defect, fixed in `90aed608`: [REF] still timed its press on the machine's counter. A check clicks it with the test's clock 600 ms between press and release, which is a hold, where the old code took it for a tap. One finding was refuted.

A further fix (`b57b118e`): that check left the Reference off, so the two checks after it began from the wrong state in the Pd-off tree. It now leaves the Reference as it found it.

Tests
- Pd on: `adi_ui_shell_tests` 2928 checks, 0 failures:
  - Play Reference from the menu latches with the clock 300 ms on at each reading;
  - a tap under 200 ms latches;
  - [REF] held 600 ms is a hold;
  - Shift+R tap and hold.
- Also Pd on: `adi_ui_seams_tests` 553, `adi_ui_document_tests` 314, `adi_ui_window_state_tests` 109.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites build and pass (shell 2914 checks).
- `tools/test_all.sh` was run once this session, on `window/grid-ticks`'s tree (main `ac07be74` plus window code only): PASS, 64092 checks across 270 suites. This branch changes window code only.
- The shadow and clang warning sweeps are clean on this branch's lines; they flag only main's own lines.
- Carried until it lands: `window/browser-devices-test`'s commit.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
