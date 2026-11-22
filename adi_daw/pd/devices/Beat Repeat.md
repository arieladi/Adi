# Beat Repeat — the contract (DEV-22, ADR-0249 d7)

Live 12's Beat Repeat, ADI's first production milestone after 3 December
(ADR-0249 d7), built by the devices lane in its own trees, UID
`org.adidaw.builtin.beat-repeat`, one pull request. It comes after Live's
Compressor, Glue Compressor, Gate and Limiter. It is **built, not verified**
until a Live 12.4 render agrees (ADR-0108): no parity claim rests on a
synthetic oracle. This file is the contract the build follows; what Live does
where this file says "reading" is settled by reading 18 on the devices log's
ONE list before the code depends on it.

## What it is

A repeat effect over its own INPUT. It keeps a bounded stereo history of what
comes in, never of what it puts out (it is not a feedback delay), and at each
Interval, offset by Offset, with probability Chance, it captures a slice of
Grid length and plays it back for Gate, repeating it, its pitch and level
decaying as Pitch Decay and Decay say, through an optional band filter, mixed
with the input as Mix mode says. Variation changes Grid as it repeats.

## The controls (ids fixed now; names, ranges and defaults from reading 18)

The ids below are this device's for good: a saved set refers to them. The
units and choices are Live 12's as its manual names them; every range and
default is provisional until reading 18 ("every control as shown") and the
tests pin Live's values once they are read.

| id | name | unit / choices | provisional default |
|---|---|---|---|
| 1 | Interval | 1/32, 1/16, 1/8, 1/4, 1/2, 1 Bar, 2 Bars, 4 Bars | 1 Bar |
| 2 | Offset | 0 to 15 sixteenths | 0 |
| 3 | Grid | 1/32, 1/16, 1/8, 1/4, 1/2, 1 Bar, 2 Bars, 4 Bars | 1/16 |
| 4 | No Triplets | off / on | off |
| 5 | Variation | 0 to 10 | 0 |
| 6 | Variation Mode | Trigger, 1/4, 1/8, 1/16, Auto | Trigger |
| 7 | Chance | 0 to 100 % | 100 % |
| 8 | Gate | 1/32 to 4 Bars (sixteenths) | 1 Bar |
| 9 | Repeat | off / on (forces a repeat while held) | off |
| 10 | Mix Mode | Mix, Insert, Gate | Mix |
| 11 | Volume | dB | 0 dB |
| 12 | Decay | 0 to 100 % | 0 % |
| 13 | Pitch | -48 to 0 st | 0 |
| 14 | Pitch Decay | 0 to 100 % | 0 % |
| 15 | Filter | off / on | on |
| 16 | Filter Freq | Hz | 1 kHz |
| 17 | Filter Width | octaves | 9 |

## The core (`dsp/beat_repeat.*`)

- **Input history.** A bounded stereo ring of the input, allocated in
  `prepare()` for the longest history any setting can read at the prepared
  rate: the longest Interval plus Offset plus Grid at the slowest tempo the
  host allows (20 BPM), as reading 18 states the oldest frame Live reads.
  Nothing allocates in `process()`.
- **The read head.** Reads the captured slice at a fractional position;
  Pitch and Pitch Decay move it at a ratio (a resampling read, interpolated),
  never by resampling the stored history.
- **The slice clock.** Interval, Offset, Grid and Gate are musical lengths.
  The core reads the song position of each segment (the native adapter's
  position path, `PositionHandler`, as ADI-Shaper's sync uses; the
  `[adi.transport]` canvas reads each Pd tick's exact position since the
  20 November work) and places every capture, slice wrap and Gate end at its
  exact frame. A slice of non-integer length carries or rounds its fractional
  phase as reading 18 says Live does.
- **Mix modes.** Mix: the input plus the repeat. Insert: the repeat replaces
  the input while it plays, the input otherwise. Gate: the repeat only.
