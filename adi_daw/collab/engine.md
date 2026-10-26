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

## 2026-10-26, afternoon — LANES §3 item 1c: the ring grow on a live graph (ADR-0085), a pre-existing race

**PR:** [arieladi/Adi#231](https://github.com/arieladi/Adi/pull/231) (`engine/delayline-grow-race`), draft, stacked on #229 (the `DelayLine` it fixes is the two-width one #229 made). #229's CI was green on 18 of 19 legs when this went up, both MSVC legs included, the macOS universal leg still running.

**What.** The race #229's reviews found. `Graph::accumulate` round-tripped each edge's cursors in every grow state, and the message thread writes two of them when it offers a ring and when it collects one. So an offer landing mid-edge lost its prime count, and the edge swapped at once and played zeros for most of its new delay. Three more races came with it: the state loaded per channel, `beginGlide` reading what the audio thread writes, and escalation reading the capacity before checking for a grow. Now there is one snapshot per edge, and the incoming ring's cursors are written back only in Priming and Fading. `beginGlide` checks a message-thread-only limit. A glide's end keeps a newer request pending; an offer supersedes a stale glide; a glide during a grow waits for the swap. No glide reads past the ring the audio thread holds.

**How it was checked.** A ThreadSanitizer harness (not committed; CI has no TSan leg): 7 distinct races over 194 grows before, 0 after. `adi_retap_tests` 175 checks (+36), deterministic: the interleaving run on one thread at the point a second thread could put it. Seven plants each fail them, and two graph-level plants are caught by TSan. `test_all.sh` 7949 checks across 95 suites. GCC and Clang `-Werror`: clean. A multi-lens adversarial review is running; what survives goes into #231 before it leaves draft.

**Found, not fixed there (pre-existing, item 1d, next):** an edge at zero delay takes `accumulate`'s fast path and never writes its ring (every edge has one at the default headroom of 8192). A later glide to a delay reads the ring as it was when the edge last had one: measured, the glide block's sample 127 is 6592 where the true value lies between 11776 and 11904, audio from about 210 ms earlier. With blocks shorter than the new delay, the block after the glide plays stale samples at full weight. ADR-0079 d2 ("both taps read the same history") is what the fast path broke. A handover adopting history from such a line inherits the same stale or zero history. Fix: the fast path keeps the ring written; a sleeping source's edge zeroes it until it holds only zeros, then costs nothing (ADR-0102 d5's skip stays). It will be benchmarked.

**Next.** 1d, then the plug-in hosts at 64 (CLAP 64-bit ports, VST3 `kSample64` through JUCE), the Pd tier, then LANES item 2 (sends and returns; its design workflow is finishing).

---

## 2026-10-26, later — LANES §3 item 1, second PR: the Processing Precision switch, the graph and the engine's nodes in double (ADR-0207 d2)

**PR:** [arieladi/Adi#229](https://github.com/arieladi/Adi/pull/229) (`engine/precision-64`), stacked on #227 (its two commits are in the branch; the diff narrows when #227 merges). #227 is green on every leg (19 checks, MSVC and both JUCE-on legs included) and ready for review.

**What.** `audio.processingPrecision`, 32 (default, Live's scheme) or 64 (Cubase's), behind the `AudioIo`/`NodeIo` seam: `Node::processesDouble()` (false by default, so a node that says nothing is handed float and converted either side at 64, Cubase's rule for a plug-in that cannot take 64); the graph's buffers, sums, delay lines (either width), suspension, taps and output at both widths; the engine's own nodes in double (sum, gain, junction, strip, input gain, console halves, clip playback, the MIDI source, the placeholder device); `Session::setPrecision` as a rebuild that carries every compensated edge's history across the widths; the registry key; `adi_block_benchmark --precision`. At 32 every float path is the same arithmetic; the suite that pins those bytes is unchanged. Plug-in hosts and the Pd tier stay float: their own PRs.

**How it was checked.** `adi_precision_tests` 92 checks (+57); every 64-bit claim planted and watched failing (16 plants, all caught). Two adversarial review workflows over the diff (lenses: 32-bit byte identity, 64-bit correctness, real-time safety, warnings, test quality, ADR conformance; each finding put to skeptics told to refute it). What they found and this PR fixes: history dropped at a precision switch; `out64` not written by the no-graph path, the fade and `SilenceProcessor`; the gain-stage analysis ignoring the precision; a strip's other-width ramp resuming a stale gain after 32→64→32 (a muted track leaking 5 ms); three of my checks that could have passed with their defect, and a double grow path no test drove; three comments that claimed more than the code. Cost of 64 measured: nothing up to 128 frames; from 256 up, +19 to +68% on the 8-track all-active fixture and +33 to +118% on 64 all-active tracks.

**Found, not fixed here (pre-existing, its own PR next):** `DelayLine::offer` (message thread, during a ring grow on a live graph) writes `inWrite_` and `primeRemaining_`, which the audio thread round-trips through `cursors()`/`setCursors()` in `Graph::accumulate` on every edge; `collectRing` writes `inWrite_` too. A grow that lands between the audio thread's read and its write-back can lose the prime count, and the edge then plays zeros for most of its new delay (about 170 ms at a 9000-sample tap). `run()` also loads the grow state per channel, so one edge's channels can see different states in one call. Fix: load the state once per edge, and have the cursors round-trip only in the states the audio thread owns.

**Next.** That race (item 1c), then the plug-in hosts at 64 (CLAP 64-bit ports, then VST3 `kSample64` through JUCE, proved on the JUCE-on legs), then the Pd tier; then LANES item 2, sends and returns, whose design is being worked out now (questions for win will be in its PR: Live keeps Pre/Post as one switch per return, the format keeps `pre_fader` per routing row).

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
