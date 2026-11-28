# Basics safety report

SC-159, SC-157, SC-151 and SC-155 have Linux proofs below. Win's merge at `a9c19691` brought the parallel safety implementations for SC-344, SC-184, SC-158, SC-169 and SC-172 onto main. I am checking those in queue order. The SC-344 follow-up for cancelling a held draft is building against main `bdcccb3e`.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-159 | `basics/sc-159-synced-projects` at `528f6f09` | Recognized sync folders and network volumes use a durable rollback journal, so committed work reaches the project file. Writable opens fold an existing WAL first. The window warns on New, Open and Save As. Touched engine Store and format, engine-win project-file publication, and window/window-fn document and app code. | Linux Debug Ninja, GCC 13.3, JUCE/Pd/Werror/acceptance on. The original Store failed the copy and WAL regressions. The fixed Store passed 82 checks. Seven core suites passed. Debug shell, document and window-state passed with DISPLAY unset. The native app displayed the warning under Xvfb. Acceptance passed 3/3. | Ready for Windows check |
| SC-157 | `basics/sc-157-project-backups` at `f47d614b` | A saved project gets a versioned backup on open and a new one after a ten-minute check finds edits. The ten newest versions are kept. Safe rotation uses the system trash after a durable write. File > Open Backup restores a separate editable project. Touched engine-win project files and window/window-fn app, File menu and tests. | Linux Debug Ninja with JUCE/Pd/Werror/acceptance. Four storage suites and window-state passed, including 99 project-file checks. Native Xvfb covered open, the real ten-minute edit backup, adjacent and read-only recovery, and archive shutdown. Acceptance passed 3/3. The document and shell timing failures are recorded below. | Ready for Windows check |
| SC-151 | `basics/sc-151-playback-problems` at `08ab0ef5` | Session playback problems mark the affected Arrangement clips and device cards. A single Status Bar action shows the count and opens the full list of reasons. The marks and action clear as the problems clear. Touched window/window-fn and device card display code. | Linux Debug Ninja with JUCE/Pd/Werror/acceptance. The native red and green runs used the same missing-WAV and missing-CLAP fixture. The new app marked both items and the visible list contained both reasons. Window-state and all three acceptance tests passed. The document and shell suites ran with the known timing failures below. | Ready for Windows check |
| SC-155 | `basics/sc-155-undo-open-state` at `0864420e` | Undo History captures the current cursor when it opens a project. The bottom row returns to that cursor from either direction. Earlier history remains accessible and any creation snapshot has its own row. Touched window/window-fn's history panel and UI submitter. | Linux Debug Ninja with JUCE/Pd/Werror/acceptance. History, history-move and window-state passed. The full shell ran the reopened-cursor regression. Native Xvfb reproduced the old four-to-two-track loss and verified the fix restored four from five with Redo intact. Acceptance passed 3/3. Known timing failures are below. | Ready for Windows check |

### SC-159 verification

- The Xvfb shell run hit JUCE's X11 BadWindow error. The shell suite passed in CI's displayless setup. The document and window-state suites also passed under Xvfb.
- Under software emulation the changeset preview exceeded 60 seconds with the original Store (103 s) and with the fix (109 s). The acceptance retry used a 90-minute local timeout for built-in realtime, which passed in 53 minutes.
- The published revision was tested on `main` at `de2a5b99`. While validation ran, the landing stack at `9dc43173` changed the neighboring warning hooks in `project_document.cpp` and `ui_main.cpp`. A dry merge identified conflicts there. The SC-157 branch carries the resolution on top of the phased Open and Save As flow.

### SC-157 verification

