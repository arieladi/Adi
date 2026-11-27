# Basics analysers · 27 November 2026

Queue from `origin/main` on `basics/analysers-chain`. The measurement reference is `adi-m4l-analyzer/`. Its shipped structural, spectrum and weighting tests passed in Linux. The ADI checks run with DISPLAY unset.

| Item | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| 1 · Measurement core (DF-W22) | `basics/analysers-chain` · `ab3547fd` (`09543efe` item commit) | Added periodic Hamming and Blackman windows to the Mix View seat's shared analysis. The meter uses the reference K response, normalized at 1 kHz. The A and C filters meet the reference points at every ADI rate. | The new tests caught 12 window checks and 40 K response differences before the fix. The full Linux Debug build passed with warnings as errors. All five validators, the binary layout check, 272 headless suites, 18 UI suites with DISPLAY unset, and three acceptance suites passed. | Ready for landing |

## Visual checks

The coordinates below use logical pixels in each component's local space. Review the live picture with the same signal and matching settings in the reference device.

**1. Measurement core.** On the Compact Main output meter at the right of Mix View, the K-weighted Dynamic and RMS numbers are aligned to the left and right edges of the readout area, inset 8 px from the component edges. Dynamic is at y=86–100 and RMS is at y=100–114. Select the dBFS scale and alternate Flat and K on a steady 100 Hz sine. Compare the RMS difference with the reference's −1.6 dB and compare the Peak numbers at the same input level.

## For the director
