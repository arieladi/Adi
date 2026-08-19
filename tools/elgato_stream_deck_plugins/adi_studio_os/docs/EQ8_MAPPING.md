# EQ8 — the mapping (V37 rebuild)

Rewritten to Adi's ruling after the original hitboxes proved unusable. The
"before" analysis that motivated this is preserved at the bottom.

**Geometry.** One zone is 200 x 100. The strip is six of them drawn as ONE
1200 x 100 picture and sliced, so a value spans nothing and each zone is
self-contained. Touch arrives per zone; `slot = floor(gx / 200)`.

---

## Dials

| dial (slot) | turn | short press |
|---|---|---|
| **1** (0) | Output Gain | **page** the band window (FULL only) |
| **2-6** (1-5) | the ACTIVE parameter of that dial's band | **cycle the mode** FREQ -> GAIN -> Q |

* Mode is **global to the strip**, not per band: every dial shows the same
  parameter, so pressing any band dial cycles all of them.
* FULL has five band dials over eight bands, so the window starts at band 1-4.
* COMPACT (State 2 docked, 4 dials) has three band dials, fixed to **B1 B2 B3**,
  and no paging.

## Touch — exactly two functions per band

| `ly` | action |
|---|---|
| 0 - 7 | **dead** (top margin) |
| **8 - 44** | **toggle band on/off** |
| 45 - 55 | **dead** (the gap; the value is printed here) |
| **56 - 92** | **cycle filter type** — a held touch cycles backwards |
| 93 - 99 | **dead** (bottom margin) |

* **`x` is ignored inside a zone.** There is no left/right split any more, so
  there is no horizontal edge to miss — only the vertical third you aimed at.
* **Dial 1's column is completely inert to touch.**
* The zone is DRAWN as its hitboxes: a top pill, the value in the gap, a bottom
  pill. What you see is what you can press.

## Removed outright

| gone | why |
|---|---|
| the FREQ/GAIN/Q/GLOB **tab row** | it was drawn in ALL SIX zones and `_tabHit` ran on zone-local `x` with nothing restricting it to one zone, so the top 17 % of the whole strip was a six-times-over mode switcher. Mode is on the dial press now. |
| **GLOB** mode | its Output Gain became a permanent dial 1; Scale is no longer reachable. |
| **pagination arrows** | 22 px targets at the extreme edges of zones 1 and 6. Paging is dial 1's press. |
| the **horizontal** split in the bottom band | left-42 %/right-58 % with no margin, so a slightly-off touch muted a band instead of changing its filter. |
| `_buildTabs`, `_tabHit`, `_pageArrow`, `_buildGlobals`, `_buildGlobalZone` | deleted, not just unused — two of them called `_buildTabs` and would have been a latent crash behind an unreachable branch. |

The per-band dB approximation and its plotting (`_bandDb`, `_buildGraph`) are
KEPT though nothing draws them: that is the expensive part to reconstruct, and it
references nothing that was removed.

---

## Two inferences in this design that are MINE, not Adi's words

1. **Dial 1 = Output.** "for EACH of the EQ bands (Dials 2-6)" says the band dials
   are 2-6. It does not say what dial 1 is. Output Gain was the only global worth
   a knob, and GLOB mode was being deleted anyway. **Consequence: COMPACT drops
   from four bands (B1/B2/B3/B6) to three (B1/B2/B3).** If four bands in compact
   matter more, dial 1 should be a band there.
2. **Pagination on dial 1's press.** Touch can no longer carry it and it had to
   live somewhere, or bands 6-8 become unreachable in FULL.

---

## Appendix — what was wrong before (the analysis that led here)

The old map had THREE overlapping Y bands with no margin at all:
`TAB 2-17`, `MID 19-60`, `BOT 62-97`. On top of that:

* the mode selector was duplicated across all six zones, so a touch aimed at a
  band that landed 10 px high silently switched the whole strip's parameter;
* the bottom 36 % was band-mute on the left and filter-type on the right with no
  dead space between them;
* a lingering touch reversed the filter-type direction;
* and until the timing fix the visuals were up to a minute stale, so correct
  hit-testing was being done against a picture Live had already left behind.
