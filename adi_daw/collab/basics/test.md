# The director’s test

This session owns the director’s test queue.

The chain started from `origin/basics/integrate-2`. Linux window suites use Debug with warnings as errors and DISPLAY unset.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-433 | `basics/test-chain` at `722582fc8` | Keep hover instructions and keys inside Info View. Window and Control Bar seats. | The peerless checks show only an icon title in the tooltip and full hover text in Info View. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-439, SC-175 | `basics/test-chain` at `82af8bf4c` | Keep a held lane click as an insert marker and draw its flag. Arrangement seat. | Peerless replay covered stationary repeats, genuine drags, the status label and marker pixels. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-441, SC-445 | `basics/test-chain` at `f06b67887` | Play on from at or after Loop End. Ordinary stereo metronome playback was audible in the synthetic device fixture. Engine, recording and project seats. | The transport and document rehearsals reproduced the Loop fault and verified forward clip playback, click audio and a linear take. Debug Werror, 4 targeted suites, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-440, SC-176 | `basics/test-chain` at `f06b67887` | Loop Selection and the normal Arrangement range menu are already integrated. Arrangement seat. | Peerless tests checked Cmd/Ctrl+L, saved brace placement, range retention and the edit and Time menu commands. The normal range menu dispatches the same Loop Selection command. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Already in integration |
| SC-425 | `basics/test-chain` at `d438ea3fd` | Publish usable plug-ins from readable folders and name the unreadable ones. Plug-in and Browser seats. | The same permission-denied VST3 fixture failed before and passed after for incremental and refresh scans on Linux. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-442 | `basics/test-chain` at `d438ea3fd` | Cmd/Ctrl+1 and +2 are already integrated in the Arrangement and key maps. Edit and window seats. | The headless command and grid fixture verified both directions, displayed spacing and persistence. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Already in integration |
| SC-444, SC-203 | `basics/test-chain` at `afceb2556` | Make the normal fade curve drag follow the pointer and keep Alt as the reversal. Clips and Arrangement seats. | An extracted source probe and the peerless fixture covered direction, the stored fade shape and one Undo. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-201 | `basics/test-chain` at `afceb2556` | Pitch, cents and the supported Warp controls are already integrated. Clip and engine seats. | The peerless Clip View, Warp actions and real audio playback suites checked the existing controls and renderer. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Already in integration |
| SC-435 | `basics/test-chain` at `a07084ccc` | Resize Main and pinned Returns from the top edge with the bottom held. Arrangement and mixer seats. | The headless fixture moved the Main header and Return mixer hit zones and measured their anchored bounds. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-436 | `basics/test-chain` at `51d7e02c3` | Update lane, header and mixer together throughout the drag. Arrangement and mixer seats. | Peerless divider pixels and actual strip bounds changed before release, including when pinned growth recycles the mixer slots. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-434 | `basics/test-chain` at `6684a5b50` | Offer all 70 track colour slots using the ADR-0211 fallback palette. Arrangement seat. | The display-free fixture painted and hit-tested all 70 slots, refused spacer clicks, and checked saved colour and Undo. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-437 | `basics/test-chain` at `948ca190e` | New projects open with Main and four source tracks, with no factory Returns. Project and Store seats. | The bootstrap failed before and passed after. Explicit-return, render-intent and stem rollback fixtures and the acceptance project verified both defaults and authored routing. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-443 | `basics/test-chain` at `5a3379305` | Show a magnifier on the Arrangement zoom strips. Arrangement seat. | The peerless fixture checks the installed cursor on the ruler and Overview and keeps the scrub cue. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-438 | `basics/test-chain` at `70270208f` | Cover empty groups and Main-only projects through the document and engine. Safety, control and project seats. | Headless fixtures verified save and reopen, routing cleanup, agent reads, freeze behavior and three exact silent stereo exports. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |

## Measurements and findings

### SC-433 · Hover text

The Arrangement previously sent its full description to both JUCE’s hovering tooltip and Info View. The tooltip now skips the Arrangement, value fields, and other components unless they are explicitly marked as icon controls. Play, Stop, Arrangement Record, Follow, Metronome, Loop, Info View and Detail View can still show their title alone. The same policy runs in the main and detached windows, with the existing user preference and hover delay. Info View retains the full phrases and keys, including Play’s Space hint. The component bounds and Info placement are unchanged.

