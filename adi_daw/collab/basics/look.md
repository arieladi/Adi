# Basics Look

The Linux session owns `basics/look-chain`, started from `origin/main`. The queue is `collab/prompts/2026-11-27-basics-look.md`. This session skips pass 9 and the Live readings. Measurements use logical pixels from peerless components and software renders. The Linux UI suites run in Debug with `DISPLAY` unset, and the session suite runs with the `acceptance` label.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-70 | `basics/look-chain` at `f8f32f63` | Plain ruler ticks, darker bar lines, a grid badge above the elapsed ruler and a whole-bar opening zoom. Window seat. | Four regression checks failed on the baseline. Debug Werror passed, as did 17 UI suites without DISPLAY and all 3 acceptance suites. The subsequent upstream conversion yielded the same GCC object with debug sections stripped. | Pushed |
| SC-112 | `basics/look-chain` at `3ddd7c56` | Independent persistent Info analyser with Spectrogram as its initial mode. Right click chooses a renderer and left click opens Analysers. Window and Mix View seats. | Peerless menu, render and persistence checks passed. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-111 | `basics/look-chain` at `587573d1` | Info uses the Browser column width through the dock layout. Window seat. | Peerless width, swap and narrow layout assertions passed. Reviewed and refreshed the structural software fixtures. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-110 | `basics/look-chain` at `26569ddf` | Removed the AI tab and its chat space so the list reaches the preview. Old projects can still decode the former flag. Window seat. | Headless geometry and legacy settings checks passed. Reviewed and refreshed the structural software fixtures. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-72 | `basics/look-chain` at `02bf4001` | Reveal the insert marker after Home, a locator jump or an entered position. Window seat. | Peerless Home, typed-position and locator tests verified the viewport and marker. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-73 | `basics/look-chain` at `4080cd3a` | Drag, wheel and per-part arrow input on the five Control Bar fields. Window seat. | The baseline fine-tempo wheel probe reproduced the failure. Peerless gesture and editing checks passed. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-76 | `basics/look-chain` at `c5f204b8` | Source the new-window Follow default and legacy fallback from one named constant. Window seat. | Tests checked fresh and legacy view states and explicit saved on and off. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-424 | `basics/look-chain` at `842c8114` | Docked Info at the foot of the full-height Browser column. Clip and Device View use the space beside the Browser and fill the width when it is hidden. Window seat. | The baseline run reproduced the earlier layout failures. Measured peerless before and after through swaps, collapse, width and Info toggles. Menu seam exercised all four modes and the Analysers click. Debug Werror, 17 UI suites without DISPLAY and all 3 acceptance suites passed. | Pushed |
| SC-06 | `basics/look-chain` at `1d04ccd5` | Display measured callback utilization and deadline misses, with Average, Current and Off. Window seat. | The focused headless callback probe passed eight checks including overrun and restart. The shell exercised the three modes and overload. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-37 | `basics/look-chain` at `55eac841` | Draw the authored clip colour through the Arrangement lane and overview. Engine and window seats. | Peerless chooser, title/body pixels and undo passed. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-90 | `basics/look-chain` at `81254e5e` | Dim the clips on inactive tracks and the full panels of bypassed devices. Window seat. | Peerless canvas and panel pixels, plus undo or reactivation, passed. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-92 | `basics/look-chain` at `164c387b` | Label the main Audio To route Main. Window seat. | Headless tests covered Main, Group, instrumentless MIDI No Output and the Main pair. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-71 | `basics/look-chain` at `d8e80eed` | Follow zoom below a quarter note in the Arrangement and Note Editor. Window seat. | The independent Debug shell also passed all 3,536 checks. The test covers grid positions, ticks and Note Editor gestures. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |

## Measurements

### SC-70

Measured in the 1440 × 900 Compact peerless fixture. Rectangles give root-relative `(x, y, width, height)` in logical pixels.

