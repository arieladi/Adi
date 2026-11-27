# Basics analysers · 27 November 2026

Queue from `origin/main` on `basics/analysers-chain`. The measurement reference is `adi-m4l-analyzer/`. Its shipped structural, spectrum and weighting tests passed in Linux. The ADI checks run with DISPLAY unset.

| Item | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| 1 · Measurement core (DF-W22) | `basics/analysers-chain` · `ab3547fd` (`09543efe` item commit) | Added periodic Hamming and Blackman windows to the Mix View seat's shared analysis. The meter uses the reference K response, normalized at 1 kHz. The A and C filters meet the reference points at every ADI rate. | The new tests caught 12 window checks and 40 K response differences before the fix. The full Linux Debug build passed with warnings as errors. All five validators, the binary layout check, 272 headless suites, 18 UI suites with DISPLAY unset, and three acceptance suites passed. | Ready for landing |
| 2 · Filled Spectrum (DF-W22) | `basics/analysers-chain` · `43c4a5be` | The Mix View seat's analysis worker computes four curves from each 4096-point analysis frame. Its 194 ms ballistics feed the Filled Spectrum in Mix View and Info. The graph reduces the worker bins to one value per display column, with a 40 Hz display schedule. | The two shell routing assertions failed on the old code. The worker verifies nine successive hops, including a peak from before the final hop. Software drawing checks cover the held fill, column reduction and cursor values. The full Debug build, five validators, layout check, 272 headless, 18 no-display UI and three acceptance suites passed. | Ready for landing |

## Visual checks

The coordinates below use logical pixels in each component's local space. Review the live picture with the same signal and matching settings in the reference device.

**1. Measurement core.** On the Compact Main output meter at the right of Mix View, the K-weighted Dynamic and RMS numbers are aligned to the left and right edges of the readout area, inset 8 px from the component edges. Dynamic is at y=86–100 and RMS is at y=100–114. Select the dBFS scale and alternate Flat and K on a steady 100 Hz sine. Compare the RMS difference with the reference's −1.6 dB and compare the Peak numbers at the same input level.

**2. Filled Spectrum.** In a 400×200 chart, the title occupies (5, 5, 390×14), with the plot at (5, 21, 360×160), a 30 px level ruler on the right and a 14 px frequency ruler below. A 224×112 Info chart uses the same origin and a 184×72 plot. The brighter 1.4 px live line and green fill sit over the darker held fill and its 1 px edge. The pointer draws 1 px crosshairs and a readout up to 172×15 px, starting 5 px right and 7 px below the pointer where space permits. Compare 10 Hz, 1 kHz and 20 kHz along the log axis, the −80 and −13 dB endpoints, and the dark fill after a short transient. At 440 Hz the note readout should say A3. On a 640 px Compact Analysers panel, the trace selector is at (314, 4, 218×17), with Realtime Maximum selected on first use.

## For the director

- Filled Spectrum uses the reference's fixed graph range and slope. The standalone analyser picker also offers Filled Spectrum and starts with the trace selected in Mix View when opened from that page. At its 800×420 starting size, the chart uses the same 5 px side inset, 21 px plot top and 30/14 px ruler widths within the chart component. Recommend reviewing that handoff with the Held Maximum trace selected, and using the existing standalone Spectrum for alternate scales and slopes.
