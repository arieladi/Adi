The function keys do functions (WIN-01; ADR-0209 d9). F1 to F8 lose Live's Activate/Deactivate Track defaults. The eight commands stay, unbound, for a user's own keys. F4 is the Mix View's alone, and F3 and F7 stay free, held for the Session View and offline processing. parity-7.6 row 6 says ADI departs from Live here.

What it does
- **Defaults:** `ActivateTrack1` to `ActivateTrack8` have no default key; every other key is as before.
- **The key-map golden** (a new shell scene, `keymapGolden`): each shortcut preset's default map (live, cubase, bitwig) as text the same across a platform family. Modifiers are read as Cmd for the command key, Ctrl for the Mac's own, then Alt and Shift.
  - No key may be the default of two commands. Main fails here at F4 (Mix View and Activate Track 4).
  - The map is compared with its golden, `ADI_UI_REFERENCES/keymap-<preset>-<pc|mac>.txt`. Each run leaves `keymap-<preset>-<family>-actual.txt` in the working directory.
  - The PC goldens follow as one more commit on this branch, from the Linux run's output. Until then the scene says no golden is there and runs the no-clash half. The Mac's are written on a Mac with `--write-references` (the director's question in the window log).
- **F4, headless:** the Mix View opens once and no track's activator changes (MX-14).

Tests
- Pd on: `adi_ui_shell_tests` 2934, `adi_ui_seams_tests` 553, `adi_ui_document_tests` 313, `adi_ui_window_state_tests` 109. All 0 failures.
- Pd off, configured as `ui.yml`: `adi_app` and the six UI suites build and pass.
- The full build stops only at main's `tests/scanner_session_reopen.hpp` 208 under GCC 13 (`-Werror=range-loop-construct`), reported to the engine seat on 22 November. It is not this branch's.
- The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6200 and 12104, `tests/test_ui_mix_functions.cpp` 675).
- Carried until it lands: `window/browser-devices-test`'s commit, so main's Pd-on shell suite runs through.
- `window/shortcut-ids` (WIN-04) and `window/command-palette` (WIN-06) stack on this branch.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
