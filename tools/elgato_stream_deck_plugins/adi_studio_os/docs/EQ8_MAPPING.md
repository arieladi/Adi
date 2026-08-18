# EQ8 — the mapping Studio OS currently believes in

Written out at Adi's request, so the touch behaviour can be judged against what
the code actually does rather than against what it was meant to do. Every line
below is read off `js/ableton/EQ8Controller.js`, not remembered.

**Geometry.** One zone is 200 × 100. The strip is six of them, drawn as ONE
1200 × 100 picture and sliced (so a curve spans all six). Touch arrives per-zone
and is mapped back into full-strip space before hit-testing:
`slot = floor(gx / 200)`, `lx = gx − slot·200`, `ly = gy` (0–99).

**The three Y bands, out of 100:**

| band | rows | what lives there |
|---|---|---|
| `TAB` | **2 – 17** | the mode selector |
| `MID` | **19 – 60** | the band readout; pagination arrows at the far edges |
| `BOT` | **62 – 97** | band on/off and filter type |

---

## FULL layout — 6 dials

Modes: **FREQ · GAIN · Q · GLOB**. `focus` (F) is 1–3, so the six dials show
bands F … F+5 — a sliding window over the eight bands.

### Dials

| mode | dial 1 | dial 2 | dial 3 | dial 4 | dial 5 | dial 6 |
|---|---|---|---|---|---|---|
| FREQ | B(F) freq | B(F+1) | B(F+2) | B(F+3) | B(F+4) | B(F+5) |
| GAIN | B(F) gain | B(F+1) | B(F+2) | B(F+3) | B(F+4) | B(F+5) |
| Q | B(F) Q | B(F+1) | B(F+2) | B(F+3) | B(F+4) | B(F+5) |
| GLOB | **Output Gain** | **Scale** | — | — | — | — |

* **Turn** → `eq8FreqDelta` / `eq8GainDelta` / `eq8QDelta` on that band,
  `ticks × 0.02` normalised. In GLOB: `eq8GlobalDelta('output'|'scale')`.
* **Push** → `eq8ToggleBand(band)`. **In GLOB, push does nothing.**

### Touch

| where | action |
|---|---|
| `ly 2–17`, **any of the six zones** | select mode from `lx`: 4 segments of 48 px → FREQ / GAIN / Q / GLOB |
| `ly 19–60`, zone 1, `lx < 22` | `eq8Page(−1)` — only if `focus > 1` |
| `ly 19–60`, last zone, `lx > 178` | `eq8Page(+1)` — only if `focus < 3` |
| `ly 62–97`, `lx < ~85` | `eq8ToggleBand(band)` |
| `ly 62–97`, `lx > ~85` | `eq8CycleType(band, +1)`; a **hold** touch cycles −1 |
| anything in GLOB below the tab row | ignored |

---

## COMPACT layout — 4 dials (State 2 docked)

Modes: **FREQ · GAIN · Q**. GLOB is dropped entirely. Bands are **FIXED**, not a
window, and there is **no pagination**:

| dial 1 | dial 2 | dial 3 | dial 4 |
|---|---|---|---|
| **B1** | **B2** | **B3** | **B6** |

Tab row is 3 segments of 64 px instead of 4 of 48.

---

## Where I think the erratic touch comes from

Three things, and I am confident about the first two because they are structural.

**1. The mode selector is duplicated in all six zones.** `_tabHit` is evaluated
against zone-local `lx`, and nothing restricts it to one zone. So the top 17 % of
the ENTIRE 1200 px strip is a mode selector, six times over — and which mode you
get depends on where you landed inside whichever 200 px zone you touched. A touch
meant for band 4 that lands 10 px too high silently becomes "switch to GAIN".
That alone would read as random.

**2. The bottom third is two destructive-ish actions with no margin.** `ly 62–97`
is 36 % of the zone height; its left 42 % toggles a band off and its right 58 %
cycles the filter type. There is no dead space, so a slightly low touch aimed at
the readout mutes a band or changes a bell into a shelf.

**3. Until V34 the visuals were up to a minute stale.** The pump was a page timer
being clamped, so the strip Adi was touching described a state Live had already
left. Correct hit-testing against a stale picture still feels random. This one is
fixed; the two above are not.

**A fourth, smaller thing:** `hold` reverses `eq8CycleType`. A touch that lingers
— which a finger on a small target naturally does — steps the filter type
backwards instead of forwards.

I have deliberately NOT changed any of this. The parameter mapping is verified
data (L4) and the touch bands are Adi's to rule on.
