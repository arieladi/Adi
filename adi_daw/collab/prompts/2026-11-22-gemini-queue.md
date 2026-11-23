# gemini — standing queue, 22 November 2026

This is your whole queue: work it until your usage limit. It replaces the trial list of 22 November.

## How to work

- You are a permanent seat, the verification seat (ADR-0252). You have no source paths of your own. Most of this
  queue is run-only: build, run, measure, report. You change code only on a loan that win names in
  `collab/README.md` and in the PLAN rows of the section below, on each row's named paths (the trial's loans for
  DF-E53 and DF-E56 ended when #713 landed), and you never push to another seat's branch.
- Fetch origin first, and before each item. Read the top entry of `collab/win.md` on main and every entry addressed
  to you in the other seats' logs, above all the engine's reviews of your two branches in `collab/engine.md` on
  `engine/log`. Changes requested on your own branches come before any item below (item 1 is the one open today).
- Work the items top to bottom. A code change gets one branch per item (`gemini/<topic>`), cut from origin/main
  unless the item says to stack, test-first where the item has a test.
- After each item: push any branch, write the entry in `collab/gemini.md` on `gemini/log` (newest first), end it with
  "PR requested" if there is a branch, and start the next item in the same turn. Run-only items end with their
  result table and no PR line.
- A finding in someone else's code goes in your log with the file:line, the compiler or tool, a smallest repro and
  the owner from `collab/README.md`. Win registers it as a DF row. You do not fix it.
- An item blocked on win, another seat or a decision gets one line in your log and you take the next. A question for
  the director goes under "FOR THE DIRECTOR" at the top of your log with your recommendation, then you continue.
- When this list is empty: repeat item 2 for every new "PR requested" line, and rerun items 3 and 10 to 13 on the new
  main. Each round, after your verification items, take the next slice of the rows under "PLAN rows (after 3
  December, ADR-0252)", in their order; never a row of the two weeks before 3 December. Never stop because
  "nothing names this seat". Stop only at your usage limit or a broken environment.

## Queue

Before 3 December (ADR-0248 d2):

1. **DF-E56-ATTACK: a refused attack leaves the sounding voice alone** (on the `mpe_output.cpp` / `.hpp` loan,
   `collab/README.md`)
   - The engine's follow-up review of 22 November (`collab/engine.md` on `engine/log`, "Gemini follow-up reviews
     for DF-E53 and DF-E56") passes da6d1629's 126 router checks but still reproduces the refused-attack case: with
     one allowed member channel and three free slots, a second note's resets move the first voice's bend from 10240
     to 8192 while its own attack is refused. Check room for the whole reset-and-attack sequence before emitting any
     of it.
   - Add the commit on `gemini/df-e56-mpe-router-overflow` (test first in `tests/test_mpe_output.cpp`), push, and log
     the new head with "PR requested": the engine's MI08-MPE item rebases over it.
   - Proof: the router suite passes on GCC 12 and Clang 14 with `-DADI_WERROR=ON`; the new case fails on da6d1629.
2. **PR-BUILDS: every requested branch on GCC 12 and Clang 14 -Werror**
   - Collect the "PR requested" lines in every seat's log (`engine/log`, `engine-win/log`, `devices/log`,
     `window/log`, `window-fn/log`, `mixview/log`, `clones/log`) and win's `land/<seat>-<date>` stacks.
   - Build each head with `-DADI_WERROR=ON` (Pd on, JUCE off) under GCC 12.2 and Clang 14, and run its ctest.
   - Proof: one log table per round: branch, SHA, compiler, warnings (file:line), ctest result, owner. Do it before
     win's landing windows. Patch DF-M62 locally only to see past it, and say so.
3. **WTS-01 on Linux**
   - Build with `-DADI_ACCEPTANCE=ON`, run `ctest -L acceptance` on main and on your two branches (03be0b99, and
     the DF-E56 head after item 1), GCC 12 and Clang 14.
   - Proof: the step table matches win's of 20 November (2b the only FAIL until DF-E64 lands). Any other difference
     goes in your log with a repro.
