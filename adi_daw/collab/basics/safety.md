# Basics safety report

SC-159 is ready for Win's Windows check. SC-157 is next.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-159 | `basics/sc-159-synced-projects` at `528f6f09` | Recognized sync folders and network volumes use a durable rollback journal, so committed work reaches the project file. Writable opens fold an existing WAL first. The window warns on New, Open and Save As. Touched engine Store and format, engine-win project-file publication, and window/window-fn document and app code. | Linux Debug Ninja, GCC 13.3, JUCE/Pd/Werror/acceptance on. The original Store failed the copy and WAL regressions; the fixed Store passed 82 checks. Seven core suites passed. Debug shell, document and window-state passed with DISPLAY unset. The native app displayed the warning under Xvfb. Acceptance passed 3/3. | PR requested |

- The Xvfb shell run hit JUCE's X11 BadWindow error. The shell suite passed in CI's displayless setup. The document and window-state suites also passed under Xvfb.
- Under software emulation the changeset preview exceeded 60 seconds with the original Store (103 s) and with the fix (109 s). The acceptance retry used a 90-minute local timeout for built-in realtime, which passed in 53 minutes.
- The published revision was tested on `main` at `de2a5b99`. While validation ran, the landing stack at `9dc43173` changed the neighboring warning hooks in `project_document.cpp` and `ui_main.cpp`; a dry merge identified conflicts there.

## For the director