The shell fixture checks the tooltip policy and the independent Info text without displaying a tooltip window.

### SC-439 and SC-175 · Arrangement insert marker

The starting branch includes the clip-body drag anchor from `afdf57e55` and marker-only primary click from `2d7a5181c`. A stationary press still enlarged the span when JUCE dispatched its held-drag repeat. At the fixture’s interior x=10 the anchor was 11,531,520 ticks and the synthetic repeat extended it to 17,297,280 ticks, one quarter note. The range now stays zero until the pointer moves.

The blue marker still uses a 1 px stem in the selected lane. A 7 × 5 px flag is centered over the stem at the lane top. The peerless fixture exercises repeat events with and without JUCE’s drag bit, the flag and stem pixels, an empty-lane click, the Insert Mark status, and real range drags in both directions.

At 1440 × 900 the focused clip-body fixture measures the stem at canvas-local `(9,0,1,86)` before and after. The flag occupies `(6,0,7,5)`.

The first integrated run surfaced two legacy fixture gestures that sent a MouseDown-shaped event into MouseDrag. Both now pass their actual initial and final pointer positions through the drag helper. The same checks still require the range to span three tracks.

### SC-441 / SC-445 Metronome and Loop

The director's follow-up on main (`500e1da66`) identified the empty four-bar brace behind the Loop symptom and set the expected behavior. At 48 kHz and 120 BPM, the saved brace covers samples 0–384,000. Starting on a clip at sample 408,000 formerly rendered from 24,000 and advanced to 24,256. The new engine renders 408,000–408,255 and advances to 408,256 with the saved switch still on. Play before the brace runs into it and wraps at End. Compensated audio and MIDI input follow that same clock. A recording beyond the brace is linear.

On Linux, the original engine produced an audible click on both channels of a synthetic physical 1/2 device after ordinary Play with Loop off, and clicked on the next grid beat after an off-grid locate. The same session reproduced the separate loss of the actual imported clip when Loop was enabled. The changes fix the platform-independent Loop fault and preserve the click. Existing SC-431 follows the explicitly selected physical Cue Out and reports an unavailable pair. Ordinary-click silence on the director's actual CoreAudio device and project remains unverified.

If the CoreAudio symptom persists, the missing reproduction data are the Cue Out pair, Cue Volume, and Click While Recording setting in the director’s project. The synthetic result supports keeping the selected Cue route while checking those settings.

The existing SC-27 shell fixture now locates explicitly inside a one-bar brace. It continues to check both refusal with MIDI armed and audio admission within the loop. The new document rehearsal covers the complementary start beyond the brace.

The separately run transport suite still encoded the old wrap rule. Follow-up `d438ea3fd` updates its independent sample-by-sample model. The Debug warnings-as-errors run passed all 20 checks, including 253,708 exhaustive positions and offsets.

### SC-440 and SC-176 · Loop Selection

The integration branch already contains Cmd/Ctrl+L from `a12f1cf75`, retention of a range when opening its menu from `493109bdc`, and the range actions and Loop Selection from `f19f5732f` and `a64c36e1f`, carried by merge `9d6bfb23d`. The window suite exercises Cmd/Ctrl+L, the persisted Loop switch and brace fields, range retention and the applicable edit and Time menu entries. The normal range menu routes Loop Selection to the same command. The fixture directly asserts its enabled state in the clip menu. A headless measurement put the default four-bar brace at `(0,0,74,8)` in the scrub and the selected bars 3–5 at `(37,0,37,8)`.

The broader clip-menu work still tracks Arrangement Crop and Consolidate separately.

### SC-425 · Plug-in discovery with unreadable folders

The candidate names each denied scope, continues through the readable folders, and publishes the successful scan. Incremental and refresh keep the existing cache records whose coverage cannot be verified in the denied scope. Cancellation and fatal discovery errors keep their previous behavior. The Browser exposes the incomplete-scan warning in its list and the Status Bar makes it available in the application.

A Linux headless fixture puts readable VST3 bundles beside a mode-000 folder. The baseline exited with no usable publication. The fixed incremental and refresh paths returned three readable descriptors and one denied-path diagnostic, and retained both prior module cache rows. The host platform test used the repository’s VST3 fixture. Loading the director’s installed macOS plug-ins remains a native retest.

