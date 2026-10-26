# engine — log

The engine lane (ADR-0207, `docs/LANES.md` §3): Claude Max, Opus 5.5. This session runs in a
Claude Code cloud container on Linux (GCC and Clang; no MSVC here, so `/WX` is win's leg), one
branch per item cut from `origin/main`, `engine/<topic>`. Only the engine lane writes to this file.
Newest entry at the top.

**Why this log has its own branch and PR (`engine/log`).** Every item PR prepending an entry here
would make each merge conflict the next open PR on this file, the failure `collab/README.md` rule
3 exists to prevent. So the item PRs never touch this file; this one does, and win merges it
whenever convenient. Each entry names its PR, so nothing is lost if this merges late.

---

## 2026-10-26 — LANES §3 item 1, first PR: 64-bit summing at every mix point (ADR-0207 d2)

**PR:** [arieladi/Adi#227](https://github.com/arieladi/Adi/pull/227) (`engine/double-summing`), ready for win's review.

**What.** Live processes in 32-bit float and sums every mix point in 64-bit (Audio Fact Sheet
38.2.4); ADI's graph summed in float. Now a node's main and sidechain sums (`Graph::mix`), the
overlapping clips of one track and a clip's mono fold accumulate in double and convert to float
once. A mix point with at most one live input keeps the float path, which gives the same bytes,
so chains and the sleeping tracks of ADR-0102 d5 pay nothing; `GraphStats::wideMixes` counts the
double sums.

**The test.** `adi_precision_tests` (new suite, 33 checks): ADR-0205 d6's statement of the Fact
Sheet case (pairs of 24-bit files and their inverted copies, summed many times, cancel to zero)
at every mix point, each beside the same sum in float, which must not cancel: 64 tracks at the
master; inside a group; through compensation delay lines; on the sidechain bus; through a real
project's 24-bit WAV clip tracks, with and without per-pair volume and pan; 16 overlapping clips on
one track; a six-channel fold. On `main` 11 of the then 24 checks fail (residue 2.26e-6 at the
master, about −113 dBFS); three plants (graph sums float, clip sum float, fold float) each fail.

**Numbers.** `test_all.sh` 7853 checks across 95 suites (main 7820 across 94). GCC and Clang
`-Werror` with the hardened library: clean. The benchmark cost is in the PR, measured pinned to one core (unpinned runs here swing ±35%).

**For win.** The Live 12 manual text is not in a cloud checkout (`reference/` is not fetched by
`--build-only`), so the test follows ADR-0205 d6's description of 38.2.4; worth one look against
the manual's own wording. `docs/FEATURES.md` row 85 still says "the graph sums in float today".

**Next.** LANES item 1, second part: the 64-bit end-to-end mode behind the Processing Precision
switch (`audio.processingPrecision`, 32 by default), starting with the seam and the graph.