- The Debug shell ran with DISPLAY unset. Its one failure in 3,848 checks was SC-421's 40 ms browser search benchmark. The first keystroke took 171 ms, or 273 ms on the focused rerun. The browser implementation and benchmark fixture match the tested main base.
- The Debug document suite reported 700 of 702 checks passing. The new backup schedule checks passed. The same two failures reproduced on a fresh, unmodified Linux Debug build of main `f1e28dd5`, which passed 685 of 687 checks. The failures were the five-second recording settlement check after device loss and SC-415's slow plug-in open timing ratio. The focused SC-415 run on main also failed, with 888 ms for the longest turn out of 2,109 ms total.
- The built-in realtime acceptance test passed in 3,668 seconds with a local 5,400-second timeout for emulation. The full Linux proof used the code tree at `e763004c` on main `f1e28dd5`. Merging main `6f96847b` before publication added two collaboration documents.
- The Windows recycling helper passed an isolated MSVC compilation and a Recycle Bin probe while an exclusive project lease was held. Win still checks the integrated Windows build.

### SC-151 verification

- The native probe ran under Xvfb with MIT-SHM disabled to avoid a second-window repaint artifact. It clicked the Status Bar and read both displayed reasons from the list. The old app opened no list for the same fixture.
- The headless document suite passed 704 of 706 checks, with the same device-loss and slow plug-in timing failures confirmed on unmodified main above. The Debug shell passed 3,854 of 3,855 checks. Its only failure was the unchanged SC-421 benchmark, with a median first keystroke of 121 ms against 40 ms. The playback warning checks passed.
- Built-in realtime acceptance took 4,005 seconds with the same local 5,400-second timeout. The tested branch includes main `6f96847b`. Main advanced to `ed3da057` during validation and a dry merge found 15 conflicts across the project-file, app and window tests and code. The published head preserves the tested tree.

### SC-155 verification

- The native app restored the opening cursor twice and Ctrl+Y recovered the new track in between. On quit the existing device-state save appended the Reverb and Delay state operations, leaving four audio tracks and all three authored steps reachable. The closed file passed SQLite integrity checking.
- The Debug shell passed 4,375 of 4,376 checks with DISPLAY unset. Its only failure was SC-421, with a 114 ms first keystroke against 40 ms. The document suite passed 914 of 916 checks, with the known device-loss and SC-415 timing failures.
- The item started directly from updated main `ed3da057` to keep its history changes separate from earlier integration conflicts. The Linux code tree was `05623050`. The merge of main `fbf541ee` before publication changed four documents. The built-in realtime test passed in 3,433 seconds with the local emulation timeout.
- When history retention has discarded the opening cursor or it requires a cross-baseline reload, the row reports that it cannot restore the saved state.

## Build storage

The safety session's dedicated Linux VM runs from `D:\adi-basics-safety-linux\ubuntu.img`.
It occupied 15,741,485,056 bytes (14.66 GiB) at the check. The original C: image
path is clear and the verified pre-migration image is archived on D:. C: had
17,119,453,184 bytes (15.94 GiB) free at 18:17 on 27 November. New build
trees, caches and downloads use D:. At 02:53 on 28 November the active image was
15,741,616,128 bytes (14.66 GiB) and C: had 15,219,154,944 bytes (14.17 GiB) free.

## For the director

Should SC-157 replace the Take Snapshot trigger in ADR-0255 d6(e) and ADR-0213 d4?
I recommend following the assigned queue. Make a backup when opening a saved song and
check every ten minutes for changes. Keep the ten newest versions and move older ones
to the system trash. [Ableton's backup behavior](https://help.ableton.com/hc/en-us/articles/360000377870-Backup-Sets)

The SC-157 recovery action sits third in File, directly below Open Project
and above the first separator, with no default shortcut. I recommend that
placement so recovery is available from a fresh Untitled window.

For SC-151, the warning marks are 9 pt and sit in the existing clip and device
title treatments. The Status Bar action is at most 136 × 16 pt, at the right of
the message field. It opens a 540 × 260 pt scrollable list. Live 12 uses a
clickable Status Bar warning and marks offline clips for missing media.
[Live 12 Manual, Locating Missing Files](https://www.ableton.com/en/manual/managing-files-and-sets/#locating-missing-files)
