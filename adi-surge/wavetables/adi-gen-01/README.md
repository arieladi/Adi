# adi-gen-01 — 80 generated wavetables

The first wavetable pack from `tools/wtgen` (ADR-0009). Every sample was
computed by `wtgen generate` from a descriptor (41 numbers) and a seed. The
pack contains no recorded or third-party audio. ADR-0010 records the rule that
makes these ours, and the null test they passed.

| Folder | Tables | What they have in common |
|---|---|---|
| `Tones/` | 14 | dark and simple, from near-sine to soft |
| `Core/` | 10 | dense, smooth, full harmonic series |
| `Voices/` | 23 | a strong formant (vowel-like) peak |
| `Grit/` | 15 | rough, or very bright |
| `Chimes/` | 18 | sparse spectra: gaps, bells, chord-like stacks |

**The names describe the sound.** They were derived from each table's own
measured descriptor, not chosen by hand:

- **The first word is brightness:** *Pure < Soft < Warm < Bright < Glass <
  Sharp*.
- **An optional middle word is character:** *Hollow* (mostly odd harmonics),
  *Even* (mostly even), *Rough*.
- **The last word is how the table moves from its first frame to its last:**
  *Rise* (brightens), *Fall* (darkens), *Morph* (changes a lot), *Wave*
  (steady).

**Format:** Serum-style tables. Each frame is 2048 samples, stored as 32-bit
float mono, with a 44.1 kHz header. Tables have 2 to 35 frames. Each file
carries a `clm ` chunk (`<!>2048 ...`) and Surge's `srge` chunk, so Surge,
Serum and Vital all read it as a wavetable.

**Using them in Surge XT today:** copy this folder into the `Wavetables` folder
inside Surge's user data folder (normally `Documents\Surge XT\Wavetables`).
Shipping them as factory content means moving them into
`surge/resources/data/wavetables/`. That is a commit inside `surge/`, so it
waits for the fork origin (ADR-0006).

**Licence:** MIT, as `LICENSE` in this folder states. This is the default for
original work in `OPEN_SOURCE_POLICY.md` §1.
