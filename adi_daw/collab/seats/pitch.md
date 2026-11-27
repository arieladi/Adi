2026-11-27 — Codex took the pitch engine seat.

## For win

- Please reserve a P1 plan row and appendix card for ADR-0241 d1 and ADR-0242 d2. TM-WT-09 Melody can use the same monophonic analysis.
- Please reserve the next available project schema minor for clip-owned pitch edit tables and the proposed op name `clip.setPitchEdits`. One batch replaces a clip's edit rows and captures the previous rows for Undo. Store stable note identities against source hash and analysis version/configuration. Authored edits need their own project rows because ADIA is evictable.
- Please record `PNOT` as the optional ADIA section for source pitch notes. Its analysis version and configuration are independent of the project schema.

| Item | Change | Proof | State |
| --- | --- | --- | --- |
| Typed note edits | Atomic worker commands for pitch, straightening, tilt, scale quantization, timing, formant, gain/mute, drift and transition. | 35 checks passed under strict GCC and ASan/UBSan. Making correction a no-op failed five contour, drift and transition checks. The fixture measured +100.25 cents with 30 cents of tilt, a 12.50 ms move and a 1.50× stretch. | Engine seam verified. Project ops, persistent rows and undo wiring depend on win's reservations. |
| Monophonic analysis | A worker-only difference-function pitch tracker with source-frame notes, 5 ms contour points, fitted drift and residual vibrato. Sustained pitch plateaus separate connected notes. | A continuous-legato regression went from one merged note to three. All 17 VocalSet notes matched, with maximum errors of 4.97 cents, 34.06 ms onset and 22.45 ms offset. The synthetic scale passed at 8, 44.1, 48 and 96 kHz. | Verified in the chain. Long scoops can divide a note. Vibrato can mask a weak connected change. |