4. **PD64: full ctest with the Pd tier in double**
   - Configure with `-DADI_PD_FLOATSIZE_64=ON` (the devices lane asked for this leg), check that the configure log
     says `Pd tier: PD_FLOATSIZE=64`, and run the whole ctest on main, then on `land/devices-1122`.
   - Proof: pass/fail per suite at 32 and at 64 on GCC 12 and Clang 14; each failure with its owner.
5. **BENCH: Release empty-project cost, for the engine's empty-project-cost item**
   - Release build with `ADI_BUILD_BENCHMARKS=ON`; run `adi_block_benchmark --self-test`, then the empty project at
     64 and 128 frames, with the breakdown.
   - Proof: p50, p99 and max per callback and the share of the deadline, on main; rerun on the engine's branch once
     it logs one. Report it for the engine.
6. **RT-TIMING: adi_builtin_realtime_tests on devices/perf-pass**
   - Build `devices/perf-pass` in Debug and in Release; run `adi_builtin_realtime_tests` with its timing.
   - Proof: wall time and pass/fail of each build against the 900 s CTest TIMEOUT, for the devices lane's
     perf-debug-timeout item. Report it for the devices lane.
7. **DF-E10: 500 parallel repeats of the Link tests**
   - `ctest -R adi_link_session_tests --repeat until-fail:500 -j 8` beside other suites (the RESOURCE_LOCK is on main), under CPU load.
   - Proof: 500 passes, or the failure with its output. Report it for the engine; the diagnostic change is the
     engine's (DF-E10-diag), not yours.
8. **DF-M62 confirmation build** (blocked: the Mix View seat has not pushed its fix; `FloatRealTransform.h` line 61
   is unchanged on `mixview/cumulative-20261122`; skip to the next)
   - When `mixview.md` logs the fix: build that tip on GCC 12.2 with `-DADI_WERROR=ON`, full tree.
   - Proof: the limiter TU and the full tree build clean; limiter and PFFFT suites unchanged. Report it for the Mix
     View seat.

After 3 December, or any time once the items above are clear:

9. **DF-E12: find the cause of the flaky `Store::create` of Cached.adi**
   - Repeat `adi_project_files_tests` under `ctest -j` and CPU load until it fails; the StoreError now prints
     (678f2e32).
   - Proof: a failure logged with its StoreError and conditions, or N clean repeats with the load recorded.
10. **WERROR-SWEEP: full tree on the older toolchains**
   - Full tree with `-DADI_WERROR=ON` under GCC 12.2 and Clang 14 (Pd on, JUCE off), DF-M62 patched locally.
   - Proof: a table of every warning-as-error beyond DF-M62 with its owner, or a clean build.
11. **ASAN: every headless suite under ASan and UBSan**
    - `-fsanitize=address,undefined` on GCC 12 and Clang 14 (CI runs these only on the fuzz job).
    - Proof: suites run, pass/fail, each report with stack, smallest repro and owner.
12. **TSAN: the threaded suites under TSan**
    - Clang 14 `-fsanitize=thread`: graph workers, host, gesture, recording, decode, scanner, Link.
    - Proof: each race with both stacks, a repro and its owner, or a clean run.
13. **GLIBCXX-DEBUG: full headless ctest with `-D_GLIBCXX_DEBUG`**
    - Proof: every debug-mode abort with a repro and owner, or all suites green.
14. **FUZZ: the two fuzzers for hours**
    - `-DADI_BUILD_FUZZERS=ON` with Clang 14; run `adi_fuzz_blob` and `adi_fuzz_textproj` on the seed corpus. If
      libFuzzer is missing, replay the corpus under ASan.
    - Proof: CPU-hours, executions, coverage, and each crash minimised with its owner, or none.