### SC-442 · Grid shortcuts

`cd58df145` already registers Cmd/Ctrl+1 for Narrow Grid and Cmd/Ctrl+2 for Widen Grid in Options and all three key maps. The existing shell fixture tests command delivery and settings persistence. In the default adaptive fixture the line spacing is 18.50 px, 9.25 px after Narrow, and 37.00 px after Widen from the default. The lower-right grid label is 46 × 14 px.

### SC-444 and SC-203

Commit `afceb2556` fixes the vertical drag sign. The curve handle now follows a regular up or down drag. Option or Alt reverses the bend. The fade shape stored in the project uses the existing engine evaluator and the gesture still saves one undo step.

The headless formula probe took the gesture bodies directly from the baseline and candidate Arrangement sources and ran them through the engine Bézier fade evaluator. At the neutral diamond midpoint, a 30 px up drag used to place the handle 23.6068% of the clip body height below its starting point. It now places it 23.6068% above. Downward is symmetric. Option or Alt keeps the reversed response. The diamond remains 8 × 8 px and its x is the midpoint between fade start and peak. The independent 7 × 7 px start and peak handles and dashed limit from a9c3a597f are already present on the starting branch.

At a normal 30 px upward drag starting at canvas `(20,224.5)`, the measured diamond y moves from 241.733 before to 207.267 after. The Option/Alt result is y241.733.

### SC-201

The starting branch already contains the semitone and cent controls and the unwarped source-rate playback path from f63e56b39. The Warp switch came in af2603ca4, the six placed-clip modes in 00a7cbd7a, editable source BPM and half/double controls in 242438058, and marker and transient editing in 09c5db0f2. The waveform timing actions were integrated with 885d1feb3 and their later fixes. No SC-201 source change is needed for those controls.

Unwarped pitch changes the source clock through resampling. Re-Pitch derives pitch from the Warp playback rate, disables the independent pitch controls, and keeps the stored value for other modes. The Warp toggle still refuses already pitched, formant-shifted or reversed clips in the conversion path. Source BPM can be entered when the analyzer cannot infer a steady beat. Warp preparation runs off the UI and the real-time audio callback.

Complex Pro’s renderer can consume an existing absolute semitone formant shift and runs the Rubber Band formant-preserving option. The stored value is not equivalent to Live’s Formants compensation percentage. The editor exposes neither that stored control nor Live’s Envelope. Beats transient loop/envelope controls and the Tones/Texture grain and fluctuation settings have renderer support with defaults but have no per-clip persisted controls on this branch. The existing six algorithm choices are the supported Warp mode controls.

The 1100 × 278 Clip View fixture measures Pitch `(6,164,69,16)`, Detune `(81,164,69,16)`, Warp `(229,2,52,13)`, BPM `(285,2,75,13)`, division `(363,2,23,13)`, multiplication `(388,2,23,13)`, and mode `(415,2,84,13)` in local pixels. The headless engine fixtures passed 171 playback and 96 Warp edit checks, and the peerless editor exercised pitch, Undo, timing controls, modes and marker editing.

### SC-435 · Resize Main and Returns from the top

Baseline `f02bd171c` has the five-pixel resize zone at the bottom of every Arrangement header and mixer strip. Commit `a07084ccc` places that zone at the top of each row in the pinned foot. An upward drag increases the height using the existing bottom anchor. Returns inside a group follow the scrolling rows and use their lower edge.

Coordinates are Compact logical pixels. The 1440×900 fixture has a 525 px lane body beginning at root y115. Rows use x328 and width 788 in the lane, x1116 and width 95 in the name header, and x1211 and width 194 in the Arrangement mixer. The values below are lane-local where specified. The peerless fixture asserts the painted cursor and these y/height changes through `rowTop` and `heightFor` during the drag.

| Stage | Main local y, height | Return immediately above Main local y, height |
|---|---|---|
| Before either drag | 485, 40 | 445, 40 |
| Drag Main top up 24 px | 461, 64 | 421, 40 |
| Drag Return top up 16 px | 461, 64 | 405, 56 |

Main’s bottom boundary is y525 throughout. The Return’s bottom boundary is y461 throughout its own drag. At the starting layout the five-pixel Main hit zone moves from y520–524 to y485–489, and the Return’s from y480–484 to y445–449. The horizontal hit range covers the header and the mixer background. The fixture starts Main’s drag on its header at local `(60, 487)` and starts the Return’s on its mixer component at `(20, 2)`.

