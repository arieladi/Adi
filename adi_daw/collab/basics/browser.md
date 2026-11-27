# Basics Browser

Queue: `collab/prompts/2026-11-27-basics-browser.md`, revised 27 November. The code chain is `basics/browser-chain`, based on `origin/basics/integrate-1`. Linux window suites run in Debug with warnings treated as errors and `DISPLAY` unset.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-121 | `basics/browser-chain` at `da5adae5a` | Show an initial list and restore the last Browser label across projects and launches. Browser, application and Settings seats. | Headless startup, persistence and missing-Place behavior passed. Debug Werror, 18 no-display UI suites, the Settings suite and all 3 acceptance suites passed. | Pushed |
| SC-120, SC-122 | `basics/browser-chain` at `0cb6389f1` | Use one indexed search for All, with independent tokens and search text per mode. Browser and library seats. | Peerless scope, plug-in, preset and mode checks passed. The indexed 200,000-file Browser fixture and library query suite passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |

## Measurements

### SC-121

At startup the Browser used to have no selected row and an empty content list. First launch now selects Instruments and immediately shows the built-in instruments at the existing 19 px row height. The first result begins at the list origin. A saved Place returns when its volume appears in the index. All provides a list while that scan runs, and remains selected if the saved label is unavailable. Current application preferences retain the label across a new project and a new process.

The six software references add exactly 600 result-ink pixels each. At 1440 × 900 the added ink is bounded by `(133,84,63,32)`. In the swapped 960 × 540 fixture it moves to `(769,84,63,32)`. These are content changes inside the existing Browser.

### SC-120 and SC-122

Service commit `9a3f65f65` from integration base `012cc2f23`. The service extraction changes no visual bounds. The All row introduced with SC-121 stays in the Categories group at 19 px high. Ctrl+F now selects it and gives the search field the keyboard.

GCC Debug with warnings as errors, DISPLAY unset, 200,000 synthetic names in three Places. Median of seven trials per query. The before run exercised the original SC-421 index with already folded input and scoped or all-sample results. The after run included query compilation and exercised either the same Place or the whole service with fixture built-in, plug-in and preset publications.

| Query | Before Place (ms) | After Place (ms) | Before all samples (ms) | After mixed (ms) | Place sample hits before/after |
|---|---:|---:|---:|---:|---:|
| Q | 0.001 | 0.002 | <0.001 | 0.007 | 5/5 |
| QX | 0.001 | 0.002 | <0.001 | 0.006 | 4/4 |
| QXY | 0.001 | 0.002 | <0.001 | 0.006 | 3/3 |
| QXY 100000 | <0.001 | 0.003 | <0.001 | 0.009 | 0/1 |
| Take | 12.645 | 13.783 | 13.361 | 13.294 | 199,999/199,999 |

Snapshot construction took 3,269.734 ms before and 3,133.703 ms after. Every measured query remained below 40 ms at the service boundary. The integrated shell fixture keeps its original SC-421 threshold and applies it to the two-word query as well.

The new `adi_library_query_tests` fixtures passed in a standalone warning-as-error build with DISPLAY unset. `ui_browser.cpp` and `test_ui_shell.cpp` passed warning-as-error syntax checks. The integrated shell build and tests belong to the Browser chain.

## For the director

This session owns the Browser queue.
