A user's key wins over the preset's, and clashes and reserved chords are told (WIN-05, part; ADR-0180 d6, ADR-0203 d21's precedent). On `window/shortcut-sets` (WIN-05's first half); it lands after it, after 3 December.

The defect it fixes
- JUCE runs the first mapping that holds a key, and `reload()` appended a user's key after the preset's. So a key a user gave one command, while another command held it by default, never ran the user's command, and nothing said so (Stop on Cmd+Y still ran Redo).

What it does (`src/juce/ui_shell.*`)
- **One plan for the keys** (`AppCommands::planKeys`, pure): from the preset's keys and `shortcuts.overrides`, laid in command order:
  - **Shadow:** a user's key takes the chord from the command that held it by default; that command keeps its other keys, and the issue is told.
  - **Clash:** a chord two user keys claim is bound to both, the first in command order running it (never the record's order), told.
  - **Reserved:** a chord the system keeps on this OS is not used: Alt+F4 and Alt+Tab on Windows; Cmd+Q, Cmd+H, Cmd+Alt+H and Cmd+\` on the Mac; none on Linux (each row with its source). It stays in the record, so a set still crosses systems.
  - **Unreadable** chords stay in the record, unused and told. A list with no usable chord keeps the preset's keys; `[]` still unbinds.
- **The record follows the plan:** written only when the keys in force differ, unknown ids and unusable chords kept; the editor's reset clears an entry. Export writes what import reads.
- **The Shortcuts page** names the first issue (and how many more); it refuses a reserved chord as it is assigned, putting back a key the editor's Change had removed. An import names its issues too.

Tests
- `adi_ui_shell_tests` `keyConflicts`: the reserved table per OS (and its rows read back), no preset default reserved (the PC goldens checked against the Windows table), `planKeys` exactly for each rule, the defect (Cmd+Y given to Stop runs Stop, Redo loses it and is told, the record untouched), clash order against alphabetical order, a record kept verbatim, the editor's refusal and its put-back, an import's issues and its round trip, and a Hebrew Z through `latinKey`. `userShortcuts`, `shortcutSets` and the key-map goldens unchanged.
- GCC 13, Debug, `-DADI_WERROR=ON`, JUCE 9.0.5, the gate's own build of `cc7174a1`: shell 3221 checks, seams 580, document 337, window_state 177, 0 failures; the full build is clean.
- Pd off as `ui.yml` builds it: adi_app and the six UI suites, 6 of 6 passed.
- The shadow and clang sweeps: `ui_shell.cpp` clean; only main's lines in `tests/test_ui_shell.cpp` (6351, 13432, 17431).
- Two adversarial review rounds (eleven defects, all fixed); the gate's first run found a twelfth (the export iterated a temporary that C++20 destroys before the loop's body), fixed in `cc7174a1`.

Left for later, each named in the window log with why: the Steal/Keep/Refuse prompt and search by pressed key, the dispatch stages, keys typed into a native plug-in editor, the native physical-key source, the full Cubase and Bitwig tables, foreign key files, the wider reserved lists.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
