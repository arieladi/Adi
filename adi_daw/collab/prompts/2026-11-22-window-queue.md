# window — standing queue, 22 November 2026

This is the whole queue for the window lane, to work until your usage limit ends the session. It replaces every earlier list win gave you.

The split with window-fn (collab/README.md, win, 22 November): the window lane keeps the look (from agreed sheets only, ADR-0243), the shell and the test kit, and WIN-01 is yours. window-fn takes the function rows (MX-*, SRF-*, AU-*, ED-*, MI-*, EX-*, the E-items and the DF-W rows marked for it). The split is by item, not by path. When both seats touch a file, each edits it for its own items, the second branch to land merges main, and each seat names in its log the other seat's items it touched. The files you share with window-fn today are project_document.cpp, ui_mixer.*, ui_shell.cpp and track_edits.hpp.

## How to work

- Fetch main first, and read the top entry of collab/win.md and docs/DEFERRED.md.
- Work the items top to bottom.
- Use one branch per item, `window/<topic>`, cut from origin/main unless the item says to stack.
- Write the test first where the item names one.
- After each item: build and test on MSVC (/W4 /WX), push, and log the entry in collab/window.md on window/log. End the entry with "PR requested", and start the next item in the same turn.
- If an item is blocked on win, another seat or a decision, park it with one line in your log and take the next item.
- A question for the director goes under "FOR THE DIRECTOR" at the top of your log, with your recommendation. Then continue.
- Stay in your own paths (collab/README.md). Put anything you need from another seat's paths in your log's needs table.
- When this list is empty, take the next unbuilt row of your lane in docs/PLAN.md, in milestone order. Work meant for after 3 December goes on branches marked "lands after 3 December".
- Never stop because "nothing names this lane". Stop only at your usage limit or when the environment is broken.

## Queue

Items before 3 December come first (ADR-0248 d2: function first, one usable session). The rest follow.

1. **LOG-1122: bring window.md up to date**
   Log the 22 November push of DF-W33, which had no entry and no "PR requested". Record that 259a2b90 on window/plugins-browser is superseded by window-fn's 8d46c9fa (on main; #676 closed) and that the branch can retire. Write the split above into the log, and list this queue's unbuilt branches.
   Files: collab/window.md on window/log.
   Done when the entry is pushed and no item is claimed in both window.md and window-fn.md.

2. **DF-W33: a return inside a group**
   Rebuild 3654f3af (window/return-in-group) as one window-only commit on #677's current head, 76c8aaa1 (engine/df-e23-plugin-workers), plus main. Build, gate, and keep the picture. Log "PR requested, lands with or after #677".
   The engine never pushes to #677's branch: its DF-E30 (a)-(c) and the one-click Post op (167d863e) come on `engine/df-e23-followups`, stacked on 76c8aaa1 (the engine's item 1). Use that Post op for the mixed switch once the branch carries it; otherwise keep the window's batch. The tap menu needs DF-E30 (c) ("before DF-W33 offers the tap"); if (c) is not there, build the rest and park the menu with one line. Coordinate track_edits.hpp with window-fn's DF-E16.
   The engine's item 1 has hunks in ui_mixer.cpp and tests/test_ui_seams.cpp, the same files as this item: review them when its log names them, and say in your log which branch lands second and merges.
   Files: src/adi/ui/send_edits.hpp, track_edits.hpp, track_order.hpp; src/juce/ui_arrangement.*, ui_mixer.*, ui_shell.*, ui_look.cpp; tests/test_ui_shell.cpp, test_ui_seams.cpp.
   Test: adi_ui_shell_tests and adi_ui_seams_tests cover these cases: Group Tracks takes a return; Insert Return Track in Group; the send knobs show only inside the group; the tap menu; the mixed switch goes to Post first. test_all must pass, and WTS-01 step 5b must pass once win adds the step.

3. **DF-W18: window/input-clock**
   Merge main, build, gate, review, request. Window and key reads go through InputClock (ADR-0231 d2). Play Reference from a menu reads the clock once.
   Files: src/juce/ui_input_clock.hpp, ui_shell.*, ui_arrangement.cpp, ui_controlbar.cpp; tests/test_ui_shell.cpp.
   Test: the reference-audition case ("a tap under 200 ms latches it") passes with a test-set clock under a slowed clock, and the shell suite is green.