### SC-436 · Keep lane, name and mixer geometry aligned during a drag

The base redraws Arrangement graphics during a drag and reflows the actual mixer components on release. Commit `51d7e02c3` refreshes the three surfaces as one drag operation. The mixer keeps the component with the active pointer bound to its track while rows come into or leave view.

In the 1440×900 group fixture, the dragged row begins at lane-local y80. The following row moves from y120 to y140 as the dragged row grows from 40 px to 60 px. The two-pixel painted separator moves from y118–119 to y138–139 in both the name header and the lane. The following mixer row moves from root y235 to y255 during the drag. Its column is x1211 and width 194. The test samples the separator pixels and the mixer bounds before issuing mouse-up.

The secondary 1200×720 fixture has a 345 px lane body and three pinned 48 px rows. An upward 72 px Return drag moves its top from lane-local y201 to y129, increases its height to 120 px, and leaves Main at y297. A regular mixer strip leaves view as the pinned foot grows. The test checks the active mixer component keeps the Return ID and follows the new geometry before mouse-up.

### SC-434 · Track colour chooser

The palette goes from a single row of 14 swatches to five rows of 14. The component grows from 246 × 22 px to 246 × 90 px. Swatches remain 14 × 14 px on a 17 px pitch, with the first at `(4,4)` and the last at `(225,72)`. A display-free software paint and hit test visited all 70 entries and six spacer or exterior points. The shell regression checks assignment beyond the former row and Undo.

ADR-0211 d3 specifies repeating the 14 tracked swatches until the approved colours are available. The complete measured table is cited under the first Live 12 checkpoint in `collab/win.md`. It was recorded in the private measurement folder. The 70 addresses are usable, and exact RGB parity remains pending that table.

### SC-437 · New projects

Before, New seeded four source tracks, A Reverb, B Delay and Main. The two Returns created eight sends and included their own devices, chains, references and parameters. New now creates four source tracks and Main. Their names and colours carry forward, and Main takes the fifth position. Opening an existing project or an authored template uses its saved routing.

The linked headless bootstrap failed on the old default and passed all 39 checks after the change, including initial silence and history. Return-specific fixtures explicitly author their Returns and sends, and the acceptance walkthrough creates them before testing routing.

The linked Debug Linux shell passed 5,365 checks, the document suite passed 1,037, the view seams passed 629, and WTS01 had zero failures. Each ran with DISPLAY unset. WTS01 begins with the clean five-track song and creates its returns explicitly before checking send levels, group routing, reopening and a bit-identical export.

### SC-443 · Magnifier on Arrangement zoom strips

The baseline routes vertical dragging to zoom on the beat-time ruler and Overview, as ADR-0129 specifies. Commit `5a3379305` gives both surfaces the same custom magnifier cursor. The cursor is an 18×18 logical-px sprite rendered into a 36×36 software image. Its hotspot is `(7, 7)`. The lens spans `(2, 2; 10×10)` before stroke. The handle runs from `(11, 11)` to `(15.5, 15.5)`.

The default 1440×900 Compact fixture has the beat-time ruler at `(328, 55; 788×29)` and the Overview at `(328, 39; 784×16)`, before and after. The scrub band remains at `(328, 84; 788×24)` with its pointing-hand cursor. The peerless test checks the two zoom surfaces share a custom cursor, while the scrub band, lane canvas and tempo editor retain their individual cues.

The peerless before-and-after replay verified the shared JUCE cursor is distinct from Normal and Pointing Hand. It measured the same vertical drag zoom from 4.625000 to 5.648988 in both runs, with DISPLAY unset.

### SC-438 · Empty groups and Main-only projects

The new regression authors a child clip and an explicit route, deletes the child and other ordinary tracks, and saves and reopens the empty group. It checks cleanup of the clip and route, the group’s implicit route to Main, level-2 agent projection, export and freeze. The ordinary Freeze action gives the expected refusal for a group, and the whole-group engine operation can freeze and unfreeze the empty group safely.

The test then deletes the group, saves and reopens the Main-only song, and checks its graph, agent read and clean Main Freeze refusal. Each of the three read-back WAVs contains 129 stereo frames of digital silence at 48 kHz. The production behavior passed without changes.

