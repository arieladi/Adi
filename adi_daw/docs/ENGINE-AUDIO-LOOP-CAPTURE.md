# CT-02 bounded audio loop-pass capture

This slice extends the existing hardware-input recorder and actual Session path.
It does not complete CT-02: no take lanes, main-lane copies or per-pass Undo are
published yet. The existing linear materializer refuses an opted-in loop result,
including one incomplete pass, before staging files or creating project rows.
The private source remains available for explicit recovery or discard.

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

Remaining CT-02: atomic one-media/multiple-take publication per target; new versus
reused lane policy; last-pass copies to main; per-pass history and the AT-3 Undo
rule; punch/pre-roll placement; recovery persistence; owner/UI integration and
long-running device tests. No new register number or CT-02 completion is claimed.
