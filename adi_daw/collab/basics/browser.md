# Basics Browser

Queue: `collab/prompts/2026-11-27-basics-browser.md`, revised 27 November. The code chain is `basics/browser-chain`, based on `origin/basics/integrate-1`. Linux window suites run in Debug with warnings treated as errors and `DISPLAY` unset.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-121 | `basics/browser-chain` at `da5adae5a` | Show an initial list and restore the last Browser label across projects and launches. Browser, application and Settings seats. | Headless startup, persistence and missing-Place behavior passed. Debug Werror, 18 no-display UI suites, the Settings suite and all 3 acceptance suites passed. | Pushed |
| SC-120, SC-122 | `basics/browser-chain` at `0cb6389f1` | Use one indexed search for All, with independent tokens and search text per mode. Browser and library seats. | Peerless scope, plug-in, preset and mode checks passed. The indexed 200,000-file Browser fixture and library query suite passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-124 | `basics/browser-chain` at `e6ff43de1` | Scroll Browser labels by wheel and reveal keyboard selections. Browser seat. | Peerless wheel, keyboard, paint and refresh checks passed. Debug Werror, all 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-125, SC-132 | `basics/browser-chain` at `d205b620e` | Add and remove Places from the Browser, including folder drops and read-only folders. Browser, application and library seats. | Peerless chooser/menu/drop tests passed. A read-only folder and a copied User Library marker also passed a direct smoke test. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-126 | `basics/browser-chain` at `2a3da1da4` | Show a Place as a directory tree while typed searches stay flat. Browser and library seats. | Peerless keyboard, folder, duplicate name and search checks passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-123 | `basics/browser-chain` at `7a1e22ab9` | Audition a sample through the app-owned streaming Cue player with keyboard and waveform control. Browser, application and engine seats. | Headless streaming, Cue, live recording isolation, key rebinding and waveform seek passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |

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

### SC-124

Peerless Browser fixture at 308 × 443 logical px, corresponding to a Compact 1200 × 720 window with Info visible. The sidebar clipping pane is `(4,27,113,385)` in Browser coordinates and the existing 19 px row height stays fixed. The Browser's search, right-hand list and preview retain their dimensions.

| Fixture | Before | After |
|---|---|---|
| Base snapshot, 21 labels and headings | Add Folder sits at `(4,408,113,19)`. The sidebar ends at y=412, so only the first 4 px of that row can paint. Wheel input has no effect. | The wheel can scroll by 15 px to bring Add Folder fully into `(4,393,113,19)`. |
| Base snapshot with 32 added folders | The 32nd folder sits at `(4,997,113,19)` and Add Folder at `(4,1016,113,19)`, both below the viewport. | At the lower stop the 32nd folder fills `(4,374,113,19)`, and Add Folder fills `(4,393,113,19)`. The wheel travel is 623 px. |
| With one earlier All label in the chain | The unscrolled Add Folder starts at y=427 in the default list. With 32 folders the final folder starts at y=1016 and Add Folder at y=1035. | The travel is 34 px by default and 642 px with 32 folders. The final two visible rows still occupy y=374 and y=393. |

The SC-124 regression directs a synthetic wheel event at the sidebar, clicks a scrolled-in folder, uses Up to reveal the next keyboard selection, and checks that a regenerated listing keeps a visible selection and clamps a shortened list. It runs without a peer or a window and does not activate Add Folder. This change touches the window seat’s Browser and shell tests.

## SC-125 · Add and remove Places

The existing Add Folder row now opens a directory chooser. Arrow keys scroll to it and Enter opens the chooser without changing the selected category. A file-manager drop adds the directory entries in one edit. A right-click on an added folder offers **Remove from Places**, which removes the configured folder even if it has disconnected. Its files and any marker remain on disk. The User Library row has no Remove action. Settings’ own folder picker uses the same path deduplication, and Browser edits update the Settings list and start a fresh background listing.

Compact measurements below use Browser-local logical pixels in the 1200×1000 headless fixture with the sidebar unscrolled. The sidebar begins at `(4, 27)` and is 113 px wide. Each row remains 113×19 px. SC-124’s scroll offset subtracts from y in smaller windows.

| Target | Before `(x, y; width×height)` | After `(x, y; width×height)` |
|---|---|---|
| Add Folder with no added Place | `(4, 408; 113×19)`, placeholder response | `(4, 408; 113×19)`, chooser action |
| First added Place | Absent in the empty fixture | `(4, 408; 113×19)` |
| Second added Place | Absent in the empty fixture | `(4, 427; 113×19)` |
| Add Folder after two configured Places | `(4, 446; 113×19)` for folders configured through Settings | `(4, 446; 113×19)` following chooser and drag |
| Remove from Places | Absent | One menu item, anchored to a 1×1 logical px target at the clicked row’s screen position |