- **No latency of its own** unless reading 18 says Live reports one; whatever
  it reports is kept while the device is bypassed (ADR-0095).
- **Locate.** What a locate does to the history and to a repeat in progress
  (kept, cleared, or kept with the pre-locate frames ineligible) is reading
  18's; the core does that and the tests hold it.

## The test method (the clones seat's Repeat Cell method)

- **Markers.** Feed independently seeded, uncorrelated left and right marker
  signals: every frame of each channel is unique, so any output frame names
  the input frame and channel it came from.
- **The frame oracle.** Keep an exact oracle outside the processor that
  states, for every output frame on each channel, the input frame expected
  there at each capture, wrap and return to the input. Compare every frame.
- **Rates and blocks.** 44.1, 48 and 96 kHz; whole 64-frame Pd ticks and
  irregular host blocks; the buffered Pd adapter's measured 64-frame
  displacement aligned before comparing the canvas with the native core.
- **Off-tick events.** Capture, Grid and release events at frame offsets 1,
  17 and 63 inside a 64-frame tick, each absolute event position fixed across
  both block arrangements; the first affected frame must be the event's own
  frame (the native path; the canvas's off-tick response is prospective).

## The tests (ADR-0249 d7)

- Live's parameter names, ranges and defaults, with the fixed ids above
  (once reading 18 gives them).
- No allocation in `process()`; no adapter delay; the latency kept while
  bypassed.
- Every output frame against the frame oracle, at the three rates, with
  irregular blocks, and with events off the 64-frame ticks.
- Chance exact at 0 % (never repeats) and 100 % (repeats every Interval).
- A locate, and a loop wrap, under the effect.
- Save and reopen: the same output.
- The native core equal to the canvas.
- Plants that prove the tests can fail; the strict warning legs.

## Reading 18 (the ONE list), with the clones seat's precision

Placed on the devices log's ONE list for the director's session. Each item is
taken with distinct markers so frames can be told apart:

- every control as shown, on a fresh Beat Repeat at 48 kHz;
- **capture direction:** markers immediately before and after the capture
  point: does the slice start at the trigger frame or end there, and does the
  trigger frame belong to the slice?
- **Grid changed while repeating,** at a marked frame: the first slice it
  affects and where playback resumes;
- **fractional phase:** at a tempo that makes a non-integer slice (117 BPM,
  Grid 1/16: 6,153.846 samples), the first frame of each of 16 successive
  wraps, to tell a carried phase from per-slice rounding;
- **the join:** across a wrap between two marked levels, the transition's
  length and weights;
- **locate:** while repeating, including a jump back to the same musical
  position: which pre-locate frames it can still play;
- **the oldest frame:** at 20 BPM with the longest Interval and Grid, the
  oldest input frame it plays, in frames and seconds, at 44.1, 48 and 96 kHz.

## One history kernel with ADI-Shaper's Time stage? (the clones seat's question)

`AdiShaperTime` (on `clones/shaper-time`) is a fixed 2²¹-frame stereo ring
allocated in `prepare()`, read at a fractional delay: 43.7 s at 48 kHz and
21.8 s at 96 kHz. Beat Repeat at 20 BPM with a 4-bar reach needs about 48 s,
so a fixed capacity cannot serve both. **The answer: yes to one kernel, with
the capacity the caller's, set in `prepare()`** (a stereo input ring, its
valid count, and an interpolated read at a fractional age), each caller
keeping its own slice boundaries, playback phase and storage, **provided**
reading 18 shows Beat Repeat's write source (the input, before the effect),
its retention across a locate and its oldest readable frame agree with Time's.
If the locate behaviour differs, the kernel stays shared and the locate policy
stays with each caller. The kernel would live in `dsp/` under the devices
lane, with the clones seat reviewing the hunk Time takes from it.

## The panel

From a sheet the director agrees first (ADR-0243, ADR-0244), by the window
lane; not before.
