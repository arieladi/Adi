# Engine export queue: first runner

`adi/render/queue.hpp` implements the first engine increment of ADR-0071 and
ADR-0205: editable application jobs and an isolated master-to-Float32-WAV runner.
Jobs are not project operations and do not enter project history.

```cpp
adi::render::Queue queue;
adi::render::Job job;
job.project = projectPath;
job.directory = existingOutputDirectory;
job.beginSample = 137;
job.endSample = 48000; // exclusive, measured at the export sample rate
job.session = {2, 48000, 512};
job.nameTemplate = "{project} - {track} - {counter}";
const auto id = queue.add(job);
queue.run(deviceLoader, control);
const auto entries = queue.snapshot();
```

The caller supplies the same kind of `engine::DeviceLoader` used for playback.
Every job opens a fresh read-only Store and Session, with fresh device instances.
Rows, device state and naming metadata are loaded under one SQLite read snapshot
when the job starts. A pending job therefore sees edits committed since it was
queued; an edit committed during loading becomes visible to the next job.

The runner executes synchronously on its caller's thread. The window can choose
an appropriate worker or dedicated render process. A plugin with a message-thread
creation/processing contract must run on that thread. `Control::serviceHost` runs
between blocks; session host callbacks are also drained there. The current host
contract does not negotiate plugin offline-quality modes.

The queue has one owner. `snapshot()` returns independent values suitable for
posting to the window. Progress callbacks may add jobs, remove pending jobs, or
edit a pending job; a running job is immutable. Jobs added during a run wait for
the next run. Completed results remain in the queue and never rerender implicitly.
Failed/cancelled jobs require `retry()`. Failure advances to the next job;
cancellation leaves the remaining jobs pending. A stop requested before a queue
run leaves all its jobs pending.

Rendering preserves the project's mixer, sends, mute, solo and automation. It
prerolls from zero to retain effect/instrument history, compensates the output's
latency, and writes exactly `[beginSample, endSample)`, including a short final
block. Audio buffers are bounded by the selected channel count and block size.
Float64 graph output is narrowed only at Float32 encoding. Missing devices,
unsupported playback, read errors, underruns, nonfinite/out-of-range samples,
and latency/topology reports fail the job rather than publishing degraded audio.

Names support `{project}`, `{track}` (the master name), `{group}` (empty for a
master job), `{bpm}` at range start, and `{counter}` (the stable, one-based enqueue
ordinal, unchanged by removal or retry). `{{` and `}}` escape braces.
Unknown or unmatched braces are errors. Expanded values are sanitized into one
portable filename component; values cannot introduce folders or more tokens.
The runner adds `.wav`. An empty name or a name over 200 UTF-8 bytes is refused.

The destination directory must already exist. A job writes into its own staging
directory, closes the WAV, checks cancellation, and publishes without replacing
an existing file. Cancellation/failure removes only that private stage. A file
successfully published is retained even if cancellation arrives afterward.
Publication uses `media::publishFile`: its fallback verifies a copy, but that copy
can be visible while in progress. No universal atomic-visibility guarantee is made.

This API explicitly produces Float32 WAV. It does not yet apply the registry's
PCM24/triangular-dither defaults. Track/group/region stems, wet/dry and master-path
choices, normalization/LUFS, other encoders and bit depths, NI Stems import/export,
queue persistence, and the Export window remain subsequent increments. There is
no new schema or silent fallback for these options.

`adi_render_queue_tests` decodes actual WAVs to check job edits, precise cropped
ranges, plugin latency plus parallel compensation, fresh state, both graph
precisions, track delay, concurrent project edits, callback mutations, queue
lifecycle, cancellation, publication collisions, failures and changing latency.