The final chain at `70270208f` built ADI in Debug with JUCE, Pd and warnings as errors. With DISPLAY unset, all 18 UI suites, 3 acceptance suites and 6 targeted suites passed. The targeted suites cover control projection, project files, track recording, library queries, SMF and Return sends.

## For the director

SC-441. Retest ordinary Play on the Mac with the same project and device. If the click is silent, record the Cue Out pair, Cue Volume and Click While Recording setting. Keep the selected Cue route while checking those settings.

SC-434. Please supply the measured 70-colour table cited in collab/win.md. I recommend using that table to replace the temporary modulo-fourteen palette in the existing 70 slots.

## The director’s second test


The first chain landed in #830. I merged `origin/main` and `origin/wincheck/basics/test-chain` into `basics/test-chain` at `2cf51c958`. All 295 headless tests outside the performance label passed in Debug with warnings as errors and DISPLAY unset.

The second chain starts from `origin/main` at `c0d22ae45`. This session owns the director’s second test queue.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-449 | `basics/test-chain-2` at `5cdad58fe` | Refresh the native Edit menu when Redo becomes available. Window and edit seats. | The peerless menu fixture follows real Undo and Redo and uses the platform chord. Physical macOS key delivery remains a native check. All 295 headless tests outside the performance label passed in Debug with warnings as errors. | Pushed |
| SC-475, SC-184 | `basics/test-chain-2` at `385447fcd` | Split uses the marker track or picked clips and ranges. The explicit all-tracks setting remains. Edit and window seats. | The focused display-free fixture failed seven checks before and passed 98 after, including one-step Undo and Redo. All 295 headless tests outside the performance label passed in Debug with warnings as errors. | Pushed |
| SC-453, SC-229 | `basics/test-chain-2` at `b33ec13de` | Open Quantize Settings and route shortcuts into focused notes and picked Arrangement MIDI. Clips, edit and engine seats. | The dialog probe covers all ten timing choices, independent endpoints, Amount, Cancel and OK. The revised Functions fixture passed all 194 checks and both mixed-grid regressions passed. All 295 headless tests outside the performance label passed in Debug with warnings as errors. | Pushed |
| SC-451 | `basics/test-chain-2` at `7494d44fa` | Pinch zooms the Note and Sample Editors around the pointer and shares the existing Follow behavior. Clips and Arrangement seats. | Real JUCE magnify calls in the peerless fixture preserve the pointer anchor across all three editor surfaces. A physical Mac trackpad remains a native check. All 295 headless tests outside the performance label passed in Debug with warnings as errors. | Pushed |
| SC-459 | `basics/test-chain-2` at `d3a4b40f1` | Present accepted device automation values in the control display. Engine and devices seats. | The headless Utility replay tracked 0.25 and 0.75 after both readbacks stalled at 0.50 on the baseline. Sound scheduling and history were checked separately. All 295 headless tests outside the performance label passed in Debug with warnings as errors. | Pushed |
| SC-460 | `basics/test-chain-2` at `8595bb034` | Volume, pan and send menus show, delete and re-enable target automation. Mixer and automation seats. | The focused peerless checks cover actual mixer menus, untouched sends, target-specific deletion and Undo. The broader Live menu reading is tracked in SC-478. All 295 headless tests outside the performance label passed in Debug with warnings as errors. | Pushed |
| SC-457, SC-462 | `basics/test-chain-2` at `c7e5385cc` | Show Info for occupied surfaces and expand the analyser through its caption row. Arrangement, Browser, window and Analysers seats. | Peerless hover replays exercise clips, blanks and controls. A software-painted spectrogram matches the full raster across all 128 by 256 pixels. All 295 headless tests outside the performance label passed in Debug with warnings as errors. | Pushed |
| SC-469, SC-470 | `basics/test-chain-2` at `1ec5604c0` | Default to No Dither and offer Show File in the completion message. Export and window seats. | The peerless export fixture checks the actual rendered file, reveal action, cancellation and a vanished file. The preferences probe and 341 settings checks passed. Playback still uses its existing path. All 295 headless tests outside the performance label passed in Debug with warnings as errors. | Pushed |

### Second test measurements and findings

#### SC-449 Redo on macOS