The OS drop target covers the Browser component and uses its existing bounds. There are no new permanent controls. The chooser and context menu use the platform/JUCE sizing. Their dimensions were not measured on this headless run.

## SC-132 · Read-only Places and synced User Library

The existing library worker first attempts the portable marker and index path. When the folder cannot supply a usable identity, the worker walks it read-only and includes its files in the same searchable publication. It uses a listing-only key derived from the canonical folder path for selection and search scope, without registering a fabricated persistent volume ID or writing cache hashes into the index. The direct walk excludes symlink files and follows no symlink directories. Inaccessible or disconnected folders retain their Places row with an error, so the Browser can remove the configured entry.

A writable User Library keeps `.adi-volume-id` as the existing portable identity. A sync service can carry that marker with the folder. Each installation uses its own ADI app-data index and probes case behavior where the synced folder is mounted. When two configured folders on one installation carry the same marker, the first uses its portable index entry and the other remains searchable individually via a listing-only key. Both marker files remain unchanged. No folder name, permission, or copied marker silently establishes a second persistent volume under the same identity.

The headless shell test exercises chooser request, keyboard activation, settings persistence, mixed OS drops, same-name scope and removal, read-only search and marker copying. It substitutes callbacks for the chooser and context menu. The new files and edits concern the Browser seat, the app’s library worker, Settings’ folder edit and the shell suite. No native window or menu was shown.

Commit `286b9e9ab` is based on the chain’s SC-124 preflight head `068670786`. The isolated Linux smoke ran with `DISPLAY` unset. It confirmed an actual mode-0555 folder refuses the marker probe but still lists its sample, cloned markers stay identical without merging two Places, only the indexed User Library writes index entries, removal leaves media on disk, and disconnected Places remain removable.

# SC-126 Sample Places

Coordinates are logical pixels in the root component for a Compact 1440 × 900 window, with the Browser left and Info visible. The bounds follow the component layout and JUCE’s 19 px row geometry. The fixture is peerless.

The Browser is `(8,35,308,623)`. The list is `(127,81,185,546)` before and after. Each visible row is 185 × 19. The sample icon is 13 × 13 and is centered vertically. Each child adds 12 px of indentation.

| Fixture row | Before | After, collapsed | After, both branches open |
|---|---|---|---|
| Empty folder | Absent | `(127,81,185,19)` | `(127,81,185,19)` |
| Drums folder | Absent | `(127,100,185,19)` | `(127,100,185,19)` |
| Drums / Acoustic folder | Absent | Hidden | `(127,119,185,19)` |
| Drums / Acoustic / Clap.wav | `(127,81,185,19)` | Hidden | `(127,138,185,19)` |
| Loops folder | Absent | `(127,119,185,19)` | `(127,157,185,19)` |
| Loops / Clap.wav | `(127,100,185,19)` | Hidden | `(127,176,185,19)` |
| Top.wav | `(127,119,185,19)` | `(127,138,185,19)` | `(127,195,185,19)` |

The closed Drums triangle draws in `(137,105.5,5,8)`, and the open triangle draws in `(135,107.5,8,5)`. The indented Clap samples use x = 175 and 163 for their text, respectively. The corresponding text previously started at x = 151 for both. A typed `clap` search uses two flat rows at y = 81 and 100 and gives each its full original path on hover.

The peerless shell fixture covers disclosure clicks, Right, Left, Enter, duplicate filenames, file-only drag, and clearing the search. An additional fixture checks empty folders in indexed and read-only Places. The shared query tests and a direct hierarchy smoke check ran with DISPLAY unset and warnings as errors. The integrated window gate is separate.

### SC-123 Browser preview

In the 256 × 320 Compact software fixture, the preview row is (24, 295, 228, 21) before and after. The former decorative 11 × 11 ring has center (37, 305). It keeps that geometry inside the new 26 × 21 switch at (24, 295). The waveform and its 1 px cursor occupy (54, 298, 194, 15), inside the existing row. For a click at x=151 in that fixture, playback starts at 97/193 of the source duration.

Preview on/off and Preview Selected are app commands `browser.toggle-preview` (29066) and `browser.preview-selected` (29067). Shift+Return and Right are assignable defaults while the sample list has focus and auto Preview is off. Space never auditions. The app stores the toggle under `browser.previewEnabled`. The app-owned service can mix a standalone audition to outputs 1 and 2; the current Browser is shown by a project window.

## For the director

This session owns the Browser queue.

SC-123. The default auto Preview setting starts off and the standalone level starts at −12 dB, both named constants. Enabling auto Preview waits for a selection gesture, and clicking the same selected row replays after the previous preview ends. These readings await an actual Live comparison. The preview row remains in today's location for the next item, SC-429.
