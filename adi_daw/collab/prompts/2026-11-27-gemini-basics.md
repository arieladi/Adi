# gemini — the basics, 27 November 2026

This comes before your standing queue (`collab/prompts/2026-11-22-gemini-queue.md`), whose rules still
hold. Read `collab/prompts/2026-11-27-basics-howto.md` to see how the six sessions work.

**What is happening.** Six sessions fix the basics of `docs/SANITY-CHECK.md` this weekend, each on
branches named `basics/<sc id>-<short name>`, with a report in `collab/basics/<name>.md` on
`basics/<name>-log`. They build on Windows or in the cloud. You are the only seat that runs their work
on the older compilers and under the sanitizers before it lands.

## Order, until your limit

1. **Every `basics/*` branch as it appears, first every time.** Fetch before each item. For each new
   head: GCC 12 and Clang 14 with `-DADI_WERROR=ON`, Pd on, JUCE off, the suites it touches and
   `ctest -L acceptance`; the touched JUCE files compiled with JUCE on; ASan and UBSan on the touched
   suites. Log one table per round. Win lands a branch only after your line on it, unless you are at
   your limit.
2. **Each landing stack of win's (`land/*`)**, the same way, as you did for `land/walk-1126e`.
3. **DF-E89, on loan to you.** Your Release sweep found 54 units that GCC 12 refuses at `-O3` with
   warnings as errors. Fix them: the mismatched new and delete in the test files (start with
   `tests/replace_operator_new.hpp`), the `-Warray-bounds` seven, the two `-Wmaybe-uninitialized` in
   `src/adi/clip_commands.cpp` and `src/adi/return_send_tap.cpp`, and `src/adi/audio/mp4_aac.cpp`. One
   branch, `gemini/df-e89-release-warnings`, behaviour unchanged, proved by the full headless suite on
   both compilers in Debug and Release. These files belong to several seats: the loan covers these
   warnings only.
4. **A Release leg for CI.** Once item 3 is green, say in your log exactly which configure line would
   make one CI leg build Release with warnings as errors on GCC, so win can add it.
5. **Then the waiting branches** as before (your item 4 of 25 November), and your standing queue.
