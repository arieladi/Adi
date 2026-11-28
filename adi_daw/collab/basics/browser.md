# Basics Browser

Queue: `collab/prompts/2026-11-27-basics-browser.md`, revised 27 November. The code chain is `basics/browser-chain`, based on `origin/basics/integrate-1`. Linux window suites run in Debug with warnings treated as errors and `DISPLAY` unset.

## Director’s macOS Release

The native Release build is blocked in this Linux session, which has no access to the director’s Mac or Xcode. A source merge preflight for `origin/basics/integrate-1` at `a7d02bcf` with `origin/main` at `9dc43173` passed. The Mac report had no Release bundle for that combined source. App build, macOS suites, and installation are pending. [Mac checks](https://github.com/arieladi/Adi/blob/basics/mac-log/adi_daw/collab/basics/mac.md)

The proposed fixed folder is `~/ADI-Director-Test`, with `ADI.app` and a separate `Data` folder. Once the bundle is installed, quit any other ADI instance and paste this into Terminal.

```sh
ADI_HOME="$HOME/ADI-Director-Test/Data" \
  "$HOME/ADI-Director-Test/ADI.app/Contents/MacOS/ADI"
```

New projects go under `Data/config/ADI/projects`. Settings go in `Data/config/ADI/settings.json`. ADI puts its test User Library, caches, and shared data under this same Data folder. Once win confirms landing, rebuild from the merged main into the same app path and retain Data for the director’s test project.

Wave 2 continues on the same chain. SC-132 was pushed with SC-125 as requested in the rewritten queue. SC-135 is recorded by the project seat. [Project report](https://github.com/arieladi/Adi/blob/basics/project-log/adi_daw/collab/basics/project.md)

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-121 | `basics/browser-chain` at `da5adae5a` | Show an initial list and restore the last Browser label across projects and launches. Browser, application and Settings seats. | Headless startup, persistence and missing-Place behavior passed. Debug Werror, 18 no-display UI suites, the Settings suite and all 3 acceptance suites passed. | Pushed |
| SC-120, SC-122 | `basics/browser-chain` at `0cb6389f1` | Use one indexed search for All, with independent tokens and search text per mode. Browser and library seats. | Peerless scope, plug-in, preset and mode checks passed. The indexed 200,000-file Browser fixture and library query suite passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-124 | `basics/browser-chain` at `e6ff43de1` | Scroll Browser labels by wheel and reveal keyboard selections. Browser seat. | Peerless wheel, keyboard, paint and refresh checks passed. Debug Werror, all 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-125, SC-132 | `basics/browser-chain` at `d205b620e` | Add and remove Places from the Browser, including folder drops and read-only folders. Browser, application and library seats. | Peerless chooser/menu/drop tests passed. A read-only folder and a copied User Library marker also passed a direct smoke test. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-126 | `basics/browser-chain` at `2a3da1da4` | Show a Place as a directory tree while typed searches stay flat. Browser and library seats. | Peerless keyboard, folder, duplicate name and search checks passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-123 | `basics/browser-chain` at `7a1e22ab9` | Audition a sample through the app-owned streaming Cue player with keyboard and waveform control. Browser, application and engine seats. | Headless streaming, Cue, live recording isolation, key rebinding and waveform seek passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-429 | `basics/browser-chain` at `f2c761cd8` | Reveal preview from the first eligible sample selection and retain the current list height. Browser seat. | The headless visibility, reservation, folder and hidden-command checks passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-431 | `basics/browser-chain` at `f23561dc8` | Report missing physical Cue outputs and map sparse device grants consistently. Engine and window seats. | Headless status, sparse stereo, failed restart, metronome and preview tests passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-127 | `basics/browser-chain` at `6286d5a37` | Keep working labels and list saved presets under Sounds. Browser seat. | The roster, fallback and preset search and load passed headless. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-128 | `basics/browser-chain` at `3bdc44247` | Ship an original starter drum kit under Drums and Samples. Browser, library and packaging seats. | The headless roster and read-only listing, deterministic WAV generator and relocated Linux installer smoke passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-432 | `basics/browser-chain` at `e073649db` | Register a stable Hot-Swap command for the active device or sample slot. Window seat. | Headless command ordering, focus, rebinding and menu-label checks passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-424 | `basics/browser-chain` at `d1ad41dcd` | Reopen a collapsed Browser from the Info shortcut, menu command or Status Bar button. Window seat. | Headless command and layout tests passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-129 | `basics/browser-chain` at `634911602` | Give Browser results and Places a context menu with an available file or folder action. Browser seat. | The headless popup seam covered files, built-ins, presets, plug-ins, nested and added folders, removal and immutable action targets. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-130 | `basics/browser-chain` at `766f8d607` | Clear the Browser search field with Escape or its x while retaining mode-specific text. Browser seat. | The headless suite verified both controls, immediate filtering, the text bounds and Hot-Swap mode restoration. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-131 | `basics/browser-chain` at `599e89c8d` | Rescan the Browser library in the background without reopening ADI. Browser and application seats. | A headless regression wrote and indexed a new sample through the rescan path and preserved selection, scroll, search, audition state and open folders. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-133 | `basics/browser-chain` at `01cf03a9b` | Persist Favorites and seven rebindable color marks for Browser items. Browser, window and Settings seats. | Headless key, popup, search and round-trip persistence checks passed. Debug Werror, all 18 no-display UI suites, Settings and all 3 acceptance suites passed. | Pushed |
| SC-134 | `basics/browser-chain` at `66cdcb395` | List owned recordings and bounces under Current Project and in All. Project, library and Browser seats. | Headless fixtures committed recorded and bounced media, checked All deduplication, and measured the 200,000-file search. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-136 | `basics/browser-chain` at `bba910fb6` | Wire the existing Browser Hide and Name controls and hide Groove Pool until MI-05. Browser seat. | The peerless test exercised both sort directions, folder hierarchy, multi-selection and the Browser collapse. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-137 | `basics/browser-chain` at `862cea353` | Browse ADI and MIDI files in Places and All, and open a project by double-click. Browser, library and application seats. | Headless typed-tree, flat query, audio-only audition, open routing and index tests passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-146 | `basics/browser-chain` at `27cd97a5e` | A typed search that finds nothing says so in the list, naming the label, or that the first scan still runs. Browser seat. | Headless `browserEmptySearch` in `adi_ui_shell_tests`. Slot check pending: see "Round of 28 November". | Pushed, check pending |
| SC-145 | `basics/browser-chain` at `f80910a6e` | The line between the Browser's two panes drags; the width is kept with the program (`browser.sidebarWidth`). Browser and Settings seats. | Headless `browserSidebarResize` in `adi_ui_shell_tests`. Slot check pending. | Pushed, check pending |
| SC-139 | `basics/browser-chain` at `b71ec7827` | Browser Back and Browser Forward: two commands (`browser.back` 29082, `browser.forward` 29083) on Ctrl+[ and Ctrl+], over a history of label and search visits. Browser and window seats; the three key map fixtures. | Headless `browserHistory` and the command-tail check in `adi_ui_shell_tests`. Slot check pending. | Pushed, check pending |
| SC-147 | `basics/browser-chain` at `62141fcbd` | Audio Effects' groups and Plug-Ins' formats and makers fold as Live's folders: closed at first, triangle, double-click, Enter, Right and Left. Browser seat. | Headless checks in `browserDevices`; six older Browser tests open the folders first through `openBrowserGroups`. Slot check pending. | Pushed, check pending |
| SC-142, its first half | `basics/browser-chain` at `f3b8691a0` | A Place whose folder could not be read is drawn dim in the left list; its right-click gains Locate Folder..., and the folder chosen takes the gone one's place in Content folders. The scanning mark waits (see "Not now"). Browser, application and Settings seats. | Headless `browserMissingPlace` in `adi_ui_shell_tests` (a real index and marker scan with one folder gone, a software picture, the captured menu, the relocation and a new scan). Slot check pending. | Pushed, check pending |
| SC-138 | `basics/browser-chain` at `8a14c3752` | The list's header menu shows Date Modified, Size and Kind beside Name and shows or hides file extensions, both kept with the program; a header click sorts by its column. Library files carry size and time. Browser, library and Settings seats. | Headless `browserColumns` in `adi_ui_shell_tests` (real files through the read-only walk, the captured header menu, sorting, layout, a software picture, persistence). Slot check pending. | Pushed, check pending |

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

### SC-429

At 256 × 320 Compact, the list previously used `(119,46,133,243)` and the preview row was always visible. An unselected list now uses `(119,46,133,270)` and the preview has no bounds. Selecting an audio file with the player available restores the list to `(119,46,133,243)` and the existing preview to `(24,295,228,21)`. The list keeps that height as selection moves or clears. A folder selection in a newly shown list leaves the preview hidden. Preview on/off remains available in the key map while the row is hidden.

The software-rendered root comparisons changed only the bottom of the Browser list and former preview row. At 960 × 540 the changed region runs from `(8,267)` to `(316,298)`, or `(644,267)` to `(952,298)` after Swap Sides. At 1200 × 720 it runs from `(8,447)` to `(316,478)` and at 1440 × 900 from `(8,627)` to `(316,658)`. End coordinates are exclusive.

# SC-431 missing Cue Out

The preview-integrated patch is `9b0fe92b5` on `9535a8a8d`. The version prepared without the SC-123 preview sources is `da61c5eb0` on `906a2dd8d`.

| State at 1000 × 650, 100% | Status Bar rectangle | Message field | Text rectangle with no action or progress |
|---|---|---|---|
| Before | `(0,612) 1000 × 38 px` | `(35,620) 581 × 22 px` | `(43,620) 565 × 22 px` |
| After | `(0,612) 1000 × 38 px` | `(35,620) 581 × 22 px` | `(43,620) 565 × 22 px` |

The positions follow the existing `StatusBar::paint` and root layout. The peerless Status Bar fixture checks the same field bounds. Before, the field carried the existing project message or selection text. After, it also says `Cue Out 3/4 unavailable. Metronome and sample preview are silent.` when the running device lacks that saved pair. Hovering the Status Bar can show the complete notice at narrow widths. The saved pair and any unrelated Status Bar message remain intact.

The driver grant and the two Cue renderers now agree on physical channel numbers. With physical 1/2 and 7/8 enabled, the older renderer could send a 3/4 request into the packed buffers for 7/8. The amended map identifies 3/4 as absent and directs a 7/8 request to those buffers. A single available side of a stereo pair is treated as missing. The chooser uses the same map.

The strict display-free DeviceCore and fake JUCE DeviceBridge checks passed. The sample-preview suite passed all 17 checks, including playback through a sparse map and silence for the missing pair. Warnings-as-errors syntax checks covered the touched engine and window sources. The exact integrated Cue and peerless Status Bar fixtures are ready for the Browser chain build.

The display-free fake DeviceBridge check also exercised a failed restart after a stop. The fake stayed closed while reporting its old sparse 1/2 and 7/8 grant and a 256-frame buffer. The check failed on `7407dc332`, which reopened the bridge, and passed on `16e23564c` with the closed-device guard. With the guard, the bridge stayed closed, made no additional prepare call, published no output map, retained the stop notice and silenced a stray callback.

### SC-127 Browser roster

Browser-local bounds at 308 × 443 in Compact density. The sidebar starts at (4, 27) and is 113 px wide. At scroll zero every row is 113 × 19, with its top at y = 28 + 19 × its zero-based index. The SC-125 base has 22 rows, including headings and gaps. SC-127 has 12. The content list keeps its current header and row layout.

| Visible roster in order | Source | Before y | After y |
|---|---|---:|---:|
| Categories | SC-127 | 85 | 28 |
| All | SC-120, SC-122 | 104 | 47 |
| Sounds | SC-85, SC-127 | 123 | 66 |
| Instruments | DF-W48 (#787) | 161 | 85 |
| Audio Effects | N15 (#395) | 180 | 104 |
| Pd Devices | N15 (#395) | 218 | 123 |
| Plug-Ins | DF-W9, DF-W34 (#700) | 237 | 142 |
| Samples | N15, SC-421 | 275 | 161 |
| Separator | SC-127 | 332 | 180 |
| Places | SC-125, SC-132 | 351 | 199 |
| User Library | SC-135 | 389 | 218 |
| Added Places, if any | SC-125, SC-132, SC-126 | before Add Folder | starting at 237 |
| Add Folder… | SC-104, SC-125 | 427 | 237 with no added Places |

Collections returns with Favorites (SC-133). The remaining hidden labels, in category or place order, are Drums (SC-128, after Sounds), MIDI Effects (SC-294; hosted effects SC-316), Clips (DF-W7; clip listing has no separate row), Grooves (MI-05 and SC-240), Templates (SC-161), AI (AGT-05 and ADR-0261 d2, at the foot of Categories), Packs (DF-W7; no separate Packs listing row) and Current Project (SC-134).

Sounds lists saved preset files alphabetically and uses the same word matcher as All. The headless fixture creates and reads two real preset files, checks that an audio file with matching words stays outside Sounds, and checks that empty Sounds, a same-named user Place and old hidden-label preferences behave as expected. SC-128 adds the ordered Drums label, which shifts the rows below it by 19 px.

The Device, Mixer and Mix View software fixture changed inside its Browser results from `(133,84)` to `(310,474)`, exclusive. The fixture’s first-label focus check now visits All because Favorites is hidden at this step. The other software references remained pixel-identical.

### SC-128 starter drums

The original kit contained zero shipped samples. The new kit has 16 signed 16-bit mono PCM WAV files at 48,000 frames per second, synthesized without input samples.

| Hit | Duration | Sound |
| --- | ---: | --- |
| Kick Deep | 620 ms | Descending low body and short transient |
| Kick Punch | 390 ms | Shorter brighter pitch sweep and click |
| Snare Dry | 350 ms | Two drum modes and a close noise burst |
| Snare Bright | 520 ms | Higher drum modes with longer high-band noise |
| Clap Scatter | 380 ms | Three spaced claps and a decaying tail |
| Rim Click | 210 ms | Three inharmonic modes and a short tick |
| Tom Low | 510 ms | Three descending low-frequency modes |
| Tom High | 390 ms | Three higher, shorter descending modes |
| Hat Closed | 175 ms | Two metal modes and short high-band noise |
| Hat Open | 690 ms | Three metal modes and long high-band noise |
| Shaker Grain | 315 ms | Three staggered bands of granular noise |
| Cymbal Wash | 1,650 ms | Five resonances and a long broadband decay |

| Loop | BPM | Frames | Pattern |
| --- | ---: | ---: | --- |
| Half Step | 96 | 120,000 | Staggered kick, half-time snare and rim accents, varied hats |
| Straight | 120 | 96,000 | Four-on-the-floor kick, backbeat claps, offbeat hats and shaker |
| Skips | 128 | 90,000 | Syncopated kicks and snares with rim and shaker accents |
| Tom Run | 150 | 76,800 | Kick and backbeat with a tom fill and cymbal wash |

The kit is staged at `share/adi/starter-drums` on Linux, `starter-drums` beside the installed `ADI.exe` on Windows, and `Contents/Resources/starter-drums` in the macOS bundle. Non-bundle development builds copy the kit beside ADI. The app path resolver takes the executable path and the configured install-relative path and checks for both Hits and Loops directories without writing.

The Python validator regenerated all 16 files byte-for-byte and checked distinct audio, bounds, sample format, and exact loop lengths. A display-free CMake smoke installed the actual adi_base rules at a relocated default prefix and under custom bindir/dataroot directories; the probe resolved both variants and the development copy, and both installed kits matched the source WAVs byte-for-byte. Linux shell syntax checks passed. Windows and macOS were checked against their packaging recipes and JUCE resource API on Linux.


## Browser layout and registration

At Browser size 308 × 443 px and zero sidebar scroll, the sidebar keeps (4, 27, 113, 412) and the results list keeps (119, 46, 185, 393) with SC-123’s preview row hidden. With the preview row open those bounds are (4, 27, 113, 385) and (119, 46, 185, 366). Bounds are x, y, width, height.

| Sidebar label | Before y | After y |
| --- | ---: | ---: |
| Categories | 28 | 28 |
| All | 47 | 47 |
| Sounds | 66 | 66 |
| Drums | — | 85 |
| Instruments | 85 | 104 |
| Audio Effects | 104 | 123 |
| Pd Devices | 123 | 142 |
| Plug-Ins | 142 | 161 |
| Samples | 161 | 180 |
| Separator | 180 | 199 |
| Places | 199 | 218 |
| User Library | 218 | 237 |
| Add Folder | 237 | 256 |

With the preview hidden, the result list is `(119,46,185,393)` in the 308 × 443 fixture. Every sidebar row is 113 × 19 px at x = 4. Every result row is 19 px high. With a fresh library, Drums and Samples show 16 results, taking 304 px of logical row height. A local place also named Drums remains independent and appears immediately above Add Folder. The starter kit itself has no duplicate Places entry. Sounds still lists saved presets.

The app resolves the kit by its real executable location and registers it under a factory library identity. The scanner uses the read-only listing path and ignores separately configured aliases of the factory root before marker acquisition. Drums scopes the shared query directly to this identity, then ordinary Samples and All include the same files with the other sample results. A headless linked scanner/query smoke also used an actual symlink alias and confirmed one indexed user file, 16 Drums hits and loops, 17 Samples when one user WAV is present, a one-result multi-word search, and zero marker writes to the kit. The UI regression intercepts the remove-menu callback before a native menu can be shown.

An explicitly added Hits or Loops subfolder still opens as an ordinary Place. The scanner skips marker and index writes inside the kit, and identical absolute paths appear once in Samples and All regardless of scan order. The linked headless regression reproduced the prior marker write through a symbolic link, then proved that both scan orders preserve a read-only 12-hit Place and leave Drums and Samples counts unchanged.

# SC-432 Hot-Swap command

The command is `Hot-Swap`, stable WIN-04 ID `browser.hot-swap`, numeric slot 29068, Browser category. The Live, Cubase and Bitwig keymaps each assign Q. In the focused sample tile it targets the chosen slot. In a device title or its controls it targets that device. Retained ordinary Stack cards share their owning strip. The context menu reads the current shortcut and retains the target it was opened on.

Component positions are calculated from the existing layout. For an ordinary unfolded device panel 220 × 140, the title Hot-Swap button is `(145,1,17,15)` before and after. In an 84-pixel-high sample tile, its Hot-Swap button is `(4,66,17,15)` before and after. The context menu action stays at its existing place. Its right-side shortcut label is Q by default and changes to the first assigned key after a rebind. No menu or window is shown by the test.

### SC-424 remainder

At 1440 × 900 Compact, the Info gesture previously left the collapsed Browser and Info with empty bounds. It now restores the Browser to `(8,35,308,623)` and Info to `(8,666,308,196)`. On the right these are `(1124,35,308,623)` and `(1124,666,308,196)`. The selected side and width are retained. With the Browser already open, the same controls still toggle only Info. The regression invokes the shortcut, View menu command and Status Bar button without opening a native menu.

### SC-129 Browser item menus

In the 308 × 443 px Compact Browser, the sidebar is (4, 27, 113, 412) and the results list is (119, 46, 185, 393) before and after this item with the preview row hidden. The corresponding bounds with the preview row open are (4, 27, 113, 385) and (119, 46, 185, 366). Sidebar rows are 113 × 19 at x = 4 and results are 19 px high. After SC-128, at scroll zero User Library and the first added Place begin at y = 237 and y = 256. Their center-click targets are (60, 246) and (60, 265). In the single-result fixture, the first result starts at (119, 46) and the plug-in result beneath its format and vendor headings starts at (119, 84). The popup target is a 1 × 1 rectangle at the click point in screen coordinates, as it already was for the added-Place popup.

| Right-click target | Before, ordered popup rows | After, ordered popup rows |
| --- | --- | --- |
| Sample, saved preset, plug-in, nested folder, or registered User Library | 0 | Show in File Manager, 136 × 16 px |
| Built-in device | 0 | Show in File Manager (built-in), disabled, 178 × 16 px |
| Added Place | Remove from Places, 132 × 16 px | Show in File Manager then Remove from Places, 136 × 32 px total item area |
| File result with no absolute path | 0 | Show in File Manager (file unavailable), disabled, 218 × 16 px |

The display-free JUCE LookAndFeel probe measured ideal menu item dimensions. Popup chrome, display-edge adjustment, and the operating system's file manager remain live-window checks. SC-131 can add a 105 × 16 px Rescan Library item to sample, folder and library menus. SC-133 supplies its own favorites actions. The shell fixture intercepts popup presentation and invokes actions against captured paths. It tests sample, preset, plug-in, built-in, nested directory and both kinds of Places, including an open popup whose source listing was replaced. No native menu or window is used.

The first integrated run caught a nested folder being passed as a relative path. The menu now resolves that path against its selected Place before capturing it, so Reveal works for the clicked directory.

### SC-130 Clear Browser search

Logical pixel bounds from the Compact 1440 × 900 layout, with the Browser on the left and Info shown. The Browser is `(8,35,308,623)` in the root. The search field stays `(24,4,280,21)` in Browser coordinates, or `(32,39,280,21)` in the root.

| Part | Before | After |
|---|---|---|
| Search field | `(32,39,280,21)` | `(32,39,280,21)` |
| Text viewport with query | `(33,40,276,19)` | `(33,40,255,19)` |
| Clear button with query | Absent | `(292,40,19,19)` |
| Clear button with empty query | Absent | Hidden with zero bounds, full text width |

The x stroke endpoints span 7 × 7 px from `(298,46)` in root coordinates. A 21 px right slot reserves space only when text is present. The mark uses the existing text colors. No key map or tracked image fixture changes are expected. The Browser parity fixtures clear their search text before rendering.

Escape clears ordinary search with the search field focused. Existing Hot-Swap Escape exits and restores the ordinary query. The x clears the active field without switching modes or taking keyboard focus. The peerless shell fixture tests the button hit area, focus policy, immediate query results and independent modes.

## Check of SC-121 and SC-120/122 against the rewritten queue

A second session checked these items on Linux for Win and built nothing of the queue. The checks were:

- **The search is a service outside `BrowserPanel`.** Met. `adi::ui::LibraryQuery`, `BrowserLibrarySnapshot` and `queryLibrary` in `src/adi/ui/library_query.*` use no JUCE and no panel. The panel's All label calls `queryLibrary`, and a floating search can call it the same way. `tests/test_library_query.cpp` checks the panel-free path: one query over samples, built-ins, plug-ins and presets, each word matched on its own, so "kick 808" finds "808 Kick.wav".
- **A remembered label that is not shown falls back to All.** Met. `BrowserPanel::restoreLabel` and `setLibrarySnapshot` select All when the saved key is not in the list. A saved Place is held only while the first scan runs, and is dropped for All once the scan finishes without it.
- **Merging `origin/basics/integrate-2` into this chain.** Tried twice, each time on the then-current heads. The second attempt was `basics/browser-chain` at `3bdc44247` with `integrate-2` at `5956fe2fa`. One conflict came up, in the Status Bar (`ui_shell.cpp`, `ui_shell.hpp`), between SC-431's Cue Out message and SC-151's playback-issues button; both were kept. Built in GCC Debug with JUCE and Pd, warnings as errors, the whole tree including `adi_app`. With `DISPLAY` unset, all 295 non-performance suites and all 3 acceptance suites passed. Before it could be pushed, the integrator made the same resolution in `0ac90cf61` (it differs only in declaration order). That merge was therefore not pushed: it would only give this chain a second, conflicting merge.

## SC-131 · Rescan the sample library

**Rescan Library** in the Browser asks the existing library worker to walk the configured sample Places again. It accepts the request even when the library settings are identical. The old list remains available during the scan. A newer request supersedes a pending scan through the worker’s existing cancellation path. A successful publication adds new samples to the current search and Place tree. The Browser keeps the category, words, expanded folders, surviving file selections, list scroll position and current sample audition. The Status Bar reports the start and completion or the existing scan error.

Compact geometry in the 1200×720 headless fixture is measured in Browser-local logical pixels. The results list starts at `(119, 46)`, with width `Browser width − 123` and 19 px rows. The viewport scrolls by 38 px in the fixture. Before publishing the new alphabetically first sample, the two selected rows begin at y255 and y274. The same two files begin at y274 and y293 after publication, retaining their order and the 38 px scroll position. The array of rows has one extra entry. The sidebar, query, preview controls and list container retain their positions and sizes throughout. The Status Bar uses the existing root rectangle `(0, 682; 1200×38)` for both scan messages. The Rescan menu action uses the shared Browser context menu described in SC-129.

The peerless regression adds a file after the first snapshot, invokes a rescan through the menu callback, and rebuilds the real library index and search snapshot on a background thread. It checks that the new file is visible in the active query, both old sample selections survive an index shift by identity, the audition is undisturbed, and clearing the filter shows the previously expanded folder. The chooser and menu are never shown by the test.

Candidate commit `bf679368e` follows the exact SC-129 head `5a3c052ee`. The production Browser and application translation units passed Linux Debug warnings-as-errors syntax checks. The full integrated `env -u DISPLAY ctest -R '^adi_ui_'` gate runs in the Browser chain.

A standalone Linux smoke with `DISPLAY` unset also scanned the same real index and configured folder twice. The second worker publication included a file created after the first scan, the query found it within the same portable Place ID, and its folder remained visible in the published tree.

### SC-133 Favorites and collection colors

Browser-local coordinates, from the 308 × 443 peerless fixture. Before, Categories was (4, 28, 113, 19), All (4, 47, 113, 19), and Add Folder at the end of the default roster was (4, 256, 113, 19). After, Collections is (4, 28, 113, 19), Favorites is (4, 47, 113, 19), and the spacer is (4, 66, 113, 19). Categories is (4, 85, 113, 19), All (4, 104, 113, 19), and Add Folder is (4, 313, 113, 19). Categories and Places rows move down 57 px. Content begins at x=119, and its 19 px rows have the same bounds before and after. In the software paint test of a 113 × 19 result, Favorite is an outlined (101, 7, 5, 5) square and Red a filled (94, 7, 5, 5) square. Marks pack from the right at 7 px intervals and reserve their width from the row's name. Before, there were no Favorites or color marks.

The existing result popup offers Add to Favorites or Remove from Favorites and a Collection Colors submenu. The numeric bindings 1–7 use the normal Browser shortcut registry. Favorites appears even empty. It contains any sample, built-in device, or plug-in carrying an explicit Favorite or color mark, and uses the Browser's usual search. Marks remain in application preferences. Samples use their normalized indexed paths, devices use their built-in UIDs, and plug-ins use format, UID, and an optional shell member.

The Device, Mixer and Mix View software fixtures return to the exact Browser results pixels from before SC-127. The restored Favorites row lets the keyboard focus fixture exercise its original first-label path. The changed area is confined to the Browser results between `(133,84)` and `(310,474)`, exclusive.

# SC-134 Current Project

The isolated change is `ddd38eed4` on `5fa6cabac`. The integration candidate is `742f933d0` on `6850edac9`, preserving Favorites, Drums, Rescan and the other Browser items already there.

The producer reads the open song's committed media catalog. It publishes regular audio files inside the project folder. This covers current `Samples/Recorded` takes, legacy `audio` takes, and the `audio/import-…` files created by Bounce. The recently committed file enters on the next app update. A first save or a project switch changes the Store connection, so the publication is redone against its new folder. Separate Export Audio/Video destinations are delivery files unless the user brings them into the song. A neighboring song's files and references outside the open project's folder do not join this Place.

The Browser shows the label while a project is backed, including when the project has no audio. It lists files directly with the ordinary sample rows, matching and audition seams. All and Favorites reuse the project publication and suppress duplicate paths in the shared library.

## Before and after bounds

Default Compact layout, 1000 × 650 root, left Browser at `(8,35)`, sidebar scrolled to its top, one added Place, no sample preview shown. Each rectangle is x, y, width, height in root coordinates. The peerless fixture checks these relationships with the production bounds and prints the measured rectangles when the integrated suite runs.

| Base | Element | Before | After |
|---|---|---|---|
| `5fa6cabac` | Current Project | Hidden | `(12,272,113,19)` |
| `5fa6cabac` | First added Place | `(12,272,113,19)` | `(12,291,113,19)` |
| `5fa6cabac` | Add Folder | `(12,291,113,19)` | `(12,310,113,19)` |
| `6850edac9` | Current Project | Hidden | `(12,348,113,19)` |
| `6850edac9` | First added Place | `(12,348,113,19)` | `(12,367,113,19)` |
| `6850edac9` | Add Folder | `(12,367,113,19)` | `(12,386,113,19)` |

The Browser frame, sidebar clipping pane and content viewport keep their bounds. The two project audio rows occupy 38 px of logical list height. They use the existing 19 px sample rows and reveal the ordinary preview strip on sample selection.

## Proof

Strict GCC Debug, DISPLAY unset, passed the library query suite with real recording-layout media commits, legacy media, an external link, a real Bounce preparation and commit, deduplication with the shared library, and opening a different song in the same folder. Strict syntax checked the app, Browser and peerless window fixture. The integrated window suite remains for the chain gate.

A strict display-free service probe kept SC-421's 200,000 shared names and added a duplicate project sample plus a distinct one. Seven-trial Place medians were 0.002 ms for Q, QX and QXY and 0.003 ms for QXY 100000. All returned the expected deduplicated counts at 0.009, 0.007, 0.007 and 0.011 ms respectively. The broad Take query returned 200,001 distinct files in 36.481 ms across both sources.

## Indexed shared-path lookup

Follow-up `e1ee062aa` builds an immutable index of filesystem path hashes and listing indices on the worker. It verifies equality against the original path. During All search, only project hits look up a shared path and locate its position in the ordered shared results. A matching project name replaces that shared hit in place. Project-only hits follow in their original order. The index stores numeric entries alone, so it copies no paths and imposes no new lifetime constraints on snapshots.

The final strict Debug test passed with DISPLAY unset. It covers two shared replacements, first-match duplicate handling, project-only order and names, and the real bounce commit. No visual bounds changed.

On the same 200,000-file fixture with one overlapping and one project-only path, the broad All median of three seven-trial medians went from 34.291 to 13.499 ms and returned the same 200,001 distinct files. The four narrow queries remained under 0.013 ms across all sources after the change. Place Q, QX and QXY stayed at 0.002 ms. The two-word Place query took 0.003 ms after the change.

The path index stores 3,200,000 bytes of entries at 200,000 files on this build. Isolated fill and sort took 121.545 ms median across seven runs. Full snapshot timings overlapped with concurrent build load and are recorded separately at `/tmp/browser-sc134-order-final.log`.

### SC-136 Browser controls

At 1440 × 900 Compact with the Browser on the left, its top round button still draws at `(8,35.5,19,19)`. Its clickable bounds are `(8,35,19,20)`. Before, those pixels had no action. A click now invokes the existing collapse command, reducing the Browser and Info to empty bounds. The Name header remains `(127,62,185,19)` with a direction triangle at `(299,68,8,7)`. It previously left the authored order unchanged. A click sorts by name descending and a second click sorts ascending, using the same 19 px rows and keeping a folder or device together with its descendants. Selection follows the items.

The Groove Pool button occupied `(8,638.5,19,19)` in that fixture and is now hidden. Its space stays part of the Browser background while MI-05 is pending. The peerless test clicks the existing controls, checks an expanded folder and a two-file selection through both sort directions, then checks the dock collapse.

All nine updated software references changed exactly 93 pixels each, confined to the former Groove Pool button. The Hide and Name controls retain their prior default pixels.

# SC-137 Browse projects and MIDI

A Place now presents native ADI projects and standard MIDI filenames alongside audio, in its normal folder tree and flat word-query results. All includes those file types. Samples, sample Hot-Swap, the preview player and the existing sample drag retain their audio scope. Project activation uses the same native path and application-owned open callback as Open Recent, including the application's export-interruption and project-open handling. MIDI appears as a file and the existing MIDI import workflow remains separate. The index format stays the same, with recognition occurring when indexed or read-only directory entries are published.

At Browser size 308 × 443 px and with the preview hidden, both versions use content list bounds (119, 46, 185, 393) and a 19 px row height. The collapsed Session directory remains at list y = 0, Browser y = 46. Once open, the Kick row remains at list y = 19, Browser y = 65. The added Old Pattern.SMF, Song Set.ADI and Verse.mid rows are at Browser y = 84, 103 and 122, respectively. The expanded rows keep the existing 12 px indentation, presenting their existing 13 px icons at Browser x = 137 and their names at x = 155. The preview stays hidden for the project and MIDI selections.

The peerless fixture pre-seeds and reopens an existing library database, exercises a second read-only Place, opens a real project path through the application seam, checks the sample drag and Hot-Swap scopes and checks the shared All query. It invokes the list model directly without showing a native window or menu.

Integrated on SC-136 as 0c0927741. The sorted fixture adds checks for a selected project row moving from the third file slot to the second with 19 px steps inside the open folder. The integrated fixture enables actual Favorites settings and checks that only the audio row can be marked. The integrated library query suite and the studio kit's alias smoke both ran headlessly. The latter confirmed one copy of each file in Samples with either scan order while treating an explicitly added Hits alias as an ordinary read-only Place. The app, shell, Browser and shell tests compiled with the Debug flags in syntax mode. The original focused peerless UI run passed 11 checks; the full integrated UI runtime gate belongs to the chain.

The final adaptation, 3ad30607d on afb2508f9, keeps SC-134’s sorted hash lookup for exact paths. The duplicate fixture verifies that the project-owned audio display name takes the shared slot when the same path matches both sources. Distinct ADI project and MIDI files with the same stems keep their own matches.

## Round of 28 November: wave 3

A new session took the queue on 28 November. Every row above, SC-120 to SC-137 and SC-424, SC-429, SC-431 and SC-432, is in `origin/land/basics-1128c`, which this chain was fast-forwarded to (`16064c0a6`) before the round. That landing stack is not yet on main, so BASE is `origin/land/basics-1128c`. Builds and tests run on the project's build PC through `slot.sh`, slot 1.

What is left of the queue is wave 3, SC-138 to SC-147. The queue file names no order for wave 3, so the order is the register's, except that SC-146, SC-145 and SC-147 were taken first: each is a fault in today's Browser that a test can pin without a reading of Live or a sheet. SC-139 followed; SC-138 is next.

**Not now**
- SC-140 (tags, the filter panel, a saved search as a label): ADR-0261 d10 puts the filter on one filter panel, which waits for the Browser sheet (d19).
- SC-143 (a clip dragged into the Browser is kept as a file): ADI has no clip file. Reserve: a clip file format (a schema minor or a format ADR) before it can be built.
- SC-144 (a track dragged into a Browser folder saves as a project): needs a writer for a project made from one track, which nothing has today. Reserve: none known; a stranger should read `src/adi/project/` for an export of a subset before starting.
- SC-141 (choose which labels show, rename one): needs a reading of Live 12's sidebar editing (which labels can be hidden, and where). Left for that reading.
- SC-142 (a scanning mark beside a folder, a missing folder greyed and located again): the mark is new on screen and needs its place agreed (ADR-0243). The greying and Locate half was built in the evening round (`f3b8691a0`); the mark is what is left.

**SC-146.** The notice is one unselectable heading row at the list's top, in the existing heading style Hot-Swap already uses for "No matching samples": Browser-local `(119,46,185,19)` in the 308 × 443 Compact fixture. Words: "No matches in <label>", or "No matches yet: the library is still being indexed" for a library label while the first scan runs. Live's own words are not read: both are named constants (`BrowserPanel::kNoMatchesIn`, `kNoMatchesWhileIndexing`). An empty label with no search is as before (its Status Bar line).

**SC-145.** The grip is `(114,27,5,412)` in the 308 × 443 fixture: the left list's last 3 px and the 2 px gap to the content pane. The pointer shows the left-right resize cursor over it. The width runs from 60 px (`settings::kBrowserSidebarWidthMin`) to what leaves the content pane 80 px (`BrowserPanel::kContentMinWidth`); both are guesses until Live is read. It is saved on release under the hidden setting `browser.sidebarWidth` (default 113) and restored with the label. A Browser narrower than the kept width shows a narrower list and keeps the value. One change at the minimum Browser width (120 px): the content pane now keeps 80 px and the left list narrows to 60 px, where before the list kept 113 px and the content pane had none.

**SC-147.** With no search, Audio Effects starts as seven closed folder rows and Plug-Ins as one closed row per format, each opening to its makers, closed in turn. Folder rows are drawn as a Place's folders are (SC-126: triangle at the row's left, 12 px indent per level, the name in the row font), are selectable, and open by the triangle, a double-click, Enter or Right; Left steps from an entry to its folder and closes an open one. Open state lasts while ADI runs and is not saved. A typed search, All, and Hot-Swap keep the fixed headings of before. Tests that read device or plug-in rows now open the folders first with `openBrowserGroups` (in `tests/test_ui_shell.cpp`).

**SC-139.** A visit is a label with the search text it shows. One is kept when a label is chosen (the one left, with any typed search, and the one chosen) and when a search is taken into the list by Enter or Down. Back first keeps a search typed and not yet entered, so Forward comes back to it. A new visit after going back drops the visits ahead. A visit whose label has gone (a removed Place) opens All and is kept as All. Hot-Swap keeps none and disables both. The history lasts while the window is open, at most 100 visits (`BrowserPanel::kHistoryLimit`, a guess). Nothing new is drawn: Live 12's back and forward arrows beside the search wait for the Browser sheet (see "For the director"). The two command ids were taken in the order the table grows (WIN-04 asks only that ids never move); no register id was needed.

**The build PC.** All four items were pushed with headless tests written for them, but no slot check has run on them yet: slot 1 has shown a warm-up check of `origin/land/basics-1128c` (mode ui) as RUNNING since its window tests passed at 15:31 (4 of 4 suites), and every `run` since has answered BUSY. Slots 2 to 6 and 8 wait for a build place. The next session should start with `slot.sh run 1 origin/basics/browser-chain all` on the chain head, then fix what it finds.

**Next.** SC-138: a column per property (the index's size and date), sorted by a header click, chosen from the header's menu, and file extensions hidden. Start at `paint` (the Name header) and `sortRows` in `src/juce/ui_browser.cpp`; `LibraryFile` carries no size or date yet. Then SC-142's greying and Locate half.

## Restart of 28 November, evening

A third session continued the stream at 17:40. `origin/land/basics-1128c` is now an ancestor of `origin/main`, so BASE is `origin/main`; it merged cleanly into the chain (`0fd68e875`). The earlier session's local branch is still checked out in its own worktree, so this session works on a local branch of another name and pushes it to `basics/browser-chain`.

**The build PC.** Slot 1 still shows the warm-up check of `origin/land/basics-1128c` (mode ui) as RUNNING, with its last line at 15:31:41 (`UI CTEST 0`); at 17:43 and after, `slot.sh run 1` answered BUSY and `slot.sh wait 1` waited its nine minutes without a change. It looks hung after its window tests, and slot.sh has no way to stop it. Slot 2 was building, slots 3, 4, 5, 7 and 8 waited for a build place. Nothing on this chain since SC-137 has had a slot check yet: SC-146, SC-145, SC-147, SC-139 and SC-138 are pushed with headless tests that have not run.

**SC-138.** Live 12's list header takes a right-click that chooses the columns beside Name. ADI offers Name (always shown, so disabled), Date Modified, Size and Kind, then Show File Extensions; the choice is kept with the program under two hidden settings, `browser.columns` (comma-separated words `date`, `size`, `kind`; an unknown word is passed over) and `browser.showExtensions` (default on, today's names). A left click on a column's header sorts by it from its lowest (oldest, smallest, A); a second click turns it round; Name's first click still turns the name order round, as SC-136 made it. A row with nothing in the sorted column (a folder's size or date, a device) comes first, by name, in both directions; ties go by name. Hiding the sorted column returns the list to its order by name. The sort keeps each folder's children under it, as SC-136's does.
- Layout in the 308 × 443 Compact fixture (content pane `(119,27,185,19)` header): the extra columns sit right of Name, in the order Date Modified (96 px), Size (52 px), Kind (48 px); Name keeps at least 60 px. With Size alone: Name `(119,27,133,19)`, Size `(252,27,52,19)`. With Date Modified and Size chosen, Size is not drawn (it would leave Name 37 px) and stays chosen: Date Modified `(208,27,96,19)`. At a 500 px Browser: Name `(119,27,229,19)`, Date Modified `(348,27,96,19)`, Size `(444,27,52,19)`. The widths are named guesses (`BrowserPanel::kDateColumnWidth` and the rest) until Live is read. The header's columns follow the rows' width, so they stand over the rows when the list shows a scroll bar; the last one drawn reaches the header's end.
- Cells use the field font in the dim text colour, a 1 px line in the Browser's edge colour at each extra column's left in the header, and the order triangle on the sorted column. Size reads as `juce::File::descriptionOfSizeInBytes` ("10 bytes", "4 KB"); Date Modified as `YYYY-MM-DD hh:mm` in local time; Kind as the audio file's extension in capitals (WAV, AIF), `MIDI`, `Project` or `Folder` (named constants `kKindFolder` and the rest; Live's words are not read). Collection-colour marks stay at the right end of the Name column.
- With no extra column chosen, nothing is drawn differently: Name's span is the whole row and the header is as before, so the software references should not move.
- Data: `LibraryFile` gains `size` and `modifiedNs` (Unix-epoch ns, as `library::Entry` keeps them). The indexed path copies them from the index entry, the read-only walk reads them from the directory entry, Current Project reads them per file. A file that cannot be read keeps 0, which shows nothing.

**SC-142, the first half.** The row's audit read "a folder that cannot be read vanishes from the list"; since SC-132 it keeps its Places row and its problem (`LibraryPlace::problem`), and a click on it says the problem in the Status Bar. What was missing: nothing showed which Place had gone, and the only way back was Settings. Now:
- The Place's name is drawn in the dim text colour (`look::textDim`, as an entry this build cannot load), its icon as before. `BrowserPanel::Entry::missing` carries it; the User Library row is dimmed the same way when its folder has gone.
- Its right-click menu reads Show in File Manager, **Locate Folder...**, Remove from Places (a Place that is there has no Locate). Locate Folder asks the app through `BrowserPanel::locateFolder`; the app opens its folder chooser ("Locate the sample folder", starting beside where the folder was when that still exists) and `relocateContentFolder` (in `adi/ui/library_folder_settings.hpp`) writes the chosen folder into `library.contentFolders` at the gone entry's index, so the Place returns in its row. A chosen folder already listed only drops the gone entry; a path that is no folder changes nothing. Then the library scans again and the Settings window refreshes, as Add Folder does. The User Library has no Locate: its path is the `library.userLibrary` setting, a separate edit.
- Live's own way of offering this is not read; the item and its words are ADI's (see "For the director").
- Nothing moves: rows keep their bounds; only the gone Place's text colour and the menu's one item are new.

**Next.** A slot check of the chain head in mode all, as soon as slot 1 takes one, and the fixes it asks for. Six items wait for it: SC-146, SC-145, SC-147, SC-139, SC-138, SC-142. After that the browser queue holds only the rows under "Not now".

## For the director

This session owns the Browser queue.

SC-138. The header's right-click now offers Date Modified, Size and Kind and Show File Extensions; none is shown until chosen, so the list looks as before. When chosen they appear right of Name at the widths in the round of 28 November, evening (96, 52 and 48 px), and a column that would leave Name under 60 px is left out rather than scrolled to, where Live scrolls the list sideways. Recommendation: settle the widths, Live's exact column list and the horizontal scroll with the Browser sheet; keep the rest.

SC-142. A Place whose folder has gone is now drawn dim and its right-click offers Locate Folder..., before Remove from Places. Live's own words and place for this are not read. Recommendation: keep it in the menu, and when the Browser sheet is drawn, decide whether the gone Place also shows its Locate in the content pane. The scanning mark beside a folder still being read needs its place on that sheet.

The build PC. Slot 1 has shown one check as RUNNING since 15:31 on 28 November and refuses new ones; slot.sh cannot stop it. Recommendation: win stops slot 1's check on the build PC so the browser chain can be checked.

SC-147. Live's Audio Effects and Plug-Ins folders start closed, and so do ADI's now. A user who knew the old long list sees seven folder rows instead. Recommendation: keep Live's way.

SC-139. Live 12 shows back and forward arrows beside the Browser's search; ADI has the two commands and their keys and draws no arrows yet. Recommendation: draw them with the Browser sheet, left of the search field, in the round-button column's style.

SC-145. The left list's least width (60 px) and the content pane's least width (80 px) are guesses until Live's are read; the width is kept with the program, not with a song. Recommendation: keep both until the Browser sheet.

SC-123. The default auto Preview setting starts off and the standalone level starts at −12 dB, both named constants. Enabling auto Preview waits for a selection gesture, and clicking the same selected row replays after the previous preview ends. These readings await an actual Live comparison. The preview row remains in today's location for the next item, SC-429.

SC-133. Keep the provisional color order Red, Orange, Yellow, Green, Blue, Purple, Gray and the single combined Favorites view until the Collections sheet is reviewed. The seven marks toggle independently on the focused list's lead row. Keep unresolved sample paths so their marks return if the folder comes back, and keep distinct files separate when their names match. A sample moved to a different path takes a new identity until there is a relink rule.
