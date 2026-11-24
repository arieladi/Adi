Live's Help menu: the manual and a keyboard reference (WIN-09; ADR-0203 d1, d5). On the Mac the system's Cmd+? menu search then finds ADI's menus.

What it does
- **The menu:** a Help menu, last on the menu bar, with two commands.
- **Live 12 Manual:** opens the online manual (`AdiRootComponent::kManualAddress`, one constant; the director's question in the window log asks which manual).
- **Keyboard Shortcuts:** writes every command and its keys, in the current key map, to an HTML page in the temporary folder and opens it.
- **Alt+H** opens the menu off the Mac (§42.28).

Tests
- `adi_ui_shell_tests`: the Help menu, last, with the manual and the keyboard reference; the manual's address opened through a hook, as the test sets it; the reference written, escaped, and naming every command.
- **Older checks updated:** the menu bar's list, AppCommands' tail and the Alt letters now include Help (`4af48b01`; the first gate found them).
- Pd on, the gate's own build of `4af48b01`: shell 2928 checks, seams 553, document 313, window_state 109, 0 failures.
- Pd off as `ui.yml` builds it (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): adi_app and the six UI suites, 6 of 6 passed.
- The full build stops only at main's GCC 13 `tests/scanner_session_reopen.hpp` 208. The shadow and clang warning sweeps flag only main's lines (`tests/test_ui_shell.cpp` 6199 and 12103, outside this branch's hunks; `tests/test_ui_mix_functions.cpp` 675, which it does not touch).
- The branch carries `912b3c98`, the window lane's fix that lets `browserDevices` pass with Pd on (Hybrid Reverb loads), so the gate runs green; it no-ops once main has it.
- `window/shortcut-ids` gives the two commands their stable ids when the second of the two lands.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
