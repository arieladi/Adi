# Basics Look

The Linux session owns `basics/look-chain`, started from `origin/main`. The queue is `collab/prompts/2026-11-27-basics-look.md`. This session skips pass 9 and the Live readings. Measurements use logical pixels from peerless components and software renders. The Linux UI suites run in Debug with `DISPLAY` unset, and the session suite runs with the `acceptance` label.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-70 | `basics/look-chain` at `f8f32f63` | Plain ruler ticks, darker bar lines, a grid badge above the elapsed ruler and a whole-bar opening zoom. Window seat. | Four regression checks failed on the baseline. Debug Werror passed, as did 17 UI suites without DISPLAY and all 3 acceptance suites. The subsequent upstream conversion yielded the same GCC object with debug sections stripped. | Pushed |
| SC-112 | `basics/look-chain` at `3ddd7c56` | Independent persistent Info analyser with Spectrogram as its initial mode. Right click chooses a renderer and left click opens Analysers. Window and Mix View seats. | Peerless menu, render and persistence checks passed. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-111 | `basics/look-chain` at `587573d1` | Info uses the Browser column width through the dock layout. Window seat. | Peerless width, swap and narrow layout assertions passed. Reviewed and refreshed the structural software fixtures. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-110 | `basics/look-chain` at `26569ddf` | Removed the AI tab and its chat space so the list reaches the preview. Old projects can still decode the former flag. Window seat. | Headless geometry and legacy settings checks passed. Reviewed and refreshed the structural software fixtures. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |
| SC-72 | `basics/look-chain` at `02bf4001` | Reveal the insert marker after Home, a locator jump or an entered position. Window seat. | Peerless Home, typed-position and locator tests verified the viewport and marker. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |

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

## For the director
