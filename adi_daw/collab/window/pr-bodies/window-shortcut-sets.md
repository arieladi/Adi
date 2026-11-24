Keys under any keyboard layout, and key sets that move between platforms (WIN-05, part; ADR-0125 d4; ADR-0180 d6).

What it does
- **Non-Latin layouts:** a key typed under a Hebrew (SI-1452) or Russian (JCUKEN) layout that is no command's runs as the Latin key at its place, with its modifiers. So Ctrl/Cmd+Z undoes and Z and X zoom whatever layout types.
- **Import and Export** on the Shortcuts page: a key set as a file (`adi-shortcuts`, version 1: the preset and the overrides in canonical chord text).
  - A set exported on Windows imports on the Mac, Cmd being the command key on both.
  - A file that is not a key set, or a chord no one can read, is refused and named, and nothing changes.
  - The keys of a command this build lacks are kept.
- **Not yet** (the rest of WIN-05): reading foreign key files; Steal, Keep or Refuse; clash against shadow; reserved system chords; search by pressed key. The A5W-015 Live reading stays AWAITING.

Tests
- `adi_ui_shell_tests` `shortcutSets`: Hebrew's zayin and samekh, and Russian's ya and che in either case, run Undo and the zooms; a Latin key and an unknown letter are left as typed; a Windows-written set imports with the command key, its preset and an unknown command's keys; export and import round trip; a bad file and an unreadable chord are refused and named.
- Pd on, JUCE 9.0.5, the gate's own build of `c08afd7b` (main merged after #739): `adi_ui_shell_tests` 3171 checks, `adi_ui_seams_tests` 580, `adi_ui_document_tests` 337, `adi_ui_window_state_tests` 177, 0 failures; the full build is clean.
- Pd off, configured as `ui.yml`: `adi_app` and the six UI suites build and pass.
- The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6351, 13432, 17113).
- **Review:** an adversarial review confirmed three defects, fixed in `09af2d40`, each with a new check:
  - an import of wrongly typed "format" or "version" threw out of the Import button;
  - an export dropped a later build's keys, which a round trip then erased;
  - the Export chooser started from a relative path.
- On main: WIN-04 (`window/shortcut-ids`) and WIN-01's goldens landed in #739.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
