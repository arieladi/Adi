Parity bookkeeping that cannot drift: the sheets' half (WTS-09; ADR-0108 d1; ADR-0232 d7).

What it does
- **`collab/window/validate_parity.py`** reads every `collab/window/parity-*.md` table and the tests, and fails when any of these breaks:
  - a row's cells match its header;
  - ids are well formed (a number, a lettered number, a section letter and number) and unique in their sheet;
  - every status starts with built, partial, open, measure, waits or withdrawn;
  - every `[P:<sheet>#<id>]` tag in `tests/` resolves;
  - the untagged built rows never rise above `parity-floors.json`'s floor;
  - no sheet's built count drops below its floor.
- **`--self-test`** passes on the tree and fails on a planted bad tag (AT-19). `--write-floors` writes today's counts.
- **Two faults on main, fixed here:**
  - an unescaped `|` split 7.2-shell row 26 into seven cells;
  - 7.2-application row 6's status began "ADI's addition" (now "built: ADI's addition").
- **The vocabulary and the tag form** are written in 7.2-shell's head.
- **The first tags:** six on the floating host's checks (7.5-floating rows 1, 2, 3, 5, 8, 9). 151 built rows, 145 untagged: the floor.
- **For win:** the script moves into `tools/` and `ci.yml`'s validator step when win lends them.

Tests
- `python3 collab/window/validate_parity.py`: clean. `--self-test`: passes. Against main's two sheets it reports both faults.
- Pd on, the gate's own build of `541e0ecd`: shell 2968 checks, seams 553, document 317, window_state 129, 0 failures.
- Pd off as `ui.yml` builds it (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): adi_app and the six UI suites, 6 of 6 passed.
- The full build stops only at main's GCC 13 `tests/scanner_session_reopen.hpp` 208. The shadow and clang warning sweeps flag only main's lines (`tests/test_ui_shell.cpp` 6202 and 12798, outside this branch's hunks).
- The branch carries `541e0ecd`, the window lane's fix that lets `browserDevices` pass with Pd on (Hybrid Reverb loads), so the gate runs green; it no-ops once main has it.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
