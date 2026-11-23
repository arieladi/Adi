Window kinds, owned plug-in windows, Esc to close, Auto-Open and cascading (WIN-03; the director's ruling of 5 November; ADR-0180 d3, d7). parity-7.5 rows 12 to 15.

What it does
- **Two kinds:** a floating window is owned (plug-in editors and device views, what `openView` makes) or normal (a view moved out with `detach`, the Mix View).
- **Owned windows:** no task bar button; they go down and come back with the main window. On Windows and the Mac they are raised above it when it comes to the front, taking no focus and leaving a minimised one down; on Linux JUCE's raise always activates, so that waits for X11's transient-for hint. Esc that reaches one closes it, while a plug-in that takes the key keeps it.
- **Auto-Open Plug-In Windows** (`plugins.autoOpenWindows`, on by default): a plug-in added from the Browser or the Mix View opens its window once the device feed shows it, by the wrench's rules.
- **Cascading:** a plug-in window never placed cascades from the main display's corner.
- **Partial:** "above the main window only" and not above other applications needs each platform's owner window (a Windows owner, a Mac child window, X11's transient-for), which JUCE does not offer. That is the next step, with WI2's Mac half. Plug-in windows always on top (on by default) keeps them above everything meanwhile.

Tests
- `adi_ui_shell_tests` `pluginWindows`: a plug-in's window is owned with no task bar button; the main window minimised takes it down and brings it back; Esc closes it; the Mix View's window stays normal with its button; Auto-Open opens an added plug-in's window, and off it opens nothing.
- Pd on, the gate's own build of `e969e387`: `adi_ui_shell_tests` 2988 checks, `adi_ui_window_state_tests` 160, `adi_ui_seams_tests` 553, `adi_ui_document_tests` 317, 0 failures.
- Pd off, configured as `ui.yml`: `adi_app` and the six UI suites build and pass.
- The full build stops only at main's GCC 13 `tests/scanner_session_reopen.hpp` 208. The shadow and clang warning sweeps flag only main's own lines (`tests/test_ui_shell.cpp` 6202 and 12835).
- **Review:** an adversarial review confirmed two defects, fixed in `e3472d65`:
  - JUCE's X11 raise activated the owned windows, taking the keys and restoring minimised ones;
  - the root kept a pointer to the floating host after the host was destroyed, which the main window's callbacks could reach.
- It carries `window/window-places`' namespace rename (`window_place`).
- AWAITING (Live 12): are plug-in windows above other applications, on the task bar, closed by Esc?
- Stacked on `window/window-places` (WIN-02).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
