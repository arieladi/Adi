# Basics safety report

SC-159 and SC-157 are ready for Win's Windows check. SC-151 is in progress.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-159 | `basics/sc-159-synced-projects` at `528f6f09` | Recognized sync folders and network volumes use a durable rollback journal, so committed work reaches the project file. Writable opens fold an existing WAL first. The window warns on New, Open and Save As. Touched engine Store and format, engine-win project-file publication, and window/window-fn document and app code. | Linux Debug Ninja, GCC 13.3, JUCE/Pd/Werror/acceptance on. The original Store failed the copy and WAL regressions. The fixed Store passed 82 checks. Seven core suites passed. Debug shell, document and window-state passed with DISPLAY unset. The native app displayed the warning under Xvfb. Acceptance passed 3/3. | Ready for Windows check |
| SC-157 | `basics/sc-157-project-backups` at `f47d614b` | A saved project gets a versioned backup on open and a new one after a ten-minute check finds edits. The ten newest versions are kept. Safe rotation uses the system trash after a durable write. File > Open Backup restores a separate editable project. Touched engine-win project files and window/window-fn app, File menu and tests. | Linux Debug Ninja with JUCE/Pd/Werror/acceptance. Four storage suites and window-state passed, including 99 project-file checks. Native Xvfb covered open, the real ten-minute edit backup, adjacent and read-only recovery, and archive shutdown. Acceptance passed 3/3. The document and shell timing failures are recorded below. | Ready for Windows check |

### SC-159 verification

- The Xvfb shell run hit JUCE's X11 BadWindow error. The shell suite passed in CI's displayless setup. The document and window-state suites also passed under Xvfb.
- Under software emulation the changeset preview exceeded 60 seconds with the original Store (103 s) and with the fix (109 s). The acceptance retry used a 90-minute local timeout for built-in realtime, which passed in 53 minutes.
- The published revision was tested on `main` at `de2a5b99`. While validation ran, the landing stack at `9dc43173` changed the neighboring warning hooks in `project_document.cpp` and `ui_main.cpp`. A dry merge identified conflicts there. The SC-157 branch carries the resolution on top of the phased Open and Save As flow.

### SC-157 verification

- The Debug shell ran with DISPLAY unset. Its one failure in 3,848 checks was SC-421's 40 ms browser search benchmark. The first keystroke took 171 ms, or 273 ms on the focused rerun. The browser implementation and benchmark fixture match the tested main base.
- The Debug document suite reported 700 of 702 checks passing. The new backup schedule checks passed. The same two failures reproduced on a fresh, unmodified Linux Debug build of main `f1e28dd5`, which passed 685 of 687 checks. The failures were the five-second recording settlement check after device loss and SC-415's slow plug-in open timing ratio. The focused SC-415 run on main also failed, with 888 ms for the longest turn out of 2,109 ms total.
- The built-in realtime acceptance test passed in 3,668 seconds with a local 5,400-second timeout for emulation. The full Linux proof used the code tree at `e763004c` on main `f1e28dd5`. Merging main `6f96847b` before publication added two collaboration documents.
- The Windows recycling helper passed an isolated MSVC compilation and a Recycle Bin probe while an exclusive project lease was held. Win still checks the integrated Windows build.

## Build storage

The safety session's dedicated Linux VM runs from `D:\adi-basics-safety-linux\ubuntu.img`.
It occupied 15,741,485,056 bytes (14.66 GiB) at the check. The original C: image
path is clear and the verified pre-migration image is archived on D:. C: had
17,119,453,184 bytes (15.94 GiB) free at 18:17 on 27 November. New build
trees, caches and downloads use D:.

## For the director

Should SC-157 replace the Take Snapshot trigger in ADR-0255 d6(e) and ADR-0213 d4?
I recommend following the assigned queue. Make a backup when opening a saved song and
check every ten minutes for changes. Keep the ten newest versions and move older ones
to the system trash. [Ableton's backup behavior](https://help.ableton.com/hc/en-us/articles/360000377870-Backup-Sets)

The SC-157 recovery action sits third in File, directly below Open Project
and above the first separator, with no default shortcut. I recommend that
placement so recovery is available from a fresh Untitled window.
