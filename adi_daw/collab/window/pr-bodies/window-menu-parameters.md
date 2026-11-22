A check that every item of a parameter shown as a menu reaches the plug-in, by the mouse's way and the keyboard's, with a frame between the pick and JUCE's delivery of it (DF-W19). main already carries the fix (`2b89c1db`, the Mix View seat's `ParameterControl` menu: the pick is recorded before any gesture, and a refused pick shows the value playing again). This branch is now that fix's test alone.

What it does
- **The walk:** from the last item, so every pick of the walk is a change (a pick of the value already playing is no gesture, main's guard). Each pick is sent as Begin, Value and End, and the next frame shows it.
- **A refused pick** shows the value playing again.

Review
An adversarial review confirmed one defect, fixed in `a27ecd33`: the walk began on the value playing, so main's guard sent nothing for its first pick and both checks failed against main's correct code. Two findings were refuted (the refused-pick check's ordering; mouse and keyboard as one input path).

Tests
- Pd on: `adi_ui_shell_tests` 2927 checks, 0 failures; `adi_ui_seams_tests` 553, `adi_ui_document_tests` 313, `adi_ui_window_state_tests` 109.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- `tools/test_all.sh` was run once this session on `window/grid-ticks`'s tree, at main `ac07be74` plus window code only (PASS, 64092 checks across 270 suites). This branch changes window code only, so its other suites are main's.
- The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6199 and one near 12100, `tests/test_ui_mix_functions.cpp` 675).
- Carried until it lands: `window/browser-devices-test`'s commit, so main's Pd-on shell suite runs through.
- For the Mix View seat: the fix now lives in your `ui_devices.cpp`; this check guards it through your rebuild.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp

