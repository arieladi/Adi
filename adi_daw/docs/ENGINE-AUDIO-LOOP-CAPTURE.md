# CT-02 bounded audio loop-pass capture

The hardware-input recorder captures one continuous file per target and a pass
log through Session. The owner prepares it with `recording::prepareLoopCommit`
and publishes it with `recording::commit`. `prepareCommit` is the linear entry
point and refuses loop results before staging files or creating project rows.

## API and intervals

`AudioRecordingSpec::loop` is absent by default, preserving linear capture and
its existing loop-discontinuity refusal. Opt in with
`AudioRecordingLoop{beginSample,endSample,maxPasses}`. Bounds are nonnegative,
half-open and fixed in session samples at the declared recording rate. Capacity
is 1–65,536 nonempty passes (default 1,024). The owner allocates that many POD
records before publishing the job; the maximum log is 2 MiB on the supported
64-bit builds. Session still supplies project identity and validates active
device format and armed track destinations. It does not read or mutate the
driver's transport from the owner thread; the callback validates the requested
loop when actual capture begins.

After callback quiescence and writer finalization, `AudioRecordingResult`
exposes the frozen `loopSpec()` and read-only `passes()`. Each pass records:

| Field | Meaning |
|---|---|
| `sourceStartFrame` | Exact first frame in every target's continuous source file |
| `frames` | Number of accepted frames in this pass |
| `timelineStartSample` | Actual first timeline sample, including a partial first pass |
| `timelineEndSample` | Exclusive timeline end; equals start plus frames |

The loop-relative offset is `timelineStartSample - loopSpec().beginSample`.
Pre-loop input is retained with its real placement and a negative offset; this
does not choose where later take publication puts pre-roll. After a partial first
pass, source offsets are cumulative actual durations, not an assumed `0,L,2L`.
For example, loop `[100,132)` starting at 108 and capturing 112 frames yields:

| Pass | Source frames | Timeline interval |
|---|---|---|
| 1 | `[0,24)` | `[108,132)` |
| 2 | `[24,56)` | `[100,132)` |
| 3 | `[56,88)` | `[100,132)` |
| 4 | `[88,112)` | `[100,124)` |

One source WAV is owned per recording target, independent of the number of
passes; every target uses the same pass intervals. Existing shared-input mapping,
writer, ring and file ownership remain in use. There is no new FIFO or callback
file I/O. The result's frame count and log describe accepted input; an I/O-failed
result does not promise that all accepted frames reached disk.

## Callback policy

The recorder splits with the transport's actual `sampleAt`/`contiguousFrames`
projection. A variable callback can contain any number of loop wraps up to its
bounded frame count. Adjacent callback fragments in the same pass are coalesced.
Ending exactly at loop end creates no zero-length next pass; incomplete last
passes are retained. Count-in input is skipped by the existing Session slicing,
and the first actual playing segment establishes the transport revision.

Every later captured segment must have the expected stream timestamp, expected
wrapped timeline position, unchanged transport revision and exactly the frozen
enabled loop bounds. A seek to even the same position, stop/restart between
callbacks, explicit loop reconfiguration, or musical retiming to different
sample bounds ends the job with `Discontinuity`. An ordinary observed transport
stop completes its accepted partial pass. A zero-frame callback adds nothing.

Before changing samples or pass records, the callback checks the entire block's
pass capacity and existing ring capacity. `PassLimit` rejects the entire block,
including any initial portion that would fit; earlier accepted metadata and
audio remain aligned. Queue overflow likewise leaves the pass log unchanged.
There is no callback vector growth, allocation, file access or mutex; existing
lock-free publication and ring atomics retain the single-owner/single-driver
lifetime protocol. The owner copies the used log only after acknowledgment.

## Publishing completed takes

The owner supplies a media ID per target, a new clip ID per pass, and a fresh
fallback lane ID per pass. Existing same-track take lanes are considered in
visible `(ord,id)` order. A lane is eligible only when every existing clip ends
at or before that pass's actual start; an empty lane qualifies. A clip ending
exactly at the boundary qualifies, while overlapping or later material prevents
reuse. Each existing lane is consumed at most once per capture. ADI chooses
the first eligible visible lane. Live's choice among several qualifying lanes
remains open (PLAN M6-A6H-027).

