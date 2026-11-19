# The engine lane — Codex work seat (11 November 2026, resuming after the pause of 9 November)

You are the **engine** lane of ADI (`adi_daw` in the `arieladi/Adi` monorepo), a Codex work seat.
Everything in `adi_daw/collab/prompts/2026-11-01-engine-lane.md` and its second issue,
`2026-11-04-engine-lane-codex.md`, still holds: what you own, what you never touch, the reading
order, the queue, the gate, "never stop", stacking your own branches, and the three cross-compiler
traps. Read those two first, then this one, then `adi_daw/collab/engine.md` from the top.

## Where you are

The director paused new engine work on 9 November to save weekly capacity. Your last entry, the
same day, is "HI-01 / N44 gesture ownership gated; queue paused at the director's request". Since
then win has landed your whole chain, N47 and HI-01 included, together with the window seat's
screens. `main` and `origin/engine/log` hold all of it (engine/log's top is "Merge main into
engine/log (win: the chain of nine landed)"). No item PR of yours is open (only the standing `engine/log` PR, #421); nothing is pending.
Fetch, cut each new branch from `origin/engine/log`, and merge `origin/main` into engine/log's
successor only if your fetch shows main ahead with a change you need.

The pause is over: the queue runs again under "never stop", from the 4 November issue.

## What to take, in order

1. **The automation undo bug (confirmed, on main today).** `src/adi/automation_lanes.cpp` captures
   and restores a lane without its `automation_segment_ranges` rows: restoring a lane deletes it
   and the foreign-key cascade drops its curve ranges, so Undo of any lane operation loses its
   nonlinear curve shapes. Separately, `clearLane` and the point operations in
   `src/adi/automation_edits.cpp` leave stale `record_index` rows. Neither file mentions either
   table today. Other code already handles `automation_segment_ranges` correctly
   (`automation_clipboard.cpp`, `automation_time_edit.cpp`, `engine/automation_snapshot.cpp`), so
   follow their capture shape. The gate needs tests that fail on main's code: a curved lane survives
   every lane operation's Undo, Redo and Undo byte for byte, and no `record_index` row outlives its
   point.
2. **N45, N46 and N48**, the oldest open needs after N44 in `collab/window.md` ("Needs from other
   lanes"): N45 Save As or Save a Copy over an existing file, as Live (an atomic replace from the
   stage through PD-04's `replaceFile`, never the open file); N46 `window_state` brought to what
   is built; N48 a held peak per strip, cleared by the reader.
3. **HI-02**, the undo cap that holds (PLAN row HI-02). The part inside authority goes now:
   best-effort compaction that reports and never fails a commit, a low-water mark, and checkpoint
   rows by content hash. What compaction may pass is the director's open Q-A; until it is
   answered, every named snapshot and branch point stays reachable, and the daily automatic
   snapshot does not block compaction (PLAN calls today's blocking a defect).
4. **The M0 items** still open in `docs/PLAN.md`, in its order.
5. Then your own queue from the 4 November issue: Phase D, the needs table, LANES §3, phase C.

## Two intermittent failures to investigate (one CI sighting each)

Take these between items or when blocked. Do not mark them flaky and do not raise a timeout to
hide them. Find the cause, or show by a stress loop that it is not in the code.

- `adi_clip_candidate_ready_tests`, "callback admission across 192 distant and crossing page
  requests retains actual native PCM until rendered" (`tests/test_clip_candidate_ready.cpp`,
  around lines 157 to 177): failed once on Ubuntu clang with libstdc++ in 1.52 s, so not its 2 s
  admission deadline. An exact-PCM or underrun mismatch is a possible real race. Run it in a loop,
  ideally under TSAN.
- `adi_plugin_scanner_tests`, "late native block refuses successful metadata publication": failed
  once on the macOS hardened-libc++ leg. Win found and fixed one real race underneath the scanner
  on 11 November (#509): `runProcess` drained the pipe before checking for the child's exit, so a
  child that wrote and exited between the two lost its output. Read #509 first. If this test still
  fails after it, the cause is somewhere else.

CI now runs ctest in parallel (#507). The added load makes races like these hit more often, so
expect more of them to surface.

## One change to your code from outside the lane

PR #507 (CI running the test suites in parallel) wraps `Store::createEmpty`'s schema and seed in
one SQLite transaction in `store.cpp`. It runs the same statements and leaves the same bytes, but
creating a store goes from about 100 journal flushes to one, and a failed create leaves no half
schema. PRAGMAs stay outside the transaction. If it is on main when you fetch, keep it. If your
item touches `createEmpty`, merge main first rather than editing around it.

## Unchanged

Log each item on `engine/log` with its "PR requested" line (head SHA, base, title); item branches
never touch `collab/engine.md`. Win opens and merges the PRs and runs MSVC; you never wait for CI.
