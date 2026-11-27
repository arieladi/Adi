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
| SC-429 | `basics/browser-chain` at `f2c761cd8` | Reveal preview from the first eligible sample selection and retain the current list height. Browser seat. | The headless visibility, reservation, folder and hidden-command checks passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-431 | `basics/browser-chain` at `f23561dc8` | Report missing physical Cue outputs and map sparse device grants consistently. Engine and window seats. | Headless status, sparse stereo, failed restart, metronome and preview tests passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-127 | `basics/browser-chain` at `6286d5a37` | Keep working labels and list saved presets under Sounds. Browser seat. | The roster, fallback and preset search and load passed headless. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-128 | `basics/browser-chain` at `3bdc44247` | Ship an original starter drum kit under Drums and Samples. Browser, library and packaging seats. | The headless roster and read-only listing, deterministic WAV generator and relocated Linux installer smoke passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |
| SC-432 | `basics/browser-chain` at `e073649db` | Register a stable Hot-Swap command for the active device or sample slot. Window seat. | Headless command ordering, focus, rebinding and menu-label checks passed. Debug Werror, all 18 no-display UI suites and all 3 acceptance suites passed. | Pushed |

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

## For the director

This session owns the Browser queue.

SC-123. The default auto Preview setting starts off and the standalone level starts at −12 dB, both named constants. Enabling auto Preview waits for a selection gesture, and clicking the same selected row replays after the previous preview ends. These readings await an actual Live comparison. The preview row remains in today's location for the next item, SC-429.