The macOS default already registered Cmd+Shift+Z. JUCE's native menu validation reads the enabled flag captured when the menu item was built. ADI's menu model did not observe command-state changes, so it could keep the disabled entry after Undo made Redo available.

The menu model now follows the command manager. Undo, Redo and history restores signal state changes. Querying an already-active project's menu does not schedule another update. Menu headings retain their existing shortcut feedback, and each platform keeps its registered keymap. Live uses Cmd+Shift+Z on macOS and Ctrl+Y on Linux and Windows.

Redo is still item 2 of Edit. There are no added controls or position changes. A peerless fixture builds the Edit menu data and exercises real history edits and mapped keys. It tests the Linux Live chord and shifted Z through the Cubase preset, and will test the native Live chord when built on macOS. No desktop or native menus are required.

The linked Debug fixture passed seven of nine checks against the baseline and all nine against the fix with DISPLAY unset. The separate isolated message-loop probe held an Edit menu cache and made no manual menu reads between actions. The baseline delivered zero updates. The fix delivered one after a normal rename, one after Undo and one after Redo. Cached Undo and Redo availability followed the history, and querying the menu during its callback did not schedule a second refresh. Native keyboard delivery on macOS remains a runner check.

The first full Redo run tripped the unchanged History List wall-time ratio. A solo run passed all 79 checks at 8.05 ms for 2,000 retained operations and 60.12 ms for 8,000. The subsequent full runs schedule that timing test on its own while retaining it in the same headless set.

#### SC-475 and SC-184 · Arrangement Split

Base `6aeeed1b1`. Main already has the batch split from `cc7bc859a`. The safety seat’s published `3789b35ac` adds picked clips, retains picked multi-track ranges, and keeps the anchor's complete span if it only touches the cut point. It still defaults to cutting every visible track when there is just one insert marker.

Candidate `1c021026d` carries the published safety fix and defaults `editing.splitAllWithNoSelection` to off in the registry and command cache. Drawing a marker in a lane and pressing Cmd/Ctrl+E cuts the clip under the marker in that lane. Doing the same with multiple picked clips cuts each selected clip that crosses the marker, in one Undo/Redo. A user's explicitly enabled REAPER option still splits every eligible track. Settings with no explicit override adopt the current registry default.

At 1440 × 900, the marker was canvas-local x18 y0 in integer coordinates. Each track originally covered x0–37. After the operation, the marker lane had spans x0–18 and x18–37. The other track retained x0–37. Component geometry is unchanged. The fixture also tests two-edge cuts across a picked time range and the optional all-tracks scope.

A focused Debug replay compiled the candidate’s shell implementation, settings registry and exact `splitAndRangeEdits` fixture with the Werror CMake flags, linked read-only built dependencies, and ran with temporary ADI_HOME and DISPLAY unset. Passed 98 checks, zero failures and no unexpected JUCE assertions. The integrated test suite is the chain owner's separate gate.

The same fixture linked without the two production replacements failed seven checks, including the new default, single-lane scope, picked-range retention, and picked-clip Split/Redo. Main's shell and settings sources are identical to those of the built baseline on these paths.

#### SC-453 and SC-229

Candidate 2015e58f3 on 6aeeed1b1. The old Edit command disabled Quantize Settings outside the Note Editor. In the editor the command changed the Functions tab and could leave its controls out of sight. The new command opens the Quantize Settings window from the Note Editor or a selected Arrangement MIDI clip. Cmd/Ctrl+U uses the mapped command and quantizes selected notes in the focused Note Editor. In the Arrangement it quantizes the MIDI clips selected there, using the Arrangement grid if Quantize To is Current Grid. The Sample Editor keeps its focused Warp quantization route. With no applicable MIDI selection the command offers a reason.

The dialog uses the previous stored defaults and offers Current Grid; 1/4; 1/8; 1/8T; 1/16; 1/16T; 1/32; 1/32T; 1/8 + 1/8T; and 1/16 + 1/16T. Adjust Note Start and Adjust Note End are independent. Amount runs from 0 to 100%. OK updates those settings and commits the operation in one undo step. Cancel discards the pending changes. The two mixed grids offer the union of their timing lines. Exact ties move toward the later line. Existing projects without an alternate-grid field continue to use their original grid.

