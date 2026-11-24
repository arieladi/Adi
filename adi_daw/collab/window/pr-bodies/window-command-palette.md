The command palette on Ctrl/Cmd+P, ranked by fzy (WIN-06; ADR-0104 d3, d4; ADR-0203 d6). It is ADI's addition (Live has none), drawn in today's look until the menus and dialogs sheet is agreed.

What it does
- **The matcher** (`src/adi/ui/fuzzy_match.hpp`): fzy's optimal matcher ported to UTF-32 and JUCE-free, with its constants, bonuses and backtrace (MIT, attributed in the file). A lower-case table of ADI's own (ASCII, Latin-1, Greek, Cyrillic) stands in for tolower and setlocale. Hebrew, which has no case, matches as it is.
- **The palette** (`src/juce/ui_palette.*`): opens over the window on Ctrl/Cmd+P, a new command with the stable id `view.command-palette`.
  - Every command is listed with its key, ranked by fzy, its matched letters in the accent; one that cannot run now is dimmed with why.
  - Up and Down choose, Enter runs, Esc and a lost focus dismiss.
- **V/** lists ADI's devices by name, and Enter inserts one on the selected track as one undo step. The plug-ins under V/ follow, once the Browser's list is reachable from the root. S/ and P/ wait for the store.

Tests
- `adi_ui_window_state_tests` `fzyCases`: fzy's 24 tests (test/test_match.c) with their exact scores, then Hebrew and Cyrillic.
- `adi_ui_shell_tests` `commandPalette`:
  - Ctrl/Cmd+P opens it centred, with every command for an empty query;
  - "hsb" ranks Hide/Show Browser first, H, S and B marked;
  - Redo with nothing to redo is dimmed with why;
  - Enter runs the row and the palette goes; reopened it is empty, and Esc dismisses;
  - V/utility inserts Utility on the selected track as one undo step (Pd on); with Pd off, V/ lists nothing.
- Pd on, JUCE 9.0.5, the gate's own build of `051ec6c0` (main merged after #739; the palette after Help, id 29055): shell 3168 checks, seams 580, document 337, window_state 204, 0 failures; the full build is clean.
- Pd off as `ui.yml` builds it (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): adi_app and the six UI suites, 6 of 6 passed.
- The shadow and clang warning sweeps flag only main's lines (`tests/test_ui_shell.cpp` 6351, 13432, 17120); `src/juce/ui_palette.cpp` is clean.
- **Review:** an adversarial review confirmed one defect, fixed in `e9af9bdb`: the shell's older check of AppCommands' tail still ended at Export Audio. It now ends with the Command Palette, the last id.
- **Key-map goldens:** the branch carries `window/function-keys`' goldens with the palette's own line added (`29055 Command Palette...: Cmd+P`, after main's Options commands, 29050 to 29052, and Help's two, 29053 and 29054), and the gate checks them against the shell's output.
- On main: WIN-04 (`window/shortcut-ids`) and WIN-09's Help landed in #739.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
