Every window reopens where it was, by one placement rule (WIN-02; SPEC §8.4; ADR-0063 d2, ADR-0180 d3 and d7). The look does not change.

What it does
- **The rule** (`src/adi/ui/window_place.hpp`, namespace `adi::ui::window_place`, JUCE-free, tested headless):
  - sanitize what a record holds;
  - find the monitor by its id, then its area, then its origin, then the one monitor of the same size and DPI, else the main display;
  - restore as it was on its own monitor, in proportion when that monitor's size or scale changed, and at the same offset, wholly on the main display, when its monitor is gone;
  - clamp the size to the usable area, and keep a 40 x 18 grab of the title bar on a screen, never above its top;
  - cascade a window that would open at another's corner.
- **Where places live:**
  - the main window and Settings in the App record `window.places` (display, area, DPI, rectangle, full screen), saved 400 ms after a move or size and written only when they moved;
  - floating windows in `ui_view` as before, now with their monitor's area and DPI, and saved on move too.
- **The JUCE half** (`src/juce/ui_place.hpp`): a project opened keeps the main window where it stands, full screen or not. A display change brings back a title bar no screen holds (`parentSizeChanged`).

Tests
- `adi_ui_window_state_tests` `placementRule`, on a 1366 x 768 laptop at 125 % (1093 x 614 units): identity's five steps; the title bar never above, below or off a side; every saved place across a sweep opens reachable and no larger than the screen; restore in proportion, moved with its monitor, and on the main display when gone; cascade; the record round trip.
- The same suite: a moved floating window keeps its place after a kill (the store opened again).
- `adi_ui_shell_tests` `windowPlaces`: first run centred and no larger than the screen; the main window's place kept in `window.places`, and after a kill back where it stood with its title bar on the screen; Settings centred; a display change; the debounce. Also a moved floating window's place saved without closing it.
- Pd on, the gate's own build of `a8c1b6a7`: `adi_ui_shell_tests` 2979 checks, `adi_ui_window_state_tests` 160, `adi_ui_seams_tests` 553, `adi_ui_document_tests` 317, 0 failures.
- Pd off, configured as `ui.yml`: `adi_app` and the six UI suites build and pass.
- The full build stops only at main's GCC 13 `tests/scanner_session_reopen.hpp` 208. The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6202 and 12798).
- **Review:** an adversarial review confirmed two defects, fixed in `d803216d`:
  - Displays' `totalArea` and `userArea` are deprecated in this JUCE, which stops a `-Werror` build;
  - full screen set before a window shows lost the rectangle it goes back to.
  Also from the review: a plug-in's own window is moved, never sized, when the displays change.
- **Its first gate** found the namespace `adi::ui::place` made `note_edits.hpp`'s `place()` calls ambiguous. It is `window_place` now (`a8c1b6a7`).
- AWAITING (Live 12): where Live reopens its windows, per install or per Set, and where a window goes when its monitor is gone.
- `window/window-kinds` (WIN-03) stacks on this branch.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
