# The engine lane — Codex work seat (18 November 2026, resuming after 12 November)

You are the **engine** lane of ADI (`adi_daw` in the `arieladi/Adi` monorepo), a Codex work seat.
Everything in `adi_daw/collab/prompts/2026-11-01-engine-lane.md`, its second issue
`2026-11-04-engine-lane-codex.md` and the resume note `2026-11-11-engine-lane-codex.md` still
holds: what you own, what you never touch, the reading order, the gate, "never stop", stacking your
own branches, the three cross-compiler traps. Read those three first, then this one, then
`adi_daw/collab/engine.md` from the top.

Use everything you have: build, run, measure, stress, install what helps, write harnesses.

## Where you are

Your last entry is 12 November: "M2 history fuzz gated and pushed; taking the seeded note-diff
gate". The queue runs again from there.

**Seven of your requested PRs were never opened. That was win's miss, found on 18 November.**

| Branch | Head | Your title |
|---|---|---|
| `engine/automation-splice` | `b65f9415` | Preserve outside automation curves during bounded replacement |
| `engine/automation-splice-store` (on the one above) | `b680966d` | Capture automation replacement with exact native storage and Undo |
| `engine/row-ids` | `dc2c025b` | Share owner allocation of persistent SQL row IDs |
| `engine/history-move-to` | `2be4cc9c` | Restore retained history points in one transaction |
| `engine/relative-mixer-gestures` | `e10dcbcd` | Add origin-based relative mixer selection gestures |
| `engine/quiet-gesture-end` | `ff6659f2` | End common gestures after an explicit source quiet interval |
| `engine/history-retention-fuzz` | `500c5ed4` | Gate history retention with 3000 edits and seeded navigation |

Win is landing them now as one chain, in the order you pushed them, after its MSVC leg and an
outside review of the undo and storage parts. Until your fetch shows the top of
`origin/engine/log` saying "the chain of 18 November landed":
- cut from your own pushed branches when an item builds on one, as the 4 November issue says;
- do not rebase or force-push those seven. Win may push fixes onto them: fetch before you touch
  one, and read win's log (`collab/win.md`, top) for what it changed and why.

**Still open and held: #530** (`engine/clip-trim`, at `da1de8db` with two commits of win's). Win's
note of 12 November is on the PR and in `collab/win.md`.

## What moved on main while you were away

- **The Clip View and its Note Editor** (win, #586 to #624). The window now leans every day on
  `readClipDetails`, `note.insert`, `.delete`, `.move`, `.resize`, `.setVelocity`, `.setMute`,
  `.quantize`, and `clip.setLoop`, `.setStartMarker`, `.setEndMarker`, `.setScale`,
  `.setTimeSignature`. A change to what any of them accepts or returns is a change to a working
  editor: say so in the item's log entry.
- **The Mix View seat** (another Codex seat) builds the Mix View across the lanes, its own DSP
  included, in the `mix_*` files; its branches are `mixview/…`.
- **The control slice (#512) merged on 16 November.** Its fence on `ops.hpp`, `ops.cpp`,
  `ops_catalog.cpp`, `changeset.*` and `src/main.cpp` is lifted.
- **`docs/DEFERRED.md` is the register of everything postponed** (ADR-0239). Whatever you cut out
  of an item is a row there in the same change; a row closes only with the PR that does the work,
  which names the row's id. Your rows are DF-E1 to DF-E12.
- **The look of the window was rejected and is being settled on sheets** (ADR-0243, ADR-0244,
  `collab/window/design/`). It is the window's and the Mix View seat's work. Nothing of it is yours
  to build unless a need row asks.

## What to take, in order

1. **#530 and DF-E2, with DF-E5.** Decide which End wins when `end_marker_ticks` and
   `source_end_ns` disagree, and make `clip.setStartMarker` and `clip.setEndMarker` keep the two in
   step so they cannot disagree; then align the rule or #535's `silentAfterEnd` fixture; then the
   linear sample-defaults test with a nonzero ns offset.
2. **DF-E9.** Automation Undo deletes and re-inserts lane and data rows, so a later minor's CASCADE
   child or extra column is lost or refused. Restore in place. Your `automation-splice-store`
   already captures exact native storage for one path: say in the entry what it covers and what
   DF-E9 still needs.
3. **DF-E11.** The audio callback sets no flush-to-zero or denormals-are-zero, though the settings
   catalogue says denormals are flushed for the callback (ADR-0042 d6).
4. **The smaller rows:** DF-E1 (End-marker rounding and `playEnd`), DF-E7, DF-E6, DF-E3, DF-E4.
5. **The intermittent failures, found, not hidden:** DF-E10 (`adi_link_session_tests` under
   parallel ctest), DF-E12 (`adi_project_files_tests`, `Store::create` of `Cached.adi` returning
   null once), and the two of 11 November if they are still unexplained. No raised timeout and no
   "flaky" mark: the cause, or a stress loop that shows it is not in the code.
6. **The window's needs** (`collab/window.md`, "Needs from other lanes"). Several rows you gated
   and win landed are not struck there yet (N45 to N51 among them); win strikes them. A row that is
   truly open and has no PR is yours, oldest first. The window lane has the browser's Plug-Ins
   (DF-W9) on its list and will raise what the registry lacks as new rows: those go to the head.
7. **What the Note Editor cannot do until notes carry more,** in PLAN's order: MI-08 (per-note
   expression: playback and the `noteExpression` writes, with gain and pan beside pitch, slide and
   pressure, DF-W12), then MI-09 (Velocity Deviation and probability groups). DF-W14's table and
   playback (repeats, occurrence, recurrence) and the tables DF-W15 names are P2: after these.
8. **Your own "Next:" line:** the seeded note-diff fuzz gate that closes M2, then the queue as the
   earlier issues order it.

## Three things the design round may ask of you

Build none of them ahead of a need row; know that they are coming.
- **"Sounding now"** in the Mix View shows the channels that sound at the moment it is chosen. The
  held peak per strip (N48, on main) is the likely source; if the Mix View seat needs more (a
  per-strip "has sounded since" that the reader clears), it will come as a row.
- **A clip must always show what is in it.** Some clips drew empty in win's retest of 17 November
  (DF-W21). Win is finding out why; if peaks were not delivered, it becomes a row of yours.
- **Reviews.** DF-M36 names you as the reviewer of the Mix View seat's `prepare` contract in three
  hosts. Take it when that seat asks through win.

## Unchanged

Log each item on `engine/log` with its "PR requested" line (head SHA, base, title); item branches
never touch `collab/engine.md`. Win opens and merges the PRs and runs MSVC; you never wait for CI.
Win reads `engine/log` for requests at the start of every session.
