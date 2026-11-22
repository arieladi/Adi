The director can now put a return track inside a group, as the agreed sheet draws it (DF-W33, the window half; ADR-0247 d2; ADR-0245 d9, d10; WTS-01 step 5b). The return stands in its group after what the group holds, lettered with the group's initial ("DA"). Its send knob is on the group's tracks only, and A and B keep their letters everywhere. One window-only commit on #677's head (`25e3dfe9`), whose engine lets a return have a group, takes its sends only from inside it, and clicks a mixed return to Post. #677 has landed on main (`cc74097c`), so this PR's base is main.

![The return-inside-a-group sheet beside the window as built, both 1920 x 1080](https://github.com/arieladi/Adi/blob/window/return-in-group/adi_daw/collab/window/pictures/2026-11-22-return-in-group.png?raw=true)

Left: the agreed sheet (`proposals/return-inside-group.png`). Right: the window as built, 1920 x 1080 at real size, Dark, Compact, on the showcase song grouped as in step 4's picture. **Drum Room** is a return made with Insert Return Track in Group on **Drums**. It stands after Kit and Loops, inside Drums' line, with "DA" on its activator.
- **Sends.** Loops sends -18 dB to it and Drum Loop -12 dB, in the third send column, which only Drums' tracks have. Kit holds no instrument, so it has no sends.
- **Mixed switch.** Drum Loop's send taps Post FX and Loops' follows the switch (Post Mixer), so the sends differ and Drum Room's Pre/Post switch is mixed: a dash in an accent edge.
- **Numbers.** Bass and the tracks after it keep their numbers (5 on), since a return takes none.
- **Not shown.** The sheet's lower Mixer panel is not on this branch: the window shows the Arrangement's track rows. The return's own strip in the Mixer follows the same rules (the scene checks it).

What it does
- **Group Tracks takes a return** (`groupable`, `track_edits.hpp`); Main alone can't be grouped ("Main can't be grouped.").
- **Insert Return Track in Group**, on a group's title menu (`ui_arrangement.cpp`, `AdiRootComponent::insertReturnTrack(group)`):
  - a return in the group, after what it holds, named for the group's initial ("DA Return"), one undo step;
  - the Create menu's Insert Return Track (Ctrl+Alt+T) still goes to the foot, as Live does (ADR-0247 d2).
- **Dragging a return into a group** (`returnDropAt`, `moveTracks`):
  - a return dragged by its title onto a group's row, or a row it holds, goes to the end of that group's returns, the group outlined while it is dragged;
  - over a grouped return, before or after it;
  - dragged back over the foot, out again;
  - a return moved with a track is still refused.
- **Where it is drawn** (`TrackPlace::foot`): a top-level return and Main at the foot; a group's return among the rows, in its group, after what it holds (rank sorts returns after the tracks).
- **Send knobs** (`returnsFor`, `send_edits.hpp`):
  - a track's knobs go to the top-level returns, then to the returns of every group holding it, at any depth, outermost first;
  - a track outside the group has no knob for its return, and the engine refuses such a send;
  - a group's return has no knob for itself, as the sheet draws it (a top-level return keeps Live's, greyed);
  - Main's Pre/Post switches cover every return.
- **Letters** (`returnLetter`): a top-level return by its place (A, B); a group's return by the group's initial, then its place among the group's returns (DA, DB). They show on the knobs, their titles, the activator, and Main's switches, re-lettered when the group is renamed or the return moves.
- **A send's own tap** (`setSendTap`, the knob's right-click menu): Pre FX, Post FX or Post Mixer, the one in use ticked.
  - Choosing the tap a send already takes is no step.
  - Choosing the switch's own tap clears the send's own, so it follows the switch again.
  - A send still fully down says to turn it up first.