15. **REGISTER-AUDIT: a second check of the engine seat's landed DF-E rows**
    - For DF-E1, E3, E4, E7, E9, E13, E14, E15, E18, E26 (a, b), E27 (a-d), E29 (a-d), E48, E49: does main already
      meet the row, with which commit and test, and what remains. The engine claims these landed whole; you confirm
      or refute each.
    - Leave out DF-E45, E47, E51, E52 and E54 (the engine's own landed-parts item checks their sub-parts), DF-E16
      (its window half is open), DF-E17 (unlanded), DF-E20 (its parts are other seats'), the engine-win rows (it
      audits its own) and DF-E5, DF-E11, DF-E12's diagnostic part (win already has them as done).
    - Proof: one table in `gemini.md` (row, commit, test, residual) that win can move to Done in one PR.

## PLAN rows (after 3 December, ADR-0252)

The director's answer of 23 November: you may take engine PLAN rows that land after 3 December. Each row below is
reserved for you in `collab/RESERVATIONS.md` and lent by the engine for its named paths only. Work them in this order,
one branch per slice (`gemini/<topic>`, cut from origin/main unless it says to stack), test first. A change you need in
any other file (the store, `row_ids.*`, `device_copy.*`, the digest, the settings registry, the network stack) is asked
of its owner in your log; you do not make it. Its `docs/PLAN.md` row is the contract: read it, and the ADRs it names,
before the first slice.

For every row: **PR requested only after the engine reviews it; lands after 3 December.** Ask for the review in your
log ("Review requested", with the branch and head); write "PR requested" only after the engine's review in
`collab/engine.md` passes that head. Build each head on GCC 12 and Clang 14 with `-DADI_WERROR=ON`, under ASan and
UBSan, and log the result.

1. **SRF-09: OSC, the core** (P2; ADR-0185 d1, d4; ADR-0188 d7; ADR-0181 d2)
   - Paths (loan): `src/adi/osc_codec.hpp` / `.cpp` (the decoder: OSC 1.0 messages and bundles, 4-byte alignment,
     the int32, float32, string and blob arguments, bundle time tags, a cap on bundle depth and packet size;
     self-contained, so the fuzz target builds from it alone), `src/adi/osc_namespace.hpp` / `.cpp` (`/adi/transport`,
     `/adi/track/{n}`, `/adi/master`, `/adi/focused` resolved to typed writes on an interface, each stamped
     `osc:<client>`; the client list; the no-echo rule), `tests/test_osc_codec.cpp`, `tests/test_osc_namespace.cpp`,
     `tests/fuzz_osc_codec.cpp`, and their lines in `CMakeLists.txt`.
   - Not yours: the UDP read loop and the encoding (the network stack, `src/juce/net_*`, the engine's), the settings
     rows (off by default, the interface, the client list: engine-win's registry), the host integration on the control
     feed (it waits on CTL-06).
   - Test it must add: a truncated, misaligned or oversized packet, and a bundle nested past the cap, are refused
     without reading past the buffer; each namespace address resolves to its typed write, an unknown one is refused;
     every write carries `osc:<client>`; a client not on the list is refused; a value that arrived in a flush is not
     echoed in that flush's feedback, and a later change is; the fuzz harness replays its seeds clean under ASan and
     UBSan.
   - PR requested only after the engine reviews it; lands after 3 December.
2. **SRF-10: OSCQuery v1, the core** (P2; ADR-0185 d4; ADR-0188 d7; stack on SRF-09)
   - Paths (loan): `src/adi/oscquery.hpp` / `.cpp` (the tree built from a namespace description with stable ids,
     FULL_PATH, float TYPE, RANGE 0 to 1 and ACCESS 3; HOST_INFO naming OSC over UDP and its port; GET only: `/`, a
     path, `?HOST_INFO`, as a pure function from a request to a response), `tests/test_oscquery.cpp`,
     `tests/fuzz_oscquery.cpp` (the request parser; a fuzz target compiles `src/adi/oscquery.cpp` alone, so it
     builds without the codec), and their lines in `CMakeLists.txt`.
   - Not yours: the HTTP listener and DNS-SD (the network stack, the engine's), version 2 (LISTEN and the rest, which
     waits for a transport ruling), FEATURES' wording (win's).
   - Test it must add: the tree of a fixture with two tracks and Main equals its golden JSON; ids stay stable when a
     track is inserted before them; `?HOST_INFO` names the transport and port; any method but GET, a malformed or
     oversized request, and an unknown path are refused with their status; no LISTEN key; the fuzz harness replays
     clean under ASan and UBSan.
   - PR requested only after the engine reviews it; lands after 3 December.
3. **PD-15: paste between projects, the engine half** (P2; ADR-0068; ADR-0111)
   - Paths (loan): `src/adi/project_paste.hpp` / `.cpp` (a track with its devices, clips, automation and media pasted
     from a second Store opened read-only into the active Store as one transaction; ids remapped through the one
     allocator; media by hash, copied once), `tests/test_project_paste.cpp`, and its lines in `CMakeLists.txt`. It
     calls `row_ids.*`, `device_copy.*` and the publication API through their public functions and edits none of them.
   - Not yours: the project tabs, Live's import of tracks through the browser, the historical tab (the window's;
     HI-04).
   - Test it must add (PLAN PD-15): a track with a device and media pasted from another project has every id
     remapped and is one undo step; undo returns the start digest; the source file's bytes are unchanged; a media
     file already present by hash is not copied again; a paste into a read-only project is refused; two Stores open at
     once run clean under ASan and TSan.
   - PR requested only after the engine reviews it; lands after 3 December.
4. **CO-01: the collaboration core** (P2; ADR-0182 d1, d2, d5; ADR-0188 d5)
   - Paths (loan): `src/adi/collab/**` (new: the batch with author, id space, base seq, vector and typed ops; apply
     in seq order through a remote commit; rebase by rewinding the pending suffix and committing it again; fractional
     sibling keys; an in-memory fake object store with put-if-absent, delays and drops), `tests/test_collab_*.cpp`,
     and their lines in `CMakeLists.txt`.
   - Not yours: the remote-commit seam in the store and history, the id space in `row_ids.*` and the digest's
     machine-local columns (ask the engine in your log); the SQLite session extension (win's build change); the
     bucket client and S3 (CO-05); undo across collaborators and presence (CO-02, CO-04, after Q-N1 and Q-N2). A
     schema minor, if a slice needs one, is reserved in `collab/RESERVATIONS.md` before the branch uses it.
   - Test it must add (PLAN CO-01): three Stores and the fake object store over 200 seeds with delays and drops
     converge to equal digests with nothing pending; the fractional-key vectors; two replicas inserting a device at
     one slot converge; the seeded run is clean under ASan and TSan.
   - PR requested only after the engine reviews it; lands after 3 December.

## Waiting on win

- Nothing. `gemini/df-e53-canonical-param-ref` (03be0b99) and `gemini/df-e56-mpe-router-overflow` (da6d1629, then
  the refused-attack commit 0b539fb2) landed on main in #713 on 23 November.

## Needs from other seats

- The engine: a review of item 1's commit in `engine.md`; its MI-08 branches rebase over your new DF-E56 head.
- The Mix View seat: its DF-M62 fix pushed, for item 8.
- Win: the MSVC /W4 /WX and macOS legs of your branches (you cannot run them), and the DF rows for your findings.
- Win: any code row for this seat comes only as a loan named in `collab/README.md` or as a PLAN row reserved for it
  in `collab/RESERVATIONS.md` (ADR-0252). DF-C5 (a second EQ or limiter
  refused on a channel) has no seat to own it; win may lend it only after #677 lands.

## For the director

Both answered by the director on 23 November, "yes to both" (ADR-0252):

- May the gemini seat take after-3-December engine PLAN rows on branches that land after 3 December (for example
  TM-SV-01)? **Yes** (ADR-0252 d4): on loan from the engine for each row's paths, reviewed by the engine, landing
  after 3 December. TM-SV-01 is in the engine's queue, so win picked four others (the section above, d6).
- Does gemini become a permanent seat with paths of its own (for example the older-toolchain and sanitizer legs), or
  stay on loans only? **Permanent** (ADR-0252 d1 to d3): the verification seat, with no source paths of its own;
  code only on loans and in its PLAN rows; it registers findings for win and does not fix other seats' findings.