The old command opened no window. The new content measures 340 × 156 px, excluding the native title bar. Logical local control bounds are Quantize To (124, 12, 204, 24), Adjust Note Start (124, 45, 92, 24), Adjust Note End (222, 45, 92, 24), Amount (124, 78, 204, 24), Cancel (180, 120, 68, 24) and OK (260, 120, 68, 24).

The linked transform suite reproduced two targeted baseline failures and passed all 38 checks on the candidate. It verifies that straight and triplet lines both apply, ties choose the later point, and undo and replay preserve the transaction. Added shell fixtures reach the actual controls through a content callback. They exercise the mapped shortcut, applying end-only quarter-note quantization at 50% to one selected note, one-step undo, and the Arrangement keyboard route. Those tests create no native dialog or menu.

A linked headless smoke of the actual JUCE dialog content passed. It selected the tenth grid, adjusted the two endpoint checkboxes and Amount, confirmed Cancel did not apply, then confirmed OK delivered the chosen settings. The measured content and settings rows match the bounds above. The Debug Werror syntax check passed for the shell, Clip View, and full shell fixture.

The full gate exposed another inline Clip View fixture tied to the former shortcut. That fixture now opens the Functions tab directly, checks all ten grid choices and the existing Start, End, Amount and Swing controls, and undoes only the transaction it creates. The exact peerless fixture changed from 10 failures to all 194 checks passing, with zero unexpected JUCE assertions.

#### SC-451 Trackpad pinch in the editors

The bundled JUCE Cocoa peer delivers `magnifyWithEvent:` to the component under the pointer. The Arrangement Canvas already handled that callback. The Note and Sample editors inherited JUCE's default forwarding, which left their timelines at the same scale. Both now use the same cursor-centred time geometry as their established wheel zoom, pause Follow while the user changes the view, and repaint the relevant controls and editor.

| Time viewport and gesture | Before (px/quarter, left quarter) | After (px/quarter, left quarter) | Anchor |
| --- | --- | --- | --- |
| Note Editor at component x=236 px, ×1.5 | (64, 4) → (64, 4) | (64, 4) → (96, 5) | 7×PPQ at x=192 px inside the grid |
| Sample Editor at component x=192 px, ×1.5 | (64, 4) → (64, 4) | (64, 4) → (96, 5) | 7×PPQ at x=192 px |
| Arrangement at component x=192 px, ×1.5 | (64, 4) → (96, 5) | (64, 4) → (96, 5) | 7×PPQ at x=192 px |

A subsequent ×0.5 pinch yields (48, 3) in all three current handlers with the same x=192 px timeline anchor. Arrangement Option-pinch takes the hit track from 88 to 132 px and keeps the scale at 48 px per quarter. The Note Editor probe also leaves the viewport and Follow alone for neutral, zero and infinite factors.

The focused display-free probe compiled the exact application callback bodies with the production timeline geometry and used real JUCE virtual mouse callbacks. The shell regression invokes the same callbacks on real Arrangement, Note, and Sample editor components with a saved-project fixture, a MIDI clip, and an imported audio clip. The Linux probe cannot exercise macOS physical input.

Candidate `e8d0a35b5`. Ninja-derived Debug syntax with warnings as errors passed for both edited translation units. The focused JUCE comparison passed with `DISPLAY` unset. The full shell and acceptance suites are the chain gate.

#### SC-459 | Device View follows automation during playback

Candidate `38ab5a83c53e30e7767ad2a333cf2e44d4c4d19c`, authored on `6aeeed1b1`. Seats touched: audio engine observation, window ParameterFeed, headless shell test.

The graph already sent the envelope to the device, but ParameterFeed obtained its playing value from the device's stored target getter. In Utility that getter retains the manual setting while the callback applies automation separately. The binding now publishes each lane's last accepted normalized event through a lock-free atomic. ParameterFeed reads it on its existing message-thread cadence only for a live, subscribed, following lane. Missing and overridden controls use their existing presentation source. The event's frame, normalization, deduplication, and processing path are unchanged, and the callback adds no allocation, lock, plugin query, or message/window work.

The peerless shell regression uses Utility Gain on 3 Audio at root size 1440 × 900. Manual value 0.50. Automation holds 0.25 initially and 0.75 after the second breakpoint. The assertions read the real Slider, the published playing and stored values, and the unchanged slider rectangle on both blocks. The fixture also verifies that the underlying getter remains 0.50 and playback adds no project history entry. The test never attaches a peer or opens a menu. Disabled-Pd builds skip the Utility fixture.

