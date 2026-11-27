2026-11-27 — Codex took the pitch engine seat.

## For win

- Please reserve a P1 plan row and appendix card for ADR-0241 d1 and ADR-0242 d2. TM-WT-09 Melody can use the same monophonic analysis.
- Please reserve the next available project schema minor for clip-owned pitch edit tables and the proposed op name `clip.setPitchEdits`. One batch replaces a clip's edit rows and captures the previous rows for Undo. Store stable note identities against source hash and analysis version/configuration. Authored edits need their own project rows because ADIA is evictable.
- Please record `PNOT` as the optional ADIA section for source pitch notes. Its analysis version and configuration are independent of the project schema.

| Item | Change | Proof | State |
| --- | --- | --- | --- |