Musical clips compare checked native ends to the pass's proven tick boundary.
Nanosecond clips compare to the original capture boundary, rounded down to
whole nanoseconds. The capture sample can fall between two musical ticks.
Missing, noninteger, negative or overflowing extents,
or inconsistent track ownership, make that lane ineligible. A lane's muted
state does not erase its material for this check. Fallback creates a fresh lane
when no lane can be proved safe; it never discards existing material.

Reused lanes retain IDs and properties, and all old clips. The new lanes appear
in creation/pass order as a group above existing lanes, whose relative order
is retained even when reused and newly created passes are interleaved. Native
lane creation may renumber ordinals; Undo restores their exact prior values.
All supplied fallback and clip IDs must be distinct, positive and unused. A
fallback ID becomes a lane only when it is needed.
The take clips reference exact windows of the captured source. Media import and
all lanes, takes and main-lane edits commit in one SQL transaction. Each pass
has a distinct `Record` Undo group. The shared media row belongs to the first
group. An incomplete last pass replaces its captured interval while the
preceding main-lane material at either edge remains audible. An Undo restores
the preceding pass's main-lane result and removes the undone take.

Main-lane placement uses the arrangement's material edit planner. Recording
leaves authored automation alone. Capture boundaries must round-trip exactly
through the current musical clock. Accepted pre-loop samples keep their logged
project positions. The procedure stages audio on the project's volume and
uses identity-bound cleanup if the transaction fails.

## Verification and remaining work

Strict x64 recorder tests exercise real Session/WAV capture at both processing
precisions, 3.5 loop lengths, count-in splitting, partial first/last passes,
multiple wraps per callback, 128 one-sample passes, exact-end stops, pre-loop
input and sample-clock ceiling arithmetic. Controls exercise pass/ring capacity,
same-position seeks, stream gaps, bounds changes without a revision, disabled
loops, first-block contract mismatch and ordinary stop. Existing callback
allocation/file-I/O counters cover these cases. Linear capture, memory capture,
publication/Undo, transport and sliced-render regressions remain enabled.

These are deterministic engine tests using supplied input, not a hardware-driver
latency or long-duration recording measurement. The pass log is retained in the
completed result, not persisted as a crash-recovery journal.

Remaining CT-02 work includes measuring the reference lane-choice policy, punch and pre-roll
policy, latency compensation, recovery persistence, desktop wiring and
long-running device checks.

The recording suite checks ordered reuse, exact boundaries in both clocks,
overlap and future material, unknown extents, overflow, same-track isolation,
one use per lane, mixed fallback ordering, atomic failure and exact Undo/Redo.

## Loop recording parity checklist

| Case | Director's Live 12.4 reading | ADI check |
|---|---|---|
| Stop audio partway through pass four | One continuous source contains four takes. The fourth replaces the main-lane prefix through Stop. Pass three stays audible afterward. | CT-02 Session and publication fixtures cover exact PCM, source offsets and the partial pass. Desktop Stop remains to test. |
| Undo audio pass four | One `Undo Record` removes the fourth pass and restores pass three across the loop. Redo reinstates the partial take. | The same fixtures compare per-pass Undo/Redo, exact PCM and shared-source lifetime. |
| MIDI Overdub Off, loop off | New notes replace the notes in the covered clip. The clip extends to Stop, one take is retained, and one Undo restores the old material. | CT-03, pending the note-creation API and desktop recording. |
| MIDI Overdub Off, loop on | Each pass records its own notes. The final partial pass replaces the main prefix, with the previous pass audible over the uncovered tail. Each pass has its own Undo. | CT-03, pending the note-creation API and desktop recording. |

Readings correspond to PLAN M6-A6H-016 and CT-03's Overdub cases. Existing ADI
engine fixtures use supplied callback input. Hardware latency and the desktop
workflow have separate gates.