The knob measured 28 × 28 px at local `(13, 13)` before and after each frame, at root `(531, 756)` in the 1440 × 900 test layout. Baseline playback read 0.50 at both breakpoints. The candidate read 0.25 and 0.75. The existing ring draws those values at 0° in the baseline and −67.5° / +67.5° after the fix. The stored value remained 0.50 in both replays.

Linux Debug Werror syntax passed for all three changed translation units, including the full shell suite source. The extracted shell fixture was compiled with the same flags and linked against existing baseline support objects, then replayed with the baseline and the two changed production objects separately. With `DISPLAY` unset, the baseline had two dial failures out of eight checks. The candidate passed all eight checks. Both runs had zero JUCE assertions. `git diff --check` passed. The integrated CTest gate is separate.

#### SC-460 Mixer control menus

Main at `6aeeed1b1` already provided target-specific Delete Automation and Re-Enable Automation when a Volume, Pan, or Send lane existed. It offered no Show Automation, and the unautomated Volume menu was empty. Volume, Pan, and Send now expose the same Show Automation action as device dials. The existing delete helper clears only the clicked target and preserves Undo. Pan modes, Send Enable and tap choices, and the track-title menu remain available. Popup anchors stay on their controls, with no control bounds changed.

The send chooser now agrees with the knob on the selected routing row. Choosing Show Automation on an untouched send prepares a silent route in a single undoable edit and preserves its enabled state. Opening a menu never creates the route. Volume, Pan, and already-routed Sends need no edit to show an envelope.

For the director, the M1–M3 menu observations are still pending. The Live readings branch has no Right-click menus report. Keep these existing commands and compare any additional entries and their order in SC-478 when the readings arrive.

#### SC-457 and SC-462 · Info hover and analyser plot

The Browser used its description throughout the column. The Arrangement canvas used its directions over bare timeline. The Info box now uses actual hit positions to tell a clip or occupied Browser row from empty space. Controls with their own description still show it. A blank part of a lane and the spacer between the Browser's sidebar sections keep the analyser visible.

The Info dock keeps the same outer bounds and frame. The spectrogram no longer reserves its 5 px inset and 16 px caption strip. In the exact 128 × 256 plot fixture the previous inset plot would start at `(5,21)` and occupy 118 × 230 px. The new plot starts at `(0,0)` and occupies the full 128 × 256 px. Compared pixel for pixel with the matching raster in the larger analysis view, the compact renderer shows the same data. Filled and line spectrum compact views also give the heading space to their plots. Arrangement, Browser, window and Analysers seats.

#### SC-469 and SC-470 · Export defaults and completion

Candidate `ac5ade66b` is based on `6aeeed1b1`. Window export choices and Settings both start at No Dither. Explicit preferences still take effect. A successful single export names the published filename in the Status Bar. A completed batch names its first published file and counts the remaining files. Show File sends the exact native path through JUCE's file-manager action. It is cleared on the next render or on a cancelled or failed result, and a press checks the file still exists. The playing engine, render worker and export interruption are unchanged.

The Status Bar had no export action before. With a completed export at 1440 × 900, Show File occupies status-local `(988,11,64,16)`, in the message field `(35,8,1021,22)`. At width 960, the corresponding bounds are `(508,11,64,16)` in `(35,8,541,22)`. The button leaves 12 px to the existing second field. It takes 68 px of message text space including the 4 px gap. The frame and Export page controls keep their previous positions.

The peerless shell regression uses the actual Export keyboard command and a real headless WAV render, directs Show File through a supplied path callback, and checks cancellation, an empty queue, vanished output and fresh versus explicit dither preferences. The production JUCE call selects the file in Explorer or Finder; JUCE's Linux implementation opens the containing directory. No native file-manager window is opened in tests.

The six affected C++ translation units, including the application and full shell fixture, passed Debug warnings-as-errors syntax checks. A focused link of the production Export page, preferences, job conversion, and completion projection passed with DISPLAY unset. It checked No Dither through the render job, an explicit Rectangular override, exact file path, batch naming, partial cancellation and empty results. The separately linked settings suite passed 341 checks. The integrated Status Bar fixture execution remains part of the chain gate.
