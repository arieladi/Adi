# Readings of Ableton Live 12, 27 November 2026

The answers to `adi_daw/collab/prompts/2026-11-27-live-readings.md`, done in Live itself on Windows 11.

- **Live's version:** Ableton Live 12 Suite 12.4 (the program file's version reads 12.4).
- **The default Set looks like Live's factory default:** two MIDI tracks (1 MIDI, 2 MIDI), two audio tracks
  (3 Audio, 4 Audio), two returns (A Reverb, B Delay) and Main, 120 BPM, 4/4. No user template is in the
  User Library's Templates folder. The new Set opens in the Session View; Tab shows the Arrangement.
  Its loop brace is at 3.1.1 with a length of 4.0.0.
- **The screen:** 1920 x 1080 at 100 %. Live is maximised. All positions below are screen pixels.
  The Arrangement's lanes run from x 344 to x 1619, so they are 1275 px wide. The track headers start
  at x 1620.
- Every reading used a new Set. Nothing was saved over an existing Set, no Setting was changed, and the
  library was not touched.

| R | What Live 12 does | How it was observed |
|---|---|---|
| R1 | **A new Set opens fully zoomed out.** The lanes are 1275 px wide. The left edge is bar 1 and the right edge is about bar 82.3, so a bar is **15.68 px** (16 bars = 251 px). **A label every 4 bars:** 1, 5, 9 … 81. An unlabelled 1 px tick falls halfway between labels (bars 3, 7, 11 …), and every label has a 6 px tick at its left. Pressing - does not zoom out further. | Pixel profile of a lane row and of the ruler, in a capture of the window. |
| R2 | **The grid text** sits in the lowest row of the lanes, the Main track's row (y 758–774). It is **right-aligned against the lanes' right edge**: the text spans x 1590–1615 and y 759–771, 4 px from the lanes' right edge and 1 px below the row's top. It is bold grey, 13 px tall, and reads **"1/1"** in a new Set. **The time ruler** is a separate strip below the Main row (y 777–792), the full width of the lanes. Its labels read "0:00", "0:10", "0:20" … (m:ss, every 10 s at the new Set's zoom). Each label has a short tick at its left. The insert marker shows as a cyan triangle on this ruler. The bar ruler, with the bar numbers, is at the top of the lanes (labels at y 114–120), above the loop brace row. | Pixel bounds read from the capture. |
| R3 | **Each zoom step is about ×1.6.** There are 30 steps from fully out to the closest zoom, centred on the insert marker. Measured levels: see the table below this one. **The grid** is the finest power-of-two note value whose lines are at least about 14 px apart (accepted spacings were 14.1–27.6 px; 12.9 px was refused). **The bar ruler's label step** is the finest of 4 bars, 2 bars, 1 bar, 1 beat, an eighth and a sixteenth that leaves room for its text. Bar and beat labels ("21", "21.2") need about 40 px; bar.beat.sixteenth labels ("21.1.3") need about 80 px. **Nothing finer than a sixteenth is ever labelled.** The time ruler's labels step 1-2-5 (10 s, 5 s, 2 s, 1 s, 500 ms … 1 ms). They are m:ss down to 1 s, then **m:ss:mmm** ("0:40:500"). **Past 1/16384 (step 22 and closer) the grid text is blank and no grid lines are drawn.** | 30 keypresses of +, capturing the window after each. Lines, labels and grid text were read from each capture. |
| R4 | **Every grid line is 1 px wide;** only the brightness differs. The lane's background alternates between two shades in bands (lum 72 and 79 in an empty lane). The bands are 16 bars wide when fully zoomed out, 4 bars at 65 px per bar, 1 bar at 165 px per bar and 1 beat from 672 px per bar. **Bar lines are darkest** (lum 51 on the 72 band, 55 on the 79 band). **Beat lines are lighter** (58 / 63). **Finer lines are lightest** (64 / 70). There is no thickness difference. | Luminance along a lane row at five zooms, matched to the musical position of each line. |
| R5 | **Follow is off in a new Set.** The Follow button, left of the position field, is unlit. Playing from bar 21 to 25.3.2 ran the playhead off the right edge (bar 24.4) and the view did not move. The button's Info View text gives the shortcut **Shift+Alt+F**, and says Follow can scroll continuously or page by page, as set in Display & Input. | Played 9 s at a 1/16 zoom and compared the view. |

**R3's zoom levels.** "Step" counts + presses from fully zoomed out. The px per bar is the grid line
spacing times the division.

| Step | px per bar | Grid text | Bar-ruler label step (form) | Time-ruler step |
|---|---|---|---|---|
| 0 | 15.7 | 1/1 | 4 bars ("21") | 10 s ("0:40") |
| 1 | 25.1 | 1/1 | 2 bars | 10 s |
| 2 | 40.2 | 1/2 | 1 bar | 5 s |
| 3 | 64.5 | 1/4 | 1 bar | 5 s |
| 4 | 103 | 1/4 | 1 bar | 2 s |
| 5 | 165 | 1/8 | 1 beat ("21.2") | 1 s |
| 6 | 264 | 1/16 | 1 beat | 1 s |
| 7 | 421 | 1/16 | 1 beat | 500 ms ("0:40:500") |
| 8 | 672 | 1/32 | an eighth ("21.1.3") | 500 ms |
| 9 | 1080 | 1/64 | an eighth | 200 ms |
| 10 | 1728 | 1/64 | a sixteenth ("21.1.2") | 100 ms |
| 11 | 2768 | 1/128 | a sixteenth | 100 ms |
| 12 | 4416 | 1/256 | a sixteenth | 50 ms |
| 13 | 7072 | 1/256 | a sixteenth | 50 ms |
| 14 | 11264 | 1/512 | a sixteenth | 20 ms |
| 15 | 18000 | 1/1024 | a sixteenth | 10 ms |
| 16 | 28900 | 1/2048 | a sixteenth | 10 ms |
| 17 | 46300 | 1/2048 | a sixteenth | 5 ms |
| 18 | 74500 | 1/4096 | a sixteenth | 5 ms |
| 19 | 119000 | 1/8192 | a sixteenth | 2 ms |
| 20 | 189000 | 1/8192 | a sixteenth | 1 ms |
| 21 | 303000 | 1/16384 | a sixteenth | 1 ms |
| 22 | 488000 | (blank, no lines) | a sixteenth | 1 ms |
| 30 | the closest zoom (about 2·10^7 by the ×1.6 rule) | (blank) | — | under one label per lane width |
