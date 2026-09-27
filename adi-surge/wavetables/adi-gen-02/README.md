# adi-gen-02 — 98 generated wavetables

The pack `adi-surge` ships (ADR-0011). Every sample was computed from first
principles by our own generator: no recorded audio, no third-party audio, and
no reference tables. MIT, in `LICENSE` next to this file.

**It does not replace Surge's own wavetables.** Surge ships 418 factory tables
and they stay exactly as they are. This pack sits beside them.

| Bank | Tables | What the measurements say |
|---|---|---|
| `Chimes/` | 14 | sparse spectra — half the harmonics missing (density 0.52), narrow (about 19 harmonics): bells, chords, struck metal |
| `Core/` | 14 | the full harmonic series to the top (bandwidth ~993), saw-like balance (odd 0.75): the bread-and-butter tables |
| `Grit/` | 14 | narrow but with a strong peak (9 dB) and the widest sweeps in the pack — brightness moves by 6x from first frame to last |
| `Talk/` | 14 | the strongest formants (13 dB), even/odd balanced, and they darken as they morph |
| `Texture/` | 14 | everything at once — brightness 65, the whole spectrum filled: noise beds and air |
| `Tones/` | 14 | dark and hollow (odd 0.91, brightness 2.6): square- and pulse-like fundamentals |
| `Voices/` | 14 | vowel-like, two formants (11 dB), narrow band: the singing tables |

**Format.** Serum-style. Each frame is 2048 samples, 32-bit float mono, 44.1
kHz header, 8 to 32 frames per table. Every file carries a `clm ` chunk
(`<!>2048 ...`), so Surge, Serum and Vital all read it as a wavetable.

**Verified before it was added** (2026-09-28, Windows):

- All 98 load through **Surge's own loader**: staged into Surge's factory
  wavetable folder and run against the upstream test "All Factory Wavetables
  Are Loadable". A deliberately corrupt file was added first to prove the test
  really reads that folder — it failed, as it should, and passed again once
  removed.
- None is silent or clipped: peaks run 0.33 to 0.999.
- One table, `Texture/14-Static`, is identical frame to frame. That is what its
  name says it is. Every other table moves.

**Using them.** Surge scans `.wt`, `.wav` and `.wtscript`. Copy the bank
folders into your user `Wavetables` directory and they appear in the browser
next to the factory ones. Moving the pack into Surge's own factory folder
waits on the fork origin (ADR-0006).
