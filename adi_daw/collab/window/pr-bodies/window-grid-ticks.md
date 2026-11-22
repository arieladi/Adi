Three smaller fixes from the second outside review of 18 November (DF-W30), each with a test, and the review's own findings on them fixed.

What it does
- **(a) The editing grid is chosen in ticks** (`ArrangementGeometry::snap`, `forEachGridLine`, `nextGridLine`).
  - **The fault:** the grid compared positions in bar fractions with a fixed 1e-9. A bar longer than about 22 quarter notes on the 8 Bars grid, or one past 173 quarters on 1 Bar, made `snap(line + 1, true)` return `line`. Only one line was drawn, and the arrow keys stopped.
  - **The fix:** it compares in ticks. In 23/4 on 8 Bars a line stands every eight bars and the arrows step from one to the next. A 180-quarter bar steps a whole bar on 1 Bar.
- **(b) Save As, Collect All and Save, and Auto Gain Stage refresh once, with the overridden lanes** (`ProjectDocument`).
  - **The fault:** each refreshed the view without the session's overridden lanes. That made two refreshes for one action, and if the session's re-read failed, the override marks read wrong.
  - **Also fixed:** the review found the same fault in `endMixerDrag` (window-fn's MX-23, named here as the split asks).
- **(c) Create Fade writes no empty undo step.**
  - **The rule:** a fade the selection gives the length it already has, playing as a plain fade of that length, is no op: "The fades are already that long."
  - **A pinned clock is still a change:** a fade whose clock an edit pinned (a move or a split, then a tempo change: it plays at another length) still takes the op, which puts it back on its ticks.
  - **A crossfade stays one:** a crossfade whose left side is already that long is still named Create Crossfade.

Review
An adversarial review (the grid in ticks; the refreshes and fades; the tests; a skeptic per finding) confirmed three defects. All are fixed in `199b6c3a`:
- **(c) A pinned fade refused:** a fade already that long but playing at another length was refused as "already that long".
- **(c) The crossfade's label:** a crossfade whose left side was unchanged was labelled Create Fade.
- **(b) The missed sibling:** `endMixerDrag` was the refresh the row missed.

One finding was refuted: (b)'s test covers one call site, and its clause about the override marks passes on the old code. The fix is the same one line at each site.

Carried until it lands: `window/browser-devices-test`'s commit, which makes main's Pd-on shell suite run through.

Tests
- Pd on:
  - `adi_ui_seams_tests`: 558 checks, 0 failures. The grid at 23/4 on 8 Bars and a 180-quarter bar on 1 Bar, lines and arrows; a plain fade already that long (no op, says so) and a pinned one (still the op).
  - `adi_ui_document_tests`: 314 checks, 0 failures. Each action refreshes once with the override marks kept.
  - `adi_ui_shell_tests`: 2930 checks, 0 failures. Create Fade again at the same length is refused with no empty undo step; a crossfade over an unchanged left side is still Create Crossfade.
  - `tools/test_all.sh build-juce`: PASS, 64092 checks across 270 suites, validators clean.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- The shadow and clang warning sweeps are clean on this PR's lines. They flag main's own lines: `project_document.cpp` 34 and two in `tests/test_ui_shell.cpp`.
- This machine's GCC 13.3 fails main's `tests/scanner_session_reopen.hpp` 208 (`-Werror=range-loop-construct`; `adi_play_render_tests`). It is not this PR's file, and is reported in the window log.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
