Stable command ids in Live's menu categories, and user keys that survive (WIN-04, first half; ADR-0180 d6; ADR-0125). Settings' Shortcuts page is JUCE's key editor.

What it does
- **Stable ids:** every command has a stable text id (`view.browser`, `edit.redo`, 122 of them) that never changes and is never reused, with Live's menu as its category in place of "ADI". `commandFor` reads an id back.
- **Canonical chords:** a chord has one text on every platform (Cmd for the command key, Ctrl the Mac's own, then Alt, Shift, the key), read back by `parseChord`. The key-map golden now shares it.
- **Overrides:** `reload()` puts `shortcuts.overrides` on top of the preset's keys, so a Settings change or a preset switch keeps the user's keys. `storeUserKeys()` writes the keys that differ from the key map, and only when they differ from what the record holds; ids this build does not know keep their keys.
- **The Shortcuts page:** JUCE's `KeyMappingEditorComponent` in place of the record's row, in the theme's colours, grouped by Live's menus. A key changed there is written at once; a failed write says why under it.

Tests
- `adi_ui_shell_tests` `userShortcuts`: every command has one stable id that names it back, and a Live menu category; every preset's chords read back from their text; the user's keys survive a Settings change; a preset switch keeps them on top while other commands take the preset's keys; unchanged keys write nothing; a changed key is written and comes back after a reload; the page shows the editor in place of the record, and a key cleared there is written at once.
- Pd on, the gate's own build of `15e46144`: `adi_ui_shell_tests` 2959 checks, `adi_ui_seams_tests` 553, `adi_ui_document_tests` 313, `adi_ui_window_state_tests` 109, 0 failures.
- Pd off, configured as `ui.yml`: `adi_app` and the six UI suites build and pass.
- The full build stops only at main's GCC 13 `tests/scanner_session_reopen.hpp` 208. The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6200 and 12104, `tests/test_ui_mix_functions.cpp` 675).
- **The head, `2cfaacbd`,** adds only `window/function-keys`' key-map goldens. They are this gate's own output, so the golden check holds.
- `window/help-menu` adds two commands. Whichever of the two lands second gives them ids (`help.manual`, `help.keyboard-shortcuts`); the stable-id check fails until then.
- An adversarial review confirmed three defects, all fixed in `15e46144`:
  - the key editor needs `juce_gui_extra`, which the shell library now links;
  - a preset switch with the page open could erase a user's key equal to the new preset's (a new check covers it);
  - a shadowed local.
  Also from the review: `setColours` reaches the editor's tree, and the View submenus' toggles are View's.
- Stacked on `window/function-keys` (WIN-01). `window/shortcut-sets` (WIN-05) and `window/command-palette` (WIN-06) stack on this branch.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
