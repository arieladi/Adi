ADI-Shaper's panel window: the window lane's four review findings and the strict GCC leg's casts, for #716 (win's ask, collab/win.md, 23 November). On #716's head `1baf1fd8`; it lands with #716 or into it.

What it does
- **An audio restart no longer closes the window** (finding 1). `Session::release`, a new rate, block size or precision gives the device a new runtime, and the session's token stopped resolving, so the window said "The plug-in closed its window." and closed. The session and the view now take up the new runtime while the id still holds an ADI-Shaper, dropping an edit in flight (its preview put back). A deleted device comes back with Undo; only another device under the id retires the editor.
- **The values refresh without the scope source** (finding 2): on a new committed project (an op, Undo, Redo) and on the device's own row in the parameter feed (its stamp and playing values). Only the scope's trace waits on the cast. The panel shows the stored values; a lane's value while it plays is not shown (that needs the built-in to publish it, a feature of its own).
- **A refusal reaches the Status Bar** (finding 3): the view's `error()` through the loaned factory in `ui_main.cpp`; `togglePluginWindow` says an error again when it returns after the editor cleared it.
- **Tests** (finding 4):
  - `adi_shaper_panel_view_tests` (new target, with Pd): the window's content on a real project; a value changed by an op, by Undo and on the device directly shows; a new block size keeps it valid; deleting the device ends it and Undo brings it back; the view goes before the document.
  - `adi_shaper_panel_session_tests`: the restarts (a new block size, a stop and start, a new rate with an edit in flight) and a refusal's message. An edit begun after a restart and before the next tick loses its preview to that tick (the engine puts the device back on its committed values): it is refused with why and journals nothing, and the next edit lands.
  - `adi_ui_shell_tests` `shaperWrench`: two built-ins presented, only ADI-Shaper's wrench shows, its name starts past it, and a click reaches the factory with its id; and the Status Bar's repeat in `pluginWindows`.
- **The strict GCC leg** ("zero warnings + hardened libstdc++ (gcc)", red on #716): the four int operands GCC refuses at `-Wconversion`, cast to float (`tests/adi_shaper_wave_tests.cpp` 55, 81, 94; `src/juce/adi_shaper_panel.cpp` 376, which CI never reached).

Files: the clones seat's (`src/juce/adi_shaper_panel*`, `tests/adi_shaper_*`, `cmake/devices/adi_shaper_panel.cmake`) and the window's (`ui_main.cpp`'s loaned factory, `ui_shell.cpp`, `test_ui_shell.cpp`).

Tests
- GCC 13, Debug, `-DADI_WERROR=ON`, the gate's own build of `074bf41a` (and `c10003c4` for the session suite):
  - every ADI-Shaper suite passes: panel, panel session, panel view, wave, DSP, time, Pd;
  - seams 553, document 319, window_state 129, 0 failures;
  - `adi_ui_shell_tests`: 2975 checks, 0 failures, `shaperWrench` among them, with main's `browserDevices` fix (`88649293`) picked locally (#716's base still has that fixture's fault; it is not in this branch).
- Pd off as `ui.yml` builds it: adi_app and the six UI suites, 6 of 6 passed.
- The shadow and clang sweeps flag only lines from #716's base (`tests/test_ui_shell.cpp` 6192 and 12863, `cbf1b870`, `a92760a4`).
- Two adversarial review rounds: seven defects, none blocking, all fixed; the gate's run found an eighth (the restart test's expectation), fixed in `c10003c4`.

For the clones seat
- One assertion changed meaning: "restored generation cannot be touched by former editor" now reads that the device Undo restores takes the open editor's gestures again, as finding 4 asks. Confirm or say otherwise.
- Not from this branch: `adi_shaper_panel_tests` logs JUCE's "a component can only be focused when it's on the screen" (juce_Component.cpp 3027) five times; the panel takes the keyboard focus while unshown (WTS-02's `focusIfShowing` is the window's way).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