- **The mixed switch** (`returnTapMixed`; the click is the engine's `clickReturnSendTap`):
  - a return whose sends tap differently from each other shows a dash in an accent edge, unlit, as the sheet's "the sends differ". The window reads the engine's own reading (`ReturnSendTap::legacyMixed`, any file);
  - one send with a tap of its own is not mixed: its knob carries no mark and its menu says it (ADR-0247 d2);
  - the click is the engine's one op: a mixed switch goes to Post with every send's own tap cleared, in one undo step; the next click goes to Pre.
- **The Mixer View lines up:** every strip keeps a send row per return in the project, so a group's tracks and their neighbours keep their faders level; a row with no knob is empty.
- **Track numbers skip the returns** (`TrackPlace::number`): a return in a group moves no number. The activator, a # in a name, the Status Bar's track name and the clip's spoken text agree with F1–F8.

Ported, not this PR's: **main's `browserDevices` scene crashed with Pd on** (the clones seat's report in its log). DEV-22 made Hybrid Reverb loadable, but the scene expected it dimmed and its double-click loaded it onto track 4. The plug-in loads before had left no track selected, so Utility never loaded and the strip was read empty. CI's Step 7 UI runs the suite with Pd off, where that part is not compiled. The fix:
- The Browser no longer calls Hybrid Reverb unbuilt; with Pd off it says it is left out with the Pd devices.
- The scene expects Hybrid Reverb loadable without loading it.
- Track 4 is selected again before the device loads, and the strip is guarded.

The same commit is on its own branch from main, `window/browser-devices-test`, requested separately, so it can land first; this PR carries it until then.

Not done
- **The lower Mixer panel** of the sheet (the Mixer under the Arrangement) is not on this branch; the Mixer View and the track rows show the strips.
- **The Pre FX tap's sound** is the engine's (#677); the window writes the op.

Review
An adversarial review (correctness, behaviour, tests, a skeptic per finding) confirmed five defects. All are fixed in this commit (the third finding's fix is now the engine's reading):
- **Choosing the tap a send already takes** from its return's switch made an undo step and pinned the send against the switch.
- **Letters went stale:** a send knob's letter, its title and Main's switch titles kept the old letter after the group was renamed or the return moved.
- **A missed mixed state:** an older file's return whose sends disagree read Post, not mixed.
- **Uneven rows:** in the Mixer View, strips with a group's return had a send row more than their neighbours, so their faders stood lower.
- **Numbers off by one:** a grouped return pushed every later track's number up by one, out of step with F1–F8.

Two findings were refuted. Both were thin test checks (the showcase's check inside its `if`, and the next click's op not checked to be Pre), and both are tightened anyway.

Tests
- Pd on:
  - `adi_ui_shell_tests`: 2981 checks, 0 failures (after the merge, main's scenes included). The `returnInGroup` scene checks:
    - Insert Return Track in Group: DA, in the group, the foot keeping A, B and Main, one undo step;
    - the knobs: A, B, DA on a Drums track; A, B outside; A, B on DA's own strip; DA on its activator;
    - a send from outside refused;
    - the send's tap: chosen, a no-op, cleared, and refused while fully down;
    - one send tapping on its own leaves the switch unmixed; a second send makes the sends differ: the dash, then the engine's two clicks;
    - the numbers after Drums;
    - the Mixer View's faders level;
    - a rename re-lettering DA to KA, and undo;
    - a drag of return A onto Drums' row, outlined, then in Drums, one step;
    - Group Tracks on a return.
  - `adi_ui_seams_tests`: 560 checks, 0 failures:
    - a return dragged into a group and out again, one step each;
    - a return with a track refused;
    - an older file's disagreeing sends read mixed, and the engine's click sets every send to Post.
  - `tools/test_all.sh build-juce`: PASS, 63701 checks across 269 suites, validators clean, on the tree this commit carries (`6d1395f2`, the same tree as `b882c743`).
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- The shadow and clang warning sweeps are clean on this PR's files (`ui_mixer.cpp`, `ui_arrangement.cpp`, `ui_shell.cpp`, `ui_look.cpp`, `tests/test_ui_seams.cpp`, and `tests/test_ui_shell.cpp`'s lines). In `tests/test_ui_shell.cpp` they flag main's own two lines, as on step 4.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
