Undo, Redo and a step restored from the Undo History first commit the knob edits still held for their quiet interval, so Undo takes back the last edit the user saw, not the one before it (DF-W29).

What it does
- **The settle** (`ProjectDocument::settleParameters`, the root's `settleEdits`): before Undo, Redo or a History restore acts, every parameter edit still held in the capture is committed as the step it was going to be. A held drag (Begin with no End) stays held.
- **Undo can act on a held edit:** with a knob edit still held, Undo is enabled (it commits the edit, then takes it back), so a project's first knob edit is undoable at once. Redo waits, since that commit clears what Redo would do. The Edit menu reads "Undo" until the edit is a step.
- **Latch release and Tap Tempo settle first:** each checks that the step it made is still the latest, so a held knob edit is committed before that check and is never the step they take back.

Review
An adversarial review confirmed two defects, both fixed in `f8979174`:
- **The wrong step undone:** latch release and Tap Tempo checked the latest step, and then Undo's settle made a newer one and undid it.
- **A first edit not undoable:** Undo's enabled state and title ignored a held edit.

Two findings were refuted: the drain's future-dated stamp, and a settle stopping at a blocked offer.

Tests
- Pd on: `adi_ui_document_tests` 313 checks; `adi_ui_shell_tests` 2927 checks (a knob edit held: Undo acts, Redo waits, the title); `adi_ui_seams_tests` 553; `adi_ui_window_state_tests` 109. All 0 failures.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- `tools/test_all.sh` was run once this session on `window/grid-ticks`'s tree, at main `ac07be74` plus window code only (PASS, 64092 checks across 270 suites). This branch changes window code only, so its other suites are main's.
- The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6199 and one near 12100, `tests/test_ui_mix_functions.cpp` 675).
- Carried until it lands: `window/browser-devices-test`'s commit, so main's Pd-on shell suite runs through.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp

