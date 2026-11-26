# Basics Look

The Linux session owns `basics/look-chain`, started from `origin/main`. The queue is `collab/prompts/2026-11-27-basics-look.md`. This session skips pass 9 and the Live readings. Measurements use logical pixels from peerless components and software renders. The Linux UI suites run in Debug with `DISPLAY` unset, and the session suite runs with the `acceptance` label.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-70 | `basics/look-chain` at `f8f32f63` | Plain ruler ticks, darker bar lines, a grid badge above the elapsed ruler and a whole-bar opening zoom. Window seat. | Four regression checks failed on the baseline. Debug Werror passed, as did 17 UI suites without DISPLAY and all 3 acceptance suites. The subsequent upstream conversion yielded the same GCC object with debug sections stripped. | Pushed |
| SC-112 | `basics/look-chain` at `3ddd7c56` | Independent persistent Info analyser with Spectrogram as its initial mode. Right click chooses a renderer and left click opens Analysers. Window and Mix View seats. | Peerless menu, render and persistence checks passed. Debug Werror, 17 UI suites without DISPLAY and 3 acceptance suites passed. | Pushed |

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

## For the director
