# Engine export queue: WAV jobs and direct stems

`adi/render/queue.hpp` implements the first engine increment of ADR-0071 and
ADR-0205: editable application jobs and an isolated WAV runner.
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

The default source is the master. Set `job.source.trackId` to an audio, MIDI,
instrument, group, return or master track ID for a direct wet stem. Zero selects
the project's master when rendering starts. `source.tap` defaults to
`engine::OfflineTap::PostFader`: after the track's effects and mixer strip, before
its parent's console channel encoder. `PreFader` captures after the track's own
effects but before its strip's gain, pan, mute and track delay. It is still wet.
A group includes its own summing and effects; a child stem excludes its parents'
effects. Other tracks and returns keep running, including dependencies of the
selected source. This is not an isolated pass through the master.

Set `job.regionId` to a cycle marker ID to use that single region instead of
`beginSample`/`endSample`. The runner resolves its current name and bounds when
the job starts. Tick-based regions use the current tempo map; absolute regions
use nanoseconds. It rounds the absolute start and end separately to the nearest
output sample, refusing missing, malformed or empty ranges. Independent region
jobs are not ADR-0070's continuous, gapless region batch.

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

Rendering preserves the project's mixer, sends, solo and automation; the selected
tap determines whether the source strip's controls are included. It
prerolls from zero to retain effect/instrument history, compensates the selected output's
latency, and writes exactly `[beginSample, endSample)`, including a short final
block. Audio buffers are bounded by the selected channel count and block size.
Float64 graph output keeps its precision through final PCM quantization or
Float32 encoding. Missing devices, unsupported playback, read errors, underruns,
nonfinite samples (or values outside Float32 range when encoding Float32),
and latency/topology reports fail the job rather than publishing degraded audio.

Names support `{project}`, `{track}` (the source name), `{group}` (the selected
group's name or its nearest ancestor group's name, otherwise empty), `{region}`
(the selected cycle marker's name, otherwise empty), `{bpm}` at range start,
and `{counter}` (the stable, one-based enqueue
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

`job.encoding` selects `audio::WavFormat::Pcm16`, `Pcm24` or `Float32` and
`audio::WavDither::None`, `Triangular` or `Rectangular`. New jobs default to
PCM24/triangular, matching the Export registry. **Callers of the earlier Float32
runner must explicitly select `Float32` to keep that encoding.** Float32 never
dithers; the setting applies only below 32 bits. Unsupported formats and dither
values are refused; noise-shaped Pow-r options are not implemented.

PCM rounds to nearest with ties away from zero and saturates to the signed
integer range. Dither is applied once, immediately before rounding: triangular
is the difference of two uniform samples (two LSB peak to peak), rectangular
is one LSB peak to peak. A specified unsigned generator keeps the sequence
portable and continuous across channels and write boundaries. Its seed is
`encoding.ditherSeed` XOR the stable enqueue ordinal, so different jobs use
different streams and unchanged retries reproduce their samples. The standalone
WAV writer retains its previous no-dither default for existing callers.

Dry and isolated through-master choices, continuous gapless region batches,
normalization/LUFS, other containers, noise shaping, NI Stems import/export,
queue persistence, and the Export window remain subsequent increments. There is
no new schema or silent fallback for these options.

`renderWav` runs a single job directly; the queue uses it. The compatibility
entry point `renderMasterWav` accepts only the default master source and reports
a failure for an explicit track or pre-fader tap.

`adi_render_queue_tests` decodes actual WAVs to check job edits, precise cropped
ranges, plugin latency plus parallel compensation, fresh state, both graph
precisions, track delay, concurrent project edits, callback mutations, queue
lifecycle, cancellation, publication collisions, failures and changing latency.
Stem checks cover group and console boundaries, return-loop echoes, multichannel
audio and edited musical/absolute regions. `adi_offline_capture_tests` verifies
all 64 channels, double precision, segmented blocks, sidechain dependencies,
silence, stale-block refusal and allocation-free capture without changing the
master output or compensation.
PCM checks decode both depths, distinguish double from prematurely narrowed
half-LSB values, and verify callback-independent dither. The WAV suite also
checks saturation, legacy float bits, seeded streams, stereo independence and
the distribution of quantized triangular/rectangular noise.