4. **DF-W19: window/menu-parameters**
   Merge main, gate, review, request. A pick from a parameter's menu reaches the plug-in.
   Files: src/juce/ui_devices.cpp/.hpp; tests/test_ui_shell.cpp.
   Test: the new check picks every item of a menu-shaped parameter, by mouse and by keyboard, with a frame between picks. Each pick gives its value and one Undo step, and the check fails on the old menu code.
   Ask in the log that the Mix View seat's rebuild of ui_devices keeps the 3-line fix.

5. **DF-W29: window/undo-settles**
   Merge main (window-fn's GestureSession for MX-23 is on main), build, gate, review, request. Undo, Redo and an Undo History restore first commit the parameterOps_ edits that are still held.
   Files: src/adi/ui/project_document.*, src/juce/ui_shell.*, ui_main.cpp; tests/test_ui_document.cpp.
   Test: in test_ui_document, a knob edit followed by Undo inside the quiet interval undoes the knob edit, and Redo restores it. The same holds for a History restore. WTS-01 step 4 still passes.

6. **DF-W30: window/grid-ticks**
   Merge main, build, gate, review, request. Three fixes: (a) snap compares in ticks; (b) Save As, Collect All and Save, and Auto Gain Stage each refresh once, with the overridden lanes; (c) createFades writes no empty fade step.
   Files: src/adi/ui/arrangement_geometry.hpp, fade_edits.hpp, project_document.cpp; tests/test_ui_document.cpp, test_ui_seams.cpp, test_ui_shell.cpp.
   Test: an 8 Bars grid with a bar longer than 22 quarters draws every line, and the arrow keys step. Each action gives one refresh. createFades at its clamp writes no op.

7. **DF-W32: window/held-peak**
   Merge main, build, gate, review, request. Strips take StripMeter::takePeak through one owner (HeldPeaks). The look does not change.
   Files: src/adi/ui/held_peaks.hpp, src/juce/ui_mixer.*; tests/test_ui_window_state.cpp.
   Test: a transient between two frames reaches the Peak Level pill and the peak line, and two views of one strip both see it. This closes the window half of N48.
   Its register trigger is "with the mixer's sheet", so request it as "lands with the mixer's sheet" and let win place it. It shares ui_mixer.* with window-fn's MX-23 and MX-02.

8. **WIN-01: the function keys do functions (a defect)**
   F1 to F8 lose the Activate Track defaults; the eight commands stay, unbound. F4 only toggles the Mix View. F3 and F7 are unbound (held for Session View and offline processing). Amend row 6 of parity-7.6. The defaults are at ui_shell.cpp 227-235 on main.
   Files: src/juce/ui_shell.cpp; tests/test_ui_shell.cpp; collab/window/parity-7.6-mixer.md.
   Test: a golden of every preset's default map with a no-clash check, which fails on main at F4. Headless, F4 toggles the Mix View once and changes no activator (MX-14).

9. **DF-E35a: main-row readers** (blocked: needs engine-win's lane id in ClipNode once #677 lands; skip to the next)
   The Arrangement paints and hit-tests only main-lane clips, so takes stay off the main row.
   Files: src/adi/ui/arrangement_geometry.hpp, src/juce/ui_arrangement.cpp; tests/test_ui_seams.cpp.
   Test: clip 1 on the main lane and clip 2 on take lane 7, both at 0-960. hit(track 1, 480) returns clip 1, and only clip 1 is painted.

10. **WIN-09: the Help menu**
    The Help menu is empty on main (`{"Help", {}}`). Give it the manual and keyboard-reference entries, so the Mac's Cmd+? menu search works.
    Files: src/juce/ui_shell.cpp, ui_main.cpp; tests/test_ui_shell.cpp.
    Test: a shell check that the Help menu lists both entries and each opens its target. The director checks Cmd+? on the Mac later.

11. **WTS-02: the rest of test hygiene** (after item 3)
    Route every key read through InputClock (keyIsDown, AppCommands::key, [REF], Tap Tempo). Add a fixed virtual display set; AssertionWatch with JUCE_LOG_ASSERTIONS on test targets only; windows_no_dialogs in every Windows test binary; repaint invariance, an extreme-zoom paint and a paint census.
    Files: src/juce/ui_input_clock.hpp, the key reads in ui_*.cpp, src/juce/ui_shell.cmake (test targets), tests/test_ui_*.cpp.
    Test: AT-4, AT-5, AT-6 (no OS key state is read; Shift+R tap and hold) and AT-8. The release ADI.exe carries no PUBLIC JUCE_LOG_ASSERTIONS.
    Making test_all.sh count the JUCE suite is win's file: ask in the needs table.

12. **WIN-04: stable command ids and user shortcuts that survive** (after item 8)
    Stable text command ids in Live's menu categories. The shortcuts.overrides record (its registry row is on main) is re-applied after every reload. Add a Shortcuts page on JUCE's key-mapping editor, in today's look.
    Files: src/juce/ui_shell.*, ui_settings.*; tests/test_ui_shell.cpp.
    Test: user keys survive a Settings change, a preset switch keeps the overrides on top, and every command has one stable id.

13. **WIN-02: every window reopens where it was**
    One placement rule: sanitize, find the monitor by id then by area, clamp, keep the title bar reachable, cascade. The main window and Settings go in the App record window.places (its row is on main); floating windows go in ui_view.
    Files: src/juce/ui_main.cpp, ui_settings.cpp, ui_floating.*; tests/test_ui_window_state.cpp.
    Test: window-infra acceptance 7-16, run headless. At 1366x768 and 125% the title bar never opens off screen, and a moved window keeps its place after a kill.
    The engine's SPEC section 8.4 text (N46) goes alongside it; the code does not wait for it.

14. **WIN-03: window kinds, owned windows and plug-in windows**
    Audit parity-7.5 and build what is missing: owned windows (above the main window only, minimised with it, no taskbar button); Esc closes unless the plug-in takes the key; Auto-Open; Auto-Hide; the track name in the title; cascading. ADI never scales a plug-in window.
    Files: src/juce/ui_floating.*, ui_devices.cpp, ui_shell.cpp; collab/window/parity-7.5-floating.md; tests.
    Test: window-infra acceptance 17-21, driven off the director's desktop. Parts that wait on a Live reading stay marked AWAITING.

15. **WTS-09: parity bookkeeping**
    Normalise row ids and status words, and add [P:file#row] tags in collab/window/parity-*.md. Write tools/validate_parity.py, with built-count floors and an untagged count that only falls.
    Test: AT-19. The script passes on main and fails on a planted bad tag.
    tools/ is win's: ask win to lend the new file and wire it into ci.yml. Do the parity-*.md half meanwhile.

16. **WIN-05: shortcut editing, reference presets, non-Latin layouts and key dispatch** (after item 12; lands after 3 December)
    Files: src/juce/ui_shell.*, ui_settings.*; tests.
    Test: under a Hebrew layout, Ctrl+Z undoes and Z and X zoom. A set exported on Windows imports on the Mac. The A5W-015 Live reading stays AWAITING.

17. **WIN-06: the command palette, Ctrl/Cmd+P** (after item 12; lands after 3 December)
    Ranking (fzy) and actions, in today's look. Its own look waits for the menus/dialogs sheet.
    Files: src/juce/ui_shell.*, new src/juce/ui_palette.*; tests.
    Test: window-infra acceptance 22-28 ("hsb" ranks Hide/Show Browser first; V/ inserts as one undo step), and fzy's 24 cases with exact scores.

18. **WTS-03: goldens that check text** (lands after 3 December)
    The comparator, glyph masks and the font experiment (Q-C answered (a), ADR-0232).
    Files: the golden harness in tests/test_ui_*.
    Test: AT-1, AT-2 (a word moved 1 px passes, a divider moved 1 px fails) and AT-24.

19. **WTS-05: UiControl, one description of the window, in process first** (blocked: its dependency, the control core's contract in src/adi/control, has no seat on the roster; skip to the next)
    Test when it clears: AT-9 to AT-12. Stable ids; clicking Play equals Space; a drag is one labelled undo step; a dropped WAV is one Insert Audio Clip step.

20. **DF-W24 step 2: the Control Bar from the agreed sheet** (blocked: waits on #674 landing the Mix View's 30fe5329 and 53403555; skip to the next)
    The Control Bar from language.png and arrangement.png, in rounded groups, with the Arrangement | Mix View switch. Remove the edge strip's view switches. Before you start, check 53403555's dark default against the sheet.
    Files: src/juce/ui_controlbar.*, ui_shell.cpp.
    Test: the shell checks, plus a 1920x1080 real-size picture of the showcase song beside the sheet.

21. **DF-W31: the theme and density tables in full** (blocked: win draws the full sets and adds the N56 registry rows; skip to the next)
    Light, Mix View and Open, from win's sets.
    Files: src/adi/ui/theme.*, density.*.
    Test: a picture of every theme and density beside win's sets, with the goldens redrawn.

22. **CT-04: take lanes and comping, the window's gestures** (blocked: needs DF-E35 (a), the lane id in the snapshot from engine-win after #677; lands after 3 December)
    From the agreed sheet (ADR-0247 d1): the header switch, the Insert Take Lane button and menu item, Ctrl+Alt+U, and seam crossfades off by default.
    Files: src/juce/ui_arrangement.*, src/adi/ui/*; tests.
    Test: shell checks for the lanes, audition and copy-to-main, each one undo step, and a picture beside take-lanes-comping.png.

23. **DF-W2: the Clip View's later parts** (blocked: win has not handed the Clip View to the lane yet; skip to the next)
    The Envelopes and MPE tabs, the Chance lane, multi-clip editing, and the Sample Editor's warp controls.
    Files: src/juce/ui_clipview.*, src/adi/ui/note_*.hpp.
    Test: for each part, its parity-7.7 rows and a picture.

Not yours, and never to queue: DF-W1, DF-W9, DF-W25, DF-W34, DF-W35 and DF-W36, which are done on main. The rest of DF-W28 belongs to window-fn; its register owner says window lane, but the move half is on main (b40ceb93).

## Waiting on win

- window/log: the 18-19 November entries, plus item 1's entry.
- window/proposals: four sheets for the director (Mixer and track rows, Browser, Clip View, menus/dialogs/Settings), one a week. No look work on those surfaces until he agrees one.
- window/return-in-group (DF-W33): it lands with or after #677, after item 2's rebuild.
- window/input-clock, window/menu-parameters, window/undo-settles, window/grid-ticks, window/held-peak: once items 3-7 request them. Do not redo them.
- window/plugins-browser: nothing to land (259a2b90 is superseded by 8d46c9fa); retire the branch.

## Needs from other seats

- win: add WTS-01 step 5b in tests/acceptance, on the window's grouping rule. This step has no other owner.
- win: lend tools/validate_parity.py and wire it into ci.yml (WTS-09); make test_all.sh count the JUCE suite (WTS-02).
- win: draw the full theme and density sets and add the N56 registry rows (DF-W31).
- win: close the DEFERRED rows that are done on main: DF-W1 (#589, #591), DF-W9 (ee8bd592), DF-W25 (#675), DF-W34 (423f1027), DF-W35 (67854419, bb8d28f1), DF-W36 (9bcd9e72).
- win: set the landing order for the shared files: project_document.cpp (DF-W29, DF-W30), ui_mixer.* (DF-W32, DF-W33) and ui_shell.cpp (DF-W18, DF-W33, WIN-01, WIN-04, WIN-09), against window-fn's and engine-win's branches.
- engine: DF-E30 (a)-(c) on engine/df-e23-followups (stacked on #677's head), (c) before the tap menu; the one-click Post op if it lands; N46, the SPEC section 8.4 text alongside WIN-02.
- engine-win: DF-E35 (a), a clip's lane id in the snapshot once #677 lands (for item 9 and CT-04).
- window-fn: coordinate track_edits.hpp (its DF-E16 with DF-W33) and ui_mixer.* (its MX-23 and MX-02 with DF-W32).
- mixview: land #674 (30fe5329, 53403555) so DF-W24 step 2 can start. Its ui_devices rebuild must keep DF-W19's fix.

## For the director

- Which of the four proposal sheets on window/proposals to agree first (win brings one a week, ADR-0248 d14).
- RE-01 / DF-W3 (C5): the remark pips and the Info View split are an ADI addition, so his picture comes first.
- MX-10 / DF-W4 (B8): Main's loudness meter waits on the Live 12 meter readings and his picture.
- Rack and macro panels, Map mode and the modulation ring (after 3 December): each needs an agreed sheet first, inside the one-a-week slot.
- DF-W7's third item: where Rec-Q and Play-Q go. They are ADI's own; Live has no header boxes for them.
