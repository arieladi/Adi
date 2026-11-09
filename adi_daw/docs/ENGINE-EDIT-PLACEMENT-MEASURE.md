# ED-03 native document measurement

The 100-track headless document path has a large synchronisation cost in this
Debug cloud build, including with the existing raw move producer. This is a
measurement of the engine/document path. ADR-0217 d5's window/drop latency
gate remains open; no raw window producer is retired by this branch.

## Build and fixture

- Measured implementation: `00c0a9aabcd0ec8363fb9e7405a1c3453da00a23`.
- Linux x86-64, GCC 14.2, Debug (`-g`, no optimisation flag), Werror, JUCE off,
  Pd on; AMD EPYC 9V74 host, four-CPU container quota, 32 GiB memory limit.
- 100 material tracks plus Main, 200 initial clips and 100 retained synthetic
  native probe devices. There are 34 fixed-audio, 33 re-pitch Warp and 33 MIDI
  tracks. The PCM source is two seconds at 48 kHz. Both processing widths use
  48 kHz and 256-frame callbacks.
- Each operation moves one or eight clips by the same amount. The raw path
  retains overlapped victims (200 clips); the planner removes one victim per
  moved member (199 or 192 clips). These are the same intent with deliberately
  different row effects, not equivalent transactions.
- One fresh and one primed observation per width/path/member-count case:
  sixteen observations. There is no percentile or steady-state distribution
  claim. Fresh is a newly opened document, not an OS-cold filesystem cache.

Cases run sequentially in one process. The first raw case also initializes
the operation registry; its journal allocation count includes that work.

The executable is `adi_edit_placement_measure`, enabled by
`ADI_BUILD_BENCHMARKS`. Reproduce with a new output directory and final argument
`1`; the optional argument controls primed repetitions and defaults to twelve.
The initial twelve-repetition run was stopped after its first completed raw
observation showed a 25.167-second synchronise phase. That diagnostic was
preserved separately and is not counted in the bounded comparison below.

## Individual observations

Each cell contains one observation. Phase times below are milliseconds;
synchronise and total owner time are seconds. Adoption is milliseconds
relative to owner return, with a negative value meaning it was observed earlier.

| Width | Path | Members | Cache | Prepare ms | Journal ms | View ms | Synchronise s | Owner s | Adoption after return ms |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|
| 32 | raw | 1 | fresh | 0.027 | 3.113 | 2.914 | 23.896 | 23.902 | 4.757 |
| 32 | raw | 1 | primed | 0.030 | 1.728 | 2.888 | 22.444 | 22.448 | 5.774 |
| 32 | raw | 8 | fresh | 0.084 | 2.560 | 3.795 | 26.910 | 26.916 | 7.047 |
| 32 | raw | 8 | primed | 0.082 | 3.332 | 3.419 | 27.208 | 27.215 | 6.065 |
| 32 | prepared | 1 | fresh | 6.115 | 4.818 | 2.896 | 23.029 | 23.042 | 3.817 |
| 32 | prepared | 1 | primed | 6.711 | 7.349 | 3.398 | 22.568 | 22.586 | 7.244 |
| 32 | prepared | 8 | fresh | 38.986 | 26.303 | 3.361 | 22.521 | 22.589 | 6.246 |
| 32 | prepared | 8 | primed | 43.862 | 29.916 | 3.536 | 21.571 | 21.648 | 5.816 |
| 64 | raw | 1 | fresh | 0.025 | 1.651 | 2.761 | 22.492 | 22.497 | 6.739 |
| 64 | raw | 1 | primed | 0.029 | 1.923 | 2.795 | 21.684 | 21.689 | 7.760 |
| 64 | raw | 8 | fresh | 0.090 | 3.788 | 3.252 | 24.297 | 24.304 | 6.581 |
| 64 | raw | 8 | primed | 0.078 | 2.244 | 2.898 | 22.886 | 22.891 | 5.857 |
| 64 | prepared | 1 | fresh | 8.369 | 7.369 | 3.241 | 25.119 | 25.138 | 4.254 |
| 64 | prepared | 1 | primed | 7.680 | 6.180 | 3.148 | 23.465 | 23.482 | 8.021 |
| 64 | prepared | 8 | fresh | 40.898 | 26.750 | 3.322 | 22.864 | 22.935 | 4.189 |
| 64 | prepared | 8 | primed | 40.727 | 30.304 | 4.008 | 22.136 | 22.211 | 8.233 |

Callback execution and new-generation source observations:

| Width | Path | Members | Cache | Calls over budget / blocks | Percent | Maximum ms | Missing source frames |
|---|---|---:|---|---:|---:|---:|---:|
| 32 | raw | 1 | fresh | 309 / 4484 | 6.89% | 13.580 | 33280 |
| 32 | raw | 1 | primed | 132 / 4211 | 3.13% | 42.727 | 17152 |
| 32 | raw | 8 | fresh | 800 / 5049 | 15.84% | 26.796 | 17152 |
| 32 | raw | 8 | primed | 273 / 5105 | 5.35% | 33.814 | 17152 |
| 32 | prepared | 1 | fresh | 289 / 4322 | 6.69% | 20.174 | 17152 |
| 32 | prepared | 1 | primed | 105 / 4237 | 2.48% | 11.458 | 17152 |
| 32 | prepared | 8 | fresh | 205 / 4238 | 4.84% | 15.247 | 17152 |
| 32 | prepared | 8 | primed | 124 / 4061 | 3.05% | 29.352 | 17152 |
| 64 | raw | 1 | fresh | 35 / 4220 | 0.83% | 12.127 | 17152 |
| 64 | raw | 1 | primed | 29 / 4069 | 0.71% | 11.453 | 17152 |
| 64 | raw | 8 | fresh | 107 / 4559 | 2.35% | 14.883 | 17152 |
| 64 | raw | 8 | primed | 43 / 4294 | 1.00% | 11.966 | 17152 |
| 64 | prepared | 1 | fresh | 127 / 4715 | 2.69% | 11.638 | 17152 |
| 64 | prepared | 1 | primed | 68 / 4405 | 1.54% | 17.817 | 17152 |
| 64 | prepared | 8 | fresh | 61 / 4302 | 1.42% | 10.168 | 17152 |
| 64 | prepared | 8 | primed | 56 / 4167 | 1.34% | 29.309 | 17152 |

All sixteen edits retain all 100 devices with zero loader calls, publish and
rebuild once, and measure zero ordinary callback allocations and callback file
I/O. All source/decode problem lists and new-generation read-error counts are
empty/zero. The source-underrun and callback execution results above remain
separate limitations; device retention alone does not establish responsiveness.

[Raw observations and build/source provenance](measurements/edit-placement-2026-11-09.json)
retain unrounded times, allocation counts and every observation. The measured
code commit precedes this documentation-only report commit.

## What the measurements mean

Preparation includes owner capture, interval resolution and row planning.
The journal interval also includes entry into the submitter/helper; view time
includes its refresh and result handling. Synchronise covers the complete
`ProjectDocument::synchronise` call: source/model refresh, Session preparation,
graph rebuild/publication and associated attachment work. Its internal costs
have not been separated by profiling. Both paths publish exactly one graph;
the raw op's Snapshot descriptor does not avoid that call in this document
path. Device-instance retention is measured separately from graph publication.

Adoption is observed at the end of the callback that reports a graph swap,
not at the precise publication or block-entry instant. A callback may adopt
before synchronise returns. The audibility field means the first nonzero
aggregate output after that swap; unchanged sources or retained device state
can provide it. Edited-source readiness is **not measured**.

Priming applies to the previous source generation. Session rebuild constructs
a fresh ClipPlayback generation on both paths. The reported new-generation
underrun count sums missing source frames across clips; it is not a count of
distinct output frames or hardware xruns. It covers the short observation
window through adoption/aggregate audibility, not steady-state playback.

Callback execution exceedances count process calls longer than 5.333 ms. The
driver does not measure missed start deadlines or hardware xruns. Counts must
be read with their callback-block denominators: the driver runs for different
durations while the owner performs each edit. Clock reads bracket processing
outside CallbackScope. Ordinary new/new[] and callback file-I/O counters are
reported; aligned allocation and direct C allocation are outside that probe.

Setup and Undo/reset are outside timed intervals. This follows the supported
document lifecycle but does not force collection of every retired setup
generation before a sample; precision/setup cases can retain different old
source generations. No other task compilation or test ran during the bounded
measurement. The cloud host itself is not a controlled hardware laboratory.
The probes do not represent real VST3/CLAP DSP load, and hardware, actual
window-driven file drop, Release-build performance and Live parity are NOT RUN.

## Static profiling lead

Source inspection identifies substantial repeated work, without assigning a
measured share of synchronise time to it. `Session::rebuild` constructs a new
whole-project `ClipPlayback`. Each Warp clip calls `audio::prepareWarp` over
its complete source, with a fresh output file and no prepared-result reuse
lookup. Re-Pitch uses the best-quality sinc resampler even at a 1:1 ratio.
The raw fixture therefore prepares 66 separate two-second Warp renders per
rebuild: 6,336,000 stereo output frames. Priming the previous generation does
not remove this work. Prepared placements can remove some victim clips, so
their exact render count differs.

The next performance investigation should time playback/source capture, Warp
preparation, graph preparation/publication and final source reclamation
separately, then compare unchanged sources with an actual source/tempo/Warp
change. Prepared-asset reuse and retaining unaffected source generations are
candidates to evaluate with those measurements. Neither optimization is
implemented or validated by this report.