| Element | Before | After |
|---|---|---|
| Beat ruler | `(328, 55, 788, 29)` | `(328, 55, 788, 29)` |
| First label's text box | `(333, 69, 60, 14)` | `(332, 56, 60, 16)` |
| First bar tick | `(328, 73, 1, 8)` and a `3 × 1` hook | `(328, 72, 1, 12)` vertical stroke |
| Lane canvas | `(328, 115, 788, 525)` | `(328, 115, 788, 525)` |
| Grid spacing badge | `(1068, 642, 46, 14)`, reads `1/4` at the default zoom | `(1068, 624, 46, 14)`, reads `1 Bar` at the default zoom |
| Elapsed-time ruler | `(328, 640, 788, 18)`, 1-second ticks 96 px apart at 120 BPM | Same bounds, 15-second ticks 138.75 px apart at 120 BPM |
| New-project zoom | 48 px per quarter, 192 px per 4/4 bar | 4.625 px per quarter, 18.5 px per 4/4 bar, 74 px per four bars |

At the same 48 px per quarter, sampled bar and beat lines were both `#252525` on a `#323232` lane. The bar line is now `#1e1e1e` and the beat line stays `#252525`. At Compact scale both are one pixel wide. The approved Arrangement sheet uses a 74 px pitch between the labels 1 and 5, with a thin straight tick. The production default uses that pitch. Existing saved zoom values, and the implicit scale in older saved view records, remain readable. The Badge is above the elapsed-time ruler as the queue specifies.

### SC-112

At 1200 × 720 Compact, the Info box is `(8, 486, 200, 196)` and the analyser is `(12, 490, 192, 188)` in root-relative logical pixels. Both rectangles have the same bounds before and after. The opening display changes from the Analysers page's spectrum plot at `(17, 511, 164, 133)` to the Info spectrogram plot at `(17, 511, 182, 162)`. The preset switch is kept by window ID. A left click opens Analysers. The right-click test intercepts and counts the menu request in process.

Notes for the Mix View seat (DF-W22).

| Info mode | Comparison with its source |
|---|---|
| Spectrogram | The shared scrolling raster and theme palette show a blank plot before audio. Compared with VISU, the plot lacks frequency marks and waveform, graticule and scale controls. |
| Filled Spectrum | The existing Extended spectrum paints a tinted fill on its 10 Hz–30 kHz and 0 to −100 dB grid. SPAN's green default, labels and peak overlays are absent. |
| Line Spectrum | The existing Device scale paints a thin unfilled 10 Hz–24 kHz, 0 to −120 dB plot with frequency labels above and dB labels at left. Live's Spectrum parameter strip is absent. |
| Classic Scope | The original face fits a `192 × 69.82` region at `(12, 549.09)` in this box. It uses the triggered capture. Its controls are read-only here so the click and alternate-click can address the Info box. |

### SC-111

At 1200 × 720 Compact the Browser is `(8, 35, 308, 443)`. The Info panel goes from `(8, 486, 200, 196)` to `(8, 486, 308, 196)` and its inset analyser goes from `(12, 490, 192, 188)` to `(12, 490, 300, 188)`. The Device View moves from `(216, 486, 976, 196)` to `(324, 486, 868, 196)`. These are root-relative logical pixels. At 480 px with both side panels open the Info width decreases from 200 to 112, the actual width available to the Browser. When the Browser is collapsed its saved width supplies the Info width, bounded to half the window.

Both columns in the 1920 × 1080 approved sheet are 326 px wide. The Browser is `(6, 74, 326, 702)` and the Info box is `(6, 782, 326, 266)`.

### SC-110

At 1440 × 900 Compact the Browser frame `(8, 35, 308, 623)`, search box `(32, 39, 280, 21)` and preview `(32, 633, 280, 21)` retain their bounds in root-relative logical pixels.

| Element | Before | After |
|---|---|---|
| Collapsed tab | `(12, 607, 300, 20)` | Removed |
| Expanded tab and chat | `(12, 391, 300, 236)` | Removed |
| Browser content viewport | `(127, 81, 185, 526)` collapsed or `(127, 81, 185, 310)` saved open | `(127, 81, 185, 546)` in both cases |

The original collapsed tab bounds were confirmed by the peerless masked regression reference. Legacy `aiChat: true` is still decoded. The approved sheet uses the full list area up to the preview and has no intervening tab.

### SC-72

At 1440 × 900 Compact with 30 px per quarter, the lane canvas stays `(328, 115, 788, 525)`. On the selected MIDI lane the insert marker stroke is 1 × 86 px.

| Gesture | Projected marker before | Visible marker after |
|---|---|---|
| Typed `33.1.1` from song start | `(4168, 115)`, beyond the canvas | `(328, 115)`, visible left edge at quarter 128 |
| Home from quarter 128 | `(-3512, 115)`, clipped on the left | `(328, 115)`, visible left edge at quarter 0 |
| Next locator at `17.1.1` from song start | `(2248, 115)`, beyond the canvas | `(328, 115)`, visible left edge at quarter 64 |

Typing the already-visible next bar from quarter 128 leaves the viewport at quarter 128 and puts the marker at `(448, 115)`.

### SC-73

At 1440 × 900 Compact these Control Bar bounds are unchanged before and after, in root-relative logical pixels.

| Field | Bounds |
|---|---|
| Tempo | `(80, 9, 41, 17)` |
| Signature | `(162, 9, 40, 17)` |
| Position | `(446, 9, 82, 17)` |
| Loop start | `(723, 9, 85, 17)` |
| Loop length | `(873, 9, 85, 17)` |

The keyboard-part underline is now 1 px tall at root y=23, inset 2 px from each edge of the active part. A drag moves one step per 4 logical px and a wheel event moves one step. Tempo supports 1 BPM and 0.01 BPM parts, signature clicks work through legal denominators, and musical fields step by bar, beat or sixteenth according to the part. A peerless baseline wheel event on the fine tempo part left 120.00 BPM. It now produces 120.01 BPM. Typing remains available.

### SC-76

The Follow button is `(663, 9, 21, 17)` at 1920 × 1080 Compact and `(313, 9, 21, 17)` at 960 × 540. Its centred glyph is approximately 16 × 7 px. Bounds and the unlit initial state are the same before and after. One named setting now supplies the default for both new windows and legacy view data with no saved value. Explicit saved on and off toggles each round trip. The approved sheet shows the Follow group at `(640, 32, 36, 28)`, including the menu bar.

### SC-424

Measured before and after in peerless Compact roots. Rectangles are root-relative logical pixels in `(x, y, width, height)` order.

| 1440 × 900 scenario | Before | After |
|---|---|---|
| Default | Browser `(8, 35, 308, 623)`, Info `(8, 666, 308, 196)` | Same visible panes in a full working column `(8, 35, 308, 827)`. Its foot meets the Status Bar at y=862. |
| Clip View opened | Browser `(8, 35, 308, 329)`, Info `(8, 372, 308, 490)` | Browser `(8, 35, 308, 623)`, Info `(8, 666, 308, 196)`. Clip View is `(324, 372, 1108, 286)` and Device View is `(324, 666, 1108, 196)`. |
| Swapped with Clip View open | Browser `(1124, 35, 308, 329)`, Info `(8, 372, 308, 490)`, Clip and Device x=324 and width=1108 | Browser `(1124, 35, 308, 623)`, Info `(1124, 666, 308, 196)`, Clip and Device x=8 and width=1108 |
| Browser hidden after swap | Browser hidden, Info `(8, 372, 308, 490)`, Clip and Device x=324 and width=1108 | Browser and Info hidden, Clip `(8, 372, 1424, 286)`, Device `(8, 666, 1424, 196)` |
| Browser width increased by 60 px | Info `(8, 666, 368, 196)`, Device `(384, 666, 1048, 196)` | Same measurement. Browser and Info each grow from 308 to 368 px wide when the width changes. |
| Info View hidden with Browser open | Browser `(8, 35, 308, 623)`, list `(127, 81, 185, 546)`, Device `(8, 666, 1424, 196)` | Browser `(8, 35, 308, 827)`, list `(127, 81, 185, 750)`, Device `(324, 666, 1108, 196)` |
| Both detail panes hidden | Arrangement `(324, 35, 887, 623)`, Info `(8, 666, 308, 196)` | Arrangement `(324, 35, 887, 819)`, Info `(8, 666, 308, 196)` |

At 960 × 540, Info starts at `(8, 306, 308, 196)` in both versions. Turning it off now expands the list from `(127, 81, 185, 186)` to `(127, 81, 185, 390)`. The approved 1920 × 1080 sheet aligns both panes at x=6 with a width of 326 px and a 6 px split. The Compact implementation uses its standard 8 px gap. In a short window Info shrinks to leave the Browser's search and preview bars usable.

The View menu still exposes Info View. After swapping, the regression uses the new Info rectangle for the four analyser menu choices and for the click that opens Analysers. It also checks that the earlier location does not request a menu. The menu request is intercepted in process.

### SC-06

At 1920 × 1080 Compact the CPU field stays `(1811, 9, 58, 17)` and its arrow stays `(1860, 15.5, 4, 6)`. The new overload dot is `(1813, 15, 5, 5)`. At 960 × 540 the field stays `(851, 9, 58, 17)`. The initial value changes from the fixed `0 %` to blank until a callback has been measured. Average and Current display measured utilization, including values over 100%. Off hides the number. The callback publishes brief deadline misses to the timer, which latches the overload indicator. A device restart clears the previous stream's warning even when the next callback arrives between timer reads. The approved sheet groups the CPU cell with the MIDI indicators, with a CPU rectangle of `(1870, 32, 38, 28)` including the menu bar.

### SC-37

At 1440 × 900 Compact with 40 px per quarter, the peerless MIDI clip's canvas-local bounds stay `(40, 1, 80, 84)`. Its title is `(40, 1, 80, 11)` and the content is `(40, 12, 80, 73)`. Before, selecting the first clip swatch left the sampled title and body red `#eb4a41`. Both samples at `(100, 9)` and `(100, 35)` now show the authored pink `#f099a7`. Undo restores the track-derived red. The overview uses the same clip colour in its existing marker rectangle.

### SC-90

In the same clip fixture as SC-37, the muted track's clip bounds stay `(40, 1, 80, 84)`. The title and body samples change from `#f099a7` to `#5c5c5c` and are restored by undo.

The Device View fixture's bypassed panel keeps its local bounds of `(0, 0, 150, 180)`. The title is `(0, 0, 150, 17)`, the activator is `(2, 1, 16, 15)` and Configure is `(94, 2, 52, 13)`. The entire device takes a 55% frame-colour overlay during bypass. The body sample at `(100, 100)` goes from `#2b2b2b` to approximately `#222222` and the title sample at `(90, 10)` from `#383838` to approximately `#282828`. The activator remains operable.

### SC-92

Audio To changes from `Master` to `Main` in the existing strip-local rectangles. These are `(5, 54, 70, 13)` for an ordinary Arrangement audio track, `(5, 3, 70, 13)` for a return or group, and `(4, 106, 85, 13)` for the standard 95 px Mixer View strip. The approved Arrangement sheet draws Main in a 104 × 16 chooser at header x + 134. A grouped track still reads Group and an instrumentless MIDI track still reads No Output.

### SC-71

The spacing follows zoom down through 1/32. At 4.625 px per quarter the first 4/4 bar line stays x=18.5. At 48, 64 and 128 px per quarter the first sub-bar grid line moves from x=48, 64 and 128 to x=24, 16 and 16 respectively. The subdivisions are 1/8, 1/16 and 1/32. At 256 px per quarter a 1 × 3 px short ruler tick is at local `(32, 26)`. At x=64 the label tick is 1 × 5 px and the text box for `1.1.2` is `(68, 1, 60, 16)`. At this zoom the previous first line was x=256 and had no subdivision mark.

In the Note Editor at 64 px per quarter, drawing at local x=34 changes the new note from x=0 with a 64 px width to x=32 with a 16 px width. Right moves x=32 to x=48. At 128 px per quarter quantize moves x=173 to x=176. The regression covers drawing, the arrow, quantize and Chop at the zoom-derived grid.

## For the director

Confirm the Control Bar gesture rate and wheel feel in a real window when the window-driving session reaches the fields. Recommendation is to start with the implemented one step per wheel event and one per four logical px of drag.

Does a stock Live 12 Set start with Follow lit? Recommendation is to retain Off until the window-driving session confirms the default.

The separate MIDI SC-71 candidate opens with a fixed 1/16 grid. Recommendation is to retain Adaptive Medium for automatic zoom following until the Live reading settles the initial setting, and to land one implementation of the Note Editor marks.
